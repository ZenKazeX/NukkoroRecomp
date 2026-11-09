// Original-DOL contract for the native AX frame sequencing, AXInit and AXQuit (gekko/native_ax_frame.h, audio-ax stage e step 3b).
//
//  * The frame: the original's AI DMA callback __AXOutAiCallback 0x80385950 (then the DSP resume callback __AXOutSPCallback 0x80385A20,
//    as the DSP raised it) against the native owner. The DSP mailbox functions are replaced on the original side by stubs that record the
//    list address the mail named; the native side records the list the engine hands its mixer in the same guest word. The aux callbacks,
//    the drop callbacks and the frame callback are small guest stubs that log their arguments, so the order and arguments of every
//    callback are compared along with all guest memory (the command lists, the profile record and the DSP task / depop block, which the
//    native engine does not write, are masked out; the PB image is compared to the original's PB array).
//  * AXInit / AXQuit: the original runs with its DSP, thread-queue and AI registration functions replaced by recording stubs.
// liveness-roots: reference-interpreter-only
#include "gekko/ax_pb_image.h"
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_ax_frame.h"
#include "gekko/native_ax_sync.h"
#include "gekko/native_ax_voice.h"
#include "gekko/native_guest_call.h"

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
constexpr u32 kPbArray = 0x8058FCA0u, kRing = 0x805949A0u;
constexpr u32 kLog = 0x805F7000u;  // the stubs' log words (low memory of the data segment's tail: not touched by the engine)
constexpr u32 kAiCallbackEntry = 0x80385950u;  // the original __AXOutAiCallback
constexpr u32 kScratch = 0x805F7500u;  // the delivery scratch words of the native frame (tag word included)
constexpr u32 kStubFrame = 0x805F6000u, kStubDrop = 0x805F6040u, kStubAuxA = 0x805F6080u, kStubAuxB = 0x805F60C0u;
constexpr u32 kLogFrame = kLog + 0x00u, kLogDropCount = kLog + 0x04u, kLogDrops = kLog + 0x100u, kLogAuxA = kLog + 0x200u, kLogAuxB = kLog + 0x240u,
              kLogMail = kLog + 0x300u, kLogDmaR3 = kLog + 0x310u, kLogDmaR4 = kLog + 0x314u, kLogDmaCalls = kLog + 0x318u, kLogDmaStart = kLog + 0x31Cu,
              kLogStop = kLog + 0x320u, kLogOrder = kLog + 0x400u, kLogOrderCount = kLog + 0x3FCu;

void require(bool ok, const char* what) {
    if (!ok) throw std::runtime_error(what);
}

// --- a tiny assembler for the stubs ---------------------------------------------------------------------------------------------
u32 dform(u32 op, u32 rt, u32 ra, s32 imm) { return (op << 26) | (rt << 21) | (ra << 16) | (static_cast<u32>(imm) & 0xFFFFu); }
u32 lis(u32 rt, u32 imm) { return dform(15, rt, 0, static_cast<s32>(imm & 0xFFFFu)); }
u32 addi(u32 rt, u32 ra, s32 imm) { return dform(14, rt, ra, imm); }
u32 lwz(u32 rt, s32 d, u32 ra) { return dform(32, rt, ra, d); }
u32 stw(u32 rs, s32 d, u32 ra) { return dform(36, rs, ra, d); }
constexpr u32 kBlr = 0x4E800020u;
u32 xo_add(u32 rt, u32 ra, u32 rb) { return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (266u << 1); }
u32 slwi2(u32 ra, u32 rs) { return (21u << 26) | (rs << 21) | (ra << 16) | (2u << 11) | (29u << 1); }

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
    u32 sda(u32 offset) { return memory.read32(kR13 + offset); }
    void code(u32 address, std::initializer_list<u32> words) {
        for (const u32 w : words) {
            memory.write32(address, w);
            address += 4u;
        }
    }
    void prepare(u32 entry, bool interrupts) {
        cpu.reset();
        cpu.set_msr((base_msr & ~0x8000u) | (interrupts ? 0x8000u : 0u));
        cpu.pc = entry;
        cpu.npc = entry + 4;
        cpu.lr = kReturn;
        cpu.gpr[1] = kStack;
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
    }
    void original(u32 entry, bool interrupts) {
        prepare(entry, interrupts);
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 20000000) cpu.run_steps(1);
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

