// The native owner of OSReport (gekko/native_os_report.h).

#include "gekko/native_os_report.h"

#include "gekko/cpu.h"
#include "gekko/native_os_debug.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gekko::os_report {

// ---- the debug console -----------------------------------------------------------------------------------------------

void DebugConsole::write(std::string_view text, bool enabled) {
    last_text_.assign(text);
    if (!enabled) {
        ++stats_.suppressed;
        return;
    }
    ++stats_.calls;
    stats_.bytes += text.size();
    for (char c : text) {
        if (c == '\n') {
            ++stats_.lines;
            if (echo_) std::fprintf(stderr, "[OSReport] %s\n", partial_.c_str());
            recent_.push_back(partial_);
            if (recent_.size() > kRecentLines) recent_.pop_front();
            partial_.clear();
        } else {
            partial_.push_back(c);
        }
    }
    if (!partial_.empty()) {  // an unterminated tail is a line of its own, as the UART wrote it
        ++stats_.lines;
        if (echo_) std::fprintf(stderr, "[OSReport] %s\n", partial_.c_str());
        recent_.push_back(partial_);
        if (recent_.size() > kRecentLines) recent_.pop_front();
        partial_.clear();
    }
}

// ---- arguments ---------------------------------------------------------------------------------------------------------

ArgCursor::ArgCursor(Cpu& cpu, u32 first_gpr_index) : cpu_(cpu), gpr_(first_gpr_index), overflow_(cpu.gpr[1] + 8u) {}

u32 ArgCursor::next_word() {
    if (gpr_ < 8u) return cpu_.gpr[3 + gpr_++];
    const u32 value = cpu_.mem_read32(overflow_);
    overflow_ += 4u;
    return value;
}

u64 ArgCursor::next_doubleword() {
    if (gpr_ & 1u) ++gpr_;  // pairs are r3:r4, r5:r6, ...
    if (gpr_ + 2u <= 8u) {
        const u64 high = cpu_.gpr[3 + gpr_], low = cpu_.gpr[4 + gpr_];
        gpr_ += 2u;
        return high << 32 | low;
    }
    gpr_ = 8u;
    overflow_ = (overflow_ + 7u) & ~7u;
    const u64 high = cpu_.mem_read32(overflow_), low = cpu_.mem_read32(overflow_ + 4u);
    overflow_ += 8u;
    return high << 32 | low;
}

