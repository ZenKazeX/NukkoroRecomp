#pragma once

// The providers: the only code in the asset runtime that knows where bytes
// physically live. Everything above them speaks AssetId.
//
// There are three, and the shape of each is dictated by what it can do without
// copying:
//
//   FileProvider     a host file, mapped. Serves in place, so a "load" is a
//                    page fault and nothing else. No read-ahead, no residency
//                    budget, no warm-up - the OS page cache is the cache, and
//                    it is shared, evictable and already warm on the second run.
//
//   ArchiveProvider  a member of a ONE container. The container itself is a
//                    mapped file, so an uncompressed member is ALSO served in
//                    place, as a subspan. Only a compressed one costs anything.
//
//   PrsProvider      a compressed member, expanded once through the verified
//                    prs_expand and then cached by the ResourceManager for as
//                    long as a handle is held.
//
//   MemoryProvider   bytes the runtime itself owns: a game file replaced by
//                    native content (asset_mount.h AssetSystem::replace).
//                    Served in place.
//
// Sofdec streams need no provider of their own: a .sfd is a host file and the
// stream pump reads ranges of it, which is exactly FileProvider::read. The
// ring buffer, the low-water hysteresis and the frame pacing live with the
// consumer, where the recovered behaviour puts them - not in storage.

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gekko/asset_runtime.h"
#include "gekko/types.h"

namespace gekko::assets {

// A read-only mapping of a whole host file. The pages are the operating
// system's page-cache pages: no second copy exists, and nothing is committed
// to this process that the OS cannot evict.
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();
    MappedFile(MappedFile&&) noexcept;
    MappedFile& operator=(MappedFile&&) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    bool open(const std::filesystem::path& path);
    void close() noexcept;

    const u8* data() const noexcept { return base_; }
    u64 size() const noexcept { return size_; }
    bool valid() const noexcept { return base_ != nullptr; }

    // Ask the OS to bring this mapping's pages in, without copying and without
    // committing anything private. Asynchronous and advisory: it returns
    // immediately and the pages arrive on the system's own time.
    //
    // This is what replaces the 1,294 MiB bulk preload. Deleting that load
    // removed a second resident copy of the page cache, but it also removed
    // the thing that was hiding cold-start latency: with a cold page cache the
    // FIRST touch of a file is now a major fault taken inline on the game
    // thread, inside the guest's own read. Warming is the honest fix -- it
    // keeps the bytes in the OS cache, where they are shared and evictable,
    // instead of copying them somewhere they are neither.
    void prefault() const noexcept;

private:
    const u8* base_ = nullptr;
    u64 size_ = 0;
    void* file_handle_ = nullptr;
    void* mapping_handle_ = nullptr;
};

// Host files, mapped lazily. `key` is an index into the path table, so an
// asset that is never touched costs one string and nothing else.
//
// Opening is the one host cost a read can pay besides the copy: a cold
// CreateFileW + CreateFileMappingW took 14-71 % of each measured cold
// stage-load stall on the game thread (findings-20261019-ES, ES_REPORT 2).
// preopen() moves that cost to a background thread at mount. It changes
// nothing a read returns or when: reads stay synchronous (the file API's
// contract), a file is opened exactly once by whichever thread gets there
// first (the other waits for that one open), and a failed open stays failed.
class FileProvider final : public AssetProvider {
public:
    FileProvider() = default;
    ~FileProvider() override;
    FileProvider(const FileProvider&) = delete;
    FileProvider& operator=(const FileProvider&) = delete;

    const char* name() const noexcept override { return "file"; }

    // Returns the key to store in the registry record.
    u64 add(std::filesystem::path path);

    // Opens and maps every file added so far on one background thread (below
    // normal priority), in key order. Once per provider; files added later
    // open on first use as before. GEKKO_ASSET_PREOPEN=0 turns it off: a
    // diagnostic control for the before/after load-stall measurement (host
    // file-open timing only - nothing the game can see changes).
    void preopen();

    ByteSpan map(u64 key, u64 size) override;
    bool read(u64 key, u64 offset, u64 length, u8* dest, u64& got) override;

    struct Stats {
        u64 known = 0;
        u64 mapped = 0;
        u64 mapped_bytes = 0;
        u64 failures = 0;
        u64 preopened = 0;  // of `mapped` + `failures`: opened by the background thread
    };
    Stats stats() const noexcept;

private:
    struct Slot {
        std::filesystem::path path;
        MappedFile file;
        std::once_flag opened;  // one open per file, by whichever thread comes first
    };
    Slot* mapped_slot(u64 key);
    void open_slot(Slot& slot, bool background);

    std::vector<std::unique_ptr<Slot>> slots_;
    mutable std::mutex mutex_;  // slots_ and stats_; never held across an open
    Stats stats_{};
    std::thread preopener_;
    std::atomic<bool> stop_{false};
};

// A member of a ONE container. The container is mapped once; an uncompressed
// member is then a subspan of it and costs nothing at all.
class ArchiveProvider final : public AssetProvider {
public:
    const char* name() const noexcept override { return "archive"; }

    struct Member {
        u64 container = 0;   // index into the container table
        u64 offset = 0;      // byte offset of the member inside the container
        u64 size = 0;        // stored size
        u64 expanded = 0;    // 0 when the member is not compressed
    };

    u64 add_container(std::filesystem::path path);
    u64 add_member(const Member& member);
    const Member* member(u64 key) const noexcept;

    ByteSpan map(u64 key, u64 size) override;
    bool read(u64 key, u64 offset, u64 length, u8* dest, u64& got) override;

    // The stored bytes of a member, mapped in place. The PRS provider reads
    // its input through this, so a compressed member is never copied before
    // being expanded.
    ByteSpan stored(u64 key);

private:
    std::vector<std::unique_ptr<MappedFile>> containers_;
    std::vector<std::filesystem::path> container_paths_;
    std::vector<Member> members_;
    mutable std::mutex mutex_;
};

// A compressed member, expanded through the verified PRS implementation.
// Cannot serve in place, by definition - map() returns an invalid span and the
// ResourceManager materialises it once.
class PrsProvider final : public AssetProvider {
public:
    explicit PrsProvider(ArchiveProvider& archive) noexcept : archive_(archive) {}

    const char* name() const noexcept override { return "prs"; }
    ByteSpan map(u64, u64) override { return ByteSpan{}; }
    bool read(u64 key, u64 offset, u64 length, u8* dest, u64& got) override;

    struct Stats {
        u64 expansions = 0;
        u64 expanded_bytes = 0;
        u64 failures = 0;
    };
    Stats stats() const noexcept;

private:
    ArchiveProvider& archive_;
    mutable std::mutex mutex_;
    Stats stats_{};
};

// Whole files held in memory. Added before the mount and never changed after,
// so reads need no lock.
class MemoryProvider final : public AssetProvider {
public:
    const char* name() const noexcept override { return "memory"; }

    // Returns the key to store in the registry record.
    u64 add(std::vector<u8> bytes);

    ByteSpan map(u64 key, u64 size) override;
    bool read(u64 key, u64 offset, u64 length, u8* dest, u64& got) override;

private:
    std::vector<std::vector<u8>> files_;
};

}  // namespace gekko::assets