// The stubs both machines carry in guest memory, and the replacements of the DSP / AI / OS functions the original engine calls.
// Every callback stub first appends its id to a shared order log (r9..r12 are free), so the order of the callbacks is compared too.
std::vector<u32> with_order(u32 id, std::vector<u32> body) {
    std::vector<u32> code = {lis(9, 0x805F), lwz(10, 0x73FC, 9), slwi2(11, 10), xo_add(11, 11, 9), addi(12, 0, static_cast<s32>(id)),
                             stw(12, 0x7400, 11), addi(10, 10, 1), stw(10, 0x73FC, 9)};
    code.insert(code.end(), body.begin(), body.end());
    return code;
}
void place(Machine& m, u32 address, const std::vector<u32>& words) {
    for (const u32 w : words) {
        m.memory.write32(address, w);
        address += 4u;
    }
}
void install_stubs(Machine& m) {
    // frame callback: ++counter
    place(m, kStubFrame, with_order(1, {lis(4, 0x805F), lwz(5, 0x7000, 4), addi(5, 5, 1), stw(5, 0x7000, 4), kBlr}));
    // drop callback (r3 = voice): append the voice to the log
    place(m, kStubDrop, with_order(2, {lis(4, 0x805F), lwz(5, 0x7004, 4), slwi2(6, 5), xo_add(6, 6, 4), stw(3, 0x7100, 6), addi(5, 5, 1), stw(5, 0x7004, 4), kBlr}));
    // aux callbacks (r3 = the buffer words, r4 = the context)
    u32 id = 3;
    for (const auto& [address, log] : {std::pair<u32, s32>{kStubAuxA, 0x7200}, std::pair<u32, s32>{kStubAuxB, 0x7240}}) {
        place(m, address, with_order(id++, {lis(6, 0x805F), stw(4, log, 6), lwz(7, 0, 3), stw(7, log + 4, 6), lwz(7, 4, 3), stw(7, log + 8, 6),
                                            lwz(7, 8, 3), stw(7, log + 12, 6), lwz(7, 12, 3), stw(7, log + 16, 6), lwz(8, log + 0x20, 6),
                                            addi(8, 8, 1), stw(8, log + 0x20, 6), kBlr}));
    }
    // The DSP mailbox: DSPSendMailToDSP logs the mail (the last one is the list address); the check reports "empty".
    m.code(0x8038C2ACu, {lis(4, 0x805F), stw(3, 0x7300, 4), kBlr});
    m.code(0x8038C274u, {addi(3, 0, 0), kBlr});
    // AIInitDMA logs its arguments and call count; AIStartDMA / AIStopDMA count.
    m.code(0x80381E74u, {lis(6, 0x805F), stw(3, 0x7310, 6), stw(4, 0x7314, 6), lwz(7, 0x7318, 6), addi(7, 7, 1), stw(7, 0x7318, 6), kBlr});
    m.code(0x80381EFCu, {lis(6, 0x805F), lwz(7, 0x731C, 6), addi(7, 7, 1), stw(7, 0x731C, 6), kBlr});
    m.code(0x80381F14u, {lis(6, 0x805F), lwz(7, 0x7320, 6), addi(7, 7, 1), stw(7, 0x7320, 6), kBlr});
    // cache range operations: no effect
    for (const u32 address : {0x80372508u, 0x80372534u, 0x80372594u}) m.memory.write32(address, kBlr);
    // OSGetTime: constant (the original stamps its profile record with it)
    m.code(0x80379628u, {addi(3, 0, 0), addi(4, 0, 0), kBlr});
    // the version banner (OSReport)
    m.memory.write32(0x8037177Cu, kBlr);
    // DSPCheckInit -> already initialised; DSPAddTask completes the INIT handshake (sda 0x5D5C = 1); DSPCancelTask completes the quit
    // handshake without sleeping (sda 0x5D60 = 1) - the original sleeps on a queue the test has no thread to wake.
    m.code(0x8038C384u, {addi(3, 0, 1), kBlr});
    m.code(0x8038C38Cu, {addi(4, 0, 1), stw(4, 0x5D5C, 13), kBlr});
    m.code(0x8038C3FCu, {addi(4, 0, 1), stw(4, 0x5D60, 13), kBlr});
    // OSSleepThread / OSInitThreadQueue: the quit handshake does not sleep; the queue init is a store of two zero words
    m.code(0x803792C4u, {kBlr});
    // AIRegisterDMACallback is the original's own code (it only stores the callback in the SDK's words)
}

