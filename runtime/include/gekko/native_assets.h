#pragma once

// Host-native asset loading for the RenderWare capture path: ONE archives,
// SEGA PRS compression and TXD texture dictionaries, parsed on the host from
// the extracted disc tree and registered under the same content hash the
// capture layer computes for guest-owned rasters (see
// docs/NATIVE_ASSET_MANAGER_2026-09-20.md).
//
// Contract, in one place:
//   * Everything here is pure host code. Nothing touches guest memory, the
//     Cpu, or any device model; the manager never writes into MEM1.
//   * The formats are transcriptions of the guest's own parsers
//     (OneFile 0x8004CD38 / 0x8004CA50, PRS 0x80043FE0, the RwGC platform-6
//     TXD stream) and were verified against the whole-disc census
//     (RENDERER_RE/_work/device/one_txd_census.py, 8430 textures, sizeMatch=1).
//   * `notify_file` is the ONLY game-thread entry: one string copy per disc
//     read. All parsing/decoding runs on the manager's own thread.
//   * A texture is served to the renderer only when `texture_hash` over the
//     natively decoded raster's stored bytes (+ palette + header) equals the
//     hash the capture computed over the guest's raster. Decode uses the same
//     gx_decode_texture, so a hash hit is bit-identical RGBA8 by construction.
//   * Misses and parse failures fall back to the existing guest-capture path;
//     they are counted, never fatal.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gekko/types.h"
#include "native_execution.h"
#include "native_memory.h"

namespace gekko {

// --- PRS decompression (guest 0x80043FE0, verified against the disc) -------
// Expands the SEGA PRS stream at [src, src+src_size) into out, stopping at the
// stream's own terminator (the 16-bit zero word) or at expected_size bytes,
// whichever comes first. Returns false when the stream is malformed or
// expected_size bytes cannot be produced. Copies may overlap.
bool prs_expand(const u8* src, std::size_t src_size, std::size_t expected_size,
                std::vector<u8>& out);
using DecompressionBytes = shadow::memory::Vector<u8, shadow::memory::Domain::Decompression>;
bool prs_expand(const u8* src, std::size_t src_size, std::size_t expected_size,
                DecompressionBytes& out);

// --- ONE archive ("One Ver 0.60") ------------------------------------------
class OneArchive {
public:
    struct Member {
        std::string name;   // exact-match key, e.g. "DOOMSEYE.TXD" (last-wins on
                            // duplicates, like the guest's backwards scan)
        u32 size = 0;       // decompressed size
        u32 offset = 0;     // stored offset, relative to file+0x0C
        u32 flags = 0;      // &1 = PRS compressed
    };

    // Parses the header and the (N+2)x56-byte entry table. Returns false and
    // leaves the archive empty when the magic, version or table bounds do not
    // verify. `data` must remain valid while the archive is used.
    static bool parse(const u8* data, std::size_t data_size, OneArchive& out);

    bool valid() const noexcept { return data_ != nullptr; }
    const std::vector<Member>& members() const noexcept { return members_; }

    // Exact name lookup, scanning backwards like the guest's 0x800145A4.
    const Member* find(const std::string& name) const noexcept;

    // Decompresses (or copies, when uncompressed) the member into `out`.
    // Returns false for an unknown member, a short file, or a bad stream.
    bool read_member(const Member& member, std::vector<u8>& out) const;
    bool read_member(const Member& member, DecompressionBytes& out) const;

private:
    template<class A> bool read_member_impl(const Member& member, std::vector<u8, A>& out) const;
    const u8* data_ = nullptr;
    std::size_t data_size_ = 0;
    std::vector<Member> members_;
};

// --- TXD texture dictionaries (platform 6) ---------------------------------
class TxdReader {
public:
    struct Texture {
        std::string name;
        u32 width = 0;
        u32 height = 0;
        u32 depth = 0;        // palette bit depth (0 = not palettised)
        u32 mip_levels = 0;
        u32 gx_format = 0;    // kGxTexFmt* / GXTexFmt
        u32 tlut_format = 0;  // kGxTlutFmt* (valid when palettised)
        u32 has_alpha = 0;
        u32 data_size = 0;    // stored texel bytes
        std::size_t texel_offset = 0;  // into the TXD buffer
        std::size_t palette_offset = 0;
        u32 palette_entries = 0;       // 1 << depth when palettised
    };

