// Oracle for the native RenderWare binary chunks (rwcore/src/binary.cpp): the ORIGINAL babinary functions of PAL
// main.dol in the reference interpreter (rwcore_oracle.h) and the native ones read the same random little-endian
// chunk data through memory streams: headers with every chunk type, new and old library IDs, versions inside and
// outside the accepted window, chunks to skip, truncated data. The original converts what it reads to big-endian
// words; the oracle compares VALUES (the guest's words against the native's), every result, the stream position, the
// held error, and the bytes of written chunk headers. _rwChunkIsComplex is compared for every type 0..0x3F and
// random ones.
// liveness-roots: reference-interpreter-only
//
// Usage: rwcore_binary_tests <main.dol>
#include "rw/core/binary.h"
#include "rw/core/error.h"
#include "rw/core/memory.h"
#include "rw/core/stream.h"
#include "rwcore_oracle.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
using namespace rworacle;

constexpr Original kIsComplex{"_rwChunkIsComplex", 0x8047DD48u, 0xBCu, 0x80030000u, 0x4E800020u};
constexpr Original kReadHeader{"_rwStreamReadChunkHeader", 0x8047DE04u, 0x13Cu, 0x9421FFC0u, 0x4E800020u};
constexpr Original kWriteHeader{"_rwStreamWriteVersionedChunkHeader", 0x8047DF40u, 0x60u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kFindChunk{"RwStreamFindChunk", 0x8047DFA0u, 0x208u, 0x9421FFA0u, 0x4E800020u};
constexpr Original kReadInt16{"RwStreamReadInt16", 0x8047E6D4u, 0x100u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kReadReal{"RwStreamReadReal", 0x8047E7D4u, 0xE4u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kReadInt32{"RwStreamReadInt32", 0x8047E8B8u, 0xE4u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kReadInfo{"RwStreamReadChunkHeaderInfo", 0x8047E99Cu, 0x1C4u, 0x9421FFB0u, 0x4E800020u};
constexpr Original kStreamOpen{"RwStreamOpen", 0x804823B8u, 0x288u, 0x9421FFC0u, 0x4E800020u};
constexpr Original kStreamClose{"RwStreamClose", 0x8048229Cu, 0x11Cu, 0x9421FFE0u, 0x4E800020u};
constexpr Original kModuleOpen{"_rwStreamModuleOpen", 0x80481CBCu, 0x8Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kMemoryOpen{"_rwMemoryOpen", 0x804812C4u, 0x118u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kErrorOpen{"_rwErrorOpen", 0x8047EB88u, 0x38u, 0x390D6100u, 0x4E800020u};
constexpr Original kErrorGet{"RwErrorGet", 0x8047EC34u, 0x44u, 0x808D615Cu, 0x4E800020u};
constexpr u32 kFreeListAlloc = 0x80480E00u, kFreeListFree = 0x80480FA0u;
constexpr u32 kGuestErrorOffset = 0x200u, kGuestStreamOffset = 0x240u;
constexpr u32 kFuncs = kBuffer, kErr = kBuffer + 0x20u, kMem = kBuffer + 0x40u, kOut = kBuffer + 0x80u,
              kInfo = kBuffer + 0xC0u, kData = 0x81010000u, kValues = 0x81030000u;

void put32(std::vector<u8>& v, u32 w) {
    for (unsigned i = 0; i < 4; ++i) v.push_back(static_cast<u8>(w >> (8 * i)));
}

/// A library ID: new style for version / build, sometimes old style (version >> 8, no high half).
u32 library_id(Rng& rng, u32& version_out) {
    if (rng.next() % 5u == 0) {
        const u32 id = rng.next() & 0xFFFFu;
        version_out = id << 8;
        return id;
    }
    // now and then a version at the edges of RwStreamFindChunk's window (3.5.0.0 .. 3.7.0.2)
    constexpr u32 kEdges[] = {0x34F3Fu, 0x35000u, 0x35001u, 0x37001u, 0x37002u, 0x37003u, 0x37100u};
    const u32 version = rng.next() % 4u == 0 ? kEdges[rng.next() % 7u] : 0x30000u + (rng.next() % 0x9000u);
    version_out = version;
    return (((version - 0x30000u) << 14) & 0xFFC00000u) | ((version & 0x3Fu) << 16) | (rng.next() & 0xFFFFu);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rwcore_binary_tests main.dol");
        Guest g(argv[1]);
        for (const Original& f : {kIsComplex, kReadHeader, kWriteHeader, kFindChunk, kReadInt16, kReadReal, kReadInt32,
                                  kReadInfo, kStreamOpen, kStreamClose, kModuleOpen, kMemoryOpen, kErrorOpen, kErrorGet}) {
            g.check_pinned(f);
        }
        GuestMemory& m = g.memory();

        // both engines: memory (free lists), errors, streams
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

        Rng rng;
        u64 instructions = 0;
        unsigned checks = 0;
        const auto same_error = [&](const char* what) {
            instructions += g.call(kErrorGet.entry, {kErr});
            RwError e{};
            RwErrorGet(&e);
            require(m.peek32(kErr) == static_cast<u32>(e.pluginID) && m.peek32(kErr + 4) == static_cast<u32>(e.errorCode),
                    what);
        };

        // ---- _rwChunkIsComplex over every type ------------------------------------------------------------------
        for (u32 i = 0; i < 0x40u + 2000u; ++i) {
            const u32 type = i < 0x40u ? i : rng.next();
            m.write32(kInfo, type);
            instructions += g.call(kIsComplex.entry, {kInfo});
            const RwChunkHeaderInfo info{type, 0, 0, 0, 0};
            require(g.r3() == static_cast<u32>(_rwChunkIsComplex(&info)), "_rwChunkIsComplex differs");
            ++checks;
        }

        // ---- reading random chunk data ---------------------------------------------------------------------------
        for (unsigned round = 0; round < 3000; ++round) {
            std::vector<u8> data;
            const unsigned chunks = 1 + rng.next() % 6u;
            for (unsigned c = 0; c < chunks; ++c) {
                const u32 type = rng.next() % 4u == 0 ? rng.next() : rng.next() % 0x20u;
                u32 version = 0;
                const u32 id = library_id(rng, version);
                const u32 length = rng.next() % 64u;
                put32(data, type), put32(data, length), put32(data, id);
                for (u32 i = 0; i < length; ++i) data.push_back(static_cast<u8>(rng.next()));
            }
            if (rng.next() % 3u == 0) data.resize(rng.next() % (data.size() + 1));   // truncated
            m.write_block(kData, data.data(), data.size());
            m.write32(kMem, kData), m.write32(kMem + 4, static_cast<u32>(data.size()));
            RwMemory memory{data.data(), data.size()};
            instructions += g.call(kStreamOpen.entry, {rwSTREAMMEMORY, rwSTREAMREAD, kMem});
            const u32 gs = g.r3();
            RwStream* ns = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &memory);
            require(gs != 0 && ns != nullptr, "memory streams open");

            for (unsigned step = 0; step < 12; ++step) {
                const u32 op = rng.next() % 5u;
                const u32 position = static_cast<u32>(ns->Type.memory.position);
                if (op == 0) {                                                   // _rwStreamReadChunkHeader
                    const u32 mask = rng.next() % 16u;
                    for (u32 i = 0; i < 4; ++i) m.write32(kOut + 4 * i, 0xA5A5A5A5u);
                    RwUInt32 out[4] = {0xA5A5A5A5u, 0xA5A5A5A5u, 0xA5A5A5A5u, 0xA5A5A5A5u};
                    instructions += g.call(kReadHeader.entry, {gs, mask & 1u ? kOut : 0u, mask & 2u ? kOut + 4 : 0u,
                                                               mask & 4u ? kOut + 8 : 0u, mask & 8u ? kOut + 12 : 0u});
                    const RwBool r = _rwStreamReadChunkHeader(ns, mask & 1u ? &out[0] : nullptr, mask & 2u ? &out[1] : nullptr,
                                                              mask & 4u ? &out[2] : nullptr, mask & 8u ? &out[3] : nullptr);
                    require(g.r3() == static_cast<u32>(r), "_rwStreamReadChunkHeader's result");
                    for (u32 i = 0; i < 4; ++i) require(m.peek32(kOut + 4 * i) == out[i], "_rwStreamReadChunkHeader's outputs");
                } else if (op == 1) {                                            // RwStreamReadChunkHeaderInfo
                    for (u32 i = 0; i < 5; ++i) m.write32(kInfo + 4 * i, 0xA5A5A5A5u);
                    RwChunkHeaderInfo info{0xA5A5A5A5u, 0xA5A5A5A5u, 0xA5A5A5A5u, 0xA5A5A5A5u, static_cast<RwBool>(0xA5A5A5A5u)};
                    instructions += g.call(kReadInfo.entry, {gs, kInfo});
                    RwStream* r = RwStreamReadChunkHeaderInfo(ns, &info);
                    require((g.r3() == gs) == (r == ns) && (g.r3() == 0) == (r == nullptr), "RwStreamReadChunkHeaderInfo's result");
                    require(m.peek32(kInfo) == info.type && m.peek32(kInfo + 4) == info.length &&
                                m.peek32(kInfo + 8) == info.version && m.peek32(kInfo + 12) == info.buildNum &&
                                m.peek32(kInfo + 16) == static_cast<u32>(info.isComplex),
                            "RwStreamReadChunkHeaderInfo's header");
                } else if (op == 2) {                                            // RwStreamFindChunk
                    const u32 type = rng.next() % 0x20u;
                    const u32 mask = rng.next() % 4u;
                    m.write32(kOut, 0xA5A5A5A5u), m.write32(kOut + 4, 0xA5A5A5A5u);
                    RwUInt32 out[2] = {0xA5A5A5A5u, 0xA5A5A5A5u};
                    instructions += g.call(kFindChunk.entry, {gs, type, mask & 1u ? kOut : 0u, mask & 2u ? kOut + 4 : 0u});
                    const RwBool r = RwStreamFindChunk(ns, type, mask & 1u ? &out[0] : nullptr, mask & 2u ? &out[1] : nullptr);
                    require(g.r3() == static_cast<u32>(r), "RwStreamFindChunk's result");
                    require(m.peek32(kOut) == out[0] && m.peek32(kOut + 4) == out[1], "RwStreamFindChunk's outputs");
                } else {                                                         // RwStreamReadInt16 / Real / Int32
                    const u32 bytes = (rng.next() % 64u) & (op == 3 ? ~1u : ~3u);
                    std::vector<u8> native(bytes + 4, 0x5A);
                    for (u32 i = 0; i < bytes + 4; ++i) m.write8(kValues + i, 0x5A);
                    const Original& f = op == 3 ? kReadInt16 : rng.next() % 2u ? kReadReal : kReadInt32;
                    instructions += g.call(f.entry, {gs, kValues, bytes});
                    RwStream* r = f.entry == kReadInt16.entry ? RwStreamReadInt16(ns, reinterpret_cast<RwInt16*>(native.data()), bytes)
                                : f.entry == kReadReal.entry  ? RwStreamReadReal(ns, reinterpret_cast<RwReal*>(native.data()), bytes)
                                                              : RwStreamReadInt32(ns, reinterpret_cast<RwInt32*>(native.data()), bytes);
                    require((g.r3() == gs) == (r == ns) && (g.r3() == 0) == (r == nullptr), "an RwStreamRead* result");
                    // the values read (words wholly inside what arrived): the guest's big-endian words, the native's
                    const u32 got = std::min<u32>(bytes, static_cast<u32>(data.size()) - position);
                    const u32 size = op == 3 ? 2u : 4u;
                    for (u32 i = 0; i + size <= got; i += size) {
                        const u32 guest = size == 2 ? m.peek16(kValues + i) : m.peek32(kValues + i);
                        u32 value = 0;
                        std::memcpy(&value, native.data() + i, size);
                        require(guest == value, "an RwStreamRead* value differs");
                    }
                }
                require(m.peek32(gs + 0xC) == ns->Type.memory.position, "the stream position differs");
                same_error("the held error differs");
                ++checks;
            }
            instructions += g.call(kStreamClose.entry, {gs, 0});
            require(g.r3() == 1 && RwStreamClose(ns, nullptr) == 1, "close");
        }

        // ---- writing chunk headers ------------------------------------------------------------------------------
        for (unsigned round = 0; round < 2000; ++round) {
            m.write32(kMem, 0), m.write32(kMem + 4, 0);
            RwMemory none{nullptr, 0};
            instructions += g.call(kStreamOpen.entry, {rwSTREAMMEMORY, rwSTREAMWRITE, kMem});
            const u32 gs = g.r3();
            RwStream* ns = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMWRITE, &none);
            for (unsigned h = 0; h < 4; ++h) {
                const u32 type = rng.next(), size = rng.next(), build = rng.next();
                u32 version = 0;
                library_id(rng, version);
                if (rng.next() % 4u == 0) version = rng.next();
                instructions += g.call(kWriteHeader.entry, {gs, type, size, version, build});
                RwStream* r = _rwStreamWriteVersionedChunkHeader(ns, static_cast<RwInt32>(type), static_cast<RwInt32>(size),
                                                                 version, build);
                require(g.r3() == gs && r == ns, "_rwStreamWriteVersionedChunkHeader's result");
            }
            const u32 gblock = m.peek32(gs + 0x14);
            for (u32 i = 0; i < 48; ++i) require(m.peek8(gblock + i) == ns->Type.memory.memBlock[i], "a written header's bytes");
            RwMemory out{};
            instructions += g.call(kStreamClose.entry, {gs, kMem});
            require(g.r3() == 1 && RwStreamClose(ns, &out) == 1, "close");
            RwFree(out.start);
            g.heap_free(m.peek32(kMem));
            checks += 4;
        }

        // ---- writing values: RwStreamWriteInt32 / Real / Int16 ----------------------------------------------------
        // The original writes per 256 bytes, the native in one call, so a memory stream's capacity may differ; its
        // data and position may not. Sizes are whole words (a trailing part-word is not data: the original writes
        // its big-endian bytes, the native the first bytes of its little-endian form).
        constexpr Original kWriteReal{"RwStreamWriteReal", 0x8047E3A0u, 0x108u, 0x9421FEE0u, 0x4E800020u};
        constexpr Original kWriteInt32{"RwStreamWriteInt32", 0x8047E4A8u, 0x108u, 0x9421FEE0u, 0x4E800020u};
        constexpr Original kWriteInt16{"RwStreamWriteInt16", 0x8047E5B0u, 0x124u, 0x9421FEE0u, 0x4E800020u};
        for (const Original& f : {kWriteReal, kWriteInt32, kWriteInt16}) g.check_pinned(f);
        for (unsigned round = 0; round < 2000; ++round) {
            m.write32(kMem, 0), m.write32(kMem + 4, 0);
            RwMemory none{nullptr, 0};
            instructions += g.call(kStreamOpen.entry, {rwSTREAMMEMORY, rwSTREAMWRITE, kMem});
            const u32 gs = g.r3();
            RwStream* ns = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMWRITE, &none);
            for (unsigned w = 0; w < 3; ++w) {
                const u32 kind = rng.next() % 3u;
                const u32 size = kind == 2 ? 2u : 4u;
                const u32 count = rng.next() % 5u == 0 ? 0u : rng.next() % 400u;
                std::vector<u8> values(std::size_t{count} * size);
                for (u32 i = 0; i < count; ++i) {
                    const u32 v = kind == 1 ? std::bit_cast<u32>(rng.real()) : rng.next();
                    if (size == 2) {
                        const u16 h = static_cast<u16>(v);
                        std::memcpy(values.data() + 2 * i, &h, 2), m.write16(kValues + 2 * i, h);
                    } else {
                        std::memcpy(values.data() + 4 * i, &v, 4), m.write32(kValues + 4 * i, v);
                    }
                }
                const u32 bytes = count * size;
                const Original& f = kind == 0 ? kWriteInt32 : kind == 1 ? kWriteReal : kWriteInt16;
                instructions += g.call(f.entry, {gs, kValues, bytes});
                RwStream* r = kind == 0 ? RwStreamWriteInt32(ns, reinterpret_cast<const RwInt32*>(values.data()), bytes)
                            : kind == 1 ? RwStreamWriteReal(ns, reinterpret_cast<const RwReal*>(values.data()), bytes)
                                        : RwStreamWriteInt16(ns, reinterpret_cast<const RwInt16*>(values.data()), bytes);
                require(g.r3() == gs && r == ns, "an RwStreamWrite* result");
                require(m.peek32(gs + 0xC) == ns->Type.memory.position, "the written position differs");
            }
            const u32 gblock = m.peek32(gs + 0x14), written = m.peek32(gs + 0xC);
            for (u32 i = 0; i < written; ++i) require(m.peek8(gblock + i) == ns->Type.memory.memBlock[i], "a written value byte");
            same_error("an RwStreamWrite* error");
            RwMemory out{};
            instructions += g.call(kStreamClose.entry, {gs, kMem});
            require(g.r3() == 1 && RwStreamClose(ns, &out) == 1, "close");
            if (out.start != nullptr) RwFree(out.start), g.heap_free(m.peek32(kMem));
            checks += 3;
        }
        {   // a stream whose writes fail: NULL from every writer, nothing else
            constexpr u32 kFailWrite = 0x817F0060u;
            g.stub(kFailWrite, [](Cpu& c) { c.gpr[3] = 0; });
            m.write32(kMem, 0), m.write32(kMem + 4, 0), m.write32(kMem + 8, kFailWrite), m.write32(kMem + 12, 0);
            m.write32(kMem + 16, 0);
            RwStreamCustom failing{nullptr, nullptr, [](void*, const void*, RwUInt32) -> RwBool { return 0; }, nullptr, nullptr};
            instructions += g.call(kStreamOpen.entry, {rwSTREAMCUSTOM, rwSTREAMWRITE, kMem});
            const u32 gs = g.r3();
            RwStream* ns = RwStreamOpen(rwSTREAMCUSTOM, rwSTREAMWRITE, &failing);
            const RwInt32 ints[3] = {1, 2, 3};
            for (const Original& f : {kWriteReal, kWriteInt32, kWriteInt16}) {
                instructions += g.call(f.entry, {gs, kValues, 12});
                RwStream* r = f.entry == kWriteInt16.entry ? RwStreamWriteInt16(ns, reinterpret_cast<const RwInt16*>(ints), 12)
                            : f.entry == kWriteReal.entry ? RwStreamWriteReal(ns, reinterpret_cast<const RwReal*>(ints), 12)
                                                          : RwStreamWriteInt32(ns, ints, 12);
                require(g.r3() == 0 && r == nullptr, "a failed RwStreamWrite* must return NULL");
                same_error("a failed RwStreamWrite* error");
            }
            instructions += g.call(kStreamClose.entry, {gs, 0});
            require(g.r3() == 1 && RwStreamClose(ns, nullptr) == 1, "close");
        }

        std::printf("rwcore_binary_tests: %u checks identical to the original (%llu original instructions)\n", checks,
                    static_cast<unsigned long long>(instructions));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_binary_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
