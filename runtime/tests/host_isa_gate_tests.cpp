// L0r0-02: the host ISA gate's decision logic against synthetic processors, and its CPUID/XGETBV decoding
// against the operating system's own independent decoder (IsProcessorFeaturePresent) on the running host.
// The image-level half (initializer order, linked gate code, refusal of the real executables) is
// tools/ppcrecomp/check_host_isa_gate.py, ctest native_host_isa_gate.

#include "host_isa_gate.h"

#include <cstdio>
#include <cstring>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

using namespace shadow::host_isa;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

std::string names(FeatureSet set) {
    char buffer[512];
    format_features(set, buffer, sizeof(buffer));
    return buffer;
}

FeatureSet set_of(const char* list) {
    FeatureSet out = 0;
    const char* bad = nullptr;
    std::size_t bad_length = 0;
    if (!parse_feature_list(list, &out, &bad, &bad_length)) {
        std::printf("FAIL test list '%s' has unknown token\n", list);
        ++failures;
    }
    return out;
}

void expect_missing(const char* processor, const CpuidSnapshot& snapshot, Level level, const char* expected) {
    const FeatureSet missing = required_features(level) & ~decode(snapshot);
    const FeatureSet want = set_of(expected);
    check(missing == want, std::string(processor) + " at " + level_name(level) + ": missing '" + names(missing) +
                               "', expected '" + names(want) + "'");
}

// CPUID(1).ECX bits
constexpr std::uint32_t SSE3 = 1u << 0, SSSE3 = 1u << 9, FMA = 1u << 12, CX16 = 1u << 13, SSE41 = 1u << 19,
                        SSE42 = 1u << 20, MOVBE = 1u << 22, POPCNT = 1u << 23, OSXSAVE = 1u << 27,
                        AVX = 1u << 28, F16C = 1u << 29;
// CPUID(7,0).EBX bits
constexpr std::uint32_t BMI1 = 1u << 3, AVX2 = 1u << 5, BMI2 = 1u << 8, AVX512F = 1u << 16, AVX512DQ = 1u << 17,
                        AVX512CD = 1u << 28, AVX512BW = 1u << 30, AVX512VL = 1u << 31;
// CPUID(80000001H).ECX bits
constexpr std::uint32_t LAHF = 1u << 0, LZCNT = 1u << 5;

CpuidSnapshot core2() {  // Merom: SSSE3, no SSE4.1/4.2/POPCNT
    CpuidSnapshot s;
    s.max_basic_leaf = 0xA;
    s.leaf1_ecx = SSE3 | SSSE3 | CX16;
    s.max_extended_leaf = 0x80000008u;
    s.ext1_ecx = LAHF;
    return s;
}

CpuidSnapshot sandy_bridge() {
    CpuidSnapshot s;
    s.max_basic_leaf = 0xD;
    s.leaf1_ecx = SSE3 | SSSE3 | CX16 | SSE41 | SSE42 | POPCNT | OSXSAVE | AVX;
    s.max_extended_leaf = 0x80000008u;
    s.ext1_ecx = LAHF;
    s.xcr0 = 0x7;
    return s;
}

CpuidSnapshot haswell() {
    CpuidSnapshot s = sandy_bridge();
    s.leaf1_ecx |= FMA | MOVBE | F16C;
    s.leaf7_ebx = BMI1 | AVX2 | BMI2;
    s.ext1_ecx |= LZCNT;
    return s;
}

CpuidSnapshot skylake_x() {
    CpuidSnapshot s = haswell();
    s.leaf7_ebx |= AVX512F | AVX512DQ | AVX512CD | AVX512BW | AVX512VL;
    s.xcr0 = 0xE7;
    return s;
}

