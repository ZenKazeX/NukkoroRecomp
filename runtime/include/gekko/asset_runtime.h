#pragma once

// The Native Asset Runtime.
//
// ---------------------------------------------------------------------------
// WHAT THIS REPLACES
// ---------------------------------------------------------------------------
//
// A GameCube disc, modelled in software, for a game that ships as files on an
// SSD. The old path was:
//
//     guest path string
//       -> FST walk (12-byte entries) to an entrynum
//       -> DVDFileInfo.startAddr, a DISC BYTE OFFSET
//       -> startAddr + offset, a position on a spinning platter
//       -> a residency budget, a read-order seek profile, an MMIO register
//          model (DICMDBUF/DIMAR/DILENGTH/DICR), a DMA engine and a watchdog
//       -> 1.29 GiB of files bulk-copied into private RAM to hide seek latency
//
// Every layer of that exists to defeat a 100 ms seek at 3 MB/s. None of it
// survives the port.
//
// The decode that authorised deleting it, in one line: a file entry's disc
// offset is copied verbatim into DVDFileInfo.startAddr and the ONLY arithmetic
// ever performed on it anywhere in the image is `startAddr + caller_offset`.
// Nothing compares two offsets, nothing seeks, nothing assumes adjacency.
// **Any injective per-file base value works.** So identity stops being a
// position on a disc and becomes an AssetId.
//
// ---------------------------------------------------------------------------
// THE SHAPE
// ---------------------------------------------------------------------------
//
//     AssetId            a dense runtime identity; not an offset, not an index
//        |
//     AssetRegistry      name -> AssetId -> AssetRecord{size, provider, key}
//        |
//     ResourceManager    acquire/release, refcounts, AssetHandle lifetime
//        |
//     StreamingScheduler explicit async: submit, then drain on the owner thread
//        |
//     AssetProvider      mapped view (files) or materialised bytes (archives)
//        |
//     Decompression      PRS, as a provider, not as a special case
//        |
//     AssetHandle        a borrowed, refcounted span the consumer reads
//
// ---------------------------------------------------------------------------
// THE CONTRACTS THIS MUST HONOUR (recovered, not invented)
// ---------------------------------------------------------------------------
//
// These come from the guest and are game-visible. They are the reason this is
// a rewrite and not a free-for-all.
//
//  1. COMPLETION IS DELIVERED ON THE OWNING THREAD, NEVER FROM I/O.
//     The game's own loader defers every completion to its main-thread task
//     walk and runs the user callback from a Task's Exec. A host that called a
//     consumer back from a worker would reorder game state against a contract
//     the whole loader is built on. `pump()` is the only place completions
//     fire, and it runs where the consumer lives.
//
//  2. NO PARTIAL COMPLETION, NO RESUME.
//     A request's bytes are complete and contiguous before it is reported
//     done. The guest's whole-file read has no partial path, and its only
//     completion signal is a memory flag - not a byte count, not a percentage,
//     not an ETA. Anything that can be observed half-done is a new state the
//     game has no code for.
//
//  3. STRICT PRIORITY, THEN FIFO WITHIN A PRIORITY.
//     Submission order is completion order at equal priority. Reordering
//     changes which of two pending loads lands first, and downstream code
//     depends on it.
//
//  4. A MISSING ASSET FAILS AT SUBMIT, BEFORE ANYTHING IS QUEUED.
//     The guest checks existence and returns false without allocating. A
//     replacement that queued and then failed would reach error paths the
//     game does not have.
//
//  5. READS PAST END-OF-FILE ARE LEGAL AND ZERO-FILL.
//     Both live issuers round length up to a 32-byte multiple, so a read of
//     the last bytes of a file always asks for up to 31 bytes that are not
//     there. That succeeds, and the tail reads as zero.
//
//  6. NO ARTIFICIAL LATENCY.
//     Where the original's timing came from the media being slow, this
//     completes as soon as the bytes are ready. Where game logic genuinely
//     depends on ordering, that is expressed as explicit state synchronisation
//     - a priority, a dependency, a pump boundary - never as a delay.
//
// ---------------------------------------------------------------------------
// WHAT IS DELIBERATELY ABSENT
// ---------------------------------------------------------------------------
//
// Grep for these and find nothing: disc offset, seek, sector, platter, LBA,
// residency budget, read-order profile, DI register, DMA, ARAM staging,
// rotational latency. Their absence is the point of the module.

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gekko/types.h"

