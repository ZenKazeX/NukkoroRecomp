#include "gekko/code_engine.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace gekko::codes {
namespace {

constexpr u32 kDefaultBase = 0x80000000u;

// ---- line decoding ------------------------------------------------------------------------------------------------
u32 off25(u32 a) { return a & 0x01FFFFFFu; }
bool po_flag(u32 a) { return (a & 0x10000000u) != 0u; }
u32 nib(u32 v, unsigned n) { return (v >> (n * 4u)) & 0xFu; }  // nibble n, 0 = least significant

// The code type with the po bit (0x10) and the 0x01000000 address bit removed. 0xF0.. is handled before.
unsigned op_of(u32 a) { return (a >> 24) & 0xEEu; }

std::size_t string_lines(u32 bytes) { return (static_cast<std::size_t>(bytes) + 7u) / 8u; }

enum class Kind { Write8, Write16, Write32, String, Serial, Cond, Ba, Po, Reg, EndIf, End, Recompile, Unsupported };
struct Decoded {
    Kind kind = Kind::Unsupported;
    std::size_t length = 1;  // lines, including this one
    const char* why = "";
};

Decoded decode(u32 a, u32 b) {
    Decoded d;
    const unsigned b0 = a >> 24;
    if (b0 >= 0xF0u) {
        if (b0 == 0xF0u) { d.kind = Kind::End; return d; }
        d.kind = Kind::Recompile;  // F2 / F4 / F6: Gecko 1.8 ASM insert / memory search
        d.why = "F2/F4/F6 (ASM insert with checksum / memory search)";
        if (b0 != 0xF2u && b0 != 0xF4u && b0 != 0xF6u) { d.kind = Kind::Unsupported; d.why = "code type F1/F3/F5/F7+"; }
        return d;
    }
    const unsigned op = op_of(a);
    switch (op) {
        case 0x00: d.kind = Kind::Write8; return d;
        case 0x02: d.kind = Kind::Write16; return d;
        case 0x04: d.kind = Kind::Write32; return d;
        case 0x06: d.kind = Kind::String; d.length = 1 + string_lines(b); return d;
        case 0x08: d.kind = Kind::Serial; d.length = 2; return d;
        case 0x20: case 0x22: case 0x24: case 0x26: case 0x28: case 0x2A: case 0x2C: case 0x2E:
            d.kind = Kind::Cond; return d;
        case 0x40: case 0x42: case 0x44: d.kind = Kind::Ba; return d;
        case 0x48: case 0x4A: case 0x4C: d.kind = Kind::Po; return d;
        case 0x46: case 0x4E:
            d.why = "46/4E (address of the next code line: the code list is not in guest memory here)";
            return d;
        case 0x80: case 0x82: case 0x84: case 0x86: case 0x88: case 0x8A: case 0x8C: d.kind = Kind::Reg; return d;
        case 0xC0: case 0xC2: case 0xC4: case 0xC6: case 0xD2: case 0xD4: case 0xD6:
            if (op == 0xC4 || op == 0xD4) { d.why = "code type C4/D4"; return d; }
            d.kind = Kind::Recompile;
            d.why = op == 0xC0 ? "C0 (execute PowerPC)" : op == 0xC2 ? "C2 (insert PowerPC)"
                  : op == 0xC6 ? "C6 (write a branch)" : "D2/D6 (PowerPC)";
            return d;
        case 0xE0: case 0xE2: d.kind = Kind::EndIf; return d;
        case 0x60: case 0x62: case 0x64: case 0x66: case 0x68:
            d.why = "60-68 (repeat / goto / gosub): not implemented, not guessed";
            return d;
        case 0xA0: case 0xA2: case 0xA4: case 0xA6: case 0xA8: case 0xAA: case 0xAC: case 0xAE:
        case 0xB0: case 0xB2: case 0xB4: case 0xB6:
            d.why = "A0-B6 (register / counter conditionals): not implemented, not guessed";
            return d;
        case 0xCC: case 0xCE: case 0xDE:
            d.why = "CC/CE/DE (switch / range check): not implemented";
            return d;
        default:
            d.why = "unknown code type";
            return d;
    }
}

bool overlaps(const Layout& l, u32 begin, u64 size) {
    const u64 end = static_cast<u64>(begin) + size;
    for (const Range& r : l.code)
        if (begin < r.end && end > r.begin) return true;
    return false;
}

std::string hex8(u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", v);
    return buf;
}

}  // namespace