    // Walks the RenderWare binary stream chunks of a decompressed TXD member
    // and fills `textures`. Returns false when the chunk chain does not parse.
    static bool parse(const u8* data, std::size_t data_size,
                      std::vector<Texture>& textures);
};

// --- the manager ------------------------------------------------------------
class NativeAssetManager {
public:
    // Process-wide instance, installed by shadow_main when the native renderer
    // is active (--native-assets; --no-native-assets leaves it null). Same
    // pattern as AssetTracer::active(): the DI seam tests one pointer.
    static NativeAssetManager* active() noexcept { return active_.load(std::memory_order_relaxed); }
    static void set_active(NativeAssetManager* manager) noexcept {
        active_.store(manager, std::memory_order_relaxed);
    }

    // The one hash definition for "these stored texels (+palette+header) are
    // this texture". FNV-1a 64 over header, texels, palette. Used by BOTH the
    // capture side (RwCapture::hash_texture / upload_texture) and the manager,
    // so a match is meaningful across the two. palette may be null with
    // palette_bytes == 0.
    static u64 texture_hash(u32 format, u32 tlut_format, u32 depth, u32 width, u32 height,
                            const u8* texels, std::size_t texel_bytes, const u8* palette,
                            std::size_t palette_bytes) noexcept;

    struct TextureAsset {
        u32 width = 0;
        u32 height = 0;
        u32 gx_format = 0;
        shadow::memory::Vector<u8, shadow::memory::Domain::Assets> rgba8;  // width*height*4, linear, row-major
    };

    // `game_root` is the extracted disc root (contains files/). Assets are
    // loaded from game_root/files/<guest_path>. `byte_budget` caps decoded
    // RGBA8 residency; the oldest asset is evicted first.
    explicit NativeAssetManager(std::filesystem::path game_root,
                                std::size_t byte_budget = 0u, unsigned workers = 0u);
    ~NativeAssetManager();
    NativeAssetManager(const NativeAssetManager&) = delete;
    NativeAssetManager& operator=(const NativeAssetManager&) = delete;

    void start();
    void stop();

    // Game thread. One string copy; deduplicated by name. Returns immediately.
    void notify_file(const std::string& guest_path);

    // Any thread. Returns the decoded asset for a content hash, or nullptr on
    // a miss (caller falls back to the guest-captured path). Touches the LRU.
    std::shared_ptr<const TextureAsset> find_texture(u64 hash);

    // Any thread. A texture served for `hash` in place of the guest's, never
    // evicted and looked up before the decoded disc textures: the modern button
    // sheets (button_glyphs.h), whose asset may be larger than the guest raster
    // (the renderer uploads the asset's own size; sampling is normalised).
    void pin_texture(u64 hash, std::shared_ptr<const TextureAsset> asset);

    struct Stats {
        u64 files_notified = 0;
        u64 files_parsed = 0;
        u64 ones_parsed = 0;
        u64 txds_parsed = 0;
        u64 textures_registered = 0;
        u64 textures_failed = 0;
        u64 lookups = 0;
        u64 lookups_hit = 0;
        u64 evictions = 0;
        u64 resident_bytes = 0;
        u64 cache_budget_bytes = 0;
        u64 execution_graphs = 0;
        u64 execution_tasks = 0;
        u64 execution_worker_tasks = 0;
        u64 execution_failures = 0;
        unsigned execution_workers = 0;
        u64 pinned_textures = 0;
        u64 pinned_hits = 0;
    };
    Stats stats() const;

private:
    struct Entry {
        u64 hash = 0;
        std::shared_ptr<const TextureAsset> asset;
        u64 last_use = 0;
        std::list<u64>::iterator lru_pos;
        bool lru_linked = false;
        std::string source;  // the file its dictionary came from (an eviction lets that file be parsed again)
    };

    void worker_loop();
    void parse_file(const std::string& guest_path);
    void register_textures(const std::string& source, const u8* data, std::size_t size);
    void evict_locked(std::size_t need_bytes);

    std::filesystem::path root_;
    std::size_t byte_budget_;
    shadow::execution::Executor executor_;
    std::vector<std::string> queue_;
    std::unordered_map<std::string, bool> seen_;
    std::vector<std::string> evicted_sources_;  // guarded by assets_mutex_; notify_file forgets them in seen_
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread worker_;

    mutable std::mutex assets_mutex_;
    std::unordered_map<u64, std::unique_ptr<Entry>> assets_;
    std::unordered_map<u64, std::shared_ptr<const TextureAsset>> pinned_;
    std::list<u64> lru_;
    u64 use_counter_ = 0;
    std::size_t resident_bytes_ = 0;

    Stats stats_;

    static std::atomic<NativeAssetManager*> active_;
};

}  // namespace gekko
