// Original-DOL contract for the AX voice setter owners (gekko/native_ax_voice.h, audio-ax stage c):
// AXSetVoiceMix 0x803871B0, AXSetVoiceSrcRatio 0x80387650 and the six block/field setters
// (SrcType, State, Addr, Adpcm, Src, AdpcmLoop). The original side is the pinned DOL's instructions in
// the reference interpreter (including its OSDisableInterrupts / OSRestoreInterrupts bracket and the SDK's float
// conversion); the owner side is the installed hook on a second machine. Compared after each call: the machine state
// register, f31 (the setter saves and restores it), the stack pointer and every word of guest memory
// (0x80000000..0x80600000), for random AXVPB images, interrupts on and off.
// liveness-roots: reference-interpreter-only
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_ax_voice.h"

#include <array>
#include <cmath>
#include <limits>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
using namespace gekko;
u32 g_extra[5] = {0, 0, 0, 0, 0};  // r6..r10 of the next call (MIXInitChannel)
unsigned g_acquired = 0, g_acquire_null = 0, g_callbacks = 0;
constexpr u32 kStack = 0x817E0000u, kReturn = 0x817FF000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kVpbBase = 0x805989A0u, kVpbStride = 0x22Cu, kMixArea = 0x805FD000u, kDropStub = 0x817F0100u, kSeenWord = 0x805ED100u, kMsrEE = 0x8000u, kMsrFP = 0x2000u;
constexpr u32 kLow = 0x80000000u, kHigh = 0x80600000u;

void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    u32 base_msr = 0;
    explicit Machine(const char* path) {
        std::ifstream input(path, std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        cpu.reset();
        base_msr = cpu.msr();
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
    }
    std::vector<u8> snapshot() {
        std::vector<u8> v(kHigh - kLow);
        memory.read_block(kLow, v.data(), v.size());
        return v;
    }
    void sda(u32 offset, u32 value) { memory.write32(kR13 + offset, value); }
    void original(u32 entry, u32 r3, u32 r4, double f1 = 0.0, u32 r5 = 0) {
        const u32 msr = cpu.msr();
        cpu.reset();
        cpu.set_msr(msr);
        std::memcpy(&cpu.fpr[1].bits[0], &f1, 8);
        cpu.pc = entry;
        cpu.npc = entry + 4;
        cpu.lr = kReturn;
        cpu.gpr[1] = kStack;
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.gpr[3] = r3;
        cpu.gpr[4] = r4;
        cpu.gpr[5] = r5;
        for (unsigned i = 0; i < 5; ++i) cpu.gpr[6 + i] = g_extra[i];
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 20000) cpu.run_steps(1);
        if (!(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack)) std::fprintf(stderr, "entry %08X pc %08X steps %u stopped %d r1 %08X msr %08X\n", entry, cpu.pc, steps, int(cpu.stopped()), cpu.gpr[1], cpu.msr());
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack, "original call did not return cleanly");
    }
};

struct Owner {
    Machine m;
    explicit Owner(const char* path) : m(path) { ax_voice::install(m.cpu); }
    void call(u32 entry, u32 r3, u32 r4) {
        const u32 msr = m.cpu.msr();
        m.cpu.reset();
        m.cpu.set_msr(msr);
        m.cpu.gpr[2] = kR2;
        m.cpu.gpr[13] = kR13;
        m.cpu.gpr[3] = r3;
        m.cpu.gpr[4] = r4;
        require(m.cpu.try_hle(entry), "the owner declined the call");
    }
};

struct Rng {
    u64 s = 0x9E3779B97F4A7C15ull;
    u32 next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<u32>(s >> 33);
    }
};

void fill_both(Machine& a, Machine& b, u32 address, const std::vector<u8>& bytes) {
    a.memory.write_block(address, bytes.data(), bytes.size());
    b.memory.write_block(address, bytes.data(), bytes.size());
}

