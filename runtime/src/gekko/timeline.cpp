#include "gekko/timeline.h"

#include <array>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>

namespace gekko::timeline {

namespace detail {
std::atomic<bool> g_enabled{false};
}

namespace {

struct Record {
    u64 ns;
    u64 a;
    u64 b;
    u32 c;
    u16 event;
    u8 lane;
    u8 thread;
};

constexpr std::size_t kChunkRecords = 1u << 16;  // 2 MiB per chunk

struct ThreadBuffer {
    u8 index = 0;
    std::vector<std::unique_ptr<std::array<Record, kChunkRecords>>> chunks;
    std::size_t used_in_last = kChunkRecords;  // forces the first chunk
    u64 records = 0;
};

struct Registry {
    std::mutex mutex;
    std::filesystem::path path;
    std::size_t capacity = 0;
    std::atomic<u64> reserved{0};  // records reserved across all threads (chunk granularity)
    std::atomic<u64> dropped{0};
    std::vector<std::unique_ptr<ThreadBuffer>> threads;
    u64 chunks = 0;
};

Registry& registry() {
    static Registry instance;
    return instance;
}

thread_local ThreadBuffer* t_buffer = nullptr;

u64 now_ns() noexcept {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

// Takes the registry lock: first record of a thread, or a full chunk.
bool acquire_chunk(ThreadBuffer*& buffer) noexcept {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    if (buffer == nullptr) {
        if (r.threads.size() >= 255u) return false;
        r.threads.push_back(std::make_unique<ThreadBuffer>());
        buffer = r.threads.back().get();
        buffer->index = static_cast<u8>(r.threads.size() - 1u);
    }
    if (r.reserved.load(std::memory_order_relaxed) + kChunkRecords > r.capacity) return false;
    try {
        buffer->chunks.push_back(std::make_unique<std::array<Record, kChunkRecords>>());
    } catch (...) {
        return false;
    }
    r.reserved.fetch_add(kChunkRecords, std::memory_order_relaxed);
    buffer->used_in_last = 0;
    ++r.chunks;
    return true;
}

}  // namespace

const char* event_name(Event event) noexcept {
    switch (event) {
        case Event::PacerWaitBegin: return "PacerWaitBegin";
        case Event::PacerWaitEnd: return "PacerWaitEnd";
        case Event::PacerBehind: return "PacerBehind";
        case Event::HostServiceBegin: return "HostServiceBegin";
        case Event::HostServiceEnd: return "HostServiceEnd";
        case Event::GuestAudioBlock: return "GuestAudioBlock";
        case Event::GuestAudioInterrupt: return "GuestAudioInterrupt";
        case Event::GuestMixBegin: return "GuestMixBegin";
        case Event::GuestMixEnd: return "GuestMixEnd";
        case Event::SegmentPushWaitBegin: return "SegmentPushWaitBegin";
        case Event::SegmentPushWaitEnd: return "SegmentPushWaitEnd";
        case Event::FlipperWaitBegin: return "FlipperWaitBegin";
        case Event::FlipperWaitEnd: return "FlipperWaitEnd";
        case Event::DiscBlockingReadBegin: return "DiscBlockingReadBegin";
        case Event::DiscBlockingReadEnd: return "DiscBlockingReadEnd";
        case Event::StorageWriteBegin: return "StorageWriteBegin";
        case Event::StorageWriteEnd: return "StorageWriteEnd";
        case Event::SavestateBegin: return "SavestateBegin";
        case Event::SavestateEnd: return "SavestateEnd";
        case Event::RenderSegmentBegin: return "RenderSegmentBegin";
        case Event::RenderSegmentEnd: return "RenderSegmentEnd";
        case Event::FenceWaitBegin: return "FenceWaitBegin";
        case Event::FenceWaitEnd: return "FenceWaitEnd";
        case Event::OutputPublished: return "OutputPublished";
        case Event::PresentBegin: return "PresentBegin";
        case Event::PresentEnd: return "PresentEnd";
        case Event::AudioQueued: return "AudioQueued";
        case Event::AudioWake: return "AudioWake";
        case Event::AudioRendered: return "AudioRendered";
        case Event::AudioUnderrun: return "AudioUnderrun";
        case Event::AudioDevicePosition: return "AudioDevicePosition";
        case Event::AudioStateChange: return "AudioStateChange";
        case Event::Count: break;
    }
    return nullptr;
}

void enable(const std::filesystem::path& path, std::size_t capacity_records) {
    Registry& r = registry();
    {
        std::lock_guard lock(r.mutex);
        r.path = path;
        r.capacity = capacity_records < kChunkRecords ? kChunkRecords : capacity_records;
    }
    detail::g_enabled.store(true, std::memory_order_relaxed);
}

void detail::emit_slow(Lane lane, Event event, u64 a, u64 b, u32 c) noexcept {
    ThreadBuffer* buffer = t_buffer;
    if (buffer == nullptr || buffer->used_in_last == kChunkRecords) {
        if (!acquire_chunk(t_buffer)) {
            registry().dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        buffer = t_buffer;
    }
    Record& record = (*buffer->chunks.back())[buffer->used_in_last++];
    record.ns = now_ns();
    record.a = a;
    record.b = b;
    record.c = c;
    record.event = static_cast<u16>(event);
    record.lane = static_cast<u8>(lane);
    record.thread = buffer->index;
    ++buffer->records;
}

bool finish(std::string* error) {
    Registry& r = registry();
    if (!detail::g_enabled.exchange(false, std::memory_order_relaxed)) return true;
    std::lock_guard lock(r.mutex);
    std::ofstream out(r.path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error != nullptr) *error = "timeline: cannot open " + r.path.string();
        return false;
    }
    // Header: magic, version, record size, event name count, then "id name\n"
    // lines, then u64 record count, then records (32 bytes, little endian).
    const char magic[8] = {'G', 'K', 'T', 'L', 'I', 'N', 'E', '1'};
    out.write(magic, sizeof(magic));
    std::string names;
    for (u16 id = 1; id < static_cast<u16>(Event::Count); ++id) {
        if (const char* name = event_name(static_cast<Event>(id))) {
            names += std::to_string(id) + " " + name + "\n";
        }
    }
    const u64 names_size = names.size();
    out.write(reinterpret_cast<const char*>(&names_size), sizeof(names_size));
    out.write(names.data(), static_cast<std::streamsize>(names.size()));
    u64 total = 0;
    for (const auto& thread : r.threads) total += thread->records;
    out.write(reinterpret_cast<const char*>(&total), sizeof(total));
    for (const auto& thread : r.threads) {
        for (std::size_t chunk = 0; chunk < thread->chunks.size(); ++chunk) {
            const std::size_t count =
                chunk + 1u == thread->chunks.size() ? thread->used_in_last : kChunkRecords;
            out.write(reinterpret_cast<const char*>(thread->chunks[chunk]->data()),
                      static_cast<std::streamsize>(count * sizeof(Record)));
        }
    }
    const u64 dropped = r.dropped.load(std::memory_order_relaxed);
    out.write(reinterpret_cast<const char*>(&dropped), sizeof(dropped));
    if (!out && error != nullptr) *error = "timeline: write failed for " + r.path.string();
    return static_cast<bool>(out);
}

Stats stats() noexcept {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    Stats s;
    for (const auto& thread : r.threads) s.records += thread->records;
    s.dropped = r.dropped.load(std::memory_order_relaxed);
    s.threads = r.threads.size();
    s.chunks = r.chunks;
    return s;
}

}  // namespace gekko::timeline
