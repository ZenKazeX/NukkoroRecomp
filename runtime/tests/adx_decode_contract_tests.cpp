// liveness-roots: reference-interpreter-only
// Contract for the ADX stereo 4-bit block decoder 0x803C5B2C, owned in gekko/native_adx_decode.h. The ORIGINAL words run in the
// reference interpreter from the pinned DOL; the native owner runs on an identical machine.
//   Fuzz: random block pairs (scale words with the stop bit 0x8000 at random places, extreme prediction coefficients,
//         histories and keys), n = 0 / 1 / negative / odd / up to 64, and overlapping buffers (the source under an output,
//         both outputs the same, a history under an output, the key inside a buffer).
//   Real: frames read from the game's own .adx files (stereo, 18-byte blocks, two windows per file), decoded in calls of
//         two block pairs with the history carried between calls; original and native are compared after every call and
//         a PCM hash of the native output is printed.
// Compared after each call: r1, r3, r13..r31, LR, CTR, CR2..CR4 (the callee-saved CR fields), XER (srawi sets CA: it is saved in thread contexts), every FPR (neither touches
// them), FPSCR, and every byte of the 0x2000-byte arena plus the 0x100 bytes at the stack pointer (the stack arguments) and the 0x40 bytes below it.
// Also compared: the 0x40 bytes below SP (the original's back chain and stmw r19..r31 image): the owner writes them, so guest
// RAM (state.json hashes it all) is byte-identical to a build without the owner.
// NOT compared (volatile or dead by the ABI): r0, r4..r12, cr0/cr1/cr5..7.
// Negative controls (the run must FAIL): --control-shift (the original's prediction shift 12 -> 11), --control-keymask
// (the key mask 0x7FFF -> 0xFFFF), --control-table (one nibble table word changed in the original), --control-frame (the original's stmw offset moved:
// the dead-frame comparison must catch it).
// usage: adx_decode_contract_tests <sys/main.dol> <game files dir> [--control-shift | --control-keymask | --control-table | --control-frame]
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_adx_decode.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
using namespace gekko::adx_decode;
constexpr u32 kArena = 0x80700000u, kArenaSize = 0x2000u, kStack = 0x817E0000u, kReturn = 0x80100000u;
constexpr u32 kSrcAt = kArena + 0x100u, kOut0At = kArena + 0x600u, kOut1At = kArena + 0xE00u, kHist0At = kArena + 0x1800u,
              kHist1At = kArena + 0x1810u, kKeyAt = kArena + 0x1820u;
constexpr u32 kPairs = 32u;  // block pairs held in the source area

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

u32 g_state = 987654321u;
u32 rnd() {
    g_state = g_state * 1664525u + 1013904223u;
    return g_state ^ (g_state >> 15);
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
        cpu.reset();
    }
    u64 hash() {
        u64 h = 0xCBF29CE484222325ull;
        std::vector<u8> block(kArenaSize);
        memory.read_block(kArena, block.data(), block.size());
        for (u8 b : block) h = (h ^ b) * 0x100000001B3ull;
        std::vector<u8> args(0x100);
        memory.read_block(kStack, args.data(), args.size());
        for (u8 b : args) h = (h ^ b) * 0x100000001B3ull;
        // The dead frame below the caller's SP: the back chain and the stmw r19..r31 image at SP-0x40..SP-1.
        std::vector<u8> frame(0x40);
        memory.read_block(kStack - 0x40u, frame.data(), frame.size());
        for (u8 b : frame) h = (h ^ b) * 0x100000001B3ull;
        return h;
    }
};

struct Call {
    u32 r3, r4, r5, r6, r7, r8, r9, r10;       // registers
    u32 key_ptr, key_word_mult, key_word_add;  // the stack words at +8, +0xC, +0x10
};

