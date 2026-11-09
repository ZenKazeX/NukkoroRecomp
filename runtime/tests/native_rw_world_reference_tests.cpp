// The world-sector pipeline owners (gekko/native_rw_world.h; gx-rw-plan cluster rwR4) against the original words.
//
// Reference interpreter, pinned DOL. 0x804651D8 and 0x80465210 are run twice from byte-identical memory - the original
// body, then the owner - and every MEM1 byte outside the call's stack window plus r3 must agree. The 18 stop owners must
// stop the machine naming their address. The frontier file pins the entry set: exactly the 22 rwR4 rows, the 18 stops,
// the two served exactly and the two documented exclusions (0x80465818, 0x804662B0), with the census hits the choice
// rests on (0 for every stop, >0 for the exclusions). A negative control proves the comparison sees a flipped byte.
//
// usage: native_rw_world_reference_tests <main.dol> <test_data/platform/gx_rw_frontier.csv>
// liveness-roots: reference-interpreter-only
// (Every address below is run as original words in the reference interpreter or named as a hook on that interpreter's Cpu;
// none is entered as translated code, so none may root a guest body in the shipping image.)

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_rw_node_stops.h"
#include "gekko/native_rw_world.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using gekko::u32;
using gekko::u8;

constexpr u32 kSda = 0x805EC500u;
constexpr u32 kSda2 = 0x805FA780u;
constexpr u32 kStackTop = 0x817E0000u;
constexpr u32 kStackWindow = 0x8000u;
constexpr u32 kFinish = 0x817FF000u;
constexpr u32 kMemoryBegin = gekko::GuestMemory::kImageBase;
constexpr u32 kMemoryEnd = gekko::GuestMemory::kLegacyHeapEnd;
constexpr u32 kMemorySize = kMemoryEnd - kMemoryBegin;

constexpr u32 kPipelineInit = 0x804651D8u;
constexpr u32 kAccessor = 0x80465210u;
constexpr u32 kPluginAttach = 0x80465818u;
constexpr u32 kSectorReader = 0x804662B0u;

int g_cases = 0, g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_cases;
    if (ok) return;
    ++g_failures;
    if (g_failures <= 40) std::fprintf(stderr, "MISMATCH %s\n", what.c_str());
}

std::string hex(u32 value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", value);
    return text;
}

std::vector<u8> read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}

struct Machine {
    gekko::GuestMemory bus;
    gekko::Cpu cpu{bus};

