// The native VIInit against the original VI library words.
//
// liveness-roots: this test enters no generated code - VIInit, the cold start
// __VIInit and every OS routine they call run as ORIGINAL words in the
// reference interpreter - so its literals must not keep the replaced bodies
// in the shipping image:
// liveness-roots: reference-interpreter-only
//
// Two machines load the original DOL. ORIGINAL runs VIInit's own words over a
// test-only register fixture standing in for the console's video interface
// (and the processor interface's mask register SetInterruptMask writes);
// NATIVE serves the same call with the production owner (NativeVideo) and no
// VI library code at all (docs/native-time-20261005/STAGE2_VI.md). Each case
// is one whole call on both from byte-identical MEM1, compared on every MEM1
// byte outside the call's stack window. One difference is intended and is
// asserted by value: the original registers __VIRetraceHandler as OS
// interrupt 24's handler; the native owner registers nothing, because
// NativeInterrupts delivers the retrace.
//
// The second family is one retrace after the original VIInit,
// VISetNextFrameBuffer, VIConfigure and VIFlush staged a configuration (PAL
// interlaced, and EURGB60 interlaced, which also changes the field timing):
// ORIGINAL calls __VIRetraceHandler's own words as __OSDispatchInterrupt did
// (the fixture raising DI0 in the first field); NATIVE raises the native VI
// source and lets the reference interpreter take it - NativeInterrupts'
// delivery, NativeVideo's handler, and the original OSWakeupThread and
// OSLoadContext words it calls. Compared: the VI library's small-data words
// and static block, the OS current context. Since stage 3 the commit no
// longer calls SIRefreshSamplingRate (it refit the SI poll grid to the
// committed field rate; NativePad has none): the original handler programs
// the SI registers, the native delivery must touch none, and the sampling-rate
// word is the same on both (the refresh re-stores it).

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_cri.h"
#include "gekko/native_interrupts.h"
#include "gekko/native_video.h"

#include <array>
#include <cstdio>
#include <cstdlib>
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

constexpr u32 kViInit = 0x8037E824u;
constexpr u32 kRetraceHandler = 0x8037E27Cu;   // __VIRetraceHandler
constexpr u32 kOSReport = 0x803731CCu;         // what OSRegisterVersion calls
constexpr u32 kSdaHandlerTable = 0x5A90u;      // InterruptHandlerTable
constexpr u32 kHandlerTable = 0x80003040u;     // where __OSInterruptInit puts it
constexpr u32 kViHandlerSlot = kHandlerTable + 4u * 24u;
constexpr u32 kOsTvMode = 0x800000CCu;
constexpr u32 kOsGlobalMask = 0x800000C4u;  // __OSInterruptInit masks everything
constexpr u32 kSramDisplayOffsetH = 0x80586FF0u;  // __OSLockSram's block + 0x10
constexpr u32 kViConfigure = 0x8037F19Cu;
constexpr u32 kViFlush = 0x8037FD38u;
constexpr u32 kViSetNextFrameBuffer = 0x8037FE68u;
constexpr u32 kRenderModePal = 0x80568938u;     // GXRenderModeObj, VI_TVMODE_PAL_INT
constexpr u32 kRenderModeEuRgb60 = 0x80568974u;  // GXRenderModeObj, VI_TVMODE_EURGB60_INT
constexpr u32 kXfb = 0x80800000u;
constexpr u32 kCurrentContext = 0x800000D4u;
constexpr u32 kCurrentContextPhysical = 0x800000C0u;
constexpr u32 kInterruptedContext = 0x81100000u;  // the interrupted thread's OSContext
constexpr u32 kSdaViFirst = 0x5BC8u;
constexpr u32 kSdaViEnd = 0x5C28u;
constexpr u32 kViStatic = 0x80587C28u;
constexpr u32 kViStaticSize = 0x148u;
constexpr u32 kSdaSamplingRate = 0x5EF8u;

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
    if (g_failures <= 40) std::fprintf(stderr, "MISMATCH %s\n", what.c_str());
}

void documented(bool ok, const std::string& what) {
    ++g_documented;
    check(ok, "documented difference broken: " + what);
}

// The console's video interface as VIInit and __VIInit see it: a register
// file whose display control register holds what the IPL left. Test-only.
class ViFixture : public gekko::MmioDevice {
public:
    explicit ViFixture(u16 dcr) {
        regs_.fill(0u);
        regs_[1] = dcr;
    }
    const char* name() const override { return "test VI"; }
    u16 read16(u32 address) override { return regs_[(address & 0xFFu) / 2u]; }
    void write16(u32 address, u16 value) override { regs_[(address & 0xFFu) / 2u] = value; }
    u32 read32(u32 address) override { return (static_cast<u32>(read16(address)) << 16) | read16(address + 2u); }
    void write32(u32 address, u32 value) override {
        write16(address, static_cast<u16>(value >> 16));
        write16(address + 2u, static_cast<u16>(value));
    }
    u16 reg(u32 offset) const { return regs_[(offset & 0xFFu) / 2u]; }
    void set(u32 offset, u16 value) { regs_[(offset & 0xFFu) / 2u] = value; }

private:
    std::array<u16, 0x80> regs_{};
};

// The serial interface's registers SISetXY / SISetSamplingRate write - the
// original handler's SIRefreshSamplingRate does, the native delivery must not.
// Test-only.
class SiFixture : public gekko::MmioDevice {
public:
    const char* name() const override { return "test SI"; }
    u32 read32(u32 address) override { return regs[(address & 0xFFu) / 4u]; }
    void write32(u32 address, u32 value) override { regs[(address & 0xFFu) / 4u] = value; }
    std::array<u32, 0x40> regs{};
};

// The processor interface's mask register (SetInterruptMask). Test-only.
class PiFixture : public gekko::MmioDevice {
public:
    const char* name() const override { return "test PI"; }
    u32 read32(u32 address) override { return address == 0x0C003004u ? mask : 0u; }
    void write32(u32 address, u32 value) override {
        if (address == 0x0C003004u) mask = value;
    }
    u32 mask = 0;
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
    std::vector<u8> image;

