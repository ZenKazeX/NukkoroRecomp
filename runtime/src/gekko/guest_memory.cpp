#include "gekko/guest_memory.h"
#include "gekko/profiling.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <cstdint>

namespace gekko {
namespace {

static_assert(GuestMemory::kGranule % GuestMemory::kPageSize == 0u, "a granule is whole pages");
static_assert(GuestMemory::kPageCount <= 0x40000u, "the page table covers exactly the 32-bit window");

u8* reserve_host(std::size_t bytes) {
#if defined(_WIN32)
    return static_cast<u8*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS));
#else
    void* p = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<u8*>(p);
#endif
}

bool commit_host(u8* begin, std::size_t bytes) {
#if defined(_WIN32)
    return VirtualAlloc(begin, bytes, MEM_COMMIT, PAGE_READWRITE) == begin;
#else
    return mprotect(begin, bytes, PROT_READ | PROT_WRITE) == 0;
#endif
}

[[maybe_unused]] void release_host(u8* base, std::size_t bytes) {
#if defined(_WIN32)
    (void)bytes;
    VirtualFree(base, 0, MEM_RELEASE);
#else
    munmap(base, bytes);
#endif
}

// ---- FM: the flat native memory map ------------------------------------------
// Every region is a section mapped twice (GuestMemory::Region::section). The
// placeholder API (Windows 10 1803+) is resolved at run time from kernelbase so
// the link needs no newer import library.
#if defined(_WIN32)
using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG,
                                        MEM_EXTENDED_PARAMETER*, ULONG);
using UnmapViewOfFile2Fn = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);
struct FmApi {
    VirtualAlloc2Fn virtual_alloc2 = nullptr;
    MapViewOfFile3Fn map_view3 = nullptr;
    UnmapViewOfFile2Fn unmap_view2 = nullptr;
};
const FmApi& fm_api() {
    static const FmApi api = [] {
        FmApi a;
        if (HMODULE k = GetModuleHandleW(L"kernelbase.dll")) {
            a.virtual_alloc2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(k, "VirtualAlloc2"));
            a.map_view3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(k, "MapViewOfFile3"));
            a.unmap_view2 = reinterpret_cast<UnmapViewOfFile2Fn>(GetProcAddress(k, "UnmapViewOfFile2"));
        }
        return a;
    }();
    return api;
}
#endif

[[noreturn]] void fm_fail(const char* what) {
    std::fprintf(stderr, "guest memory (FM): %s\n", what);
    std::abort();
}

std::string format_address(const char* what, u32 address, u32 size) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%s %u-bit at 0x%08X", what, size * 8u, address);
    return std::string(buffer);
}

}  // namespace

u8 MmioDevice::read8(u32 address) {
    const u32 word = read32(address & ~3u);
    return static_cast<u8>(word >> (8u * (3u - (address & 3u))));
}

u16 MmioDevice::read16(u32 address) {
    const u32 word = read32(address & ~3u);
    return static_cast<u16>(word >> (16u * (1u - ((address >> 1) & 1u))));
}

u32 MmioDevice::read32(u32 address) {
    return (static_cast<u32>(read16(address)) << 16) | read16(address + 2u);
}

void MmioDevice::write8(u32 address, u8 value) {
    const u32 aligned = address & ~1u;
    u16 half = read16(aligned);
    if ((address & 1u) != 0u) {
        half = static_cast<u16>((half & 0xff00u) | value);
    } else {
        half = static_cast<u16>((half & 0x00ffu) | (static_cast<u16>(value) << 8));
    }
    write16(aligned, half);
}

void MmioDevice::write16(u32 address, u16 value) {
    u32 word = read32(address & ~3u);
    if ((address & 2u) != 0u) {
        word = (word & 0xffff0000u) | value;
    } else {
        word = (word & 0x0000ffffu) | (static_cast<u32>(value) << 16);
    }
    write32(address & ~3u, word);
}

void MmioDevice::write32(u32 address, u32 value) {
    write16(address, static_cast<u16>(value >> 16));
    write16(address + 2u, static_cast<u16>(value));
}

const char* GuestMemory::kind_name(RegionKind kind) noexcept {
    switch (kind) {
        case RegionKind::Image: return "program image";
        case RegionKind::LockedCache: return "locked-cache scratch";
        case RegionKind::LegacyHeap: return "legacy OS heap (M1 only)";
        case RegionKind::Arena: return "native arena";
    }
    return "?";
}

