// ppcrecomp - static PowerPC 750CL to native x86-64 recompiler for the
// Shadow the Hedgehog GameCube image.
//
// Input:  the original main.dol plus a table of function addresses and sizes
//         (extract/functions.csv, produced by the offline analysis).
// Output: C++ translation units in which each guest function becomes one native
//         function. MSVC compiles them to real x86-64; nothing is interpreted
//         at run time.
//
// What this is not: it does not lift the game to some other representation and
// it does not reimplement anything. Each guest instruction becomes the same
// operation on the same architected state, in the same order, reaching guest
// memory through the same bus. The interpreter remains the reference: the
// runtime can execute any function either way and compare the results.
//
// Control flow:
//   * branches inside a function become goto, and so does a bctr whose target
//     turns out at run time to be one of the function's labels;
//   * a call whose target was also recompiled becomes a direct native call;
//   * every other transfer - indirect calls, calls into code that was not
//     recompiled, supervisor instructions - leaves through leave_to_dispatcher,
//     which unwinds the native frames with the guest state consistent and lets
//     the interpreter continue from the exact next guest address.
//
// A function is emitted only if every reachable instruction in it is one this
// generator models. Anything else is reported, with a count per cause, so the
// generator gets fixed rather than the generated code getting patched.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gekko/ppc_instruction_traits.h"

static std::uint64_t g_fp_checks_emitted = 0;
static std::uint64_t g_fp_checks_hoisted = 0;
static std::uint64_t g_ir_insns = 0;
static std::uint64_t g_hot_label_insns = 0;
static std::uint64_t g_cold_mid_insns = 0;
static std::uint64_t g_hot_src_bytes = 0;
static std::uint64_t g_cold_src_bytes = 0;
// R13 (2026-10-20): the profile-chosen hot set (--hot-set). Only the HOT body
// of a listed row calls the force-inlined c.mem_*_hot accessors; every other
// body, and every cold trampoline, keeps the NOINLINE leaves. Empty: no row
// changes, and the output is byte-identical to a build without the option.
static std::unordered_set<std::uint32_t> g_hot_set;
static std::uint64_t g_hot_set_rows = 0;
static std::uint64_t g_hot_set_sites = 0;
// decode-cfg-3: gap-sweep roots discarded because their tentative walk reached
// an unsupported instruction or left an executable section (main analysis only).
static std::uint64_t g_sweep_roots_dropped = 0;

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// Set from --no-gap-sweep / --no-wide-entries. Both switches exist so the cost
// of the machinery added to reach zero interpreter fallbacks can be measured
// against what it actually prevents, instead of being assumed worthwhile.
bool g_gap_sweep = true;
bool g_wide_entries = true;

// T1 generated-code annotator (docs/recompiler-core-audit-2026-09-13/
// instruction-lowering/annotator). Set ONLY by --annotate-lines. Emits a #line
// directive before every piece of generated text, so a /Z7 compile's CodeView
// line table maps each native instruction back to the guest PC and to the kind
// of text that produced it:
//   i hot instruction    c cold instruction
//   t end-of-function tail  j cold join into hot  k cold forward to a hot label
//   g hot prologue/guard  d hot entry switch  e cold prologue/guard/entry switch
//   x exported _at/entry thunk  n interior entry thunk  a absorbed-row forward
// The file name "gpc.<kind>.<address as 8 hex digits>" carries the guest PC and
// the directive restarts the line count at 1, so a CodeView line L is physical
// line L-1 after the directive (the generated C++ statement that produced it;
// a line number cannot hold the PC because #line counts on across multi-line
// snippets and CodeView keeps only 24 bits of it). With the switch off every
// call returns an empty string and the generated text is byte-identical to a
// build without this code.
bool g_annotate_lines = false;

// --wait-points (extract/wait-points.txt, tools/ppcrecomp/wait_points.py):
// back-edge site -> loop head of every side-effect-free polling loop. Taking
// such a back edge means the guest waits for an event (docs/native-time-
// 20261005/DESIGN.md), so the generated code calls Cpu::idle_wait there.
std::unordered_map<u32, u32> g_wait_points;
// The `progress` sites among them: the runtime confirms a fixed point first
// (Cpu::poll_point) instead of waiting unconditionally (Cpu::idle_wait).
std::unordered_set<u32> g_poll_points;
std::set<u32> g_wait_points_emitted;

// --platform-sites (extract/platform-sites.txt; docs/native-time-20261005/
// STAGE2_VI.md D5): guest accesses to a platform register made by code whose
// own subsystem is not native yet. Each listed lhz/sth, pinned by its exact
// DOL word, is emitted as a call into the native runtime in place of the bus
// access (Cpu::platform_read16 / Cpu::platform_write16), at the same
// effective address; the runtime answers for that site or refuses. The kind
// `dead` (an sth only; STAGE6_PI_MI.md D4) is a store whose register has no
// consumer on the PC: the effective address is computed and nothing else is
// emitted, as for a dead GX pipe site. A listed li (addi rD,0,imm) is a
// console constant of the platform written into game code (the length of a
// console video field, ...): its register gets the native platform's value
// instead (Cpu::platform_value).
struct PlatformSite {
    u32 word = 0;
    std::string kind;
};
std::map<u32, PlatformSite> g_platform_sites;
std::set<u32> g_platform_sites_emitted;
std::size_t g_platform_site_emissions = 0;
std::size_t g_platform_site_dead_emissions = 0;
const char* const kPlatformKindDead = "dead";

// --gx-sites (extract/gx-sites.txt; docs/native-time-20261005/GX_DATA_PLANE.md):
// every store of the image to the GX write-gather pipe page, pinned by its
// exact DOL word, with the family it is lowered to. There is no pipe, no FIFO
// and no command decoder behind these stores any more: dead transport (state
// for Flipper, display-list calls, FIFO padding) emits nothing - an update form
// keeps its base update - and the live families become Cpu::gx_* calls that
// carry the site address (the draw-sync token, draw-done, the copy trigger,
// GXBegin's header, immediate vertex data). A listed site in a pruned or owned
// row is not emitted and needs nothing; one a surviving row translates must be
// emitted, or generation is refused.
enum class GxFamily { Dead, DrawSyncToken, DrawSyncTokenIrq, DrawDone, CopyTrigger, PrimitiveOpcode, PrimitiveCount, Vertex };
struct GxSite {
    u32 word = 0;
    GxFamily family = GxFamily::Dead;
};
std::map<u32, GxSite> g_gx_sites;
std::set<u32> g_gx_sites_emitted;
std::size_t g_gx_site_emissions = 0;
// GXD (2026-10-20, HW's GC_ENVIRONMENT row 7): GX pipe sites proven dead in the
// lifted build (--gx-fail-sites; "GX immediate: 0 primitives" in every lifted
// run). Such a site keeps its effective address and its update-form base
// register write, and instead of the family's call it stops the machine naming
// the site (Cpu::gx_dead_site): fail closed, never a silent no-op.
std::set<u32> g_gx_fail_sites;
std::size_t g_gx_fail_emissions = 0;

// R16 (2026-10-20): paired-single sites specialised for one GQR value
// (--psq-sites, "site gqr_index K"). The site's psq_l/psq_lu/psq_st/psq_stu
// becomes `if (c.spr[GQRi] == K) ppc::psq_*_k<K>(...) else ppc::psq_*(...)`:
// the guard is exact, so the generic helper still decides every other value.
// A listed site whose DOL word is not a D-form psq with that GQR index is
// refused at load; a listed site a surviving translation holds must be emitted.
struct PsqSite {
    u32 gqr = 0;
    u32 value = 0;
};
std::map<u32, PsqSite> g_psq_sites;
std::set<u32> g_psq_sites_emitted;
std::size_t g_psq_site_emissions = 0;

bool gx_family_from_name(const std::string& name, GxFamily& family) {
    static const std::pair<const char*, GxFamily> kNames[] = {
        {"dead", GxFamily::Dead},
        {"draw_sync_token", GxFamily::DrawSyncToken},
        {"draw_sync_token_irq", GxFamily::DrawSyncTokenIrq},
        {"draw_done", GxFamily::DrawDone},
        {"copy_trigger", GxFamily::CopyTrigger},
        {"primitive_opcode", GxFamily::PrimitiveOpcode},
        {"primitive_count", GxFamily::PrimitiveCount},
        {"vertex", GxFamily::Vertex},
    };
    for (const auto& entry : kNames) {
        if (name == entry.first) {
            family = entry.second;
            return true;
        }
    }
    return false;
}

const char* gx_family_enumerator(GxFamily family) {
    switch (family) {
        case GxFamily::Dead: return "Dead";
        case GxFamily::DrawSyncToken: return "DrawSyncToken";
        case GxFamily::DrawSyncTokenIrq: return "DrawSyncTokenIrq";
        case GxFamily::DrawDone: return "DrawDone";
        case GxFamily::CopyTrigger: return "CopyTrigger";
        case GxFamily::PrimitiveOpcode: return "PrimitiveOpcode";
        case GxFamily::PrimitiveCount: return "PrimitiveCount";
        case GxFamily::Vertex: return "Vertex";
    }
    return "Dead";
}

// The store forms each family is lowered from (primary opcodes; no indexed
// forms): dead - any D-form store or psq_st(u); the BP payload families - stw;
// GXBegin's header - stb then sth; vertex data - stb, sth, stw, stfs, psq_st.
bool gx_family_accepts(GxFamily family, u32 primary) {
    switch (family) {
        case GxFamily::Dead:
            return (primary >= 36u && primary <= 39u) || primary == 44u || primary == 45u ||
                   (primary >= 52u && primary <= 55u) || primary == 60u || primary == 61u;
        case GxFamily::DrawSyncToken:
        case GxFamily::DrawSyncTokenIrq:
        case GxFamily::DrawDone:
        case GxFamily::CopyTrigger: return primary == 36u;
        case GxFamily::PrimitiveOpcode: return primary == 38u;
        case GxFamily::PrimitiveCount: return primary == 44u;
        case GxFamily::Vertex:
            return primary == 36u || primary == 38u || primary == 44u || primary == 52u || primary == 60u;
    }
    return false;
}
std::string annotate(char kind, u32 address) {
    if (!g_annotate_lines) return std::string();
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "#line 1 \"gpc.%c.%08X\"\n", kind,
                  static_cast<unsigned>(address));
    return std::string(buffer);
}

// ---------------------------------------------------------------------------
// DOL image
// ---------------------------------------------------------------------------

struct Section {
    u32 address = 0;
    u32 size = 0;
    u32 file_offset = 0;
    bool executable = false;
};

struct DolImage {
    std::vector<u8> bytes;
    std::vector<Section> sections;
    u32 entry = 0;

    const u8* at(u32 address, u32 length) const {
        for (const Section& section : sections) {
            if (address >= section.address && address + length <= section.address + section.size) {
                return bytes.data() + section.file_offset + (address - section.address);
            }
        }
        return nullptr;
    }

    bool executable_at(u32 address) const {
        for (const Section& section : sections) {
            if (section.executable && address >= section.address &&
                address + 4u <= section.address + section.size) {
                return true;
            }
        }
        return false;
    }

    // An instruction word of the DOL's own text (an executable section).
    bool text_at(u32 address) const {
        for (const Section& section : sections) {
            if (section.executable && address >= section.address &&
                address - section.address < section.size &&
                section.size - (address - section.address) >= 4u) {
                return true;
            }
        }
        return false;
    }

    u32 read32(u32 address) const {
        const u8* p = at(address, 4);
        if (p == nullptr) {
            return 0;
        }
        return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
               (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
    }
};

u32 be32(const u8* p) {
    return (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) |
           (static_cast<u32>(p[2]) << 8) | static_cast<u32>(p[3]);
}

bool load_dol(const std::string& path, DolImage& image) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    image.bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (image.bytes.size() < 0x100) {
        return false;
    }
    const u8* header = image.bytes.data();
    for (int index = 0; index < 18; ++index) {
        Section section;
        section.file_offset = be32(header + index * 4);
        section.address = be32(header + 0x48 + index * 4);
        section.size = be32(header + 0x90 + index * 4);
        section.executable = index < 7;
        if (section.size != 0 && section.address != 0) {
            image.sections.push_back(section);
        }
    }
    image.entry = be32(header + 0xe0);
    return !image.sections.empty();
}

// ---------------------------------------------------------------------------
// Function table
// ---------------------------------------------------------------------------

struct FunctionRecord {
    u32 address = 0;
    u32 size = 0;
    std::string name;
};

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    bool quoted = false;
    for (char character : line) {
        if (character == '"') {
            quoted = !quoted;
        } else if (character == ',' && !quoted) {
            fields.push_back(current);
            current.clear();
        } else {
            current.push_back(character);
        }
    }
    fields.push_back(current);
    return fields;
}

bool load_functions(const std::string& path, std::vector<FunctionRecord>& out) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    std::string line;
    if (!std::getline(file, line)) {
        return false;
    }
    const std::vector<std::string> header = split_csv(line);
    std::size_t address_column = 0;
    std::size_t name_column = 2;
    std::size_t size_column = 3;
    for (std::size_t index = 0; index < header.size(); ++index) {
        if (header[index] == "addr_hex") address_column = index;
        if (header[index] == "name") name_column = index;
        if (header[index] == "size") size_column = index;
    }
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        const std::vector<std::string> fields = split_csv(line);
        if (fields.size() <= size_column || fields.size() <= address_column) {
            continue;
        }
        FunctionRecord record;
        record.address = static_cast<u32>(std::strtoul(fields[address_column].c_str(), nullptr, 16));
        record.size = static_cast<u32>(std::strtoul(fields[size_column].c_str(), nullptr, 10));
        record.name = name_column < fields.size() ? fields[name_column] : std::string();
        if (record.address != 0 && record.size >= 4) {
            out.push_back(record);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Instruction fields
// ---------------------------------------------------------------------------

inline u32 op_primary(u32 i) { return i >> 26; }
inline u32 op_extended(u32 i) { return (i >> 1) & 0x3ffu; }
inline u32 op_extended5(u32 i) { return (i >> 1) & 0x1fu; }
inline u32 field_d(u32 i) { return (i >> 21) & 31u; }
inline u32 field_a(u32 i) { return (i >> 16) & 31u; }
inline u32 field_b(u32 i) { return (i >> 11) & 31u; }
inline u32 field_c(u32 i) { return (i >> 6) & 31u; }
inline bool field_rc(u32 i) { return (i & 1u) != 0u; }
inline bool field_oe(u32 i) { return ((i >> 10) & 1u) != 0u; }
inline s32 field_simm(u32 i) { return static_cast<s16>(i & 0xffffu); }
inline u32 field_uimm(u32 i) { return i & 0xffffu; }
inline u32 field_crfd(u32 i) { return (i >> 23) & 7u; }
inline u32 field_crfs(u32 i) { return (i >> 18) & 7u; }
inline u32 spr_number(u32 i) { return ((i >> 16) & 0x1fu) | (((i >> 11) & 0x1fu) << 5); }

s32 sign_extend(u32 value, u32 bits) {
    const u32 shift = 32u - bits;
    return static_cast<s32>(value << shift) >> shift;
}

u32 mask_from(u32 mb, u32 me) {
    const u32 begin = 0xffffffffu >> mb;
    const u32 end = (me >= 31u) ? 0u : (0xffffffffu >> (me + 1u));
    const u32 mask = begin ^ end;
    return (me < mb) ? ~mask : mask;
}

std::string hex32(u32 value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    return buffer;
}

std::string hexu(u32 value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%Xu", value);
    return buffer;
}

// SCAFFOLDING (R2): entry/return marshalling for reconstructed empty leaves;
// deleted when their remaining translated callers use native values directly.
enum class EmptyVariadicKind { Preserve, Zero, VaList };
std::map<u32, EmptyVariadicKind> g_empty_variadic_leaves;

std::string fn_symbol(u32 address) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), g_empty_variadic_leaves.count(address) ? "empty_variadic_%08X" : "fn_%08X", address);
    return buffer;
}

// Name of the thunk that enters a function at an address other than its entry.
std::string entry_symbol(u32 function, u32 address) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "fn_%08X_e%08X", function, address);
    return buffer;
}

std::string label(u32 address) {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "L_%08X", address);
    return buffer;
}

// ---------------------------------------------------------------------------
// Lowered call sites (--lift-sites, extract/lift-sites.txt;
// docs/lift-20261006/LIFT_PROGRAMME.md section 2.1)
// ---------------------------------------------------------------------------
//
// A lifted subsystem is native C++ that knows nothing of the translated
// machine. Translated code that still calls it reaches it through its own call
// sites, rewritten here: a listed `bl`, or a devirtualised `bctrl` (an
// interface call whose only implementation was lifted), becomes a call of the
// generated function lift_site_<address>. That function reads the caller's
// arguments, calls the native API with values, writes the results where the
// caller expects them and sets the link register as the call would have. The
// same function serves the dispatcher and the reference interpreter
// (recomp::kLiftSites, Cpu::find_lift_site), so the oracle cuts at a site the
// way it cuts before an owner call and the native call runs once.
//
// A retired root is a data word (a vtable slot, a registration record) that
// only lowered code read: it no longer seeds liveness, so the code it pointed
// at is pruned unless something else reaches it.
enum class LiftKind {
    PeripheralPortCount,   // bctrl  r3 = port_count()
    PeripheralRead,        // bctrl  (r4 = PortRecord[r5]) <- read(); r3 = 1
    PeripheralStickRange,  // bctrl  (r5 = s16[4]) <- stick_range(r4); r3 = 1
    PeripheralRumble,      // bl     set_rumble(r3, r4 == 1)
    PeripheralRumbleAll,   // bl     set_rumble(i, u32 r3[i] == 1), i < 4
    ReturnTrue,            // bl     r3 = 1 (nothing to do on this platform)
    Nothing,               // bl     nothing to do on this platform
    ScalarSquareRoot,      // bl     f1 = native scalar square_root(f1), with caller-owned error state
    AffineMatrixCopy,      // bl     copy twelve float representations between caller-owned spans
    AffineMatrixCompose,   // bl     compose two affine matrices through the native value API
    TypeQuery,             // bl     native type query; may transfer to exception handling
    // The game's MSL math (runtime/lift/math/msl.h, docs/NATIVE_MSL_MATH.md), in the caller's FPSCR[NI] mode:
    MslSin, MslCos, MslTan, MslAtan, MslAcos,   // bl  f1 = fn(f1)
    MslAtan2,                                   // bl  f1 = atan2(f1 = y, f2 = x)
    MslSinF, MslCosF, MslTanF, MslAtanF, MslAcosF,  // bl  f1 = frsp(fn(f1))
    MslAtan2F,                                  // bl  f1 = frsp(atan2(f1, f2))
    MslSinCosF,                                 // bl  *(f32*)r3 = frsp(sin(f1)), *(f32*)r4 = frsp(cos(f1)); f1 = cos
    Call,                  // bl or bctrl  r3 = scaffold::<function>(c, r3, ..., r(2+argc))
    Unreachable,           // bl     fail closed: the lift proved the path dead; reaching it stops the machine
};
struct LiftSite {
    u32 word = 0;
    LiftKind kind = LiftKind::Nothing;
    // LiftKind::Call: the scaffold function, its integer argument count and whether r3 takes its result.
    std::string function;
    u32 argc = 0;
    u32 fpr_count = 0;  // floating-point arguments f1..f<fpr_count>, after the integer ones
    bool keep_result = true;
    u32 query = 0;          // TypeQuery: native query ordinal, never an image address
    u32 target_descriptor = 0;  // disposable caller ABI contract
};
std::map<u32, LiftSite> g_lift_sites;
std::set<u32> g_lift_sites_emitted;
std::map<u32, std::string> g_lift_site_lists;  // where each site is listed (file:line), for diagnostics
std::map<u32, u32> g_retired_roots;  // data word address -> the code address it held
constexpr u32 kBctrlWord = 0x4E800421u;

bool lift_kind_from_name(const std::string& name, LiftKind& kind) {
    static const std::pair<const char*, LiftKind> kNames[] = {
        {"peripheral_port_count", LiftKind::PeripheralPortCount},
        {"peripheral_read", LiftKind::PeripheralRead},
        {"peripheral_stick_range", LiftKind::PeripheralStickRange},
        {"peripheral_rumble", LiftKind::PeripheralRumble},
        {"peripheral_rumble_all", LiftKind::PeripheralRumbleAll},
        {"return_true", LiftKind::ReturnTrue},
        {"nothing", LiftKind::Nothing},
        {"scalar_square_root", LiftKind::ScalarSquareRoot},
        {"affine_matrix_copy", LiftKind::AffineMatrixCopy},
        {"affine_matrix_compose", LiftKind::AffineMatrixCompose},
        {"type_query", LiftKind::TypeQuery},
        {"msl_sin", LiftKind::MslSin},
        {"msl_cos", LiftKind::MslCos},
        {"msl_tan", LiftKind::MslTan},
        {"msl_atan", LiftKind::MslAtan},
        {"msl_acos", LiftKind::MslAcos},
        {"msl_atan2", LiftKind::MslAtan2},
        {"msl_sinf", LiftKind::MslSinF},
        {"msl_cosf", LiftKind::MslCosF},
        {"msl_tanf", LiftKind::MslTanF},
        {"msl_atanf", LiftKind::MslAtanF},
        {"msl_acosf", LiftKind::MslAcosF},
        {"msl_atan2f", LiftKind::MslAtan2F},
        {"msl_sincosf", LiftKind::MslSinCosF},
        {"call", LiftKind::Call},
        {"unreachable", LiftKind::Unreachable},
    };
    for (const auto& entry : kNames) {
        if (name == entry.first) {
            kind = entry.second;
            return true;
        }
    }
    return false;
}

// Whether a kind is one of the MSL math kinds (math/msl.h).
bool lift_kind_is_msl(LiftKind kind) {
    switch (kind) {
        case LiftKind::MslSin: case LiftKind::MslCos: case LiftKind::MslTan: case LiftKind::MslAtan:
        case LiftKind::MslAcos: case LiftKind::MslAtan2: case LiftKind::MslSinF: case LiftKind::MslCosF:
        case LiftKind::MslTanF: case LiftKind::MslAtanF: case LiftKind::MslAcosF: case LiftKind::MslAtan2F:
        case LiftKind::MslSinCosF:
            return true;
        default:
            return false;
    }
}

// Whether a kind lowers a devirtualised interface call (bctrl) rather than a bl.
bool lift_kind_is_bctrl(LiftKind kind) {
    return kind == LiftKind::PeripheralPortCount || kind == LiftKind::PeripheralRead ||
           kind == LiftKind::PeripheralStickRange;
}

// Whether a listed site lowers a bctrl (the generic call kind takes either form; its word decides).
bool lift_site_is_bctrl(const LiftSite& site) {
    return site.kind == LiftKind::Call ? site.word == kBctrlWord : lift_kind_is_bctrl(site.kind);
}

std::string lift_site_symbol(u32 site) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "lift_site_%08X", site);
    return buffer;
}

// The statements of lift_site_<site> (Cpu& c): what the call meant, with values.
std::string lift_site_body(u32 site, const LiftSite& listed) {
    std::ostringstream body;
    body << "    c.lr = " << hexu(site + 4u) << ";\n";
    switch (listed.kind) {
        case LiftKind::ScalarSquareRoot:
            body << "    const auto result = shadow::math::square_root(bits_to_double(c.fpr[1].bits[0]));\n"
                    "    c.fpr[1].bits[0] = double_to_bits(result.value);\n"
                    "    if (result.domain_error) c.mem_write32(c.gpr[13] + 0x5E98u, 33u);\n";
            break;
        case LiftKind::MslSin: case LiftKind::MslCos: case LiftKind::MslTan: case LiftKind::MslAtan:
        case LiftKind::MslAcos: case LiftKind::MslAtan2: case LiftKind::MslSinF: case LiftKind::MslCosF:
        case LiftKind::MslTanF: case LiftKind::MslAtanF: case LiftKind::MslAcosF: case LiftKind::MslAtan2F:
        case LiftKind::MslSinCosF: {
            // The MSL routine's value (its other effects are its own stack frames and FPSCR status bits, which
            // nothing reads: docs/NATIVE_MSL_MATH.md); a float wrapper's frsp rounds in the caller's mode.
            const auto k = listed.kind;
            const bool single = k == LiftKind::MslSinF || k == LiftKind::MslCosF || k == LiftKind::MslTanF ||
                                k == LiftKind::MslAtanF || k == LiftKind::MslAcosF || k == LiftKind::MslAtan2F;
            const char* fn = (k == LiftKind::MslSin || k == LiftKind::MslSinF)     ? "sin"
                             : (k == LiftKind::MslCos || k == LiftKind::MslCosF)   ? "cos"
                             : (k == LiftKind::MslTan || k == LiftKind::MslTanF)   ? "tan"
                             : (k == LiftKind::MslAtan || k == LiftKind::MslAtanF) ? "atan"
                             : (k == LiftKind::MslAcos || k == LiftKind::MslAcosF) ? "acos"
                                                                                    : "atan2";
            body << "    const auto mode = (c.fpscr & kFpscrNI) != 0u ? shadow::math::SubnormalMode::Flush\n"
                    "                                                : shadow::math::SubnormalMode::Preserve;\n"
                    "    (void)mode;\n"
                    "    const double x = bits_to_double(c.fpr[1].bits[0]);\n";
            if (k == LiftKind::MslSinCosF) {
                body << "    const float s = shadow::math::msl::round_to_single(shadow::math::msl::sin(x), mode);\n"
                        "    const double co = shadow::math::msl::cos(x);\n"
                        "    c.mem_write32(c.gpr[3], float_to_bits(s));\n"
                        "    c.mem_write32(c.gpr[4], float_to_bits(shadow::math::msl::round_to_single(co, mode)));\n"
                        "    c.fpr[1].bits[0] = double_to_bits(co);\n";
                break;
            }
            std::string value = std::string("shadow::math::msl::") + fn +
                                (std::string(fn) == "atan2" ? "(x, bits_to_double(c.fpr[2].bits[0]), mode)" : "(x)");
            if (single) value = "double(shadow::math::msl::round_to_single(" + value + ", mode))";
            body << "    c.fpr[1].bits[0] = double_to_bits(" << value << ");\n";
            break;
        }
        case LiftKind::AffineMatrixCopy:
            body << "    copy_matrix_boundary(c);\n";
            break;
        case LiftKind::AffineMatrixCompose:
            body << "    compose_matrix_boundary(c);\n";
            break;
        case LiftKind::TypeQuery:
            body << "    c.pc = " << hexu(site) << ";\n";
            body << "    type_query_boundary::prepare(c, static_cast<shadow::object::game_type_queries::Query>("
                 << listed.query << "u), " << hexu(listed.target_descriptor) << ");\n"
                    "    return;\n";
            return body.str();
        case LiftKind::Unreachable:
            body << "    c.request_stop(StopReason::ExecutionFault, \"lowered site " << hex32(site)
                 << " reached: the lift proved this path dead\");\n";
            break;
        case LiftKind::Call: {
            body << "    " << (listed.keep_result ? "c.gpr[3] = " : "") << "::gekko::scaffold::" << listed.function
                 << "(c";
            for (u32 i = 0; i < listed.argc; ++i) body << ", c.gpr[" << (3u + i) << "]";
            for (u32 i = 0; i < listed.fpr_count; ++i) body << ", bits_to_double(c.fpr[" << (1u + i) << "].bits[0])";
            body << ");\n";
            break;
        }
        case LiftKind::PeripheralPortCount:
            body << "    c.gpr[3] = c.lifted().peripheral.port_count();\n";
            break;
        case LiftKind::PeripheralRead:
            // The engine's 16-byte port record: state, controls, then the four
            // stick halfwords. An absent port gets only its state, as the
            // shipped implementation wrote it.
            body << "    using Imp = shadow::peripheral::ManagerImpPc;\n"
                    "    const u32 out = c.gpr[4];\n"
                    "    const u32 count = c.gpr[5];\n"
                    "    Imp::PortReading readings[Imp::kPorts];\n"
                    "    const std::size_t read = count < Imp::kPorts ? count : Imp::kPorts;\n"
                    "    c.lifted().peripheral.read(readings, read);\n"
                    "    for (u32 port = 0; port < count; ++port) {\n"
                    "        const u32 record = out + 16u * port;\n"
                    "        if (port >= read || readings[port].state != Imp::PortState::Ready) {\n"
                    "            c.mem_write32(record, static_cast<u32>(Imp::PortState::Absent));\n"
                    "            continue;\n"
                    "        }\n"
                    "        const Imp::PortReading& reading = readings[port];\n"
                    "        c.mem_write32(record, static_cast<u32>(reading.state));\n"
                    "        c.mem_write32(record + 4u, reading.controls);\n"
                    "        c.mem_write16(record + 8u, static_cast<u16>(reading.left_x));\n"
                    "        c.mem_write16(record + 10u, static_cast<u16>(reading.left_y));\n"
                    "        c.mem_write16(record + 12u, static_cast<u16>(reading.right_x));\n"
                    "        c.mem_write16(record + 14u, static_cast<u16>(reading.right_y));\n"
                    "    }\n"
                    "    c.gpr[3] = 1u;\n";
            break;
        case LiftKind::PeripheralStickRange:
            body << "    const auto range = c.lifted().peripheral.stick_range(c.gpr[4]);\n"
                    "    const u32 out = c.gpr[5];\n"
                    "    c.mem_write16(out, static_cast<u16>(range.left_dead));\n"
                    "    c.mem_write16(out + 2u, static_cast<u16>(range.right_dead));\n"
                    "    c.mem_write16(out + 4u, static_cast<u16>(range.left_range));\n"
                    "    c.mem_write16(out + 6u, static_cast<u16>(range.right_range));\n"
                    "    c.gpr[3] = 1u;\n";
            break;
        case LiftKind::PeripheralRumble:
            // The game's motor command: 1 rumbles, 0 (stop) and 2 (hard stop)
            // both stop.
            body << "    c.lifted().peripheral.set_rumble(c.gpr[3], c.gpr[4] == 1u);\n";
            break;
        case LiftKind::PeripheralRumbleAll:
            body << "    const u32 commands = c.gpr[3];\n"
                    "    for (u32 port = 0; port < 4u; ++port) {\n"
                    "        c.lifted().peripheral.set_rumble(port, c.mem_read32(commands + 4u * port) == 1u);\n"
                    "    }\n";
            break;
        case LiftKind::ReturnTrue:
            body << "    c.gpr[3] = 1u;\n";
            break;
        case LiftKind::Nothing:
            break;
    }
    return body.str();
}

// ---------------------------------------------------------------------------
// Control-flow classification
// ---------------------------------------------------------------------------

enum class Flow {
    Normal,       // falls through
    Branch,       // unconditional direct branch
    CondBranch,   // conditional direct branch, may fall through
    Call,         // direct branch with link, falls through
    CondCall,     // conditional branch with link, falls through
    Return,       // bclr
    Indirect,     // bcctr
    Terminal,     // sc, rfi - handed to the interpreter, no successor here
};

inline bool flow_falls_through(Flow flow) {
    return flow == Flow::Normal || flow == Flow::CondBranch || flow == Flow::Call ||
           flow == Flow::CondCall;
}

struct Decoded {
    Flow flow = Flow::Normal;
    u32 target = 0;      // for direct branches
    bool supported = true;
    const char* reason = "";
};

// Reports how an instruction affects control flow, and whether this generator
// models it at all. Kept separate from emission so the reachability walk can run
// first and only reachable instructions have to be supported.
Decoded classify(u32 instruction, u32 address);

// ---------------------------------------------------------------------------
// Emission
// ---------------------------------------------------------------------------

struct EmitContext {
    const DolImage* image = nullptr;
    // Every dispatchable guest address -> the start of the function that
    // translates it. An entry equal to its key is a function's own entry point;
    // any other entry is a thunk into the middle of that function's body.
    const std::unordered_map<u32, u32>* recompiled = nullptr;  // address -> owner start
    u32 function_start = 0;
    u32 function_end = 0;
    std::set<u32> labels;
    std::string body;
    bool ok = true;
    std::string failure;
    // CPU-18: compare/branch fusion state. A compare writes c.cr and the next
    // instruction reads it straight back - a store-to-load forward the branch
    // cannot hide. 47,404 of the game's 66,995 conditional branches (70.8%) sit
    // immediately after a compare on the same field.
    //
    // Legal ONLY in the hot body, and only for an instruction that is not itself
    // a label: there, nothing can jump to it, so control provably came from the
    // compare. fusion_allowed is set by the dual-body builder.
    bool fusion_allowed = false;
    bool share_indirect_dispatch = false;
    bool indirect_dispatch_used = false;
    u32 current_address = 0;
    int cr_fuse_field = -1;        // field the instruction being emitted may fuse with
    std::string cr_fuse_bit[4];    // LT, GT, EQ, SO
    int cr_fuse_armed_field = -1;  // armed by the instruction just emitted
    std::string cr_fuse_armed_bit[4];
    // Hot-body MSR[FP] hoist. Once a sequential unlabeled run has proved
    // MSR[FP] is on, later FP instructions in that run cannot fault: nothing
    // between them writes MSR. Cleared at labels (merge points), after calls
    // (the callee may switch FPU context) and after mtmsr.
    bool fp_guard_live = false;
    // The same hoist for the HID2 enables (audit paired-single-10): the HID2
    // bits an earlier Gekko-extension guard in this run proved set. Cleared
    // wherever fp_guard_live is, and by mtspr to HID2 (SPR 920).
    u32 hid2_proven = 0;
};

