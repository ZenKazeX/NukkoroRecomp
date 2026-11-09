// liveness-roots: reference-interpreter-only
// The OS's interrupt gate (docs/agents/card-saves.md, os.interrupts): the owners of OSDisableInterrupts 0x8037611C,
// OSEnableInterrupts 0x80376130 and OSRestoreInterrupts 0x80376144 (NativeInterrupts::install) against the ORIGINAL
// words of the pinned DOL in the reference interpreter, and the exception-vector table (gekko/os_exception_vector.h)
// against the original __OSExceptionVector words at 0x80371620.
//
// ORIGINAL runs the DOL's instructions on a bare machine; NATIVE runs the same machine with the owners installed. Both
// are driven by run_steps, so the original's mtmsr and the owner's hand-back go through the same recognition points.
// After every call the registers, MSR and the interrupts taken must be equal. A pending external interrupt is taken
// through the OS's vector (the real page) into a stand-in handler that does what OSLoadContext does: it restores the
// interrupted registers from the OSContext the vector filled and returns.
//
// usage: os_interrupts_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_interrupts.h"
#include "gekko/os_exception_vector.h"
#include "support/os_entry.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;

constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u;
constexpr u32 kDisable = 0x8037611Cu, kEnable = 0x80376130u, kRestore = 0x80376144u;
constexpr u32 kVectorTemplate = 0x80371620u;
constexpr u32 kContext = 0x81006000u;
constexpr u32 kHandler = 0x81005000u;

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        if (g_failures <= 60) std::printf("FAIL: %s\n", what.c_str());
    }
}
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08X", v);
    return b;
}

enum class Variant { Original, Native, WrongOwner };

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    NativeInterrupts interrupts{cpu};
    unsigned handled = 0;  // external interrupts the stand-in handler served

    Machine(const std::vector<u8>& dol, Variant variant) {
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
        cpu.reset();
        if (variant == Variant::Native) interrupts.install();
        if (variant == Variant::WrongOwner) {
            // A gate that answers the previous EE wrongly (0) and never changes MSR: the comparator must see it.
            for (const u32 entry : {kDisable, kEnable, kRestore})
                cpu.install_hle(entry, [](Cpu& c) {
                    c.gpr[3] = 0;
                    return true;
                }, "test: wrong gate");
        }
        // The OS's external-interrupt entry as __OSExceptionInit and OSSetCurrentContext leave it.
        test_support::install_os_exception(memory, Exception::ExternalInterrupt, 4u, kHandler);
        memory.write32(test_support::kOsCurrentContext, kContext);
        memory.write32(kReturn, 0x60000000u);  // nop: where the call returns; a pending interrupt is taken before it
        // The stand-in for the OS's handler and OSLoadContext: count, drop the line, put back what the vector saved.
        cpu.install_hle(kHandler, [this](Cpu& c) {
            ++handled;
            c.set_external_interrupt(false);
            c.gpr[3] = memory.read32(kContext + 0x00Cu);
            c.gpr[4] = memory.read32(kContext + 0x010u);
            c.gpr[5] = memory.read32(kContext + 0x014u);
            c.cr = memory.read32(kContext + 0x080u);
            c.lr = memory.read32(kContext + 0x084u);
            c.ctr = memory.read32(kContext + 0x088u);
            c.xer = memory.read32(kContext + 0x08Cu);
            c.set_msr(memory.read32(kContext + 0x19Cu) | kMsrIR | kMsrDR);
            c.leave_to_dispatcher(memory.read32(kContext + 0x198u), FallbackReason::Yield, kHandler);
            return true;
        }, "test: external handler");
    }
};

struct Result {
    u32 gpr[32];
    u32 cr, xer, ctr, lr, msr, pc;
    unsigned handled;
    bool stopped;
};

struct Setup {
    u32 msr;
    u32 xer = 0;
    u32 r3 = 0;
    bool pending = false;
};

