// The native GX synchronisation against the original GX library words.
//
// liveness-roots: this test enters no generated code - the GX SDK functions,
// their interrupt handlers, RenderWare's XFB-ring callback and every OS routine
// they call run as ORIGINAL words in the reference interpreter - so its
// literals must not keep the replaced bodies in the shipping image:
// liveness-roots: reference-interpreter-only
//
// Two machines load the original DOL (docs/native-time-20261005/STAGE4_GX_SYNC.md).
// ORIGINAL runs the SDK's own words over test-only register fixtures standing in
// for the console's command processor, pixel engine, processor interface,
// memory interface and write-gather pipe (the fixtures echo what is written,
// as the emulated devices did); NATIVE serves the same calls with the
// production owners (NativeGxSync) and takes the interrupts as native
// deliveries. Compared:
//
//   FIFO binding   GXSetCPUFifo / GXSetGPFifo / GXGetFifoPtrs, linked and
//                  unlinked, on every MEM1 byte outside the stack window;
//   init           __GXFifoInit and __GXPEInit on every MEM1 byte outside the
//                  stack window and the handler-table slots 17-19 (documented:
//                  ORIGINAL registers the SDK's three handlers, NATIVE none,
//                  because NativeInterrupts delivers them);
//   breakpoint     GXEnableBreakPt, then ORIGINAL calls GXCPInterruptHandler
//                  (17, context) with the CP status a reached breakpoint shows
//                  while NATIVE takes the native breakpoint delivery; the
//                  callback is RenderWare's real XFB-ring callback 0x80494A2C
//                  in its three paths (advance and re-arm, ring empty: disarm,
//                  displayed XFB: defer); compared: __GXData, the GX library's
//                  small-data words, RenderWare's ring words and ring, the OS
//                  current context;
//   finish         ORIGINAL GXFinishInterruptHandler(19, context) vs the
//                  native finish delivery: DrawDone, FinishQueue, the same;
//   shutdown       __GXShutdown(FALSE) through its settle (no time, 9 ticks,
//                  10 ticks, a moved counter) and the final pass, on every MEM1
//                  byte outside the stack window. ORIGINAL's final pass runs
//                  __GXAbort, whose OSGetTime spins cannot end under a frozen
//                  clock: the fixture's OSGetTime advances one tick per call
//                  for that pass only.

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "support/os_entry.h"
#include "gekko/native_gx_sync.h"
#include "gekko/native_interrupts.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using gekko::u16;
using gekko::u32;
using gekko::u64;
using gekko::u8;

constexpr u32 kSda = 0x805EC500u;
constexpr u32 kSda2 = 0x805FA780u;
constexpr u32 kStackTop = 0x817E0000u;
constexpr u32 kStackWindow = 0x8000u;
constexpr u32 kFinish = 0x817FF000u;  // sentinel return address (holds `b .`)

constexpr u32 kGxShutdown = 0x80393754u;
constexpr u32 kGxSetCpuFifo = 0x80394BDCu;
constexpr u32 kGxSetGpFifo = 0x80394D04u;
constexpr u32 kGxGetFifoPtrs = 0x80394EA4u;
constexpr u32 kGxEnableBreakPt = 0x80394F80u;
constexpr u32 kGxFifoInit = 0x80395088u;
constexpr u32 kGxPeInit = 0x80396840u;
constexpr u32 kGxCpHandler = 0x803949C0u;      // GXCPInterruptHandler
constexpr u32 kGxFinishHandler = 0x803967C0u;  // GXFinishInterruptHandler
constexpr u32 kRwBreakPointCallback = 0x80494A2Cu;
constexpr u32 kOsGetTime = 0x80379628u;

