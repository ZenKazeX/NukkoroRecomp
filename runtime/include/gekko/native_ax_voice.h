#pragma once

// The AX library's voice setters, owned natively (docs/agents/audio-ax.md, stage c). Each is the original's field
// write into the guest AXVPB (stride 0x22C; the game's GCAX code and CRI read those fields back), with the
// OSDisableInterrupts / OSRestoreInterrupts bracket dropped: an owner runs between guest instructions, so the
// update is atomic and the MSR is untouched (the original restores it). The sync-flag word at AXVPB+0x1C is the
// SDK's dirty mask for its AXVPB -> PB copy; the guest __AXSyncPB still consumes it until the frame engine is native.
//
// Original (main.dol, PAL GUPP8P):
//   AXSetVoiceMix     0x803871B0 (vpb, mix*): 18 halfwords from mix* to vpb+0x14A.., each stored as is; vpb+0x144 = the
//                                OR of the mask of every non-zero halfword (kMixMasks); vpb+0x1C |= 0x12. Caller 0x8043D2F4.
//   AXSetVoiceSrcType 0x80387090, State 0x80387154, Addr 0x80387404, Adpcm 0x80387514, Src 0x803875B8, AdpcmLoop
//                                0x803876E8: field and block copies into the AXVPB (kAddrBlock...) with the SDK's dirty
//                                bits in vpb+0x1C; SrcType / State / Addr / Src are also called by CRI's ADX voice.
//   AXSetVoiceSrcRatio 0x80387650 (vpb, f1 ratio): ratio*65536 as float, converted to u32 with the SDK's saturating
//                                float->u32 (0x803A1DC8: negative 0, >= 2^32 or NaN 0xFFFFFFFF), clamped to 0x40000;
//                                vpb+0x1DE = high half, vpb+0x1E0 = low half; vpb+0x1C |= 0x80000. Caller 0x8043D1C0.

#include <cstddef>
#include <cstring>
#include <string>

#include "gekko/cpu.h"
#include "gekko/native_guest_call.h"
#include "gekko/types.h"

