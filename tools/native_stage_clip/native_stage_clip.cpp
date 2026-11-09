// native_stage_clip - a stage drawn entirely by the native path, along a camera path the game recorded: the converted
// stage (tools/stage_convert) loaded by the native RenderWare core (rwcore + rpworld: RwTexDictionaryStreamRead,
// RpWorldStreamRead), the stage's light set made as the game makes it (RpLight, RpWorldAddLight), a native RwCamera
// per frame driven by the game's own camera (SHADOW_CAMERA_TRACE records of the running game; runtime/src/gekko/
// system.cpp) and culling the world sectors with RwCameraFrustumTestSphere, drawn by the D3D12 renderer
// (runtime/lift/render) and encoded by ffmpeg. No GameCube code runs and no GameCube data is decoded here.
//
//   native_stage_clip <converted directory> <stage> <camera trace> <out.mp4 | out.png> [--width W] [--height H]
//                     [--first N] [--frames N] [--light-set S] [--every K]
//
// The land is unlit and prelit, as the game draws it (its level decoder clears the light flag of every land class);
// the light set is loaded into the world for lit objects (none are drawn yet). The land classes' recipes and pass order
// are the game's (tools/stage_preview/stage_scene.cpp, R2_MATERIAL_CENSUS.md 4.1).
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "assets/geometry_import.h"
#include "assets/texture_import.h"
#include "render/frame.h"
#include "render/renderer.h"
#include "rw/core/binary.h"
#include "rw/core/camera.h"
#include "rw/core/engine.h"
#include "rw/core/frame.h"
#include "rw/core/stream.h"
#include "rw/core/texture.h"
#include "rw/world/light.h"
#include "rw/world/world.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using namespace shadow;

