// cpu/opt2: the identity ppcrecomp's hot-body value pass relies on (--hot-value bit 1, lfs -> stfs forwarding).
// For every one of the 2^32 single-precision words w: the D.7 store conversion of the D.6 load expansion of w is w.
// So an stfs of a register an lfs loaded stores the word the lfs read, and the generated code may store that word
// without narrowing the register again. Checked for the fast path the bodies use, for the literal D.7 function,
// and for the psq_st type-0 conversion wherever it is not the denormal flush.
#include "gekko/types.h"

#include <cstdio>

namespace {
// cpu/opt4: D.6 as it was written before single_to_double_bits lost its branch (the host widening, and for a NaN
// the sign, an all-ones exponent and the 23 fraction bits moved up). The branch-free definition must equal it for
// every word.
gekko::u64 d6_branching(gekko::u32 word) {
    const double value = static_cast<double>(gekko::bits_to_float(word));
    if (value != value) {
        return (static_cast<gekko::u64>(word & 0x80000000u) << 32) | 0x7ff0000000000000ull |
               (static_cast<gekko::u64>(word & 0x007fffffu) << 29);
    }
    return gekko::double_to_bits(value);
}
}  // namespace

int main() {
    using namespace gekko;
    unsigned long long bad = 0;
    unsigned long long bad_d6 = 0;
    unsigned long long bad_literal = 0;
    unsigned long long bad_flush = 0;
    u32 first = 0;
    u32 word = 0;
    do {
        const u64 bits = single_to_double_bits(word);
        if (bits != d6_branching(word)) ++bad_d6;
        if (double_to_single_bits(bits) != word) {
            if (bad == 0) first = word;
            ++bad;
        }
        const u32 exponent = static_cast<u32>(bits >> 52) & 0x7ffu;
        // The fast path of double_to_single_bits takes every exactly narrowed value; the literal function is its
        // definition for the rest (types.h). Both must give w.
        if (double_to_single_bits_slow(bits) != word) ++bad_literal;
        // psq_st: a nonzero single denormal stores a signed zero instead; every other word is unchanged.
        const bool denormal = exponent <= 896u && (bits << 1) != 0u;
        if (double_to_single_bits_flush(bits) != (denormal ? (word & 0x80000000u) : word)) ++bad_flush;
    } while (++word != 0u);
    std::printf("single round trip over 2^32 words: %llu differ (first 0x%08X), literal D.7 %llu, psq flush %llu, "
                "branch-free D.6 against the branching one %llu\n",
                bad, first, bad_literal, bad_flush, bad_d6);
    return (bad == 0 && bad_literal == 0 && bad_flush == 0 && bad_d6 == 0) ? 0 : 1;
}