unsigned compare(Machine& a, Owner& b, u32 entry, u32 vpb, u32 r4, double f1, bool interrupts, u32 r5 = 0) {
    const u32 msr = (a.base_msr & ~kMsrEE) | (interrupts ? kMsrEE : 0u);
    a.cpu.set_msr(msr);
    b.m.cpu.set_msr(msr);
    a.original(entry, vpb, r4, f1, r5);
    // the owner machine resets its register file too, then gets the same f1
    b.m.cpu.reset();
    b.m.cpu.set_msr(msr);
    std::memcpy(&b.m.cpu.fpr[1].bits[0], &f1, 8);
    b.m.cpu.gpr[2] = kR2;
    b.m.cpu.gpr[13] = kR13;
    b.m.cpu.gpr[3] = vpb;
    b.m.cpu.gpr[4] = r4;
    b.m.cpu.gpr[5] = r5;
    for (unsigned i = 0; i < 5; ++i) b.m.cpu.gpr[6 + i] = g_extra[i];
    b.m.cpu.gpr[1] = kStack;
    require(b.m.cpu.try_hle(entry), "the owner declined the call");
    require(a.cpu.msr() == b.m.cpu.msr(), "the machine state register differs");
    require(a.cpu.fpr[31].bits[0] == b.m.cpu.fpr[31].bits[0], "f31 differs");
    if (entry == ax_voice::kAcquireVoice) {
        require(a.cpu.gpr[3] == b.m.cpu.gpr[3], "AXAcquireVoice returns a different voice");
        ++g_acquired;
        if (a.cpu.gpr[3] == 0) ++g_acquire_null;
        if (a.memory.read32(kSeenWord) != 0) ++g_callbacks;
        require(a.memory.read32(kSeenWord) == b.m.memory.read32(kSeenWord), "the drop callback saw a different voice");
    }
    const std::vector<u8> sa = a.snapshot(), sb = b.m.snapshot();
    if (sa != sb) {
        unsigned shown = 0;
        for (std::size_t i = 0; i < sa.size() && shown < 6; ++i)
            if (sa[i] != sb[i]) {
                std::fprintf(stderr, "  entry %08X vpb %08X: byte %08X original %02X owner %02X\n", entry, vpb,
                             unsigned(kLow + i), sa[i], sb[i]);
                ++shown;
            }
    }
    require(sa == sb, "guest memory differs");
    return 1;
}