namespace {

std::vector<std::uint8_t> readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ---- the game's land classes (stage_scene.cpp) ----------------------------------------------------------------
struct LandClass {
    const char* token;
    enum Kind { Opaque, Punch, Translucent, Additive, Glare } kind;
    bool far_scenery;
};
const LandClass kClasses[] = {
    {"_N_", LandClass::Opaque, true},       {"_D_", LandClass::Opaque, true},        {"_OS_", LandClass::Opaque, false},
    {"_OSL_", LandClass::Opaque, false},    {"_O_", LandClass::Opaque, false},       {"_OW_", LandClass::Opaque, false},
    {"_OL_", LandClass::Opaque, false},     {"_P_", LandClass::Punch, false},        {"_AF_", LandClass::Translucent, false},
    {"_A_", LandClass::Translucent, false}, {"_AL_", LandClass::Translucent, false}, {"_K_", LandClass::Additive, false},
    {"_M_", LandClass::Additive, false},    {"_G_", LandClass::Glare, false},        {"_GL_", LandClass::Glare, false},
};
render::Recipe recipeOf(const LandClass& land) {
    render::Recipe r;
    switch (land.kind) {
    case LandClass::Opaque:
        r.alpha_cutoff = 1.0f / 255.0f;
        r.cull = std::strcmp(land.token, "_OW_") != 0 ? render::Cull::Back : render::Cull::None;
        break;
    case LandClass::Punch:
        r.alpha_cutoff = 200.0f / 255.0f;
        r.cull = render::Cull::Back;
        break;
    case LandClass::Translucent:
        r.depth_write = false;
        r.cull = render::Cull::None;
        break;
    case LandClass::Additive:
        r.dst = render::Blend::One;
        r.depth_write = false;
        r.cull = render::Cull::None;
        break;
    case LandClass::Glare:
        break;
    }
    return r;
}
render::Address addressOf(std::uint32_t mode) {
    return mode == 2 ? render::Address::Mirror : (mode == 3 || mode == 4) ? render::Address::Clamp : render::Address::Wrap;
}

// ---- the camera trace -------------------------------------------------------------------------------------------
struct CameraRecord {
    std::uint32_t sequence, camera, width, height, projection;
    float windowX, windowY, offsetX, offsetY, nearPlane, farPlane;
    float ltm[12];   // right, up, at, pos
};
static_assert(sizeof(CameraRecord) == 92);

bool identityFrame(const CameraRecord& r) {
    static constexpr float kIdentity[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    return std::memcmp(r.ltm, kIdentity, sizeof kIdentity) == 0;
}

/// The game's main camera, one record per displayed frame. Measured on the Westopolis replay trace (.build/recordings/
/// native-clip-camera): one perspective camera on the 640x480 raster takes 458285 of the 458374 updates; each frame
/// ends its updates with 3 under an identity frame (window 0.6598 x 0.5, the 2D pass), and every run of its 3D updates
/// between two such groups has exactly one state (13697 of 13697 runs) - as many as the run's glare composites (one per
/// 3D frame, session report). So a frame is the first 3D update after a 2D group, whichever 3D pass it belongs to.
std::vector<CameraRecord> mainCameraPath(const std::vector<CameraRecord>& all) {
    std::map<std::uint32_t, std::size_t> count;
    for (const CameraRecord& r : all) {
        if (r.projection == 1 && r.width >= 600) ++count[r.camera];
    }
    std::uint32_t best = 0;
    std::size_t most = 0;
    for (const auto& [camera, n] : count) {
        if (n > most) most = n, best = camera;
    }
    std::vector<CameraRecord> path;
    bool frameEnded = true;
    for (const CameraRecord& r : all) {
        if (r.camera != best || r.projection != 1 || r.width < 600) continue;
        if (identityFrame(r)) {
            frameEnded = true;
        } else if (frameEnded) {
            path.push_back(r), frameEnded = false;
        }
    }
    return path;
}

// ---- the stage in the native core, uploaded to the renderer ----------------------------------------------------
struct Draw {
    render::MeshHandle mesh;
    std::uint32_t submesh;
    render::Material material;
    render::Recipe recipe;
    LandClass::Kind kind;
    RwSphere bound;   // the sector's, for the native camera's culling
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: native_stage_clip <converted dir> <stage> <camera trace> <out.mp4|out.png> [--width W] "
                             "[--height H] [--first N] [--frames N] [--light-set S] [--every K]\n");
        return 2;
    }
    const std::filesystem::path dir = std::filesystem::u8path(argv[1]);
    const std::string stage = argv[2];
    const std::string tracePath = argv[3], out = argv[4];
    std::uint32_t width = 5120, height = 2880, first = 0, frames = 0, every = 1;
    int lightSet = 4;   // the stage's default (0x80048574 selects 4)
    for (int i = 5; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        const auto v = static_cast<std::uint32_t>(std::stoul(argv[i + 1]));
        if (a == "--width") width = v;
        else if (a == "--height") height = v;
        else if (a == "--first") first = v;
        else if (a == "--frames") frames = v;
        else if (a == "--light-set") lightSet = static_cast<int>(v);
        else if (a == "--every") every = std::max(1u, v);
    }
    // the camera path
    std::vector<CameraRecord> records;
    {
        const std::vector<std::uint8_t> bytes = readFile(tracePath);
        records.resize(bytes.size() / sizeof(CameraRecord));
        std::memcpy(records.data(), bytes.data(), records.size() * sizeof(CameraRecord));
    }
    const std::vector<CameraRecord> path = mainCameraPath(records);
    std::printf("camera trace: %zu updates, the main camera's %zu\n", records.size(), path.size());
    if (path.empty()) return 1;

