// Host-native ONE / PRS / TXD loading for the native renderer's asset supply
// path. See include/gekko/native_assets.h for the contract and
// docs/NATIVE_ASSET_MANAGER_2026-09-20.md for the pipeline map and the
// verified formats this transcribes.

#include "gekko/native_assets.h"
#include "gekko/thread_name.h"  // ES-T1

#include "gekko/gx_texture_decode.h"
#include "gekko/profiling.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

namespace gekko {

namespace {

constexpr u64 kFnvOffsetBasis = 1469598103934665603ull;
constexpr u64 kFnvPrime = 1099511628211ull;

u64 fnv1a(const u8* data, std::size_t size, u64 hash = kFnvOffsetBasis) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= kFnvPrime;
    }
    return hash;
}

void fnv1a_u32(u64& hash, u32 value) noexcept {
    const u8* bytes = reinterpret_cast<const u8*>(&value);
    for (u32 i = 0; i < 4u; ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
}

constexpr std::size_t kOneEntrySize = 56u;
constexpr std::size_t kOneEntryNameSize = 0x20u;
constexpr std::size_t kOneEntrySizeOffset = 0x2Cu;
constexpr std::size_t kOneEntryOffsetOffset = 0x30u;
constexpr std::size_t kOneEntryFlagsOffset = 0x34u;

bool load_le32(const u8* p, std::size_t size, u32& out) noexcept {
    if (size < 4u) return false;
    out = static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8u) |
          (static_cast<u32>(p[2]) << 16u) | (static_cast<u32>(p[3]) << 24u);
    return true;
}

}  // namespace

// PRS lives in prs.cpp. It is a codec, not part of the texture manager:
// pulling it out is what lets the asset runtime link the decompressor
// without dragging in an executor, a memory system and a GX decoder.

// --- ONE archive -------------------------------------------------------------

bool OneArchive::parse(const u8* data, std::size_t data_size, OneArchive& out) {
    // The header check plus the (count+2) x 56-byte entry table walk, with a
    // memchr and a std::string build per surviving entry.
    GEKKO_PROFILE_ZONE("res/one-parse");
    out.data_ = nullptr;
    out.data_size_ = 0;
    out.members_.clear();
    if (data == nullptr || data_size < 0x40u) return false;
    // Layout: [0x00] u32 LE 0 (reserved), [0x04] u32 LE payload size,
    // [0x08] u32 LE library id, [0x0C] "One Ver 0.60" (16 bytes),
    // [0x1C] u32 LE entry count. The leading 12 bytes are not part of the
    // parsed stream the guest walks. Disc files carry either 0.60
    // (libID 0x1C020037) or the legacy 0.50 (libID 0x1C020020, fields at
    // +0x20/+0x24/+0x28 - the guest's own legacy rearrange branch).
    if (std::memcmp(data + 0x0Cu, "One Ver ", 8u) != 0) return false;
    u32 library_id = 0;
    if (!load_le32(data + 8u, data_size - 8u, library_id) ||
        (library_id != 0x1C020037u && library_id != 0x1C020020u)) {
        return false;
    }
    const bool legacy = data[0x14u] == '0' && data[0x15u] == '.' && data[0x16u] == '5';
    const std::size_t size_offset = legacy ? 0x20u : kOneEntrySizeOffset;
    const std::size_t offset_offset = legacy ? 0x24u : kOneEntryOffsetOffset;
    const std::size_t flags_offset = legacy ? 0x28u : kOneEntryFlagsOffset;
    u32 count = 0;
    if (!load_le32(data + 0x1Cu, data_size - 0x1Cu, count)) return false;
    // The guest iterates index 2 .. count+1 over a table at stream+0x34 with a
    // 56-byte stride (0x8004CA50); the first two entries are reserved.
    const std::size_t table_entries = static_cast<std::size_t>(count) + 2u;
    const std::size_t table_bytes = table_entries * kOneEntrySize;
    if (table_bytes > data_size - 0x40u) return false;
    const u8* table = data + 0x40u;
    out.members_.reserve(table_entries);
    for (std::size_t i = 0; i < table_entries; ++i) {
        const u8* entry = table + i * kOneEntrySize;
        if (entry[kOneEntryNameSize - 1u] == 0u && entry[0] == 0u && entry[1] == 0u &&
            entry[2] == 0u && entry[3] == 0u) {
            continue;  // reserved / unused slot
        }
        Member member;
        const u8* name_end =
            static_cast<const u8*>(std::memchr(entry, 0, kOneEntryNameSize));
        const std::size_t name_length =
            name_end != nullptr ? static_cast<std::size_t>(name_end - entry)
                                : kOneEntryNameSize;
        member.name.assign(reinterpret_cast<const char*>(entry), name_length);
        u32 size = 0, offset = 0, flags = 0;
        if (!load_le32(entry + size_offset, kOneEntrySize - size_offset, size) ||
            !load_le32(entry + offset_offset, kOneEntrySize - offset_offset, offset) ||
            !load_le32(entry + flags_offset, kOneEntrySize - flags_offset, flags)) {
            return false;
        }
        member.size = size;
        member.offset = offset;
        member.flags = flags;
        out.members_.push_back(std::move(member));
    }
    // The member count every later per-member rate is derived from.
    GEKKO_PROFILE_VALUE(out.members_.size());
    out.data_ = data;
    out.data_size_ = data_size;
    return true;
}

