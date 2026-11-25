// liveness-roots: reference-interpreter-only
// Contract for the GCAX dB -> amplitude search 0x8043C3F0, owned in gekko/native_sound_ampdb.h. The ORIGINAL words run in the
// reference interpreter from the pinned DOL; the native owner runs on an identical machine.
//   Static: the table at 0x8056C150 (0x401 x {amp, dB}) is non-decreasing in dB and NaN-free, and no lis+addi/ori pair of the
//           code and no data word of the image names an address inside it except the routine's own pair (read-only proof).
//   Edge:   +-0, +-Inf, NaN (both signs, quiet and signalling, several payloads), denormals, +-FLT_MAX, values beyond both ends,
//           every table key and its two neighbours either side, midpoints, non-single double images.
//   Fuzz:   random f32 bit patterns and random doubles. Real: the inputs of a call census csv (--census <csv>).
//   Sweep:  the owner search against the linear definition over the single bit patterns (strided by default, every one of the 2^32 with --sweep);
//           the host model itself is checked against the interpreter on every input above.
// Compared after each call: r0, r1, r3, r4, r5, r13..r31, LR, CTR, all of CR, every FPR (both slots), FPSCR, XER.
// Negative controls (the run must FAIL): --control-offbyone (the original's index step 8 -> 4), --control-nanmatch (the original's
// ble becomes bge: a NaN counts as a match), --control-table (one dB key changed in the original), --control-model (the host
// model's > turned into >=, caught by the model-vs-interpreter check).
// usage: sound_ampdb_contract_tests <sys/main.dol> [--control-...] [--census <csv>] [--sweep]
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_sound_ampdb.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
using namespace gekko::sound_ampdb;
constexpr u32 kStack = 0x817E0000u, kReturn = 0x80100000u;

void require(bool c, const std::string& m) {
    if (!c) throw std::runtime_error(m);
}
u32 g_state = 24681357u;
u32 rnd() {
    g_state = g_state * 1664525u + 1013904223u;
    return g_state ^ (g_state >> 15);
}
u64 rnd64() { return (u64{rnd()} << 32) | rnd(); }

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
            memory.write_block(address, dol.data() + offset, size);
        }
        cpu.reset();
    }
};