constexpr u32 kSdaHandlerTable = 0x5A90u;
constexpr u32 kHandlerTable = 0x80003040u;
constexpr u32 kOsGlobalMask = 0x800000C4u;
constexpr u32 kOsUserMask = 0x800000C8u;
constexpr u32 kCurrentContext = 0x800000D4u;
constexpr u32 kCurrentContextPhysical = 0x800000C0u;
constexpr u32 kOsCurrentThread = 0x800000E4u;
constexpr u32 kInterruptedContext = 0x81100000u;
constexpr u32 kThread = 0x81200000u;           // what OSGetCurrentThread returns
constexpr u32 kFifo = 0x81000000u;             // a GXFifoObj
constexpr u32 kOtherFifo = 0x81000040u;
constexpr u32 kOut = 0x81000080u;              // GXGetFifoPtrs' rd / wr words
constexpr u32 kSdaGxFirst = 0x5DF8u;           // the GX library's small-data words
constexpr u32 kSdaGxEnd = 0x5E58u;
constexpr u32 kSdaRwFirst = 0x6198u;           // RenderWare's XFB-ring words
constexpr u32 kSdaRwEnd = 0x61F8u;
constexpr u32 kRwRing = 0x805E4238u;           // {xfb, write pointer} x 3
constexpr u32 kGxDataSize = 0x600u;

int g_failures = 0;
int g_cases = 0;
int g_documented = 0;

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

std::vector<u8> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}

std::string hex(u32 value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", value);
    return text;
}

void check(bool ok, const std::string& what) {
    ++g_cases;
    if (ok) return;
    ++g_failures;
    if (g_failures <= 60) std::fprintf(stderr, "MISMATCH %s\n", what.c_str());
}

void documented(bool ok, const std::string& what) {
    ++g_documented;
    check(ok, "documented difference broken: " + what);
}

// A 16-bit register file that echoes writes (the emulated CP / PE / MI did).
// Offset 0 can be pinned to a status value (the CP status register). Test-only.
class Regs16 : public gekko::MmioDevice {
public:
    explicit Regs16(const char* name) : name_(name) { regs.fill(0u); }
    const char* name() const override { return name_; }
    u16 read16(u32 address) override {
        const u32 index = (address & 0xFFu) / 2u;
        if (index == 0u && pin_status) return status;
        return regs[index];
    }
    void write16(u32 address, u16 value) override {
        regs[(address & 0xFFu) / 2u] = value;
        ++writes;
    }
    u32 read32(u32 address) override { return (static_cast<u32>(read16(address)) << 16) | read16(address + 2u); }
    void write32(u32 address, u32 value) override {
        write16(address, static_cast<u16>(value >> 16));
        write16(address + 2u, static_cast<u16>(value));
    }
    std::array<u16, 0x80> regs{};
    bool pin_status = false;
    u16 status = 0;
    u64 writes = 0;

private:
    const char* name_;
};

// The processor interface: the mask register SetInterruptMask writes and the
// CPU FIFO registers GXSetCPUFifo writes, echoed as the emulated PI did
// (0x3FFFFFE0 on the FIFO registers). Test-only.
class PiFixture : public gekko::MmioDevice {
public:
    const char* name() const override { return "test PI"; }
    u32 read32(u32 address) override { return regs[(address & 0xFFu) / 4u]; }
    void write32(u32 address, u32 value) override {
        const u32 offset = address & 0xFFu;
        regs[offset / 4u] = (offset >= 0x0Cu && offset <= 0x14u) ? (value & 0x3FFFFFE0u) : value;
        if (offset == 0x18u) ++fifo_resets;
    }
    std::array<u32, 0x40> regs{};
    u64 fifo_resets = 0;
};

// The write-gather pipe: __GXShutdown pads it with zero words. Test-only.
class PipeFixture : public gekko::MmioDevice {
public:
    const char* name() const override { return "test write-gather pipe"; }
    void write8(u32, u8) override { ++bytes; }
    void write16(u32, u16) override { bytes += 2u; }
    void write32(u32, u32) override { bytes += 4u; }
    u64 bytes = 0;
};

