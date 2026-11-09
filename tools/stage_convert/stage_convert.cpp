// stage_convert - the OFFLINE converter of a stage's GameCube data to the native PC formats the native RenderWare core
// loads directly (rwcore/README.md "Stage conversion"; the user's rule: no GameCube-format decoding at load time).
//
//   stage_convert <files directory> <stage, e.g. stg0100> <output directory>
//
// Writes into the output directory:
//   <stage>.txd   a RenderWare stream of one texture dictionary: every texture of the stage's ONE archives (first one
//                 of each name, upper-case compare - the game's texture lookup), decoded from its GameCube texel
//                 encoding to R G B A with every mip level (runtime/lift/assets texture importer), as host native
//                 textures (rwHOSTTEXTUREPLATFORM; rw/core/texture.h)
//   <stage>.light the stage's light sets, as the game reads stg%04d/stg%04d_light.bin (16 records of 0x34 bytes; already
//                 little-endian on the disc - the GameCube swapped them, a little-endian PC reads them as they are)
//   <stage>.wld   (with the world module) the land worlds as RenderWare platform-independent world chunks
//
// Nothing is guessed: a texture that does not decode is reported and left out; the counts are printed.
#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "assets/archive.h"
#include "assets/chunk_stream.h"
#include "assets/geometry_import.h"
#include "assets/texture_dictionary.h"
#include "assets/texture_import.h"

using namespace shadow;

namespace {

constexpr std::uint32_t kVersion = 0x36003u;   // the library version the converted chunks carry (3.6.0.3, build 0x37)
constexpr std::uint32_t kHostPlatform = 0x54534F48u;
constexpr std::uint32_t libraryID(std::uint32_t v) {
    return (((v - 0x30000u) << 14) & 0xFFC00000u) | ((v & 0x3Fu) << 16) | 0x37u;
}

/// A RenderWare stream being written: chunks whose sizes are patched when they close.
struct Writer {
    std::vector<std::uint8_t> bytes;
    std::vector<std::size_t> open;
    void u32(std::uint32_t w) { for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(w >> (8 * i))); }
    void raw(const void* p, std::size_t n) {
        bytes.insert(bytes.end(), static_cast<const std::uint8_t*>(p), static_cast<const std::uint8_t*>(p) + n);
    }
    void begin(std::uint32_t type) {
        u32(type), u32(0), u32(libraryID(kVersion));
        open.push_back(bytes.size());
    }
    void end() {
        const std::size_t start = open.back();
        open.pop_back();
        const auto size = static_cast<std::uint32_t>(bytes.size() - start);
        for (int i = 0; i < 4; ++i) bytes[start - 8 + i] = static_cast<std::uint8_t>(size >> (8 * i));
    }
    bool save(const std::filesystem::path& path) const {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(out);
    }
};

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Every member of every ONE archive of the stage folder, expanded (kept alive: texture records point into them).
struct Members {
    std::vector<std::unique_ptr<std::vector<std::uint8_t>>> keep;
    struct Member {
        std::string archive, name;
        const std::vector<std::uint8_t>* bytes;
    };
    std::vector<Member> all;
};

bool loadMembers(const std::filesystem::path& folder, Members* out) {
    std::vector<std::filesystem::path> archives;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (upper(entry.path().extension().string()) == ".ONE") archives.push_back(entry.path());
    }
    std::sort(archives.begin(), archives.end());   // a fixed order: the output is the same on every run
    for (const auto& path : archives) {
        auto bytes = std::make_unique<std::vector<std::uint8_t>>(readFile(path));
        const assets::ByteView view{bytes->data(), bytes->size()};
        assets::OneArchive archive;
        std::string problem;
        if (!assets::parse_one_archive(view, &archive, &problem)) {
            std::fprintf(stderr, "  %s: not a ONE archive (%s)\n", path.filename().string().c_str(), problem.c_str());
            continue;
        }
        for (const assets::OneMember& member : archive.members) {
            auto expanded = std::make_unique<std::vector<std::uint8_t>>();
            if (!assets::read_one_member(view, archive, member, expanded.get(), &problem)) {
                std::fprintf(stderr, "  %s / %s: %s\n", path.filename().string().c_str(), member.name.c_str(), problem.c_str());
                continue;
            }
            out->all.push_back({path.filename().string(), member.name, expanded.get()});
            out->keep.push_back(std::move(expanded));
        }
        out->keep.push_back(std::move(bytes));
    }
    return true;
}