double ArgCursor::next_double() {
    u64 bits;
    if (fpr_ < 8u) {
        bits = cpu_.fpr[1 + fpr_++].bits[0];
    } else {
        overflow_ = (overflow_ + 7u) & ~7u;
        bits = static_cast<u64>(cpu_.mem_read32(overflow_)) << 32 | cpu_.mem_read32(overflow_ + 4u);
        overflow_ += 8u;
    }
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// ---- printf ------------------------------------------------------------------------------------------------------------

namespace {

constexpr std::size_t kStringLimit = 1u << 16;

bool read_string(Cpu& cpu, u32 address, std::size_t limit, std::string& out, bool stop_at_limit) {
    out.clear();
    for (std::size_t i = 0; i < limit; ++i) {
        const u8 c = cpu.mem_read8(address + static_cast<u32>(i));
        if (c == 0u) return true;
        out.push_back(static_cast<char>(c));
    }
    return stop_at_limit;
}

struct Spec {
    bool minus = false, plus = false, space = false, alt = false, zero = false;
    int width = 0;
    int precision = -1;
    int length = 0;  // 0 none, 1 hh/h/l/z/t, 2 ll/j
    char conversion = 0;
};

void pad(std::string& out, const std::string& body, const Spec& spec, std::size_t prefix, bool zero_ok) {
    const std::size_t width = spec.width > 0 ? static_cast<std::size_t>(spec.width) : 0u;
    if (body.size() >= width) {
        out += body;
    } else if (spec.minus) {
        out += body;
        out.append(width - body.size(), ' ');
    } else if (spec.zero && zero_ok) {
        out += body.substr(0, prefix);
        out.append(width - body.size(), '0');
        out += body.substr(prefix);
    } else {
        out.append(width - body.size(), ' ');
        out += body;
    }
}

std::string digits(u64 value, unsigned base, bool upper) {
    if (value == 0u) return "0";
    std::string reversed;
    const char* alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    while (value != 0u) {
        reversed.push_back(alphabet[value % base]);
        value /= base;
    }
    return std::string(reversed.rbegin(), reversed.rend());
}

}  // namespace

bool format(Cpu& cpu, u32 format_address, ArgCursor& args, std::string& out, std::string& why, unsigned* pieces) {
    out.clear();
    unsigned piece_count = 0;
    bool in_literal = false;
    std::string text;
    if (!read_string(cpu, format_address, kStringLimit, text, false)) {
        why = "unterminated format string";
        return false;
    }
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i++];
        if (c != '%') {
            if (!in_literal) ++piece_count;
            in_literal = true;
            out.push_back(c);
            continue;
        }
        in_literal = false;
        const std::size_t before_conversion = out.size();
        Spec spec;
        for (; i < text.size(); ++i) {
            const char f = text[i];
            if (f == '-') spec.minus = true;
            else if (f == '+') spec.plus = true;
            else if (f == ' ') spec.space = true;
            else if (f == '#') spec.alt = true;
            else if (f == '0') spec.zero = true;
            else break;
        }
        if (i < text.size() && text[i] == '*') {
            const int w = static_cast<int>(args.next_word());
            ++i;
            if (w < 0) { spec.minus = true; spec.width = -w; } else spec.width = w;
        } else {
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') spec.width = spec.width * 10 + (text[i++] - '0');
        }
        if (i < text.size() && text[i] == '.') {
            ++i;
            spec.precision = 0;
            if (i < text.size() && text[i] == '*') {
                const int p = static_cast<int>(args.next_word());
                ++i;
                spec.precision = p < 0 ? -1 : p;
            } else {
                while (i < text.size() && text[i] >= '0' && text[i] <= '9') spec.precision = spec.precision * 10 + (text[i++] - '0');
            }
        }
        if (i < text.size() && (text[i] == 'h' || text[i] == 'l' || text[i] == 'j' || text[i] == 'z' || text[i] == 't')) {
            const char first = text[i++];
            if (first == 'j') spec.length = 2;
            else if (first == 'l' && i < text.size() && text[i] == 'l') { ++i; spec.length = 2; }
            else if (first == 'h' && i < text.size() && text[i] == 'h') { ++i; spec.length = 1; }
            else spec.length = 1;
        }
        if (i >= text.size()) {
            why = "format string ends inside a conversion";
            return false;
        }
        spec.conversion = text[i++];
        const char conv = spec.conversion;
        switch (conv) {
        case '%':
            out.push_back('%');
            break;
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            const bool is_signed = conv == 'd' || conv == 'i';
            u64 raw = spec.length == 2 ? args.next_doubleword() : args.next_word();
            bool negative = false;
            u64 magnitude;
            if (is_signed) {
                const s64 value = spec.length == 2 ? static_cast<s64>(raw) : static_cast<s64>(static_cast<s32>(raw));
                negative = value < 0;
                magnitude = negative ? 0ull - static_cast<u64>(value) : static_cast<u64>(value);
            } else {
                magnitude = spec.length == 2 ? raw : static_cast<u32>(raw);
            }
            const unsigned base = conv == 'o' ? 8u : (conv == 'x' || conv == 'X') ? 16u : 10u;
            std::string number = digits(magnitude, base, conv == 'X');
            if (spec.precision == 0 && magnitude == 0u) number.clear();
            if (spec.precision > 0 && number.size() < static_cast<std::size_t>(spec.precision))
                number.insert(0, static_cast<std::size_t>(spec.precision) - number.size(), '0');
            if (conv == 'o' && spec.alt && (number.empty() || number[0] != '0')) number.insert(0, 1, '0');
            std::string prefix;
            if (is_signed) {
                if (negative) prefix = "-";
                else if (spec.plus) prefix = "+";
                else if (spec.space) prefix = " ";
            } else if ((conv == 'x' || conv == 'X') && spec.alt && magnitude != 0u) {
                prefix = conv == 'x' ? "0x" : "0X";
            }
            pad(out, prefix + number, spec, prefix.size(), spec.precision < 0);
            break;
        }
        case 'c': {
            const std::string body(1, static_cast<char>(args.next_word()));
            Spec plain = spec;
            plain.zero = false;
            pad(out, body, plain, 0, false);
            break;
        }
        case 's': {
            const u32 pointer = args.next_word();
            std::string body;
            if (pointer == 0u) {
                // MSL prints a null %s as nothing (measured against the original: contract test).
            } else if (spec.precision >= 0) {
                if (!read_string(cpu, pointer, static_cast<std::size_t>(spec.precision), body, true)) {
                    why = "unterminated %s";
                    return false;
                }
            } else if (!read_string(cpu, pointer, kStringLimit, body, false)) {
                why = "unterminated %s";
                return false;
            }
            Spec plain = spec;
            plain.zero = false;
            pad(out, body, plain, 0, false);
            break;
        }
        case 'p': {
            const u32 pointer = args.next_word();
            Spec as_hex = spec;
            as_hex.alt = true;
            std::string number = digits(pointer, 16u, false);
            if (number.size() < 8u) number.insert(0, 8u - number.size(), '0');  // MSL: 0x%08x, also for null
            if (spec.precision > 0 && number.size() < static_cast<std::size_t>(spec.precision))
                number.insert(0, static_cast<std::size_t>(spec.precision) - number.size(), '0');
            pad(out, "0x" + number, as_hex, 2u, spec.precision < 0);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            const double value = args.next_double();
            char pattern[32];
            int n = 0;
            pattern[n++] = '%';
            if (spec.minus) pattern[n++] = '-';
            if (spec.plus) pattern[n++] = '+';
            if (spec.space) pattern[n++] = ' ';
            if (spec.alt) pattern[n++] = '#';
            if (spec.zero) pattern[n++] = '0';
            pattern[n++] = '*';
            pattern[n++] = '.';
            pattern[n++] = '*';
            pattern[n++] = conv;
            pattern[n] = 0;
            char buffer[512];
            const int precision = spec.precision < 0 ? 6 : spec.precision;
            if (std::isnan(value) || std::isinf(value)) {
                // MSL spells them in lower case for f / e / g and upper case for F / E / G (measured: contract test).
                std::string body = std::isnan(value) ? "nan" : "inf";
                if (conv == 'F' || conv == 'E' || conv == 'G') body = std::isnan(value) ? "NAN" : "INF";
                // No '+' or ' ' sign for them, and the 0 flag pads with zeros (measured: contract test).
                std::size_t prefix = 0;
                if (std::isinf(value) && std::signbit(value)) { body.insert(0, "-"); prefix = 1; }
                pad(out, body, spec, prefix, true);
                break;
            }
            // MSL rounds 0.5 to 1 at precision 0 where the host rounds the tie to even (measured: contract test).
            const double rounded = ((conv == 'f' || conv == 'F') && precision == 0 && std::fabs(value) == 0.5)
                                       ? std::copysign(1.0, value) : value;
            const int length = std::snprintf(buffer, sizeof(buffer), pattern, spec.width, precision, rounded);
            if (length < 0 || length >= static_cast<int>(sizeof(buffer))) {
                why = "floating-point conversion too long";
                return false;
            }
            std::string rendered(buffer, static_cast<std::size_t>(length));
            if (conv == 'f' || conv == 'F') {
                // MSL's decimal conversion keeps 32 significant digits and writes zeros after them
                // (measured against the original: contract test, 1e300 and 1.7e40).
                std::size_t begin = 0;
                while (begin < rendered.size() && (rendered[begin] < '0' || rendered[begin] > '9')) ++begin;
                std::size_t end = begin;
                while (end < rendered.size() && rendered[end] >= '0' && rendered[end] <= '9') ++end;
                if (end - begin > 32u) {
                    const bool round_up = rendered[begin + 32u] >= '5';  // the 33rd digit rounds the 32nd
                    rendered.replace(begin + 32u, end - begin - 32u, end - begin - 32u, '0');
                    if (round_up) {
                        std::size_t at = begin + 32u;
                        while (at > begin && rendered[at - 1u] == '9') rendered[--at] = '0';
                        if (at > begin) ++rendered[at - 1u];
                        else rendered.insert(begin, 1, '1');
                    }
                }
            }
            out += rendered;
            break;
        }
        default:
            why = std::string("unsupported conversion %") + conv;
            return false;
        }
        if (out.size() > before_conversion) ++piece_count;
    }
    if (pieces != nullptr) *pieces = piece_count;
    return true;
}


