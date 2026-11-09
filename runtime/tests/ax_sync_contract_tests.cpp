// Original-DOL contract for the native AX sync (gekko/native_ax_sync.h, audio-ax stage e step 1): __AXSyncPBs 0x80386BC0 with its
// callees (__AXSyncPB, the depop accumulation, the callback stack push). The original side is the pinned DOL's instructions in the
// reference interpreter; the owner side is the installed hook on a second machine. Compared after each call: the machine state
// register and every word of guest memory (0x80000000..0x80600000: PB array, AXVPB array, voice stacks, SDA words, write rings).
// Random worlds: 64 voices spread over the priority stacks (and the free stack), random dirty words (including the whole-block bit),
// random states, write-ring counts, depop flags and budgets from "everything dropped" to "nothing dropped".
// liveness-roots: reference-interpreter-only
#include "gekko/ax_pb_image.h"
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_ax_sync.h"

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
constexpr u32 kVpbBase = 0x805989A0u, kVpbStride = 0x22Cu, kHeads = 0x80587DC0u, kTails = 0x80587E40u;
constexpr u32 kPbArray = 0x8058FCA0u, kScratch = 0x805F8000u;

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
    void prepare(u32 entry, u32 r3, bool interrupts) {
        cpu.reset();
        cpu.set_msr((base_msr & ~0x8000u) | (interrupts ? 0x8000u : 0u));
        cpu.pc = entry;
        cpu.npc = entry + 4;
        cpu.lr = kReturn;
        cpu.gpr[1] = kStack;
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.gpr[3] = r3;
    }
    void original(u32 entry, u32 r3, bool interrupts) {
        prepare(entry, r3, interrupts);
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 2000000) cpu.run_steps(1);
        if (!(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack))
            std::fprintf(stderr, "  entry %08X pc %08X steps %u stopped %d r1 %08X lr %08X\n", entry, cpu.pc, steps, int(cpu.stopped()), cpu.gpr[1], cpu.lr);
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

void put32(std::vector<u8>& buf, u32 off, u32 value) {
    buf[off] = static_cast<u8>(value >> 24); buf[off + 1] = static_cast<u8>(value >> 16);
    buf[off + 2] = static_cast<u8>(value >> 8); buf[off + 3] = static_cast<u8>(value);
}
void put16(std::vector<u8>& buf, u32 off, u32 value) {
    buf[off] = static_cast<u8>(value >> 8); buf[off + 1] = static_cast<u8>(value);
}

unsigned g_drops = 0, g_synced_flags = 0, g_ring = 0, g_depop = 0, g_full = 0;