namespace gekko::assets {

// ---------------------------------------------------------------------------
// AssetId
// ---------------------------------------------------------------------------

// A dense runtime identity. Zero is reserved for "none", so a
// default-constructed id is invalid rather than accidentally meaning the first
// asset - the mistake a raw index invites.
struct AssetId {
    u32 value = 0;

    constexpr bool valid() const noexcept { return value != 0u; }
    constexpr bool operator==(const AssetId& o) const noexcept { return value == o.value; }
    constexpr bool operator!=(const AssetId& o) const noexcept { return value != o.value; }
};

// ---------------------------------------------------------------------------
// Providers - where bytes actually come from
// ---------------------------------------------------------------------------

enum class ProviderKind : u16 {
    None = 0,
    File,      // a file on the host filesystem, served as a mapped view
    Archive,   // a member of a ONE container
    Decoded,   // a PRS-compressed member, expanded once and cached
    Memory,    // bytes the runtime owns: a native replacement of a game file
};

// A borrowed, read-only span. Not an owner: lifetime belongs to whatever the
// handle holds open.
struct ByteSpan {
    const u8* data = nullptr;
    u64 size = 0;
    constexpr bool valid() const noexcept { return data != nullptr; }
};

class AssetProvider {
public:
    virtual ~AssetProvider() = default;
    virtual const char* name() const noexcept = 0;

    // The whole asset, in place, with no copy. Returns an invalid span when
    // this provider cannot serve it without materialising.
    virtual ByteSpan map(u64 key, u64 size) = 0;

    // Copy `length` bytes at `offset`. Short by design: a range that runs past
    // the end succeeds and zero-fills the tail, because every caller rounds
    // its length up to 32 bytes and would otherwise fail on every last read of
    // every file.
    virtual bool read(u64 key, u64 offset, u64 length, u8* dest, u64& got) = 0;
};

// ---------------------------------------------------------------------------
// AssetRegistry - identity, and nothing else
// ---------------------------------------------------------------------------

struct AssetRecord {
    AssetId id{};
    u64 size = 0;
    ProviderKind provider = ProviderKind::None;
    u64 key = 0;        // provider-private locator: a file index, a member index
    std::string path;   // the logical name, which is the only name the game has
};

class AssetRegistry {
public:
    // Interns a logical path. Re-interning the same path returns the same id,
    // so identity is stable for the life of the run - which is what lets a
    // handle outlive the lookup that produced it.
    AssetId intern(std::string path, u64 size, ProviderKind provider, u64 key);

    AssetId find(std::string_view path) const noexcept;
    const AssetRecord* get(AssetId id) const noexcept;

    std::size_t count() const noexcept { return records_.size(); }
    void clear() noexcept;

    // Every record, for the compatibility bridge that still has to publish a
    // guest-visible directory.
    const std::vector<AssetRecord>& records() const noexcept { return records_; }

private:
    std::vector<AssetRecord> records_;  // index = id.value - 1
    std::unordered_map<std::string, u32> by_path_;
    mutable std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// AssetAddressSpace - the identity the GUEST sees
// ---------------------------------------------------------------------------
//
// Recompiled guest code still addresses assets with a single 32-bit token: the
// FST's word1 is copied verbatim into DVDFileInfo.startAddr (0x8037AB34) and
// the only arithmetic ever performed on it is `startAddr + caller_offset`
// (0x8037AFA4, 0x8037B08C). That token used to be a position on a platter.
// Here it is a base allocated by the registry, and the platter is gone.
//
// Layout: page 0 is reserved, so address 0 - an uninitialised startAddr -
// always faults instead of silently reading the first asset. Each asset gets
// the next free run of 64 KiB pages large enough to hold its bytes plus the
// guest's own bounded overrun. Decode is one array index; there is no search,
// no tree and no ordering relationship between neighbours.
//
// Measured over the real 3,480-file table: 1.411 GiB of address space used,
// 64.7% of u32 still free, 23,112 page entries = 90 KiB of RAM.
//
// See docs/storage-2026-09-21/BRIDGE_DESIGN.md and DVD_ENTRY_DECODE_VERIFIED.md.
class AssetAddressSpace {
public:
    static constexpr u32 kPageShift = 16;
    static constexpr u32 kPageSize = 1u << kPageShift;