float as_float(u32 w) {
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

unsigned g_calls = 0;
// One call on both machines (identical memory); returns the index (r3 / 8) the original produced.
u32 run_pair(Machine& o, Machine& n, u64 f1_bits, const std::string& at) {
    const u32 seed = rnd();
    const u32 cr = rnd(), fpscr_bits = rnd(), xer = rnd() & 0xE000007Fu;
    for (Machine* m : {&o, &n}) {
        Cpu& c = m->cpu;
        u32 x = seed | 1u;
        for (u32 i = 0; i < 32u; ++i) {
            x = x * 1664525u + 1013904223u;
            c.gpr[i] = x;
            c.fpr[i].bits[0] = (u64{x} << 32) | (x * 2654435761u);
            c.fpr[i].bits[1] = (u64{x ^ 0xA5A5A5A5u} << 32) | x;
        }
        c.gpr[1] = kStack;
        c.gpr[2] = 0x805FA780u;
        c.gpr[13] = 0x805EC500u;
        c.fpr[1].bits[0] = f1_bits;
        c.lr = kReturn;
        c.ctr = 0x77777u;
        c.cr = cr;
        // Random FPSCR status (sticky exception bits, FPCC, rounding mode, and the VE enable on a few).
        c.fpscr = (fpscr_bits & 0xFFFFF0FFu) | ((fpscr_bits % 8u == 0u) ? 0x80u : 0u);
        c.xer = xer;
    }
    Cpu& c = o.cpu;
    c.pc = kSearch;
    c.npc = kSearch + 4u;
    for (unsigned long steps = 0; steps < 20000ul && c.pc != kReturn && !c.stopped(); ++steps) c.run_steps(1u);
    require(c.pc == kReturn, "the original did not return" + at);
    Cpu& d = n.cpu;
    require(serve_search(d), "the owner declined" + at);
    ++g_calls;
    for (u32 i : {0u, 1u, 3u, 4u, 5u})
        require(c.gpr[i] == d.gpr[i], "r" + std::to_string(i) + " differs: original " + std::to_string(c.gpr[i]) + " native " + std::to_string(d.gpr[i]) + at);
    for (u32 i = 13; i < 32u; ++i) require(c.gpr[i] == d.gpr[i], "r" + std::to_string(i) + " differs" + at);
    require(c.lr == d.lr, "LR differs" + at);
    require(c.ctr == d.ctr, "CTR differs: original " + std::to_string(c.ctr) + " native " + std::to_string(d.ctr) + at);
    require(c.cr == d.cr, "CR differs: original " + std::to_string(c.cr) + " native " + std::to_string(d.cr) + at);
    for (u32 i = 0; i < 32u; ++i)
        require(c.fpr[i].bits[0] == d.fpr[i].bits[0] && c.fpr[i].bits[1] == d.fpr[i].bits[1], "f" + std::to_string(i) + " differs" + at);
    require(c.fpscr == d.fpscr, "FPSCR differs: original " + std::to_string(c.fpscr) + " native " + std::to_string(d.fpscr) + at);
    require(c.xer == d.xer, "XER differs" + at);
    return c.gpr[3] / 8u;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        set_enabled(true);  // the owner is off by default in the library
        require(argc >= 2, "usage: sound_ampdb_contract_tests main.dol [--control-...] [--census csv] [--sweep]");
        std::string control, census;
        bool sweep = false;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--census" && i + 1 < argc) census = argv[++i];
            else if (a == "--sweep") sweep = true;
            else control = a;
        }
        std::ifstream input(argv[1], std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        u64 hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        Machine o(dol), n(dol);
        require(o.memory.read32(kSearch) == 0x3C608057u && o.memory.read32(kSearch + 0x38u) == 0x4E800020u, "unexpected routine words");

        // ---- the table (static proof) ----
        std::vector<u32> db(kEntries);
        for (u32 i = 0; i < kEntries; ++i) db[i] = o.memory.read32(kTable + 8u * i + 4u);
        for (u32 i = 0; i < kEntries; ++i) {
            require(as_float(db[i]) == as_float(db[i]), "NaN key");
            if (i != 0u) require(as_float(db[i - 1u]) <= as_float(db[i]), "the dB keys are not non-decreasing at " + std::to_string(i));
        }
        require(as_float(db[0]) == -96.0f && as_float(db[kEntries - 1u]) == 0.0f, "unexpected table ends");
        {
            unsigned code_hits = 0, data_hits = 0;
            const u32 lo = kTable, hi = kTable + 8u * kEntries;
            const auto w = [&](std::size_t off) {
                return u32{dol[off]} << 24 | u32{dol[off + 1]} << 16 | u32{dol[off + 2]} << 8 | u32{dol[off + 3]};
            };
            for (u32 i = 0; i < 18u; ++i) {
                const u32 offset = w(i * 4u), size = w(0x90u + i * 4u);
                if (size == 0u) continue;
                for (u32 a = 0; a + 4u <= size; a += 4u) {
                    const u32 word = w(offset + a);
                    if (i < 7u) {
                        if ((word >> 26) == 15u) {  // lis rD, hi: look at the next four instructions for an addi / ori on rD
                            for (u32 k = 1; k <= 4u && a + 4u * (k + 1u) <= size; ++k) {
                                const u32 next = w(offset + a + 4u * k);
                                const u32 op = next >> 26;
                                if ((op == 14u || op == 24u || (op >= 32u && op <= 55u)) && ((word >> 21) & 31u) == (op == 24u ? ((next >> 21) & 31u) : ((next >> 16) & 31u))) {  // addi / ori / a D-form load or store
                                    const u32 hi16 = (word & 0xFFFFu) << 16;
                                    const u32 lo16 = next & 0xFFFFu;
                                    const u32 value = op != 24u ? hi16 + static_cast<u32>(static_cast<std::int32_t>(static_cast<std::int16_t>(lo16))) : (hi16 | lo16);
                                    if (value >= lo && value < hi) ++code_hits;
                                }
                            }
                        }
                    } else if (word >= lo && word < hi) {
                        ++data_hits;
                    }
                }
            }
            require(code_hits == 1u, "the table is named by " + std::to_string(code_hits) + " lis+addi pairs, expected exactly the routine's");
            require(data_hits == 0u, "a data word points into the table");
        }

        // Controls on the ORIGINAL words / the model.
        if (control == "--control-offbyone") o.memory.write32(kSearch + 0x28u, 0x38630004u);       // addi r3, r3, 4
        else if (control == "--control-nanmatch") o.memory.write32(kSearch + 0x18u, 0x4080000Cu);  // bge instead of ble
        else if (control == "--control-table") o.memory.write32(kTable + 8u * 300u + 4u, 0xC2000000u);
        else require(control.empty() || control == "--control-model", "unknown control " + control);

        // ---- the inputs ----
        std::vector<u64> inputs;
        const auto add32 = [&](u32 word) { inputs.push_back(single_to_double_bits(word)); };
        const u32 specials[] = {0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0xFF800001u,
                                0x7FA00000u, 0x7FFFFFFFu, 0xFFFFFFFFu, 0x7FC12345u, 0xFFA54321u, 0x00000001u, 0x80000001u, 0x007FFFFFu,
                                0x807FFFFFu, 0x00800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x3F800000u, 0xBF800000u, 0xC2C00000u, 0xC2C00001u,
                                0xC2BFFFFFu, 0x42C00000u, 0xC3000000u};
        for (u32 word : specials) add32(word);
        for (u32 i = 0; i < kEntries; ++i)
            for (int d = -2; d <= 2; ++d) add32(db[i] + static_cast<u32>(d));
        for (u32 i = 0; i + 1u < kEntries; i += 7u) {
            const float mid = (as_float(db[i]) + as_float(db[i + 1u])) * 0.5f;
            u32 word;
            std::memcpy(&word, &mid, 4);
            add32(word);
        }
        for (u32 i = 0; i < kEntries; i += 5u) {  // non-single doubles next to a key
            const double v = static_cast<double>(as_float(db[i]));
            u64 b;
            std::memcpy(&b, &v, 8);
            inputs.push_back(b + 1u);
            inputs.push_back(b - 1u);
        }
        inputs.push_back(0x7FF0000000000001ull);
        inputs.push_back(0xFFF8000000000000ull);
        inputs.push_back(0x7FF8000000000123ull);
        for (u32 i = 0; i < 6000u; ++i) add32(rnd());
        for (u32 i = 0; i < 2000u; ++i) inputs.push_back(rnd64());
        for (u32 i = 0; i < 2000u; ++i) {
            const float f = -96.5f + static_cast<float>(rnd() % 100000u) * 0.00100f;
            u32 word;
            std::memcpy(&word, &f, 4);
            add32(word);
        }
        std::size_t real = 0;
        if (!census.empty()) {
            std::ifstream csv(census);
            require(static_cast<bool>(csv), "cannot read the census csv");
            std::string line;
            std::getline(csv, line);
            while (std::getline(csv, line)) {
                // address,game_ticks,r3,r4,lr,f1,r3_word : f1 is the 16-hex-digit field after the fifth comma
                std::size_t p = 0;
                for (int k = 0; k < 5; ++k) p = line.find(',', p) + 1u;
                inputs.push_back(std::strtoull(line.substr(p, 16).c_str(), nullptr, 16));
                ++real;
                if (real >= 20000u && !sweep) break;  // default size: the first 20,000 real inputs; --sweep takes all
            }
        }

        // ---- original vs owner, and the host model vs the interpreter ----
        const bool model_ge = control == "--control-model";
        const auto model = [&](u64 f1) {
            const double x = bits_to_double(f1);
            for (u32 i = 0; i < kEntries; ++i) {
                const double k = static_cast<double>(as_float(db[i]));
                if (model_ge ? k >= x : k > x) return i;
            }
            return kEntries;
        };
        unsigned hits = 0, none = 0, nans = 0;
        for (std::size_t k = 0; k < inputs.size(); ++k) {
            const u64 f1 = inputs[k];
            const std::string at = " (input " + std::to_string(k) + ", f1 bits " + std::to_string(f1) + ")";
            const u32 index = run_pair(o, n, f1, at);
            const u32 expected = model(f1);
            require(index == expected, "host model differs from the interpreter: interpreter " + std::to_string(index) + " model " + std::to_string(expected) + at);
            require(scan_index_binary(db.data(), f1) == expected && scan_index_linear(db.data(), f1) == expected, "owner search differs from the model" + at);
            (index >= kEntries ? none : hits)++;
            if ((f1 & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (f1 & 0xFFFFFFFFFFFFFull) != 0u) ++nans;
        }
        std::printf("calls %u (real census inputs %zu), found %u, none %u, NaN inputs %u\n", g_calls, real, hits, none, nans);

        // ---- the sweep over single bit patterns: owner search vs the linear definition ----
        // The linear definition is "the first entry whose dB exceeds x". For non-NaN x in ascending order that index never
        // decreases (the keys are non-decreasing: checked above), so the model advances one pointer through the table while the
        // sweep walks every non-NaN single in ascending value order (negatives by decreasing magnitude, then positives): the same
        // definition, amortised O(1) per pattern, which makes the exhaustive 2^32 pass feasible. Every 4093rd pattern is also
        // checked against the plain from-zero linear scan, so the pointer model itself is tied to the definition. NaN patterns
        // must give "none". The default takes a stride of 2^12 patterns; --sweep takes all of them.
        const u32 stride = sweep ? 1u : (1u << 12);
        u64 swept = 0, probes = 0;
        u32 pointer = 0;
        const auto step = [&](u32 pattern) {
            const u64 f1 = single_to_double_bits(pattern);
            const double x = bits_to_double(f1);
            while (pointer < kEntries && !(static_cast<double>(as_float(db[pointer])) > x)) ++pointer;
            const u32 got = scan_index_binary(db.data(), f1);
            require(got == pointer, "sweep: search " + std::to_string(got) + " differs from the scan " + std::to_string(pointer) + " at word " + std::to_string(pattern));
            if (swept % 4093u == 0u) {
                require(scan_index_linear(db.data(), f1) == pointer, "sweep: pointer model differs from the linear scan at word " + std::to_string(pattern));
                ++probes;
            }
            ++swept;
        };
        for (u32 mag = 0x7F800000u;; mag -= stride) {  // negative singles: ascending value = decreasing magnitude, down to -0
            step(mag | 0x80000000u);
            if (mag < stride) break;
        }
        for (u32 mag = 0; mag <= 0x7F800000u; mag += stride) step(mag);
        for (u32 mag = 0x7F800001u; mag < 0x80000000u; mag += (sweep ? 1u : 977u)) {
            for (u32 sign : {0u, 0x80000000u}) {
                const u64 f1 = single_to_double_bits(mag | sign);
                require(scan_index_binary(db.data(), f1) == kEntries && scan_index_linear(db.data(), f1) == kEntries, "sweep: a NaN pattern is not 'none'");
                ++swept;
            }
        }
        std::printf("sweep: %llu single bit patterns%s agree (%llu tied to the from-zero linear scan)\n", static_cast<unsigned long long>(swept),
                    sweep ? " (all 2^32)" : " (strided)", static_cast<unsigned long long>(probes));
        std::printf("sound_ampdb_contract_tests: OK\n");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