GuestMemory::GuestMemory()
    : fixture_pages_(kFixtureSize / 256u, nullptr) {
    // FM: the two 4 GiB windows, before any region exists.
#if defined(_WIN32)
    const FmApi& api = fm_api();
    if (api.virtual_alloc2 == nullptr || api.map_view3 == nullptr || api.unmap_view2 == nullptr) {
        fm_fail("the placeholder memory API (VirtualAlloc2 / MapViewOfFile3) is not available");
    }
    fm_window_ = static_cast<u8*>(api.virtual_alloc2(GetCurrentProcess(), nullptr, kFmWindowBytes,
                                                     MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
                                                     nullptr, 0));
    fm_null_window_ = static_cast<u8*>(VirtualAlloc(nullptr, kFmWindowBytes, MEM_RESERVE, PAGE_NOACCESS));
#else
    fm_window_ = reserve_host(kFmWindowBytes);
    fm_null_window_ = reserve_host(kFmWindowBytes);
#endif
    if (fm_window_ == nullptr || fm_null_window_ == nullptr) fm_fail("cannot reserve the 4 GiB windows");
    // The fixed regions every machine has: the program image at its link
    // addresses and the locked-cache scratch. M1 also maps the range the
    // game's own OS heap still runs over (README section 4).
    const bool ok = map(kImageBase, kImageEnd - kImageBase, kImageEnd - kImageBase, RegionKind::Image, "image") &&
                    map(kLegacyHeapBase, kLegacyHeapEnd - kLegacyHeapBase, kLegacyHeapEnd - kLegacyHeapBase,
                        RegionKind::LegacyHeap, "legacy-os-heap") &&
                    map(kLockedCacheBase, kLockedCacheSize, kLockedCacheSize, RegionKind::LockedCache,
                        "locked-cache");
    if (!ok) {
        std::fprintf(stderr, "guest memory: cannot map the fixed regions\n");
        std::abort();
    }
}

GuestMemory::~GuestMemory() {
    for (Region& region : regions_) {
        GEKKO_PROFILE_FREE(region.host, "Guest memory region");
#if defined(_WIN32)
        fm_api().unmap_view2(GetCurrentProcess(), fm_window_ + region.base, MEM_PRESERVE_PLACEHOLDER);
        UnmapViewOfFile(region.host);
        CloseHandle(static_cast<HANDLE>(region.section));
#else
        munmap(fm_window_ + region.base, region.size);
        munmap(region.host, region.size);
        close(static_cast<int>(reinterpret_cast<std::intptr_t>(region.section)));
#endif
    }
#if defined(_WIN32)
    VirtualFree(fm_window_, kFmWindowBytes, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
    VirtualFree(fm_window_, 0, MEM_RELEASE);
    VirtualFree(fm_null_window_, 0, MEM_RELEASE);
#else
    munmap(fm_window_, kFmWindowBytes);
    munmap(fm_null_window_, kFmWindowBytes);
#endif
}

// ---- regions --------------------------------------------------------------------

bool GuestMemory::map(u32 base, u32 size, u32 commit_bytes, RegionKind kind, std::string name) {
    if (size == 0u || (base % kGranule) != 0u || (size % kGranule) != 0u || (commit_bytes % kGranule) != 0u ||
        commit_bytes > size || static_cast<u64>(base) + size > 0x100000000ull) {
        return false;
    }
    // FM step 2: no page table; overlap is checked against the few regions.
    for (const Region& other : regions_) {
        if (static_cast<u64>(base) < static_cast<u64>(other.base) + other.size &&
            static_cast<u64>(other.base) < static_cast<u64>(base) + size) {
            return false;
        }
    }
    if (regions_.size() >= 0xFFFEu) return false;
    // FM: one section, two views - the native view (`host`) and the window
    // view at fm_window_ + base. Uncommitted pages fault in both.
    void* section = nullptr;
    u8* host = nullptr;
#if defined(_WIN32)
    {
        const HANDLE mapping =
            CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE, 0, size, nullptr);
        if (mapping == nullptr) return false;
        host = static_cast<u8*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size));
        // Split the window's placeholder at exactly [base, base + size); a
        // range that already is one whole placeholder (a region re-mapped at
        // its old place) needs no split, so a failed split is only an error
        // if the view then cannot replace it.
        VirtualFree(fm_window_ + base, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        u8* const view = host == nullptr ? nullptr
                                         : static_cast<u8*>(fm_api().map_view3(
                                               mapping, GetCurrentProcess(), fm_window_ + base, 0, size,
                                               MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0));
        if (view != fm_window_ + base) {
            if (view != nullptr) fm_api().unmap_view2(GetCurrentProcess(), view, MEM_PRESERVE_PLACEHOLDER);
            if (host != nullptr) UnmapViewOfFile(host);
            CloseHandle(mapping);
            return false;
        }
        section = mapping;
    }