namespace {
constexpr std::size_t kStdoutBuffer = 0x100;  // FILE.buffer_size of stdout, in the pinned image

bool console_enabled(Cpu& cpu) { return (os_debug::console_type(cpu) & os_debug::kDevelopmentBit) != 0u; }

// One print to a console FILE (0 = stdout, 1 = stderr). With a UART the text is written and r3 is its length. Without
// one the first write fails and latches the FILE's error; later prints return -1 and write nothing. The failing
// call itself returns the length when the failure comes at the end (stdout is line buffered with 256 bytes: a text
// that fits is flushed after the formatter finished), and -1 when the formatter meets it (a longer text flushes
// in the middle, as does a newline before the end; stderr is unbuffered, so a second piece of text finds the failure) - measured against the original: contract test.
void serve_formatted(Cpu& cpu, DebugConsole& console, unsigned file, u32 format_address, u32 first_argument,
                     const char* who) {
    ArgCursor args(cpu, first_argument);
    std::string text, why;
    unsigned pieces = 0;
    if (!format(cpu, format_address, args, text, why, &pieces)) {
        cpu.request_stop(StopReason::ExecutionFault, std::string(who) + ": " + why);
        return;
    }
    if (console.file_failed(file)) {
        cpu.gpr[3] = 0xFFFFFFFFu;
        return;
    }
    const bool enabled = console_enabled(cpu);
    console.write(text, enabled);
    if (!enabled) {
        console.fail_file(file);
        // stdout also flushes at a newline in the middle of the text; the final newline's flush comes after the formatter.
        const std::size_t newline = text.find('\n');
        const bool met_by_formatter =
            file == 1u ? pieces > 1u
                       : text.size() > kStdoutBuffer || (newline != std::string::npos && newline + 1 != text.size());
        cpu.gpr[3] = met_by_formatter ? 0xFFFFFFFFu : static_cast<u32>(text.size());
        return;
    }
    cpu.gpr[3] = static_cast<u32>(text.size());
}
}  // namespace

