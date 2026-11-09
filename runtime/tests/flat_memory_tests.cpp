// liveness-roots: reference-interpreter-only
//
// FM (EV, 2026-10-20; EV_REPORT 17): the flat native memory map's guest-visible behaviour, case by case, against
// the semantics the page-table accessors had (the *_slow bodies decide every access the window does not serve).
// Every width (1 / 2 / 4 / 8) x read / write through the CPU's inline accessors, in a machine with
// unmapped-access stops on (the production setting):
//   A  mapped, committed memory, at every offset around 4 KiB and 16 KiB page boundaries inside a region:
//      the big-endian value, no fault, no handler entry;
//   B  a hole of the window (0x90000000) and the retired device page 0xCC008000: read 0 / store dropped, the
//      fault latched with DAR = the effective address and the write flag, the machine stopped; afterwards every
//      access (even to mapped memory) is suppressed: reads 0, stores leave memory unchanged;
//   C  a region's uncommitted tail: as B;
//   D  an access that straddles the end of a region's committed prefix: as B, and NOT ONE byte below the
//      boundary changes (no partial store);
//   E  MSR[DR] = 0: a translation fault (latched, unmapped = false), read 0 / store dropped;
//   F  an access trace installed: values exact, and the trace sees every access as (address, size, writing),
//      in order, exactly as GuestMemory::trace_access reported it for the slow path;
//   G  the journal (journal_begin / journal_end): values exact while journaling.
// usage: flat_memory_tests

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"

#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <vector>

namespace {
using gekko::u16;
using gekko::u32;
using gekko::u64;
using gekko::u8;

int g_failures = 0;
long g_checks = 0;
void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        if (g_failures <= 60) std::printf("FAIL: %s\n", what.c_str());
    }
}

struct Machine {
    gekko::GuestMemory bus;
    gekko::Cpu cpu{bus};
    Machine() {
        cpu.reset();
        cpu.set_unmapped_access_stops(true);
        cpu.set_msr(cpu.msr() | gekko::kMsrDR | gekko::kMsrIR | gekko::kMsrFP);
        // An arena with a committed prefix and an uncommitted tail (cases C, D).
        if (!bus.map(0xA0000000u, 0x10000u, 0x4000u, gekko::GuestMemory::RegionKind::Arena, "fm-test-arena")) {
            std::printf("FAIL: cannot map the test arena\n");
            ++g_failures;
        }
    }
};

u64 read_access(gekko::Cpu& c, u32 ea, unsigned width) {
    switch (width) {
        case 1: return c.mem_read8(ea);
        case 2: return c.mem_read16(ea);
        case 4: return c.mem_read32(ea);
        default: return c.mem_read64(ea);
    }
}
void write_access(gekko::Cpu& c, u32 ea, unsigned width, u64 v) {
    switch (width) {
        case 1: c.mem_write8(ea, static_cast<u8>(v)); break;
        case 2: c.mem_write16(ea, static_cast<u16>(v)); break;
        case 4: c.mem_write32(ea, static_cast<u32>(v)); break;
        default: c.mem_write64(ea, v); break;
    }
}
u64 bus_value(gekko::GuestMemory& bus, u32 ea, unsigned width) {
    u64 v = 0;
    for (unsigned i = 0; i < width; ++i) v = (v << 8) | bus.read8(ea + i);
    return v;
}
u64 mask(unsigned width) { return width == 8 ? ~0ull : (1ull << (8 * width)) - 1u; }
std::string at(const char* what, u32 ea, unsigned width, bool write) {
    char b[128];
    std::snprintf(b, sizeof b, "%s %s%u at 0x%08X", what, write ? "write" : "read", width * 8u, ea);
    return b;
}

void case_mapped(std::mt19937_64& random) {
    for (unsigned width : {1u, 2u, 4u, 8u}) {
        Machine m;
        for (u32 boundary : {0x80104000u, 0x80105000u, 0x80700000u}) {  // 16 KiB, 4 KiB, legacy heap
            for (u32 offset = 0; offset < 16u; ++offset) {
                const u32 ea = boundary - 8u + offset;
                const u64 before = gekko::Cpu::fm_fault_count();
                const u64 v = random() & mask(width);
                write_access(m.cpu, ea, width, v);
                check(bus_value(m.bus, ea, width) == v, at("A mapped", ea, width, true));
                check(read_access(m.cpu, ea, width) == v, at("A mapped", ea, width, false));
                check(gekko::Cpu::fm_fault_count() == before, at("A no handler entry", ea, width, false));
                check(!m.cpu.memory_fault().latched && !m.cpu.stopped(), at("A no fault", ea, width, false));
            }
        }
    }
}

