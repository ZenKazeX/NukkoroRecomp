#pragma once

// Gate 2's dynamic half (docs/GC_REMOVAL_PLAN.md "Gates for every removal step"): every access to given guest bytes,
// attributed exactly to the translated row that made it - or to native runtime code. DIAGNOSTIC ONLY (compiled out of
// shipping builds): it is the evidence that no surviving code reads state a removal step retires.
//
// Mechanism: the host pages backing the watched guest ranges are made inaccessible in BOTH host views of guest
// memory (the native view and the flat-map window). A vectored exception handler sees every access, records it,
// re-opens the page for exactly one instruction (the trap flag) and closes it again. The access is attributed by
// unwinding the faulting host stack (x64 unwind data) until a frame whose function is a translated row's host entry
// (RecompiledCode::Entry::fn); a stack with no such frame is native runtime code (recorded by its innermost RVA).
// No generator change, no link-register guesswork: the frame that executes the guest row is the row.
//
// Limits (stated in every report): page granularity - accesses to other bytes of a watched page fault and are counted
// as "same page, outside the ranges", then ignored; while one thread single-steps through an opened page, another
// thread's access to that page in that instant is not seen (the game thread is the only one running guest code; the
// render / audio threads read guest memory only through native code); Windows x64 only.

#include <cstdint>
#include <string>
#include <vector>

#include "gekko/types.h"

namespace gekko {

class GuestMemory;
class RecompiledCode;

namespace guest_watch {

struct Range {
    u32 base = 0;
    u32 size = 0;
    std::string label;
};

// A row's (or native code's) accesses to one watched range.
struct Hit {
    std::string label;          // the range
    bool write = false;
    u32 row = 0;                // guest address of the translated row; 0 = native code
    std::uint64_t native_rva = 0;  // native: the innermost frame's RVA in its module
    std::uint64_t native_address = 0;  // native: the innermost frame's address (named in the report)
    std::uint64_t count = 0;
    u32 first_guest_address = 0;
};

struct Totals {
    std::uint64_t faults = 0;          // every watched-page fault
    std::uint64_t outside_ranges = 0;  // a watched page, a byte outside every range
    std::uint64_t unattributed = 0;    // the unwind failed (reported, never dropped)
    bool symbols = false;              // exact function extents (a PDB); without, a leaf row reads as native code
};

// Protects the ranges' pages (both views). Ranges must be committed guest memory. False (with `error`) when a page
// cannot be protected or the platform is unsupported. Call once, from the thread that owns the machine.
bool start(GuestMemory& memory, const RecompiledCode& code, std::vector<Range> ranges, std::string* error);
// Restores every page; hits stay readable.
void stop();
bool active();
std::vector<Hit> hits();
Totals totals();
// Writes a CSV: label,kind,row,native_rva,native_symbol,count,first_guest_address - and the totals as comments.
bool write_report(const std::string& path, std::string* error);
// Parses "0xBASE 0xSIZE label" lines ('#' comments).
bool parse_ranges(const std::string& path, std::vector<Range>& out, std::string* error);

}  // namespace guest_watch
}  // namespace gekko