const OneArchive::Member* OneArchive::find(const std::string& name) const noexcept {
    // Backwards scan like the guest's FindResourceByName (0x800145A4): later
    // entries win.
    for (auto it = members_.rbegin(); it != members_.rend(); ++it) {
        if (it->name == name) return &*it;
    }
    return nullptr;
}

template<class A>
bool OneArchive::read_member_impl(const Member& member, std::vector<u8, A>& out) const {
    // One zone per archive MEMBER. It covers the straight assign AND the PRS
    // path (res/prs-expand nests inside), so the two are directly comparable,
    // and it covers the O(N) scan below that bounds the compressed extent -
    // quadratic in member count overall, and real work at 331 members.
    GEKKO_PROFILE_ZONE("res/one-member");
    GEKKO_PROFILE_VALUE(member.size);
    GEKKO_PROFILE_TEXT(member.name.c_str(), member.name.size());
    out.clear();
    if (data_ == nullptr) return false;
    // Data offsets are relative to the stream base at file+0x0C.
    const std::size_t start = static_cast<std::size_t>(member.offset) + 0x0Cu;
    if (start > data_size_) return false;
    const u8* src = data_ + start;
    const std::size_t available = data_size_ - start;
    if ((member.flags & 1u) == 0u) {
        if (member.size > available) return false;
        out.assign(src, src + member.size);
        return true;
    }
    // PRS: the stored length is bounded by the next member's offset or the end
    // of the file; the decompressor stops at its own terminator.
    std::size_t stored_bytes = available;
    // Find the smallest member offset greater than this one to bound the
    // compressed extent (members are stored sequentially).
    for (const Member& other : members_) {
        const std::size_t other_start = static_cast<std::size_t>(other.offset) + 0x0Cu;
        if (other_start > start && other_start - start < stored_bytes) {
            stored_bytes = other_start - start;
        }
    }
    if (!prs_expand(src, stored_bytes, member.size, out)) return false;
    // The stream may carry a few bytes of alignment padding past the declared
    // size (the guest allocator rounds up); the guest reads until the
    // terminator. Require at least the declared size.
    return out.size() >= member.size;
}

bool OneArchive::read_member(const Member& member, std::vector<u8>& out) const {
    return read_member_impl(member, out);
}

bool OneArchive::read_member(const Member& member, DecompressionBytes& out) const {
    return read_member_impl(member, out);
}

// --- TXD ---------------------------------------------------------------------