void run(const char* dol) {
    Machine original(dol);
    Owner owner(dol);
    Rng rng;
    unsigned cases = 0;
    const std::array<double, 24> ratios{0.0, -0.0, 1.0, 0.5, 2.0, 3.99999, 4.0, 4.0000001, 4.5, 1e9, -1.0, 1e-30, 0.1, 0.000015258789,
                                        65535.9, 16.0, 17.25, 1e300, -1e300, 0.99999,
                                        std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                                        std::numeric_limits<double>::quiet_NaN(), 65536.0};
    for (u32 round = 0; round < 40; ++round) {
        const u32 vpb = kVpbBase + kVpbStride * (rng.next() % 64u);
        std::vector<u8> image(kVpbStride), mix(64);
        for (u8& byte : image) byte = static_cast<u8>(rng.next());
        for (u32 i = 0; i < 18; ++i) {
            const u32 pick = rng.next() % 3u;  // zero, small, full-range
            const u32 value = pick == 0 ? 0u : pick == 1 ? 1u + rng.next() % 4u : rng.next() & 0xFFFFu;
            mix[2 * i] = static_cast<u8>(value >> 8);
            mix[2 * i + 1] = static_cast<u8>(value);
        }
        fill_both(original, owner.m, vpb, image);
        fill_both(original, owner.m, kMixArea, mix);
        const bool interrupts = (round & 1u) != 0u;
        cases += compare(original, owner, ax_voice::kSetVoiceMix, vpb, kMixArea, 0.0, interrupts);
        for (const double ratio : ratios) {
            fill_both(original, owner.m, vpb, image);
            cases += compare(original, owner, ax_voice::kSetVoiceSrcRatio, vpb, 0u, ratio, interrupts);
        }
    }
    // The block setters: random AXVPB image (random dirty word too) and random argument bytes; the Addr loop modes
    // 0xA / 0x19 are forced in half the rounds because a random halfword would almost never hit them.
    for (u32 round = 0; round < 60; ++round) {
        const u32 vpb = kVpbBase + kVpbStride * (rng.next() % 64u);
        std::vector<u8> image(kVpbStride), args(64);
        for (u8& byte : image) byte = static_cast<u8>(rng.next());
        for (u8& byte : args) byte = static_cast<u8>(rng.next());
        const bool interrupts = (round & 1u) != 0u;
        const u32 modes[] = {0x0Au, 0x19u, 0u, 0x0Bu};
        if (round & 2u) {
            const u32 mode = modes[(round >> 2) & 3u];
            args[2] = static_cast<u8>(mode >> 8);
            args[3] = static_cast<u8>(mode);
        }
        fill_both(original, owner.m, kMixArea, args);
        for (const u32 entry : {ax_voice::kSetVoiceAddr, ax_voice::kSetVoiceAdpcm, ax_voice::kSetVoiceSrc,
                                ax_voice::kSetVoiceAdpcmLoop}) {
            fill_both(original, owner.m, vpb, image);
            cases += compare(original, owner, entry, vpb, kMixArea, 0.0, interrupts);
        }
        for (const u32 state : {0u, 1u, 0x10000u, 0xFFFF0000u, 0xFFFFu, rng.next()}) {
            fill_both(original, owner.m, vpb, image);
            cases += compare(original, owner, ax_voice::kSetVoiceState, vpb, state, 0.0, interrupts);
        }
        for (s32 type = -3; type <= 7; ++type) {
            fill_both(original, owner.m, vpb, image);
            cases += compare(original, owner, ax_voice::kSetVoiceSrcType, vpb, static_cast<u32>(type), 0.0, interrupts);
        }
    }
    // AXFreeVoice over random priority stacks: 64 voices spread over priorities 0..31, each stack a consistent doubly
    // linked list (head/tail tables and next/prev words), then one voice is freed - head, tail, middle, sole element,
    // free-stack member, and a voice in state 1 (the depop word).
    for (u32 round = 0; round < 120; ++round) {
        std::vector<u8> image(kVpbStride * 64);
        for (u8& byte : image) byte = static_cast<u8>(rng.next());
        std::array<std::vector<u32>, 32> stacks;
        const u32 spread = 1u + rng.next() % 32u;  // few priorities -> long stacks (middle cases), many -> sole elements
        for (u32 v = 0; v < 64; ++v) stacks[rng.next() % spread].push_back(kVpbBase + kVpbStride * v);
        std::vector<u8> heads(0x80, 0), tails(0x80, 0);
        const auto put32 = [&](std::vector<u8>& buf, u32 off, u32 value) {
            buf[off] = static_cast<u8>(value >> 24); buf[off + 1] = static_cast<u8>(value >> 16);
            buf[off + 2] = static_cast<u8>(value >> 8); buf[off + 3] = static_cast<u8>(value);
        };
        for (u32 p = 0; p < 32; ++p) {
            const auto& list = stacks[p];
            for (std::size_t i = 0; i < list.size(); ++i) {
                const u32 off = list[i] - kVpbBase;
                put32(image, off + 0, i + 1 < list.size() ? list[i + 1] : 0u);  // next
                put32(image, off + 4, i > 0 ? list[i - 1] : 0u);                // prev
                put32(image, off + 0xC, p);
            }
            if (!list.empty()) {
                put32(heads, p * 4, list.front());
                put32(tails, p * 4, list.back());
            }
        }
        // choose the victim: a random member of a random non-empty stack
        u32 p = rng.next() % 32u;
        while (stacks[p].empty()) p = (p + 1u) % 32u;
        const u32 victim = stacks[p][rng.next() % stacks[p].size()];
        if (round & 1u) {  // state 1 -> depop word
            image[victim - kVpbBase + 0x146] = 0;
            image[victim - kVpbBase + 0x147] = 1;
        }
        fill_both(original, owner.m, kVpbBase, image);
        fill_both(original, owner.m, ax_voice::kStackHeads, heads);
        fill_both(original, owner.m, ax_voice::kStackTails, tails);
        cases += compare(original, owner, ax_voice::kFreeVoice, victim, 0u, 0.0, (round & 2u) != 0u);
    }
    // AXAcquireVoice: free stack empty or not, requests of priority 0..33, drop callbacks (none / a stub that records its
    // argument) and states 1/0 on the stolen voice. The stub runs in the original's interpreter and in the owner's
    // guest_call; both store r3 at kCallbackSeen, which the memory comparison covers.
    const std::vector<u8> stub_code{0x3C, 0x80, 0x80, 0x5F,   // lis r4, 0x805F
                                    0x90, 0x64, 0xD1, 0x00,   // stw r3, -0x2F00(r4)  -> 0x805ED100 records the argument
                                    0x4E, 0x80, 0x00, 0x20};  // blr
    for (u32 round = 0; round < 240; ++round) {
        std::vector<u8> image(kVpbStride * 64);
        for (u8& byte : image) byte = static_cast<u8>(rng.next());
        std::array<std::vector<u32>, 32> stacks;
        const u32 spread = 1u + rng.next() % 32u;
        for (u32 v = 0; v < 64; ++v) {
            u32 p = rng.next() % spread;
            if (p == 0 && (round & 4u) != 0u) p = 1u + rng.next() % 31u;  // sometimes an empty free stack
            stacks[p].push_back(kVpbBase + kVpbStride * v);
        }
        std::vector<u8> heads(0x80, 0), tails(0x80, 0);
        const auto put32 = [&](std::vector<u8>& buf, u32 off, u32 value) {
            buf[off] = static_cast<u8>(value >> 24); buf[off + 1] = static_cast<u8>(value >> 16);
            buf[off + 2] = static_cast<u8>(value >> 8); buf[off + 3] = static_cast<u8>(value);
        };
        for (u32 p = 0; p < 32; ++p) {
            const auto& list = stacks[p];
            for (std::size_t i = 0; i < list.size(); ++i) {
                const u32 off = list[i] - kVpbBase;
                put32(image, off + 0, i + 1 < list.size() ? list[i + 1] : 0u);
                put32(image, off + 4, i > 0 ? list[i - 1] : 0u);
                put32(image, off + 0xC, p);
                const u32 pick = rng.next() % 3u;  // no callback / the stub / the stub again with state 1
                put32(image, off + 0x10, pick == 0 ? 0u : kDropStub);
                image[off + 0x146] = 0;
                image[off + 0x147] = pick == 2 ? 1 : static_cast<u8>(rng.next() % 3u);
            }
            if (!list.empty()) {
                put32(heads, p * 4, list.front());
                put32(tails, p * 4, list.back());
            }
        }
        fill_both(original, owner.m, kVpbBase, image);
        fill_both(original, owner.m, ax_voice::kStackHeads, heads);
        fill_both(original, owner.m, ax_voice::kStackTails, tails);
        fill_both(original, owner.m, kDropStub, stub_code);
        fill_both(original, owner.m, kSeenWord, std::vector<u8>(4, 0));
        const u32 priority = rng.next() % 34u;
        const u32 callback = (round & 1u) ? 0x8043D8C0u : 0u, context = rng.next();
        cases += compare(original, owner, ax_voice::kAcquireVoice, priority, callback, 0.0, (round & 2u) != 0u, context);
    }
    // MIX channel entries: random channel table, AXVPB index 0..63, pan any signed value (clamped by the owner), aux level
    // 0..127 (the table index), sound mode 3 and others.
    for (u32 round = 0; round < 200; ++round) {
        std::vector<u8> chans(64 * ax_voice::kMixChannelSize), vpbs(kVpbStride);
        for (u8& byte : chans) byte = static_cast<u8>(rng.next());
        for (u8& byte : vpbs) byte = static_cast<u8>(rng.next());
        const u32 index = rng.next() % 64u, vpb = kVpbBase + kVpbStride * (rng.next() % 64u);
        const auto put = [&](std::vector<u8>& buf, u32 off, u32 value) {
            buf[off] = static_cast<u8>(value >> 24); buf[off + 1] = static_cast<u8>(value >> 16);
            buf[off + 2] = static_cast<u8>(value >> 8); buf[off + 3] = static_cast<u8>(value);
        };
        put(vpbs, 0x18, index);
        put(chans, index * ax_voice::kMixChannelSize + 0x14, rng.next() % 128u);
        put(chans, index * ax_voice::kMixChannelSize + 0x18, rng.next() % 128u);
        const u32 mode = (round % 4u == 0u) ? 3u : (round % 4u);
        const s32 pans[] = {-5, 0, 1, 63, 127, 128, 1000, static_cast<s32>(rng.next() % 200u) - 40};
        for (const s32 pan : pans) {
            fill_both(original, owner.m, vpb, vpbs);
            fill_both(original, owner.m, ax_voice::kMixChannels, chans);
            original.sda(ax_voice::kSdaMixMode, mode);
            owner.m.sda(ax_voice::kSdaMixMode, mode);
            cases += compare(original, owner, ax_voice::kMixSetPan, vpb, static_cast<u32>(pan), 0.0, (round & 1u) != 0u);
        }
        fill_both(original, owner.m, vpb, vpbs);
        fill_both(original, owner.m, ax_voice::kMixChannels, chans);
        cases += compare(original, owner, ax_voice::kMixSetInput, vpb, rng.next(), 0.0, false);
        fill_both(original, owner.m, ax_voice::kMixChannels, chans);
        cases += compare(original, owner, ax_voice::kMixReleaseChannel, vpb, 0u, 0.0, false);
    }
    // MIXInit (all 64 channels, the pan tables under the sound mode that stands) and the hook setter.
    for (u32 round = 0; round < 12; ++round) {
        std::vector<u8> chans(64 * ax_voice::kMixChannelSize);
        for (u8& byte : chans) byte = static_cast<u8>(rng.next());
        for (const u32 entry : {ax_voice::kMixInit, ax_voice::kMixHookSetter}) {
            fill_both(original, owner.m, ax_voice::kMixChannels, chans);
            const u32 mode = round % 5u;
            original.sda(ax_voice::kSdaMixMode, mode);
            owner.m.sda(ax_voice::kSdaMixMode, mode);
            for (const u32 off : {ax_voice::kSdaMixState0, ax_voice::kSdaMixState1, ax_voice::kSdaMixHookA, ax_voice::kSdaMixHookB}) {
                original.sda(off, 0xA5A50000u + off);
                owner.m.sda(off, 0xA5A50000u + off);
            }
            cases += compare(original, owner, entry, rng.next(), rng.next(), 0.0, (round & 1u) != 0u);
        }
    }
    // MIXInitChannel: random arguments (levels spanning the table's clamps), the flag bits 0..2, every sound mode (-1..4),
    // pan/aux 0 or 127 and in between, random previous channel and AXVPB contents.
    {
        unsigned init_cases = 0;
        for (u32 round = 0; round < 600; ++round) {
            std::vector<u8> chans(64 * ax_voice::kMixChannelSize), vpbs(kVpbStride);
            for (u8& byte : chans) byte = static_cast<u8>(rng.next());
            for (u8& byte : vpbs) byte = static_cast<u8>(rng.next());
            const u32 index = rng.next() % 64u, vpb = kVpbBase + kVpbStride * (rng.next() % 64u);
            const auto put = [&](std::vector<u8>& buf, u32 off, u32 value) {
                buf[off] = static_cast<u8>(value >> 24); buf[off + 1] = static_cast<u8>(value >> 16);
                buf[off + 2] = static_cast<u8>(value >> 8); buf[off + 3] = static_cast<u8>(value);
            };
            put(vpbs, 0x18, index);
            const auto level = [&]() -> u32 {
                switch (rng.next() % 6u) {
                    case 0: return static_cast<u32>(-0x388 - static_cast<s32>(rng.next() % 3u));
                    case 1: return 0x3Cu + rng.next() % 3u;
                    case 2: return static_cast<u32>(-static_cast<s32>(rng.next() % 0x388u));
                    case 3: return rng.next() % 0x3Cu;
                    case 4: return 0u;
                    default: return static_cast<u32>(static_cast<s32>(rng.next() % 2000u) - 1000);
                }
            };
            const u32 pan = rng.next() % 128u, aux = (round & 8u) ? (round & 16u ? 0u : 127u) : rng.next() % 128u;
            const u32 mode = static_cast<u32>(static_cast<s32>(round % 6u) - 1);
            fill_both(original, owner.m, vpb, vpbs);
            fill_both(original, owner.m, ax_voice::kMixChannels, chans);
            original.sda(ax_voice::kSdaMixMode, mode);
            owner.m.sda(ax_voice::kSdaMixMode, mode);
            // arguments r3 = vpb, r4 = flags, r5 = input, r6/r7 = f/g, r8 = pan, r9 = aux, r10 = p
            g_extra[0] = level(); g_extra[1] = level(); g_extra[2] = pan; g_extra[3] = aux; g_extra[4] = level();
            init_cases += compare(original, owner, ax_voice::kMixInitChannel, vpb, rng.next(), 0.0, (round & 1u) != 0u, level());
            for (u32& r : g_extra) r = 0;
        }
        cases += init_cases;
    }
    // MIXUpdateSettings: random channel table where some channels are free (vpb 0) and the pending-change flag bits are
    // random, the sound modes -1..4, random stream-volume words; the AI stream-volume functions run in the interpreter
    // on both machines (no owner for them is installed here), so the register writes cancel out.
    {
        unsigned update_cases = 0;
        for (u32 round = 0; round < 150; ++round) {
            std::vector<u8> chans(64 * ax_voice::kMixChannelSize);
            for (u8& byte : chans) byte = static_cast<u8>(rng.next());
            const auto put = [&](std::vector<u8>& buf, u32 off, u32 value) {
                buf[off] = static_cast<u8>(value >> 24); buf[off + 1] = static_cast<u8>(value >> 16);
                buf[off + 2] = static_cast<u8>(value >> 8); buf[off + 3] = static_cast<u8>(value);
            };
            const auto level = [&]() -> u32 {
                switch (rng.next() % 5u) {
                    case 0: return static_cast<u32>(-0x388 - static_cast<s32>(rng.next() % 3u));
                    case 1: return 0x3Cu + rng.next() % 3u;
                    case 2: return static_cast<u32>(-static_cast<s32>(rng.next() % 0x388u));
                    case 3: return rng.next() % 0x3Cu;
                    default: return static_cast<u32>(static_cast<s32>(rng.next() % 2000u) - 1000);
                }
            };
            for (u32 ch = 0; ch < 64; ++ch) {
                const u32 base = ch * ax_voice::kMixChannelSize;
                put(chans, base, (rng.next() % 4u == 0u) ? 0u : kVpbBase + kVpbStride * (rng.next() % 64u));
                put(chans, base + 4, rng.next());
                for (const u32 off : {8u, 0xCu, 0x10u, 0x1Cu, 0x28u, 0x2Cu, 0x20u, 0x24u, 0x30u, 0x34u}) put(chans, base + off, level());
                put(chans, base + 0x14, rng.next() % 128u);
                put(chans, base + 0x18, rng.next() % 128u);
            }
            std::vector<u8> vpbs(kVpbStride * 64);
            for (u8& byte : vpbs) byte = static_cast<u8>(rng.next());
            fill_both(original, owner.m, kVpbBase, vpbs);
            fill_both(original, owner.m, ax_voice::kMixChannels, chans);
            const u32 mode = static_cast<u32>(static_cast<s32>(round % 6u) - 1);
            const u32 volume_now = static_cast<u32>(static_cast<s32>(rng.next() % 5u) - 2);
            const u32 volume_target = static_cast<u32>(static_cast<s32>(rng.next() % 5u) - 2);
            for (Machine* m : {&original, &owner.m}) {
                m->sda(ax_voice::kSdaMixMode, mode);
                m->sda(ax_voice::kSdaMixState0, volume_now);
                m->sda(ax_voice::kSdaMixState1, volume_target);
            }
            update_cases += compare(original, owner, ax_voice::kMixUpdateSettings, 0u, 0u, 0.0, (round & 1u) != 0u);
        }
        cases += update_cases;
    }
    // all-zero and all-ones mix halfwords: the flag word covers every mask
    for (const u8 fill : {u8{0}, u8{0xFF}}) {
        const u32 vpb = kVpbBase;
        fill_both(original, owner.m, vpb, std::vector<u8>(kVpbStride, 0x5A));
        fill_both(original, owner.m, kMixArea, std::vector<u8>(64, fill));
        cases += compare(original, owner, ax_voice::kSetVoiceMix, vpb, kMixArea, 0.0, false);
    }
    require(g_callbacks > 10 && g_acquire_null > 3 && g_acquired == 240, "the acquire cases did not reach the steal / callback / null paths");
    std::printf("acquire: %u calls, %u steals that ran a drop callback, %u returned null\n", g_acquired, g_callbacks, g_acquire_null);
    // Negative control: the owner against the other setter on the same arguments differs.
    const u32 vpb = kVpbBase + kVpbStride;
    fill_both(original, owner.m, vpb, std::vector<u8>(kVpbStride, 0x11));
    original.original(ax_voice::kSetVoiceSrcRatio, vpb, 0u);
    owner.call(ax_voice::kSetVoiceMix, vpb, kMixArea);
    require(original.snapshot() != owner.m.snapshot(), "negative control: the comparison missed a wrong owner");
    std::printf("PASS AX voice setters: %u cases (random AXVPB images, 18 mix halfword patterns, %zu ratios incl. NaN, "
                "inf, clamp, interrupts on/off) equal the original in MSR, f31 and all guest memory; negative control differs\n",
                cases, ratios.size());
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc > 1, "usage: ax_voice_contract_tests <main.dol>");
        run(argv[1]);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