/// The stage's texture dictionary: every texture of every member's dictionaries, first of each name.
bool convertTextures(const Members& members, const std::filesystem::path& out, std::size_t* written) {
    std::map<std::string, assets::TextureRecord> records;   // by upper-case name, first wins
    std::vector<std::string> order;
    for (const Members::Member& m : members.all) {
        const assets::ByteView view{m.bytes->data(), m.bytes->size()};
        std::vector<assets::Chunk> top;
        if (!assets::read_stream(view, &top)) continue;
        for (const assets::Chunk& c : top) {
            if (c.type != assets::chunk::kTextureDictionary) continue;
            assets::TextureDictionary dictionary;
            std::string problem;
            if (!assets::parse_texture_dictionary(view, c, &dictionary, &problem)) {
                std::fprintf(stderr, "  %s / %s: dictionary: %s\n", m.archive.c_str(), m.name.c_str(), problem.c_str());
                continue;
            }
            for (assets::TextureRecord& t : dictionary.textures) {
                const std::string key = upper(t.name);
                if (records.emplace(key, std::move(t)).second) order.push_back(key);
            }
        }
    }
    Writer w;
    w.begin(0x16);   // rwID_TEXDICTIONARY
    w.begin(0x01);
    const std::size_t countAt = w.bytes.size();
    w.u32(0);        // {texture count, device id 0}: patched below
    w.end();
    std::uint32_t count = 0;
    for (const std::string& key : order) {
        const assets::TextureRecord& record = records.at(key);
        assets::TextureImage image;
        std::string problem;
        if (!assets::import_texture(record, assets::ImportOptions{}, &image, &problem)) {
            std::fprintf(stderr, "  texture %s: %s (left out)\n", record.name.c_str(), problem.c_str());
            continue;
        }
        w.begin(0x15);   // rwID_TEXTURENATIVE
        w.begin(0x01);
        w.u32(kHostPlatform);
        w.u32(record.filter_address);
        char name[32] = {}, mask[32] = {};
        std::strncpy(name, record.name.c_str(), 31);
        std::strncpy(mask, record.mask_name.c_str(), 31);
        w.raw(name, 32), w.raw(mask, 32);
        w.u32(image.width), w.u32(image.height), w.u32(image.mip_levels);
        for (std::uint32_t level = 0; level < image.mip_levels; ++level) {
            const std::size_t at = image.level_offsets[level];
            const std::size_t size = static_cast<std::size_t>(image.level_width(level)) * image.level_height(level) * 4u;
            w.u32(static_cast<std::uint32_t>(size));
            w.raw(image.rgba.data() + at, size);
        }
        w.end();
        w.begin(0x03), w.end();   // no plugin data
        w.end();
        ++count;
    }
    w.begin(0x03), w.end();
    w.end();
    for (int i = 0; i < 4; ++i) w.bytes[countAt + i] = static_cast<std::uint8_t>((count & 0xFFFFu) >> (8 * i));
    *written = count;
    return count <= 0xFFFFu && w.save(out);
}

// ---- worlds ---------------------------------------------------------------------------------------------------

std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
}

/// The land classes in the game's table order (tools/stage_preview/stage_scene.cpp, R2_MATERIAL_CENSUS.md 4.1): a
/// world member belongs to the first class token its name contains.
const char* const kClassTokens[] = {"_N_", "_D_", "_OS_", "_OSL_", "_O_", "_OW_", "_OL_", "_P_",
                                    "_AF_", "_A_", "_AL_", "_K_", "_M_", "_G_", "_GL_"};

struct WorldCounts {
    std::uint32_t vertices = 0, triangles = 0, sectors = 0, planes = 0;
};

