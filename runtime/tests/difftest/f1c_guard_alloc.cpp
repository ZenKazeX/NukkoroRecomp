// F1c test-only guard allocator: replaces the global operator new/delete of the test executable it is
// linked into. See f1c_guard_alloc.h. Never linked into a shipped target.
//
// Block layouts (every live block is on one intrusive doubly linked list, so allocation, release and a full
// check are O(1), O(1) and O(live blocks) whatever the churn):
//   guard-paged  [NOACCESS page][Header 64][head canary >= 32][back pointer 8][object][tail canary][NOACCESS page]
//                the object ends at the trailing guard page, rounded down to its alignment
//   small        [Header 64][canary 40][back pointer 8][object][canary 16]            (HeapAlloc)
// The back pointer (header address xor a key) sits immediately before the object: release() finds the header
// from the user pointer and verifies it before trusting it.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "f1c_guard_alloc.h"

#include <cstdio>
#include <cstring>
#include <new>

namespace f1c::guard {
namespace {

constexpr std::size_t kPage = 4096;
constexpr std::size_t kHeader = 64;
constexpr std::size_t kSmallPrefix = 112;  // header + 40 canary + 8 back pointer; 16-aligned object
constexpr std::size_t kSmallFooter = 16;
constexpr std::uint64_t kMagicGuarded = 0xF1C06A12D0B10C4Bull;
constexpr std::uint64_t kMagicSmall = 0xF1C05AA11B10C4E5ull;
constexpr std::uintptr_t kBackKey = 0x5A5AC3C3F1C0F1C0ull;

struct Header {
    std::uint64_t magic;
    Header* prev;
    Header* next;
    std::uintptr_t user;
    std::size_t size;
    std::uintptr_t base;
    std::size_t total;
    std::uint64_t magic_not;
};

Header g_head{};  // sentinel of the live-block list (prev/next null until the first allocation)
SRWLOCK g_lock = SRWLOCK_INIT;
Stats g_stats;
constexpr std::size_t kMaxViolations = 256;
Violation g_violations[kMaxViolations];
std::size_t g_violation_count = 0;

inline std::uint8_t pattern(std::uintptr_t address) noexcept {
    return static_cast<std::uint8_t>(0x5Au ^ address ^ (address >> 8) ^ (address >> 17));
}

void fill(std::uintptr_t begin, std::uintptr_t end) noexcept {
    for (std::uintptr_t a = begin; a < end; ++a) *reinterpret_cast<std::uint8_t*>(a) = pattern(a);
}

void record(std::uintptr_t block, std::size_t size, std::uintptr_t first, std::size_t bad, const char* where,
            const char* when) noexcept {
    ++g_stats.violations;
    if (g_violation_count < kMaxViolations) g_violations[g_violation_count++] = Violation{block, size, first, bad, where, when};
}

void verify(std::uintptr_t begin, std::uintptr_t end, const Header& h, const char* where, const char* when) noexcept {
    g_stats.canary_bytes_checked += end - begin;
    std::uintptr_t first = 0;
    std::size_t bad = 0;
    for (std::uintptr_t a = begin; a < end; ++a) {
        if (*reinterpret_cast<std::uint8_t*>(a) != pattern(a)) {
            if (bad == 0) first = a;
            ++bad;
        }
    }
    if (bad != 0) record(h.user, h.size, first, bad, where, when);
}

bool header_ok(const Header& h) noexcept {
    return (h.magic == kMagicGuarded || h.magic == kMagicSmall) && h.magic_not == ~h.magic;
}

void verify_block(const Header& h, const char* when) noexcept {
    const auto back = *reinterpret_cast<const std::uintptr_t*>(h.user - 8u);
    if (back != (reinterpret_cast<std::uintptr_t>(&h) ^ kBackKey)) record(h.user, h.size, h.user - 8u, 8u, "back-pointer", when);
    if (h.magic == kMagicGuarded) {
        verify(h.base + kPage + kHeader, h.user - 8u, h, "head", when);
        verify(h.user + h.size, h.base + h.total - kPage, h, "tail", when);
    } else {
        verify(h.base + kHeader, h.user - 8u, h, "header", when);
        verify(h.user + h.size, h.user + h.size + kSmallFooter, h, "footer", when);
    }
}

void link(Header* h) noexcept {
    if (g_head.next == nullptr) {
        g_head.next = &g_head;
        g_head.prev = &g_head;
    }
    h->next = g_head.next;
    h->prev = &g_head;
    g_head.next->prev = h;
    g_head.next = h;
}

void unlink(Header* h) noexcept {
    h->prev->next = h->next;
    h->next->prev = h->prev;
    h->prev = h->next = nullptr;
}

void* allocate(std::size_t size, std::size_t alignment) noexcept {
    if (size == 0) size = 1;
    if (alignment < 16) alignment = 16;
    AcquireSRWLockExclusive(&g_lock);
    void* result = nullptr;
    if (guard_paged_layout(size, alignment)) {
        const std::size_t span = (size + alignment + kHeader + 8u + 32u + kPage - 1u) & ~(kPage - 1u);
        const std::size_t total = span + 2u * kPage;
        void* raw = VirtualAlloc(nullptr, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (raw != nullptr) {
            const auto base = reinterpret_cast<std::uintptr_t>(raw);
            DWORD old = 0;
            VirtualProtect(raw, kPage, PAGE_NOACCESS, &old);
            VirtualProtect(reinterpret_cast<void*>(base + total - kPage), kPage, PAGE_NOACCESS, &old);
            const std::uintptr_t end = base + total - kPage;
            const std::uintptr_t user = (end - size) & ~(static_cast<std::uintptr_t>(alignment) - 1u);
            auto* h = reinterpret_cast<Header*>(base + kPage);
            *h = Header{kMagicGuarded, nullptr, nullptr, user, size, base, total, ~kMagicGuarded};
            fill(base + kPage + kHeader, user - 8u);
            *reinterpret_cast<std::uintptr_t*>(user - 8u) = reinterpret_cast<std::uintptr_t>(h) ^ kBackKey;
            fill(user + size, end);
            link(h);
            ++g_stats.guarded_allocations;
            ++g_stats.live_guarded;
            g_stats.guarded_bytes_live += size;
            result = reinterpret_cast<void*>(user);
        }
    } else {
        const std::size_t total = kSmallPrefix + size + kSmallFooter;
        void* raw = HeapAlloc(GetProcessHeap(), 0, total);
        if (raw != nullptr) {
            const auto base = reinterpret_cast<std::uintptr_t>(raw);
            const std::uintptr_t user = base + kSmallPrefix;
            auto* h = reinterpret_cast<Header*>(base);
            *h = Header{kMagicSmall, nullptr, nullptr, user, size, base, total, ~kMagicSmall};
            fill(base + kHeader, user - 8u);
            *reinterpret_cast<std::uintptr_t*>(user - 8u) = reinterpret_cast<std::uintptr_t>(h) ^ kBackKey;
            fill(user + size, user + size + kSmallFooter);
            link(h);
            ++g_stats.small_allocations;
            ++g_stats.live_small;
            result = reinterpret_cast<void*>(user);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return result;
}

Header* header_of(std::uintptr_t user) noexcept {
    const auto back = *reinterpret_cast<const std::uintptr_t*>(user - 8u);
    auto* h = reinterpret_cast<Header*>(back ^ kBackKey);
    return h;
}

void release(void* pointer) noexcept {
    if (pointer == nullptr) return;
    const auto user = reinterpret_cast<std::uintptr_t>(pointer);
    AcquireSRWLockExclusive(&g_lock);
    Header* h = header_of(user);
    const auto hp = reinterpret_cast<std::uintptr_t>(h);
    bool readable = hp < user && (user - hp == kSmallPrefix || ((hp & (kPage - 1u)) == 0u && user - hp < (std::uintptr_t{1} << 40)));
    if (readable) {
        MEMORY_BASIC_INFORMATION info{};
        readable = user - hp == kSmallPrefix ||
                   (VirtualQuery(h, &info, sizeof(info)) != 0 && info.State == MEM_COMMIT &&
                    (info.Protect & PAGE_READWRITE) != 0 && (info.Protect & PAGE_GUARD) == 0);
    }
    if (!readable || !header_ok(*h) || h->user != user) {
        // A corrupted back pointer or header, or a pointer this allocator never returned: leak it.
        ++g_stats.untracked;
        record(user, 0u, user - 8u, 8u, "back-pointer", "delete");
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    verify_block(*h, "delete");
    unlink(h);
    if (h->magic == kMagicGuarded) {
        const std::uintptr_t base = h->base;
        --g_stats.live_guarded;
        g_stats.guarded_bytes_live -= h->size;
        h->magic = 0;
        VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE);
    } else {
        const std::uintptr_t base = h->base;
        --g_stats.live_small;
        h->magic = 0;
        HeapFree(GetProcessHeap(), 0, reinterpret_cast<void*>(base));
    }
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace

std::size_t check_all(const char* when) {
    AcquireSRWLockExclusive(&g_lock);
    const std::uint64_t before = g_stats.violations;
    ++g_stats.checks;
    if (g_head.next != nullptr) {
        for (Header* h = g_head.next; h != &g_head; h = h->next) {
            if (!header_ok(*h)) {
                record(h->user, h->size, reinterpret_cast<std::uintptr_t>(h), kHeader, "header-magic", when);
                break;  // the list itself cannot be trusted past a corrupted header
            }
            verify_block(*h, when);
        }
    }
    const auto found = static_cast<std::size_t>(g_stats.violations - before);
    ReleaseSRWLockExclusive(&g_lock);
    return found;
}

Stats stats() {
    AcquireSRWLockShared(&g_lock);
    const Stats s = g_stats;
    ReleaseSRWLockShared(&g_lock);
    return s;
}

std::vector<Violation> violations() {
    // No allocation while the lock is held: operator new takes the same lock.
    Violation local[kMaxViolations];
    AcquireSRWLockShared(&g_lock);
    const std::size_t n = g_violation_count;
    for (std::size_t i = 0; i < n; ++i) local[i] = g_violations[i];
    ReleaseSRWLockShared(&g_lock);
    return std::vector<Violation>(local, local + n);
}

bool guarded_block(std::uintptr_t pointer, std::size_t& size) {
    AcquireSRWLockShared(&g_lock);
    bool ok = false;
    if (g_head.next != nullptr) {
        for (Header* h = g_head.next; h != &g_head; h = h->next) {
            if (h->user == pointer) {
                ok = h->magic == kMagicGuarded;
                if (ok) size = h->size;
                break;
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

bool tracked_block(std::uintptr_t pointer, std::size_t& size, bool& guarded) {
    AcquireSRWLockShared(&g_lock);
    bool ok = false;
    if (g_head.next != nullptr) {
        for (Header* h = g_head.next; h != &g_head; h = h->next) {
            if (h->user == pointer) {
                ok = header_ok(*h);
                if (ok) {
                    size = h->size;
                    guarded = h->magic == kMagicGuarded;
                }
                break;
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

std::vector<std::uintptr_t> guarded_of_size(std::size_t size) {
    // No allocation while the lock is held: operator new takes the same lock.
    std::uintptr_t local[64];
    std::size_t n = 0;
    AcquireSRWLockShared(&g_lock);
    if (g_head.next != nullptr) {
        for (Header* h = g_head.next; h != &g_head; h = h->next) {
            if (h->magic == kMagicGuarded && h->size == size && n < 64u) local[n++] = h->user;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    return std::vector<std::uintptr_t>(local, local + n);
}

std::string summary_json() {
    const Stats s = stats();
    const std::vector<Violation> v = violations();
    char b[512];
    std::snprintf(b, sizeof(b),
                  "{\"guarded_allocations\":%llu,\"small_allocations\":%llu,\"live_guarded\":%llu,\"live_small\":%llu,"
                  "\"untracked\":%llu,\"checks\":%llu,\"canary_bytes_checked\":%llu,\"violations\":%llu,"
                  "\"guarded_bytes_live\":%llu,\"violation_list\":[",
                  static_cast<unsigned long long>(s.guarded_allocations), static_cast<unsigned long long>(s.small_allocations),
                  static_cast<unsigned long long>(s.live_guarded), static_cast<unsigned long long>(s.live_small),
                  static_cast<unsigned long long>(s.untracked), static_cast<unsigned long long>(s.checks),
                  static_cast<unsigned long long>(s.canary_bytes_checked), static_cast<unsigned long long>(s.violations),
                  static_cast<unsigned long long>(s.guarded_bytes_live));
    std::string j = b;
    for (std::size_t i = 0; i < v.size(); ++i) {
        std::snprintf(b, sizeof(b), "%s{\"block\":\"0x%llx\",\"size\":%zu,\"first_bad\":\"0x%llx\",\"bad_bytes\":%zu,"
                                    "\"where\":\"%s\",\"when\":\"%s\"}",
                      i ? "," : "", static_cast<unsigned long long>(v[i].block), v[i].size,
                      static_cast<unsigned long long>(v[i].first_bad), v[i].bad_bytes, v[i].where, v[i].when);
        j += b;
    }
    return j + "]}";
}

}  // namespace f1c::guard

// ---- global replacements (this test executable only) ----------------------------------------------------
void* operator new(std::size_t size) {
    if (void* p = f1c::guard::allocate(size, 16)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    if (void* p = f1c::guard::allocate(size, 16)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return f1c::guard::allocate(size, 16); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return f1c::guard::allocate(size, 16); }
void* operator new(std::size_t size, std::align_val_t align) {
    if (void* p = f1c::guard::allocate(size, static_cast<std::size_t>(align))) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t align) {
    if (void* p = f1c::guard::allocate(size, static_cast<std::size_t>(align))) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return f1c::guard::allocate(size, static_cast<std::size_t>(align));
}
void* operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t&) noexcept {
    return f1c::guard::allocate(size, static_cast<std::size_t>(align));
}
void operator delete(void* p) noexcept { f1c::guard::release(p); }
void operator delete[](void* p) noexcept { f1c::guard::release(p); }
void operator delete(void* p, std::size_t) noexcept { f1c::guard::release(p); }
void operator delete[](void* p, std::size_t) noexcept { f1c::guard::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { f1c::guard::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { f1c::guard::release(p); }
void operator delete(void* p, std::align_val_t) noexcept { f1c::guard::release(p); }
void operator delete[](void* p, std::align_val_t) noexcept { f1c::guard::release(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { f1c::guard::release(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { f1c::guard::release(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { f1c::guard::release(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { f1c::guard::release(p); }