bool Layout::in_code(u32 address, std::size_t size) const noexcept { return overlaps(*this, address, size); }

Layout pal_layout() {
    // The PAL DOL's two text sections (values only: the engine refuses writes into them, it never enters them).
    constexpr u32 kText0 = 0x80003100u;  // liveness-roots: value-only
    constexpr u32 kText1 = 0x80008D40u;  // liveness-roots: value-only
    Layout l;
    l.code.push_back({kText0, kText0 + 9472u});
    l.code.push_back({kText1, kText1 + 4857632u});
    return l;
}

const char* support_name(Support s) noexcept {
    switch (s) {
        case Support::Runs: return "runs";
        case Support::NeedsRecompile: return "needs-recompile";
        case Support::Unsupported: return "unsupported";
        case Support::Malformed: return "malformed";
    }
    return "?";
}

Verdict analyze(const Code& code, const Layout& layout) {
    Verdict v;
    if (code.lines.empty()) {
        v.support = Support::Malformed;
        v.why = "no code lines";
        return v;
    }
    bool ba_known = true;
    u32 ba = kDefaultBase;
    std::size_t i = 0;
    while (i < code.lines.size()) {
        const u32 a = code.lines[i].a, b = code.lines[i].b;
        const Decoded d = decode(a, b);
        v.line = i;
        auto fail = [&](Support s, const std::string& why) {
            v.support = s;
            v.why = "line " + std::to_string(i + 1) + " (" + hex8(a) + " " + hex8(b) + "): " + why;
            return v;
        };
        if (d.kind == Kind::Recompile) return fail(Support::NeedsRecompile, d.why);
        if (d.kind == Kind::Unsupported) return fail(Support::Unsupported, d.why);
        if (d.kind == Kind::End) break;
        if (i + d.length > code.lines.size()) return fail(Support::Malformed, "the code is cut short (string / serial data lines missing)");
        u64 size = 0;
        switch (d.kind) {
            case Kind::Write8: size = static_cast<u64>(b >> 8) + 1u; break;
            case Kind::Write16: size = (static_cast<u64>(b >> 16) + 1u) * 2u; break;
            case Kind::Write32: size = 4; break;
            case Kind::String: size = b; break;
            case Kind::Serial: {
                const u32 t = code.lines[i + 1].a >> 28, n = (code.lines[i + 1].a >> 16) & 0xFFFu, z = code.lines[i + 1].a & 0xFFFFu;
                if (t > 2u) return fail(Support::Malformed, "serial code with a size other than 8 / 16 / 32 bits");
                size = static_cast<u64>(n) * z + (static_cast<u64>(1) << t);
                break;
            }
            default: break;
        }
        if (size != 0 && !po_flag(a) && ba_known && layout.in_code(ba + off25(a), static_cast<std::size_t>(size)))
            return fail(Support::NeedsRecompile, "writes into the game's PowerPC at " + hex8(ba + off25(a)) + " (a recompiled variant is needed)");
        // Track a literal base address for the check above; anything computed makes it unknown.
        if (d.kind == Kind::Ba) {
            const unsigned op = op_of(a);
            if (op == 0x42 && nib(a, 5) == 0u && nib(a, 4) == 0u && nib(a, 3) == 0u) ba = b, ba_known = true;
            else if (op != 0x44) ba_known = false;
        } else if (d.kind == Kind::EndIf && (b >> 16) != 0u) {
            ba = (b >> 16) << 16;
            ba_known = true;
        }
        i += d.length;
    }
    return v;
}

// ---- ini ---------------------------------------------------------------------------------------------------------------
namespace {
std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}
bool is_hex(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}
// "XXXXXXXX XXXXXXXX" (or 16 digits); true when the line is a code line.
bool code_line(const std::string& raw, Line& out) {
    std::string t;
    for (char c : trim(raw)) {
        if (c == ' ' || c == '\t') continue;
        t.push_back(c);
    }
    if (t.size() != 16 || !is_hex(t)) return false;
    out.a = static_cast<u32>(std::stoull(t.substr(0, 8), nullptr, 16));
    out.b = static_cast<u32>(std::stoull(t.substr(8, 8), nullptr, 16));
    return true;
}
}  // namespace

