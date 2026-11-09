// liveness-roots: reference-interpreter-only
// Original-DOL contract for the native-data stream callbacks of the geometry and the world sector (gekko/native_rw_registry.h):
// size 0x8046615C / 0x804661FC, write 0x80466188 / 0x80466250 (with the chunk body 0x80465DFC), read 0x804661D0 (with
// 0x80465AE4). Each owner runs against the ORIGINAL body on byte-identical memory.
//
// Reference interpreter, pinned DOL. The RenderWare functions the bodies call are C++ doubles installed on BOTH machines:
// they log every argument they are given (the registers, the word a pointer argument points at, a hash of the bytes a
// byte-range argument covers AT THE TIME OF THE CALL, so a table that is relocated at the wrong moment shows), return
// scripted values and scribble over the words below r1 (a callee's own frame, so a local the owner keeps below its stack
// pointer is destroyed). Compared: r3, r1, r2, r13, r14..r31, lr, f14..f31, every byte of the engine / small-data window and of
// the test area (the objects and the log). DCFlushRange and GXInvalidateVtxCache, which the original also calls, are the
// silent owners on both machines (no cache, no vertex cache); the owner drops them.
//
// Negative controls: owners that do nothing, that relocate wrongly, and ORIGINAL code words patched (a chunk-header
// argument, the chunk's version, the length handed to the second block write) are all told apart by the same comparison.
//
// usage: rw_world_stream_contract_tests <sys/main.dol>
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_gx_state.h"
#include "gekko/native_guest_call.h"
#include "gekko/native_rw_registry.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;
namespace reg = gekko::rw_registry;

constexpr u32 kSda = 0x805EC500u, kSda2 = 0x805FA780u;
constexpr u32 kStack = 0x817E0000u, kFinish = 0x817FF000u;
constexpr u32 kDataLo = 0x80560000u, kDataHi = 0x80600000u;
constexpr u32 kTestLo = 0x81300000u, kTestHi = 0x81310000u;
constexpr u32 kLog = 0x81301000u;            // [0] entry count, entries of 0x40 bytes from +0x10
constexpr u32 kEngine = 0x805E3C08u;
constexpr u32 kGeometry = 0x81302000u, kExtent = 0x81302100u, kSector = 0x81302200u, kRecord = 0x81302300u;
constexpr u32 kObject = 0x81304000u, kAlloc = 0x81306000u, kStream = 0x81302900u;
constexpr u32 kScratchWords = 80;

void require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error(what);
}

struct Rng {
    u32 state;
    explicit Rng(u32 seed) : state(seed * 2654435761u + 12345u) {}
    u32 next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
};

struct Snapshot {
    u32 r3, r1, r2, r13, lr;
    u32 saved[18];
    u64 f[18];
    std::vector<u8> data, test;
};

// What the doubles answer. fail_at: the n-th (1-based) stream write call, counted over the three writers, answers 0.
struct Script {
    u32 header_ret = 1u;
    int fail_at = 0;
    u32 lookup_ret = kRecord;
    bool find_ok = true;
    u32 find_size = 0x2000u, find_version = 0x36000u;
    u32 tag = 6u, first = 0x100u, second = 0x180u;
    u32 ret_tag = 1u, ret_first = 1u, ret_second = 1u;
    u32 delta_first = 0u, delta_second = 0u;
};

struct Machine {
    GuestMemory memory;
    Cpu cpu{memory};
    std::shared_ptr<Script> script = std::make_shared<Script>();
    std::shared_ptr<int> writes = std::make_shared<int>(0), reads = std::make_shared<int>(0), blocks = std::make_shared<int>(0);

