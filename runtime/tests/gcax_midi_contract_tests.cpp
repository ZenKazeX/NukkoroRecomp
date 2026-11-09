// GCAX's MIDI input, native against the original instructions (gekko/native_gcax_midi.h).
// liveness-roots: reference-interpreter-only
//
// Both sides get the same guest memory: a GCAX context with eight ports (some open, some closed), the sound object
// (vtable 0x8053F42C, port handles at +8) and the output configuration. The original gcaxPortPush 0x8043F054 and the
// sound object's methods run from main.dol in the interpreter; the native push and SoundObject run natively. After
// every call every byte of the context's ports must match.

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_gcax_midi.h"

#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
constexpr u32 kContext = 0x81000000u;
constexpr u32 kObject = kContext + 0x20000u;
constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kVtable = 0x8053F42Cu;
constexpr u32 kArea = 0x30000u;  // context + object

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Side {
    GuestMemory memory;
    Cpu cpu{memory};
    gcax_midi::Report report;
    explicit Side(const std::vector<u8>& dol) {
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
    }
    void setup(u32 open_mask, u32 mode, bool context) {
        const std::vector<u8> zero(kArea);
        memory.write_block(kContext, zero.data(), zero.size());
        memory.write32(gcax_midi::kContextPointer, context ? kContext : 0u);
        for (u32 p = 0; p < 8; ++p) {
            const u32 port = kContext + gcax_midi::kPorts + p * gcax_midi::kPortBytes;
            memory.write32(port, (open_mask >> p) & 1u ? 0x80000000u : 0u);
            memory.write32(kObject + 8 + 4 * p, port);
        }
        memory.write32(kObject, kVtable);
        memory.write32(gcax_midi::kOutputConfig + 0x78, mode);
        cpu.fpscr = 0;
    }
    std::vector<u8> image() {
        std::vector<u8> bytes(kArea);
        memory.read_block(kContext, bytes.data(), bytes.size());
        return bytes;
    }
};

struct Harness {
    Side original, native;
    u64 calls = 0, instructions = 0;
    explicit Harness(const std::vector<u8>& dol) : original(dol), native(dol) {}

    void setup(u32 open_mask, u32 mode, bool context = true) {
        original.setup(open_mask, mode, context);
        native.setup(open_mask, mode, context);
    }
    void set_ring(u32 port_index, u32 count, u32 write) {
        for (Side* s : {&original, &native}) {
            const u32 port = kContext + gcax_midi::kPorts + port_index * gcax_midi::kPortBytes;
            s->memory.write32(port + gcax_midi::kCount, count);
            s->memory.write32(port + gcax_midi::kWrite, write);
        }
    }
    // Runs `entry` with r3..r8 on the original side, `native_call` on the native side, then compares.
    void call(u32 entry, std::vector<u32> args, const std::function<void(gcax_midi::SoundObject&)>& native_call,
              const std::string& what, bool fresh_rings = true) {
        // A method's messages are compared only if they are stored: each method call starts from empty rings at a
        // moving write position (the push cases set their rings themselves).
        if (fresh_rings)
            for (u32 p = 0; p < 8; ++p) set_ring(p, 0u, static_cast<u32>((calls * 7u + p * 131u) % 0x400u));
        Cpu& cpu = original.cpu;
        cpu.reset();
        cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kReturn;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
        cpu.fpscr = 0;
        for (std::size_t i = 0; i < args.size(); ++i) cpu.gpr[3 + i] = args[i];
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 100000) { cpu.run_steps(1); ++instructions; }
        require(cpu.pc == kReturn && !cpu.stopped(), "original " + what + " did not return");
        native.cpu.reset();
        native.cpu.fpscr = 0;
        gcax_midi::SoundObject object{native.cpu, kObject, native.report};
        native_call(object);
        require(original.image() == native.image(), what + ": ring memory differs");
        ++calls;
    }
};