// CPU-18: arm the fusion for the NEXT instruction only. A compare writes no GPR,
// and the consumer is emitted immediately after, so the operand expressions
// cannot change in between.
inline void arm_cr_fuse(EmitContext& e, unsigned field, const std::string& lt,
                        const std::string& gt, const std::string& eq) {
    e.cr_fuse_armed_field = static_cast<int>(field);
    e.cr_fuse_armed_bit[0] = lt;
    e.cr_fuse_armed_bit[1] = gt;
    e.cr_fuse_armed_bit[2] = eq;
    e.cr_fuse_armed_bit[3] = "((c.xer & kXerSO) != 0u)";
}

void line(EmitContext& e, const std::string& text) {
    e.body += "    ";
    e.body += text;
    e.body += "\n";
}

std::string gpr(u32 n) { return "c.gpr[" + std::to_string(n) + "]"; }
std::string gpr_or_zero(u32 n) { return n == 0 ? std::string("0u") : gpr(n); }

// CPU-21: hot-only mem_load* inlining was measured and reverted. Emitting the
// force-inlined fast path in the sequential body grew the image 138 → 171 MiB
// and lost 6.4% wall on this host (the ~150 MiB front-end cliff, same shape as
// CPU-04/13). The helper stays so a future size cut can re-enable it; both
// bodies emit mem_read* today.
std::string mem_r(const EmitContext& e, const char* width, const std::string& ea) {
    (void)e;
    std::string call = "c.mem_read";
    call += width;
    call += "(";
    call += ea;
    call += ")";
    return call;
}
std::string ps0(u32 n) { return "c.fpr[" + std::to_string(n) + "].bits[0]"; }
std::string ps1(u32 n) { return "c.fpr[" + std::to_string(n) + "].bits[1]"; }
std::string d0(u32 n) { return "bits_to_double(" + ps0(n) + ")"; }
std::string d1(u32 n) { return "bits_to_double(" + ps1(n) + ")"; }

// Distinguishes an unresolved branch target from the trailing transfer emitted
// when execution runs off the end of a function's analysed extent.
enum class FallbackReasonTag { NotRecompiled, EndOfFunction };

void fail(EmitContext& e, const std::string& why) {
    e.ok = false;
    if (e.failure.empty()) {
        e.failure = why;
    }
}

// The immediate of a d-form load or store, as a u32 constant.
std::string simm_u32(u32 instruction) {
    return hexu(static_cast<u32>(field_simm(instruction)));
}

void emit_cr0(EmitContext& e, u32 instruction, const std::string& value) {
    if (field_rc(instruction)) {
        line(e, "ppc::update_cr0(c, " + value + ");");
        // CPU-18: update_cr0 compares the same value against zero, signed.
        const std::string s = "static_cast<s32>(" + value + ")";
        arm_cr_fuse(e, 0u, s + " < 0", s + " > 0", s + " == 0");
    }
}

void emit_cr1(EmitContext& e, u32 instruction) {
    if (field_rc(instruction)) {
        line(e, "ppc::update_cr1(c);");
    }
}

// Condition evaluation for bc/bclr/bcctr. Returns a C++ boolean expression and
// emits the CTR decrement when the encoding calls for one. decrement_ctr is
// false for bcctr, which never touches CTR.
std::string emit_branch_condition(EmitContext& e, u32 instruction, bool decrement_ctr) {
    const u32 bo = field_d(instruction);
    const u32 bi = field_a(instruction);
    std::vector<std::string> parts;

    if (decrement_ctr && (bo & 0x04u) == 0u) {
        line(e, "--c.ctr;");
        parts.push_back((bo & 0x02u) != 0u ? "c.ctr == 0u" : "c.ctr != 0u");
    }
    if ((bo & 0x10u) == 0u) {
        // CPU-18: fuse with the preceding compare when all three preconditions
        // hold - hot body, this branch is not itself a label (so nothing can jump
        // to it and control came from the compare), and the compare armed this
        // exact field. Otherwise read the CR word exactly as before.
        std::string bit;
        const bool sequential =
            e.fusion_allowed && e.labels.count(e.current_address) == 0;
        if (sequential && e.cr_fuse_field >= 0 &&
            static_cast<unsigned>(e.cr_fuse_field) == (bi >> 2) &&
            !e.cr_fuse_bit[bi & 3u].empty()) {
            bit = "(" + e.cr_fuse_bit[bi & 3u] + ")";
        } else {
            bit = "ppc::cr_bit(c, " + std::to_string(bi) + "u)";
        }
        parts.push_back((bo & 0x08u) != 0u ? bit : ("!" + bit));
    }
    if (parts.empty()) {
        return "true";
    }
    std::string expression = parts[0];
    for (std::size_t index = 1; index < parts.size(); ++index) {
        expression += " && " + parts[index];
    }
    return expression;
}

// A function's entry thunk: try_hle's filter inline, then the body as a tail call; a hooked slot (or the verify /
// fault fill) takes Cpu::enter_slow, also a tail call - the same order and checks as
// `if (c.try_hle(A)) return; body(c, A);`, with no stack frame in the thunk (Cpu::hle_clear).
std::string entry_thunk(const std::string& name, const std::string& body_symbol, u32 address) {
    return "void " + name + "(Cpu& c) { if (c.hle_clear(" + hexu(address) + ")) return " + body_symbol + "(c, " +
           hexu(address) + "); c.enter_slow(" + hexu(address) + ", &" + body_symbol + "); }\n";
}

// Name of the native function that enters translated code at exactly `target`.
// The target is either a function's own entry point or one of its entry thunks.
std::string transfer_symbol(const EmitContext& e, u32 target) {
    const auto it = e.recompiled->find(target);
    const u32 owner = it->second;
    return owner == target ? fn_symbol(target) : entry_symbol(owner, target);
}

// The transfer half of a call: a native call when the target was recompiled
// too, otherwise a dispatch that ends in the interpreter, followed by the checks
// that decide whether this function may keep running natively.
//
// A callee that returns normally leaves guest control at c.pc. That is the link
// address for an ordinary return, but a function that reloads LR - a longjmp, a
// dispatcher, a context switch - returns somewhere else, and the caller has to
// notice and hand over rather than carry on with its own next instruction.
//
// The exiting() check is also recognition point R2 (cpu.h, interrupt_lines_):
// an interrupt that became deliverable anywhere inside the callee - an MMIO
// store that unmasked a cause, an event delivered at a wait point the callee
// reached - set Cpu's exit request, so the caller unwinds here with pc at the
// instruction the callee returned to and the dispatcher takes it (audit
// dispatch-exceptions-6). No new test is emitted for that.
//
// R12: this frame is where the callee's LK=0 transfers run. A callee that ended
// in a transfer published it and returned (exiting() is then true); the
// finish_transfers call runs the chain before the R2 test, exactly where the
// callee's tail call used to run it. kCallExitCheck is the one spelling of that
// check; the bcctrl emitter uses it too.
constexpr const char* kCallExitCheck = "if (c.exiting() && c.finish_transfers()) return;";

void emit_call_transfer(EmitContext& e, u32 target, u32 link) {
    if (e.recompiled->find(target) != e.recompiled->end()) {
        line(e, transfer_symbol(e, target) + "(c);");
    } else {
        line(e, "recomp::call_indirect(c, " + hexu(target) + ", " + hexu(link) + ");");
    }
    line(e, kCallExitCheck);
    line(e, "if (c.pc != " + hexu(link) + ") { recomp::resume_elsewhere(c, " + hexu(link - 4u) +
                "); return; }");
    // The callee can clear MSR[FP] (OS lazy FPU switch). Re-check on the next
    // floating-point instruction of this body.
    e.fp_guard_live = false;
    e.hid2_proven = 0u;
}

// A listed lowered call site (g_lift_sites, validated against its DOL word):
// the generated lift_site function in place of the call. Under --verify-recomp
// (or while a data-access fault unwinds) Cpu::lift_site_cut ends this pass at
// the site, exactly as a call to an owner is cut, and the dispatcher runs the
// site once. Execution then continues after the site, as after the call.
void emit_lift_site(EmitContext& e, u32 address) {
    // The copy family has hundreds of callers. Keep its cut, return-address
    // publication and exit handling in one temporary boundary rather than
    // duplicating the same console bookkeeping in every hot/cold caller.
    const auto kind = g_lift_sites.at(address).kind;
    if (kind == LiftKind::TypeQuery) {
        const auto& site = g_lift_sites.at(address);
        line(e, "if (call_type_query(c, " + hexu(address) + ", " + std::to_string(site.query) +
                "u, " + hexu(site.target_descriptor) + ")) return;");
        g_lift_sites_emitted.insert(address);
        e.fp_guard_live = false;
        e.hid2_proven = 0u;
        return;
    }
    if (kind == LiftKind::AffineMatrixCopy || kind == LiftKind::AffineMatrixCompose) {
        const std::string call = kind == LiftKind::AffineMatrixCopy ? "call_matrix_copy" : "call_matrix_compose";
        line(e, "if (" + call + "(c, " + hexu(address) + ")) return;");
        g_lift_sites_emitted.insert(address);
        e.fp_guard_live = false;
        e.hid2_proven = 0u;
        return;
    }
    line(e, "if (c.lift_site_cut(" + hexu(address) + ")) return;");
    line(e, lift_site_symbol(address) + "(c);");
    // A scaffold call (the generic kind) can make an interrupt deliverable - the present's breakpoint, for one
    // (r2/SITES_PRESENT_EFFECTS.md) - so it ends at the same precise recognition point (R2) as any native call:
    // pc is the link address and the exit check follows. The named kinds raise nothing and keep their code.
    if (const auto site = g_lift_sites.find(address);
        site != g_lift_sites.end() &&
        (site->second.kind == LiftKind::Call || site->second.kind == LiftKind::Unreachable ||
         site->second.kind == LiftKind::ScalarSquareRoot || site->second.kind == LiftKind::AffineMatrixCopy ||
         site->second.kind == LiftKind::AffineMatrixCompose || lift_kind_is_msl(site->second.kind))) {
        line(e, "c.pc = " + hexu(address + 4u) + ";");
        line(e, kCallExitCheck);
    }
    g_lift_sites_emitted.insert(address);
    // Conservative across every call site, lowered or not.
    e.fp_guard_live = false;
    e.hid2_proven = 0u;
}

// A listed polling loop's taken back edge (g_wait_points): the iteration is a
// pure function of what it reads, so only an event can end the loop. The game
// clock advances to its next event here; the needs_dispatch() test that
// follows takes an interrupt the event made deliverable. Emitted wherever the
// back edge is - the hot body, the cold body, a cross-record transfer - so a
// loop re-entered mid-function still waits.
void emit_wait_point(EmitContext& e, u32 target, u32 site) {
    const auto it = g_wait_points.find(site);
    if (it == g_wait_points.end() || it->second != target) return;
    if (g_poll_points.count(site) != 0u) {
        line(e, "c.poll_point(" + hexu(site) + ");");
    } else {
        line(e, "c.idle_wait();");
    }
    g_wait_points_emitted.insert(site);
}

void emit_jump(EmitContext& e, u32 target, u32 site, FallbackReasonTag tag = FallbackReasonTag::NotRecompiled) {
    if (target >= e.function_start && target < e.function_end && e.labels.count(target) != 0) {
        if (target <= site) {
            // A backward branch closes a loop. Two things happen here (and at
            // the backward transfer to another record below), once per
            // iteration:
            //
            //   * pc is published, so a machine that is spinning has a guest
            //     address to report and the sampling profiler can see native
            //     code at all;
            //   * a deliverable interrupt (external or decrementer) is taken,
            //     because the guest spins in loops like this waiting for a
            //     handler to set a flag and would otherwise never leave.
            line(e, "c.pc = " + hexu(target) + ";");
            emit_wait_point(e, target, site);
            line(e, "if (c.needs_dispatch()) { c.leave_to_dispatcher(" + hexu(target) +
                        ", FallbackReason::Yield, " + hexu(site) + "); return; }");
        }
        line(e, "goto " + label(target) + ";");
        return;
    }
    if (e.recompiled->find(target) != e.recompiled->end()) {
        // A branch is NIA <- target whether or not the target lies in this
        // record: a backward one closes a loop exactly like the goto above
        // (audit branch-sys-2). functions.csv starts records after bctrl/bl,
        // so loops like `bcctrl; ...; blt head` span two records and used to
        // cycle through these calls with no pc publish and no poll: a pending
        // interrupt, budget or stop waited for the whole loop. Same poll, same
        // recognition point R3. The end-of-function fall-through (tag
        // EndOfFunction, target == site == function_end) is not a branch.
        if (target <= site && tag == FallbackReasonTag::NotRecompiled) {
            line(e, "c.pc = " + hexu(target) + ";");
            emit_wait_point(e, target, site);
            line(e, "if (c.needs_dispatch()) { c.leave_to_dispatcher(" + hexu(target) +
                        ", FallbackReason::Yield, " + hexu(site) + "); return; }");
        }
        // R12: published, not called - this body returns before the target
        // runs, so a cycle through records keeps no host frame per lap
        // (Cpu::transfer_to). Same entry thunk, so the same try_hle.
        line(e, "c.transfer_to(&" + transfer_symbol(e, target) + ");");
        line(e, "return;");
        return;
    }
    line(e, "c.leave_to_dispatcher(" + hexu(target) + ", FallbackReason::" +
                std::string(tag == FallbackReasonTag::EndOfFunction ? "EndOfFunction"
                                                                    : "NotRecompiled") +
                ", " + hexu(site) + ");");
    line(e, "return;");
}

// The transfer half of a non-linking bcctr: NIA <- CTR[0-29] || 0b00, a plain
// jump to the address already in `tmp`. It leaves nothing on the guest stack,
// so it must leave nothing on the host stack either. recomp::jump_indirect
// CALLS the native code for its target and only returns once that code does,
// so a guest loop that closes through a bctr - a jump-table token decoder,
// `lwzx; mtctr; bctr` with every case ending in `b head` - used to keep one
// host frame (two, via the cold body) per iteration until the guest returned:
// 95,440 bytes per token in fn_803E1D58.
//
// Every hot label of this function is therefore reached with a goto, the same
// transfer a direct branch to it uses:
//   * a target at or before the site closes a loop, so - exactly like
//     emit_jump's back edge - pc is published and needs_dispatch polled;
//   * a target that is an accepted function's own entry first runs that
//     entry's try_hle, the choke point (HLE hooks, --verify-recomp call cuts)
//     the thunk jump_indirect used to call would have run;
//   * any other value keeps the jump_indirect path.
// A label resets the FP-check hoist and forbids CR fusion, so arriving here
// from the switch assumes nothing about the straight-line predecessor.
void emit_indirect_jump(EmitContext& e, u32 site) {
    // Multiple computed jumps share the same target set. Carry the actual
    // branch site to one dispatch block rather than duplicating its switches
    // at every site. A single-site function keeps its specialized lowering.
    if (e.share_indirect_dispatch) {
        e.indirect_dispatch_used = true;
        line(e, "gekko_indirect_site = " + hexu(site) + ";");
        line(e, "goto gekko_indirect_dispatch;");
        return;
    }
    std::vector<u32> forward;
    std::vector<u32> backward;
    for (auto it = e.labels.lower_bound(e.function_start);
         it != e.labels.end() && *it < e.function_end; ++it) {
        (*it <= site ? backward : forward).push_back(*it);
    }
    if (forward.empty() && backward.empty()) {
        line(e, "recomp::jump_indirect(c, tmp);");
        line(e, "return;");
        return;
    }
    const auto enter = [&](u32 target) {
        std::string text;
        const auto owner = e.recompiled->find(target);
        if (owner != e.recompiled->end() && owner->second == target) {
            text = "if (c.try_hle(" + hexu(target) + ")) return; ";
        }
        return text + "goto " + label(target) + ";";
    };
    line(e, "switch (tmp) {");
    for (const u32 target : forward) {
        line(e, "    case " + hexu(target) + ": " + enter(target));
    }
    for (const u32 target : backward) {
        line(e, "    case " + hexu(target) + ":");
    }
    if (!backward.empty()) {
        line(e, "        break;");
    }
    line(e, "    default:");
    line(e, "        recomp::jump_indirect(c, tmp);");
    line(e, "        return;");
    line(e, "}");
    if (backward.empty()) {
        return;
    }
    line(e, "c.pc = tmp;");
    line(e, "if (c.needs_dispatch()) { c.leave_to_dispatcher(tmp, FallbackReason::Yield, " +
                hexu(site) + "); return; }");
    line(e, "switch (tmp) {");
    for (const u32 target : backward) {
        line(e, "    case " + hexu(target) + ": " + enter(target));
    }
    line(e, "    default:");
    line(e, "        break;");
    line(e, "}");
    line(e, "recomp::jump_indirect(c, tmp);");
    line(e, "return;");
}

void emit_shared_indirect_dispatch(EmitContext& e) {
    if (!e.indirect_dispatch_used) return;
    e.body += "gekko_indirect_dispatch:\n";
    // Only known local labels get the local backward-branch poll. Unknown
    // targets retain jump_indirect's exact handling, even inside this extent.
    line(e, "switch (tmp) {");
    for (const u32 target : e.labels) {
        if (target >= e.function_start && target < e.function_end)
            line(e, "    case " + hexu(target) + ":");
    }
    line(e, "        goto gekko_indirect_local;");
    line(e, "    default: recomp::jump_indirect(c, tmp); return;");
    line(e, "}");
    e.body += "gekko_indirect_local:\n";
    line(e, "if (tmp <= gekko_indirect_site) {");
    line(e, "    c.pc = tmp;");
    line(e, "    if (c.needs_dispatch()) { c.leave_to_dispatcher(tmp, FallbackReason::Yield, gekko_indirect_site); return; }");
    line(e, "}");
    line(e, "switch (tmp) {");
    for (const u32 target : e.labels) {
        if (target < e.function_start || target >= e.function_end) continue;
        const auto owner = e.recompiled->find(target);
        const std::string hook = owner != e.recompiled->end() && owner->second == target
            ? "if (c.try_hle(" + hexu(target) + ")) return; " : "";
        line(e, "    case " + hexu(target) + ": " + hook + "goto " + label(target) + ";");
    }
    line(e, "    default: return;"); // Membership was checked above; tmp is a local.
    line(e, "}");
}

bool emit_instruction(EmitContext& e, u32 address, u32 instruction);

}  // namespace

// ---------------------------------------------------------------------------
// classify
// ---------------------------------------------------------------------------

