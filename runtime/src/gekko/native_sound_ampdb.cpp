// The native owner of the GCAX dB -> amplitude search 0x8043C3F0 (gekko/native_sound_ampdb.h).

#include "gekko/native_sound_ampdb.h"

#include "gekko/cpu.h"
#include "gekko/ppc_ops.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gekko::sound_ampdb {

namespace {
inline double single_value(u32 word) noexcept {
    float f;
    std::memcpy(&f, &word, 4);
    return static_cast<double>(f);
}
inline bool is_nan_double(u64 bits) noexcept { return (bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (bits & 0x000FFFFFFFFFFFFFull) != 0u; }

struct Column {
    bool built = false;
    bool usable = false;
    u32 db[kEntries];
    u32 amp[kEntries];
};
Column g_column;  // the owner is served on the game thread only (the table is immutable guest data)
u64 g_calls = 0, g_served = 0;  // GEKKO_OP3_STATS=1 prints them at exit
#ifdef SHADOW_OP3_AMPDB_DEFAULT
bool g_enabled = true;  // CMake SHADOW_OP3_AMPDB=ON; GEKKO_OP3_AMPDB=0 disables
#else
bool g_enabled = false;  // off by default (A/B pending); GEKKO_OP3_AMPDB=1 enables
#endif

void build(Cpu& c) {
    g_column.built = true;
    g_column.usable = true;
    for (u32 i = 0; i < kEntries; ++i) {
        g_column.amp[i] = c.mem_read32(kTable + 8u * i);
        g_column.db[i] = c.mem_read32(kTable + 8u * i + 4u);
        const double k = single_value(g_column.db[i]);
        if (k != k) g_column.usable = false;
        if (i != 0u && single_value(g_column.db[i - 1u]) > k) g_column.usable = false;
    }
}
}  // namespace

u32 scan_index_linear(const u32* db_bits, u64 f1_bits) noexcept {
    const double x = bits_to_double(f1_bits);
    for (u32 i = 0; i < kEntries; ++i)
        if (single_value(db_bits[i]) > x) return i;
    return kEntries;
}

u32 scan_index_binary(const u32* db_bits, u64 f1_bits) noexcept {
    if (is_nan_double(f1_bits)) return kEntries;  // unordered with every entry: none is "greater"
    const double x = bits_to_double(f1_bits);
    u32 lo = 0u, hi = kEntries;  // first index with db > x
    while (lo < hi) {
        const u32 mid = lo + ((hi - lo) >> 1);
        if (single_value(db_bits[mid]) > x) hi = mid;
        else lo = mid + 1u;
    }
    return lo;
}

void set_enabled(bool enabled) noexcept { g_enabled = enabled; }
bool enabled() noexcept { return g_enabled; }

bool serve_search(Cpu& c) {
    if (!g_enabled) return false;
    ++g_calls;
    if (!g_column.built) build(c);
    if (!g_column.usable) return false;
    ++g_served;
    const u64 x = c.fpr[1].bits[0];
    const u32 index = scan_index_binary(g_column.db, x);
    const bool none = index >= kEntries;
    const u32 last = none ? kEntries - 1u : index;  // the entry of the last fcmpo executed
    // The last compare decides CR0 / FPCC and carries the exception bits (an unordered one raises the same bits every
    // iteration; fpscr_raise is idempotent, so one call equals 1025).
    const u64 key_bits = single_to_double_bits(g_column.db[last]);
    ppc::fp_compare(c, 0u, key_bits, x, true);
    c.fpr[0].bits[0] = c.fpr[0].bits[1] = key_bits;
    const u64 result = none ? single_to_double_bits(c.mem_read32(c.gpr[2] + kEndConstant)) : single_to_double_bits(g_column.amp[index]);
    c.fpr[1].bits[0] = c.fpr[1].bits[1] = result;
    c.gpr[0] = kEntries;
    c.gpr[3] = none ? 8u * kEntries : 8u * index;
    c.gpr[4] = kTable;
    c.gpr[5] = kTable + 8u * last;
    c.ctr = none ? 0u : kEntries - index;
    return true;
}

void print_stats() {
    std::fprintf(stderr, "op3 ampdb: %llu calls, %llu served (table %s)\n", static_cast<unsigned long long>(g_calls),
                 static_cast<unsigned long long>(g_served), g_column.built ? (g_column.usable ? "usable" : "NOT usable") : "not read");
}

void install(Cpu& cpu) {
    if (const char* v = std::getenv("GEKKO_OP3_STATS"); v != nullptr && v[0] == '1') std::atexit(print_stats);
    if (const char* v = std::getenv("GEKKO_OP3_AMPDB"); v != nullptr && (v[0] == '0' || v[0] == '1')) g_enabled = v[0] == '1';
    cpu.install_hle(kSearch, [](Cpu& c) { return serve_search(c); }, "GCAX dB to amplitude search (native)");
}

}  // namespace gekko::sound_ampdb