// Runs one call on both machines (their memory is already identical), compares, returns the original's r3.
u32 run_pair(Machine& o, Machine& n, const Call& k, const std::string& at) {
    const u32 xer_seed = rnd() & 0xE000007Fu;  // SO / OV / CA and the byte count, all random
    for (Machine* m : {&o, &n}) {
        Cpu& c = m->cpu;
        u32 x = 0x5EEDu;
        for (u32 i = 0; i < 32u; ++i) {
            x = x * 1664525u + 1013904223u;
            c.gpr[i] = x;
            c.fpr[i].bits[0] = (u64{x} << 32) | (x * 2654435761u);
            c.fpr[i].bits[1] = (u64{x ^ 0xA5A5A5A5u} << 32) | x;
        }
        c.gpr[1] = kStack;
        c.gpr[2] = 0x805F0000u;
        c.gpr[13] = 0x805EC500u;
        c.gpr[3] = k.r3; c.gpr[4] = k.r4; c.gpr[5] = k.r5; c.gpr[6] = k.r6;
        c.gpr[7] = k.r7; c.gpr[8] = k.r8; c.gpr[9] = k.r9; c.gpr[10] = k.r10;
        c.lr = kReturn;
        c.ctr = 0x77777u;
        c.cr = 0x12345678u;
        c.fpscr = 0;
        c.xer = xer_seed;
        m->memory.write32(kStack + 8u, k.key_ptr);
        m->memory.write32(kStack + 0xCu, k.key_word_mult);
        m->memory.write32(kStack + 0x10u, k.key_word_add);
    }
    Cpu& c = o.cpu;
    c.pc = kDecodeSte4;
    c.npc = kDecodeSte4 + 4u;
    for (unsigned long steps = 0; steps < 4000000ul && c.pc != kReturn && !c.stopped(); ++steps) c.run_steps(1u);
    require(c.pc == kReturn, "the original did not return" + at);
    Cpu& d = n.cpu;
    serve_decode_ste4(d);
    require(c.gpr[1] == d.gpr[1], "r1 differs" + at);
    require(c.gpr[3] == d.gpr[3],
            "return value differs: original " + std::to_string(c.gpr[3]) + " native " + std::to_string(d.gpr[3]) + at);
    for (u32 i = 13; i < 32u; ++i) require(c.gpr[i] == d.gpr[i], "r" + std::to_string(i) + " differs" + at);
    require(c.lr == d.lr, "LR differs" + at);
    require(c.ctr == d.ctr, "CTR differs: original " + std::to_string(c.ctr) + " native " + std::to_string(d.ctr) + at);
    require((c.cr & 0x00FFF000u) == (d.cr & 0x00FFF000u), "CR2..CR4 differ" + at);
    for (u32 i = 0; i < 32u; ++i)
        require(c.fpr[i].bits[0] == d.fpr[i].bits[0] && c.fpr[i].bits[1] == d.fpr[i].bits[1], "f" + std::to_string(i) + " differs" + at);
    require(c.fpscr == d.fpscr, "FPSCR differs" + at);
    require(c.xer == d.xer, "XER differs: original " + std::to_string(c.xer) + " native " + std::to_string(d.xer) + at);
    require(o.hash() == n.hash(), "memory differs" + at);
    return c.gpr[3];
}

