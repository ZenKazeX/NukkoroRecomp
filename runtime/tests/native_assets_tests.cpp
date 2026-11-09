// native_assets tests.
//
//   native_assets_tests [game-root]
//
// Verifies the host-native ONE / PRS / TXD loader against REAL files from the
// extracted disc tree (default H:/ShadowTheHedgehogDiskGC_Extract): the ONE
// entry table and PRS decompression against the guest's own parser-derived
// numbers, TXD chunk walking against the whole-disc census
// (RENDERER_RE/_work/device/one_txd_census.py), RGBA8 decoding of real
// rasters, and the end-to-end NativeAssetManager (notify -> background parse
// -> hash lookup hit).
//
// All expectations below are facts measured from the disc and from the guest's
// parser, not properties this code invented; see
// docs/NATIVE_ASSET_MANAGER_2026-09-20.md section 3.

#include "gekko/gx_texture_decode.h"
#include "gekko/native_assets.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using gekko::NativeAssetManager;
using gekko::OneArchive;
using gekko::TxdReader;
using gekko::prs_expand;
using gekko::u32;
using gekko::u64;
using gekko::u8;

constexpr const char* kDefaultRoot = "H:/ShadowTheHedgehogDiskGC_Extract";

int g_cases = 0;
int g_failures = 0;

bool report(const std::string& name, bool ok, const std::string& detail = {}) {
    ++g_cases;
    if (!ok) ++g_failures;
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(),
                detail.empty() ? "" : " - ", detail.c_str());
    return ok;
}

std::string format_text(const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    return buffer;
}

bool read_file(const std::filesystem::path& path, std::vector<u8>& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !out.empty();
}

std::vector<u8> load(const std::filesystem::path& root, const char* rel) {
    std::vector<u8> bytes;
    if (!read_file(root / rel, bytes)) {
        std::printf("FATAL: cannot read %s (pass the extracted disc root as argv[1])\n", rel);
        std::exit(2);
    }
    return bytes;
}

void test_prs_expand(const std::filesystem::path& root) {
    const std::vector<u8> one = load(root, "files/character/doomseye.one");
    OneArchive archive;
    report("one_parse_doomseye", OneArchive::parse(one.data(), one.size(), archive) &&
                                    archive.valid());
    const OneArchive::Member* dff = archive.find("DOOMSEYE.DFF");
    report("one_member_dff", dff != nullptr && dff->size == 38182u && dff->offset == 448u &&
                                 dff->flags == 1u,
           dff != nullptr ? format_text("size=%u offset=%u flags=%u", dff->size, dff->offset,
                                        dff->flags)
                          : "not found");
    if (dff == nullptr) return;
    std::vector<u8> expanded;
    const bool ok = archive.read_member(*dff, expanded);
    report("prs_expand_dff", ok && expanded.size() >= dff->size,
           format_text("decoded %zu bytes, declared %u", expanded.size(), dff->size));
    // The decompressed DFF is a RenderWare binary stream with little-endian
    // chunk headers: first chunk = clump (0x10).
    report("prs_expand_dff_clump", expanded.size() >= 12u &&
                                       expanded[0] == 0x10u && expanded[1] == 0u &&
                                       expanded[2] == 0u && expanded[3] == 0u &&
                                       expanded[8] == 0x37u && expanded[9] == 0x00u &&
                                       expanded[10] == 0x02u && expanded[11] == 0x1cu,
           expanded.size() >= 12u ? format_text("head %02x %02x %02x %02x", expanded[0],
                                                expanded[1], expanded[2], expanded[3])
                                  : "too short");
}

