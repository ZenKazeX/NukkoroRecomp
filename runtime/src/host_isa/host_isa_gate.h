#pragma once

// L0r0-02: the host instruction-set contract of an image, checked before the first instruction that relies on it.
//
// SHADOW_X64_ARCH (CMakeLists.txt) compiles the generated recomp shards, the runtime and every TU that
// includes runtime/include/gekko with /arch:<level>. From the first C++ static initializer on, those images
// execute LZCNT (gekko/ppc_ops.h cntlzw), FMA3 (fused_multiply_add), BMI1/BMI2 and AVX/AVX2 without asking
// the processor. On a host that lacks one of them the process died with an unexplained
// STATUS_ILLEGAL_INSTRUCTION - or, for LZCNT on a processor without it, did not fault at all: the encoding
// F3 0F BD decodes as BSR and returns the index of the highest set bit instead of the leading-zero count.
//
// host_isa_gate.cpp installs a C initializer (.CRT$XIU) that runs after the C runtime has initialised and
// before every C++ initializer and main. It reads CPUID and, when the OS has enabled XSAVE, XGETBV, and
// refuses to continue with a diagnostic and exit code kRefusalExitCode when the level the image was compiled
// for is not available. That TU is compiled for the x86-64 baseline and includes no gekko header, so the check
// itself never depends on what it checks (ctest native_host_isa_gate verifies the linked code).
//
// This header is the pure decision logic, free of Windows and of CPUID, so it can be tested with synthetic
// processor snapshots (runtime/tests/host_isa_gate_tests.cpp).

#include <cstddef>
#include <cstdint>

namespace shadow::host_isa {

// Features the gate can require. CPUID bit positions: Intel SDM Vol. 2A, CPUID instruction,
// "Feature Information Returned in the ECX Register" (leaf 01H), "Structured Extended Feature Flags"
// (leaf 07H sub-leaf 0 EBX) and leaf 80000001H ECX. XCR0 state components: Intel SDM Vol. 1 section 13.1.
enum class Feature : unsigned {
    SSE3,        // 01H ECX[0]
    SSSE3,       // 01H ECX[9]
    SSE41,       // 01H ECX[19]
    SSE42,       // 01H ECX[20]
    POPCNT,      // 01H ECX[23]
    CX16,        // 01H ECX[13] CMPXCHG16B
    LAHF_SAHF,   // 80000001H ECX[0] LAHF/SAHF in 64-bit mode
    OSXSAVE,     // 01H ECX[27] the OS has set CR4.OSXSAVE: XGETBV is usable
    YMM_STATE,   // XCR0[2:1] == 11b: the OS saves SSE and AVX (YMM upper) state
    AVX,         // 01H ECX[28]
    AVX2,        // 07H EBX[5]
    BMI1,        // 07H EBX[3]
    BMI2,        // 07H EBX[8]
    F16C,        // 01H ECX[29]
    FMA3,        // 01H ECX[12]
    LZCNT,       // 80000001H ECX[5] (AMD: ABM)
    MOVBE,       // 01H ECX[22]
    ZMM_STATE,   // XCR0[7:5] == 111b in addition to YMM_STATE: opmask, ZMM_Hi256, Hi16_ZMM
    AVX512F,     // 07H EBX[16]
    AVX512BW,    // 07H EBX[30]
    AVX512CD,    // 07H EBX[28]
    AVX512DQ,    // 07H EBX[17]
    AVX512VL,    // 07H EBX[31]
    Count
};

using FeatureSet = std::uint32_t;

constexpr FeatureSet feature_bit(Feature feature) noexcept {
    return FeatureSet{1} << static_cast<unsigned>(feature);
}

// The SHADOW_X64_ARCH levels. Requirements follow the x86-64 psABI micro-architecture levels, which bound
// what MSVC may emit at each /arch level (Microsoft: /arch:AVX2 "enables use of Fused Multiply-Add (FMA)
// instructions" and "certain BMI instructions") and what gekko/ppc_ops.h selects under __AVX2__ (LZCNT):
//   SSE2    x86-64 baseline                           nothing to check
//   SSE4_2  x86-64-v2                                 SSE3 SSSE3 SSE4.1 SSE4.2 POPCNT CMPXCHG16B LAHF-SAHF
//   AVX     x86-64-v2 + AVX with OS YMM state
//   AVX2    x86-64-v3                                 + AVX2 BMI1 BMI2 F16C FMA3 LZCNT MOVBE
//   AVX512  x86-64-v4                                 + AVX512F/BW/CD/DQ/VL with OS ZMM state
enum class Level : unsigned { SSE2, SSE4_2, AVX, AVX2, AVX512 };

// Raw processor answers. Leaves the processor does not implement are zero.
struct CpuidSnapshot {
    std::uint32_t max_basic_leaf = 0;     // CPUID(0).EAX
    std::uint32_t leaf1_ecx = 0;          // CPUID(1).ECX
    std::uint32_t leaf7_ebx = 0;          // CPUID(7,0).EBX
    std::uint32_t max_extended_leaf = 0;  // CPUID(80000000H).EAX
    std::uint32_t ext1_ecx = 0;           // CPUID(80000001H).ECX
    std::uint64_t xcr0 = 0;               // XGETBV(0); meaningful only when leaf1_ecx has OSXSAVE
};

constexpr int kRefusalExitCode = 29;  // distinct from the program's own exit codes (0-5)
constexpr const char* kMaskVariable = "SHADOW_HOST_ISA_MASK";

const char* feature_name(Feature feature) noexcept;
const char* level_name(Level level) noexcept;  // the SHADOW_X64_ARCH spelling: SSE2 SSE4.2 AVX AVX2 AVX512
bool parse_level(const char* text, Level* level) noexcept;

FeatureSet required_features(Level level) noexcept;

// Which features the snapshot provides. Leaves above the reported maximum are ignored whatever their bits say,
// and the XCR0-derived state features exist only when OSXSAVE is set.
FeatureSet decode(const CpuidSnapshot& snapshot) noexcept;

// Remove features (the SHADOW_HOST_ISA_MASK diagnostic). Removing OSXSAVE removes the XCR0-derived state too,
// because without it XGETBV cannot be executed. A mask can only ever make the gate stricter.
FeatureSet apply_mask(FeatureSet available, FeatureSet mask) noexcept;

// Comma, space, semicolon or plus separated, case-insensitive feature names. On failure returns false and
// points *bad_token / *bad_length at the first unknown name.
bool parse_feature_list(const char* text, FeatureSet* features, const char** bad_token,
                        std::size_t* bad_length) noexcept;

// Space-separated names in Feature order into a NUL-terminated buffer; returns the length written.
std::size_t format_features(FeatureSet features, char* buffer, std::size_t capacity) noexcept;

// The level this image's gate was compiled to require (from SHADOW_X64_ARCH).
Level compiled_level() noexcept;

// CPUID/XGETBV of the running host (x64 only).
CpuidSnapshot read_host() noexcept;

}  // namespace shadow::host_isa