    explicit Machine(const std::vector<u8>& dol) {
        const auto word = [&](std::size_t offset) { return gekko::load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            if (static_cast<std::size_t>(offset) + size > dol.size()) throw std::runtime_error("invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        bus.write32(kFinish, 0x48000000u);
        cpu.clear_stop();
        cpu.reset();
        cpu.gpr[1] = kStackTop;
        cpu.gpr[2] = kSda2;
        cpu.gpr[13] = kSda;
    }
    // Runs `entry` until it returns to the sentinel or the machine stops; true if it returned.
    bool call(u32 entry, u32 r3 = 0u, u32 r4 = 0u) {
        cpu.gpr[3] = r3;
        cpu.gpr[4] = r4;
        cpu.pc = entry;
        cpu.lr = kFinish;
        unsigned steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 1000000u) cpu.run_steps(1u);
        return cpu.pc == kFinish && !cpu.stopped();
    }
};

// Differing MEM1 bytes outside the stack window.
u32 differences(Machine& a, Machine& b) {
    const std::vector<u8> x = a.bus.window_bytes(kMemoryBegin, kMemoryEnd);
    const std::vector<u8> y = b.bus.window_bytes(kMemoryBegin, kMemoryEnd);
    const u32 low = kStackTop - kStackWindow - kMemoryBegin, high = kStackTop + 8u - kMemoryBegin;
    u32 count = 0;
    for (u32 offset = 0; offset < kMemorySize; ++offset)
        if (x[offset] != y[offset] && !(offset >= low && offset < high)) ++count;
    return count;
}

// One exact-service case. Returns the number of differing bytes (the negative control reads it).
u32 served_case(const std::vector<u8>& dol, const std::string& label, u32 entry, u32 object, u32 block, u32 fill,
                bool perturb = false) {
    const auto setup = [&](Machine& m) {
        for (u32 off = 0; off < 0x40u; off += 4u) m.cpu.mem_write32(block + off, fill ^ off);
        m.cpu.mem_write32(object + 0x14u, block);
    };
    Machine original(dol);
    setup(original);
    check(original.call(entry, object), label + ": the original returns");
    Machine native(dol);
    setup(native);
    gekko::install_native_rw_world_owners(native.cpu);
    check(native.call(entry, object), label + ": the owner returns");
    if (perturb) native.cpu.mem_write8(block + 5u, static_cast<u8>(native.bus.peek8(block + 5u) ^ 0x10u));
    const u32 d = differences(original, native);
    if (!perturb) {
        check(d == 0u, label + ": " + std::to_string(d) + " differing memory bytes");
        check(original.cpu.gpr[3] == native.cpu.gpr[3],
              label + ": r3 original " + hex(original.cpu.gpr[3]) + " native " + hex(native.cpu.gpr[3]));
    }
    return d;
}

// ---- rwR5: the registry's self-contained rows, original vs owner over generated state ---------------------------------
constexpr u32 kErrorFormat = 0x8047EC78u;  // what the original calls on an exhausted arena (stubbed on both machines)
constexpr u32 kErrorReport = 0x8047EBD4u;

struct Rng {
    u32 s = 0x2545F491u;
    u32 next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
};

// Runs `entry` on a fresh original and a fresh owner machine after `setup`, compares every MEM1 byte outside the stack
// window and r3 (when `compare_r3`). Returns the number of differing bytes.
template <typename Setup>
u32 registry_case(const std::vector<u8>& dol, const std::string& label, u32 entry, u32 r3, bool compare_r3, Setup setup,
                  u32 r4 = 0u) {
    Machine original(dol), native(dol);
    for (Machine* m : {&original, &native}) {
        m->cpu.install_hle(kErrorFormat, [](gekko::Cpu& c) { c.gpr[3] = 0x81000000u; return true; }, "stub");
        m->cpu.install_hle(kErrorReport, [](gekko::Cpu&) { return true; }, "stub");
    }
    gekko::install_native_rw_world_owners(native.cpu);
    // the stubs above were replaced by the owners only for the owned entries; the error stubs stay on the native machine
    native.cpu.install_hle(kErrorFormat, [](gekko::Cpu& c) { c.gpr[3] = 0x81000000u; return true; }, "stub");
    native.cpu.install_hle(kErrorReport, [](gekko::Cpu&) { return true; }, "stub");
    setup(original);
    setup(native);
    const bool o = original.call(entry, r3, r4);
    const bool n = native.call(entry, r3, r4);
    check(o, label + ": the original returns");
    if (!n) return 0xFFFFFFFFu;  // the owner stopped (the error paths)
    const u32 d = differences(original, native);
    check(d == 0u, label + ": " + std::to_string(d) + " differing memory bytes");
    if (compare_r3)
        check(original.cpu.gpr[3] == native.cpu.gpr[3],
              label + ": r3 original " + hex(original.cpu.gpr[3]) + " native " + hex(native.cpu.gpr[3]));
    return d;
}

void registry_cases(const std::vector<u8>& dol) {
    constexpr u32 kGlobals = 0x81300000u, kOffset = 0x40u;
    // 0x8049BCE0: the plugin-globals slot, set / restored; the slot's saved value zero or not.
    for (const u32 arg : {0u, 0x81234560u})
        for (const u32 saved : {0u, 0x81222220u})
            for (const u32 previous : {0u, 0x81999990u}) {
                registry_case(dol, "slot setter arg " + hex(arg) + " saved " + hex(saved) + " previous " + hex(previous),
                              0x8049BCE0u, arg, true, [&](Machine& m) {
                                  m.cpu.mem_write32(kSda + 0x615Cu, kGlobals);
                                  m.cpu.mem_write32(kSda + 0x622Cu, kOffset);
                                  m.cpu.mem_write32(kGlobals + kOffset, previous);
                                  m.cpu.mem_write32(kGlobals + kOffset + 0x1Cu, saved);
                              });
            }
    // 0x8049BD30: push / pop of the slot of every kind (including the invalid ones, where both raise the error).
    for (const u32 kind : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 0xFFFFFFFFu})
        for (const u32 value : {0u, 0x81234560u})
            for (const u32 saved : {0u, 0x81222220u}) {
                const std::string tag = "slot stack kind " + hex(kind) + " value " + hex(value) + " saved " + hex(saved);
                const bool valid = kind >= 1u && kind <= 6u;
                const u32 d = registry_case(dol, tag, 0x8049BD30u, value, valid, [&](Machine& m) {
                    m.cpu.mem_write32(kSda + 0x615Cu, kGlobals);
                    m.cpu.mem_write32(kSda + 0x622Cu, kOffset);
                    for (u32 off = 0; off < 0x40u; off += 4u) m.cpu.mem_write32(kGlobals + kOffset + off, 0x70000000u + off);
                    for (u32 off = 0x20u; off < 0x38u; off += 4u) m.cpu.mem_write32(kGlobals + kOffset + off, saved != 0u ? saved + off : 0u);  // distinct per slot
                }, kind);
                if (!valid) check(d == 0xFFFFFFFFu, tag + ": the owner stops on an invalid kind");
            }
    // 0x8049CC9C: a node record registered in a table; limits fit, sit at the edge, and are exceeded (errors).
    for (const u32 count : {0u, 1u, 5u, 0x20u, 0x21u})
        for (const u32 keys : {0u, 7u, 0x21u})
            for (const u32 limit : {0x10u, 0x21u, 0x40u}) {
                const std::string tag = "node register count " + hex(count) + " keys " + hex(keys) + " limit " + hex(limit);
                const bool fits = count <= 0x20u && keys <= 0x20u && count < limit;
                const auto setup = [&](Machine& m) {
                    m.cpu.mem_write32(kSda + 0x615Cu, kGlobals);
                    m.cpu.mem_write32(kSda + 0x6238u, 0x100u);
                    m.cpu.mem_write32(kGlobals + 0x100u + 0x38u, limit);
                    m.cpu.gpr[5] = 0x81620000u;                   // the definition
                    m.cpu.mem_write32(0x81600004u, 2u);           // slots already used
                    m.cpu.mem_write32(0x81600008u, 0x81610000u);  // storage
                    m.cpu.mem_write32(0x8162002Cu, count);
                    m.cpu.mem_write32(0x81620020u, keys);
                    for (u32 off = 0; off < 0x40u; off += 4u) m.cpu.mem_write32(0x81630000u + off, 0xABCD0000u + off);
                };
                const u32 d = registry_case(dol, tag, 0x8049CC9Cu, 0x81600000u, fits, setup, 0x81630000u);
                if (!fits) check(d == 0xFFFFFFFFu, tag + ": the owner stops on a limit error");
            }
    // 0x8049C190.
    registry_case(dol, "TRUE return", 0x8049C190u, 0x55u, true, [](Machine&) {});
    // 0x8049C9F4: distinct keys of a generated table (duplicates, zero, 0xFFFFFFFF, unsorted).
    Rng rng;
    for (int round = 0; round < 24; ++round) {
        const u32 count = 1u + rng.next() % 4u;
        registry_case(dol, "distinct count round " + std::to_string(round), 0x8049C9F4u, 0x81400000u, true, [&](Machine& m) {
            Rng r = rng;  // the same state on both machines
            m.cpu.mem_write32(0x81400004u, count);
            m.cpu.mem_write32(0x81400008u, 0x81400100u);
            for (u32 i = 0; i < count; ++i) {
                const u32 entry = 0x81401000u + 0x100u * i, keys = 0x81402000u + 0x100u * i;
                m.cpu.mem_write32(0x81400100u + 0x28u * i, entry);
                const u32 nkeys = r.next() % 6u;
                m.cpu.mem_write32(entry + 0x20u, nkeys);
                m.cpu.mem_write32(entry + 0x24u, keys);
                for (u32 k = 0; k < nkeys; ++k) {
                    const u32 pick = r.next() % 8u;
                    const u32 key = pick == 0u ? 0u : pick == 1u ? 0xFFFFFFFFu : pick == 2u ? 0x80000000u : r.next() % 5u;
                    m.cpu.mem_write32(keys + 12u * k, key);
                }
            }
        });
        rng.next();  // advance the generator for the next round
    }
    // 0x8049C90C / 0x8049C97C: the static arena; sizes that fit, exactly fill, and overflow (the error path).
    for (const u32 size : {1u, 4u, 5u, 0x100u, 0xFFu, 0xFFFCu}) {
        for (const u32 span : {0x10000u, 0x104u, 0x100u, 0x80u}) {
            const auto arena = [&](Machine& m) {
                m.cpu.mem_write32(kSda + 0x2B08u, 0x81500000u + span);  // top
                m.cpu.mem_write32(kSda + 0x2B0Cu, 0x81500000u);         // floor
            };
            const std::string tag = " size " + hex(size) + " span " + hex(span);
            const bool fits = ((size + 3u) & ~3u) <= span;
            const u32 top = registry_case(dol, "arena top" + tag, 0x8049C90Cu, size, fits, arena);
            const u32 floor = registry_case(dol, "arena floor" + tag, 0x8049C97Cu, size, fits, arena);
            if (!fits) {
                check(top == 0xFFFFFFFFu && floor == 0xFFFFFFFFu, "arena exhausted" + tag + ": the owners stop");
                // the arena words after the owner's stop equal the original's after its error path
                for (const u32 entry : {0x8049C90Cu, 0x8049C97Cu}) {
                    Machine original(dol), native(dol);
                    for (Machine* m : {&original, &native}) {
                        m->cpu.install_hle(kErrorFormat, [](gekko::Cpu& c) { c.gpr[3] = 0x81000000u; return true; }, "stub");
                        m->cpu.install_hle(kErrorReport, [](gekko::Cpu&) { return true; }, "stub");
                    }
                    gekko::install_native_rw_world_owners(native.cpu);
                    arena(original);
                    arena(native);
                    original.call(entry, size);
                    native.call(entry, size);
                    check(native.cpu.stopped(), "arena exhausted" + tag + ": the owner stops the machine");
                    check(original.bus.peek32(kSda + 0x2B08u) == native.bus.peek32(kSda + 0x2B08u) &&
                              original.bus.peek32(kSda + 0x2B0Cu) == native.bus.peek32(kSda + 0x2B0Cu) &&
                              original.cpu.gpr[3] == 0u,
                          "arena exhausted" + tag + ": the arena words after the error path");
                }
            }
        }
    }
}

