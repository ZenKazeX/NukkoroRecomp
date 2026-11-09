#include "render/gpu_memory.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

#include "render/gpu_device.h"

#include <D3D12MemAlloc.h>

// GPU memory trace (gpu_memory.h). MEASUREMENT ONLY: everything here runs only with GEKKO_GPU_MEMORY set.

namespace shadow::render {

namespace {

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string utf8(const wchar_t* text) {
    if (text == nullptr || text[0] == L'\0')
        return std::string();
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
        return std::string();
    std::string out(static_cast<std::size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

// The persistent mappings: CPU address -> (name, bytes). Under a leaf lock; any thread.
struct Mappings {
    std::mutex mutex;
    std::map<const void*, std::pair<std::string, std::uint64_t>> live;
    std::uint64_t bytes = 0;
    bool file_started = false;
};

Mappings& mappings() {
    static Mappings m;
    return m;
}

void append_mapping_event(Mappings& m, const char* event, const std::string& name, const void* cpu,
                          std::uint64_t bytes) {
    const std::string file = gpu_memory::path() + ".maps.csv";
    std::FILE* out = std::fopen(file.c_str(), m.file_started ? "ab" : "wb");
    if (out == nullptr)
        return;
    if (!m.file_started) {
        std::fputs("# shadow gpu memory mappings 1, times in steady_clock ns\nns,event,name,begin,end,bytes,mapped_total\n",
                   out);
        m.file_started = true;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(cpu);
    std::fprintf(out, "%lld,%s,%s,0x%016llX,0x%016llX,%llu,%llu\n", static_cast<long long>(now_ns()), event,
                 name.c_str(), static_cast<unsigned long long>(begin),
                 static_cast<unsigned long long>(begin + bytes), static_cast<unsigned long long>(bytes),
                 static_cast<unsigned long long>(m.bytes));
    std::fclose(out);
}

constexpr const char* kHeapNames[5] = {"default", "upload", "readback", "custom", "gpu_upload"};
constexpr std::uint64_t kMapRise = 32ull << 20;
constexpr int kMostMaps = 32;
constexpr std::int64_t kRowInterval = 1'000'000'000;

}  // namespace

namespace gpu_memory {

const std::string& path() {
    static const std::string value = [] {
        const char* p = std::getenv("GEKKO_GPU_MEMORY");
        return p != nullptr ? std::string(p) : std::string();
    }();
    return value;
}

void note_map(const wchar_t* name, const void* cpu, std::uint64_t bytes) {
    if (!enabled() || cpu == nullptr)
        return;
    Mappings& m = mappings();
    std::lock_guard<std::mutex> lock(m.mutex);
    const std::string n = utf8(name);
    m.live[cpu] = {n, bytes};
    m.bytes += bytes;
    append_mapping_event(m, "map", n, cpu, bytes);
}

void note_unmap(const void* cpu) {
    if (!enabled() || cpu == nullptr)
        return;
    Mappings& m = mappings();
    std::lock_guard<std::mutex> lock(m.mutex);
    const auto found = m.live.find(cpu);
    if (found == m.live.end())
        return;
    const std::string name = found->second.first;
    const std::uint64_t bytes = found->second.second;
    m.bytes -= bytes;
    m.live.erase(found);
    append_mapping_event(m, "unmap", name, cpu, bytes);
}

}  // namespace gpu_memory

struct GpuMemoryTrace::State {
    std::int64_t last_row_ns = 0;
    bool header_written = false;
    int maps = 0;
    std::uint64_t non_local_at_last_map = 0;
    bool first = true;
};

GpuMemoryTrace::GpuMemoryTrace() : state_(std::make_unique<State>()) {}
GpuMemoryTrace::~GpuMemoryTrace() = default;

void GpuMemoryTrace::tick(Device& device, const Counters& counters) {
    const std::int64_t now = now_ns();
    if (!state_->first && now - state_->last_row_ns < kRowInterval)
        return;
    row(device, counters, state_->first, state_->first ? "start" : "rise");
}

void GpuMemoryTrace::finish(Device& device, const Counters& counters) {
    row(device, counters, true, "end");
}

void GpuMemoryTrace::row(Device& device, const Counters& counters, bool force_map, const char* why) {
    State& s = *state_;
    D3D12MA::Allocator* allocator = device.allocator();
    if (allocator == nullptr)
        return;
    const std::int64_t now = now_ns();
    s.last_row_ns = now;
    s.first = false;
    D3D12MA::TotalStatistics total{};
    allocator->CalculateStatistics(&total);
    D3D12MA::Budget local{}, non_local{};
    allocator->GetBudget(&local, &non_local);
    std::uint64_t mapped = 0;
    {
        Mappings& m = mappings();
        std::lock_guard<std::mutex> lock(m.mutex);
        mapped = m.bytes;
    }
    const std::string& file = gpu_memory::path();
    std::FILE* out = std::fopen(file.c_str(), s.header_written ? "ab" : "wb");
    if (out != nullptr) {
        if (!s.header_written) {
            std::fputs("# shadow gpu memory trace 1, times in steady_clock ns, bytes\nns,frame", out);
            for (const char* h : kHeapNames)
                std::fprintf(out, ",%s_blocks,%s_block_bytes,%s_allocations,%s_allocation_bytes", h, h, h, h);
            std::fputs(",local_block_bytes,non_local_block_bytes,local_usage,local_budget,non_local_usage,"
                       "non_local_budget,mapped_bytes,copy_allocators_created,copy_allocators_spare,"
                       "copy_submissions_in_flight,upload_submissions,upload_staged_bytes,upload_ring_waits,"
                       "upload_dedicated,retired_pending,persistent_descriptors,map\n",
                       out);
            s.header_written = true;
        }
        std::fprintf(out, "%lld,%llu", static_cast<long long>(now), static_cast<unsigned long long>(counters.frame));
        for (const D3D12MA::DetailedStatistics& h : total.HeapType)
            std::fprintf(out, ",%u,%llu,%u,%llu", h.Stats.BlockCount,
                         static_cast<unsigned long long>(h.Stats.BlockBytes), h.Stats.AllocationCount,
                         static_cast<unsigned long long>(h.Stats.AllocationBytes));
        const std::uint64_t non_local_usage = non_local.UsageBytes;
        const bool rose = non_local_usage >= s.non_local_at_last_map + kMapRise;
        const bool map = (force_map || rose) && s.maps < kMostMaps;
        std::fprintf(out, ",%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%d\n",
                     static_cast<unsigned long long>(total.MemorySegmentGroup[0].Stats.BlockBytes),
                     static_cast<unsigned long long>(total.MemorySegmentGroup[1].Stats.BlockBytes),
                     static_cast<unsigned long long>(local.UsageBytes),
                     static_cast<unsigned long long>(local.BudgetBytes),
                     static_cast<unsigned long long>(non_local.UsageBytes),
                     static_cast<unsigned long long>(non_local.BudgetBytes), static_cast<unsigned long long>(mapped),
                     static_cast<unsigned long long>(counters.copy_allocators_created),
                     static_cast<unsigned long long>(counters.copy_allocators_spare),
                     static_cast<unsigned long long>(counters.copy_submissions_in_flight),
                     static_cast<unsigned long long>(counters.upload_submissions),
                     static_cast<unsigned long long>(counters.upload_staged_bytes),
                     static_cast<unsigned long long>(counters.upload_ring_waits),
                     static_cast<unsigned long long>(counters.upload_dedicated),
                     static_cast<unsigned long long>(counters.retired_pending),
                     static_cast<unsigned long long>(counters.persistent_descriptors), map ? s.maps : -1);
        std::fclose(out);
        if (map) {
            WCHAR* text = nullptr;
            allocator->BuildStatsString(&text, TRUE);
            // D3D12MA emits a document BOM. This value is nested inside our
            // JSON object, where a BOM is invalid rather than a file marker.
            const std::string json = utf8(text != nullptr && text[0] == 0xFEFF ? text + 1 : text);
            allocator->FreeStatsString(text);
            const std::string map_file = file + "." + std::to_string(s.maps) + ".json";
            if (std::FILE* m = std::fopen(map_file.c_str(), "wb")) {
                std::fprintf(m, "{\"why\":\"%s\",\"ns\":%lld,\"frame\":%llu,\"d3d12ma\":", why,
                             static_cast<long long>(now), static_cast<unsigned long long>(counters.frame));
                std::fwrite(json.data(), 1, json.size(), m);
                std::fputs("}\n", m);
                std::fclose(m);
            }
            ++s.maps;
            s.non_local_at_last_map = non_local_usage;
        } else if (non_local_usage < s.non_local_at_last_map) {
            s.non_local_at_last_map = non_local_usage;  // a later rise is measured from the low point
        }
    }
}

}  // namespace shadow::render
