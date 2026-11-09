// liveness-roots: reference-interpreter-only
// The RenderWare GameCube driver's raster life cycle, slice 1 (docs/agents/card-saves.md, graphics.rw-gamecube-driver
// cluster rwR2): GcnTextureSetRaster, GcnRasterSubRaster, GcnRasterSetContext, GcnRasterGetMipLevels, the draw-sync
// token test 0x8049B7DC and GcnRasterDestroy, as ORIGINAL words in the reference interpreter, against the native owners
// (gekko/native_rw_raster.h).
//
// Both machines own the GX synchronisation (NativeGxSync: GXReadDrawSync, GXSetDrawSync) - the original body therefore
// reads the same token the owner does, and what differs between the two is exactly the raster code. For every case the
// test compares: the return value, every guest word either machine wrote outside the stack (address and value, so a
// word only one side wrote is found), the callee-saved registers, r1 and LR, NativeGxSync's token and the number of
// tokens it consumed, and the engine's free callback (a test function: it counts its calls and records the block it was
// given; a PPC stub on both sides: the owner reaches it through gekko::guest_call).
//
// Negative controls: an owner that left the token alone, one that skipped the free, and one that freed another block
// are each told apart; the refused cases (a raster type the original reports an error for, a token the original waits
// for forever) stop the owner and keep the original from returning.
//
// usage: rw_raster_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_gx_sync.h"
#include "gekko/native_interrupts.h"
#include "gekko/native_rw_raster.h"
#include "gekko/native_vi_wait.h"
#include "support/os_entry.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;

constexpr u32 kSda = 0x805EC500u;
constexpr u32 kSda2 = 0x805FA780u;
constexpr u32 kStackTop = 0x817E0000u;
constexpr u32 kStackFloor = 0x817D0000u;
constexpr u32 kFinish = 0x817FF000u;
constexpr u32 kOsContext = 0x817F0000u;
constexpr u32 kEngine = 0x81310000u;       // the engine instance: memory functions at +0x108 / +0x10C
constexpr u32 kFreeStub = 0x81311000u;     // the engine's free function (a test function)
constexpr u32 kFreeLog = 0x81312000u;      // +0 call count, +4 last block
constexpr u32 kMallocStub = 0x81311200u;   // the engine allocator (a test function): count +0x10, size +0x14, flags +0x18, result +0x1C
constexpr u32 kNotifyStub = 0x81311100u;   // the engine's raster-changed hook (a test function)
constexpr u32 kRasters = 0x81320000u;
constexpr u32 kPluginOffset = 0x40u;       // the raster plugin's offset inside a raster
constexpr u32 kTexturePlugin = 0x30u;      // the texture plugin's offset inside a texture
constexpr u32 kTexture = 0x81330000u;
constexpr u32 kBlock = 0x81340000u;

constexpr u32 kSdaPlugin = 0x6200u, kSdaEngine = 0x615Cu, kSdaBound = 0x620Cu, kSdaTexturePlugin = 0x6218u,
              kSdaContext = 0x6210u, kSdaTokenRead = 0x6220u, kSdaTokenNext = 0x2AF0u;
constexpr u32 kMsrEE = 0x8000u, kMsrFP = 0x2000u;

constexpr u32 kTextureSetRaster = 0x8049819Cu, kSubRaster = 0x804981B8u, kSetContext = 0x8049A570u,
              kGetMipLevels = 0x804966D8u, kTokenReached = 0x8049B7DCu, kDestroy = 0x80498058u,
              kSetImage = 0x80494614u;

int g_failures = 0;
bool g_quiet = false;   // a mutation check expects failures: they are counted but not printed
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        if (g_failures <= 80 && !g_quiet) std::printf("FAIL: %s\n", what.c_str());
    }
}
void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}
std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08X", v);
    return b;
}

std::vector<u8> read_file(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), std::string("cannot read ") + path);
    return std::vector<u8>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

// The engine's free function. Original side: PPC words; owner side: the same effect natively.
//   lis r12, 0x8131 ; lwz r11, 0x2000(r12) ; addi r11, r11, 1 ; stw r11, 0x2000(r12) ; stw r3, 0x2004(r12) ; blr
struct Machine {
    GuestMemory bus;
    Cpu cpu{bus};
    NativeInterrupts interrupts{cpu};
    NativeGxSync sync{cpu, interrupts};
    bool native;
    std::set<u32>* trace = nullptr;
    u32 arg6 = 0, arg7 = 0, arg8 = 0;   // r6..r8 of the next run()