    explicit Machine(const std::vector<u8>& dol) {
        const auto word = [&](std::size_t offset) { return gekko::load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(static_cast<u64>(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        bus.write32(kFinish, 0x48000000u);
    }
    // The state both machines start a case from: the OS globals VIInit reads
    // (as the boot left them) and the SRAM display offset.
    void setup(bool pal, u8 display_offset) {
        cpu.clear_stop();
        cpu.reset();
        cpu.gpr[1] = kStackTop;
        cpu.gpr[2] = kSda2;
        cpu.gpr[13] = kSda;
        cpu.mem_write32(kSda + kSdaHandlerTable, kHandlerTable);
        cpu.mem_write32(kOsGlobalMask, 0xFFFFFFFFu);
        cpu.mem_write32(kOsTvMode, pal ? 1u : 0u);
        cpu.mem_write8(kSramDisplayOffsetH, display_offset);
        image = bus.window_bytes(kMemoryBegin, kMemoryEnd);
    }
    void call(u32 entry, u32 r3 = 0u, u32 r4 = 0u) {
        cpu.gpr[3] = r3;
        cpu.gpr[4] = r4;
        cpu.pc = entry;
        cpu.lr = kFinish;
        unsigned steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 20000000u) cpu.run_steps(1u);
        require(!cpu.stopped(), "the call stopped the machine");
        require(cpu.pc == kFinish, "the call did not return");
        require(cpu.gpr[1] == kStackTop, "the call left r1 moved");
    }
    u32 word(u32 address) { return bus.peek32(address); }
};

bool excluded(u32 offset) {
    const u32 low = kStackTop - kStackWindow - kMemoryBegin;
    const u32 high = kStackTop + 8u - kMemoryBegin;
    if (offset >= low && offset < high) return true;
    const u32 slot = kViHandlerSlot - kMemoryBegin;
    return offset >= slot && offset < slot + 4u;
}

void compare(Machine& original, Machine& native, const std::string& label) {
    const std::vector<u8> a = original.bus.window_bytes(kMemoryBegin, kMemoryEnd);
    const std::vector<u8> b = native.bus.window_bytes(kMemoryBegin, kMemoryEnd);
    u32 differences = 0;
    for (u32 offset = 0; offset < kMemorySize; ++offset) {
        if (a[offset] == b[offset] || excluded(offset)) continue;
        if (++differences <= 8u) {
            check(false, label + ": memory 0x" + hex(kMemoryBegin + offset) + " original " + hex(a[offset]) +
                             " native " + hex(b[offset]));
        }
    }
    check(differences == 0u, label + ": " + std::to_string(differences) + " differing memory bytes");
    documented(original.word(kViHandlerSlot) == kRetraceHandler && native.word(kViHandlerSlot) == 0u,
               label + ": OS interrupt 24's handler is the SDK's in ORIGINAL and unset in NATIVE");
}

void run_case(const std::vector<u8>& dol, bool pal, u8 display_offset) {
    const std::string label = std::string(pal ? "PAL warm start" : "NTSC cold start") + ", display offset " +
                              std::to_string(static_cast<int>(static_cast<signed char>(display_offset)));
    // ORIGINAL: the SDK's words over the fixture.
    Machine original(dol);
    ViFixture vi(pal ? 0x0101u : 0x0100u);  // what apply_boot_configuration used to leave
    PiFixture pi;
    original.bus.register_mmio(0xCC002000u, 0x100u, &vi);
    original.bus.register_mmio(0xCC003000u, 0x100u, &pi);
    original.cpu.install_hle(kOSReport, [](gekko::Cpu&) { return true; }, "fixture OSReport");
    original.setup(pal, display_offset);
    original.call(kViInit);
    check((vi.reg(0x02u) & 1u) == 1u, label + ": the original leaves the display running");

    // NATIVE: the owner, no VI library code.
    Machine native(dol);
    gekko::NativeInterrupts interrupts(native.cpu);
    gekko::NativeVideo video(native.cpu, interrupts);
    video.boot(pal);
    native.setup(pal, display_offset);
    video.install();
    native.call(kViInit);
    check(video.enabled(), label + ": the native display runs");
    check(interrupts.unmasked(gekko::NativeInterrupts::Source::Vi), label + ": PI_VI is unmasked");
    compare(original, native, label);
}

// The guest state a retrace leaves that game code can read.
bool vi_retained(u32 offset);  // the words VIConfigure's owner writes (below)

struct RetraceState {
    std::vector<u8> sda;
    std::vector<u8> vi;
    u32 sampling_rate = 0;
    std::array<u32, 0x40> si{};
    u32 current_context = 0;
};

RetraceState capture(Machine& m, const SiFixture& si) {
    RetraceState state;
    // The VI register image is retired (docs/agents/vi.md): the shadow registers + shdwChanged are native fields, the
    // registers the framebuffer / blanking calls wrote are not written. Those bytes are not compared.
    const auto retired_byte = [](u32 a) { return a >= kViStatic && a - kViStatic < kViStaticSize && !vi_retained(a - kViStatic); };
    for (u32 a = kSda + kSdaViFirst; a < kSda + kSdaViEnd; ++a) {
        const bool retired_word = (a >= kSda + 0x5C08u && a < kSda + 0x5C10u) || (a >= kSda + 0x5BF8u && a < kSda + 0x5C00u);
        state.sda.push_back(retired_word ? u8{0} : m.bus.peek8(a));
    }
    for (u32 a = kViStatic; a < kViStatic + kViStaticSize; ++a) state.vi.push_back(retired_byte(a) ? u8{0} : m.bus.peek8(a));
    state.sampling_rate = m.word(kSda + kSdaSamplingRate);
    state.si = si.regs;
    state.current_context = m.word(kCurrentContext);
    return state;
}

// Both machines: the fixtures, the original VIInit, then the game's staging
// of a configuration (VISetNextFrameBuffer, VIConfigure, VIFlush).
void stage(Machine& m, ViFixture& vi, SiFixture& si, PiFixture& pi, u32 render_mode, gekko::NativeVideo* video = nullptr) {
    m.bus.register_mmio(0xCC002000u, 0x100u, &vi);
    m.bus.register_mmio(0xCC003000u, 0x100u, &pi);
    m.bus.register_mmio(0xCC006400u, 0x100u, &si);
    m.cpu.install_hle(kOSReport, [](gekko::Cpu&) { return true; }, "fixture OSReport");
    m.setup(true, 0u);
    if (video != nullptr) {  // the NATIVE machine stages through the owners
        video->boot(true);
        video->install();
    }
    m.call(kViInit);
    m.call(kViSetNextFrameBuffer, kXfb);
    m.call(kViConfigure, render_mode);
    m.call(kViFlush);
    require(m.word(kSda + 0x5BD0u) == 1u, "VIFlush staged a configuration");
    // The interrupted thread: its OSContext current, interrupts enabled.
    m.cpu.mem_write32(kCurrentContext, kInterruptedContext);
    m.cpu.mem_write32(kCurrentContextPhysical, kInterruptedContext & 0x3FFFFFFFu);
}

void retrace_case(const std::vector<u8>& dol, u32 render_mode, u32 expected_half_lines) {
    const std::string label = std::string("retrace commit, render mode ") + hex(render_mode);
    // ORIGINAL: __VIRetraceHandler(__OS_INTERRUPT_PI_VI, context), as
    // __OSDispatchInterrupt calls it, with DI0 latched in the first field.
    Machine original(dol);
    ViFixture ovi(0x0101u);
    SiFixture osi;
    PiFixture opi;
    stage(original, ovi, osi, opi, render_mode);
    ovi.set(0x30u, static_cast<u16>(ovi.reg(0x30u) | 0x8000u));  // DI0 status
    ovi.set(0x2Cu, 1u);  // VCT: the first line of the frame
    ovi.set(0x2Eu, 1u);  // HCT
    original.call(kRetraceHandler, 24u, kInterruptedContext);
    const RetraceState expected = capture(original, osi);

    // NATIVE: the native source, taken by the reference interpreter.
    Machine native(dol);
    gekko::NativeInterrupts interrupts(native.cpu);
    interrupts.install();
    ViFixture nvi(0x0101u);
    SiFixture nsi;
    PiFixture npi;
    gekko::NativeVideo video(native.cpu, interrupts);
    stage(native, nvi, nsi, npi, render_mode, &video);
    check(interrupts.unmasked(gekko::NativeInterrupts::Source::Vi), label + ": VIInit unmasked PI_VI");
    // Two field ends: the retrace lands at the start of a first field.
    video.on_retrace(0u);
    video.on_retrace(0u);
    native.cpu.set_msr(native.cpu.msr() | gekko::kMsrEE | gekko::kMsrRI);
    native.cpu.gpr[1] = kStackTop;
    native.cpu.pc = kFinish;
    native.cpu.npc = kFinish + 4u;
    const std::size_t vi_slot = gekko::NativeInterrupts::slot(gekko::NativeInterrupts::Source::Vi);
    unsigned steps = 0;
    while (!native.cpu.stopped() && steps++ < 2000000u &&
           !(interrupts.stats().delivered[vi_slot] == 1u && native.cpu.pc == kFinish)) {
        native.cpu.run_steps(1u);
    }
    require(!native.cpu.stopped(), ("the native delivery stopped the machine: " + native.cpu.stop().detail).c_str());
    require(interrupts.stats().delivered[vi_slot] == 1u && native.cpu.pc == kFinish,
            "the native retrace delivery did not return to the interrupted code");
    check(native.cpu.gpr[1] == kStackTop, label + ": the delivery restored r1");
    const RetraceState actual = capture(native, nsi);

    check(actual.sda == expected.sda, label + ": the VI library's small-data words");
    check(actual.vi == expected.vi, label + ": the VI library's static block");
    check(actual.sampling_rate == expected.sampling_rate, label + ": the SI sampling-rate word");
    check(expected.si != std::array<u32, 0x40>{},
          label + ": the original handler refit the SI poll grid (SIRefreshSamplingRate)");
    check(actual.si == std::array<u32, 0x40>{}, label + ": the native delivery touched no SI register");
    check(actual.current_context == expected.current_context, label + ": the OS current context");
    check(video.stats().commits == 1u, label + ": one commit");
    check(video.half_lines_per_field() == expected_half_lines, label + ": the committed field length");
    if (actual.sda != expected.sda) {
        for (std::size_t i = 0; i < actual.sda.size(); ++i) {
            if (actual.sda[i] != expected.sda[i]) {
                check(false, label + ": r13+0x" + hex(kSdaViFirst + static_cast<u32>(i)) + " original " +
                                 hex(expected.sda[i]) + " native " + hex(actual.sda[i]));
            }
        }
    }
}


// ---- The staging entry points: VIFlush, VISetNextFrameBuffer, the current-buffer word, VISetBlack,
// VISetPreRetraceCallback and VIGetTvFormat. ORIGINAL runs the SDK's words; NATIVE runs the owners over the
// byte-identical state the original VIInit and VIConfigure left. Compared: every MEM1 byte outside the call's stack
// window and outside the RETIRED ranges, and the result register. The retired ranges are the VI register image words
// setFramebuffer / setVerticalRegs wrote and the "changed" bits naming them: hardware-only state that no code outside
// the VI library reads (docs/agents/vi.md; the guest watch is the proof). The test pins them: the original must
// differ there (so they are retirements, not accidents), the native must not have written them, and the changed
// mask must be the original's with exactly the retired bits cleared.
constexpr u32 kViGetCurrentFrameBuffer = 0x8037FED4u;
constexpr u32 kViSetBlack = 0x8037FEDCu;
constexpr u32 kViGetTvFormat = 0x80380138u;
constexpr u32 kViSetPreRetraceCallback = 0x8037E4F0u;
constexpr u32 kViSetPostRetraceCallback = 0x8037E534u;
constexpr u32 kHorVer = kViStatic + 0xF0u;

using Scenario = void (*)(Machine&);

// A machine after the original VIInit and VIConfigure(render_mode) - the state the game's staging runs over -
// then the scenario's own words.
void prepare(Machine& m, ViFixture& vi, SiFixture& si, PiFixture& pi, u32 render_mode, Scenario scenario,
             gekko::NativeVideo* video = nullptr) {
    m.bus.register_mmio(0xCC002000u, 0x100u, &vi);
    m.bus.register_mmio(0xCC003000u, 0x100u, &pi);
    m.bus.register_mmio(0xCC006400u, 0x100u, &si);
    m.cpu.install_hle(kOSReport, [](gekko::Cpu&) { return true; }, "fixture OSReport");
    m.setup(true, 0u);
    m.call(kViInit);
    m.call(kViConfigure, render_mode);
    if (video != nullptr) {
        // The native machine marks the same registers changed: the owner's own (idempotent) VIConfigure over the
        // state the original just left.
        video->install();
        m.call(kViConfigure, render_mode);
    }
    if (scenario != nullptr) scenario(m);
}

struct Retired {
    std::vector<std::pair<u32, u32>> ranges;  // [begin, end) guest addresses
    u32 changed_hi_bits = 0;                  // bits of the "changed" high word the original set for them
    bool flush = false;                       // VIFlush: only shdwRegs[1] / [54] and their shdwChanged bits are staged
};

u32 count_differences(Machine& original, Machine& native, std::vector<std::string>* first,
                      const Retired& retired = Retired{}) {
    const std::vector<u8> a = original.bus.window_bytes(kMemoryBegin, kMemoryEnd);
    const std::vector<u8> b = native.bus.window_bytes(kMemoryBegin, kMemoryEnd);
    u32 differences = 0;
    for (u32 offset = 0; offset < kMemorySize; ++offset) {
        if (a[offset] == b[offset]) continue;
        const u32 low = kStackTop - kStackWindow - kMemoryBegin, high = kStackTop + 8u - kMemoryBegin;
        if (offset >= low && offset < high) continue;
        const u32 address = kMemoryBegin + offset;
        bool skip = address >= kSda + 0x5BF8u && address < kSda + 0x5BFCu && retired.changed_hi_bits != 0u;
        if (retired.flush) {
            const u32 shadow = kViStatic + 0x78u;
            skip = skip || (address >= shadow && address < shadow + 120u) || (address >= kSda + 0x5C08u && address < kSda + 0x5C10u) ||
                   (address >= kSda + 0x5BF8u && address < kSda + 0x5C00u);
        }
        for (const auto& range : retired.ranges) skip = skip || (address >= range.first && address < range.second);
        if (skip) continue;
        ++differences;
        if (first != nullptr && first->size() < 8u)
            first->push_back("0x" + hex(kMemoryBegin + offset) + " original " + hex(a[offset]) + " native " + hex(b[offset]));
    }
    return differences;
}

// One staging call on both machines. Returns the number of differing memory bytes (the negative control reads it).
u32 staging_case(const std::vector<u8>& dol, const std::string& label, u32 render_mode, Scenario scenario, u32 entry,
                 u32 r3, bool compare_result, const Retired& retired = Retired{}, bool perturb_native = false,
                 bool pre_flush = false) {
    Machine original(dol);
    ViFixture ovi(0x0101u);
    SiFixture osi;
    PiFixture opi;
    prepare(original, ovi, osi, opi, render_mode, scenario);
    Machine native(dol);
    ViFixture nvi(0x0101u);
    SiFixture nsi;
    PiFixture npi;
    gekko::NativeInterrupts interrupts(native.cpu);
    gekko::NativeVideo video(native.cpu, interrupts);
    prepare(native, nvi, nsi, npi, render_mode, scenario, &video);
    // A retirement can only be observed on a clean slate: VIConfigure already wrote these registers and set their changed
    // bits. Clear both on both machines (identically) before the call.
    if (!retired.ranges.empty() || retired.changed_hi_bits != 0u) {
        for (Machine* m : {&original, &native}) {
            for (const auto& range : retired.ranges)
                for (u32 a = range.first; a < range.second; ++a) m->cpu.mem_write8(a, 0u);
            m->cpu.mem_write32(kSda + 0x5BF8u, 0u);
            m->cpu.mem_write32(kSda + 0x5BFCu, 0u);
        }
    }
    // Both start from identical memory (the owners' install touches none).
    {
        std::vector<std::string> start_diff;
        const u32 start = count_differences(original, native, &start_diff);
        for (const std::string& line : start_diff) std::fprintf(stderr, "start difference %s\n", line.c_str());
        require(start == 0u, ("the staging case did not start identical: " + label).c_str());
    }
    if (pre_flush) {
        original.call(kViFlush);
        native.call(kViFlush);
    }
    const std::vector<u8> shadow_before = native.bus.window_bytes(kViStatic + 0x78u, kViStatic + 0xF0u);
    const u32 shadow_changed_before = native.word(kSda + 0x5C08u) ^ native.word(kSda + 0x5C0Cu);
    original.call(entry, r3);
    native.call(entry, r3);
    if (!perturb_native && entry == kViSetBlack) {
        check(video.black_requested() == (r3 != 0u), label + ": black_requested follows the flag");
    }
    if (!perturb_native && retired.flush) {
        check(native.bus.window_bytes(kViStatic + 0x78u, kViStatic + 0xF0u) == shadow_before &&
                  (native.word(kSda + 0x5C08u) ^ native.word(kSda + 0x5C0Cu)) == shadow_changed_before,
              label + ": VIFlush wrote no shadow register image into guest memory");
    }
    if (perturb_native) native.cpu.mem_write8(kSda + 0x5BD3u, static_cast<u8>(native.bus.peek8(kSda + 0x5BD3u) ^ 0x10u));  // the flush flag's low byte
    std::vector<std::string> first;
    const u32 differences = count_differences(original, native, &first, retired);
    if (!perturb_native && retired.flush) {
        // VIFlush hands registers 1 (display control) and 54 (clock select) to the native field: staged exactly when the
        // original's shdwChanged named them, with the value the original copied into shdwRegs.
        const bool o_display = (original.word(kSda + 0x5C08u) & (0x80000000u >> 1)) != 0u;
        const bool o_clock = (original.word(kSda + 0x5C0Cu) & (0x80000000u >> (54u - 32u))) != 0u;
        u8 expected = 0u;
        if (o_display) expected |= 1u | (((original.cpu.mem_read16(kViStatic + 0x78u + 2u) & 1u) != 0u) ? 2u : 0u);
        if (o_clock) expected |= 4u | (((original.cpu.mem_read16(kViStatic + 0x78u + 108u) & 1u) != 0u) ? 8u : 0u);
        check(video.staged() == expected, label + ": native staged " + hex(video.staged()) + " original " + hex(expected));
    }
    if (!perturb_native && (!retired.ranges.empty() || retired.changed_hi_bits != 0u)) {
        // The retirements are pinned: the original wrote them, the native did not, and the changed mask is the
        // original's without exactly the retired bits.
        bool original_wrote = false;
        for (const auto& range : retired.ranges) {
            for (u32 a = range.first; a < range.second; ++a) {
                original_wrote = original_wrote || original.bus.peek8(a) != native.bus.peek8(a);
            }
        }
        check(original_wrote, label + ": the original wrote the retired register image words");
        const u32 ohi = original.word(kSda + 0x5BF8u), nhi = native.word(kSda + 0x5BF8u);
        check((ohi & ~retired.changed_hi_bits) == nhi, label + ": changed high word original " + hex(ohi) + " native " + hex(nhi));
        check((ohi & retired.changed_hi_bits) == retired.changed_hi_bits, label + ": the original set the retired changed bits");
        check(original.word(kSda + 0x5BFCu) == native.word(kSda + 0x5BFCu), label + ": changed low word");
    }
    if (!perturb_native) {
        for (const std::string& line : first) check(false, label + ": memory " + line);
        check(differences == 0u, label + ": " + std::to_string(differences) + " differing memory bytes");
        if (compare_result) check(original.cpu.gpr[3] == native.cpu.gpr[3], label + ": r3 original " + hex(original.cpu.gpr[3]) + " native " + hex(native.cpu.gpr[3]));
    }
    return differences;
}

void set_halfword(Machine& m, u32 address, u16 value) { m.cpu.mem_write16(address, value); }

// ---- scenarios (each over the state VIConfigure left)
void sc_default(Machine&) {}
void sc_pan(Machine& m) {  // a panned, field-mode (fbMode 1) buffer
    set_halfword(m, kHorVer + 0x0Eu, 3u);
    set_halfword(m, kHorVer + 0x16u, 5u);
    m.cpu.mem_write32(kHorVer + 0x20u, 1u);
}
void sc_odd_field(Machine& m) { set_halfword(m, kHorVer + 0x0Au, 1u); }
void sc_stereo(Machine& m) {  // a second (right-eye) buffer
    m.cpu.mem_write32(kHorVer + 0x44u, 1u);
    m.cpu.mem_write32(kHorVer + 0x48u, 0x80900000u);
}
void sc_stereo_high(Machine& m) {  // ... above 16 MB: every address is stored in 32-byte units
    m.cpu.mem_write32(kHorVer + 0x44u, 1u);
    m.cpu.mem_write32(kHorVer + 0x48u, 0x81200000u);
}
void sc_flush_masks(Machine& m) {  // staged state before VIFlush: registers dirty, an earlier flush pending
    for (u32 i = 0; i < 0x30u; ++i) m.cpu.mem_write16(kViStatic + 2u * i, static_cast<u16>(0x1234u + 0x111u * i));
    m.cpu.mem_write32(kSda + 0x5BF8u, 0xC0010400u);  // changed: register 0, 1, 15 and 21
    m.cpu.mem_write32(kSda + 0x5BFCu, 0x00000200u);  // and register 54
    m.cpu.mem_write32(kSda + 0x5C08u, 0x00000200u);  // shdwChanged already holds register 22
    m.cpu.mem_write32(kSda + 0x5C0Cu, 0x40000000u);
    m.cpu.mem_write32(kSda + 0x5BF0u, 1u);           // a mode change staged
    m.cpu.mem_write32(kSda + 0x5C18u, 0x80ABCDEFu);  // the buffer VIFlush last made current
    m.cpu.mem_write32(kSda + 0x5C00u, 0u);
}
void sc_flush_low_only(Machine& m) {
    m.cpu.mem_write32(kSda + 0x5BF8u, 0u);
    m.cpu.mem_write32(kSda + 0x5BFCu, 0x00008200u);
    m.cpu.mem_write32(kSda + 0x5BF0u, 0u);
}
void sc_flush_nothing(Machine& m) {
    m.cpu.mem_write32(kSda + 0x5BF8u, 0u);
    m.cpu.mem_write32(kSda + 0x5BFCu, 0u);
}
void sc_callback_set(Machine& m) { m.cpu.mem_write32(kSda + 0x5BDCu, 0x80123450u); }  // a callback already installed
void sc_post_callback_set(Machine& m) { m.cpu.mem_write32(kSda + 0x5BE0u, 0x80123454u); }  // a post callback already installed
void sc_clock54(Machine& m) { set_halfword(m, kViStatic + 0x6Cu, 1u); }
void sc_clock54_odd(Machine& m) {
    set_halfword(m, kViStatic + 0x6Cu, 1u);
    set_halfword(m, kHorVer + 0x0Au, 3u);
}

void staging_cases(const std::vector<u8>& dol) {
    struct Mode { u32 render_mode; const char* name; };
    // VISetNextFrameBuffer retires setFramebuffer's registers 0x1C-0x2B, the four buffer-address words and the
    // changed bits 16, 17, 12-15, 10, 11.
    Retired framebuffer;
    framebuffer.ranges = {{kViStatic + 0x1Cu, kViStatic + 0x2Cu}, {kViStatic + 0x124u, kViStatic + 0x12Cu},
                          {kViStatic + 0x13Cu, kViStatic + 0x144u}};
    framebuffer.changed_hi_bits = 0x00010000u | 0x00020000u | 0x00001000u | 0x00002000u;
    // (the stereo bits 0x4000 / 0x8000 / 0x400 / 0x800 are set only with a right buffer: scenarios below.)
    Retired framebuffer_stereo = framebuffer;
    framebuffer_stereo.changed_hi_bits |= 0x00004000u | 0x00008000u | 0x00000400u | 0x00000800u;
    // VISetBlack retires setVerticalRegs' registers 0x00 and 0x0C-0x13 and changed bits 0, 6, 7, 9, 8.
    Retired black_regs;
    black_regs.ranges = {{kViStatic + 0x00u, kViStatic + 0x02u}, {kViStatic + 0x0Cu, kViStatic + 0x14u}};
    black_regs.changed_hi_bits = 0x80000000u | 0x01000000u | 0x02000000u | 0x00400000u | 0x00800000u;
    Retired flush_retired;
    flush_retired.flush = true;
    for (const Mode mode : {Mode{kRenderModePal, "PAL"}, Mode{kRenderModeEuRgb60, "EURGB60"}}) {
        const std::string m = mode.name;
        for (const u32 fb : {0x80800000u, 0x81000000u, 0xC0800020u, 0x80C00040u}) {
            staging_case(dol, m + " VISetNextFrameBuffer " + hex(fb), mode.render_mode, sc_default, kViSetNextFrameBuffer, fb, false, framebuffer);
            staging_case(dol, m + " VISetNextFrameBuffer panned " + hex(fb), mode.render_mode, sc_pan, kViSetNextFrameBuffer, fb, false, framebuffer);
            staging_case(dol, m + " VISetNextFrameBuffer odd field " + hex(fb), mode.render_mode, sc_odd_field, kViSetNextFrameBuffer, fb, false, framebuffer);
            staging_case(dol, m + " VISetNextFrameBuffer stereo " + hex(fb), mode.render_mode, sc_stereo, kViSetNextFrameBuffer, fb, false, framebuffer_stereo);
            staging_case(dol, m + " VISetNextFrameBuffer stereo >16MB " + hex(fb), mode.render_mode, sc_stereo_high, kViSetNextFrameBuffer, fb, false, framebuffer_stereo);
        }
        staging_case(dol, m + " VIFlush after VIConfigure", mode.render_mode, sc_default, kViFlush, 0u, false, flush_retired);
        staging_case(dol, m + " VIFlush mixed masks", mode.render_mode, sc_flush_masks, kViFlush, 0u, false, flush_retired);
        // A second flush: nothing is left changed.
        staging_case(dol, m + " VIFlush nothing changed", mode.render_mode, sc_default, kViFlush, 0u, false, flush_retired, false, true);
        for (const u32 black : {0u, 1u, 0xFFFFFFFFu}) {
            const std::string b = m + " VISetBlack(" + std::to_string(black) + ")";
            staging_case(dol, b, mode.render_mode, sc_default, kViSetBlack, black, false, black_regs);
            staging_case(dol, b + " odd field", mode.render_mode, sc_odd_field, kViSetBlack, black, false, black_regs);
            staging_case(dol, b + " 54 MHz", mode.render_mode, sc_clock54, kViSetBlack, black, false, black_regs);
        }
        staging_case(dol, m + " VIGetCurrentFrameBuffer", mode.render_mode, sc_flush_masks, kViGetCurrentFrameBuffer, 0u, true);
        // VISetPreRetraceCallback returns the previous callback and installs the new one.
        for (const u32 callback : {0u, 0x8032D17Cu}) {
            staging_case(dol, m + " VISetPreRetraceCallback(" + hex(callback) + ")", mode.render_mode, sc_callback_set, kViSetPreRetraceCallback, callback, true);
        }
        // VISetPostRetraceCallback: the twin of the pre callback (the word at r13 + 0x5BE0).
        for (const u32 callback : {0u, 0x8032D17Cu}) {
            staging_case(dol, m + " VISetPostRetraceCallback(" + hex(callback) + ")", mode.render_mode, sc_post_callback_set, kViSetPostRetraceCallback, callback, true);
        }
    }
    // VIGetTvFormat over every committed mode the jump table covers (the committed word is r13 + 0x5C14).
    for (u32 tv = 0; tv < 8u; ++tv) {
        Machine original(dol);
        ViFixture ovi(0x0101u);
        SiFixture osi;
        PiFixture opi;
        prepare(original, ovi, osi, opi, kRenderModePal, nullptr);
        Machine native(dol);
        ViFixture nvi(0x0101u);
        SiFixture nsi;
        PiFixture npi;
        prepare(native, nvi, nsi, npi, kRenderModePal, nullptr);
        gekko::NativeInterrupts interrupts(native.cpu);
        gekko::NativeVideo video(native.cpu, interrupts);
        video.install();
        original.cpu.mem_write32(kSda + 0x5C14u, tv);
        native.cpu.mem_write32(kSda + 0x5C14u, tv);
        original.call(kViGetTvFormat);
        native.call(kViGetTvFormat);
        check(original.cpu.gpr[3] == native.cpu.gpr[3],
              "VIGetTvFormat(mode " + std::to_string(tv) + ") original " + hex(original.cpu.gpr[3]) + " native " + hex(native.cpu.gpr[3]));
        check(count_differences(original, native, nullptr) == 0u, "VIGetTvFormat(mode " + std::to_string(tv) + ") left memory untouched");
    }
    // Negative control: the comparison must see a single flipped bit in a small-data word VIFlush wrote.
    check(staging_case(dol, "negative control", kRenderModePal, sc_flush_masks, kViFlush, 0u, false, [] { Retired r; r.flush = true; return r; }(), true) == 1u,
          "negative control: one perturbed byte is exactly one difference");
}

// ---- the CRI vsync tick as plain guest calls of the retrace delivery (native_cri.h) -------------------------------
// The servers are test stubs written over the original words of their entries; each logs {r1, r3, sequence} and
// returns 0. Everything else is the original OS code and the production NativeInterrupts / NativeVideo / NativeCri.
constexpr u32 kCriBlock = gekko::NativeCri::kBlock;
constexpr u32 kVsyncServer = 0x803CC3FCu, kSvtype4Server = 0x803CC36Cu, kUserCallbackSlot = 0x80569D38u;
constexpr u32 kCallbackStub = 0x81101800u;  // outside the image's code: a stub the user-callback word names
constexpr u32 kLog = 0x81101000u;           // three 16-byte records, then the sequence word
constexpr u32 kSequence = kLog + 0x40u;
constexpr u32 kVsyncStackTop = 0x81200000u;

void write_stub(Machine& m, u32 address, u32 record) {
    const u32 log = kLog + 16u * record;
    const u32 words[] = {
        0x3D200000u | (log >> 16), 0x61290000u | (log & 0xFFFFu),                 // r9 = record
        0x3D400000u | (kSequence >> 16), 0x614A0000u | (kSequence & 0xFFFFu),     // r10 = &sequence
        0x816A0000u, 0x396B0001u, 0x916A0000u,                                     // lwz r11,0(r10); addi r11,r11,1; stw
        0x90290000u, 0x90690004u, 0x91690008u,                                     // stw r1,0(r9); stw r3,4(r9); stw r11,8(r9)
        0x3D008000u, 0x818800D4u, 0x918800D8u, 0x9189000Cu,                        // r12 = current context; it takes the FPU; log it
        0x38600000u, 0x4E800020u};                                                 // li r3,0; blr
    for (std::size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        m.bus.write32(address + 4u * static_cast<u32>(i), words[i]);
    }
}

struct TickRig {
    Machine native;
    gekko::NativeInterrupts interrupts;
    ViFixture vi{0x0101u};
    SiFixture si;
    PiFixture pi;
    gekko::NativeVideo video;
    gekko::NativeCri cri;
    explicit TickRig(const std::vector<u8>& dol)
        : native(dol), interrupts(native.cpu), video(native.cpu, interrupts), cri(native.cpu, interrupts) {
        _putenv_s("SHADOW_NATIVE_CRI", "1");
        interrupts.install();
        stage(native, vi, si, pi, kRenderModePal);
        video.boot(true);
        video.install();
        cri.install();
        video.set_retrace_tick([this](gekko::Cpu& c, u32& frame) { return cri.retrace_tick(c, frame); });
        // ADXGC init created the four server threads (OSCreateThread: r3 thread, r4 entry, r6 stack top).
        const u32 entries[4] = {gekko::NativeCri::kVsyncEntry, gekko::NativeCri::kSvtype4Entry,
                                gekko::NativeCri::kIdleEntry, gekko::NativeCri::kSpinnerEntry};
        const u32 offsets[4] = {0x348u, 0x660u, 0x30u, 0x978u};
        const u32 stacks[4] = {kVsyncStackTop, 0x81210000u, 0x81220000u, 0x81230000u};
        for (int i = 0; i < 4; ++i) {
            native.cpu.gpr[3] = kCriBlock + offsets[i];
            native.cpu.gpr[4] = entries[i];
            native.cpu.gpr[6] = stacks[i];
            require(cri.create_thread(native.cpu), "a CRI server thread was not replaced");
        }
        native.cpu.mem_write32(kCriBlock + 0xCA4u, 1u);  // the Vsync loop runs
        native.cpu.mem_write32(kCriBlock + 0xC9Cu, 1u);  // the svtype 4 loop runs
        native.cpu.mem_write32(kCriBlock + 0xCA8u, 0u);  // the Idle loop has not exited: the user callback is called
        native.cpu.mem_write32(kUserCallbackSlot, kCallbackStub);
        native.cpu.mem_write32(kUserCallbackSlot + 4u, 0x1234u);
        write_stub(native, kVsyncServer, 0u);
        write_stub(native, kCallbackStub, 1u);
        write_stub(native, kSvtype4Server, 2u);
        for (u32 r = 14; r < 32u; ++r) native.cpu.gpr[r] = 0xC0DE0000u + r;
        video.on_retrace(0u);
        video.on_retrace(0u);
        native.cpu.set_msr(native.cpu.msr() | gekko::kMsrEE | gekko::kMsrRI);
        native.cpu.gpr[1] = kStackTop;
        native.cpu.pc = kFinish;
        native.cpu.npc = kFinish + 4u;
    }
    // Runs until the given numbers of retrace and HostWork deliveries have returned to the interrupted code.
    void run(u64 vi_deliveries, u64 host_work_deliveries) {
        const std::size_t vi_slot = gekko::NativeInterrupts::slot(gekko::NativeInterrupts::Source::Vi);
        const std::size_t hw_slot = gekko::NativeInterrupts::slot(gekko::NativeInterrupts::Source::HostWork);
        unsigned steps = 0;
        while (!native.cpu.stopped() && steps++ < 4000000u &&
               !(interrupts.stats().delivered[vi_slot] == vi_deliveries &&
                 interrupts.stats().delivered[hw_slot] == host_work_deliveries && native.cpu.pc == kFinish)) {
            native.cpu.run_steps(1u);
        }
    }
    u32 log(u32 record, u32 field) { return native.word(kLog + 16u * record + 4u * field); }
};

void tick_cases(const std::vector<u8>& dol) {
    // 1. The tick runs inside the delivery: Vsync server, user callback, svtype 4 server, in that order, all on the
    //    Vsync thread's stack, with the CRI counters as the loops kept them; the interrupted code resumes intact.
    {
        TickRig rig(dol);
        rig.run(1u, 0u);
        require(!rig.native.cpu.stopped(), ("the tick stopped the machine: " + rig.native.cpu.stop().detail).c_str());
        const std::string label = "CRI tick";
        check(rig.native.cpu.pc == kFinish && rig.native.cpu.gpr[1] == kStackTop, label + ": back in the interrupted code");
        check(rig.log(0, 2) == 1u && rig.log(1, 2) == 2u && rig.log(2, 2) == 3u,
              label + ": Vsync, callback, svtype 4 in order");
        check(rig.log(1, 1) == 0x1234u, label + ": the callback's argument");
        for (u32 i = 0; i < 3u; ++i) {
            const u32 r1 = rig.log(i, 0);
            check(r1 < kVsyncStackTop && r1 > kVsyncStackTop - 0x1000u,
                  label + ": server " + std::to_string(i) + " on the Vsync stack (r1 " + hex(r1) + ")");
        }
        check(rig.native.word(kCriBlock + 0x14u) == 1u && rig.native.word(kCriBlock + 0x18u) == 1u,
              label + ": loop counters");
        check(rig.native.word(0x805A6924u) == 1u, label + ": the global vsync counter");
        check(rig.cri.stats().vsync_ticks == 1u && rig.cri.stats().svtype4_ticks == 1u &&
                  rig.cri.stats().callbacks == 1u,
              label + ": stats");
        check(rig.native.word(kCurrentContext) == kInterruptedContext,
              label + ": the interrupted context is current again");
        for (u32 r = 14; r < 32u; ++r) {
            check(rig.native.cpu.gpr[r] == 0xC0DE0000u + r, label + ": r" + std::to_string(r) + " restored");
        }
        check(rig.video.stats().commits == 1u, label + ": the retrace itself committed");
        check(rig.native.word(0x800000D8u) == 0u, label + ": no exception context keeps the FPU after the delivery");
        for (u32 i = 0; i < 3u; ++i) {
            const u32 ctx = rig.log(i, 3);
            check(ctx > kVsyncStackTop - 0x1000u && ctx < kVsyncStackTop, label + ": server " + std::to_string(i) + " ran under an exception context on the Vsync stack");
        }
        check(rig.interrupts.stats().upcalls == 0u, label + ": no thread-context upcall");
    }
    // 2. CRI's lock held at the retrace: the tick is deferred; the lock leave's HostWork delivery runs it, again on
    //    the Vsync stack.
    {
        TickRig rig(dol);
        rig.native.cpu.mem_write32(kCriBlock + 0x08u, 1u);
        rig.run(1u, 0u);
        require(!rig.native.cpu.stopped(), "the deferred case stopped");
        check(rig.cri.stats().deferred_ticks == 1u && rig.native.word(kSequence) == 0u,
              "deferred tick: no server ran under the lock");
        rig.native.cpu.mem_write32(kCriBlock + 0x08u, 0u);
        rig.interrupts.raise(gekko::NativeInterrupts::Source::HostWork);
        rig.run(1u, 1u);
        require(!rig.native.cpu.stopped(), ("the HostWork tick stopped: " + rig.native.cpu.stop().detail).c_str());
        check(rig.log(0, 2) == 1u && rig.log(1, 2) == 2u && rig.log(2, 2) == 3u,
              "deferred tick: the servers ran in the HostWork delivery");
        for (u32 i = 0; i < 3u; ++i) {
            const u32 r1 = rig.log(i, 0);
            check(r1 < kVsyncStackTop && r1 > kVsyncStackTop - 0x1000u,
                  "deferred tick: server " + std::to_string(i) + " on the Vsync stack");
        }
        check(rig.native.cpu.pc == kFinish && rig.native.cpu.gpr[1] == kStackTop,
              "deferred tick: back in the interrupted code");
    }
    // 3. A server that waits for an event in the tick stops the run with the reason (no interrupt can end it).
    {
        TickRig rig(dol);
        gekko::NativeCri* cri = &rig.cri;
        rig.native.cpu.install_hle(kSvtype4Server, [cri](gekko::Cpu& c) { cri->note_wait(c); return true; },
                                   "test: a waiting server");
        rig.run(1u, 0u);
        check(rig.native.cpu.stopped() &&
                  rig.native.cpu.stop().detail.find("waits for an event inside a CRI server tick") != std::string::npos,
              "a wait inside the tick stops the run");
    }
}


// ---- VIConfigure 0x8037F19C. ORIGINAL runs the SDK's words (the hardware register image included); NATIVE runs the
// owner (VIInit native too: both start from the same VIInit state, which run_case pins). Compared: every MEM1 byte
// outside the call's stack window, except the VI register image the owner retires (see vi_retained) and the "changed"
// mask words (native flags). The retirements are pinned: the original must have written some retired byte; the changed
// flags equal the original's mask for registers 1 and 54.
struct RenderMode {
    u32 tv = 0;
    u16 fb_width = 640, efb_height = 480, xfb_height = 480, vi_x = 40, vi_y = 0, vi_width = 640, vi_height = 480;
    u32 xfb_mode = 1;
};
constexpr u32 kSyntheticMode = 0x81100100u;

void write_mode(Machine& m, const RenderMode& r) {
    m.cpu.mem_write32(kSyntheticMode + 0x00u, r.tv);
    m.cpu.mem_write16(kSyntheticMode + 0x04u, r.fb_width);
    m.cpu.mem_write16(kSyntheticMode + 0x06u, r.efb_height);
    m.cpu.mem_write16(kSyntheticMode + 0x08u, r.xfb_height);
    m.cpu.mem_write16(kSyntheticMode + 0x0Au, r.vi_x);
    m.cpu.mem_write16(kSyntheticMode + 0x0Cu, r.vi_y);
    m.cpu.mem_write16(kSyntheticMode + 0x0Eu, r.vi_width);
    m.cpu.mem_write16(kSyntheticMode + 0x10u, r.vi_height);
    m.cpu.mem_write32(kSyntheticMode + 0x14u, r.xfb_mode);
}

// The words other code reads and the owner writes: static + 2 / + 0x6C (the two registers), HorVer 0xF0-0xF7 and
// 0x106-0x11B, the black flag, the buffer address and the timing; every other byte of the static block is the retired
// register image / adjusted geometry.
bool vi_retained(u32 offset) {
    return (offset >= 0x02u && offset < 0x04u) || (offset >= 0x6Cu && offset < 0x6Eu) ||
           (offset >= 0xF0u && offset < 0xF8u) || (offset >= 0x106u && offset < 0x11Cu) ||
           (offset >= 0x120u && offset < 0x124u) || (offset >= 0x130u && offset < 0x134u) ||
           (offset >= 0x144u && offset < 0x148u);
}

struct ConfigureResult {
    u32 differences = 0;
    u32 retired_differences = 0;
    bool native_stopped = false;
};

// calls: modes to configure in order (0 = the synthetic one).
ConfigureResult configure_case(const std::vector<u8>& dol, const std::string& label, bool pal, u32 encoder_type,
                               const std::vector<u32>& calls, const RenderMode& synthetic, bool perturb = false) {
    ConfigureResult result;
    const auto run = [&](Machine& m, ViFixture& vi, SiFixture& si, PiFixture& pi, gekko::NativeVideo* video) -> bool {
        bool failed = false;
        m.bus.register_mmio(0xCC002000u, 0x100u, &vi);
        m.bus.register_mmio(0xCC003000u, 0x100u, &pi);
        m.bus.register_mmio(0xCC006400u, 0x100u, &si);
        m.cpu.install_hle(kOSReport, [](gekko::Cpu&) { return true; }, "fixture OSReport");
        m.setup(pal, 0u);
        if (video != nullptr) {
            video->boot(pal);
            video->install();
        }
        m.call(kViInit);
        m.cpu.mem_write32(kSda + 0x5BE8u, encoder_type);
        write_mode(m, synthetic);
        for (const u32 mode : calls) {
            if (m.cpu.stopped() || failed) break;
            try {
                m.call(kViConfigure, mode == 0u ? kSyntheticMode : mode);
            } catch (const std::exception&) {
                failed = true;  // the call did not return (the SDK's assert) or stopped the machine
                break;
            }
        }
        return failed || m.cpu.stopped();
    };
    Machine original(dol);
    ViFixture ovi(pal ? 0x0101u : 0x0100u);
    SiFixture osi;
    PiFixture opi;
    const bool original_failed = run(original, ovi, osi, opi, nullptr);
    Machine native(dol);
    ViFixture nvi(pal ? 0x0101u : 0x0100u);
    SiFixture nsi;
    PiFixture npi;
    gekko::NativeInterrupts interrupts(native.cpu);
    gekko::NativeVideo video(native.cpu, interrupts);
    const bool native_failed = run(native, nvi, nsi, npi, &video);
    if (original_failed || native_failed) {
        result.native_stopped = native_failed;
        if (original_failed) {
            check(native_failed, label + ": the original asserts, the native stops");
        } else {
            // The SDK ran on with a NULL timing (getTiming has no record for the mode: PAL / EURGB60 progressive);
            // the native stops instead of replaying the garbage.
            documented(native_failed, label + ": the native stops where the SDK read through a NULL timing");
        }
        return result;
    }
    if (perturb) native.cpu.mem_write8(kViStatic + 0x110u, static_cast<u8>(native.bus.peek8(kViStatic + 0x110u) ^ 1u));
    const std::vector<u8> a = original.bus.window_bytes(kMemoryBegin, kMemoryEnd);
    const std::vector<u8> b = native.bus.window_bytes(kMemoryBegin, kMemoryEnd);
    std::vector<std::string> first;
    for (u32 offset = 0; offset < kMemorySize; ++offset) {
        if (a[offset] == b[offset]) continue;
        const u32 address = kMemoryBegin + offset;
        const u32 low = kStackTop - kStackWindow - kMemoryBegin, high = kStackTop + 8u - kMemoryBegin;
        if (offset >= low && offset < high) continue;
        if (excluded(offset)) continue;  // the VI handler slot: the original VIInit installs the SDK handler (documented)
        const bool in_static = address >= kViStatic && address < kViStatic + kViStaticSize;
        if (in_static && !vi_retained(address - kViStatic)) {
            ++result.retired_differences;
            continue;
        }
        if (address >= kSda + 0x5BF8u && address < kSda + 0x5C00u) continue;  // the changed mask: native flags
        ++result.differences;
        if (first.size() < 6u) first.push_back("0x" + hex(address) + " original " + hex(a[offset]) + " native " + hex(b[offset]));
    }
    if (perturb) return result;
    for (const std::string& line : first) check(false, label + ": memory " + line);
    check(result.differences == 0u, label + ": " + std::to_string(result.differences) + " differing memory bytes");
    const u32 hi = original.word(kSda + 0x5BF8u), lo = original.word(kSda + 0x5BFCu);
    check((hi & 0x40000000u) != 0u && (lo & 0x200u) != 0u, label + ": the original marked registers 1 and 54 changed");
    check(video.changed() == 3u, label + ": the native marked registers 1 and 54 changed");
    return result;
}

void configure_cases(const std::vector<u8>& dol) {
    // The game's presets on a PAL console (kinds 1 and 5), then on an NTSC console (kinds 0 and 2).
    const u32 ntsc_int = 0x805688C0u, ntsc_prog = 0x805688FCu;
    for (const u32 encoder : {1u, 0u}) {
        const std::string e = " encoder " + std::to_string(encoder);
        ConfigureResult r = configure_case(dol, "PAL console, PAL then EURGB60" + e, true, encoder, {kRenderModePal, kRenderModeEuRgb60}, RenderMode{});
        check(r.retired_differences > 0u, "PAL console" + e + ": the original wrote retired register image bytes");
        configure_case(dol, "PAL console, EURGB60 twice" + e, true, encoder, {kRenderModeEuRgb60, kRenderModeEuRgb60}, RenderMode{});
        r = configure_case(dol, "NTSC console, interlaced then progressive" + e, false, encoder, {ntsc_int, ntsc_prog}, RenderMode{});
        check(r.retired_differences > 0u, "NTSC console" + e + ": the original wrote retired register image bytes");
        configure_case(dol, "NTSC console, progressive twice" + e, false, encoder, {ntsc_prog, ntsc_prog}, RenderMode{});
    }
    // Synthetic modes: every low-bits value (field / non-interlaced / progressive / 3D), frame-buffer modes and sizes.
    for (const u32 ni : {0u, 1u, 2u, 3u}) {
        for (const u32 xfb : {0u, 1u}) {
            RenderMode r;
            r.xfb_mode = xfb;
            r.fb_width = static_cast<u16>(512u + 8u * ni);
            r.xfb_height = static_cast<u16>(448u + 2u * xfb);
            r.vi_x = static_cast<u16>(20u + ni);
            r.vi_y = static_cast<u16>(12u + 2u * xfb);
            r.vi_width = 600u;
            r.tv = (0u << 2) | ni;  // kind 0 on an NTSC console
            configure_case(dol, "synthetic NTSC ni=" + std::to_string(ni) + " xfb=" + std::to_string(xfb), false, 1u, {0u, 0u}, r);
            r.tv = (1u << 2) | ni;  // kind 1 on a PAL console
            configure_case(dol, "synthetic PAL ni=" + std::to_string(ni) + " xfb=" + std::to_string(xfb), true, 1u, {0u}, r);
            r.tv = (5u << 2) | ni;
            configure_case(dol, "synthetic EURGB60 ni=" + std::to_string(ni) + " xfb=" + std::to_string(xfb), true, 0u, {0u}, r);
        }
    }
    // Negative control: one flipped byte in a word the owner writes is exactly one detected difference.
    check(configure_case(dol, "negative control", true, 1u, {kRenderModePal}, RenderMode{}, true).differences == 1u,
          "negative control: one perturbed HorVer byte is exactly one difference");
    // A mode of the wrong kind for the console is the SDK's assert: both stop.
    RenderMode wrong;
    wrong.tv = (1u << 2);
    const ConfigureResult bad = configure_case(dol, "PAL render mode on an NTSC console", false, 1u, {0u}, wrong);
    check(bad.native_stopped, "a render mode of the wrong kind stops the native run (the SDK's assert)");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "usage: native_video_reference_tests <main.dol>");
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() >= 256u, "missing original DOL");
        // The PAL warm start: no offset, both clamps of AdjustedDispPosX (40 +
        // offset in 0..80) and a negative offset inside them.
        for (const u8 offset : {u8{0x00}, u8{0xF6}, u8{0x3C}, u8{0xCE}}) run_case(dol, true, offset);
        // The NTSC cold start (__VIInit(VI_TVMODE_NTSC_INT)).
        run_case(dol, false, 0x00u);
        // One retrace committing a staged configuration.
        retrace_case(dol, kRenderModePal, 625u);
        retrace_case(dol, kRenderModeEuRgb60, 525u);
        // The register-staging entry points.
        staging_cases(dol);
        // VIConfigure.
        configure_cases(dol);
        // The CRI vsync tick inside the retrace delivery.
        tick_cases(dol);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
    std::printf("%d checks, %d failures, %d documented differences\n", g_cases, g_failures, g_documented);
    return g_failures == 0 ? 0 : 1;
}