namespace {

Decoded classify(u32 instruction, u32 address) {
    Decoded result;
    const u32 primary = op_primary(instruction);
    switch (primary) {
        case 16: {  // bc
            const u32 bo = field_d(instruction);
            const bool absolute = ((instruction >> 1) & 1u) != 0u;
            result.target = static_cast<u32>(sign_extend(instruction & 0xfffcu, 16)) +
                            (absolute ? 0u : address);
            const bool unconditional = (bo & 0x14u) == 0x14u;
            if ((instruction & 1u) != 0u) {
                result.flow = unconditional ? Flow::Call : Flow::CondCall;
            } else {
                result.flow = unconditional ? Flow::Branch : Flow::CondBranch;
            }
            return result;
        }
        case 17:  // sc
            result.flow = Flow::Terminal;
            return result;
        case 18: {  // b
            const bool absolute = ((instruction >> 1) & 1u) != 0u;
            result.target = static_cast<u32>(sign_extend(instruction & 0x03fffffcu, 26)) +
                            (absolute ? 0u : address);
            result.flow = (instruction & 1u) != 0u ? Flow::Call : Flow::Branch;
            return result;
        }
        case 19: {
            const u32 extended = op_extended(instruction);
            // The link bit decides whether control comes back. bclrl and bcctrl
            // are calls: the callee returns to the instruction after them, so
            // the code that follows is reachable and has to be translated. Only
            // the non-linking forms end the path here.
            const bool link = (instruction & 1u) != 0u;
            if (extended == 16) {  // bclr / bclrl
                const u32 bo = field_d(instruction);
                result.target = 0xffffffffu;
                result.flow = link ? Flow::CondCall
                                   : (((bo & 0x14u) == 0x14u) ? Flow::Return : Flow::CondBranch);
                return result;
            }
            if (extended == 528) {  // bcctr / bcctrl
                const u32 bo = field_d(instruction);
                result.target = 0xffffffffu;
                result.flow = link ? Flow::CondCall
                                   : (((bo & 0x10u) != 0u) ? Flow::Indirect : Flow::CondBranch);
                return result;
            }
            if (extended == 50) {  // rfi
                result.flow = Flow::Terminal;
                return result;
            }
            return result;
        }
        default:
            return result;
    }
}

// The GPRs an instruction writes (bit n = rn), for the formation analysis's
// block-local reaching definitions; kUnmodelledWrites for a form not listed
// here, which keeps that analysis conservative. Branch-with-link and system
// calls are handled by the caller (they end the walk), not here.
constexpr u64 kUnmodelledWrites = ~0ull;

u64 gpr_writes_31(u32 instruction, u64 d, u64 a) {
    switch (op_extended(instruction)) {
        case 19: case 20: case 23: case 83: case 87: case 279: case 310: case 339: case 343:
        case 371: case 534: case 595: case 659: case 790:
            return d;  // mfcr lwarx lwzx mfmsr lbzx lhzx eciwx mfspr lhax mftb lwbrx mfsr mfsrin lhbrx
        case 55: case 119: case 311: case 375:
            return d | a;  // lwzux lbzux lhzux lhaux
        case 24: case 26: case 28: case 60: case 124: case 284: case 316: case 412: case 444:
        case 476: case 536: case 792: case 824: case 922: case 954:
            return a;  // slw cntlzw and andc nor eqv xor orc or nand srw sraw srawi extsh extsb
        case 183: case 247: case 439: case 567: case 631: case 695: case 759:
            return a;  // stwux stbux sthux lfsux lfdux stfsux stfdux
        case 0: case 4: case 32: case 54: case 86: case 144: case 146: case 150: case 151:
        case 210: case 215: case 242: case 246: case 278: case 306: case 370: case 407:
        case 438: case 467: case 470: case 512: case 535: case 566: case 598: case 599:
        case 661: case 662: case 663: case 725: case 727: case 854: case 918: case 982:
        case 983: case 1014:
            return 0u;  // compares, traps, cache ops, mt*, stores, FP loads/stores, syncs
        default:
            break;
    }
    switch (op_extended(instruction) & 0x1ffu) {  // XO-form arithmetic, OE either way
        case 8: case 10: case 11: case 40: case 75: case 104: case 136: case 138: case 200:
        case 202: case 232: case 234: case 235: case 266: case 459: case 491:
            return d;
        default:
            return kUnmodelledWrites;
    }
}

u64 gpr_writes(u32 instruction) {
    const u64 d = 1ull << field_d(instruction);
    const u64 a = 1ull << field_a(instruction);
    switch (op_primary(instruction)) {
        case 7: case 8: case 12: case 13: case 14: case 15:  // mulli subfic addic addic. addi addis
        case 32: case 34: case 40: case 42:                    // lwz lbz lhz lha
            return d;
        case 33: case 35: case 41: case 43:                    // lwzu lbzu lhzu lhau
            return d | a;
        case 20: case 21: case 23:                             // rlwimi rlwinm rlwnm
        case 24: case 25: case 26: case 27: case 28: case 29:  // ori oris xori xoris andi. andis.
        case 37: case 39: case 45:                             // stwu stbu sthu
        case 49: case 51: case 53: case 55:                    // lfsu lfdu stfsu stfdu
        case 57: case 61:                                      // psq_lu psq_stu
            return a;
        case 3: case 10: case 11:                              // twi cmpli cmpi
        case 16: case 18: case 19:                             // branches, CR logic
        case 36: case 38: case 44: case 47:                    // stw stb sth stmw
        case 48: case 50: case 52: case 54:                    // lfs lfd stfs stfd
        case 56: case 59: case 60: case 63:                    // psq_l, FP single, psq_st, FP double
            return 0u;
        case 46:                                               // lmw: rD..r31
            return (0xffffffffull >> field_d(instruction)) << field_d(instruction);
        case 4:                                                // paired singles, dcbz_l
            return ((instruction >> 1) & 0x3fu) == 38u || ((instruction >> 1) & 0x3fu) == 39u ? a : 0u;
        case 31:
            return gpr_writes_31(instruction, d, a);
        default:
            return kUnmodelledWrites;
    }
}

// ---------------------------------------------------------------------------
// Reachability walk inside one function
// ---------------------------------------------------------------------------

struct FunctionAnalysis {
    std::set<u32> reachable;
    std::set<u32> labels;
    std::set<u32> direct_calls;
    // Addresses inside this function that execution can arrive at from outside
    // it: the instruction after every call. A callee that hands control back,
    // or a guest handler that returns through a saved SRR0, resumes here, and
    // unless the address is an entry point the dispatcher has no native code to
    // resume into and the rest of the function runs interpreted.
    std::set<u32> resume_points;
    // Backward-branch targets: the head of every loop. Loop heads, together with
    // the resume points above (after a call returns) and the addresses of and
    // after mtmsr, are where a translated function hands back for an interrupt
    // or a budget yield - and therefore where rfi resumes the guest. Unless
    // each is a dispatch entry, returning from an interrupt strands the rest of
    // the loop, and everything it calls, in the interpreter. (An mtmsr address
    // itself is not made an entry: the dispatcher's range entry resumes there,
    // and it is reached only when an interrupt was taken just before it.)
    std::set<u32> loop_heads;
    // Entries the function table missed: code that follows a terminator inside
    // the declared extent, i.e. a second routine merged into one record.
    std::set<u32> recovered_entries;
    // Direct branch targets outside the extent that are BACKWARD relative to
    // the branch site, i.e. the back edge of a guest loop whose head lives in
    // another record. These are the only transfers that can recurse one host
    // frame per iteration (F1c finding D1), and they are tracked separately
    // because `loop_heads` and `external_targets` are mutually exclusive by
    // construction - the in-extent test is an if/else - so their intersection
    // is empty and cannot be used to find them.
    std::set<u32> external_loop_targets;
    // Direct branch targets that land outside this function's declared extent.
    // The function table has overlapping records, so a branch can legitimately
    // aim at an address the analysis of *this* function cannot label but that
    // another accepted function translates. Recorded here so the global pass can
    // turn each one into a real entry point instead of an interpreter hand-back.
    std::set<u32> external_targets;
    bool ok = true;
    std::string failure;
};

FunctionAnalysis analyse(const DolImage& image, u32 start, u32 size) {
    FunctionAnalysis analysis;
    const u32 end = start + size;
    std::vector<u32> work{start};
    while (!work.empty()) {
        const u32 address = work.back();
        work.pop_back();
        if (address < start || address >= end) {
            continue;
        }
        if (!analysis.reachable.insert(address).second) {
            continue;
        }
        if (!image.executable_at(address)) {
            analysis.ok = false;
            analysis.failure = "code outside an executable section";
            return analysis;
        }
        const u32 instruction = image.read32(address);
        const Decoded decoded = classify(instruction, address);
        switch (decoded.flow) {
            case Flow::Normal:
                work.push_back(address + 4u);
                break;
            case Flow::Call:
            case Flow::CondCall:
                if (decoded.target != 0xffffffffu) {
                    analysis.direct_calls.insert(decoded.target);
                }
                if (address + 4u < end) {
                    analysis.resume_points.insert(address + 4u);
                }
                work.push_back(address + 4u);
                break;
            case Flow::Branch:
                if (decoded.target >= start && decoded.target < end) {
                    analysis.labels.insert(decoded.target);
                    if (decoded.target <= address) analysis.loop_heads.insert(decoded.target);
                    work.push_back(decoded.target);
                } else if (decoded.target != 0xffffffffu) {
                    analysis.external_targets.insert(decoded.target);
                    if (decoded.target <= address) {
                        analysis.external_loop_targets.insert(decoded.target);
                    }
                }
                break;
            case Flow::CondBranch:
                if (decoded.target != 0xffffffffu) {
                    if (decoded.target >= start && decoded.target < end) {
                        analysis.labels.insert(decoded.target);
                        if (decoded.target <= address) analysis.loop_heads.insert(decoded.target);
                        work.push_back(decoded.target);
                    } else {
                        analysis.external_targets.insert(decoded.target);
                        if (decoded.target <= address) {
                            analysis.external_loop_targets.insert(decoded.target);
                        }
                    }
                }
                work.push_back(address + 4u);
                break;
            case Flow::Terminal:
                // sc resumes at the following instruction through SRR0 once the
                // handler returns, so that address is reachable and has to be an
                // entry point - otherwise every system call strands the rest of
                // the function. rfi has no such successor.
                if (op_primary(instruction) == 17u && address + 4u < end) {
                    analysis.resume_points.insert(address + 4u);
                    work.push_back(address + 4u);
                }
                break;
            case Flow::Return:
            case Flow::Indirect:
                break;
        }
    }

    // The function table under-segments: several records cover two routines,
    // because the second one is only ever reached through a computed address and
    // nothing branches to it directly. Walking from the declared entry stops at
    // the first routine's blr and leaves the rest of the extent untranslated, so
    // the guest's computed jump lands on an address with no native code and the
    // machine has to interpret from there.
    //
    // Code that follows a terminator inside the declared extent is another
    // entry. Seeding the walk there recovers it. But a root is only a guess:
    // the same gaps also hold alignment padding, embedded tables and strings
    // (0x4D657472, "Metr", right after a blr in .init). Everything reached
    // from a root is therefore walked into a TENTATIVE set first, and the root
    // is dropped - its instructions, labels, loop heads, calls, resume points
    // and external targets with it - if any of those instructions is at an
    // address outside an executable section or is one the emitter does not
    // model. A speculative root can never make a function that is valid from
    // its declared entry fail; only the walk from the entry above can.
    if (g_gap_sweep) {
        const std::unordered_map<u32, u32> no_translations;
        EmitContext probe;
        probe.image = &image;
        probe.recompiled = &no_translations;
        probe.function_start = start;
        probe.function_end = end;
        // Exactly the per-instruction emitter emit_function runs, on a scratch
        // context: emission succeeds or fails on the instruction word alone.
        const auto emittable = [&probe](u32 address, u32 instruction) {
            probe.body.clear();
            probe.ok = true;
            probe.failure.clear();
            probe.fp_guard_live = false;
            probe.hid2_proven = 0u;
            probe.cr_fuse_armed_field = -1;
            return emit_instruction(probe, address, instruction);
        };
        std::set<u32> dropped_roots;
        for (;;) {
            u32 root = 0;
            bool found = false;
            for (u32 address = start; address + 4u <= end; address += 4u) {
                if (analysis.reachable.count(address) != 0) continue;
                if (analysis.reachable.count(address - 4u) == 0) continue;
                const Decoded previous = classify(image.read32(address - 4u), address - 4u);
                if (previous.flow != Flow::Return && previous.flow != Flow::Branch &&
                    previous.flow != Flow::Indirect && previous.flow != Flow::Terminal) {
                    continue;
                }
                if (!image.executable_at(address)) continue;
                if (image.read32(address) == 0u) continue;  // padding, not code
                if (dropped_roots.count(address) != 0) continue;
                root = address;
                found = true;
                break;
            }
            if (!found) break;

            FunctionAnalysis tentative;
            bool keep = true;
            std::vector<u32> pending{root};
            while (keep && !pending.empty()) {
                const u32 address = pending.back();
                pending.pop_back();
                if (address < start || address >= end) continue;
                if (analysis.reachable.count(address) != 0) continue;
                if (!tentative.reachable.insert(address).second) continue;
                const u32 instruction = image.read32(address);
                if (!image.executable_at(address) || !emittable(address, instruction)) {
                    keep = false;
                    break;
                }
                const Decoded decoded = classify(instruction, address);
                switch (decoded.flow) {
                    case Flow::Normal:
                        pending.push_back(address + 4u);
                        break;
                    case Flow::Call:
                    case Flow::CondCall:
                        if (decoded.target != 0xffffffffu) {
                            tentative.direct_calls.insert(decoded.target);
                        }
                        if (address + 4u < end) tentative.resume_points.insert(address + 4u);
                        pending.push_back(address + 4u);
                        break;
                    case Flow::Branch:
                        if (decoded.target >= start && decoded.target < end) {
                            tentative.labels.insert(decoded.target);
                            if (decoded.target <= address) tentative.loop_heads.insert(decoded.target);
                            pending.push_back(decoded.target);
                        } else if (decoded.target != 0xffffffffu) {
                            tentative.external_targets.insert(decoded.target);
                            if (decoded.target <= address) {
                                tentative.external_loop_targets.insert(decoded.target);
                            }
                        }
                        break;
                    case Flow::CondBranch:
                        if (decoded.target != 0xffffffffu) {
                            if (decoded.target >= start && decoded.target < end) {
                                tentative.labels.insert(decoded.target);
                                if (decoded.target <= address) {
                                    tentative.loop_heads.insert(decoded.target);
                                }
                                pending.push_back(decoded.target);
                            } else {
                                tentative.external_targets.insert(decoded.target);
                                if (decoded.target <= address) {
                                    tentative.external_loop_targets.insert(decoded.target);
                                }
                            }
                        }
                        pending.push_back(address + 4u);
                        break;
                    case Flow::Terminal:
                        if (op_primary(instruction) == 17u && address + 4u < end) {
                            tentative.resume_points.insert(address + 4u);
                            pending.push_back(address + 4u);
                        }
                        break;
                    case Flow::Return:
                    case Flow::Indirect:
                        break;
                }
            }
            if (!keep) {
                dropped_roots.insert(root);
                ++g_sweep_roots_dropped;
                continue;
            }
            analysis.recovered_entries.insert(root);
            analysis.reachable.insert(tentative.reachable.begin(), tentative.reachable.end());
            analysis.labels.insert(tentative.labels.begin(), tentative.labels.end());
            analysis.loop_heads.insert(tentative.loop_heads.begin(), tentative.loop_heads.end());
            analysis.direct_calls.insert(tentative.direct_calls.begin(),
                                         tentative.direct_calls.end());
            analysis.resume_points.insert(tentative.resume_points.begin(),
                                          tentative.resume_points.end());
            analysis.external_loop_targets.insert(tentative.external_loop_targets.begin(),
                                                  tentative.external_loop_targets.end());
            analysis.external_targets.insert(tentative.external_targets.begin(),
                                             tentative.external_targets.end());
        }
    }
    return analysis;
}

// ---------------------------------------------------------------------------
// Instruction emission
// ---------------------------------------------------------------------------

void emit_load_store_d(EmitContext& e, u32 address, u32 instruction, u32 primary);
bool emit_group4(EmitContext& e, u32 address, u32 instruction);
bool emit_group19(EmitContext& e, u32 address, u32 instruction);
bool emit_group31(EmitContext& e, u32 address, u32 instruction);
bool emit_group59(EmitContext& e, u32 address, u32 instruction);
bool emit_group63(EmitContext& e, u32 address, u32 instruction);

bool emit_instruction(EmitContext& e, u32 address, u32 instruction) {
    // CPU-18: only the instruction IMMEDIATELY after a compare may fuse with it.
    e.current_address = address;
    e.cr_fuse_field = e.cr_fuse_armed_field;
    for (int i = 0; i < 4; ++i) e.cr_fuse_bit[i] = e.cr_fuse_armed_bit[i];
    e.cr_fuse_armed_field = -1;
    const u32 primary = op_primary(instruction);
    const u32 d = field_d(instruction);
    const u32 a = field_a(instruction);
    const u32 next = address + 4u;

    // Floating-point unavailable, exception 0x800.
    //
    // The GameCube OS switches FPU context LAZILY: it clears MSR[FP] on a thread
    // switch and never saves or loads the register file itself, relying on the
    // resumed thread's next floating-point instruction faulting here so its
    // handler can swap the FPRs and set MSR[FP]. The game installs that handler
    // (SDK exception index 7, entered through the native OS exception entry).
    //
    // Executing floating point regardless of MSR[FP] therefore turns the FPU
    // into ONE register file shared by every guest thread. Measured on the Gun
    // Fortress route: MSR[FP] goes 1 -> 0 once during OS init and never returns,
    // and the guest reschedules from inside its interrupt handlers 1,911 times
    // before the four-second mark. EffectParticle*::vf2 (0x80434824) then ran
    // with f1..f4 = {-1.0, 0.0, 0.6, 0.0} where it had loaded {-pi, 2.0, 0.0,
    // pi}, so its rotation-wrap loop stepped by f2*f4 = 0.0, never converged,
    // and the guest span there for ever.
    //
    // This is a FAULT: the instruction does not execute, and SRR0 addresses it
    // so the handler's rfi re-executes it once MSR[FP] is on.
    if (e.labels.count(address) != 0) {
        e.fp_guard_live = false;
        e.hid2_proven = 0u;
    }
    // HID2 enables, exception 0x700 (audit paired-single-10). PAGE 90: dcbz_l
    // needs HID2[LCE], psq_l/psq_lu/psq_st/psq_stu HID2[PSE|LSQE], every other
    // primary-4 instruction HID2[PSE]. LSQE and PSE are machine invariants
    // (gekko::kHid2PairedSingleEnables: set at reset and kept by every HID2
    // write the runtime accepts, Cpu::write_hid2), so the one run-time test
    // left is LCE, for dcbz_l, which is not an FP instruction (change C,
    // 2026-10-05). HID2 changes only through mtspr 920, so that proof hoists
    // exactly like MSR[FP].
    const u32 hid2_required = gekko::instruction_checked_hid2(instruction);
    const bool hid2_check = hid2_required != 0u &&
                            !(e.fusion_allowed && (e.hid2_proven & hid2_required) == hid2_required);
    if (gekko::instruction_requires_fp(instruction)) {
        if (hid2_required != 0u) {
            fail(e, "an FP instruction with a run-time HID2 enable at " + hexu(address));
        }
        // Hot body, not a label, and a previous FP instruction in this run
        // already proved MSR[FP]: the bit cannot change without mtmsr, a call,
        // or a merge point, all of which clear fp_guard_live. Cold bodies label
        // every address, so they keep the per-instruction check (sc/rfi entry).
        const bool hoist = e.fusion_allowed && e.fp_guard_live;
        if (hoist) {
            ++g_fp_checks_hoisted;
        } else {
            ++g_fp_checks_emitted;
            line(e, "if ((c.msr() & 0x2000u) == 0u) { recomp::fp_unavailable(c, " + hexu(address) +
                        "); return; }");
        }
        e.fp_guard_live = true;
    } else if (hid2_check) {
        line(e, "if ((c.spr[920] & " + hexu(hid2_required) + ") != " + hexu(hid2_required) +
                    ") { recomp::illegal_instruction(c, " + hexu(address) + "); return; }");
    }
    e.hid2_proven |= hid2_required;

    switch (primary) {
        case 3: {  // twi
            // Perform the same exception entry as Cpu::execute: the OS's
            // entry is native, and the dispatcher continues at the handler.
            line(e, "if (ppc::trap_taken(" + hexu(d) + ", static_cast<s32>(" + gpr(a) + "), " +
                        std::to_string(field_simm(instruction)) + ")) {");
            line(e, "    c.pc = " + hexu(address) + ";");
            line(e, "    c.raise_exception(Exception::Program);");
            line(e, "    c.leave_to_dispatcher(c.npc, FallbackReason::Trap, " + hexu(address) + ");");
            line(e, "    return;");
            line(e, "}");
            return true;
        }
        case 4:
            return emit_group4(e, address, instruction);
        case 7:  // mulli
            line(e, gpr(d) + " = ppc::mulli(" + gpr(a) + ", " +
                        std::to_string(field_simm(instruction)) + ");");
            return true;
        case 8:  // subfic
            line(e, gpr(d) + " = ppc::subfic(c, " + gpr(a) + ", " + simm_u32(instruction) + ");");
            return true;
        case 10: {  // cmpli
            line(e, "ppc::cmpl(c, " + std::to_string(field_crfd(instruction)) + "u, " + gpr(a) +
                        ", " + hexu(field_uimm(instruction)) + ");");
            const std::string r = hexu(field_uimm(instruction));
            arm_cr_fuse(e, field_crfd(instruction), gpr(a) + " < " + r, gpr(a) + " > " + r,
                        gpr(a) + " == " + r);
            return true;
        }
        case 11: {  // cmpi
            line(e, "ppc::cmp(c, " + std::to_string(field_crfd(instruction)) + "u, static_cast<s32>(" +
                        gpr(a) + "), " + std::to_string(field_simm(instruction)) + ");");
            const std::string l = "static_cast<s32>(" + gpr(a) + ")";
            const std::string r = std::to_string(field_simm(instruction));
            arm_cr_fuse(e, field_crfd(instruction), l + " < " + r, l + " > " + r, l + " == " + r);
            return true;
        }
        case 12:  // addic
        case 13:  // addic.
            line(e, gpr(d) + " = ppc::addic(c, " + gpr(a) + ", " + simm_u32(instruction) + ");");
            if (primary == 13) {
                line(e, "ppc::update_cr0(c, " + gpr(d) + ");");
            }
            return true;
        case 14:  // addi / li
            // A listed platform value site (validated as exactly this li
            // when the list was read): the native platform's value.
            if (const auto site = g_platform_sites.find(address); site != g_platform_sites.end()) {
                if (site->second.word != instruction || a != 0u) {
                    fail(e, "platform site " + hex32(address) + " is not its listed li");
                    return true;
                }
                line(e, gpr(d) + " = c.platform_value(" + hexu(address) + ");");
                g_platform_sites_emitted.insert(address);
                ++g_platform_site_emissions;
                return true;
            }
            line(e, gpr(d) + " = " + gpr_or_zero(a) + " + " + simm_u32(instruction) + ";");
            return true;
        case 15:  // addis / lis
            line(e, gpr(d) + " = " + gpr_or_zero(a) + " + " +
                        hexu(field_uimm(instruction) << 16) + ";");
            return true;
        case 16: {  // bc
            const Decoded decoded = classify(instruction, address);
            const std::string condition = emit_branch_condition(e, instruction, true);
            const bool link = (instruction & 1u) != 0u;
            // The link register is written whether or not the branch is taken.
            if (link) {
                line(e, "c.lr = " + hexu(next) + ";");
            }
            if (condition == "true") {
                if (link) {
                    emit_call_transfer(e, decoded.target, next);
                } else {
                    emit_jump(e, decoded.target, address);
                }
                return true;
            }
            line(e, "cnd = " + condition + ";");
            line(e, "if (cnd) {");
            {
                // The taken side is emitted into a scratch buffer and indented,
                // so calls and jumps go through exactly one code path.
                std::string saved;
                saved.swap(e.body);
                if (link) {
                    emit_call_transfer(e, decoded.target, next);
                } else {
                    emit_jump(e, decoded.target, address);
                }
                std::string taken;
                taken.swap(e.body);
                e.body.swap(saved);
                std::istringstream stream(taken);
                std::string text;
                while (std::getline(stream, text)) {
                    e.body += "    " + text + "\n";
                }
            }
            line(e, "}");
            return true;
        }
        case 17:  // sc
            // The transfer is performed natively - the same arithmetic the
            // interpreter did - and then control is handed back to the
            // dispatcher, which continues at the handler the OS entry chose.
            // An exception return must unwind rather than call forward, or every
            // interrupt taken inside a guest loop would leave frames on the host
            // stack that are never released. Nothing here is interpreted.
            line(e, "recomp::system_call(c, " + hexu(address) + ");");
            line(e, "return;");
            return true;
        case 18: {  // b / bl
            const Decoded decoded = classify(instruction, address);
            if ((instruction & 1u) != 0u) {
                if (const auto site = g_lift_sites.find(address); site != g_lift_sites.end()) {
                    if (site->second.word != instruction || lift_site_is_bctrl(site->second)) {
                        fail(e, "lowered call site " + hex32(address) + " is not its listed bl");
                        return false;
                    }
                    emit_lift_site(e, address);
                    return true;
                }
                line(e, "c.lr = " + hexu(next) + ";");
                emit_call_transfer(e, decoded.target, next);
            } else {
                emit_jump(e, decoded.target, address);
            }
            return true;
        }
        case 19:
            return emit_group19(e, address, instruction);
        case 20: {  // rlwimi
            const u32 mask = mask_from((instruction >> 6) & 31u, (instruction >> 1) & 31u);
            line(e, gpr(a) + " = (ppc::rotl32(" + gpr(d) + ", " + std::to_string(field_b(instruction)) +
                        "u) & " + hexu(mask) + ") | (" + gpr(a) + " & " + hexu(~mask) + ");");
            emit_cr0(e, instruction, gpr(a));
            return true;
        }
        case 21: {  // rlwinm
            const u32 mask = mask_from((instruction >> 6) & 31u, (instruction >> 1) & 31u);
            line(e, gpr(a) + " = ppc::rotl32(" + gpr(d) + ", " +
                        std::to_string(field_b(instruction)) + "u) & " + hexu(mask) + ";");
            emit_cr0(e, instruction, gpr(a));
            return true;
        }
        case 23: {  // rlwnm
            const u32 mask = mask_from((instruction >> 6) & 31u, (instruction >> 1) & 31u);
            line(e, gpr(a) + " = ppc::rotl32(" + gpr(d) + ", " + gpr(field_b(instruction)) +
                        " & 31u) & " + hexu(mask) + ";");
            emit_cr0(e, instruction, gpr(a));
            return true;
        }
        case 24:  // ori
            line(e, gpr(a) + " = " + gpr(d) + " | " + hexu(field_uimm(instruction)) + ";");
            return true;
        case 25:  // oris
            line(e, gpr(a) + " = " + gpr(d) + " | " + hexu(field_uimm(instruction) << 16) + ";");
            return true;
        case 26:  // xori
            line(e, gpr(a) + " = " + gpr(d) + " ^ " + hexu(field_uimm(instruction)) + ";");
            return true;
        case 27:  // xoris
            line(e, gpr(a) + " = " + gpr(d) + " ^ " + hexu(field_uimm(instruction) << 16) + ";");
            return true;
        case 28:  // andi.
            line(e, gpr(a) + " = " + gpr(d) + " & " + hexu(field_uimm(instruction)) + ";");
            line(e, "ppc::update_cr0(c, " + gpr(a) + ");");
            return true;
        case 29:  // andis.
            line(e, gpr(a) + " = " + gpr(d) + " & " + hexu(field_uimm(instruction) << 16) + ";");
            line(e, "ppc::update_cr0(c, " + gpr(a) + ");");
            return true;
        case 31:
            return emit_group31(e, address, instruction);
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37:
        case 38:
        case 39:
        case 40:
        case 41:
        case 42:
        case 43:
        case 44:
        case 45:
        case 46:
        case 47:
        case 48:
        case 49:
        case 50:
        case 51:
        case 52:
        case 53:
        case 54:
        case 55:
            emit_load_store_d(e, address, instruction, primary);
            return e.ok;
        case 56:  // psq_l
        case 57:  // psq_lu
        case 60:  // psq_st
        case 61: {  // psq_stu
            const u32 offset = static_cast<u32>(sign_extend(instruction & 0xfffu, 12));
            const bool w = ((instruction >> 15) & 1u) != 0u;
            const u32 gqr = (instruction >> 12) & 7u;
            const bool update = (primary == 57 || primary == 61);
            line(e, "ea = " + gpr_or_zero(a) + " + " + hexu(offset) + ";");
            if (const auto site = g_gx_sites.find(address); site != g_gx_sites.end()) {
                // A listed GX pipe site: dead (nothing, keep the update) or
                // vertex data, quantised by its GQR exactly as the store would.
                if (site->second.word != instruction || !gx_family_accepts(site->second.family, primary)) {
                    fail(e, "GX pipe site " + hex32(address) + " is not its listed word or form");
                    return false;
                }
                if (g_gx_fail_sites.count(address) != 0u) {
                    line(e, "c.gx_dead_site(" + hexu(address) + ");");
                    ++g_gx_fail_emissions;
                } else if (site->second.family == GxFamily::Vertex) {
                    line(e, "ppc::psq_store_gx(c, " + hexu(address) + ", " + std::to_string(d) + "u, " +
                                (w ? "true" : "false") + ", " + std::to_string(gqr) + "u);");
                }
                if (update && a != 0) {
                    line(e, gpr(a) + " = ea;");
                }
                g_gx_sites_emitted.insert(address);
                ++g_gx_site_emissions;
                return true;
            }
            const std::string psq_args = std::to_string(d) + "u, ea, " + (w ? "true" : "false") + ", " +
                                         std::to_string(gqr) + "u);";
            const char* const psq_kind = (primary == 56 || primary == 57) ? "load" : "store";
            if (const auto spec = g_psq_sites.find(address); spec != g_psq_sites.end()) {
                // Checked at load: the listed GQR index is this instruction's.
                line(e, "if (c.spr[" + std::to_string(912u + gqr) + "u] == " + hexu(spec->second.value) +
                            ") ppc::psq_" + psq_kind + "_k<" + hexu(spec->second.value) + ">(c, " + psq_args +
                            " else ppc::psq_" + psq_kind + "(c, " + psq_args);
                g_psq_sites_emitted.insert(address);
                ++g_psq_site_emissions;
            } else {
                line(e, std::string("ppc::psq_") + psq_kind + "(c, " + psq_args);
            }
            if (update && a != 0) {
                line(e, gpr(a) + " = ea;");
            }
            return true;
        }
        case 59:
            return emit_group59(e, address, instruction);
        case 63:
            return emit_group63(e, address, instruction);
        default:
            fail(e, "primary opcode " + std::to_string(primary));
            return false;
    }
}

void emit_load_store_d(EmitContext& e, u32 address, u32 instruction, u32 primary) {
    const u32 d = field_d(instruction);
    const u32 a = field_a(instruction);
    const bool update = (primary & 1u) != 0u && primary != 46 && primary != 47;
    const std::string base = update ? gpr(a) : gpr_or_zero(a);

    if (primary == 46 || primary == 47) {  // lmw / stmw
        line(e, "ea = " + gpr_or_zero(a) + " + " + simm_u32(instruction) + ";");
        for (u32 reg = d; reg < 32; ++reg) {
            if (primary == 46) {
                line(e, gpr(reg) + " = " + mem_r(e, "32", "ea") + ";");
            } else {
                line(e, "c.mem_write32(ea, " + gpr(reg) + ");");
            }
            if (reg != 31) {
                line(e, "ea += 4u;");
            }
        }
        return;
    }

    line(e, "ea = " + base + " + " + simm_u32(instruction) + ";");
    // A listed platform site (g_platform_sites, validated as exactly this
    // lhz/sth word when the list was read): the effective address above, then
    // the native runtime instead of the bus. Every copy of the instruction -
    // hot body, cold body - is lowered; nothing else about it changes.
    if (const auto site = g_platform_sites.find(address); site != g_platform_sites.end()) {
        if (site->second.word != instruction || (primary != 40u && primary != 44u)) {
            fail(e, "platform site " + hex32(address) + " is not its listed lhz/sth");
            return;
        }
        if (site->second.kind == kPlatformKindDead) {
            // A dead store (sth only, checked when the list was read): the
            // register it would have programmed has no consumer.
            ++g_platform_site_dead_emissions;
        } else if (primary == 40u) {  // lhz rD,d(rA)
            line(e, gpr(d) + " = static_cast<u32>(c.platform_read16(" + hexu(address) + ", ea));");
        } else {  // sth rS,d(rA)
            line(e, "c.platform_write16(" + hexu(address) + ", ea, static_cast<u16>(" + gpr(d) + "));");
        }
        g_platform_sites_emitted.insert(address);
        ++g_platform_site_emissions;
        return;
    }
    // A listed GX pipe site (g_gx_sites, validated as exactly this word and a
    // form its family accepts when the list was read): the effective address
    // above, then what the store meant instead of the store.
    if (const auto site = g_gx_sites.find(address); site != g_gx_sites.end()) {
        if (site->second.word != instruction || !gx_family_accepts(site->second.family, primary)) {
            fail(e, "GX pipe site " + hex32(address) + " is not its listed word or form");
            return;
        }
        const std::string at = hexu(address);
        switch (g_gx_fail_sites.count(address) != 0u ? GxFamily::Dead : site->second.family) {
            case GxFamily::Dead:
                if (g_gx_fail_sites.count(address) != 0u) {
                    line(e, "c.gx_dead_site(" + at + ");");
                    ++g_gx_fail_emissions;
                }
                break;
            case GxFamily::DrawSyncToken:
                line(e, "c.gx_draw_sync_token(" + at + ", static_cast<u16>(" + gpr(d) + "), false);");
                break;
            case GxFamily::DrawSyncTokenIrq:
                line(e, "c.gx_draw_sync_token(" + at + ", static_cast<u16>(" + gpr(d) + "), true);");
                break;
            case GxFamily::DrawDone: line(e, "c.gx_draw_done(" + at + ", " + gpr(d) + ");"); break;
            case GxFamily::CopyTrigger: line(e, "c.gx_copy_trigger(" + at + ", " + gpr(d) + ");"); break;
            case GxFamily::PrimitiveOpcode:
                line(e, "c.gx_primitive_opcode(" + at + ", static_cast<u8>(" + gpr(d) + "));");
                break;
            case GxFamily::PrimitiveCount:
                line(e, "c.gx_primitive_count(" + at + ", static_cast<u16>(" + gpr(d) + "));");
                break;
            case GxFamily::Vertex:
                if (primary == 38u) {
                    line(e, "c.gx_vertex(" + at + ", static_cast<u8>(" + gpr(d) + "), 1u);");
                } else if (primary == 44u) {
                    line(e, "c.gx_vertex(" + at + ", static_cast<u16>(" + gpr(d) + "), 2u);");
                } else if (primary == 36u) {
                    line(e, "c.gx_vertex(" + at + ", " + gpr(d) + ", 4u);");
                } else {  // stfs: D.7 store conversion, as the store itself would write it
                    line(e, "c.gx_vertex(" + at + ", double_to_single_bits(" + ps0(d) + "), 4u);");
                }
                break;
        }
        if (update) {
            line(e, gpr(a) + " = ea;");
        }
        g_gx_sites_emitted.insert(address);
        ++g_gx_site_emissions;
        return;
    }
    switch (primary) {
        case 32:
        case 33: line(e, gpr(d) + " = " + mem_r(e, "32", "ea") + ";"); break;
        case 34:
        case 35: line(e, gpr(d) + " = " + mem_r(e, "8", "ea") + ";"); break;
        case 36:
        case 37: line(e, "c.mem_write32(ea, " + gpr(d) + ");"); break;
        case 38:
        case 39: line(e, "c.mem_write8(ea, static_cast<u8>(" + gpr(d) + "));"); break;
        case 40:
        case 41: line(e, gpr(d) + " = " + mem_r(e, "16", "ea") + ";"); break;
        case 42:
        case 43:
            line(e, gpr(d) +
                        " = static_cast<u32>(static_cast<s32>(static_cast<s16>(" +
                        mem_r(e, "16", "ea") + ")));");
            break;
        case 44:
        case 45: line(e, "c.mem_write16(ea, static_cast<u16>(" + gpr(d) + "));"); break;
        case 48:
        case 49:
            // D.6 bit expansion: a signalling NaN is not quieted (audit fpu-4).
            line(e, "tmp64 = single_to_double_bits(" + mem_r(e, "32", "ea") + ");");
            line(e, ps0(d) + " = tmp64;");
            line(e, ps1(d) + " = tmp64;");
            break;
        case 50:
        case 51: line(e, ps0(d) + " = " + mem_r(e, "64", "ea") + ";"); break;
        case 52:
        case 53:
            // D.7 store conversion: bit selection, truncating denormalisation.
            line(e, "c.mem_write32(ea, double_to_single_bits(" + ps0(d) + "));");
            break;
        case 54:
        case 55: line(e, "c.mem_write64(ea, " + ps0(d) + ");"); break;
        default: fail(e, "load/store form"); return;
    }
    if (update) {
        line(e, gpr(a) + " = ea;");
    }
}

bool emit_group19(EmitContext& e, u32 address, u32 instruction) {
    const u32 extended = op_extended(instruction);
    const u32 next = address + 4u;
    switch (extended) {
        case 0:  // mcrf
            line(e, "ppc::set_cr_field(c, " + std::to_string(field_crfd(instruction)) +
                        "u, ppc::cr_field(c, " + std::to_string(field_crfs(instruction)) + "u));");
            return true;
        case 16: {  // bclr
            const std::string condition = emit_branch_condition(e, instruction, true);
            line(e, "tmp = c.lr & ~3u;");
            const bool link = (instruction & 1u) != 0u;
            if (link) {
                line(e, "c.lr = " + hexu(next) + ";");
            }
            if (condition == "true") {
                line(e, "c.pc = tmp;");
                line(e, "return;");
            } else {
                line(e, "cnd = " + condition + ";");
                line(e, "if (cnd) { c.pc = tmp; return; }");
            }
            return true;
        }
        case 50:  // rfi
            // Restore MSR from SRR1 and resume at SRR0, then unwind to the
            // dispatcher so the interrupted frame's host stack is released. SRR0
            // is a guest-chosen address, so this is genuinely dynamic control
            // flow and uses the same translated-address lookup as any indirect
            // branch - not an interpreter.
            line(e, "recomp::return_from_interrupt(c, " + hexu(address) + ");");
            line(e, "return;");
            return true;
        case 150:  // isync
            return true;
        case 528: {  // bcctr
            const std::string condition = emit_branch_condition(e, instruction, false);
            line(e, "tmp = c.ctr & ~3u;");
            const bool link = (instruction & 1u) != 0u;
            if (link) {
                if (const auto site = g_lift_sites.find(address); site != g_lift_sites.end()) {
                    if (site->second.word != instruction || condition != "true" ||
                        !lift_site_is_bctrl(site->second)) {
                        fail(e, "lowered call site " + hex32(address) + " is not its listed bctrl");
                        return false;
                    }
                    emit_lift_site(e, address);
                    return true;
                }
                line(e, "c.lr = " + hexu(next) + ";");
                const std::string transfer =
                    "recomp::call_indirect(c, tmp, " + hexu(next) + "); " + kCallExitCheck +
                    " if (c.pc != " + hexu(next) +
                    ") { recomp::resume_elsewhere(c, " + hexu(address) + "); return; }";
                if (condition == "true") {
                    line(e, transfer);
                } else {
                    line(e, "cnd = " + condition + ";");
                    line(e, "if (cnd) { " + transfer + " }");
                }
            } else if (condition == "true") {
                emit_indirect_jump(e, address);
            } else {
                line(e, "cnd = " + condition + ";");
                // The taken side is multi-line; emit it through a scratch
                // buffer and indent it, as case 16 (bc) does.
                line(e, "if (cnd) {");
                std::string saved;
                saved.swap(e.body);
                emit_indirect_jump(e, address);
                std::string taken;
                taken.swap(e.body);
                e.body.swap(saved);
                std::istringstream stream(taken);
                std::string text;
                while (std::getline(stream, text)) {
                    e.body += "    " + text + "\n";
                }
                line(e, "}");
            }
            if (link) {
                e.fp_guard_live = false;
                e.hid2_proven = 0u;
            }
            return true;
        }
        case 33:
        case 129:
        case 193:
        case 225:
        case 257:
        case 289:
        case 417:
        case 449: {  // CR logical operations
            const std::string va = "(c.cr >> " + std::to_string(31u - field_a(instruction)) + "u) & 1u";
            const std::string vb = "(c.cr >> " + std::to_string(31u - field_b(instruction)) + "u) & 1u";
            line(e, "cra = " + va + ";");
            line(e, "crb = " + vb + ";");
            std::string expression;
            switch (extended) {
                case 33: expression = "~(cra | crb) & 1u"; break;
                case 129: expression = "cra & ~crb & 1u"; break;
                case 193: expression = "cra ^ crb"; break;
                case 225: expression = "~(cra & crb) & 1u"; break;
                case 257: expression = "cra & crb"; break;
                case 289: expression = "~(cra ^ crb) & 1u"; break;
                case 417: expression = "(cra | ~crb) & 1u"; break;
                default: expression = "cra | crb"; break;
            }
            line(e, "ppc::set_cr_bit(c, " + std::to_string(field_d(instruction)) + "u, " +
                        expression + ");");
            return true;
        }
        default:
            fail(e, "group-19 extended " + std::to_string(extended));
            return false;
    }
}

bool emit_group31(EmitContext& e, u32 address, u32 instruction) {
    const u32 extended = op_extended(instruction);
    const u32 d = field_d(instruction);
    const u32 a = field_a(instruction);
    const u32 b = field_b(instruction);
    const bool rc = field_rc(instruction);
    const bool oe = field_oe(instruction);
    const std::string oe_text = oe ? "true" : "false";
    const std::string address_ab = gpr_or_zero(a) + " + " + gpr(b);

    auto simple = [&](const std::string& expression) {
        line(e, gpr(a) + " = " + expression + ";");
        if (rc) {
            line(e, "ppc::update_cr0(c, " + gpr(a) + ");");
        }
    };
    auto arith = [&](const std::string& expression) {
        line(e, gpr(d) + " = " + expression + ";");
        if (rc) {
            line(e, "ppc::update_cr0(c, " + gpr(d) + ");");
        }
    };

    switch (extended) {
        case 0: {  // cmp
            line(e, "ppc::cmp(c, " + std::to_string(field_crfd(instruction)) + "u, static_cast<s32>(" +
                        gpr(a) + "), static_cast<s32>(" + gpr(b) + "));");
            const std::string l = "static_cast<s32>(" + gpr(a) + ")";
            const std::string r = "static_cast<s32>(" + gpr(b) + ")";
            arm_cr_fuse(e, field_crfd(instruction), l + " < " + r, l + " > " + r, l + " == " + r);
            return true;
        }
        case 32: {  // cmpl
            line(e, "ppc::cmpl(c, " + std::to_string(field_crfd(instruction)) + "u, " + gpr(a) +
                        ", " + gpr(b) + ");");
            arm_cr_fuse(e, field_crfd(instruction), gpr(a) + " < " + gpr(b),
                        gpr(a) + " > " + gpr(b), gpr(a) + " == " + gpr(b));
            return true;
        }
        case 4:  // tw
            line(e, "if (ppc::trap_taken(" + hexu(d) + ", static_cast<s32>(" + gpr(a) +
                        "), static_cast<s32>(" + gpr(b) + "))) {");
            line(e, "    c.pc = " + hexu(address) + ";");
            line(e, "    c.raise_exception(Exception::Program);");
            line(e, "    c.leave_to_dispatcher(c.npc, FallbackReason::Trap, " + hexu(address) + ");");
            line(e, "    return;");
            line(e, "}");
            return true;
        case 8:
        case 520: arith("ppc::subfc(c, " + gpr(a) + ", " + gpr(b) + ", " + oe_text + ")"); return true;
        case 10:
        case 522: arith("ppc::addc(c, " + gpr(a) + ", " + gpr(b) + ", " + oe_text + ")"); return true;
        case 11: arith("ppc::mulhwu(" + gpr(a) + ", " + gpr(b) + ")"); return true;
        case 19:  // mfcr
            line(e, gpr(d) + " = c.cr;");
            return true;
        case 20:  // lwarx: load and set the reservation (ppc::lwarx)
            line(e, "ea = " + address_ab + ";");
            line(e, gpr(d) + " = ppc::lwarx(c, ea);");
            return true;
        case 23:  // lwzx
            line(e, "ea = " + address_ab + ";");
            line(e, gpr(d) + " = " + mem_r(e, "32", "ea") + ";");
            return true;
        case 24: simple("ppc::slw(" + gpr(d) + ", " + gpr(b) + ")"); return true;
        case 26: simple("ppc::cntlzw(" + gpr(d) + ")"); return true;
        case 28: simple(gpr(d) + " & " + gpr(b)); return true;
        case 40:
        case 552:
            if (oe) {
                arith("ppc::subf_oe(c, " + gpr(a) + ", " + gpr(b) + ")");
            } else {
                arith(gpr(b) + " - " + gpr(a));
            }
            return true;
        case 54:
        case 86:
        case 246:
        case 278:
        case 470:
        case 982:
        case 598:
        case 566:
        case 854:
        case 306:
        case 370:  // cache and TLB hints: architecturally no visible effect here
            return true;
        case 55:  // lwzux
            // Every X-form update (manual Chapter 12): EA <- rA + rB, access
            // MEM(EA), rA <- EA. EA is computed before any register is written
            // and rA is written last, as emit_load_store_d does: a store with
            // rS == rA must store the ORIGINAL rS (stwux r1,r1,rX prologues).
            line(e, "ea = " + gpr(a) + " + " + gpr(b) + ";");
            line(e, gpr(d) + " = " + mem_r(e, "32", "ea") + ";");
            line(e, gpr(a) + " = ea;");
            return true;
        case 60: simple(gpr(d) + " & ~" + gpr(b)); return true;
        case 75: arith("ppc::mulhw(" + gpr(a) + ", " + gpr(b) + ")"); return true;
        case 83:  // mfmsr
            line(e, gpr(d) + " = c.msr();");
            return true;
        case 87:  // lbzx
            line(e, "ea = " + address_ab + ";");
            line(e, gpr(d) + " = " + mem_r(e, "8", "ea") + ";");
            return true;
        case 104:
        case 616:
            if (oe) {
                arith("ppc::neg_oe(c, " + gpr(a) + ")");
            } else {
                arith("0u - " + gpr(a));
            }
            return true;
        case 119:  // lbzux
            line(e, "ea = " + gpr(a) + " + " + gpr(b) + ";");
            line(e, gpr(d) + " = " + mem_r(e, "8", "ea") + ";");
            line(e, gpr(a) + " = ea;");
            return true;
        case 124: simple("~(" + gpr(d) + " | " + gpr(b) + ")"); return true;
        case 136:
        case 648: arith("ppc::subfe(c, " + gpr(a) + ", " + gpr(b) + ", " + oe_text + ")"); return true;
        case 138:
        case 650: arith("ppc::adde(c, " + gpr(a) + ", " + gpr(b) + ", " + oe_text + ")"); return true;
        case 144: {  // mtcrf
            const u32 crm = (instruction >> 12) & 0xffu;
            u32 mask = 0;
            for (u32 index = 0; index < 8; ++index) {
                if ((crm & (0x80u >> index)) != 0u) {
                    mask |= 0xfu << (28u - 4u * index);
                }
            }
            line(e, "c.cr = (c.cr & " + hexu(~mask) + ") | (" + gpr(d) + " & " + hexu(mask) + ");");
            return true;
        }
        case 146:  // mtmsr
            // Recognition point R4 (cpu.h, interrupt_lines_), on both sides of
            // the write:
            //   * A condition already deliverable under the OLD MSR is taken
            //     before mtmsr executes: hand back AT this instruction, which the
            //     handler's rfi re-executes.
            //   * After the write, an EE 0 -> 1 with a condition pending is taken
            //     before the next instruction (gekko_user_manual.txt PAGE 475).
            // With nothing deliverable execution stays native, which matters
            // because the OS brackets almost every critical section with this
            // instruction.
            line(e, "if (c.interrupt_pending()) { c.leave_to_dispatcher(" + hexu(address) +
                        ", FallbackReason::Yield, " + hexu(address) + "); return; }");
            line(e, "c.set_msr(" + gpr(d) + ");");
            line(e, "if (c.interrupt_pending()) { c.leave_to_dispatcher(" + hexu(address + 4u) +
                        ", FallbackReason::Yield, " + hexu(address) + "); return; }");
            e.fp_guard_live = false;
            e.hid2_proven = 0u;
            return true;
        case 150:  // stwcx.: stores only while a reservation exists (ppc::stwcx)
            line(e, "ea = " + address_ab + ";");
            line(e, "ppc::stwcx(c, ea, " + gpr(d) + ");");
            return true;
        case 151:  // stwx
            line(e, "ea = " + address_ab + ";");
            line(e, "c.mem_write32(ea, " + gpr(d) + ");");
            return true;
        case 183:  // stwux
            line(e, "ea = " + gpr(a) + " + " + gpr(b) + ";");
            line(e, "c.mem_write32(ea, " + gpr(d) + ");");
            line(e, gpr(a) + " = ea;");
            return true;
        case 200:
        case 712: arith("ppc::subfze(c, " + gpr(a) + ", " + oe_text + ")"); return true;
        case 202:
        case 714: arith("ppc::addze(c, " + gpr(a) + ", " + oe_text + ")"); return true;
        case 210:  // mtsr
            line(e, "c.sr[" + std::to_string((instruction >> 16) & 15u) + "] = " + gpr(d) + ";");
            return true;
        case 242:  // mtsrin
            line(e, "c.sr[(" + gpr(b) + " >> 28) & 15u] = " + gpr(d) + ";");
            return true;
        case 215:  // stbx
            line(e, "ea = " + address_ab + ";");
            line(e, "c.mem_write8(ea, static_cast<u8>(" + gpr(d) + "));");
            return true;
        case 247:  // stbux
            line(e, "ea = " + gpr(a) + " + " + gpr(b) + ";");
            line(e, "c.mem_write8(ea, static_cast<u8>(" + gpr(d) + "));");
            line(e, gpr(a) + " = ea;");
            return true;
        case 232:
        case 744: arith("ppc::subfme(c, " + gpr(a) + ", " + oe_text + ")"); return true;
        case 234:
        case 746: arith("ppc::addme(c, " + gpr(a) + ", " + oe_text + ")"); return true;
        case 235:
        case 747: arith("ppc::mullw(c, " + gpr(a) + ", " + gpr(b) + ", " + oe_text + ")"); return true;
        case 266:
        case 778:
            if (oe) {
                arith("ppc::add_oe(c, " + gpr(a) + ", " + gpr(b) + ")");
            } else {
                arith(gpr(a) + " + " + gpr(b));
            }
            return true;
        case 279:  // lhzx
            line(e, "ea = " + address_ab + ";");
            line(e, gpr(d) + " = " + mem_r(e, "16", "ea") + ";");
            return true;
        case 284: simple("~(" + gpr(d) + " ^ " + gpr(b) + ")"); return true;
        case 311:  // lhzux
            line(e, "ea = " + gpr(a) + " + " + gpr(b) + ";");
            line(e, gpr(d) + " = " + mem_r(e, "16", "ea") + ";");
            line(e, gpr(a) + " = ea;");
            return true;
        case 316: simple(gpr(d) + " ^ " + gpr(b)); return true;
        case 339: {  // mfspr
            const u32 number = spr_number(instruction);
            // The time base and the decrementer are views of the game clock,
            // exact at every instruction boundary (cpu.h).
            line(e, gpr(d) + " = ppc::read_spr(c, " + std::to_string(number) + "u);");
            return true;
        }
        case 343:  // lhax
            line(e, "ea = " + address_ab + ";");
            line(e, gpr(d) +
                        " = static_cast<u32>(static_cast<s32>(static_cast<s16>(" +
                        mem_r(e, "16", "ea") + ")));");
            return true;
        case 371: {  // mftb
            const u32 number = spr_number(instruction);
            if (number == 269u) {
                line(e, gpr(d) + " = static_cast<u32>(c.time_base() >> 32);");
            } else {
                line(e, gpr(d) + " = static_cast<u32>(c.time_base());");
            }
            return true;
        }
        case 375:  // lhaux
            line(e, "ea = " + gpr(a) + " + " + gpr(b) + ";");
            line(e, gpr(d) + " = static_cast<u32>(static_cast<s32>(static_cast<s16>(" +
                        mem_r(e, "16", "ea") + ")));");
            line(e, gpr(a) + " = ea;");
            return true;
        case 407:  // sthx
            line(e, "ea = " + address_ab + ";");
            line(e, "c.mem_write16(ea, static_cast<u16>(" + gpr(d) + "));");
            return true;
        case 412: simple(gpr(d) + " | ~" + gpr(b)); return true;
        case 439:  // sthux
            line(e, "ea = " + gpr(a) + " + " + gpr(b) + ";");
            line(e, "c.mem_write16(ea, static_cast<u16>(" + gpr(d) + "));");
            line(e, gpr(a) + " = ea;");
            return true;
        case 444: simple(gpr(d) + " | " + gpr(b)); return true;
        case 459:
        case 971: arith("ppc::divwu(c, " + gpr(a) + ", " + gpr(b) + ", " + oe_text + ")"); return true;
        case 467: {  // mtspr
            const u32 number = spr_number(instruction);
            if (number == 284u || number == 285u || number == 22u) {
            }
            line(e, "ppc::write_spr(c, " + std::to_string(number) + "u, " + gpr(d) + ");");
            if (number == 920u) {
                e.hid2_proven = 0u;  // HID2: the next Gekko extension re-tests its enables
            }
            return true;
        }
        case 476: simple("~(" + gpr(d) + " & " + gpr(b) + ")"); return true;
        case 491:
        case 1003: arith("ppc::divw(c, " + gpr(a) + ", " + gpr(b) + ", " + oe_text + ")"); return true;
        case 512:  // mcrxr
            line(e, "ppc::set_cr_field(c, " + std::to_string(field_crfd(instruction)) +
                        "u, (c.xer >> 28) & 0xfu);");
            line(e, "c.xer &= 0x0fffffffu;");
            return true;
        case 534:  // lwbrx
            line(e, "ea = " + address_ab + ";");
            line(e, gpr(d) + " = bswap32(" + mem_r(e, "32", "ea") + ");");
            return true;
        case 535:  // lfsx
            line(e, "ea = " + address_ab + ";");
            line(e, "tmp64 = single_to_double_bits(" + mem_r(e, "32", "ea") + ");");
            line(e, ps0(d) + " = tmp64;");
            line(e, ps1(d) + " = tmp64;");
            return true;
        case 536: simple("ppc::srw(" + gpr(d) + ", " + gpr(b) + ")"); return true;
        case 567:  // lfsux
            line(e, "ea = " + gpr(a) + " + " + gpr(b) + ";");
            line(e, "tmp64 = single_to_double_bits(" + mem_r(e, "32", "ea") + ");");
            line(e, ps0(d) + " = tmp64;");
            line(e, ps1(d) + " = tmp64;");
            line(e, gpr(a) + " = ea;");
            return true;
        case 595:  // mfsr
            line(e, gpr(d) + " = c.sr[" + std::to_string((instruction >> 16) & 15u) + "];");
            return true;
        case 599:  // lfdx
            line(e, "ea = " + address_ab + ";");
            line(e, ps0(d) + " = " + mem_r(e, "64", "ea") + ";");
            return true;
        case 631:  // lfdux
            line(e, "ea = " + gpr(a) + " + " + gpr(b) + ";");
            line(e, ps0(d) + " = " + mem_r(e, "64", "ea") + ";");
            line(e, gpr(a) + " = ea;");
            return true;
        case 659:  // mfsrin
            line(e, gpr(d) + " = c.sr[(" + gpr(b) + " >> 28) & 15u];");
            return true;
        case 662:  // stwbrx
            line(e, "ea = " + address_ab + ";");
            line(e, "c.mem_write32(ea, bswap32(" + gpr(d) + "));");
            return true;
        case 663:  // stfsx
            line(e, "ea = " + address_ab + ";");
            line(e, "c.mem_write32(ea, double_to_single_bits(" + ps0(d) + "));");
            return true;
        case 695:  // stfsux
            line(e, "ea = " + gpr(a) + " + " + gpr(b) + ";");
            line(e, "c.mem_write32(ea, double_to_single_bits(" + ps0(d) + "));");
            line(e, gpr(a) + " = ea;");
            return true;
        case 727:  // stfdx
            line(e, "ea = " + address_ab + ";");
            line(e, "c.mem_write64(ea, " + ps0(d) + ");");
            return true;
        case 759:  // stfdux
            line(e, "ea = " + gpr(a) + " + " + gpr(b) + ";");
            line(e, "c.mem_write64(ea, " + ps0(d) + ");");
            line(e, gpr(a) + " = ea;");
            return true;
        case 790:  // lhbrx
            line(e, "ea = " + address_ab + ";");
            line(e, gpr(d) + " = bswap16(" + mem_r(e, "16", "ea") + ");");
            return true;
        case 792: simple("ppc::sraw(c, " + gpr(d) + ", " + gpr(b) + ")"); return true;
        case 824:
            simple("ppc::srawi(c, " + gpr(d) + ", " + std::to_string(field_b(instruction)) + "u)");
            return true;
        case 918:  // sthbrx
            line(e, "ea = " + address_ab + ";");
            line(e, "c.mem_write16(ea, bswap16(static_cast<u16>(" + gpr(d) + ")));");
            return true;
        case 922: simple("static_cast<u32>(static_cast<s32>(static_cast<s16>(" + gpr(d) + ")))"); return true;
        case 954: simple("static_cast<u32>(static_cast<s32>(static_cast<s8>(" + gpr(d) + ")))"); return true;
        case 983:  // stfiwx
            line(e, "ea = " + address_ab + ";");
            line(e, "c.mem_write32(ea, static_cast<u32>(" + ps0(d) + " & 0xffffffffull));");
            return true;
        case 1014:  // dcbz
            line(e, "ea = (" + address_ab + ") & ~31u;");
            line(e, "for (u32 i = 0; i < 32u; i += 4u) { c.mem_write32(ea + i, 0u); }");
            return true;
        case 310:  // eciwx
            line(e, "ea = " + address_ab + ";");
            line(e, gpr(d) + " = " + mem_r(e, "32", "ea") + ";");
            return true;
        case 438:  // ecowx
            line(e, "ea = " + address_ab + ";");
            line(e, "c.mem_write32(ea, " + gpr(d) + ");");
            return true;
        // String load/store (repair L0r0-03). Until then these four legal Gekko words were
        // fail()ed, which dropped the WHOLE containing function from kFunctions/kRanges and
        // turned every call to it into a production NotRecompiled stop.
        //
        // Manual PAGE 448 (lswi), 449 (lswx), 561 (stswi), 562 (stswx): n bytes move between
        // EA and GPR(first), GPR(first+1), ... (mod 32, wrapping r31 -> r0), left to right,
        // one 1-byte access per byte in EA order - the access sequence the reference
        // interpreter performs (cpu.cpp load_string / the stswi/stswx loops). EA is latched in
        // `ea` before any register is written, so an rA/rB inside the loaded range (an invalid
        // form) still addresses the original string. Rc (bit 31) is reserved and ignored, as
        // for lmw/stmw. XER, CR and every other register are unchanged ("Other registers
        // altered: None").
        case 597:    // lswi
        case 725: {  // stswi
            // n = NB, 32 when NB == 0: the byte count and the register sequence are static, so
            // the lowering is unrolled with constant register slots and constant offsets.
            const u32 count = b == 0u ? 32u : b;
            const bool load = extended == 597u;
            line(e, "ea = " + gpr_or_zero(a) + ";");
            u32 reg = d;
            for (u32 index = 0; index < count; ++index) {
                const u32 slot = index & 3u;
                const u32 shift = 24u - 8u * slot;
                const std::string at = index == 0u ? std::string("ea") : "ea + " + hexu(index);
                const std::string shift_text = shift == 0u ? std::string() : " >> " + std::to_string(shift) + "u";
                if (load) {
                    // A register is cleared when its first byte loads (GPR(r) <- 0), so the
                    // unfilled low-order bytes of the last register read as zero. The bytes
                    // accumulate in tmp and the register is written once they are complete:
                    // a 1-byte load cannot observe or change a GPR, so this equals the RTL.
                    std::string byte = "static_cast<u32>(" + mem_r(e, "8", at) + ")";
                    if (shift != 0u) {
                        byte += " << " + std::to_string(shift) + "u";
                    }
                    line(e, (slot == 0u ? "tmp = " : "tmp |= ") + byte + ";");
                    if (slot == 3u || index + 1u == count) {
                        line(e, gpr(reg) + " = tmp;");
                    }
                } else {
                    // A 1-byte store cannot change a GPR, so each source register is read
                    // once, before its first byte is stored.
                    if (slot == 0u) {
                        line(e, "tmp = " + gpr(reg) + ";");
                    }
                    line(e, "c.mem_write8(" + at + ", static_cast<u8>(tmp" + shift_text + "));");
                }
                if (slot == 3u) {
                    reg = (reg + 1u) & 31u;
                }
            }
            return true;
        }
        case 533:    // lswx
        case 661: {  // stswx
            // n = XER[25-31] (0..127) at execution time: a counted loop. Byte i belongs to
            // GPR((first + i/4) mod 32) at bit offset 8*(i mod 4). n == 0 moves nothing (lswx
            // leaves rD undefined, PAGE 449: it is left unchanged; stswx stores nothing, PAGE 562).
            line(e, "ea = " + address_ab + ";");
            line(e, "tmp = c.xer & 0x7Fu;");
            const std::string reg_text = "c.gpr[(" + std::to_string(d) + "u + (i >> 2u)) & 31u]";
            const std::string shift_text = "(24u - ((i & 3u) << 3u))";
            line(e, "for (u32 i = 0u; i < tmp; ++i) {");
            if (extended == 533u) {
                line(e, "    if ((i & 3u) == 0u) { " + reg_text + " = 0u; }");
                line(e, "    " + reg_text + " |= static_cast<u32>(" + mem_r(e, "8", "ea + i") + ") << " +
                            shift_text + ";");
            } else {
                line(e, "    c.mem_write8(ea + i, static_cast<u8>(" + reg_text + " >> " + shift_text + "));");
            }
            line(e, "}");
            return true;
        }
        default:
            fail(e, "group-31 extended " + std::to_string(extended));
            return false;
    }
}

bool emit_group59(EmitContext& e, u32 address, u32 instruction) {
    (void)address;
    const u32 extended = op_extended5(instruction);
    const u32 d = field_d(instruction);
    const u32 a = field_a(instruction);
    const u32 b = field_b(instruction);
    const u32 c_reg = field_c(instruction);
    // Single-precision arithmetic through the ppc::fps_* helpers (NaN
    // propagation, FPSCR[NI], one rounding of the multiply-add family); the
    // result goes to ps0 and ps1 (PAGE 83).
    const std::string ab = ps0(a) + ", " + ps0(b) + ")";
    const std::string acb = ps0(a) + ", " + ps0(c_reg) + ", " + ps0(b) + ")";
    std::string expression;
    switch (extended) {
        case 18: expression = "ppc::fps_div(c, " + ab; break;
        case 20: expression = "ppc::fps_sub(c, " + ab; break;
        case 21: expression = "ppc::fps_add(c, " + ab; break;
        case 22: expression = "ppc::fps_sqrt(c, " + ps0(b) + ")"; break;
        case 24: expression = "ppc::fps_res(c, " + ps0(b) + ")"; break;
        case 25: expression = "ppc::fps_mul(c, " + ps0(a) + ", " + ps0(c_reg) + ")"; break;
        case 28: expression = "ppc::fps_msub(c, " + acb; break;
        case 29: expression = "ppc::fps_madd(c, " + acb; break;
        case 30: expression = "ppc::fps_nmsub(c, " + acb; break;
        case 31: expression = "ppc::fps_nmadd(c, " + acb; break;
        default:
            fail(e, "group-59 extended " + std::to_string(extended));
            return false;
    }
    line(e, "tmp64 = " + expression + ";");
    line(e, ps0(d) + " = tmp64;");
    line(e, ps1(d) + " = tmp64;");
    emit_cr1(e, instruction);
    return true;
}

bool emit_group63(EmitContext& e, u32 address, u32 instruction) {
    (void)address;
    const u32 short_extended = op_extended5(instruction);
    const u32 d = field_d(instruction);
    const u32 a = field_a(instruction);
    const u32 b = field_b(instruction);
    const u32 c_reg = field_c(instruction);

    const bool is_a_form =
        short_extended == 18 || short_extended == 20 || short_extended == 21 ||
        short_extended == 22 || short_extended == 23 || short_extended == 25 ||
        short_extended == 26 || short_extended == 28 || short_extended == 29 ||
        short_extended == 30 || short_extended == 31;
    if (is_a_form) {
        // Double-precision arithmetic through the ppc::fp_* helpers: NaN
        // propagation, FPSCR[NI], and a fused multiply-add family (PAGE 403).
        const std::string ab = ps0(a) + ", " + ps0(b) + ")";
        const std::string acb = ps0(a) + ", " + ps0(c_reg) + ", " + ps0(b) + ")";
        switch (short_extended) {
            case 18: line(e, ps0(d) + " = ppc::fp_div(c, " + ab + ";"); break;
            case 20: line(e, ps0(d) + " = ppc::fp_sub(c, " + ab + ";"); break;
            case 21: line(e, ps0(d) + " = ppc::fp_add(c, " + ab + ";"); break;
            case 22: line(e, ps0(d) + " = ppc::fp_sqrt(c, " + ps0(b) + ");"); break;
            case 23:
                line(e, "fa = " + d0(a) + ";");
                line(e, ps0(d) + " = double_to_bits((fa >= 0.0 && !std::isnan(fa)) ? " + d0(c_reg) +
                            " : " + d0(b) + ");");
                break;
            case 25: line(e, ps0(d) + " = ppc::fp_mul(c, " + ps0(a) + ", " + ps0(c_reg) + ");"); break;
            case 26: line(e, ps0(d) + " = ppc::fp_rsqrte(c, " + ps0(b) + ");"); break;
            case 28: line(e, ps0(d) + " = ppc::fp_msub(c, " + acb + ";"); break;
            case 29: line(e, ps0(d) + " = ppc::fp_madd(c, " + acb + ";"); break;
            case 30: line(e, ps0(d) + " = ppc::fp_nmsub(c, " + acb + ";"); break;
            default: line(e, ps0(d) + " = ppc::fp_nmadd(c, " + acb + ";"); break;
        }
        emit_cr1(e, instruction);
        return true;
    }

    const u32 extended = op_extended(instruction);
    switch (extended) {
        case 0:
        case 32:  // fcmpu / fcmpo: only the ordered form signals VXVC on a NaN
            line(e, "ppc::fp_compare(c, " + std::to_string(field_crfd(instruction)) + "u, " +
                        ps0(a) + ", " + ps0(b) + ", " + (extended == 32 ? "true" : "false") + ");");
            return true;
        case 12:  // frsp
            line(e, ps0(d) + " = ppc::fps_round(c, " + ps0(b) + ");");
            emit_cr1(e, instruction);
            return true;
        case 14:
        case 15:  // fctiw / fctiwz
            line(e, ps0(d) + " = ppc::fctiw(c, " + ps0(b) + ", " +
                        (extended == 15 ? "true" : "false") + ");");
            emit_cr1(e, instruction);
            return true;
        case 38:  // mtfsb1
            line(e, "c.fpscr |= " + hexu(1u << (31u - d)) + ";");
            emit_cr1(e, instruction);
            return true;
        case 70:  // mtfsb0
            line(e, "c.fpscr &= " + hexu(~(1u << (31u - d))) + ";");
            emit_cr1(e, instruction);
            return true;
        case 40:  // fneg
            line(e, ps0(d) + " = " + ps0(b) + " ^ 0x8000000000000000ull;");
            emit_cr1(e, instruction);
            return true;
        case 64:  // mcrfs
            line(e, "ppc::set_cr_field(c, " + std::to_string(field_crfd(instruction)) +
                        "u, (c.fpscr >> " + std::to_string(28u - 4u * field_crfs(instruction)) +
                        "u) & 0xfu);");
            return true;
        case 72:  // fmr
            line(e, ps0(d) + " = " + ps0(b) + ";");
            emit_cr1(e, instruction);
            return true;
        case 134: {  // mtfsfi
            const u32 field = field_crfd(instruction);
            const u32 value = (instruction >> 12) & 0xfu;
            const u32 shift = 28u - 4u * field;
            line(e, "c.fpscr = (c.fpscr & " + hexu(~(0xfu << shift)) + ") | " +
                        hexu(value << shift) + ";");
            emit_cr1(e, instruction);
            return true;
        }
        case 136:  // fnabs
            line(e, ps0(d) + " = " + ps0(b) + " | 0x8000000000000000ull;");
            emit_cr1(e, instruction);
            return true;
        case 264:  // fabs
            line(e, ps0(d) + " = " + ps0(b) + " & 0x7fffffffffffffffull;");
            emit_cr1(e, instruction);
            return true;
        case 583:  // mffs
            line(e, ps0(d) + " = 0xfff8000000000000ull | c.fpscr;");
            emit_cr1(e, instruction);
            return true;
        case 711: {  // mtfsf
            const u32 fm = (instruction >> 17) & 0xffu;
            u32 mask = 0;
            for (u32 index = 0; index < 8; ++index) {
                if ((fm & (0x80u >> index)) != 0u) {
                    mask |= 0xfu << (28u - 4u * index);
                }
            }
            line(e, "tmp = static_cast<u32>(" + ps0(b) + " & 0xffffffffull);");
            line(e, "c.fpscr = (c.fpscr & " + hexu(~mask) + ") | (tmp & " + hexu(mask) + ");");
            emit_cr1(e, instruction);
            return true;
        }
        default:
            fail(e, "group-63 extended " + std::to_string(extended));
            return false;
    }
}

bool emit_group4(EmitContext& e, u32 address, u32 instruction) {
    (void)address;
    const u32 xo6 = (instruction >> 1) & 0x3fu;
    const u32 d = field_d(instruction);
    const u32 a = field_a(instruction);
    const u32 b = field_b(instruction);
    const u32 c_reg = field_c(instruction);

    if (xo6 == 6 || xo6 == 38 || xo6 == 7 || xo6 == 39) {  // psq_lx / psq_stx forms
        const bool store = (xo6 == 7 || xo6 == 39);
        const bool update = (xo6 == 38 || xo6 == 39);
        const bool w = ((instruction >> 10) & 1u) != 0u;
        const u32 gqr = (instruction >> 7) & 7u;
        line(e, "ea = " + gpr_or_zero(a) + " + " + gpr(b) + ";");
        if (store) {
            line(e, "ppc::psq_store(c, " + std::to_string(d) + "u, ea, " + (w ? "true" : "false") +
                        ", " + std::to_string(gqr) + "u);");
        } else {
            line(e, "ppc::psq_load(c, " + std::to_string(d) + "u, ea, " + (w ? "true" : "false") +
                        ", " + std::to_string(gqr) + "u);");
        }
        if (update && a != 0) {
            line(e, gpr(a) + " = ea;");
        }
        return true;
    }

    const u32 short_extended = op_extended5(instruction);
    // Both halves through the ppc::fps_* helpers (NaN propagation, FPSCR[NI],
    // one rounding of the multiply-add forms). Each half reads only operand
    // bits, and the ps1 result is computed before either store, so frD may
    // alias any operand.
    auto set_pair = [&](const std::string& first, const std::string& second) {
        line(e, "tmp64 = " + first + ";");
        line(e, "{ const u64 ps1_result = " + second + "; " + ps0(d) + " = tmp64; " + ps1(d) +
                    " = ps1_result; }");
        emit_cr1(e, instruction);
    };
    const auto binary = [&](const char* helper, const std::string& x, const std::string& y) {
        return std::string("ppc::") + helper + "(c, " + x + ", " + y + ")";
    };
    const auto fused = [&](const char* helper, const std::string& x, const std::string& z,
                           const std::string& y) {
        return std::string("ppc::") + helper + "(c, " + x + ", " + z + ", " + y + ")";
    };

    switch (short_extended) {
        // ps_sum0 / ps_sum1 (PAGE 530/531): only the sum is rounded to single;
        // frC's other half "is placed into" frD - a bit copy. The sum is staged
        // first, so frD aliasing frA/frB/frC cannot change either result.
        case 10:
            line(e, "tmp64 = " + binary("fps_add", ps0(a), ps1(b)) + ";");
            line(e, ps1(d) + " = " + ps1(c_reg) + ";");
            line(e, ps0(d) + " = tmp64;");
            emit_cr1(e, instruction);
            return true;
        case 11:
            line(e, "tmp64 = " + binary("fps_add", ps0(a), ps1(b)) + ";");
            line(e, ps0(d) + " = " + ps0(c_reg) + ";");
            line(e, ps1(d) + " = tmp64;");
            emit_cr1(e, instruction);
            return true;
        case 12:  // ps_muls0: both halves of frA times frC(ps0)
            set_pair(binary("fps_mul", ps0(a), ps0(c_reg)), binary("fps_mul", ps1(a), ps0(c_reg)));
            return true;
        case 13:  // ps_muls1
            set_pair(binary("fps_mul", ps0(a), ps1(c_reg)), binary("fps_mul", ps1(a), ps1(c_reg)));
            return true;
        case 14:  // ps_madds0
            set_pair(fused("fps_madd", ps0(a), ps0(c_reg), ps0(b)),
                     fused("fps_madd", ps1(a), ps0(c_reg), ps1(b)));
            return true;
        case 15:  // ps_madds1
            set_pair(fused("fps_madd", ps0(a), ps1(c_reg), ps0(b)),
                     fused("fps_madd", ps1(a), ps1(c_reg), ps1(b)));
            return true;
        case 18: set_pair(binary("fps_div", ps0(a), ps0(b)), binary("fps_div", ps1(a), ps1(b))); return true;
        case 20: set_pair(binary("fps_sub", ps0(a), ps0(b)), binary("fps_sub", ps1(a), ps1(b))); return true;
        case 21: set_pair(binary("fps_add", ps0(a), ps0(b)), binary("fps_add", ps1(a), ps1(b))); return true;
        case 23:
            // ps_sel (PAGE 528): frD is "set to the contents of" frC or frB, no
            // rounding. >= 0.0 is false for a NaN (selects frB) and true for -0.
            // ps0 is staged in tmp64; the ps1 line reads only ps1 halves.
            line(e, "tmp64 = " + d0(a) + " >= 0.0 ? " + ps0(c_reg) + " : " + ps0(b) + ";");
            line(e, ps1(d) + " = " + d1(a) + " >= 0.0 ? " + ps1(c_reg) + " : " + ps1(b) + ";");
            line(e, ps0(d) + " = tmp64;");
            emit_cr1(e, instruction);
            return true;
        case 24:  // ps_res
            set_pair("ppc::fps_res(c, " + ps0(b) + ")", "ppc::fps_res(c, " + ps1(b) + ")");
            return true;
        case 25: set_pair(binary("fps_mul", ps0(a), ps0(c_reg)), binary("fps_mul", ps1(a), ps1(c_reg))); return true;
        case 26:  // ps_rsqrte
            set_pair("ppc::fps_rsqrte(c, " + ps0(b) + ")", "ppc::fps_rsqrte(c, " + ps1(b) + ")");
            return true;
        case 28:
            set_pair(fused("fps_msub", ps0(a), ps0(c_reg), ps0(b)), fused("fps_msub", ps1(a), ps1(c_reg), ps1(b)));
            return true;
        case 29:
            set_pair(fused("fps_madd", ps0(a), ps0(c_reg), ps0(b)), fused("fps_madd", ps1(a), ps1(c_reg), ps1(b)));
            return true;
        case 30:
            set_pair(fused("fps_nmsub", ps0(a), ps0(c_reg), ps0(b)), fused("fps_nmsub", ps1(a), ps1(c_reg), ps1(b)));
            return true;
        case 31:
            set_pair(fused("fps_nmadd", ps0(a), ps0(c_reg), ps0(b)), fused("fps_nmadd", ps1(a), ps1(c_reg), ps1(b)));
            return true;
        default:
            break;
    }

    const u32 extended = op_extended(instruction);
    switch (extended) {
        case 0:
        case 32:
        case 64:
        case 96: {  // ps_cmpu0 / ps_cmpo0 / ps_cmpu1 / ps_cmpo1
            const bool high = extended >= 64;
            const bool ordered = extended == 32 || extended == 96;
            line(e, "ppc::fp_compare(c, " + std::to_string(field_crfd(instruction)) + "u, " +
                        (high ? ps1(a) : ps0(a)) + ", " + (high ? ps1(b) : ps0(b)) + ", " +
                        (ordered ? "true" : "false") + ");");
            break;
        }
        case 40:  // ps_neg
            line(e, ps0(d) + " = " + ps0(b) + " ^ 0x8000000000000000ull;");
            line(e, ps1(d) + " = " + ps1(b) + " ^ 0x8000000000000000ull;");
            break;
        case 72:  // ps_mr
            line(e, "tmp64 = " + ps0(b) + ";");
            line(e, ps1(d) + " = " + ps1(b) + ";");
            line(e, ps0(d) + " = tmp64;");
            break;
        case 136:  // ps_nabs
            line(e, ps0(d) + " = " + ps0(b) + " | 0x8000000000000000ull;");
            line(e, ps1(d) + " = " + ps1(b) + " | 0x8000000000000000ull;");
            break;
        case 264:  // ps_abs
            line(e, ps0(d) + " = " + ps0(b) + " & 0x7fffffffffffffffull;");
            line(e, ps1(d) + " = " + ps1(b) + " & 0x7fffffffffffffffull;");
            break;
        case 528:  // ps_merge00
            line(e, "tmp64 = " + ps0(a) + ";");
            line(e, ps1(d) + " = " + ps0(b) + ";");
            line(e, ps0(d) + " = tmp64;");
            break;
        case 560:  // ps_merge01
            line(e, "tmp64 = " + ps0(a) + ";");
            line(e, ps1(d) + " = " + ps1(b) + ";");
            line(e, ps0(d) + " = tmp64;");
            break;
        case 592:  // ps_merge10
            line(e, "tmp64 = " + ps1(a) + ";");
            line(e, ps1(d) + " = " + ps0(b) + ";");
            line(e, ps0(d) + " = tmp64;");
            break;
        case 624:  // ps_merge11
            line(e, "tmp64 = " + ps1(a) + ";");
            line(e, ps1(d) + " = " + ps1(b) + ";");
            line(e, ps0(d) + " = tmp64;");
            break;
        case 1014:  // dcbz_l
            line(e, "ea = (" + gpr_or_zero(a) + " + " + gpr(b) + ") & ~31u;");
            line(e, "for (u32 i = 0; i < 32u; i += 4u) { c.mem_write32(ea + i, 0u); }");
            break;
        default:
            fail(e, "group-4 extended " + std::to_string(extended));
            return false;
    }
    emit_cr1(e, instruction);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

namespace {

// Keep output identity stable: opening an unchanged source for writing would
// force its translation unit to compile again. Compare in text mode on both
// sides so Windows CRLF output compares against the generator's LF strings.
struct OutputStats {
    std::size_t written = 0;
    std::size_t unchanged = 0;
};

bool write_if_changed(const std::string& path, const std::string& text, OutputStats& stats) {
    std::ifstream before(path);
    bool same = before.is_open();
    std::size_t offset = 0;
    char block[65536];
    while (same && offset < text.size()) {
        const std::size_t count = std::min(sizeof(block), text.size() - offset);
        before.read(block, static_cast<std::streamsize>(count));
        same = before.gcount() == static_cast<std::streamsize>(count) &&
               std::memcmp(block, text.data() + offset, count) == 0;
        offset += count;
    }
    same = same && before.peek() == std::char_traits<char>::eof() && !before.bad();
    before.close();
    if (same) {
        ++stats.unchanged;
        return true;
    }
    std::ofstream after(path);
    after.write(text.data(), static_cast<std::streamsize>(text.size()));
    after.close();
    if (!after) {
        std::fprintf(stderr, "cannot write generated output %s\n", path.c_str());
        return false;
    }
    ++stats.written;
    return true;
}

// "0x" and one to eight hex digits, nothing else (the platform-site list).
bool parse_hex32(const std::string& text, u32& value) {
    if (text.size() < 3u || text.size() > 10u || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) {
        return false;
    }
    value = 0u;
    for (std::size_t index = 2; index < text.size(); ++index) {
        const char ch = text[index];
        u32 digit = 0u;
        if (ch >= '0' && ch <= '9') digit = static_cast<u32>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') digit = static_cast<u32>(ch - 'a') + 10u;
        else if (ch >= 'A' && ch <= 'F') digit = static_cast<u32>(ch - 'A') + 10u;
        else return false;
        value = (value << 4) | digit;
    }
    return true;
}

// A platform-site kind: the name the runtime switches on, [a-z0-9_]+.
bool platform_kind_valid(const std::string& kind) {
    if (kind.empty()) return false;
    for (const char ch : kind) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
    }
    return true;
}

struct Options {
    // A/B control for the D1 loop-extent merge. The merge changes record
    // boundaries, so the ONLY honest way to measure it is to produce both trees
    // from the same binary with the same arguments - reconstructing a
    // "baseline" from an older tree compares two different generators and has
    // already produced three wrong answers.
    bool loop_merge = true;
    std::string dol;
    std::string functions;
    std::string out;
    std::string only;
    std::size_t shards = 48;
    std::size_t limit = 0;
    std::string report;
    // Extra liveness roots, one address per line ('#' comments). Non-empty
    // enables unreachable-translation pruning (see the pass in main).
    std::string prune_roots;
    // Native owners, one function start per line ('#' comments): rows whose
    // original body is never emitted because a runtime handler always serves
    // them (see the owner handling in main). Several lists: one per
    // --native-owners (the lifted build adds its own).
    std::vector<std::string> native_owners;
    // Explicit recovered empty variadic leaves. Complete instruction templates,
    // entry population and overlaps are checked before generated source changes.
    std::string empty_variadic_leaves;
    // Recognized jump tables, one per line ('#' comments): "dispatch_site table
    // entries" (extract/jump-tables.txt). A table's words are reached from the
    // row holding its dispatch site instead of seeding liveness as data words
    // (see the pruning pass). Empty: every data word stays a root.
    std::string jump_tables;
    // Wait points, one per line ('#' comments): "back_edge_site loop_head
    // class" (extract/wait-points.txt). Every site must be a backward branch
    // to its head; the generator emits Cpu::idle_wait on that back edge.
    std::string wait_points;
    // Platform sites, one per line ('#' comments): "site word kind [note]"
    // (extract/platform-sites.txt). Every site must hold exactly its listed
    // lhz/sth (D-form, no update) in the DOL text and must be emitted; the
    // generator calls Cpu::platform_read16/platform_write16 there instead of
    // the bus.
    std::string platform_sites;
    // GX pipe sites, one per line ('#' comments): "site word family [note]"
    // (extract/gx-sites.txt). Every site must hold exactly its listed store in
    // the DOL text in a form its family accepts; a site a surviving row
    // translates must be emitted.
    std::string gx_sites;
    // Lowered call sites and retired roots (extract/lift-sites.txt;
    // docs/lift-20261006/LIFT_PROGRAMME.md 2.1): "site address word kind" - a
    // bl or devirtualised bctrl the generator replaces with its generated
    // lift_site function - and "retire address value" - a data word only
    // lowered code read, which then seeds no liveness. Every site must hold
    // exactly its listed word and be emitted; every retired word must hold
    // exactly its listed value. Repeatable: a build that lowers a further
    // subsystem adds its own list (docs/lift-20261006/R2_DRIVER.md 4.3); a site
    // or a retired word listed in two files is refused like one listed twice.
    std::vector<std::string> lift_sites;
    // Diagnostics, comma-separated hex addresses: after the pruning pass, why
    // each row holding one is live (the first reach events back to a seed), or
    // that it was pruned. Changes nothing that is emitted.
    std::string why_live;
    // Diagnostics: write the reach events of the pruning pass (all of them, not
    // only the first) as a graph: "seed KIND VIA ROW" lines (every seed address)
    // and "edge FROM VIA ROW" lines (one per row pair, its lowest address),
    // rows by start address. Changes nothing that is emitted.
    std::string live_graph;
    // R13 hot set: "ppcrecomp-hotset-v1", then one "0xADDR,weight" per line
    // ('#' comments): rows whose hot body inlines the accessor fast half.
    std::string hot_set;
    // R16 paired-single sites: "site gqr_index K" per line ('#' comments).
    std::string psq_sites;
    // GXD: GX pipe sites dead in this (lifted) build: "site word family [note]"
    // (the columns of gx-sites.txt); each must be listed in --gx-sites with the
    // same word and family.
    std::string gx_fail_sites;
};

bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto value = [&]() -> std::string {
            return index + 1 < argc ? std::string(argv[++index]) : std::string();
        };
        if (argument == "--dol") options.dol = value();
        else if (argument == "--functions") options.functions = value();
        else if (argument == "--no-gap-sweep") g_gap_sweep = false;
        else if (argument == "--no-wide-entries") g_wide_entries = false;
        else if (argument == "--annotate-lines") g_annotate_lines = true;
        else if (argument == "--out") options.out = value();
        else if (argument == "--only") options.only = value();
        else if (argument == "--shards") options.shards = std::strtoul(value().c_str(), nullptr, 10);
        else if (argument == "--limit") options.limit = std::strtoul(value().c_str(), nullptr, 10);
        else if (argument == "--report") options.report = value();
        else if (argument == "--no-loop-merge") options.loop_merge = false;
        else if (argument == "--prune-roots") options.prune_roots = value();
        else if (argument == "--native-owners") options.native_owners.push_back(value());
        else if (argument == "--empty-variadic-leaves") options.empty_variadic_leaves = value();
        else if (argument == "--jump-tables") options.jump_tables = value();
        else if (argument == "--wait-points") options.wait_points = value();
        else if (argument == "--platform-sites") options.platform_sites = value();
        else if (argument == "--gx-sites") options.gx_sites = value();
        else if (argument == "--lift-sites") options.lift_sites.push_back(value());
        else if (argument == "--why-live") options.why_live = value();
        else if (argument == "--live-graph") options.live_graph = value();
        else if (argument == "--hot-set") options.hot_set = value();
        else if (argument == "--psq-sites") options.psq_sites = value();
        else if (argument == "--gx-fail-sites") options.gx_fail_sites = value();
        else {
            std::fprintf(stderr, "unknown option %s\n", argument.c_str());
            return false;
        }
    }
    return !options.dol.empty() && !options.functions.empty() && !options.out.empty();
}