// ---- rwR3: the Im3D entry points, the original bodies against the owners with the pipeline execution observed ----------
struct ExecuteCall {
    u32 pipeline, data, option;
    std::vector<u32> record;  // the execution record / vertex block the pipeline reads (0x40 bytes from `data`)
    bool operator==(const ExecuteCall& o) const {
        return pipeline == o.pipeline && data == o.data && option == o.option && record == o.record;
    }
};
std::vector<ExecuteCall> g_native_calls;
u32 g_execute_result = 0;

u32 native_execute(gekko::Cpu& c, u32 pipeline, u32 data, u32 option) {
    ExecuteCall call{pipeline, data, option, {}};
    for (u32 off = 0; off < 0x40u; off += 4u) call.record.push_back(c.mem_read32(data + off));
    g_native_calls.push_back(call);
    return g_execute_result;
}

// Runs one Im3D entry on both machines. `native_stops` says the owner must stop (no case now: the original reports RwError through
// RwErrorSet and the owner does the same, carrying on exactly as the original does).
void im3d_case(const std::vector<u8>& dol, const std::string& label, u32 entry, u32 r3, u32 r4, u32 r5, u32 r6,
               u32 execute_result, bool native_stops, bool state_open) {
    constexpr u32 kGlobals = 0x81300000u, kOffset = 0x40u, kSlot = kGlobals + kOffset;
    const auto setup = [&](Machine& m) {
        m.cpu.mem_write32(kSda + 0x615Cu, kGlobals);
        m.cpu.mem_write32(kSda + 0x622Cu, kOffset);
        for (u32 off = 0; off < 0x100u; off += 4u) m.cpu.mem_write32(kSlot + off, 0x60000000u + off);
        for (u32 i = 0; i < 6u; ++i) m.cpu.mem_write32(kSlot + 4u * i, 0x81700000u + 0x100u * i);  // the pipelines
        m.cpu.mem_write32(kSlot + 0x3Cu, state_open ? 0x81800000u : 0u);
        m.cpu.gpr[5] = r5;
        m.cpu.gpr[6] = r6;
    };
    Machine original(dol), native(dol);
    std::vector<ExecuteCall> original_calls;
    original.cpu.install_hle(0x8049C6C4u, [&](gekko::Cpu& c) {
        ExecuteCall call{c.gpr[3], c.gpr[4], c.gpr[5], {}};
        for (u32 off = 0; off < 0x40u; off += 4u) call.record.push_back(c.mem_read32(c.gpr[4] + off));
        original_calls.push_back(call);
        c.gpr[3] = execute_result;
        return true;
    }, "stub RxPipelineExecute");
    // The error report: RwErrorSet is a counting stub on both machines; its record (r3 -> {1, code}) must be the same.
    std::vector<std::pair<u32, u32>> reports[2];
    {
        int index = 0;
        for (Machine* m : {&original, &native}) {
            auto* log = &reports[index++];
            m->cpu.install_hle(kErrorFormat, [](gekko::Cpu& c) { c.gpr[3] = c.gpr[3] == 0x23u || c.gpr[3] == 0x25u || c.gpr[3] == 0x32u ? c.gpr[3] : 0x81000000u; return true; }, "stub");
            m->cpu.install_hle(kErrorReport, [log](gekko::Cpu& c) { log->push_back({c.mem_read32(c.gpr[3]), c.mem_read32(c.gpr[3] + 4u)}); return true; }, "stub");
        }
    }
    g_native_calls.clear();
    g_execute_result = execute_result;
    gekko::install_native_rw_im3d_owners(native.cpu, native_execute, native_execute);
    setup(original);
    setup(native);
    original.call(entry, r3, r4);
    const bool returned = native.call(entry, r3, r4);
    if (native_stops) {
        check(!returned && native.cpu.stopped(), label + ": the owner stops where the original raises RwError");
        return;
    }
    check(returned, label + ": the owner returns");
    check(original.cpu.gpr[3] == native.cpu.gpr[3], label + ": r3 original " + hex(original.cpu.gpr[3]) + " native " + hex(native.cpu.gpr[3]));
    check(reports[0] == reports[1], label + ": the RwErrorSet records (" + std::to_string(reports[0].size()) + " original, " +
                                        std::to_string(reports[1].size()) + " native)");
    check(original_calls == g_native_calls, label + ": the pipeline execution calls (" + std::to_string(original_calls.size()) + " original, " +
                                                std::to_string(g_native_calls.size()) + " native)");
    const u32 d = differences(original, native);
    check(d == 0u, label + ": " + std::to_string(d) + " differing memory bytes");
}