u32 be32(const std::vector<u8>& b, std::size_t o) { return u32{b[o]} << 24 | u32{b[o + 1]} << 16 | u32{b[o + 2]} << 8 | b[o + 3]; }
u32 be16(const std::vector<u8>& b, std::size_t o) { return u32{b[o]} << 8 | b[o + 1]; }
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc >= 3, "usage: adx_decode_contract_tests main.dol <game files dir> [--control-...]");
        const std::string control = argc >= 4 ? argv[3] : "";
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        Machine o(dol), n(dol);
        require(o.memory.read32(kDecodeSte4) == 0x9421FFC0u && o.memory.read32(kDecodeSte4 + 0x25Cu) == 0x4E800020u,
                "unexpected routine words");
        for (u32 i = 0; i < 16u; ++i)
            require(o.memory.read32(kNibbleTable + 4u * i) == static_cast<u32>(static_cast<s32>(static_cast<s8>(i << 4)) >> 4),
                    "the nibble table is not the sign-extended nibble at word " + std::to_string(i));
        if (control == "--control-shift") o.memory.write32(0x803C5C28u, 0x7E945E70u);          // srawi r20, r20, 11
        else if (control == "--control-keymask") o.memory.write32(0x803C5BACu, 0x5694043Eu);   // clrlwi r20, r20, 16
        else if (control == "--control-table") o.memory.write32(kNibbleTable + 4u * 5u, 0xFFFFFFFFu);
        else if (control == "--control-frame") o.memory.write32(0x803C5B3Cu, 0xBE610010u);       // stmw r19, 0x10(r1): the image moves 4 bytes
        else require(control.empty(), "unknown control " + control);

        unsigned runs = 0, stops = 0, overlapped = 0, tiny_runs = 0, clipped_runs = 0;
        // ---- fuzz ----
        for (u32 iteration = 0; iteration < 3000u; ++iteration) {
            // 0 plain; 1 source under out0; 2 out1 = out0; 3 hist0 under out0; 4 key inside out1; 5 hist0 = hist1; 6 and 7 plain
            u32 mode = iteration % 8u;
            s32 n_samples;
            switch (rnd() % 8u) {
            case 0: n_samples = static_cast<s32>(rnd() % 4u); break;
            case 1: n_samples = -static_cast<s32>(1u + rnd() % 5u); break;
            case 2: n_samples = static_cast<s32>(0x80000000u); break;
            case 3: n_samples = 0x7FFFFFFF; break;
            default: n_samples = static_cast<s32>(1u + rnd() % 64u); break;
            }
            std::vector<u32> arena(kArenaSize / 4u);
            for (u32& w : arena) w = rnd();
            const bool huge = n_samples == 0x7FFFFFFF;  // the loop would run off the arena: it must stop in the first pairs
            if (huge) mode = 0;                         // (an output over the source could clear the stop bit and run on)
            const u32 stop_pair = huge ? rnd() % 4u : (rnd() % 6u == 0u ? rnd() % 40u : 1000u);
            const u32 stop_channel = rnd() & 1u;
            for (u32 p = 0; p < kPairs; ++p)
                for (u32 ch = 0; ch < 2u; ++ch) {
                    const u32 at = (kSrcAt - kArena) + p * 0x24u + ch * 0x12u;  // an even offset, 2 or 0 mod 4
                    u32 w = rnd() & 0x7FFFu;
                    if (p == stop_pair && ch == stop_channel) w |= 0x8000u;
                    u32& word = arena[at / 4u];
                    word = (at & 2u) ? ((word & 0xFFFF0000u) | w) : ((word & 0x0000FFFFu) | (w << 16));
                }
            Call k{};
            k.r3 = kSrcAt; k.r4 = static_cast<u32>(n_samples); k.r5 = kOut0At; k.r6 = kHist0At; k.r7 = kOut1At; k.r8 = kHist1At;
            const u32 extremes[] = {0x7FFFu, 0x8000u, 0x0000u, 0xFFFFu, 0x1000u, 0x2000u};
            k.r9 = rnd() % 3u == 0u ? (rnd() & 0xFFFF0000u) | extremes[rnd() % 6u] : rnd();
            k.r10 = rnd() % 3u == 0u ? (rnd() & 0xFFFF0000u) | extremes[rnd() % 6u] : rnd();
            k.key_ptr = kKeyAt;
            k.key_word_mult = rnd();
            k.key_word_add = rnd();
            if (rnd() % 4u == 0u) {  // a key that stays small
                k.key_word_mult = (k.key_word_mult & 0xFFFF0000u) | (rnd() % 2u ? 0u : 1u);
                k.key_word_add &= 0xFFFF0000u;
            }
            if (mode == 1) k.r5 = kSrcAt + 4u * (rnd() % 16u);
            else if (mode == 2) k.r7 = k.r5;
            else if (mode == 3) k.r6 = k.r5 + 4u * (rnd() % 8u);
            else if (mode == 4) k.key_ptr = kOut1At + 2u * (rnd() % 16u);
            else if (mode == 5) k.r8 = k.r6;
            if (mode >= 1 && mode <= 5) ++overlapped;
            for (u32 h : {(kHist0At - kArena) / 4u, (kHist1At - kArena) / 4u, (kHist0At - kArena) / 4u + 1u, (kHist1At - kArena) / 4u + 1u})
                if (rnd() % 3u == 0u) arena[h] = rnd() % 2u ? 0x7FFF8000u : 0x80007FFFu;
            for (Machine* m : {&o, &n})
                for (u32 i = 0; i < arena.size(); ++i) m->memory.write32(kArena + 4u * i, arena[i]);
            const std::string at = " iteration " + std::to_string(iteration) + " mode " + std::to_string(mode) + " n " + std::to_string(n_samples);
            const u32 r = run_pair(o, n, k, at);
            ++runs;
            if (n_samples > 1 && r != static_cast<u32>(n_samples)) ++stops;
            if (n_samples <= 1) ++tiny_runs;
            if (n_samples > 1 && r == static_cast<u32>(n_samples) && mode == 0) {
                bool clip = false;
                for (s32 i = 0; i < (n_samples >> 1) * 32 && !clip; ++i) {
                    const u16 v = o.memory.read16(k.r5 + 2u * static_cast<u32>(i));
                    clip = v == 0x7FFFu || v == 0x8000u;
                }
                clipped_runs += clip;
            }
        }
        // ---- real ADX frames ----
        unsigned files = 0, real_calls = 0;
        u64 pcm_hash = 0xCBF29CE484222325ull;
        std::vector<std::filesystem::path> adx;
        for (const auto& e : std::filesystem::directory_iterator(argv[2]))
            if (e.path().extension().string() == ".adx") adx.push_back(e.path());
        std::sort(adx.begin(), adx.end());
        for (const auto& path : adx) {
            std::ifstream f(path, std::ios::binary);
            std::vector<u8> head(0x30);
            f.read(reinterpret_cast<char*>(head.data()), 0x30);
            if (!f || head[0] != 0x80 || head[1] != 0x00 || head[5] != 18 || head[6] != 4 || head[7] != 2) continue;  // stereo, 18-byte blocks
            const u32 data_at = be16(head, 2) + 4u, rate = be32(head, 8), cutoff = be16(head, 0x10);
            // the format's highpass coefficients, computed on the host (the original only receives them)
            const double sq2 = std::sqrt(2.0), a = sq2 - std::cos(2.0 * 3.14159265358979323846 * cutoff / rate), b = sq2 - 1.0;
            const double cc = (a - std::sqrt((a + b) * (a - b))) / (a - b);
            const s32 coef1 = static_cast<s32>(std::floor(cc * 8192.0)), coef2 = static_cast<s32>(std::floor(-cc * cc * 4096.0));
            const std::uint64_t size = std::filesystem::file_size(path);
            for (u32 window = 0; window < 2u; ++window) {
                f.clear();
                f.seekg(window == 0u ? data_at : data_at + ((size - data_at) / 2u / 0x24u) * 0x24u);
                std::vector<u8> bytes(0x24u * kPairs);
                f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                const u32 got = static_cast<u32>(f.gcount()) / 0x24u;
                if (got < 4u) continue;
                for (Machine* m : {&o, &n}) {
                    m->memory.write_block(kSrcAt, bytes.data(), got * 0x24u);
                    const std::vector<u8> zeros(0x800u, 0u);  // a window that meets an end-of-stream frame leaves part of the output untouched
                    m->memory.write_block(kOut0At, zeros.data(), zeros.size());
                    m->memory.write_block(kOut1At, zeros.data(), zeros.size());
                    m->memory.write32(kHist0At, 0u);
                    m->memory.write32(kHist1At, 0u);
                    m->memory.write32(kKeyAt, 0u);
                }
                u32 done = 0;
                for (; done + 2u <= got; done += 2u) {  // 64 samples per channel = two block pairs per call
                    Call k{};
                    k.r3 = kSrcAt + done * 0x24u; k.r4 = 64u;
                    k.r5 = kOut0At + done * 64u; k.r6 = kHist0At; k.r7 = kOut1At + done * 64u; k.r8 = kHist1At;
                    k.r9 = static_cast<u32>(coef1); k.r10 = static_cast<u32>(coef2);
                    k.key_ptr = kKeyAt; k.key_word_mult = 0u; k.key_word_add = 0u;
                    run_pair(o, n, k, " real file " + path.filename().string() + " window " + std::to_string(window) + " pair " + std::to_string(done));
                    ++real_calls;
                }
                std::vector<u8> pcm(done * 64u);
                for (u32 base : {kOut0At, kOut1At}) {
                    n.memory.read_block(base, pcm.data(), pcm.size());
                    for (u8 v : pcm) pcm_hash = (pcm_hash ^ v) * 0x100000001B3ull;
                }
            }
            if (++files >= 24u) break;
        }
        require(files >= 4u, "no real stereo ADX files found in " + std::string(argv[2]));
        std::printf("PASS adx decode: %u fuzz calls (%u overlapping-buffer, %u stopped on a scale stop bit, %u with n <= 1, %u with a "
                    "saturated sample) + %u real calls over %u game .adx files (native PCM hash %016llx): r1, r3, r13-r31, LR, CTR, "
                    "CR2-4, XER, f0-f31, FPSCR and the arena equal to the original\n",
                    runs, overlapped, stops, tiny_runs, clipped_runs, real_calls, files, static_cast<unsigned long long>(pcm_hash));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL adx decode contract: %s\n", error.what());
        return 1;
    }
}