void synthetic_processors() {
    expect_missing("core2", core2(), Level::SSE2, "");
    expect_missing("core2", core2(), Level::SSE4_2, "SSE4.1 SSE4.2 POPCNT");
    expect_missing("sandy bridge", sandy_bridge(), Level::AVX, "");
    expect_missing("sandy bridge", sandy_bridge(), Level::AVX2, "AVX2 BMI1 BMI2 F16C FMA3 LZCNT MOVBE");
    expect_missing("haswell", haswell(), Level::AVX2, "");
    expect_missing("haswell", haswell(), Level::AVX512,
                   "ZMM-STATE AVX512F AVX512BW AVX512CD AVX512DQ AVX512VL");
    expect_missing("skylake-x", skylake_x(), Level::AVX512, "");

    // The OS has not enabled XSAVE (e.g. bcdedit xsavedisable): XGETBV must not be trusted - here even
    // a stale XCR0 value with every bit set must not produce the state features.
    CpuidSnapshot no_osxsave = haswell();
    no_osxsave.leaf1_ecx &= ~OSXSAVE;
    no_osxsave.xcr0 = ~std::uint64_t{0};
    expect_missing("haswell without OSXSAVE", no_osxsave, Level::AVX2, "OSXSAVE YMM-STATE");
    expect_missing("haswell without OSXSAVE", no_osxsave, Level::SSE4_2, "");

    CpuidSnapshot sse_only_os = haswell();
    sse_only_os.xcr0 = 0x3;  // x87 | SSE: the OS does not save YMM
    expect_missing("haswell, OS without YMM state", sse_only_os, Level::AVX, "YMM-STATE");

    CpuidSnapshot no_zmm_os = skylake_x();
    no_zmm_os.xcr0 = 0x7;
    expect_missing("skylake-x, OS without ZMM state", no_zmm_os, Level::AVX512, "ZMM-STATE");

    CpuidSnapshot zmm_without_ymm = skylake_x();
    zmm_without_ymm.xcr0 = 0xE3;  // opmask/ZMM bits but AVX state off: not usable
    expect_missing("skylake-x, XCR0 without AVX bit", zmm_without_ymm, Level::AVX512, "YMM-STATE ZMM-STATE");

    // Leaves above the reported maximum return data of another leaf on real processors; never decode them.
    CpuidSnapshot low_basic = haswell();
    low_basic.max_basic_leaf = 6;
    expect_missing("haswell reporting max leaf 6", low_basic, Level::AVX2, "AVX2 BMI1 BMI2");
    CpuidSnapshot low_extended = haswell();
    low_extended.max_extended_leaf = 0x80000000u;
    expect_missing("haswell reporting max extended leaf 80000000H", low_extended, Level::AVX2, "LAHF-SAHF LZCNT");
    CpuidSnapshot nothing;
    nothing.leaf1_ecx = ~0u;
    nothing.leaf7_ebx = ~0u;
    nothing.ext1_ecx = ~0u;
    nothing.xcr0 = ~std::uint64_t{0};
    check(decode(nothing) == 0, "a snapshot reporting no leaves decodes to nothing, got " + names(decode(nothing)));

    // AVX2 present but LZCNT absent: the one case where the image would not fault but compute BSR.
    CpuidSnapshot no_lzcnt = haswell();
    no_lzcnt.ext1_ecx &= ~LZCNT;
    expect_missing("haswell without LZCNT", no_lzcnt, Level::AVX2, "LZCNT");

    // Every CPUID bit maps to exactly one feature (Intel SDM Vol. 2A CPUID tables).
    struct Single { std::uint32_t CpuidSnapshot::*reg; std::uint32_t mask; const char* feature; };
    const Single singles[] = {
        {&CpuidSnapshot::leaf1_ecx, SSE3, "SSE3"}, {&CpuidSnapshot::leaf1_ecx, SSSE3, "SSSE3"},
        {&CpuidSnapshot::leaf1_ecx, FMA, "FMA3"}, {&CpuidSnapshot::leaf1_ecx, CX16, "CMPXCHG16B"},
        {&CpuidSnapshot::leaf1_ecx, SSE41, "SSE4.1"}, {&CpuidSnapshot::leaf1_ecx, SSE42, "SSE4.2"},
        {&CpuidSnapshot::leaf1_ecx, MOVBE, "MOVBE"}, {&CpuidSnapshot::leaf1_ecx, POPCNT, "POPCNT"},
        {&CpuidSnapshot::leaf1_ecx, AVX, "AVX"}, {&CpuidSnapshot::leaf1_ecx, F16C, "F16C"},
        {&CpuidSnapshot::leaf7_ebx, BMI1, "BMI1"}, {&CpuidSnapshot::leaf7_ebx, AVX2, "AVX2"},
        {&CpuidSnapshot::leaf7_ebx, BMI2, "BMI2"}, {&CpuidSnapshot::leaf7_ebx, AVX512F, "AVX512F"},
        {&CpuidSnapshot::leaf7_ebx, AVX512DQ, "AVX512DQ"}, {&CpuidSnapshot::leaf7_ebx, AVX512CD, "AVX512CD"},
        {&CpuidSnapshot::leaf7_ebx, AVX512BW, "AVX512BW"}, {&CpuidSnapshot::leaf7_ebx, AVX512VL, "AVX512VL"},
        {&CpuidSnapshot::ext1_ecx, LAHF, "LAHF-SAHF"}, {&CpuidSnapshot::ext1_ecx, LZCNT, "LZCNT"},
    };
    for (const Single& single : singles) {
        CpuidSnapshot s;
        s.max_basic_leaf = 7;
        s.max_extended_leaf = 0x80000001u;
        s.*single.reg = single.mask;
        check(decode(s) == set_of(single.feature),
              std::string("single bit for ") + single.feature + " decodes to '" + names(decode(s)) + "'");
    }
}

