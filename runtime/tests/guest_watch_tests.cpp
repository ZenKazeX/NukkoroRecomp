// liveness-roots: reference-interpreter-only
// The guest watch (gate 2's dynamic half, include/gekko/guest_watch.h): an access by a translated row is attributed
// to that row, through either host view of guest memory, also when the access happens in a helper the row calls; an
// access by native code is attributed to native code; an access to the same page outside the ranges is counted, not
// attributed; nothing is missed; the pages are usable again after stop().

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/guest_watch.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
using namespace gekko;

constexpr u32 kWatched = 0x80100040u;  // inside the program image region (committed)
constexpr u32 kRowA = 0x80001000u, kRowB = 0x80001100u;

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

GuestMemory* g_memory = nullptr;

// A "translated row": reads and writes the watched bytes through the flat-map window, as inline accessors do.
__declspec(noinline) void row_a(Cpu&) {
    volatile u32* p = reinterpret_cast<volatile u32*>(g_memory->fm_window() + kWatched);
    const u32 v = *p;
    *p = v + 1u;
}
// A helper a row calls (not itself a row): the access must be attributed to the calling row.
__declspec(noinline) u32 helper_read(u32 address) {
    return *reinterpret_cast<volatile u32*>(g_memory->fm_window() + address);
}
__declspec(noinline) void row_b(Cpu&) {
    volatile u32 sink = helper_read(kWatched + 4u);
    (void)sink;
}
// Native code: through the native view.
__declspec(noinline) u32 native_read() {
    return *reinterpret_cast<const volatile u32*>(g_memory->peek_pointer(kWatched + 8u, 4u));
}
// Same page, outside the range.
__declspec(noinline) u32 outside_read() {
    return *reinterpret_cast<const volatile u32*>(g_memory->fm_window() + kWatched + 0x100u);
}

}  // namespace

int main() {
    GuestMemory memory;
    g_memory = &memory;
    Cpu cpu{memory};
    RecompiledCode code;
    RecompiledCode::Entry entries[] = {{kRowA, 4u, &row_a, "row A"}, {kRowB, 4u, &row_b, "row B"}};
    code.install(entries, 2);
    memory.write32(kWatched, 41u);

    std::string error;
    std::vector<guest_watch::Range> ranges{{kWatched, 0x10u, "probe"}};
    if (!guest_watch::start(memory, code, ranges, &error)) {
        std::printf("FAIL: start: %s\n", error.c_str());
        return 1;
    }
    row_a(cpu);
    row_b(cpu);
    const u32 n = native_read();
    const u32 o = outside_read();
    guest_watch::stop();
    (void)n;
    (void)o;

    // row A incremented the host-order word: the guest (big-endian) value is the byte-swapped one
    check(memory.read32(kWatched) == _byteswap_ulong(_byteswap_ulong(41u) + 1u),
          "row A's write took effect (the page was opened for exactly its access)");
    check(guest_watch::totals().symbols, "the test runs with symbols (a PDB): leaf rows are attributable");
    const auto hits = guest_watch::hits();
    const auto find = [&](u32 row, bool write) -> const guest_watch::Hit* {
        for (const auto& h : hits)
            if (h.row == row && h.write == write) return &h;
        return nullptr;
    };
    const auto* a_read = find(kRowA, false);
    const auto* a_write = find(kRowA, true);
    const auto* b_read = find(kRowB, false);
    const auto* native = find(0u, false);
    check(a_read && a_read->count == 1 && a_read->first_guest_address == kWatched, "row A's read, attributed to row A");
    check(a_write && a_write->count == 1, "row A's write, attributed to row A");
    check(b_read && b_read->count == 1 && b_read->first_guest_address == kWatched + 4u,
          "row B's read inside a helper, attributed to row B (the unwind reached the row's frame)");
    check(native && native->count == 1 && native->native_rva != 0, "native code's read, attributed to native code");
    check(hits.size() == 4u, "exactly four distinct hits (got " + std::to_string(hits.size()) + ")");
    const auto t = guest_watch::totals();
    check(t.outside_ranges == 1u, "the same-page access outside the range is counted, not attributed");
    check(t.unattributed == 0u, "every access attributed");
    check(t.faults == 5u, "five faults (got " + std::to_string(t.faults) + ")");
    // After stop() the bytes are plain memory again.
    check(native_read() == memory.read32(kWatched + 8u), "readable after stop");
    std::printf("guest watch: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
