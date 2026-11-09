// Tests of the asset importers (docs/lift-20261006/R2_DRIVER.md 2.3; R_DESIGN.md 2.6): PRS, ONE archives,
// RenderWare chunk streams, texture dictionaries, texel decoding and mip generation.
//
// With the game data present (argument: sys/main.dol), every file of the disc is walked: every ONE member expands
// to its declared size and identically to the current PRS decoder; every texture dictionary parses; every texture
// decodes byte for byte as the current decoder decodes it (the oracle, retired with the old renderer); the totals
// match the material census (R2_MATERIAL_CENSUS.md: 1,091 dictionaries, 10,275 textures).

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "assets/archive.h"
#include "assets/chunk_stream.h"
#include "assets/geometry_import.h"
#include "assets/light_slots.h"
#include "assets/texel_decode.h"
#include "assets/texture_dictionary.h"
#include "assets/texture_import.h"

#include "gekko/gx_texture_decode.h"
#include "gekko/native_assets.h"

namespace {

using namespace shadow::assets;

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

// ---- synthetic -------------------------------------------------------------------------------------------------

void cmpr_block_colours() {
    // One 8x8 tile: four blocks. Block 0 (top-left): c0 = pure red > c1 = pure blue, selectors 0,1,2,3 on row 0.
    std::vector<std::uint8_t> tile(32, 0);
    tile[0] = 0xF8; tile[1] = 0x00;          // c0 = RGB565 red (0xF800)
    tile[2] = 0x00; tile[3] = 0x1F;          // c1 = RGB565 blue (0x001F)
    tile[4] = 0x1B;                          // row 0: 00 01 10 11 -> texels 0,1,2,3 take colours 0,1,2,3
    // Block 1 (top-right): c0 <= c1 -> three colours + averaged colour with alpha 0.
    tile[8] = 0x00; tile[9] = 0x1F;          // c0 = blue
    tile[10] = 0xF8; tile[11] = 0x00;        // c1 = red
    tile[12] = 0xFF;                         // row 0: all selector 3
    std::vector<std::uint8_t> rgba(8 * 8 * 4);
    std::string error;
    check(decode_level(TexelEncoding::CMPR, 8, 8, ByteView{tile.data(), tile.size()}, {}, PaletteEncoding::IA8,
                       rgba.data(), &error), "CMPR decodes: " + error);
    auto at = [&](int x, int y) { return rgba.data() + (y * 8 + x) * 4; };
    check(at(0, 0)[0] == 255 && at(0, 0)[2] == 0 && at(0, 0)[3] == 255, "selector 0 = c0");
    check(at(1, 0)[0] == 0 && at(1, 0)[2] == 255, "selector 1 = c1");
    check(at(2, 0)[0] == (5 * 255 + 3 * 0) >> 3 && at(2, 0)[2] == (5 * 0 + 3 * 255) >> 3,
          "selector 2 = the 5/8 - 3/8 blend");
    check(at(3, 0)[0] == (3 * 255) >> 3 && at(3, 0)[2] == (5 * 255) >> 3, "selector 3 = the 3/8 - 5/8 blend");
    check(at(4, 0)[0] == 127 && at(4, 0)[2] == 127 && at(4, 0)[3] == 0,
          "three-colour mode: entry 3 is the averaged colour with alpha 0");
}

void indexed_tiles() {
    // C4, 8x8: one tile, row-major nibbles, high nibble first. Palette RGB5A3: entry 1 opaque white, entry 2 alpha.
    std::vector<std::uint8_t> tile(32, 0);
    tile[0] = 0x12;                          // texel (0,0) = 1, texel (1,0) = 2
    tile[4] = 0x20;                          // texel (0,1) = 2
    std::vector<std::uint16_t> palette(16, 0);
    palette[1] = 0xFFFF;                     // opaque white
    palette[2] = 0x3F00;                     // alpha 3 of 7, red 15
    std::vector<std::uint8_t> rgba(8 * 8 * 4);
    std::string error;
    check(decode_level(TexelEncoding::C4, 8, 8, ByteView{tile.data(), tile.size()}, palette, PaletteEncoding::RGB5A3,
                       rgba.data(), &error), "C4 decodes: " + error);
    check(rgba[0] == 255 && rgba[3] == 255, "C4 (0,0) -> opaque white");
    check(rgba[4] == 255 && rgba[7] == ((3 << 5) | (3 << 2) | (3 >> 1)), "C4 (1,0) -> red with 3-bit alpha 3");
    check(rgba[8 * 4 + 0] == 255 && rgba[8 * 4 + 3] == rgba[7], "C4 (0,1) is in the tile's second row");
}

void mip_chain_ignores_transparent_colour() {
    TextureImage image;
    image.width = 2;
    image.height = 2;
    image.mip_levels = 1;
    image.level_offsets = {0};
    // Three opaque mid-grey texels and one transparent magenta: the 1x1 level stays grey, alpha 3/4.
    image.rgba = {128, 128, 128, 255, 128, 128, 128, 255, 128, 128, 128, 255, 255, 0, 255, 0};
    generate_mip_chain(image);
    check(image.mip_levels == 2 && image.level_offsets.size() == 2, "a 2x2 image gets one more level");
    const std::uint8_t* m = image.rgba.data() + image.level_offsets[1];
    check(m[0] == 128 && m[1] == 128 && m[2] == 128, "transparent texels lend no colour to the mip");
    check(m[3] == 191, "alpha averages to 3/4");
}

// ---- the disc -------------------------------------------------------------------------------------------------------

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

struct DiscTotals {
    std::size_t files = 0, archives = 0, members = 0, prs_members = 0, streams = 0;
    std::size_t dictionaries = 0, textures = 0, mips_made = 0;
    std::size_t has_alpha_word_set = 0, texel_alpha = 0, has_alpha_disagrees = 0;
    std::map<std::string, std::size_t> by_encoding;
    std::uint64_t level0_texels = 0;
    // geometry
    std::size_t clumps = 0, geometries = 0, skins = 0, delta_morph = 0, geometry_triangles = 0;
    std::map<std::uint32_t, std::size_t> skin_max_weights;
    std::size_t prepared_worlds = 0, other_worlds = 0, sectors = 0, sectors_with_data = 0, plane_sectors = 0;
    std::size_t strips = 0, strip_vertices = 0, world_vertices = 0, world_triangles = 0;
    std::map<std::uint32_t, std::size_t> sector_attributes;
    std::size_t collision_worlds = 0, collision_worlds_without_meshes = 0, independent_sectors = 0,
                independent_triangles = 0;
    std::vector<std::size_t> independent_triangle_list;
    std::size_t delta_targets = 0, delta_targets_with_normals = 0;
    // materials of model geometries
    std::size_t model_materials = 0, ka_not_one = 0, colour_white = 0, colour_other = 0, colour_alpha = 0;
    std::size_t environment_slots = 0, environment_fb_alpha = 0;
    std::map<std::uint32_t, std::size_t> effects;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> dual_pairs;
};

bool mesh_is_consistent(const MeshData& mesh) {
    for (std::uint32_t index : mesh.indices) {
        if (index >= mesh.vertex_count)
            return false;
    }
    std::size_t covered = 0;
    for (const MeshData::Submesh& sub : mesh.submeshes) {
        if (sub.first_index != covered || sub.index_count % 3 != 0)
            return false;
        covered += sub.index_count;
    }
    return covered == mesh.indices.size() && mesh.positions.size() == std::size_t{mesh.vertex_count} * 3 &&
           (mesh.colours.empty() || mesh.colours.size() == std::size_t{mesh.vertex_count} * 4) &&
           (mesh.uv0.empty() || mesh.uv0.size() == std::size_t{mesh.vertex_count} * 2) &&
           (mesh.uv1.empty() || mesh.uv1.size() == std::size_t{mesh.vertex_count} * 2);
}

void check_geometry_chunks(const std::string& where, ByteView bytes, const Chunk& c, DiscTotals& totals) {
    std::string error;
    if (c.type == chunk::kClump) {
        ++totals.clumps;
        std::vector<Chunk> parts;
        if (!children(bytes, c, &parts)) {
            check(false, where + ": clump with a broken child run");
            return;
        }
        const Chunk* list = find(parts, chunk::kGeometryList);
        std::vector<Chunk> geometries;
        if (list == nullptr || !children(bytes, *list, &geometries))
            return;
        for (const Chunk& g : geometries) {
            if (g.type != chunk::kGeometry)
                continue;
            ModelGeometry model;
            if (!import_geometry(bytes, g, &model, &error)) {
                check(false, where + ": geometry: " + error);
                continue;
            }
            ++totals.geometries;
            totals.geometry_triangles += model.mesh.indices.size() / 3;
            check(mesh_is_consistent(model.mesh), where + ": geometry mesh is consistent");
            if (model.skinned) {
                ++totals.skins;
                ++totals.skin_max_weights[model.skin.max_weights];
                check(model.mesh.skin_weights.size() == std::size_t{model.mesh.vertex_count} * 4,
                      where + ": skin weights per vertex");
            }
            if (model.delta_morph)
                ++totals.delta_morph;
            for (const DeltaMorphTarget& target : model.delta_targets) {
                ++totals.delta_targets;
                if (!target.normals.empty())
                    ++totals.delta_targets_with_normals;
                check(target.positions.size() == std::size_t{model.mesh.vertex_count} * 3,
                      where + ": delta-morph target '" + target.name + "' covers every vertex");
            }
            for (const MaterialRecord& m : model.materials) {
                ++totals.model_materials;
                if (m.ambient != 1.0f)
                    ++totals.ka_not_one;
                if (m.colour[3] < 255)
                    ++totals.colour_alpha;
                else if (m.colour[0] == 255 && m.colour[1] == 255 && m.colour[2] == 255)
                    ++totals.colour_white;
                else
                    ++totals.colour_other;
                if (m.effect != EffectKind::None)
                    ++totals.effects[static_cast<std::uint32_t>(m.effect)];
                for (std::uint32_t s = 0; s < m.slot_count; ++s) {
                    if (m.slots[s].type == 2) {
                        ++totals.environment_slots;
                        if (m.slots[s].framebuffer_alpha)
                            ++totals.environment_fb_alpha;
                    } else if (m.slots[s].type == 4) {
                        ++totals.dual_pairs[{m.slots[s].src_blend, m.slots[s].dst_blend}];
                    }
                }
            }
        }
    } else if (c.type == chunk::kWorld) {
        World world;
        std::string upper_where = where;
        for (char& ch : upper_where)
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        const bool collision = upper_where.find(".BSP") != std::string::npos;
        if (!import_world(bytes, c, &world, &error)) {
            if (collision) {
                ++totals.collision_worlds_without_meshes;  // collision data, never drawn (census 4.3)
                return;
            }
            check(false, where + ": world: " + error);
            return;
        }
        if ((world.format & 0x01000000u) == 0) {
            ++totals.other_worlds;
            if (collision) {
                ++totals.collision_worlds;
                return;
            }
            for (const WorldSector& sector : world.sectors) {
                ++totals.independent_sectors;
                totals.independent_triangles += sector.mesh.indices.size() / 3;
                check(mesh_is_consistent(sector.mesh), where + ": independent sector mesh is consistent");
            }
            totals.independent_triangle_list.push_back(world.sectors.empty() ? 0 : world.sectors[0].mesh.indices.size() / 3);
            return;
        }
        ++totals.prepared_worlds;
        totals.plane_sectors += world.plane_sectors;
        for (const WorldSector& sector : world.sectors) {
            ++totals.sectors;
            if (sector.mesh.vertex_count == 0)
                continue;
            ++totals.sectors_with_data;
            ++totals.sector_attributes[sector.attributes];
            totals.strips += sector.strips;
            totals.strip_vertices += sector.strip_vertices;
            totals.world_vertices += sector.mesh.vertex_count;
            totals.world_triangles += sector.mesh.indices.size() / 3;
            check(mesh_is_consistent(sector.mesh), where + ": sector mesh is consistent");
        }
    }
}

void check_stream(const std::string& where, ByteView bytes, DiscTotals& totals) {
    std::vector<Chunk> top;
    if (!read_stream(bytes, &top))
        return;
    ++totals.streams;
    for (const Chunk& c : top) {
        check_geometry_chunks(where, bytes, c, totals);
        if (c.type != chunk::kTextureDictionary)
            continue;
        TextureDictionary dictionary;
        std::string error;
        if (!parse_texture_dictionary(bytes, c, &dictionary, &error)) {
            check(false, where + ": " + error);
            continue;
        }
        ++totals.dictionaries;
        for (const TextureRecord& t : dictionary.textures) {
            ++totals.textures;
            ++totals.by_encoding[std::string(encoding_name(t.encoding)) +
                                 (is_indexed(t.encoding) ? (t.palette_encoding == PaletteEncoding::RGB565 ? "+RGB565"
                                                            : t.palette_encoding == PaletteEncoding::RGB5A3 ? "+RGB5A3"
                                                                                                            : "+IA8")
                                                         : "")];
            const std::size_t level0 = level_bytes(t.encoding, t.width, t.height);
            std::vector<std::uint8_t> mine(static_cast<std::size_t>(t.width) * t.height * 4);
            std::vector<std::uint8_t> oracle(mine.size(), 0xCD);
            const bool decoded = decode_level(t.encoding, t.width, t.height, t.texels.slice(0, level0), t.palette,
                                              t.palette_encoding, mine.data(), &error);
            std::vector<std::uint8_t> palette_be(t.palette.size() * 2);
            for (std::size_t i = 0; i < t.palette.size(); ++i) {
                palette_be[i * 2] = static_cast<std::uint8_t>(t.palette[i] >> 8);
                palette_be[i * 2 + 1] = static_cast<std::uint8_t>(t.palette[i]);
            }
            const bool old = gekko::gx_decode_texture(
                static_cast<gekko::u32>(t.encoding), t.width, t.height, t.texels.data, level0,
                palette_be.empty() ? nullptr : palette_be.data(), static_cast<gekko::u32>(t.palette_encoding),
                static_cast<gekko::u32>(t.palette.size()), oracle.data());
            check(decoded && old && mine == oracle, where + ": texture '" + t.name + "' (" + encoding_name(t.encoding) +
                                                        ") decodes as the current decoder does");
            totals.level0_texels += static_cast<std::uint64_t>(t.width) * t.height;
            if (t.has_alpha != 0)
                ++totals.has_alpha_word_set;
            TextureImage image;
            ImportOptions options;
            if (import_texture(t, options, &image, &error)) {
                totals.mips_made += image.mip_levels - 1;
                if (image.texel_alpha)
                    ++totals.texel_alpha;
                if ((t.has_alpha != 0) != image.texel_alpha)
                    ++totals.has_alpha_disagrees;
                std::uint32_t expected_levels = 1;
                for (std::uint32_t s = std::max<std::uint32_t>(t.width, t.height); s > 1; s >>= 1)
                    ++expected_levels;
                check(image.mip_levels == expected_levels, where + ": '" + t.name + "' has a full mip chain");
            } else {
                check(false, where + ": import '" + t.name + "': " + error);
            }
        }
    }
}

void walk_disc(const std::filesystem::path& files) {
    const auto start = std::chrono::steady_clock::now();
    DiscTotals totals;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(files)) {
        if (!entry.is_regular_file())
            continue;
        const std::string ext = entry.path().extension().string();
        if (ext == ".adx" || ext == ".ADX" || ext == ".sfd" || ext == ".SFD" || ext == ".afs" || ext == ".AFS")
            continue;  // audio and movie streams
        ++totals.files;
        const std::vector<std::uint8_t> data = read_file(entry.path());
        const ByteView bytes{data.data(), data.size()};
        const std::string where = std::filesystem::relative(entry.path(), files).generic_string();
        if (is_one_archive(bytes)) {
            ++totals.archives;
            OneArchive archive;
            std::string error;
            if (!parse_one_archive(bytes, &archive, &error)) {
                check(false, where + ": " + error);
                continue;
            }
            for (const OneMember& member : archive.members) {
                ++totals.members;
                std::vector<std::uint8_t> expanded;
                if (!read_one_member(bytes, archive, member, &expanded, &error)) {
                    check(false, where + ":" + member.name + ": " + error);
                    continue;
                }
                if ((member.flags & 1u) != 0) {
                    ++totals.prs_members;
                    // The current decoder (the guest's own algorithm, transcribed) is the oracle.
                    std::vector<gekko::u8> oracle;
                    const std::size_t start_offset = static_cast<std::size_t>(member.offset) + 0x0C;
                    const bool ok = gekko::prs_expand(data.data() + start_offset, data.size() - start_offset,
                                                      member.size, oracle);
                    check(ok && oracle == expanded, where + ":" + member.name + ": PRS output equals the oracle's");
                }
                check_stream(where + ":" + member.name, ByteView{expanded.data(), expanded.size()}, totals);
            }
        } else {
            check_stream(where, bytes, totals);
        }
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("disc: %zu files, %zu ONE archives, %zu members (%zu PRS), %zu RenderWare streams, %zu texture "
                "dictionaries, %zu textures (%.1f M texels), %zu mip levels generated, %.1f s\n",
                totals.files, totals.archives, totals.members, totals.prs_members, totals.streams,
                totals.dictionaries, totals.textures, totals.level0_texels / 1e6, totals.mips_made, seconds);
    for (const auto& [encoding, count] : totals.by_encoding)
        std::printf("  %-14s %zu\n", encoding.c_str(), count);
    std::printf("  has-alpha word set on %zu textures; texel alpha on %zu; they disagree on %zu\n",
                totals.has_alpha_word_set, totals.texel_alpha, totals.has_alpha_disagrees);
    check(totals.dictionaries == 1091, "1,091 texture dictionaries, as the census counted");
    check(totals.textures == 10275, "10,275 textures, as the census counted");

    std::printf("models: %zu clumps, %zu geometries (%zu triangles), %zu skins, %zu with delta morphs; skin max "
                "weights:", totals.clumps, totals.geometries, totals.geometry_triangles, totals.skins,
                totals.delta_morph);
    for (const auto& [weights, count] : totals.skin_max_weights)
        std::printf(" %u:%zu", weights, count);
    std::printf("\nworlds: %zu prepared (%zu other), %zu atomic sectors (%zu with data), %zu plane sectors, %zu strips, "
                "%zu strip vertices -> %zu vertices, %zu triangles; attributes:",
                totals.prepared_worlds, totals.other_worlds, totals.sectors, totals.sectors_with_data,
                totals.plane_sectors, totals.strips, totals.strip_vertices, totals.world_vertices,
                totals.world_triangles);
    for (const auto& [mask, count] : totals.sector_attributes)
        std::printf(" 0x%X:%zu", mask, count);
    std::printf("\n");
    // R2_MATERIAL_CENSUS.md 2.1, 3, 4.3.
    check(totals.clumps == 5231 && totals.geometries == 11682, "5,231 clumps and 11,682 geometries, as the census");
    check(totals.skins == 628 && totals.skin_max_weights[1] == 15 && totals.skin_max_weights[2] == 150 &&
              totals.skin_max_weights[3] == 168 && totals.skin_max_weights[4] == 295,
          "628 skins with max weights 15 / 150 / 168 / 295, as the census");
    check(totals.delta_morph == 545, "545 delta-morph geometries, as the census");
    std::printf("delta-morph targets: %zu (%zu with normals); independent worlds: %zu (collision %zu, %zu of them "
                "without draw meshes), %zu drawable sectors, %zu triangles:",
                totals.delta_targets, totals.delta_targets_with_normals, totals.other_worlds + totals.collision_worlds_without_meshes,
                totals.collision_worlds + totals.collision_worlds_without_meshes, totals.collision_worlds_without_meshes,
                totals.independent_sectors, totals.independent_triangles);
    for (std::size_t n : totals.independent_triangle_list)
        std::printf(" %zu", n);
    std::printf("\n");
    // DMorph target flags on the disc (census out/dmorph.txt): 0x12 (positions + normals) 2,223, 0x02 16.
    check(totals.delta_targets == 2239 && totals.delta_targets_with_normals == 2223,
          "2,239 delta-morph targets, 2,223 with normals, as the census");
    std::vector<std::size_t> rp2 = totals.independent_triangle_list;
    std::sort(rp2.begin(), rp2.end());
    check(rp2 == std::vector<std::size_t>{82, 128, 214}, "the three RP2 worlds: 82 / 128 / 214 triangles, as the census");
    std::printf("model materials: %zu (white %zu, other colour %zu, alpha < 255 %zu, ka != 1 %zu); effects:",
                totals.model_materials, totals.colour_white, totals.colour_other, totals.colour_alpha,
                totals.ka_not_one);
    for (const auto& [effect, count] : totals.effects)
        std::printf(" %u:%zu", effect, count);
    std::printf("; environment slots %zu (framebuffer alpha %zu); dual blend pairs:", totals.environment_slots,
                totals.environment_fb_alpha);
    for (const auto& [pair, count] : totals.dual_pairs)
        std::printf(" %u/%u:%zu", pair.first, pair.second, count);
    std::printf("\n");
    // R2_MATERIAL_CENSUS.md 2.4-2.5 (blend enumeration: 1 ZERO, 2 ONE, 5 SRCALPHA, 6 INVSRCALPHA).
    check(totals.model_materials == 17430, "17,430 model materials, as the census");
    check(totals.colour_white == 14316 && totals.colour_other == 3102 && totals.colour_alpha == 12,
          "material colours 14,316 white / 3,102 other / 12 with alpha, as the census");
    check(totals.ka_not_one == 0, "ka = 1.0 on every material, as the census");
    check(totals.effects[2] == 4837 && totals.effects[5] == 552 && totals.effects[4] == 310 &&
              totals.effects[6] == 75 && totals.effects[3] == 2 && totals.effects[1] == 0,
          "effects ENVMAP 4,837 / UVTRANSFORM 552 / DUAL 310 / DUALUVTRANSFORM 75 / BUMPENVMAP 2, as the census");
    check(totals.environment_slots == 4839 && totals.environment_fb_alpha == 0,
          "4,839 environment slots, none blending by framebuffer alpha, as the census");
    check(totals.dual_pairs.size() == 4 && totals.dual_pairs.count({5, 6}) && totals.dual_pairs.count({5, 2}) &&
              totals.dual_pairs.count({1, 5}) && totals.dual_pairs.count({1, 2}),
          "exactly the 4 dual blend pairs of the census");
    check(totals.sectors == 4890 && totals.sectors_with_data == 3092 && totals.plane_sectors == 3265,
          "4,890 atomic sectors (3,092 with data) and 3,265 plane sectors, as the census");
    check(totals.strips == 411955 && totals.strip_vertices == 2788379,
          "411,955 strips with 2,788,379 strip vertices, as the census");
    check(totals.sector_attributes[0x7] == 2793 && totals.sector_attributes[0xF] == 276 &&
              totals.sector_attributes[0x3] == 12 && totals.sector_attributes[0x5] == 2 &&
              totals.sector_attributes[0x1] == 9,
          "sector attribute sets as the census: POS CLR0 TEX0 2,793 / +TEX1 276 / POS CLR0 12 / POS TEX0 2 / POS 9");
}


// The corridor-light table (assets/light_slots.h): moves and adds touch only the marked submeshes, an added light
// copies its template's attributes and closes the template's submesh, the later submeshes move up.
void light_slots_move_and_add() {
    MeshData mesh;
    mesh.vertex_count = 5;
    mesh.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, /* other submesh */ 0, 0, 0};
    mesh.uv0 = {0, 0, 1, 0, 1, 1, 0, 1, 9, 9};
    mesh.indices = {0, 1, 2, 0, 2, 3, 4, 4, 4};
    mesh.submeshes = {{0, 6, 0}, {6, 3, 1}};
    const LightSlotMove moves[] = {{{1, 0, 0}, {2, 0, 0}}, {{0, 0, 0}, {-1, 0, 0}}};
    LightSlotAdd add{};
    add.vertex_count = 4;
    const float from[4][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const float to[4][3] = {{5, 0, 0}, {6, 0, 0}, {6, 1, 0}, {5, 1, 0}};
    std::memcpy(add.from, from, sizeof(from));
    std::memcpy(add.to, to, sizeof(to));
    add.triangle_count = 2;
    const std::uint32_t tris[2][3] = {{0, 1, 2}, {0, 2, 3}};
    std::memcpy(add.triangles, tris, sizeof(tris));
    const LightSlotResult r = fix_light_slots(mesh, {true, false}, moves, 2, &add, 1);
    check(r.added_lights == 1 && r.moved_vertices == 2, "light slots: one light added, two vertices moved");
    check(mesh.vertex_count == 9 && mesh.positions[9 * 3 - 3] == 5 && mesh.uv0[8 * 2 + 1] == 1,
          "light slots: the added light's positions are the table's, its UVs the template's");
    check(mesh.positions[0] == -1 && mesh.positions[3] == 2 && mesh.positions[12] == 0,
          "light slots: the marked submesh moved, the other submesh's vertex at the same place did not");
    check(mesh.submeshes[0].index_count == 12 && mesh.submeshes[1].first_index == 12 && mesh.indices[12] == 4,
          "light slots: the added triangles close the template's submesh, the next submesh moves up");
}

// GUN Fortress on the disc: the table finds every light it was generated from (tools/assets/light_slots.py).
void light_slots_match_the_disc(const std::filesystem::path& files) {
    const std::filesystem::path stage = files / "stg0600";
    if (!std::filesystem::exists(stage))
        return;
    LightSlotResult total;
    for (const auto& entry : std::filesystem::directory_iterator(stage)) {
        if (entry.path().extension() != ".one")
            continue;
        const std::vector<std::uint8_t> data = read_file(entry.path());
        const ByteView bytes{data.data(), data.size()};
        OneArchive archive;
        std::string error;
        if (!parse_one_archive(bytes, &archive, &error))
            continue;
        for (const OneMember& member : archive.members) {
            if (member.name.find("_G_") == std::string::npos)
                continue;
            std::vector<std::uint8_t> expanded;
            if (!read_one_member(bytes, archive, member, &expanded, &error))
                continue;
            const ByteView view{expanded.data(), expanded.size()};
            std::vector<Chunk> top;
            if (!read_stream(view, &top))
                continue;
            for (const Chunk& c : top) {
                World world;
                if (c.type != chunk::kWorld || !import_world(view, c, &world, &error))
                    continue;
                for (WorldSector& sector : world.sectors) {
                    std::vector<bool> glow(sector.mesh.submeshes.size(), false);
                    for (std::size_t s = 0; s < glow.size(); ++s) {
                        const std::uint32_t m = sector.mesh.submeshes[s].material;
                        glow[s] = m < world.materials.size() &&
                                  world.materials[m].texture.name.find("_zn055.") != std::string::npos;
                    }
                    const LightSlotResult r = fix_light_slots(sector.mesh, glow);
                    total.moved_vertices += r.moved_vertices;
                    total.added_lights += r.added_lights;
                }
            }
        }
    }
    check(total.moved_vertices == 1834 && total.added_lights == 8,
          "light slots: GUN Fortress's table moves 1834 vertices and adds 8 lights on the disc (moved " +
              std::to_string(total.moved_vertices) + ", added " + std::to_string(total.added_lights) + ")");
}

}  // namespace

int main(int argc, char** argv) {
    cmpr_block_colours();
    indexed_tiles();
    mip_chain_ignores_transparent_colour();
    light_slots_move_and_add();
    if (argc > 1) {
        const std::filesystem::path dol = std::filesystem::u8path(argv[1]);
        const std::filesystem::path files = dol.parent_path().parent_path() / "files";
        if (std::filesystem::exists(files)) {
            walk_disc(files);
            light_slots_match_the_disc(files);
        }
        else
            std::printf("asset_import_tests: no game data at %s - disc checks skipped\n", files.string().c_str());
    }
    if (g_failures == 0) {
        std::printf("asset_import_tests: all passed\n");
        return 0;
    }
    std::printf("asset_import_tests: %d failure(s)\n", g_failures);
    return 1;
}
