#pragma once

// GCAX's MIDI input, owned natively (docs/GCAX_SOUND_MAP.md "Phase 2 - design"; sound phase 2).
//
// Original behaviour (main.dol, PAL GUPP8P): every sound the game plays through GCAX arrives as 3-byte MIDI
// messages in one of eight per-port rings inside the GCAX context (ctx = *(r13 + 0x5FAC) = *0x805F24AC; port i at
// ctx + 0x5478 + 0x1530 i: +0 open flag, +0x524 u32 ring[0x400], +0x1524 read, +0x1528 write, +0x152C count). The sound
// object (AudioGCN, vtable 0x8053F42C; port handles at object + 8 + 4 port) builds them with gcaxMidiMsg 0x8044046C
// and gcaxSend 0x804403AC, which refuse a missing context or a handle that is not an open port, and pack
// status << 24 | (d1 & 0xFF) << 16 | (d2 & 0xFF) << 8 | priority (d2 is 0 for program change and other two-byte
// statuses). gcaxPortPush 0x8043F054 stores the word at ring[write], counts it and advances write modulo 0x400 - or
// drops it silently when 0x400 are waiting. GCAX drains the rings in its AX callback.
//
// Native implementation: the push is a native owner (every caller - the still-translated GCAX front end and the
// native SE manager - lands here), and the SE manager's messages are built here with the sound object's own
// encodings, so no translated code is called to send them. The rings stay where GCAX's still-translated drain reads
// them (SCAFFOLDING: deleted by sound phase 3, whose native voices take the messages directly).

#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
class GuestMemory;
}

namespace gekko::gcax_midi {

constexpr u32 kPortPush = 0x8043F054u;       // gcaxPortPush(port handle, message): served natively
constexpr u32 kContextPointer = 0x805F24ACu;  // *(r13 + 0x5FAC)
constexpr u32 kPorts = 0x5478u, kPortBytes = 0x1530u, kPortCount = 8u;
constexpr u32 kRing = 0x524u, kRead = 0x1524u, kWrite = 0x1528u, kCount = 0x152Cu, kRingWords = 0x400u;
constexpr u32 kOutputConfig = 0x8057E760u;    // +0x78: output mode (2 = surround)

struct Report {
    u64 pushed = 0, dropped = 0, refused = 0;
};

// gcaxPortPush's body: true when stored, false when the ring was full (dropped, as the original).
bool push(GuestMemory& memory, u32 port, u32 message, Report& report) noexcept;

// gcaxMidiMsg + gcaxSend: the context and handle checks, the packing, the push. False when refused or dropped.
bool send(GuestMemory& memory, u32 handle, u32 status, u32 d1, u32 d2, u32 priority, Report& report) noexcept;

// The sound object's methods for ports 0..2 (AudioGCN; object = the sound object's guest address). Each sends
// exactly the messages the original method sends, in the same order; their arithmetic uses the CPU's own
// floating-point model (gekko/ppc_ops.h) so every value is the translated method's.
struct SoundObject {
    Cpu& cpu;
    u32 object;
    Report& report;
    u32 handle(s32 port) const noexcept;
    void set_bank_program(s32 port, u32 channel, s32 bank, s32 program) noexcept;  // vt+0x50
    void note_on(u32 priority, s32 port, u32 channel) noexcept;                     // vt+0x24
    void note_off(s32 port, u32 channel) noexcept;                                  // vt+0x30
    void volume(s32 attenuation, s32 port, u32 channel) noexcept;                   // vt+0x38
    void pan(s32 value, s32 port, u32 channel) noexcept;                            // vt+0x3C
    void surround(s32 value, s32 port, u32 channel) noexcept;                       // vt+0x44 (CC62)
    void bend(s32 value, s32 port, u32 channel) noexcept;                           // vt+0x4C
    void reset_pan(s32 port, u32 channel) noexcept;                                 // vt+0x48
};

// The guest entries served on every call: must equal their rows of extract/native-owners.txt.
struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kPortPush, "gcaxPortPush"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

// The push owner (r3 = port handle, r4 = message).
void serve_push(Cpu& cpu, Report& report) noexcept;

}  // namespace gekko::gcax_midi