bool TxdReader::parse(const u8* data, std::size_t data_size,
                      std::vector<Texture>& textures) {
    // The RenderWare chunk walk. register_textures runs this speculatively on
    // EVERY ONE member, so most invocations reject a DFF within the first few
    // compares - which is the point: the gap between this zone's count and
    // res/txds-parsed IS the cost of the probe.
    GEKKO_PROFILE_ZONE("res/txd-parse");
    textures.clear();
    std::size_t at = 0;
    auto read_u32 = [&](std::size_t pos, u32& value) -> bool {
        if (pos + 4u > data_size) return false;
        value = static_cast<u32>(data[pos]) | (static_cast<u32>(data[pos + 1u]) << 8u) |
                (static_cast<u32>(data[pos + 2u]) << 16u) |
                (static_cast<u32>(data[pos + 3u]) << 24u);
        return true;
    };
    auto read_u32_be = [&](std::size_t pos, u32& value) -> bool {
        if (pos + 4u > data_size) return false;
        value = (static_cast<u32>(data[pos]) << 24u) |
                (static_cast<u32>(data[pos + 1u]) << 16u) |
                (static_cast<u32>(data[pos + 2u]) << 8u) | static_cast<u32>(data[pos + 3u]);
        return true;
    };
    auto read_u16_be = [&](std::size_t pos, u32& value) -> bool {
        if (pos + 2u > data_size) return false;
        value = (static_cast<u32>(data[pos]) << 8u) | static_cast<u32>(data[pos + 1u]);
        return true;
    };
    auto read_u16_le = [&](std::size_t pos, u32& value) -> bool {
        if (pos + 2u > data_size) return false;
        value = static_cast<u32>(data[pos]) | (static_cast<u32>(data[pos + 1u]) << 8u);
        return true;
    };

    u32 type = 0, size = 0, version = 0;
    if (!read_u32(at, type) || !read_u32(at + 4u, size) || !read_u32(at + 8u, version)) {
        return false;
    }
    if (type != 0x16u) return false;  // rwID_TEXDICTIONARY
    if (size < 12u || at + 12u + size > data_size) return false;
    at += 12u;
    const std::size_t dict_end = at + size;

    // struct chunk: [type][size][version] then [numTextures u16 BE][deviceId u16 BE]
    u32 sid = 0, ssize = 0, sver = 0;
    if (!read_u32(at, sid) || !read_u32(at + 4u, ssize) || !read_u32(at + 8u, sver)) {
        return false;
    }
    at += 12u;
    if (ssize < 4u || at + ssize > dict_end) return false;
    u32 count = 0, device_id = 0;
    if (!read_u16_le(at, count) || !read_u16_le(at + 2u, device_id)) return false;
    at += ssize;

    for (u32 t = 0; t < count; ++t) {
        // textureNative chunk
        u32 tid = 0, tsize = 0, tver = 0;
        if (!read_u32(at, tid) || !read_u32(at + 4u, tsize) || !read_u32(at + 8u, tver)) {
            return false;
        }
        if (tid != 0x15u) return false;  // rwID_TEXTURENATIVE
        if (tsize < 12u || at + 12u + tsize > dict_end) return false;
        at += 12u;
        const std::size_t texture_end = at + tsize;

        // inner struct
        if (!read_u32(at, sid) || !read_u32(at + 4u, ssize) || !read_u32(at + 8u, sver)) {
            return false;
        }
        at += 12u;
        if (ssize < 88u || at + ssize > texture_end) return false;
        // platform, filterAddressing, maxAniso, biasClamp, edgeLOD are big
        // endian; the name is 32 bytes at struct+24.
        Texture texture;
        const u8* name =
            static_cast<const u8*>(std::memchr(data + at + 24u, 0, 32u));
        const std::size_t name_length =
            name != nullptr ? static_cast<std::size_t>(name - (data + at + 24u)) : 32u;
        texture.name.assign(reinterpret_cast<const char*>(data + at + 24u), name_length);
        at += 88u;

        // Raster header (" >IHHBBBBI"): u32 BE rasterFlags, u16 BE width,
        // u16 BE height, u8 depth, u8 mipLevels, u8 gxFormat, u8 tlutFormat,
        // u8 hasAlpha, 4 bytes reserved (16 bytes total).
        u32 raster_flags = 0, width = 0, height = 0;
        if (!read_u32_be(at, raster_flags) || !read_u16_be(at + 4u, width) ||
            !read_u16_be(at + 6u, height)) {
            return false;
        }
        if (at + 16u > texture_end) return false;
        texture.width = width;
        texture.height = height;
        texture.depth = data[at + 8u];
        texture.mip_levels = data[at + 9u];
        texture.gx_format = data[at + 10u];
        texture.tlut_format = data[at + 11u];
        texture.has_alpha = data[at + 12u];
        at += 16u;
        const bool palettised = (raster_flags & 0x6000u) != 0u && texture.depth <= 14u;
        if (palettised) {
            texture.palette_entries = 1u << texture.depth;
            const std::size_t palette_bytes = static_cast<std::size_t>(texture.palette_entries) * 2u;
            if (at + palette_bytes > texture_end) return false;
            texture.palette_offset = at;
            at += palette_bytes;
        }
        // Stored data size: big-endian u32 (the census ">I"; the raster header
        // fields are BE while the chunk walk itself is LE).
        u32 stored_size = 0;
        if (!read_u32_be(at, stored_size)) return false;
        at += 4u;
        if (at + stored_size > texture_end) return false;
        texture.data_size = stored_size;
        texture.texel_offset = at;
        at += stored_size;
        at = texture_end;
        textures.push_back(std::move(texture));
    }
    GEKKO_PROFILE_VALUE(textures.size());
    return true;
}

// --- NativeAssetManager ------------------------------------------------------

