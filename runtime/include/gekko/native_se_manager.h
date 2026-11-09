#pragma once

// The game's sound-effect manager, owned natively (docs/SE_MANAGER_MAP.md; docs/GCAX_SOUND_MAP.md "Phase 2 - design";
// sound phase 2).
//
// Original behaviour (main.dol, PAL GUPP8P): the SE manager (singleton 0x80571C34, from GetSoundManager 0x8005328C)
// owns 43 channels of 0x34 bytes (mgr + 4) and the sound tables (mgr + 8). The game asks for sounds through four
// wrappers - PlaySimple 0x801D59AC, PlayFlags 0x801D5894, Play3D 0x801D5764 and the ring sound PlayRing 0x801D56A0 -
// which go through PlaySE 0x801D5F14 (request gate, table lookup, distance cull, priority), the allocator 0x801D5A18
// (a 32-channel round robin that steals the last lower-priority or the oldest equal-priority channel and silences it)
// and Start 0x801D9B2C. Keep-alive requests (an owner word) find their channel with 0x801D5E04 and refresh its
// position and timer. Every AX frame's game update calls FrameSE 0x801D6270, which clears the tables' per-frame start
// counters (0x801D87EC), applies the pause and request gates and runs UpdateChan 0x801D8D2C on every active channel:
// the start (bank/program, volume, pan, bend, note on), positional volume / pan / Doppler tracking with their
// one-frame lags, the timer, retrigger and expiry (reset pan, bend 0, note off). Four tiny gates 0x801D5190 /
// 0x801D51C0 / 0x801D51F0 / 0x801D5218 set and clear the request gate (mgr + 0x14 bit 1). Everything reaches GCAX
// through the sound object's methods (gekko/native_gcax_midi.h).
//
// Native implementation: these nine entries are native owners and every internal function above (PlaySE, the
// allocator, FindOwned, RowFlags, Start, the per-frame counters, UpdateChan, SendPan and the request helpers) is
// unreachable translated code. The positional math is gekko/native_se_math.h; the table lookup is
// gekko/native_sound_tables.h; the MIDI messages go straight into GCAX's rings (gekko/native_gcax_midi.h). The
// manager's state stays where the original keeps it - the manager object, the channels and the table rows in guest
// memory - because the still-translated StopAllSE, SetSoundMode, Pause/Resume and the stage lifecycle functions use
// the same channels (SCAFFOLDING: sound phase 3 moves them, with GCAX's voices, to the host).
//
// The special ring (channels 0x26..0x2A, rows with flags bit 1) is implemented as the original, but its channels
// above 0x26 route to GCAX ports 4..7, whose methods are not native yet: no shipped table has such a row
// (se_manager_contract_tests checks every AudioData*.bin), and a message for such a port is counted
// (Report::unsupported_port) instead of sent.

#include "gekko/native_gcax_midi.h"
#include "gekko/native_sound_tables.h"
#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::se_manager {

constexpr u32 kManager = 0x80571C34u;
constexpr u32 kManagerPointer = 0x805EF0B8u;  // r13 + 0x2BB8: GetSoundManager's result
constexpr u32 kPlaySimple = 0x801D59ACu;
constexpr u32 kPlayFlags = 0x801D5894u;
constexpr u32 kPlay3D = 0x801D5764u;
constexpr u32 kPlayRing = 0x801D56A0u;
constexpr u32 kGateClearIfZero = 0x801D5190u;
constexpr u32 kGateSetIfZero = 0x801D51C0u;
constexpr u32 kGateClear = 0x801D51F0u;
constexpr u32 kGateSet = 0x801D5218u;
constexpr u32 kFrame = 0x801D6270u;

constexpr u32 kChannels = 43u, kChannelBytes = 0x34u;

struct Report {
    u64 requests = 0, started = 0, kept_alive = 0, gated = 0, culled = 0, no_channel = 0, rings = 0;
    u64 frames = 0, channel_updates = 0, expired = 0, retriggered = 0;
    u64 special_ring = 0, unsupported_port = 0;
};

// The services the manager writes to: its own counters, the MIDI rings' and the table lookup's.
struct Services {
    Report& report;
    gcax_midi::Report& midi;
    sound_tables::Report& tables;
};

// The owners (arguments in r3..r9 as the originals take them; r3 = the result where the original returns one).
void serve_play_simple(Cpu& cpu, Services& services) noexcept;
void serve_play_flags(Cpu& cpu, Services& services) noexcept;
void serve_play_3d(Cpu& cpu, Services& services) noexcept;
void serve_play_ring(Cpu& cpu, Services& services) noexcept;
void serve_gate(Cpu& cpu, u32 entry) noexcept;  // the four request-gate entries
void serve_frame(Cpu& cpu, Services& services) noexcept;

// The guest entries served on every call: must equal their rows of extract/native-owners.txt.
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {
            {kPlaySimple, "SE PlaySimple"},         {kPlayFlags, "SE PlayFlags"},
            {kPlay3D, "SE Play3D"},                 {kPlayRing, "SE PlayRing"},
            {kGateClearIfZero, "SE GateClearIfZero"}, {kGateSetIfZero, "SE GateSetIfZero"},
            {kGateClear, "SE GateClear"},           {kGateSet, "SE GateSet"},
            {kFrame, "SE FrameSE"},
        };
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

}  // namespace gekko::se_manager