// B / C / H: an unserved address. The first access faults; afterwards all access is suppressed.
void case_unserved(const char* name, u32 ea, std::mt19937_64& random) {
    for (unsigned width : {1u, 2u, 4u, 8u}) {
        for (bool write : {false, true}) {
            Machine m;
            const u32 probe = 0x80200000u;  // mapped memory, checked after the fault
            const u64 probe_value = random() & mask(width);
            write_access(m.cpu, probe, width, probe_value);
            const u64 before = gekko::Cpu::fm_fault_count();
            if (write) {
                write_access(m.cpu, ea, width, random());
            } else {
                check(read_access(m.cpu, ea, width) == 0u, at(name, ea, width, false) + " reads 0");
            }
            const auto& f = m.cpu.memory_fault();
            check(f.latched && f.unmapped && f.effective == ea && f.write == write, at(name, ea, width, write) + " latch");
            check(m.cpu.stopped(), at(name, ea, width, write) + " stops the machine");
            check(gekko::Cpu::fm_fault_count() > before, at(name, ea, width, write) + " went through the handler");
            check(read_access(m.cpu, probe, width) == 0u, at(name, ea, width, write) + " later read suppressed");
            write_access(m.cpu, probe, width, ~probe_value);
            check(bus_value(m.bus, probe, width) == probe_value, at(name, ea, width, write) + " later store dropped");
            check(m.cpu.memory_fault().effective == ea, at(name, ea, width, write) + " first fault kept");
        }
    }
}

void case_straddle() {
    for (unsigned width : {2u, 4u, 8u}) {
        for (unsigned inside = 1; inside < width; ++inside) {
            Machine m;
            const u32 ea = 0xA0004000u - inside;
            for (u32 i = 0; i < 8u; ++i) m.bus.write8(0xA0004000u - 8u + i, 0x5Au);
            write_access(m.cpu, ea, width, ~0ull);
            bool unchanged = true;
            for (u32 i = 0; i < 8u; ++i) unchanged = unchanged && m.bus.read8(0xA0004000u - 8u + i) == 0x5Au;
            check(unchanged, at("D straddle: no partial store", ea, width, true));
            const auto& f = m.cpu.memory_fault();
            check(f.latched && f.effective == ea && f.write, at("D straddle latch", ea, width, true));
            Machine r;
            check(read_access(r.cpu, ea, width) == 0u && r.cpu.memory_fault().latched,
                  at("D straddle read", ea, width, false));
        }
    }
}

void case_translation_off() {
    for (unsigned width : {1u, 2u, 4u, 8u}) {
        for (bool write : {false, true}) {
            Machine m;
            m.cpu.set_msr(m.cpu.msr() & ~gekko::kMsrDR);
            const u32 ea = 0x80300000u;
            if (write) {
                m.bus.write8(ea, 0x11u);
                write_access(m.cpu, ea, width, 0);
                check(m.bus.read8(ea) == 0x11u, at("E DR=0 store dropped", ea, width, true));
            } else {
                check(read_access(m.cpu, ea, width) == 0u, at("E DR=0 read 0", ea, width, false));
            }
            const auto& f = m.cpu.memory_fault();
            check(f.latched && !f.unmapped && f.effective == ea && f.write == write,
                  at("E DR=0 translation fault", ea, width, write));
        }
    }
}

void case_trace(std::mt19937_64& random) {
    Machine m;
    std::vector<std::tuple<u32, std::size_t, bool>> seen, want;
    m.bus.set_access_trace([&](u32 a, std::size_t s, bool w) { seen.emplace_back(a, s, w); });
    m.cpu.select_memory_window();
    for (int i = 0; i < 200; ++i) {
        const unsigned width = 1u << (random() % 4);
        const u32 ea = 0x80400000u + static_cast<u32>(random() % 0x8000u);
        const bool write = (random() & 1u) != 0u;
        const u64 v = random() & mask(width);
        if (write) {
            write_access(m.cpu, ea, width, v);
        } else {
            (void)read_access(m.cpu, ea, width);
        }
        want.emplace_back(ea, width, write);
    }
    m.bus.set_access_trace({});
    m.cpu.select_memory_window();
    check(seen == want, "F access trace: every access, in order, as (address, size, writing)");
}

void case_journal(std::mt19937_64& random) {
    Machine m;
    m.bus.journal_begin();
    m.cpu.select_memory_window();
    for (unsigned width : {1u, 2u, 4u, 8u}) {
        const u32 ea = 0x80500000u + 16u * width;
        const u64 v = random() & mask(width);
        write_access(m.cpu, ea, width, v);
        check(read_access(m.cpu, ea, width) == v, at("G journal value", ea, width, false));
    }
    m.bus.journal_end();
    m.cpu.select_memory_window();
    check(!m.cpu.memory_fault().latched, "G journal: no fault");
}
}  // namespace

int main() {
    std::mt19937_64 random(20261020u);
    case_mapped(random);
    case_unserved("B hole", 0x90000000u, random);
    case_unserved("H retired device page", 0xCC008000u, random);
    case_unserved("C uncommitted tail", 0xA0008000u, random);
    case_straddle();
    case_translation_off();
    case_trace(random);
    case_journal(random);
    std::printf("flat_memory_tests: %ld checks, %d failures, %llu handler entries\n", g_checks, g_failures,
                static_cast<unsigned long long>(gekko::Cpu::fm_fault_count()));
    return g_failures == 0 ? 0 : 1;
}