// The game's memory a comparison covers: the program image and the OS heap
// region after it - one window range over two regions (offset = address -
// kMemoryBegin).
constexpr u32 kMemoryBegin = gekko::GuestMemory::kImageBase;
constexpr u32 kMemoryEnd = gekko::GuestMemory::kLegacyHeapEnd;
constexpr u32 kMemorySize = kMemoryEnd - kMemoryBegin;

struct Machine {
    gekko::GuestMemory bus;
    gekko::Cpu cpu{bus};
    u32 gx = 0;

    explicit Machine(const std::vector<u8>& dol) {
        const auto word = [&](std::size_t offset) { return gekko::load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(static_cast<u64>(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        bus.write32(kFinish, 0x48000000u);
        // The OS's system-call vector as __OSExceptionInit / __OSInitSystemCall
        // leave it (PPCSync is an `sc`; the native OS entry performs its head,
        // which only toggles HID0's sync bits), and a current OSContext - the
        // current thread's - for the entry to find.
        test_support::install_os_system_call(bus);
        bus.write32(test_support::kOsCurrentContext, kThread);
        gx = bus.peek32(kSda2 + 0xFD8u);
        require(gx >= kMemoryBegin && gx < kMemoryEnd, "the DOL's __GXData pointer");
    }
    // The state every case starts from: the OS globals, the pointer words GXInit
    // stores, a zeroed __GXData, two FIFO objects.
    void setup() {
        cpu.clear_stop();
        cpu.reset();
        cpu.gpr[1] = kStackTop;
        cpu.gpr[2] = kSda2;
        cpu.gpr[13] = kSda;
        cpu.mem_write32(kSda + kSdaHandlerTable, kHandlerTable);
        cpu.mem_write32(kOsGlobalMask, 0xFFFFFFFFu);
        cpu.mem_write32(kOsUserMask, 0u);
        cpu.mem_write32(kOsCurrentThread, kThread);
        cpu.mem_write32(kSda + 0x5DF8u, 0xCC003000u);  // __piReg
        cpu.mem_write32(kSda + 0x5DFCu, 0xCC000000u);  // __cpReg
        cpu.mem_write32(kSda + 0x5E00u, 0xCC001000u);  // __peReg
        cpu.mem_write32(kSda + 0x5E04u, 0xCC004000u);  // __memReg
        for (u32 a = kSda + 0x5E08u; a < kSda + kSdaGxEnd; a += 4u) cpu.mem_write32(a, 0u);
        for (u32 i = 0; i < kGxDataSize; i += 4u) cpu.mem_write32(gx + i, 0u);
        const u32 fifo[8] = {0x80F00000u, 0x80F3FFFCu, 0x00040000u, 0x0003C000u,
                             0x00020000u, 0x80F00000u, 0x80F00060u, 0x00000060u};
        const u32 other[8] = {0x80E00000u, 0x80E0FFFCu, 0x00010000u, 0x0000C000u,
                              0x00008000u, 0x80E08000u, 0x80E00020u, 0u};
        for (u32 i = 0; i < 8u; ++i) {
            cpu.mem_write32(kFifo + 4u * i, fifo[i]);
            cpu.mem_write32(kOtherFifo + 4u * i, other[i]);
        }
        cpu.mem_write32(kOut, 0u);
        cpu.mem_write32(kOut + 4u, 0u);
    }
    void call(u32 entry, u32 r3 = 0u, u32 r4 = 0u, u32 r5 = 0u) {
        cpu.gpr[3] = r3;
        cpu.gpr[4] = r4;
        cpu.gpr[5] = r5;
        cpu.pc = entry;
        cpu.lr = kFinish;
        unsigned steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 20000000u) cpu.run_steps(1u);
        require(!cpu.stopped(), ("the call stopped the machine: " + cpu.stop().detail).c_str());
        require(cpu.pc == kFinish, "the call did not return");
        require(cpu.gpr[1] == kStackTop, "the call left r1 moved");
    }
    u32 word(u32 address) { return bus.peek32(address); }
    u8 byte(u32 address) { return bus.peek8(address); }
};

struct Original {
    Machine m;
    Regs16 cp{"test CP"};
    Regs16 pe{"test PE"};
    Regs16 mi{"test MI"};
    PiFixture pi;
    PipeFixture pipe;
    u64 clock = 0;
    bool advancing_clock = false;
    explicit Original(const std::vector<u8>& dol) : m(dol) {
        m.bus.register_mmio(0xCC000000u, 0x100u, &cp);
        m.bus.register_mmio(0xCC001000u, 0x100u, &pe);
        m.bus.register_mmio(0xCC003000u, 0x100u, &pi);
        m.bus.register_mmio(0xCC004000u, 0x100u, &mi);
        m.bus.register_mmio(0xCC008000u, 0x100u, &pipe);
        // OSGetTime: the test's clock (fixed unless advancing, see shutdown).
        m.cpu.install_hle(kOsGetTime, [this](gekko::Cpu& c) {
            if (advancing_clock) ++clock;
            c.gpr[3] = static_cast<u32>(clock >> 32);
            c.gpr[4] = static_cast<u32>(clock);
            return true;
        }, "fixture OSGetTime");
        m.setup();
    }
};

struct Native {
    Machine m;
    gekko::NativeInterrupts interrupts{m.cpu};
    gekko::NativeGxSync sync{m.cpu, interrupts};
    explicit Native(const std::vector<u8>& dol) : m(dol) {
        m.setup();
        interrupts.install();
        sync.install();
    }
    // Take one native delivery of `source` in the reference interpreter from
    // an interrupted thread at kFinish, back to kFinish.
    void deliver(gekko::NativeInterrupts::Source source) {
        m.cpu.set_msr(m.cpu.msr() | gekko::kMsrEE | gekko::kMsrRI);
        m.cpu.gpr[1] = kStackTop;
        m.cpu.pc = kFinish;
        m.cpu.npc = kFinish + 4u;
        const std::size_t slot = gekko::NativeInterrupts::slot(source);
        const u64 before = interrupts.stats().delivered[slot];
        unsigned steps = 0;
        while (!m.cpu.stopped() && steps++ < 4000000u &&
               !(interrupts.stats().delivered[slot] == before + 1u && m.cpu.pc == kFinish)) {
            m.cpu.run_steps(1u);
        }
        require(!m.cpu.stopped(), ("the native delivery stopped the machine: " + m.cpu.stop().detail).c_str());
        require(interrupts.stats().delivered[slot] == before + 1u && m.cpu.pc == kFinish,
                "the native delivery did not return to the interrupted code");
        require(m.cpu.gpr[1] == kStackTop, "the native delivery left r1 moved");
    }
};

bool in_stack(u32 offset) {
    return offset >= kStackTop - kStackWindow - kMemoryBegin && offset < kStackTop + 8u - kMemoryBegin;
}

void compare_mem1(Machine& a, Machine& b, const std::string& label, const std::vector<u32>& excluded_words) {
    const std::vector<u8> x = a.bus.window_bytes(kMemoryBegin, kMemoryEnd);
    const std::vector<u8> y = b.bus.window_bytes(kMemoryBegin, kMemoryEnd);
    u32 differences = 0;
    for (u32 offset = 0; offset < kMemorySize; ++offset) {
        if (x[offset] == y[offset] || in_stack(offset)) continue;
        bool skip = false;
        for (const u32 w : excluded_words) skip = skip || (offset >= w - kMemoryBegin && offset < w + 4u - kMemoryBegin);
        if (skip) continue;
        if (++differences <= 8u) {
            check(false, label + ": memory 0x" + hex(kMemoryBegin + offset) + " original " + hex(x[offset]) +
                             " native " + hex(y[offset]));
        }
    }
    check(differences == 0u, label + ": " + std::to_string(differences) + " differing memory bytes");
}

std::vector<u8> region(Machine& m, u32 first, u32 end) {
    std::vector<u8> bytes;
    for (u32 a = first; a < end; ++a) bytes.push_back(m.byte(a));
    return bytes;
}

void compare_region(Machine& a, Machine& b, u32 first, u32 end, const std::string& label) {
    const std::vector<u8> x = region(a, first, end), y = region(b, first, end);
    if (x == y) {
        check(true, label);
        return;
    }
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (x[i] != y[i]) {
            check(false, label + ": 0x" + hex(first + static_cast<u32>(i)) + " original " + hex(x[i]) + " native " +
                             hex(y[i]));
        }
    }
}

// ---- FIFO binding ---------------------------------------------------------------

void fifo_case(const std::vector<u8>& dol, bool linked) {
    const std::string label = std::string("FIFO binding, ") + (linked ? "linked" : "unlinked");
    Original o(dol);
    Native n(dol);
    for (Machine* m : {&o.m, &n.m}) {
        if (linked) {
            m->call(kGxSetCpuFifo, kFifo);
            m->call(kGxSetGpFifo, kFifo);
            m->call(kGxSetCpuFifo, kFifo);
            // The FIFO object's own pointers moved: GXGetFifoPtrs refreshes them.
            m->cpu.mem_write32(kFifo + 0x14u, 0x80F20000u);
            m->cpu.mem_write32(kFifo + 0x18u, 0x80F10000u);
            m->cpu.mem_write32(kFifo + 0x1Cu, 0x1234u);
            m->call(kGxGetFifoPtrs, kFifo, kOut, kOut + 4u);
            m->call(kGxGetFifoPtrs, kOtherFifo, kOut + 8u, kOut + 12u);
        } else {
            m->call(kGxSetGpFifo, kOtherFifo);
            m->call(kGxSetCpuFifo, kFifo);
            m->call(kGxGetFifoPtrs, kFifo, kOut, kOut + 4u);
            m->call(kGxGetFifoPtrs, kOtherFifo, kOut + 8u, kOut + 12u);
            m->call(kGxSetCpuFifo, kOtherFifo);  // links the two again
            m->call(kGxGetFifoPtrs, kOtherFifo, kOut + 16u, kOut + 20u);
        }
    }
    compare_mem1(o.m, n.m, label, {});
    check(o.m.word(kOut + 4u) != 0u && o.m.word(kOut) != 0u, label + ": the original answered pointers");
}

// ---- init ---------------------------------------------------------------------

void init_case(const std::vector<u8>& dol) {
    const std::string label = "__GXFifoInit + __GXPEInit";
    Original o(dol);
    Native n(dol);
    o.m.cpu.mem_write32(kSda + 0x5E30u, 9u);
    n.m.cpu.mem_write32(kSda + 0x5E30u, 9u);
    o.m.cpu.mem_write32(kSda + 0x5E4Cu, 0x81000100u);
    n.m.cpu.mem_write32(kSda + 0x5E4Cu, 0x81000100u);
    for (Machine* m : {&o.m, &n.m}) {
        m->call(kGxFifoInit);
        m->call(kGxPeInit);
    }
    const u32 slot17 = kHandlerTable + 4u * 17u, slot18 = kHandlerTable + 4u * 18u, slot19 = kHandlerTable + 4u * 19u;
    compare_mem1(o.m, n.m, label, {slot17, slot18, slot19});
    documented(o.m.word(slot17) == kGxCpHandler && o.m.word(slot18) == 0x803966F4u &&
                   o.m.word(slot19) == kGxFinishHandler && n.m.word(slot17) == 0u && n.m.word(slot18) == 0u &&
                   n.m.word(slot19) == 0u,
               label + ": OS interrupts 17-19 have the SDK's handlers in ORIGINAL and none in NATIVE");
    using Source = gekko::NativeInterrupts::Source;
    check(n.interrupts.unmasked(Source::GxBreakpoint) && n.interrupts.unmasked(Source::PeFinish),
          label + ": PI_CP and PI_PE_FINISH unmasked natively (PI_PE_TOKEN in the OS mask: compared above)");
    check((o.pe.regs[0x0Au / 2u] & 0x3u) == 0x3u && n.sync.token_interrupt_enabled() && n.sync.finish_interrupt_enabled(),
          label + ": both PE interrupts enabled");
}

// ---- breakpoint ------------------------------------------------------------------

// RenderWare's XFB ring as RasterShowRaster leaves it: ring[0] = {A, wrA},
// ring[1] = {B, wrB}, read index 0, write index `write`; the displayed XFB
// selects the callback's path.
void stage_ring(Machine& m, u32 write, u32 displayed) {
    constexpr u32 kA = 0x80C00000u, kB = 0x80D00000u;
    for (u32 a = kSda + kSdaRwFirst; a < kSda + kSdaRwEnd; a += 4u) m.cpu.mem_write32(a, 0u);
    m.cpu.mem_write32(kRwRing + 0u, kA);
    m.cpu.mem_write32(kRwRing + 4u, 0x00F00060u);
    m.cpu.mem_write32(kRwRing + 8u, kB);
    m.cpu.mem_write32(kRwRing + 12u, 0x00F000C0u);
    m.cpu.mem_write32(kRwRing + 16u, 0u);
    m.cpu.mem_write32(kRwRing + 20u, 0u);
    m.cpu.mem_write32(kSda + 0x61A0u, kA);
    m.cpu.mem_write32(kSda + 0x61A4u, kB);
    m.cpu.mem_write32(kSda + 0x61ACu, displayed);
    m.cpu.mem_write32(kSda + 0x61B0u, 0u);
    m.cpu.mem_write32(kSda + 0x61B4u, write);
    m.cpu.mem_write32(kSda + 0x61C0u, 1u);  // RenderWare's armed flag
    m.cpu.mem_write32(kSda + 0x5E34u, kRwBreakPointCallback);
    m.cpu.mem_write32(kCurrentContext, kInterruptedContext);
    m.cpu.mem_write32(kCurrentContextPhysical, kInterruptedContext & 0x3FFFFFFFu);
}

void breakpoint_case(const std::vector<u8>& dol, const char* path, u32 write, bool displayed_first) {
    const std::string label = std::string("breakpoint delivery, ") + path;
    constexpr u32 kA = 0x80C00000u, kB = 0x80D00000u;
    const u32 displayed = displayed_first ? kA : kB;
    Original o(dol);
    Native n(dol);
    for (Machine* m : {&o.m, &n.m}) {
        m->call(kGxFifoInit);
        m->call(kGxSetCpuFifo, kFifo);
        m->call(kGxSetGpFifo, kFifo);
        stage_ring(*m, write, displayed);
        m->call(kGxEnableBreakPt, 0x80F00060u);
    }
    // ORIGINAL: the CP status a reached breakpoint shows (the emulation's).
    o.cp.pin_status = true;
    o.cp.status = 0x001Cu;
    o.m.call(kGxCpHandler, 17u, kInterruptedContext);
    using Source = gekko::NativeInterrupts::Source;
    check(n.interrupts.status(Source::GxBreakpoint), label + ": the armed breakpoint was reached at once");
    n.deliver(Source::GxBreakpoint);
    compare_region(o.m, n.m, o.m.gx, o.m.gx + kGxDataSize, label + ": __GXData");
    compare_region(o.m, n.m, kSda + kSdaGxFirst, kSda + kSdaGxEnd, label + ": the GX library's small-data words");
    compare_region(o.m, n.m, kSda + kSdaRwFirst, kSda + kSdaRwEnd, label + ": RenderWare's ring words");
    compare_region(o.m, n.m, kRwRing, kRwRing + 24u, label + ": RenderWare's ring");
    check(o.m.word(kCurrentContext) == n.m.word(kCurrentContext), label + ": the OS current context");
    check(n.sync.stats().breakpoint_callbacks == 1u, label + ": RenderWare's callback ran once");
}

// ---- finish ---------------------------------------------------------------------

void finish_case(const std::vector<u8>& dol) {
    const std::string label = "finish delivery";
    Original o(dol);
    Native n(dol);
    for (Machine* m : {&o.m, &n.m}) {
        m->call(kGxPeInit);
        m->cpu.mem_write8(kSda + 0x5E48u, 0u);  // GXDrawDone cleared DrawDone
        m->cpu.mem_write32(kCurrentContext, kInterruptedContext);
        m->cpu.mem_write32(kCurrentContextPhysical, kInterruptedContext & 0x3FFFFFFFu);
    }
    o.m.call(kGxFinishHandler, 19u, kInterruptedContext);
    n.sync.on_draw_done();
    n.deliver(gekko::NativeInterrupts::Source::PeFinish);
    check(o.m.byte(kSda + 0x5E48u) == 1u && n.m.byte(kSda + 0x5E48u) == 1u, label + ": DrawDone set");
    compare_region(o.m, n.m, kSda + kSdaGxFirst, kSda + kSdaGxEnd, label + ": the GX library's small-data words");
    compare_region(o.m, n.m, o.m.gx, o.m.gx + kGxDataSize, label + ": __GXData");
    check(o.m.word(kCurrentContext) == n.m.word(kCurrentContext), label + ": the OS current context");
}

// ---- shutdown -------------------------------------------------------------------

void shutdown_case(const std::vector<u8>& dol) {
    const std::string label = "__GXShutdown";
    Original o(dol);
    Native n(dol);
    const auto both_at = [&](u64 ticks) {
        o.clock = ticks;
        n.m.cpu.game_clock().set_now(ticks);
        n.m.cpu.set_time_base(ticks);
    };
    const auto pass = [&](u32 final_pass, const std::string& what) {
        o.m.call(kGxShutdown, final_pass);
        const u32 original = o.m.cpu.gpr[3];
        n.m.call(kGxShutdown, final_pass);
        check(original == n.m.cpu.gpr[3], label + " " + what + ": result " + hex(original) + " / " + hex(n.m.cpu.gpr[3]));
        compare_mem1(o.m, n.m, label + " " + what, {});
    };
    both_at(1000u);
    pass(0u, "first pass");
    pass(0u, "no time");
    both_at(1009u);
    pass(0u, "9 ticks");
    both_at(1010u);
    pass(0u, "10 ticks");
    o.m.cpu.mem_write32(kSda + 0x5E08u, 3u);
    n.m.cpu.mem_write32(kSda + 0x5E08u, 3u);
    both_at(1500u);
    pass(0u, "a moved counter");
    for (Machine* m : {&o.m, &n.m}) {
        m->cpu.mem_write32(kSda + 0x5E34u, kRwBreakPointCallback);
        m->cpu.mem_write32(kSda + 0x5E40u, 0x80001234u);
        m->cpu.mem_write32(kSda + 0x5E44u, 0x80005678u);
        m->cpu.mem_write32(kSda + 0x5E24u, kFifo);  // a GP FIFO: __GXAbort waits on the counter
    }
    o.advancing_clock = true;
    pass(1u, "final pass");
    check(o.pipe.bytes == 32u && o.pi.fifo_resets == 2u, label + ": the original padded the pipe and pulsed the FIFO reset");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "usage: native_gx_sync_reference_tests <main.dol>");
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() >= 256u, "missing original DOL");
        fifo_case(dol, true);
        fifo_case(dol, false);
        init_case(dol);
        breakpoint_case(dol, "advance and re-arm", 2u, false);
        breakpoint_case(dol, "ring empty: disarm", 1u, false);
        breakpoint_case(dol, "displayed XFB: defer", 2u, true);
        finish_case(dol);
        shutdown_case(dol);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
    std::printf("%d checks, %d failures, %d documented differences\n", g_cases, g_failures, g_documented);
    return g_failures == 0 ? 0 : 1;
}
