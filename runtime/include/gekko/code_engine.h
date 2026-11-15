#pragma once

// The native engine for data-writing Gecko / Action Replay codes (docs/mods/codemods/ENGINE.md).
//
// A Gecko code is a list of 8-byte lines the console's code handler ran once per frame from a hook. Codes that only
// write data (RAM, bss, heap, a pointer target; conditionally or every frame) need no PowerPC: this engine runs them
// natively, once per frame, through the guest memory the game's translated code uses. Codes that execute or patch
// PowerPC (C0 / C2 / C6 / D2 / D6, or a write into the DOL's code) cannot run in a static recompilation: they are
// classified NeedsRecompile and never installed.
//
// Nothing is installed in the game's frame loop unless at least one runnable code is configured.
//
// The engine knows memory only through CodeMemory, so it is testable without the game and the same object serves the
// guest window (code_engine_guest.h).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gekko::codes {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// What the engine needs from memory. Addresses are guest addresses; values are the guest's (big-endian layout
// already applied: read32 returns the word the game's lwz would load).
class CodeMemory {
public:
    virtual ~CodeMemory() = default;
    virtual bool mapped(u32 address, std::size_t size) const = 0;  // the whole range is committed guest memory
    virtual u8 read8(u32 address) = 0;
    virtual u16 read16(u32 address) = 0;
    virtual u32 read32(u32 address) = 0;
    virtual void write8(u32 address, u8 value) = 0;
    virtual void write16(u32 address, u16 value) = 0;
    virtual void write32(u32 address, u32 value) = 0;
};

struct Line {
    u32 a = 0;
    u32 b = 0;
};

struct Code {
    std::string name;
    std::vector<Line> lines;
    bool enabled = true;  // from [Gecko_Enabled] when parsed from an ini; the loader sets it from the mod's options
};

struct Range {
    u32 begin = 0;
    u32 end = 0;  // exclusive
};

// Where the game's PowerPC lives: a data write that lands here changes nothing in a static recompilation.
struct Layout {
    std::vector<Range> code;
    bool in_code(u32 address, std::size_t size) const noexcept;
};
// The PAL DOL's text sections (extract/dol-sections.json), for callers without the file.
Layout pal_layout();

enum class Support {
    Runs,            // the engine runs it
    NeedsRecompile,  // changes or executes PowerPC: only a recompiled variant can carry it
    Unsupported,     // a code type this engine version does not implement (never guessed)
    Malformed,       // lines that cannot be parsed (truncated string, empty)
};
const char* support_name(Support s) noexcept;

struct Verdict {
    Support support = Support::Runs;
    std::string why;      // empty when Runs
    std::size_t line = 0; // index of the line the verdict is about
};

// Static classification of one code (what configure() decides with).
Verdict analyze(const Code& code, const Layout& layout);

// ---- Dolphin .ini [Gecko] ---------------------------------------------------------------------------------------------
struct IniResult {
    std::vector<Code> codes;                    // in file order; `enabled` set from [Gecko_Enabled]
    std::vector<std::string> enabled_names;     // [Gecko_Enabled] entries
    std::vector<std::string> warnings;
};
// Parses the [Gecko], [Gecko_Enabled] and [Gecko_Disabled] sections of a Dolphin game ini; `$Name` starts a code,
// `*` lines are notes, "XXXXXXXX XXXXXXXX" lines are code lines. Other sections are ignored. Headings without lines
// (Reloaded's "$=== Cheats ===") are dropped.
IniResult parse_ini(const std::string& text);
// Plain code lines ("044978F0 60000000", one per line, 16 hex digits with or without a space). False on a bad line.
bool parse_lines(const std::string& text, std::vector<Line>& out, std::string& error);

// ---- the engine -------------------------------------------------------------------------------------------------------
struct Stats {
    u64 frames = 0;
    u64 lines_run = 0;
    u64 writes = 0;          // stores performed
    u64 writes_changed = 0;  // of those, stores that changed the stored value
    u64 conditions_true = 0;
    u64 conditions_false = 0;
    u64 unmapped_writes = 0;  // target not committed guest memory: skipped
    u64 unmapped_reads = 0;   // a read the code needed: the condition is false / the register is 0
    u64 code_writes_blocked = 0;  // a write whose target is in the Layout's code ranges: skipped
    u32 first_bad_address = 0;
};

class Engine {
public:
    Engine(CodeMemory& memory) : memory_(&memory) {}

    // Replaces the code set with the enabled, runnable codes of `codes`; returns one verdict per input code (disabled
    // codes get Runs / "disabled" only if they would run). Codes that do not run are NOT installed.
    std::vector<Verdict> configure(const std::vector<Code>& codes, const Layout& layout);
    bool active() const noexcept { return !programs_.empty(); }
    std::size_t installed() const noexcept { return programs_.size(); }

    // One pass of every installed code, in order: what the console's code handler ran once per frame.
    void run_frame();

    const Stats& stats() const noexcept { return stats_; }
    const std::vector<std::string>& installed_names() const noexcept { return names_; }
    u32 gecko_register(unsigned n) const noexcept { return gr_[n & 15u]; }
    std::string report_json() const;

private:
    struct Program {
        std::string name;
        std::vector<Line> lines;
    };
    void run_program(const Program& program);
    CodeMemory* memory_;
    Layout layout_;
    std::vector<Program> programs_;
    std::vector<std::string> names_;
    u32 gr_[16] = {};
    Stats stats_;
};

}  // namespace gekko::codes