Result run(Machine& m, u32 entry, const Setup& setup) {
    Cpu& cpu = m.cpu;
    cpu.reset();
    for (u32 i = 0; i < 32; ++i) cpu.gpr[i] = 0x11110000u + i * 0x101u;  // distinct sentinels: what a call leaves alone
    cpu.gpr[1] = kStack;
    cpu.gpr[3] = setup.r3;
    cpu.lr = kReturn;
    cpu.xer = setup.xer;
    cpu.cr = 0x20400080u;
    cpu.ctr = 0x55AA55AAu;
    cpu.set_msr(setup.msr);
    m.handled = 0;
    cpu.pc = entry;
    cpu.npc = entry + 4;
    if (setup.pending) cpu.set_external_interrupt(true);
    unsigned steps = 0;
    while ((cpu.pc != kReturn || cpu.interrupt_pending()) && !cpu.stopped() && steps++ < 4000) cpu.run_steps(1);
    Result r{};
    for (u32 i = 0; i < 32; ++i) r.gpr[i] = cpu.gpr[i];
    r.cr = cpu.cr;
    r.xer = cpu.xer;
    r.ctr = cpu.ctr;
    r.lr = cpu.lr;
    r.msr = cpu.msr();
    r.pc = cpu.pc;
    r.handled = m.handled;
    r.stopped = cpu.stopped() || cpu.pc != kReturn;
    cpu.set_external_interrupt(false);
    return r;
}

bool equal(const Result& a, const Result& b, std::string* why = nullptr) {
    const auto differ = [&](const char* what, u32 x, u32 y) {
        if (why) *why += std::string(" ") + what + " " + hex(x) + "/" + hex(y);
        return true;
    };
    bool different = false;
    for (u32 i = 0; i < 32; ++i)
        if (a.gpr[i] != b.gpr[i]) different |= differ(("r" + std::to_string(i)).c_str(), a.gpr[i], b.gpr[i]);
    if (a.cr != b.cr) different |= differ("cr", a.cr, b.cr);
    if (a.xer != b.xer) different |= differ("xer", a.xer, b.xer);
    if (a.ctr != b.ctr) different |= differ("ctr", a.ctr, b.ctr);
    if (a.lr != b.lr) different |= differ("lr", a.lr, b.lr);
    if (a.msr != b.msr) different |= differ("msr", a.msr, b.msr);
    if (a.pc != b.pc) different |= differ("pc", a.pc, b.pc);
    if (a.handled != b.handled) different |= differ("handled", a.handled, b.handled);
    if (a.stopped != b.stopped) different |= differ("stopped", a.stopped, b.stopped);
    return !different;
}

std::vector<u8> read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}

const char* name_of(u32 entry) { return entry == kDisable ? "Disable" : entry == kEnable ? "Enable" : "Restore"; }

void test_vector_table(const std::vector<u8>& dol) {
    Machine original(dol, Variant::Original);
    bool same = true;
    for (u32 word = 0; word < kOsVectorWords; ++word) {
        const u32 expected = original.memory.peek32(kVectorTemplate + 4u * word);
        if (word * 4u == kOsVectorPatchOffset) {
            check((expected & 0xFFFF0000u) == 0x38600000u, "the template's +0x68 is a `li r3, ...`");
        } else if (kOsExceptionVector[word] != expected) {
            same = false;
            std::printf("  vector word %u: table %s, original %s\n", word, hex(kOsExceptionVector[word]).c_str(),
                        hex(expected).c_str());
        }
    }
    check(same, "gekko/os_exception_vector.h equals the original __OSExceptionVector words (all but the patched one)");
    // Negative control: a table with one different word is told apart.
    check(kOsExceptionVector[1] == original.memory.peek32(kVectorTemplate + 4u), "negative control setup: word 1 equals");
    check(kOsExceptionVector[1] != (original.memory.peek32(kVectorTemplate + 4u) ^ 1u),
          "negative control: a flipped bit is told apart");
    // The index patch: each index builds the page __OSExceptionInit builds (li r3, index).
    for (u32 index = 0; index < 15; ++index)
        check(os_exception_vector_word(kOsVectorPatchOffset / 4u, index) == (0x38600000u | index), "index patch");
    std::printf("  exception-vector table: %u words compared\n", kOsVectorWords);
}

