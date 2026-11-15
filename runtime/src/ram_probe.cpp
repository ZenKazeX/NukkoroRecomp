// RAM probe (cs2/ram, 2026-11-13). MEASUREMENT ONLY: compiled only with -DSHADOW_RAM_PROBE=ON (off by default; no shipped
// binary contains it). It replaces the global operator new / delete of the executable with versions that keep, per
// allocation call stack, the live and total bytes, so a run can say which code owns the host heap at the end of the
// run and at its peak. Every allocation carries a 32-byte header {magic, site, size, raw pointer}.
//
// GEKKO_RAM_PROBE=<path> : report() (called once from shadow_main before the process-memory line) writes a TSV there:
//   line 1  "# image_base 0x..."
//   then one row per call stack: live_bytes live_count peak_bytes total_bytes total_count frame RVAs (hex, ';' separated)
// where peak_bytes is the site's live bytes at the moment the process's live total was highest (snapshotted whenever
// the live total passed the previous snapshot by 2 MiB). The RVAs resolve against the exe's linker map.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ram_probe {
namespace {

constexpr std::uint64_t kMagic = 0x52414D50524F4245ull;  // "RAMPROBE"
constexpr std::size_t kHeader = 32;
constexpr std::size_t kSites = 1u << 16;
constexpr int kFrames = 12;

struct Site {
    std::atomic<std::uint64_t> hash{0};
    void* frames[kFrames];
    std::uint32_t frame_count = 0;
    std::atomic<std::int64_t> live_bytes{0};
    std::atomic<std::int64_t> live_count{0};
    std::atomic<std::int64_t> total_bytes{0};
    std::atomic<std::int64_t> total_count{0};
    std::int64_t peak_bytes = 0;  // written under g_snapshot_lock
};

Site g_sites[kSites];
std::atomic<std::int64_t> g_live_total{0};
std::int64_t g_snapshot_total = 0;
std::atomic_flag g_snapshot_lock = ATOMIC_FLAG_INIT;
thread_local bool t_inside = false;

std::uint32_t site_of(void** frames, int count) {
    std::uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < count; ++i) {
        h ^= reinterpret_cast<std::uint64_t>(frames[i]);
        h *= 1099511628211ull;
    }
    if (h == 0) h = 1;
    std::uint32_t index = static_cast<std::uint32_t>(h) & (kSites - 1);
    for (std::uint32_t probe = 0; probe < kSites; ++probe) {
        Site& s = g_sites[index];
        const std::uint64_t seen = s.hash.load(std::memory_order_acquire);
        if (seen == h) return index;
        if (seen == 0) {
            std::uint64_t expected = 0;
            if (s.hash.compare_exchange_strong(expected, h, std::memory_order_acq_rel)) {
                s.frame_count = static_cast<std::uint32_t>(count);
                std::memcpy(s.frames, frames, sizeof(void*) * static_cast<std::size_t>(count));
                return index;
            }
            if (expected == h) return index;
        }
        index = (index + 1) & (kSites - 1);
    }
    return 0;
}

void take_peak_snapshot() {
    if (g_snapshot_lock.test_and_set(std::memory_order_acquire)) return;
    const std::int64_t now = g_live_total.load(std::memory_order_relaxed);
    if (now > g_snapshot_total + (2 << 20)) {
        g_snapshot_total = now;
        for (std::size_t i = 0; i < kSites; ++i)
            if (g_sites[i].hash.load(std::memory_order_relaxed) != 0)
                g_sites[i].peak_bytes = g_sites[i].live_bytes.load(std::memory_order_relaxed);
    }
    g_snapshot_lock.clear(std::memory_order_release);
}