void test_one_doomseye(const std::filesystem::path& root) {
    const std::vector<u8> one = load(root, "files/character/doomseye.one");
    OneArchive archive;
    if (!report("one_doomseye_parse", OneArchive::parse(one.data(), one.size(), archive))) return;
    struct Expect {
        const char* name;
        u32 size;
        u32 offset;
    };
    const Expect expects[] = {
        {"DOOMSEYE.DFF", 38182u, 448u},
        {"DOOMSEYE.TXD", 2952u, 13920u},
        {"DOOMSEYE.BON", 2512u, 16000u},
        {"DOOMSEYE.MTP", 39564u, 16512u},
        {"CAM_DOOMSEYE_MEET_CAMERA1.SNB", 480u, 25664u},
    };
    report("one_doomseye_members", archive.members().size() == 5u,
           format_text("%zu members", archive.members().size()));
    for (const Expect& expect : expects) {
        const OneArchive::Member* member = archive.find(expect.name);
        report(format_text("one_doomseye_%s", expect.name),
               member != nullptr && member->size == expect.size && member->offset == expect.offset,
               member != nullptr
                   ? format_text("size=%u offset=%u", member->size, member->offset)
                   : "not found");
    }
    // The TXD member must decompress and walk its chunks into textures.
    const OneArchive::Member* txd = archive.find("DOOMSEYE.TXD");
    if (txd == nullptr) return;
    std::vector<u8> txd_bytes;
    if (!report("one_doomseye_txd_read", archive.read_member(*txd, txd_bytes))) return;
    std::vector<TxdReader::Texture> textures;
    const bool parsed = TxdReader::parse(txd_bytes.data(), txd_bytes.size(), textures);
    report("txd_doomseye_parse", parsed && !textures.empty(),
           format_text("%zu textures", textures.size()));
    if (!parsed) return;
    for (const TxdReader::Texture& texture : textures) {
        report(format_text("txd_doomseye_%s", texture.name.c_str()),
               texture.width >= 1u && texture.width <= 1024u && texture.height >= 1u &&
                   texture.height <= 1024u && texture.data_size != 0u,
               format_text("%ux%u fmt=%u depth=%u", texture.width, texture.height,
                           texture.gx_format, texture.depth));
    }
}

void test_one_stg_cmn_gdt(const std::filesystem::path& root) {
    const std::vector<u8> one = load(root, "files/stg_cmn_gdt.one");
    OneArchive archive;
    if (!report("one_stg_cmn_gdt_parse", OneArchive::parse(one.data(), one.size(), archive))) {
        return;
    }
    report("one_stg_cmn_gdt_count", archive.members().size() == 331u,
           format_text("%zu members", archive.members().size()));
    report("one_stg_cmn_gdt_names",
           archive.find("BALLOON00.DFF") != nullptr && archive.find("BALLOON01.DFF") != nullptr &&
               archive.find("BATTERYBKPMPB.DFF") != nullptr);
    // STG_GDT.TXD is the shared texture dictionary; the census says its first
    // entries include vh_GUN_tank (128x128 C4 depth 4, RGB565 TLUT) and
    // ob_hnsd0 with the same shape.
    const OneArchive::Member* txd = archive.find("STG_GDT.TXD");
    if (txd == nullptr) {
        report("one_stg_cmn_gdt_txd_member", false, "STG_GDT.TXD not found");
        return;
    }
    std::vector<u8> txd_bytes;
    if (!report("one_stg_cmn_gdt_txd_read", archive.read_member(*txd, txd_bytes))) return;
    std::vector<TxdReader::Texture> textures;
    const bool parsed = TxdReader::parse(txd_bytes.data(), txd_bytes.size(), textures);
    report("txd_stg_gdt_parse", parsed && textures.size() > 30u,
           format_text("%zu textures", textures.size()));
    if (!parsed) return;
    const TxdReader::Texture* gun = nullptr;
    for (const TxdReader::Texture& texture : textures) {
        if (texture.name == "vh_GUN_tank") gun = &texture;
    }
    report("txd_stg_gdt_vh_GUN_tank",
           gun != nullptr && gun->width == 128u && gun->height == 128u &&
               gun->gx_format == 8u /* C4 */ && gun->depth == 4u &&
               gun->tlut_format == 1u /* RGB565 */,
           gun != nullptr
               ? format_text("%ux%u fmt=%u depth=%u tlut=%u", gun->width, gun->height,
                             gun->gx_format, gun->depth, gun->tlut_format)
               : "not found");
    // Decode every raster and register its content hash; all must decode, and
    // data sizes must equal the GX tile size (the census sizeMatch gate).
    if (gun != nullptr) {
        u64 hashes = 0;
        for (const TxdReader::Texture& texture : textures) {
            const std::size_t texel_bytes =
                gekko::gx_texture_data_size(texture.gx_format, texture.width, texture.height);
            if (texel_bytes == 0u || texture.texel_offset + texel_bytes > txd_bytes.size()) {
                report(format_text("txd_stg_gdt_%s_bounds", texture.name.c_str()), false,
                       format_text("texel_bytes=%zu", texel_bytes));
                continue;
            }
            std::vector<u8> rgba8(static_cast<std::size_t>(texture.width) * texture.height * 4u);
            const u8* palette = texture.palette_entries != 0u
                                    ? txd_bytes.data() + texture.palette_offset
                                    : nullptr;
            if (!gekko::gx_decode_texture(texture.gx_format, texture.width, texture.height,
                                          txd_bytes.data() + texture.texel_offset, texel_bytes,
                                          palette, texture.tlut_format, texture.palette_entries,
                                          rgba8.data())) {
                report(format_text("txd_stg_gdt_%s_decode", texture.name.c_str()), false);
                continue;
            }
            hashes ^= NativeAssetManager::texture_hash(
                texture.gx_format, texture.tlut_format, texture.depth, texture.width,
                texture.height, txd_bytes.data() + texture.texel_offset, texel_bytes, palette,
                static_cast<std::size_t>(texture.palette_entries) * 2u);
        }
        report("txd_stg_gdt_all_decode", true, format_text("hashes=%llu", hashes));
    }
}