std::atomic<NativeAssetManager*> NativeAssetManager::active_{nullptr};

u64 NativeAssetManager::texture_hash(u32 format, u32 tlut_format, u32 depth, u32 width,
                                     u32 height, const u8* texels, std::size_t texel_bytes,
                                     const u8* palette, std::size_t palette_bytes) noexcept {
    u64 hash = kFnvOffsetBasis;
    fnv1a_u32(hash, format);
    fnv1a_u32(hash, tlut_format);
    fnv1a_u32(hash, depth);
    fnv1a_u32(hash, width);
    fnv1a_u32(hash, height);
    if (texels != nullptr && texel_bytes != 0u) {
        hash = fnv1a(texels, texel_bytes, hash);
    }
    if (palette != nullptr && palette_bytes != 0u) {
        hash = fnv1a(palette, palette_bytes, hash);
    }
    return hash;
}

NativeAssetManager::NativeAssetManager(std::filesystem::path game_root, std::size_t byte_budget,
                                       unsigned workers)
    : root_(std::move(game_root)),
      byte_budget_(byte_budget != 0u ? byte_budget : static_cast<std::size_t>(shadow::memory::system().budget() / 4u)),
      executor_(workers) {}

NativeAssetManager::~NativeAssetManager() {
    NativeAssetManager* expected = this;
    active_.compare_exchange_strong(expected, nullptr);
    stop();
    // Without this drain every still-resident texture is reported as a leak.
    // After stop(), not before: the worker owns assets_ until it is joined.
    GEKKO_PROFILE_ONLY(std::lock_guard drain(assets_mutex_);
                       for (const auto& item : assets_) {
                           if (!item.second || !item.second->asset) continue;
                           const std::size_t bytes = item.second->asset->rgba8.size();
                           if (bytes != 0u)
                               GEKKO_PROFILE_FREE(item.second->asset->rgba8.data(),
                                                  "res/texture-cache");
                       })
}

void NativeAssetManager::start() {
    std::lock_guard lock(mutex_);
    if (worker_.joinable()) return;
    // Declared once, before the worker runs. Step matters most here: every one
    // of these holds its value between events, and drawn interpolated they
    // imply continuous activity across the idle stretch between two loads.
    // res/parse-bytes is the exception - a per-event sample, not a total.
    GEKKO_PROFILE_PLOT_CONFIG("res/files-notified", GEKKO_PLOT_NUMBER, true, true, 0x4fa8ff);
    GEKKO_PROFILE_PLOT_CONFIG("res/files-parsed", GEKKO_PLOT_NUMBER, true, true, 0x4fd8a8);
    GEKKO_PROFILE_PLOT_CONFIG("res/ones-parsed", GEKKO_PLOT_NUMBER, true, true, 0x8cd8ff);
    GEKKO_PROFILE_PLOT_CONFIG("res/txds-parsed", GEKKO_PLOT_NUMBER, true, true, 0xa88cff);
    GEKKO_PROFILE_PLOT_CONFIG("res/textures-registered", GEKKO_PLOT_NUMBER, true, true, 0x4fff8c);
    GEKKO_PROFILE_PLOT_CONFIG("res/textures-failed", GEKKO_PLOT_NUMBER, true, true, 0xff5f5f);
    GEKKO_PROFILE_PLOT_CONFIG("res/texture-lookups", GEKKO_PLOT_NUMBER, true, true, 0xffc04f);
    GEKKO_PROFILE_PLOT_CONFIG("res/evictions", GEKKO_PLOT_NUMBER, true, true, 0xff8c4f);
    GEKKO_PROFILE_PLOT_CONFIG("res/evict-pinned", GEKKO_PLOT_NUMBER, true, true, 0xff8c8c);
    GEKKO_PROFILE_PLOT_CONFIG("res/parse-queue", GEKKO_PLOT_NUMBER, true, true, 0xd8d8d8);
    GEKKO_PROFILE_PLOT_CONFIG("res/resident-bytes", GEKKO_PLOT_MEMORY, true, true, 0x4fd8a8);
    GEKKO_PROFILE_PLOT_CONFIG("res/cache-budget", GEKKO_PLOT_MEMORY, true, false, 0x8c8c8c);
    GEKKO_PROFILE_PLOT_CONFIG("res/parse-bytes", GEKKO_PLOT_MEMORY, false, true, 0xa88cff);
    GEKKO_PROFILE_PLOT_CONFIG("res/cache-fill", GEKKO_PLOT_PERCENTAGE, true, true, 0xffc04f);
    GEKKO_PROFILE_PLOT_CONFIG("res/texture-hit-rate", GEKKO_PLOT_PERCENTAGE, true, true, 0x4fa8ff);
    // Fixed at construction and never changed, so one point is honest: Tracy
    // holds the last value across the timeline, which is what gives the
    // residency curve a reference line.
    GEKKO_PROFILE_PLOT("res/cache-budget", static_cast<double>(byte_budget_));
    stop_ = false;
    worker_ = std::thread(&NativeAssetManager::worker_loop, this);
}

