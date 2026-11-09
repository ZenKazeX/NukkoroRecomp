#pragma once

// Host-side measurement helpers. Nothing here touches guest state; every
// counter is owner-thread written and read after the owning thread has been
// joined, or is explicitly atomic. Added for the 2026-09 resource mission,
// whose first rule is that a cost is not a cost until it has a number.

#include <array>
#include <cstddef>

#include "gekko/types.h"

namespace gekko {

// Fixed-resolution histogram with an overflow bucket. unit_ns is the bucket
// width; percentile() returns the midpoint of the bucket that holds the
// requested rank, so its resolution is one bucket. max is exact.
template <std::size_t N>
struct Histogram {
    u64 unit_ns = 1;
    std::array<u64, N> buckets{};
    u64 count = 0;
    u64 sum = 0;
    u64 max = 0;

    explicit Histogram(u64 unit) : unit_ns(unit == 0u ? 1u : unit) {}

    void add(u64 value) noexcept {
        std::size_t bucket = static_cast<std::size_t>(value / unit_ns);
        if (bucket >= N) bucket = N - 1u;
        ++buckets[bucket];
        ++count;
        sum += value;
        if (value > max) max = value;
    }
    double percentile_ns(double p) const noexcept {
        if (count == 0u) return 0.0;
        if (p < 0.0) p = 0.0;
        if (p > 1.0) p = 1.0;
        const u64 rank = static_cast<u64>(p * static_cast<double>(count - 1u));
        u64 seen = 0;
        for (std::size_t index = 0; index < N; ++index) {
            seen += buckets[index];
            if (seen > rank) {
                if (index == N - 1u) return static_cast<double>(max);
                return (static_cast<double>(index) + 0.5) * static_cast<double>(unit_ns);
            }
        }
        return static_cast<double>(max);
    }
    double percentile_ms(double p) const noexcept { return percentile_ns(p) / 1.0e6; }
    double mean_ns() const noexcept {
        return count != 0u ? static_cast<double>(sum) / static_cast<double>(count) : 0.0;
    }
    // Samples whose bucket starts at or above the threshold.
    u64 count_at_or_above(u64 threshold_ns) const noexcept {
        u64 total = 0;
        for (std::size_t index = 0; index < N; ++index) {
            if (static_cast<u64>(index) * unit_ns >= threshold_ns) total += buckets[index];
        }
        return total;
    }
};

// The CPU's page-table use (Cpu::select_page_table). GuestMemory maps pages
// itself, so nothing is installed by the CPU (installs stays 0, kept for the
// report schema); invalidations counts the re-selections (journal, watch,
// access trace, fault latch). Written only by the guest thread; read at exit.
struct HostPageStats {
    u64 installs = 0;
    u64 invalidations = 0;
    u64 invalidation_ns = 0;
};

HostPageStats& host_page_stats() noexcept;

}  // namespace gekko
