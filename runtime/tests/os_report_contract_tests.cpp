// Original-DOL ownership oracle for OSReport (gekko/native_os_report.h).
// liveness-roots: reference-interpreter-only
// The pinned DOL's OSReport (vprintf, __pformatter, the stdout FILE) runs in the reference interpreter with the console
// write routine 0x803AF5A8 replaced by a capture of the bytes it is asked to write. The native formatter reads the same
// registers and guest memory; the text must be equal, and the native owner must write no guest byte.
//
// Census: every printf-style string in the DOL's data sections (all conversions the game's strings use, incl. the ones
// OSReport's callers pass), each with several argument sets chosen per conversion (integers incl. 32/64-bit edge values,
// strings of 0, 3, 300 and 5,000 bytes, a null pointer, doubles incl. +-0, inf, nan, 1e300), placed in registers and in
// the caller's overflow area the way the EABI places them (independent of the formatter under test). A format without
// a trailing newline gets one: the original line-buffers stdout and would keep the tail for the next call.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_os_misc.h"
#include "gekko/native_os_report.h"
#include "gekko/native_os_debug.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
using namespace gekko::os_report;

constexpr u32 kReturn = 0x817FF000u, kStack = 0x81170000u, kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kConsoleWrite = 0x803AF5A8u;
constexpr u32 kFormat = 0x81100000u, kPool = 0x81110000u;
constexpr u32 kWatched = 0x81000000u, kWatchedBytes = 0x180000u;  // pool, formats, stack (below 0x81180000)

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Oracle {
    GuestMemory memory;
    Cpu cpu{memory};
    std::string captured;
    u64 instructions = 0;
    std::vector<u8> dol;

    explicit Oracle(const char* path) {
        std::ifstream input(path, std::ios::binary);
        dol.assign(std::istreambuf_iterator<char>(input), {});
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
    }
    void zero_scratch() {
        const std::vector<u8> zero(0x200000);
        memory.write_block(0x81000000u, zero.data(), zero.size());
    }
};

// ---- the argument model of the caller (independent of ArgCursor) ----------------------------------------------------
enum class Kind { Word, DoubleWord, Pointer, Double, Char, Star };

struct Arg {
    Kind kind;
    u64 integer = 0;
    double real = 0;
};

struct Placed {
    u32 gpr[8]{};                 // r3..r10
    double fpr[8]{};
    unsigned fprs = 0;
    std::vector<u32> overflow;    // words from r1 + 8
};

Placed place(const std::vector<u32>& fixed, const std::vector<Arg>& args) {
    Placed p;
    for (std::size_t i = 0; i < fixed.size(); ++i) p.gpr[i] = fixed[i];
    unsigned g = static_cast<unsigned>(fixed.size());
    for (const Arg& a : args) {
        switch (a.kind) {
        case Kind::Word: case Kind::Pointer: case Kind::Char: case Kind::Star:
            if (g < 8) p.gpr[g++] = static_cast<u32>(a.integer);
            else p.overflow.push_back(static_cast<u32>(a.integer));
            break;
        case Kind::DoubleWord:
            if (g & 1u) ++g;
            if (g + 2 <= 8) { p.gpr[g] = static_cast<u32>(a.integer >> 32); p.gpr[g + 1] = static_cast<u32>(a.integer); g += 2; }
            else {
                g = 8;
                if (p.overflow.size() & 1u) p.overflow.push_back(0xEEEEEEEEu);
                p.overflow.push_back(static_cast<u32>(a.integer >> 32));
                p.overflow.push_back(static_cast<u32>(a.integer));
            }
            break;
        case Kind::Double: {
            u64 bits;
            std::memcpy(&bits, &a.real, sizeof(bits));
            if (p.fprs < 8) p.fpr[p.fprs++] = a.real;
            else {
                if (p.overflow.size() & 1u) p.overflow.push_back(0xEEEEEEEEu);
                p.overflow.push_back(static_cast<u32>(bits >> 32));
                p.overflow.push_back(static_cast<u32>(bits));
            }
            break;
        }
        }
    }
    return p;
}