void NativeAssetManager::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!worker_.joinable()) return;
        stop_ = true;
    }
    cv_.notify_all();
    worker_.join();
}

void NativeAssetManager::notify_file(const std::string& guest_path) {
    // The ONLY place this manager touches the game thread, and it takes BOTH
    // mutexes in one scoped_lock. The worker holds assets_mutex_ across every
    // texture commit and across the whole of evict_locked, so a guest read can
    // block here behind decode work - a guest-thread stall with no counter
    // anywhere. At the top, so the early return for uninteresting paths is
    // priced too: that is the overhead the game thread pays for a manager it
    // mostly does not use.
    GEKKO_PROFILE_ZONE("res/notify-file");
    // Only files this manager can load, and never a movie/audio stream.
    // The suffix in any case (the namespace folds ASCII case; PrsAhead matches ".one" the same way).
    const auto suffix = [&](const char* ext) {
        if (guest_path.size() <= 4u) return false;
        for (std::size_t i = 0; i < 4u; ++i) {
            char ch = guest_path[guest_path.size() - 4u + i];
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            if (ch != ext[i]) return false;
        }
        return true;
    };
    const bool interesting = suffix(".one") || suffix(".txd");
    if (!interesting) return;
    {
        std::scoped_lock lock(mutex_, assets_mutex_);
        if (stop_) return;
        ++stats_.files_notified;
        for (const std::string& evicted : evicted_sources_) seen_.erase(evicted);
        evicted_sources_.clear();
        // The cleanest "a load is happening" signal in the whole runtime.
        GEKKO_PROFILE_PLOT("res/files-notified", static_cast<double>(stats_.files_notified));
        if (seen_.emplace(guest_path, true).second) {
            queue_.push_back(guest_path);
            // The backlog waiting for the worker: a growing one means the
            // worker is behind the guest and textures are served from the
            // fallback capture path. queue_ is guarded by mutex_, held here.
            GEKKO_PROFILE_PLOT("res/parse-queue", static_cast<double>(queue_.size()));
        }
    }
    cv_.notify_one();
}

std::shared_ptr<const NativeAssetManager::TextureAsset> NativeAssetManager::find_texture(u64 hash) {
    // Before the lock, so the zone includes any wait on assets_mutex_ while
    // the worker holds it. One live caller (RwCapture::upload_texture, once per
    // successful upload), so this is ~155 events per RUN against 1.6M binds.
    GEKKO_PROFILE_ZONE("res/texture-lookup");
    std::lock_guard lock(assets_mutex_);
    ++stats_.lookups;
    GEKKO_PROFILE_PLOT("res/texture-lookups", static_cast<double>(stats_.lookups));
    if (const auto pin = pinned_.find(hash); pin != pinned_.end()) {
        ++stats_.lookups_hit;
        ++stats_.pinned_hits;
        return pin->second;
    }
    const auto it = assets_.find(hash);
    if (it == assets_.end()) {
        // Emitted from BOTH outcomes, or the curve could only ever rise.
        GEKKO_PROFILE_PLOT("res/texture-hit-rate",
                           static_cast<double>(stats_.lookups_hit) * 100.0 /
                               static_cast<double>(stats_.lookups));
        return nullptr;
    }
    Entry& entry = *it->second;
    ++stats_.lookups_hit;
    GEKKO_PROFILE_PLOT("res/texture-hit-rate",
                       static_cast<double>(stats_.lookups_hit) * 100.0 /
                           static_cast<double>(stats_.lookups));
    entry.last_use = ++use_counter_;
    if (entry.lru_linked) {
        lru_.erase(entry.lru_pos);
        entry.lru_linked = false;
    }
    lru_.push_front(hash);
    entry.lru_pos = lru_.begin();
    entry.lru_linked = true;
    return entry.asset;
}

void NativeAssetManager::pin_texture(u64 hash, std::shared_ptr<const TextureAsset> asset) {
    std::lock_guard lock(assets_mutex_);
    pinned_[hash] = std::move(asset);
    stats_.pinned_textures = pinned_.size();
}

