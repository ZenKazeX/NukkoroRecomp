// PRS streams decoded ahead of the game's expand calls. Contract: include/gekko/prs_ahead.h.

#include "gekko/prs_ahead.h"

#include <algorithm>
#include <cctype>
#include <chrono>

#include "gekko/native_assets.h"
#include "gekko/thread_name.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace gekko {

namespace {

constexpr u64 kOneDataOffset = 0x0Cu;  // a member's stored offset counts from file + 0x0C (OneArchive)

bool is_one_archive(const std::string& path) {
    if (path.size() < 4u) return false;
    std::string ext = path.substr(path.size() - 4u);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char ch) { return static_cast<char>(std::tolower(ch)); });
    return ext == ".one";
}

}  // namespace

PrsAhead::PrsAhead(assets::ResourceManager& resources, unsigned workers) : resources_(resources) {
    for (unsigned i = 0; i < workers; ++i) workers_.emplace_back([this] { work(); });
}

PrsAhead::~PrsAhead() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        queue_.clear();
    }
    wake_.notify_all();
    for (std::thread& t : workers_) t.join();
    members_.clear();
    tables_.clear();  // the asset handles, while the resource manager exists
}

void PrsAhead::work() {
    name_this_thread("prs-ahead");
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        job();
    }
}

const PrsAhead::Table* PrsAhead::table(assets::AssetId id) {
    const auto found = tables_.find(id.value);
    if (found != tables_.end()) return &found->second;
    Table& t = tables_[id.value];
    t.bytes = std::make_shared<assets::AssetHandle>(resources_.acquire(id));
    if (!t.bytes->valid()) return &t;
    const assets::ByteSpan span = t.bytes->bytes();
    OneArchive archive;
    if (OneArchive::parse(span.data, static_cast<std::size_t>(span.size), archive)) {
        for (const OneArchive::Member& member : archive.members()) {
            const u64 at = u64{member.offset} + kOneDataOffset;
            if ((member.flags & 1u) != 0u && at < span.size) t.streams.emplace_back(at, member.size);
        }
        std::sort(t.streams.begin(), t.streams.end());
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.archives;
    return &t;
}

u64 PrsAhead::key_of(const u8* first) noexcept {
    u64 h = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < kKeyBytes; ++i) {
        h ^= first[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

void PrsAhead::evict_oldest() {
    auto oldest = members_.end();
    for (auto m = members_.begin(); m != members_.end(); ++m) {
        if (oldest == members_.end() || m->second.order < oldest->second.order) oldest = m;
    }
    if (oldest == members_.end()) return;
    pending_bytes_ -= oldest->second.size;
    pending_members_.erase({oldest->second.asset, oldest->second.stream_at});
    members_.erase(oldest);
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.dropped;
}

void PrsAhead::announce(assets::AssetId id, const std::string& path, u64 offset, u32 address, u64 length) {
    (void)address;  // found by content (see the header): where the bytes landed does not matter
    if (length == 0u || !id.valid() || !is_one_archive(path)) return;
    const Table* t = table(id);
    if (t == nullptr || t->streams.empty()) return;
    const assets::ByteSpan span = t->bytes->bytes();
    const std::shared_ptr<assets::AssetHandle> bytes = t->bytes;
    // Every member from the delivered part to the archive's end: the ones whose stream starts in this part (again,
    // if one was taken before - a second load of the archive), and the later ones once, ahead of their own reads
    // (the game reads a member and expands it in the same frame, too late for a worker that starts at the read).
    auto stream = std::lower_bound(t->streams.begin(), t->streams.end(), std::pair<u64, u32>{offset, 0u});
    for (; stream != t->streams.end(); ++stream) {
        if (stream->first + kKeyBytes > span.size) continue;
        if (pending_members_.count({id.value, stream->first}) != 0u) continue;  // queued, not taken yet
        const bool delivered = stream->first < offset + length;
        if (!delivered && !prefetched_.insert({id.value, stream->first}).second) continue;  // ahead once
        const u32 size = stream->second;
        if (size > kMaxPendingBytes) continue;
        while (!members_.empty() && pending_bytes_ + size > kMaxPendingBytes) evict_oldest();
        const u8* data = span.data + stream->first;
        const std::size_t available = static_cast<std::size_t>(span.size - stream->first);
        auto task = std::make_shared<std::packaged_task<std::shared_ptr<const PrsDecoded>()>>(
            [this, data, available, size, bytes]() -> std::shared_ptr<const PrsDecoded> {
                auto decoded = std::make_shared<PrsDecoded>();
                const bool ok = prs_decode_ahead(data, available, size, *decoded);
                std::lock_guard<std::mutex> lock(mutex_);
                if (!ok) {
                    ++stats_.unqualified;
                    return nullptr;
                }
                ++stats_.decoded;
                return decoded;
            });
        const u64 key = key_of(data);
        members_.emplace(key, Member{task->get_future().share(), size, id.value, stream->first, next_order_++});
        pending_members_[{id.value, stream->first}] = key;
        pending_bytes_ += size;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back([task] { (*task)(); });
            ++stats_.queued;
        }
        wake_.notify_one();
    }
}

std::shared_ptr<const PrsDecoded> PrsAhead::take(u64 key, const std::function<bool(const PrsDecoded&)>& accept) {
    const auto range = members_.equal_range(key);
    bool waiting = false, matched = false;
    for (auto m = range.first; m != range.second; ++m) {
        if (m->second.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            waiting = true;
            continue;
        }
        const std::shared_ptr<const PrsDecoded> decoded = m->second.result.get();
        if (decoded == nullptr) {
            // The decode failed: the entry can never serve; it leaves the table (and the budget), so a later
            // announce can queue the stream again.
            pending_bytes_ -= m->second.size;
            pending_members_.erase({m->second.asset, m->second.stream_at});
            members_.erase(m);
            break;  // the range is invalidated; a sibling (another stream of the key) waits for the next take
        }
        matched = true;
        if (!accept(*decoded)) continue;
        pending_bytes_ -= m->second.size;
        pending_members_.erase({m->second.asset, m->second.stream_at});
        members_.erase(m);
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.taken;
        return decoded;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (waiting) ++stats_.not_ready;
    else if (matched) ++stats_.refused;
    return nullptr;
}

bool PrsAhead::ready(u64 key, const std::function<bool(const PrsDecoded&)>& check) {
    const auto range = members_.equal_range(key);
    for (auto m = range.first; m != range.second; ++m) {
        if (m->second.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) continue;
        const std::shared_ptr<const PrsDecoded> decoded = m->second.result.get();
        if (decoded != nullptr && check(*decoded)) return true;
    }
    return false;
}

PrsAhead::Stats PrsAhead::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

}  // namespace gekko
