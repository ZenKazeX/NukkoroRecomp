#pragma once
// F1c test-only guard allocator. Linking f1c_guard_alloc.cpp into a TEST executable replaces the global
// operator new / delete (all forms) for that executable only; no shipped target links it.
//
//   * size >= kGuardThreshold: a dedicated VirtualAlloc block
//       [PAGE_NOACCESS guard page][canary-filled head slack][object][canary tail pad < alignment][PAGE_NOACCESS]
//     The object ENDS at the trailing guard page (rounded to its alignment), so any read or write past its end
//     faults at once; an underrun lands in the canary-filled head slack or the leading guard page.
//   * smaller: a heap block with a 32-byte canary header and a 16-byte canary footer.
// Every live block is registered; check_all() verifies every canary byte of every live block. delete verifies
// the block being freed first. Violations are recorded (never aborted on) and reported by summary().
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace f1c::guard {

constexpr std::size_t kGuardThreshold = 4096;

struct Violation {
    std::uintptr_t block = 0;      // user pointer
    std::size_t size = 0;
    std::uintptr_t first_bad = 0;  // address of the first corrupted canary byte
    std::size_t bad_bytes = 0;
    const char* where = "";        // "head", "tail", "header", "footer"
    const char* when = "";         // "check", "delete"
};

struct Stats {
    std::uint64_t guarded_allocations = 0;
    std::uint64_t small_allocations = 0;
    std::uint64_t live_guarded = 0;
    std::uint64_t live_small = 0;
    std::uint64_t untracked = 0;       // registry full (never expected)
    std::uint64_t checks = 0;
    std::uint64_t canary_bytes_checked = 0;
    std::uint64_t violations = 0;
    std::uint64_t guarded_bytes_live = 0;
};

// Verifies every canary of every live block; returns the number of NEW violations found.
std::size_t check_all(const char* when = "check");
Stats stats();
std::vector<Violation> violations();

// The guarded block holding `pointer` (user pointer of a live guarded allocation), or false.
bool guarded_block(std::uintptr_t pointer, std::size_t& size);
// Any live tracked block whose user pointer is `pointer`: its size and whether it is guard-paged, or false.
bool tracked_block(std::uintptr_t pointer, std::size_t& size, bool& guarded);
// The layout allocate() selects for an object of this size and alignment.
constexpr bool guard_paged_layout(std::size_t size, std::size_t alignment) {
    return size >= kGuardThreshold || alignment > 16;
}
// All live guarded allocations of exactly `size` bytes (user pointers), for locating host tables.
std::vector<std::uintptr_t> guarded_of_size(std::size_t size);

std::string summary_json();

}  // namespace f1c::guard