    Machine(const std::vector<u8>& dol, bool native_owners) : native(native_owners) {
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(u64(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        bus.write32(kFinish, 0x48000000u);
        test_support::install_os_system_call(bus);
        bus.write32(test_support::kOsCurrentContext, kOsContext);
        cpu.reset();
        sync.install();
        if (native) {
            rw_raster::install(cpu, sync);
        }
        cpu.set_msr(cpu.msr() | kMsrEE | kMsrFP);
        // Every test callback also scribbles below its caller's r1 (stw r3,-8(r1) ; stw r3,-0x20(r1)): an owner that kept a
        // temporary below r1 across a guest_call would read it back wrong and differ from the original.
        const u32 stub[] = {0x3D808131u, 0x816C2000u, 0x396B0001u, 0x916C2000u, 0x906C2004u, 0x9061FFF8u, 0x9061FFE0u, 0x4E800020u};
        for (u32 i = 0; i < 8u; ++i) bus.write32(kFreeStub + i * 4u, stub[i]);
        // The raster-changed hook the engine table names (kNotifyStub): counts its calls (+8) and keeps its r3 (+0xC).
        const u32 notify[] = {0x3D808131u, 0x816C2008u, 0x396B0001u, 0x916C2008u, 0x906C200Cu, 0x908C2038u, 0x9061FFF8u, 0x9061FFE0u, 0x4E800020u};
        for (u32 i = 0; i < 9u; ++i) bus.write32(kNotifyStub + i * 4u, notify[i]);
        const u32 allocate[] = {0x3D808131u, 0x816C2010u, 0x396B0001u, 0x916C2010u, 0x906C2014u, 0x908C2018u, 0x806C201Cu, 0x9061FFF8u, 0x9061FFE0u, 0x4E800020u};
        for (u32 i = 0; i < 10u; ++i) bus.write32(kMallocStub + i * 4u, allocate[i]);
        // DCFlushRange and GXInvalidateTexAll write no guest word (cache and pipe only): both sides stub them, the owner
        // never calls them.
        cpu.install_hle(0x80372534u, [](Cpu&) { return true; }, "test: DCFlushRange");
        cpu.install_hle(0x803987B8u, [](Cpu&) { return true; }, "test: GXInvalidateTexAll");
        bus.set_access_trace([this](u32 address, std::size_t size, bool writing) {
            if (!writing || trace == nullptr) return;
            for (std::size_t i = 0; i < size; ++i) trace->insert((address + static_cast<u32>(i)) & ~3u);
        });
    }

    void setup(u32 token_state, u32 next) {
        cpu.clear_stop();
        cpu.gpr[1] = kStackTop;
        cpu.gpr[2] = kSda2;
        cpu.gpr[13] = kSda;
        cpu.mem_write32(kSda + kSdaPlugin, kPluginOffset);
        cpu.mem_write32(kSda + kSdaEngine, kEngine);
        cpu.mem_write32(kEngine + 0x10Cu, kFreeStub);
        cpu.mem_write32(kSda + 0x617Cu, 0x200u);                 // the hook table's offset inside the engine
        cpu.mem_write32(kEngine + 0x200u + 0x2Cu, kNotifyStub);
        cpu.mem_write32(kFreeLog + 8u, 0u);
        cpu.mem_write32(kFreeLog + 0xCu, 0u);
        cpu.mem_write32(kEngine + 0x108u, kMallocStub);
        cpu.mem_write32(kSda + 0x6100u, 0x300u);   // the engine's error slot (RwErrorSet)
        for (u32 i = 0; i < 0x10u; i += 4u) cpu.mem_write32(kEngine + 0x300u + i, 0u);
        cpu.mem_write32(kEngine + 0x304u, 0x80000000u);   // "no error pending": RwErrorSet only records into this state
        for (u32 i = 0x10u; i < 0x40u; i += 4u) cpu.mem_write32(kFreeLog + i, 0u);
        cpu.mem_write32(kFreeLog + 0x1Cu, kBlock + 0x6000u);   // the allocator answer
        cpu.mem_write32(kSda + kSdaTexturePlugin, kTexturePlugin);
        cpu.mem_write32(kSda + kSdaBound, 0u);
        cpu.mem_write32(kSda + kSdaContext, 0u);
        cpu.mem_write16(kSda + kSdaTokenNext, static_cast<u16>(next));
        cpu.mem_write16(kSda + kSdaTokenRead, 0xA5A5u);
        cpu.mem_write32(kFreeLog, 0u);
        cpu.mem_write32(kFreeLog + 4u, 0u);
        sync.on_draw_sync_token(static_cast<u16>(token_state), false);
        for (u32 i = 0; i < 0x200u; i += 4u) {
            cpu.mem_write32(kRasters + i, 0x11110000u + i);
            cpu.mem_write32(kTexture + i, 0x22220000u + i);
        }
    }

    struct Outcome {
        bool returned = false;
        std::set<u32> written;
        u32 r1 = 0, r3 = 0, lr = 0;
        std::string stop;
    };
    Outcome run(u32 entry, u32 r3, u32 r4, u32 steps_cap = 400000u, u32 r5 = 0u) {
        Outcome o;
        std::set<u32> written;
        trace = &written;
        cpu.gpr[3] = r3;
        cpu.gpr[4] = r4;
        cpu.gpr[5] = r5;
        cpu.gpr[6] = arg6;
        cpu.gpr[7] = arg7;
        cpu.gpr[8] = arg8;
        cpu.pc = entry;
        cpu.npc = entry + 4;
        cpu.lr = kFinish;
        unsigned steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < steps_cap) cpu.run_steps(1u);
        trace = nullptr;
        o.returned = !cpu.stopped() && cpu.pc == kFinish;
        o.r1 = cpu.gpr[1];
        o.r3 = cpu.gpr[3];
        o.lr = cpu.lr;
        o.written = std::move(written);
        if (cpu.stopped()) o.stop = cpu.stop().detail;
        return o;
    }
};

void compare(const std::string& label, Machine& original, Machine& owned, const Machine::Outcome& a, const Machine::Outcome& b,
             bool compare_r3 = true) {
    check(a.returned && b.returned, label + ": both return (" + a.stop + " / " + b.stop + ")");
    if (!(a.returned && b.returned)) return;
    if (compare_r3) check(a.r3 == b.r3, label + ": r3 " + hex(a.r3) + " / " + hex(b.r3));
    check(a.r1 == b.r1 && a.r1 == kStackTop && a.lr == b.lr, label + ": r1 and LR");
    for (u32 r = 14; r < 32u; ++r) check(original.cpu.gpr[r] == owned.cpu.gpr[r], label + ": r" + std::to_string(r));
    std::set<u32> all;
    for (const u32 w : a.written)
        if (!(w >= kStackFloor && w < kStackTop + 8u)) all.insert(w);
    for (const u32 w : b.written)
        if (!(w >= kStackFloor && w < kStackTop + 8u)) all.insert(w);
    for (const u32 w : all)
        check(original.cpu.mem_read32(w) == owned.cpu.mem_read32(w),
              label + ": word " + hex(w) + " " + hex(original.cpu.mem_read32(w)) + " / " + hex(owned.cpu.mem_read32(w)));
    check(original.sync.token() == owned.sync.token(), label + ": draw-sync token");
    check(original.sync.stats().tokens == owned.sync.stats().tokens, label + ": tokens consumed");
    check(original.cpu.mem_read32(kFreeLog) == owned.cpu.mem_read32(kFreeLog) &&
              original.cpu.mem_read32(kFreeLog + 4u) == owned.cpu.mem_read32(kFreeLog + 4u),
          label + ": the free callback's calls");
    check(original.cpu.mem_read32(kFreeLog + 8u) == owned.cpu.mem_read32(kFreeLog + 8u) &&
              original.cpu.mem_read32(kFreeLog + 0xCu) == owned.cpu.mem_read32(kFreeLog + 0xCu),
          label + ": the raster-changed hook's calls");
    for (u32 i = 0x10u; i < 0x40u; i += 4u)
        check(original.cpu.mem_read32(kFreeLog + i) == owned.cpu.mem_read32(kFreeLog + i),
              label + ": the allocator call word " + hex(i));
}

// A raster at kRasters + slot * 0x100: parent, flags, type, format, size and the plugin data.
struct Raster {
    u32 address;
    u32 parent;          // 0 = itself
    u8 type, flags, format;
    u32 width, height;
    u8 mips;             // the platform data's mip byte
    u16 token;
    u32 block;
};
void build(Machine& m, const Raster& r) {
    Cpu& c = m.cpu;
    c.mem_write32(r.address, r.parent != 0u ? r.parent : r.address);
    c.mem_write8(r.address + 0x20u, r.type);
    c.mem_write8(r.address + 0x21u, r.flags);
    c.mem_write8(r.address + 0x23u, r.format);
    c.mem_write32(r.address + 0x0Cu, r.width);
    c.mem_write32(r.address + 0x10u, r.height);
    c.mem_write32(r.address + 0x14u, 16u);
    c.mem_write32(r.address + 0x18u, 0x40u);
    c.mem_write8(r.address + kPluginOffset + 0x32u, r.mips);
    c.mem_write16(r.address + kPluginOffset + 0x30u, r.token);
    c.mem_write32(r.address + kPluginOffset + 0x18u, r.block);
    if (r.parent != 0u) {
        c.mem_write32(r.parent, r.parent);
        c.mem_write8(r.parent + kPluginOffset + 0x32u, r.mips);
    }
}

void test_small(const std::vector<u8>& dol) {
    unsigned cases = 0;
    // GcnTextureSetRaster / SetContext
    for (const u32 raster : {0u, kRasters, 0xFFFFFFF0u & 0x81350000u}) {
        Machine a(dol, false), b(dol, true);
        for (Machine* m : {&a, &b}) m->setup(0, 0);
        const auto x = a.run(kTextureSetRaster, kTexture, raster);
        const auto y = b.run(kTextureSetRaster, kTexture, raster);
        compare("GcnTextureSetRaster " + hex(raster), a, b, x, y);
        check(b.cpu.mem_read32(kTexture) == raster && b.cpu.mem_read32(kTexture + kTexturePlugin + 0x20u) == 0x01000000u,
              "GcnTextureSetRaster stores the raster and the 0x01000000 mark");
        ++cases;
    }
    for (const u32 context : {0u, 0x81234560u, 0xFFFFFFFFu}) {
        Machine a(dol, false), b(dol, true);
        for (Machine* m : {&a, &b}) m->setup(0, 0);
        compare("GcnRasterSetContext " + hex(context), a, b, a.run(kSetContext, 7u, context), b.run(kSetContext, 7u, context));
        check(b.cpu.mem_read32(kSda + kSdaContext) == context, "GcnRasterSetContext stores the context");
        ++cases;
    }
    // GcnRasterSubRaster
    for (const u8 type : {u8{0}, u8{4}, u8{5}, u8{0xFF}}) {
        for (const u8 format : {u8{0}, u8{0x80}, u8{0x0F}}) {
            Machine a(dol, false), b(dol, true);
            for (Machine* m : {&a, &b}) {
                m->setup(0, 0);
                build(*m, {kRasters, 0, type, 0, format, 64, 32, 0xFF, 0, 0});
                m->cpu.mem_write32(kRasters + 0x14u, 0xDEADBEEFu);
                m->cpu.mem_write32(kRasters + 0x18u, 0x12345678u);
                m->cpu.mem_write32(kRasters + 0x100u + 0x04u, 0x99999999u);
            }
            compare("GcnRasterSubRaster type " + std::to_string(type), a, b, a.run(kSubRaster, kRasters + 0x100u, kRasters),
                    b.run(kSubRaster, kRasters + 0x100u, kRasters));
            ++cases;
        }
    }
    // GcnRasterGetMipLevels: the stored byte, and the computed count for the sizes the game uses and the boundaries.
    const u32 sizes[] = {0, 1, 2, 3, 4, 7, 8, 9, 15, 16, 31, 32, 63, 64, 100, 128, 255, 256, 480, 512, 640, 1000, 1024, 2048, 0x7FFFFFFF};
    for (const u8 mips : {u8{0xFF}, u8{0}, u8{3}, u8{0xFE}}) {
        for (const u8 format : {u8{0}, u8{0x80}, u8{0x81}, u8{0x7F}}) {
            for (const u32 w : sizes) {
                for (const u32 h : {0u, 1u, 5u, 64u, 640u}) {
                    Machine a(dol, false), b(dol, true);
                    for (Machine* m : {&a, &b}) {
                        m->setup(0, 0);
                        build(*m, {kRasters, 0, 4, 0, format, w, h, mips, 0, 0});
                        m->cpu.mem_write32(kBlock + 0x80u, 0xCCCCCCCCu);
                    }
                    compare("GcnRasterGetMipLevels w " + std::to_string(w) + " h " + std::to_string(h) + " mips " + std::to_string(mips) +
                                " format " + hex(format),
                            a, b, a.run(kGetMipLevels, kBlock + 0x80u, kRasters), b.run(kGetMipLevels, kBlock + 0x80u, kRasters));
                    ++cases;
                }
            }
        }
    }
    // A sub-raster asks its parent's mip byte.
    {
        Machine a(dol, false), b(dol, true);
        for (Machine* m : {&a, &b}) {
            m->setup(0, 0);
            build(*m, {kRasters + 0x100u, kRasters, 4, 0, 0x80, 64, 64, 5, 0, 0});
            m->cpu.mem_write32(kBlock + 0x80u, 0u);
        }
        compare("GcnRasterGetMipLevels (sub-raster)", a, b, a.run(kGetMipLevels, kBlock + 0x80u, kRasters + 0x100u),
                b.run(kGetMipLevels, kBlock + 0x80u, kRasters + 0x100u));
        check(b.cpu.mem_read32(kBlock + 0x80u) == 6u, "a sub-raster reports its parent's mip byte + 1");
        ++cases;
    }
    // A negative size never ends in the original; the owner refuses.
    {
        Machine a(dol, false), b(dol, true);
        for (Machine* m : {&a, &b}) {
            m->setup(0, 0);
            build(*m, {kRasters, 0, 4, 0, 0x80, 0xFFFFFFF0u, 0xFFFFFFF0u, 0xFF, 0, 0});
        }
        const auto x = a.run(kGetMipLevels, kBlock + 0x80u, kRasters, 100000u);
        const auto y = b.run(kGetMipLevels, kBlock + 0x80u, kRasters, 100000u);
        check(!x.returned && !y.returned && y.stop.find("negative size") != std::string::npos,
              "a negative raster size: the original does not return and the owner refuses (" + y.stop + ")");
    }
    std::printf("  small rows: %u cases (the table words, sub-raster copy, mip levels) equal\n", cases);
}

void test_token_reached(const std::vector<u8>& dol) {
    unsigned cases = 0, reached = 0, waiting = 0;
    const u32 currents[] = {0, 1, 2, 0x1234, 0x7FFF, 0xDFFE, 0xDFFF, 0xE000, 0xE001, 0xFFFF};
    const u32 nexts[] = {0, 1, 2, 0x1233, 0x1234, 0x1235, 0x7FFF, 0xDFFE, 0xDFFF, 0xE000, 0xFFFF};
    const u32 tokens[] = {0, 1, 2, 3, 0x1233, 0x1234, 0x1235, 0x2000, 0x7FFF, 0x8000, 0xDFFE, 0xDFFF, 0xE000, 0xFFFE, 0xFFFF};
    for (const u32 cur : currents) {
        for (const u32 next : nexts) {
            for (const u32 token : tokens) {
                Machine a(dol, false), b(dol, true);
                for (Machine* m : {&a, &b}) m->setup(cur, next);
                const auto x = a.run(kTokenReached, token, 0);
                const auto y = b.run(kTokenReached, token, 0);
                compare("token reached cur " + hex(cur) + " next " + hex(next) + " token " + hex(token), a, b, x, y);
                check(a.cpu.mem_read16(kSda + kSdaTokenRead) == static_cast<u16>(cur) && b.cpu.mem_read16(kSda + kSdaTokenRead) == static_cast<u16>(cur),
                      "SDA 0x6220 holds the token read");
                (y.r3 != 0u ? reached : waiting)++;
                ++cases;
            }
        }
    }
    check(reached > 100 && waiting > 100, "the grid covers reached and waiting tokens");
    std::printf("  draw-sync token test: %u cases (%u reached, %u waiting) equal\n", cases, reached, waiting);
}

void test_destroy(const std::vector<u8>& dol) {
    unsigned cases = 0, frees = 0, issued = 0;
    const u32 nexts[] = {0, 1, 5, 0x1234, 0xDFFE, 0xDFFF};   // the ring counts below 0xE000
    for (const u8 type : {u8{0}, u8{1}, u8{2}, u8{4}, u8{5}}) {
        for (const u8 flags : {u8{0}, u8{0x80}, u8{0x7F}}) {
            for (const bool own_parent : {true, false}) {
                for (const u32 next : nexts) {
                    for (const int token_kind : {0, 1, 2}) {   // the raster's token: == next, older (reached), 0
                        for (const int bound_kind : {0, 1, 2}) {   // no binding, bound to this raster, bound to another
                            const u32 token = token_kind == 0 ? next : token_kind == 1 ? (next > 2u ? next - 2u : 0u) : 0u;
                            // the sync token the GPU has reached: everything issued so far
                            const u32 reached_token = next > 0u ? next - 1u : 0u;
                            Machine a(dol, false), b(dol, true);
                            for (Machine* m : {&a, &b}) {
                                m->setup(reached_token, next);
                                build(*m, {kRasters, own_parent ? 0u : kRasters + 0x100u, type, flags, 0, 64, 64, 3,
                                           static_cast<u16>(token), kBlock + 0x200u});
                                build(*m, {kRasters + 0x100u, 0, type, flags, 0, 64, 64, 3, 0, 0});
                                const u32 slot = kBlock + 0x300u;
                                if (bound_kind != 0) {
                                    m->cpu.mem_write32(kSda + kSdaBound, slot);
                                    m->cpu.mem_write32(slot, bound_kind == 1 ? kRasters : kRasters + 0x100u);
                                    m->cpu.mem_write32(kTexture, bound_kind == 1 ? kRasters : kRasters + 0x100u);
                                }
                                // The word the binding's reset writes: slot + the texture plugin offset + 0x20.
                                m->cpu.mem_write32(slot + kTexturePlugin + 0x20u, 0xABCDEF01u);
                            }
                            const std::string label = "GcnRasterDestroy type " + std::to_string(type) + " flags " + hex(flags) +
                                                      (own_parent ? " own" : " sub") + " next " + hex(next) + " token " + hex(token) +
                                                      " bound " + std::to_string(bound_kind);
                            const auto x = a.run(kDestroy, 0u, kRasters);
                            const auto y = b.run(kDestroy, 0u, kRasters);
                            compare(label, a, b, x, y);
                            frees += b.cpu.mem_read32(kFreeLog);
                            issued += static_cast<u32>(b.sync.stats().tokens > 0u);
                            ++cases;
                        }
                    }
                }
            }
        }
    }
    check(frees > 50 && issued > 50, "the grid frees blocks and issues tokens");
    std::printf("  GcnRasterDestroy: %u cases (%u frees, %u draw-sync issues) equal\n", cases, frees, issued);

    // An error type (3, 6, 7, 0xFF): the original reports it through RwErrorSet and answers 0; so does the owner (the error
    // slot words, r3 and every other write are compared).
    for (const u8 type : {u8{3}, u8{6}, u8{7}, u8{0xFF}}) {
        for (const u32 preset : {0u, 0x77u}) {   // the engine's error slot empty, or already holding an error (kept)
            Machine a(dol, false), b(dol, true);
            for (Machine* m : {&a, &b}) {
                m->setup(0, 0);
                build(*m, {kRasters, 0, type, 0, 0, 64, 64, 3, 0, kBlock + 0x200u});
                m->cpu.mem_write32(kEngine + 0x300u, preset);
                if (preset != 0u) m->cpu.mem_write32(kEngine + 0x304u, 0x12345u);   // an earlier error stays
            }
            const auto x = a.run(kDestroy, 0u, kRasters);
            const auto y = b.run(kDestroy, 0u, kRasters);
            compare("GcnRasterDestroy error type " + std::to_string(type) + " preset " + hex(preset), a, b, x, y);
            check(x.returned && x.r3 == 0u && (preset != 0u ? a.cpu.mem_read32(kEngine + 0x304u) == 0x12345u : a.cpu.mem_read32(kEngine + 0x304u) == 0x8000000Du),
                  "the original answers 0 and records the error code 0x8000000D");
        }
    }
    // A token the original waits for forever (outstanding, never completed): the owner stops, the original does not return.
    {
        Machine a(dol, false), b(dol, true);
        for (Machine* m : {&a, &b}) {
            m->setup(10, 20);   // the GPU is at 10, the next to issue is 20
            build(*m, {kRasters, 0, 4, 0, 0, 64, 64, 3, 15, kBlock + 0x200u});   // waits for 15, in (10, 20]
        }
        const auto x = a.run(kDestroy, 0u, kRasters, 100000u);
        const auto y = b.run(kDestroy, 0u, kRasters, 100000u);
        check(!x.returned && !y.returned && y.stop.find("not reached") != std::string::npos,
              "a token never reached: the original spins and the owner refuses (" + y.stop + ")");
    }
}

// The pixel layout rows: GcnRasterTile 0x8049677C, GcnRasterUntile 0x80496A10, GcnRasterLevelBytes 0x80496478.
void test_layout(const std::vector<u8>& dol) {
    constexpr u32 kTile = 0x8049677Cu, kUntile = 0x80496A10u, kBytes = 0x80496478u;
    unsigned cases = 0, moved = 0;
    const std::pair<u32, u32> sizes[] = {{1, 1}, {4, 4}, {8, 8}, {16, 8}, {20, 12}, {33, 9}, {64, 32}, {7, 5}, {0, 4}, {4, 0}};
    for (const u32 depth : {4u, 8u, 16u, 32u}) {
        for (const auto& size : sizes) {
            for (const u32 extra : {0u, 8u}) {   // padding at the end of a linear row
                const u32 stride = (depth == 4u ? (size.first + 1u) / 2u : size.first * depth / 8u) + extra;
                for (const u32 entry : {kTile, kUntile}) {
                    Machine a(dol, false), b(dol, true);
                    for (Machine* m : {&a, &b}) {
                        m->setup(0, 0);
                        for (u32 i = 0; i < 0x2000u; i += 4u) {
                            m->cpu.mem_write32(kBlock + 0x2000u + i, 0x01020304u * (i / 4u + 1u) + 0x10101010u);
                            m->cpu.mem_write32(kBlock + 0x6000u + i, 0xEEEEEEEEu);
                        }
                        // kTile: r3 tiles (0x6000), r4 linear (0x2000). kUntile: r3 linear (0x6000), r4 tiles (0x2000).
                        m->arg6 = size.second;
                        m->arg7 = depth;
                        m->arg8 = stride;
                    }
                    const std::string label = std::string(entry == kTile ? "GcnRasterTile" : "GcnRasterUntile") + " depth " + std::to_string(depth) + " " +
                                              std::to_string(size.first) + "x" + std::to_string(size.second) + " stride " + std::to_string(stride);
                    compare(label, a, b, a.run(entry, kBlock + 0x6000u, kBlock + 0x2000u, 4000000u, size.first),
                            b.run(entry, kBlock + 0x6000u, kBlock + 0x2000u, 4000000u, size.first), false);
                    unsigned written = 0;
                    for (u32 i = 0; i < 0x400u; i += 4u) written += b.cpu.mem_read32(kBlock + 0x6000u + i) != 0xEEEEEEEEu ? 1u : 0u;
                    moved += written;
                    ++cases;
                }
            }
        }
    }
    // Level byte sizes: whole-raster answers (lock flags set) and per-level answers.
    for (const u32 depth : {4u, 8u, 16u, 32u}) {
        for (const u8 priv : {u8{0}, u8{2}, u8{4}, u8{6}, u8{0x20}}) {
            for (const u32 level : {0u, 1u, 2u, 5u, 0x7Fu, 0x25u, 0x1FFu}) {
                for (const auto& size : sizes) {
                    Machine a(dol, false), b(dol, true);
                    for (Machine* m : {&a, &b}) {
                        m->setup(0, 0);
                        build(*m, {kRasters, 0, 4, 0, 0x80, size.first, size.second, 3, 0, 0});
                        m->cpu.mem_write32(kRasters + 0x14u, depth);
                        m->cpu.mem_write8(kRasters + 0x22u, priv);
                    }
                    compare("GcnRasterLevelBytes depth " + std::to_string(depth) + " priv " + hex(priv) + " level " + std::to_string(level) + " " +
                                std::to_string(size.first) + "x" + std::to_string(size.second),
                            a, b, a.run(kBytes, kRasters, level), b.run(kBytes, kRasters, level));
                    ++cases;
                }
            }
        }
    }
    check(moved > 2000, "the pixel layout grid moves pixels (" + std::to_string(moved) + " words)");
    std::printf("  pixel layout rows: %u cases (tiles, untiles, level sizes) equal\n", cases);
    // A depth the original reports (through RwErrorSet, then it returns): exact, error slot included.
    for (const u32 entry : {kTile, kUntile, kBytes}) {
        Machine a(dol, false), b(dol, true);
        for (Machine* m : {&a, &b}) {
            m->setup(0, 0);
            build(*m, {kRasters, 0, 4, 0, 0x80, 16, 8, 3, 0, 0});
            m->cpu.mem_write32(kRasters + 0x14u, 24u);
            m->arg6 = 8;
            m->arg7 = 24;
            m->arg8 = 32;
        }
        const u32 r3 = entry == kBytes ? kRasters : kBlock + 0x6000u;
        const auto x = a.run(entry, r3, entry == kBytes ? 0u : kBlock + 0x2000u, 400000u, 16);
        const auto y = b.run(entry, r3, entry == kBytes ? 0u : kBlock + 0x2000u, 400000u, 16);
        compare("a pixel layout row of depth 24", a, b, x, y, entry == kBytes);
        check(x.returned && a.cpu.mem_read32(kEngine + 0x304u) == 0x8000000Cu, "the original records the depth error 0x8000000C");
    }
}

// GcnRasterFormat 0x80497458 and GcnRasterCreate 0x80497CA8: the original reports some combinations through RwErrorSet and
// answers 0 (or does not return in this bare machine); for each such case the owner must stop. Every other case is
// compared word for word.
void test_create(const std::vector<u8>& dol) {
    constexpr u32 kFormat = 0x80497458u, kCreate = 0x80497CA8u;
    unsigned cases = 0, refused = 0, allocated = 0, palettes = 0;
    const auto make = [&](Machine& m, u32 depth, u32 w, u32 h, u32 flag, u32 allocator_result) {
        m.setup(0, 0);
        for (u32 i = 0; i < 0x40u; i += 4u) m.cpu.mem_write32(kRasters + 0x40u + i, 0x5A5A5A5Au);
        m.cpu.mem_write32(kRasters, kRasters);
        m.cpu.mem_write32(kRasters + 0x0Cu, w);
        m.cpu.mem_write32(kRasters + 0x10u, h);
        m.cpu.mem_write32(kRasters + 0x14u, depth);
        m.cpu.mem_write32(kRasters + 0x18u, 0x77u);
        m.cpu.mem_write8(kRasters + 0x20u, 0x7Eu);
        m.cpu.mem_write8(kRasters + 0x21u, 0x7Fu);
        m.cpu.mem_write8(kRasters + 0x22u, 0x66u);
        m.cpu.mem_write8(kRasters + 0x23u, 0x55u);
        m.cpu.mem_write32(kSda + 0x61F4u, kBlock + 0x7000u);
        m.cpu.mem_write8(kBlock + 0x7000u + 0x19u, static_cast<u8>(flag));
        m.cpu.mem_write16(kSda + kSdaTokenRead, 0x1234u);
        m.cpu.mem_write32(kFreeLog + 0x1Cu, allocator_result);
        for (u32 i = 0; i < 0x1000u; i += 4u) m.cpu.mem_write32(kBlock + 0x6000u + i, 0xEEEEEEEEu);
    };
    // Run both; the original's error (returns 0 or never returns) must be the owner's refusal, anything else must be equal.
    const auto both = [&](const std::string& label, u32 entry, u32 r3, u32 r4, u32 r5, u32 depth, u32 w, u32 h, u32 flag, u32 alloc) {
        Machine a(dol, false), b(dol, true);
        make(a, depth, w, h, flag, alloc);
        make(b, depth, w, h, flag, alloc);
        const auto x = a.run(entry, r3, r4, 400000u, r5);
        const auto y = b.run(entry, r3, r4, 400000u, r5);
        if (!x.returned) {   // the original does not end (nothing here): the owner must not return either
            check(!y.returned, label + ": the original does not return and the owner stops (" + y.stop + ")");
            ++refused;
        } else {
            if (x.r3 == 0u) ++refused;   // an error the original reports and answers 0 for: compared below
            compare(label, a, b, x, y);
            ++cases;
            allocated += b.cpu.mem_read32(kFreeLog + 0x10u);
            palettes += (b.cpu.mem_read8(kRasters + 0x23u) & 0x60u) != 0u && b.cpu.mem_read32(kFreeLog + 0x10u) != 0u ? 1u : 0u;
        }
    };
    // GcnRasterFormat(raster, word)
    for (u32 type = 0; type < 8u; ++type) {
        for (const u32 low : {0x00u, 0x80u}) {
            for (const u32 code : {0x000u, 0x100u, 0x200u, 0x300u, 0x400u, 0x500u, 0x600u, 0x700u, 0x800u, 0x900u, 0xA00u, 0xB00u}) {
                for (const u32 extra : {0x0000u, 0x1000u, 0x2000u, 0x4000u, 0x6000u, 0x8000u}) {
                    for (const u32 depth : {0u, 4u, 8u, 16u, 32u, 5u}) {
                        if (code != 0u && depth != 16u) continue;
                        for (const u32 flag : {0u, 1u}) {
                            if (code != 0u && type != 1u && type != 2u && flag == 1u) continue;
                            const u32 word = type | low | code | extra;
                            both("GcnRasterFormat word " + hex(word) + " depth " + std::to_string(depth) + " flag " + std::to_string(flag), kFormat, kRasters,
                                 word, 0u, depth, 16, 8, flag, 0u);
                        }
                    }
                }
            }
        }
    }
    // GcnRasterCreate(_, raster, word): sizes, mip chains, palettes, the allocator's refusal.
    const std::pair<u32, u32> sizes[] = {{16, 8}, {20, 12}, {64, 32}, {1, 1}, {0, 8}, {8, 0}, {128, 64}};
    for (const u32 type : {0u, 4u, 5u, 1u, 2u}) {
        for (const u32 code : {0x000u, 0x100u, 0x200u, 0xA00u, 0x500u, 0x600u, 0x700u}) {
            for (const u32 extra : {0x0000u, 0x2000u, 0x4000u, 0x8000u, 0xA000u, 0xC000u}) {
                for (const u32 depth : {0u, 4u, 8u, 16u, 32u}) {
                    if (code != 0u && depth != 16u) continue;
                    for (const auto& size : sizes) {
                        for (const u32 alloc : {kBlock + 0x6000u, 0u}) {
                            if (alloc == 0u && size.first != 16u) continue;
                            for (const u32 low : {0u, 0x80u}) {
                                const u32 word = type | low | code | extra;
                                both("GcnRasterCreate word " + hex(word) + " depth " + std::to_string(depth) + " " + std::to_string(size.first) + "x" +
                                         std::to_string(size.second) + " alloc " + hex(alloc),
                                     kCreate, 0u, kRasters, word, depth, size.first, size.second, 1u, alloc);
                            }
                        }
                    }
                }
            }
        }
    }
    // GcnRasterSetMipLevels(raster, count) 0x80497A70: the level count from the caller, the parent's byte, or the format.
    {
        constexpr u32 kSetLevels = 0x80497A70u;
        unsigned set_cases = 0;
        for (const u32 format : {0x00u, 0x80u, 0x20u, 0x40u, 0xA0u, 0xC0u}) {
            for (const u32 depth : {4u, 8u, 16u, 32u}) {
                for (const auto& size : sizes) {
                    for (const u32 count : {0u, 1u, 2u, 3u, 5u, 0x101u}) {
                        for (const u32 stored : {0xFFu, 0u, 4u}) {
                            for (const u32 alloc : {kBlock + 0x6000u, 0u}) {
                                if (alloc == 0u && size.first != 16u) continue;
                                Machine a(dol, false), b(dol, true);
                                for (Machine* m : {&a, &b}) {
                                    make(*m, depth, size.first, size.second, 1u, alloc);
                                    m->cpu.mem_write8(kRasters + 0x23u, static_cast<u8>(format));
                                    m->cpu.mem_write8(kRasters + 0x22u, 0u);
                                    m->cpu.mem_write8(kRasters + kPluginOffset + 0x32u, static_cast<u8>(stored));
                                }
                                const std::string label = "GcnRasterSetMipLevels format " + hex(format) + " depth " + std::to_string(depth) + " " +
                                                          std::to_string(size.first) + "x" + std::to_string(size.second) + " count " + std::to_string(count) +
                                                          " stored " + hex(stored) + " alloc " + hex(alloc);
                                const auto x = a.run(kSetLevels, kRasters, count, 400000u);
                                const auto y = b.run(kSetLevels, kRasters, count, 400000u);
                                if (!x.returned) {
                                    check(!y.returned, label + ": the original does not return and the owner stops");
                                } else {
                                    compare(label, a, b, x, y);
                                    ++set_cases;
                                }
                            }
                        }
                    }
                }
            }
        }
        check(set_cases > 1000, "the set-levels grid ran (" + std::to_string(set_cases) + ")");
        std::printf("  GcnRasterSetMipLevels: %u cases equal\n", set_cases);
    }
    check(cases > 1500 && refused > 100 && allocated > 300 && palettes > 20,
          "the create grid covers formats, allocations and palettes (" + std::to_string(cases) + " / " + std::to_string(refused) + " / " +
              std::to_string(allocated) + " / " + std::to_string(palettes) + ")");
    std::printf("  GcnRasterFormat / GcnRasterCreate: %u cases equal (%u allocations, %u palette rasters; %u of them answer an error, reported exactly)\n",
                cases, allocated, palettes, refused);
}

// The core's RwRasterLock(raster, level, mode) / RwRasterUnlock(raster) the clear calls: test doubles (installed on both sides by
// the clear test only; they log their arguments at +0x20.. / +0x30.., a write lock marks the raster and hands out the pixel area).
void install_core_lock_doubles(Machine& m) {
    m.cpu.install_hle(0x8048AFC0u, [](Cpu& c) {
            const u32 raster = c.gpr[3];
            c.mem_write32(kFreeLog + 0x20u, c.mem_read32(kFreeLog + 0x20u) + 1u);
            c.mem_write32(kFreeLog + 0x24u, raster);
            c.mem_write32(kFreeLog + 0x28u, c.gpr[4]);
            c.mem_write32(kFreeLog + 0x2Cu, c.gpr[5]);
            if ((c.gpr[5] & 1u) != 0u) c.mem_write8(raster + 0x22u, static_cast<u8>(c.mem_read8(raster + 0x22u) | 4u));
            c.mem_write32(raster + 4u, kBlock + 0x3000u);
            c.gpr[3] = kBlock + 0x3000u;
            return true;
        }, "test: RwRasterLock");
    m.cpu.install_hle(0x8048AB90u, [](Cpu& c) {
            const u32 raster = c.gpr[3];
            c.mem_write32(kFreeLog + 0x30u, c.mem_read32(kFreeLog + 0x30u) + 1u);
            c.mem_write32(kFreeLog + 0x34u, raster);
            c.mem_write8(raster + 0x22u, static_cast<u8>(c.mem_read8(raster + 0x22u) & ~4u));
            c.gpr[3] = 1u;
            return true;
        }, "test: RwRasterUnlock");
}

// GcnNativeTextureRead 0x8049A8DC: the original words against the owner, with the RenderWare core calls it makes as test
// doubles on both sides (FindChunk, StreamRead from a byte buffer, RasterCreate, RasterDestroy, TextureCreate, the texture
// hook, SetName, and the LOD step 0x8049B568 / the owner's TextureLoadedFn). The doubles log into guest memory
// (kFreeLog + 0x100..), so `compare` sees every call's arguments; GcnRasterSetMipLevels is the real one on both sides.
constexpr u32 kStreamData = kBlock + 0x9000u, kScenario = kBlock + 0x8F00u;

void install_texture_doubles(Machine& m) {
    m.cpu.install_hle(0x8047DFA0u, [](Cpu& c) {   // RwStreamFindChunk(stream, type, &length, &version)
        c.mem_write32(kFreeLog + 0x100u, c.mem_read32(kFreeLog + 0x100u) + 1u);
        c.mem_write32(kFreeLog + 0x104u, c.gpr[4]);
        c.mem_write32(kFreeLog + 0x160u, c.gpr[3]);   // the stream
        c.mem_write32(c.gpr[5], 0x1234u);
        c.mem_write32(c.gpr[6], c.mem_read32(kScenario + 4u));
        c.gpr[3] = c.mem_read32(kScenario);
        return true;
    }, "test: RwStreamFindChunk");
    m.cpu.install_hle(0x80481D9Cu, [](Cpu& c) {   // RwStreamRead(stream, dst, n): from kStreamData, short at the scenario's read
        const u32 dst = c.gpr[4], n = c.gpr[5];
        const u32 index = c.mem_read32(kFreeLog + 0x110u), cursor = c.mem_read32(kFreeLog + 0x114u);
        c.mem_write32(kFreeLog + 0x108u, c.mem_read32(kFreeLog + 0x108u) + 1u);
        c.mem_write32(kFreeLog + 0x10Cu, n);
        c.mem_write32(kFreeLog + 0x164u, c.gpr[3]);   // the stream
        c.mem_write32(kFreeLog + 0x110u, index + 1u);
        u32 got = n;
        if (index == c.mem_read32(kScenario + 8u)) got = n > 0u ? n - 1u : 0u;
        for (u32 i = 0; i < got; ++i) c.mem_write8(dst + i, c.mem_read8(kStreamData + cursor + i));
        c.mem_write32(kFreeLog + 0x114u, cursor + got);
        c.gpr[3] = got;
        return true;
    }, "test: RwStreamRead");
    m.cpu.install_hle(0x8048AEC4u, [](Cpu& c) {   // RwRasterCreate(width, height, depth, flags)
        c.mem_write32(kFreeLog + 0x118u, c.mem_read32(kFreeLog + 0x118u) + 1u);
        c.mem_write32(kFreeLog + 0x11Cu, c.gpr[3]);
        c.mem_write32(kFreeLog + 0x120u, c.gpr[4]);
        c.mem_write32(kFreeLog + 0x124u, c.gpr[5]);
        c.mem_write32(kFreeLog + 0x128u, c.gpr[6]);
        if (c.mem_read32(kScenario + 0xCu) != 0u) {
            c.gpr[3] = 0u;
            return true;
        }
        const u32 r = kRasters;
        for (u32 i = 0; i < 0x80u; i += 4u) c.mem_write32(r + i, 0u);
        c.mem_write32(r, r);
        c.mem_write32(r + 0x0Cu, c.gpr[3]);
        c.mem_write32(r + 0x10u, c.gpr[4]);
        c.mem_write32(r + 0x14u, c.gpr[5]);
        c.mem_write8(r + 0x20u, 4u);
        c.mem_write8(r + 0x21u, static_cast<u8>(c.gpr[6]));
        c.mem_write8(r + 0x23u, static_cast<u8>(c.mem_read32(kScenario + 0x14u)));
        c.mem_write8(r + kPluginOffset + 0x32u, 0xFFu);
        c.mem_write32(r + kPluginOffset + 0x20u, kBlock + 0xA000u);   // where a palette is read
        c.gpr[3] = r;
        return true;
    }, "test: RwRasterCreate");
    m.cpu.install_hle(0x8048AC2Cu, [](Cpu& c) {   // RwRasterDestroy(raster)
        c.mem_write32(kFreeLog + 0x12Cu, c.mem_read32(kFreeLog + 0x12Cu) + 1u);
        c.mem_write32(kFreeLog + 0x138u, c.gpr[3]);
        c.gpr[3] = 1u;
        return true;
    }, "test: RwRasterDestroy");
    m.cpu.install_hle(0x8048DD2Cu, [](Cpu& c) {   // RwTextureCreate(raster)
        c.mem_write32(kFreeLog + 0x130u, c.mem_read32(kFreeLog + 0x130u) + 1u);
        c.mem_write32(kFreeLog + 0x168u, c.gpr[3]);   // the raster
        if (c.mem_read32(kScenario + 0x10u) != 0u) {
            c.gpr[3] = 0u;
            return true;
        }
        c.mem_write32(kTexture + 0x50u, 0xAABBCCDDu);
        c.mem_write32(kTexture, c.gpr[3]);
        c.gpr[3] = kTexture;
        return true;
    }, "test: RwTextureCreate");
    // RwTextureSetName(texture, name) and RwTextureSetMaskName(texture, name): the texture and the 32 bytes of the name each
    // is handed (the name lives in the owner's frame, so its bytes are what identifies the argument)
    const auto name_double = [](u32 at) {
        return [at](Cpu& c) {
            c.mem_write32(kFreeLog + at, c.mem_read32(kFreeLog + at) + 1u);
            c.mem_write32(kFreeLog + at + 4u, c.gpr[3]);
            for (u32 i = 0; i < 32u; ++i) c.mem_write8(kFreeLog + at + 8u + i, c.mem_read8(c.gpr[4] + i));
            return true;
        };
    };
    m.cpu.install_hle(0x8048DEA4u, name_double(0x170u), "test: RwTextureSetName");
    m.cpu.install_hle(0x8048DF50u, name_double(0x1A0u), "test: RwTextureSetMaskName");
}

// The original's LOD step 0x8049B568 as a double: it logs its arguments (texture, bias clamp, edge LOD, max aniso, the LOD
// bias as binary32) exactly where the owner's TextureLoadedFn double does.
bool original_lod_double(Cpu& c) {
    double d;
    const std::uint64_t raw = c.fpr[1].bits[0];
    std::memcpy(&d, &raw, sizeof(d));
    float f = static_cast<float>(d);
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    c.mem_write32(kFreeLog + 0x148u, c.mem_read32(kFreeLog + 0x148u) + 1u);
    c.mem_write32(kFreeLog + 0x14Cu, c.gpr[3]);
    c.mem_write32(kFreeLog + 0x150u, c.gpr[4]);
    c.mem_write32(kFreeLog + 0x154u, c.gpr[5]);
    c.mem_write32(kFreeLog + 0x158u, c.gpr[6]);
    c.mem_write32(kFreeLog + 0x15Cu, bits);
    return true;
}

void test_texture_read(const std::vector<u8>& dol) {
    constexpr u32 kRead = 0x8049A8DCu;
    unsigned cases = 0, complete = 0;
    static const auto loaded = [](Cpu& c, std::uint32_t texture, std::uint32_t bias_clamp, std::uint32_t edge_lod, std::uint32_t max_aniso,
                                  double lod_bias) -> std::uint32_t {
        float f = static_cast<float>(lod_bias);
        std::uint32_t bits;
        std::memcpy(&bits, &f, sizeof(bits));
        c.mem_write32(kFreeLog + 0x148u, c.mem_read32(kFreeLog + 0x148u) + 1u);
        c.mem_write32(kFreeLog + 0x14Cu, texture);
        c.mem_write32(kFreeLog + 0x150u, bias_clamp);
        c.mem_write32(kFreeLog + 0x154u, edge_lod);
        c.mem_write32(kFreeLog + 0x158u, max_aniso);
        c.mem_write32(kFreeLog + 0x15Cu, bits);
        return 0u;
    };
    const auto build_stream = [&](Machine& m, u32 platform, u32 w, u32 h, u32 depth, u32 levels, u32 pal_flag, u32 size, bool palette,
                                  u32 flags_word, u32 filter, float lod) {
        Cpu& c = m.cpu;
        for (u32 i = 0; i < 0x600u; i += 4u) c.mem_write32(kStreamData + i, 0xC0DE0000u + i);
        c.mem_write32(kStreamData + 0x00u, platform);
        c.mem_write32(kStreamData + 0x04u, filter);
        c.mem_write32(kStreamData + 0x08u, 0x11u);   // max aniso (sp2c)
        c.mem_write32(kStreamData + 0x0Cu, 0x22u);   // bias clamp (sp30)
        c.mem_write32(kStreamData + 0x10u, 0x33u);   // edge lod (sp34)
        std::uint32_t lod_bits;
        std::memcpy(&lod_bits, &lod, sizeof(lod_bits));
        c.mem_write32(kStreamData + 0x14u, lod_bits);   // the LOD bias (sp38)
        for (u32 i = 0; i < 32u; ++i) {
            c.mem_write8(kStreamData + 0x18u + i, static_cast<u8>(0x41u + i));   // the texture's name (frame + 0x3C): 'A'..
            c.mem_write8(kStreamData + 0x38u + i, static_cast<u8>(0x61u + i));   // its mask name (frame + 0x5C): 'a'..
        }
        c.mem_write32(kStreamData + 0x58u, flags_word);
        c.mem_write16(kStreamData + 0x5Cu, static_cast<u16>(w));
        c.mem_write16(kStreamData + 0x5Eu, static_cast<u16>(h));
        c.mem_write8(kStreamData + 0x60u, static_cast<u8>(depth));
        c.mem_write8(kStreamData + 0x61u, static_cast<u8>(levels));
        c.mem_write8(kStreamData + 0x62u, 6u);    // texture format
        c.mem_write8(kStreamData + 0x63u, 2u);    // palette format
        c.mem_write32(kStreamData + 0x64u, pal_flag);
        const u32 palette_bytes = palette ? ((depth >= 32u ? 0u : (1u << depth)) << 1) : 0u;
        c.mem_write32(kStreamData + 0x68u + palette_bytes, size);
    };
    for (const u32 platform : {6u, 5u}) {
        for (const u32 version : {0x34FFFu, 0x35000u, 0x36003u, 0x37002u, 0x37003u}) {
            for (const u32 found : {1u, 0u}) {
                for (const u32 format : {0x00u, 0x10u, 0x20u, 0x40u, 0x70u}) {   // none, mip bit, palette variants
                    for (const u32 depth : {4u, 8u, 16u, 32u}) {
                        for (const int short_at : {-1, 0, 1, 2, 3, 4, 5}) {
                            for (const u32 raster_null : {0u, 1u}) {
                                for (const u32 texture_null : {0u, 1u}) {
                                    for (const u32 alloc : {kBlock + 0x6000u, 0u}) {
                                        if ((raster_null != 0u || texture_null != 0u || alloc == 0u) && (version != 0x35000u || format != 0x10u || depth != 16u)) continue;
                                        if (depth == 32u && format >= 0x20u) continue;
                                        const bool palette = (format & 0x60u) != 0u;
                                        Machine a(dol, false), b(dol, true);
                                        for (Machine* m : {&a, &b}) {
                                            m->setup(0, 0);
                                            install_texture_doubles(*m);
                                            for (u32 i = 0; i < 0x20u; i += 4u) m->cpu.mem_write32(kScenario + i, 0u);
                                            m->cpu.mem_write32(kScenario, found);
                                            m->cpu.mem_write32(kScenario + 4u, version);
                                            m->cpu.mem_write32(kScenario + 8u, static_cast<u32>(short_at));
                                            m->cpu.mem_write32(kScenario + 0xCu, raster_null);
                                            m->cpu.mem_write32(kScenario + 0x10u, texture_null);
                                            m->cpu.mem_write32(kScenario + 0x14u, format);
                                            m->cpu.mem_write32(kFreeLog + 0x1Cu, alloc);
                                            for (u32 i = 0x100u; i < 0x180u; i += 4u) m->cpu.mem_write32(kFreeLog + i, 0u);
                                            m->cpu.mem_write32(kBlock + 0x80u, 0xCCCCCCCCu);
                                            for (u32 i = 0; i < 0x800u; i += 4u) m->cpu.mem_write32(kBlock + 0xA000u + i, 0xEEEEEEEEu);
                                            for (u32 i = 0; i < 0x800u; i += 4u) m->cpu.mem_write32(kBlock + 0x6000u + i, 0xEEEEEEEEu);
                                            build_stream(*m, platform, 16u, 8u, depth, 3u, 1u, 0x40u, palette, 0x300u, 0x00001234u, 1.5f);
                                        }
                                        b.cpu.install_hle(0x8049B568u, [](Cpu&) { return true; }, "unused");   // (the original's LOD step is the double below)
                                        a.cpu.install_hle(0x8049B568u, [](Cpu& c) { return original_lod_double(c); }, "test: the LOD step 0x8049B568");
                                        rw_raster::install_lifted(b.cpu, loaded);
                                        const std::string label = "GcnNativeTextureRead platform " + std::to_string(platform) + " version " + hex(version) +
                                                                  " found " + std::to_string(found) + " format " + hex(format) + " depth " + std::to_string(depth) +
                                                                  " short_at " + std::to_string(short_at) + " raster_null " + std::to_string(raster_null) +
                                                                  " texture_null " + std::to_string(texture_null) + " alloc " + hex(alloc);
                                        const auto x = a.run(kRead, kBlock + 0x7000u, kBlock + 0x80u, 800000u);
                                        const auto y = b.run(kRead, kBlock + 0x7000u, kBlock + 0x80u, 800000u);
                                        if (!x.returned) {
                                            check(!y.returned, label + ": the original does not return and the owner stops");
                                        } else {
                                            compare(label, a, b, x, y);
                                            ++cases;
                                            complete += x.r3 == 1u ? 1u : 0u;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    // Mutations of the ORIGINAL (one code word of 0x8049A8DC patched): each name argument swapped for the other must make the
    // comparison fail - the doubles see the argument, not just the call.
    {
        struct Mutation {
            const char* what;
            u32 address, word;
        };
        const Mutation mutations[] = {{"RwTextureSetName handed the mask name (addi r4, r1, 0x5C)", 0x8049AAF8u, 0x3881005Cu},
                                      {"RwTextureSetMaskName handed the name (addi r4, r1, 0x3C)", 0x8049AB34u, 0x3881003Cu},
                                      {"RwTextureSetMaskName handed the stream instead of the texture (mr r3, r28)", 0x8049AB30u, 0x7F83E378u}};
        for (const Mutation& mutation : mutations) {
            Machine a(dol, false), b(dol, true);
            for (Machine* m : {&a, &b}) {
                m->setup(0, 0);
                install_texture_doubles(*m);
                for (u32 i = 0; i < 0x20u; i += 4u) m->cpu.mem_write32(kScenario + i, 0u);
                m->cpu.mem_write32(kScenario, 1u);
                m->cpu.mem_write32(kScenario + 4u, 0x35000u);
                m->cpu.mem_write32(kScenario + 8u, 0xFFFFFFFFu);
                m->cpu.mem_write32(kScenario + 0x14u, 0x10u);
                m->cpu.mem_write32(kFreeLog + 0x1Cu, kBlock + 0x6000u);
                for (u32 i = 0x100u; i < 0x200u; i += 4u) m->cpu.mem_write32(kFreeLog + i, 0u);
                for (u32 i = 0; i < 0x800u; i += 4u) m->cpu.mem_write32(kBlock + 0xA000u + i, 0xEEEEEEEEu);
                for (u32 i = 0; i < 0x800u; i += 4u) m->cpu.mem_write32(kBlock + 0x6000u + i, 0xEEEEEEEEu);
                build_stream(*m, 6u, 16u, 8u, 16u, 3u, 1u, 0x40u, false, 0x300u, 0x00001234u, 1.5f);
            }
            a.cpu.install_hle(0x8049B568u, [](Cpu& c) { return original_lod_double(c); }, "test: the LOD step 0x8049B568");
            rw_raster::install_lifted(b.cpu, loaded);
            a.cpu.mem_write32(mutation.address, mutation.word);
            const unsigned before = static_cast<unsigned>(g_failures);
            g_quiet = true;
            compare(std::string("mutation: ") + mutation.what, a, b, a.run(kRead, kBlock + 0x7000u, kBlock + 0x80u, 800000u),
                    b.run(kRead, kBlock + 0x7000u, kBlock + 0x80u, 800000u));
            g_quiet = false;
            const bool detected = static_cast<unsigned>(g_failures) > before;
            g_failures = static_cast<int>(before);   // the failures of a mutation are the point of the check
            check(detected, std::string("the comparison tells apart: ") + mutation.what);
        }
    }
    check(cases > 500 && complete > 20, "the texture read grid reads textures and fails every way (" + std::to_string(cases) + " / " + std::to_string(complete) + ")");
    std::printf("  GcnNativeTextureRead: %u cases equal (%u complete reads)\n", cases, complete);
}

// GcnRasterClearRect 0x8049A124 and its device wrappers 0x8049A52C / 0x8049A508: a rectangle fill of the current raster.
void test_clear(const std::vector<u8>& dol) {
    constexpr u32 kClearRect = 0x8049A124u, kClearAll = 0x8049A52Cu, kClearEntry = 0x8049A508u;
    unsigned cases = 0, filled = 0, locks = 0, errors = 0;
    const auto make = [&](Machine& m, u8 type, u32 depth, u32 w, u32 h, u32 stride, u8 priv, int hx, int hy) {
        m.setup(0, 0);
        install_core_lock_doubles(m);
        build(m, {kRasters, 0, type, 0, 0, w, h, 3, 0, kBlock});
        m.cpu.mem_write32(kRasters + 0x14u, depth);
        m.cpu.mem_write32(kRasters + 0x18u, stride);
        m.cpu.mem_write32(kRasters + 0x04u, kBlock + 0x3000u);
        m.cpu.mem_write8(kRasters + 0x22u, priv);
        m.cpu.mem_write16(kRasters + 0x1Cu, static_cast<u16>(hx));
        m.cpu.mem_write16(kRasters + 0x1Eu, static_cast<u16>(hy));
        m.cpu.mem_write32(kSda + kSdaContext, kRasters);
        for (u32 i = 0; i < 0x2000u; i += 4u) m.cpu.mem_write32(kBlock + 0x3000u + i, 0xEEEEEEEEu);
    };
    const auto put_rect = [&](Machine& m, int x, int y, int w, int h) {
        m.cpu.mem_write32(kBlock + 0x100u, static_cast<u32>(x));
        m.cpu.mem_write32(kBlock + 0x104u, static_cast<u32>(y));
        m.cpu.mem_write32(kBlock + 0x108u, static_cast<u32>(w));
        m.cpu.mem_write32(kBlock + 0x10Cu, static_cast<u32>(h));
    };
    struct RectCase {
        int x, y, w, h;
    };
    const RectCase rects[] = {{0, 0, 16, 8}, {4, 2, 8, 4}, {0, 0, 8, 8}, {2, 1, 6, 3}, {0, 0, 0, 4}, {1, 0, 7, 2}};
    for (const u8 type : {u8{0}, u8{4}, u8{5}, u8{3}, u8{6}, u8{7}}) {
        for (const u32 depth : {4u, 8u, 16u, 32u, 24u}) {
            for (const u8 priv : {u8{0}, u8{4}}) {
                for (const auto& r : rects) {
                    for (const u32 pixel : {0u, 0xFFFFFFFFu, 0x12345678u, 0xABu}) {
                        for (const u32 entry : {kClearRect, kClearAll, kClearEntry}) {
                            for (const int hx : {0, 3}) {
                                if (entry == kClearAll && (r.x != 0 || r.y != 0)) continue;   // the wrapper's own rectangle
                                if (hx != 0 && entry == kClearAll) continue;
                                Machine a(dol, false), b(dol, true);
                                for (Machine* m : {&a, &b}) {
                                    make(*m, type, depth, 16, 8, 64, priv, hx, 0);
                                    put_rect(*m, r.x, r.y, r.w, r.h);
                                }
                                const std::string label = "clear entry " + hex(entry) + " type " + std::to_string(type) + " depth " + std::to_string(depth) +
                                                          " priv " + std::to_string(priv) + " rect " + std::to_string(r.x) + "," + std::to_string(r.y) + "," +
                                                          std::to_string(r.w) + "," + std::to_string(r.h) + " pixel " + hex(pixel) + " hx " + std::to_string(hx);
                                const auto x = a.run(entry, kRasters, kBlock + 0x100u, 2000000u, pixel);
                                const auto y = b.run(entry, kRasters, kBlock + 0x100u, 2000000u, pixel);
                                compare(label, a, b, x, y);
                                ++cases;
                                for (u32 i = 0; i < 0x400u; i += 4u) filled += b.cpu.mem_read32(kBlock + 0x3000u + i) != 0xEEEEEEEEu ? 1u : 0u;
                                locks += b.cpu.mem_read32(kFreeLog + 0x20u);
                                errors += (type == 3u || type >= 6u || (type < 3u && depth == 24u)) ? 1u : 0u;
                            }
                        }
                    }
                }
            }
        }
    }
    check(filled > 5000 && locks > 100 && errors > 100, "the clear grid fills, locks and reports errors");
    std::printf("  GcnRasterClearRect + wrappers: %u cases equal (%u pixel words filled, %u locks)\n", cases, filled, locks);
    // z-buffer / camera rasters: the original draws a clear quad (ClearQuad 0x80499E58); the owner enters the ClearQuad owner,
    // which hands the clear to the renderer (a recording double here). Checked: what the renderer is asked, the caller's
    // rectangle moved to the raster's place, r3, and - with no renderer set - the stop.
    struct Recorded {
        int calls = 0;
        std::int32_t x = 0, y = 0, w = 0, h = 0;
        std::uint32_t rgba = 0;
        bool colour = false, depth = false;
    };
    static Recorded recorded;
    for (const u8 type : {u8{1}, u8{2}}) {
        for (const int ox : {0, 7}) {
            for (const u32 pixel : {0x00000000u, 0xFFFFFFFFu, 0x80402010u}) {
                recorded = Recorded{};
                rw_raster::set_raster_clear([](Cpu&, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h, std::uint32_t rgba,
                                               bool colour, bool depth) -> std::uint32_t {
                    recorded = Recorded{recorded.calls + 1, x, y, w, h, rgba, colour, depth};
                    return 1u;
                });
                Machine b(dol, true);
                make(b, type, 16, 16, 8, 64, 0, ox, 3);
                // RwRGBAFromPixel(&rgba, pixel, format << 8): a double that writes R, G, B, A bytes = pixel's four bytes
                b.cpu.install_hle(0x8048A168u, [](Cpu& c) {
                    c.mem_write32(kFreeLog + 0x1C0u, c.gpr[4]);   // the pixel
                    c.mem_write32(kFreeLog + 0x1C4u, c.gpr[5]);   // the format << 8
                    for (u32 i = 0; i < 4u; ++i) c.mem_write8(c.gpr[3] + i, static_cast<u8>(c.gpr[4] >> (8u * i)));
                    return true;
                }, "test: RwRGBAFromPixel");
                put_rect(b, 2, 1, 8, 4);
                const auto y = b.run(kClearRect, kRasters, kBlock + 0x100u, 400000u, pixel);
                const std::string label = "clear of a type " + std::to_string(type) + " raster, offset " + std::to_string(ox) + ", pixel " + hex(pixel);
                check(y.returned && y.r3 == 1u, label + ": returns 1");
                check(recorded.calls == 1 && recorded.x == 2 + ox && recorded.y == 1 + 3 && recorded.w == 8 && recorded.h == 4, label + ": the renderer is asked for the rectangle at the raster's place");
                check(recorded.colour == (type == 2u) && recorded.depth == (type == 1u), label + ": colour for a camera raster, depth for a z-buffer");
                if (type == 2u) {
                    check(recorded.rgba == pixel, label + ": the colour is the pixel's RGBA bytes");
                    check(b.cpu.mem_read32(kFreeLog + 0x1C0u) == pixel && b.cpu.mem_read32(kFreeLog + 0x1C4u) == (static_cast<u32>(b.cpu.mem_read8(kRasters + 0x23u)) << 8),
                          label + ": RwRGBAFromPixel is given the pixel and the format byte << 8");
                }
                check(b.cpu.mem_read32(kBlock + 0x100u) == static_cast<u32>(2 + ox) && b.cpu.mem_read32(kBlock + 0x104u) == 4u,
                      label + ": the caller's rectangle moved to the raster's place");
            }
        }
        // negative control: no renderer set - the owner stops, naming ClearQuad
        rw_raster::set_raster_clear(nullptr);
        Machine b(dol, true);
        make(b, type, 16, 16, 8, 64, 0, 0, 0);
        b.cpu.install_hle(0x8048A168u, [](Cpu&) { return true; }, "test: RwRGBAFromPixel");
        put_rect(b, 0, 0, 16, 8);
        const auto y = b.run(kClearRect, kRasters, kBlock + 0x100u, 400000u, 0u);
        check(!y.returned && y.stop.find("ClearQuad") != std::string::npos, "clear of a raster of type " + std::to_string(type) + " without a renderer stops naming ClearQuad: " + y.stop);
    }
}

// GcnRasterLock 0x80496CA4: the start of a level lock (byte sizes of the levels below, the lock buffer from the engine, the
// pixel pointer, the tile -> linear conversion for a read lock, the draw-sync wait of a write lock).
void test_lock(const std::vector<u8>& dol) {
    constexpr u32 kLock = 0x80496CA4u;
    unsigned cases = 0, buffers = 0, waits = 0, unconverted = 0;
    const auto make = [&](Machine& m, u8 type, bool own_parent, u8 priv, u8 format, u32 depth, u32 w, u32 h, u32 token, u32 next,
                          u32 allocator_result, int x, int y) {
        m.setup(next > 0u ? next - 1u : 0u, next);
        const u32 parent = own_parent ? kRasters : kRasters + 0x100u;
        build(m, {kRasters + 0x100u, 0, type, 0, format, w, h, 3, static_cast<u16>(token), kBlock});
        build(m, {kRasters, own_parent ? 0u : parent, type, 0, format, w, h, 3, static_cast<u16>(token), kBlock});
        const u32 ext = parent + kPluginOffset;
        m.cpu.mem_write32(kRasters + 0x14u, depth);
        m.cpu.mem_write32(kRasters + 0x18u, 0x40u);
        m.cpu.mem_write8(kRasters + 0x22u, priv);
        m.cpu.mem_write8(parent + 0x22u, priv);
        m.cpu.mem_write16(kRasters + 0x1Cu, static_cast<u16>(x));
        m.cpu.mem_write16(kRasters + 0x1Eu, static_cast<u16>(y));
        m.cpu.mem_write32(ext + 0x1Cu, kBlock + 0x2000u);   // the base of the level data
        m.cpu.mem_write32(ext + 0x24u, 0xA5A5A5A5u);
        m.cpu.mem_write32(ext + 0x28u, 0x5A5A5A5Au);
        m.cpu.mem_write32(kFreeLog + 0x1Cu, allocator_result);
        for (u32 i = 0; i < 0x2000u; i += 4u) m.cpu.mem_write32(kBlock + 0x2000u + i, 0x01020304u * (i / 4u + 1u));
        for (u32 i = 0; i < 0x1000u; i += 4u) m.cpu.mem_write32(kBlock + 0x6000u + i, 0xEEEEEEEEu);
        m.cpu.mem_write32(kBlock + 0x80u, 0xCCCCCCCCu);
    };
    const std::pair<u32, u32> sizes[] = {{16, 8}, {20, 12}, {64, 32}};
    for (const u8 type : {u8{0}, u8{4}, u8{5}}) {
        for (const bool own_parent : {true, false}) {
            for (const u32 flags : {0u, 1u, 2u, 3u, 8u, 9u, 10u, 11u}) {
                for (const u32 level : {0u, 1u, 2u, 3u}) {
                    for (const u32 depth : {4u, 8u, 16u, 32u}) {
                        for (const auto& size : sizes) {
                            for (const int where : {0, 1}) {   // the token: 0 = must be issued (== next), 1 = already reached
                                if (!own_parent && (flags & 8u) != 0u) continue;   // refused below
                                if (size.first != 16u && depth != 16u) continue;
                                if (where == 1 && (flags & 1u) == 0u) continue;
                                const u32 next = 5u, token = where == 0 ? 5u : 2u;
                                for (const u32 alloc : {kBlock + 0x6000u, 0u}) {
                                    if (alloc == 0u && (flags & 8u) != 0u) continue;
                                    Machine a(dol, false), b(dol, true);
                                    make(a, type, own_parent, 0x10, 0x80, depth, size.first, size.second, token, next, alloc, 8, -4);
                                    make(b, type, own_parent, 0x10, 0x80, depth, size.first, size.second, token, next, alloc, 8, -4);
                                    const u32 argument = flags | (level << 8);
                                    compare("GcnRasterLock type " + std::to_string(type) + (own_parent ? " own" : " sub") + " flags " + hex(flags) +
                                                " level " + std::to_string(level) + " depth " + std::to_string(depth) + " " +
                                                std::to_string(size.first) + "x" + std::to_string(size.second) + " token " + std::to_string(token) +
                                                " alloc " + hex(alloc),
                                            a, b, a.run(kLock, kBlock + 0x80u, kRasters, 400000u, argument),
                                            b.run(kLock, kBlock + 0x80u, kRasters, 400000u, argument));
                                    buffers += b.cpu.mem_read32(kFreeLog + 0x10u);
                                    waits += b.sync.stats().tokens > 0u ? 1u : 0u;
                                    unconverted += ((flags & 2u) != 0u && (flags & 8u) == 0u && b.cpu.mem_read32(kBlock + 0x6000u) != 0xEEEEEEEEu) ? 1u : 0u;
                                    ++cases;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    check(buffers > 100 && waits > 20 && unconverted > 50, "the lock grid allocates, waits and converts");
    std::printf("  GcnRasterLock: %u cases (%u lock buffers, %u token issues, %u tile conversions) equal\n", cases, buffers, waits, unconverted);
    // Refused: error types, a depth the original reports, a sub-raster locked in place, a token never reached.
    // Errors the original reports and answers: exact (error slot, r3, every write).
    const auto exact = [&](const std::string& what, u8 type, bool own_parent, u32 flags, u32 depth, u32 expect) {
        Machine a(dol, false), b(dol, true);
        make(a, type, own_parent, 0x10, 0x80, depth, 16, 8, 0, 20, kBlock + 0x6000u, 0, 0);
        make(b, type, own_parent, 0x10, 0x80, depth, 16, 8, 0, 20, kBlock + 0x6000u, 0, 0);
        const auto x = a.run(kLock, kBlock + 0x80u, kRasters, 400000u, flags);
        const auto y = b.run(kLock, kBlock + 0x80u, kRasters, 400000u, flags);
        compare("GcnRasterLock " + what, a, b, x, y);
        check(x.returned && a.cpu.mem_read32(kEngine + 0x304u) == expect, what + ": the original records error " + hex(expect));
    };
    for (const u8 type : {u8{1}, u8{2}, u8{3}, u8{6}, u8{7}}) exact("error type " + std::to_string(type), type, true, 3u, 16u, 0x8000000Eu);
    exact("depth 24 (own, locked in memory)", 4, true, 3u, 24u, 0x8000000Cu);
    exact("depth 24 (sub-raster)", 4, false, 3u, 24u, 0x8000000Cu);
    exact("sub-raster in place", 4, false, 8u, 16u, 0x8000000Eu);
    {   // the one stop: the original waits for a token that never arrives
        Machine b(dol, true);
        make(b, 4, true, 0x10, 0x80, 16, 16, 8, 15, 20, kBlock + 0x6000u, 0, 0);
        b.sync.on_draw_sync_token(10, false);
        const auto y = b.run(kLock, kBlock + 0x80u, kRasters, 400000u, 1u);
        check(!y.returned && y.stop.find("not reached") != std::string::npos,
              "a write lock waiting for a token never reached stops (the original spins): " + y.stop);
    }
}

// GcnRasterUnlock 0x80497104: the end of a level lock (tile conversion, free of the linear copy, the engine hook).
void test_unlock(const std::vector<u8>& dol) {
    constexpr u32 kUnlock = 0x80497104u;
    unsigned cases = 0, converted = 0, freed = 0, notified = 0;
    const auto make = [&](Machine& m, u8 type, bool own_parent, u8 priv, u8 format, u8 level, u32 depth, u32 w, u32 h) {
        m.setup(0, 0);
        const u32 parent = own_parent ? kRasters : kRasters + 0x100u;
        build(m, {kRasters, own_parent ? 0u : parent, type, 0, format, w, h, 3, 0, kBlock});
        build(m, {kRasters + 0x100u, 0, type, 0, format, w, h, 3, 0, kBlock});
        const u32 ext = parent + kPluginOffset;
        m.cpu.mem_write32(kRasters + 0x14u, depth);
        m.cpu.mem_write32(kRasters + 0x18u, 0x40u);
        m.cpu.mem_write8(kRasters + 0x22u, priv);
        m.cpu.mem_write8(parent + 0x22u, priv);
        m.cpu.mem_write32(kRasters + 0x28u, 0x1234u);   // the lock size the raster takes back
        m.cpu.mem_write32(kRasters + 0x2Cu, 0x5678u);
        m.cpu.mem_write32(ext + 0x24u, kBlock + 0x1000u);   // the driver's tiles
        m.cpu.mem_write32(ext + 0x28u, kBlock + 0x4000u);   // the linear copy (freed)
        m.cpu.mem_write8(ext + 0x33u, level);
        // the converter reads +0x28 (the linear copy the program wrote) and fills +0x24 (the driver's tiles)
        for (u32 i = 0; i < 0x1000u; i += 4u) m.cpu.mem_write32(kBlock + 0x4000u + i, 0x01020304u * (i / 4u + 1u));
        for (u32 i = 0; i < 0x1000u; i += 4u) m.cpu.mem_write32(kBlock + 0x1000u + i, 0xEEEEEEEEu);
    };
    const std::pair<u32, u32> sizes[] = {{16, 8}, {20, 12}, {8, 4}};
    for (const u8 type : {u8{0}, u8{4}, u8{5}}) {
        for (const bool own_parent : {true, false}) {
            for (const u8 priv : {u8{0}, u8{0x04}, u8{0x24}, u8{0x20}, u8{0x02}, u8{0x26}, u8{0xFF}}) {
                for (const u8 format : {u8{0}, u8{0x10}, u8{0x80}, u8{0x90}}) {
                    for (const u8 level : {u8{0}, u8{5}, u8{0xFF}}) {
                        for (const u32 depth : {4u, 8u, 16u, 32u}) {
                            for (const auto& size : sizes) {
                                if (size.first != 16u && depth != 16u) continue;
                                Machine a(dol, false), b(dol, true);
                                make(a, type, own_parent, priv, format, level, depth, size.first, size.second);
                                make(b, type, own_parent, priv, format, level, depth, size.first, size.second);
                                compare("GcnRasterUnlock type " + std::to_string(type) + (own_parent ? " own" : " sub") + " priv " + hex(priv) +
                                            " format " + hex(format) + " level " + std::to_string(level) + " depth " + std::to_string(depth) +
                                            " " + std::to_string(size.first) + "x" + std::to_string(size.second),
                                        a, b, a.run(kUnlock, 0u, kRasters), b.run(kUnlock, 0u, kRasters));
                                converted += ((priv & 0x24u) == 0x04u && b.cpu.mem_read32(kBlock + 0x1000u) != 0xEEEEEEEEu) ? 1u : 0u;
                                freed += b.cpu.mem_read32(kFreeLog);
                                notified += b.cpu.mem_read32(kFreeLog + 8u);
                                ++cases;
                            }
                        }
                    }
                }
            }
        }
    }
    check(converted > 50 && freed > 100 && notified > 20, "the unlock grid converts, frees and notifies");
    std::printf("  GcnRasterUnlock: %u cases (%u converted, %u frees, %u hook calls) equal\n", cases, converted, freed, notified);
    for (const u8 type : {u8{1}, u8{2}, u8{3}, u8{6}, u8{7}}) {
        Machine a(dol, false), b(dol, true);
        make(a, type, true, 0x04, 0, 0, 16, 16, 8);
        make(b, type, true, 0x04, 0, 0, 16, 16, 8);
        const auto x = a.run(kUnlock, 0u, kRasters);
        const auto y = b.run(kUnlock, 0u, kRasters);
        compare("GcnRasterUnlock error type " + std::to_string(type), a, b, x, y);
        check(x.returned && x.r3 == 0u && a.cpu.mem_read32(kEngine + 0x304u) == 0x80000011u, "the original answers 0 and records 0x80000011");
    }
}

// GcnRasterLockPalette 0x804972B8 / GcnRasterUnlockPalette 0x80497388 (slice 2).
void test_palette(const std::vector<u8>& dol) {
    constexpr u32 kLock = 0x804972B8u, kUnlock = 0x80497388u;
    unsigned cases = 0, locked = 0, unlocked = 0;
    const auto make = [&](Machine& m, u8 type, bool own_parent, u32 lock_ptr, u8 priv, u32 depth) {
        m.setup(0, 0);
        build(m, {kRasters, own_parent ? 0u : kRasters + 0x100u, type, 0, 0, 64, 64, 3, 0, kBlock});
        m.cpu.mem_write32(kRasters + 0x14u, depth);
        m.cpu.mem_write8(kRasters + 0x22u, priv);
        m.cpu.mem_write32(kRasters + 0x08u, lock_ptr);
        m.cpu.mem_write32(kRasters + kPluginOffset + 0x20u, 0x81360000u);
        m.cpu.mem_write32(kBlock + 0x80u, 0xCCCCCCCCu);
    };
    for (const u8 type : {u8{0}, u8{4}, u8{8}, u8{12}, u8{0x14}}) {   // the type's low three bits decide: 0 and 4 are fine
        for (const bool own_parent : {true, false}) {
            for (const u32 lock_ptr : {0u, 0x81350000u}) {
                for (const u8 priv : {u8{0}, u8{0x08}, u8{0x10}, u8{0x18}, u8{0xE7}, u8{0xFF}}) {
                    for (const u32 depth : {0u, 4u, 8u}) {
                        for (const u32 flags : {0u, 1u, 2u, 3u, 0xFFFFFFFCu, 0xFFFFFFFFu}) {
                            Machine a(dol, false), b(dol, true);
                            make(a, type, own_parent, lock_ptr, priv, depth);
                            make(b, type, own_parent, lock_ptr, priv, depth);
                            const std::string tail = " type " + std::to_string(type) + (own_parent ? " own" : " sub") + " lock " + hex(lock_ptr) +
                                                     " priv " + hex(priv) + " flags " + hex(flags);
                            compare("GcnRasterLockPalette" + tail, a, b, a.run(kLock, kBlock + 0x80u, kRasters, 400000u, flags),
                                    b.run(kLock, kBlock + 0x80u, kRasters, 400000u, flags));
                            locked += (b.cpu.mem_read32(kBlock + 0x80u) != 0xCCCCCCCCu) ? 1u : 0u;
                            ++cases;
                        }
                        Machine a(dol, false), b(dol, true);
                        make(a, type, own_parent, lock_ptr, priv, depth);
                        make(b, type, own_parent, lock_ptr, priv, depth);
                        const std::string tail = " type " + std::to_string(type) + (own_parent ? " own" : " sub") + " lock " + hex(lock_ptr) +
                                                 " priv " + hex(priv) + " depth " + std::to_string(depth);
                        compare("GcnRasterUnlockPalette" + tail, a, b, a.run(kUnlock, 0u, kRasters), b.run(kUnlock, 0u, kRasters));
                        unlocked += (b.cpu.mem_read32(kRasters + 0x08u) == 0u && lock_ptr != 0u) ? 1u : 0u;
                        ++cases;
                    }
                }
            }
        }
    }
    check(locked > 100 && unlocked > 10, "the palette grid locks and unlocks");
    std::printf("  palette lock / unlock: %u cases (%u locks, %u unlocks that cleared a lock) equal\n", cases, locked, unlocked);
    // The error types (low three bits 1, 2, 3, 5, 6, 7): the original reports through RwErrorSet; the owner stops.
    for (const u8 type : {u8{1}, u8{2}, u8{3}, u8{5}, u8{6}, u8{7}, u8{0x0B}}) {
        for (const u32 entry : {kLock, kUnlock}) {
            Machine a(dol, false), b(dol, true);
            make(a, type, true, 0u, 0u, 4u);
            make(b, type, true, 0u, 0u, 4u);
            const auto x = a.run(entry, kBlock + 0x80u, kRasters, 400000u, 3u);
            const auto y = b.run(entry, kBlock + 0x80u, kRasters, 400000u, 3u);
            compare("palette error type " + std::to_string(type) + (entry == kLock ? " lock" : " unlock"), a, b, x, y);
            check(x.returned && x.r3 == 0u && a.cpu.mem_read32(kEngine + 0x304u) == (entry == kLock ? 0x8000000Eu : 0x80000011u),
                  "the original answers 0 and records the lock / unlock error");
        }
    }
    // Negative controls: a lock that did not mark the flags, and an unlock that kept the lock pointer, are told apart.
    {
        Machine a(dol, false), b(dol, true);
        make(a, 0, true, 0u, 0u, 4u);
        make(b, 0, true, 0u, 0u, 4u);
        (void)a.run(kLock, kBlock + 0x80u, kRasters, 400000u, 3u);
        check(a.cpu.mem_read8(kRasters + 0x22u) == 0x18u && a.cpu.mem_read32(kRasters + 8u) == 0x81360000u &&
                  a.cpu.mem_read32(kBlock + 0x80u) == 0x81360000u,
              "the original's lock marks 0x18 and hands out the palette words");
        (void)a.run(kUnlock, 0u, kRasters);
        check(a.cpu.mem_read8(kRasters + 0x22u) == 0u && a.cpu.mem_read32(kRasters + 8u) == 0u, "the original's unlock clears both");
        check(b.cpu.mem_read8(kRasters + 0x22u) != 0x18u, "negative control: a machine that never locked differs from one that did");
    }
}

// GcnRasterSetImage (0x80494614) and the image-to-raster bodies it dispatches to (0x80493C38 / 0x80493E90 / 0x804940EC /
// 0x80494380) with the quantiser (0x80493B74) and the five converters (0x80492B24..), as ORIGINAL words against the owners.
// What stays RenderWare core (the lock helpers, RwImageCreate / AllocatePixels / Destroy, the quantiser set-up, RwErrorSet)
// is replaced on BOTH machines by PPC test doubles at their own addresses that append an entry to a log and return a
// configured value, so the compared state is exactly the driver's own: its pixel and palette writes, the choice, order
// and arguments of the core calls, and the error reports.
//   entry (64 bytes, from kStubLog + 8): +0 id, +4.. the first n argument registers, +28 / +32 {[r3], [r3+4]} for RwErrorSet,
//   +40 f1 (the quantiser set-up's float argument).
struct Core {
    u32 address;
    u32 args;   // how many argument registers the callee's prototype takes (the others are garbage in the original)
};
constexpr Core kCores[] = {
    {0x8048AFC0u, 3}, {0x8048ACE4u, 2}, {0x8048ABD8u, 1}, {0x8048AB90u, 1}, {0x80488D58u, 3}, {0x80488E68u, 1}, {0x80490F28u, 1},
    {0x8048EF00u, 2}, {0x80490620u, 3}, {0x80490B00u, 6}, {0x80480D2Cu, 1}, {0x80488E00u, 1}, {0x8047EC78u, 1}, {0x8047EBD4u, 1}};
enum CoreId : u32 { kLock, kLockPixels, kUnlockPixels, kUnlock, kImageCreate, kImageAlloc, kQuantInit, kQuantSetup, kQuantPalette,
                    kQuantConvert, kAllocatorFree, kImageFree, kErrorFormat, kErrorSet };
constexpr u32 kStubLog = 0x81313000u, kStubRet = 0x81312F00u;
constexpr u32 kEntry0 = kStubLog + 8u, kEntrySize = 64u;

u32 enc_d(u32 op, u32 rt, u32 ra, int d) { return (op << 26) | (rt << 21) | (ra << 16) | (static_cast<u32>(d) & 0xFFFFu); }

// log_r4: the error formatter also logs r4 (the code-2 report passes a string there; every other report leaves r4 unset).
void install_cores(Machine& m, const u32 (&returns)[14], bool log_r4 = false) {
    for (u32 id = 0; id < 14u; ++id) {
        std::vector<u32> w = {enc_d(15, 12, 0, 0x8131), enc_d(32, 11, 12, 0x3000),
                              (21u << 26) | (11u << 21) | (10u << 16) | (6u << 11) | (0u << 6) | (25u << 1),   // slwi r10, r11, 6
                              0x7C000000u | (10u << 21) | (10u << 16) | (12u << 11) | (266u << 1),              // add r10, r10, r12
                              enc_d(14, 9, 0, static_cast<int>(id)), enc_d(36, 9, 10, 0x3008)};
        const u32 args = kCores[id].args + ((id == kErrorFormat && log_r4) ? 1u : 0u);
        for (u32 a = 0; a < args; ++a) w.push_back(enc_d(36, 3 + a, 10, 0x300C + 4 * static_cast<int>(a)));
        if (id == kErrorSet) {
            w.push_back(enc_d(32, 9, 3, 0));
            w.push_back(enc_d(36, 9, 10, 0x3008 + 28));
            w.push_back(enc_d(32, 9, 3, 4));
            w.push_back(enc_d(36, 9, 10, 0x3008 + 32));
        }
        if (id == kQuantInit) {   // the quantiser context starts empty: no tree, no allocator (the real init does this)
            w.push_back(enc_d(14, 9, 0, 0));
            w.push_back(enc_d(36, 9, 3, 0x4000));
            w.push_back(enc_d(36, 9, 3, 0x4004));
        }
        if (id == kQuantSetup) w.push_back(enc_d(54, 1, 10, 0x3008 + 40));   // stfd f1, 40(r10)
        // A callee's own frame: scribble the 256 bytes below r1 (a six-word loop: the doubles sit only 0x48 bytes apart). A temporary
        // the owner passes by pointer must live ABOVE the r1 the owner calls from, or this overwrites it before the callee's consumer reads it.
        w.insert(w.begin(), {enc_d(14, 9, 0, 32), 0x7D2903A6u /* mtctr r9 */, enc_d(14, 10, 1, -0x100), enc_d(36, 0, 10, 0), enc_d(14, 10, 10, 8),
                             0x4200FFF8u /* bdnz -8 */});
        w.push_back(enc_d(14, 11, 11, 1));
        w.push_back(enc_d(36, 11, 12, 0x3000));
        w.push_back(enc_d(32, 3, 12, 0x2F00 + 4 * static_cast<int>(id)));
        w.push_back(0x4E800020u);
        for (u32 i = 0; i < w.size(); ++i) m.bus.write32(kCores[id].address + i * 4u, w[i]);
        m.cpu.mem_write32(kStubRet + 4u * id, returns[id]);
    }
    m.cpu.mem_write32(kStubLog, 0u);
}

// A deterministic fill (an LCG), so the source pixels and palettes are not patterns the loops could hide behind.
void fill(Machine& m, u32 address, u32 bytes, u32 seed) {
    u32 s = seed;
    for (u32 i = 0; i < bytes; i += 4u) {
        s = s * 1664525u + 1013904223u;
        m.cpu.mem_write32(address + i, s);
    }
}

constexpr u32 kImg = 0x81350000u, kImgPixels = 0x81360000u, kImgPalette = 0x81362000u, kDstPixels = 0x81370000u,
              kDstPalette = 0x81372000u, kTemp = 0x81380000u, kTempPixels = 0x81381000u, kTempPalette = 0x81382000u;

struct Shape {
    u32 kind = 4;        // the raster's word at +0x14: which body
    u32 format = 0x0A;   // cFormat (+0x23)
    u32 flags = 0;       // privateFlags (+0x22)
    u32 depth = 8;       // the image's depth
    u32 width = 5, height = 3;
    u32 locked = 0;      // the platform data's +0x33 byte
    u32 stride = 0x40;
};

void prepare(Machine& m, const Shape& s, u32 seed) {
    m.setup(0, 0);
    Cpu& c = m.cpu;
    fill(m, kImgPixels, 0x2000u, seed);
    fill(m, kImgPalette, 0x400u, seed ^ 0x5A5A5A5Au);
    fill(m, kDstPixels, 0x1000u, 0xCDCDCDCDu);
    fill(m, kDstPalette, 0x400u, 0xABABABABu);
    fill(m, kTempPixels, 0x1000u, seed ^ 0x1234567u);
    fill(m, kTempPalette, 0x400u, seed ^ 0x7654321u);
    // the raster
    c.mem_write32(kRasters, kRasters);
    c.mem_write32(kRasters + 0x04u, kDstPixels);
    c.mem_write32(kRasters + 0x08u, kDstPalette);
    c.mem_write32(kRasters + 0x0Cu, s.width);
    c.mem_write32(kRasters + 0x10u, s.height);
    c.mem_write32(kRasters + 0x14u, s.kind);
    c.mem_write32(kRasters + 0x18u, 0x40u);
    c.mem_write8(kRasters + 0x22u, static_cast<u8>(s.flags));
    c.mem_write8(kRasters + 0x23u, static_cast<u8>(s.format));
    c.mem_write8(kRasters + kPluginOffset + 0x33u, static_cast<u8>(s.locked));
    // the source image
    c.mem_write32(kImg + 0x04u, s.width);
    c.mem_write32(kImg + 0x08u, s.height);
    c.mem_write32(kImg + 0x0Cu, s.depth);
    c.mem_write32(kImg + 0x10u, s.stride);
    c.mem_write32(kImg + 0x14u, kImgPixels);
    c.mem_write32(kImg + 0x18u, kImgPalette);
    // the quantiser's temporary image (what RwImageCreate returns)
    c.mem_write32(kTemp + 0x04u, s.width);
    c.mem_write32(kTemp + 0x08u, s.height);
    c.mem_write32(kTemp + 0x0Cu, s.kind == 4u ? 4u : 8u);
    c.mem_write32(kTemp + 0x10u, s.stride);
    c.mem_write32(kTemp + 0x14u, kTempPixels);
    c.mem_write32(kTemp + 0x18u, kTempPalette);
}

struct Returns {
    u32 lock = 1, lock_pixels = 1, create = kTemp, quant_init = 1;
};
void cores_for(Machine& m, const Returns& r, bool log_r4) {
    const u32 returns[14] = {r.lock, r.lock_pixels, 0x11u, 0x22u, r.create, 0x33u, r.quant_init, 0x44u, 0x55u, 0x66u, 0x77u, 0x88u,
                             0x5A5A0000u, 0x99u};
    install_cores(m, returns, log_r4);
}

// Runs the original and the owner on identical state and compares everything compare() does, plus the call log.
// `expect_stop`: the owner must stop (the original calls address 0); the original is not run.
unsigned g_image_cases = 0, g_image_calls = 0;
void compare_image(const std::vector<u8>& dol, const std::string& label, u32 entry, const Shape& s, const Returns& r, u32 seed,
                   bool log_r4 = false) {
    Machine a(dol, false), b(dol, true);
    for (Machine* m : {&a, &b}) {
        prepare(*m, s, seed);
        cores_for(*m, r, log_r4);
    }
    const auto x = a.run(entry, kRasters, kImg);
    const auto y = b.run(entry, kRasters, kImg);
    compare(label, a, b, x, y, entry == 0x80494614u);
    check(a.cpu.mem_read32(kStubLog) == b.cpu.mem_read32(kStubLog), label + ": core call count");
    g_image_calls += b.cpu.mem_read32(kStubLog);
    ++g_image_cases;
}

void test_image_bodies(const std::vector<u8>& dol) {
    // formats with a converter; the dimensions include empty rasters and odd widths (the 4-bit body steps by two)
    const u32 formats[] = {0x01, 0x02, 0x03, 0x05, 0x06, 0x0A};
    const std::pair<u32, u32> dims[] = {{0, 0}, {1, 1}, {2, 3}, {5, 2}, {8, 4}, {3, 0}, {0, 3}};
    unsigned base = g_image_cases;
    for (const u32 kind : {4u, 8u, 16u, 32u}) {
        const u32 entry = kind == 4u ? 0x80493C38u : kind == 8u ? 0x80493E90u : kind == 16u ? 0x804940ECu : 0x80494380u;
        for (const u32 format : formats) {
            for (const u32 depth : {4u, 8u, 0x20u, 16u, 0u}) {
                for (const auto& dim : dims) {
                    for (const u32 locked : {0u, 1u}) {
                        Shape s;
                        s.kind = kind; s.format = format; s.depth = depth; s.width = dim.first; s.height = dim.second; s.locked = locked;
                        for (const u32 temp : {kTemp, 0u}) {   // RwImageCreate succeeds / fails
                            Returns r;
                            r.create = temp;
                            if (temp == 0u && !(depth == 0x20u || ((kind == 4u) && depth == 8u))) continue;   // only the quantiser path asks
                            compare_image(dol, "image body " + hex(entry) + " format " + hex(format) + " depth " + std::to_string(depth) + " " +
                                                   std::to_string(dim.first) + "x" + std::to_string(dim.second) + " locked " + std::to_string(locked) +
                                                   (temp == 0u ? " create-fails" : ""),
                                          entry, s, r, 0x1000u + kind * 7u + format + depth + dim.first * 31u + dim.second);
                        }
                    }
                }
            }
        }
        // the quantiser's set-up failing leaves the temporary and returns without converting
        for (const u32 depth : {0x20u}) {
            Shape s;
            s.kind = kind; s.format = 0x0A; s.depth = depth;
            Returns r;
            r.quant_init = 0;
            compare_image(dol, "image body " + hex(entry) + " quantiser init fails", entry, s, r, 77u);
        }
    }
    // The unknown-format reports (code 0x8000000D with no converter) and format 0x400 (code 2 + string): the report is
    // compared where no converter call follows (the original would call address 0 otherwise).
    for (const u32 kind : {4u, 8u, 16u, 32u}) {
        const u32 entry = kind == 4u ? 0x80493C38u : kind == 8u ? 0x80493E90u : kind == 16u ? 0x804940ECu : 0x80494380u;
        for (const u32 format : {0x00u, 0x04u, 0x07u, 0x0Bu, 0x0Fu}) {
            for (const u32 depth : {4u, 8u, 0x20u, 16u}) {
                for (const auto& dim : {std::pair<u32, u32>{0, 0}, {4, 0}, {0, 4}, {4, 4}}) {
                    for (const u32 locked : {0u, 1u}) {
                        // when does the original call the converter? K4/K8: only with an unlocked palette (4/8 bit); K16/K32: a
                        // paletted source always (entries), a 32-bit source per pixel.
                        const bool paletted_source = depth == 4u || depth == 8u;
                        bool converts;
                        if (kind == 4u) converts = (depth == 4u || depth == 8u || depth == 0x20u) && locked == 0u;
                        else if (kind == 8u) converts = (depth == 4u || depth == 8u || depth == 0x20u) && locked == 0u;
                        else converts = paletted_source || (depth == 0x20u && dim.first != 0u && dim.second != 0u);
                        Shape s;
                        s.kind = kind; s.format = format; s.depth = depth; s.width = dim.first; s.height = dim.second; s.locked = locked;
                        if (!converts) {
                            compare_image(dol, "image body " + hex(entry) + " no-converter format " + hex(format) + " depth " + std::to_string(depth) +
                                                   " " + std::to_string(dim.first) + "x" + std::to_string(dim.second) + " locked " + std::to_string(locked),
                                          entry, s, Returns{}, 99u, format == 0x04u && depth == 4u);
                        } else {
                            Machine b(dol, true);
                            prepare(b, s, 5u);
                            cores_for(b, Returns{}, false);
                            const auto y = b.run(entry, kRasters, kImg, 200000u);
                            check(!y.returned && y.stop.find("address 0") != std::string::npos,
                                  "image body " + hex(entry) + " format " + hex(format) + " depth " + std::to_string(depth) +
                                      ": the owner refuses the call of address 0 (" + y.stop + ")");
                        }
                    }
                }
            }
        }
    }
    std::printf("  image bodies: %u cases equal (%u core calls compared)\n", g_image_cases - base, g_image_calls);
}

void test_set_image(const std::vector<u8>& dol) {
    unsigned base = g_image_cases;
    for (const u32 kind : {3u, 4u, 8u, 16u, 24u, 32u, 0x100u, 0xFFFFFFFCu}) {
        for (const u32 flags : {0u, 0x04u, 0x10u, 0x14u, 0xFFu}) {
            for (const u32 format : {0x0Au, 0x2Au, 0x4Au, 0x6Au, 0x01u, 0x26u}) {
                for (const u32 depth : {4u, 8u, 0x20u}) {
                    for (const u32 lock : {1u, 0u}) {
                        for (const u32 lock_pixels : {1u, 0u}) {
                            Shape s;
                            s.kind = kind; s.format = format; s.flags = flags; s.depth = depth;
                            Returns r;
                            r.lock = lock; r.lock_pixels = lock_pixels;
                            compare_image(dol, "GcnRasterSetImage kind " + std::to_string(kind) + " flags " + hex(flags) + " format " + hex(format) +
                                                   " depth " + std::to_string(depth) + " lock " + std::to_string(lock) + " lockpixels " +
                                                   std::to_string(lock_pixels),
                                          kSetImage, s, r, 0x2000u + kind + flags + format + depth);
                        }
                    }
                }
            }
        }
    }
    // The sequence of an unlocked 16-bit raster with a pixel stage: lock, lock-pixels, (converter), unlock-pixels, unlock.
    {
        Machine b(dol, true);
        Shape s;
        s.kind = 16; s.format = 0x2Au; s.depth = 8; s.width = 0; s.height = 0;
        prepare(b, s, 3u);
        cores_for(b, Returns{}, false);
        const auto y = b.run(kSetImage, kRasters, kImg);
        const u32 expect[] = {kLock, kLockPixels, kUnlockPixels, kUnlock};
        bool same = y.returned && y.r3 == 1u;
        const u32 calls = b.cpu.mem_read32(kStubLog);
        // 16 palette entries are converted by the real converter (no core call), so the log is the four helpers
        same = same && calls == 4u;
        for (u32 i = 0; same && i < 4u; ++i) same = b.cpu.mem_read32(kEntry0 + kEntrySize * i) == expect[i];
        check(same, "GcnRasterSetImage: lock, lock-pixels, unlock-pixels, unlock in that order (" + std::to_string(calls) + " core calls)");
    }
    // The error report of an unknown kind: RwErrorSet {1, the formatter's message}.
    {
        Machine b(dol, true);
        Shape s;
        s.kind = 24; s.format = 0x0A;
        prepare(b, s, 3u);
        cores_for(b, Returns{}, false);
        const auto y = b.run(kSetImage, kRasters, kImg);
        const u32 e = kEntry0 + kEntrySize * 1u;   // lock = entry 0, the formatter entry 1, RwErrorSet entry 2
        check(y.returned && y.r3 == 1u && b.cpu.mem_read32(kStubLog) == 4u && b.cpu.mem_read32(e) == kErrorFormat &&
                  b.cpu.mem_read32(e + 4u) == 0x8000000Cu && b.cpu.mem_read32(e + kEntrySize) == kErrorSet &&
                  b.cpu.mem_read32(e + kEntrySize + 28u) == 1u && b.cpu.mem_read32(e + kEntrySize + 32u) == 0x5A5A0000u,
              "GcnRasterSetImage kind 24 reports RwErrorSet {1, formatted message} and still returns 1");
    }
    std::printf("  GcnRasterSetImage: %u cases equal\n", g_image_cases - base);
}

// The five converters, native against the original words through a 16-bit raster of one pixel per format.
void test_converters(const std::vector<u8>& dol) {
    unsigned cases = 0;
    const u32 formats[] = {0x01, 0x02, 0x03, 0x05, 0x06, 0x0A};
    for (const u32 format : formats) {
        for (u32 pixel = 0; pixel < 0x100u; ++pixel) {
            const u32 r = pixel * 0x9E3779B1u;
            const u32 word = (pixel & 3u) == 0u ? (r | 0xFF000000u) : (pixel & 3u) == 1u ? (r & 0x00FFFFFFu) : r;   // alpha 0xFF / 0 / any
            for (const u32 kind : {16u, 32u}) {
                Machine a(dol, false), b(dol, true);
                Shape s;
                s.kind = kind; s.format = format; s.depth = 0x20; s.width = 1; s.height = 1;
                for (Machine* m : {&a, &b}) {
                    prepare(*m, s, 1u);
                    cores_for(*m, Returns{}, false);
                    m->cpu.mem_write32(kImgPixels, word);
                }
                const u32 entry = kind == 16u ? 0x804940ECu : 0x80494380u;
                compare("converter format " + hex(format) + " pixel " + hex(word) + " kind " + std::to_string(kind), a, b, a.run(entry, kRasters, kImg),
                        b.run(entry, kRasters, kImg), false);
                ++cases;
            }
        }
    }
    std::printf("  converters: %u cases equal\n", cases);
}

u32 log2_of(u32 v) { u32 n = 0; while (v > 1u) { v >>= 1; ++n; } return n; }

// The raster size arithmetic: 0x80492B04 (log2 loop), 0x80496478 (one mip level), 0x804965A8 (all levels), 0x8049A57C
// (GcnRasterGetSize), original words against the owners. RwErrorSet / its formatter are the logging doubles above.
void test_size(const std::vector<u8>& dol) {
    unsigned cases = 0;
    const auto make = [&](Machine& m, u32 kind, u32 flags, u32 format, u32 width, u32 height, u32 mips) {
        m.setup(0, 0);
        cores_for(m, Returns{}, false);
        Cpu& c = m.cpu;
        c.mem_write32(kRasters, kRasters);
        c.mem_write32(kRasters + 0x0Cu, width);
        c.mem_write32(kRasters + 0x10u, height);
        c.mem_write32(kRasters + 0x14u, kind);
        c.mem_write8(kRasters + 0x22u, static_cast<u8>(flags));
        c.mem_write8(kRasters + 0x23u, static_cast<u8>(format));
        c.mem_write8(kRasters + kPluginOffset + 0x32u, static_cast<u8>(mips));
        c.mem_write32(kBlock, kRasters);   // the pointer GetSize reads
    };
    // log2 loop
    for (const u32 value : {0u, 1u, 2u, 3u, 4u, 7u, 8u, 255u, 256u, 0x7FFFFFFFu, 0x10000u, 0x12345678u}) {
        Machine a(dol, false), b(dol, true);
        for (Machine* m : {&a, &b}) m->setup(0, 0);
        compare("log2 " + hex(value), a, b, a.run(0x80492B04u, value, 0u), b.run(0x80492B04u, value, 0u));
        check(b.cpu.gpr[3] == (value == 0u ? 0xFFFFFFFFu : log2_of(value)), "log2 value");
        ++cases;
    }
    {
        Machine b(dol, true);
        b.setup(0, 0);
        const auto y = b.run(0x80492B04u, 0x80000000u, 0u);
        check(!y.returned && y.stop.find("negative") != std::string::npos, "log2 of a negative value is refused (the original never ends)");
    }
    // one mip level
    const u32 dims[] = {0u, 1u, 3u, 9u, 64u, 100u, 0x7FFFFFFFu, 0xFFFFFFF0u};
    for (const u32 kind : {4u, 8u, 16u, 32u})   // the owner of 0x80496478 is card-saves's and refuses other depths
        for (const u32 flags : {0u, 2u, 4u, 6u, 1u})
            for (const u32 width : dims)
                for (const u32 height : {1u, 5u, 480u})
                    for (const u32 level : {0u, 1u, 3u, 33u, 255u}) {
                        Machine a(dol, false), b(dol, true);
                        for (Machine* m : {&a, &b}) make(*m, kind, flags, 0, width, height, 0xFF);
                        compare("mip bytes kind " + std::to_string(kind) + " flags " + std::to_string(flags) + " " + std::to_string(width) + "x" +
                                    std::to_string(height) + " level " + std::to_string(level),
                                a, b, a.run(0x80496478u, kRasters, level), b.run(0x80496478u, kRasters, level));
                        check(a.cpu.mem_read32(kStubLog) == b.cpu.mem_read32(kStubLog), "mip bytes: core call count");
                        ++cases;
                    }
    // all levels and GetSize
    for (const u32 kind : {4u, 8u, 16u, 32u, 7u})
        for (const u32 format : {0x00u, 0x80u, 0x20u, 0x60u, 0xA0u})
            for (const u32 mips : {0xFFu, 0u, 1u, 3u, 6u})
                for (const auto& dim : {std::pair<u32, u32>{1, 1}, {8, 8}, {64, 32}, {100, 70}, {640, 480}}) {
                    Machine a(dol, false), b(dol, true);
                    for (Machine* m : {&a, &b}) make(*m, kind, 0, format, dim.first, dim.second, mips);
                    const std::string label = "all mips kind " + std::to_string(kind) + " format " + hex(format) + " mips " + std::to_string(mips) +
                                              " " + std::to_string(dim.first) + "x" + std::to_string(dim.second);
                    compare(label, a, b, a.run(0x804965A8u, kRasters, 0u), b.run(0x804965A8u, kRasters, 0u));
                    ++cases;
                    Machine c(dol, false), d(dol, true);
                    for (Machine* m : {&c, &d}) {
                        make(*m, kind, 0, format, dim.first, dim.second, mips);
                        m->cpu.mem_write32(kBlock + 0x40u, 0xCAFEBABEu);
                    }
                    compare("GetSize " + label, c, d, c.run(0x8049A57Cu, kBlock + 0x40u, kBlock), d.run(0x8049A57Cu, kBlock + 0x40u, kBlock));
                    ++cases;
                }
    {   // a null raster
        Machine a(dol, false), b(dol, true);
        for (Machine* m : {&a, &b}) {
            make(*m, 4, 0, 0, 8, 8, 0xFF);
            m->cpu.mem_write32(kBlock, 0u);
            m->cpu.mem_write32(kBlock + 0x40u, 0xCAFEBABEu);
        }
        compare("GetSize of a null raster", a, b, a.run(0x8049A57Cu, kBlock + 0x40u, kBlock), b.run(0x8049A57Cu, kBlock + 0x40u, kBlock));
        check(b.cpu.mem_read32(kBlock + 0x40u) == 0x64u, "GetSize of a null raster reports 100");
        ++cases;
    }
    std::printf("  raster size arithmetic: %u cases equal\n", cases);
}

// 0x80491448 (quantiser end) with 0x80491124 (the node-tree free) under it: random 16-ary trees, the engine free function a PPC
// stub that appends {r3, r4} to a log, so the ORDER of the frees is compared word by word.
void test_quant_end(const std::vector<u8>& dol) {
    unsigned cases = 0, frees = 0;
    constexpr u32 kCtx = 0x81398000u, kNodes = 0x81390000u, kFreeLogBase = 0x81313800u, kEngineFree = 0x81311800u;
    // stub: lis r12,0x8131 ; lwz r11,0x3800(r12) ; slwi r10,r11,3 ; add r10,r10,r12 ; stw r3,0x3804(r10) ; stw r4,0x3808(r10) ;
    //       addi r11,r11,1 ; stw r11,0x3800(r12) ; blr
    const u32 stub[] = {enc_d(15, 12, 0, 0x8131), enc_d(32, 11, 12, 0x3800), (21u << 26) | (11u << 21) | (10u << 16) | (3u << 11) | (0u << 6) | (28u << 1),
                        0x7C000000u | (10u << 21) | (10u << 16) | (12u << 11) | (266u << 1), enc_d(36, 3, 10, 0x3804), enc_d(36, 4, 10, 0x3808),
                        enc_d(14, 11, 11, 1), enc_d(36, 11, 12, 0x3800), 0x4E800020u};
    for (const u32 depth : {0u, 1u, 2u, 3u, 8u, 9u, 10u, 12u, 0xFFFFFFFFu}) {
        for (const u32 shape_index : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u}) {   // empty, root only, flat, chain, sparse random, dense random; x entry 0x80491448 / 0x80491124
            const u32 shape = shape_index % 6u, direct = shape_index / 6u;
            Machine a(dol, false), b(dol, true);
            for (Machine* m : {&a, &b}) {
                m->setup(0, 0);
                const u32 none[14] = {};
                install_cores(*m, none);
                for (u32 i = 0; i < sizeof(stub) / 4; ++i) m->bus.write32(kEngineFree + 4u * i, stub[i]);
                m->cpu.mem_write32(kEngine + 0x11Cu, kEngineFree);
                m->cpu.mem_write32(kFreeLogBase, 0u);
                m->cpu.mem_write32(kSda + 0x2AD0u, depth);
                m->cpu.mem_write32(kCtx + 0x4004u, 0xA110C000u);
                u32 seed = 7u + shape * 13u + depth;
                const auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
                u32 next = 0;
                const auto make_node = [&]() { const u32 n = kNodes + 0x80u * next++; for (u32 k = 0; k < 0x80u; k += 4u) m->cpu.mem_write32(n + k, 0u); return n; };
                u32 root = 0;
                if (shape != 0u) {
                    root = make_node();
                    if (shape == 2u) { for (u32 k = 0; k < 16u; ++k) m->cpu.mem_write32(root + 0x1Cu + 4u * k, make_node()); }
                    if (shape == 3u) { u32 cur = root; for (u32 d = 0; d < 13u; ++d) { const u32 n = make_node(); m->cpu.mem_write32(cur + 0x1Cu + 4u * (d % 3u), n); cur = n; } }
                    if (shape == 4u || shape == 5u) {
                        std::vector<u32> frontier{root};
                        for (u32 d = 0; d < 4u && next < 400u; ++d) {
                            std::vector<u32> fresh;
                            for (const u32 parent : frontier)
                                for (u32 k = 0; k < 16u; ++k)
                                    if ((rnd() % (shape == 4u ? 6u : 2u)) == 0u && next < 400u) { const u32 n = make_node(); m->cpu.mem_write32(parent + 0x1Cu + 4u * k, n); fresh.push_back(n); }
                            frontier = fresh;
                        }
                    }
                }
                m->cpu.mem_write32(kCtx + 0x4000u, root);
            }
            const std::string label = std::string(direct != 0u ? "quantiser tree free" : "quantiser end") + " depth " + std::to_string(depth) + " shape " + std::to_string(shape);
            const u32 entry = direct != 0u ? 0x80491124u : 0x80491448u, root_arg = direct != 0u ? a.cpu.mem_read32(kCtx + 0x4000u) : 0u;
            compare(label, a, b, a.run(entry, kCtx, root_arg, 400000u, depth), b.run(entry, kCtx, root_arg, 400000u, depth), false);
            check(a.cpu.mem_read32(kFreeLogBase) == b.cpu.mem_read32(kFreeLogBase), label + ": number of frees");
            if (direct == 0u) check(a.cpu.mem_read32(kCtx + 0x4000u) == 0u && b.cpu.mem_read32(kCtx + 0x4000u) == 0u, label + ": the root is cleared");
            frees += b.cpu.mem_read32(kFreeLogBase);
            ++cases;
        }
    }
    check(frees > 400, "the trees free many blocks");
    std::printf("  quantiser end / tree free: %u cases (%u frees) equal\n", cases, frees);
}

// The display swap (0x80495CBC ShowRaster, the three callbacks, 0x80494CD4, GcnSystem 0x80494EDC, 0x804981E8, 0x8049847C): original words
// against the owners. The SDK leaves both sides call (GX, VI, OS) are test owners installed on BOTH machines: each appends
// {id, r3..r9, [r3 or r4], f1..f6} to a log and returns a configured value (a few have effects the callers read back); RenderWare
// core and the engine's memory functions are the logging PPC doubles of install_cores (guest_call reaches them in both builds).
struct DisplayLeaf {
    u32 address;
    u32 regs;     // bit i: log r(3 + i)
    u32 fregs;    // bit i: log f(1 + i)
    int deref;    // 0 none, 3 = [r3], 4 = [r4]
};
constexpr DisplayLeaf kDisplayLeaves[] = {
    {0x803792C4u, 0x01, 0, 0}, {0x803793B0u, 0x01, 0, 0}, {0x8037847Cu, 0x01, 0, 0}, {0x80396098u, 0x00, 0, 0}, {0x803952FCu, 0x00, 0, 0},
    {0x80394EA4u, 0x06, 0, 0}, {0x80394F80u, 0x01, 0, 0}, {0x80395034u, 0x00, 0, 0}, {0x80394F3Cu, 0x01, 0, 0}, {0x80397444u, 0x03, 0, 0},
    {0x80396428u, 0x01, 0, 0}, {0x803964DCu, 0x00, 0, 0}, {0x80396E54u, 0x01, 0, 0}, {0x80396BD4u, 0x0F, 0, 0}, {0x80396CCCu, 0x03, 0, 0},
    {0x80397228u, 0x0F, 0, 0}, {0x803970E4u, 0x00, 0x01, 0}, {0x80399FCCu, 0x03, 0, 0}, {0x80399E58u, 0x03, 0, 0}, {0x80397430u, 0x01, 0, 0},
    {0x80393A88u, 0x03, 0, 0}, {0x80394AF4u, 0x07, 0, 0}, {0x80394BDCu, 0x01, 0, 0}, {0x80394D04u, 0x01, 0, 0}, {0x8039A444u, 0x01, 0x3F, 0},
    {0x8039A49Cu, 0x00, 0x3F, 0}, {0x8039A4E4u, 0x0F, 0, 0}, {0x8039A55Cu, 0x03, 0, 0}, {0x8037F19Cu, 0x01, 0, 0}, {0x8037FD38u, 0x00, 0, 0},
    {0x8037FE68u, 0x01, 0, 0}, {0x8037FEDCu, 0x01, 0, 0}, {0x80380004u, 0x00, 0, 0}, {0x80380138u, 0x00, 0, 0}, {0x8037E4F0u, 0x01, 0, 0},
    {0x8037E534u, 0x01, 0, 0}, {0x8049B7DCu, 0x01, 0, 0}, {0x80372508u, 0x03, 0, 0}, {0x80399DF0u, 0x07, 0, 0}, {0x80399E24u, 0x01, 0, 0},
    {0x80399778u, 0x1F, 0, 0}, {0x80399D44u, 0x0F, 0, 0}, {0x80396B54u, 0x01, 0, 0}, {0x80397CBCu, 0x7F, 0, 0}, {0x80397B98u, 0x03, 0, 4},
    {0x80399D98u, 0x01, 0, 0}, {0x80399DC4u, 0x01, 0, 0}, {0x803971B0u, 0x03, 0, 3}, {0x8039A2CCu, 0x01, 0, 0}};
constexpr u32 kDLog = 0x81314000u;   // +0 count; +4 retrace-count increment guard; entries (128 bytes) from +8
struct DisplayConfig {
    u32 tv_format = 0, next_field = 0, token_reached = 1, read_sync_first_mismatch = 0;
};

void install_display_leaves(Machine& m, const DisplayConfig& cfg) {
    for (u32 id = 0; id < sizeof(kDisplayLeaves) / sizeof(kDisplayLeaves[0]); ++id) {
        const DisplayLeaf leaf = kDisplayLeaves[id];
        m.cpu.install_hle(leaf.address, [leaf, id, cfg](Cpu& c) {
            const u32 index = c.mem_read32(kDLog);
            const u32 entry = kDLog + 8u + 128u * index;
            c.mem_write32(entry, id);
            for (u32 i = 0; i < 7u; ++i)
                if ((leaf.regs >> i) & 1u) c.mem_write32(entry + 4u + 4u * i, c.gpr[3 + i]);
            if (leaf.deref != 0) c.mem_write32(entry + 32u, c.mem_read32(c.gpr[leaf.deref]));
            for (u32 i = 0; i < 6u; ++i)
                if ((leaf.fregs >> i) & 1u) {
                    c.mem_write32(entry + 36u + 8u * i, static_cast<u32>(c.fpr[1 + i].bits[0] >> 32));
                    c.mem_write32(entry + 40u + 8u * i, static_cast<u32>(c.fpr[1 + i].bits[0]));
                }
            c.mem_write32(kDLog, index + 1u);
            u32 result = 0x1000u + id;
            switch (leaf.address) {
            case 0x80394EA4u: c.mem_write32(c.gpr[4], 0x11110000u + index); c.mem_write32(c.gpr[5], 0x22220000u + index); break;
            case 0x80380138u: result = cfg.tv_format; break;
            case 0x80380004u: result = cfg.next_field; break;
            case 0x8049B7DCu: result = cfg.token_reached; break;
            case 0x803964DCu: {   // the first read disagrees with the token the caller waits for (when asked), then agrees
                const u32 token = c.mem_read16(c.gpr[13] + 0x6220u);
                result = (cfg.read_sync_first_mismatch != 0u && c.mem_read32(kDLog + 4u) == 0u) ? token ^ 1u : token;
                c.mem_write32(kDLog + 4u, c.mem_read32(kDLog + 4u) + 1u);
                break;
            }
            case 0x803792C4u:   // a sleep on the retrace queue ends with a retrace
                if (c.gpr[3] == c.gpr[13] + 0x5BD4u) c.mem_write32(c.gpr[13] + 0x5BCCu, c.mem_read32(c.gpr[13] + 0x5BCCu) + 1u);
                break;
            default: break;
            }
            c.gpr[3] = result;
            return true;
        }, "display test leaf");
    }
}


// A guest function double (PPC) in the log format of the display leaves: it appends {id, r3.. (args)} to the log at kDLog and returns `returns`.
void stub_function(Machine& m, u32 address, u32 id, u32 args, u32 returns) {
    std::vector<u32> w = {enc_d(15, 12, 0, 0x8131), enc_d(32, 11, 12, 0x4000), (21u << 26) | (11u << 21) | (10u << 16) | (7u << 11) | (0u << 6) | (24u << 1),
                          0x7C000000u | (10u << 21) | (10u << 16) | (12u << 11) | (266u << 1), enc_d(14, 9, 0, static_cast<int>(id)), enc_d(36, 9, 10, 0x4008)};
    for (u32 a = 0; a < args; ++a) w.push_back(enc_d(36, 3 + a, 10, 0x400C + 4 * static_cast<int>(a)));
    w.push_back(enc_d(14, 11, 11, 1));
    w.push_back(enc_d(36, 11, 12, 0x4000));
    w.push_back(enc_d(15, 3, 0, static_cast<int>(returns >> 16)));
    w.push_back(enc_d(24, 3, 3, static_cast<int>(returns & 0xFFFFu)));   // ori r3, r3, lo
    w.push_back(0x4E800020u);
    for (u32 i = 0; i < w.size(); ++i) m.bus.write32(address + i * 4u, w[i]);
}

constexpr u32 kEngineMalloc = 0x81311400u, kEngineFreeFn = 0x81311500u, kArenaBlock = 0x81500000u;

void display_stubs(Machine& m) {
    stub_function(m, kEngineMalloc, 90, 2, kArenaBlock + 8u);   // the engine's malloc / free: the logging doubles of the leaves' format
    stub_function(m, kEngineFreeFn, 91, 1, 0u);
    m.cpu.mem_write32(kEngine + 0x108u, kEngineMalloc);
    m.cpu.mem_write32(kEngine + 0x10Cu, kEngineFreeFn);
}

constexpr u32 kModeRec = 0x81360000u, kDescriptor = 0x81360200u, kOutArea = 0x81360400u, kArgPtrCell = 0x81360800u, kArgBlock = 0x81360820u,
              kCtxRecord = 0x81361000u, kLockPointer = 0x81361400u, kScratchRaster = 0x81361800u;

struct DisplayState {
    u32 dbl = 0, first = 0, ee = 1, token = 5, ring_size = 3, read = 0, write = 1, vi_write = 0, vi_read = 0, cb = 0, pending = 0, hs_a = 0, hs_b = 0,
        field = 0, fifo_made = 0, interlace = 0, aa = 1, h8 = 480, h10 = 480, mode_word = 0, mode_kind = 0, black = 0, since = 3, minimum = 2;
};

u32 test_submit_frame(Cpu& c, u32 xfb, u32 clear) {   // the lowered GXCopyDisp site: logs as the GXCopyDisp leaf does
    c.gpr[3] = xfb;
    c.gpr[4] = clear;
    return c.serve_owner(0x80397444u) ? 1u : 0u;
}
u32 test_state_reset(Cpu& c, u32 a, u32 b, u32 d) {   // the lowered GXSetZMode site
    c.gpr[3] = a;
    c.gpr[4] = b;
    c.gpr[5] = d;
    return c.serve_owner(0x80399DF0u) ? 1u : 0u;
}

void prepare_display(Machine& m, const DisplayState& s, const DisplayConfig& cfg, u32 seed) {
    m.setup(0, 0);
    Cpu& c = m.cpu;
    install_display_leaves(m, cfg);
    if (m.native) rw_raster::install_display_owners(m.cpu, rw_raster::DisplayHooks{test_submit_frame, test_state_reset});
    const u32 none[14] = {};
    install_cores(m, none);   // the error pair and the scratch raster calls are the logging doubles of the image tests
    display_stubs(m);
    stub_function(m, 0x8048DD2Cu, 92, 1, kCtxRecord);
    stub_function(m, 0x8048AEC4u, 93, 4, kScratchRaster);
    stub_function(m, 0x8048AFC0u, 94, 3, kLockPointer);
    stub_function(m, 0x8000540Cu, 95, 3, 0u);
    stub_function(m, 0x8048AB90u, 96, 1, 0u);
    stub_function(m, 0x8048AC2Cu, 97, 1, 0u);
    stub_function(m, 0x8048DA50u, 98, 2, 0u);
    stub_function(m, 0x8048DDDCu, 99, 1, 0u);
    c.mem_write32(kDLog, 0u);
    c.mem_write32(kDLog + 4u, 0u);
    u32 lcg = seed;
    const auto rnd = [&]() { lcg = lcg * 1664525u + 1013904223u; return lcg >> 4; };
    const auto sda = [&](u32 off, u32 v) { c.mem_write32(kSda + off, v); };
    // the swap state
    const u32 base0 = 0x81500000u + 0x1000u * (rnd() % 8u), base1 = base0 + 0x40000u;
    sda(0x61A0u, base0); sda(0x61A4u, base1);
    sda(0x61A8u, (rnd() & 1u) ? base0 : base1); sda(0x61ACu, (rnd() & 1u) ? base0 : base1);
    sda(0x61B0u, s.read); sda(0x61B4u, s.write); sda(0x2AE4u, s.ring_size);
    sda(0x61B8u, s.vi_write); sda(0x61BCu, s.vi_read);
    sda(0x61C0u, s.cb); sda(0x61C4u, s.pending); sda(0x61C8u, s.hs_a); sda(0x61CCu, s.hs_b); sda(0x61D0u, s.field); sda(0x61D4u, s.fifo_made);
    sda(0x6198u, s.dbl); sda(0x2AD8u, s.first); sda(0x6188u, rnd()); sda(0x6194u, s.mode_kind); sda(0x618Cu, 1u + rnd() % 2u); sda(0x6190u, 1u + rnd() % 2u);
    sda(0x61F4u, kModeRec); sda(0x61E8u, 0x81502000u + 0x20u * (rnd() % 4u)); sda(0x61ECu, 0x81503000u); sda(0x619Cu, 0x81504000u); sda(0x2ADCu, 0x2000u + 0x400u * (rnd() % 3u));
    sda(0x2AE0u, s.black); sda(0x2AE8u, 0u); c.mem_write8(kSda + 0x2AE8u, static_cast<u8>(s.since)); c.mem_write8(kSda + 0x2AE9u, static_cast<u8>(s.minimum));
    c.mem_write16(kSda + 0x2AF0u, static_cast<u16>(s.token)); c.mem_write16(kSda + 0x6220u, static_cast<u16>(rnd()));
    sda(0x5BCCu, rnd()); sda(0x6200u, kPluginOffset); sda(0x620Cu, kCtxRecord); sda(0x6208u, kScratchRaster);
    for (u32 i = 0; i < 8u; ++i) {
        c.mem_write32(0x805E4238u + 8u * i, (rnd() & 1u) ? base0 : base1);
        c.mem_write32(0x805E4238u + 8u * i + 4u, rnd());
        c.mem_write16(kSda + 0x61E0u + 2u * i, static_cast<u16>(rnd()));
    }
    // the mode record
    c.mem_write32(kModeRec, s.mode_word);
    c.mem_write16(kModeRec + 4u, 640u); c.mem_write16(kModeRec + 6u, 480u); c.mem_write16(kModeRec + 8u, static_cast<u16>(s.h8));
    c.mem_write16(kModeRec + 0x10u, static_cast<u16>(s.h10));
    c.mem_write8(kModeRec + 0x18u, static_cast<u8>(s.interlace)); c.mem_write8(kModeRec + 0x19u, static_cast<u8>(s.aa));
    for (u32 i = 0x1Au; i < 0x3Cu; ++i) c.mem_write8(kModeRec + i, static_cast<u8>(rnd()));
    for (u32 i = 0; i < 0x3Cu; i += 4u) c.mem_write32(kDescriptor + i, rnd());
    c.mem_write16(kDescriptor + 4u, 640u); c.mem_write16(kDescriptor + 6u, 480u); c.mem_write16(kDescriptor + 8u, 480u);
    c.mem_write8(kDescriptor + 0x19u, static_cast<u8>(s.aa));
    c.mem_write32(kArgBlock, kDescriptor); c.mem_write32(kArgBlock + 4u, 2u); c.mem_write32(kArgBlock + 8u, 0x800u);
    c.mem_write32(kArgPtrCell, kArgBlock);
    c.mem_write32(kArgPtrCell + 0x40u, 0u);   // a null argument block
    c.mem_write32(kCtxRecord + 0x50u, rnd());
    for (u32 i = 0; i < 0x400u; i += 4u) c.mem_write32(kOutArea + i, 0xAAAA0000u + i);
    c.set_msr((c.msr() & ~0x8000u) | (s.ee != 0u ? 0x8000u : 0u));
}

void compare_display(const std::vector<u8>& dol, const std::string& label, u32 entry, const DisplayState& s, const DisplayConfig& cfg, u32 seed, u32 r3, u32 r4,
                     u32 r5 = 0, u32 r6 = 0, bool compare_r3 = true, u32* stop_check = nullptr) {
    Machine a(dol, false), b(dol, true);
    for (Machine* m : {&a, &b}) prepare_display(*m, s, cfg, seed);
    a.arg6 = b.arg6 = r6;   // Machine::run sets r5 from its parameter and r6 from arg6
    const auto x = a.run(entry, r3, r4, 400000u, r5);
    const auto y = b.run(entry, r3, r4, 400000u, r5);
    compare(label, a, b, x, y, compare_r3);
    check(a.cpu.msr() == b.cpu.msr(), label + ": MSR");
    check(a.cpu.mem_read32(kDLog) == b.cpu.mem_read32(kDLog) && a.cpu.mem_read32(kStubLog) == b.cpu.mem_read32(kStubLog), label + ": leaf / core call counts");
    g_image_calls += b.cpu.mem_read32(kDLog);
    ++g_image_cases;
    (void)stop_check;
}

void test_display(const std::vector<u8>& dol) {
    unsigned base = g_image_cases;
    const DisplayConfig cfg;
    // ShowRaster
    for (const u32 dbl : {0u, 1u})
        for (const u32 first : {0u, 1u})
            for (const u32 full : {0u, 1u})
                for (const u32 cb : {0u, 1u})
                    for (const u32 ee : {0u, 1u})
                        for (const u32 token : {0u, 0xDFFFu, 0x1234u, 0xFFFFu}) {
                            DisplayState s;
                            s.dbl = dbl; s.first = first; s.cb = cb; s.ee = ee; s.token = token; s.ring_size = 3;
                            s.read = 0; s.write = full != 0u ? 2u : 1u;   // full: write - read == size - 1
                            compare_display(dol, "ShowRaster dbl " + std::to_string(dbl) + " first " + std::to_string(first) + " full " + std::to_string(full) + " cb " +
                                                     std::to_string(cb) + " ee " + std::to_string(ee) + " token " + hex(token),
                                            0x80495CBCu, s, cfg, 11u + dbl * 3u + first * 5u + full * 7u + token, 0u, 0u);
                        }
    for (const u32 write : {0u, 1u, 2u})   // write - read == -1 is "full" too
        for (const u32 read : {1u, 2u}) {
            DisplayState s; s.read = read; s.write = write; s.ring_size = 4;
            compare_display(dol, "ShowRaster ring " + std::to_string(read) + "/" + std::to_string(write), 0x80495CBCu, s, cfg, 31u + read * 3u + write, 0u, 0u);
        }
    // the callbacks
    for (const u32 entry : {0x80494A2Cu, 0x80494B0Cu, 0x80494BE8u})
        for (const u32 dbl : {0u, 1u})
            for (const u32 field : {0u, 1u})
                for (const u32 pending : {0u, 1u})
                    for (const u32 hs_a : {0u, 1u})
                        for (const u32 hs_b : {0u, 1u})
                            for (const u32 write : {0u, 1u, 2u})
                                for (const u32 since : {0u, 1u, 5u})
                                    for (const u32 reached : {0u, 1u})
                                        for (const u32 cbflag : {0u, 1u}) {
                                        DisplayState s;
                                        s.dbl = dbl; s.field = field; s.pending = pending; s.hs_a = hs_a; s.hs_b = hs_b; s.write = write; s.read = 1; s.vi_read = 1;
                                        s.vi_write = write; s.since = since; s.minimum = 2; s.black = hs_a; s.cb = cbflag;
                                        DisplayConfig c2 = cfg; c2.token_reached = reached;
                                        compare_display(dol, "callback " + hex(entry) + " dbl " + std::to_string(dbl) + " field " + std::to_string(field) + " pending " +
                                                                 std::to_string(pending) + " a " + std::to_string(hs_a) + " b " + std::to_string(hs_b) + " write " +
                                                                 std::to_string(write) + " since " + std::to_string(since) + " reached " + std::to_string(reached) + " cb " + std::to_string(cbflag),
                                                        entry, s, c2, 77u + hs_a * 3u + write, 0u, 0u, 0u, 0u, false);
                                    }
    // the video mode setter
    for (const u32 count : {0u, 1u, 2u, 3u, 0xFFFFFFFFu})
        for (const u32 desc : {0u, kDescriptor}) {
            for (const u32 tv : {0u, 1u, 2u, 3u, 5u, 0xFFFFFFFFu}) {
                if (desc != 0u && tv != 0u) continue;
                DisplayConfig c2 = cfg; c2.tv_format = tv;
                DisplayState s;
                compare_display(dol, "video mode count " + hex(count) + (desc != 0u ? " descriptor" : " tv " + std::to_string(tv)), 0x80494CD4u, s, c2, 5u + count, desc, count, 0u, 0u, false);
            }
        }
    compare_display(dol, "the default standard function", 0x80494CCCu, DisplayState{}, cfg, 1u, 7u, 0u);
    // GcnSystem
    for (u32 request = 0; request <= 24u; ++request)
        for (const u32 count : {0u, 1u, 2u, 27u, 0xFFFFFFFFu})
            for (const u32 variant : {0u, 1u}) {
                if (request != 0u && request != 2u && request != 6u && request != 11u && request != 4u && variant != 0u) continue;
                DisplayState s;
                s.mode_kind = variant == 0u ? (request == 6u ? count % 3u : 0u) : 0x2Au;
                DisplayConfig c2 = cfg; c2.tv_format = request == 2u ? (count % 4u == 3u ? 3u : count % 3u) : 0u;
                c2.next_field = variant;
                c2.read_sync_first_mismatch = variant;
                u32 arg = request == 0u ? (variant != 0u ? kArgPtrCell : kArgPtrCell + 0x40u) : 0xB0B0B0B0u;
                if (request == 0u && variant == 0u) { /* a null argument block */ }
                if (request == 2u) { s.interlace = variant; s.fifo_made = variant; s.h8 = variant != 0u ? 480u : 240u; s.h10 = count == 1u ? s.h8 : 480u; s.mode_word = (count & 1u) != 0u ? 1u : 0u; }
                if (request == 3u) { s.pending = 0; s.vi_write = s.vi_read = 1; }
                compare_display(dol, "GcnSystem request " + std::to_string(request) + " count " + hex(count) + " variant " + std::to_string(variant), 0x80494EDCu, s, c2,
                                9u + request * 7u + count, request, kOutArea, arg, count);
            }
    std::printf("  display swap: %u cases equal (%u leaf calls compared)\n", g_image_cases - base, g_image_calls);
}

// VIWaitForRetrace 0x8037ECD4 (native_vi_wait.cpp): the original 14 words against the owner and its two resume rows. The three leaves
// (OSDisableInterrupts, OSRestoreInterrupts, OSSleepThread) are test owners installed on BOTH machines; the gates follow the real
// gates' rule (a deliverable interrupt is taken at the entry and, after the restore, before the return). Every leaf entry logs
// {id, r0, r1, r3, r4, r5, r30, r31, LR, MSR, CR0, retrace count}; the native interrupt handler logs {tag, pc, r0, r1, r3, LR, r30, r31}.
constexpr u32 kVLog = 0x81316000u;
struct WaitCase {
    u32 ee = 1;            // MSR[EE] on entry
    u32 count = 5;         // the retrace count
    u32 delta = 1;         // what the count changes by at the waking sleep
    u32 wake_on = 1;       // the sleep (1-based) that ends with a changed count
    u32 spurious = 0;      // 1: the first sleep (before the waking one) also changes the count by `delta`, then the count is back
    u32 interrupt_on = 0;  // the sleep (1-based) during which a native interrupt becomes pending; 0 = none
    u32 interrupt_at_start = 0;
    u32 so = 0;                  // XER[SO] set on entry
    u32 interrupt_at_gate = 0;   // the interrupt becomes deliverable just before the first OSDisableInterrupts (not before the function)
    u32 expect_taken = 0;  // the pc the native interrupt handler must see (the interrupt is taken once), 0 = it is not taken
};

void install_wait_leaves(Machine& m, const WaitCase& k) {
    struct State { u32 sleeps = 0, gates = 0; };
    const auto state = std::make_shared<State>();
    const auto log = [](Cpu& c, u32 id) {
        const u32 index = c.mem_read32(kVLog), entry = kVLog + 16u + 64u * index;
        const u32 words[12] = {id, c.gpr[0], c.gpr[1], c.gpr[3], c.gpr[4], c.gpr[5], c.gpr[30], c.gpr[31], c.lr, c.msr(), c.cr_field(0),
                               c.mem_read32(c.gpr[13] + 0x5BCCu)};
        for (u32 i = 0; i < 12u; ++i) c.mem_write32(entry + 4u * i, words[i]);
        c.mem_write32(kVLog, index + 1u);
    };
    const auto gate = [log, state, k](Cpu& c, u32 id, u32 entry, bool restore) {
        if (!restore && k.interrupt_at_gate != 0u && state->gates++ == 0u) c.set_native_interrupt(true);
        log(c, id);
        if (c.interrupt_pending()) {
            c.leave_to_dispatcher(entry, FallbackReason::Yield, entry);
            return true;
        }
        const u32 previous = c.msr(), argument = c.gpr[3];
        const bool enable = restore && argument != 0u;
        const u32 next = enable ? (previous | kMsrEE) : (previous & ~kMsrEE);
        if (restore) {
            const s32 value = static_cast<s32>(argument);
            c.set_cr_field(0, (value < 0 ? 8u : value > 0 ? 4u : 2u) | ((c.xer & kXerSO) != 0u ? 1u : 0u));
            c.gpr[4] = previous;
            c.gpr[5] = next;
        } else {
            c.gpr[4] = next;
        }
        c.set_msr(next);
        c.gpr[3] = (previous & kMsrEE) != 0u ? 1u : 0u;
        if (c.interrupt_pending()) c.leave_to_dispatcher(c.lr, FallbackReason::Yield, entry);
        return true;
    };
    m.cpu.install_hle(0x8037611Cu, [gate](Cpu& c) { return gate(c, 1u, 0x8037611Cu, false); }, "test: OSDisableInterrupts");
    m.cpu.install_hle(0x80376144u, [gate](Cpu& c) { return gate(c, 2u, 0x80376144u, true); }, "test: OSRestoreInterrupts");
    m.cpu.install_hle(0x803792C4u, [log, state, k](Cpu& c) {
        log(c, 3u);
        if (c.interrupt_pending()) { c.leave_to_dispatcher(0x803792C4u, FallbackReason::Yield, 0x803792C4u); return true; }
        ++state->sleeps;
        if (state->sleeps > 40u) { c.request_stop(StopReason::ExecutionFault, "test: the wait never ends"); return true; }
        const u32 count_word = c.gpr[13] + 0x5BCCu;
        if (c.gpr[3] == c.gpr[13] + 0x5BD4u) {
            if (state->sleeps == k.wake_on) c.mem_write32(count_word, c.mem_read32(count_word) + k.delta);
            else if (k.spurious != 0u && state->sleeps == 1u) c.mem_write32(count_word, c.mem_read32(count_word) + k.delta);
            else if (k.spurious != 0u && state->sleeps == 2u && k.wake_on > 2u) c.mem_write32(count_word, c.mem_read32(count_word) - k.delta);
        }
        if (state->sleeps == k.interrupt_on) c.set_native_interrupt(true);
        c.gpr[3] = 0x1234u + state->sleeps;
        c.gpr[4] = 0x5A5A0000u + state->sleeps;
        c.gpr[0] = 0x0BADu;
        return true;
    }, "test: OSSleepThread");
    m.cpu.set_native_interrupt_handler([](Cpu& c) {
        const u32 index = c.mem_read32(kVLog), entry = kVLog + 16u + 64u * index;
        const u32 words[8] = {0xA5A50000u, c.pc, c.gpr[0], c.gpr[1], c.gpr[3], c.lr, c.gpr[30], c.gpr[31]};
        for (u32 i = 0; i < 8u; ++i) c.mem_write32(entry + 4u * i, words[i]);
        c.mem_write32(kVLog, index + 1u);
        c.set_native_interrupt(false);
    });
}

void compare_wait(const std::vector<u8>& dol, const std::string& label, const WaitCase& k, u32 seed) {
    Machine a(dol, false), b(dol, true);
    vi_wait::install(b.cpu);
    for (u32 w = 0x8037ECD8u; w < 0x8037ED28u; w += 4u) b.bus.write32(w, 0u);   // the lifted build prunes the body: only the owner and its two rows may run
    u32 lcg = seed;
    const auto rnd = [&]() { lcg = lcg * 1664525u + 1013904223u; return lcg >> 4; };
    const u32 caller_r30 = rnd() | 0x10u, caller_r31 = rnd() | 0x20u, caller_r0 = rnd(), caller_r4 = rnd(), caller_r5 = rnd();
    for (Machine* m : {&a, &b}) {
        m->setup(0, 0);
        install_wait_leaves(*m, k);
        m->cpu.mem_write32(kVLog, 0u);
        m->cpu.mem_write32(kSda + 0x5BCCu, k.count);
        m->cpu.set_msr((m->cpu.msr() & ~kMsrEE) | (k.ee != 0u ? kMsrEE : 0u));
        m->cpu.gpr[30] = caller_r30; m->cpu.gpr[31] = caller_r31; m->cpu.gpr[0] = caller_r0;
        m->cpu.set_cr_field(0, 1u); m->cpu.set_cr_field(2, 5u);
        if (k.so != 0u) m->cpu.xer |= kXerSO;
        if (k.interrupt_at_start != 0u) m->cpu.set_native_interrupt(true);
    }
    const auto x = a.run(0x8037ECD4u, 0xC0DE0001u, caller_r4, 400000u, caller_r5);
    const auto y = b.run(0x8037ECD4u, 0xC0DE0001u, caller_r4, 400000u, caller_r5);
    compare(label, a, b, x, y, true);
    Cpu& p = a.cpu;
    Cpu& q = b.cpu;
    for (u32 r = 0; r < 14u; ++r) check(p.gpr[r] == q.gpr[r], label + ": r" + std::to_string(r) + " " + hex(p.gpr[r]) + " / " + hex(q.gpr[r]));
    for (u32 f = 0; f < 8u; ++f) check(p.cr_field(f) == q.cr_field(f), label + ": CR" + std::to_string(f));
    check(p.msr() == q.msr(), label + ": MSR " + hex(p.msr()) + " / " + hex(q.msr()));
    check(p.xer == q.xer && p.ctr == q.ctr, label + ": XER / CTR");
    check(p.native_interrupt() == q.native_interrupt(), label + ": the native interrupt line");
    check(p.mem_read32(kSda + 0x5BCCu) == q.mem_read32(kSda + 0x5BCCu), label + ": the retrace count");
    for (u32 w = kStackTop - 0x40u; w < kStackTop + 0x8u; w += 4u)
        check(p.mem_read32(w) == q.mem_read32(w), label + ": stack word " + hex(w) + " " + hex(p.mem_read32(w)) + " / " + hex(q.mem_read32(w)));
    const u32 n = p.mem_read32(kVLog);
    check(n == q.mem_read32(kVLog), label + ": leaf calls " + std::to_string(n) + " / " + std::to_string(q.mem_read32(kVLog)));
    for (u32 i = 0; i < n * 16u && i < 64u * 16u; ++i)
        check(p.mem_read32(kVLog + 16u + 4u * i) == q.mem_read32(kVLog + 16u + 4u * i), label + ": log word " + std::to_string(i));
    u32 taken = 0, taken_pc = 0;
    for (u32 i = 0; i < n && i < 64u; ++i)
        if (q.mem_read32(kVLog + 16u + 64u * i) == 0xA5A50000u) { ++taken; taken_pc = q.mem_read32(kVLog + 16u + 64u * i + 4u); }
    check(taken == (k.expect_taken != 0u ? 1u : 0u) && (k.expect_taken == 0u || taken_pc == k.expect_taken),
          label + ": the interrupt is taken " + std::to_string(taken) + " time(s) at " + hex(taken_pc) + ", expected " + hex(k.expect_taken));
    g_image_calls += q.mem_read32(kVLog);
    ++g_image_cases;
}

void test_vi_wait(const std::vector<u8>& dol) {
    const unsigned base = g_image_cases;
    u32 seed = 1u;
    for (const u32 ee : {0u, 1u})
        for (const u32 count : {0u, 5u, 0x7FFFFFFFu, 0xFFFFFFFFu, 0x80000000u})
            for (const u32 delta : {1u, 0x80000001u, 0xFFFFFFFFu, 0x100u})
                for (const u32 wake_on : {1u, 2u, 3u}) {
                    WaitCase k; k.ee = ee; k.count = count; k.delta = delta; k.wake_on = wake_on;
                    compare_wait(dol, "VIWaitForRetrace ee " + std::to_string(ee) + " count " + hex(count) + " delta " + hex(delta) + " wake " + std::to_string(wake_on), k, seed++);
                    if (wake_on > 1u) {
                        k.spurious = 1u;
                        compare_wait(dol, "VIWaitForRetrace spurious ee " + std::to_string(ee) + " count " + hex(count) + " delta " + hex(delta) + " wake " + std::to_string(wake_on), k, seed++);
                    }
                }
    for (const u32 count : {5u, 0x7FFFFFFFu}) {
        WaitCase k; k.so = 1; k.count = count; k.wake_on = 2;
        compare_wait(dol, "VIWaitForRetrace XER[SO] count " + hex(count), k, seed++);
    }
    // interrupts: deliverable at OSDisableInterrupts (taken at the gate's entry), and made deliverable by the restore (taken before 0x8037ED10)
    for (const u32 ee : {0u, 1u})
        for (const u32 wake_on : {1u, 2u})
            for (const u32 at : {0u, 1u, 2u, 3u}) {
                WaitCase k; k.ee = ee; k.wake_on = wake_on;
                k.interrupt_at_start = at == 0u ? 1u : 0u;
                k.interrupt_at_gate = at == 3u ? 1u : 0u;
                k.interrupt_on = at == 3u ? 0u : at;
                if (ee != 0u) k.expect_taken = at == 0u ? 0x8037ECD4u : at == 3u ? 0x8037611Cu : (at <= wake_on ? 0x8037ED10u : 0u);   // at the gate's entry / before the row after the restore
                compare_wait(dol, "VIWaitForRetrace interrupt ee " + std::to_string(ee) + " wake " + std::to_string(wake_on) + " at " + std::to_string(at), k, seed++);
            }
    std::printf("  VIWaitForRetrace: %u cases equal (%u leaf calls compared)\n", g_image_cases - base, g_image_calls);
}

// The controls: a changed behaviour of the compared state must be told apart by `compare`.
void test_negative_controls(const std::vector<u8>& dol) {
    const auto run_destroy = [&](bool native, u32 token, u32 next) {
        auto machine = std::make_unique<Machine>(dol, native);
        machine->setup(next > 0u ? next - 1u : 0u, next);
        build(*machine, {kRasters, 0, 4, 0, 0, 64, 64, 3, static_cast<u16>(token), kBlock + 0x200u});
        const auto outcome = machine->run(kDestroy, 0u, kRasters);
        return std::make_pair(std::move(machine), outcome);
    };
    auto x = run_destroy(false, 5, 5);
    auto y = run_destroy(true, 5, 5);
    check(x.first->cpu.mem_read32(kFreeLog) == 1u && x.first->cpu.mem_read32(kFreeLog + 4u) == kBlock + 0x200u,
          "the original frees the raster's block once");
    check(x.first->sync.token() == 5u && y.first->sync.token() == 5u, "the token it issued reaches the sync owner");
    check(x.first->cpu.mem_read16(kSda + kSdaTokenNext) == 6u && y.first->cpu.mem_read16(kSda + kSdaTokenNext) == 6u,
          "the next token advances by one");
    // an owner that left the token alone would differ from the original in these observables
    auto z = run_destroy(true, 4, 5);   // token 4 != next 5: no new token issued
    check(z.first->sync.token() != 5u || z.first->cpu.mem_read16(kSda + kSdaTokenNext) != 6u,
          "negative control: a Destroy that did not issue a token is told apart");
    check(z.first->cpu.mem_read32(kFreeLog) == 1u, "the block is freed either way");
    // the ring wrap: next 0xDFFF issues, then next is 0
    auto w = run_destroy(false, 0xDFFF, 0xDFFF);
    auto v = run_destroy(true, 0xDFFF, 0xDFFF);
    check(w.first->cpu.mem_read16(kSda + kSdaTokenNext) == 0u && v.first->cpu.mem_read16(kSda + kSdaTokenNext) == 0u,
          "the token ring wraps at 0xE000");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "usage: rw_raster_contract_tests <sys/main.dol>");
        const std::vector<u8> dol = read_file(argv[1]);
        if (const char* only = std::getenv("ONLY_TESTS")) {   // a focused run (mutation sweeps): ONLY_TESTS=display
            if (std::string(only) == "vi") {
                test_vi_wait(dol);
                std::printf("rw_raster_contract_tests (vi only): %s\n", g_failures == 0 ? "ok" : "FAILED");
                return g_failures == 0 ? 0 : 1;
            }
            if (std::string(only) == "display") {
                test_display(dol);
                std::printf("rw_raster_contract_tests (display only): %s\n", g_failures == 0 ? "ok" : "FAILED");
                return g_failures == 0 ? 0 : 1;
            }
        }
        test_small(dol);
        test_token_reached(dol);
        test_destroy(dol);
        test_palette(dol);
        test_unlock(dol);
        test_lock(dol);
        test_layout(dol);
        test_set_image(dol);
        test_image_bodies(dol);
        test_converters(dol);
        test_create(dol);
        test_size(dol);
        test_quant_end(dol);
        test_clear(dol);
        test_display(dol);
        test_vi_wait(dol);
        test_texture_read(dol);
        test_negative_controls(dol);
    } catch (const std::exception& e) {
        std::printf("FATAL: %s\n", e.what());
        return 2;
    }
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("rw_raster_contract_tests: ok\n");
    return 0;
}
