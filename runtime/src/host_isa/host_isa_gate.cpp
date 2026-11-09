// L0r0-02: refuse to run an image on a host that lacks the instruction set it was compiled for.
// See host_isa_gate.h. Rules for this translation unit - each one is checked by ctest native_host_isa_gate:
//   * compiled for the x86-64 baseline (CMake target shadow_host_isa_gate does not link shadow_host_isa; the
//     #error below rejects /arch:AVX and above), so the gate never executes what it is about to check;
//   * includes no gekko header and calls no inline or template function another TU could supply as a COMDAT
//     compiled for /arch: only its own functions, compiler intrinsics, Win32 imports and the C runtime;
//   * runs as a C initializer (.CRT$XIU): after the C runtime's own initializers, before every C++ initializer
//     (.CRT$XC*) and main. No TLS callback or thread_local initializer of the project precedes it.

#include "host_isa_gate.h"

#if !defined(_MSC_VER) || !defined(_M_X64)
#error "host_isa_gate.cpp targets MSVC x64 (CPUID/XGETBV intrinsics, .CRT$XIU initializer)"
#endif
#if defined(__AVX__) || defined(__AVX2__) || defined(__AVX512F__)
#error "host_isa_gate.cpp must be compiled for the x86-64 baseline: it checks the instruction set the rest of the image uses"
#endif

#include <intrin.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace shadow::host_isa {

// The level comes from CMake (SHADOW_X64_ARCH -> SHADOW_HOST_ISA_REQUIRE_<level>). The detect_mismatch key is
// the one gekko/types.h records in every TU compiled with the gekko headers, so a gate built for a different
// level than the code it guards cannot be linked into the same image (LNK2038).
#if defined(SHADOW_HOST_ISA_REQUIRE_AVX512)
static constexpr Level kCompiledLevel = Level::AVX512;
#pragma detect_mismatch("gekko_host_isa", "AVX512")
#elif defined(SHADOW_HOST_ISA_REQUIRE_AVX2)
static constexpr Level kCompiledLevel = Level::AVX2;
#pragma detect_mismatch("gekko_host_isa", "AVX2")
#elif defined(SHADOW_HOST_ISA_REQUIRE_AVX)
static constexpr Level kCompiledLevel = Level::AVX;
#pragma detect_mismatch("gekko_host_isa", "AVX")
#elif defined(SHADOW_HOST_ISA_REQUIRE_SSE4_2)
static constexpr Level kCompiledLevel = Level::SSE4_2;
#pragma detect_mismatch("gekko_host_isa", "SSE")
#elif defined(SHADOW_HOST_ISA_REQUIRE_SSE2)
static constexpr Level kCompiledLevel = Level::SSE2;
#pragma detect_mismatch("gekko_host_isa", "SSE")
#else
#error "define SHADOW_HOST_ISA_REQUIRE_<level> (CMakeLists.txt, target shadow_host_isa_gate)"
#endif