struct Candidate {
    FunctionRecord record;
    FunctionAnalysis analysis;
    // A native owner: a runtime handler serves every call, so only a thunk to
    // it is emitted (no body, no dispatch range). Its interior entries are
    // only the declared owner entries (addresses native code uses as
    // guest-visible entries, e.g. a completion return), each a thunk too.
    bool native_owner = false;
    std::set<u32> owner_entries;
    // Every guest address the dispatcher, or another translated function, may
    // enter this function at. Always contains record.address; computed once
    // globally in main so the body, the header and the table cannot disagree.
    std::set<u32> entries;
    // CPU-11: containment collapse. Non-zero when every address this row
    // translates is also translated by the row starting at this address, in
    // which case the body is emitted as a one-line forward instead of a
    // duplicate translation. Every exported symbol, table row and dispatch
    // entry is unchanged; only the duplicate instruction stream disappears.
    u32 absorbed_by = 0;
    // The entries of every row whose absorbed_by is this row. Their thunks
    // forward into this translation, so each is an address control arrives at
    // here - most often a jump-table case the gap sweep recovered as a row of
    // its own - and each must be a hot label. Otherwise a jump to one runs as
    // cold snippets and re-enters the hot body through a nested call, and a
    // bctr in this function cannot reach it with a goto.
    std::set<u32> absorbed_entries;
};