    // ---- the native engine and the converted stage -------------------------------------------------------------
    RwEngineOpenParams params{nullptr};
    if (!RwEngineInit(nullptr, 0, 0) || !RpWorldPluginAttach() || !RwEngineOpen(&params) || !RwEngineStart()) {
        std::fprintf(stderr, "engine start\n");
        return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> txd = readFile(dir / (stage + ".txd")), wld = readFile(dir / (stage + ".wld"));
    RwMemory tm{txd.data(), txd.size()};
    RwStream* ts = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &tm);
    RwTexDictionary* dict = RwStreamFindChunk(ts, rwID_TEXDICTIONARY, nullptr, nullptr) ? RwTexDictionaryStreamRead(ts) : nullptr;
    RwStreamClose(ts, nullptr);
    if (dict == nullptr) return std::fprintf(stderr, "the texture dictionary\n"), 1;
    RwTexDictionarySetCurrent(dict);
    std::vector<std::string> classOf;
    {
        std::ifstream manifest(dir / (stage + ".worlds"));
        std::string line;
        while (std::getline(manifest, line)) {
            std::istringstream in(line);
            std::string archive, member, token;
            in >> archive >> member >> token;
            classOf.push_back(token);
        }
    }
    std::vector<RpWorld*> worlds;
    RwMemory wm{wld.data(), wld.size()};
    RwStream* ws = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &wm);
    while (worlds.size() < classOf.size() && RwStreamFindChunk(ws, rwID_WORLD, nullptr, nullptr)) {
        RpWorld* w = RpWorldStreamRead(ws);
        if (w == nullptr) return std::fprintf(stderr, "RpWorldStreamRead failed on world %zu\n", worlds.size()), 1;
        worlds.push_back(w);
    }
    RwStreamClose(ws, nullptr);
    const double loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("native load: %zu worlds in %.1f ms\n", worlds.size(), loadMs);

    // the stage's light set, as the game makes it (0x80047870): an ambient and a directional light; the directional's
    // frame turned about X by the record's first angle, then about Y by its second (degrees)
    RpLight* ambient = RpLightCreate(rpLIGHTAMBIENT);
    RpLight* directional = RpLightCreate(rpLIGHTDIRECTIONAL);
    RwFrame* lightFrame = RwFrameCreate();
    {
        const std::vector<std::uint8_t> sets = readFile(dir / (stage + ".light"));
        if (sets.size() != 16u * 0x34u) return std::fprintf(stderr, "the light sets\n"), 1;
        float f[11];
        std::int32_t angles[2];
        std::memcpy(f, sets.data() + 0x34u * static_cast<std::size_t>(lightSet), 44);
        std::memcpy(angles, sets.data() + 0x34u * static_cast<std::size_t>(lightSet) + 44, 8);
        const RwRGBAReal a{f[0], f[1], f[2], f[3]}, d{f[4], f[5], f[6], f[7]};
        RpLightSetColor(ambient, &a);
        RpLightSetColor(directional, &d);
        const RwV3d xAxis{1, 0, 0}, yAxis{0, 1, 0};
        RwFrameRotate(lightFrame, &xAxis, static_cast<RwReal>(angles[0]), rwCOMBINEREPLACE);
        RwFrameRotate(lightFrame, &yAxis, static_cast<RwReal>(angles[1]), rwCOMBINEPOSTCONCAT);
        RpLightSetFrame(directional, lightFrame);
        RpWorldAddLight(worlds.front(), ambient);
        RpWorldAddLight(worlds.front(), directional);
        _rwFrameSyncDirty();
        std::printf("light set %d: ambient (%.3f %.3f %.3f), directional (%.3f %.3f %.3f) at %d / %d degrees\n", lightSet,
                    a.red, a.green, a.blue, d.red, d.green, d.blue, angles[0], angles[1]);
    }

    // ---- the renderer ----------------------------------------------------------------------------------------------
    render::Renderer renderer;
    render::RendererOptions options;
    std::string error;
    if (!renderer.create(options, &error)) return std::fprintf(stderr, "renderer: %s\n", error.c_str()), 1;
    std::printf("device: %s\n", renderer.device().caps().adapter_name.c_str());
    std::map<const RwTexture*, render::TextureHandle> textureOf;
    RwTexDictionaryForAllTextures(dict, [](RwTexture* t, void* data) {
        auto& ctx = *static_cast<std::pair<render::Renderer*, std::map<const RwTexture*, render::TextureHandle>*>*>(data);
        rwHostRaster* host = rwHostRasterOf(t->raster);
        assets::TextureImage image;
        image.width = static_cast<std::uint32_t>(host->width);
        image.height = static_cast<std::uint32_t>(host->height);
        image.mip_levels = static_cast<std::uint32_t>(host->levels);
        std::size_t at = 0;
        for (RwInt32 l = 0; l < host->levels; ++l) {
            image.level_offsets.push_back(at);
            at += rwHostRasterLevelSize(host, l);
        }
        image.rgba.assign(host->pixels, host->pixels + at);
        for (std::size_t p = 3; p < rwHostRasterLevelSize(host, 0); p += 4) image.texel_alpha |= image.rgba[p] < 255;
        std::string problem;
        const render::TextureHandle h = ctx.first->create_texture(render::Scope::Stage, image, &problem);
        if (h) (*ctx.second)[t] = h;
        return t;
    }, new std::pair<render::Renderer*, std::map<const RwTexture*, render::TextureHandle>*>(&renderer, &textureOf));
    std::vector<Draw> draws;
    std::size_t triangles = 0;
    for (std::size_t wi = 0; wi < worlds.size(); ++wi) {
        RpWorld* w = worlds[wi];
        const LandClass* land = nullptr;
        for (const LandClass& c : kClasses) if (classOf[wi] == c.token) land = &c;
        if (land == nullptr || land->kind == LandClass::Glare) continue;   // glare sources belong to the glare pass
        struct Ctx {
            std::vector<Draw>* draws;
            render::Renderer* renderer;
            std::map<const RwTexture*, render::TextureHandle>* textures;
            RpWorld* world;
            const LandClass* land;
            std::size_t* triangles;
        } ctx{&draws, &renderer, &textureOf, w, land, &triangles};
        RpWorldForAllWorldSectors(w, [](RpWorldSector* s, void* data) {
            auto& c = *static_cast<Ctx*>(data);
            if (s->numVertices == 0 || s->mesh == nullptr || s->mesh->numMeshes == 0) return s;
            assets::MeshData mesh;
            mesh.vertex_count = s->numVertices;
            for (RwUInt16 v = 0; v < s->numVertices; ++v) {
                mesh.positions.insert(mesh.positions.end(), {s->vertices[v].x, s->vertices[v].y, s->vertices[v].z});
                if (s->preLitLum != nullptr) {
                    const RwRGBA& col = s->preLitLum[v];
                    mesh.colours.insert(mesh.colours.end(), {col.red, col.green, col.blue, col.alpha});
                }
                if (s->texCoords[0] != nullptr) mesh.uv0.insert(mesh.uv0.end(), {s->texCoords[0][v].u, s->texCoords[0][v].v});
                if (s->texCoords[1] != nullptr) mesh.uv1.insert(mesh.uv1.end(), {s->texCoords[1][v].u, s->texCoords[1][v].v});
            }
            const RpMesh* meshes = RpMeshHeaderGetMeshes(s->mesh);
            std::vector<RwInt32> materialOf;
            for (RwUInt16 m = 0; m < s->mesh->numMeshes; ++m) {
                assets::MeshData::Submesh sub;
                sub.first_index = static_cast<std::uint32_t>(mesh.indices.size());
                std::vector<std::uint32_t> run(meshes[m].indices, meshes[m].indices + meshes[m].numIndices);
                if ((s->mesh->flags & 1u) != 0) assets::append_strip_triangles(run.data(), run.size(), mesh.indices);
                else mesh.indices.insert(mesh.indices.end(), run.begin(), run.end());
                sub.index_count = static_cast<std::uint32_t>(mesh.indices.size()) - sub.first_index;
                sub.material = m;
                mesh.submeshes.push_back(sub);
                materialOf.push_back(_rpMaterialListFindMaterialIndex(&c.world->matList, meshes[m].material));
            }
            mesh.bounds = assets::bounding_sphere(mesh.positions);
            std::string problem;
            const render::MeshHandle handle = c.renderer->create_mesh(render::Scope::Stage, mesh, &problem);
            if (!handle) return s;
            const RwBBox& b = s->tightBoundingBox;
            const RwV3d centre{(b.sup.x + b.inf.x) * 0.5f, (b.sup.y + b.inf.y) * 0.5f, (b.sup.z + b.inf.z) * 0.5f};
            const float dx = b.sup.x - centre.x, dy = b.sup.y - centre.y, dz = b.sup.z - centre.z;
            const RwSphere bound{centre, std::sqrt(dx * dx + dy * dy + dz * dz)};
            for (std::uint32_t m = 0; m < mesh.submeshes.size(); ++m) {
                const RpMaterial* mat = meshes[m].material;
                Draw d;
                d.mesh = handle, d.submesh = m, d.kind = c.land->kind, d.recipe = recipeOf(*c.land), d.bound = bound;
                d.material.combine = render::Combine::OneStage;
                d.material.vertex_colours = (c.world->flags & rpWORLDPRELIT) != 0;
                d.material.modulate = (c.world->flags & rpWORLDMODULATEMATERIALCOLOR) != 0;
                d.material.colour = {mat->color.red / 255.0f, mat->color.green / 255.0f, mat->color.blue / 255.0f,
                                     mat->color.alpha / 255.0f};
                if (mat->texture != nullptr) {
                    const auto found = c.textures->find(mat->texture);
                    if (found != c.textures->end()) {
                        d.material.texture = found->second;
                        d.material.sampler = {render::Filter::Anisotropic, addressOf(RwTextureGetAddressingU(mat->texture)),
                                              addressOf(RwTextureGetAddressingV(mat->texture))};
                    }
                }
                *c.triangles += mesh.submeshes[m].index_count / 3;
                c.draws->push_back(d);
            }
            return s;
        }, &ctx);
    }
    std::stable_sort(draws.begin(), draws.end(), [](const Draw& a, const Draw& b) { return a.kind < b.kind; });   // pass order
    std::printf("uploaded: %zu textures, %zu draws, %zu triangles\n", textureOf.size(), draws.size(), triangles);

    // ---- the native camera ---------------------------------------------------------------------------------------
    RwCamera* camera = RwCameraCreate();
    RwFrame* cameraFrame = RwFrameCreate();
    RwCameraSetFrame(camera, cameraFrame);
    const bool png = out.size() > 4 && out.substr(out.size() - 4) == ".png";
    std::FILE* ffmpeg = nullptr;
    if (!png) {
        const std::string command = "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s " + std::to_string(width) + "x" +
                                    std::to_string(height) + " -r " + std::to_string(60 / every) +
                                    " -i - -c:v hevc_nvenc -preset p5 -rc vbr -cq 22 -pix_fmt yuv420p \"" + out + "\"";
        ffmpeg = _popen(command.c_str(), "wb");
        if (ffmpeg == nullptr) return std::fprintf(stderr, "ffmpeg\n"), 1;
    }
    const std::size_t last = frames == 0 ? path.size() : std::min<std::size_t>(path.size(), first + std::size_t{frames} * every);
    std::size_t drawn = 0, culled = 0, written = 0;
    double renderMs = 0;
    std::vector<std::uint8_t> pixels;
    for (std::size_t f = first; f < last; f += every) {
        const CameraRecord& r = path[f];
        // the native camera takes the game's: window, offset, clip planes, and the frame placed at its LTM
        const RwV2d window{r.windowX, r.windowY}, offset{r.offsetX, r.offsetY};
        // Hor+ for the output's aspect: the game's vertical window, the width the output's aspect gives it
        const RwV2d displayed{r.windowY * static_cast<float>(width) / static_cast<float>(height), r.windowY};
        (void)window;
        RwCameraSetViewWindow(camera, &displayed);
        RwCameraSetViewOffset(camera, &offset);
        RwCameraSetNearClipPlane(camera, r.nearPlane);
        RwCameraSetFarClipPlane(camera, r.farPlane);
        RwMatrix m{};
        std::memcpy(&m.right, &r.ltm[0], 12), std::memcpy(&m.up, &r.ltm[3], 12);
        std::memcpy(&m.at, &r.ltm[6], 12), std::memcpy(&m.pos, &r.ltm[9], 12);
        cameraFrame->modelling = m;
        RwFrameUpdateObjects(cameraFrame);
        RwCameraBeginUpdate(camera);   // syncs the dirty frames: the camera's frustum follows its frame
        render::Camera view;
        const RwMatrix& ltm = cameraFrame->ltm;
        const RwV3d columns[4] = {{-ltm.right.x, -ltm.right.y, -ltm.right.z}, ltm.up, ltm.at, ltm.pos};
        for (int c = 0; c < 4; ++c) {
            view.frame.m[0][c] = columns[c].x;
            view.frame.m[1][c] = columns[c].y;
            view.frame.m[2][c] = columns[c].z;
        }
        view.view_window_x = camera->viewWindow.x;
        view.view_window_y = camera->viewWindow.y;
        view.near_plane = camera->nearPlane;
        view.far_plane = camera->farPlane;
        render::FrameBuilder builder;
        builder.begin_frame(f + 1, render::Float4{0, 0, 0, 1});
        const render::ViewId v = builder.view(view, render::ViewRect{});
        // the world's lights as the renderer's light set (the land is unlit; lit objects would use it)
        render::LightSet set;
        set.ambient = {ambient->color.red, ambient->color.green, ambient->color.blue};
        set.directional_count = 1;
        set.directional_colour[0] = {directional->color.red, directional->color.green, directional->color.blue};
        set.directional_vector[0] = {lightFrame->ltm.at.x, lightFrame->ltm.at.y, lightFrame->ltm.at.z};
        const render::LightSetId lights = builder.lights(set);
        render::Clear clear;
        clear.colour = true, clear.depth = true;
        builder.begin_pass(v, render::PassKind::Scene, clear);
        for (const Draw& d : draws) {
            if (RwCameraFrustumTestSphere(camera, &d.bound) == rwSPHEREOUTSIDE) {
                ++culled;
                continue;
            }
            builder.draw(d.mesh, d.submesh, d.material, render::Float3x4{}, d.recipe, lights);
            ++drawn;
        }
        builder.end_pass();
        RwCameraEndUpdate(camera);
        const auto a = std::chrono::steady_clock::now();
        if (!renderer.render_to_image(builder.finish(), width, height, &pixels, &error)) {
            return std::fprintf(stderr, "render: %s\n", error.c_str()), 1;
        }
        renderMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        if (png) {
            stbi_write_png(out.c_str(), static_cast<int>(width), static_cast<int>(height), 4, pixels.data(), static_cast<int>(width * 4));
            ++written;
            break;
        }
        std::fwrite(pixels.data(), 1, pixels.size(), ffmpeg);
        if (++written % 120 == 0) std::printf("  %zu frames\n", written), std::fflush(stdout);
    }
    if (ffmpeg != nullptr) _pclose(ffmpeg);
    std::printf("%zu frames at %ux%u: %zu draws, %zu culled by the native camera; %.1f ms a frame (render + readback)\n",
                written, width, height, drawn, culled, written ? renderMs / static_cast<double>(written) : 0.0);
    for (RpWorld* w : worlds) RpWorldDestroy(w);
    return 0;
}