namespace {

constexpr const char* kFeatureNames[] = {
    "SSE3", "SSSE3", "SSE4.1", "SSE4.2", "POPCNT", "CMPXCHG16B", "LAHF-SAHF", "OSXSAVE", "YMM-STATE", "AVX",
    "AVX2", "BMI1", "BMI2", "F16C", "FMA3", "LZCNT", "MOVBE", "ZMM-STATE", "AVX512F", "AVX512BW", "AVX512CD",
    "AVX512DQ", "AVX512VL",
};
static_assert(sizeof(kFeatureNames) / sizeof(kFeatureNames[0]) == static_cast<unsigned>(Feature::Count));

constexpr const char* kLevelNames[] = {"SSE2", "SSE4.2", "AVX", "AVX2", "AVX512"};

constexpr FeatureSet bit(Feature feature) noexcept { return feature_bit(feature); }

constexpr FeatureSet kX86_64_v2 = bit(Feature::SSE3) | bit(Feature::SSSE3) | bit(Feature::SSE41) |
                                  bit(Feature::SSE42) | bit(Feature::POPCNT) | bit(Feature::CX16) |
                                  bit(Feature::LAHF_SAHF);
constexpr FeatureSet kAvx = kX86_64_v2 | bit(Feature::OSXSAVE) | bit(Feature::YMM_STATE) | bit(Feature::AVX);
constexpr FeatureSet kX86_64_v3 = kAvx | bit(Feature::AVX2) | bit(Feature::BMI1) | bit(Feature::BMI2) |
                                  bit(Feature::F16C) | bit(Feature::FMA3) | bit(Feature::LZCNT) |
                                  bit(Feature::MOVBE);
constexpr FeatureSet kX86_64_v4 = kX86_64_v3 | bit(Feature::ZMM_STATE) | bit(Feature::AVX512F) |
                                  bit(Feature::AVX512BW) | bit(Feature::AVX512CD) | bit(Feature::AVX512DQ) |
                                  bit(Feature::AVX512VL);
constexpr FeatureSet kXcr0Derived = bit(Feature::YMM_STATE) | bit(Feature::ZMM_STATE);

bool bit_set(std::uint32_t reg, unsigned position) noexcept { return ((reg >> position) & 1u) != 0; }

char upper(char c) noexcept { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

bool is_separator(char c) noexcept { return c == ',' || c == ' ' || c == ';' || c == '+' || c == '\t'; }

bool equals_name(const char* token, std::size_t length, const char* name) noexcept {
    std::size_t i = 0;
    for (; i < length; ++i) {
        if (name[i] == '\0' || upper(token[i]) != name[i]) return false;
    }
    return name[i] == '\0';
}

}  // namespace

const char* feature_name(Feature feature) noexcept {
    const unsigned index = static_cast<unsigned>(feature);
    return index < static_cast<unsigned>(Feature::Count) ? kFeatureNames[index] : "?";
}

const char* level_name(Level level) noexcept {
    const unsigned index = static_cast<unsigned>(level);
    return index < sizeof(kLevelNames) / sizeof(kLevelNames[0]) ? kLevelNames[index] : "?";
}

bool parse_level(const char* text, Level* level) noexcept {
    if (text == nullptr) return false;
    std::size_t length = 0;
    while (text[length] != '\0') ++length;
    for (unsigned i = 0; i < sizeof(kLevelNames) / sizeof(kLevelNames[0]); ++i) {
        if (equals_name(text, length, kLevelNames[i])) {
            *level = static_cast<Level>(i);
            return true;
        }
    }
    return false;
}

FeatureSet required_features(Level level) noexcept {
    switch (level) {
        case Level::SSE2: return 0;
        case Level::SSE4_2: return kX86_64_v2;
        case Level::AVX: return kAvx;
        case Level::AVX2: return kX86_64_v3;
        case Level::AVX512: return kX86_64_v4;
    }
    return ~FeatureSet{0};  // unknown level: nothing satisfies it
}

FeatureSet decode(const CpuidSnapshot& s) noexcept {
    FeatureSet f = 0;
    if (s.max_basic_leaf >= 1) {
        const std::uint32_t ecx = s.leaf1_ecx;
        if (bit_set(ecx, 0)) f |= bit(Feature::SSE3);
        if (bit_set(ecx, 9)) f |= bit(Feature::SSSE3);
        if (bit_set(ecx, 12)) f |= bit(Feature::FMA3);
        if (bit_set(ecx, 13)) f |= bit(Feature::CX16);
        if (bit_set(ecx, 19)) f |= bit(Feature::SSE41);
        if (bit_set(ecx, 20)) f |= bit(Feature::SSE42);
        if (bit_set(ecx, 22)) f |= bit(Feature::MOVBE);
        if (bit_set(ecx, 23)) f |= bit(Feature::POPCNT);
        if (bit_set(ecx, 28)) f |= bit(Feature::AVX);
        if (bit_set(ecx, 29)) f |= bit(Feature::F16C);
        if (bit_set(ecx, 27)) {
            f |= bit(Feature::OSXSAVE);
            const bool ymm = (s.xcr0 & 0x6u) == 0x6u;         // XCR0 bit 1 SSE, bit 2 AVX
            if (ymm) f |= bit(Feature::YMM_STATE);
            if (ymm && (s.xcr0 & 0xE0u) == 0xE0u) f |= bit(Feature::ZMM_STATE);  // bits 5 opmask, 6, 7 ZMM
        }
    }
    if (s.max_basic_leaf >= 7) {
        const std::uint32_t ebx = s.leaf7_ebx;
        if (bit_set(ebx, 3)) f |= bit(Feature::BMI1);
        if (bit_set(ebx, 5)) f |= bit(Feature::AVX2);
        if (bit_set(ebx, 8)) f |= bit(Feature::BMI2);
        if (bit_set(ebx, 16)) f |= bit(Feature::AVX512F);
        if (bit_set(ebx, 17)) f |= bit(Feature::AVX512DQ);
        if (bit_set(ebx, 28)) f |= bit(Feature::AVX512CD);
        if (bit_set(ebx, 30)) f |= bit(Feature::AVX512BW);
        if (bit_set(ebx, 31)) f |= bit(Feature::AVX512VL);
    }
    if (s.max_extended_leaf >= 0x80000001u) {
        if (bit_set(s.ext1_ecx, 0)) f |= bit(Feature::LAHF_SAHF);
        if (bit_set(s.ext1_ecx, 5)) f |= bit(Feature::LZCNT);
    }
    return f;
}

FeatureSet apply_mask(FeatureSet available, FeatureSet mask) noexcept {
    FeatureSet result = available & ~mask;
    if ((result & bit(Feature::OSXSAVE)) == 0) result &= ~kXcr0Derived;
    if ((result & bit(Feature::YMM_STATE)) == 0) result &= ~bit(Feature::ZMM_STATE);
    return result;
}

bool parse_feature_list(const char* text, FeatureSet* features, const char** bad_token,
                        std::size_t* bad_length) noexcept {
    FeatureSet result = 0;
    const char* p = text != nullptr ? text : "";
    while (*p != '\0') {
        while (*p != '\0' && is_separator(*p)) ++p;
        if (*p == '\0') break;
        const char* token = p;
        while (*p != '\0' && !is_separator(*p)) ++p;
        const std::size_t length = static_cast<std::size_t>(p - token);
        bool known = false;
        for (unsigned i = 0; i < static_cast<unsigned>(Feature::Count); ++i) {
            if (equals_name(token, length, kFeatureNames[i])) {
                result |= FeatureSet{1} << i;
                known = true;
                break;
            }
        }
        if (!known) {
            if (bad_token != nullptr) *bad_token = token;
            if (bad_length != nullptr) *bad_length = length;
            return false;
        }
    }
    *features = result;
    return true;
}

std::size_t format_features(FeatureSet features, char* buffer, std::size_t capacity) noexcept {
    std::size_t size = 0;
    if (capacity == 0) return 0;
    for (unsigned i = 0; i < static_cast<unsigned>(Feature::Count); ++i) {
        if ((features & (FeatureSet{1} << i)) == 0) continue;
        if (size != 0 && size + 1 < capacity) buffer[size++] = ' ';
        for (const char* n = kFeatureNames[i]; *n != '\0' && size + 1 < capacity; ++n) buffer[size++] = *n;
    }
    buffer[size] = '\0';
    return size;
}

Level compiled_level() noexcept { return kCompiledLevel; }

CpuidSnapshot read_host() noexcept {
    CpuidSnapshot s;
    int r[4] = {0, 0, 0, 0};
    __cpuid(r, 0);
    s.max_basic_leaf = static_cast<std::uint32_t>(r[0]);
    if (s.max_basic_leaf >= 1) {
        __cpuid(r, 1);
        s.leaf1_ecx = static_cast<std::uint32_t>(r[2]);
    }
    if (s.max_basic_leaf >= 7) {
        __cpuidex(r, 7, 0);
        s.leaf7_ebx = static_cast<std::uint32_t>(r[1]);
    }
    __cpuid(r, static_cast<int>(0x80000000u));
    s.max_extended_leaf = static_cast<std::uint32_t>(r[0]);
    if (s.max_extended_leaf >= 0x80000001u) {
        __cpuid(r, static_cast<int>(0x80000001u));
        s.ext1_ecx = static_cast<std::uint32_t>(r[2]);
    }
    // XGETBV raises #UD unless the OS has set CR4.OSXSAVE, which CPUID(1).ECX[27] reflects.
    if (bit_set(s.leaf1_ecx, 27)) s.xcr0 = _xgetbv(0);
    return s;
}

namespace {

class Message {
public:
    void add(const char* text) noexcept {
        for (; *text != '\0' && size_ + 1 < sizeof(data_); ++text) data_[size_++] = *text;
        data_[size_] = '\0';
    }
    void add(const char* text, std::size_t length) noexcept {
        for (std::size_t i = 0; i < length && text[i] != '\0' && size_ + 1 < sizeof(data_); ++i) {
            data_[size_++] = text[i];
        }
        data_[size_] = '\0';
    }
    void features(FeatureSet set) noexcept {
        char names[512];
        format_features(set, names, sizeof(names));
        add(names);
    }
    const char* text() const noexcept { return data_; }
    std::size_t size() const noexcept { return size_; }

private:
    char data_[4096] = {};
    std::size_t size_ = 0;
};

[[noreturn]] void refuse(const Message& message) noexcept {
    const HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    const bool have_error = error != nullptr && error != INVALID_HANDLE_VALUE;
    if (have_error) {
        DWORD written = 0;
        WriteFile(error, message.text(), static_cast<DWORD>(message.size()), &written, nullptr);
    }
    // Started from Explorer or a shortcut, stderr is a console that belongs to this process alone and closes
    // with it, so the text above would vanish unread: show it in a message box as well. Only then - stderr
    // redirected to a pipe or file (a test runner, a log), a console shared with the parent (a terminal), or
    // no console at all gets the text only, so no unattended caller can be left waiting on a dialog.
    // user32 is loaded only on this path.
    DWORD processes[2] = {0, 0};
    if (have_error && GetFileType(error) == FILE_TYPE_CHAR && GetConsoleProcessList(processes, 2) == 1) {
        if (const HMODULE user32 = LoadLibraryW(L"user32.dll")) {
            using MessageBoxFn = int(WINAPI*)(HWND, LPCSTR, LPCSTR, UINT);
            if (const auto box = reinterpret_cast<MessageBoxFn>(
                    reinterpret_cast<void*>(GetProcAddress(user32, "MessageBoxA")))) {
                box(nullptr, message.text(), "Unsupported processor", MB_OK | MB_ICONERROR);
            }
        }
    }
    ExitProcess(static_cast<UINT>(kRefusalExitCode));
}

int __cdecl gate_initializer() noexcept {
    const Level level = compiled_level();
    const FeatureSet required = required_features(level);
    FeatureSet available = decode(read_host());

    char mask_text[512];
    const DWORD mask_length = GetEnvironmentVariableA(kMaskVariable, mask_text, sizeof(mask_text));
    const bool masked = mask_length != 0;
    if (mask_length >= sizeof(mask_text)) {
        Message message;
        message.add("host ISA gate: ");
        message.add(kMaskVariable);
        message.add(" is longer than 511 characters\r\n");
        refuse(message);
    }
    if (masked) {
        FeatureSet mask = 0;
        const char* bad = nullptr;
        std::size_t bad_length = 0;
        if (!parse_feature_list(mask_text, &mask, &bad, &bad_length)) {
            Message message;
            message.add("host ISA gate: ");
            message.add(kMaskVariable);
            message.add(" names '");
            message.add(bad, bad_length);
            message.add("', which is not a feature; known features: ");
            message.features(~FeatureSet{0} >> (32u - static_cast<unsigned>(Feature::Count)));
            message.add("\r\n");
            refuse(message);
        }
        available = apply_mask(available, mask);
    }

    const FeatureSet missing = required & ~available;
    if (missing == 0) return 0;

    char path[MAX_PATH + 1] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    Message message;
    message.add("host ISA gate: ");
    message.add(path);
    message.add(": this program cannot run on this computer.\r\n");
    message.add("host ISA gate: level: ");
    message.add(level_name(level));
    message.add("\r\nhost ISA gate: required: ");
    message.features(required);
    message.add("\r\nhost ISA gate: missing: ");
    message.features(missing);
    message.add("\r\n");
    if (masked) {
        message.add("host ISA gate: mask: ");
        message.add(mask_text);
        message.add("   (");
        message.add(kMaskVariable);
        message.add(" simulated the absence of these features)\r\n");
    }
    message.add("host ISA gate: it was built with SHADOW_X64_ARCH=");
    message.add(level_name(level));
    message.add(" (MSVC /arch:");
    message.add(level_name(level));
    message.add(") and this processor or operating system does not provide the features listed as missing ");
    message.add("(YMM-STATE / ZMM-STATE: the operating system has not enabled saving that vector state). ");
    message.add("Rebuild with a lower -DSHADOW_X64_ARCH; SSE2 runs on every x86-64 processor.\r\n");
    refuse(message);
}

}  // namespace
}  // namespace shadow::host_isa

// A C initializer: _initterm_e runs .CRT$XIA..XIZ after the C runtime is initialised and before the C++
// initializers in .CRT$XCA..XCZ (vcstartup exe_common.inl). The pointer is extern and force-included so neither
// the compiler nor /OPT:REF can drop an otherwise unreferenced object.
extern "C" {
#pragma section(".CRT$XIU", long, read)
__declspec(allocate(".CRT$XIU"))
extern int(__cdecl* const shadow_host_isa_gate_initializer)(void) = shadow::host_isa::gate_initializer;
}
#pragma comment(linker, "/INCLUDE:shadow_host_isa_gate_initializer")
