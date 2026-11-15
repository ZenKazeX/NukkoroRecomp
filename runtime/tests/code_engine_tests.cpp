// The data-code engine (include/gekko/code_engine.h): every supported code type against a flat fake memory, the
// ini parser on Reloaded-shaped text, the classification of code that needs a recompiled variant, and the
// zero-cost-when-off property (an engine with nothing configured does nothing and installs nothing).
#include "gekko/code_engine.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

using namespace gekko::codes;

// Guest addresses of the fake memory as offsets from the image base: the test's values are data, not guest code
// the runtime enters (tools/ppcrecomp/liveness_roots.py counts every 0x8xxxxxxx literal under runtime/).
static constexpr unsigned G(unsigned offset) { return 0x80000000u + offset; }

namespace {
int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// 0x80000000..0x80010000 mapped, big-endian bytes.
class Fake final : public CodeMemory {
public:
    std::map<u32, u8> bytes;
    bool mapped(u32 a, std::size_t n) const override { return a >= 0x80000000u && static_cast<u64>(a) + n <= G(0x010000u); }
    u8 read8(u32 a) override { auto it = bytes.find(a); return it == bytes.end() ? 0 : it->second; }
    u16 read16(u32 a) override { return static_cast<u16>(read8(a) << 8 | read8(a + 1)); }
    u32 read32(u32 a) override { return static_cast<u32>(read16(a)) << 16 | read16(a + 2); }
    void write8(u32 a, u8 v) override { bytes[a] = v; }
    void write16(u32 a, u16 v) override { write8(a, v >> 8); write8(a + 1, v & 0xFF); }
    void write32(u32 a, u32 v) override { write16(a, static_cast<u16>(v >> 16)); write16(a + 2, v & 0xFFFF); }
};

Code make(const char* name, const char* lines) {
    Code c;
    c.name = name;
    std::string err;
    check(parse_lines(lines, c.lines, err), std::string("parse ") + name + " " + err);
    return c;
}

// Runs `lines` for `frames` frames on a fresh engine over `mem`.
Engine* run(Fake& mem, const char* lines, int frames = 1, Layout layout = {}) {
    static Engine* e = nullptr;
    delete e;
    e = new Engine(mem);
    auto v = e->configure({make("t", lines)}, layout);
    check(v[0].support == Support::Runs, std::string("runs: ") + v[0].why);
    for (int i = 0; i < frames; ++i) e->run_frame();
    return e;
}
}  // namespace