void run(Harness& h) {
    constexpr u32 kPush = gcax_midi::kPortPush;
    const u32 port0 = kContext + gcax_midi::kPorts;
    // The push itself: empty, wrap, full (0x3FF, 0x400, 0x401) rings.
    h.setup(0xFFu, 0);
    for (u32 count : {0u, 1u, 0x3FEu, 0x3FFu, 0x400u, 0x401u})
        for (u32 write : {0u, 1u, 0x200u, 0x3FEu, 0x3FFu}) {
            h.set_ring(0, count, write);
            const u32 message = 0x90307F01u ^ (count * 0x10001u) ^ write;
            h.call(kPush, {port0, message}, [&](gcax_midi::SoundObject& o) {
                gcax_midi::push(o.cpu.guest_memory(), port0, message, o.report);
            }, "push count " + std::to_string(count) + " write " + std::to_string(write), false);
        }
    // Methods, on open and closed ports, with and without a context, in stereo and surround modes.
    for (int mode : {0, 2})
        for (u32 mask : {0x07u, 0x05u}) {
            h.setup(mask, static_cast<u32>(mode));
            for (s32 port = 0; port < 3; ++port)
                for (u32 ch : {0u, 5u, 15u}) {
                    const std::string at = " mode " + std::to_string(mode) + " mask " + std::to_string(mask) +
                                           " port " + std::to_string(port) + " ch " + std::to_string(ch);
                    for (s32 v = -128; v < 128; ++v) {
                        h.call(0x801F27B4u, {kObject, static_cast<u32>(v), static_cast<u32>(port), ch},
                               [&](gcax_midi::SoundObject& o) { o.volume(v, port, ch); }, "volume " + std::to_string(v) + at);
                        h.call(0x801F26D4u, {kObject, static_cast<u32>(v), static_cast<u32>(port), ch},
                               [&](gcax_midi::SoundObject& o) { o.pan(v, port, ch); }, "pan " + std::to_string(v) + at);
                        h.call(0x801F25A8u, {kObject, static_cast<u32>(v), static_cast<u32>(port), ch},
                               [&](gcax_midi::SoundObject& o) { o.surround(v, port, ch); }, "cc62 " + std::to_string(v) + at);
                    }
                    for (s32 b = -0x5000; b <= 0x5000; b += 0x137) {
                        const s32 v = static_cast<s16>(b);
                        h.call(0x801F245Cu, {kObject, static_cast<u32>(v), static_cast<u32>(port), ch},
                               [&](gcax_midi::SoundObject& o) { o.bend(v, port, ch); }, "bend " + std::to_string(v) + at);
                    }
                    for (s32 edge : {-0x4001, -0x4000, -1, 0, 1, 0x3FFF, 0x4000})
                        h.call(0x801F245Cu, {kObject, static_cast<u32>(edge), static_cast<u32>(port), ch},
                               [&](gcax_midi::SoundObject& o) { o.bend(edge, port, ch); }, "bend edge" + at);
                    for (u32 prio = 0; prio < 5; ++prio)
                        h.call(0x801F297Cu, {kObject, 7u, 0x31u, prio, static_cast<u32>(port), ch},
                               [&](gcax_midi::SoundObject& o) { o.note_on(prio, port, ch); }, "note on" + at);
                    h.call(0x801F28F8u, {kObject, static_cast<u32>(port), ch},
                           [&](gcax_midi::SoundObject& o) { o.note_off(port, ch); }, "note off" + at);
                    for (s32 bank : {0, 7, 0x7F}) for (s32 program : {0, 49, 0x7F, -1})
                        h.call(0x801F2270u, {kObject, static_cast<u32>(port), ch, static_cast<u32>(bank), static_cast<u32>(program)},
                               [&](gcax_midi::SoundObject& o) { o.set_bank_program(port, ch, bank, program); }, "bank/program" + at);
                    h.call(0x801F2508u, {kObject, static_cast<u32>(port), ch},
                           [&](gcax_midi::SoundObject& o) { o.reset_pan(port, ch); }, "reset pan" + at);
                }
        }
    // No context: every method refuses.
    h.setup(0xFFu, 0, false);
    h.call(0x801F27B4u, {kObject, 5u, 0u, 1u}, [&](gcax_midi::SoundObject& o) { o.volume(5, 0, 1); }, "volume without a context");
    h.call(0x801F297Cu, {kObject, 0u, 0u, 1u, 0u, 1u}, [&](gcax_midi::SoundObject& o) { o.note_on(1, 0, 1); }, "note on without a context");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "usage: gcax_midi_contract_tests <main.dol>");
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        // SHA-256: fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af.
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        Harness h(dol);
        run(h);
        std::printf("PASS native GCAX MIDI input: %llu calls identical to %llu original instructions (push incl. full "
                    "and wrapping rings; volume, pan, CC62 over every value; bend past both clamps; note on/off; "
                    "bank/program; reset pan in stereo and surround; closed ports and no context); native pushed %llu, "
                    "dropped %llu, refused %llu\n",
                    static_cast<unsigned long long>(h.calls), static_cast<unsigned long long>(h.instructions),
                    static_cast<unsigned long long>(h.native.report.pushed),
                    static_cast<unsigned long long>(h.native.report.dropped),
                    static_cast<unsigned long long>(h.native.report.refused));
        return 0;
    } catch (const std::exception& error) {
        std::printf("FAIL: %s\n", error.what());
        return 1;
    }
}