NativeAssetManager::Stats NativeAssetManager::stats() const {
    std::lock_guard lock(assets_mutex_);
    Stats result = stats_;
    result.resident_bytes = resident_bytes_;
    result.cache_budget_bytes = byte_budget_;
    const auto execution = executor_.stats();
    result.execution_graphs = execution.graphs;
    result.execution_tasks = execution.executed;
    result.execution_worker_tasks = execution.worker_tasks;
    result.execution_workers = execution.workers;
    return result;
}

void NativeAssetManager::worker_loop() {
    // Without this, every res/parse-file, res/one-member, res/prs-expand and
    // res/txd-parse zone lands on an anonymous thread id, indistinguishable
    // from the executor pool and the render thread.
    GEKKO_PROFILE_THREAD("asset-worker");
    name_this_thread("native-asset-worker");  // ES-T1
    for (;;) {
        std::string path;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_ && queue_.empty()) return;
            path = queue_.back();
            queue_.pop_back();
            // The draining half of res/parse-queue; mutex_ is still held.
            GEKKO_PROFILE_PLOT("res/parse-queue", static_cast<double>(queue_.size()));
        }
        try { parse_file(path); }
        catch (...) {
            std::lock_guard lock(assets_mutex_);
            ++stats_.execution_failures;
            GEKKO_PROFILE_MESSAGE_LITERAL("res/parse-threw");
        }
    }
}

void NativeAssetManager::parse_file(const std::string& guest_path) {
    // One zone per asset FILE, with identity and size attached: the ONE parse,
    // every member expansion, the TXD parse and the decode graph all nest
    // inside it, so one glance says which file cost the load its time. A stable
    // literal name plus text, not a dynamic name, which would fragment the
    // statistics view and make cross-file totals impossible.
    GEKKO_PROFILE_ZONE("res/parse-file");
    GEKKO_PROFILE_TEXT(guest_path.c_str(), guest_path.size());
    {
        std::lock_guard lock(assets_mutex_);
        ++stats_.files_parsed;
        GEKKO_PROFILE_PLOT("res/files-parsed", static_cast<double>(stats_.files_parsed));
    }
    const std::filesystem::path file = root_ / "files" / guest_path;
    // Blocking host I/O of the ENTIRE archive into a Domain::Streaming buffer,
    // on the worker thread - a separate allocation and a separate read from the
    // mapped view FileProvider hands the guest for the same file, so the same
    // bytes can be paid for twice. `bytes` outlives the zone, so it is declared
    // above the block and filled inside it; `input` is dead after the read and
    // stays in.
    shadow::memory::Vector<u8, shadow::memory::Domain::Streaming> bytes;
    {
        GEKKO_PROFILE_ZONE("res/parse-read");
        std::ifstream input(file, std::ios::binary | std::ios::ate);
        if (!input) {
            GEKKO_PROFILE_MESSAGE_LITERAL("res/parse-file-missing");
            return;
        }
        const auto file_size = input.tellg();
        if (file_size <= 0) return;
        GEKKO_PROFILE_VALUE(static_cast<std::streamoff>(file_size));
        bytes.resize(static_cast<std::size_t>(file_size));
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return;
        // The largest transient allocation in this subsystem, as a quantity
        // over time rather than only as a zone value - this is the plot that
        // makes the double payment above concrete.
        GEKKO_PROFILE_PLOT("res/parse-bytes", static_cast<double>(bytes.size()));
    }
    GEKKO_PROFILE_VALUE(bytes.size());

    if (guest_path.compare(guest_path.size() - 4u, 4u, ".one") == 0) {
        OneArchive archive;
        if (!OneArchive::parse(bytes.data(), bytes.size(), archive)) {
            GEKKO_PROFILE_MESSAGE_LITERAL("res/one-parse-failed");
            return;
        }
        {
            std::lock_guard lock(assets_mutex_);
            ++stats_.ones_parsed;
            GEKKO_PROFILE_PLOT("res/ones-parsed", static_cast<double>(stats_.ones_parsed));
        }
        // One scratch owner for this archive, bounded by its largest expanded
        // member. register_textures joins its entire decode graph before it
        // returns; no decoder retains a view when the next member clears or
        // grows this buffer. Release it with the file, rather than retaining
        // an asset-worker lifetime high-water allocation.
        DecompressionBytes member_bytes;
        for (const OneArchive::Member& member : archive.members()) {
            if (!archive.read_member(member, member_bytes)) continue;
            register_textures(guest_path + ":" + member.name, member_bytes.data(),
                              member_bytes.size());
        }
    } else {
        register_textures(guest_path, bytes.data(), bytes.size());
    }
}

