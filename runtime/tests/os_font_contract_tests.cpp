// Original-DOL contract for the owned OS font library (OSGetFontEncode, OSInitFont, OSGetFontTexture).
// liveness-roots: reference-interpreter-only
// Every SDK instruction comes from the pinned DOL. The video encoder status register is the one fixture: the
// translated body reads it with lhz 0xCC00206E, and a production run serves it from native_video.cpp.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_os_font.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kSdaEncodeCache = kR13 + 0x2708u, kSdaParser = kR13 + 0x5A88u;
constexpr u32 kSdaAnsiHeader = kR13 + 0x5A80u, kSdaSjisHeader = kR13 + 0x5A84u;
constexpr u32 kEncoderRegister = 0xCC00206Eu;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// 0xCC002000 / 0x6E: the VI's DTV / encoder status the original reads (bit 1: Japanese encoder).
class EncoderStatus final : public MmioDevice {
public:
    u16 value = 0;
    unsigned reads = 0;
    const char* name() const override { return "video encoder status fixture"; }
    u16 read16(u32 address) override {
        require(address == kEncoderRegister, "the original read a video register other than 0xCC00206E");
        ++reads;
        return value;
    }
};

struct Oracle {
    GuestMemory memory;
    Cpu cpu{memory};
    EncoderStatus encoder;
    u64 instructions = 0;