void im3d_cases(const std::vector<u8>& dol) {
    for (const u32 result : {0u, 1u, 0x81234560u}) {
        for (const u32 count : {0u, 1u, 7u, 0xFFFFu, 0x10000u})
            im3d_case(dol, "Im3DTransform count " + hex(count) + " result " + hex(result), 0x8049B87Cu, 0x81A00000u, count, 0x81B00000u,
                      0x12u, result, false, false);
        im3d_case(dol, "Im3DTransform count over the limit", 0x8049B87Cu, 0x81A00000u, 0x10001u, 0x81B00000u, 0x12u, result, false, false);
        for (const bool open : {false, true})
            im3d_case(dol, std::string("Im3DEnd ") + (open ? "open" : "closed"), 0x8049B95Cu, 0u, 0u, 0u, 0u, result, false, open);
        for (const u32 type : {1u, 2u, 3u, 4u, 5u})
            im3d_case(dol, "Im3DRenderPrimitive type " + hex(type) + " result " + hex(result), 0x8049BB48u, type, 0u, 0u, 0u, result, false, true);
        for (const u32 type : {0u, 6u, 7u, 0xFFFFFFFFu})
            im3d_case(dol, "Im3DRenderPrimitive invalid type " + hex(type), 0x8049BB48u, type, 0u, 0u, 0u, result, false, true);
        im3d_case(dol, "Im3DRenderPrimitive without transform", 0x8049BB48u, 3u, 0u, 0u, 0u, result, false, false);
        for (const u32 type : {1u, 2u, 3u, 4u, 5u})
            for (const u32 count : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 100u, 0xFFFFFFFDu /* -3 */, 0xFFFFFFFFu /* -1 */, 0x7FFFFFFFu})
                im3d_case(dol, "Im3DRenderIndexed type " + hex(type) + " count " + hex(count) + " result " + hex(result), 0x8049B9ACu, type,
                          0x81C00000u, count, 0u, result, false, true);
        for (const u32 type : {0u, 6u, 0xFFFFFFFFu})
            im3d_case(dol, "Im3DRenderIndexed invalid type " + hex(type), 0x8049B9ACu, type, 0x81C00000u, 9u, 0u, result, false, true);
        im3d_case(dol, "Im3DRenderIndexed without transform", 0x8049B9ACu, 3u, 0x81C00000u, 9u, 0u, result, false, false);
    }
}