    // The space begins at 1.5 GiB, and nothing below that can ever decode.
    //
    // This is not padding. Almost every 32-bit value in the guest that is NOT
    // an asset base is a small one: zero from an uninitialised DVDFileInfo, a
    // count, an index, the low half of a pointer - and, specifically, any
    // retired disc offset, because the largest one this image ever used was
    // 1.27 GiB. Basing the space above all of them turns every such value into
    // a COUNTED decode failure instead of a silent hit on whichever asset
    // happened to be allocated there. The whole point of an injective map is
    // that misuse is detectable; a map starting at zero would have handed a
    // stale disc offset somebody else's bytes and said nothing.
    //
    // 1.5 GiB + the 1.41 GiB the real table needs leaves 27% of u32 spare.
    static constexpr u32 kSpaceBase = 0x60000000u;

    // NOT slack. Both guest read entries assert
    //     offset + length < fileInfo->length + 0x20
    // and OSPanic otherwise (0x8037AF64 / 0x8037B050), so the highest address a
    // legal read can touch is base + size + 31. A region that contains
    // size + 32 therefore contains every legal read by construction, and a
    // request that leaves the region is provably malformed rather than a
    // neighbour's bytes.
    static constexpr u32 kTailAllowance = 32;

    enum class Status : u8 {
        Ok = 0,
        Unmapped,     // no asset owns this page
        OutOfRange,   // inside an asset, but the request leaves its region
    };

    struct Decoded {
        AssetId id{};
        u64 offset = 0;
        u64 size = 0;  // the asset's real size; bytes past it read as zero
    };

    // Assigns a base to every record in the registry, in registry order.
    // Returns false only if the assets do not fit in 32 bits of address.
    bool build(const AssetRegistry& registry);
    void clear() noexcept;

    // 0 when the id is unknown. A valid base is never 0 (page 0 is reserved).
    u32 base_of(AssetId id) const noexcept;

    Status decode(u32 address, u64 length, Decoded& out) const noexcept;

    bool empty() const noexcept { return base_.size() <= 1u; }
    std::size_t asset_count() const noexcept { return base_.empty() ? 0u : base_.size() - 1u; }
    // Bytes of address space the assets actually occupy. NOT the end address:
    // the space starts at kSpaceBase, and reporting that as "space used" would
    // add 1.5 GiB of deliberately-unreachable range to every figure.
    u64 span_bytes() const noexcept { return end_ == 0u ? 0u : end_ - kSpaceBase; }
    u64 end_address() const noexcept { return end_; }
    std::size_t page_count() const noexcept { return page_owner_.size(); }
    u64 table_bytes() const noexcept {
        return static_cast<u64>(page_owner_.size()) * sizeof(u32) +
               static_cast<u64>(base_.size()) * (sizeof(u32) + sizeof(u64));
    }

private:
    std::vector<u32> page_owner_;  // page index -> AssetId::value, 0 = unmapped
    std::vector<u32> base_;        // [id.value] -> base address, [0] unused
    std::vector<u64> size_;        // [id.value] -> asset size
    u64 end_ = 0;                  // one past the last assigned byte
};

// ---------------------------------------------------------------------------
// AssetHandle - lifetime
// ---------------------------------------------------------------------------

class ResourceManager;

// A refcounted claim on an asset's bytes. While one exists the bytes stay
// valid; when the last one drops the manager may release them. Move-only: a
// silently copied handle is a refcount bug waiting to happen.
class AssetHandle {
public:
    AssetHandle() = default;
    ~AssetHandle();
    AssetHandle(AssetHandle&& other) noexcept;
    AssetHandle& operator=(AssetHandle&& other) noexcept;
    AssetHandle(const AssetHandle&) = delete;
    AssetHandle& operator=(const AssetHandle&) = delete;

    bool valid() const noexcept { return manager_ != nullptr && span_.valid(); }
    AssetId id() const noexcept { return id_; }
    ByteSpan bytes() const noexcept { return span_; }
    u64 size() const noexcept { return span_.size; }

