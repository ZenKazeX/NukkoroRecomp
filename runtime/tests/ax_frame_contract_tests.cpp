// Original-DOL contract for the native AX frame builder (gekko/native_ax_frame.h, audio-ax stage e step 3a): __AXNextFrame 0x803850AC.
// The original builds the 0x300-byte DSP command list in the slot the guest list pointer (sda 0x5D2C) names and accumulates its cost in
// sda 0x5D30; the owner stores the list's meaning (frame settings, output buffer) in an AxLists slot and the same cost. Compared after
// each call: the machine state register, the cost, the decoded list, and every other guest word (the list bytes masked out).
// liveness-roots: reference-interpreter-only
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_ax_frame.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
using namespace gekko;
constexpr u32 kStack = 0x817E0000u, kReturn = 0x817FF000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kLow = 0x80000000u, kHigh = 0x80600000u;
constexpr u32 kSlot0 = 0x8058ABC0u;
constexpr u8 kWords[0x14] = {2, 5, 2, 0, 4, 4, 2, 2, 4, 2, 0, 0, 0, 3, 4, 0, 4, 2, 4, 12};

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
    void prepare(u32 entry, u32 r3, u32 r4, bool interrupts) {
        cpu.reset();
        cpu.set_msr((base_msr & ~0x8000u) | (interrupts ? 0x8000u : 0u));
        cpu.pc = entry;
        cpu.npc = entry + 4;
        cpu.lr = kReturn;
        cpu.gpr[1] = kStack;
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.gpr[3] = r3;
        cpu.gpr[4] = r4;
    }
    void original(u32 entry, u32 r3, u32 r4, bool interrupts) {
        prepare(entry, r3, r4, interrupts);
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 200000) cpu.run_steps(1);
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack, "original call did not return cleanly");
    }
};

struct Rng {
    u64 s = 0x9E3779B97F4A7C15ull;
    u32 next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<u32>(s >> 33);
    }
};

// What the list says, decoded the way the microcode (and the native importer) reads it.
struct Decoded {
    shadow::audio::FrameSettings settings;
    u32 output = 0, head = 0;
    bool ended = false;
};
Decoded decode(GuestMemory& memory, u32 list) {
    Decoded d;
    u32 cursor = list;
    while (cursor < list + 0x300u) {
        const u16 id = memory.read16(cursor);
        cursor += 2u;
        require(id <= 0x13u, "list command above 0x13");
        switch (id) {
            case 0x02: d.head = memory.read32(cursor); break;
            case 0x07: d.settings.mode = shadow::audio::OutputMode::StereoFold; break;
            case 0x11: d.settings.mode = shadow::audio::OutputMode::OppositeFold; break;
            case 0x10: d.settings.mode = shadow::audio::OutputMode::DolbyMatrix; break;
            case 0x04: case 0x13: d.settings.aux_a = true; break;
            case 0x05: d.settings.aux_b = true; break;
            case 0x12:
                d.settings.compressor = true;
                d.settings.compressor_threshold = memory.read16(cursor);
                d.settings.compressor_release_frames = memory.read16(cursor + 2u);
                require(memory.read32(cursor + 4u) == 0x80563420u, "compressor table address");
                break;
            case 0x0E: d.output = memory.read32(cursor + 4u); break;
            default: break;
        }
        cursor += 2u * kWords[id];
        if (id == 0x0Fu) {
            d.ended = true;
            break;
        }
    }
    return d;
}