    Machine(const char* path, bool native, const std::function<void(Cpu&)>& wrong = nullptr,
            const std::vector<std::pair<u32, u32>>& patch_original = {}) {
        std::ifstream input(path, std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        unsigned long long hash = 0xCBF29CE484222325ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 0x100000001B3ull;
        require(hash == 0xD2CFB4A1CA897F9Bull, "unexpected original DOL identity");
        const auto word = [&](unsigned offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(static_cast<unsigned long long>(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
        gx_state::install(cpu);   // GXInvalidateVtxCache
        install_doubles();
        if (native) {
            reg::install(cpu);
            if (wrong) wrong(cpu);
        } else {
            for (const auto& [address, value] : patch_original) memory.write32(address, value);
        }
    }

    // a log entry: id, then up to 6 words
    void log(Cpu& c, u32 id, std::initializer_list<u32> words) {
        const u32 n = c.mem_read32(kLog);
        const u32 at = kLog + 0x10u + n * 0x40u;
        c.mem_write32(kLog, n + 1u);
        c.mem_write32(at, id);
        u32 k = 1;
        for (u32 w : words) c.mem_write32(at + 4u * k++, w);
    }
    static u32 hash_bytes(Cpu& c, u32 address, u32 length) {
        u32 h = 2166136261u;
        for (u32 i = 0; i < length && i < 0x2000u; ++i) h = (h ^ c.mem_read8(address + i)) * 16777619u;
        return h;
    }
    static void scribble(Cpu& c) {   // a callee's own frame
        for (u32 k = 1; k <= kScratchWords; ++k) c.mem_write32(c.gpr[1] - 4u * k, 0xDEAD0000u + k);
    }

    void install_doubles() {
        const std::shared_ptr<Script> sc = script;
        const auto w = writes, rd = reads, bl = blocks;
        // 0x8047DF40 chunk header (stream, type, size, version, library)
        cpu.install_hle(0x8047DF40u, [this, sc, w](Cpu& c) {
            ++*w;
            log(c, 1u, {c.gpr[3], c.gpr[4], c.gpr[5], c.gpr[6], c.gpr[7]});
            scribble(c);
            c.gpr[3] = (sc->fail_at == *w) ? 0u : sc->header_ret;
            return true;
        }, "test: RwStreamWriteChunkHeader");
        // 0x8047E4A8 stream word write (stream, &word, 4)
        cpu.install_hle(0x8047E4A8u, [this, sc, w](Cpu& c) {
            ++*w;
            log(c, 2u, {c.gpr[3], c.gpr[4], c.mem_read32(c.gpr[4]), c.gpr[5]});
            scribble(c);
            c.gpr[3] = (sc->fail_at == *w) ? 0u : 1u;
            return true;
        }, "test: RwStreamWriteInt32");
        // 0x80481F20 stream block write (stream, bytes, length)
        cpu.install_hle(0x80481F20u, [this, sc, w](Cpu& c) {
            ++*w;
            log(c, 3u, {c.gpr[3], c.gpr[4], c.gpr[5], hash_bytes(c, c.gpr[4], c.gpr[5])});
            scribble(c);
            c.gpr[3] = (sc->fail_at == *w) ? 0u : 1u;
            return true;
        }, "test: RwStreamWrite");
        // 0x80460EF0 the object lookup (object)
        cpu.install_hle(0x80460EF0u, [this, sc](Cpu& c) {
            log(c, 4u, {c.gpr[3]});
            scribble(c);
            c.gpr[3] = sc->lookup_ret;
            return true;
        }, "test: lookup");
        // the reader's functions
        cpu.install_hle(0x8047DFA0u, [this, sc](Cpu& c) {   // find chunk (stream, type, &size, &version)
            log(c, 5u, {c.gpr[3], c.gpr[4], c.gpr[5], c.gpr[6]});
            c.mem_write32(c.gpr[5], sc->find_size);
            c.mem_write32(c.gpr[6], sc->find_version);
            scribble(c);
            c.gpr[3] = sc->find_ok ? 1u : 0u;
            return true;
        }, "test: RwStreamFindChunk");
        cpu.install_hle(0x8047E8B8u, [this, sc, rd](Cpu& c) {   // read 4 bytes (stream, &word, 4)
            const int k = (*rd)++;
            log(c, 6u, {c.gpr[3], c.gpr[4], c.gpr[5], static_cast<u32>(k)});
            const u32 values[3] = {sc->tag, sc->first, sc->second};
            const u32 rets[3] = {sc->ret_tag, sc->ret_first, sc->ret_second};
            c.mem_write32(c.gpr[4], values[k % 3]);
            scribble(c);
            c.gpr[3] = rets[k % 3];
            return true;
        }, "test: RwStreamReadInt32");
        cpu.install_hle(0x80481D9Cu, [this, sc, bl](Cpu& c) {   // read a block (stream, buffer, length)
            const int k = (*bl)++;
            log(c, 7u, {c.gpr[3], c.gpr[4], c.gpr[5], static_cast<u32>(k)});
            scribble(c);
            c.gpr[3] = c.gpr[5] + (k % 2 == 0 ? sc->delta_first : sc->delta_second);
            return true;
        }, "test: RwStreamRead");
        cpu.install_hle(kAlloc, [this](Cpu& c) {   // the engine allocator ([engine + 0x108])
            log(c, 8u, {c.gpr[3], c.gpr[4]});
            scribble(c);
            c.gpr[3] = kObject;
            return true;
        }, "test: engine allocator");
        cpu.install_hle(0x8047EC78u, [this](Cpu& c) {   // RwErrorGenerate-like: hands its first argument back
            log(c, 9u, {c.gpr[3]});
            scribble(c);
            return true;
        }, "test: variadic error format");
        cpu.install_hle(0x8047EBD4u, [this](Cpu& c) {   // RwErrorSet(&{plugin, code})
            log(c, 10u, {c.gpr[3], c.mem_read32(c.gpr[3]), c.mem_read32(c.gpr[3] + 4u)});
            scribble(c);
            return true;
        }, "test: RwErrorSet");
    }

    void w(u32 address, u32 value) { memory.write32(address, value); }

    void fill(const Script& s) {
        *script = s;
        *writes = *reads = *blocks = 0;
        for (u32 a = kDataLo; a < kDataHi; a += 4) memory.write32(a, 0xA5000000u ^ (a * 2654435761u));
        for (u32 a = kTestLo; a < kTestHi; a += 4) memory.write32(a, 0x5A000000u ^ (a * 40503u));
        w(0x80372534u, 0x4E800020u);   // DCFlushRange: returns on both
        w(kLog, 0u);
        w(kSda + 0x615Cu, kEngine);
        w(kEngine + 0x108u, kAlloc);
    }

    void call(u32 entry, u32 r3, u32 r4) {
        cpu.reset();
        cpu.pc = entry; cpu.npc = entry + 4; cpu.lr = kFinish;
        for (unsigned i = 0; i < 32; ++i) cpu.gpr[i] = 0x1000u + i * 0x111u;
        cpu.gpr[1] = kStack; cpu.gpr[2] = kSda2; cpu.gpr[13] = kSda; cpu.gpr[3] = r3; cpu.gpr[4] = r4;
        for (unsigned i = 14; i < 32; ++i) {
            const u64 value = 0x4000000000000000ull + i;
            std::memcpy(&cpu.fpr[i].bits[0], &value, sizeof(value));
        }
        unsigned steps = 0;
        while (cpu.pc != kFinish && !cpu.stopped() && steps++ < 800000) cpu.run_steps(1);
        require(cpu.pc == kFinish && !cpu.stopped(), "the call did not return");
    }

    Snapshot snapshot() const {
        Snapshot s{};
        s.r3 = cpu.gpr[3]; s.r1 = cpu.gpr[1]; s.r2 = cpu.gpr[2]; s.r13 = cpu.gpr[13]; s.lr = cpu.lr;
        for (unsigned i = 0; i < 18; ++i) {
            s.saved[i] = cpu.gpr[14 + i];
            std::memcpy(&s.f[i], &cpu.fpr[14 + i].bits[0], sizeof(u64));
        }
        s.data.resize(kDataHi - kDataLo);
        for (u32 i = 0; i < kDataHi - kDataLo; ++i) s.data[i] = memory.peek8(kDataLo + i);
        s.test.resize(kTestHi - kTestLo);
        for (u32 i = 0; i < kTestHi - kTestLo; ++i) s.test[i] = memory.peek8(kTestLo + i);
        return s;
    }
};

std::string diff(const Snapshot& a, const Snapshot& b) {
    char text[120];
    if (a.r3 != b.r3) { std::snprintf(text, sizeof text, "r3 original=%08X native=%08X", a.r3, b.r3); return text; }
    if (a.r1 != b.r1 || a.r2 != b.r2 || a.r13 != b.r13 || a.lr != b.lr) return "r1 / r2 / r13 / lr differ";
    if (std::memcmp(a.saved, b.saved, sizeof a.saved) != 0) return "a callee-saved register differs";
    if (std::memcmp(a.f, b.f, sizeof a.f) != 0) return "a callee-saved FPR differs";
    for (u32 i = 0; i < a.data.size(); ++i)
        if (a.data[i] != b.data[i]) { std::snprintf(text, sizeof text, "data byte 0x%08X differs", kDataLo + i); return text; }
    for (u32 i = 0; i < a.test.size(); ++i)
        if (a.test[i] != b.test[i]) { std::snprintf(text, sizeof text, "test byte 0x%08X differs", kTestLo + i); return text; }
    return {};
}

struct Params {
    u32 seed = 1u;
    u32 flags = 0x01000000u;        // the object's flag word (geometry [+8], lookup record [+8])
    u32 entries = 3u;               // [object + 0x20]: the first block's pointer entries
    u32 count = 5u;                 // the halfword at [extent + 4]
    u32 total = 0x1000u;            // [object + 8]
    bool data_null = false;         // [geometry + 0x58] / [sector + 0x34] = 0
    Script script;
};

unsigned g_cases = 0;
const std::function<void(Cpu&)>* g_wrong = nullptr;
const std::vector<std::pair<u32, u32>>* g_patch = nullptr;

void setup(Machine& m, const Params& p) {
    m.fill(p.script);
    Rng rng(p.seed);
    for (u32 a = kObject; a < kObject + 0x800u; a += 4) m.w(a, rng.next());
    m.w(kObject + 8u, p.total);
    m.w(kObject + 0x20u, p.entries);
    // the first pointer of the second table is read as the second block's address after the tables are relocated back: a real
    // address (the doubles hash the bytes it points at), not a wild one
    m.w(kObject + 0x18u + (p.entries - 1u) * 8u + 0x14u, kObject + 0x400u);
    // the geometry (+8 flags, +0x54 extent, +0x58 data) and the world sector (+0x34 data, +0x78 extent)
    m.w(kGeometry + 8u, p.flags);
    m.w(kGeometry + 0x54u, kExtent);
    m.w(kGeometry + 0x58u, p.data_null ? 0u : kObject);
    m.w(kSector + 0x34u, p.data_null ? 0u : kObject);
    m.w(kSector + 0x78u, kExtent);
    m.memory.write16(kExtent + 4u, static_cast<u16>(p.count));
    m.w(kRecord + 8u, p.flags);
    m.memory.write16(kSda + 0x2AF0u, static_cast<u16>(0x4000u + p.seed));
}

void compare(const char* path, const char* what, u32 entry, const Params& p, u32 geometry_or_sector = kGeometry) {
    Machine original(path, false, nullptr, g_patch ? *g_patch : std::vector<std::pair<u32, u32>>{});
    Machine native(path, true, g_wrong ? *g_wrong : std::function<void(Cpu&)>{});
    setup(original, p);
    setup(native, p);
    const bool sized = entry == reg::kGeometrySize || entry == reg::kSectorSize;
    const u32 r3 = sized ? geometry_or_sector : kStream;
    const u32 r4 = sized ? 0u : geometry_or_sector;
    original.call(entry, r3, r4);
    native.call(entry, r3, r4);
    const std::string d = diff(original.snapshot(), native.snapshot());
    if (!d.empty()) {
        char text[220];
        std::snprintf(text, sizeof text, "%s %08X (seed %u): %s", what, entry, p.seed, d.c_str());
        throw std::runtime_error(text);
    }
    ++g_cases;
}

void run_all(const char* path) {
    g_cases = 0;
    u32 seed = 1;
    // sizes: the flag, the data pointer and the size word
    for (u32 flags : {0u, 0x01000000u, 0xFEFFFFFFu, 0xFFFFFFFFu, 0x00800000u})
        for (bool null_data : {false, true}) {
            Params p; p.flags = flags; p.data_null = null_data; p.seed = ++seed; p.total = 0x20u + seed * 7u;
            compare(path, "geometry size", reg::kGeometrySize, p, kGeometry);
            compare(path, "sector size", reg::kSectorSize, p, kSector);
        }
    // writes: every table shape, every failing stream write, the flag clear
    for (u32 entries : {0u, 1u, 2u, 3u, 6u})
        for (u32 count : {0u, 1u, 5u, 8u, 9u, 17u})
            for (int fail : {0, 1, 2, 3, 4, 5, 6}) {
                Params p; p.entries = entries; p.count = count; p.script.fail_at = fail; p.seed = ++seed;
                p.total = 0x180u + (seed & 0x3Fu) * 4u;
                compare(path, "geometry write", reg::kGeometryWrite, p, kGeometry);
                compare(path, "sector write", reg::kSectorWrite, p, kSector);
            }
    for (u32 flags : {0u, 0xFEFFFFFFu}) {
        Params p; p.flags = flags; p.seed = ++seed;
        compare(path, "geometry write, no native data", reg::kGeometryWrite, p, kGeometry);
        compare(path, "sector write, no native data", reg::kSectorWrite, p, kSector);
    }
    {
        Params p; p.script.header_ret = 0u; p.seed = ++seed;
        compare(path, "geometry write, header refused", reg::kGeometryWrite, p, kGeometry);
        compare(path, "sector write, header refused", reg::kSectorWrite, p, kSector);
    }
    // reads: the geometry reader over the same body as the world-sector reader
    {
        Params p; p.seed = ++seed;
        p.script.find_ok = false; compare(path, "geometry read, no chunk", reg::kGeometryRead, p, kGeometry);
    }
    for (u32 version : {0x34FFFu, 0x35000u, 0x36000u, 0x37002u, 0x37003u, 0u, 0xFFFFFFFFu}) {
        Params p; p.seed = ++seed; p.script.find_version = version;
        compare(path, "geometry read version", reg::kGeometryRead, p, kGeometry);
    }
    {
        Params p; p.seed = ++seed;
        p.script.ret_tag = 0; compare(path, "geometry read, tag read fails", reg::kGeometryRead, p, kGeometry);
        p.script = Script{}; p.script.tag = 5; compare(path, "geometry read, wrong tag", reg::kGeometryRead, p, kGeometry);
        p.script = Script{}; p.script.ret_first = 0; compare(path, "geometry read, first read fails", reg::kGeometryRead, p, kGeometry);
        p.script = Script{}; p.script.ret_second = 0; compare(path, "geometry read, second read fails", reg::kGeometryRead, p, kGeometry);
        p.script = Script{}; p.script.delta_first = 1; compare(path, "geometry read, first block short", reg::kGeometryRead, p, kGeometry);
        p.script = Script{}; p.script.delta_second = 0xFFFFFFFFu; compare(path, "geometry read, second block short", reg::kGeometryRead, p, kGeometry);
    }
    for (u32 entries : {0u, 1u, 3u, 6u})
        for (u32 count : {0u, 1u, 5u, 8u, 9u, 24u})
            for (u32 first : {0x40u, 0x105u}) {
                Params p; p.entries = entries; p.count = count; p.script.first = first; p.script.second = 0x20u + count * 3u;
                p.seed = ++seed;
                compare(path, "geometry read", reg::kGeometryRead, p, kGeometry);
            }
}

bool caught(const char* path, const std::function<void(Cpu&)>* wrong, const std::vector<std::pair<u32, u32>>* patch) {
    g_wrong = wrong;
    g_patch = patch;
    bool found = false;
    try {
        run_all(path);
    } catch (const std::exception&) {
        found = true;
    }
    g_wrong = nullptr;
    g_patch = nullptr;
    return found;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rw_world_stream_contract_tests main.dol");
        run_all(argv[1]);
        const unsigned cases = g_cases;
        const std::function<void(Cpu&)> nothing = [](Cpu& cpu) {
            cpu.install_hle(reg::kGeometryWrite, [](Cpu& c) {
                c.gpr[3] = c.gpr[3];   // returns the stream and writes nothing
                return true;
            }, "wrong: writes nothing");
        };
        require(caught(argv[1], &nothing, nullptr), "negative control: a writer that writes nothing was not told apart");
        const std::function<void(Cpu&)> wrong_size = [](Cpu& cpu) {
            cpu.install_hle(reg::kGeometrySize, [](Cpu& c) {
                const u32 data = c.mem_read32(c.gpr[3] + 0x58u);
                c.gpr[3] = data != 0u && (c.mem_read32(c.gpr[3] + 8u) & 0x01000000u) != 0u ? c.mem_read32(data + 8u) - 8u : 0u;
                return true;
            }, "wrong: size off by one");
        };
        require(caught(argv[1], &wrong_size, nullptr), "negative control: a size that is off by one was not told apart");
        // ORIGINAL code words patched: one argument of the chunk header, one of the second block write
        const std::vector<std::pair<u32, u32>> header_type = {{0x80465E3Cu, 0x38800002u}};      // li r4, 2 instead of 1
        const std::vector<std::pair<u32, u32>> header_lib = {{0x80465E4Cu, 0x38E00038u}};       // li r7, 0x38 instead of 0x37
        const std::vector<std::pair<u32, u32>> second_len = {{0x80466118u, 0x80A1000Cu}};       // lwz r5, 0xC(r1) instead of 8(r1)
        require(caught(argv[1], nullptr, &header_type), "mutation: the chunk type argument was not told apart");
        require(caught(argv[1], nullptr, &header_lib), "mutation: the chunk library argument was not told apart");
        require(caught(argv[1], nullptr, &second_len), "mutation: the second block's length was not told apart");
        std::printf("PASS rw world stream contract: %u original-vs-native cases\n", cases);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL rw world stream contract: %s\n", error.what());
        return 1;
    }
}