std::set<u32> frontier_rows(const char* path, const char* cluster, std::vector<std::pair<u32, long>>* hits) {
    std::set<u32> rows;
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::string field;
        bool quoted = false;
        for (const char ch : line) {
            if (ch == '"') quoted = !quoted;
            else if (ch == ',' && !quoted) { f.push_back(field); field.clear(); }
            else field += ch;
        }
        f.push_back(field);
        if (f.size() < 8u || f[2] != cluster) continue;
        const u32 address = static_cast<u32>(std::strtoul(f[0].c_str(), nullptr, 16));
        rows.insert(address);
        if (hits != nullptr) hits->push_back({address, std::strtol(f[7].c_str(), nullptr, 10)});
    }
    return rows;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3) throw std::runtime_error("usage: native_rw_world_reference_tests <main.dol> <gx_rw_frontier.csv>");
        const std::vector<u8> dol = read_file(argv[1]);
        if (dol.size() < 256u) throw std::runtime_error("missing original DOL");

        // 0x804651D8 over several objects / blocks / previous contents.
        for (const u32 fill : {0x00000000u, 0xAAAAAAAAu, 0xDEADBEEFu}) {
            served_case(dol, "PipelineInit fill " + hex(fill), kPipelineInit, 0x81100000u, 0x81100100u, fill);
            served_case(dol, "PipelineInit other block " + hex(fill), kPipelineInit, 0x81200010u, 0x81234560u, fill);
        }
        // 0x80465210.
        {
            Machine original(dol), native(dol);
            check(original.call(kAccessor), "accessor: the original returns");
            gekko::install_native_rw_world_owners(native.cpu);
            check(native.call(kAccessor), "accessor: the owner returns");
            check(original.cpu.gpr[3] == native.cpu.gpr[3] && original.cpu.gpr[3] == 0x8056E9F4u,
                  "accessor r3 original " + hex(original.cpu.gpr[3]) + " native " + hex(native.cpu.gpr[3]));
            check(differences(original, native) == 0u, "accessor: memory untouched");
        }
        // The structural twins: the empty hook leaves memory and r3 alone; the getter returns the small-data word.
        {
            Machine original(dol), native(dol);
            gekko::install_native_rw_world_owners(native.cpu);
            check(original.call(0x8049C198u, 0x1234u) && native.call(0x8049C198u, 0x1234u), "empty hook: both return");
            check(original.cpu.gpr[3] == 0x1234u && native.cpu.gpr[3] == 0x1234u && differences(original, native) == 0u,
                  "empty hook: r3 and memory untouched on both");
            for (const u32 value : {0u, 0x80123456u, 0xFFFFFFFFu}) {
                original.cpu.mem_write32(kSda + 0x6244u, value);
                native.cpu.mem_write32(kSda + 0x6244u, value);
                check(original.call(0x8049C6BCu) && native.call(0x8049C6BCu), "getter: both return");
                check(original.cpu.gpr[3] == value && native.cpu.gpr[3] == value && differences(original, native) == 0u,
                      "getter " + hex(value) + ": r3 original " + hex(original.cpu.gpr[3]) + " native " + hex(native.cpu.gpr[3]));
            }
        }
        registry_cases(dol);
        im3d_cases(dol);
        // Negative control: one flipped byte is exactly one difference.
        check(served_case(dol, "negative control", kPipelineInit, 0x81100000u, 0x81100100u, 0u, true) == 1u,
              "negative control: one perturbed byte is exactly one difference");

        // The 9 stop owners (the five native-data stream callbacks and the four decoders are exact owners: rw_world_stream_contract_tests,
        // rw_world_decode_tests).
        std::size_t count = 0;
        const gekko::RwWorldStopEntry* stops = gekko::rw_world_stop_entries(&count);
        check(count == 9u, "the stop entry count is " + std::to_string(count));
        for (std::size_t i = 0; i < count; ++i) {
            Machine native(dol);
            gekko::install_native_rw_world_owners(native.cpu);
            const bool returned = native.call(stops[i].address);
            const std::string label = "stop owner " + hex(stops[i].address);
            check(!returned && native.cpu.stopped(), label + ": stops the machine");
            check(native.cpu.stop().detail.find("0x" + hex(stops[i].address)) != std::string::npos,
                  label + ": the stop names the entry");
        }

        // Step K: the RenderWare entries nothing enters in the lifted build (rw_node_stops): each stops and names itself,
        // and a call of any other entry is not stopped (the owners are exactly this set).
        {
            std::size_t node_count = 0;
            const gekko::rw_node_stops::StopEntry* node = gekko::rw_node_stops::entries(node_count);
            check(node_count == 4u, "the node stop entry count is " + std::to_string(node_count));
            for (std::size_t i = 0; i < node_count; ++i) {
                Machine native(dol);
                gekko::rw_node_stops::install_lifted(native.cpu);
                const bool returned = native.call(node[i].address);
                const std::string label = "node stop owner " + hex(node[i].address);
                check(!returned && native.cpu.stopped(), label + ": stops the machine");
                check(native.cpu.stop().detail.find("0x" + hex(node[i].address)) != std::string::npos,
                      label + ": the stop names the entry");
            }
        }
        // The entry set and the census it rests on.
        std::vector<std::pair<u32, long>> hits;
        const std::set<u32> rows = frontier_rows(argv[2], "rwR4", &hits);
        std::set<u32> expected = {kPipelineInit, kAccessor, kPluginAttach, kSectorReader};
        // the stream callbacks of the native geometry / world sector and the vertex-format decoders: exact owners, still 0 hits
        const std::set<u32> stream_owners = {0x8046615Cu, 0x80466188u, 0x804661D0u, 0x804661FCu, 0x80466250u,
                                             0x8046798Cu, 0x804679F0u, 0x80467A3Cu, 0x80467AA4u};   // + the four decoders
        expected.insert(stream_owners.begin(), stream_owners.end());
        const std::set<u32> r3_stops = {};   // (the 0x8049A124 family became exact owners: native_rw_raster.cpp)
        const std::set<u32> r3_clear = {0x8049A124u, 0x8049A304u, 0x8049A364u, 0x8049A3B4u, 0x8049A41Cu, 0x8049A480u};
        for (std::size_t i = 0; i < count; ++i)
            if (r3_stops.count(stops[i].address) == 0u) expected.insert(stops[i].address);  // the rwR4 stops
        check(rows == expected, "the frontier's rwR4 rows are exactly the served, stopped and excluded entries (" +
                                    std::to_string(rows.size()) + " rows, " + std::to_string(expected.size()) + " expected)");
        std::set<u32> stop_set;
        for (std::size_t i = 0; i < count; ++i) stop_set.insert(stops[i].address);
        for (const auto& [address, h] : hits) {
            if (stop_set.count(address) != 0u) check(h == 0, "stop entry " + hex(address) + " has census hits " + std::to_string(h));
            if (stream_owners.count(address) != 0u) check(h == 0, "stream callback " + hex(address) + " has census hits " + std::to_string(h));
            if (address == kPluginAttach || address == kSectorReader)
                check(h > 0, "excluded entry " + hex(address) + " is executed (census hits " + std::to_string(h) + ")");
        }
        // rwR3: six stops (0 hits on three recordings) and the four Im3D entries served.
        {
            std::vector<std::pair<u32, long>> hits3;
            const std::set<u32> rows3 = frontier_rows(argv[2], "rwR3", &hits3);
            std::set<u32> expected3 = r3_clear;   // served by native_rw_raster.cpp now (census hits stay 0)
            const std::set<u32> im3d = {0x8049B87Cu, 0x8049B95Cu, 0x8049B9ACu, 0x8049BB48u};
            expected3.insert(im3d.begin(), im3d.end());
            check(rows3 == expected3, "the frontier's rwR3 rows are exactly the six clear entries and the four Im3D entries (" +
                                          std::to_string(rows3.size()) + " rows)");
            for (const auto& [address, h] : hits3) {
                if (r3_clear.count(address) != 0u) check(h == 0, "rwR3 clear entry " + hex(address) + " has census hits " + std::to_string(h));
                if (im3d.count(address) != 0u) check(h > 0, "rwR3 Im3D entry " + hex(address) + " is executed (hits " + std::to_string(h) + ")");
            }
        }
        // rwR5: the registry cluster's partition and the census it rests on.
        {
            std::vector<std::pair<u32, long>> hits5;
            const std::set<u32> rows5 = frontier_rows(argv[2], "rwR5", &hits5);
            const std::set<u32> served5 = {0x8049BCE0u, 0x8049C190u, 0x8049C198u, 0x8049C9F4u, 0x8049C90Cu, 0x8049C97Cu,
                                           0x8049BD30u, 0x8049CC9Cu,
                                           0x80491448u};  // the quantiser end: exact owner in native_rw_raster_image.cpp
            const std::set<u32> stops5 = {0x8049CA80u};
            // translated by design (they call the translated plugin registry / pipeline code through the engine's
            // function table, or are large registry bodies the rwcore reconstruction replaces)
            const std::set<u32> excluded5 = {0x80494EB4u, 0x8049BFE4u, 0x8049C048u, 0x8049C11Cu, 0x8049C160u,
                                             0x8049C3A0u, 0x8049C7BCu, 0x8049C850u, 0x8049CE40u};
            std::set<u32> all5 = served5;
            all5.insert(stops5.begin(), stops5.end());
            all5.insert(excluded5.begin(), excluded5.end());
            check(rows5 == all5, "the frontier's rwR5 rows are exactly the served, stopped and excluded entries (" +
                                     std::to_string(rows5.size()) + " rows, " + std::to_string(all5.size()) + " expected)");
            for (const auto& [address, h] : hits5) {
                if (stops5.count(address) != 0u) check(h == 0, "rwR5 stop entry " + hex(address) + " has census hits " + std::to_string(h));
                if (excluded5.count(address) != 0u) check(h > 0, "rwR5 excluded entry " + hex(address) + " is executed (hits " + std::to_string(h) + ")");
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
    std::printf("%d checks, %d failures\n", g_cases, g_failures);
    return g_failures == 0 ? 0 : 1;
}