void serve_os_report(Cpu& cpu, DebugConsole& console) { serve_formatted(cpu, console, 0u, cpu.gpr[3], 1u, "OSReport"); }

void serve_printf(Cpu& cpu, DebugConsole& console) { serve_formatted(cpu, console, 0u, cpu.gpr[3], 1u, "printf"); }

void serve_fprintf(Cpu& cpu, DebugConsole& console) {
    const u32 file = cpu.gpr[3];
    if (file != kStdout && file != kStderr) {
        char detail[80];
        std::snprintf(detail, sizeof(detail), "fprintf to FILE 0x%08X: only the console files are served", file);
        cpu.request_stop(StopReason::ExecutionFault, detail);
        return;
    }
    serve_formatted(cpu, console, file == kStdout ? 0u : 1u, cpu.gpr[4], 2u, "fprintf");
}

void install(Cpu& cpu, DebugConsole& console) {
    if (const char* echo = std::getenv("SHADOW_DEBUG_CONSOLE")) console.set_echo(echo[0] == '1');
    DebugConsole* const sink = &console;
    cpu.install_hle(kOsReport, [sink](Cpu& c) {
        serve_os_report(c, *sink);
        return true;  // an owner never declines
    }, "OSReport (native)");
    cpu.install_hle(kPrintf, [sink](Cpu& c) {
        serve_printf(c, *sink);
        return true;
    }, "printf (native)");
    cpu.install_hle(kFprintf, [sink](Cpu& c) {
        serve_fprintf(c, *sink);
        return true;
    }, "fprintf (native)");
}

}  // namespace gekko::os_report