int main() {
    {  // 04 write32, big-endian layout
        Fake m;
        run(m, "04001000 DEADBEEF");
        check(m.read32(G(0x001000u)) == 0xDEADBEEF && m.bytes[G(0x001000u)] == 0xDE && m.bytes[G(0x001003u)] == 0xEF, "04 writes big-endian");
        run(m, "05000010 11223344");
        check(m.read32(G(0x01000010u)) == 0, "05 targets 0x81xxxxxx (unmapped here: skipped)");
    }
    {  // 00 fill (count = Y+1), 02 fill
        Fake m;
        run(m, "00002000 00000207");
        check(m.read8(G(0x002000u)) == 7 && m.read8(G(0x002002u)) == 7 && m.read8(G(0x002003u)) == 0, "00 fills 3 bytes");
        run(m, "02003000 0001ABCD");
        check(m.read16(G(0x003000u)) == 0xABCD && m.read16(G(0x003002u)) == 0xABCD && m.read16(G(0x003004u)) == 0, "02 fills 2 halfwords");
    }
    {  // 06 string with 9 bytes -> 2 data lines
        Fake m;
        run(m, "06004000 00000009\n41424344 45464748\n49000000 00000000");
        check(m.read8(G(0x004000u)) == 'A' && m.read8(G(0x004008u)) == 'I' && m.read8(G(0x004009u)) == 0, "06 string writes exactly 9 bytes");
    }
    {  // 08 serial: 4 words, address +4, value +0x10
        Fake m;
        run(m, "08005000 00000001\n20030004 00000010");
        check(m.read32(G(0x005000u)) == 1 && m.read32(G(0x005004u)) == 0x11 && m.read32(G(0x00500Cu)) == 0x31 && m.read32(G(0x005010u)) == 0, "08 serial");
    }
    {  // conditionals: 32 eq/ne/gt/lt, 16-bit mask, endif, else
        Fake m;
        m.write32(G(0x006000u), 5);
        run(m, "20006000 00000005\n04006100 11111111\nE2000001 00000000");
        check(m.read32(G(0x006100u)) == 0x11111111, "20 equal true");
        Fake m2; m2.write32(G(0x006000u), 6);
        run(m2, "20006000 00000005\n04006100 11111111\nE2000001 00000000\n04006104 22222222");
        check(m2.read32(G(0x006100u)) == 0 && m2.read32(G(0x006104u)) == 0x22222222, "20 false skips to endif, code after runs");
        run(m2, "22006000 00000005\n04006108 33333333\nE2000001 00000000");
        check(m2.read32(G(0x006108u)) == 0x33333333, "22 not equal");
        run(m2, "24006000 00000005\n0400610C 44444444\nE2000001 00000000");
        check(m2.read32(G(0x00610Cu)) == 0x44444444, "24 greater (unsigned)");
        run(m2, "26006000 00000005\n04006110 55555555\nE2000001 00000000");
        check(m2.read32(G(0x006110u)) == 0, "26 less false");
        Fake m3; m3.write16(G(0x006200u), 0x0123);   // moonjump-style masked test: only bit 0x0020 compared
        run(m3, "28006200 FFDF0020\n04006300 00000001\nE2000001 80008000");
        check(m3.read32(G(0x006300u)) == 1, "28 masked equal (bit set)");
        Fake m4; m4.write16(G(0x006200u), 0x0103);
        run(m4, "28006200 FFDF0020\n04006300 00000001\nE2000001 80008000");
        check(m4.read32(G(0x006300u)) == 0, "28 masked equal (bit clear)");
        run(m4, "28006200 FFDF0020\n04006300 00000001\nE2100001 00000000\n04006304 00000002");
        check(m4.read32(G(0x006304u)) == 2, "endif then code");
        // else: E2 with T=1
        Fake m5; m5.write32(G(0x006000u), 9);
        run(m5, "20006000 00000005\n04006100 00000001\nE2100001 00000000");  // T=1 inverts after popping 1: nothing open, ignored
        // nested: outer true, inner false, E2 000001 closes inner, rest runs
        Fake m6; m6.write32(G(0x006000u), 5);
        run(m6, "20006000 00000005\n20006000 00000006\n04006100 00000001\nE2000001 00000000\n04006104 00000002\nE2000001 00000000");
        check(m6.read32(G(0x006100u)) == 0 && m6.read32(G(0x006104u)) == 2, "nested if: inner false, outer still true");
        // if/else
        Fake m7; m7.write32(G(0x006000u), 9);
        run(m7, "20006000 00000005\n04006100 00000001\nE2100000 00000000\n04006104 00000002\nE2000001 00000000");
        check(m7.read32(G(0x006100u)) == 0 && m7.read32(G(0x006104u)) == 2, "else branch");
        // inactive parent: nested condition not evaluated, writes skipped even in else of the nested if
        Fake m8; m8.write32(G(0x006000u), 9);
        run(m8, "20006000 00000005\n22006000 00000005\n04006100 00000001\nE2100000 00000000\n04006104 00000002\nE2000002 00000000\n04006108 00000003");
        check(m8.read32(G(0x006100u)) == 0 && m8.read32(G(0x006104u)) == 0 && m8.read32(G(0x006108u)) == 3, "if inside a false if stays false (also its else)");
    }
    {  // ba / po: set, load, store, offsets
        Fake m;
        m.write32(G(0x007000u), G(0x008000u));  // a pointer
        run(m, "48000000 80007000\n14000010 CAFEBABE\n4A000000 00000040\n10000000 00000009");   // po=[ptr]; write32 at po+0x10; ...
        check(m.read32(G(0x008010u)) == 0xCAFEBABE, "48 loads po from a pointer; 14 writes at po+offset");
        // ba = 0x80009000 (42), then 04 at ba+8; 40 with +ba: ba = [ba + 4]
        Fake m2; m2.write32(G(0x009004u), G(0x00A000u));
        run(m2, "42000000 80009000\n04000008 12345678\n40010000 00000004\n04000000 AAAAAAAA");
        check(m2.read32(G(0x009008u)) == 0x12345678 && m2.read32(G(0x00A000u)) == 0xAAAAAAAA, "42 sets ba, 40 loads ba=[ba+4]");
        // add: 42 T=1: ba += 0x100
        Fake m3;
        run(m3, "42000000 80001000\n42100000 00000100\n04000000 77777777");
        check(m3.read32(G(0x001100u)) == 0x77777777, "42 add");
        // store ba: [X] = ba
        Fake m4;
        run(m4, "42000000 80003210\n44000000 80004000");
        check(m4.read32(G(0x004000u)) == G(0x003210u), "44 stores ba");
        // E0 sets ba from XXXX0000 (and clears conditions)
        Fake m5;
        run(m5, "E0000000 80008000\n04000000 00000001");
        check(m5.read32(0x80000000) == 1, "E0 ba = 0x80000000");
    }
    {  // gecko registers
        Fake m;
        m.write32(G(0x00B000u), 10);
        run(m, "80000000 00000005\n82200000 8000B000\n86000000 00000003\n84200000 8000B100");  // gr0=5; gr0=[B000]=10 -> add 3 -> store
        check(m.read32(G(0x00B100u)) == 13, "80/82/86/84 chain");
        Fake m2;
        run(m2, "80000001 00000007\n80000002 00000003\n88100001 00000002\n84200001 8000B200");   // gr1=7, gr2=3, gr1 = gr1*gr2
        check(m2.read32(G(0x00B200u)) == 21, "88 mul with a register");
        Fake m3; m3.write32(G(0x00B000u), 0x01020304);
        // 8A: YYYY=4, N=0, K=1  => 8A 0004 0 1
        run(m3, "80000000 8000B000\n80000001 8000C000\n8A000401 00000008");
        check(m3.read32(G(0x00C008u)) == 0x01020304, "8A memory copy");
        // registers persist across frames: counter
        Fake m4;
        Engine* e = run(m4, "80100000 00000001\n84200000 8000D000", 5);
        check(m4.read32(G(0x00D000u)) == 5 && e->gecko_register(0) == 5, "gecko register persists across frames");
    }
    {  // classification
        Layout l = pal_layout();
        Code c = make("asm", "C2206238 00000002\nD00301E0 7FE00735\n60000000 00000000");
        check(analyze(c, l).support == Support::NeedsRecompile, "C2 needs a recompiled variant");
        c = make("patch", "040A53D8 60000000");
        Verdict v = analyze(c, l);
        check(v.support == Support::NeedsRecompile && v.why.find("PowerPC") != std::string::npos, "write into the game's code needs a recompiled variant");
        c = make("data", "045F2BD8 43A00000");
        check(analyze(c, l).support == Support::Runs, "write into data runs");
        c = make("flow", "60000000 00000001");
        check(analyze(c, l).support == Support::Unsupported, "60 is refused, not guessed");
        c = make("short", "06004000 00000009\n41424344 45464748");
        check(analyze(c, l).support == Support::Malformed, "truncated string is malformed");
        // runtime guard: a computed pointer into code is not written
        Fake m;
        Engine e(m);
        e.configure({make("g", "4A000000 80003200\n14000000 11111111")}, [] { Layout x; x.code.push_back({G(0x003100u), G(0x004000u)}); return x; }());
        e.run_frame();
        check(e.stats().code_writes_blocked == 1 && e.stats().writes == 0, "a pointer write into code is blocked at run time");
    }
    {  // unmapped targets are skipped and counted, never fatal
        Fake m;
        Engine* e = run(m, "04200000 11111111\n20200000 00000000\n04001000 00000001\nE2000001 00000000");
        check(e->stats().unmapped_writes == 1 && e->stats().unmapped_reads == 1 && m.read32(G(0x001000u)) == 0, "unmapped: skipped, condition false");
    }
    {  // ini
        const char* ini =
            "[Gecko]\n$=== Cheats ===\n*notes\n$Moonjump [Someone]\n2856ED4E FFDF0020\n045F2BD8 43A00000\nE2000001 80008000\n"
            "*Hold Z\n$No HUD [X]\n0057D7E8 00000000\n$Mute\n0458F99C 00000000\n[Gecko_Enabled]\n$No HUD [X]\n[Other]\n$ignored\n";
        IniResult r = parse_ini(ini);
        check(r.codes.size() == 3, "ini: heading dropped, 3 codes");
        check(r.codes[0].name == "Moonjump [Someone]" && r.codes[0].lines.size() == 3 && !r.codes[0].enabled, "ini: code 1");
        check(r.codes[1].name == "No HUD [X]" && r.codes[1].enabled && r.codes[1].lines[0].a == 0x0057D7E8u, "ini: enabled flag");
        check(r.enabled_names.size() == 1, "ini: enabled list");
    }
    {  // zero cost when off: nothing configured, nothing installed, nothing touched
        Fake m;
        Engine e(m);
        check(!e.active(), "an engine with no codes is inactive");
        e.configure({}, pal_layout());
        check(!e.active(), "configure({}) installs nothing");
        Code off = make("off", "04001000 00000001");
        off.enabled = false;
        e.configure({off}, pal_layout());
        check(!e.active() && m.bytes.empty(), "a disabled code installs nothing");
        Code bad = make("bad", "C0000000 00000001");
        bad.enabled = true;
        e.configure({bad}, pal_layout());
        check(!e.active(), "a refused code installs nothing");
    }
    std::printf(g_failures == 0 ? "code_engine_tests: all passed\n" : "code_engine_tests: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