    explicit Oracle(const char* path) {
        std::ifstream input(path, std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        // SHA-256: fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af.
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
        memory.register_mmio(0xCC002000u, 0x100u, &encoder);
    }

    // Runs the original body at `entry` with r3 = r3 to its return; the stack must balance.
    void call(u32 entry, u32 r3 = 0) {
        cpu.reset();
        cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kReturn;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13; cpu.gpr[3] = r3;
        unsigned steps = 0;
        while (cpu.pc != kReturn && !cpu.stopped() && steps++ < 100000) { cpu.run_steps(1); ++instructions; }
        require(cpu.pc == kReturn && !cpu.stopped() && cpu.gpr[1] == kStack, "original call failed return/stack contract");
    }
    // The original OSGetFontEncode from a state: TV-format word, encoder status, cache.
    u32 original_encode(u32 tv_format, u16 status, u16 cache) {
        memory.write32(os_font::kTvFormatWord, tv_format);
        encoder.value = status;
        memory.write16(kSdaEncodeCache, cache);
        memory.write32(kSdaParser, 0);
        call(os_font::kGetFontEncode);
        return cpu.gpr[3];
    }
    u16 peek16(u32 address) { return memory.peek16(address); }
};

// What OSGetFontEncode must answer for every TV format and encoder status (cache unset), against the original.
void owned_encode_equals_original(Oracle& o, const std::function<u16(u32, u16)>& native, const char* what) {
    const u32 formats[] = {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 0x7Fu, 0x80000000u, 0xFFFFFFFFu, 0x00010000u};
    const u16 statuses[] = {0x0000u, 0x0001u, 0x0002u, 0x0003u, 0x0004u, 0x0080u, 0x8000u, 0xFFFDu, 0xFFFFu};
    for (u32 format : formats)
        for (u16 status : statuses) {
            const u32 expected = o.original_encode(format, status, 0xFFFFu);
            const u32 actual = native(format, status);
            if (expected != actual) {
                std::fprintf(stderr, "%s: tv=%08X status=%04X original=%u native=%u\n", what, format, status, expected,
                             actual);
                throw std::runtime_error(std::string(what) + ": the native encode differs from the original");
            }
        }
}

void encode_contract(Oracle& o) {
    owned_encode_equals_original(o, [](u32 f, u16 s) { return os_font::font_encode(f, s); }, "font_encode");

    // What the original does besides returning: it reads the TV word and (for NTSC, cache unset) the encoder status
    // once, caches the answer, sets the parser slot, and touches no other SDA slot of the font library.
    o.memory.write32(os_font::kTvFormatWord, 0u);
    o.encoder.value = 0x0002u; o.encoder.reads = 0;
    o.memory.write16(kSdaEncodeCache, 0xFFFFu);
    o.call(os_font::kGetFontEncode);
    require(o.cpu.gpr[3] == 1u && o.encoder.reads == 1, "NTSC + Japanese encoder: SJIS, one register read");
    require(o.peek16(kSdaEncodeCache) == 1u, "the original caches the answer in sda+0x2708");
    require(o.memory.peek32(kSdaParser) == 0x80375618u, "the original sets the string parser pointer (sda+0x5A88)");
    require(o.memory.peek32(kSdaAnsiHeader) == 0u && o.memory.peek32(kSdaSjisHeader) == 0u,
            "OSGetFontEncode does not touch the font header slots");
    // A hit in the cache answers without the register: the cache is written only by this function, so under a TV word
    // and an encoder status that never change (the runtime's) the cached and the recomputed answers are the same.
    o.encoder.reads = 0;
    o.call(os_font::kGetFontEncode);
    require(o.cpu.gpr[3] == 1u && o.encoder.reads == 0, "a cached answer must not read the register again");
    // PAL (non-zero TV word) never reads the chip, whatever the encoder reports.
    o.encoder.reads = 0;
    require(o.original_encode(1u, 0x0002u, 0xFFFFu) == 0u && o.encoder.reads == 0, "PAL: ANSI without a register read");

    // The runtime's environment (encoder status 0): the original answers 0 for every TV format, as the owner does.
    for (u32 format : {0u, 1u, 2u, 5u, 0xFFFFFFFFu}) {
        require(o.original_encode(format, os_font::kEncoderStatus, 0xFFFFu) == os_font::kEncodeAnsi,
                "the original with no Japanese encoder must answer ANSI");
        o.memory.write32(os_font::kTvFormatWord, format);
        o.cpu.reset(); o.cpu.gpr[3] = 0xDEADBEEFu;
        o.memory.write32(0x800000CCu, format);
        os_font::serve_get_font_encode(o.cpu);
        require(o.cpu.gpr[3] == os_font::kEncodeAnsi, "the owner must answer ANSI with no Japanese encoder");
    }
}

// The negative control: a wrong native encode (SJIS whenever the chip says Japanese, ignoring the TV word) must be
// caught by the very comparison above.
void negative_control(Oracle& o) {
    bool caught = false;
    try {
        owned_encode_equals_original(o, [](u32, u16 s) { return u16((s & 2u) ? 1u : 0u); }, "control");
    } catch (const std::exception&) {
        caught = true;
    }
    require(caught, "negative control: the comparison did not catch an encode that ignores the TV format");
}

// The never-entered owners stop the machine with the entry's name; the encode owner returns to its caller.
void installed_owners(Oracle& o) {
    struct Result { bool stopped; std::string detail; u32 pc, r3; };
    const auto run = [&](u32 entry, u32 tv_format) {
        GuestMemory memory;
        Cpu cpu(memory);
        os_font::install(cpu);
        memory.write32(os_font::kTvFormatWord, tv_format);
        cpu.reset();
        cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kReturn; cpu.gpr[3] = 0x1234u;
        cpu.run_steps(1);
        return Result{cpu.stopped(), cpu.stopped() ? cpu.stop().detail : std::string(), cpu.pc, cpu.gpr[3]};
    };
    for (u32 format : {0u, 1u, 5u}) {
        const Result r = run(os_font::kGetFontEncode, format);
        require(!r.stopped && r.pc == kReturn && r.r3 == 0u, "the installed OSGetFontEncode owner must return 0 to its caller");
    }
    const Result init = run(os_font::kInitFont, 0u);
    require(init.stopped && init.detail.find("OSInitFont") != std::string::npos, "OSInitFont must stop the run by name");
    const Result texture = run(os_font::kGetFontTexture, 0u);
    require(texture.stopped && texture.detail.find("OSGetFontTexture") != std::string::npos,
            "OSGetFontTexture must stop the run by name");
    (void)o;
}

// The dependency that makes "never entered" honest: the original OSInitFont fills its buffers through __OSReadROM
// 0x80377C7C (4 ReadFont calls), which the program refuses; and OSGetFontTexture reads headers only OSInitFont writes.
void rom_dependency(Oracle& o) {
    const auto word = [&](u32 address) { return o.memory.peek32(address); };
    bool reaches_read_font = false, reaches_rom = false;
    for (u32 a = os_font::kInitFont; a < os_font::kInitFont + 0x20C; a += 4) {
        const u32 w = word(a);
        if ((w >> 26) == 18 && (w & 1u) != 0u) {  // bl
            s32 offset = s32(w & 0x03FFFFFCu); if (offset & 0x02000000) offset |= ~0x03FFFFFF;
            if (a + u32(offset) == 0x803751B4u) reaches_read_font = true;
        }
    }
    for (u32 a = 0x803751B4u; a < 0x803751B4u + 804u; a += 4) {
        const u32 w = word(a);
        if ((w >> 26) == 18 && (w & 1u) != 0u) {
            s32 offset = s32(w & 0x03FFFFFCu); if (offset & 0x02000000) offset |= ~0x03FFFFFF;
            if (a + u32(offset) == 0x80377C7Cu) reaches_rom = true;
        }
    }
    require(reaches_read_font && reaches_rom, "OSInitFont -> ReadFont -> __OSReadROM is the original's only font source");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: os_font_contract_tests main.dol");
        Oracle original(argv[1]);
        encode_contract(original);
        negative_control(original);
        installed_owners(original);
        rom_dependency(original);
        std::printf("PASS os font contract: %llu original instructions\n",
                    static_cast<unsigned long long>(original.instructions));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL os font contract: %s\n", error.what());
        return 1;
    }
}