    void reset() noexcept;

private:
    friend class ResourceManager;
    AssetHandle(ResourceManager* manager, AssetId id, ByteSpan span) noexcept
        : manager_(manager), id_(id), span_(span) {}

    ResourceManager* manager_ = nullptr;
    AssetId id_{};
    ByteSpan span_{};
};

// ---------------------------------------------------------------------------
// ResourceManager - acquisition and lifetime
// ---------------------------------------------------------------------------

class ResourceManager {
public:
    ResourceManager(AssetRegistry& registry) noexcept : registry_(registry) {}

    void set_provider(ProviderKind kind, AssetProvider* provider) noexcept;
    AssetProvider* provider(ProviderKind kind) const noexcept;

    // Acquire the whole asset. Synchronous, because for a mapped file it costs
    // nothing: the bytes are the page cache's and the first touch is a fault,
    // not a read. Callers that need it off the critical path use the scheduler.
    AssetHandle acquire(AssetId id);

    // A range copy, for the compatibility bridge, which speaks offsets because
    // the guest code behind it still does.
    bool read(AssetId id, u64 offset, u64 length, u8* dest, u64& got);

    struct Stats {
        u64 acquires = 0;
        u64 releases = 0;
        u64 live = 0;
        u64 peak_live = 0;
        u64 map_hits = 0;      // served in place, zero copies
        u64 materialised = 0;  // had to be produced (archive member, PRS)
        u64 misses = 0;        // unknown id or provider refusal
    };
    Stats stats() const noexcept;

private:
    friend class AssetHandle;
    void release(AssetId id) noexcept;

    struct Entry {
        ByteSpan span{};
        std::vector<u8> owned;  // only for materialised assets
        u32 refs = 0;
        bool resident = false;
    };

    AssetRegistry& registry_;
    std::vector<AssetProvider*> providers_{5, nullptr};
    std::unordered_map<u32, Entry> entries_;
    mutable std::mutex mutex_;
    mutable Stats stats_{};
};

// ---------------------------------------------------------------------------
// StreamingScheduler - explicit async, drained where the consumer lives
// ---------------------------------------------------------------------------

// Lower value = more urgent. The guest runs three queues and takes the head of
// the most urgent non-empty one, one per task walk.
enum class Priority : u8 { High = 0, Normal = 1, Low = 2, Count = 3 };

struct Request {
    AssetId id{};
    u64 offset = 0;
    u64 length = 0;
    u8* dest = nullptr;     // caller-owned; must outlive the request
    Priority priority = Priority::Normal;
    u64 user = 0;           // opaque, returned verbatim in the completion
};

struct Completion {
    AssetId id{};
    u64 user = 0;
    u64 transferred = 0;
    bool ok = false;
};

class StreamingScheduler {
public:
    StreamingScheduler(ResourceManager& resources) noexcept : resources_(resources) {}

    // Rejects an unknown asset IMMEDIATELY and returns false, queueing
    // nothing - the guest checks existence before it allocates, and a
    // replacement that failed later would reach error paths the game lacks.
    bool submit(const Request& request);

    // Runs pending work and returns the completions that finished, in strict
    // priority-then-FIFO order. Call it where the consumer lives: this is the
    // only place a completion is ever observed, which is what keeps the
    // main-thread delivery contract true by construction rather than by
    // discipline.
    //
    // `budget` caps how many requests are retired in one call, mirroring the
    // guest's one-per-task-walk behaviour. Zero means no cap.
    std::size_t pump(std::vector<Completion>& out, std::size_t budget = 0);

    std::size_t pending() const noexcept;
    bool idle() const noexcept { return pending() == 0; }

    struct Stats {
        u64 submitted = 0;
        u64 rejected = 0;
        u64 completed = 0;
        u64 failed = 0;
        u64 bytes = 0;
        u64 peak_pending = 0;
    };
    Stats stats() const noexcept { return stats_; }

private:
    ResourceManager& resources_;
    // One FIFO per priority. Strict priority between them, insertion order
    // within one - the recovered ordering, expressed directly.
    std::vector<Request> queues_[static_cast<std::size_t>(Priority::Count)];
    mutable std::mutex mutex_;
    Stats stats_{};
};

}  // namespace gekko::assets