void NativeAssetManager::register_textures(const std::string& source, const u8* data,
                                           std::size_t size) {
    std::vector<TxdReader::Texture> textures;
    if (!TxdReader::parse(data, size, textures)) return;
    {
        std::lock_guard lock(assets_mutex_);
        ++stats_.txds_parsed;
        // Only members that actually parsed as a dictionary; the difference
        // from the res/txd-parse zone count is the speculative probe.
        GEKKO_PROFILE_PLOT("res/txds-parsed", static_cast<double>(stats_.txds_parsed));
    }
    struct Decoded {
        TextureAsset asset;
        u64 hash = 0;
        bool valid = false;
    };
    std::vector<Decoded> decoded(textures.size());
    shadow::execution::TaskGraph graph;
    std::vector<shadow::execution::TaskGraph::TaskId> decoders;
    decoders.reserve(textures.size());
    for (std::size_t index = 0; index < textures.size(); ++index) {
        decoders.push_back(graph.add([&, index] {
            // One zone per texture decoded: gx_decode_texture over the stored
            // texels plus the rgba8.resize that allocates width*height*4 - the
            // CPU cost the native serve path exists to pay once instead of per
            // bind. Bounded by the textures in the parsed dictionaries, a few
            // thousand per route, which is well inside the event budget.
            GEKKO_PROFILE_ZONE("res/texture-decode");
            const auto& texture = textures[index];
            auto& output = decoded[index];
            const std::size_t texel_bytes =
                gx_texture_data_size(texture.gx_format, texture.width, texture.height);
            GEKKO_PROFILE_VALUE(texel_bytes);
            if (texel_bytes == 0u || texture.texel_offset > size || texel_bytes > size - texture.texel_offset)
                return;
            const std::size_t palette_bytes = static_cast<std::size_t>(texture.palette_entries) * 2u;
            if (texture.palette_offset > size || palette_bytes > size - texture.palette_offset) return;
            const u8* texels = data + texture.texel_offset;
            const u8* palette = palette_bytes != 0u ? data + texture.palette_offset : nullptr;
            output.hash = texture_hash(texture.gx_format, texture.tlut_format, texture.depth,
                                       texture.width, texture.height, texels, texel_bytes, palette, palette_bytes);
            auto& asset = output.asset;
            asset.width = texture.width;
            asset.height = texture.height;
            asset.gx_format = texture.gx_format;
            asset.rgba8.resize(static_cast<std::size_t>(texture.width) * texture.height * 4u);
            output.valid = gx_decode_texture(texture.gx_format, texture.width, texture.height, texels,
                                             texel_bytes, palette, texture.tlut_format,
                                             texture.palette_entries, asset.rgba8.data());
        }));
    }
    shadow::execution::TaskGraph::TaskId previous;
    for (std::size_t index = 0; index < decoded.size(); ++index) {
        std::vector<shadow::execution::TaskGraph::TaskId> dependencies{decoders[index]};
        if (index != 0u) dependencies.push_back(previous);
        previous = graph.add([&, index] {
            // BEFORE the lock_guard, so the zone includes the wait for it.
            // These tasks are chained by construction and therefore serialise,
            // and each takes the same mutex the game thread needs in
            // notify_file: this is the exact mechanism by which decoding a
            // dictionary stalls a guest read.
            GEKKO_PROFILE_ZONE("res/texture-commit");
            auto& output = decoded[index];
            std::lock_guard lock(assets_mutex_);
            if (!output.valid) {
                // A texture the renderer will take from the guest-capture path
                // forever; the enclosing res/decode-graph zone says which
                // dictionary it came from.
                ++stats_.textures_failed;
                GEKKO_PROFILE_PLOT("res/textures-failed", static_cast<double>(stats_.textures_failed));
                return;
            }
            const auto hash = output.hash;
            auto& asset = output.asset;
            const std::size_t bytes = asset.rgba8.size();
            if (assets_.count(hash) != 0u) return;  // identical content already resident
            if (bytes > byte_budget_) return;
            if (resident_bytes_ > byte_budget_ - bytes)
                evict_locked(resident_bytes_ - (byte_budget_ - bytes));
            if (resident_bytes_ > byte_budget_ - bytes) return;
            auto entry = std::make_unique<Entry>();
            entry->hash = hash;
            entry->source = source;
            entry->asset = std::make_shared<const TextureAsset>(std::move(asset));
            entry->last_use = ++use_counter_;
            lru_.push_front(hash);
            entry->lru_pos = lru_.begin();
            entry->lru_linked = true;
            // The decoded raster's own heap buffer, which the move into the
            // shared asset carried over unchanged. Taken before the entry is
            // moved into the map, and recorded only on this path - the three
            // early returns above decode without inserting. A zero-byte raster
            // would have a null data(), and two of them would report the same
            // address as a double alloc, so the pair is guarded.
            GEKKO_PROFILE_ONLY(const void* const texels = entry->asset->rgba8.data();)
            assets_[hash] = std::move(entry);
            resident_bytes_ += bytes;
            GEKKO_PROFILE_ONLY(if (bytes != 0u)
                                   GEKKO_PROFILE_ALLOC(texels, bytes, "res/texture-cache");)
            // The central residency pair: the exit report prints one endpoint,
            // but the question is the shape - monotonic climb, plateau, or saw
            // against the budget. Emitted at the only place it rises.
            GEKKO_PROFILE_PLOT("res/resident-bytes", static_cast<double>(resident_bytes_));
            GEKKO_PROFILE_PLOT("res/cache-fill",
                               byte_budget_ != 0u
                                   ? static_cast<double>(resident_bytes_) * 100.0 /
                                         static_cast<double>(byte_budget_)
                                   : 0.0);
            ++stats_.textures_registered;
            GEKKO_PROFILE_PLOT("res/textures-registered",
                               static_cast<double>(stats_.textures_registered));
        }, std::move(dependencies));
    }
    {
        // The whole parallel decode+commit graph for one dictionary as seen by
        // the submitting thread. Executor::execute blocks the caller AND makes
        // it participate in the run, so this is full graph wall time and some
        // decode/commit zones legitimately land on this same thread. It is also
        // where a fan-out of N decodes collapses back into a serial, lock-held
        // commit chain.
        GEKKO_PROFILE_ZONE("res/decode-graph");
        GEKKO_PROFILE_VALUE(textures.size());
        executor_.execute(graph);
    }
}

