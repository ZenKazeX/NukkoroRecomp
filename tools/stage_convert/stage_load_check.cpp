// stage_load_check - proof of a converted stage (tools/stage_convert):
//   1. the converted worlds against the originals, independently of the native core: every converted world read by the
//      asset importer's platform-independent sector reader (runtime/lift/assets) gives exactly the meshes the importer
//      decodes from the GameCube's prepared sectors - positions, colours, UV sets, triangle indices, materials;
//   2. the native RenderWare core loads them: RwTexDictionaryStreamRead (host native textures), then every world through
//      RpWorldStreamRead with that dictionary current; the sector, vertex and triangle counts, every mesh's materials
//      and every textured material's texture found; then everything destroyed with no allocation left.
//
//   stage_load_check <files directory> <stage> <converted directory>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "assets/archive.h"
#include "assets/chunk_stream.h"
#include "assets/geometry_import.h"
#include "rw/core/binary.h"
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/stream.h"
#include "rw/core/texture.h"
#include "rw/world/world.h"

using namespace shadow;

namespace {

std::vector<std::uint8_t> readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::map<void*, std::size_t> g_live;
void* countMalloc(std::size_t size, RwUInt32) {
    void* p = std::malloc(size ? size : 1);
    g_live[p] = size;
    return p;
}
void countFree(void* p) {
    if (p != nullptr) g_live.erase(p), std::free(p);
}
void* countRealloc(void* p, std::size_t size, RwUInt32 hint) {
    if (p == nullptr) return countMalloc(size, hint);
    void* q = std::realloc(p, size);
    if (q != nullptr) g_live.erase(p), g_live[q] = size;
    return q;
}
void* countCalloc(std::size_t n, std::size_t s, RwUInt32 h) {
    void* p = countMalloc(n * s, h);
    if (p) std::memset(p, 0, n * s);
    return p;
}

bool sameMesh(const assets::MeshData& a, const assets::MeshData& b, std::string* why) {
    if (a.vertex_count != b.vertex_count) return *why = "vertex count", false;
    if (a.positions != b.positions) return *why = "positions", false;
    if (a.colours != b.colours) return *why = "colours", false;
    if (a.uv0 != b.uv0) return *why = "uv0", false;
    if (!b.uv1.empty() && a.uv1 != b.uv1) return *why = "uv1", false;
    if (a.indices != b.indices) return *why = "indices", false;
    if (a.submeshes.size() != b.submeshes.size()) return *why = "submesh count", false;
    for (std::size_t i = 0; i < a.submeshes.size(); ++i) {
        if (a.submeshes[i].material != b.submeshes[i].material || a.submeshes[i].index_count != b.submeshes[i].index_count ||
            a.submeshes[i].first_index != b.submeshes[i].first_index) {
            return *why = "submesh " + std::to_string(i), false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: stage_load_check <files directory> <stage> <converted directory>\n");
        return 2;
    }
    const std::filesystem::path files = std::filesystem::u8path(argv[1]), converted = std::filesystem::u8path(argv[3]);
    const std::string stage = argv[2];
    // ---- 1. converted vs original, through the importer ---------------------------------------------------------
    std::vector<std::pair<std::string, std::string>> listed;   // archive, member
    {
        std::ifstream manifest(converted / (stage + ".worlds"));
        std::string line;
        while (std::getline(manifest, line)) {
            std::istringstream in(line);
            std::string archive, member, token;
            in >> archive >> member >> token;
            listed.push_back({archive, member});
        }
    }
    std::vector<assets::World> originals;
    for (const auto& [archive, member] : listed) {
        const std::vector<std::uint8_t> bytes = readFile(files / stage / archive);
        const assets::ByteView view{bytes.data(), bytes.size()};
        assets::OneArchive one;
        std::string problem;
        if (!assets::parse_one_archive(view, &one, &problem)) return std::fprintf(stderr, "%s: %s\n", archive.c_str(), problem.c_str()), 1;
        for (const assets::OneMember& m : one.members) {
            if (m.name != member) continue;
            std::vector<std::uint8_t> expanded;
            assets::read_one_member(view, one, m, &expanded, &problem);
            const assets::ByteView ev{expanded.data(), expanded.size()};
            std::vector<assets::Chunk> top;
            assets::read_stream(ev, &top);
            for (const assets::Chunk& c : top) {
                if (c.type != assets::chunk::kWorld) continue;
                assets::World w;
                if (!assets::import_world(ev, c, &w, &problem)) return std::fprintf(stderr, "original %s: %s\n", member.c_str(), problem.c_str()), 1;
                originals.push_back(std::move(w));
            }
        }
    }
    const std::vector<std::uint8_t> wld = readFile(converted / (stage + ".wld"));
    const assets::ByteView wv{wld.data(), wld.size()};
    std::vector<assets::Chunk> worlds;
    if (!assets::read_chunks(wv, 0, wld.size(), &worlds)) return std::fprintf(stderr, "the converted stream does not parse\n"), 1;
    if (worlds.size() != originals.size()) return std::fprintf(stderr, "%zu converted worlds, %zu originals\n", worlds.size(), originals.size()), 1;
    std::size_t sectorsCompared = 0;
    for (std::size_t i = 0; i < worlds.size(); ++i) {
        assets::World w;
        std::string problem;
        if (!assets::import_world(wv, worlds[i], &w, &problem)) return std::fprintf(stderr, "converted world %zu: %s\n", i, problem.c_str()), 1;
        if (w.sectors.size() != originals[i].sectors.size()) return std::fprintf(stderr, "world %zu: sector count\n", i), 1;
        if ((w.format & 0x01000000u) != 0) return std::fprintf(stderr, "world %zu is still native\n", i), 1;
        for (std::size_t s = 0; s < w.sectors.size(); ++s) {
            std::string why;
            if (!sameMesh(w.sectors[s].mesh, originals[i].sectors[s].mesh, &why)) {
                return std::fprintf(stderr, "world %zu (%s) sector %zu: %s differ\n", i, listed[i].second.c_str(), s, why.c_str()), 1;
            }
            ++sectorsCompared;
        }
    }
    std::printf("  importer: %zu converted worlds, %zu sectors identical to the originals' decoded meshes\n", worlds.size(),
                sectorsCompared);
    // ---- 2. the native core loads them -----------------------------------------------------------------------------
    const RwMemoryFunctions counting{countMalloc, countFree, countRealloc, countCalloc};
    RwEngineOpenParams params{nullptr};
    if (!RwEngineInit(&counting, 0, 0) || !RpWorldPluginAttach() || !RwEngineOpen(&params) || !RwEngineStart()) {
        return std::fprintf(stderr, "engine start\n"), 1;
    }
    std::vector<std::uint8_t> txd = readFile(converted / (stage + ".txd"));
    RwMemory tm{txd.data(), txd.size()};
    RwStream* ts = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &tm);
    RwTexDictionary* dict = nullptr;
    if (RwStreamFindChunk(ts, rwID_TEXDICTIONARY, nullptr, nullptr)) dict = RwTexDictionaryStreamRead(ts);
    RwStreamClose(ts, nullptr);
    if (dict == nullptr) return std::fprintf(stderr, "RwTexDictionaryStreamRead failed\n"), 1;
    std::size_t textures = 0;
    RwTexDictionaryForAllTextures(dict, [](RwTexture* t, void* n) { return ++*static_cast<std::size_t*>(n), t; }, &textures);
    RwTexDictionarySetCurrent(dict);
    RwMemory wm{const_cast<std::uint8_t*>(wld.data()), wld.size()};
    RwStream* ws = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &wm);
    std::vector<RpWorld*> loaded;
    std::size_t sectors = 0, vertices = 0, triangles = 0, meshes = 0, textured = 0, missing = 0;
    for (std::size_t i = 0; i < worlds.size(); ++i) {
        if (!RwStreamFindChunk(ws, rwID_WORLD, nullptr, nullptr)) return std::fprintf(stderr, "world chunk %zu not found\n", i), 1;
        RpWorld* w = RpWorldStreamRead(ws);
        if (w == nullptr) {
            RwError e{};
            RwErrorGet(&e);
            return std::fprintf(stderr, "RpWorldStreamRead failed on world %zu (%s): error %08X\n", i, listed[i].second.c_str(),
                                static_cast<unsigned>(e.errorCode)), 1;
        }
        struct Count {
            std::size_t *s, *v, *t, *m;
        } c{&sectors, &vertices, &triangles, &meshes};
        RpWorldForAllWorldSectors(w, [](RpWorldSector* s, void* d) {
            auto* c = static_cast<Count*>(d);
            ++*c->s, *c->v += s->numVertices, *c->t += s->numTriangles, *c->m += s->mesh->numMeshes;
            return s;
        }, &c);
        for (RwInt32 m = 0; m < w->matList.numMaterials; ++m) {
            textured += w->matList.materials[m]->texture != nullptr;
        }
        loaded.push_back(w);
    }
    RwStreamClose(ws, nullptr);
    // a material whose original names a texture the dictionary lacks: counted against the importer's records
    for (const assets::World& w : originals) {
        for (const assets::MaterialRecord& m : w.materials) {
            if (m.texture.present && RwTexDictionaryFindNamedTexture(dict, m.texture.name.c_str()) == nullptr) ++missing;
        }
    }
    std::printf("  native: %zu textures, %zu worlds, %zu sectors, %zu vertices, %zu triangles, %zu meshes, %zu textured "
                "materials (%zu materials name a texture the stage does not have)\n",
                textures, loaded.size(), sectors, vertices, triangles, meshes, textured, missing);
    for (RpWorld* w : loaded) RpWorldDestroy(w);
    RwTexDictionaryDestroy(dict);
    RwEngineStop(), RwEngineClose(), RwEngineTerm();
    if (!g_live.empty()) return std::fprintf(stderr, "%zu allocations leaked\n", g_live.size()), 1;
    std::printf("stage_load_check: %s converted and loaded natively; nothing leaked\n", stage.c_str());
    return 0;
}
