#pragma once

// OSReport (0x803731CC), owned natively (docs/agents/axfx-effects.md, "Step D: OSReport").
//
// Original behaviour (main.dol, PAL GUPP8P): a varargs wrapper. It spills r3..r10 (and f1..f8 when cr1.eq says
// floating-point arguments were passed) into its frame, builds an EABI va_list ({gpr = 1, fpr = 0, overflow area =
// caller's r1 + 8, register save area}) and calls MSL's vprintf (0x803AA414): critical region, __pformatter against the
// stdout FILE (0x80568F20), whose writer reaches the console write routine 0x803AF5A8: the UART bring-up
// (InitializeUART, native_os_debug.h) and WriteUARTN (native_ipl.h) on development hardware. It returns nothing.
//
// Nothing in the game reads the product: the stdout FILE, the UART cells and the formatted buffer are private to this
// path (guest watch over the Westopolis replay: only MSL's stdio rows touch them). The owner therefore reads the format
// string and the arguments from the guest as the EABI places them, formats the line with an MSL-equivalent printf
// (restricted to the conversions the game's format strings use), and hands the text to the host debug console
// (DebugConsole below: counted and stored, echoed to stderr behind SHADOW_DEBUG_CONSOLE=1). It writes no guest memory.
//
// Like the original it prints nothing on a retail console (console type without bit 0x10000000: no UART).
// An unsupported conversion stops the run (the original's behaviour for it is not reproduced).

#include "gekko/types.h"

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>

namespace gekko {
class Cpu;
}

namespace gekko::os_report {

constexpr u32 kOsReport = 0x803731CCu;
constexpr u32 kFprintf = 0x803AA4ACu;  // MSL fprintf(FILE*, format, ...)
constexpr u32 kPrintf = 0x803AA58Cu;   // MSL printf(format, ...)
// The console files of MSL's stdio (the FILE table at 0x80568ED0, 0x50 bytes each): stdin, stdout, stderr.
constexpr u32 kStdout = 0x80568F20u;
constexpr u32 kStderr = 0x80568F70u;

// The host side of the debug console: what OSReport's text becomes.
class DebugConsole {
public:
    struct Stats {
        u64 lines = 0;     // newline-terminated lines (plus a final unterminated one per call)
        u64 bytes = 0;
        u64 calls = 0;     // OSReport calls that produced text
        u64 suppressed = 0;  // calls on a retail console (no UART)
    };
    // Text from one call; `enabled` is whether the console has a UART.
    void write(std::string_view text, bool enabled);
    const Stats& stats() const noexcept { return stats_; }
    // The most recent lines (bounded), oldest first.
    const std::deque<std::string>& recent() const noexcept { return recent_; }
    // The text of the most recent write() (tests and diagnostics).
    const std::string& last_text() const noexcept { return last_text_; }
    void set_echo(bool echo) noexcept { echo_ = echo; }
    // MSL latches a write error in the FILE: after the first failed console write (a console without a UART) every
    // later print to that FILE returns -1 and writes nothing. Index 0 = stdout, 1 = stderr.
    bool file_failed(unsigned file) const noexcept { return failed_[file]; }
    void fail_file(unsigned file) noexcept { failed_[file] = true; }

    static constexpr std::size_t kRecentLines = 256;

private:
    Stats stats_;
    std::deque<std::string> recent_;
    std::string partial_;
    std::string last_text_;
    bool echo_ = false;
    bool failed_[2] = {false, false};
};

// Argument access in the EABI order over the live registers and the caller's overflow area (r1 + 8 at entry).
class ArgCursor {
public:
    ArgCursor(Cpu& cpu, u32 first_gpr_index);
    u32 next_word();
    u64 next_doubleword();     // a 64-bit integer: an even register pair, or an 8-aligned overflow slot
    double next_double();      // f1..f8, then the 8-aligned overflow area
private:
    Cpu& cpu_;
    u32 gpr_;                  // registers consumed, counted from r3
    u32 fpr_ = 0;
    u32 overflow_;
};

// printf over guest memory. False (with `why`) on a conversion or length this formatter does not implement.
// `pieces`, when given, receives the number of non-empty pieces MSL's formatter hands its writer: runs of literal
// text and the output of each conversion.
bool format(Cpu& cpu, u32 format_address, ArgCursor& args, std::string& out, std::string& why,
            unsigned* pieces = nullptr);

void serve_os_report(Cpu& cpu, DebugConsole& console);
// printf(format, ...) and fprintf(FILE*, format, ...) on the console files: r3 = characters written (as MSL returns
// it), or -1 when the console has no UART. A FILE other than stdout / stderr stops the run.
void serve_printf(Cpu& cpu, DebugConsole& console);
void serve_fprintf(Cpu& cpu, DebugConsole& console);

// Installs the owner (System::start).
void install(Cpu& cpu, DebugConsole& console);

struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {{kOsReport, "OSReport"}, {kFprintf, "fprintf"}, {kPrintf, "printf"}};
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

}  // namespace gekko::os_report
