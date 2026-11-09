#include "gekko/heap_census.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace gekko {
namespace {

constexpr std::uint32_t kCellHeader = 0x20u;
constexpr std::uint32_t kMaxCells = 1u << 20;  // a list longer than this is a cycle
constexpr std::int32_t kMaxHeaps = 64;

std::string hex(std::uint32_t v) {
    static const char* digits = "0123456789ABCDEF";
    std::string s = "0x00000000";
    for (int i = 0; i < 8; ++i) s[9 - i] = digits[(v >> (4 * i)) & 0xFu];
    return s;
}

}  // namespace

std::vector<std::pair<std::uint32_t, std::uint32_t>> os_heap_list(const GuestRead32& read32, std::uint32_t head,
                                                                   std::uint32_t arena_start, std::uint32_t arena_end,
                                                                   std::string* error) {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> cells;
    std::uint32_t prev = 0u;
    for (std::uint32_t cell = head; cell != 0u; cell = read32(cell + 4u)) {
        if (cell < arena_start || cell >= arena_end || (cell & 0x1Fu) != 0u) {
            if (error) *error = "cell " + hex(cell) + " outside the arena or misaligned";
            break;
        }
        if (cells.size() >= kMaxCells) {
            if (error) *error = "list longer than " + std::to_string(kMaxCells) + " cells (a cycle?)";
            break;
        }
        const std::uint32_t size = read32(cell + 8u);
        if (size < kCellHeader || size > arena_end - cell) {
            if (error) *error = "cell " + hex(cell) + " has size " + hex(size);
            break;
        }
        if (read32(cell) != prev) {
            if (error) *error = "cell " + hex(cell) + ": prev " + hex(read32(cell)) + ", expected " + hex(prev);
            break;
        }
        cells.emplace_back(cell, size);
        prev = cell;
    }
    return cells;
}

OsHeapsCensus census_os_heaps(const GuestRead32& read32, std::uint32_t sda_base) {
    OsHeapsCensus out;
    out.heap_array = read32(sda_base + 0x5A60u);
    out.heaps = static_cast<std::int32_t>(read32(sda_base + 0x5A64u));
    out.arena_start = read32(sda_base + 0x5A68u);
    out.arena_end = read32(sda_base + 0x5A6Cu);
    out.current = static_cast<std::int32_t>(read32(sda_base + 0x26E0u));
    if (out.heap_array == 0u || out.heaps <= 0) {
        out.error = "no heaps (OSInitAlloc has not run)";
        return out;
    }
    if (out.heaps > kMaxHeaps || out.arena_end <= out.arena_start) {
        out.error = "implausible heap globals";
        return out;
    }
    for (std::int32_t h = 0; h < out.heaps; ++h) {
        OsHeapCensus c;
        c.index = h;
        c.desc = out.heap_array + static_cast<std::uint32_t>(h) * 12u;
        c.size = static_cast<std::int32_t>(read32(c.desc));
        if (c.size < 0) {
            out.census.push_back(c);
            continue;
        }
        std::string error;
        const auto free_cells = os_heap_list(read32, read32(c.desc + 4u), out.arena_start, out.arena_end, &error);
        const auto used_cells =
            error.empty() ? os_heap_list(read32, read32(c.desc + 8u), out.arena_start, out.arena_end, &error)
                          : decltype(free_cells){};
        c.error = error;
        for (std::size_t i = 0; i < free_cells.size(); ++i) {
            const auto [cell, size] = free_cells[i];
            c.free_bytes += size;
            c.largest_free = std::max(c.largest_free, size);
            if (i > 0u) {
                const auto [before, before_size] = free_cells[i - 1u];
                if (cell <= before) c.free_sorted = false;
                if (before + before_size == cell) c.free_coalesced = false;
            }
        }
        c.free_cells = static_cast<std::uint32_t>(free_cells.size());
        for (const auto& [cell, size] : used_cells) c.allocated_bytes += size;
        c.allocated_cells = static_cast<std::uint32_t>(used_cells.size());
        out.census.push_back(c);
    }
    return out;
}

void HeapTrace::tick(std::uint64_t game_ticks, const GuestRead32& read32) {
    constexpr std::uint64_t kTicksPerSecond = 40'500'000u;
    static std::FILE* file = [] {
        const char* path = std::getenv("GEKKO_HEAP_TRACE");
        std::FILE* f = (path != nullptr && path[0] != '\0') ? std::fopen(path, "w") : nullptr;
        if (f != nullptr)
            std::fputs("game_s,heap,size,allocated_bytes,allocated_cells,free_bytes,free_cells,largest_free,sorted,"
                       "coalesced,error\n",
                       f);
        return f;
    }();
    static std::uint64_t next_second = 0u;
    if (file == nullptr || game_ticks < next_second * kTicksPerSecond) return;
    next_second = game_ticks / kTicksPerSecond + 1u;
    const OsHeapsCensus census = census_os_heaps(read32);
    const double seconds = static_cast<double>(game_ticks) / static_cast<double>(kTicksPerSecond);
    if (!census.error.empty()) {
        std::fprintf(file, "%.3f,-1,0,0,0,0,0,0,0,0,%s\n", seconds, census.error.c_str());
    }
    for (const OsHeapCensus& h : census.census) {
        std::fprintf(file, "%.3f,%d,%d,%u,%u,%u,%u,%u,%d,%d,%s\n", seconds, h.index, h.size, h.allocated_bytes,
                     h.allocated_cells, h.free_bytes, h.free_cells, h.largest_free, h.free_sorted ? 1 : 0,
                     h.free_coalesced ? 1 : 0, h.error.c_str());
    }
    std::fflush(file);
}

}  // namespace gekko