void test_gate(const std::vector<u8>& dol) {
    Machine original(dol, Variant::Original), native(dol, Variant::Native);
    unsigned cases = 0, taken = 0;
    for (const u32 entry : {kDisable, kEnable, kRestore}) {
        for (const u32 msr : {0x00002032u, 0x0000B032u, 0x00000032u, 0x0000A832u, 0x0000B132u}) {  // EE 0 / 1, other bits
            for (const u32 xer : {0u, 0x80000000u, 0x20000000u}) {
                for (const u32 r3 : {0u, 1u, 2u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu}) {
                    if (entry != kRestore && r3 != 0u) continue;  // only Restore reads its argument
                    for (const bool pending : {false, true}) {
                        const Setup setup{msr, xer, r3, pending};
                        const Result a = run(original, entry, setup);
                        const Result b = run(native, entry, setup);
                        std::string why;
                        const bool same = equal(a, b, &why) && !a.stopped;
                        check(same, std::string(name_of(entry)) + " msr " + hex(msr) + " xer " + hex(xer) + " r3 " +
                                        hex(r3) + (pending ? " pending" : "") + ":" + why);
                        ++cases;
                        taken += a.handled;
                    }
                }
            }
        }
    }
    std::printf("  gate: %u cases equal, %u external interrupts taken in the original (same in the owner)\n", cases,
                taken);
    check(taken > 0, "the pending cases really took an interrupt");

    // The cases this test exists for: EE 0 -> 1 with a condition pending (the write makes it deliverable), and
    // a pending condition under EE = 1 at entry (taken before the write).
    const Result enabled_original = run(original, kEnable, {0x00002032u, 0, 0, true});
    const Result enabled_native = run(native, kEnable, {0x00002032u, 0, 0, true});
    check(enabled_original.handled == 1u && enabled_native.handled == 1u, "Enable with a pending line takes it once");
    check(enabled_original.gpr[3] == 0u && (enabled_original.msr & kMsrEE) != 0u,
          "Enable returns the previous EE (0) and leaves EE set");
    const Result restored_original = run(original, kRestore, {0x00002032u, 0, 1, true});
    const Result restored_native = run(native, kRestore, {0x00002032u, 0, 1, true});
    check(restored_original.handled == 1u && restored_native.handled == 1u,
          "Restore(1) with a pending line takes it once, at the EE 0 -> 1 write");
    const Result disabled_original = run(original, kDisable, {0x0000A032u, 0, 0, true});
    const Result disabled_native = run(native, kDisable, {0x0000A032u, 0, 0, true});
    check(disabled_original.handled == disabled_native.handled && (disabled_native.msr & kMsrEE) == 0u,
          "Disable with a pending line under EE = 1: equal interrupts taken, EE clear at the end");
    const Result held_original = run(original, kRestore, {0x0000A032u, 0, 0, false});
    check(held_original.gpr[3] == 1u && (held_original.msr & kMsrEE) == 0u, "Restore(0) clears EE and returns the old EE");
}

void test_negative_control(const std::vector<u8>& dol) {
    Machine original(dol, Variant::Original), wrong(dol, Variant::WrongOwner);
    unsigned told_apart = 0, cases = 0;
    for (const u32 entry : {kDisable, kEnable, kRestore}) {
        const Setup setup{0x0000B032u, 0, 1, false};
        ++cases;
        if (!equal(run(original, entry, setup), run(wrong, entry, setup))) ++told_apart;
    }
    check(told_apart == cases, "negative control: a gate that returns EE = 0 and leaves MSR alone is told apart in every function");
    // And the comparator tells a machine with a different MSR apart from itself.
    Machine other(dol, Variant::Original);
    check(!equal(run(original, kDisable, {0x0000B032u, 0, 0, false}), run(other, kDisable, {0x00002032u, 0, 0, false})),
          "negative control: results of different inputs are told apart");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "usage: os_interrupts_contract_tests <sys/main.dol>");
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        test_vector_table(dol);
        test_gate(dol);
        test_negative_control(dol);
    } catch (const std::exception& e) {
        std::printf("os_interrupts_contract_tests: %s\n", e.what());
        return 2;
    }
    if (g_failures != 0) {
        std::printf("os_interrupts_contract_tests: %d failures\n", g_failures);
        return 1;
    }
    std::printf("os_interrupts_contract_tests: all checks passed\n");
    return 0;
}