IniResult parse_ini(const std::string& text) {
    IniResult r;
    enum class Sec { None, Gecko, Enabled, Disabled } sec = Sec::None;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty()) continue;
        if (line.front() == '[') {
            std::string name = line.substr(1, line.find(']') == std::string::npos ? std::string::npos : line.find(']') - 1);
            sec = name == "Gecko" ? Sec::Gecko : name == "Gecko_Enabled" ? Sec::Enabled : name == "Gecko_Disabled" ? Sec::Disabled : Sec::None;
            continue;
        }
        if (sec == Sec::None) continue;
        if (sec == Sec::Gecko) {
            if (line.front() == '$') {
                Code c;
                c.name = trim(line.substr(1));
                c.enabled = false;
                r.codes.push_back(std::move(c));
            } else if (line.front() == '*') {
                continue;
            } else {
                Line l;
                if (code_line(line, l)) {
                    if (r.codes.empty()) r.warnings.push_back("code line before any $name: " + line);
                    else r.codes.back().lines.push_back(l);
                } else {
                    r.warnings.push_back("ignored line: " + line);
                }
            }
        } else if (sec == Sec::Enabled) {
            if (line.front() == '$') r.enabled_names.push_back(trim(line.substr(1)));
        }
    }
    // Headings without lines are not codes.
    r.codes.erase(std::remove_if(r.codes.begin(), r.codes.end(), [](const Code& c) { return c.lines.empty(); }), r.codes.end());
    for (Code& c : r.codes)
        c.enabled = std::find(r.enabled_names.begin(), r.enabled_names.end(), c.name) != r.enabled_names.end();
    return r;
}

bool parse_lines(const std::string& text, std::vector<Line>& out, std::string& error) {
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line.front() == '*' || line.front() == '#') continue;
        Line l;
        if (!code_line(line, l)) {
            error = "not a code line (expected 16 hex digits): " + line;
            return false;
        }
        out.push_back(l);
    }
    return true;
}

// ---- the engine -------------------------------------------------------------------------------------------------------
std::vector<Verdict> Engine::configure(const std::vector<Code>& codes, const Layout& layout) {
    layout_ = layout;
    programs_.clear();
    names_.clear();
    std::fill(std::begin(gr_), std::end(gr_), 0u);
    stats_ = Stats{};
    std::vector<Verdict> verdicts;
    for (const Code& c : codes) {
        Verdict v = analyze(c, layout);
        verdicts.push_back(v);
        if (!c.enabled || v.support != Support::Runs) continue;
        programs_.push_back({c.name, c.lines});
        names_.push_back(c.name);
    }
    return verdicts;
}

void Engine::run_frame() {
    ++stats_.frames;
    for (const Program& p : programs_) run_program(p);
}

namespace {
struct Exec {
    CodeMemory& m;
    const Layout& layout;
    Stats& st;
    u32 (&gr)[16];
    u32 ba = kDefaultBase, po = kDefaultBase;
    std::vector<bool> stack;  // one entry per open if: did its condition hold (else flips it)

    bool active() const {
        for (bool c : stack)
            if (!c) return false;
        return true;
    }
    void bad(u32 address) {
        if (st.first_bad_address == 0u) st.first_bad_address = address;
    }
    bool rd(u32 address, unsigned size, u32& v) {
        if (!m.mapped(address, size)) { ++st.unmapped_reads; bad(address); v = 0; return false; }
        v = size == 1 ? m.read8(address) : size == 2 ? m.read16(address) : m.read32(address);
        return true;
    }
    void wr(u32 address, unsigned size, u32 v) {
        if (layout.in_code(address, size)) { ++st.code_writes_blocked; bad(address); return; }
        if (!m.mapped(address, size)) { ++st.unmapped_writes; bad(address); return; }
        const u32 old = size == 1 ? m.read8(address) : size == 2 ? m.read16(address) : m.read32(address);
        if (size == 1) m.write8(address, static_cast<u8>(v));
        else if (size == 2) m.write16(address, static_cast<u16>(v));
        else m.write32(address, v);
        ++st.writes;
        const u32 mask = size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
        if ((old & mask) != (v & mask)) ++st.writes_changed;
    }
    void pop(unsigned n) {
        while (n-- != 0u && !stack.empty()) stack.pop_back();
    }
    u32 arith(unsigned op, u32 x, u32 y) {
        switch (op) {
            case 0: return x + y;
            case 1: return x * y;
            case 2: return x | y;
            case 3: return x & y;
            case 4: return x ^ y;
            case 5: return (y & 0x20u) != 0u ? 0u : x << (y & 31u);
            case 6: return (y & 0x20u) != 0u ? 0u : x >> (y & 31u);
            case 7: return std::rotl(x, static_cast<int>(y & 31u));
            case 8: return static_cast<u32>(static_cast<std::int32_t>(x) >> ((y & 0x20u) != 0u ? 31u : (y & 31u)));
            case 9: return std::bit_cast<u32>(std::bit_cast<float>(x) + std::bit_cast<float>(y));
            case 10: return std::bit_cast<u32>(std::bit_cast<float>(x) * std::bit_cast<float>(y));
            default: return x;
        }
    }
};
}  // namespace