void load_registers(Cpu& cpu, const Placed& p, bool floats) {
    cpu.reset();
    cpu.fpscr = 0;
    for (unsigned i = 0; i < 32; ++i) cpu.gpr[i] = 0x10101010u * (i + 1u) + 7u;
    cpu.gpr[1] = kStack; cpu.gpr[2] = kR2; cpu.gpr[13] = kR13;
    for (unsigned i = 0; i < 8; ++i) cpu.gpr[3 + i] = p.gpr[i];
    for (unsigned i = 0; i < 32; ++i) cpu.fpr[i] = FpReg{};
    for (unsigned i = 0; i < p.fprs; ++i) std::memcpy(&cpu.fpr[1 + i].bits[0], &p.fpr[i], sizeof(double));
    cpu.cr = floats ? 0x02000000u : 0x00000000u;  // cr1.eq (bit 6) as `creqv 6,6,6` / `crxor 6,6,6` leave it
    cpu.lr = kReturn;
}

// ---- parse a format into the argument kinds it consumes ------------------------------------------------------------
bool arg_kinds(const std::string& text, std::vector<Kind>& kinds, std::string& unsupported) {
    kinds.clear();
    for (std::size_t i = 0; i < text.size();) {
        if (text[i++] != '%') continue;
        while (i < text.size() && std::strchr("-+ #0", text[i])) ++i;
        if (i < text.size() && text[i] == '*') { kinds.push_back(Kind::Star); ++i; }
        else while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
        if (i < text.size() && text[i] == '.') {
            ++i;
            if (i < text.size() && text[i] == '*') { kinds.push_back(Kind::Star); ++i; }
            else while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
        }
        bool ll = false;
        while (i < text.size() && std::strchr("hlLjzt", text[i])) { if (text[i] == 'l' && i + 1 < text.size() && text[i + 1] == 'l') ll = true; if (text[i] == 'j') ll = true; ++i; }
        if (i >= text.size()) return false;
        const char c = text[i++];
        switch (c) {
        case '%': break;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': kinds.push_back(ll ? Kind::DoubleWord : Kind::Word); break;
        case 'c': kinds.push_back(Kind::Char); break;
        case 's': kinds.push_back(Kind::Pointer); break;
        case 'p': kinds.push_back(Kind::Pointer); break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': kinds.push_back(Kind::Double); break;
        default: unsupported = std::string("%") + c; return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: os_report_contract_tests main.dol\n"); return 2; }
    try {
        Oracle original(argv[1]);
        // The census: printf-style strings of the DOL's data sections.
        std::vector<std::string> formats;
        {
            const auto word = [&](unsigned offset) { return load_be32(original.dol.data() + offset); };
            for (unsigned s = 7; s < 18; ++s) {
                const u32 offset = word(s * 4), size = word(0x90 + s * 4);
                if (!size) continue;
                std::string run;
                for (u32 i = 0; i < size; ++i) {
                    const u8 c = original.dol[offset + i];
                    if (c == 0) {
                        if (run.size() >= 3 && run.size() <= 200 && run.find('%') != std::string::npos) formats.push_back(run);
                        run.clear();
                    } else if ((c >= 0x20 && c < 0x7F) || c == '\n' || c == '\t') run.push_back(static_cast<char>(c));
                    else run.clear();
                }
            }
        }
        require(!formats.empty(), "no census");
        // Every special value of the floating-point conversions, with flags and widths (the string census alone
        // samples them).
        for (const char* extra : {"%f", "%e", "%g", "%F", "%E", "%G", "%10.3f|", "%-10f|", "%+f", "% f", "%010.2f", "%.0f",
                                  "%#.0f", "%5.1e", "%12g|", "%.3g", "%.1f", "%.2f", "%.3f", "%.4f", "%s"})
            formats.push_back(extra);

        const u64 integers[] = {0, 1, 0xFFFFFFFFull, 42, 0x7FFFFFFFull, 0x80000000ull, 0xDEADBEEFull, 7, 100000, 0x1234ull,
                                0xFFFFFFFFFFFFFFFFull, 0x123456789ABCDEF0ull};
        const double reals[] = {0.0, -0.0, 1.5, -2.25, 3.14159265358979, 1e10, 1e-5, 123456789.987654321, 1e300,
                                0.5, 2.5, 999.9996, -0.001, 1.7e40, 6.02214076e33, 9.87654321e50, 123456789012345678.0,
                                std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::quiet_NaN(),
                                0.25, 0.125, 0.375, 0.0625, 10.5, 11.5, 1.5, 3.5, 0.05, 0.35, 1.005, 2.675, -0.5, -1.5, -2.5};
        // (the last five straddle stdout's 256-byte buffer once the newline is added)
        const std::string strings[] = {"", "abc", std::string(300, 'x'), std::string(5000, 'Z'), "line\twith tab", "%d%s",
                                       std::string(254, 'y'), std::string(255, 'y'), std::string(256, 'y'),
                                       std::string(257, 'y'), std::string(258, 'y')};

        // The FILE objects of the pinned image (their state changes with every print: an error latches).
        std::vector<u8> file_image(0xF0);
        original.memory.read_block(0x80568ED0u, file_image.data(), file_image.size());
        const auto restore_files = [&]() { original.memory.write_block(0x80568ED0u, file_image.data(), file_image.size()); };
        u32 console_result = 0u;  // what the replaced console write routine returns
        const auto run_original = [&](const Placed& placed, bool has_float, const std::string& raw, u32 entry = kOsReport) {
            load_registers(original.cpu, placed, has_float);
            original.cpu.pc = entry; original.cpu.npc = entry + 4;
            original.captured.clear();
            unsigned steps = 0;
            while (original.cpu.pc != kReturn && !original.cpu.stopped() && steps++ < 20000000) {
                if (original.cpu.pc == kConsoleWrite) {
                    // (handle, buffer, &size, context): the bytes asked for are the buffer's first *size.
                    const u32 buffer = original.cpu.gpr[4], size = original.memory.peek32(original.cpu.gpr[5]);
                    for (u32 k = 0; k < size; ++k) original.captured.push_back(static_cast<char>(original.memory.peek8(buffer + k)));
                    original.cpu.gpr[3] = console_result;
                    original.cpu.pc = original.cpu.lr; original.cpu.npc = original.cpu.pc + 4;
                    continue;
                }
                original.cpu.run_steps(1); ++original.instructions;
            }
            if (original.cpu.pc != kReturn || original.cpu.stopped())
                throw std::runtime_error("the original OSReport did not return for format: " + raw);
        };
        unsigned cases = 0, skipped_formats = 0, skipped_with_pointer_misuse = 0;
        std::set<std::string> unsupported_seen;
        std::map<std::string, unsigned> failures;
        for (const std::string& raw : formats) {
            std::vector<Kind> kinds;
            std::string unsupported;
            std::string text = raw;
            if (text.back() != '\n') text.push_back('\n');
            if (!arg_kinds(text, kinds, unsupported)) {
                unsupported_seen.insert(unsupported.empty() ? raw : unsupported);
                ++skipped_formats;
                continue;
            }
            if (kinds.size() > 12) { ++skipped_formats; continue; }
            for (unsigned set = 0; set < 32; ++set) {
                original.zero_scratch();
                original.memory.write_block(kFormat, text.c_str(), text.size() + 1);
                std::vector<Arg> args;
                u32 pool_cursor = kPool;
                bool has_float = false;
                for (std::size_t j = 0; j < kinds.size(); ++j) {
                    Arg a{kinds[j]};
                    switch (kinds[j]) {
                    case Kind::Word: case Kind::DoubleWord: a.integer = integers[(set + 3 * j) % (sizeof(integers) / sizeof(integers[0]))]; break;
                    case Kind::Char: a.integer = 0x41 + (set + j) % 26; break;
                    case Kind::Star: a.integer = (set * 5 + j * 3) % 12; break;  // a width / precision 0..11
                    case Kind::Double: a.real = reals[(set + 2 * j) % (sizeof(reals) / sizeof(reals[0]))]; has_float = true; break;
                    case Kind::Pointer: {
                        const std::size_t pick = (set + j) % 7u;
                        if (pick == 6u) { a.integer = 0u; break; }  // a null pointer
                        const std::string& value = strings[pick % (sizeof(strings) / sizeof(strings[0]))];
                        original.memory.write_block(pool_cursor, value.c_str(), value.size() + 1);
                        a.integer = pool_cursor;
                        pool_cursor += static_cast<u32>((value.size() + 16u) & ~15u);
                        break;
                    }
                    }
                    args.push_back(a);
                }
                const Placed placed = place({kFormat}, args);
                for (std::size_t w = 0; w < placed.overflow.size(); ++w)
                    original.memory.write32(kStack + 8u + static_cast<u32>(w) * 4u, placed.overflow[w]);

                run_original(placed, has_float, raw);

                // The owner, on a fresh copy of the same machine state.
                std::vector<u8> before(kWatchedBytes);
                original.memory.read_block(kWatched, before.data(), before.size());
                load_registers(original.cpu, placed, has_float);
                std::string native, why;
                ArgCursor cursor(original.cpu, 1u);
                const bool ok = format(original.cpu, kFormat, cursor, native, why);
                std::vector<u8> after(kWatchedBytes);
                original.memory.read_block(kWatched, after.data(), after.size());
                require(before == after, "the native formatter wrote guest memory");
                ++cases;
                if (!ok) { unsupported_seen.insert(why + " in: " + raw); ++failures[why]; continue; }
                if (native != original.captured) {
                    std::string key = "text differs for: " + raw;
                    if (failures[key]++ == 0)
                        std::fprintf(stderr, "DIFF fmt=[%s] set=%u\n  original=[%.200s] (%zu)\n  native  =[%.200s] (%zu)\n", raw.c_str(), set,
                                     original.captured.c_str(), original.captured.size(), native.c_str(), native.size());
                }
            }
        }

        // The entry points MSL's console files are written through: printf(format, ...) and fprintf(FILE*, format,
        // ...) on stdout / stderr, the whole owner (registers, console text, the return value) against the original,
        // with the console write succeeding and failing (the UART-less case: the write routine returns 1).
        {
            struct Entry { const char* name; u32 address; std::vector<u32> fixed; void (*serve)(Cpu&, DebugConsole&); };
            const Entry entries[] = {
                {"printf", kPrintf, {kFormat}, serve_printf},
                {"fprintf(stdout)", kFprintf, {kStdout, kFormat}, serve_fprintf},
                {"fprintf(stderr)", kFprintf, {kStderr, kFormat}, serve_fprintf},
            };
            unsigned entry_cases = 0;
            for (const Entry& entry : entries) {
                for (std::size_t f = 0; f < formats.size(); ++f) {
                    if (f % 3 != 0 && formats[f] != "%s") continue;
                    std::vector<Kind> kinds;
                    std::string unsupported;
                    std::string text = formats[f];
                    if (text.back() != '\n') text.push_back('\n');
                    if (!arg_kinds(text, kinds, unsupported) || kinds.size() > 8) continue;
                    for (unsigned set = 0; set < 11; ++set) {
                        for (u32 failing = 0; failing < 2; ++failing) {
                            original.zero_scratch();
                            original.memory.write_block(kFormat, text.c_str(), text.size() + 1);
                            std::vector<Arg> args;
                            bool has_float = false;
                            u32 pool_cursor = kPool;
                            for (std::size_t j = 0; j < kinds.size(); ++j) {
                                Arg a{kinds[j]};
                                switch (kinds[j]) {
                                case Kind::Word: case Kind::DoubleWord: a.integer = integers[(set * 5 + 3 * j) % 12]; break;
                                case Kind::Char: a.integer = 0x41 + (set + j) % 26; break;
                                case Kind::Star: a.integer = (set * 5 + j * 3) % 12; break;
                                case Kind::Double: a.real = reals[(set * 7 + 2 * j) % 20]; has_float = true; break;
                                case Kind::Pointer: {
                                    const std::string& value = strings[(set + j) % 11];
                                    original.memory.write_block(pool_cursor, value.c_str(), value.size() + 1);
                                    a.integer = pool_cursor;
                                    pool_cursor += static_cast<u32>((value.size() + 16u) & ~15u);
                                    break;
                                }
                                }
                                args.push_back(a);
                            }
                            const Placed placed = place(entry.fixed, args);
                            for (std::size_t w = 0; w < placed.overflow.size(); ++w)
                                original.memory.write32(kStack + 8u + static_cast<u32>(w) * 4u, placed.overflow[w]);
                            restore_files();
                            console_result = failing;
                            run_original(placed, has_float, formats[f], entry.address);
                            const u32 original_r3 = original.cpu.gpr[3];
                            const std::string original_text = original.captured;
                            u32 original_second = 0u;
                            if (failing) {  // the error is latched: the same print again returns -1 and writes nothing
                                run_original(placed, has_float, formats[f], entry.address);
                                original_second = original.cpu.gpr[3];
                                require(original.captured.empty(), "a print to an errored FILE wrote");
                            }
                            // The owner.
                            std::vector<u8> before(kWatchedBytes);
                            original.memory.read_block(kWatched, before.data(), before.size());
                            load_registers(original.cpu, placed, has_float);
                            DebugConsole console;
                            // A failing console write is a retail console to the owner: no UART.
                            const u32 bi2 = kWatched + 0x1000u;
                            original.memory.write32(kR13 + 0x5A20u, failing ? bi2 : 0u);
                            original.memory.write32(bi2 + 0x2Cu, 1u);
                            entry.serve(original.cpu, console);
                            const u32 native_first = original.cpu.gpr[3];
                            u32 native_second = 0u;
                            if (failing) {
                                load_registers(original.cpu, placed, has_float);
                                entry.serve(original.cpu, console);
                                native_second = original.cpu.gpr[3];
                            }
                            original.memory.write32(kR13 + 0x5A20u, 0u);
                            original.memory.write32(bi2 + 0x2Cu, 0u);
                            std::vector<u8> after(kWatchedBytes);
                            original.memory.read_block(kWatched, after.data(), after.size());
                            require(before == after, "the printf owner wrote guest memory");
                            ++entry_cases;
                            const bool same_text = failing ? console.stats().suppressed == 1u : console.last_text() == original_text;
                            const bool same_result = native_first == original_r3 && native_second == original_second;
                            if (!same_text || !same_result) {
                                std::string key = std::string(entry.name) + (failing ? " (console error) " : " ") + "differs for: " + formats[f];
                                if (failures[key]++ == 0)
                                    std::fprintf(stderr, "DIFF %s r3 original=%d native=%d\n  original=[%.100s]\n  native  =[%.100s]\n", key.c_str(),
                                                 static_cast<int>(original_r3), static_cast<int>(native_first), original_text.c_str(),
                                                 console.last_text().c_str());
                            }
                        }
                    }
                }
            }
            std::printf("entry points: %u cases (printf, fprintf stdout / stderr; console ok and failing)\n", entry_cases);
        }

        // OSRegisterVersion (0x8037177C): OSReport of "%s" plus newline, with the SDA format string, whole owner against the original,
        // with the console write succeeding and failing.
        {
            unsigned version_cases = 0;
            for (const std::string& version : strings) {
                for (u32 failing = 0; failing < 2; ++failing) {
                    original.zero_scratch();
                    original.memory.write_block(kPool, version.c_str(), version.size() + 1);
                    const Placed placed = place({kPool}, {});
                    restore_files();
                    console_result = failing;
                    run_original(placed, false, version, os_misc::kReportString);
                    const std::string original_text = original.captured;
                    DebugConsole console;
                    const u32 bi2 = kWatched + 0x1000u;
                    original.memory.write32(kR13 + 0x5A20u, failing ? bi2 : 0u);
                    original.memory.write32(bi2 + 0x2Cu, 1u);
                    std::vector<u8> before(kWatchedBytes);
                    original.memory.read_block(kWatched, before.data(), before.size());
                    load_registers(original.cpu, placed, false);
                    os_misc::serve_report_string(original.cpu, console);
                    std::vector<u8> after(kWatchedBytes);
                    original.memory.read_block(kWatched, after.data(), after.size());
                    original.memory.write32(kR13 + 0x5A20u, 0u);
                    original.memory.write32(bi2 + 0x2Cu, 0u);
                    require(before == after, "the OSRegisterVersion owner wrote guest memory");
                    ++version_cases;
                    const bool same = failing ? console.stats().suppressed == 1u : console.last_text() == original_text;
                    if (!same) {
                        std::string key = std::string("OSRegisterVersion") + (failing ? " (console error)" : "") + " differs for: " + version.substr(0, 40);
                        if (failures[key]++ == 0)
                            std::fprintf(stderr, "DIFF %s\n  original=[%.100s]\n  native  =[%.100s]\n", key.c_str(), original_text.c_str(),
                                         console.last_text().c_str());
                    }
                }
            }
            // The control: a wrong version string pointer must show.
            {
                original.zero_scratch();
                original.memory.write_block(kPool, "ABC", 4);
                original.memory.write_block(kPool + 16u, "ABD", 4);
                const Placed placed = place({kPool}, {});
                restore_files();
                console_result = 0u;
                run_original(placed, false, "control", os_misc::kReportString);
                const std::string original_text = original.captured;
                const Placed wrong = place({kPool + 16u}, {});
                DebugConsole console;
                load_registers(original.cpu, wrong, false);
                os_misc::serve_report_string(original.cpu, console);
                require(console.last_text() != original_text, "the OSRegisterVersion comparison cannot see a wrong version string");
            }
            std::printf("OSRegisterVersion: %u cases (console ok and failing) plus a negative control\n", version_cases);
        }

        // Negative controls: the same comparison must catch a native formatter that reads a wrong input.
        {
            struct Control { const char* name; std::string format; std::vector<Arg> args; std::function<void(Cpu&)> perturb; bool floats; };
            original.zero_scratch();
            const std::string text = "abcdef";
            original.memory.write_block(kPool, text.c_str(), text.size() + 1);
            std::vector<Arg> eight;
            for (unsigned k = 0; k < 8; ++k) eight.push_back({Kind::Word, 100u + k});
            const Control controls[] = {
                {"r4 (first word argument)", "%d\n", {{Kind::Word, 1234}}, [](Cpu& c) { c.gpr[4] ^= 1u; }, false},
                {"f1 (first double)", "%f\n", {Arg{Kind::Double, 0, 2.5}}, [](Cpu& c) { c.fpr[1].bits[0] ^= 1ull << 52; }, true},
                {"string byte", "%s\n", {{Kind::Pointer, kPool}}, [](Cpu& c) { c.mem_write8(kPool, 'X'); }, false},
                {"r6 (low word of a 64-bit pair)", "%lld\n", {{Kind::DoubleWord, 0x1122334455667788ull}}, [](Cpu& c) { c.gpr[6] ^= 1u; }, false},
                {"overflow word", "%d%d%d%d%d%d%d%d\n", eight, [](Cpu& c) { c.mem_write32(c.gpr[1] + 8u, 999u); }, false},
                {"the format string", "%d\n", {{Kind::Word, 255}}, [](Cpu& c) { c.mem_write8(kFormat + 1, 'x'); }, false},
            };
            for (const Control& control : controls) {
                original.zero_scratch();
                original.memory.write_block(kPool, text.c_str(), text.size() + 1);
                original.memory.write_block(kFormat, control.format.c_str(), control.format.size() + 1);
                const Placed placed = place({kFormat}, control.args);
                for (std::size_t w = 0; w < placed.overflow.size(); ++w)
                    original.memory.write32(kStack + 8u + static_cast<u32>(w) * 4u, placed.overflow[w]);
                run_original(placed, control.floats, control.format);
                load_registers(original.cpu, placed, control.floats);
                control.perturb(original.cpu);
                ArgCursor cursor(original.cpu, 1u);
                std::string native, why;
                const bool ok = format(original.cpu, kFormat, cursor, native, why);
                require(!ok || native != original.captured, (std::string("negative control not detected: ") + control.name).c_str());
            }
        }
        for (const auto& u : unsupported_seen) std::fprintf(stderr, "unsupported: %s\n", u.c_str());
        unsigned total_failures = 0;
        for (const auto& f : failures) total_failures += f.second;
        std::printf("census: %zu formats, %u cases, %u formats skipped, %u failing cases, %llu original instructions\n",
                    formats.size(), cases, skipped_formats, total_failures,
                    static_cast<unsigned long long>(original.instructions));
        (void)skipped_with_pointer_misuse;
        require(total_failures == 0, "native text differs from the original's");
        std::printf("PASS os report contract: 6 negative controls detected\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL os report contract: %s\n", error.what());
        return 1;
    }
}