#else
    {
        const int fd = memfd_create("gekko-guest-region", 0);
        if (fd < 0 || ftruncate(fd, size) != 0) return false;
        host = static_cast<u8*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        if (host == MAP_FAILED ||
            mmap(fm_window_ + base, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
            close(fd);
            return false;
        }
        section = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
    }
#endif
    if (commit_bytes != 0u && !commit_host(host, commit_bytes)) {
#if defined(_WIN32)
        fm_api().unmap_view2(GetCurrentProcess(), fm_window_ + base, MEM_PRESERVE_PLACEHOLDER);
        UnmapViewOfFile(host);
        CloseHandle(static_cast<HANDLE>(section));
#endif
        return false;
    }
    regions_.push_back(Region{base, size, commit_bytes, kind, std::move(name), host, section});
    GEKKO_PROFILE_ALLOC(host, size, "Guest memory region");
    return true;
}

bool GuestMemory::commit(u32 base, u32 committed) {
    for (Region& region : regions_) {
        if (region.base != base) continue;
        if ((committed % kGranule) != 0u || committed > region.size) return false;
        if (committed <= region.committed) return true;
        if (!commit_host(region.host + region.committed, committed - region.committed)) return false;
        region.committed = committed;
        return true;
    }
    return false;
}

bool GuestMemory::unmap(u32 base) {
    for (std::size_t index = 0; index < regions_.size(); ++index) {
        if (regions_[index].base != base) continue;
        const Region region = regions_[index];
        GEKKO_PROFILE_FREE(region.host, "Guest memory region");
#if defined(_WIN32)
        fm_api().unmap_view2(GetCurrentProcess(), fm_window_ + region.base, MEM_PRESERVE_PLACEHOLDER);
        UnmapViewOfFile(region.host);
        CloseHandle(static_cast<HANDLE>(region.section));
#else
        mmap(fm_window_ + region.base, region.size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
             -1, 0);
        munmap(region.host, region.size);
        close(static_cast<int>(reinterpret_cast<std::intptr_t>(region.section)));
#endif
        regions_.erase(regions_.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }
    return false;
}

const GuestMemory::Region* GuestMemory::region_at(u32 address) const noexcept {
    // FM step 2: a scan of the (three or four) regions replaces the 2^18-entry
    // per-page index.
    for (const Region& region : regions_) {
        if (address - region.base < region.size) return &region;
    }
    return nullptr;
}

u64 GuestMemory::reserved_bytes() const noexcept {
    u64 total = 0;
    for (const Region& region : regions_) total += region.size;
    return total;
}

u64 GuestMemory::committed_bytes() const noexcept {
    u64 total = 0;
    for (const Region& region : regions_) total += region.committed;
    return total;
}

// One region, committed, the whole range: a contiguous host span.
u8* GuestMemory::resolve(u32 address, std::size_t size) const noexcept {
    if (size == 0u || size > 0x100000000ull - address) return nullptr;
    const Region* const found = region_at(address);
    if (found == nullptr) return nullptr;
    const Region& region = *found;
    const u32 offset = address - region.base;
    if (offset > region.committed || size > region.committed - offset) return nullptr;
    return region.host + offset;
}

// ---- savestate ----------------------------------------------------------------------

namespace {

template <typename T>
void put_pod(std::vector<u8>& out, const T& value) {
    const auto* bytes = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T>
bool get_pod(const u8*& cursor, const u8* end, T& value) {
    if (static_cast<std::size_t>(end - cursor) < sizeof(T)) return false;
    std::memcpy(&value, cursor, sizeof(T));
    cursor += sizeof(T);
    return true;
}

}  // namespace

void GuestMemory::save_state(std::vector<u8>& out) const {
    put_pod<u32>(out, static_cast<u32>(regions_.size()));
    for (const Region& region : regions_) {
        put_pod<u32>(out, region.base);
        put_pod<u32>(out, region.size);
        put_pod<u32>(out, region.committed);
        put_pod<u8>(out, static_cast<u8>(region.kind));
        put_pod<u32>(out, static_cast<u32>(region.name.size()));
        out.insert(out.end(), region.name.begin(), region.name.end());
        out.insert(out.end(), region.host, region.host + region.committed);
    }
}

bool GuestMemory::load_state(const u8*& cursor, const u8* end, std::string& why) {
    u32 count = 0;
    if (!get_pod(cursor, end, count) || count > 0xFFFEu) {
        why = "region count";
        return false;
    }
    // Regions the file does not name are not part of the restored machine: they go first, so a saved region at
    // another base can never collide with one of them half-way through the load (earlier regions already written).
    {
        std::vector<u32> bases;
        const u8* scan = cursor;
        for (u32 index = 0; index < count; ++index) {
            u32 base = 0, size = 0, committed = 0, name_length = 0;
            u8 kind = 0;
            if (!get_pod(scan, end, base) || !get_pod(scan, end, size) || !get_pod(scan, end, committed) ||
                !get_pod(scan, end, kind) || !get_pod(scan, end, name_length) ||
                static_cast<std::size_t>(end - scan) < u64{name_length} + committed) {
                why = "region header";
                return false;
            }
            scan += u64{name_length} + committed;
            bases.push_back(base);
        }
        for (std::size_t index = regions_.size(); index-- > 0;) {
            const u32 base = regions_[index].base;
            if (std::find(bases.begin(), bases.end(), base) == bases.end()) unmap(base);
        }
    }
    std::vector<u32> named;
    for (u32 index = 0; index < count; ++index) {
        u32 base = 0, size = 0, committed = 0, name_length = 0;
        u8 kind = 0;
        if (!get_pod(cursor, end, base) || !get_pod(cursor, end, size) || !get_pod(cursor, end, committed) ||
            !get_pod(cursor, end, kind) || !get_pod(cursor, end, name_length) || name_length > 256u ||
            static_cast<std::size_t>(end - cursor) < name_length) {
            why = "region header";
            return false;
        }
        std::string name(reinterpret_cast<const char*>(cursor), name_length);
        cursor += name_length;
        if (committed > size || static_cast<std::size_t>(end - cursor) < committed) {
            why = "region " + name + " bytes";
            return false;
        }
        const Region* existing = nullptr;
        for (const Region& region : regions_) {
            if (region.base == base) existing = &region;
        }
        // A region committed past the saved size is mapped again at the saved size: zeroing the tail alone would
        // leave addresses resolving that faulted in the saved machine (and a save after the load would differ).
        if (existing != nullptr && (existing->size != size || existing->kind != static_cast<RegionKind>(kind) ||
                                    existing->committed > committed)) {
            unmap(base);
            existing = nullptr;
        }
        if (existing == nullptr) {
            if (!map(base, size, committed, static_cast<RegionKind>(kind), name)) {
                why = "region " + name + " cannot be mapped";
                return false;
            }
        } else if (!commit(base, committed)) {
            why = "region " + name + " cannot be committed";
            return false;
        }
        for (Region& region : regions_) {
            if (region.base != base) continue;
            std::memcpy(region.host, cursor, committed);
            if (region.committed > committed) std::memset(region.host + committed, 0, region.committed - committed);
        }
        cursor += committed;
        named.push_back(base);
    }
    // Regions the file does not name are not part of the restored machine.
    for (std::size_t index = regions_.size(); index-- > 0;) {
        const u32 base = regions_[index].base;
        if (std::find(named.begin(), named.end(), base) == named.end()) unmap(base);
    }
    return true;
}

// ---- guest addresses ----------------------------------------------------------------

u8* GuestMemory::span(GcAddr address, std::size_t size) { return host_pointer(address.value(), size); }

const u8* GuestMemory::peek(GcAddr address, std::size_t size) const { return resolve(address.value(), size); }

bool GuestMemory::write(GcAddr address, const void* source, std::size_t size) {
    if (resolve(address.value(), size) == nullptr) return false;
    write_block(address.value(), source, size);
    return true;
}

bool GuestMemory::read(GcAddr address, void* destination, std::size_t size) {
    if (resolve(address.value(), size) == nullptr) return false;
    read_block(address.value(), destination, size);
    return true;
}

// ---- fixtures -------------------------------------------------------------------------

void GuestMemory::register_mmio(u32 address, u32 size, MmioDevice* device) {
    if (address < kFixtureBase) return;
    const u32 first = (address - kFixtureBase) / 256u;
    const u32 count = (size + 255u) / 256u;
    for (u32 index = 0; index < count; ++index) {
        if (first + index < fixture_pages_.size()) fixture_pages_[first + index] = device;
    }
}

MmioDevice* GuestMemory::mmio_for(u32 address) const {
    if (address < kFixtureBase || address - kFixtureBase >= kFixtureSize) return nullptr;
    return fixture_pages_[(address - kFixtureBase) / 256u];
}

// ---- access trace and listeners ---------------------------------------------------------

u8* GuestMemory::host_pointer(u32 address, std::size_t size) {
    if (access_trace_) trace_access(address, size, false);
    return resolve(address, size);
}

const u8* GuestMemory::host_pointer(u32 address, std::size_t size) const {
    return const_cast<GuestMemory*>(this)->host_pointer(address, size);
}

void GuestMemory::set_access_trace(AccessTrace trace) {
    access_trace_ = std::move(trace);
    // Accesses served from the page table would not reach the trace.
    notify_translation_change();
}

void GuestMemory::trace_access(u32 address, std::size_t size, bool writing) {
    // Per-thread recursion latch: the callback's own accesses are not guest
    // traffic. Only committed extents are reported.
    static thread_local bool tracing = false;
    if (size == 0u || tracing || !access_trace_ || resolve(address, size) == nullptr) return;
    tracing = true;
    try {
        access_trace_(address, size, writing);
        tracing = false;
    } catch (...) {
        tracing = false;
        throw;
    }
}

void GuestMemory::add_translation_listener(const void* owner, TranslationChange change) {
    for (auto& entry : translation_listeners_) {
        if (entry.first == owner) {
            entry.second = std::move(change);
            ++translation_invalidations_;
            if (entry.second) entry.second();
            return;
        }
    }
    translation_listeners_.emplace_back(owner, std::move(change));
    ++translation_invalidations_;
    const TranslationChange& added = translation_listeners_.back().second;
    if (added) added();
}

void GuestMemory::remove_translation_listener(const void* owner) noexcept {
    for (auto it = translation_listeners_.begin(); it != translation_listeners_.end(); ++it) {
        if (it->first == owner) {
            translation_listeners_.erase(it);
            return;
        }
    }
}

void GuestMemory::notify_translation_change() const {
    ++translation_invalidations_;
    // Index loop: a listener only re-enters its own Cpu, never this vector.
    for (std::size_t index = 0; index < translation_listeners_.size(); ++index) {
        const TranslationChange& change = translation_listeners_[index].second;
        if (change) change();
    }
}

// ---- unmapped-access diagnostics (fixtures and unit tests) ------------------------------

void GuestMemory::note_unmapped_read(u32 address, u32 size) {
    ++diagnostics_.unmapped_reads;
    if (diagnostics_.unmapped_reads == 1) diagnostics_.first_unmapped_read_address = address;
    if (trace_unmapped_ && diagnostics_.messages.size() < 64) {
        diagnostics_.messages.push_back(format_address("unmapped read", address, size));
    }
}

void GuestMemory::note_unmapped_write(u32 address, u32 size, u64 value) {
    ++diagnostics_.unmapped_writes;
    if (diagnostics_.unmapped_writes == 1) diagnostics_.first_unmapped_write_address = address;
    if (trace_unmapped_ && diagnostics_.messages.size() < 64) {
        char buffer[128];
        std::snprintf(buffer, sizeof(buffer), "unmapped write %u-bit at 0x%08X value 0x%016llX", size * 8u, address,
                      static_cast<unsigned long long>(value));
        diagnostics_.messages.push_back(buffer);
    }
}

// ---- journal and watch ------------------------------------------------------------------

void GuestMemory::journal_begin() {
    journal_.clear();
    journal_device_ = false;
    journal_overflow_ = false;
    journaling_ = true;
    notify_translation_change();
}

void GuestMemory::watch_begin(std::size_t limit) {
    watch_limit_ = limit == 0u ? 1u : limit;
    journal_begin();
}

bool GuestMemory::watch_unchanged() const {
    if (!watch_active() || journal_device_ || journal_overflow_) return false;
    for (const JournalEntry& entry : journal_) {
        const u8* p = resolve(entry.address, entry.size);
        if (p == nullptr) return false;
        u64 current = 0;
        for (u32 index = 0; index < entry.size; ++index) current = (current << 8) | p[index];
        if (current != entry.previous) return false;
    }
    return true;
}

void GuestMemory::journal_note(u32 address, u32 size) {
    if (watch_limit_ != 0u && (journal_.size() >= watch_limit_ || resolve(address, size) == nullptr)) {
        // A watched iteration that writes this much, or reaches a fixture, is
        // doing real work: the watch ends itself and this write takes the
        // ordinary path.
        ++watch_aborts_;
        watch_end();
        return;
    }
    if (journal_.size() >= kJournalLimit) {
        if (!journal_overflow_) GEKKO_PROFILE_MESSAGE_LITERAL("mem/journal overflow");
        journal_overflow_ = true;
        return;
    }
    const u8* p = resolve(address, size);
    if (p == nullptr) {
        // A fixture or an unmapped address: neither can be put back.
        journal_device_ = true;
        return;
    }
    u64 previous = 0;
    for (u32 index = 0; index < size; ++index) previous = (previous << 8) | p[index];
    journal_.push_back(JournalEntry{address, size, previous});
}

void GuestMemory::journal_rollback() {
    GEKKO_PROFILE_ZONE("mem/journal rollback");
    GEKKO_PROFILE_VALUE(journal_.size());
    if (GEKKO_PROFILE_CONNECTED()) {
        static bool configured = false;
        if (!configured) {
            configured = true;
            GEKKO_PROFILE_PLOT_CONFIG("mem/journal entries", GEKKO_PLOT_NUMBER, true, true, 0xa88cff);
        }
        GEKKO_PROFILE_PLOT("mem/journal entries", static_cast<double>(journal_.size()));
    }
    for (std::size_t index = journal_.size(); index-- > 0;) {
        const JournalEntry& entry = journal_[index];
        u8* p = resolve(entry.address, entry.size);
        if (p == nullptr) continue;
        for (u32 byte = 0; byte < entry.size; ++byte) {
            p[byte] = static_cast<u8>(entry.previous >> (8u * (entry.size - 1u - byte)));
        }
    }
    journal_.clear();
}

// ---- guest-address accesses -------------------------------------------------------------

u8 GuestMemory::read8(u32 address) {
    const u8* p = host_pointer(address, 1u);
    if (journaling_ && p == nullptr) journal_device_ = true;
    if (p != nullptr) return *p;
    if (MmioDevice* device = mmio_for(address)) return device->read8(address);
    note_unmapped_read(address, 1);
    return 0;
}

u16 GuestMemory::read16(u32 address) {
    const u8* p = host_pointer(address, 2u);
    if (journaling_ && p == nullptr) journal_device_ = true;
    if (p != nullptr) return load_be16(p);
    if (MmioDevice* device = mmio_for(address)) return device->read16(address);
    note_unmapped_read(address, 2);
    return 0;
}

u32 GuestMemory::read32(u32 address) {
    const u8* p = host_pointer(address, 4u);
    if (journaling_ && p == nullptr) journal_device_ = true;
    if (p != nullptr) return load_be32(p);
    if (MmioDevice* device = mmio_for(address)) return device->read32(address);
    note_unmapped_read(address, 4);
    return 0;
}

u64 GuestMemory::read64(u32 address) {
    const u8* p = host_pointer(address, 8u);
    if (journaling_ && p == nullptr) journal_device_ = true;
    if (p != nullptr) return load_be64(p);
    return (static_cast<u64>(read32(address)) << 32) | read32(address + 4u);
}

void GuestMemory::write8(u32 address, u8 value) {
    if (access_trace_) trace_access(address, 1u, true);
    if (journaling_) journal_note(address, 1u);
    if (u8* p = resolve(address, 1)) {
        *p = value;
        return;
    }
    if (MmioDevice* device = mmio_for(address)) {
        device->write8(address, value);
        return;
    }
    note_unmapped_write(address, 1, value);
}

void GuestMemory::write16(u32 address, u16 value) {
    if (access_trace_) trace_access(address, 2u, true);
    if (journaling_) journal_note(address, 2u);
    if (u8* p = resolve(address, 2)) {
        store_be16(p, value);
        return;
    }
    if (MmioDevice* device = mmio_for(address)) {
        device->write16(address, value);
        return;
    }
    note_unmapped_write(address, 2, value);
}

void GuestMemory::write32(u32 address, u32 value) {
    if (access_trace_) trace_access(address, 4u, true);
    if (journaling_) journal_note(address, 4u);
    if (u8* p = resolve(address, 4)) {
        store_be32(p, value);
        return;
    }
    if (MmioDevice* device = mmio_for(address)) {
        device->write32(address, value);
        return;
    }
    note_unmapped_write(address, 4, value);
}

void GuestMemory::write64(u32 address, u64 value) {
    if (access_trace_) trace_access(address, 8u, true);
    if (journaling_) journal_note(address, 8u);
    if (u8* p = resolve(address, 8)) {
        store_be64(p, value);
        return;
    }
    write32(address, static_cast<u32>(value >> 32));
    write32(address + 4u, static_cast<u32>(value));
}

u8 GuestMemory::peek8(u32 address) const {
    const u8* p = resolve(address, 1u);
    return p != nullptr ? *p : 0u;
}

u16 GuestMemory::peek16(u32 address) const {
    const u8* p = resolve(address, 2u);
    return p != nullptr ? load_be16(p) : 0u;
}

u32 GuestMemory::peek32(u32 address) const {
    const u8* p = resolve(address, 4u);
    return p != nullptr ? load_be32(p) : 0u;
}

void GuestMemory::peek_block(u32 address, void* destination, std::size_t size) const {
    auto* out = static_cast<u8*>(destination);
    if (const u8* p = resolve(address, size)) {
        std::copy_n(p, size, out);
        return;
    }
    // Page runs: a committed page is one copy, anything else reads as zero.
    std::size_t done = 0;
    while (done < size) {
        const u32 at = address + static_cast<u32>(done);
        const std::size_t in_page = kPageSize - (at & kPageOffsetMask);
        const std::size_t chunk = in_page < size - done ? in_page : size - done;
        if (const u8* p = resolve(at, chunk)) {
            std::copy_n(p, chunk, out + done);
        } else {
            std::fill_n(out + done, chunk, u8{0});
        }
        done += chunk;
    }
}

std::vector<u8> GuestMemory::window_bytes(u32 begin, u32 end) const {
    std::vector<u8> bytes(end > begin ? end - begin : 0u);
    if (!bytes.empty()) peek_block(begin, bytes.data(), bytes.size());
    return bytes;
}

void GuestMemory::read_block(u32 address, void* destination, std::size_t size) {
    auto* out = static_cast<u8*>(destination);
    if (const u8* p = host_pointer(address, size)) {
        std::copy_n(p, size, out);
        return;
    }
    for (std::size_t index = 0; index < size; ++index) out[index] = read8(address + static_cast<u32>(index));
}

void GuestMemory::write_block(u32 address, const void* source, std::size_t size) {
    const auto* in = static_cast<const u8*>(source);
    if (access_trace_) trace_access(address, size, true);
    if (journaling_) {
        for (std::size_t index = 0; index < size; ++index) journal_note(address + static_cast<u32>(index), 1u);
    }
    if (u8* p = resolve(address, size)) {
        std::copy_n(in, size, p);
        return;
    }
    for (std::size_t index = 0; index < size; ++index) write8(address + static_cast<u32>(index), in[index]);
}

void GuestMemory::clear_block(u32 address, std::size_t size) {
    if (access_trace_) trace_access(address, size, true);
    if (journaling_) {
        for (std::size_t index = 0; index < size; ++index) journal_note(address + static_cast<u32>(index), 1u);
    }
    if (u8* p = resolve(address, size)) {
        std::fill_n(p, size, u8{0});
        return;
    }
    for (std::size_t index = 0; index < size; ++index) write8(address + static_cast<u32>(index), 0);
}

}  // namespace gekko
