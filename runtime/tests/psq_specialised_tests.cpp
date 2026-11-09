// liveness-roots: reference-interpreter-only
//
// R16 (EV, 2026-10-20): ppc::psq_load_k<K> / psq_store_k<K> (the guarded paired-single specialisations ppcrecomp
// --psq-sites emits) against the generic ppc::psq_load / psq_store they replace when c.spr[GQRi] == K.
// Two machines start from the same memory and the same FPR file; one runs the specialisation, the other the generic
// helper; equal means every FPR (both halves), every byte of the arena, and the access trace's sequence of
// (address, size, write) when a trace is installed.
// Covered, for 30 values of K (load and store types 0, 4, 5, 6, 7 x scales 0, 1, 7, 31, 32, 63):
//   * W = 0 and W = 1;
//   * addresses at every offset 0..7 into a word, and the last bytes of a host page (the pair crosses the page:
//     the specialisation must fall back to the generic helper and still match);
//   * FPR values: random bit patterns (NaNs, infinities, denormals, signalling NaNs), exact integers, values just
//     inside and outside every saturation bound, negative zero, halves (round-to-nearest-even through the casts);
//   * with an access trace installed (the page table then serves nothing: every access takes the generic path).
//
// usage: psq_specialised_tests

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/ppc_ops.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using gekko::u32;
using gekko::u64;
using gekko::u8;

constexpr u32 kArena = 0x80400000u;
constexpr u32 kArenaSize = 3u * gekko::Cpu::kHostPageSize;
constexpr u32 kGqrIndex = 4u;

int g_failures = 0;
long g_checks = 0;
void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        if (g_failures <= 40) std::printf("FAIL: %s\n", what.c_str());
    }
}

struct Machine {
    gekko::GuestMemory bus;
    gekko::Cpu cpu{bus};
    std::vector<std::tuple<u32, std::size_t, bool>> trace;
    bool traced = false;

    // Once per K and trace mode: a fresh CPU, the arena, translation on, GQR4 = K.
    void prepare(u32 k, const std::vector<u8>& arena, bool with_trace) {
        cpu.reset();
        cpu.attach_recompiled(nullptr);
        bus.set_access_trace({});
        bus.write_block(kArena, arena.data(), arena.size());
        traced = with_trace;
        if (with_trace) {
            bus.set_access_trace([this](u32 address, std::size_t size, bool write) {
                if (traced) trace.emplace_back(address, size, write);
            });
        }
        cpu.set_msr(cpu.msr() | gekko::kMsrDR | gekko::kMsrIR | gekko::kMsrFP);
        for (u32 i = 0; i < 8u; ++i) cpu.spr[gekko::SPR_GQR0 + i] = 0u;
        cpu.spr[gekko::SPR_GQR0 + kGqrIndex] = k;
    }
    // Per case: the 24-byte window around the access and the FPR pair, set identically on both machines.
    void set_case(u32 window, const std::array<u8, 24>& bytes, const std::array<u64, 2>& fpr) {
        const bool was = traced;
        traced = false;  // the test's own set-up and read-back are not part of the compared trace
        for (u32 i = 0; i < 24u; ++i) bus.write8(window + i, bytes[i]);
        traced = was;
        trace.clear();
        cpu.fpr[1].bits[0] = fpr[0];
        cpu.fpr[1].bits[1] = fpr[1];
    }
    std::array<u8, 24> window(u32 at) {
        const bool was = traced;
        traced = false;
        std::array<u8, 24> out{};
        for (u32 i = 0; i < 24u; ++i) out[i] = bus.read8(at + i);
        traced = was;
        return out;
    }
};