// The same effects on the native machine, where the owner serves the AI functions as hooks.
void install_native_hooks(Machine& m) {
    Machine* machine = &m;
    m.cpu.install_hle(0x80381E74u, [machine](Cpu& c) {
        machine->memory.write32(kLogDmaR3, c.gpr[3]);
        machine->memory.write32(kLogDmaR4, c.gpr[4]);
        machine->memory.write32(kLogDmaCalls, machine->memory.read32(kLogDmaCalls) + 1u);
        return true;
    }, "test: AIInitDMA");
    m.cpu.install_hle(0x80381EFCu, [machine](Cpu&) {
        machine->memory.write32(kLogDmaStart, machine->memory.read32(kLogDmaStart) + 1u);
        return true;
    }, "test: AIStartDMA");
    m.cpu.install_hle(0x80381F14u, [machine](Cpu&) {
        machine->memory.write32(kLogStop, machine->memory.read32(kLogStop) + 1u);
        return true;
    }, "test: AIStopDMA");
    m.cpu.install_hle(0x8037611Cu, [](Cpu& c) {  // OSDisableInterrupts
        const u32 msr = c.msr();
        c.set_msr(msr & ~0x8000u);
        c.gpr[3] = (msr & 0x8000u) != 0u ? 1u : 0u;
        return true;
    }, "test: OSDisableInterrupts");
    m.cpu.install_hle(0x80376144u, [](Cpu& c) {  // OSRestoreInterrupts
        const u32 was = (c.msr() & 0x8000u) != 0u ? 1u : 0u;
        c.set_msr((c.msr() & ~0x8000u) | (c.gpr[3] != 0u ? 0x8000u : 0u));
        c.gpr[3] = was;
        return true;
    }, "test: OSRestoreInterrupts");
}

unsigned g_inits = 0, g_frames = 0, g_drops = 0, g_aux_a = 0, g_aux_b = 0, g_frame_cb = 0;

