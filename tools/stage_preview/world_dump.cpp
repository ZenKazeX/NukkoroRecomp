// Every world of a stage folder, straight from the disc, as text: one OBJ per stage with an object per world sector
// (archive member and sector index), its positions and UVs, and a group per submesh named by its material's texture.
// Used to measure how the levels lay one surface on another (docs/RENDER_FLICKER.md: the corridor lights against the
// painted slots of their walls).
//
//     world_dump <game files folder> <out folder> [stage ...]     (no stage: every stgNNNN folder)

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "assets/archive.h"
#include "assets/chunk_stream.h"
#include "assets/geometry_import.h"

namespace assets = shadow::assets;

namespace {

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void dump_stage(const std::filesystem::path& folder, const std::filesystem::path& out_path) {
    std::FILE* out = std::fopen(out_path.string().c_str(), "w");
    if (out == nullptr)
        return;
    std::size_t base = 1, worlds = 0;
    std::string problem;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (entry.path().extension() != ".one" && entry.path().extension() != ".ONE")
            continue;
        const std::vector<std::uint8_t> archive_bytes = read_file(entry.path());
        const assets::ByteView archive_view{archive_bytes.data(), archive_bytes.size()};
        assets::OneArchive archive;
        if (!assets::parse_one_archive(archive_view, &archive, &problem))
            continue;
        for (const auto& member : archive.members) {
            std::vector<std::uint8_t> bytes;
            if (!assets::read_one_member(archive_view, archive, member, &bytes, &problem))
                continue;
            const assets::ByteView view{bytes.data(), bytes.size()};
            std::vector<assets::Chunk> top;
            if (!assets::read_stream(view, &top))
                continue;
            for (const assets::Chunk& c : top) {
                if (c.type != assets::chunk::kWorld)
                    continue;
                assets::World world;
                if (!assets::import_world(view, c, &world, &problem))
                    continue;
                ++worlds;
                for (std::size_t s = 0; s < world.sectors.size(); ++s) {
                    const assets::MeshData& mesh = world.sectors[s].mesh;
                    if (mesh.vertex_count == 0)
                        continue;
                    std::fprintf(out, "o %s/%s/%zu\n", entry.path().filename().string().c_str(), member.name.c_str(), s);
                    for (std::size_t i = 0; i + 2 < mesh.positions.size(); i += 3)
                        std::fprintf(out, "v %.9g %.9g %.9g\n", mesh.positions[i], mesh.positions[i + 1],
                                     mesh.positions[i + 2]);
                    for (std::uint32_t i = 0; i < mesh.vertex_count; ++i) {
                        const bool has = mesh.uv0.size() >= 2u * (i + 1u);
                        std::fprintf(out, "vt %.9g %.9g\n", has ? mesh.uv0[2 * i] : 0.0f, has ? mesh.uv0[2 * i + 1] : 0.0f);
                    }
                    for (const auto& sub : mesh.submeshes) {
                        const std::string texture = sub.material < world.materials.size() &&
                                                            world.materials[sub.material].texture.present
                                                        ? world.materials[sub.material].texture.name
                                                        : std::string("-");
                        std::fprintf(out, "g %u %s\n", sub.material, texture.c_str());
                        for (std::uint32_t k = sub.first_index; k + 2 < sub.first_index + sub.index_count; k += 3)
                            std::fprintf(out, "f %zu %zu %zu\n", base + mesh.indices[k], base + mesh.indices[k + 1],
                                         base + mesh.indices[k + 2]);
                    }
                    base += mesh.vertex_count;
                }
            }
        }
    }
    std::fclose(out);
    std::printf("%s: %zu worlds\n", folder.filename().string().c_str(), worlds);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: world_dump <game files folder> <out folder> [stage ...]\n");
        return 2;
    }
    const std::filesystem::path files = argv[1], out = argv[2];
    std::filesystem::create_directories(out);
    std::vector<std::string> stages;
    for (int i = 3; i < argc; ++i)
        stages.push_back(argv[i]);
    if (stages.empty())
        for (const auto& entry : std::filesystem::directory_iterator(files))
            if (entry.is_directory() && entry.path().filename().string().rfind("stg", 0) == 0)
                stages.push_back(entry.path().filename().string());
    for (const std::string& stage : stages)
        dump_stage(files / stage, out / (stage + ".obj"));
    return 0;
}