u64 bits(double value) {
    u64 out;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

std::vector<u64> fpr_values(std::mt19937_64& random) {
    std::vector<u64> out;
    const double specials[] = {0.0, -0.0, 0.5, 1.5, 2.5, -0.5, -1.5, 127.0, 127.5, 128.0, -128.0, -128.5, -129.0,
                               255.0, 255.5, 256.0, 32767.0, 32767.5, 32768.0, -32768.0, -32768.5, -32769.0,
                               65535.0, 65535.5, 65536.0, 1e30, -1e30, 3.0e-39, -3.0e-39, 1.0e-45,
                               std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN(), 0.25, 0.75, 1.0 / 3.0};
    for (double s : specials) out.push_back(bits(s));
    out.push_back(0x7FF0000000000001ull);  // signalling NaN
    out.push_back(0xFFF4000000000000ull);
    for (int i = 0; i < 40; ++i) out.push_back(random());
    for (int i = 0; i < 40; ++i) out.push_back(bits(static_cast<double>(static_cast<int>(random() % 140000) - 70000) / 2.0));
    return out;
}

template <u32 K>
void run_k(std::mt19937_64& random, const std::vector<u64>& values) {
    const u32 page = gekko::Cpu::kHostPageSize;
    std::vector<u32> offsets;
    for (u32 o = 0; o < 8u; ++o) offsets.push_back(page + o);
    for (u32 o = 1; o <= 8u; ++o) offsets.push_back(2u * page - o);  // pairs that cross into the next page
    std::vector<u8> arena(kArenaSize);
    for (auto& byte : arena) byte = static_cast<u8>(random());
    for (const bool traced : {false, true}) {
        Machine a, b;  // a: specialisation, b: generic
        a.prepare(K, arena, traced);
        b.prepare(K, arena, traced);
        for (const bool w : {false, true}) {
            for (const u32 offset : offsets) {
                const u32 address = kArena + offset;
                const u32 window = address - 8u;
                for (std::size_t v = 0; v < values.size(); ++v) {
                    std::array<u8, 24> bytes{};
                    for (auto& byte : bytes) byte = static_cast<u8>(random());
                    const std::array<u64, 2> fpr{values[v], values[(v * 7 + 3) % values.size()]};
                    char what[160];
                    std::snprintf(what, sizeof what, "K=%08X w=%d off=%u traced=%d fpr=%016llX/%016llX", K, int(w),
                                  offset, int(traced), static_cast<unsigned long long>(fpr[0]),
                                  static_cast<unsigned long long>(fpr[1]));
                    a.set_case(window, bytes, fpr);
                    b.set_case(window, bytes, fpr);
                    gekko::ppc::psq_store_k<K>(a.cpu, 1u, address, w, kGqrIndex);
                    gekko::ppc::psq_store(b.cpu, 1u, address, w, kGqrIndex);
                    check(a.trace == b.trace, std::string("store ") + what + " access trace");
                    check(a.window(window) == b.window(window), std::string("store ") + what + " memory");
                    a.set_case(window, bytes, fpr);
                    b.set_case(window, bytes, fpr);
                    gekko::ppc::psq_load_k<K>(a.cpu, 1u, address, w, kGqrIndex);
                    gekko::ppc::psq_load(b.cpu, 1u, address, w, kGqrIndex);
                    check(a.trace == b.trace, std::string("load ") + what + " access trace");
                    check(a.cpu.fpr[1].bits[0] == b.cpu.fpr[1].bits[0] &&
                              a.cpu.fpr[1].bits[1] == b.cpu.fpr[1].bits[1],
                          std::string("load ") + what + " fpr");
                }
            }
        }
    }
}

constexpr u32 make_k(u32 type, u32 scale) { return type | (scale << 8) | (type << 16) | (scale << 24); }

template <std::size_t... I>
void run_all(std::mt19937_64& random, const std::vector<u64>& values, std::index_sequence<I...>) {
    constexpr u32 kTypes[] = {0u, 4u, 5u, 6u, 7u};
    constexpr u32 kScales[] = {0u, 1u, 7u, 31u, 32u, 63u};
    (run_k<make_k(kTypes[I / 6], kScales[I % 6])>(random, values), ...);
}
}  // namespace

int main() {
    std::mt19937_64 random(20261020u);
    const std::vector<u64> values = fpr_values(random);
    run_all(random, values, std::make_index_sequence<30>{});
    std::printf("psq_specialised_tests: %ld checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
