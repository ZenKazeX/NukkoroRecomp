// Oracle for the native RenderWare plugin data chunks (rwcore/src/pluginstream.cpp): the ORIGINAL batkbin functions of
// PAL main.dol in the reference interpreter (rwcore_oracle.h) and the native ones handle the same random registries
// and extension chunks: plugins with and without read / write / size / always / rights callbacks, chunks of known and
// unknown plugins, wrong totals, bad versions, missing and truncated chunks. Compared: every result, every callback
// call (which, plugin offset and size, the chunk length and the bytes a reader consumed), the stream position, the
// held error, and every byte WriteDataChunks writes. Callbacks answer from one shared script on both sides.
// liveness-roots: reference-interpreter-only
//
// Usage: rwcore_pluginstream_tests <main.dol>
#include "rw/core/binary.h"
#include "rw/core/error.h"
#include "rw/core/memory.h"
#include "rw/core/plugin.h"
#include "rw/core/stream.h"
#include "rwcore_oracle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {
using namespace rworacle;

constexpr Original kAddStream{"_rwPluginRegistryAddPluginStream", 0x80482640u, 0x44u, 0x80630010u, 0x4E800020u};
constexpr Original kAddAlways{"_rwPluginRegistryAddPlgnStrmlwysCB", 0x80482684u, 0x3Cu, 0x80630010u, 0x4E800020u};
constexpr Original kAddRights{"_rwPluginRegistryAddPlgnStrmRightsCB", 0x804826C0u, 0x3Cu, 0x80630010u, 0x4E800020u};
constexpr Original kReadChunks{"_rwPluginRegistryReadDataChunks", 0x804826FCu, 0x1C4u, 0x9421FFD0u, 0x4E800020u};
constexpr Original kRights{"_rwPluginRegistryInvokeRights", 0x804828C0u, 0x8Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kGetSize{"_rwPluginRegistryGetSize", 0x8048294Cu, 0x84u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kWriteChunks{"_rwPluginRegistryWriteDataChunks", 0x804829D0u, 0x154u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kSkipChunks{"_rwPluginRegistrySkipDataChunks", 0x80482B24u, 0xC4u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kRegOpen{"_rwPluginRegistryOpen", 0x80482BE8u, 0x5Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRegClose{"_rwPluginRegistryClose", 0x80482CA8u, 0x134u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kAdd{"_rwPluginRegistryAddPlugin", 0x80482E18u, 0x2D4u, 0x9421FFC0u, 0x4E800020u};
constexpr Original kStreamOpen{"RwStreamOpen", 0x804823B8u, 0x288u, 0x9421FFC0u, 0x4E800020u};
constexpr Original kStreamClose{"RwStreamClose", 0x8048229Cu, 0x11Cu, 0x9421FFE0u, 0x4E800020u};
constexpr Original kModuleOpen{"_rwStreamModuleOpen", 0x80481CBCu, 0x8Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kMemoryOpen{"_rwMemoryOpen", 0x804812C4u, 0x118u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kErrorOpen{"_rwErrorOpen", 0x8047EB88u, 0x38u, 0x390D6100u, 0x4E800020u};
constexpr Original kErrorGet{"RwErrorGet", 0x8047EC34u, 0x44u, 0x808D615Cu, 0x4E800020u};
constexpr u32 kFreeListAlloc = 0x80480E00u, kFreeListFree = 0x80480FA0u;
constexpr u32 kReadStub = 0x817F0060u, kWriteStub = 0x817F0070u, kSizeStub = 0x817F0080u, kAlwaysStub = 0x817F0090u,
              kRightsStub = 0x817F00A0u;
constexpr u32 kGuestErrorOffset = 0x200u, kGuestStreamOffset = 0x240u;
constexpr u32 kFuncs = kBuffer, kErr = kBuffer + 0x20u, kMem = kBuffer + 0x40u, kReg = kBuffer + 0x80u,
              kObject = 0x81002000u, kData = 0x81010000u;

constexpr RwInt32 round8(RwInt32 v) { return (v + 7) & ~7; }

struct Seen {
    int kind;    // 0 read, 1 write, 2 size, 3 always, 4 rights
    u32 offset, size, a, b;
    std::string bytes;
    bool operator==(const Seen&) const = default;
};
std::vector<Seen> g_native_seen;
std::vector<u32> g_script;
std::size_t g_native_next = 0;
u32 script(std::size_t& next) { return g_script[next++ % g_script.size()]; }
unsigned char g_object[8];

RwStream* nRead(RwStream* stream, RwInt32 length, void* object, RwInt32 offset, RwInt32 size) {
    u32 want = static_cast<u32>(length) % 5u == 4u ? static_cast<u32>(length) / 2u : static_cast<u32>(length);
    const std::size_t left = stream->Type.memory.nSize - stream->Type.memory.position;   // reads what is there
    if (want > left) want = static_cast<u32>(left);
    std::string bytes(want, '\0');
    const RwUInt32 got = RwStreamRead(stream, bytes.data(), want);
    bytes.resize(got);
    g_native_seen.push_back({0, static_cast<u32>(offset), static_cast<u32>(round8(size)), static_cast<u32>(length),
                             object == g_object, bytes});
    return script(g_native_next) % 7u == 0 ? nullptr : stream;
}
RwStream* nWrite(RwStream* stream, RwInt32 length, const void* object, RwInt32 offset, RwInt32 size) {
    g_native_seen.push_back({1, static_cast<u32>(offset), static_cast<u32>(round8(size)), static_cast<u32>(length),
                             object == g_object, {}});
    return script(g_native_next) % 9u == 0 ? nullptr : stream;
}
RwInt32 nSize(const void* object, RwInt32 offset, RwInt32 size) {
    const u32 r = script(g_native_next);
    g_native_seen.push_back({2, static_cast<u32>(offset), static_cast<u32>(round8(size)), 0, object == g_object, {}});
    return r % 4u == 0 ? -static_cast<RwInt32>(r % 3u) : static_cast<RwInt32>(r % 40u);
}
RwBool nAlways(void* object, RwInt32 offset, RwInt32 size) {
    g_native_seen.push_back({3, static_cast<u32>(offset), static_cast<u32>(round8(size)), 0, object == g_object, {}});
    return script(g_native_next) % 8u != 0;
}
RwBool nRights(void* object, RwInt32 offset, RwInt32 size, RwUInt32 extra) {
    g_native_seen.push_back({4, static_cast<u32>(offset), static_cast<u32>(round8(size)), extra, object == g_object, {}});
    return script(g_native_next) % 2u;
}

void put32(std::vector<u8>& v, u32 w) {
    for (unsigned i = 0; i < 4; ++i) v.push_back(static_cast<u8>(w >> (8 * i)));
}
u32 library_id(u32 version, u32 build) {
    return (((version - 0x30000u) << 14) & 0xFFC00000u) | ((version & 0x3Fu) << 16) | (build & 0xFFFFu);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rwcore_pluginstream_tests main.dol");
        Guest g(argv[1]);
        for (const Original& f : {kAddStream, kAddAlways, kAddRights, kReadChunks, kRights, kGetSize, kWriteChunks,
                                  kSkipChunks, kRegOpen, kRegClose, kAdd, kStreamOpen, kStreamClose, kModuleOpen,
                                  kMemoryOpen, kErrorOpen, kErrorGet}) {
            g.check_pinned(f);
        }
        GuestMemory& m = g.memory();
        std::vector<Seen> guest_seen;
        std::size_t guest_next = 0;
        g.stub(kReadStub, [&](Cpu& c) {   // consumes from the memory stream directly: its position and block
            const u32 stream = c.gpr[3], length = c.gpr[4];
            const u32 want = length % 5u == 4u ? length / 2u : length;
            const u32 pos = m.peek32(stream + 0xC), size = m.peek32(stream + 0x10), block = m.peek32(stream + 0x14);
            const u32 got = std::min(want, size - pos);
            std::string bytes(got, '\0');
            m.peek_block(block + pos, reinterpret_cast<u8*>(bytes.data()), got);
            m.write32(stream + 0xC, pos + got);
            guest_seen.push_back({0, c.gpr[6], c.gpr[7], length, c.gpr[5] == kObject, bytes});
            c.gpr[3] = script(guest_next) % 7u == 0 ? 0u : stream;
        });
        g.stub(kWriteStub, [&](Cpu& c) {
            guest_seen.push_back({1, c.gpr[6], c.gpr[7], c.gpr[4], c.gpr[5] == kObject, {}});
            c.gpr[3] = script(guest_next) % 9u == 0 ? 0u : c.gpr[3];
        });
        g.stub(kSizeStub, [&](Cpu& c) {
            const u32 r = script(guest_next);
            guest_seen.push_back({2, c.gpr[4], c.gpr[5], 0, c.gpr[3] == kObject, {}});
            c.gpr[3] = r % 4u == 0 ? static_cast<u32>(-static_cast<int>(r % 3u)) : r % 40u;
        });
        g.stub(kAlwaysStub, [&](Cpu& c) {
            guest_seen.push_back({3, c.gpr[4], c.gpr[5], 0, c.gpr[3] == kObject, {}});
            c.gpr[3] = script(guest_next) % 8u != 0;
        });
        g.stub(kRightsStub, [&](Cpu& c) {
            guest_seen.push_back({4, c.gpr[4], c.gpr[5], c.gpr[6], c.gpr[3] == kObject, {}});
            c.gpr[3] = script(guest_next) % 2u;
        });

        // engines: memory (free lists), errors, streams, plugin registry
        std::vector<std::max_align_t> block(64 + sizeof(RwGlobals) / sizeof(std::max_align_t));
        RwEngineInstance = block.data();
        rwSrcGlobals() = RwGlobals{};
        RwMemoryFunctions host = rwHostMemoryFunctions();
        const RwInt32 nativeErrorOffset = static_cast<RwInt32>((sizeof(RwGlobals) + 15) & ~std::size_t{15});
        _rwErrorOpen(RwEngineInstance, nativeErrorOffset, 8);
        require(_rwMemoryOpen(&host) == 1, "native memory");
        rwSrcGlobals().memoryAlloc = _rwFreeListAllocReal;
        rwSrcGlobals().memoryFree = _rwFreeListFreeReal;
        require(_rwStreamModuleOpen(RwEngineInstance, nativeErrorOffset + 16, 4) != nullptr, "native streams");
        m.write32(kErrorModule, 0), m.write32(kErrorModule + 4, 0);
        g.call(kErrorOpen.entry, {kEngine, kGuestErrorOffset, 8});
        m.write32(kFuncs, kMallocStub), m.write32(kFuncs + 4, kFreeStub), m.write32(kFuncs + 8, kReallocStub);
        m.write32(kFuncs + 12, 0);
        g.call(kMemoryOpen.entry, {kFuncs});
        m.write32(kEngine + 0x118u, kFreeListAlloc), m.write32(kEngine + 0x11Cu, kFreeListFree);
        g.call(kModuleOpen.entry, {kEngine, kGuestStreamOffset, 4});
        require(g.r3() == kEngine, "guest streams");
        m.write32(0x805F2658u, 0);   // no engine started: plugins may register
        rwNumEngineInstances = 0;

        Rng rng;
        for (unsigned i = 0; i < 97; ++i) g_script.push_back(rng.next());
        u64 instructions = 0;
        unsigned checks = 0;
        const auto same = [&](const char* what) {
            require(guest_seen == g_native_seen, what);
            guest_seen.clear(), g_native_seen.clear();
            instructions += g.call(kErrorGet.entry, {kErr});
            RwError e{};
            RwErrorGet(&e);
            require(m.peek32(kErr) == static_cast<u32>(e.pluginID) && m.peek32(kErr + 4) == static_cast<u32>(e.errorCode),
                    what);
        };
        const auto open_read = [&](const std::vector<u8>& data, RwMemory& memory) {
            m.write_block(kData, data.data(), data.size());
            m.write32(kMem, kData), m.write32(kMem + 4, static_cast<u32>(data.size()));
            memory = {const_cast<u8*>(data.data()), data.size()};
            instructions += g.call(kStreamOpen.entry, {rwSTREAMMEMORY, rwSTREAMREAD, kMem});
            const u32 gs = g.r3();
            RwStream* ns = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &memory);
            require(gs != 0 && ns != nullptr, "streams open");
            return std::pair{gs, ns};
        };

        for (unsigned round = 0; round < 1500; ++round) {
            instructions += g.call(kRegOpen.entry, {});
            require(g.r3() == 1 && _rwPluginRegistryOpen() == 1, "registry open");
            const RwInt32 orig = round8(static_cast<RwInt32>(rng.next() % 64u));
            RwPluginRegistry reg = rwPLUGINREGISTRYINIT(orig);
            m.write32(kReg, static_cast<u32>(orig)), m.write32(kReg + 4, static_cast<u32>(orig));
            for (u32 off = 8; off < 0x18; off += 4) m.write32(kReg + off, 0);
            std::vector<u32> ids;
            const unsigned plugins = rng.next() % 7u;
            for (unsigned i = 0; i < plugins; ++i) {
                const u32 id = 0x100u + rng.next() % 12u;
                const RwInt32 size = static_cast<RwInt32>(rng.next() % 40u);
                instructions += g.call(kAdd.entry, {kReg, static_cast<u32>(round8(size)), id, 0, 0, 0});
                require(g.r3() == static_cast<u32>(_rwPluginRegistryAddPlugin(&reg, size, id, nullptr, nullptr, nullptr)),
                        "AddPlugin");
                ids.push_back(id);
            }
            // stream callbacks for some IDs (registered or not)
            for (unsigned i = 0; i < 6; ++i) {
                const u32 id = 0x100u + rng.next() % 14u;
                const u32 pick = rng.next();
                const u32 which = pick % 3u;
                if (which == 0) {
                    const bool r = pick & 8u, w = pick & 16u, s = pick & 32u;
                    instructions += g.call(kAddStream.entry, {kReg, id, r ? kReadStub : 0u, w ? kWriteStub : 0u, s ? kSizeStub : 0u});
                    require(g.r3() == static_cast<u32>(_rwPluginRegistryAddPluginStream(&reg, id, r ? nRead : nullptr,
                                                                                        w ? nWrite : nullptr,
                                                                                        s ? nSize : nullptr)),
                            "_rwPluginRegistryAddPluginStream's result");
                } else if (which == 1) {
                    instructions += g.call(kAddAlways.entry, {kReg, id, kAlwaysStub});
                    require(g.r3() == static_cast<u32>(_rwPluginRegistryAddPlgnStrmlwysCB(&reg, id, nAlways)),
                            "_rwPluginRegistryAddPlgnStrmlwysCB's result");
                } else {
                    instructions += g.call(kAddRights.entry, {kReg, id, kRightsStub});
                    require(g.r3() == static_cast<u32>(_rwPluginRegistryAddPlgnStrmRightsCB(&reg, id, nRights)),
                            "_rwPluginRegistryAddPlgnStrmRightsCB's result");
                }
            }

            // an extension chunk
            std::vector<u8> body;
            const unsigned chunks = rng.next() % 5u;
            for (unsigned c = 0; c < chunks; ++c) {
                const u32 id = 0x100u + rng.next() % 14u, length = rng.next() % 24u;
                put32(body, id), put32(body, length), put32(body, library_id(0x36000u + rng.next() % 0x1003u, 0x37));
                for (u32 i = 0; i < length; ++i) body.push_back(static_cast<u8>(rng.next()));
            }
            std::vector<u8> data;
            const u32 shape = rng.next() % 12u;
            if (shape == 0) put32(data, rwID_STRUCT), put32(data, 0), put32(data, library_id(0x37002u, 0x37));  // skipped first
            const u32 version = shape == 1 ? 0x37003u : shape == 2 ? 0x34000u : 0x37002u;
            const u32 total = shape == 3 ? static_cast<u32>(body.size()) + 12u : static_cast<u32>(body.size());
            put32(data, rwID_EXTENSION), put32(data, total), put32(data, library_id(version, 0x37));
            data.insert(data.end(), body.begin(), body.end());
            if (shape == 4) data.resize(rng.next() % (data.size() + 1));
            for (unsigned i = 0; i < 8; ++i) data.push_back(static_cast<u8>(rng.next()));   // what follows the object

            // read, skip
            for (unsigned pass = 0; pass < 2; ++pass) {
                RwMemory memory{};
                const auto [gs, ns] = open_read(data, memory);
                if (pass == 0) {
                    instructions += g.call(kReadChunks.entry, {kReg, gs, kObject});
                    const RwPluginRegistry* r = _rwPluginRegistryReadDataChunks(&reg, ns, g_object);
                    require((g.r3() == kReg) == (r == &reg) && (g.r3() == 0) == (r == nullptr),
                            "_rwPluginRegistryReadDataChunks' result");
                } else {
                    instructions += g.call(kSkipChunks.entry, {kReg, gs});
                    const RwPluginRegistry* r = _rwPluginRegistrySkipDataChunks(&reg, ns);
                    require((g.r3() == kReg) == (r == &reg) && (g.r3() == 0) == (r == nullptr),
                            "_rwPluginRegistrySkipDataChunks' result");
                }
                require(m.peek32(gs + 0xC) == ns->Type.memory.position, "the stream position differs");
                same(pass == 0 ? "_rwPluginRegistryReadDataChunks' calls / error differ"
                               : "_rwPluginRegistrySkipDataChunks' calls / error differ");
                instructions += g.call(kStreamClose.entry, {gs, 0});
                require(g.r3() == 1 && RwStreamClose(ns, nullptr) == 1, "close");
            }

            // size, write
            instructions += g.call(kGetSize.entry, {kReg, kObject});
            require(g.r3() == static_cast<u32>(_rwPluginRegistryGetSize(&reg, g_object)), "_rwPluginRegistryGetSize");
            same("_rwPluginRegistryGetSize's calls differ");
            {
                m.write32(kMem, 0), m.write32(kMem + 4, 0);
                RwMemory none{nullptr, 0};
                instructions += g.call(kStreamOpen.entry, {rwSTREAMMEMORY, rwSTREAMWRITE, kMem});
                const u32 gs = g.r3();
                RwStream* ns = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMWRITE, &none);
                instructions += g.call(kWriteChunks.entry, {kReg, gs, kObject});
                const RwPluginRegistry* r = _rwPluginRegistryWriteDataChunks(&reg, ns, g_object);
                require((g.r3() == kReg) == (r == &reg) && (g.r3() == 0) == (r == nullptr),
                        "_rwPluginRegistryWriteDataChunks' result");
                same("_rwPluginRegistryWriteDataChunks' calls differ");
                const u32 written = m.peek32(gs + 0xC);
                require(written == ns->Type.memory.position, "written length");
                for (u32 i = 0; i < written; ++i) {
                    require(m.peek8(m.peek32(gs + 0x14) + i) == ns->Type.memory.memBlock[i], "a written byte differs");
                }
                RwMemory out{};
                instructions += g.call(kStreamClose.entry, {gs, kMem});
                require(g.r3() == 1 && RwStreamClose(ns, &out) == 1, "close");
                if (out.start != nullptr) RwFree(out.start), g.heap_free(m.peek32(kMem));
            }

            // rights
            for (unsigned i = 0; i < 3; ++i) {
                const u32 id = 0x100u + rng.next() % 14u, extra = rng.next();
                instructions += g.call(kRights.entry, {kReg, id, kObject, extra});
                const RwPluginRegistry* r = _rwPluginRegistryInvokeRights(&reg, id, g_object, extra);
                require((g.r3() == kReg) == (r == &reg) && (g.r3() == 0) == (r == nullptr),
                        "_rwPluginRegistryInvokeRights' result");
                same("_rwPluginRegistryInvokeRights' calls differ");
            }

            instructions += g.call(kRegClose.entry, {});
            require(g.r3() == 1 && _rwPluginRegistryClose() == 1, "registry close");
            checks += 8;
        }

        std::printf("rwcore_pluginstream_tests: %u checks identical to the original (%llu original instructions)\n",
                    checks, static_cast<unsigned long long>(instructions));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_pluginstream_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