bool empty_variadic_kind(const DolImage& image, const Candidate& candidate, EmptyVariadicKind& kind) {
    // Exact leaf implementations: no loads, calls, or use of the saved
    // argument/va_list bytes. Residual scratch-register values are preserved. Floating arguments are copied only to the
    // private frame. The frame and all ordinary register state are restored.
    const std::vector<u32> preserve = {
        0x9421FF90u,0x40860024u,0xD8210028u,0xD8410030u,0xD8610038u,0xD8810040u,
        0xD8A10048u,0xD8C10050u,0xD8E10058u,0xD9010060u,0x90610008u,0x9081000Cu,
        0x90A10010u,0x90C10014u,0x90E10018u,0x9101001Cu,0x91210020u,0x91410024u,
        0x38210070u,0x4E800020u};
    const auto zero = [&] {
        auto result = preserve;
        result.insert(result.begin() + 11, 0x38600000u);
        return result;
    }();
    const std::vector<u32> va_list = {
        0x9421FF80u,0x40860024u,0xD8210028u,0xD8410030u,0xD8610038u,0xD8810040u,
        0xD8A10048u,0xD8C10050u,0xD8E10058u,0xD9010060u,0x39610088u,0x38010008u,
        0x3D800100u,0x90610008u,0x9081000Cu,0x90A10010u,0x90C10014u,0x90E10018u,
        0x9101001Cu,0x91210020u,0x91410024u,0x91810068u,0x9161006Cu,0x90010070u,
        0x38210080u,0x4E800020u};
    for (const auto& pattern : {std::make_pair(EmptyVariadicKind::Preserve, &preserve),
                                std::make_pair(EmptyVariadicKind::Zero, &zero),
                                std::make_pair(EmptyVariadicKind::VaList, &va_list)}) {
        if (candidate.record.size != pattern.second->size() * 4u) continue;
        bool matches = true;
        for (std::size_t i = 0; i < pattern.second->size(); ++i)
            if (image.read32(candidate.record.address + static_cast<u32>(4u*i)) != (*pattern.second)[i]) matches = false;
        if (matches) { kind = pattern.first; return true; }
    }
    return false;
}


struct ShardLayout {
    std::vector<std::size_t> placement;
    std::string text;
};

bool plan_shards(const std::string& path, const std::vector<Candidate>& candidates,
                 std::size_t shards, ShardLayout& result) {
    std::map<u32, std::size_t> previous;
    std::ifstream input(path);
    if (input.is_open()) {
        std::string line;
        std::getline(input, line);
        const std::string prefix = "ppcrecomp-shards-v1,";
        if (line.compare(0, prefix.size(), prefix) != 0) {
            std::fprintf(stderr, "invalid shard layout header: %s\n", path.c_str());
            return false;
        }
        std::istringstream header(line.substr(prefix.size()));
        std::size_t prior_shards = 0;
        if (!(header >> prior_shards) || prior_shards == 0u) return false;
        header >> std::ws;
        if (!header.eof()) return false;
        // An explicit shard-count change defines a new partition. Ordinary
        // insertion/removal never reassigns a surviving guest region.
        if (prior_shards == shards) {
            while (std::getline(input, line)) {
                std::istringstream row(line);
                u64 address = 0, shard = 0;
                char comma = 0;
                if (!(row >> std::hex >> address >> comma >> std::dec >> shard) ||
                    comma != ',' || address > 0xffffffffull || (address & 3u) != 0u ||
                    shard >= shards) {
                    std::fprintf(stderr, "invalid shard layout row: %s\n", path.c_str());
                    return false;
                }
                row >> std::ws;
                if (!row.eof() ||
                    !previous.emplace(static_cast<u32>(address), static_cast<std::size_t>(shard)).second) {
                    std::fprintf(stderr, "duplicate/invalid shard identity: %s\n", path.c_str());
                    return false;
                }
            }
            if (input.bad()) return false;
        }
    }
    result.placement.resize(candidates.size(), shards);
    std::vector<u64> weights(shards, 0u);
    const auto weight = [](const Candidate& candidate) -> u64 {
        // A forwarded row carries entry wrappers, not its old reachable body.
        // These are deterministic compile-work weights, not a hotness claim.
        return 1u + candidate.entries.size() +
               (candidate.absorbed_by == 0u ? candidate.analysis.reachable.size() : 0u);
    };
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const auto found = previous.find(candidates[index].record.address);
        if (found != previous.end()) {
            result.placement[index] = found->second;
            weights[found->second] += weight(candidates[index]);
        }
    }
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (result.placement[index] != shards) continue;
        // Bootstrap the historical placement for byte-identical adoption.
        // Later new regions use the least-loaded existing shard.
        const std::size_t shard = previous.empty() ? index % shards :
            static_cast<std::size_t>(std::min_element(weights.begin(), weights.end()) - weights.begin());
        result.placement[index] = shard;
        weights[shard] += weight(candidates[index]);
    }
    std::ostringstream output;
    output << "ppcrecomp-shards-v1," << shards << "\n";
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        output << hex32(candidates[index].record.address) << "," << result.placement[index] << "\n";
    }
    result.text = output.str();
    return true;
}


// R13: retarget the eight guest-memory accessor calls in one hot-body snippet
// to their force-inlined twins. A call is matched by its exact emitted spelling
// "c.mem_<read|write><width>(", so nothing else (helpers, cold halves) can be
// renamed. Returns the number of call sites changed.
std::size_t retarget_hot_accessors(std::string& code) {
    static const char* const kCalls[] = {
        "c.mem_read8(",  "c.mem_read16(",  "c.mem_read32(",  "c.mem_read64(",
        "c.mem_write8(", "c.mem_write16(", "c.mem_write32(", "c.mem_write64(",
    };
    std::size_t changed = 0;
    for (const char* call : kCalls) {
        const std::string from(call);
        std::string to = from;
        to.insert(to.size() - 1u, "_hot");
        std::size_t at = 0;
        while ((at = code.find(from, at)) != std::string::npos) {
            code.replace(at, from.size(), to);
            at += to.size();
            ++changed;
        }
    }
    return changed;
}