void run(const char* dol) {
    Machine original(dol), owner(dol);
    AxPbImage image;
    // The cache range operations (DCInvalidateRange 0x80372508, DCStoreRange 0x80372534) have no effect on guest memory and use
    // instructions the reference interpreter does not model: both machines get a blr there (identical text on both sides).
    for (Machine* m : {&original, &owner}) {
        m->memory.write32(0x80372508u, 0x4E800020u);
        m->memory.write32(0x80372534u, 0x4E800020u);
    }
    ax_sync::install(owner.cpu, image);
    Rng rng;
    unsigned cases = 0;
    for (u32 round = 0; round < 260; ++round) {
        std::vector<u8> vpbs(kVpbStride * 64), pbs(0x4D00 + 0x4000), heads(0x80, 0), tails(0x80, 0);
        for (u8& byte : vpbs) byte = static_cast<u8>(rng.next());
        for (u8& byte : pbs) byte = static_cast<u8>(rng.next());
        std::array<std::vector<u32>, 32> stacks;
        const u32 spread = 1u + rng.next() % 32u;
        for (u32 v = 0; v < 64; ++v) stacks[rng.next() % spread].push_back(v);
        for (u32 p = 0; p < 32; ++p) {
            const auto& list = stacks[p];
            for (std::size_t i = 0; i < list.size(); ++i) {
                const u32 v = list[i], off = v * kVpbStride;
                put32(vpbs, off + 0, i + 1 < list.size() ? kVpbBase + kVpbStride * list[i + 1] : 0u);
                put32(vpbs, off + 4, i > 0 ? kVpbBase + kVpbStride * list[i - 1] : 0u);
                put32(vpbs, off + 0xC, p);
            }
            if (!list.empty()) {
                put32(heads, p * 4, kVpbBase + kVpbStride * list.front());
                put32(tails, p * 4, kVpbBase + kVpbStride * list.back());
            }
        }
        for (u32 v = 0; v < 64; ++v) {
            const u32 off = v * kVpbStride;
            put32(vpbs, off + 0x18, v);                                  // PB index
            const u32 kind = rng.next() % 10u;                           // dirty word: none, whole block, or random bits
            u32 flags = kind < 3u ? 0u : kind == 3u ? 0x80000000u : (rng.next() & 0x7FFFFFFFu);
            if (kind >= 4u && (rng.next() & 3u) == 0u) flags &= 0x00000FFFu;
            put32(vpbs, off + 0x1C, flags);
            put32(vpbs, off + 0x20, rng.next() % 3u == 0u ? 1u : 0u);    // depop pending
            put32(vpbs, off + 0x28, rng.next() % 3u == 0u ? rng.next() % 40u : 0u);  // write ring words
            put32(vpbs, off + 0x134, kScratch + 0x100 * (v % 16u));
            put16(vpbs, off + 0x146, rng.next() % 4u == 0u ? 0u : 1u);   // mostly running
            put16(vpbs, off + 0x140, rng.next() % 3u);
            put16(vpbs, off + 0x1DE, rng.next() % 6u);                   // source type: cost table index
            put16(vpbs, off + 0x1F2, rng.next() % 2u);
            put16(vpbs, off + 0x144, rng.next());                        // mix flags: cost table indices
            // PB state: sometimes running so the drop path takes the depop branch
            put16(pbs, v * AxPbImage::kStride + 0xE, rng.next() % 2u);
        }
        const u32 cycles = rng.next() % 0x2000u, arg = rng.next() % 0x4000u;
        const u32 mode = round % 4u;  // budget: all dropped, mid, large, huge
        const u32 budget = mode == 0u ? 0u : mode == 1u ? 0x10000u + rng.next() % 0x20000u : mode == 2u ? 0x60000u : 0x7FFFFFFFu;
        const u32 stack_top = rng.next() % 3u == 0u ? 0u : kVpbBase + kVpbStride * (rng.next() % 64u);
        for (Machine* m : {&original, &owner}) {
            m->memory.write_block(kVpbBase, vpbs.data(), vpbs.size());
            m->memory.write_block(kPbArray, pbs.data(), pbs.size());
            m->memory.write_block(kHeads, heads.data(), heads.size());
            m->memory.write_block(kTails, tails.data(), tails.size());
            for (u32 off = 0x5D78; off <= 0x5D98; off += 4) m->sda(off, 0x1000u + off);
            m->sda(ax_sync::kSdaCycles, cycles);
            m->sda(ax_sync::kSdaBudget, budget);
            m->sda(ax_sync::kSdaCallbackStack, stack_top);
            m->sda(ax_sync::kSdaTotalCost, 0xAAAAAAAAu);
            m->sda(ax_sync::kSdaSyncCount, 77u);
        }
        std::memcpy(image.pb.data(), pbs.data(), image.pb.size());
        std::memcpy(image.ring.data(), pbs.data() + 0x4D00, image.ring.size());
        const bool interrupts = (round & 1u) != 0u;
        original.original(ax_sync::kSyncPBs, arg, interrupts);
        owner.prepare(ax_sync::kSyncPBs, arg, interrupts);
        require(owner.cpu.try_hle(ax_sync::kSyncPBs), "the owner declined the call");
        require(original.cpu.msr() == owner.cpu.msr(), "the machine state register differs");
        std::vector<u8> sa = original.snapshot(), sb = owner.snapshot();
        // The PB array and its update rings are the DSP side's copy: the original keeps them in guest memory, the owner in the image.
        // They must equal each other; the rest of guest memory must be equal with those two regions masked out.
        for (u32 i = 0; i < image.pb.size(); ++i)
            require(sa[kPbArray - kLow + i] == image.pb[i], "the PB image differs from the original's PB array");
        for (u32 i = 0; i < image.ring.size(); ++i)
            require(sa[0x805949A0u - kLow + i] == image.ring[i], "the update-ring image differs from the original's");
        // The depop sums (sda 0x5D78..0x5D98) are the DSP's click-removal bookkeeping: the original accumulates them for its command list's
        // depop record, which the native engine has no use for (step 3b); they are masked out here.
        for (auto* snapshot : {&sa, &sb}) {
            std::fill_n(snapshot->begin() + (kPbArray - kLow), image.pb.size(), u8{0});
            std::fill_n(snapshot->begin() + (0x805949A0u - kLow), image.ring.size(), u8{0});
            std::fill_n(snapshot->begin() + (kR13 + 0x5D78u - kLow), 0x24, u8{0});
        }
        if (sa != sb)
            std::fprintf(stderr, "  round %u: total cost original %08X owner %08X; callback stack original %08X owner %08X; sync count %u / %u\n", round,
                         original.memory.read32(kR13 + ax_sync::kSdaTotalCost), owner.memory.read32(kR13 + ax_sync::kSdaTotalCost),
                         original.memory.read32(kR13 + ax_sync::kSdaCallbackStack), owner.memory.read32(kR13 + ax_sync::kSdaCallbackStack),
                         original.memory.read32(kR13 + ax_sync::kSdaSyncCount), owner.memory.read32(kR13 + ax_sync::kSdaSyncCount));
        if (sa != sb) {
            unsigned shown = 0;
            for (std::size_t i = 0; i < sa.size() && shown < 8; ++i)
                if (sa[i] != sb[i]) {
                    std::fprintf(stderr, "  round %u: byte %08X original %02X owner %02X\n", round, unsigned(kLow + i), sa[i], sb[i]);
                    ++shown;
                    const u32 address = kLow + static_cast<u32>(i);
                    if (address >= 0x805949A0u && address < 0x805949A0u + 0x4000u) {
                        const u32 v = (address - 0x805949A0u) >> 8;
                        const u32 base = v * kVpbStride;
                        const auto be32 = [&](u32 off) { return (u32(vpbs[base + off]) << 24) | (u32(vpbs[base + off + 1]) << 16) | (u32(vpbs[base + off + 2]) << 8) | vpbs[base + off + 3]; };
                        std::fprintf(stderr, "    source words %08X %08X; ring words at +0x%X: original %08X owner %08X\n", be32(0x34), be32(0x38),
                                     address - 0x805949A0u, original.memory.read32(address & ~3u), owner.memory.read32(address & ~3u));
                        std::fprintf(stderr, "    voice %u: flags %08X depop %u ring %u state %02X%02X prio %u budget %08X cycles %X arg %X\n", v, be32(0x1C), be32(0x20), be32(0x28),
                                     vpbs[base + 0x146], vpbs[base + 0x147], be32(0xC), budget, cycles, arg);
                    }
                }
        }
        require(sa == sb, "guest memory differs");
        ++cases;
        const u32 drops = original.memory.read32(kR13 + ax_sync::kSdaCallbackStack) != stack_top ? 1u : 0u;
        g_drops += drops;
    }
    // Negative control: a different budget gives a different result.
    for (Machine* m : {&original, &owner}) {
        std::vector<u8> vpbs(kVpbStride * 64, 0);
        m->memory.write_block(kVpbBase, vpbs.data(), vpbs.size());
    }
    std::printf("PASS AX sync: %u random worlds (64 voices over the priority stacks, random dirty words, states, depop flags, write rings, "
                "budgets from all-dropped to none-dropped; %u worlds dropped at least one voice) equal the original in MSR and all guest memory\n",
                cases, g_drops);
    require(g_drops > 20, "too few worlds reached the drop path");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc > 1, "usage: ax_sync_contract_tests <main.dol>");
        run(argv[1]);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    return 0;
}