void levels_and_names() {
    check(required_features(Level::SSE2) == 0, "SSE2 requires nothing");
    const Level order[] = {Level::SSE2, Level::SSE4_2, Level::AVX, Level::AVX2, Level::AVX512};
    for (unsigned i = 1; i < 5; ++i) {
        const FeatureSet lower = required_features(order[i - 1]), higher = required_features(order[i]);
        check((lower & ~higher) == 0 && lower != higher,
              std::string(level_name(order[i])) + " strictly contains " + level_name(order[i - 1]));
    }
    check(names(required_features(Level::AVX2)) ==
              "SSE3 SSSE3 SSE4.1 SSE4.2 POPCNT CMPXCHG16B LAHF-SAHF OSXSAVE YMM-STATE AVX AVX2 BMI1 BMI2 F16C "
              "FMA3 LZCNT MOVBE",
          "AVX2 = x86-64-v3, got " + names(required_features(Level::AVX2)));
    for (const Level level : order) {
        Level parsed = Level::SSE2;
        check(parse_level(level_name(level), &parsed) && parsed == level,
              std::string("level name round-trips: ") + level_name(level));
    }
    Level parsed = Level::SSE2;
    check(!parse_level("AVX10.1", &parsed), "AVX10.1 is not a supported level");
    for (unsigned i = 0; i < static_cast<unsigned>(Feature::Count); ++i) {
        const Feature feature = static_cast<Feature>(i);
        check(set_of(feature_name(feature)) == feature_bit(feature),
              std::string("feature name round-trips: ") + feature_name(feature));
    }
    check(set_of(" avx2,bmi2;lzcnt+Sse4.1 ") ==
              (feature_bit(Feature::AVX2) | feature_bit(Feature::BMI2) | feature_bit(Feature::LZCNT) |
               feature_bit(Feature::SSE41)),
          "case-insensitive separated list");
    FeatureSet unused = 0;
    const char* bad = nullptr;
    std::size_t bad_length = 0;
    check(!parse_feature_list("AVX2,AVX3,BMI2", &unused, &bad, &bad_length) && bad_length == 4 &&
              std::strncmp(bad, "AVX3", 4) == 0,
          "unknown token is reported");
    check(!parse_feature_list("AVX", &unused, &bad, &bad_length) || unused == feature_bit(Feature::AVX),
          "AVX is not a prefix match of AVX2");
    check(!parse_feature_list("AVX2X", &unused, &bad, &bad_length), "AVX2X is not AVX2");

    // A mask can only remove; removing OSXSAVE removes the XCR0-derived state with it.
    const FeatureSet all = decode(skylake_x());
    check(apply_mask(all, 0) == all, "empty mask changes nothing");
    check(apply_mask(all, feature_bit(Feature::OSXSAVE)) ==
              (all & ~(feature_bit(Feature::OSXSAVE) | feature_bit(Feature::YMM_STATE) |
                       feature_bit(Feature::ZMM_STATE))),
          "masking OSXSAVE removes YMM-STATE and ZMM-STATE");
    check(apply_mask(all, feature_bit(Feature::YMM_STATE)) ==
              (all & ~(feature_bit(Feature::YMM_STATE) | feature_bit(Feature::ZMM_STATE))),
          "masking YMM-STATE removes ZMM-STATE");
    check((apply_mask(0, ~FeatureSet{0}) | apply_mask(decode(core2()), 0)) == decode(core2()),
          "a mask never adds features");
}