std::string emit_function(const DolImage& image, const Candidate& candidate,
                          const std::unordered_map<u32, u32>& recompiled,
                          std::string& failure, bool validate_only = false) {
    EmitContext context;
    context.image = &image;
    context.recompiled = &recompiled;
    context.function_start = candidate.record.address;
    context.function_end = candidate.record.address + candidate.record.size;
    context.labels = candidate.analysis.labels;

    // Execution arrives inside a function, not only at its entry: a callee that
    // hands control back, and a guest exception handler returning through SRR0,
    // both resume at the instruction after a call. Unless that address is an
    // entry point the dispatcher has no native code to resume into, and the rest
    // of the function - including any loop it is sitting in - runs interpreted.
    // The OS idle thread spins in exactly such a loop, which is where 3.47 of
    // 3.6 billion interpreted instructions were going.
    std::vector<u32> entries(candidate.entries.begin(), candidate.entries.end());
    if (entries.empty()) {
        entries.push_back(candidate.record.address);
    }
    // Entry points are labels: control can arrive there from outside the body.
    for (const u32 address : entries) {
        context.labels.insert(address);
    }
    // Absorbed rows enter this translation at their own entries (see
    // Candidate::absorbed_entries). Every one lies in this row's reachable
    // set - absorption requires reachable-set inclusion - so each label is
    // emitted.
    for (const u32 address : candidate.absorbed_entries) {
        context.labels.insert(address);
    }

    const std::string body_symbol = fn_symbol(candidate.record.address) + "_body";
    if (candidate.absorbed_by != 0u) {
        // CPU-11: containment collapse. Every address this row translates is
        // also translated by its absorber, so re-emitting the instructions
        // would be a byte-for-byte duplicate. Forward into the absorber at the
        // same guest address instead. Do not build either body: they would be
        // discarded, and building them twice (hot+cold) for 3,601 absorbed
        // rows is pure generator cost.
        std::string text = annotate('a', candidate.record.address);
        text += "static void " + body_symbol + "(Cpu& c, u32 entry) { " +
                fn_symbol(candidate.absorbed_by) + "_at(c, entry); }\n";
        text += "void " + fn_symbol(candidate.record.address) +
                "_at(Cpu& c, u32 entry) { " + body_symbol + "(c, entry); }\n";
        for (const u32 address : entries) {
            const std::string name = address == candidate.record.address
                                         ? fn_symbol(candidate.record.address)
                                         : entry_symbol(candidate.record.address, address);
            if (address == candidate.record.address) {
                text += entry_thunk(name, body_symbol, address);
            } else {
                text += "void " + name + "(Cpu& c) { " + body_symbol + "(c, " +
                        hexu(address) + "); }\n";
            }
        }
        return text;
    }
    // Shared IR + optimized HOT layout + minimal COLD trampoline.
    //
    // CPU-17 emitted the instruction stream TWICE: fn_X_body (labels only at
    // CFG points) and fn_X_body_at (a label on every address). That doubled
    // generated C++ and ~54 MiB of cold .text that is entered ~22k times per
    // 2e9 instructions. CPU-14 dropped the extra labels and lost sc/rfi.
    //
    // Now: one walk produces an IR of per-instruction snippets. HOT is the
    // single optimized layout (fusion, FP hoist, normal basic blocks). COLD
    // is a trampoline: a hot label calls the hot body at that label; a
    // mid-block address runs only the IR suffix to the next hot label and
    // joins hot there. Interior entry stays native; the second full body
    // is gone.
    const std::string cold_symbol =
        fn_symbol(candidate.record.address) + "_body_at";

    struct IrInsn {
        u32 address = 0;
        bool hot_label = false;
        bool falls_through = true;
        std::string hot_code;
        std::string cold_code;
    };

    std::size_t indirect_sites = 0;
    for (const u32 address : candidate.analysis.reachable) {
        const u32 word = image.read32(address);
        if (op_primary(word) == 19u && op_extended(word) == 528u && (word & 1u) == 0u)
            ++indirect_sites;
    }

    const auto make_ctx = [&](bool fusion) {
        EmitContext ctx;
        ctx.image = &image;
        ctx.recompiled = &recompiled;
        ctx.function_start = candidate.record.address;
        ctx.function_end = candidate.record.address + candidate.record.size;
        ctx.labels = context.labels;
        ctx.fusion_allowed = fusion;
        ctx.share_indirect_dispatch = indirect_sites > 1u;
        return ctx;
    };

    std::vector<IrInsn> ir;
    if (!validate_only) ir.reserve(candidate.analysis.reachable.size());
    EmitContext hot_cap = make_ctx(true);
    EmitContext cold_cap = make_ctx(false);
    u32 prev = 0;
    bool is_first = true;
    for (u32 address : candidate.analysis.reachable) {
        if (!is_first && address != prev + 4u) {
            hot_cap.fp_guard_live = false;
            hot_cap.hid2_proven = 0u;
            hot_cap.cr_fuse_field = -1;
            hot_cap.cr_fuse_armed_field = -1;
        }
        IrInsn insn;
        insn.address = address;
        insn.hot_label = context.labels.count(address) != 0;
        const u32 instruction = image.read32(address);
        insn.falls_through = flow_falls_through(classify(instruction, address).flow);
        const std::size_t hot_at = hot_cap.body.size();
        if (!emit_instruction(hot_cap, address, instruction)) {
            failure = hot_cap.failure;
            return std::string();
        }
        if (!validate_only) insn.hot_code = hot_cap.body.substr(hot_at);
        hot_cap.body.resize(hot_at);
        if (!insn.hot_label) {
            const std::size_t cold_at = cold_cap.body.size();
            if (!emit_instruction(cold_cap, address, instruction)) {
                failure = cold_cap.failure;
                return std::string();
            }
            if (!validate_only) insn.cold_code = cold_cap.body.substr(cold_at);
            cold_cap.body.resize(cold_at);
        }
        if (!validate_only) ir.push_back(std::move(insn));
        prev = address;
        is_first = false;
    }
    // Preflight still exercises both instruction lowerings and their state
    // transitions. Assembling and retaining layouts that are immediately
    // discarded adds no validation; only the final pass publishes code.
    if (validate_only) return "validated";
    EmitContext tail_ctx = make_ctx(true);
    emit_jump(tail_ctx, tail_ctx.function_end, tail_ctx.function_end,
              FallbackReasonTag::EndOfFunction);
    const std::string tail_code = tail_ctx.body;

    if (g_hot_set.count(candidate.record.address) != 0u) {
        std::size_t changed = 0;
        for (IrInsn& insn : ir) changed += retarget_hot_accessors(insn.hot_code);
        ++g_hot_set_rows;
        g_hot_set_sites += changed;
    }

    std::string hot_body;
    std::size_t hot_annotation_bytes = 0;  // --annotate-lines only; kept out of the report
    const auto hot_annotate = [&](char kind, u32 address) {
        const std::string directive = annotate(kind, address);
        hot_annotation_bytes += directive.size();
        hot_body += directive;
    };
    for (const IrInsn& insn : ir) {
        if (insn.hot_label) {
            hot_body += label(insn.address) + ":\n";
        }
        hot_annotate('i', insn.address);
        hot_body += insn.hot_code;
    }
    hot_annotate('t', candidate.record.address + candidate.record.size);
    hot_body += tail_code;
    emit_shared_indirect_dispatch(hot_cap);
    hot_body += hot_cap.body;

    g_ir_insns += ir.size();
    for (const IrInsn& insn : ir) {
        if (insn.hot_label) ++g_hot_label_insns;
        else ++g_cold_mid_insns;
    }
    g_hot_src_bytes += hot_body.size() - hot_annotation_bytes;

    std::string text;
    // HOT: labels only where control can actually arrive, so basic blocks span
    // whole runs and MSVC may keep guest state in host registers. The switch
    // accepts every hot label, not only declared thunks, so a cold trampoline
    // can join at a branch target without regenerating the rest of the function.
    text += "static void " + cold_symbol + "(Cpu& c, u32 entry);\n";
    {
        std::vector<u32> hot_entries;
        for (const IrInsn& insn : ir) {
            if (insn.hot_label && insn.address != candidate.record.address) {
                hot_entries.push_back(insn.address);
            }
        }
        text += "static void " + body_symbol + "(Cpu& c, u32 entry) {\n";
        if (hot_cap.indirect_dispatch_used) text += "    u32 gekko_indirect_site = 0;\n";
        text += annotate('g', candidate.record.address);
        text += "    u32 ea = 0; u32 tmp = 0; u32 cra = 0; u32 crb = 0; bool cnd = false;\n";
        text += "    u64 tmp64 = 0; double fa = 0.0; double fb = 0.0;\n";
        text += "    (void)ea; (void)tmp; (void)cra; (void)crb; (void)cnd;\n";
        text += "    (void)tmp64; (void)fa; (void)fb;\n";
        if (hot_entries.empty()) {
            text += "    (void)entry;\n";
            text += hot_body;
            text += "}\n";
        } else {
            text += "    if (entry != " + hexu(candidate.record.address) +
                    ") goto gekko_hot_dispatch;\n";
            text += hot_body;
            text += annotate('d', candidate.record.address);
            text += "gekko_hot_dispatch:\n";
            text += "    switch (entry) {\n";
            for (const u32 address : hot_entries) {
                text += "        case " + hexu(address) + ": goto " + label(address) + ";\n";
            }
            text += "        default: break;\n";
            text += "    }\n";
            text += "    " + cold_symbol + "(c, entry);\n";
            text += "    return;\n";
            text += "}\n";
        }
    }
    text += "static void " + cold_symbol + "(Cpu& c, u32 entry) {\n";
    if (cold_cap.indirect_dispatch_used) text += "    u32 gekko_indirect_site = 0;\n";
    text += annotate('e', candidate.record.address);
    text += "    u32 ea = 0; u32 tmp = 0; u32 cra = 0; u32 crb = 0; bool cnd = false;\n";
    text += "    u64 tmp64 = 0; double fa = 0.0; double fb = 0.0;\n";
    text += "    (void)ea; (void)tmp; (void)cra; (void)crb; (void)cnd;\n";
    text += "    (void)tmp64; (void)fa; (void)fb;\n";
    // COLD trampoline. Interior entry is still native: every reachable address
    // has a dispatch case. A hot label joins the optimized body; a mid-block
    // address runs only the IR suffix to the next hot label (same snippets the
    // hot walk produced, without fusion/FP-hoist) and then joins hot. There is
    // no second full instruction stream.
    const u32 dispatch_base = candidate.record.address;
    const auto cold_label = [](u32 address) {
        return std::string("C_") + hex32(address).substr(2);
    };
    std::vector<const IrInsn*> interior;
    u32 last_interior = dispatch_base;
    for (const IrInsn& insn : ir) {
        if (insn.address == dispatch_base) continue;
        interior.push_back(&insn);
        last_interior = insn.address;
    }
    if (interior.empty()) {
        text += "    (void)entry;\n";
        text += "    " + body_symbol + "(c, " + hexu(dispatch_base) + ");\n";
        text += "}\n";
    } else {
        const std::size_t slots =
            static_cast<std::size_t>((last_interior - dispatch_base) / 4u) + 1u;

        text += "    if (entry == " + hexu(dispatch_base) + ") { " + body_symbol +
                "(c, entry); return; }\n";
        text += "    {\n";
        text += "        const unsigned gekko_slot = (entry - " + hexu(dispatch_base) +
                ") >> 2;\n";
        text += "        if ((entry & 3u) == 0u && gekko_slot < " + std::to_string(slots) +
                "u) {\n";
        text += "            switch (gekko_slot) {\n";
        for (const IrInsn* insn : interior) {
            text += "                case " +
                    std::to_string((insn->address - dispatch_base) / 4u) + "u: goto " +
                    (insn->hot_label ? label(insn->address) : cold_label(insn->address)) +
                    ";\n";
        }
        text += "                default: break;\n";
        text += "            }\n";
        text += "        }\n";
        text += "    }\n";
        text += "    " + body_symbol + "(c, " + hexu(dispatch_base) + ");\n";
        text += "    return;\n";
        // Backward branches in cold snippets target the function start with
        // `goto L_<start>`. That label lives in the hot body; provide a
        // trampoline here so the goto stays in this function.
        text += label(dispatch_base) + ":\n";
        text += "    " + body_symbol + "(c, " + hexu(dispatch_base) + ");\n";
        text += "    return;\n";

        // Join chains: one fallthrough run per mid-block span, then join hot.
        // Hot-label cases only forward; they share no extra instruction copy.
        for (std::size_t i = 0; i < ir.size(); ++i) {
            const IrInsn& insn = ir[i];
            if (insn.address == dispatch_base) continue;
            if (insn.hot_label) {
                text += annotate('k', insn.address);
                text += label(insn.address) + ":\n";
                text += "    " + body_symbol + "(c, " + hexu(insn.address) + ");\n";
                text += "    return;\n";
                continue;
            }
            text += cold_label(insn.address) + ":\n";
            text += annotate('c', insn.address);
            text += insn.cold_code;
            g_cold_src_bytes += insn.cold_code.size();
            if (!insn.falls_through) {
                continue;
            }
            const bool last = (i + 1u) == ir.size();
            const IrInsn* next = last ? nullptr : &ir[i + 1u];
            const bool run_continues =
                next != nullptr && next->address == insn.address + 4u && !next->hot_label;
            if (run_continues) {
                continue;
            }
            if (next != nullptr && next->hot_label) {
                text += annotate('j', next->address);
                text += "    " + body_symbol + "(c, " + hexu(next->address) + ");\n";
                text += "    return;\n";
            } else {
                text += annotate('t', candidate.record.address + candidate.record.size);
                text += tail_code;
            }
        }
        emit_shared_indirect_dispatch(cold_cap);
        text += cold_cap.body;
        g_cold_src_bytes += cold_cap.body.size();
        text += "}\n";
    }
    // Dispatcher interior entry. Ordinary calls use fn_X / the declared thunks
    // (hot). fn_X_at is the find_range path for an address that is not a
    // declared entry point.
    text += annotate('x', candidate.record.address);
    text += "void " + fn_symbol(candidate.record.address) + "_at(Cpu& c, u32 entry) { " +
            cold_symbol + "(c, entry); }\n";
    // The entry point proper, plus one thunk per resume point. Each thunk is a
    // table entry, so the dispatcher can put control back into native code at
    // any address the guest can arrive at.
    for (const u32 address : entries) {
        const std::string name = address == candidate.record.address
                                     ? fn_symbol(candidate.record.address)
                                     : entry_symbol(candidate.record.address, address);
        text += annotate(address == candidate.record.address ? 'x' : 'n', address);
        if (address == candidate.record.address) {
            text += entry_thunk(name, body_symbol, address);
        } else {
            text += "void " + name + "(Cpu& c) { " + body_symbol + "(c, " + hexu(address) +
                    "); }\n";
        }
    }
    return text;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        std::fprintf(stderr,
                     "usage: ppcrecomp --dol <main.dol> --functions <functions.csv> --out <dir>\n"
                     "                 [--only <addresses.txt>] [--shards N] [--limit N]\n"
                     "                 [--report <file>] [--annotate-lines]\n"
                     "                 [--no-loop-merge]  A/B control: emit guest loops that\n"
                     "                                    span records as recursive CALLs again\n");
        return 2;
    }

    DolImage image;
    if (!load_dol(options.dol, image)) {
        std::fprintf(stderr, "cannot read DOL %s\n", options.dol.c_str());
        return 1;
    }

    std::vector<FunctionRecord> functions;
    if (!load_functions(options.functions, functions)) {
        std::fprintf(stderr, "cannot read function table %s\n", options.functions.c_str());
        return 1;
    }

    // R13: the hot set. Refused unless it is exactly the format hotset.py
    // writes, each address once; that every address starts an accepted row is
    // checked once the rows exist (below). A stale or hand-edited list must
    // fail the build, not silently inline elsewhere.
    g_hot_set.clear();
    if (!options.hot_set.empty()) {
        std::ifstream file(options.hot_set);
        if (!file) {
            std::fprintf(stderr, "cannot read hot set %s\n", options.hot_set.c_str());
            return 1;
        }
        std::string line_text;
        if (!std::getline(file, line_text) || line_text != "ppcrecomp-hotset-v1") {
            std::fprintf(stderr, "hot set %s: missing ppcrecomp-hotset-v1 header\n",
                         options.hot_set.c_str());
            return 1;
        }
        std::size_t line_number = 1;
        while (std::getline(file, line_text)) {
            ++line_number;
            if (line_text.empty() || line_text[0] == '#') continue;
            char* end = nullptr;
            const unsigned long address = std::strtoul(line_text.c_str(), &end, 16);
            if (line_text.compare(0, 2, "0x") != 0 || end == nullptr || *end != ',' ||
                end[1] == '\0' || address > 0xFFFFFFFFul ||
                !g_hot_set.insert(static_cast<u32>(address)).second) {
                std::fprintf(stderr,
                             "hot set %s:%zu: not \"0xADDR,weight\" with a new address: %s\n",
                             options.hot_set.c_str(), line_number, line_text.c_str());
                return 1;
            }
        }
    }

    // R16: the paired-single sites. Each must hold a D-form psq_l / psq_lu /
    // psq_st / psq_stu whose GQR index is the listed one; K may be any value
    // (the emitted guard decides at run time).
    g_psq_sites.clear();
    if (!options.psq_sites.empty()) {
        std::ifstream file(options.psq_sites);
        if (!file) {
            std::fprintf(stderr, "cannot read psq sites %s\n", options.psq_sites.c_str());
            return 1;
        }
        std::string line_text;
        std::size_t line_number = 0;
        while (std::getline(file, line_text)) {
            ++line_number;
            if (line_text.empty() || line_text[0] == '#') continue;
            std::istringstream fields(line_text);
            std::string site_text, gqr_text, value_text;
            fields >> site_text >> gqr_text >> value_text;
            const u32 site = static_cast<u32>(std::strtoul(site_text.c_str(), nullptr, 0));
            const u32 gqr = static_cast<u32>(std::strtoul(gqr_text.c_str(), nullptr, 0));
            const u32 value = static_cast<u32>(std::strtoul(value_text.c_str(), nullptr, 0));
            const u32 word = image.text_at(site) ? image.read32(site) : 0u;
            const u32 primary = word >> 26;
            const bool psq = primary == 56 || primary == 57 || primary == 60 || primary == 61;
            if (value_text.empty() || !psq || ((word >> 12) & 7u) != gqr || gqr > 7u ||
                !g_psq_sites.emplace(site, PsqSite{gqr, value}).second) {
                std::fprintf(stderr, "psq sites %s:%zu: not a new D-form psq site with GQR index %s: %s\n",
                             options.psq_sites.c_str(), line_number, gqr_text.c_str(), line_text.c_str());
                return 1;
            }
        }
    }

    std::set<u32> filter;
    if (!options.only.empty()) {
        std::ifstream file(options.only);
        if (!file) {
            std::fprintf(stderr, "cannot read %s\n", options.only.c_str());
            return 1;
        }
        std::string line_text;
        while (std::getline(file, line_text)) {
            if (line_text.empty() || line_text[0] == '#') {
                continue;
            }
            filter.insert(static_cast<u32>(std::strtoul(line_text.c_str(), nullptr, 0)));
        }
    }

    // The size column of the function table is the size of the *recovered* body,
    // which is often short: a tail block reached only through a jump the analysis
    // did not follow is left out. Control flow that leaves the declared extent
    // then has to be handed back at run time, which is both slow and a false
    // signal in the fallback audit. A function actually owns every byte up to
    // the next function entry, so that is the bound the reachability walk uses.
    // Nothing beyond the real body is ever emitted, because the walk only
    // follows control flow that the entry point can reach.
    std::sort(functions.begin(), functions.end(),
              [](const FunctionRecord& a, const FunctionRecord& b) {
                  return a.address < b.address;
              });
    for (std::size_t index = 0; index < functions.size(); ++index) {
        const u32 next = (index + 1 < functions.size()) ? functions[index + 1].address
                                                        : functions[index].address +
                                                              functions[index].size;
        if (next > functions[index].address) {
            functions[index].size = std::max(functions[index].size, next - functions[index].address);
        }
    }

    // The analysis merges functions when nothing in the image reveals the join:
    // 0x80393634 and 0x80393730 are two functions in one record, the second
    // reachable only through a pointer. Walking from the declared entry leaves
    // the second one unreached, and every call to it would have to be handed
    // back at run time. Any run of unreached instructions that begins right
    // after a terminator is a function the analysis missed, so it is recovered
    // here and translated in its own right.
    {
        std::set<u32> entries;
        for (const FunctionRecord& record : functions) {
            entries.insert(record.address);
        }
        std::vector<FunctionRecord> recovered;
        const auto add_entry = [&](u32 address) {
            if (address == 0 || !image.executable_at(address) || entries.count(address) != 0) {
                return;
            }
            FunctionRecord split;
            split.address = address;
            split.size = 4;  // widened to the next entry by the pass below
            split.name = "fn_" + hex32(address).substr(2);
            recovered.push_back(split);
            entries.insert(address);
        };
        for (const FunctionRecord& record : functions) {
            if (!image.executable_at(record.address)) {
                continue;
            }
            const FunctionAnalysis walk = analyse(image, record.address, record.size);
            if (!walk.ok) {
                continue;
            }

            // Every address recompiled code can hand control back at is made an
            // entry point, so the dispatcher can put execution straight back
            // into native code instead of interpreting from the middle of a
            // function until the next call.
            //
            // This matters far more than it looks. The OS idle thread spins on
            //
            //   bl OSEnableInterrupts ; lwz r0,flag ; cmplwi ; beq -8
            //
            // and the spin itself calls nothing. When mtmsr inside
            // OSEnableInterrupts handed back, execution resumed mid-function and
            // the whole idle loop ran interpreted until the flag changed -
            // 3.47 billion of 3.6 billion interpreted instructions in a six
            // billion instruction boot.
            for (u32 address : walk.reachable) {
                const u32 instruction = image.read32(address);
                const u32 primary = op_primary(instruction);
                if (primary == 31 && op_extended(instruction) == 146) {  // mtmsr
                    add_entry(address + 4u);
                } else if (primary == 17) {  // sc
                    add_entry(address + 4u);
                } else if (primary == 19 && op_extended(instruction) == 50) {  // rfi
                    add_entry(address + 4u);
                }
            }
            const u32 end = record.address + record.size;
            bool previous_reached = true;
            for (u32 address = record.address; address < end; address += 4u) {
                const bool reached = walk.reachable.count(address) != 0;
                if (!reached && previous_reached && address != record.address &&
                    entries.count(address) == 0) {
                    const u32 previous = image.read32(address - 4u);
                    const Decoded decoded = classify(previous, address - 4u);
                    const bool terminator = decoded.flow == Flow::Return ||
                                            decoded.flow == Flow::Branch ||
                                            decoded.flow == Flow::Indirect ||
                                            decoded.flow == Flow::Terminal;
                    if (terminator) {
                        add_entry(address);
                    }
                }
                previous_reached = reached;
            }
        }
        if (!recovered.empty()) {
            std::fprintf(stdout, "  recovered entries    : %zu\n", recovered.size());
            functions.insert(functions.end(), recovered.begin(), recovered.end());
            std::sort(functions.begin(), functions.end(),
                      [](const FunctionRecord& a, const FunctionRecord& b) {
                          return a.address < b.address;
                      });
            for (std::size_t index = 0; index < functions.size(); ++index) {
                const u32 next = (index + 1 < functions.size())
                                     ? functions[index + 1].address
                                     : functions[index].address + functions[index].size;
                // Widen, never narrow. A branch inside a function must stay
                // inside it: cutting an extent at a newly recovered entry would
                // turn intra-function branches into cross-function transfers.
                // Overlapping extents just translate the shared tail twice.
                if (next > functions[index].address) {
                    functions[index].size =
                        std::max(functions[index].size, next - functions[index].address);
                }
            }
        }
    }

    std::map<std::string, u32> rejections;
    std::map<u32, std::string> rejected_functions;
    std::vector<Candidate> candidates;
    std::size_t skipped_filtered = 0;
    g_sweep_roots_dropped = 0;

    for (const FunctionRecord& record : functions) {
        if (!filter.empty() && filter.count(record.address) == 0) {
            ++skipped_filtered;
            continue;
        }
        if (!image.executable_at(record.address)) {
            ++rejections["not in an executable section"];
            rejected_functions[record.address] = "not in an executable section";
            continue;
        }
        FunctionAnalysis analysis = analyse(image, record.address, record.size);
        if (!analysis.ok) {
            ++rejections[analysis.failure];
            rejected_functions[record.address] = analysis.failure;
            continue;
        }
        Candidate candidate;
        candidate.record = record;
        candidate.analysis = std::move(analysis);
        candidates.push_back(std::move(candidate));
    }

    std::fprintf(stderr, "gap sweep: %llu speculative roots dropped\n",
                 static_cast<unsigned long long>(g_sweep_roots_dropped));

    // First pass: find out which functions can be emitted at all. The set of
    // recompiled addresses affects only whether a call is direct or dispatched,
    // so a single extra pass reaches a fixed point.
    std::unordered_map<u32, u32> recompiled;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        std::string failure;
        const std::string text = emit_function(image, candidates[index], recompiled, failure, true);
        if (text.empty()) {
            ++rejections[failure];
            rejected_functions[candidates[index].record.address] = failure;
            candidates[index].record.size = 0;  // mark rejected
        }
    }
    std::vector<Candidate> accepted;
    for (Candidate& candidate : candidates) {
        if (candidate.record.size != 0) {
            accepted.push_back(std::move(candidate));
        }
    }
    if (options.limit != 0 && accepted.size() > options.limit) {
        accepted.resize(options.limit);
    }
    // ---- entry points -------------------------------------------------------
    //
    // Control does not only arrive at a function's declared start. It arrives at
    // the instruction after every call (a callee that hands back, or a guest
    // handler returning through SRR0), and - because the function table has
    // overlapping records - at any address another function branches or calls
    // directly. Every such address that some accepted function actually
    // translates is made a dispatch entry, so control is put back into native
    // code there instead of stranding the rest of that function, and any loop it
    // is sitting in, in the interpreter.
    std::sort(accepted.begin(), accepted.end(), [](const Candidate& a, const Candidate& b) {
        return a.record.address < b.record.address;
    });
    // ---- D1: loop-closing extent merge -----------------------------------
    //
    // A guest loop has O(1) architected state, so its translation must run in
    // O(1) HOST state. Until this pass it did not.
    //
    // A record is one C++ function and `goto` is function-scoped, so emit_jump
    // can only close a loop with a goto when the back edge stays inside the
    // record (the `target >= function_start && target < function_end &&
    // labels.count(target)` gate). functions.csv starts a new record after every
    // bl/bctrl, so a guest loop that contains a call has its back edge on the
    // far side of that gate - and the only construct left is a CALL to the
    // other record's entry symbol. The callee never returns while the loop is
    // running, so the caller's frame stays parked: ONE HOST FRAME PER GUEST
    // ITERATION, 48-224 bytes, measured to STATUS_STACK_OVERFLOW at ~340,000
    // iterations of the real fn_803D946C cycle (F1c finding D1).
    //
    // A `needs_dispatch()` poll was added at that transfer by an earlier audit
    // (branch-sys-2). It is a COMPENSATION, not a fix: it unwinds the chain
    // only when an interrupt, budget expiry or stop is already pending, so with
    // MSR[EE]=0 - every OSDisableInterrupts window - nothing unwinds and the
    // growth is unbounded again.
    //
    // The root cause is that the loop is not EXPRESSIBLE, so fix that: extend
    // the head record's extent to cover the record holding the back edge. The
    // analysis then labels the loop head inside one function, emit_jump takes
    // the goto arm, and the loop runs in O(1) host state with no call at all.
    //
    // Detection uses `external_loop_targets`: an out-of-extent branch target
    // that is BACKWARD relative to its own branch site. It is a separate set on
    // purpose - `loop_heads` and `external_targets` are filled by the two arms
    // of one if/else on whether the target is in-extent, so a target lands in
    // exactly one of them and their intersection is always empty. (That error
    // cost a regeneration: the pass found 0 records to merge.)
    //
    // A backward branch is still not automatically a loop: `b` reaches +-32 MB
    // and most out-of-extent backward transfers are tail calls to a shared
    // routine, which keep one frame once and return. The cycle test below - A
    // can arrive at B, AND B branches back into A, both judged on the ORIGINAL
    // extents - is what separates the two. Judging it after widening merged
    // 1,064 records and 3.5 MB, collapsing half the program into giant rows.
    //
    // Measured on the shipped table before this pass: 19 such loops, every one
    // a 2-record pair, largest merged extent 1,888 B (472 instructions), median
    // 212 B. 13 of 19 are perfectly adjacent so nothing is pulled in; the rest
    // absorb 36-72 B of inter-record padding, or already overlap.
    //
    // The merged row is then absorbed by the containment collapse below, which
    // already hands every absorbed row's entry points to its absorber - the
    // property that keeps `_at`/`_e` thunk dispatch and find_range() correct.
    if (options.loop_merge) {
        std::size_t merged = 0;
        std::size_t merged_bytes = 0;
        // Iterate to a fixed point. One sweep extends a head record by at most
        // one back edge, but a loop can be closed by several and a three-record
        // chain needs the first merge before the second becomes visible.
        // MEASURED: a single sweep fixed 14 of the 19 known loops outright and
        // only reduced three of the rest. Bounded so a pathological table
        // cannot spin.
        constexpr int kMaxSweeps = 8;
        for (int sweep = 0; sweep < kMaxSweeps; ++sweep) {
        const std::size_t before = merged;
        std::vector<std::size_t> order(accepted.size());
        for (std::size_t i = 0; i < accepted.size(); ++i) order[i] = i;
        for (const std::size_t bi : order) {
            Candidate& b = accepted[bi];
            if (b.absorbed_by != 0u) continue;
            for (const u32 target : b.analysis.external_loop_targets) {
                if (target >= b.record.address) continue;  // must precede this record
                // The head record: the accepted row that owns the loop head and
                // can reach this record, i.e. the one the back edge closes on.
                std::size_t ai = accepted.size();
                for (std::size_t j = 0; j < accepted.size(); ++j) {
                    const Candidate& a = accepted[j];
                    if (a.absorbed_by != 0u) continue;
                    if (target < a.record.address) continue;
                    if (target >= a.record.address + a.record.size) continue;
                    if (a.analysis.reachable.count(target) == 0) continue;
                    ai = j;
                    break;
                }
                if (ai == accepted.size()) continue;
                Candidate& a = accepted[ai];
                const u32 a_end = a.record.address + a.record.size;
                const u32 b_end = b.record.address + b.record.size;
                if (b_end <= a_end) continue;  // already covered
                // THE CYCLE TEST, and it must be made on the ORIGINAL extents.
                //
                // A backward branch is not a loop. PowerPC `b` reaches +-32 MB,
                // so most out-of-extent backward transfers are tail calls to a
                // shared routine: they keep one frame once and return. Only a
                // CYCLE - A reaches B, and B branches back into A - recurses
                // per iteration.
                //
                // Testing reachability after widening is worthless (the widened
                // extent contains B by construction, so it is nearly a
                // tautology, and it merged 1,064 records / 3.5 MB on the first
                // attempt, collapsing half the program into giant rows). A must
                // be able to arrive at B while it is still its own record:
                // either it branches there, or it runs off its end into it.
                const bool a_branches_to_b =
                    a.analysis.external_targets.count(b.record.address) != 0;
                const bool a_falls_into_b =
                    a_end == b.record.address && a.analysis.reachable.count(a_end - 4u) != 0;
                // OVERLAPPING ROWS. The function table gives every row in an
                // overlapping cluster a size running to the same end address,
                // so B's entry frequently lies INSIDE A rather than after it.
                // A reaches B by simply running through its own code, and a
                // branch to that address is an in-extent label rather than an
                // external target - so neither test above sees it.
                //
                // Missing this left fn_803A3254's four back edges into
                // fn_803A2E08 unmerged through every sweep: B starts inside A
                // and ends 20 bytes past it, so it is neither "after" A nor
                // contained by it. It is also the shape of the two negative-gap
                // pairs (scc 6 at -704 B, scc 14 at -52 B).
                const bool a_contains_b_entry =
                    b.record.address >= a.record.address && b.record.address < a_end &&
                    a.analysis.reachable.count(b.record.address) != 0;
                if (!a_branches_to_b && !a_falls_into_b && !a_contains_b_entry) continue;
                // Re-analyse at the new extent. Extending the size alone would
                // be inert: `reachable` and `labels` were computed against the
                // old end, so emit_jump would still fail the in-record gate and
                // still emit the call. `analyse` is pure, so this is simply the
                // same walk over a longer extent.
                FunctionAnalysis widened = analyse(image, a.record.address, b_end - a.record.address);
                if (!widened.ok) continue;            // refuse rather than emit something worse
                if (widened.labels.count(target) == 0) continue;  // goto still not expressible
                if (widened.reachable.count(b.record.address) == 0) continue;  // head cannot reach it
                a.record.size = b_end - a.record.address;
                a.analysis = std::move(widened);
                merged_bytes += b_end - a_end;
                ++merged;
                break;  // one extension per row; a second back edge re-tests next sweep
            }
        }
        std::fprintf(stderr, "D1 loop-extent merge: sweep %d extended %zu records\n",
                     sweep, merged - before);
        if (merged == before) break;  // fixed point
        if (sweep + 1 == kMaxSweeps) {
            // NOT converged. Say so loudly rather than silently emitting a
            // program that still recurses: the bound exists to stop a runaway
            // table, not to cap a real workload.
            std::fprintf(stderr,
                         "D1 loop-extent merge: WARNING - hit the %d-sweep bound without "
                         "reaching a fixed point; some loop back edges remain cross-record\n",
                         kMaxSweeps);
        }
        }
        std::fprintf(stderr,
                     "D1 loop-extent merge: %zu records extended by %zu bytes so a "
                     "cross-record loop back edge becomes an in-record goto\n",
                     merged, merged_bytes);
    }

    std::set<u32> function_starts;
    std::set<u32> direct_targets;
    for (const Candidate& candidate : accepted) {
        function_starts.insert(candidate.record.address);
        direct_targets.insert(candidate.analysis.direct_calls.begin(),
                              candidate.analysis.direct_calls.end());
        direct_targets.insert(candidate.analysis.external_targets.begin(),
                              candidate.analysis.external_targets.end());
    }
    // An address must resolve to exactly one translation, so where records
    // overlap the lowest-addressed function wins and the choice is stable.
    std::set<u32> claimed(function_starts);
    std::size_t interior_entries = 0;
    std::size_t loop_entries = 0;
    std::size_t recovered_entry_points = 0;
    for (Candidate& candidate : accepted) {
        candidate.entries.insert(candidate.record.address);
        for (const u32 address : candidate.analysis.resume_points) {
            if (candidate.analysis.reachable.count(address) == 0) continue;
            if (!claimed.insert(address).second) continue;
            candidate.entries.insert(address);
        }
        for (const u32 address : candidate.analysis.loop_heads) {
            if (candidate.analysis.reachable.count(address) == 0) continue;
            if (!claimed.insert(address).second) continue;
            candidate.entries.insert(address);
            ++loop_entries;
        }
        for (const u32 address : candidate.analysis.recovered_entries) {
            if (candidate.analysis.reachable.count(address) == 0) continue;
            if (!claimed.insert(address).second) continue;
            candidate.entries.insert(address);
            ++recovered_entry_points;
        }
        const u32 end = candidate.record.address + candidate.record.size;
        for (auto it = direct_targets.lower_bound(candidate.record.address);
             it != direct_targets.end() && *it < end; ++it) {
            const u32 address = *it;
            if (candidate.analysis.reachable.count(address) == 0) continue;
            if (!claimed.insert(address).second) continue;
            candidate.entries.insert(address);
            ++interior_entries;
        }
    }
    std::fprintf(stderr,
                 "entry points: %zu functions, %zu loop heads, %zu recovered routines, "
                 "%zu interior branch/call targets\n",
                 accepted.size(), loop_entries, recovered_entry_points, interior_entries);
    for (const Candidate& candidate : accepted) {
        for (const u32 address : candidate.entries) {
            recompiled[address] = candidate.record.address;
        }
    }

    // ---- CPU-11: containment collapse ------------------------------------
    //
    // 3,601 of the 39,420 rows translate a STRICT SUBSET of another row's
    // instructions, because the function table gives every row in an overlapping
    // cluster a size running to the same end address. That is 740,158 duplicate
    // instruction copies, 37.8% of everything emitted, up to 87 copies of one
    // address.
    //
    // A row is absorbed only when every address it translates is also translated
    // by the absorber - reachable-set inclusion, not merely overlapping extents.
    // That is exactly the property the forward depends on: the absorber's
    // interior-entry dispatch must have a case for every address the absorbed
    // row could be entered at.
    //
    // kRanges already discards these rows (the disjoint clip below drops any
    // range wholly inside an earlier one), so find_range() already dispatches
    // their addresses to the absorber. This only stops the unreachable duplicate
    // body from being emitted.
    //
    // accepted is sorted by address, and the absorber is always at a strictly
    // lower address, so a backward scan finds it. The prefix maximum of the end
    // address rejects the 90% of rows that cannot be contained without any set
    // comparison at all.
    //
    // A native owner (--native-owners) is never an absorber: its original body
    // is not emitted, so there is no translation to forward into. A row inside
    // an owner's extent - e.g. the resume point after an mtmsr in a function
    // the runtime now owns - stays a row of its own, and the liveness pass
    // below prunes it unless something can still reach it.
    std::set<u32> owner_rows;
    for (const std::string& owners_list : options.native_owners) {
        std::ifstream file(owners_list);
        std::string line_text;
        while (std::getline(file, line_text)) {
            if (line_text.empty() || line_text[0] == '#') continue;
            owner_rows.insert(static_cast<u32>(std::strtoul(line_text.c_str(), nullptr, 0)));
        }
    }
    {
        std::vector<u32> prefix_max_end(accepted.size(), 0u);
        u32 running = 0u;
        for (std::size_t i = 0; i < accepted.size(); ++i) {
            running = std::max(running, accepted[i].record.address + accepted[i].record.size);
            prefix_max_end[i] = running;
        }
        std::size_t absorbed = 0;
        std::size_t absorbed_instructions = 0;
        for (std::size_t i = 1; i < accepted.size(); ++i) {
            Candidate& b = accepted[i];
            const u32 b_end = b.record.address + b.record.size;
            if (prefix_max_end[i - 1] < b_end) continue;  // nothing can contain it
            // Take the LOWEST-addressed container, so a whole cluster collapses
            // onto one translation rather than into a forwarding chain.
            for (std::size_t j = 0; j < i; ++j) {
                const Candidate& a = accepted[j];
                if (a.absorbed_by != 0u) continue;  // never forward to a forward
                if (owner_rows.count(a.record.address) != 0u) continue;  // nor into an owner
                if (a.record.address + a.record.size < b_end) continue;
                if (!std::includes(a.analysis.reachable.begin(), a.analysis.reachable.end(),
                                   b.analysis.reachable.begin(), b.analysis.reachable.end())) {
                    continue;
                }
                b.absorbed_by = a.record.address;
                ++absorbed;
                absorbed_instructions += b.analysis.reachable.size();
                break;
            }
        }
        std::fprintf(stderr,
                     "containment collapse: %zu of %zu rows absorbed, "
                     "%zu duplicate instructions not emitted\n",
                     absorbed, accepted.size(), absorbed_instructions);
        // Hand every absorbed row's entries to its absorber, which must label
        // them (Candidate::absorbed_entries).
        std::unordered_map<u32, std::size_t> index_of;
        index_of.reserve(accepted.size());
        for (std::size_t i = 0; i < accepted.size(); ++i) {
            index_of.emplace(accepted[i].record.address, i);
        }
        for (std::size_t i = 0; i < accepted.size(); ++i) {
            if (accepted[i].absorbed_by == 0u) continue;
            Candidate& absorber = accepted[index_of.at(accepted[i].absorbed_by)];
            absorber.absorbed_entries.insert(accepted[i].entries.begin(),
                                             accepted[i].entries.end());
        }
    }

    // ---- unreachable-translation pruning (--prune-roots) ------------------------
    //
    // A row nothing can reach is never executed, so its translation is not
    // emitted. Liveness is decided here, after entry points and the containment
    // collapse are final, so every surviving row's labels, entries and text are
    // exactly what they would have been. Roots: the DOL entry, every aligned
    // word of every non-executable section that lands in code (except the words
    // of a verified jump table, below), and the roots file. Edges, over a live row's REACHABLE
    // instructions: direct branch/call targets; every word-aligned code address
    // its lis/oris high halves and addi/addic/ori low halves can form, each pair
    // forming only what it computes (an addi adds, an ori ors; all
    // combinations, no data flow); its extent end when a reachable instruction
    // falls into it; every row sharing one of its instructions; the words of
    // every verified jump table whose dispatch site it holds; and the census
    // roots it constructs. An address no row reaches falls back to every row
    // whose extent holds it.
    //
    // A jump table is verified when every one of its words is a code address and
    // no data word points into it; its words are then reached from every row
    // that can form an address inside it (its dispatcher among them), and an
    // unverified table's words stay roots. A census
    // root ("census" line of the roots file) is reached through the rows that
    // construct it - those holding its recorded construction sites and those
    // whose hi/lo halves form it - and is an unconditional root only when
    // nothing forms it.
    if (!options.wait_points.empty()) {
        std::ifstream file(options.wait_points);
        if (!file) {
            std::fprintf(stderr, "cannot read %s\n", options.wait_points.c_str());
            return 1;
        }
        std::string line_text;
        while (std::getline(file, line_text)) {
            if (line_text.empty() || line_text[0] == '#') continue;
            std::istringstream fields(line_text);
            std::string site_text, head_text, kind;
            fields >> site_text >> head_text >> kind;
            const u32 site = static_cast<u32>(std::strtoul(site_text.c_str(), nullptr, 0));
            const u32 head = static_cast<u32>(std::strtoul(head_text.c_str(), nullptr, 0));
            // A stale list must not put a wait anywhere else: the site must be
            // a non-linking direct branch back to the listed head.
            const bool executable = image.executable_at(site);
            const Decoded decoded = executable ? classify(image.read32(site), site) : Decoded{};
            if (!executable || head > site ||
                (decoded.flow != Flow::Branch && decoded.flow != Flow::CondBranch) ||
                decoded.target != head || op_primary(image.read32(site)) == 19u) {
                std::fprintf(stderr, "wait point %s is not a backward branch to %s (stale %s?)\n",
                             hex32(site).c_str(), hex32(head).c_str(), options.wait_points.c_str());
                return 1;
            }
            g_wait_points[site] = head;
            if (kind == "progress") {
                g_poll_points.insert(site);
            } else if (kind != "poll" && kind != "clock" && kind != "spin" && kind != "halt") {
                std::fprintf(stderr, "wait point %s has an unknown class '%s'\n", hex32(site).c_str(),
                             kind.c_str());
                return 1;
            }
        }
    }
    if (!options.platform_sites.empty()) {
        std::ifstream file(options.platform_sites);
        if (!file) {
            std::fprintf(stderr, "cannot read %s\n", options.platform_sites.c_str());
            return 1;
        }
        const char* list = options.platform_sites.c_str();
        std::string line_text;
        std::size_t line_number = 0;
        while (std::getline(file, line_text)) {
            ++line_number;
            std::istringstream fields(line_text);
            std::string site_text, word_text, kind;
            if (!(fields >> site_text) || site_text[0] == '#') continue;  // blank or comment
            u32 site = 0;
            u32 word = 0;
            if (!(fields >> word_text >> kind) || !parse_hex32(site_text, site) ||
                !parse_hex32(word_text, word)) {
                std::fprintf(stderr, "%s:%zu: expected \"<site hex> <instruction word hex> <kind> [note]\"\n",
                             list, line_number);
                return 1;
            }
            // A stale list must not lower anything else: the site is an
            // instruction of the DOL text holding exactly the listed word, and
            // that word is a D-form lhz or sth (no update form, no index form)
            // or an li (addi rD,0,imm).
            if ((site & 3u) != 0u || !image.text_at(site)) {
                std::fprintf(stderr, "platform site %s is not an aligned instruction of the DOL text (%s:%zu)\n",
                             hex32(site).c_str(), list, line_number);
                return 1;
            }
            if (image.read32(site) != word) {
                std::fprintf(stderr, "platform site %s: the DOL word is %s, the list says %s (stale %s?)\n",
                             hex32(site).c_str(), hex32(image.read32(site)).c_str(), hex32(word).c_str(), list);
                return 1;
            }
            const bool li = op_primary(word) == 14u && field_a(word) == 0u;
            if (op_primary(word) != 40u && op_primary(word) != 44u && !li) {
                std::fprintf(stderr,
                             "platform site %s holds %s (primary opcode %u), not lhz, sth or li: only the D-form "
                             "without update and addi rD,0 are lowered (%s:%zu)\n",
                             hex32(site).c_str(), hex32(word).c_str(), op_primary(word), list, line_number);
                return 1;
            }
            if (!platform_kind_valid(kind)) {
                std::fprintf(stderr, "platform site %s has an invalid kind '%s', expected [a-z0-9_]+ (%s:%zu)\n",
                             hex32(site).c_str(), kind.c_str(), list, line_number);
                return 1;
            }
            if (kind == kPlatformKindDead && op_primary(word) != 44u) {
                std::fprintf(stderr,
                             "platform site %s is listed dead but holds %s: only a store (sth) can be dead - a "
                             "read's value has a consumer (%s:%zu)\n",
                             hex32(site).c_str(), hex32(word).c_str(), list, line_number);
                return 1;
            }
            if (!g_platform_sites.emplace(site, PlatformSite{word, kind}).second) {
                std::fprintf(stderr, "platform site %s is listed twice (%s:%zu)\n", hex32(site).c_str(), list,
                             line_number);
                return 1;
            }
        }
    }
    if (!options.gx_sites.empty()) {
        std::ifstream file(options.gx_sites);
        if (!file) {
            std::fprintf(stderr, "cannot read %s\n", options.gx_sites.c_str());
            return 1;
        }
        const char* list = options.gx_sites.c_str();
        std::string line_text;
        std::size_t line_number = 0;
        while (std::getline(file, line_text)) {
            ++line_number;
            std::istringstream fields(line_text);
            std::string site_text, word_text, family_text;
            if (!(fields >> site_text) || site_text[0] == '#') continue;  // blank or comment
            u32 site = 0;
            u32 word = 0;
            GxFamily family = GxFamily::Dead;
            if (!(fields >> word_text >> family_text) || !parse_hex32(site_text, site) ||
                !parse_hex32(word_text, word)) {
                std::fprintf(stderr, "%s:%zu: expected \"<site hex> <instruction word hex> <family> [note]\"\n",
                             list, line_number);
                return 1;
            }
            if ((site & 3u) != 0u || !image.text_at(site)) {
                std::fprintf(stderr, "GX pipe site %s is not an aligned instruction of the DOL text (%s:%zu)\n",
                             hex32(site).c_str(), list, line_number);
                return 1;
            }
            if (image.read32(site) != word) {
                std::fprintf(stderr, "GX pipe site %s: the DOL word is %s, the list says %s (stale %s?)\n",
                             hex32(site).c_str(), hex32(image.read32(site)).c_str(), hex32(word).c_str(), list);
                return 1;
            }
            if (!gx_family_from_name(family_text, family)) {
                std::fprintf(stderr, "GX pipe site %s has an unknown family '%s' (%s:%zu)\n", hex32(site).c_str(),
                             family_text.c_str(), list, line_number);
                return 1;
            }
            if (!gx_family_accepts(family, op_primary(word))) {
                std::fprintf(stderr,
                             "GX pipe site %s holds %s (primary opcode %u), a form family '%s' is not lowered "
                             "from (%s:%zu)\n",
                             hex32(site).c_str(), hex32(word).c_str(), op_primary(word), family_text.c_str(), list,
                             line_number);
                return 1;
            }
            if (g_platform_sites.count(site) != 0u || !g_gx_sites.emplace(site, GxSite{word, family}).second) {
                std::fprintf(stderr, "GX pipe site %s is listed twice (%s:%zu)\n", hex32(site).c_str(), list,
                             line_number);
                return 1;
            }
        }
    }
    // GXD: vertex / primitive_opcode / primitive_count sites become fail-closed;
    // any other family in the list (draw_done, copy_trigger: HW wants a
    // counter run first) is reported and left as it is.
    g_gx_fail_sites.clear();
    if (!options.gx_fail_sites.empty()) {
        std::ifstream file(options.gx_fail_sites);
        if (!file) {
            std::fprintf(stderr, "cannot read %s\n", options.gx_fail_sites.c_str());
            return 1;
        }
        std::string line_text;
        std::size_t line_number = 0, deferred = 0;
        while (std::getline(file, line_text)) {
            ++line_number;
            std::istringstream fields(line_text);
            std::string site_text, word_text, family_text;
            if (!(fields >> site_text) || site_text[0] == '#') continue;
            u32 site = 0, word = 0;
            GxFamily family = GxFamily::Dead;
            if (!(fields >> word_text >> family_text) || !parse_hex32(site_text, site) ||
                !parse_hex32(word_text, word) || !gx_family_from_name(family_text, family)) {
                std::fprintf(stderr, "%s:%zu: expected \"<site> <word> <family> [note]\"\n",
                             options.gx_fail_sites.c_str(), line_number);
                return 1;
            }
            if (family != GxFamily::Vertex && family != GxFamily::PrimitiveOpcode &&
                family != GxFamily::PrimitiveCount) {
                ++deferred;
                continue;
            }
            const auto listed = g_gx_sites.find(site);
            if (listed == g_gx_sites.end() || listed->second.word != word || listed->second.family != family ||
                !g_gx_fail_sites.insert(site).second) {
                std::fprintf(stderr, "GX fail site %s is not a new %s site of %s with word %s (%s:%zu)\n",
                             hex32(site).c_str(), family_text.c_str(), options.gx_sites.c_str(),
                             hex32(word).c_str(), options.gx_fail_sites.c_str(), line_number);
                return 1;
            }
        }
        std::fprintf(stderr, "gx fail sites: %zu listed fail-closed, %zu other-family lines left as they are\n",
                     g_gx_fail_sites.size(), deferred);
    }
    for (const std::string& lift_list : options.lift_sites) {
        std::ifstream file(lift_list);
        if (!file) {
            std::fprintf(stderr, "cannot read %s\n", lift_list.c_str());
            return 1;
        }
        const char* list = lift_list.c_str();
        std::string line_text;
        std::size_t line_number = 0;
        while (std::getline(file, line_text)) {
            ++line_number;
            const auto hash = line_text.find('#');
            if (hash != std::string::npos) line_text.erase(hash);
            std::istringstream fields(line_text);
            std::string verb;
            if (!(fields >> verb)) continue;  // blank or comment
            if (verb == "site") {
                std::string site_text, word_text, kind_text;
                u32 site = 0;
                u32 word = 0;
                LiftKind kind = LiftKind::Nothing;
                if (!(fields >> site_text >> word_text >> kind_text) || !parse_hex32(site_text, site) ||
                    !parse_hex32(word_text, word)) {
                    std::fprintf(stderr, "%s:%zu: expected \"site <address hex> <instruction word hex> <kind>\"\n",
                                 list, line_number);
                    return 1;
                }
                if (!lift_kind_from_name(kind_text, kind)) {
                    std::fprintf(stderr, "lowered call site %s has an unknown kind '%s' (%s:%zu)\n",
                                 hex32(site).c_str(), kind_text.c_str(), list, line_number);
                    return 1;
                }
                if ((site & 3u) != 0u || !image.text_at(site)) {
                    std::fprintf(stderr, "lowered call site %s is not an aligned instruction of the DOL text (%s:%zu)\n",
                                 hex32(site).c_str(), list, line_number);
                    return 1;
                }
                if (image.read32(site) != word) {
                    std::fprintf(stderr, "lowered call site %s: the DOL word is %s, the list says %s (stale %s?)\n",
                                 hex32(site).c_str(), hex32(image.read32(site)).c_str(), hex32(word).c_str(), list);
                    return 1;
                }
                LiftSite listed{word, kind, {}, 0u, 0u, true};
                if (kind == LiftKind::Call) {
                    // call <function> <argument count> [void]
                    std::string argc_text, tail;
                    if (!(fields >> listed.function >> argc_text)) {
                        std::fprintf(stderr, "%s:%zu: expected \"site <address> <word> call <function> <argc> [void]\"\n",
                                     list, line_number);
                        return 1;
                    }
                    bool identifier = !listed.function.empty() &&
                                      !std::isdigit(static_cast<unsigned char>(listed.function[0]));
                    for (const char ch : listed.function) {
                        identifier = identifier && (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_');
                    }
                    char* argc_end = nullptr;
                    const unsigned long argument_count = std::strtoul(argc_text.c_str(), &argc_end, 10);
                    if (!identifier || argc_end == argc_text.c_str() || *argc_end != '\0' || argument_count > 8u) {
                        std::fprintf(stderr, "%s:%zu: call needs a C++ identifier and 0..8 arguments\n", list,
                                     line_number);
                        return 1;
                    }
                    listed.argc = static_cast<u32>(argument_count);
                    while (fields >> tail) {
                        if (tail == "void") {
                            listed.keep_result = false;
                        } else if (tail == "fpr") {
                            std::string count_text;
                            char* count_end = nullptr;
                            if (!(fields >> count_text) ||
                                (listed.fpr_count = static_cast<u32>(std::strtoul(count_text.c_str(), &count_end, 10)),
                                 count_end == count_text.c_str() || *count_end != '\0' || listed.fpr_count > 8u)) {
                                std::fprintf(stderr, "%s:%zu: fpr needs a count of 0..8\n", list, line_number);
                                return 1;
                            }
                        } else {
                            std::fprintf(stderr, "%s:%zu: unexpected '%s' after the argument count\n", list,
                                         line_number, tail.c_str());
                            return 1;
                        }
                    }
                    // One function, one arity: the generated declarations must agree with every site.
                    for (const auto& other : g_lift_sites) {
                        if (other.second.kind == LiftKind::Call && other.second.function == listed.function &&
                            (other.second.argc != listed.argc || other.second.fpr_count != listed.fpr_count)) {
                            std::fprintf(stderr, "%s:%zu: scaffold::%s is listed with %u and %u arguments\n", list,
                                         line_number, listed.function.c_str(), other.second.argc, listed.argc);
                            return 1;
                        }
                    }
                }
                if (kind == LiftKind::TypeQuery) {
                    std::string query_text, target_text, extra;
                    if (!(fields >> query_text >> target_text) || !parse_hex32(query_text, listed.query) ||
                        listed.query > 0xFFFFu || !parse_hex32(target_text, listed.target_descriptor) ||
                        (listed.target_descriptor & 3u) != 0u ||
                        (fields >> extra) || classify(word, site).target != 0x803A1AFCu) {
                        std::fprintf(stderr, "%s:%zu: type_query needs a 16-bit query ordinal and target descriptor in hex, at a call of 803A1AFC\n",
                                     list, line_number);
                        return 1;
                    }
                }
                // A devirtualised interface call lowers an unconditional bctrl; the named kinds a direct bl; the
                // generic call either.
                const bool is_bl = op_primary(word) == 18u && (word & 3u) == 1u;
                const bool form = kind == LiftKind::Call ? (word == kBctrlWord || is_bl)
                                  : lift_kind_is_bctrl(kind) ? word == kBctrlWord
                                                             : is_bl;
                if (!form) {
                    std::fprintf(stderr, "lowered call site %s holds %s, not the %s kind '%s' lowers (%s:%zu)\n",
                                 hex32(site).c_str(), hex32(word).c_str(),
                                 kind == LiftKind::Call ? "bl / bctrl" : lift_kind_is_bctrl(kind) ? "bctrl" : "bl",
                                 kind_text.c_str(), list, line_number);
                    return 1;
                }
                if (g_gx_sites.count(site) != 0u || g_platform_sites.count(site) != 0u ||
                    !g_lift_sites.emplace(site, listed).second) {
                    std::fprintf(stderr, "lowered call site %s is listed twice (%s:%zu)\n", hex32(site).c_str(), list,
                                 line_number);
                    return 1;
                }
                g_lift_site_lists[site] = std::string(list) + ":" + std::to_string(line_number);
            } else if (verb == "retire") {
                std::string address_text, value_text;
                u32 address = 0;
                u32 value = 0;
                if (!(fields >> address_text >> value_text) || !parse_hex32(address_text, address) ||
                    !parse_hex32(value_text, value)) {
                    std::fprintf(stderr, "%s:%zu: expected \"retire <data word hex> <value hex>\"\n", list,
                                 line_number);
                    return 1;
                }
                bool data_word = (address & 3u) == 0u;
                if (data_word) {
                    data_word = false;
                    for (const Section& section : image.sections) {
                        if (!section.executable && address >= section.address &&
                            address + 4u <= section.address + section.size) {
                            data_word = true;
                        }
                    }
                }
                if (!data_word || image.read32(address) != value) {
                    std::fprintf(stderr,
                                 "retired root %s is not a data word holding %s (stale %s:%zu?)\n",
                                 hex32(address).c_str(), hex32(value).c_str(), list, line_number);
                    return 1;
                }
                if (!g_retired_roots.emplace(address, value).second) {
                    std::fprintf(stderr, "retired root %s is listed twice (%s:%zu)\n", hex32(address).c_str(), list,
                                 line_number);
                    return 1;
                }
            } else {
                std::fprintf(stderr, "%s:%zu: unknown line kind '%s' (site or retire)\n", list, line_number,
                             verb.c_str());
                return 1;
            }
        }
    }
    std::size_t native_owner_rows = 0;
    if (!options.native_owners.empty()) {
        std::set<u32> owners;
        for (const std::string& owners_list : options.native_owners) {
            std::ifstream file(owners_list);
            if (!file) {
                std::fprintf(stderr, "cannot read %s\n", owners_list.c_str());
                return 1;
            }
            std::string line_text;
            while (std::getline(file, line_text)) {
                if (line_text.empty() || line_text[0] == '#') continue;
                owners.insert(static_cast<u32>(std::strtoul(line_text.c_str(), nullptr, 0)));
            }
        }
        for (Candidate& candidate : accepted) {
            if (owners.erase(candidate.record.address) != 0u) {
                candidate.native_owner = true;
                ++native_owner_rows;
            }
        }
        // What remains must be an owner entry: inside a listed owner's extent.
        for (const u32 address : owners) {
            Candidate* holder = nullptr;
            for (Candidate& candidate : accepted) {
                if (candidate.native_owner && address > candidate.record.address &&
                    address < candidate.record.address + candidate.record.size) {
                    holder = &candidate;
                    break;
                }
            }
            if (holder == nullptr) {
                std::fprintf(stderr,
                             "native owner %s is not the start of an accepted row nor inside a listed owner\n",
                             hex32(address).c_str());
                return 1;
            }
            holder->owner_entries.insert(address);
        }
    }
    // R13: every hot-set address must start an accepted row (a gap-sweep row
    // included): the profile names fn_<row>_body symbols, so anything else is a
    // stale or hand-edited list. Checked before any output is written.
    if (!g_hot_set.empty()) {
        std::set<u32> row_starts;
        for (const Candidate& candidate : accepted) row_starts.insert(candidate.record.address);
        for (const u32 address : std::set<u32>(g_hot_set.begin(), g_hot_set.end())) {
            if (row_starts.count(address) == 0u) {
                std::fprintf(stderr, "hot set %s: %s is not the start of an accepted row\n",
                             options.hot_set.c_str(), hex32(address).c_str());
                return 1;
            }
        }
    }
    // The listed platform sites an accepted row translates before pruning: a
    // site held here that no surviving row emits was pruned (stale check below).
    std::set<u32> platform_sites_held;
    for (const auto& listed : g_platform_sites) {
        for (const Candidate& candidate : accepted) {
            if (candidate.analysis.reachable.count(listed.first) != 0u) {
                platform_sites_held.insert(listed.first);
                break;
            }
        }
    }
    std::size_t pruned_rows = 0;
    std::size_t pruned_instructions = 0;
    if (!options.prune_roots.empty()) {
        std::vector<u32> extra_roots;
        struct CensusRoot {
            u32 root;
            std::vector<u32> sites;
        };
        std::vector<CensusRoot> census_roots;
        {
            std::ifstream file(options.prune_roots);
            if (!file) {
                std::fprintf(stderr, "cannot read %s\n", options.prune_roots.c_str());
                return 1;
            }
            std::string line_text;
            while (std::getline(file, line_text)) {
                if (line_text.empty() || line_text[0] == '#') continue;
                std::istringstream fields(line_text);
                std::string first, kind;
                fields >> first;
                const u32 address = static_cast<u32>(std::strtoul(first.c_str(), nullptr, 0));
                if (fields >> kind && kind == "census") {
                    CensusRoot root{address, {}};
                    std::string site;
                    while (fields >> site) root.sites.push_back(static_cast<u32>(std::strtoul(site.c_str(), nullptr, 0)));
                    census_roots.push_back(std::move(root));
                } else {
                    extra_roots.push_back(address);
                }
            }
        }
        std::vector<std::pair<u32, u32>> owned;  // (reachable address, row)
        for (std::size_t i = 0; i < accepted.size(); ++i) {
            for (const u32 address : accepted[i].analysis.reachable) {
                owned.emplace_back(address, static_cast<u32>(i));
            }
        }
        std::sort(owned.begin(), owned.end());
        std::vector<u32> prefix_end(accepted.size(), 0u);
        {
            u32 running = 0u;
            for (std::size_t i = 0; i < accepted.size(); ++i) {
                running = std::max(running, accepted[i].record.address + accepted[i].record.size);
                prefix_end[i] = running;
            }
        }
        u32 code_low = 0xffffffffu;
        u32 code_high = 0u;
        for (const Candidate& c : accepted) {
            code_low = std::min(code_low, c.record.address);
            code_high = std::max(code_high, c.record.address + c.record.size);
        }
        const auto rows_holding = [&](u32 address) {
            std::vector<u32> rows;
            auto it = std::lower_bound(owned.begin(), owned.end(), std::make_pair(address, 0u));
            for (; it != owned.end() && it->first == address; ++it) rows.push_back(it->second);
            return rows;
        };
        // The listed jump tables (verified below, once every row's formations
        // are known).
        struct ListedTable {
            u32 site;
            u32 table;
            u32 entries;
        };
        std::vector<ListedTable> listed_tables;
        if (!options.jump_tables.empty()) {
            std::ifstream file(options.jump_tables);
            if (!file) {
                std::fprintf(stderr, "cannot read %s\n", options.jump_tables.c_str());
                return 1;
            }
            std::string line_text;
            while (std::getline(file, line_text)) {
                if (line_text.empty() || line_text[0] == '#') continue;
                std::istringstream fields(line_text);
                std::string site_text, table_text, count_text;
                fields >> site_text >> table_text >> count_text;
                listed_tables.push_back(ListedTable{static_cast<u32>(std::strtoul(site_text.c_str(), nullptr, 0)),
                                                    static_cast<u32>(std::strtoul(table_text.c_str(), nullptr, 0)),
                                                    static_cast<u32>(std::strtoul(count_text.c_str(), nullptr, 0))});
            }
        }
        std::vector<std::pair<u32, u32>> listed_ranges;  // [table, end), sorted
        for (const ListedTable& t : listed_tables) listed_ranges.push_back({t.table, t.table + 4u * t.entries});
        std::sort(listed_ranges.begin(), listed_ranges.end());
        std::vector<u32> listed_prefix_end(listed_ranges.size());
        for (std::size_t i = 0; i < listed_ranges.size(); ++i)
            listed_prefix_end[i] = std::max(i != 0u ? listed_prefix_end[i - 1u] : 0u, listed_ranges[i].second);
        const auto in_listed_range = [&](u32 value) {
            std::size_t j = static_cast<std::size_t>(
                std::upper_bound(listed_ranges.begin(), listed_ranges.end(), std::make_pair(value, 0xffffffffu)) -
                listed_ranges.begin());
            while (j > 0u) {
                --j;
                if (listed_prefix_end[j] <= value) break;
                if (value < listed_ranges[j].second) return true;
            }
            return false;
        };
        // Every row's hi/lo formations, each pair forming only what it computes
        // (high: lis = rA 0 addis, or oris; low: addi/addic/addic. add, ori ors;
        // li-then-oris ors too). Word-aligned code values are edges; any value
        // inside a listed jump table is recorded to verify that table.
        //
        // H5 (2026-10-05): the halves must also MEET THROUGH A REGISTER - the
        // low half reads the register the high half wrote (lis/addis/oris rX,
        // then addi/addic/ori from rX), or, li-then-oris, the oris reads the
        // register the low half wrote. Any such pair in the row counts, in
        // either order and across blocks, so control flow stays conservative;
        // what is refused is a high and a low that never touch the same
        // register (AIInit 0x80382244's `lis r3,0x8038` and its unrelated
        // `addi r5,r4,-0x5BD8` "formed" 0x8037A428 inside DVDLowInquiry).
        //
        // H6 (2026-10-06): a low half whose source register is defined earlier
        // in its own straight-line block meets only that definition - the
        // exact formation when it is a high half, none otherwise. The walk back
        // stops (and the low half keeps H5's pairing) at a block boundary - any
        // label, entry, resume point, branch target, root or jump-table case -
        // at a call, at an instruction that does not fall through, and at an
        // unmodelled register write (gpr_writes). The EXI library's
        // `lis r3,0x805A; addi r0,r3,0x6650` (the data address 0x805A6650) had
        // also met a later `lis r3,0x803B` and "formed" 0x803B6650 inside
        // SIGetStatus.
        std::vector<std::vector<u32>> formed(accepted.size());
        std::map<u32, std::vector<u32>> formers;       // code address -> rows forming it
        std::vector<std::pair<u32, u32>> table_forms;  // (value inside a listed table, row)
        struct Half {
            u32 kind;    // high: 0 lis | 1 oris; low: 0 add | 1 ori
            u32 half;
            u32 dest;    // the register the instruction writes
            u32 source;  // the register it reads (32: none - lis, or addi from 0)
            bool operator<(const Half& o) const noexcept {
                return std::tie(kind, half, dest, source) < std::tie(o.kind, o.half, o.dest, o.source);
            }
        };
        // H6's block boundaries: every address control can arrive at other than
        // by falling through from the instruction before it.
        std::unordered_set<u32> block_starts;
        for (const Candidate& c : accepted) {
            block_starts.insert(c.record.address);
            block_starts.insert(c.entries.begin(), c.entries.end());
            block_starts.insert(c.analysis.labels.begin(), c.analysis.labels.end());
            block_starts.insert(c.analysis.resume_points.begin(), c.analysis.resume_points.end());
            block_starts.insert(c.analysis.loop_heads.begin(), c.analysis.loop_heads.end());
            block_starts.insert(c.analysis.recovered_entries.begin(), c.analysis.recovered_entries.end());
            block_starts.insert(c.analysis.external_targets.begin(), c.analysis.external_targets.end());
        }
        block_starts.insert(extra_roots.begin(), extra_roots.end());
        for (const CensusRoot& root : census_roots) block_starts.insert(root.root);
        for (const ListedTable& listed : listed_tables) {
            for (u32 i = 0; i < listed.entries; ++i) {
                if (image.at(listed.table + 4u * i, 4u) != nullptr) block_starts.insert(image.read32(listed.table + 4u * i));
            }
        }
        const auto high_of = [](u32 instruction, Half& high) {
            if (op_primary(instruction) == 15u && field_a(instruction) == 0u) {
                high = {0u, instruction & 0xffffu, field_d(instruction), 32u};  // lis rD
                return true;
            }
            if (op_primary(instruction) == 25u) {
                high = {1u, instruction & 0xffffu, field_a(instruction), field_d(instruction)};  // oris rA,rS
                return true;
            }
            return false;
        };
        std::size_t h6_exact = 0, h6_none = 0, h6_conservative = 0;
        for (std::size_t i = 0; i < accepted.size(); ++i) {
            const std::set<u32>& reachable = accepted[i].analysis.reachable;
            std::set<Half> highs;
            std::set<Half> lows;
            std::set<u32> values;
            const auto form = [&values](const Half& high, const Half& low) {
                const u32 added = (high.half << 16) + static_cast<u32>(sign_extend(low.half, 16));
                const u32 ored = (high.half << 16) | low.half;
                if (low.kind == 0u) {
                    values.insert(added);
                    if (high.kind == 1u) values.insert(ored);
                } else {
                    values.insert(ored);
                }
            };
            for (const u32 address : reachable) {
                const u32 instruction = image.read32(address);
                const u32 primary = op_primary(instruction);
                const u32 half = instruction & 0xffffu;
                Half high{};
                if (high_of(instruction, high)) {
                    highs.insert(high);
                    continue;
                }
                Half low{};
                if (primary == 14u || primary == 12u || primary == 13u) {
                    const u32 source = field_a(instruction);  // addi rD,0 is li: no source
                    low = {0u, half, field_d(instruction), primary == 14u && source == 0u ? 32u : source};
                } else if (primary == 24u) {
                    low = {1u, half, field_a(instruction), field_d(instruction)};  // ori rA,rS
                } else {
                    continue;
                }
                if (low.source == 32u) {  // li: the li-then-oris pairing stays H5's
                    lows.insert(low);
                    continue;
                }
                // H6: walk back through the low half's own block.
                enum class Reach { Exact, None, Conservative } outcome = Reach::Conservative;
                Half exact{};
                for (u32 at = address;;) {
                    if (block_starts.count(at) != 0u) break;
                    const u32 previous = at - 4u;
                    if (reachable.count(previous) == 0u) break;
                    const u32 before = image.read32(previous);
                    const Flow flow = classify(before, previous).flow;
                    if (flow != Flow::Normal && flow != Flow::CondBranch) break;  // calls, jumps, sc, rfi
                    if (high_of(before, exact) && exact.dest == low.source) {
                        outcome = Reach::Exact;
                        break;
                    }
                    const u64 writes = gpr_writes(before);
                    if (writes == kUnmodelledWrites) break;
                    if ((writes >> low.source) & 1u) {
                        outcome = Reach::None;
                        break;
                    }
                    at = previous;
                }
                if (outcome == Reach::Exact) {
                    ++h6_exact;
                    form(exact, low);
                } else if (outcome == Reach::None) {
                    ++h6_none;
                } else {
                    ++h6_conservative;
                    lows.insert(low);
                }
            }
            for (const Half& high : highs) {
                for (const Half& low : lows) {
                    const bool meet = low.source == high.dest || (high.kind == 1u && high.source == low.dest);
                    if (!meet) continue;
                    form(high, low);
                }
            }
            for (const u32 value : values) {
                if (in_listed_range(value)) table_forms.push_back({value, static_cast<u32>(i)});
                if ((value & 3u) != 0u || value < code_low || value >= code_high) continue;
                formed[i].push_back(value);
                formers[value].push_back(static_cast<u32>(i));
            }
        }
        std::sort(table_forms.begin(), table_forms.end());
        std::fprintf(stderr,
                     "formations (H6): %zu low halves met their block's high half, %zu a non-high "
                     "definition, %zu kept the conservative pairing\n",
                     h6_exact, h6_none, h6_conservative);
        // Jump tables: verified ones become edges from their dispatching rows.
        struct JumpTable {
            u32 table;
            u32 entries;
        };
        std::vector<JumpTable> tables;
        std::map<u32, std::vector<u32>> row_tables;  // row -> verified table indices
        std::size_t tables_refused = 0;
        if (!listed_tables.empty()) {
            std::vector<u32> data_targets;  // every aligned data word, by value
            for (const Section& section : image.sections) {
                if (section.executable) continue;
                for (u32 offset = 0; offset + 4u <= section.size; offset += 4u)
                    data_targets.push_back(image.read32(section.address + offset));
            }
            std::sort(data_targets.begin(), data_targets.end());
            for (const ListedTable& listed : listed_tables) {
                const u32 table = listed.table;
                const u32 entries = listed.entries;
                const std::vector<u32> dispatchers = rows_holding(listed.site);
                const u32 table_end = table + 4u * entries;
                bool verified = entries != 0u && !dispatchers.empty() && table_end > table &&
                                image.at(table, 4u * entries) != nullptr;
                for (u32 i = 0; verified && i < entries; ++i) {
                    const u32 target = image.read32(table + 4u * i);
                    verified = target >= code_low && target < code_high;
                }
                if (verified) {  // no data word points into the table
                    verified = std::lower_bound(data_targets.begin(), data_targets.end(), table) ==
                               std::lower_bound(data_targets.begin(), data_targets.end(), table_end);
                }
                if (!verified) {
                    ++tables_refused;
                    continue;
                }
                // Every row that can form an address inside the table may read
                // it - the dispatcher and any other: the words are reached from
                // each of them.
                std::set<u32> readers(dispatchers.begin(), dispatchers.end());
                for (auto it = std::lower_bound(table_forms.begin(), table_forms.end(), std::make_pair(table, 0u));
                     it != table_forms.end() && it->first < table_end; ++it)
                    readers.insert(it->second);
                for (auto it = formers.lower_bound(table); it != formers.end() && it->first < table_end; ++it)
                    readers.insert(it->second.begin(), it->second.end());
                for (const u32 row : readers) row_tables[row].push_back(static_cast<u32>(tables.size()));
                tables.push_back(JumpTable{table, entries});
            }
        }
        const auto in_table = [&](u32 address) {
            for (const JumpTable& t : tables)
                if (address >= t.table && address < t.table + 4u * t.entries) return true;
            return false;
        };
        // Census roots: conditional on the rows that construct them.
        std::map<u32, std::vector<u32>> row_census;  // row -> census roots it constructs
        std::size_t census_unconditional = 0;
        for (const CensusRoot& root : census_roots) {
            std::set<u32> constructors;
            for (const u32 site : root.sites)
                for (const u32 row : rows_holding(site)) constructors.insert(row);
            if (const auto it = formers.find(root.root); it != formers.end())
                constructors.insert(it->second.begin(), it->second.end());
            if (constructors.empty()) {
                extra_roots.push_back(root.root);
                ++census_unconditional;
                continue;
            }
            for (const u32 row : constructors) row_census[row].push_back(root.root);
        }

        std::vector<char> live(accepted.size(), 0);
        std::vector<u32> work;
        // --why-live: who first reached each row - a row (>= 0) or a seed kind
        // (< 0) - and the address it was reached through (the data word's own
        // address for a data-word seed).
        constexpr long long kWhyEntry = -1, kWhyRoot = -2, kWhyData = -4, kWhyNone = -5;
        std::vector<std::pair<long long, u32>> why(accepted.size(), {kWhyNone, 0u});
        long long why_source = kWhyEntry;
        u32 why_data_word = 0u;
        // --live-graph: (source, via, row) for every reach event, reduced on write.
        std::vector<std::tuple<long long, u32, u32>> graph;
        const bool want_graph = !options.live_graph.empty();
        const auto reach = [&](u32 address) {
            auto it = std::lower_bound(owned.begin(), owned.end(), std::make_pair(address, 0u));
            bool any = false;
            for (; it != owned.end() && it->first == address; ++it) {
                any = true;
                if (want_graph)
                    graph.emplace_back(why_source, why_source == kWhyData ? why_data_word : address, it->second);
                if (!live[it->second]) {
                    live[it->second] = 1;
                    why[it->second] = {why_source, why_source == kWhyData ? why_data_word : address};
                    work.push_back(it->second);
                }
            }
            if (any) return;
            // Extent fallback: every row whose [start, start + size) holds it.
            std::size_t j = static_cast<std::size_t>(
                std::upper_bound(accepted.begin(), accepted.end(), address,
                                 [](u32 value, const Candidate& c) { return value < c.record.address; }) -
                accepted.begin());
            while (j > 0) {
                --j;
                if (prefix_end[j] <= address) break;
                const Candidate& c = accepted[j];
                if (want_graph && address < c.record.address + c.record.size)
                    graph.emplace_back(why_source, why_source == kWhyData ? why_data_word : address, static_cast<u32>(j));
                if (address < c.record.address + c.record.size && !live[j]) {
                    live[j] = 1;
                    why[j] = {why_source, why_source == kWhyData ? why_data_word : address};
                    work.push_back(static_cast<u32>(j));
                }
            }
        };
        why_source = kWhyEntry;
        reach(image.entry);
        why_source = kWhyRoot;
        for (const u32 address : extra_roots) reach(address);
        std::size_t data_words = 0;
        why_source = kWhyData;
        for (const Section& section : image.sections) {
            if (section.executable) continue;
            for (u32 offset = 0; offset + 4u <= section.size; offset += 4u) {
                const u32 value = image.read32(section.address + offset);
                // A retired executable root (--lift-sites): its pinned contract
                // proves it cannot dispatch code (e.g. a retired frame's range key).
                if (g_retired_roots.count(section.address + offset) != 0u) continue;
                if (value >= code_low && value < code_high && !in_table(section.address + offset)) {
                    ++data_words;
                    why_data_word = section.address + offset;
                    reach(value);
                }
            }
        }
        while (!work.empty()) {
            const u32 row = work.back();
            work.pop_back();
            why_source = static_cast<long long>(row);
            const Candidate& c = accepted[row];
            // An owner's original body never executes: nothing it calls,
            // branches to, forms or shares is reached through it.
            if (c.native_owner) continue;
            if (const auto it = row_tables.find(row); it != row_tables.end())
                for (const u32 index : it->second)
                    for (u32 i = 0; i < tables[index].entries; ++i) reach(image.read32(tables[index].table + 4u * i));
            if (const auto it = row_census.find(row); it != row_census.end())
                for (const u32 root : it->second) reach(root);
            const u32 end = c.record.address + c.record.size;
            bool tail = false;
            for (const u32 address : c.analysis.reachable) {
                reach(address);  // every row sharing this instruction
                const u32 instruction = image.read32(address);
                const Decoded decoded = classify(instruction, address);
                // A lowered call site calls the native subsystem, not its old callee.
                const auto lift = g_lift_sites.find(address);
                const bool lowered = lift != g_lift_sites.end();
                if (lowered && lift->second.kind == LiftKind::TypeQuery) {
                    // The native lookup replaces the old cast, but a failed
                    // reference still enters throw and supplies its destructor.
                    // These are executable dependencies, unlike the retired
                    // cast's return PC retained only for exception range lookup.
                    reach(0x803A2660u);
                    reach(0x803A1D60u);
                }
                if (!lowered && decoded.flow != Flow::Normal && decoded.target != 0xffffffffu &&
                    (decoded.flow == Flow::Branch || decoded.flow == Flow::CondBranch ||
                     decoded.flow == Flow::Call || decoded.flow == Flow::CondCall)) {
                    reach(decoded.target);
                }
                if (address + 4u == end &&
                    (flow_falls_through(decoded.flow) || op_primary(instruction) == 17u)) {
                    tail = true;
                }
            }
            if (tail) reach(end);
            for (const u32 value : formed[row]) reach(value);
        }
        if (want_graph) {
            // One edge per (from row, to row), with its lowest instruction address; every seed address is kept
            // (dropping one root must not hide another root of the same row).
            using Event = std::tuple<long long, u32, u32>;
            std::sort(graph.begin(), graph.end(), [](const Event& a, const Event& b) {
                return std::tie(std::get<0>(a), std::get<2>(a), std::get<1>(a)) <
                       std::tie(std::get<0>(b), std::get<2>(b), std::get<1>(b));
            });
            graph.erase(std::unique(graph.begin(), graph.end(),
                                    [](const Event& a, const Event& b) {
                                        return std::get<0>(a) == std::get<0>(b) && std::get<2>(a) == std::get<2>(b) &&
                                               (std::get<0>(a) >= 0 || std::get<1>(a) == std::get<1>(b));
                                    }),
                        graph.end());
            std::ofstream out(options.live_graph);
            out << "ppcrecomp-live-graph-v1\n";
            for (std::size_t j = 0; j < accepted.size(); ++j)
                out << "row " << hex32(accepted[j].record.address) << ' ' << accepted[j].record.size << ' '
                    << (accepted[j].native_owner ? "owner" : "body") << ' ' << (live[j] ? "live" : "pruned") << ' '
                    << accepted[j].record.name << '\n';
            for (const auto& [source, via, row] : graph) {
                const std::string target = hex32(accepted[row].record.address);
                if (source >= 0)
                    out << "edge " << hex32(accepted[static_cast<std::size_t>(source)].record.address) << ' '
                        << hex32(via) << ' ' << target << '\n';
                else
                    out << "seed " << (source == kWhyEntry ? "entry" : source == kWhyRoot ? "root" : "data") << ' '
                        << hex32(via) << ' ' << target << '\n';
            }
            if (!out) {
                std::fprintf(stderr, "cannot write %s\n", options.live_graph.c_str());
                return 1;
            }
        }
        if (!options.why_live.empty()) {
            std::stringstream list(options.why_live);
            for (std::string item; std::getline(list, item, ',');) {
                const u32 target = static_cast<u32>(std::strtoul(item.c_str(), nullptr, 16));
                bool held = false;
                for (std::size_t j = 0; j < accepted.size(); ++j) {
                    const Candidate& c = accepted[j];
                    if (target < c.record.address || target >= c.record.address + c.record.size) continue;
                    held = true;
                    std::fprintf(stderr, "why-live %08X: row %08X %s %s\n", target, c.record.address,
                                 c.record.name.c_str(), live[j] ? "LIVE" : "pruned");
                    std::size_t at = j;
                    for (int depth = 0; live[at] && depth < 256; ++depth) {
                        const long long source = why[at].first;
                        const u32 via = why[at].second;
                        if (source >= 0) {
                            const Candidate& parent = accepted[static_cast<std::size_t>(source)];
                            std::fprintf(stderr, "  <- row %08X %s (through %08X)\n", parent.record.address,
                                         parent.record.name.c_str(), via);
                            at = static_cast<std::size_t>(source);
                            continue;
                        }
                        const char* kind = source == kWhyEntry       ? "the DOL entry"
                                           : source == kWhyRoot      ? "a liveness root"
                                           : source == kWhyData      ? "the data word at"
                                                                     : "no recorded reason";
                        std::fprintf(stderr, "  <- %s %08X\n", kind, via);
                        break;
                    }
                }
                if (!held) std::fprintf(stderr, "why-live %08X: no row holds it\n", target);
            }
        }
        std::ofstream pruned_list(options.out + "/recomp_pruned.csv");
        pruned_list << "address,name,reachable_instructions\n";
        std::vector<Candidate> kept;
        kept.reserve(accepted.size());
        for (std::size_t i = 0; i < accepted.size(); ++i) {
            if (live[i]) {
                kept.push_back(std::move(accepted[i]));
                continue;
            }
            ++pruned_rows;
            pruned_instructions += accepted[i].analysis.reachable.size();
            pruned_list << hex32(accepted[i].record.address) << "," << accepted[i].record.name << ","
                        << accepted[i].analysis.reachable.size() << "\n";
            for (const u32 address : accepted[i].entries) {
                const auto it = recompiled.find(address);
                if (it != recompiled.end() && it->second == accepted[i].record.address) recompiled.erase(it);
            }
        }
        accepted.swap(kept);
        std::fprintf(stderr,
                     "unreachable pruning: %zu rows (%zu instructions) not emitted, %zu kept; "
                     "%zu data words and %zu extra roots seeded\n",
                     pruned_rows, pruned_instructions, accepted.size(), data_words, extra_roots.size());
        std::fprintf(stderr,
                     "liveness: %zu jump tables verified as dispatch edges, %zu left rooted; "
                     "%zu census roots conditional on their constructors, %zu unconditional\n",
                     tables.size(), tables_refused, census_roots.size() - census_unconditional,
                     census_unconditional);
    }

    if (native_owner_rows != 0u) {
        // Whole-function replacement: no surviving code may enter an owner
        // anywhere but its start (an owner has no body to enter).
        std::set<u32> owner_starts;
        for (const Candidate& c : accepted) {
            if (c.native_owner) owner_starts.insert(c.record.address);
        }
        std::set<u32> owned_instructions;
        for (const Candidate& c : accepted) {
            if (c.native_owner) owned_instructions.insert(c.analysis.reachable.begin(), c.analysis.reachable.end());
        }
        for (const Candidate& c : accepted) {
            if (c.native_owner) continue;
            for (const u32 address : c.analysis.reachable) {
                const Decoded decoded = classify(image.read32(address), address);
                if (decoded.target == 0xffffffffu || decoded.flow == Flow::Normal ||
                    decoded.flow == Flow::Return || decoded.flow == Flow::Indirect ||
                    decoded.flow == Flow::Terminal) {
                    continue;
                }
                if (owned_instructions.count(decoded.target) != 0u && owner_starts.count(decoded.target) == 0u &&
                    c.analysis.reachable.count(decoded.target) == 0u) {
                    bool declared = false;
                    for (const Candidate& owner : accepted) {
                        if (owner.native_owner && owner.owner_entries.count(decoded.target) != 0u) declared = true;
                    }
                    if (declared) continue;
                    std::fprintf(stderr, "%s branches into the interior of a native owner at %s\n",
                                 hex32(address).c_str(), hex32(decoded.target).c_str());
                    return 1;
                }
            }
        }
        for (Candidate& c : accepted) {
            if (!c.native_owner) continue;
            for (const u32 address : c.entries) {
                if (address == c.record.address || c.owner_entries.count(address) != 0u) continue;
                const auto it = recompiled.find(address);
                if (it != recompiled.end() && it->second == c.record.address) recompiled.erase(it);
            }
            for (const u32 address : c.owner_entries) recompiled[address] = c.record.address;
        }
        std::fprintf(stderr, "native owners: %zu rows emit only a thunk to the runtime owner\n",
                     native_owner_rows);
    }

    if (!options.empty_variadic_leaves.empty()) {
        std::ifstream list(options.empty_variadic_leaves);
        if (!list) {
            std::fprintf(stderr, "cannot open empty variadic leaf list %s\n", options.empty_variadic_leaves.c_str());
            return 1;
        }
        std::string row;
        while (std::getline(list, row)) {
            if (const auto comment = row.find('#'); comment != std::string::npos) row.erase(comment);
            std::istringstream fields(row);
            std::string address_text, extra;
            if (!(fields >> address_text)) continue;
            u32 address = 0;
            if (!parse_hex32(address_text, address) || (fields >> extra) || (address & 3u) ||
                g_empty_variadic_leaves.count(address)) {
                std::fprintf(stderr, "malformed/duplicate empty variadic leaf: %s\n", row.c_str());
                return 1;
            }
            const auto found = std::find_if(accepted.begin(), accepted.end(), [&](const Candidate& c) {
                return c.record.address == address;
            });
            EmptyVariadicKind kind{};
            if (found == accepted.end() || found->native_owner || found->absorbed_by ||
                !found->absorbed_entries.empty() || found->entries != std::set<u32>{address} ||
                !empty_variadic_kind(image, *found, kind)) {
                std::fprintf(stderr, "empty variadic leaf %s has changed body, entries or ownership\n", address_text.c_str());
                return 1;
            }
            const u32 end = address + found->record.size;
            for (const Candidate& other : accepted) {
                if (&other == &*found) continue;
                if (other.record.address < end && address < other.record.address + other.record.size) {
                    std::fprintf(stderr, "empty variadic leaf %s overlaps another emitted context\n", address_text.c_str());
                    return 1;
                }
            }
            for (const auto& site : g_lift_sites) if (address <= site.first && site.first < end) {
                std::fprintf(stderr, "empty variadic leaf overlaps a lowered call site\n");
                return 1;
            }
            g_empty_variadic_leaves.emplace(address, kind);
        }
    }

    // Every listed platform site is emitted, or the list is stale: refused
    // before any source is rewritten. A site is emitted exactly when a
    // surviving row that is neither a native owner nor absorbed (its absorber
    // translates the same instructions) reaches it.
    for (const auto& listed : g_platform_sites) {
        bool emitted = false;
        bool owned = false;
        for (const Candidate& candidate : accepted) {
            if (candidate.analysis.reachable.count(listed.first) == 0u) continue;
            if (candidate.native_owner) owned = true;
            else if (candidate.absorbed_by == 0u) emitted = true;
        }
        if (emitted) continue;
        std::fprintf(stderr, "stale platform site %s (%s): %s\n", hex32(listed.first).c_str(),
                     options.platform_sites.c_str(),
                     owned ? "its function is a native owner (the original body is not emitted)"
                     : platform_sites_held.count(listed.first) != 0u ? "its function is pruned as unreachable"
                                                                     : "no emitted function translates it");
        return 1;
    }

    // Second pass: emit with the final set known, so intra-set calls are direct.
    g_wait_points_emitted.clear();
    g_platform_sites_emitted.clear();
    g_platform_site_emissions = 0;
    g_platform_site_dead_emissions = 0;
    g_gx_sites_emitted.clear();
    g_psq_sites_emitted.clear();
    g_psq_site_emissions = 0;
    g_gx_fail_emissions = 0;
    g_gx_site_emissions = 0;
    g_lift_sites_emitted.clear();
    g_fp_checks_emitted = 0;
    g_fp_checks_hoisted = 0;
    g_ir_insns = 0;
    g_hot_label_insns = 0;
    g_cold_mid_insns = 0;
    g_hot_src_bytes = 0;
    g_cold_src_bytes = 0;
    g_hot_set_rows = 0;
    g_hot_set_sites = 0;
    // ---- write the generated sources ----
    const std::string out = options.out;
    OutputStats output_stats;
    const std::size_t shards = std::max<std::size_t>(1, options.shards);
    ShardLayout layout;
    if (!plan_shards(out + "/recomp_layout.csv", accepted, shards, layout)) return 1;
    std::vector<std::vector<std::size_t>> shard_rows(shards);
    for (std::size_t index = 0; index < accepted.size(); ++index)
        shard_rows[layout.placement[index]].push_back(index);

    {
        std::ostringstream header;
        header << "#pragma once\n";
        header << "// Generated by tools/ppcrecomp. Do not edit.\n";
        header << "#include \"gekko/recomp.h\"\n\n";
        header << "namespace gekko {\nnamespace recomp {\n\n";
        for (const Candidate& candidate : accepted) {
            if (g_empty_variadic_leaves.count(candidate.record.address)) {
                header << "void " << fn_symbol(candidate.record.address) << "(Cpu& c);\n";
                continue;
            }
            if (candidate.native_owner) {
                header << "void " << fn_symbol(candidate.record.address) << "(Cpu& c);\n";
                for (const u32 address : candidate.owner_entries) {
                    header << "void " << entry_symbol(candidate.record.address, address) << "(Cpu& c);\n";
                }
                continue;
            }
            header << "void " << fn_symbol(candidate.record.address) << "_at(Cpu& c, u32 e);\n";
            for (const u32 address : candidate.entries) {
                if (address == candidate.record.address) {
                    header << "void " << fn_symbol(address) << "(Cpu& c);\n";
                } else {
                    header << "void " << entry_symbol(candidate.record.address, address)
                           << "(Cpu& c);\n";
                }
            }
        }
        // The lowered call sites' generated functions (recomp_table.cpp).
        if (std::any_of(g_lift_sites.begin(), g_lift_sites.end(), [](const auto& entry) {
                return entry.second.kind == LiftKind::AffineMatrixCopy;
            })) {
            header << "bool call_matrix_copy(Cpu& c, u32 site);\n";
        }
        if (std::any_of(g_lift_sites.begin(), g_lift_sites.end(), [](const auto& entry) {
                return entry.second.kind == LiftKind::AffineMatrixCompose;
            })) {
            header << "bool call_matrix_compose(Cpu& c, u32 site);\n";
        }
        if (std::any_of(g_lift_sites.begin(), g_lift_sites.end(), [](const auto& entry) {
                return entry.second.kind == LiftKind::TypeQuery;
            })) {
            header << "bool call_type_query(Cpu& c, u32 site, u16 query, u32 target);\n";
        }
        for (const auto& listed : g_lift_sites) {
            header << "void " << lift_site_symbol(listed.first) << "(Cpu& c);\n";
        }
        for (const auto& leaf : g_empty_variadic_leaves) {
            header << "void " << lift_site_symbol(leaf.first) << "(Cpu& c);\n";
        }
        header << "\n}  // namespace recomp\n}  // namespace gekko\n";
        if (!write_if_changed(out + "/recomp_decls.h", header.str(), output_stats)) return 1;
    }


    // Emit one shard at a time. Keeping every emitted body alive duplicated
    // the whole generated corpus in RAM; only a single shard and current
    // function need storage. Preserve the existing row order and placement.
    for (std::size_t shard = 0; shard < shards; ++shard) {
        std::ostringstream stream;
        stream << "// Generated by tools/ppcrecomp. Do not edit.\n";
        stream << "#include \"recomp_decls.h\"\n\n";
        stream << "namespace gekko {\nnamespace recomp {\n\n";
        for (const std::size_t index : shard_rows[shard]) {
            const Candidate& candidate = accepted[index];
            if (g_empty_variadic_leaves.count(candidate.record.address)) {
                const u32 address = candidate.record.address;
                stream << "// Reconstructed empty variadic leaf; original argument-save frame removed.\n"
                       << "void " << fn_symbol(address) << "(Cpu& c) {\n"
                       << "    if (c.try_hle(" << hexu(address) << ") || c.lift_site_cut(" << hexu(address) << ")) return;\n"
                       << "    " << lift_site_symbol(address) << "(c);\n}\n\n";
                continue;
            }
            if (candidate.native_owner) {
                stream << "// " << hex32(candidate.record.address) << "  " << candidate.record.name
                       << "  (native owner: the original body is not emitted)\n";
                stream << "void " << fn_symbol(candidate.record.address) << "(Cpu& c) { c.serve_native_owner("
                       << hexu(candidate.record.address) << "); }\n";
                for (const u32 address : candidate.owner_entries) {
                    stream << "void " << entry_symbol(candidate.record.address, address)
                           << "(Cpu& c) { c.serve_native_owner(" << hexu(address) << "); }\n";
                }
                stream << "\n";
                continue;
            }
            std::string failure;
            const std::string body = emit_function(image, candidate, recompiled, failure);
            if (body.empty()) {
                std::fprintf(stderr, "second pass rejected %s: %s\n",
                             hex32(candidate.record.address).c_str(), failure.c_str());
                return 1;
            }
            stream << "// " << hex32(candidate.record.address) << "  " << candidate.record.name
                   << "  (" << candidate.record.size << " bytes, "
                   << candidate.analysis.reachable.size() << " reachable instructions)\n";
            stream << body << "\n";
        }
        stream << "}  // namespace recomp\n}  // namespace gekko\n";
        char name[64];
        std::snprintf(name, sizeof(name), "/recomp_%03zu.cpp", shard);
        if (!write_if_changed(out + name, stream.str(), output_stats)) return 1;
    }
    if (!g_hot_set.empty()) {
        std::fprintf(stderr, "hot set: %llu of %zu listed rows inline %llu accessor sites\n",
                     static_cast<unsigned long long>(g_hot_set_rows), g_hot_set.size(),
                     static_cast<unsigned long long>(g_hot_set_sites));
    }
    // Every listed GX pipe site a surviving translation holds was lowered: a
    // site in a row that is neither a native owner nor absorbed must have been
    // emitted (a listed site the generator left as an ordinary store would be
    // a store to a pipe that no longer exists).
    std::size_t gx_sites_not_emitted = 0;
    for (const auto& listed : g_gx_sites) {
        bool required = false;
        for (const Candidate& candidate : accepted) {
            if (candidate.analysis.reachable.count(listed.first) == 0u) continue;
            if (!candidate.native_owner && candidate.absorbed_by == 0u) required = true;
        }
        if (g_gx_sites_emitted.count(listed.first) != 0u) continue;
        if (required) {
            std::fprintf(stderr, "stale GX pipe site %s (%s): translated but never lowered\n",
                         hex32(listed.first).c_str(), options.gx_sites.c_str());
            return 1;
        }
        ++gx_sites_not_emitted;
    }
    // R16: a listed psq site a surviving translation holds must be specialised.
    for (const auto& listed : g_psq_sites) {
        bool required = false;
        for (const Candidate& candidate : accepted) {
            if (candidate.analysis.reachable.count(listed.first) == 0u) continue;
            if (!candidate.native_owner && candidate.absorbed_by == 0u) required = true;
        }
        if (required && g_psq_sites_emitted.count(listed.first) == 0u) {
            std::fprintf(stderr, "stale psq site %s (%s): translated but not specialised\n",
                         hex32(listed.first).c_str(), options.psq_sites.c_str());
            return 1;
        }
    }
    if (!g_gx_fail_sites.empty()) {
        std::fprintf(stderr, "gx fail sites: %zu emissions (hot and cold snippets)\n", g_gx_fail_emissions);
    }
    if (!g_psq_sites.empty()) {
        std::fprintf(stderr, "psq sites: %zu of %zu listed sites specialised (%zu emissions)\n",
                     g_psq_sites_emitted.size(), g_psq_sites.size(), g_psq_site_emissions);
    }
    // Every lowered call site is emitted: a listed site no surviving
    // translation holds is a stale list (its caller was pruned or changed).
    // Every stale site is named before the refusal, so a list is corrected in
    // one pass.
    std::size_t stale_lift_sites = 0;
    for (const auto& listed : g_lift_sites) {
        if (g_lift_sites_emitted.count(listed.first) == 0u) {
            std::fprintf(stderr, "stale lowered call site %s (%s): no surviving translation emits it\n",
                         hex32(listed.first).c_str(), g_lift_site_lists[listed.first].c_str());
            ++stale_lift_sites;
        }
    }
    if (stale_lift_sites != 0u) {
        std::fprintf(stderr, "%zu stale lowered call site(s)\n", stale_lift_sites);
        return 1;
    }
    // The emission bookkeeping (the report line) agrees with that check.
    for (const auto& listed : g_platform_sites) {
        if (g_platform_sites_emitted.count(listed.first) == 0u) {
            std::fprintf(stderr, "stale platform site %s (%s): reachable but never emitted\n",
                         hex32(listed.first).c_str(), options.platform_sites.c_str());
            return 1;
        }
    }

    std::size_t entry_points = 0;
    {
        std::ostringstream table;
        table << "// Generated by tools/ppcrecomp. Do not edit.\n";
        table << "#include \"recomp_decls.h\"\n";
        // The lifted subsystems the lowered call sites call (docs/lift-20261006).
        table << "#include \"gekko/lifted.h\"\n\n";
        if (std::any_of(g_lift_sites.begin(), g_lift_sites.end(), [](const auto& entry) {
                return entry.second.kind == LiftKind::ScalarSquareRoot;
            })) {
            table << "#include \"math/scalar.h\"\n\n";
        }
        if (std::any_of(g_lift_sites.begin(), g_lift_sites.end(), [](const auto& entry) {
                return entry.second.kind == LiftKind::AffineMatrixCopy ||
                       entry.second.kind == LiftKind::AffineMatrixCompose;
            })) {
            table << "#include \"math/matrix.h\"\n\n";
        }
        if (std::any_of(g_lift_sites.begin(), g_lift_sites.end(),
                        [](const auto& entry) { return lift_kind_is_msl(entry.second.kind); })) {
            table << "#include \"math/msl.h\"\n\n";
        }
        if (std::any_of(g_lift_sites.begin(), g_lift_sites.end(), [](const auto& entry) {
                return entry.second.kind == LiftKind::TypeQuery;
            })) {
            table << "#include \"native_type_boundary.h\"\n\n";
        }
        // gekko::scaffold declarations: the functions the generic call kind names (R2_DRIVER.md 2.1), with the
        // arity every listed site agrees on; their definitions are the named scaffolding outside generated code.
        {
            std::map<std::string, std::pair<u32, u32>> scaffold_functions;  // integer and float arguments
            for (const auto& listed : g_lift_sites) {
                if (listed.second.kind == LiftKind::Call)
                    scaffold_functions.emplace(listed.second.function,
                                               std::make_pair(listed.second.argc, listed.second.fpr_count));
            }
            if (!scaffold_functions.empty()) {
                table << "namespace gekko {\nnamespace scaffold {\n";
                for (const auto& [function, counts] : scaffold_functions) {
                    table << "u32 " << function << "(Cpu& c";
                    for (u32 i = 0; i < counts.first; ++i) table << ", u32";
                    for (u32 i = 0; i < counts.second; ++i) table << ", double";
                    table << ");\n";
                }
                table << "}  // namespace scaffold\n}  // namespace gekko\n\n";
            }
        }
        table << "namespace gekko {\nnamespace recomp {\n\n";
        if (std::any_of(g_lift_sites.begin(), g_lift_sites.end(), [](const auto& entry) {
                return entry.second.kind == LiftKind::AffineMatrixCopy;
            })) {
            table << R"CPP(// SCAFFOLDING (R2): removed when the remaining copy callers own native spans.
// Keep one boundary for the copy family; the pure value API owns the algorithm.
static GEKKO_NOINLINE void copy_matrix_boundary(Cpu& c) {
    auto& memory = c.guest_memory();
    const auto* source = memory.host_pointer(c.gpr[4], shadow::math::kAffineMatrixBytes);
    if (!source || !memory.write_in_place(c.gpr[3], shadow::math::kAffineMatrixBytes,
            [&](void* destination) { shadow::math::copy_affine_matrix(destination, source); }))
        c.request_stop(StopReason::ExecutionFault, "matrix copy requires two complete spans");
}

// Same ordering as the generic emitted call; a cut must not change LR or data.
// This belongs to the same disposable boundary, not the native value algorithm.
GEKKO_NOINLINE bool call_matrix_copy(Cpu& c, u32 site) {
    if (c.lift_site_cut(site)) return true;
    c.lr = site + 4u;
    copy_matrix_boundary(c);
    c.pc = site + 4u;
    return c.exiting() && c.finish_transfers();
}

)CPP";
        }
        if (std::any_of(g_lift_sites.begin(), g_lift_sites.end(), [](const auto& entry) {
                return entry.second.kind == LiftKind::AffineMatrixCompose;
            })) {
            table << R"CPP(// SCAFFOLDING (R2): removed when the remaining matrix callers own native values.
// One marshalling boundary for every site; numerical work belongs to math/matrix.
static GEKKO_NOINLINE void compose_matrix_boundary(Cpu& c) {
    auto& memory = c.guest_memory();
    constexpr std::size_t bytes = shadow::math::kAffineMatrixBytes;
    const auto* left = memory.host_pointer(c.gpr[3], bytes);
    const auto* right = memory.host_pointer(c.gpr[4], bytes);
    if (!left || !right || ((c.gpr[3] | c.gpr[4]) & 3u)) {
        c.request_stop(StopReason::ExecutionFault, "matrix composition requires complete aligned float spans");
        return;
    }
    // Reconstruct overlap using resolved storage, including address aliases.
    const auto l = reinterpret_cast<std::uintptr_t>(left);
    const auto r = reinterpret_cast<std::uintptr_t>(right);
    unsigned li = 0, ri = 12;
    if (l >= r && l - r < bytes) { li = unsigned((l - r) / 4); ri = 0; }
    else if (r > l && r - l < bytes) ri = unsigned((r - l) / 4);
    float values[24];
    for (unsigned i = 0; i < 12; ++i) {
        const u32 a = load_be32(left + i * 4), b = load_be32(right + i * 4);
        std::memcpy(values + li + i, &a, 4);
        std::memcpy(values + ri + i, &b, 4);
    }
    shadow::math::compose_affine_matrix(values + li, values + ri,
        (c.fpscr & 4u) ? shadow::math::SubnormalMode::Flush : shadow::math::SubnormalMode::Preserve);
    if (!memory.write_in_place(c.gpr[3], bytes, [&](void* output) {
        for (unsigned i = 0; i < 12; ++i) {
            u32 word;
            std::memcpy(&word, values + li + i, 4);
            store_be32(static_cast<u8*>(output) + i * 4, word);
        }
    })) c.request_stop(StopReason::ExecutionFault, "matrix composition destination became unavailable");
}

// One disposable boundary for every generated composition caller.
GEKKO_NOINLINE bool call_matrix_compose(Cpu& c, u32 site) {
    if (c.lift_site_cut(site)) return true;
    c.lr = site + 4u;
    compose_matrix_boundary(c);
    c.pc = site + 4u;
    return c.exiting() && c.finish_transfers();
}

)CPP";
        }
        if (std::any_of(g_lift_sites.begin(), g_lift_sites.end(), [](const auto& entry) {
                return entry.second.kind == LiftKind::TypeQuery;
            })) {
            table << R"CPP(// SCAFFOLDING (R2): translated type-query callers and exception transfer,
// deleted by native game-object ownership and native exception callers.
GEKKO_NOINLINE bool call_type_query(Cpu& c, u32 site, u16 query, u32 target) {
    if (c.lift_site_cut(site)) return true;
    c.lr = site + 4u;
    c.pc = site;
    const auto outcome = type_query_boundary::prepare(c,
        static_cast<shadow::object::game_type_queries::Query>(query), target);
    if (outcome == type_query_boundary::Outcome::Fault) return true;
    if (outcome == type_query_boundary::Outcome::Throw) {
        recomp::call_indirect(c, c.pc, c.lr);
        if (c.exiting() && c.finish_transfers()) return true;
        if (c.pc != site + 4u) {
            recomp::resume_elsewhere(c, site);
            return true;
        }
    }
    return c.exiting() && c.finish_transfers();
}

)CPP";
        }
        table << "const RecompiledCode::Entry kFunctions[] = {\n";
        for (const Candidate& candidate : accepted) {
            std::string name = candidate.record.name;
            for (char& character : name) {
                if (character == '"' || character == '\\') {
                    character = '_';
                }
            }
            table << "    {" << hexu(candidate.record.address) << ", "
                  << hexu(candidate.record.size) << ", &" << fn_symbol(candidate.record.address)
                  << ", \"" << name << "\"},\n";
            if (candidate.native_owner) {
                for (const u32 address : candidate.owner_entries) {
                    table << "    {" << hexu(address) << ", 4u, &" << entry_symbol(candidate.record.address, address)
                          << ", \"" << name << "\"},\n";
                    ++entry_points;
                }
                continue;
            }
            for (const u32 address : candidate.entries) {
                if (address == candidate.record.address) continue;
                table << "    {" << hexu(address) << ", 4u, &"
                      << entry_symbol(candidate.record.address, address) << ", \"" << name
                      << "\"},\n";
                ++entry_points;
            }
        }
        if (accepted.empty()) {
            table << "    {0u, 0u, nullptr, nullptr},\n";
        }
        table << "};\n";
        table << "const std::size_t kFunctionCount = " << (accepted.size() + entry_points)
              << ";\n\n";
        // Address ranges, sorted, so the dispatcher can enter a translated
        // function at an instruction the guest computed rather than interpret.
        // One entry per contiguous run of *reachable* instructions, so every
        // address a range covers really does have a dispatch case. An
        // over-approximation would let the switch miss and fall through to the
        // function's first instruction, which is not what the guest asked for.
        struct EmittedRange {
            u32 begin;
            u32 end;
            std::string symbol;
        };
        std::vector<EmittedRange> ranges;
        for (const Candidate& candidate : accepted) {
            if (candidate.native_owner || g_empty_variadic_leaves.count(candidate.record.address)) continue;
            const std::string symbol = fn_symbol(candidate.record.address) + "_at";
            u32 run_begin = 0;
            u32 run_end = 0;
            for (const u32 address : candidate.analysis.reachable) {
                if (run_end != 0u && address == run_end) {
                    run_end = address + 4u;
                    continue;
                }
                if (run_end != 0u) ranges.push_back({run_begin, run_end, symbol});
                run_begin = address;
                run_end = address + 4u;
            }
            if (run_end != 0u) ranges.push_back({run_begin, run_end, symbol});
        }
        std::sort(ranges.begin(), ranges.end(),
                  [](const EmittedRange& a, const EmittedRange& b) { return a.begin < b.begin; });
        // The function table contains overlapping records, so two translations
        // can claim the same bytes. Give the overlap to the lower address; both
        // translate the same instructions, so which one wins is immaterial as
        // long as the choice is deterministic and the table stays searchable.
        std::vector<EmittedRange> disjoint;
        for (EmittedRange range : ranges) {
            if (!disjoint.empty() && range.begin < disjoint.back().end) {
                range.begin = disjoint.back().end;
            }
            if (range.begin >= range.end) continue;
            disjoint.push_back(range);
        }
        table << "const RecompiledCode::Range kRanges[] = {\n";
        for (const EmittedRange& range : disjoint) {
            table << "    {" << hexu(range.begin) << ", " << hexu(range.end) << ", &"
                  << range.symbol << "},\n";
        }
        if (disjoint.empty()) {
            table << "    {0u, 0u, nullptr},\n";
        }
        table << "};\n";
        table << "const std::size_t kRangeCount = " << disjoint.size() << ";\n\n";
        // The native owners the generated code depends on: startup refuses to
        // run recompiled code while one of them is not installed.
        table << "const u32 kNativeOwners[] = {";
        std::size_t owner_count = 0;
        for (const Candidate& candidate : accepted) {
            if (!candidate.native_owner) continue;
            table << (owner_count == 0u ? "" : ", ") << hexu(candidate.record.address);
            ++owner_count;
            for (const u32 address : candidate.owner_entries) {
                table << ", " << hexu(address);
                ++owner_count;
            }
        }
        if (owner_count == 0u) table << "0u";
        table << "};\n";
        table << "const std::size_t kNativeOwnerCount = " << owner_count << ";\n\n";
        // Every listed GX pipe site, sorted, for the interpreter: a store it
        // executes at one of these words means what the generated code calls.
        table << "const GxSiteRecord kGxSites[] = {\n";
        for (const auto& listed : g_gx_sites) {
            table << "    {" << hexu(listed.first) << ", " << hexu(listed.second.word) << ", GxSiteFamily::"
                  << gx_family_enumerator(listed.second.family) << "},\n";
        }
        if (g_gx_sites.empty()) {
            table << "    {0u, 0u, GxSiteFamily::Dead},\n";
        }
        table << "};\n";
        table << "const std::size_t kGxSiteCount = " << g_gx_sites.size() << ";\n\n";
        // Every listed platform site, sorted, for the interpreter: an lhz / sth
        // it executes at one of these words means what the generated code
        // calls (or, dead, nothing).
        table << "const PlatformSiteRecord kPlatformSites[] = {\n";
        for (const auto& listed : g_platform_sites) {
            table << "    {" << hexu(listed.first) << ", " << hexu(listed.second.word) << ", "
                  << (listed.second.kind == kPlatformKindDead ? "true" : "false") << "},\n";
        }
        if (g_platform_sites.empty()) {
            table << "    {0u, 0u, false},\n";
        }
        table << "};\n";
        table << "const std::size_t kPlatformSiteCount = " << g_platform_sites.size() << ";\n\n";
        // The lowered call sites (--lift-sites): one generated function each,
        // and the sorted table the dispatcher and the interpreter use.
        for (const auto& listed : g_lift_sites) {
            table << "void " << lift_site_symbol(listed.first) << "(Cpu& c) {\n"
                  << lift_site_body(listed.first, listed.second) << "}\n\n";
        }
        for (const auto& leaf : g_empty_variadic_leaves) {
            table << "// SCAFFOLDING (R2): remaining caller register/return ABI; deleted with that caller.\n"
                  << "void " << lift_site_symbol(leaf.first) << "(Cpu& c) {\n";
            if (leaf.second == EmptyVariadicKind::Zero) table << "    c.gpr[3] = 0u;\n";
            if (leaf.second == EmptyVariadicKind::VaList) {
                table << "    c.gpr[11] = c.gpr[1] + 8u;\n"
                         "    c.gpr[0] = c.gpr[1] - 120u;\n"
                         "    c.gpr[12] = 0x1000000u;\n";
            }
            table << "    c.pc = c.lr & ~3u;\n}\n\n";
        }
        std::map<u32, u32> lift_records;
        for (const auto& listed : g_lift_sites) lift_records.emplace(listed.first, listed.second.word);
        for (const auto& leaf : g_empty_variadic_leaves) lift_records.emplace(leaf.first, image.read32(leaf.first));
        table << "const LiftSiteRecord kLiftSites[] = {\n";
        for (const auto& listed : lift_records) {
            const auto call = g_lift_sites.find(listed.first);
            const bool prepared = g_empty_variadic_leaves.count(listed.first) ||
                (call != g_lift_sites.end() && call->second.kind == LiftKind::TypeQuery);
            table << "    {" << hexu(listed.first) << ", LiftSiteContinuation::"
                  << (prepared ? "Prepared" : "Sequential") << ", &" << lift_site_symbol(listed.first) << "},\n";
        }
        if (lift_records.empty()) {
            table << "    {0u, LiftSiteContinuation::Sequential, nullptr},\n";
        }
        table << "};\n";
        table << "const std::size_t kLiftSiteCount = " << lift_records.size() << ";\n\n";
        table << "#if !defined(GEKKO_SHIPPING)\nconst u32 kLiftSiteWords[] = {\n";
        for (const auto& listed : lift_records) table << "    " << hexu(listed.second) << ",\n";
        if (lift_records.empty()) table << "    0u,\n";
        table << "};\n#endif\n\n";
        std::fprintf(stderr, "dispatch ranges: %zu contiguous translated runs\n", disjoint.size());
        table << "}  // namespace recomp\n}  // namespace gekko\n";
        if (!write_if_changed(out + "/recomp_table.cpp", table.str(), output_stats)) return 1;
    }

    // ---- report ----
    std::ostringstream report;
    report << "ppcrecomp\n";
    report << "  functions in table   : " << functions.size() << "\n";
    if (!g_empty_variadic_leaves.empty())
        report << "  empty variadic leaves: " << g_empty_variadic_leaves.size() << " bodies removed, one boundary per leaf\n";
    if (!options.prune_roots.empty()) {
        report << "  pruned unreachable   : " << pruned_rows << " rows, " << pruned_instructions
               << " instructions\n";
    }
    if (native_owner_rows != 0u) {
        report << "  native owners        : " << native_owner_rows << " rows (thunks only)\n";
    }
    if (!g_lift_sites.empty() || !g_retired_roots.empty()) {
        report << "  lowered call sites   : " << g_lift_sites_emitted.size() << " of " << g_lift_sites.size()
               << " emitted; retired roots: " << g_retired_roots.size() << "\n";
    }
    if (!g_wait_points.empty()) {
        std::size_t poll_emitted = 0;
        for (const u32 site : g_wait_points_emitted) poll_emitted += g_poll_points.count(site);
        report << "  wait points          : " << g_wait_points_emitted.size() << " of "
               << g_wait_points.size() << " listed back edges emitted (" << poll_emitted
               << " fixed-point checks)\n";
    }
    if (!g_platform_sites.empty()) {
        report << "  platform sites       : " << g_platform_sites_emitted.size() << " of "
               << g_platform_sites.size() << " listed sites emitted (" << g_platform_site_emissions
               << " emissions; " << g_platform_site_dead_emissions << " dead stores emit nothing)\n";
    }
    if (!g_gx_sites.empty()) {
        report << "  GX pipe sites        : " << g_gx_sites_emitted.size() << " of " << g_gx_sites.size()
               << " listed sites lowered (" << g_gx_site_emissions << " emissions; " << gx_sites_not_emitted
               << " in pruned or owned rows)\n";
    }
    if (!filter.empty()) {
        report << "  filtered out         : " << skipped_filtered << "\n";
    }
    report << "  recompiled           : " << accepted.size() << "\n";
    report << "  extra entry points   : " << entry_points << "\n";
    std::size_t instructions = 0;
    for (const Candidate& candidate : accepted) {
        instructions += candidate.analysis.reachable.size();
    }
    report << "  guest instructions   : " << instructions << "\n";
    report << "  ir instructions      : " << g_ir_insns << "\n";
    report << "  hot-label insns      : " << g_hot_label_insns << "\n";
    report << "  cold mid-block insns : " << g_cold_mid_insns << "\n";
    report << "  hot src bytes        : " << g_hot_src_bytes << "\n";
    if (!g_hot_set.empty()) {
        report << "  hot-set rows         : " << g_hot_set_rows << " of " << g_hot_set.size()
               << " listed (" << g_hot_set_sites << " accessor sites inlined)\n";
    }
    report << "  cold mid src bytes   : " << g_cold_src_bytes << "\n";
    report << "  fp checks emitted    : " << g_fp_checks_emitted << "\n";
    report << "  fp checks hoisted    : " << g_fp_checks_hoisted << "\n";
    report << "  rejected             : " << (functions.size() - accepted.size() - skipped_filtered - pruned_rows)
           << "\n";
    std::vector<std::pair<std::string, u32>> sorted(rejections.begin(), rejections.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });
    for (const auto& entry : sorted) {
        report << "      " << entry.second << "  " << entry.first << "\n";
    }
    if (!rejected_functions.empty()) {
        report << "  rejected function entries (may overlap an accepted translation):\n";
        for (const auto& entry : rejected_functions) {
            report << "    " << hex32(entry.first) << "  " << entry.second << "\n";
        }
    }
    std::fputs(report.str().c_str(), stdout);
    if (!options.report.empty()) {
        if (!write_if_changed(options.report, report.str(), output_stats)) return 1;
    }
    if (!write_if_changed(out + "/recomp_layout.csv", layout.text, output_stats)) return 1;
    std::fprintf(stderr, "generated outputs: %zu written, %zu unchanged\n",
                 output_stats.written, output_stats.unchanged);
    return 0;
}