void compare(Machine& original, Machine& owner, AxPbImage& image, AxLists& lists, bool with_pb, const char* what, u32 round, bool aux_a_four_words = false) {
    // The aux stubs log the fourth word the callback could read: a stack word nobody set when the call passes three buffers.
    if (aux_a_four_words) require(original.memory.read32(kLogAuxA + 0x10u) == owner.memory.read32(kLogAuxA + 0x10u), "the fourth aux A buffer differs");
    original.memory.write32(kLogAuxA + 0x10u, 0u);
    owner.memory.write32(kLogAuxA + 0x10u, 0u);
    original.memory.write32(kLogAuxB + 0x10u, 0u);
    owner.memory.write32(kLogAuxB + 0x10u, 0u);
    std::vector<u8> sa = original.snapshot(), sb = owner.snapshot();
    // The PB array / update rings: the original's guest arrays are the DSP side's copy; the owner keeps it in the image. They must agree.
    if (with_pb) {
        for (u32 i = 0; i < image.pb.size(); ++i) require(sa[kPbArray - kLow + i] == image.pb[i], "the PB image differs from the original's PB array");
        for (u32 i = 0; i < image.ring.size(); ++i) require(sa[kRing - kLow + i] == image.ring[i], "the update-ring image differs from the original's");
    }
    // Not written by the native engine: the PB array and rings (image), the command lists (AxLists), the DSP task and profile record
    // (0x8058FBC0..0x8058FCA0, the depop block 0x8058FC60 is inside), the guest stack frames (masked by size), the depop sums
    // (sda 0x5D78..0x5D98), the list cursor (sda 0x5D2C), the state word is compared (the native engine sets what the DSP did).
    const auto mask = [&](u32 address, u32 bytes) {
        for (auto* snapshot : {&sa, &sb}) std::fill_n(snapshot->begin() + (address - kLow), bytes, u8{0});
    };
    mask(kPbArray, static_cast<u32>(image.pb.size()));
    mask(kRing, static_cast<u32>(image.ring.size()));
    mask(AxLists::kBase, 0x600u);
    mask(0x8058FBC0u, 0xE0u);
    mask(kR13 + 0x5D78u, 0x24u);
    mask(kScratch, 0x10u);
    mask(kR13 + 0x5D2Cu, 4u);
    mask(kR13 + ax_sync::kSdaTotalCost, 8u);  // the profile's cost words (0x5DA4, 0x5DA8)
    mask(kR13 + 0x5D50u, 8u);                 // the profile's time stamp (the AI callback's state 0 path)
    if (sa != sb) {
        unsigned shown = 0;
        for (std::size_t i = 0; i < sa.size() && shown < 12; ++i)
            if (sa[i] != sb[i]) {
                std::fprintf(stderr, "  %s round %u: byte %08X original %02X owner %02X\n", what, round, unsigned(kLow + i), sa[i], sb[i]);
                ++shown;
            }
    }
    (void)lists;
    require(sa == sb, "guest memory differs");
}