void NativeAssetManager::evict_locked(std::size_t need_bytes) {
    // Runs with assets_mutex_ held, inside a commit task, and walks the LRU
    // backwards skipping everything the renderer still holds - so under
    // pressure it can scan the whole list and free nothing: an unbounded
    // lock-held loop on the path a guest read blocks behind. This is the site
    // that would explain a load hitch no counter predicts.
    GEKKO_PROFILE_ZONE("res/evict");
    GEKKO_PROFILE_VALUE(need_bytes);
    GEKKO_PROFILE_ONLY(std::size_t pinned = 0;)
    auto candidate = lru_.end();
    while (need_bytes != 0u && candidate != lru_.begin()) {
        --candidate;
        const auto it = assets_.find(*candidate);
        if (it == assets_.end()) {
            candidate = lru_.erase(candidate);
            continue;
        }
        if (it->second->asset.use_count() != 1u) {
            GEKKO_PROFILE_ONLY(++pinned;)
            continue;
        }
        const std::size_t bytes = it->second->asset->rgba8.size();
        GEKKO_PROFILE_ONLY(if (bytes != 0u)
                               GEKKO_PROFILE_FREE(it->second->asset->rgba8.data(),
                                                  "res/texture-cache");)
        candidate = lru_.erase(candidate);
        // Its file must be parseable again: otherwise its textures come from the capture path for the rest of the run.
        if (!it->second->source.empty()) evicted_sources_.push_back(std::move(it->second->source));
        assets_.erase(it);
        resident_bytes_ -= std::min(bytes, resident_bytes_);
        // The only place residency falls, so the curve is exact rather than
        // sampled.
        GEKKO_PROFILE_PLOT("res/resident-bytes", static_cast<double>(resident_bytes_));
        GEKKO_PROFILE_PLOT("res/cache-fill",
                           byte_budget_ != 0u
                               ? static_cast<double>(resident_bytes_) * 100.0 /
                                     static_cast<double>(byte_budget_)
                               : 0.0);
        ++stats_.evictions;
        // Zero on every run measured so far, which is what makes its FIRST
        // movement the interesting event.
        GEKKO_PROFILE_PLOT("res/evictions", static_cast<double>(stats_.evictions));
        need_bytes -= std::min(bytes, need_bytes);
    }
    // Distinguishes "nothing needed evicting" from "nothing COULD be evicted":
    // every skip above is a texture the renderer still holds a reference to,
    // and if the cache ever fails to free memory that is why.
    GEKKO_PROFILE_PLOT("res/evict-pinned", static_cast<double>(pinned));
}

}  // namespace gekko