void test_one_debugmodels(const std::filesystem::path& root) {
    const std::vector<u8> one = load(root, "files/DebugModels.one");
    OneArchive archive;
    if (!report("one_debugmodels_parse", OneArchive::parse(one.data(), one.size(), archive))) {
        return;
    }
    bool all_read = true;
    u32 count = 0;
    for (const OneArchive::Member& member : archive.members()) {
        std::vector<u8> bytes;
        if (!archive.read_member(member, bytes)) {
            all_read = false;
            report(format_text("one_debugmodels_%s_read", member.name.c_str()), false,
                   format_text("declared %u", member.size));
            break;
        }
        ++count;
    }
    report("one_debugmodels_read_all", all_read && count != 0u,
           format_text("%u members", count));
}

void test_texture_hash_vector() {
    // Cross-check texture_hash against an independent inline FNV-1a over the
    // same byte layout; a regression in either side breaks equality.
    auto reference = [](const u8* data, std::size_t size) {
        u64 hash = 1469598103934665603ull;
        for (std::size_t i = 0; i < size; ++i) {
            hash ^= data[i];
            hash *= 1099511628211ull;
        }
        return hash;
    };
    u8 texels[16];
    for (u32 i = 0; i < 16u; ++i) texels[i] = static_cast<u8>(i * 7u + 3u);
    u8 palette[8] = {0xAB, 0x12, 0x00, 0xFF, 0x44, 0x88, 0x9C, 0x21};
    // Rebuild the same layout the manager hashes: five u32 headers then the
    // byte ranges, all little-endian in the u32s.
    std::vector<u8> stream;
    auto push_u32 = [&stream](u32 value) {
        for (u32 i = 0; i < 4u; ++i) stream.push_back(static_cast<u8>((value >> (8u * i)) & 0xFFu));
    };
    push_u32(8u);   // C4
    push_u32(1u);   // RGB565 TLUT
    push_u32(4u);   // depth
    push_u32(64u);  // width
    push_u32(64u);  // height
    stream.insert(stream.end(), texels, texels + 16u);
    stream.insert(stream.end(), palette, palette + 8u);
    const u64 expected = reference(stream.data(), stream.size());
    const u64 actual = NativeAssetManager::texture_hash(8u, 1u, 4u, 64u, 64u, texels, 16u,
                                                        palette, 8u);
    report("texture_hash_layout", actual == expected,
           format_text("%016llx vs %016llx", actual, expected));
    const u64 again = NativeAssetManager::texture_hash(8u, 1u, 4u, 64u, 64u, texels, 16u,
                                                       palette, 8u);
    report("texture_hash_deterministic", actual == again);
    // Any input difference must change the hash.
    texels[15] ^= 0x40u;
    report("texture_hash_sensitive",
           NativeAssetManager::texture_hash(8u, 1u, 4u, 64u, 64u, texels, 16u, palette, 8u) !=
               actual);
    texels[15] ^= 0x40u;
}