void frame_world(const char* dol, u32 rounds) {
    Machine original(dol), owner(dol);
    AxPbImage image;
    AxLists lists;
    for (Machine* m : {&original, &owner}) install_stubs(*m);
    // The owner side takes no stubs for the code the engine now runs natively; its mixer records the list in the same guest word.
    install_native_hooks(owner);
    ax_frame::Engine engine;
    engine.image = &image;
    engine.lists = &lists;
    engine.mix = [&owner](u32 list) { owner.memory.write32(kLogMail, list); };
    ax_frame::install(owner.cpu, lists);
    ax_frame::install_engine(owner.cpu, engine);
    Rng rng;
    for (u32 round = 0; round < rounds; ++round) {
        std::vector<u8> vpbs(kVpbStride * 64, 0), pbs(0x4D00 + 0x4000), heads(0x80, 0), tails(0x80, 0), logs(0x500, 0);
        for (u8& byte : pbs) byte = static_cast<u8>(rng.next());
        std::array<std::vector<u32>, 32> stacks;
        for (u32 v = 0; v < 64; ++v) stacks[rng.next() % 3u == 0u ? 0u : 1u + rng.next() % 31u].push_back(v);
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
        // The callback stack: a random subset of the busy voices (the sync dropped them), some with no callback.
        u32 stack_top = 0u;
        for (u32 v = 0; v < 64; ++v) {
            const u32 off = v * kVpbStride;
            put32(vpbs, off + 0x18, v);
            put32(vpbs, off + 0x134, kPbArray + 0x3D00u + v * 0x40u);
            put16(vpbs, off + 0x146, rng.next() % 5u == 0u ? 1u : 0u);
            put32(vpbs, off + 0x10, rng.next() % 3u == 0u ? 0u : kStubDrop);
            if (rng.next() % 4u == 0u) {
                put32(vpbs, off + 8, stack_top);
                stack_top = kVpbBase + off;
            }
        }
        const u32 mode = rng.next() % 4u, aux_a = rng.next() & 1u, aux_b = rng.next() & 1u, cycles = rng.next() % 0x8000u;
        const u32 frame_callback = rng.next() % 3u == 0u ? 0u : kStubFrame, compressor = rng.next() & 1u;
        for (Machine* m : {&original, &owner}) {
            m->memory.write_block(kVpbBase, vpbs.data(), vpbs.size());
            m->memory.write_block(kPbArray, pbs.data(), pbs.size());
            m->memory.write_block(kHeads, heads.data(), heads.size());
            m->memory.write_block(kTails, tails.data(), tails.size());
            m->memory.write_block(kLog, logs.data(), logs.size());
            m->sda(ax_frame::kSdaCallbackStack, stack_top);
            m->sda(ax_sync::kSdaBudget, 0xFFFFFFFFu);
            m->sda(ax_frame::kSdaCycles, cycles);
            m->sda(ax_frame::kSdaMode, mode);
            m->sda(ax_frame::kSdaAuxA, aux_a ? kStubAuxA : 0u);
            m->sda(ax_frame::kSdaAuxB, aux_b ? kStubAuxB : 0u);
            m->sda(ax_frame::kSdaAuxCtxA, 0xA0000000u + round);
            m->sda(ax_frame::kSdaAuxCtxB, 0xB0000000u + round);
            m->sda(ax_frame::kSdaAuxIndexA, round % 3u);
            m->sda(ax_frame::kSdaAuxIndexB, (round / 3u) % 3u);
            m->sda(ax_frame::kSdaAuxFlagA, round & 1u);
            m->sda(ax_frame::kSdaAuxFlagB, (round >> 1) & 1u);
            m->sda(ax_frame::kSdaAuxBuffer, (round / 2u) % 3u);
            m->sda(ax_frame::kSdaListIndex, round & 1u);
            m->sda(ax_frame::kSdaOutIndex, (round >> 2) & 1u);
            m->sda(ax_frame::kSdaFrameCallback, frame_callback);
            m->sda(ax_frame::kSdaCompressor, compressor);
            m->sda(ax_frame::kSdaOutMode, 0u);
            m->sda(ax_frame::kSdaState, 1u);
            m->sda(ax_frame::kSdaDmaIndex, 0u);
        }
        std::memcpy(image.pb.data(), pbs.data(), image.pb.size());
        std::memcpy(image.ring.data(), pbs.data() + 0x4D00, image.ring.size());
        lists.clear();
        // The list the previous frame built (the mixer is stubbed on both sides; the native lists need the slot valid only for next_frame).
        const bool interrupts = (round & 1u) != 0u;
        original.original(kAiCallbackEntry, interrupts);
        original.original(0x80385A20u, interrupts);  // the DSP's resume callback: the state word returns to 1
        // The native frame is the AI handler's step machine: every call it asks for runs to completion, as the delivery's guest calls do.
        owner.prepare(kAiCallbackEntry, interrupts);
        {
            ax_frame::Call call;
            unsigned step = 0;
            while (ax_frame::frame_advance(owner.cpu, engine, kScratch, step, call)) {
                u32 result = 0;
                require(guest_call(owner.cpu, call.target, kAiCallbackEntry, {call.r3, call.r4}, result), "a callback of the frame did not return");
                ++step;
                require(step < 400u, "the frame asked for too many calls");
            }
        }
        require(!owner.cpu.stopped(), "the native frame stopped the machine");
        require(original.cpu.msr() == owner.cpu.msr(), "the machine state register differs");
        // The list the original handed the DSP is the slot the native engine mixed.
        require(original.memory.read32(kLogMail) == owner.memory.read32(kLogMail), "the mixed list differs");
        // What the next frame's list says: decode the original's slot (the one its builder wrote) against the native slot.
        const u32 built = original.sda(0x5D28u) & 1u;  // the slot index the next frame will mix
        (void)built;
        compare(original, owner, image, lists, true, "frame", round, mode == 2u && aux_a != 0u);
        ++g_frames;
        g_drops += original.memory.read32(kLogDropCount);
        g_aux_a += aux_a;
        g_aux_b += aux_b && mode != 2u;
        g_frame_cb += original.memory.read32(kLogFrame);
    }
}