/// One GameCube atomic sector rewritten as a platform-independent one: its struct (counts, its tight box kept), the
/// arrays the world's format asks for, triangles of the decoded draw lists, and an extension of its plugin chunks
/// without the GameCube's prepared data (0x510) and with a triangle-list mesh chunk (0x50E) in place of the original's.
bool writeSector(const assets::ByteView& bytes, const assets::Chunk& node, std::uint32_t format, Writer& w,
                 WorldCounts& counts, std::string* problem) {
    assets::WorldSector sector;
    if (!assets::import_sector(bytes, node, &sector, problem)) return false;
    const assets::MeshData& mesh = sector.mesh;
    const std::uint32_t nv = mesh.vertex_count, nt = static_cast<std::uint32_t>(mesh.indices.size() / 3);
    if (nv > 0xFFFFu || nt > 0xFFFFu) return *problem = "a sector beyond 65535 vertices or triangles", false;
    std::vector<assets::Chunk> parts;
    assets::children(bytes, node, &parts);
    const assets::Chunk* body = assets::find(parts, assets::chunk::kStruct);
    if (body == nullptr || body->size < 0x2C) return *problem = "a sector without its struct", false;
    w.begin(0x09);
    w.begin(0x01);
    w.u32(0), w.u32(nt), w.u32(nv);                       // matListWindowBase 0: triangles name world materials
    w.raw(bytes.data + body->body() + 12, 0x2C - 12);    // the tight box, collision flag, unused: as the original
    if (nv != 0) {   // the arrays follow the header inside the struct chunk
        for (float f : mesh.positions) w.u32(std::bit_cast<std::uint32_t>(f));
        if ((format & 0x10u) != 0) {   // normals the format promises: the prepared data has none - zero ones
            for (std::uint32_t v = 0; v < nv; ++v) w.u32(0);
        }
        if ((format & 0x08u) != 0) {
            if (mesh.colours.size() == std::size_t{nv} * 4) w.raw(mesh.colours.data(), mesh.colours.size());
            else for (std::uint32_t v = 0; v < nv; ++v) w.u32(0xFFFFFFFFu);
        }
        const std::uint32_t sets = (format & 0x00FF0000u) ? (format >> 16) & 0xFFu : (format & 0x80u) ? 2u : (format >> 2) & 1u;
        for (std::uint32_t set = 0; set < sets; ++set) {
            const std::vector<float>& uv = set == 0 ? mesh.uv0 : mesh.uv1;
            for (std::uint32_t v = 0; v < nv; ++v) {
                w.u32(std::bit_cast<std::uint32_t>(uv.size() == std::size_t{nv} * 2 ? uv[v * 2] : 0.0f));
                w.u32(std::bit_cast<std::uint32_t>(uv.size() == std::size_t{nv} * 2 ? uv[v * 2 + 1] : 0.0f));
            }
        }
    }
    for (const assets::MeshData::Submesh& sub : mesh.submeshes) {   // {v0, v1, v2, matIndex} as 16-bit values
        for (std::uint32_t i = 0; i < sub.index_count; i += 3) {
            const std::uint32_t* t = mesh.indices.data() + sub.first_index + i;
            const std::uint16_t words[4] = {static_cast<std::uint16_t>(t[0]), static_cast<std::uint16_t>(t[1]),
                                            static_cast<std::uint16_t>(t[2]), static_cast<std::uint16_t>(sub.material)};
            w.raw(words, 8);
        }
    }
    w.end();
    w.begin(0x03);
    if (const assets::Chunk* extension = assets::find(parts, assets::chunk::kExtension)) {
        std::vector<assets::Chunk> plugins;
        assets::children(bytes, *extension, &plugins);
        for (const assets::Chunk& c : plugins) {
            if (c.type == 0x50Eu || c.type == 0x510u) continue;
            w.raw(bytes.data + c.offset, 12 + std::size_t{c.size});
        }
    }
    w.begin(0x50E);   // the mesh: {flags 0 (triangle lists), meshes, indices}, per mesh {count, material, indices}
    w.u32(0), w.u32(static_cast<std::uint32_t>(mesh.submeshes.size())), w.u32(static_cast<std::uint32_t>(mesh.indices.size()));
    for (const assets::MeshData::Submesh& sub : mesh.submeshes) {
        w.u32(sub.index_count), w.u32(sub.material);
        for (std::uint32_t i = 0; i < sub.index_count; ++i) w.u32(mesh.indices[sub.first_index + i]);
    }
    w.end();
    w.end();
    w.end();
    counts.vertices += nv, counts.triangles += nt, ++counts.sectors;
    return true;
}

bool writeTree(const assets::ByteView& bytes, const assets::Chunk& node, std::uint32_t format, Writer& w, WorldCounts& counts,
               std::string* problem, int depth) {
    if (depth > 4096) return *problem = "a sector tree beyond 4096 levels", false;
    if (node.type == assets::chunk::kAtomicSector) return writeSector(bytes, node, format, w, counts, problem);
    std::vector<assets::Chunk> kids;
    if (!assets::children(bytes, node, &kids)) return *problem = "a plane sector with a broken child run", false;
    w.begin(0x0A);
    ++counts.planes;
    for (const assets::Chunk& kid : kids) {
        if (kid.type == assets::chunk::kAtomicSector || kid.type == assets::chunk::kPlaneSector) {
            if (!writeTree(bytes, kid, format, w, counts, problem, depth + 1)) return false;
        } else {
            w.raw(bytes.data + kid.offset, 12 + std::size_t{kid.size});   // its struct (and anything else) as it is
        }
    }
    w.end();
    return true;
}