namespace gekko::ax_voice {

constexpr u32 kSetVoiceMix = 0x803871B0u;
constexpr u32 kSetVoiceSrcRatio = 0x80387650u;
constexpr u32 kSetVoiceSrcType = 0x80387090u;
constexpr u32 kSetVoiceState = 0x80387154u;
constexpr u32 kSetVoiceAddr = 0x80387404u;
constexpr u32 kSetVoiceAdpcm = 0x80387514u;
constexpr u32 kSetVoiceSrc = 0x803875B8u;
constexpr u32 kSetVoiceAdpcmLoop = 0x803876E8u;
constexpr u32 kFreeVoice = 0x803849D4u;
// MIX channel entries (0x60 bytes each at kMixChannels, indexed by AXVPB+0x18): word 0 = in use, +4 = dirty flags,
// +8 = input level, +0x14 = pan, +0x18 = aux level, +0x20..+0x34 = the pan table lookups MIXUpdateSettings consumes.
constexpr u32 kMixInit = 0x8038A1F4u;
constexpr u32 kMixHookSetter = 0x8038A12Cu;  // stores r3, r4 at sda+0x27B8 / +0x27BC (callers 0x8043F304 / 0x8043F33C)
constexpr u32 kSdaMixHookA = 0x27B8u, kSdaMixHookB = 0x27BCu, kSdaMixState0 = 0x5DC0u, kSdaMixState1 = 0x5DC4u;
constexpr u32 kMixChannelCount = 64u;
constexpr u32 kMixInitChannel = 0x8038A2C8u;
constexpr u32 kMixUpdateSettings = 0x8038B1F8u;
constexpr u32 kAiSetStreamVolLeft = 0x803821ECu, kAiSetStreamVolRight = 0x80382218u;  // audio.ai owners
constexpr u32 kMixReleaseChannel = 0x8038B144u;
constexpr u32 kMixSetInput = 0x8038B164u;
constexpr u32 kMixSetPan = 0x8038B18Cu;
constexpr u32 kMixChannels = 0x805A14A0u, kMixChannelSize = 0x60u, kVpbIndexWord = 0x18u;
constexpr u32 kSdaMixMode = 0x5DC8u;       // MIXSetSoundMode's word (native_mix_entries); 3 = the 16-bit pan tables
constexpr u32 kPanTables = 0x80567590u;    // r4 = 0x80560000 + 0x7590 in 0x8038A138
constexpr u32 kAcquireVoice = 0x80384A54u;
constexpr u32 kAcquireSearchLoop = 0x80384AB8u;       // the steal search's loop head: an owner entry (see below)
// The drop callback returns to the owner's own entry (an address of the original function that is never fetched; using the original's
// interior return point 0x80384B40 made the generator keep that row as a frontier row).
constexpr u32 kMsrEE = 0x8000u;

// The voice stacks (AXInit builds them): per priority 0..31 a head at 0x80587DC0 + 4p and a tail at 0x80587E40 + 4p, the
// voices chained through AXVPB+0 (next) / +4 (prev), the priority at +0xC. Priority 0 is the free stack (singly used: head
// only). AXVPB+0x10 / +0x14 are the drop callback and its context.
constexpr u32 kStackHeads = 0x80587DC0u, kStackTails = 0x80587E40u;
constexpr u32 kPriorityWord = 0xCu;

constexpr u32 kEmbeddedPb = 0x138u;  // the PB copy inside the AXVPB; SrcType writes its words at +8 / +0xA
constexpr u32 kStateHalfword = 0x146u, kDepopWord = 0x20u;
constexpr u32 kAddrBlock = 0x1A6u, kAdpcmBlock = 0x1B6u, kSrcBlock = 0x1DEu, kAdpcmLoopBlock = 0x1ECu;

constexpr u32 kMixCount = 18u;
constexpr u32 kMixFirst = 0x14Au, kMixFlags = 0x144u, kSyncFlags = 0x1Cu;
constexpr u32 kSrcRatioHigh = 0x1DEu, kSrcRatioLow = 0x1E0u;
constexpr u32 kSrcRatioScaleSda2 = 0xF50u;  // f32 65536.0 at r2 + 0xF50
constexpr u32 kSrcRatioMax = 0x40000u;
constexpr u16 kMixMasks[kMixCount] = {0x1,   0x9,   0x2,   0xA,   0x10,  0x50,  0x20,  0x60,  0x200,
                                      0xA00, 0x400, 0xC00, 0x1000, 0x3000, 0x4,   0xC,   0x80,  0x180};

// The SDK's float -> u32 (0x803A1DC8): the compares are fcmpu, so a NaN is neither below zero nor below 2^32.
inline u32 saturating_u32(double value) noexcept {
    if (value < 0.0) return 0u;
    if (!(value < 4294967296.0)) return 0xFFFFFFFFu;
    return static_cast<u32>(static_cast<u64>(value));
}

// The guest entries served on every call: must equal their rows of extract/native-owners.txt (native_file_api_tests).
struct Owners {
    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kSetVoiceMix, "AXSetVoiceMix", false},
                                                 {kSetVoiceSrcRatio, "AXSetVoiceSrcRatio", false},
                                                 {kSetVoiceSrcType, "AXSetVoiceSrcType", false},
                                                 {kSetVoiceState, "AXSetVoiceState", false},
                                                 {kSetVoiceAddr, "AXSetVoiceAddr", false},
                                                 {kSetVoiceAdpcm, "AXSetVoiceAdpcm", false},
                                                 {kSetVoiceSrc, "AXSetVoiceSrc", false},
                                                 {kSetVoiceAdpcmLoop, "AXSetVoiceAdpcmLoop", false},
                                                 {kFreeVoice, "AXFreeVoice", false},
                                                 {kMixInit, "MIXInit", false},
                                                 {kMixInitChannel, "MIXInitChannel", false},
                                                 {kMixUpdateSettings, "MIXUpdateSettings", false},
                                                 {kMixHookSetter, "MIX hook setter", false},
                                                 {kMixReleaseChannel, "MIXReleaseChannel", false},
                                                 {kMixSetInput, "MIXSetInput", false},
                                                 {kMixSetPan, "MIXSetPan", false},
                                                 {kAcquireVoice, "AXAcquireVoice", false},
                                                 {kAcquireSearchLoop, "AXAcquireVoice: search loop resumes", true}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

// 0x80384940 (called by AXFreeVoice and by the AXQuit chain): unlink a voice from its priority stack.
inline void unlink_from_stack(Cpu& c, u32 vpb) {
    const u32 index = c.mem_read32(vpb + kPriorityWord);
    const u32 head_at = kStackHeads + (index << 2), tail_at = kStackTails + (index << 2);
    const u32 head = c.mem_read32(head_at), tail = c.mem_read32(tail_at);
    if (head == tail) {
        c.mem_write32(tail_at, 0u);
        c.mem_write32(head_at, 0u);
    } else if (vpb == head) {
        c.mem_write32(head_at, c.mem_read32(vpb));
        c.mem_write32(c.mem_read32(head_at) + 4u, 0u);
    } else if (vpb == tail) {
        c.mem_write32(tail_at, c.mem_read32(vpb + 4u));
        c.mem_write32(c.mem_read32(tail_at), 0u);
    } else {
        const u32 prev = c.mem_read32(vpb + 4u), next = c.mem_read32(vpb);
        c.mem_write32(prev, next);
        c.mem_write32(next + 4u, prev);
    }
}

// 0x80386E44 __AXSetPBDefault: the fields a free voice carries.
inline void set_pb_default(Cpu& c, u32 vpb) {
    c.mem_write16(vpb + 0x146u, 0);
    c.mem_write16(vpb + 0x16Eu, 0);
    c.mem_write32(vpb + kSyncFlags, 0x2000A4u);
    c.mem_write32(vpb + 0x28u, 0u);
    c.mem_write32(vpb + 0x24u, 0u);
    c.mem_write32(vpb + 0x30u, vpb + 0x34u);
    for (const u32 off : {0x184u, 0x182u, 0x180u, 0x17Eu, 0x17Cu, 0x1F2u}) c.mem_write16(vpb + off, 0);
}

// 0x8038A138: the pan/aux table lookups of one MIX channel (stored as words at +0x20..), by the sound mode.
inline void mix_pan_tables(Cpu& c, u32 channel) {
    const u32 mode = c.mem_read32(c.gpr[13] + kSdaMixMode);
    const u32 pan = c.mem_read32(channel + 0x14u), aux = c.mem_read32(channel + 0x18u);
    const u32 pan_rev = 0x7Fu - pan, aux_rev = 0x7Fu - aux;
    if (mode == 3u) {
        const auto lha = [&](u32 index, u32 bias) {
            return static_cast<u32>(static_cast<s32>(static_cast<s16>(c.mem_read16(kPanTables + (index << 1) + bias))));
        };
        c.mem_write32(channel + 0x20u, lha(pan, 0x98Cu));
        c.mem_write32(channel + 0x24u, lha(pan_rev, 0x98Cu));
        c.mem_write32(channel + 0x28u, lha(aux_rev, 0x98Cu));
        c.mem_write32(channel + 0x2Cu, lha(aux, 0x98Cu));
        c.mem_write32(channel + 0x30u, lha(pan_rev, 0xA8Cu));
        c.mem_write32(channel + 0x34u, lha(pan, 0xA8Cu));
    } else {
        const auto word = [&](u32 index) { return c.mem_read32(kPanTables + (index << 2) + 0x78Cu); };
        c.mem_write32(channel + 0x20u, word(pan));
        c.mem_write32(channel + 0x24u, word(pan_rev));
        c.mem_write32(channel + 0x28u, word(aux_rev));
        c.mem_write32(channel + 0x2Cu, word(aux));
    }
}

// The attenuation table of MIXInitChannel / MIXUpdateSettings: a level in 1/10 dB units (signed) -> a 16-bit gain; at or below
// -0x388 silence, at or above +0x3C the maximum 0xFF64. The table sits at 0x80567590 + 0x710 and is indexed by the signed level.
inline u16 mix_level_gain(Cpu& c, s32 level) {
    if (level <= -0x388) return 0;
    if (level >= 0x3C) return 0xFF64u;
    return c.mem_read16(kPanTables + 0x710u + static_cast<u32>(level) * 2u);
}

// The gains of MIXInitChannel (written at +0x3C.. with delta 0) and of MIXUpdateSettings (written at +0x3E.. with delta 2, the
// target the ramp walks to), derived from the channel's levels under the sound mode and the channel flags +4 bit 0 (the f group
// is absolute) and bit 1 (the g group is absolute). Returns the dirty bit sound mode 3 adds to the mix flags (0x4000).
inline u32 mix_derive_gains(Cpu& c, u32 channel, u32 delta) {
    const u32 chan_flags = c.mem_read32(channel + 4u);
    const auto rd = [&](u32 off) { return c.mem_read32(channel + off); };
    const auto put = [&](u32 off, s32 level) { c.mem_write16(channel + off + delta, mix_level_gain(c, level)); };
    const u32 mode = c.mem_read32(c.gpr[13] + kSdaMixMode);
    const u32 a = rd(0x1C), b = rd(0x28), p = rd(0x2C), f = rd(0xC), g = rd(0x10);
    const u32 l0 = rd(0x20), l1 = rd(0x24), l2 = rd(0x30), l3 = rd(0x34);
    u32 mix_flags = 0;
    if (mode == 0u) {
        put(0x3C, static_cast<s32>(a + b));
        put(0x40, static_cast<s32>(a + b));
        put(0x44, static_cast<s32>(a + (p - 0x1Eu)));
        if ((chan_flags & 1u) != 0u) {
            put(0x48, static_cast<s32>(f + b));
            put(0x4C, static_cast<s32>(f + b));
            put(0x50, static_cast<s32>(f + (p - 0x1Eu)));
        } else {
            put(0x48, static_cast<s32>(a + (f + b)));
            put(0x4C, static_cast<s32>(a + (f + b)));
            put(0x50, static_cast<s32>(a + ((f + p) - 0x1Eu)));
        }
        if ((chan_flags & 2u) != 0u) {
            put(0x54, static_cast<s32>(g + b));
            put(0x58, static_cast<s32>(g + b));
            put(0x5C, static_cast<s32>(g + (p - 0x1Eu)));
        } else {
            put(0x54, static_cast<s32>(a + (g + b)));
            put(0x58, static_cast<s32>(a + (g + b)));
            put(0x5C, static_cast<s32>(a + ((g + p) - 0x1Eu)));
        }
    } else if (mode == 1u || mode == 2u) {
        put(0x3C, static_cast<s32>(a + (l0 + b)));
        put(0x40, static_cast<s32>(a + (l1 + b)));
        put(0x44, static_cast<s32>(a + (p - 0x1Eu)));
        if ((chan_flags & 1u) != 0u) {
            put(0x48, static_cast<s32>(f + (l0 + b)));
            put(0x4C, static_cast<s32>(f + (l1 + b)));
            put(0x50, static_cast<s32>(f + (p - 0x1Eu)));
        } else {
            put(0x48, static_cast<s32>(a + (f + l0 + b)));
            put(0x4C, static_cast<s32>(a + (f + l1 + b)));
            put(0x50, static_cast<s32>(a + ((f + p) - 0x1Eu)));
        }
        if ((chan_flags & 2u) != 0u) {
            put(0x54, static_cast<s32>(g + (l0 + b)));
            put(0x58, static_cast<s32>(g + (l1 + b)));
            put(0x5C, static_cast<s32>(g + (p - 0x1Eu)));
        } else {
            put(0x54, static_cast<s32>(a + (g + l0 + b)));
            put(0x58, static_cast<s32>(a + (g + l1 + b)));
            put(0x5C, static_cast<s32>(a + ((g + p) - 0x1Eu)));
        }
    } else if (mode == 3u) {
        put(0x3C, static_cast<s32>(a + (l0 + b)));
        put(0x40, static_cast<s32>(a + (l1 + b)));
        put(0x54, static_cast<s32>(a + (l2 + p)));
        put(0x58, static_cast<s32>(a + (l3 + p)));
        if ((chan_flags & 1u) != 0u) {
            put(0x48, static_cast<s32>(f + (l0 + b)));
            put(0x4C, static_cast<s32>(f + (l1 + b)));
            put(0x50, static_cast<s32>(f + (l2 + p)));
            put(0x5C, static_cast<s32>(f + (l3 + p)));
        } else {
            put(0x48, static_cast<s32>(a + (f + l0 + b)));
            put(0x4C, static_cast<s32>(a + (f + l1 + b)));
            put(0x50, static_cast<s32>(a + (f + l2 + p)));
            put(0x5C, static_cast<s32>(a + (f + l3 + p)));
        }
        mix_flags = 0x4000u;
    }
    return mix_flags;
}

// MIXInitChannel(vpb, mode, input, f, g, pan, aux, p): fills the channel from the arguments, derives the sixteen-bit gains
// at +0x38..+0x5C under the sound mode (0: two-channel, 1/2: with the pan tables' 0x20/0x24, 3: four-channel with 0x30/0x34)
// and commits them as the voice's mix block (the same 18-halfword block AXSetVoiceMix writes) with the dirty bits 0x212.
inline void mix_init_channel(Cpu& c) {
    const u32 vpb = c.gpr[3];
    const u32 channel = kMixChannels + c.mem_read32(vpb + kVpbIndexWord) * kMixChannelSize;
    const u32 flags_arg = c.gpr[4];
    c.mem_write32(channel, vpb);
    c.mem_write32(channel + 4u, flags_arg & 7u);
    c.mem_write32(channel + 8u, c.gpr[5]);
    c.mem_write32(channel + 0xCu, c.gpr[6]);
    c.mem_write32(channel + 0x10u, c.gpr[7]);
    c.mem_write32(channel + 0x14u, c.gpr[8]);
    c.mem_write32(channel + 0x18u, c.gpr[9]);
    c.mem_write32(channel + 0x1Cu, c.gpr[10]);
    mix_pan_tables(c, channel);
    const u32 chan_flags = c.mem_read32(channel + 4u);
    const auto rd = [&](u32 off) { return c.mem_read32(channel + off); };
    const auto put = [&](u32 off, s32 level) { c.mem_write16(channel + off, mix_level_gain(c, level)); };
    if ((chan_flags & 4u) != 0u) c.mem_write16(channel + 0x38u, 0);
    else put(0x38u, static_cast<s32>(c.gpr[5]));
    const u32 mix_extra = mix_derive_gains(c, channel, 0u);
    u32 mix_flags = mix_extra;
    // The commit: one gain per mix slot, a zero halfword in between, and the dirty mask of the non-zero ones.
    c.mem_write16(vpb + 0x19Cu, c.mem_read16(channel + 0x38u));
    c.mem_write16(vpb + 0x19Eu, 0);
    struct Slot { u32 gain_at, vpb_at, mask; };
    static constexpr Slot kSlots[] = {{0x3C, 0x14A, 1},   {0x40, 0x14E, 2},   {0x48, 0x152, 0x10}, {0x4C, 0x156, 0x20},
                                      {0x54, 0x15A, 0x200}, {0x58, 0x15E, 0x400}, {0x5C, 0x162, 0x1000}, {0x44, 0x166, 4},
                                      {0x50, 0x16A, 0x80}};
    for (const Slot& slot : kSlots) {
        const u16 gain = c.mem_read16(channel + slot.gain_at);
        if (slot.vpb_at != 0x14A) c.mem_write16(vpb + slot.vpb_at - 2u, 0);
        c.mem_write16(vpb + slot.vpb_at, gain);
        if (gain != 0) mix_flags |= slot.mask;
    }
    c.mem_write16(vpb + 0x16Cu, 0);
    c.mem_write16(vpb + kMixFlags, static_cast<u16>(mix_flags));
    c.mem_write32(vpb + kSyncFlags, c.mem_read32(vpb + kSyncFlags) | 0x212u);
}


// MIXUpdateSettings (every frame, from the GCAX frame callback): for each active channel apply the pending changes in its
// flag word (+4): 0x20000000 = the input gain's ramp target becomes current; 0x10000000 = recompute the input target; 0x80000000
// = the gain targets (+0x3E..) become current; 0x40000000 = recompute the targets under the sound mode. Then write the voice's
// ramp: for the input gain vpb+0x19C (current) / +0x19E (delta = (target - current) / 160, truncated), for the mix block the
// current gain and its delta per slot, with the dirty bits; vpb+0x1C |= 0x200 / 0x12. At the end, the stream-volume ramp: one
// step per call of the stream volume (sda+0x5DC0) towards its target (sda+0x5DC4) through the AISetStreamVolLeft / Right owners (serve_owner).
inline void mix_update_settings(Cpu& c) {
    const u32 sda = c.gpr[13];
    const auto ramp = [](u32 current, u32 target) {  // mulhw(0x66666667, d) >> 6, plus the sign bit: d / 160 toward zero
        const s64 product = static_cast<s64>(0x66666667) * static_cast<s64>(static_cast<s32>(target - current));
        s32 step = static_cast<s32>(product >> 32) >> 6;
        step += static_cast<s32>(static_cast<u32>(step) >> 31);
        return static_cast<u16>(step);
    };
    for (u32 i = 0; i < kMixChannelCount; ++i) {
        const u32 channel = kMixChannels + i * kMixChannelSize;
        const u32 vpb = c.mem_read32(channel);
        if (vpb == 0u) continue;
        bool input_changed = false, mix_changed = false;
        u32 mix_flags = 0;
        u32 flags = c.mem_read32(channel + 4u);
        if ((flags & 0x20000000u) != 0u) {
            c.mem_write16(channel + 0x38u, c.mem_read16(channel + 0x3Au));
            input_changed = true;
            c.mem_write32(channel + 4u, c.mem_read32(channel + 4u) & ~0x20000000u);
        }
        flags = c.mem_read32(channel + 4u);
        if ((flags & 0x10000000u) != 0u) {
            if ((flags & 4u) != 0u) c.mem_write16(channel + 0x3Au, 0);
            else c.mem_write16(channel + 0x3Au, mix_level_gain(c, static_cast<s32>(c.mem_read32(channel + 8u))));
            c.mem_write32(channel + 4u, c.mem_read32(channel + 4u) & ~0x10000000u);
            input_changed = true;
            c.mem_write32(channel + 4u, c.mem_read32(channel + 4u) | 0x20000000u);
        }
        if ((c.mem_read32(channel + 4u) & 0x80000000u) != 0u) {
            static constexpr u32 kTargets[] = {0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x58, 0x5C};
            for (const u32 off : kTargets) c.mem_write16(channel + off, c.mem_read16(channel + off + 2u));
            mix_changed = true;
            c.mem_write32(channel + 4u, c.mem_read32(channel + 4u) & 0x7FFFFFFFu);
        }
        if ((c.mem_read32(channel + 4u) & 0x40000000u) != 0u) {
            mix_flags = mix_derive_gains(c, channel, 2u);
            c.mem_write32(channel + 4u, c.mem_read32(channel + 4u) & ~0x40000000u);
            mix_changed = true;
            c.mem_write32(channel + 4u, c.mem_read32(channel + 4u) | 0x80000000u);
        }
        if (input_changed) {
            c.mem_write16(vpb + 0x19Cu, c.mem_read16(channel + 0x38u));
            c.mem_write16(vpb + 0x19Eu, ramp(c.mem_read16(channel + 0x38u), c.mem_read16(channel + 0x3Au)));
            c.mem_write32(vpb + kSyncFlags, c.mem_read32(vpb + kSyncFlags) | 0x200u);
        }
        if (!mix_changed) continue;
        struct Slot { u32 gain_at, vpb_at, value_mask, delta_mask; };
        static constexpr Slot kSlots[] = {{0x3C, 0x14A, 1, 8},        {0x40, 0x14E, 2, 8},        {0x48, 0x152, 0x10, 0x40},
                                          {0x4C, 0x156, 0x20, 0x40},  {0x54, 0x15A, 0x200, 0x800}, {0x58, 0x15E, 0x400, 0x800},
                                          {0x5C, 0x162, 0x1000, 0x2000}, {0x44, 0x166, 4, 8},      {0x50, 0x16A, 0x80, 0x100}};
        for (const Slot& slot : kSlots) {
            const u16 current = c.mem_read16(channel + slot.gain_at);
            const u16 delta = ramp(current, c.mem_read16(channel + slot.gain_at + 2u));
            c.mem_write16(vpb + slot.vpb_at, current);
            if (current != 0) mix_flags |= slot.value_mask;
            c.mem_write16(vpb + slot.vpb_at + 2u, delta);
            if (delta != 0) mix_flags |= slot.delta_mask;
        }
        c.mem_write16(vpb + kMixFlags, static_cast<u16>(mix_flags));
        c.mem_write32(vpb + kSyncFlags, c.mem_read32(vpb + kSyncFlags) | 0x12u);
    }
    const s32 target = static_cast<s32>(c.mem_read32(sda + kSdaMixState1));
    s32 current = static_cast<s32>(c.mem_read32(sda + kSdaMixState0));
    if (target != current) {
        current += target > current ? 1 : -1;
        c.mem_write32(sda + kSdaMixState0, static_cast<u32>(current));
        const u32 volume = c.mem_read8(kPanTables + 0xB8Cu + static_cast<u32>(current));
        // The two audio.ai owners (NativeAudio), served natively: not a call into SDK code. Without them installed (the contract
        // test) the original's MMIO writes fall outside the compared memory.
        c.gpr[3] = volume;
        c.serve_owner(kAiSetStreamVolLeft);
        c.gpr[3] = volume;
        c.serve_owner(kAiSetStreamVolRight);
    }
}

inline void install(Cpu& cpu) {
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, std::string("AX voice: ") + name);
    };
    own(kSetVoiceMix, "AXSetVoiceMix", [](Cpu& c) {
        const u32 vpb = c.gpr[3], mix = c.gpr[4];
        u32 flags = 0;
        for (u32 i = 0; i < kMixCount; ++i) {
            const u16 value = c.mem_read16(mix + 2u * i);
            c.mem_write16(vpb + kMixFirst + 2u * i, value);
            if (value != 0) flags |= kMixMasks[i];
        }
        c.mem_write16(vpb + kMixFlags, static_cast<u16>(flags));
        c.mem_write32(vpb + kSyncFlags, c.mem_read32(vpb + kSyncFlags) | 0x12u);
    });
    own(kSetVoiceSrcRatio, "AXSetVoiceSrcRatio", [](Cpu& c) {
        const u32 vpb = c.gpr[3];
        const u32 scale_bits = c.mem_read32(c.gpr[2] + kSrcRatioScaleSda2);
        float scale;
        std::memcpy(&scale, &scale_bits, sizeof scale);
        double ratio;
        std::memcpy(&ratio, &c.fpr[1].bits[0], sizeof ratio);
        const float scaled = static_cast<float>(static_cast<double>(scale) * ratio);  // fmuls
        u32 value = saturating_u32(static_cast<double>(scaled));
        if (value > kSrcRatioMax) value = kSrcRatioMax;
        c.mem_write16(vpb + kSrcRatioHigh, static_cast<u16>(value >> 16));
        c.mem_write16(vpb + kSrcRatioLow, static_cast<u16>(value));
        c.mem_write32(vpb + kSyncFlags, c.mem_read32(vpb + kSyncFlags) | 0x80000u);
    });
    const auto sync = [](Cpu& c, u32 vpb, u32 clear, u32 set) {
        c.mem_write32(vpb + kSyncFlags, (c.mem_read32(vpb + kSyncFlags) & ~clear) | set);
    };
    own(kSetVoiceSrcType, "AXSetVoiceSrcType", [sync](Cpu& c) {
        const u32 vpb = c.gpr[3], pb = vpb + kEmbeddedPb;
        switch (static_cast<s32>(c.gpr[4])) {  // signed compares; other values change nothing
            case 0: c.mem_write16(pb + 8, 2); break;
            case 1: c.mem_write16(pb + 8, 1); break;
            case 2: c.mem_write16(pb + 8, 0); c.mem_write16(pb + 0xA, 0); break;
            case 3: c.mem_write16(pb + 8, 0); c.mem_write16(pb + 0xA, 1); break;
            case 4: c.mem_write16(pb + 8, 0); c.mem_write16(pb + 0xA, 2); break;
            default: break;
        }
        sync(c, vpb, 0, 1u);
    });
    own(kSetVoiceState, "AXSetVoiceState", [sync](Cpu& c) {
        const u32 vpb = c.gpr[3];
        const u16 state = static_cast<u16>(c.gpr[4]);
        c.mem_write16(vpb + kStateHalfword, state);
        sync(c, vpb, 0, 4u);
        if (state == 0) c.mem_write32(vpb + kDepopWord, 1u);
    });
    own(kSetVoiceAddr, "AXSetVoiceAddr", [sync](Cpu& c) {
        const u32 vpb = c.gpr[3], src = c.gpr[4], dst = vpb + kAddrBlock;
        for (u32 i = 0; i < 16; i += 4) c.mem_write32(dst + i, c.mem_read32(src + i));
        const u16 loop = c.mem_read16(src + 2);
        if (loop == 0x0A || loop == 0x19) {  // the two modes that carry a fixed loop/end block
            for (u32 off = 0x10; off <= 0x34; off += 4) c.mem_write32(dst + off, 0u);
            c.mem_write32(dst + 0x30, loop == 0x0A ? 0x08000000u : 0x01000000u);
        }
        sync(c, vpb, 0x1E000u, 0x21000u);
    });
    own(kSetVoiceAdpcm, "AXSetVoiceAdpcm", [sync](Cpu& c) {
        const u32 vpb = c.gpr[3], src = c.gpr[4];
        for (u32 i = 0; i < 40; i += 4) c.mem_write32(vpb + kAdpcmBlock + i, c.mem_read32(src + i));
        sync(c, vpb, 0, 0x20000u);
    });
    own(kSetVoiceSrc, "AXSetVoiceSrc", [sync](Cpu& c) {
        const u32 vpb = c.gpr[3], src = c.gpr[4];
        for (u32 i = 0; i < 14; i += 2) c.mem_write16(vpb + kSrcBlock + i, c.mem_read16(src + i));
        sync(c, vpb, 0x80000u, 0x40000u);
    });
    own(kFreeVoice, "AXFreeVoice", [](Cpu& c) {
        const u32 vpb = c.gpr[3];
        unlink_from_stack(c, vpb);
        if (c.mem_read16(vpb + kStateHalfword) == 1) c.mem_write32(vpb + kDepopWord, 1u);
        set_pb_default(c, vpb);
        c.mem_write32(vpb, c.mem_read32(kStackHeads));  // push on the free stack (priority 0)
        c.mem_write32(kStackHeads, vpb);
        c.mem_write32(vpb + kPriorityWord, 0u);
    });
    // AXAcquireVoice(priority, dropCallback, context) -> AXVPB*: the free stack first; else the lowest-priority voice of a
    // priority below the request (its tail, or the sole element), told through its drop callback with interrupts off.
    own(kAcquireVoice, "AXAcquireVoice", [](Cpu& c) {
        const u32 priority = c.gpr[3], callback = c.gpr[4], context = c.gpr[5];
        u32 voice = c.mem_read32(kStackHeads);
        if (voice != 0) c.mem_write32(kStackHeads, c.mem_read32(voice));
        if (voice == 0 && priority > 1) {
            for (u32 p = 1; p < priority; ++p) {
                const u32 head_at = kStackHeads + (p << 2), tail_at = kStackTails + (p << 2);
                const u32 head = c.mem_read32(head_at);
                u32 taken = 0;
                if (head != 0) {
                    const u32 tail = c.mem_read32(tail_at);
                    if (head == tail) {
                        c.mem_write32(tail_at, 0u);
                        taken = head;
                        c.mem_write32(head_at, 0u);
                    } else if (tail != 0) {
                        taken = tail;
                        c.mem_write32(tail_at, c.mem_read32(tail + 4u));
                        c.mem_write32(c.mem_read32(tail_at), 0u);
                    }
                }
                if (taken == 0) continue;
                voice = taken;
                if (c.mem_read16(voice + kStateHalfword) == 1) c.mem_write32(voice + kDepopWord, 1u);
                const u32 drop = c.mem_read32(voice + 0x10u);
                if (drop != 0) {
                    const u32 saved_msr = c.msr();
                    c.set_msr(saved_msr & ~kMsrEE);  // the original runs it between OSDisableInterrupts and the restore
                    u32 result = 0;
                    const bool ran = guest_call(c, drop, kAcquireVoice, {voice}, result);
                    c.set_msr((c.msr() & ~kMsrEE) | (saved_msr & kMsrEE));
                    if (!ran) return;  // the run is stopped; nothing resumes halfway
                }
                break;
            }
        }
        if (voice != 0) {
            const u32 head_at = kStackHeads + (priority << 2);
            const u32 old_head = c.mem_read32(head_at);
            c.mem_write32(voice, old_head);
            c.mem_write32(voice + 4u, 0u);
            if (c.mem_read32(voice) != 0) {
                c.mem_write32(c.mem_read32(head_at) + 4u, voice);
                c.mem_write32(head_at, voice);
            } else {
                c.mem_write32(kStackTails + (priority << 2), voice);
                c.mem_write32(head_at, voice);
            }
            c.mem_write32(voice + kPriorityWord, priority);
            c.mem_write32(voice + 0x10u, callback);
            c.mem_write32(voice + 0x14u, context);
            set_pb_default(c, voice);
        }
        c.gpr[3] = voice;
    });
    // The generator keeps the search loop's tail (0x80384B4C bdnz) as a row of its own that jumps to the loop head, so the
    // head must be a declared owner entry. The owner runs the search atomically, so no state stands inside it (a saved
    // state cannot hold one); an entry here can only come from a state of a build that translated AXAcquireVoice, and
    // stopping is the loud answer (the registers the loop needs are not reconstructed).
    own(kAcquireSearchLoop, "AXAcquireVoice: search loop resumes", [](Cpu& c) {
        c.request_stop(StopReason::ExecutionFault, "native AX voice: AXAcquireVoice search loop entered mid-search");
    });
    const auto channel_of = [](Cpu& c) {
        return kMixChannels + c.mem_read32(c.gpr[3] + kVpbIndexWord) * kMixChannelSize;
    };
    own(kMixInit, "MIXInit", [](Cpu& c) {
        const u32 sda = c.gpr[13];
        for (u32 i = 0; i < kMixChannelCount; ++i) {
            const u32 channel = kMixChannels + i * kMixChannelSize;
            c.mem_write32(channel + 4u, 0x50000000u);
            c.mem_write32(channel + 8u, 0u);
            c.mem_write32(channel + 0xCu, 0xFFFFFC40u);  // -0x3C0
            c.mem_write32(channel + 0x10u, 0xFFFFFC40u);
            c.mem_write32(channel + 0x1Cu, 0u);
            c.mem_write32(channel + 0x14u, 0x40u);
            c.mem_write32(channel + 0x18u, 0x7Fu);
            for (u32 off = 0x38u; off <= 0x5Cu; off += 4u) c.mem_write16(channel + off, 0);
            mix_pan_tables(c, channel);  // with the sound mode as it stands: MIXInit sets it to 1 only afterwards
        }
        c.mem_write32(sda + kSdaMixState0, 0u);
        c.mem_write32(sda + kSdaMixState1, 0u);
        c.mem_write32(sda + kSdaMixMode, 1u);
    });
    own(kMixHookSetter, "MIX hook setter", [](Cpu& c) {
        c.mem_write32(c.gpr[13] + kSdaMixHookA, c.gpr[3]);
        c.mem_write32(c.gpr[13] + kSdaMixHookB, c.gpr[4]);
    });
    own(kMixUpdateSettings, "MIXUpdateSettings", [](Cpu& c) { mix_update_settings(c); });
    own(kMixInitChannel, "MIXInitChannel", [](Cpu& c) { mix_init_channel(c); });
    own(kMixReleaseChannel, "MIXReleaseChannel", [channel_of](Cpu& c) { c.mem_write32(channel_of(c), 0u); });
    own(kMixSetInput, "MIXSetInput", [channel_of](Cpu& c) {
        const u32 channel = channel_of(c);
        c.mem_write32(channel + 8u, c.gpr[4]);
        c.mem_write32(channel + 4u, c.mem_read32(channel + 4u) | 0x10000000u);
    });
    own(kMixSetPan, "MIXSetPan", [channel_of](Cpu& c) {
        const u32 channel = channel_of(c);
        const s32 pan = static_cast<s32>(c.gpr[4]);
        c.mem_write32(channel + 0x14u, pan < 0 ? 0u : pan > 0x7F ? 0x7Fu : static_cast<u32>(pan));
        mix_pan_tables(c, channel);
        c.mem_write32(channel + 4u, c.mem_read32(channel + 4u) | 0x40000000u);
    });
    own(kSetVoiceAdpcmLoop, "AXSetVoiceAdpcmLoop", [sync](Cpu& c) {
        const u32 vpb = c.gpr[3], src = c.gpr[4];
        for (u32 i = 0; i < 6; i += 2) c.mem_write16(vpb + kAdpcmLoopBlock + i, c.mem_read16(src + i));
        sync(c, vpb, 0, 0x100000u);
    });
}

}  // namespace gekko::ax_voice
