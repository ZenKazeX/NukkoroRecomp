// The native owners of the OS arena accessors and the time-base readers (gekko/native_os_arena_clock.h).

#include "gekko/native_os_arena_clock.h"

#include "gekko/cpu.h"

namespace gekko::os_arena_clock {

void serve_get_arena_hi(Cpu& cpu) { cpu.gpr[3] = cpu.mem_read32(cpu.gpr[13] + kArenaHiCell); }

void serve_get_arena_lo(Cpu& cpu) { cpu.gpr[3] = cpu.mem_read32(cpu.gpr[13] + kArenaLoCell); }

void serve_set_arena_lo(Cpu& cpu) { cpu.mem_write32(cpu.gpr[13] + kArenaLoCell, cpu.gpr[3]); }

void serve_get_time(Cpu& cpu) {
    const u64 time = cpu.time_base();
    cpu.gpr[3] = static_cast<u32>(time >> 32);
    cpu.gpr[4] = static_cast<u32>(time);
}

void serve_get_tick(Cpu& cpu) { cpu.gpr[3] = static_cast<u32>(cpu.time_base()); }

void install(Cpu& cpu) {
    static constexpr struct {
        u32 address;
        void (*serve)(Cpu&);
        const char* name;
    } kOwners[] = {{kGetArenaHi, serve_get_arena_hi, "OSGetArenaHi (native)"},
                   {kGetArenaLo, serve_get_arena_lo, "OSGetArenaLo (native)"},
                   {kSetArenaLo, serve_set_arena_lo, "OSSetArenaLo (native)"},
                   {kGetTime, serve_get_time, "OSGetTime (native)"},
                   {kGetTick, serve_get_tick, "OSGetTick (native)"}};
    for (const auto& owner : kOwners) {
        const auto serve = owner.serve;
        cpu.install_hle(owner.address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, owner.name);
    }
}

}  // namespace gekko::os_arena_clock