void host_against_operating_system() {
    const CpuidSnapshot host = read_host();
    const FeatureSet f = decode(host);
    std::printf("host: max leaf %u, max extended %08X, 01H.ECX %08X, 07H.EBX %08X, 80000001H.ECX %08X, XCR0 %llX\n",
                host.max_basic_leaf, host.max_extended_leaf, host.leaf1_ecx, host.leaf7_ebx, host.ext1_ecx,
                static_cast<unsigned long long>(host.xcr0));
    std::printf("host features: %s\n", names(f).c_str());
    auto has = [&](Feature feature) { return (f & feature_bit(feature)) != 0; };
    struct Pair { DWORD pf; bool gate; const char* what; };
    const Pair pairs[] = {
        {PF_SSE3_INSTRUCTIONS_AVAILABLE, has(Feature::SSE3), "SSE3"},
        {PF_SSSE3_INSTRUCTIONS_AVAILABLE, has(Feature::SSSE3), "SSSE3"},
        {PF_SSE4_1_INSTRUCTIONS_AVAILABLE, has(Feature::SSE41), "SSE4.1"},
        {PF_SSE4_2_INSTRUCTIONS_AVAILABLE, has(Feature::SSE42), "SSE4.2"},
        // The OS reports AVX/AVX2/AVX-512 as available only when it also saves the state.
        {PF_AVX_INSTRUCTIONS_AVAILABLE, has(Feature::AVX) && has(Feature::YMM_STATE), "AVX with YMM state"},
        {PF_AVX2_INSTRUCTIONS_AVAILABLE, has(Feature::AVX2) && has(Feature::YMM_STATE), "AVX2 with YMM state"},
        {PF_AVX512F_INSTRUCTIONS_AVAILABLE, has(Feature::AVX512F) && has(Feature::ZMM_STATE),
         "AVX512F with ZMM state"},
        {PF_BMI2_INSTRUCTIONS_AVAILABLE, has(Feature::BMI2), "BMI2"},
    };
    for (const Pair& pair : pairs) {
        const bool os = IsProcessorFeaturePresent(pair.pf) != FALSE;
        std::printf("  %-24s gate %d  IsProcessorFeaturePresent(%lu) %d\n", pair.what, pair.gate ? 1 : 0,
                    static_cast<unsigned long>(pair.pf), os ? 1 : 0);
        check(os == pair.gate, std::string("host decode of ") + pair.what + " agrees with the operating system");
    }
    // This test executable itself links the gate (it is compiled for this build's level): reaching here
    // means the host passed, which must agree with the decision logic.
    const FeatureSet missing = required_features(compiled_level()) & ~f;
    check(missing == 0, std::string("this host passed the gate but the logic reports missing: ") + names(missing));
}

}  // namespace

int main() {
#ifdef SHADOW_TEST_EXPECTED_LEVEL
    check(std::strcmp(level_name(compiled_level()), SHADOW_TEST_EXPECTED_LEVEL) == 0,
          std::string("compiled level ") + level_name(compiled_level()) + " is SHADOW_X64_ARCH " +
              SHADOW_TEST_EXPECTED_LEVEL);
#endif
    synthetic_processors();
    levels_and_names();
    host_against_operating_system();
    if (failures != 0) {
        std::printf("host_isa_gate_tests: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("host_isa_gate_tests: PASS\n");
    return 0;
}