/// One world chunk, converted; appended to w.
bool convertWorld(const assets::ByteView& bytes, const assets::Chunk& world, Writer& w, WorldCounts* out, std::string* problem) {
    std::vector<assets::Chunk> parts;
    if (!assets::children(bytes, world, &parts)) return *problem = "a world with a broken child run", false;
    const assets::Chunk* body = assets::find(parts, assets::chunk::kStruct);
    if (body == nullptr || body->size < 0x40) return *problem = "a world without its struct", false;
    std::uint32_t wc[16];
    for (int i = 0; i < 16; ++i) wc[i] = le32(bytes.data + body->body() + 4 * i);
    const std::uint32_t format = wc[9] & ~0x01000000u;   // no longer rpWORLDNATIVE
    Writer tree;
    WorldCounts counts;
    for (const assets::Chunk& c : parts) {
        if (c.type == assets::chunk::kStruct) continue;
        if (c.type == assets::chunk::kAtomicSector || c.type == assets::chunk::kPlaneSector) {
            if (!writeTree(bytes, c, format, tree, counts, problem, 0)) return false;
        } else {
            tree.raw(bytes.data + c.offset, 12 + std::size_t{c.size});   // the material list, the extension: as they are
        }
    }
    w.begin(0x0B);
    w.begin(0x01);
    wc[4] = counts.triangles, wc[5] = counts.vertices, wc[9] = format;
    if (wc[6] != counts.planes || wc[7] != counts.sectors) {
        return *problem = "the world's sector counts do not match its tree", false;
    }
    for (std::uint32_t word : wc) w.u32(word);
    w.raw(bytes.data + body->body() + 0x40, body->size - 0x40);
    w.end();
    w.raw(tree.bytes.data(), tree.bytes.size());
    w.end();
    *out = counts;
    return true;
}

/// Every land world of the stage (each archive's members by the class rule), into one stream, and a manifest line
/// per world: "archive member class".
bool convertWorlds(const Members& members, const std::filesystem::path& out, const std::filesystem::path& manifest,
                   std::size_t* worlds, WorldCounts* total) {
    Writer w;
    std::ofstream list(manifest);
    std::map<std::string, std::vector<const Members::Member*>> byArchive;
    std::vector<std::string> archives;
    for (const Members::Member& m : members.all) {
        if (byArchive[m.archive].empty()) archives.push_back(m.archive);
        byArchive[m.archive].push_back(&m);
    }
    for (const std::string& archive : archives) {
        for (const char* token : kClassTokens) {
            for (const Members::Member* m : byArchive[archive]) {
                if (upper(m->name).find(token) == std::string::npos) continue;
                const assets::ByteView view{m->bytes->data(), m->bytes->size()};
                std::vector<assets::Chunk> top;
                if (assets::read_stream(view, &top)) {
                    for (const assets::Chunk& c : top) {
                        if (c.type != assets::chunk::kWorld) continue;
                        WorldCounts counts;
                        std::string problem;
                        if (!convertWorld(view, c, w, &counts, &problem)) {
                            std::fprintf(stderr, "  %s / %s: %s\n", archive.c_str(), m->name.c_str(), problem.c_str());
                            return false;
                        }
                        list << archive << ' ' << m->name << ' ' << token << '\n';
                        ++*worlds;
                        total->vertices += counts.vertices, total->triangles += counts.triangles;
                        total->sectors += counts.sectors, total->planes += counts.planes;
                    }
                }
                break;   // the first member of the class, as the game takes it
            }
        }
    }
    return w.save(out) && static_cast<bool>(list);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: stage_convert <files directory> <stage> <output directory>\n");
        return 2;
    }
    const std::filesystem::path files = std::filesystem::u8path(argv[1]);
    const std::string stage = argv[2];
    const std::filesystem::path out = std::filesystem::u8path(argv[3]);
    std::filesystem::create_directories(out);
    Members members;
    if (!loadMembers(files / stage, &members)) return 1;
    std::printf("%s: %zu archive members\n", stage.c_str(), members.all.size());
    std::size_t textures = 0;
    if (!convertTextures(members, out / (stage + ".txd"), &textures)) {
        std::fprintf(stderr, "could not write the texture dictionary\n");
        return 1;
    }
    std::printf("  %s.txd: %zu textures\n", stage.c_str(), textures);
    std::size_t worlds = 0;
    WorldCounts total;
    if (!convertWorlds(members, out / (stage + ".wld"), out / (stage + ".worlds"), &worlds, &total)) {
        std::fprintf(stderr, "could not convert the worlds\n");
        return 1;
    }
    std::printf("  %s.wld: %zu land worlds, %u sectors, %u plane sectors, %u vertices, %u triangles\n", stage.c_str(), worlds,
                total.sectors, total.planes, total.vertices, total.triangles);
    const std::filesystem::path light = files / stage / (stage + "_light.bin");
    const std::vector<std::uint8_t> lights = readFile(light);
    if (lights.size() == 0x340u) {
        std::ofstream(out / (stage + ".light"), std::ios::binary).write(reinterpret_cast<const char*>(lights.data()), 0x340);
        std::printf("  %s.light: 16 light sets\n", stage.c_str());
    } else {
        std::fprintf(stderr, "  %s: %zu bytes, not the 16 light sets of 0x34 bytes the game reads\n",
                     light.filename().string().c_str(), lights.size());
    }
    return 0;
}