void run(const char* dol) {
    Machine original(dol), owner(dol);
    AxLists lists;
    // DCInvalidateRange / DCStoreRange use instructions the reference interpreter does not model and have no effect on guest memory.
    for (Machine* m : {&original, &owner}) {
        m->memory.write32(0x80372508u, 0x4E800020u);
        m->memory.write32(0x80372534u, 0x4E800020u);
    }
    ax_frame::install(owner.cpu, lists);
    Rng rng;
    unsigned cases = 0, modes[4] = {}, aux_a_count = 0, aux_b_count = 0, compressor_count = 0;
    for (u32 round = 0; round < 600; ++round) {
        const u32 pick = rng.next() % 8u;
        const u32 mode = pick < 3u ? pick : pick == 3u ? 0xFFFFFFFFu : pick == 4u ? 3u : pick == 5u ? 0x80000000u : pick - 3u;
        const u32 aux_a = rng.next() & 1u ? 0x8043AA00u : 0u, aux_b = rng.next() & 1u ? 0x8043BB00u : 0u;
        const u32 compressor = rng.next() % 4u == 0u ? 0u : rng.next() | 1u;
        const u32 slot = rng.next() & 1u;
        const u32 mid = 0x8058B940u, out = 0x8058B1C0u + 0x280u * (rng.next() % 3u);
        std::vector<u8> junk(0x600);
        for (u8& byte : junk) byte = static_cast<u8>(rng.next());
        for (Machine* m : {&original, &owner}) {
            m->memory.write_block(kSlot0, junk.data(), junk.size());
            m->sda(ax_frame::kSdaMode, mode);
            m->sda(ax_frame::kSdaAuxA, aux_a);
            m->sda(ax_frame::kSdaAuxB, aux_b);
            m->sda(ax_frame::kSdaCompressor, compressor);
            m->sda(ax_frame::kSdaListPtr, kSlot0 + 0x300u * slot);
            m->sda(ax_frame::kSdaCycles, 0xDEADBEEFu);
        }
        const bool interrupts = (round & 1u) != 0u;
        original.original(ax_frame::kNextFrame, mid, out, interrupts);
        owner.prepare(ax_frame::kNextFrame, mid, out, interrupts);
        require(owner.cpu.try_hle(ax_frame::kNextFrame), "the owner declined the call");
        require(original.cpu.msr() == owner.cpu.msr(), "the machine state register differs");
        const Decoded d = decode(original.memory, kSlot0 + 0x300u * slot);
        require(d.ended, "the original's list has no end command");
        const AxLists::List& got = lists.slot[slot];
        require(got.valid && !lists.slot[slot ^ 1u].valid, "the owner stored the list in the wrong slot");
        require(d.head == 0x8058FCA0u && got.head == d.head, "list head");
        require(d.output == out && got.output == out, "list output");
        require(d.settings.mode == got.settings.mode && d.settings.aux_a == got.settings.aux_a && d.settings.aux_b == got.settings.aux_b &&
                    d.settings.compressor == got.settings.compressor &&
                    (!d.settings.compressor || (d.settings.compressor_threshold == got.settings.compressor_threshold &&
                                                d.settings.compressor_release_frames == got.settings.compressor_release_frames)),
                "the frame settings differ from the original list");
        require(original.memory.read32(kR13 + ax_frame::kSdaCycles) == owner.memory.read32(kR13 + ax_frame::kSdaCycles), "the list cost differs");
        std::vector<u8> sa = original.snapshot(), sb = owner.snapshot();
        // The list bytes are the native list's other half; the original's write cursor (the list pointer, left behind the list) is
        // reassigned by the next frame before anything reads it.
        for (auto* snapshot : {&sa, &sb}) {
            std::fill_n(snapshot->begin() + (kSlot0 - kLow), 0x600, u8{0});
            std::fill_n(snapshot->begin() + (kR13 + ax_frame::kSdaListPtr - kLow), 4, u8{0});
        }
        require(sa == sb, "guest memory differs");
        lists.clear();
        ++modes[mode == 0u ? 0u : mode == 1u ? 1u : mode == 2u ? 2u : 3u];
        aux_a_count += d.settings.aux_a;
        aux_b_count += d.settings.aux_b;
        compressor_count += d.settings.compressor;
        ++cases;
    }
    std::printf("PASS native AX frame builder: %u random worlds equal the original (modes %u/%u/%u/other %u, aux A %u, aux B %u, compressor %u)\n",
                cases, modes[0], modes[1], modes[2], modes[3], aux_a_count, aux_b_count, compressor_count);
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: ax_frame_contract_tests <main.dol>");
        run(argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