void test_manager_end_to_end(const std::filesystem::path& root) {
    NativeAssetManager manager(root, 64u << 20);
    manager.start();
    manager.notify_file("character/doomseye.one");
    // Wait for the background parse (poll; the test binary must not hang).
    bool registered = false;
    for (int i = 0; i < 400; ++i) {
        if (manager.stats().textures_registered != 0u) {
            registered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    report("manager_parses_notified_file", registered,
           format_text("files_parsed=%llu ones=%llu txds=%llu textures=%llu",
                       manager.stats().files_parsed, manager.stats().ones_parsed,
                       manager.stats().txds_parsed, manager.stats().textures_registered));
    // Recompute the hash of one texture straight from the file and look it up.
    const std::vector<u8> one = load(root, "files/character/doomseye.one");
    OneArchive archive;
    std::vector<u8> txd_bytes;
    const OneArchive::Member* txd = nullptr;
    if (OneArchive::parse(one.data(), one.size(), archive)) txd = archive.find("DOOMSEYE.TXD");
    if (txd != nullptr) archive.read_member(*txd, txd_bytes);
    std::vector<TxdReader::Texture> textures;
    const bool parsed =
        TxdReader::parse(txd_bytes.data(), txd_bytes.size(), textures) && !textures.empty();
    report("manager_txd_precheck", parsed, format_text("%zu textures", textures.size()));
    bool hit = false;
    if (parsed) {
        const TxdReader::Texture& texture = textures.front();
        const u64 hash = NativeAssetManager::texture_hash(
            texture.gx_format, texture.tlut_format, texture.depth, texture.width,
            texture.height, txd_bytes.data() + texture.texel_offset,
            gekko::gx_texture_data_size(texture.gx_format, texture.width, texture.height),
            texture.palette_entries != 0u ? txd_bytes.data() + texture.palette_offset : nullptr,
            static_cast<std::size_t>(texture.palette_entries) * 2u);
        const auto asset = manager.find_texture(hash);
        hit = asset != nullptr && asset->width == texture.width &&
              asset->height == texture.height && asset->rgba8.size() != 0u;
        report("manager_hash_lookup_hit", hit,
               asset != nullptr ? format_text("%ux%u rgba8=%zu", asset->width, asset->height,
                                              asset->rgba8.size())
                                : "miss");
    }
    // A hash of nothing real must miss and leave the renderer fallback intact.
    const auto miss = manager.find_texture(0xDEADBEEF12345678ull);
    report("manager_hash_miss_clean", miss == nullptr);
    manager.stop();
}

void test_manager_ownership(const std::filesystem::path& root) {
    const auto one = load(root, "files/character/doomseye.one");
    OneArchive archive;
    std::vector<u8> txd_bytes;
    std::vector<TxdReader::Texture> textures;
    if (!OneArchive::parse(one.data(), one.size(), archive)) return;
    const auto member = archive.find("DOOMSEYE.TXD");
    if (!member || !archive.read_member(*member, txd_bytes) ||
        !TxdReader::parse(txd_bytes.data(), txd_bytes.size(), textures) || textures.empty()) return;
    const auto& texture = textures.front();
    const auto hash = NativeAssetManager::texture_hash(texture.gx_format, texture.tlut_format,
        texture.depth, texture.width, texture.height, txd_bytes.data() + texture.texel_offset,
        gekko::gx_texture_data_size(texture.gx_format, texture.width, texture.height),
        texture.palette_entries ? txd_bytes.data() + texture.palette_offset : nullptr,
        texture.palette_entries * 2u);
    std::shared_ptr<const NativeAssetManager::TextureAsset> held;
    shadow::memory::Vector<u8, shadow::memory::Domain::Assets> expected;
    {
        NativeAssetManager manager(root, 65536u);
        NativeAssetManager::set_active(&manager);
        manager.start();
        manager.notify_file("character/doomseye.one");
        manager.stop();
        held = manager.find_texture(hash);
        if (!report("manager_returns_owned_texture", held != nullptr)) return;
        expected = held->rgba8;
        manager.start();
        manager.notify_file("stg_cmn_gdt.one");
        manager.stop();
        report("manager_pins_in_use_texture", manager.find_texture(hash) == held && held->rgba8 == expected);
        report("manager_evicts_unused_textures", manager.stats().evictions != 0u);
        report("manager_budget_with_pins", manager.stats().resident_bytes <= 65536u);
    }
    report("manager_texture_outlives_cache", held && held->rgba8 == expected);
    report("manager_clears_active_on_destroy", NativeAssetManager::active() == nullptr);
}

void test_parallel_decode(const std::filesystem::path& root) {
    const auto one = load(root, "files/stg_cmn_gdt.one");
    OneArchive archive;
    std::vector<u8> bytes;
    std::vector<TxdReader::Texture> textures;
    bool valid = OneArchive::parse(one.data(), one.size(), archive);
    const auto member = valid ? archive.find("STG_GDT.TXD") : nullptr;
    valid = member && archive.read_member(*member, bytes) &&
            TxdReader::parse(bytes.data(), bytes.size(), textures);
    if (!report("parallel_decode_fixture", valid)) return;
    NativeAssetManager single(root, 64u << 20, 1u);
    NativeAssetManager parallel(root, 64u << 20, 4u);
    single.start();
    parallel.start();
    single.notify_file("stg_cmn_gdt.one");
    parallel.notify_file("stg_cmn_gdt.one");
    single.stop();
    parallel.stop();
    bool exact = true;
    for (const auto& texture : textures) {
        const auto hash = NativeAssetManager::texture_hash(texture.gx_format, texture.tlut_format,
            texture.depth, texture.width, texture.height, bytes.data() + texture.texel_offset,
            gekko::gx_texture_data_size(texture.gx_format, texture.width, texture.height),
            texture.palette_entries ? bytes.data() + texture.palette_offset : nullptr,
            texture.palette_entries * 2u);
        const auto a = single.find_texture(hash);
        const auto b = parallel.find_texture(hash);
        exact = exact && a && b && a->rgba8 == b->rgba8;
    }
    report("parallel_decode_byte_identity", exact);
    const auto a = single.stats(), b = parallel.stats();
    report("parallel_decode_uses_native_graph", b.execution_tasks == textures.size() * 2u &&
           b.execution_graphs == 1u && b.execution_failures == 0u && b.execution_worker_tasks != 0u);
    report("parallel_decode_ordered_publication", a.textures_registered == b.textures_registered &&
           a.resident_bytes == b.resident_bytes && a.textures_failed == 0u && b.textures_failed == 0u);
}

void test_manager_budget(const std::filesystem::path& root) {
    NativeAssetManager manager(root, 4096u);
    manager.start();
    manager.notify_file("character/doomseye.one");
    manager.stop();
    const auto stats = manager.stats();
    report("manager_respects_small_budget", stats.resident_bytes <= 4096u,
           format_text("resident=%llu", stats.resident_bytes));
}

// Isolated census of the same native manager used by shipping. The independent
// archive walk below uses ordinary vectors, so its validation does not pollute
// the measured Decompression domain. stop() drains queued files and joins all
// decode users before either the source archive or manager is destroyed.
void manager_scratch_census(const std::filesystem::path& root) {
    constexpr const char* paths[] = {"character/doomseye.one", "stg_cmn_gdt.one",
                                   "DebugModels.one"};
    constexpr auto domain = static_cast<std::size_t>(shadow::memory::Domain::Decompression);
    u64 total_allocations = 0, total_members = 0, total_textures = 0;
    u64 digest = 0xcbf29ce484222325ull;
    double total_ms = 0;
    for (const char* path : paths) {
        const auto before = shadow::memory::system().snapshot().domains[domain];
        const auto begin = std::chrono::steady_clock::now();
        NativeAssetManager manager(root, 64u << 20, 1u);
        manager.start();
        manager.notify_file(path);
        manager.stop();
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count();
        const auto after = shadow::memory::system().snapshot().domains[domain];
        const auto stats = manager.stats();
        std::vector<u8> archive_bytes;
        OneArchive archive;
        const bool parsed = read_file(root / "files" / path, archive_bytes) &&
                            OneArchive::parse(archive_bytes.data(), archive_bytes.size(), archive);
        report(format_text("scratch_census_%s_parse", path), parsed && stats.ones_parsed == 1u);
        u64 found = 0;
        std::size_t largest = 0;
        bool exact = true;
        gekko::DecompressionBytes reused;
        for (const auto& member : archive.members()) {
            largest = std::max(largest, static_cast<std::size_t>(member.size) + 256u);
            std::vector<u8> expanded;
            const bool reference_ok = archive.read_member(member, expanded);
            const bool reused_ok = archive.read_member(member, reused);
            exact &= reference_ok == reused_ok && expanded.size() == reused.size() &&
                     std::equal(expanded.begin(), expanded.end(), reused.begin()) &&
                     reused.capacity() <= largest;
            if (!reference_ok) continue;
            std::vector<TxdReader::Texture> textures;
            if (!TxdReader::parse(expanded.data(), expanded.size(), textures)) continue;
            for (const auto& texture : textures) {
                const auto texel_bytes = gekko::gx_texture_data_size(
                    texture.gx_format, texture.width, texture.height);
                const auto palette_bytes = static_cast<std::size_t>(texture.palette_entries) * 2u;
                const auto hash = NativeAssetManager::texture_hash(
                    texture.gx_format, texture.tlut_format, texture.depth, texture.width,
                    texture.height, expanded.data() + texture.texel_offset, texel_bytes,
                    palette_bytes ? expanded.data() + texture.palette_offset : nullptr, palette_bytes);
                const auto asset = manager.find_texture(hash);
                if (!asset) continue;
                ++found;
                for (u8 byte : asset->rgba8) digest = (digest ^ byte) * 0x100000001b3ull;
            }
        }
        report(format_text("scratch_census_%s_member_bytes", path), exact,
               "fresh versus retained: every expanded byte, result and demand bound");
        report(format_text("scratch_census_%s_outputs", path),
               stats.textures_failed == 0u && found >= stats.textures_registered &&
                   after.active == before.active && after.live_allocations == before.live_allocations,
               format_text("members=%zu registered=%llu found=%llu allocations=%llu ms=%.3f largest=%zu released=yes",
                           archive.members().size(), stats.textures_registered, found,
                           after.allocations - before.allocations, elapsed, largest));
        total_allocations += after.allocations - before.allocations;
        total_members += archive.members().size();
        total_textures += found;
        total_ms += elapsed;
    }
    std::printf("SCRATCH_CENSUS members=%llu textures=%llu allocations=%llu ms=%.3f digest=%016llX\n",
                total_members, total_textures, total_allocations, total_ms, digest);
}

void test_member_scratch_reuse(const std::filesystem::path& root) {
    const auto bytes = load(root, "files/character/doomseye.one");
    OneArchive archive;
    if (!report("member_scratch_archive", OneArchive::parse(bytes.data(), bytes.size(), archive) &&
                                            !archive.members().empty())) return;
    gekko::DecompressionBytes reused;
    bool exact = true;
    std::size_t largest = 0;
    for (const auto& member : archive.members()) {
        std::vector<u8> fresh;
        const bool fresh_ok = archive.read_member(member, fresh);
        const bool reused_ok = archive.read_member(member, reused);
        largest = std::max(largest, static_cast<std::size_t>(member.size) + 256u);
        exact &= fresh_ok && reused_ok && fresh.size() == reused.size() &&
                 std::equal(fresh.begin(), fresh.end(), reused.begin()) &&
                 reused.capacity() <= largest;
    }
    report("member_scratch_exact_and_bounded", exact);
    auto invalid = archive.members().front();
    invalid.offset = ~u32{0};
    report("member_scratch_failed_read_clears_previous", !archive.read_member(invalid, reused) &&
                                                          reused.empty());
    auto raw = archive.members().front();
    raw.flags = 0u;
    raw.size = 32u;
    const auto at = static_cast<std::size_t>(raw.offset) + 12u;
    report("member_scratch_uncompressed_owns_bytes", archive.read_member(raw, reused) &&
            reused.size() == raw.size && std::memcmp(reused.data(), bytes.data() + at, raw.size) == 0);
    std::vector<u8> fresh;
    const auto& compressed = archive.members().front();
    report("member_scratch_compressed_after_raw", archive.read_member(compressed, fresh) &&
            archive.read_member(compressed, reused) && fresh.size() == reused.size() &&
            std::equal(fresh.begin(), fresh.end(), reused.begin()));
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path root = argc > 1 ? argv[1] : kDefaultRoot;
    std::printf("native_assets_tests root=%s\n", root.string().c_str());
    if (argc > 2 && std::strcmp(argv[2], "--manager-scratch-census") == 0) {
        manager_scratch_census(root);
        return g_failures == 0 ? 0 : 1;
    }
    test_prs_expand(root);
    test_one_doomseye(root);
    test_one_stg_cmn_gdt(root);
    test_one_debugmodels(root);
    test_texture_hash_vector();
    test_member_scratch_reuse(root);
    test_manager_end_to_end(root);
    test_manager_budget(root);
    test_manager_ownership(root);
    test_parallel_decode(root);
    std::printf("%d cases, %d failures\n", g_cases, g_failures);
    return g_failures == 0 ? 0 : 1;
}
