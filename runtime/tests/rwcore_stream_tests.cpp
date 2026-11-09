// Oracle for the native RenderWare streams (rwcore/src/stream.cpp): the ORIGINAL bastream functions of PAL main.dol
// in the reference interpreter (rwcore_oracle.h) and the native ones run the same random sessions on memory, file,
// file-name and custom streams (and invalid types / access types). The file layer under them - the GameCube's
// RwFopen / RwFclose / RwFread / RwFwrite / RwFseek / RwFeof / RwFtell - is replaced on both sides by the same
// in-memory files (host stubs in the guest, rwFileInterface natively), so the oracle compares what the streams ask of
// it. After every operation: the result, the bytes read, the stream's state (type, access, memory position and size),
// the held error, every file and custom-function call with its arguments, and every RwMalloc / RwRealloc / RwFree
// (hints; sizes of the memory streams' blocks). A written memory stream's block is compared byte for byte at close.
// liveness-roots: reference-interpreter-only
//
// Usage: rwcore_stream_tests <main.dol>
#include "rw/core/error.h"
#include "rw/core/memory.h"
#include "rw/core/stream.h"
#include "rwcore_oracle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
using namespace rworacle;

constexpr Original kModuleOpen{"_rwStreamModuleOpen", 0x80481CBCu, 0x8Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kModuleClose{"_rwStreamModuleClose", 0x80481D48u, 0x54u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRead{"RwStreamRead", 0x80481D9Cu, 0x184u, 0x9421FFC0u, 0x4E800020u};
constexpr Original kWrite{"RwStreamWrite", 0x80481F20u, 0x22Cu, 0x9421FFC0u, 0x4E800020u};
constexpr Original kSkip{"RwStreamSkip", 0x8048214Cu, 0x150u, 0x9421FFD0u, 0x4E800020u};
constexpr Original kClose{"RwStreamClose", 0x8048229Cu, 0x11Cu, 0x9421FFE0u, 0x4E800020u};
constexpr Original kOpen{"RwStreamOpen", 0x804823B8u, 0x288u, 0x9421FFC0u, 0x4E800020u};
constexpr Original kMemoryOpen{"_rwMemoryOpen", 0x804812C4u, 0x118u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kMemoryClose{"_rwMemoryClose", 0x804813DCu, 0x1A4u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kErrorOpen{"_rwErrorOpen", 0x8047EB88u, 0x38u, 0x390D6100u, 0x4E800020u};
constexpr Original kErrorGet{"RwErrorGet", 0x8047EC34u, 0x44u, 0x808D615Cu, 0x4E800020u};
constexpr u32 kFopen = 0x8047B1C8u, kFclose = 0x8047B3D8u, kFread = 0x8047B430u, kFwrite = 0x8047B478u,
              kFseek = 0x8047B4C0u, kFeof = 0x8047B6A0u, kFtell = 0x8047B810u;     // the GameCube's RwF* functions
constexpr u32 kFreeListAlloc = 0x80480E00u, kFreeListFree = 0x80480FA0u;
constexpr u32 kStreamModule = 0x805F2628u;
constexpr u32 kCustomClose = 0x817F0060u, kCustomRead = 0x817F0070u, kCustomWrite = 0x817F0080u,
              kCustomSkip = 0x817F0090u;
constexpr u32 kGuestErrorOffset = 0x200u, kGuestStreamOffset = 0x240u;
constexpr u32 kFuncs = kBuffer, kErr = kBuffer + 0x20u, kMem = kBuffer + 0x40u, kCustom = kBuffer + 0x60u,
              kName = kBuffer + 0x80u, kIo = 0x81010000u, kFileHandles = 0x81500000u;
constexpr u32 kIoSize = 0x4000u;

// ---- the in-memory files both sides use ----------------------------------------------------------------------
struct FakeFile {
    std::vector<u8> data;
    std::size_t pos = 0;
    bool eof = false, open = true, tellFails = false;
};
std::vector<FakeFile> g_files;   // identical for both sides: each side has its own copy
struct FileCall {
    int what;          // 0 open, 1 close, 2 read, 3 write, 4 seek, 5 eof, 6 tell; 7.. custom close/read/write/skip
    u32 file, a, b;
    std::string text;
    bool operator==(const FileCall&) const = default;
};

/// One side's file layer: the same behaviour, its own files and its own record.
struct Files {
    std::vector<FakeFile> files;
    std::vector<FileCall> calls;
    int fail_open_at = -1, opens = 0;

    long open(const std::string& name, const std::string& access) {
        calls.push_back({0, 0, 0, 0, name + "|" + access});
        if (opens++ == fail_open_at) return -1;
        files.push_back({});
        if (access == "rb") files.back().data.assign(name.begin(), name.end());   // a named file holds its name
        return static_cast<long>(files.size()) - 1;
    }
    RwInt32 close(u32 f) {
        calls.push_back({1, f, 0, 0, {}});
        files[f].open = false;
        return files[f].data.size() % 3u == 0 ? -1 : 0;     // some closes fail
    }
    std::size_t read(u32 f, u8* out, std::size_t n) {
        calls.push_back({2, f, static_cast<u32>(n), 0, {}});
        FakeFile& file = files[f];
        const std::size_t got = std::min(n, file.data.size() - file.pos);
        std::memcpy(out, file.data.data() + file.pos, got);
        file.pos += got;
        if (got < n) file.eof = true;
        return got;
    }
    std::size_t write(u32 f, const u8* in, std::size_t n) {
        calls.push_back({3, f, static_cast<u32>(n), 0, std::string(reinterpret_cast<const char*>(in), n)});
        FakeFile& file = files[f];
        const std::size_t put = file.data.size() > 3000 ? n / 2 : n;   // a "full disc" writes half
        file.data.insert(file.data.end(), in, in + put);
        return put;
    }
    RwInt32 seek(u32 f, RwInt64 offset, RwInt32 origin) {
        calls.push_back({4, f, static_cast<u32>(offset), static_cast<u32>(origin), {}});
        FakeFile& file = files[f];
        if (file.pos + static_cast<std::size_t>(offset) > file.data.size()) {
            file.pos = file.data.size();
            file.eof = (offset & 1) == 0;                   // some failed seeks are not at the end
            return -1;
        }
        file.pos += static_cast<std::size_t>(offset);
        return 0;
    }
    RwInt32 eof(u32 f) {
        calls.push_back({5, f, 0, 0, {}});
        return files[f].eof ? 1 : 0;
    }
    RwInt32 tell(u32 f) {
        calls.push_back({6, f, 0, 0, {}});
        return files[f].tellFails ? -1 : static_cast<RwInt32>(files[f].pos);
    }
};
Files g_guest_files, g_native_files;

// native file layer: a handle is the file's index + 1, as a pointer
u32 index_of(void* fp) { return static_cast<u32>(reinterpret_cast<std::uintptr_t>(fp)) - 1u; }
void* nOpen(const RwChar* name, const RwChar* access) {
    const long f = g_native_files.open(name, access);
    return f < 0 ? nullptr : reinterpret_cast<void*>(static_cast<std::uintptr_t>(f) + 1);
}
RwInt32 nClose(void* fp) { return g_native_files.close(index_of(fp)); }
std::size_t nRead(void* addr, std::size_t size, std::size_t count, void* fp) {
    return g_native_files.read(index_of(fp), static_cast<u8*>(addr), size * count) / size;
}
std::size_t nWrite(const void* addr, std::size_t size, std::size_t count, void* fp) {
    return g_native_files.write(index_of(fp), static_cast<const u8*>(addr), size * count) / size;
}
RwInt32 nSeek(void* fp, RwInt64 offset, RwInt32 origin) { return g_native_files.seek(index_of(fp), offset, origin); }
RwInt32 nEof(void* fp) { return g_native_files.eof(index_of(fp)); }
RwInt64 nTell(void* fp) { return g_native_files.tell(index_of(fp)); }

// native custom-stream functions: results from a shared script
std::vector<u32> g_custom_results;
std::size_t g_native_custom_next = 0;
u32 next_custom(std::size_t& next) { return g_custom_results[next++ % g_custom_results.size()]; }
std::vector<u8> g_native_custom_bytes;
RwBool nCustomClose(void* data) {
    g_native_files.calls.push_back({7, static_cast<u32>(reinterpret_cast<std::uintptr_t>(data)), 0, 0, {}});
    return 1;
}
RwUInt32 nCustomRead(void* data, void* buffer, RwUInt32 length) {
    const u32 n = next_custom(g_native_custom_next) % (length + 1u);
    g_native_files.calls.push_back({8, static_cast<u32>(reinterpret_cast<std::uintptr_t>(data)), length, n, {}});
    for (u32 i = 0; i < n; ++i) static_cast<u8*>(buffer)[i] = static_cast<u8>(i * 7u + length);
    return n;
}
RwBool nCustomWrite(void* data, const void* buffer, RwUInt32 length) {
    g_native_files.calls.push_back({9, static_cast<u32>(reinterpret_cast<std::uintptr_t>(data)), length, 0,
                                    std::string(static_cast<const char*>(buffer), length)});
    return next_custom(g_native_custom_next) & 1u;
}
RwBool nCustomSkip(void* data, RwUInt32 offset) {
    g_native_files.calls.push_back({10, static_cast<u32>(reinterpret_cast<std::uintptr_t>(data)), offset, 0, {}});
    return next_custom(g_native_custom_next) & 1u;
}

// recorded engine memory functions
struct Call {
    std::size_t size;
    RwUInt32 hint;
};
std::vector<Call> g_mallocs, g_reallocs;
std::size_t g_frees = 0;
int g_fail_malloc = -1, g_fail_realloc = -1;
void* recMalloc(std::size_t size, RwUInt32 hint) {
    g_mallocs.push_back({size, hint});
    return static_cast<int>(g_mallocs.size()) - 1 == g_fail_malloc ? nullptr : std::malloc(size);
}
void* recRealloc(void* p, std::size_t size, RwUInt32 hint) {
    g_reallocs.push_back({size, hint});
    return static_cast<int>(g_reallocs.size()) - 1 == g_fail_realloc ? nullptr : std::realloc(p, size);
}
void recFree(void* p) {
    ++g_frees;
    std::free(p);
}

struct Pair {
    u32 guest;
    RwStream* native;
    std::size_t defined = 0;   // a memory stream: the bytes of its block that hold data (the rest is uninitialised)
};

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rwcore_stream_tests main.dol");
        Guest g(argv[1]);
        for (const Original& f : {kModuleOpen, kModuleClose, kRead, kWrite, kSkip, kClose, kOpen, kMemoryOpen,
                                  kMemoryClose, kErrorOpen, kErrorGet}) {
            g.check_pinned(f);
        }
        GuestMemory& m = g.memory();
        const auto guest_string = [&](u32 a) {
            std::string s;
            for (u8 c; (c = m.peek8(a)) != 0 && s.size() < 256; ++a) s.push_back(static_cast<char>(c));
            return s;
        };
        const auto handle = [](long f) { return f < 0 ? 0u : kFileHandles + static_cast<u32>(f) * 0x40u; };
        const auto file_of = [](u32 h) { return (h - kFileHandles) / 0x40u; };
        g.stub(kFopen, [&](Cpu& c) { c.gpr[3] = handle(g_guest_files.open(guest_string(c.gpr[3]), guest_string(c.gpr[4]))); });
        g.stub(kFclose, [&](Cpu& c) { c.gpr[3] = static_cast<u32>(g_guest_files.close(file_of(c.gpr[3]))); });
        g.stub(kFread, [&](Cpu& c) {
            std::vector<u8> bytes(std::size_t{c.gpr[4]} * c.gpr[5]);
            const std::size_t got = g_guest_files.read(file_of(c.gpr[6]), bytes.data(), bytes.size());
            m.write_block(c.gpr[3], bytes.data(), got);
            c.gpr[3] = static_cast<u32>(got / c.gpr[4]);
        });
        g.stub(kFwrite, [&](Cpu& c) {
            std::vector<u8> bytes(std::size_t{c.gpr[4]} * c.gpr[5]);
            m.peek_block(c.gpr[3], bytes.data(), bytes.size());
            c.gpr[3] = static_cast<u32>(g_guest_files.write(file_of(c.gpr[6]), bytes.data(), bytes.size()) / c.gpr[4]);
        });
        g.stub(kFseek, [&](Cpu& c) {
            c.gpr[3] = static_cast<u32>(g_guest_files.seek(file_of(c.gpr[3]), static_cast<RwInt64>(c.gpr[4]),
                                                           static_cast<RwInt32>(c.gpr[5])));
        });
        g.stub(kFeof, [&](Cpu& c) { c.gpr[3] = static_cast<u32>(g_guest_files.eof(file_of(c.gpr[3]))); });
        g.stub(kFtell, [&](Cpu& c) { c.gpr[3] = static_cast<u32>(g_guest_files.tell(file_of(c.gpr[3]))); });
        std::size_t guest_custom_next = 0;
        g.stub(kCustomClose, [&](Cpu& c) {
            g_guest_files.calls.push_back({7, c.gpr[3], 0, 0, {}});
            c.gpr[3] = 1;
        });
        g.stub(kCustomRead, [&](Cpu& c) {
            const u32 length = c.gpr[5], n = next_custom(guest_custom_next) % (length + 1u);
            g_guest_files.calls.push_back({8, c.gpr[3], length, n, {}});
            for (u32 i = 0; i < n; ++i) m.write8(c.gpr[4] + i, static_cast<u8>(i * 7u + length));
            c.gpr[3] = n;
        });
        g.stub(kCustomWrite, [&](Cpu& c) {
            std::string bytes(c.gpr[5], '\0');
            m.peek_block(c.gpr[4], reinterpret_cast<u8*>(bytes.data()), bytes.size());
            g_guest_files.calls.push_back({9, c.gpr[3], c.gpr[5], 0, bytes});
            c.gpr[3] = next_custom(guest_custom_next) & 1u;
        });
        g.stub(kCustomSkip, [&](Cpu& c) {
            g_guest_files.calls.push_back({10, c.gpr[3], c.gpr[4], 0, {}});
            c.gpr[3] = next_custom(guest_custom_next) & 1u;
        });
        rwFileInterface() = {nOpen, nClose, nRead, nWrite, nSeek, nEof, nTell};

        // engines: RwGlobals, the error module, the stream module
        std::vector<std::max_align_t> block(64 + sizeof(RwGlobals) / sizeof(std::max_align_t));
        RwEngineInstance = block.data();
        rwSrcGlobals() = RwGlobals{};
        RwMemoryFunctions recording = rwHostMemoryFunctions();
        recording.rwmalloc = recMalloc;
        recording.rwfree = recFree;
        recording.rwrealloc = recRealloc;
        const RwInt32 nativeErrorOffset = static_cast<RwInt32>((sizeof(RwGlobals) + 15) & ~std::size_t{15});
        const RwInt32 nativeStreamOffset = nativeErrorOffset + 16;
        _rwErrorOpen(RwEngineInstance, nativeErrorOffset, 8);
        m.write32(kErrorModule, 0), m.write32(kErrorModule + 4, 0);
        g.call(kErrorOpen.entry, {kEngine, kGuestErrorOffset, 8});
        m.write32(kFuncs, kMallocStub);
        m.write32(kFuncs + 4, kFreeStub);
        m.write32(kFuncs + 8, kReallocStub);
        m.write32(kFuncs + 12, 0);
        m.write32(kStreamModule, 0), m.write32(kStreamModule + 4, 0);

        Rng rng;
        for (unsigned i = 0; i < 64; ++i) g_custom_results.push_back(rng.next());
        u64 instructions = 0;
        unsigned checks = 0;
        std::size_t gm = 0, nm = 0, gr = 0, nr = 0, gf = 0, nf = 0;
        const auto same_calls = [&](const char* what) {
            require(g.mallocs.size() - gm == g_mallocs.size() - nm && g.reallocs.size() - gr == g_reallocs.size() - nr &&
                        g.frees.size() - gf == g_frees - nf,
                    what);
            for (std::size_t i = 0; i < g_mallocs.size() - nm; ++i) {
                const auto& a = g.mallocs[gm + i];
                const auto& b = g_mallocs[nm + i];
                require(a.hint == b.hint && ((a.size == 0x200u) == (b.size == 0x200u)), what);   // a memory stream's first block
            }
            for (std::size_t i = 0; i < g_reallocs.size() - nr; ++i) {
                require(g.reallocs[gr + i].hint == g_reallocs[nr + i].hint && g.reallocs[gr + i].size == g_reallocs[nr + i].size,
                        what);
            }
            gm = g.mallocs.size(), nm = g_mallocs.size(), gr = g.reallocs.size(), nr = g_reallocs.size();
            gf = g.frees.size(), nf = g_frees;
            require(g_guest_files.calls == g_native_files.calls, what);
            g_guest_files.calls.clear(), g_native_files.calls.clear();
        };
        const auto same_error = [&](const char* what) {
            instructions += g.call(kErrorGet.entry, {kErr});
            RwError e{};
            RwErrorGet(&e);
            require(m.peek32(kErr) == static_cast<u32>(e.pluginID) && m.peek32(kErr + 4) == static_cast<u32>(e.errorCode),
                    what);
        };
        const auto same_stream = [&](const Pair& p, const char* what) {
            const RwStream& n = *p.native;
            require(m.peek32(p.guest) == static_cast<u32>(n.type) && m.peek32(p.guest + 4) == static_cast<u32>(n.accessType) &&
                        m.peek32(p.guest + 0x20) == static_cast<u32>(n.rwOwned),
                    what);
            if (n.type == rwSTREAMMEMORY) {
                require(m.peek32(p.guest + 0xC) == n.Type.memory.position && m.peek32(p.guest + 0x10) == n.Type.memory.nSize &&
                            (m.peek32(p.guest + 0x14) == 0) == (n.Type.memory.memBlock == nullptr),
                        what);
            }
        };

        // closes a pair; a written memory block is then freed on both sides alike (the caller owns it)
        const auto close_pair = [&](const Pair& p, bool give) {
            m.write32(kMem, 0xDEAD0000u), m.write32(kMem + 4, 0xDEADu);
            RwUInt8* const untouched = reinterpret_cast<RwUInt8*>(std::uintptr_t{0xDEAD0000u});
            RwMemory out{untouched, 0xDEADu};
            const bool written = p.native->type == rwSTREAMMEMORY && p.native->accessType != rwSTREAMREAD;
            const bool memory = p.native->type == rwSTREAMMEMORY;
            const u32 guest_block = memory ? m.peek32(p.guest + 0x14) : 0u;
            RwUInt8* const native_block = memory ? p.native->Type.memory.memBlock : nullptr;
            if (written && native_block != nullptr) {
                std::vector<u8> bytes(std::min<std::size_t>(m.peek32(p.guest + 0xC), p.defined));
                m.peek_block(guest_block, bytes.data(), bytes.size());
                require(std::memcmp(bytes.data(), native_block, bytes.size()) == 0, "a written memory stream's bytes differ");
            }
            instructions += g.call(kClose.entry, {p.guest, give ? kMem : 0u});
            const RwBool closed = RwStreamClose(p.native, give ? &out : nullptr);
            require(g.r3() == static_cast<u32>(closed), "RwStreamClose's result");
            if (give) {
                require((m.peek32(kMem) == 0xDEAD0000u) == (out.start == untouched) && m.peek32(kMem + 4) == out.length &&
                            (out.start == untouched || (m.peek32(kMem) == guest_block && out.start == native_block && written)),
                        "RwStreamClose's memory result");
            }
            if (native_block != nullptr) {
                RwFree(native_block);
                g.frees.push_back(guest_block), g.heap_free(guest_block);
            }
        };

        for (unsigned session = 0; session < 300; ++session) {
            instructions += g.call(kMemoryOpen.entry, {kFuncs});
            require(g.r3() == 1 && _rwMemoryOpen(&recording) == 1, "_rwMemoryOpen must succeed");
            m.write32(kEngine + 0x118u, kFreeListAlloc);
            m.write32(kEngine + 0x11Cu, kFreeListFree);
            rwSrcGlobals().memoryAlloc = _rwFreeListAllocReal;
            rwSrcGlobals().memoryFree = _rwFreeListFreeReal;
            const bool fail_module = session % 11 == 10;
            if (fail_module) g.fail_call = static_cast<int>(g.mallocs.size()), g_fail_malloc = static_cast<int>(g_mallocs.size());
            instructions += g.call(kModuleOpen.entry, {kEngine, kGuestStreamOffset, 4});
            void* opened = _rwStreamModuleOpen(RwEngineInstance, nativeStreamOffset, 8);
            g.fail_call = -1, g_fail_malloc = -1;
            require((g.r3() == kEngine) == (opened == RwEngineInstance) && (g.r3() == 0) == (opened == nullptr) &&
                        m.peek32(kStreamModule + 4) == static_cast<u32>(streamModule.numInstances),
                    "_rwStreamModuleOpen's result");
            same_calls("_rwStreamModuleOpen's calls differ");
            if (opened == nullptr) {                                   // the module stays closed this session
                instructions += g.call(kModuleClose.entry, {kEngine, kGuestStreamOffset, 4});
                _rwStreamModuleClose(RwEngineInstance, nativeStreamOffset, 8);
                instructions += g.call(kMemoryClose.entry, {});
                _rwMemoryClose();
                same_calls("a failed session's calls differ");
                same_error("a failed session's error differs");
                continue;
            }

            std::vector<Pair> streams;
            std::vector<std::vector<u8>> sources;   // read / append sources stay alive for the session
            const unsigned ops = 30 + rng.next() % 90u;
            for (unsigned step = 0; step < ops; ++step) {
                const u32 op = rng.next() % 100u;
                if (op < 22 || streams.empty()) {                                       // open
                    const u32 kind = rng.next() % 16u;
                    const RwStreamType type = kind < 6 ? rwSTREAMMEMORY : kind < 9 ? rwSTREAMFILE
                                            : kind < 12 ? rwSTREAMFILENAME : kind < 15 ? rwSTREAMCUSTOM
                                            : static_cast<RwStreamType>(rng.next() % 2u ? 0 : 5);
                    const RwStreamAccessType access = static_cast<RwStreamAccessType>(rng.next() % 9u == 0 ? rng.next() % 5u
                                                                                                         : 1 + rng.next() % 3u);
                    u32 garg = 0;
                    const void* narg = nullptr;
                    RwMemory memory{};
                    RwStreamCustom custom{};
                    std::string name;
                    if (type == rwSTREAMMEMORY) {
                        sources.emplace_back(rng.next() % 900u);
                        for (u8& b : sources.back()) b = static_cast<u8>(rng.next());
                        const std::size_t length = sources.back().size();
                        u32 at = kIo + static_cast<u32>(sources.size() % 8u) * 0x800u;
                        RwUInt8* start = sources.back().data();
                        {
                            // a memory stream's block may grow by RwRealloc (append, or a write to any memory
                            // stream), so it is the engine allocator's on both sides: taken outside the recorded
                            // calls, and freed after close
                            at = g.heap_alloc(static_cast<u32>(length));
                            start = static_cast<RwUInt8*>(std::malloc(length + 1));
                            std::memcpy(start, sources.back().data(), length);
                        }
                        m.write_block(at, sources.back().data(), length);
                        m.write32(kMem, at), m.write32(kMem + 4, static_cast<u32>(length));
                        memory = {start, length};
                        garg = kMem, narg = &memory;
                    } else if (type == rwSTREAMFILE) {
                        FakeFile file;                                 // a file the caller opened
                        file.data.resize(rng.next() % 600u);
                        for (u8& b : file.data) b = static_cast<u8>(rng.next());
                        file.tellFails = rng.next() % 6u == 0;
                        g_guest_files.files.push_back(file), g_native_files.files.push_back(file);
                        const long f = static_cast<long>(g_guest_files.files.size()) - 1;
                        garg = handle(f), narg = reinterpret_cast<void*>(static_cast<std::uintptr_t>(f) + 1);
                    } else if (type == rwSTREAMFILENAME) {
                        name = "stage/file" + std::to_string(rng.next() % 1000u) + std::string(rng.next() % 30u, 'x');
                        for (std::size_t i = 0; i <= name.size(); ++i) m.write8(kName + static_cast<u32>(i), i < name.size() ? static_cast<u8>(name[i]) : 0);
                        if (rng.next() % 7u == 0) g_guest_files.fail_open_at = g_guest_files.opens, g_native_files.fail_open_at = g_native_files.opens;
                        garg = kName, narg = name.c_str();
                    } else if (type == rwSTREAMCUSTOM) {
                        const u32 data = rng.next();
                        const bool closes = rng.next() % 2u;
                        m.write32(kCustom, closes ? kCustomClose : 0u), m.write32(kCustom + 4, kCustomRead);
                        m.write32(kCustom + 8, kCustomWrite), m.write32(kCustom + 12, kCustomSkip), m.write32(kCustom + 16, data);
                        custom = {closes ? nCustomClose : nullptr, nCustomRead, nCustomWrite, nCustomSkip,
                                  reinterpret_cast<void*>(static_cast<std::uintptr_t>(data))};
                        garg = kCustom, narg = &custom;
                    }
                    instructions += g.call(kOpen.entry, {static_cast<u32>(type), static_cast<u32>(access), garg});
                    RwStream* s = RwStreamOpen(type, access, narg);
                    const u32 guest_stream = g.r3();
                    g_guest_files.fail_open_at = g_native_files.fail_open_at = -1;
                    require((guest_stream == 0) == (s == nullptr), "RwStreamOpen succeeded on one side only");
                    same_calls("RwStreamOpen's calls differ");
                    same_error("RwStreamOpen's error differs");
                    if (s != nullptr) {
                        streams.push_back({guest_stream, s, s->type == rwSTREAMMEMORY ? s->Type.memory.nSize : 0});
                        same_stream(streams.back(), "an opened stream differs");
                    }
                } else if (op < 47) {                                                   // read
                    const std::size_t k = rng.next() % streams.size();
                    const Pair& p = streams[k];
                    const std::size_t from = p.native->type == rwSTREAMMEMORY ? p.native->Type.memory.position : 0;
                    const u32 length = rng.next() % 700u;
                    std::vector<u8> native_bytes(length + 1u, 0xEE);
                    for (u32 i = 0; i < length; ++i) m.write8(kIo + 0x6000u + i, 0xEE);
                    instructions += g.call(kRead.entry, {p.guest, kIo + 0x6000u, length});
                    const RwUInt32 got = RwStreamRead(p.native, native_bytes.data(), length);
                    require(g.r3() == got, "RwStreamRead's result");
                    for (u32 i = 0; i < length; ++i) {
                        if (p.native->type == rwSTREAMMEMORY && from + i >= p.defined) break;
                        require(m.peek8(kIo + 0x6000u + i) == native_bytes[i], "RwStreamRead's bytes");
                    }
                    same_stream(p, "a stream differs after RwStreamRead");
                    same_calls("RwStreamRead's calls differ");
                    same_error("RwStreamRead's error differs");
                } else if (op < 72) {                                                   // write
                    Pair& p = streams[rng.next() % streams.size()];
                    const u32 length = rng.next() % 4u == 0 ? 0x200u + rng.next() % 900u : rng.next() % 300u;
                    std::vector<u8> bytes(length + 1u);
                    for (u32 i = 0; i < length; ++i) bytes[i] = static_cast<u8>(rng.next()), m.write8(kIo + 0x6000u + i, bytes[i]);
                    if (rng.next() % 20u == 0) g.fail_call = static_cast<int>(g.mallocs.size()), g_fail_malloc = static_cast<int>(g_mallocs.size());
                    if (rng.next() % 20u == 0) g.fail_realloc = static_cast<int>(g.reallocs.size()), g_fail_realloc = static_cast<int>(g_reallocs.size());
                    instructions += g.call(kWrite.entry, {p.guest, kIo + 0x6000u, length});
                    const std::size_t from = p.native->type == rwSTREAMMEMORY ? p.native->Type.memory.position : 0;
                    RwStream* r = RwStreamWrite(p.native, bytes.data(), length);
                    if (r != nullptr && p.native->type == rwSTREAMMEMORY && from <= p.defined) {   // the data prefix grows
                        p.defined = std::max(p.defined, p.native->Type.memory.position);
                    }
                    g.fail_call = g.fail_realloc = g_fail_malloc = g_fail_realloc = -1;
                    require((g.r3() == p.guest) == (r == p.native) && (g.r3() == 0) == (r == nullptr), "RwStreamWrite's result");
                    same_stream(p, "a stream differs after RwStreamWrite");
                    same_calls("RwStreamWrite's calls differ");
                    same_error("RwStreamWrite's error differs");
                } else if (op < 88) {                                                   // skip
                    const Pair& p = streams[rng.next() % streams.size()];
                    const u32 offset = rng.next() % 8u == 0 ? 0u : rng.next() % 500u;
                    instructions += g.call(kSkip.entry, {p.guest, offset});
                    RwStream* r = RwStreamSkip(p.native, offset);
                    require((g.r3() == p.guest) == (r == p.native) && (g.r3() == 0) == (r == nullptr), "RwStreamSkip's result");
                    same_stream(p, "a stream differs after RwStreamSkip");
                    same_calls("RwStreamSkip's calls differ");
                    same_error("RwStreamSkip's error differs");
                } else {                                                                // close
                    const std::size_t k = rng.next() % streams.size();
                    const Pair p = streams[k];
                    streams.erase(streams.begin() + static_cast<long>(k));
                    close_pair(p, rng.next() % 4u != 0);
                    same_calls("RwStreamClose's calls differ");
                    same_error("RwStreamClose's error differs");
                }
                ++checks;
            }
            for (const Pair& p : streams) close_pair(p, false);
            same_calls("the closing calls differ");
            instructions += g.call(kModuleClose.entry, {kEngine, kGuestStreamOffset, 4});
            require(g.r3() == kEngine && _rwStreamModuleClose(RwEngineInstance, nativeStreamOffset, 8) == RwEngineInstance &&
                        m.peek32(kStreamModule + 4) == static_cast<u32>(streamModule.numInstances),
                    "_rwStreamModuleClose");
            instructions += g.call(kMemoryClose.entry, {});
            _rwMemoryClose();
            same_calls("_rwMemoryClose's calls differ");
            same_error("the error after a session differs");
        }

        std::printf("rwcore_stream_tests: %u operations identical to the original (%llu original instructions)\n",
                    checks, static_cast<unsigned long long>(instructions));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_stream_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
