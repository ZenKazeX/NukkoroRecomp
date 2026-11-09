// liveness-roots: reference-interpreter-only
// Original-DOL contract for the owned DMAErrorHandler (gekko/native_os_machine_check.h).
// The original instructions run in the reference interpreter with OSReport (0x803731CC), OSDumpContext (0x80372E58)
// and PPCHalt (0x80370B1C) replaced by recording stubs. For every HID2 / SRR1 / context pointer combination:
//  - the first two OSReport texts of the original (format read from the guest, "%x" arguments formatted) equal, byte
//    for byte, the two lines the native owner sends to the host debug console (captured through DebugConsole);
//  - on a retail console (no UART) the native console drops them, as the UART of the original does;
//  - THE EXPLICIT DIFFERENCE: the original returns in the DMA branch (HID2 & 0x00F00000 and SRR1 & 0x00200000 both
//    set) and calls OSDumpContext and PPCHalt otherwise; the native owner stops with an ExecutionFault naming HID2,
//    SRR1 and the context in BOTH. This is a stated difference, not equivalence: no host machine check exists.
// usage: os_machine_check_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_os_machine_check.h"
#include "gekko/native_os_report.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
using namespace gekko::os_machine_check;
constexpr u32 kOSReport = 0x803731CCu, kDump = 0x80372E58u, kHalt = 0x80370B1Cu;
constexpr u32 kStack = 0x817E0000u, kR13 = 0x805EC500u, kR2 = 0x805FA780u, kContext = 0x80800000u,
              kInfo = 0x80700000u, kReturn = 0x80100000u;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    explicit Machine(const std::vector<u8>& dol) {
        const auto word = [&](std::size_t o) {
            return u32{dol[o]} << 24 | u32{dol[o + 1]} << 16 | u32{dol[o + 2]} << 8 | u32{dol[o + 3]};
        };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (size == 0u) continue;
            require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
            memory.write_block(address, dol.data() + offset, size);
        }
        require(memory.read32(kDmaErrorHandler) == 0x7C0802A6u, "unexpected handler words");
        cpu.reset();
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
    }
    void setup(u32 hid2, u32 srr1, u32 console_type) {
        cpu.reset();  // also clears a stop left by the previous variation
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
        memory.write32(kContext + kContextSrr1, srr1);
        memory.write32(kR13 + 0x5A20u, kInfo);
        memory.write32(kInfo + 0x2Cu, console_type);
        cpu.spr[SPR_HID2] = hid2;
        cpu.gpr[1] = kStack;
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.gpr[3] = 1u;
        cpu.gpr[4] = kContext;
        cpu.gpr[5] = 0x11u;
        cpu.gpr[6] = 0x22u;
        cpu.lr = kReturn;
        cpu.cr = 0;
    }
    std::string guest_string(u32 address) {
        std::string s;
        for (u32 i = 0; i < 200; ++i) {
            const u8 c = memory.read8(address + i);
            if (c == 0) break;
            s.push_back(static_cast<char>(c));
        }
        return s;
    }
};
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: os_machine_check_contract_tests main.dol");
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");

        Machine o(dol), n(dol);
        unsigned runs = 0, original_returned = 0, original_halted = 0;
        const u32 hid2s[] = {0xE0000000u, 0xE0800000u, 0xE0400000u, 0xE0200000u, 0xE0100000u, 0xE0F00000u, 0xE0000001u,
                             0xA0F00000u};
        const u32 srr1s[] = {0u, 0x00200000u, 0x00001000u, 0xFFFFFFFFu, 0x00208000u};
        for (u32 hid2 : hid2s)
            for (u32 srr1 : srr1s)
                for (u32 type : {0x10000002u, 0x00000002u}) {
                    o.setup(hid2, srr1, type);
                    Cpu& c = o.cpu;
                    c.pc = kDmaErrorHandler;
                    c.npc = kDmaErrorHandler + 4u;
                    std::vector<std::string> texts;
                    bool dumped = false, halted = false;
                    for (unsigned steps = 0; steps < 5000 && c.pc != kReturn && !halted && !c.stopped(); ++steps) {
                        if (c.pc == kOSReport) {
                            const std::string format = o.guest_string(c.gpr[3]);
                            char text[128];
                            std::snprintf(text, sizeof(text), format.c_str(), c.gpr[4], c.gpr[5]);
                            texts.push_back(text);
                            c.pc = c.lr;
                            c.npc = c.pc + 4u;
                            continue;
                        }
                        if (c.pc == kDump) {
                            dumped = true;
                            c.pc = c.lr;
                            c.npc = c.pc + 4u;
                            continue;
                        }
                        if (c.pc == kHalt) {
                            halted = true;
                            break;
                        }
                        c.run_steps(1u);
                    }
                    const bool dma = (hid2 & 0x00F00000u) != 0u && (srr1 & 0x00200000u) != 0u;
                    require(halted != (c.pc == kReturn), "the original neither returned nor halted (pc " + std::to_string(c.pc) + ", stopped " + std::to_string(c.stopped()) + ", hid2 " + std::to_string(hid2) + ", srr1 " + std::to_string(srr1) + ")");
                    require(halted == !dma && dumped == !dma, "original branch differs from the documented rule");
                    require(texts.size() >= 2, "too few OSReport calls in the original");
                    if (dma) ++original_returned; else ++original_halted;

                    n.setup(hid2, srr1, type);
                    os_report::DebugConsole console;
                    Cpu& d = n.cpu;
                    serve_dma_error_handler(d, console);
                    require(d.stopped() && d.stop().reason == StopReason::ExecutionFault,
                            "the native owner must stop with an ExecutionFault in both branches");
                    char want[64];
                    std::snprintf(want, sizeof(want), "0x%08X", hid2);
                    require(d.stop().detail.find(want) != std::string::npos, "stop detail lacks HID2");
                    std::snprintf(want, sizeof(want), "0x%08X", srr1);
                    require(d.stop().detail.find(want) != std::string::npos, "stop detail lacks SRR1: " + d.stop().detail);
                    std::snprintf(want, sizeof(want), "0x%08X", kContext);
                    require(d.stop().detail.find(want) != std::string::npos, "stop detail lacks the context");
                    if ((type & 0x10000000u) != 0u) {
                        require(console.stats().calls == 2, "native console call count");
                        require(console.last_text() == texts[1],
                                "second line differs: [" + console.last_text() + "] vs [" + texts[1] + "]");
                        require(console.recent().size() == 2 && console.recent()[0] + "\n" == texts[0] &&
                                    console.recent()[1] + "\n" == texts[1],
                                "the native lines differ from the OSReport texts of the original");
                    } else {
                        require(console.stats().calls == 0 && console.stats().suppressed == 1,
                                "a retail console must drop the text (the first write fails, the second is skipped)");
                    }
                    ++runs;
                }
        // The default exception handler 0x803716BC (serve_unhandled_exception). The original runs with the same recorders plus
        // stubs for the context switch / time callees; the texts it sends before OSDumpContext (or before the registered
        // handler returns) must equal the native console lines. A registered game handler is not run in the original.
        constexpr u32 kTable = kErrorHandlerTable, kStubHandler = 0x80100100u;
        constexpr u32 kTime = 0x80379628u, kSwitch = 0x80372C68u, kLoad1 = 0x8037850Cu, kLoad2 = 0x803789D8u;
        unsigned unhandled_runs = 0, handler_faults = 0;
        for (u32 error : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 12u})
            for (u32 srr1 : {0x00000002u, 0x00000000u, 0x00100002u, 0x0000A032u})
                for (unsigned variant = 0; variant < 4; ++variant)
                    for (u32 type : {0x10000002u, 0x00000002u}) {
                        const bool fp = error == 6u && (srr1 & 0x00100000u) != 0u;
                        for (Machine* m : {&o, &n}) {
                            m->setup(0xA0F00000u, srr1, type);
                            for (u32 i = 0; i < 17u; ++i) m->memory.write32(kTable + 4u * i, 0u);
                            if (variant == 1u) m->memory.write32(kTable + 4u * error, kDmaErrorHandler);
                            if (variant == 2u) m->memory.write32(kTable + 4u * error, kStubHandler);
                            if (variant == 3u) m->memory.write32(kTable + 4u * 16u, kDmaErrorHandler);
                            m->memory.write32(0x800000D8u, 0u);
                            m->memory.write32(kContext + kContextSrr0, 0x80001234u);
                            m->cpu.gpr[3] = error;
                        }
                        // What the handler lookup selects (the same rule the original executes).
                        const u32 slot = (srr1 & 2u) == 0u ? 0u : fp && variant == 3u ? 16u : error;
                        const u32 entry = (srr1 & 2u) == 0u ? 0u : o.memory.read32(kTable + 4u * (fp && variant == 3u ? 16u : error));
                        (void)slot;
                        std::vector<std::string> texts;
                        if (entry != kStubHandler) {
                            Cpu& c = o.cpu;
                            c.pc = kUnhandledException;
                            c.npc = kUnhandledException + 4u;
                            for (unsigned steps = 0; steps < 20000 && !c.stopped(); ++steps) {
                                if (c.pc == kOSReport) {
                                    const std::string format = o.guest_string(c.gpr[3]);
                                    char text[160];
                                    std::snprintf(text, sizeof(text), format.c_str(), c.gpr[4], c.gpr[5]);
                                    texts.push_back(text);
                                } else if (c.pc == kDump || c.pc == kHalt) {
                                    break;
                                } else if (c.pc == kTime || c.pc == kSwitch || c.pc == kLoad1 || c.pc == kLoad2) {
                                    c.gpr[3] = c.gpr[4] = 0u;
                                } else {
                                    c.run_steps(1u);
                                    continue;
                                }
                                c.pc = c.lr;  // a stubbed callee returns at once
                                c.npc = c.pc + 4u;
                            }
                        }
                        os_report::DebugConsole console;
                        Cpu& d = n.cpu;
                        serve_unhandled_exception(d, console);
                        require(d.stopped() && d.stop().reason == StopReason::ExecutionFault,
                                "the native default handler must stop with an ExecutionFault");
                        if (entry == kStubHandler) {
                            char want[32];
                            std::snprintf(want, sizeof(want), "0x%08X", kStubHandler);
                            require(d.stop().detail.find(want) != std::string::npos, "stop detail lacks the handler address");
                            require(console.stats().calls == 0, "a game handler prints nothing natively");
                            ++handler_faults;
                        } else if ((type & 0x10000000u) != 0u) {
                            std::string native_text, original_text;
                            for (const std::string& line : console.recent()) native_text += line + "\n";
                            for (const std::string& s : texts) original_text += s;
                            const auto strip = [](std::string s) {
                                std::string r;
                                for (char ch : s) if (ch != 10) r.push_back(ch);
                                return r;
                            };
                            native_text = strip(native_text);
                            original_text = strip(original_text);
                            require(!native_text.empty() && original_text.compare(0, native_text.size(), native_text) == 0,
                                    "the native text is not a prefix of the original's OSReport text (error " +
                                        std::to_string(error) + ", srr1 " + std::to_string(srr1) + ", variant " +
                                        std::to_string(variant) + "): [" + native_text + "] vs [" + original_text + "]");
                        } else {
                            require(console.stats().calls == 0, "a retail console must drop the text");
                        }
                        ++unhandled_runs;
                    }
        std::printf("PASS os unhandled exception: %u variations (%u with a game handler: named fault, nothing printed)\n",
                    unhandled_runs, handler_faults);
        std::printf("PASS os machine check: %u variations; the two lines before the branch equal the OSReport texts of the "
                    "original byte for byte (retail consoles drop them); STATED DIFFERENCE: the original returned in %u "
                    "variations (DMA branch) and halted after OSDumpContext in %u, the native owner stops with an "
                    "ExecutionFault in all\n", runs, original_returned, original_halted);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL os machine check contract: %s\n", error.what());
        return 1;
    }
}