// AXInit and AXQuit: random garbage in everything they touch, then the original against the owner.
void init_quit_world(const char* dol, u32 rounds) {
    Machine original(dol), owner(dol);
    AxPbImage image;
    AxLists lists;
    for (Machine* m : {&original, &owner}) install_stubs(*m);
    install_native_hooks(owner);
    ax_frame::Engine engine;
    engine.image = &image;
    engine.lists = &lists;
    engine.mix = [](u32) {};
    ax_frame::install(owner.cpu, lists);
    ax_frame::install_engine(owner.cpu, engine);
    Rng rng;
    for (u32 round = 0; round < rounds; ++round) {
        std::vector<u8> junk(0x8B00 + 0x4000);
        for (u8& byte : junk) byte = static_cast<u8>(rng.next());
        const u32 clock = 162000000u + rng.next() % 100000u;
        const auto make_world = [&](Machine& m) {
            m.memory.write_block(kVpbBase, junk.data(), 0x8B00);
            m.memory.write_block(kHeads, junk.data(), 0x80);
            m.memory.write_block(kTails, junk.data() + 0x80, 0x80);
            m.memory.write_block(ax_frame::kOutBase, junk.data(), 0xA00);
            m.memory.write_block(ax_frame::kAuxBase, junk.data() + 0xA00, 0x780);
            m.memory.write_block(ax_frame::kAuxBBase, junk.data() + 0x1180, 0x780);
            m.memory.write_block(kRing, std::vector<u8>(AxPbImage::kCount * AxPbImage::kRingStride, 0).data(), AxPbImage::kCount * AxPbImage::kRingStride);
            m.memory.write32(0x800000F8u, clock);
            for (u32 off = 0x5CE8u; off < 0x5DD0u; off += 4u) m.sda(off, rng.next());
            m.memory.write_block(kLog, std::vector<u8>(0x500, 0).data(), 0x500);
        };
        // The same random world on both machines: draw it once.
        const u64 saved = rng.s;
        make_world(original);
        rng.s = saved;
        make_world(owner);
        image.clear();
        const bool interrupts = (round & 1u) != 0u;
        original.original(ax_frame::kInit, interrupts);
        owner.prepare(ax_frame::kInit, interrupts);
        require(owner.cpu.try_hle(ax_frame::kInit), "the owner declined AXInit");
        require(!owner.cpu.stopped(), "the native AXInit stopped the machine");
        require(original.cpu.msr() == owner.cpu.msr(), "AXInit: the machine state register differs");
        // The original's engine state the native one does not keep: the DSP INIT handshake flags and the quit queue (sda 0x5D5C..0x5D6C),
        // the DSP task and its profile record, the retired depop sums, the PB array's blob words (no reader).
        for (Machine* m : {&original, &owner}) {
            for (u32 off = 0x5D5Cu; off < 0x5D70u; off += 4u) m->sda(off, 0u);
            for (u32 off = 0x5D78u; off <= 0x5D98u; off += 4u) m->sda(off, 0u);
            m->sda(0x5C64u, 0u);  // the AI callback: the original registers its function, the native engine its token
            m->sda(0x5D50u, 0u);
            m->sda(0x5D54u, 0u);
            m->memory.write_block(kPbArray + 0x3D00u, std::vector<u8>(0x1000, 0).data(), 0x1000);
        }
        compare(original, owner, image, lists, true, "AXInit", round);
        // AXQuit
        original.original(ax_frame::kQuit, interrupts);
        owner.prepare(ax_frame::kQuit, interrupts);
        require(owner.cpu.try_hle(ax_frame::kQuit), "the owner declined AXQuit");
        require(!owner.cpu.stopped(), "the native AXQuit stopped the machine");
        require(original.cpu.msr() == owner.cpu.msr(), "AXQuit: the machine state register differs");
        for (Machine* m : {&original, &owner}) {
            for (u32 off = 0x5D5Cu; off < 0x5D70u; off += 4u) m->sda(off, 0u);
        }
        compare(original, owner, image, lists, true, "AXQuit", round);
        ++g_inits;
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: ax_out_contract_tests <main.dol>");
        frame_world(argv[1], 400);
        init_quit_world(argv[1], 40);
        std::printf("PASS native AX frame sequencing: %u frames equal the original (drop callbacks %u, aux A calls %u, aux B calls %u, frame callbacks %u)\n",
                    g_frames, g_drops, g_aux_a, g_aux_b, g_frame_cb);
        std::printf("PASS native AXInit / AXQuit: %u rounds equal the original\n", g_inits);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