void Engine::run_program(const Program& p) {
    Exec e{*memory_, layout_, stats_, gr_};
    const std::vector<Line>& L = p.lines;
    std::size_t i = 0;
    while (i < L.size()) {
        const u32 a = L[i].a, b = L[i].b;
        const Decoded d = decode(a, b);
        if (d.kind == Kind::End) break;
        ++stats_.lines_run;
        const std::size_t next = i + d.length;
        const bool po_f = po_flag(a);
        const u32 src = po_f ? e.po : e.ba;
        switch (d.kind) {
            case Kind::EndIf: {
                e.pop(a & 0xFFu);
                if (nib(a, 5) == 1u && !e.stack.empty()) e.stack.back() = !e.stack.back();
                if ((b >> 16) != 0u) e.ba = (b >> 16) << 16;
                if ((b & 0xFFFFu) != 0u) e.po = (b & 0xFFFFu) << 16;
                if (op_of(a) == 0xE0) e.stack.clear();
                break;
            }
            case Kind::Cond: {
                if ((a & 1u) != 0u) e.pop(1);
                const unsigned op = op_of(a);
                const u32 address = src + (a & 0x01FFFFFEu);
                bool result = false;
                if (!e.active()) {
                    e.stack.push_back(true);  // a skipped if still opens a level; its outcome cannot matter
                    break;
                }
                const bool is16 = op >= 0x28u;
                const unsigned rel = (op >> 1) & 3u;  // 0 eq, 1 ne, 2 gt, 3 lt
                u32 mem = 0, rhs = 0;
                const bool ok = e.rd(address, is16 ? 2u : 4u, mem);
                if (is16) {
                    mem &= ~(b >> 16) & 0xFFFFu;
                    rhs = b & 0xFFFFu;
                } else {
                    rhs = b;
                }
                if (ok) result = rel == 0 ? mem == rhs : rel == 1 ? mem != rhs : rel == 2 ? mem > rhs : mem < rhs;
                ++(result ? stats_.conditions_true : stats_.conditions_false);
                e.stack.push_back(result);
                break;
            }
            default: {
                if (!e.active()) break;
                const u32 address = src + off25(a);
                switch (d.kind) {
                    case Kind::Write8: {
                        const u32 count = (b >> 8) + 1u;
                        for (u32 k = 0; k < count; ++k) e.wr(address + k, 1, b & 0xFFu);
                        break;
                    }
                    case Kind::Write16: {
                        const u32 count = (b >> 16) + 1u;
                        for (u32 k = 0; k < count; ++k) e.wr(address + k * 2u, 2, b & 0xFFFFu);
                        break;
                    }
                    case Kind::Write32: e.wr(address, 4, b); break;
                    case Kind::String: {
                        for (u32 k = 0; k < b; ++k) {
                            const Line& dl = L[i + 1 + k / 8u];
                            const u32 word = (k % 8u) < 4u ? dl.a : dl.b;
                            e.wr(address + k, 1, (word >> (24u - 8u * (k % 4u))) & 0xFFu);
                        }
                        break;
                    }
                    case Kind::Serial: {
                        const Line& s = L[i + 1];
                        const u32 t = s.a >> 28, n = (s.a >> 16) & 0xFFFu, z = s.a & 0xFFFFu;
                        u32 value = b;
                        for (u32 k = 0; k <= n; ++k) {
                            e.wr(address + k * z, 1u << t, value);
                            value += s.b;
                        }
                        break;
                    }
                    case Kind::Ba:
                    case Kind::Po: {
                        const unsigned op = op_of(a);
                        const u32 T = nib(a, 5), Y = nib(a, 4), Z = nib(a, 3), N = nib(a, 0);
                        const u32 where = (Y != 0u ? src : 0u) + (Z != 0u ? e.gr[N] : 0u) + b;
                        u32& reg = d.kind == Kind::Ba ? e.ba : e.po;
                        const unsigned kindop = op & 0x06u;  // 0 load, 2 set, 4 store
                        if (kindop == 0u) {
                            u32 v = 0;
                            e.rd(where, 4, v);
                            reg = T != 0u ? reg + v : v;
                        } else if (kindop == 2u) {
                            reg = T != 0u ? reg + where : where;
                        } else {
                            e.wr(where, 4, reg);
                        }
                        break;
                    }
                    case Kind::Reg: {
                        const unsigned op = op_of(a);
                        const u32 N = nib(a, 0);
                        u32& r = e.gr[N];
                        if (op == 0x80u) {
                            const u32 v = b + (nib(a, 4) != 0u ? src : 0u);
                            r = nib(a, 5) != 0u ? r + v : v;
                        } else if (op == 0x82u) {
                            const unsigned size = nib(a, 5) == 0u ? 1u : nib(a, 5) == 1u ? 2u : 4u;
                            u32 v = 0;
                            e.rd(b + (nib(a, 4) != 0u ? src : 0u), size, v);
                            r = v;
                        } else if (op == 0x84u) {
                            const u32 T = nib(a, 5), sel = nib(a, 4), count = ((a >> 4) & 0xFFFu) + 1u;
                            const unsigned size = T == 0u ? 1u : T == 1u ? 2u : 4u;
                            const u32 base = b + (sel != 0u ? src : 0u);
                            for (u32 k = 0; k < count; ++k) e.wr(base + k * size, size, r);
                        } else if (op == 0x86u || op == 0x88u) {
                            const u32 T = nib(a, 5), form = nib(a, 4) & 3u;
                            const u32 operand = op == 0x86u ? b : e.gr[b & 15u];
                            u32 x = r, y = operand;
                            if (form == 1u || form == 3u) e.rd(r, 4, x);
                            if (form >= 2u) e.rd(operand, 4, y);
                            const u32 res = e.arith(T, x, y);
                            if (form == 1u || form == 3u) e.wr(r, 4, res);
                            else r = res;
                        } else if (op == 0x8Au || op == 0x8Cu) {
                            const u32 len = (a >> 8) & 0xFFFFu, Nn = nib(a, 1), K = nib(a, 0);
                            u32 from, to;
                            if (op == 0x8Au) {
                                from = e.gr[Nn];
                                to = (K == 0xFu ? src : e.gr[K]) + b;
                            } else {
                                from = (Nn == 0xFu ? src : e.gr[Nn]) + b;
                                to = e.gr[K];
                            }
                            if (!e.m.mapped(from, len)) { ++stats_.unmapped_reads; e.bad(from); break; }
                            if (layout_.in_code(to, len)) { ++stats_.code_writes_blocked; e.bad(to); break; }
                            if (!e.m.mapped(to, len)) { ++stats_.unmapped_writes; e.bad(to); break; }
                            std::vector<u8> tmp(len);
                            for (u32 k = 0; k < len; ++k) tmp[k] = e.m.read8(from + k);
                            for (u32 k = 0; k < len; ++k) e.m.write8(to + k, tmp[k]);
                            stats_.writes += len;
                            stats_.writes_changed += len;
                        }
                        break;
                    }
                    default: break;
                }
                break;
            }
        }
        i = next;
    }
}

