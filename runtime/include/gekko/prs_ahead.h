#pragma once

// PRS streams decoded ahead of the game's expand calls (docs/GPU_FILL.md "stage start").
//
// What it fixes: a stage start expands megabytes of PRS archive members on the game thread inside one frame (37 % of
// the 17.7-18.5 ms frame 33 of the Westopolis start, measured at 5K fullscreen). The decoder is near this format's
// limit (~1.7 ns per output byte, the literal / match branch); the bytes, though, are known as soon as the game reads
// the archive, a frame or more before it expands them.
//
// How: when the file API delivers part of a ".one" archive into guest memory (NativeFileApi's read observer), the
// archive's member table is parsed once from the mapped host file, and each PRS member whose stream starts inside
// the delivered part is queued to worker threads, which decode it with the expand owner's own token decoder
// (prs_decode_ahead). Decoded streams are found by CONTENT - the hash of their first kKeyBytes bytes - not by
// address: RenderWare's file system reads an unaligned piece through a bounce buffer and copies it into place, so a
// member's first bytes reach the guest at an address the expand call never sees. At the game's expand call, GameMemory asks take(src) for the
// member whose stream starts at src; it serves the call from it only when the guest's stream bytes equal the decoded
// ones, nothing overlaps and no hand-back is due (GameMemory::serve_prs_ahead); otherwise - or when the member is not
// decoded yet - the call decodes as before. The game sees the same bytes and registers either way; timing decides
// only which code made them. A member is dropped when taken; at most kMaxPendingBytes of output wait at a time (the
// oldest go first).
//
// Ownership: System owns it after the asset system (destroyed first: its workers stop and its asset handles are
// released while the resource manager still exists). Thread use: announce and take on the game thread; the
// workers only decode.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "gekko/asset_runtime.h"
#include "gekko/game_memory.h"
#include "gekko/types.h"

namespace gekko {

class PrsAhead {
public:
    explicit PrsAhead(assets::ResourceManager& resources, unsigned workers = 2);
    ~PrsAhead();
    PrsAhead(const PrsAhead&) = delete;
    PrsAhead& operator=(const PrsAhead&) = delete;

    // The file API delivered [offset, offset + length) of asset `id` (`path`) to guest `address`.
    void announce(assets::AssetId id, const std::string& path, u64 offset, u32 address, u64 length);
    // The key of a stream: FNV-1a 64 of its first kKeyBytes bytes.
    static constexpr std::size_t kKeyBytes = 16;
    static u64 key_of(const u8* first) noexcept;
    // A decoded stream with this key that `accept` takes (GameMemory::serve_prs_ahead's checks), removed; null when
    // none is decoded yet or none is accepted.
    std::shared_ptr<const PrsDecoded> take(u64 key, const std::function<bool(const PrsDecoded&)>& accept);

    struct Stats {
        u64 archives = 0;        // archives parsed (once per asset)
        u64 queued = 0;          // members queued
        u64 decoded = 0;         // members decoded ahead
        u64 unqualified = 0;     // members prs_decode_ahead declined
        u64 taken = 0;           // members handed to an expand call
        u64 not_ready = 0;       // expand calls that came before their member was decoded
        u64 dropped = 0;         // members dropped unused (the oldest, over the pending limit)
        u64 refused = 0;         // expand calls whose key matched but whose stream did not
    };
    Stats stats() const;

    static constexpr std::size_t kMaxPendingBytes = std::size_t{96} << 20;

private:
    struct Member {
        std::shared_future<std::shared_ptr<const PrsDecoded>> result;
        std::size_t size = 0;  // the declared output size (pending accounting)
        u32 asset = 0;         // which archive member it is: asset id and stream offset in the file
        u64 stream_at = 0;
        u64 order = 0;         // queue order (eviction: oldest first)
    };
    // An archive's PRS members by stream offset (parsed once per asset), and the mapped file they decode from.
    struct Table {
        std::shared_ptr<assets::AssetHandle> bytes;
        std::vector<std::pair<u64, u32>> streams;  // (file offset of the stream, declared size), sorted
    };
    void work();
    const Table* table(assets::AssetId id);
    void evict_oldest();

    assets::ResourceManager& resources_;
    std::multimap<u64, Member> members_;                  // by key
    std::map<std::pair<u32, u64>, u64> pending_members_;  // (asset, stream offset) -> key, while waiting
    std::map<u32, Table> tables_;                         // by asset id
    std::size_t pending_bytes_ = 0;
    u64 next_order_ = 0;
    Stats stats_;

    mutable std::mutex mutex_;  // the queue and the stats the workers touch
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
    bool stop_ = false;
    std::vector<std::thread> workers_;
};

}  // namespace gekko