void* allocate(std::size_t size, std::size_t align) {
    const std::size_t extra = align > 16 ? align : 16;
    unsigned char* raw = static_cast<unsigned char*>(std::malloc(size + kHeader + extra));
    if (raw == nullptr) return nullptr;
    std::uintptr_t user =
        (reinterpret_cast<std::uintptr_t>(raw) + kHeader + extra - 1) & ~(static_cast<std::uintptr_t>(extra) - 1);
    if (user < reinterpret_cast<std::uintptr_t>(raw) + kHeader) user += extra;
    std::uint64_t* header = reinterpret_cast<std::uint64_t*>(user - kHeader);
    header[0] = kMagic;
    header[1] = ~0ull;
    header[2] = size;
    header[3] = reinterpret_cast<std::uint64_t>(raw);
    if (!t_inside) {
        t_inside = true;
        void* frames[kFrames];
        const USHORT count = CaptureStackBackTrace(2, kFrames, frames, nullptr);
        const std::uint32_t site = site_of(frames, count);
        t_inside = false;
        header[1] = site;
        Site& s = g_sites[site];
        s.live_bytes.fetch_add(static_cast<std::int64_t>(size), std::memory_order_relaxed);
        s.live_count.fetch_add(1, std::memory_order_relaxed);
        s.total_bytes.fetch_add(static_cast<std::int64_t>(size), std::memory_order_relaxed);
        s.total_count.fetch_add(1, std::memory_order_relaxed);
        const std::int64_t total =
            g_live_total.fetch_add(static_cast<std::int64_t>(size), std::memory_order_relaxed) + static_cast<std::int64_t>(size);
        if (total > g_snapshot_total + (2 << 20)) take_peak_snapshot();
    }
    return reinterpret_cast<void*>(user);
}

void release(void* p) {
    if (p == nullptr) return;
    std::uint64_t* header = reinterpret_cast<std::uint64_t*>(static_cast<unsigned char*>(p) - kHeader);
    if (header[0] != kMagic) {  // not ours (should not happen): hand it back untouched
        std::free(p);
        return;
    }
    if (header[1] != ~0ull) {
        Site& s = g_sites[static_cast<std::uint32_t>(header[1])];
        s.live_bytes.fetch_sub(static_cast<std::int64_t>(header[2]), std::memory_order_relaxed);
        s.live_count.fetch_sub(1, std::memory_order_relaxed);
        g_live_total.fetch_sub(static_cast<std::int64_t>(header[2]), std::memory_order_relaxed);
    }
    void* raw = reinterpret_cast<void*>(header[3]);
    header[0] = 0;
    std::free(raw);
}

}  // namespace

void report() {
    const char* path = std::getenv("GEKKO_RAM_PROBE");
    if (path == nullptr || path[0] == '\0') return;
    std::FILE* out = std::fopen(path, "w");
    if (out == nullptr) return;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    std::fprintf(out, "# image_base 0x%llx live_total %lld peak_snapshot_total %lld\n",
                 static_cast<unsigned long long>(base), static_cast<long long>(g_live_total.load()),
                 static_cast<long long>(g_snapshot_total));
    for (std::size_t i = 0; i < kSites; ++i) {
        Site& s = g_sites[i];
        if (s.hash.load() == 0) continue;
        std::fprintf(out, "%lld\t%lld\t%lld\t%lld\t%lld\t", static_cast<long long>(s.live_bytes.load()),
                     static_cast<long long>(s.live_count.load()), static_cast<long long>(s.peak_bytes),
                     static_cast<long long>(s.total_bytes.load()), static_cast<long long>(s.total_count.load()));
        for (std::uint32_t f = 0; f < s.frame_count; ++f)
            std::fprintf(out, "%s%llx", f == 0 ? "" : ";",
                         static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(s.frames[f]) - base));
        std::fprintf(out, "\n");
    }
    std::fclose(out);
}

}  // namespace ram_probe

void* operator new(std::size_t n) {
    void* p = ram_probe::allocate(n, 16);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return ram_probe::allocate(n, 16); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return ram_probe::allocate(n, 16); }
void* operator new(std::size_t n, std::align_val_t a) {
    void* p = ram_probe::allocate(n, static_cast<std::size_t>(a));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return ram_probe::allocate(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return ram_probe::allocate(n, static_cast<std::size_t>(a));
}
void operator delete(void* p) noexcept { ram_probe::release(p); }
void operator delete[](void* p) noexcept { ram_probe::release(p); }
void operator delete(void* p, std::size_t) noexcept { ram_probe::release(p); }
void operator delete[](void* p, std::size_t) noexcept { ram_probe::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { ram_probe::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ram_probe::release(p); }
void operator delete(void* p, std::align_val_t) noexcept { ram_probe::release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { ram_probe::release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { ram_probe::release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { ram_probe::release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { ram_probe::release(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { ram_probe::release(p); }