std::string Engine::report_json() const {
    std::string s = "{\"codes\":[";
    for (std::size_t i = 0; i < names_.size(); ++i) {
        if (i != 0) s += ',';
        s += '"';
        for (char c : names_[i]) {
            if (c == '"' || c == '\\') s += '\\';
            if (static_cast<unsigned char>(c) >= 0x20) s += c;
        }
        s += '"';
    }
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "],\"frames\":%llu,\"lines_run\":%llu,\"writes\":%llu,\"writes_changed\":%llu,\"conditions_true\":%llu,"
                  "\"conditions_false\":%llu,\"unmapped_writes\":%llu,\"unmapped_reads\":%llu,\"code_writes_blocked\":%llu}",
                  static_cast<unsigned long long>(stats_.frames), static_cast<unsigned long long>(stats_.lines_run),
                  static_cast<unsigned long long>(stats_.writes), static_cast<unsigned long long>(stats_.writes_changed),
                  static_cast<unsigned long long>(stats_.conditions_true), static_cast<unsigned long long>(stats_.conditions_false),
                  static_cast<unsigned long long>(stats_.unmapped_writes), static_cast<unsigned long long>(stats_.unmapped_reads),
                  static_cast<unsigned long long>(stats_.code_writes_blocked));
    return s + buf;
}

}  // namespace gekko::codes
