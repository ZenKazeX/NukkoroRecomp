// Oracle for the native RenderWare world reader (rpworld/src/world.cpp): the ORIGINAL baworld / basector / babinwor /
// bamatlst / bamesh / world-light functions of PAL main.dol in the reference interpreter (rwcore_oracle.h) and the
// native ones read the same random platform-independent world streams - the shape tools/stage_convert writes: a world
// struct (0x40 bytes, or 0x20 .. 0x3F of it), a material list (materials read or referring to an earlier one, with
// and without textures from a pool in the current dictionary), plane sectors (0x18-byte structs, or shorter ones;
// overlapping sectors or not; BSP chains up to 60 deep) and atomic sectors whose struct chunk holds the 0x2C header and
// then the vertex / normal / prelit / texture-coordinate (0 .. 8 sets) / triangle arrays (the 3.5 / 3.6.0.0 triangle
// order included), each with an extension of the 0x50E mesh chunk {flags, meshes, indices, per mesh count, material,
// int32 indices}, a sector plugin chunk and unknown chunks; then the world's extension (a world plugin chunk, unknown
// chunks). Malformed streams: versions outside 3.5.0.0 .. 3.7.0.2 in any chunk, truncation anywhere, container chunks
// of the wrong type (9 / 0xA / 7 / 8 / 3 / unknown in place of each other), a positive colSectorSize, a material count
// past the materials, a mesh count below the meshes written, mesh totals above the indices, world totals above the
// tree's, failing plugin reads, failing allocations (the world block, the n-th mesh header, the material array).
// Worlds live on (up to 4) while lights are created, framed, moved, given radii, added to and removed from them
// (positional ones tied to sectors at every frame sync), sectors walked (RpWorldForAllWorldSectors with a callback
// that stops) and looked up (RpWorldSectorGetWorld, RpLightGetWorld), and worlds destroyed.
// After every operation: every world field by field (object bytes, flags, render order, the material list - count,
// space, each slot's material as the first slot holding it - texture coordinate sets, clumps, both light lists in
// order, origin, box, render callback, pipeline, plugin data), the BSP tree (plane type, value, left / right values,
// children), each atomic sector (counts, window base, every array's presence and contents, repEntry, atomic and light
// tie lists in order, both boxes bit for bit, pipeline, pad, plugin data, the mesh header and each mesh's indices
// position, count, material and every index), every material (texture, colour, surface, references), every pool
// texture's references, every light (type, world, its sector ties in order), the plugin constructor / destructor
// calls in order (a sector destructor records whether its mesh was still there, a world destructor its material
// count), results, the held error.
//
// The guest engine (the world module without the GameCube pipelines):
// - memory, error, plugin registry, string, vector, matrix, stream, frame, texture, light and material modules opened
//   by their originals, as in rpworld_material_tests / rpworld_light_tests;
// - _rpMeshOpen (0x8045D00C) called as its module open (registered by RpWorldPluginAttach at 0x804618A4, size 0x30);
// - WorldOpen (0x80460AA0) is not called: it opens the GameCube pipelines first (_rpWorldPipelineOpen, bl 0x804637E8
//   at 0x80460AB8). Its other steps are done by hand: the globals offset stored at r13 + 0x60A8 (0x80460AB4), the
//   registry's free list RwFreeListCreateAndPreallocateSpace(0x10, 8, 4, 1, 0x805E26D8, 0x0004000B) (0x80460ACC ..
//   0x80460AEC, the original called) stored at globals + 0 (0x80460AF8), the list head at globals + 4 self-linked
//   (0x80460B1C .. 0x80460B3C), numInstances (r13 + 0x60AC) incremented (0x80460B40);
// - _rpWorldObjRegisterExtensions (0x80462E70) is not called whole: it also registers the camera, atomic, clump and
//   geometry extensions, the GameCube's native sector data 0x510 and the rights-to-render readers, none of which the
//   native reader has. Its steps the reader needs are the originals called with its arguments: the world-object module
//   open WorldObjectOpen (0x804619C4, registered at 0x80462E98 with globals size 0xC: the light ties' free list); the
//   light extension RpLightRegisterPlugin(8, 0x509, 0x80462598, 0x80462604, 0x804625C0) (0x80462F44) with its offset
//   stored at r13 + 0x60BC (0x80462F48, by hand); RpWorldSectorRegisterPlugin(0, 0x50E, 0, 0, 0) (0x80462F80) and
//   RpWorldSectorRegisterPluginStream(0x50E, 0x80462DCC, 0x80462D78, 0x80462E38) (0x80462FC8);
// - the texture read callback is a stub that finds nothing (the original's would read image files);
// - the guest heap behind rwmalloc / rwfree / rwrealloc is the test's own (first fit, freed ranges merged: world
//   blocks of every size come and go, and the harness's heap does not merge).
// The test's own plugins: a material plugin, a world plugin and a sector plugin (4 bytes each) with constructor /
// destructor stubs; the world / sector plugins' stream reader is a small guest routine (written into guest memory)
// that calls the original RwStreamReadInt32 into the plugin slot and fails when the word read has the top byte 0xFA.
//
// Allowed and counted: the original runs a sector's or a world's plugin destructor on plugin data never constructed
// when a read fails before its _rwPluginRegistryInitObject (0x80456250 / 0x80456C9C; spec 3.2, 3.3); the native does
// not (rwcore/README.md "Native contracts" -> Worlds). Such a guest destructor call (its object never had a
// constructor call) is dropped from the comparison and counted; the native must have none.
// Not generated (deliberate differences, rpworld_world_native_tests): world structs longer than 0x40 and plane structs
// longer than 0x18 (the original overflows its stack buffers), GameCube-native worlds (rpWORLDNATIVE: refused), more
// than 8 texture coordinate sets, world totals below the tree's (the original overruns its block), sector counts above
// 0xFFFF, sector struct chunks shorter or longer than their arrays, triangle vertex indices outside the sector, plane
// types other than 0 / 4 / 8, a truncation inside a prelit array (the original accepts a short prelit read), a
// texture's plugin data failing (a bad version or a cut at its extension header: the original keeps the reference
// RwTextureStreamRead took - a texture contract, rpworld_material_native_tests), a cut inside a 32-bit word of the
// material list struct or a mesh chunk (a short RwStreamReadInt32 converts the whole word, and the original reads
// those into its stack or a fresh heap block: the rest of the word is whatever memory held), negative
// material counts and material indices that are not an earlier entry, mesh material indices outside the list, mesh
// indices past the total, sectors without a mesh chunk (the original builds one; the mesh generators are not ported),
// a world whose registration failed, lights destroyed or worlds destroyed while lights are in them, a light added twice.
// liveness-roots: reference-interpreter-only
//
// Usage: rpworld_world_tests <main.dol>
#include "rw/core/binary.h"
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/frame.h"
#include "rw/core/stream.h"
#include "rw/core/texture.h"
#include "rw/world/light.h"
#include "rw/world/material.h"
#include "rw/world/world.h"
#include "rwcore_oracle.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace rworacle;

constexpr Original kWorldStreamRead{"RpWorldStreamRead", 0x80456744u, 0x5F0u, 0x9421FF60u, 0x4E800020u};
constexpr Original kAtomicSectorRead{"AtomicSectorRead", 0x80455D34u, 0x5C8u, 0x9421FF60u, 0x4E800020u};
constexpr Original kPlaneSectorRead{"PlaneSectorRead", 0x804562FCu, 0x448u, 0x9421FF90u, 0x4E800020u};
constexpr Original kMatListStreamRead{"_rpMaterialListStreamRead", 0x8045C974u, 0x60Cu, 0x9421FFA0u, 0x4E800020u};
constexpr Original kMeshRead{"_rpMeshRead", 0x8045D7C0u, 0x258u, 0x9421FBB0u, 0x4E800020u};
constexpr Original kMeshDestroy{"_rpMeshDestroy", 0x8045D324u, 0x6Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kMeshOpen{"_rpMeshOpen", 0x8045D00Cu, 0x188u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kReadSectorMesh{"readSectorMesh", 0x80462DCCu, 0x6Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSetupBoxes{"_rpWorldSetupSectorBoundingBoxes", 0x80460BF4u, 0x158u, 0x9421F8D0u, 0x4E800020u};
constexpr Original kBuildMissingMeshes{"WorldBuildMissingMeshes", 0x80460E00u, 0xF0u, 0x9421FEE0u, 0x4E800020u};
constexpr Original kWorldDestroy{"RpWorldDestroy", 0x80460FF0u, 0x180u, 0x9421FEE0u, 0x4E800020u};
constexpr Original kSectorDestroy{"SectorDestroySingleMalloc", 0x80460730u, 0xF0u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kSectorGetWorld{"RpWorldSectorGetWorld", 0x80460EF0u, 0xBCu, 0x9421FFE0u, 0x4E800020u};
constexpr Original kForAllSectors{"RpWorldForAllWorldSectors", 0x80461740u, 0xA0u, 0x9421FEE0u, 0x4E800020u};
constexpr Original kRegisterWorld{"_rpWorldRegisterWorld", 0x80460D4Cu, 0xB4u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSetRenderCB{"RpWorldSetSectorRenderCallBack", 0x80461170u, 0x18u, 0x28040000u, 0x4E800020u};
constexpr Original kWorldObjectOpen{"WorldObjectOpen", 0x804619C4u, 0x114u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kInitLightExt{"WorldInitLightExt", 0x80462598u, 0x28u, 0x800D60BCu, 0x4E800020u};
constexpr Original kDeInitLightExt{"WorldDeInitLightExt", 0x80462604u, 0xA4u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kCopyLightExt{"WorldCopyLightExt", 0x804625C0u, 0x44u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kWorldLightSync{"WorldLightSync", 0x804622F8u, 0x2A0u, 0x9421FEB0u, 0x4E800020u};
constexpr Original kAddLight{"RpWorldAddLight", 0x804635C8u, 0xA0u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRemoveLight{"RpWorldRemoveLight", 0x80463668u, 0xD4u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kLightGetWorld{"RpLightGetWorld", 0x8046373Cu, 0xCu, 0x800D60BCu, 0x4E800020u};
constexpr Original kAtomicTieDestroy{"AtomicTieDestroy", 0x80461E40u, 0x84u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kLightTieDestroy{"LightTieDestroy", 0x80461DD0u, 0x70u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSectorRegister{"RpWorldSectorRegisterPlugin", 0x80460348u, 0x40u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSectorRegisterStream{"RpWorldSectorRegisterPluginStream", 0x80460388u, 0x3Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kWorldRegister{"RpWorldRegisterPlugin", 0x804617E0u, 0x40u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kWorldRegisterStream{"RpWorldRegisterPluginStream", 0x80461820u, 0x3Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kFreeListCreatePre{"RwFreeListCreateAndPreallocateSpace", 0x80480C54u, 0x20u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kReadInt32{"RwStreamReadInt32", 0x8047E8B8u, 0xE4u, 0x9421FFE0u, 0x4E800020u};
// the modules and objects around them (pinned by rpworld_material_tests / rpworld_light_tests too)
constexpr Original kMatOpen{"_rpMaterialOpen", 0x8045C098u, 0x90u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kMatDestroy{"RpMaterialDestroy", 0x8045C244u, 0x7Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kMatSetTexture{"RpMaterialSetTexture", 0x8045C2C0u, 0x5Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kMatRegister{"RpMaterialRegisterPlugin", 0x8045C31Cu, 0x40u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kMatStreamRead{"RpMaterialStreamRead", 0x8045C398u, 0x3A8u, 0x9421FFB0u, 0x4E800020u};
constexpr Original kListDeinit{"_rpMaterialListDeinitialize", 0x8045C740u, 0x98u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kListGet{"_rpMaterialListGetMaterial", 0x8045C7ECu, 0x10u, 0x80630000u, 0x4E800020u};
constexpr Original kTextureOpen{"_rwTextureOpen", 0x8048E52Cu, 0x2E4u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kTextureCreate{"RwTextureCreate", 0x8048DD2Cu, 0xB0u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSetName{"RwTextureSetName", 0x8048DEA4u, 0xACu, 0x9421FFE0u, 0x4E800020u};
constexpr Original kAddTexture{"RwTexDictionaryAddTexture", 0x8048DFFCu, 0x50u, 0x80040004u, 0x4E800020u};
constexpr Original kStringOpen{"_rwStringOpen", 0x80483E70u, 0x140u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kLightOpen{"_rpLightOpen", 0x8045BEE8u, 0x90u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kLightCreate{"RpLightCreate", 0x8045BD84u, 0xECu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kLightDestroy{"RpLightDestroy", 0x8045BD24u, 0x60u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kLightRegister{"RpLightRegisterPlugin", 0x8045BAF0u, 0x40u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSetRadius{"RpLightSetRadius", 0x8045B7D4u, 0x40u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kLightSync{"LightSync", 0x8045B7D0u, 0x4u, 0x4E800020u, 0x4E800020u};
constexpr Original kFrameOpen{"_rwFrameOpen", 0x804878D8u, 0xA4u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kFrameCreate{"RwFrameCreate", 0x80487D50u, 0x12Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kFrameTranslate{"RwFrameTranslate", 0x80488600u, 0x9Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSetFrame{"_rwObjectHasFrameSetFrame", 0x8048E810u, 0x74u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kSyncDirty{"_rwFrameSyncDirty", 0x8048C3A4u, 0x6Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kMemoryOpen{"_rwMemoryOpen", 0x804812C4u, 0x118u, 0x9421FFF0u, 0x4E800020u};
constexpr Original kRegOpen{"_rwPluginRegistryOpen", 0x80482BE8u, 0x5Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kErrorOpen{"_rwErrorOpen", 0x8047EB88u, 0x38u, 0x390D6100u, 0x4E800020u};
constexpr Original kErrorGet{"RwErrorGet", 0x8047EC34u, 0x44u, 0x808D615Cu, 0x4E800020u};
constexpr Original kMatrixOpen{"_rwMatrixOpen", 0x8047F8DCu, 0xD4u, 0x9421FFE0u, 0x4E800020u};
constexpr Original kVectorOpen{"_rwVectorOpen", 0x8048363Cu, 0x398u, 0x9421FFB0u, 0x4E800020u};
constexpr Original kStreamModuleOpen{"_rwStreamModuleOpen", 0x80481CBCu, 0x8Cu, 0x9421FFF0u, 0x4E800020u};
constexpr Original kStreamOpen{"RwStreamOpen", 0x804823B8u, 0x288u, 0x9421FFC0u, 0x4E800020u};
constexpr Original kStreamClose{"RwStreamClose", 0x8048229Cu, 0x11Cu, 0x9421FFE0u, 0x4E800020u};
constexpr u32 kFreeListAlloc = 0x80480E00u, kFreeListFree = 0x80480FA0u;
constexpr u32 kDefaultRenderCB = 0x80460B60u;   // the GameCube's sector render callback (RpWorldSetSectorRenderCallBack)
// module infos {globalsOffset, numInstances} and statics (r13 = 0x805EC500)
constexpr u32 kStreamModule = 0x805F2628u, kTextureModule = 0x805F267Cu, kDefaultDict = 0x805F2678u,
              kMaterialModule = 0x805F2590u, kLightModule = 0x805F2580u, kMeshModule = 0x805F2598u,   // r13 + 0x6098
              kWorldModule = 0x805F25A8u,                                                            // r13 + 0x60A8
              kWorldObjModule = 0x805F25C0u,                                                         // r13 + 0x60C0
              kLightExtOffset = 0x805F25BCu,                                                         // r13 + 0x60BC
              kBuildMeshList = 0x805EEE70u,                                                          // r13 + 0x2970
              kRegistrySpace = 0x805E26D8u;                                                          // WorldOpen's in-place free list
constexpr u32 kGuestErrorOffset = 0x200u, kGuestVectorOffset = 0x210u, kGuestMatrixOffset = 0x240u,
              kGuestFrameOffset = 0x280u, kGuestStreamOffset = 0x290u, kGuestLightOffset = 0x2B0u,
              kGuestTextureOffset = 0x380u, kGuestMaterialOffset = 0x3C0u, kGuestMeshOffset = 0x3D0u,
              kGuestWorldOffset = 0x410u, kGuestWorldObjOffset = 0x420u;
constexpr u32 kTexGlobals = kEngine + kGuestTextureOffset, kWorldGlobals = kEngine + kGuestWorldOffset;
// stubs and the plugin reader routine
constexpr u32 kCtorW = 0x817F0200u, kDtorW = 0x817F0210u, kCtorS = 0x817F0220u, kDtorS = 0x817F0230u,
              kCtorM = 0x817F0240u, kDtorM = 0x817F0250u, kTexReadCB = 0x817F0260u, kSectorCB = 0x817F0270u,
              kPluginFail = 0x817F0280u, kPluginRead = 0x817F0300u;
constexpr u32 kFuncs = kBuffer, kErr = kBuffer + 0x20u, kName = kBuffer + 0x100u, kMem = kBuffer + 0x200u,
              kVec = kBuffer + 0x240u, kData = 0x81010000u, kDataEnd = kEngine;
constexpr u32 kPluginFailMark = 0xFA000000u;
constexpr u32 kMaterialPlugin = 0x7301u, kSectorPlugin = 0x7302u, kWorldPlugin = 0x7303u, kUnknownChunk = 0x7FFFu;

struct Seen {
    int kind;   // 0 constructor, 1 destructor
    int reg;    // 0 world, 1 sector, 2 material
    u32 tag;    // world: worldOrigin.x bits; sector: tightBoundingBox.inf.x bits; material: surface ambient bits
    u32 extra;  // world destructor: matList.numMaterials; sector destructor: mesh still there
    bool operator==(const Seen&) const = default;
};
std::vector<Seen> g_nseen;
RwInt32 g_nWorldOff = 0, g_nSectorOff = 0;
u32 bits(float f) { return std::bit_cast<u32>(f); }
void* nCtorW(void* o, RwInt32, RwInt32) { g_nseen.push_back({0, 0, bits(static_cast<RpWorld*>(o)->worldOrigin.x), 0}); return o; }
void* nDtorW(void* o, RwInt32, RwInt32) {
    const auto* w = static_cast<RpWorld*>(o);
    g_nseen.push_back({1, 0, bits(w->worldOrigin.x), static_cast<u32>(w->matList.numMaterials)});
    return o;
}
void* nCtorS(void* o, RwInt32, RwInt32) { g_nseen.push_back({0, 1, bits(static_cast<RpWorldSector*>(o)->tightBoundingBox.inf.x), 0}); return o; }
void* nDtorS(void* o, RwInt32, RwInt32) {
    const auto* s = static_cast<RpWorldSector*>(o);
    g_nseen.push_back({1, 1, bits(s->tightBoundingBox.inf.x), s->mesh != nullptr ? 1u : 0u});
    return o;
}
void* nCtorM(void* o, RwInt32, RwInt32) { g_nseen.push_back({0, 2, bits(static_cast<RpMaterial*>(o)->surfaceProps.ambient), 0}); return o; }
void* nDtorM(void* o, RwInt32, RwInt32) { g_nseen.push_back({1, 2, bits(static_cast<RpMaterial*>(o)->surfaceProps.ambient), 0}); return o; }
RwStream* nPluginRead(RwStream* stream, RwInt32, void* object, RwInt32 offset, RwInt32) {
    auto* slot = reinterpret_cast<RwInt32*>(static_cast<char*>(object) + offset);
    if (RwStreamReadInt32(stream, slot, 4) == nullptr) return nullptr;
    return (static_cast<u32>(*slot) & 0xFF000000u) == kPluginFailMark ? nullptr : stream;
}
std::vector<RpWorldSector*> g_nvisited;
int g_visitStop = 0;
RpWorldSector* nSectorCB(RpWorldSector* s, void*) {
    g_nvisited.push_back(s);
    return static_cast<int>(g_nvisited.size()) == g_visitStop ? nullptr : s;
}

// allocation failure on both sides: the n-th allocation with this hint (0 = the first) fails
u32 g_failHint = 0;
int g_failN = -1, g_nFailLeft = -1;
void* nMalloc(std::size_t size, RwUInt32 hint) {
    if (g_failHint != 0 && hint == g_failHint && g_nFailLeft-- == 0) return nullptr;
    return std::malloc(size ? size : 1);
}
void nFree(void* p) { std::free(p); }
void* nRealloc(void* p, std::size_t size, RwUInt32) { return std::realloc(p, size ? size : 1); }
void* nCalloc(std::size_t n, std::size_t size, RwUInt32) { return std::calloc(n ? n : 1, size ? size : 1); }
const RwMemoryFunctions kNativeMemory{nMalloc, nFree, nRealloc, nCalloc};

constexpr u32 libraryID(u32 v) { return (((v - 0x30000u) << 14) & 0xFFC00000u) | ((v & 0x3Fu) << 16) | 0x37u; }

/// A little-endian RenderWare stream being written: chunks whose lengths are patched when they end.
struct Writer {
    std::vector<u8> b;
    std::vector<std::size_t> open;
    struct Header {
        std::size_t at;
        bool retypable;   // a container whose wrong type a find skips (never a struct or a plugin chunk)
        bool versionable = true;
    };
    std::vector<Header> headers;
    std::vector<std::pair<std::size_t, std::size_t>> noCut;   // prelit arrays, textures' extension headers
    std::vector<std::pair<std::size_t, std::size_t>> wordCut;   // words read into uninitialised memory: cut between them
    void put(u32 w) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<u8>(w >> (8 * i))); }
    void put16(u32 h) { b.push_back(static_cast<u8>(h)), b.push_back(static_cast<u8>(h >> 8)); }
    void putF(float f) { put(std::bit_cast<u32>(f)); }
    void begin(u32 type, u32 version, bool retypable) {
        headers.push_back({b.size(), retypable});
        put(type), put(0), put(libraryID(version));
        open.push_back(b.size());
    }
    void end() {
        const std::size_t start = open.back();
        open.pop_back();
        const u32 length = static_cast<u32>(b.size() - start);
        for (int i = 0; i < 4; ++i) b[start - 8 + i] = static_cast<u8>(length >> (8 * i));
    }
    void patch(std::size_t at, u32 w) { for (int i = 0; i < 4; ++i) b[at + i] = static_cast<u8>(w >> (8 * i)); }
};

struct Box {
    float sup[3], inf[3];
};

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rpworld_world_tests main.dol");
        Guest g(argv[1]);
        for (const Original& f :
             {kWorldStreamRead, kAtomicSectorRead, kPlaneSectorRead, kMatListStreamRead, kMeshRead, kMeshDestroy, kMeshOpen,
              kReadSectorMesh, kSetupBoxes, kBuildMissingMeshes, kWorldDestroy, kSectorDestroy, kSectorGetWorld, kForAllSectors,
              kRegisterWorld, kSetRenderCB, kWorldObjectOpen, kInitLightExt, kDeInitLightExt, kCopyLightExt, kWorldLightSync,
              kAddLight, kRemoveLight, kLightGetWorld, kAtomicTieDestroy, kLightTieDestroy, kSectorRegister, kSectorRegisterStream,
              kWorldRegister, kWorldRegisterStream, kFreeListCreatePre, kReadInt32, kMatOpen, kMatDestroy, kMatSetTexture,
              kMatRegister, kMatStreamRead, kListDeinit, kListGet, kTextureOpen, kTextureCreate, kSetName, kAddTexture,
              kStringOpen, kLightOpen, kLightCreate, kLightDestroy, kLightRegister, kSetRadius, kLightSync, kFrameOpen,
              kFrameCreate, kFrameTranslate, kSetFrame, kSyncDirty, kMemoryOpen, kRegOpen, kErrorOpen, kErrorGet, kMatrixOpen,
              kVectorOpen, kStreamModuleOpen, kStreamOpen, kStreamClose}) {
            g.check_pinned(f);
        }
        GuestMemory& m = g.memory();
        Rng rng;
        u64 instructions = 0;

        // ---- stubs --------------------------------------------------------------------------------------------
        std::vector<Seen> gseen;
        g.stub(kCtorW, [&](Cpu& c) { gseen.push_back({0, 0, m.peek32(c.gpr[3] + 0x44u), 0}); });
        g.stub(kDtorW, [&](Cpu& c) { gseen.push_back({1, 0, m.peek32(c.gpr[3] + 0x44u), m.peek32(c.gpr[3] + 0x14u)}); });
        g.stub(kCtorS, [&](Cpu& c) { gseen.push_back({0, 1, m.peek32(c.gpr[3] + 0x6Cu), 0}); });
        g.stub(kDtorS, [&](Cpu& c) { gseen.push_back({1, 1, m.peek32(c.gpr[3] + 0x6Cu), m.peek32(c.gpr[3] + 0x78u) != 0 ? 1u : 0u}); });
        g.stub(kCtorM, [&](Cpu& c) { gseen.push_back({0, 2, m.peek32(c.gpr[3] + 0xCu), 0}); });
        g.stub(kDtorM, [&](Cpu& c) { gseen.push_back({1, 2, m.peek32(c.gpr[3] + 0xCu), 0}); });
        g.stub(kTexReadCB, [&](Cpu& c) { c.gpr[3] = 0; });
        std::vector<u32> gvisited;
        g.stub(kSectorCB, [&](Cpu& c) {
            gvisited.push_back(c.gpr[3]);
            if (static_cast<int>(gvisited.size()) == g_visitStop) c.gpr[3] = 0;
        });
        // the plugin reader's last step: r3 = the stream or NULL from RwStreamReadInt32, r4 = the slot
        g.stub(kPluginFail, [&](Cpu& c) {
            if (c.gpr[3] != 0 && (m.peek32(c.gpr[4]) & 0xFF000000u) == kPluginFailMark) c.gpr[3] = 0;
        });
        // the guest heap: first fit, freed ranges merged with their neighbours (world blocks of every size come and go;
        // the harness's heap does not merge)
        std::map<u32, u32> heapUsed, heapFree{{kHeap, kHeapEnd - kHeap}};
        std::string context;   // the operation running (for a message from inside a stub)
        const auto heapAlloc = [&](u32 n) -> u32 {
            const u32 need = (std::max(n, 1u) + 31u) & ~31u;
            for (auto it = heapFree.begin(); it != heapFree.end(); ++it) {
                if (it->second < need) continue;
                const u32 at = it->first, left = it->second - need;
                heapFree.erase(it);
                if (left) heapFree[at + need] = left;
                heapUsed[at] = need;
                return at;
            }
            require(false, "guest heap exhausted (" + std::to_string(n) + " bytes asked) at " + context);
            return 0;
        };
        const auto heapRelease = [&](u32 a) {
            const auto it = heapUsed.find(a);
            if (it == heapUsed.end()) return;
            u32 at = it->first, size = it->second;
            heapUsed.erase(it);
            auto next = heapFree.lower_bound(at);
            if (next != heapFree.end() && next->first == at + size) size += next->second, next = heapFree.erase(next);
            if (next != heapFree.begin()) {
                auto prev = std::prev(next);
                if (prev->first + prev->second == at) at = prev->first, size += prev->second, heapFree.erase(prev);
            }
            heapFree[at] = size;
        };
        int gFailLeft = -1;
        g.stub(kMallocStub, [&](Cpu& c) {
            const u32 size = c.gpr[3], hint = c.gpr[4];
            g.mallocs.push_back({size, hint});
            if (g_failHint != 0 && hint == g_failHint && gFailLeft-- == 0) {
                c.gpr[3] = 0;
                return;
            }
            c.gpr[3] = heapAlloc(size);
        });
        g.stub(kFreeStub, [&](Cpu& c) { heapRelease(c.gpr[3]); });
        g.stub(kReallocStub, [&](Cpu& c) {
            const u32 old = c.gpr[3], size = c.gpr[4];
            const u32 fresh = heapAlloc(size);
            if (const auto it = heapUsed.find(old); it != heapUsed.end()) {
                std::vector<u8> bytes(std::min(it->second, size));
                m.peek_block(old, bytes.data(), bytes.size());
                m.write_block(fresh, bytes.data(), bytes.size());
                heapRelease(old);
            }
            c.gpr[3] = fresh;
        });
        {   // the plugin reader: RwStreamReadInt32(stream, object + offset, 4), then b kPluginFail (which returns)
            const auto branch = [](u32 from, u32 to) { return 0x48000000u | ((to - from) & 0x03FFFFFCu); };
            const u32 code[] = {
                0x9421FFF0u,                                    // stwu r1, -0x10(r1)
                0x7C0802A6u,                                    // mflr r0
                0x90010014u,                                    // stw r0, 0x14(r1)
                0x93E1000Cu,                                    // stw r31, 0xc(r1)
                0x7FE53214u,                                    // add r31, r5, r6
                0x7FE4FB78u,                                    // mr r4, r31
                0x38A00004u,                                    // li r5, 4
                0x3D800000u | (kReadInt32.entry >> 16),         // lis r12, hi
                0x618C0000u | (kReadInt32.entry & 0xFFFFu),     // ori r12, r12, lo
                0x7D8903A6u,                                    // mtctr r12
                0x4E800421u,                                    // bctrl
                0x7FE4FB78u,                                    // mr r4, r31
                0x83E1000Cu,                                    // lwz r31, 0xc(r1)
                0x80010014u,                                    // lwz r0, 0x14(r1)
                0x7C0803A6u,                                    // mtlr r0
                0x38210010u,                                    // addi r1, r1, 0x10
                0u,                                             // b kPluginFail
            };
            for (u32 i = 0; i < std::size(code); ++i) {
                const u32 at = kPluginRead + 4u * i;
                m.write32(at, i + 1 == std::size(code) ? branch(at, kPluginFail) : code[i]);
            }
        }

        // ---- the guest engine -----------------------------------------------------------------------------------
        g.reset_engine();   // (the heap above is the test's own)
        for (u32 a : {kErrorModule, kVectorModule, kMatrixModule, kStreamModule, kTextureModule, kMaterialModule, kLightModule,
                      kMeshModule, kWorldModule, kWorldObjModule}) {
            m.write32(a, 0), m.write32(a + 4u, 0);
        }
        for (u32 a : {kDefaultDict, kLightExtOffset, kBuildMeshList}) m.write32(a, 0);
        m.write32(kFuncs, kMallocStub), m.write32(kFuncs + 4, kFreeStub), m.write32(kFuncs + 8, kReallocStub);
        m.write32(kFuncs + 12, 0);
        g.call(kMemoryOpen.entry, {kFuncs});
        m.write32(kEngine + 0x118u, kFreeListAlloc), m.write32(kEngine + 0x11Cu, kFreeListFree);
        g.call(kErrorOpen.entry, {kEngine, kGuestErrorOffset, 8});
        g.call(kRegOpen.entry, {});
        g.call(kStringOpen.entry, {});
        g.call(kVectorOpen.entry, {kEngine, kGuestVectorOffset, 8});
        g.call(kMatrixOpen.entry, {kEngine, kGuestMatrixOffset, 0x18});
        g.call(kStreamModuleOpen.entry, {kEngine, kGuestStreamOffset, 4});
        m.write32(0x805F2658u, 0);
        // plugins, in the native order: RpWorldPluginAttach's (the light extension, the sector mesh), then the test's
        g.call(kLightRegister.entry, {8, 0x509, kInitLightExt.entry, kDeInitLightExt.entry, kCopyLightExt.entry});
        require(static_cast<int>(g.r3()) >= 0x40, "the guest light extension");
        m.write32(kLightExtOffset, g.r3());
        g.call(kSectorRegister.entry, {0, 0x50E, 0, 0, 0});
        require(static_cast<int>(g.r3()) >= 0, "the guest sector mesh plugin");
        g.call(kSectorRegisterStream.entry, {0x50E, kReadSectorMesh.entry, 0x80462D78u, 0x80462E38u});
        require(static_cast<int>(g.r3()) >= 0, "the guest sector mesh stream");
        g.call(kMatRegister.entry, {4, kMaterialPlugin, kCtorM, kDtorM, 0});
        g.call(kWorldRegister.entry, {4, kWorldPlugin, kCtorW, kDtorW, 0});
        const u32 gWorldOff = g.r3();
        g.call(kWorldRegisterStream.entry, {kWorldPlugin, kPluginRead, 0, 0});
        g.call(kSectorRegister.entry, {4, kSectorPlugin, kCtorS, kDtorS, 0});
        const u32 gSectorOff = g.r3();
        g.call(kSectorRegisterStream.entry, {kSectorPlugin, kPluginRead, 0, 0});
        require(static_cast<int>(gWorldOff) >= 0x70 && static_cast<int>(gSectorOff) >= 0x88, "the guest test plugins");
        g.call(kFrameOpen.entry, {kEngine, kGuestFrameOffset, 4});
        g.call(kTextureOpen.entry, {kEngine, kGuestTextureOffset, 0x34});
        require(g.r3() == kEngine, "guest textures open");
        m.write32(kTexGlobals + 0x14u, kTexReadCB);
        g.call(kLightOpen.entry, {kEngine, kGuestLightOffset, 4});
        require(g.r3() == kEngine, "guest lights open");
        g.call(kMatOpen.entry, {kEngine, kGuestMaterialOffset, 4});
        require(g.r3() == kEngine, "guest materials open");
        g.call(kMeshOpen.entry, {kEngine, kGuestMeshOffset, 0x30});
        require(g.r3() == kEngine, "guest meshes open");
        // WorldOpen without its pipelines (see the header)
        m.write32(kWorldModule, kGuestWorldOffset);
        g.call(kFreeListCreatePre.entry, {0x10, 8, 4, 1, kRegistrySpace, 0x0004000Bu});
        require(g.r3() != 0, "the guest world registry");
        m.write32(kWorldGlobals, g.r3());
        m.write32(kWorldGlobals + 4u, kWorldGlobals + 4u), m.write32(kWorldGlobals + 8u, kWorldGlobals + 4u);
        m.write32(kWorldModule + 4u, m.peek32(kWorldModule + 4u) + 1u);
        g.call(kWorldObjectOpen.entry, {kEngine, kGuestWorldObjOffset, 0xC});
        require(g.r3() == kEngine && m.peek32(kWorldObjModule) == kGuestWorldObjOffset, "guest world objects open");
        const u32 gLightExt = m.peek32(kLightExtOffset);

        // ---- the native engine ----------------------------------------------------------------------------------
        RwEngineOpenParams params{nullptr};
        require(RwEngineInit(&kNativeMemory, 0, 0) == 1 && RpWorldPluginAttach() == 1, "native init");
        require(RpMaterialRegisterPlugin(4, kMaterialPlugin, nCtorM, nDtorM, nullptr) >= 0, "the native material plugin");
        g_nWorldOff = RpWorldRegisterPlugin(4, kWorldPlugin, nCtorW, nDtorW, nullptr);
        g_nSectorOff = RpWorldSectorRegisterPlugin(4, kSectorPlugin, nCtorS, nDtorS, nullptr);
        require(g_nWorldOff >= static_cast<RwInt32>(sizeof(RpWorld)) && g_nSectorOff >= static_cast<RwInt32>(sizeof(RpWorldSector)) &&
                    RpWorldRegisterPluginStream(kWorldPlugin, nPluginRead, nullptr, nullptr) >= 0 &&
                    RpWorldSectorRegisterPluginStream(kSectorPlugin, nPluginRead, nullptr, nullptr) >= 0,
                "the native test plugins");
        require(RwEngineOpen(&params) == 1 && RwEngineStart() == 1, "native start");
        RwTextureSetReadCallBack([](const RwChar*, const RwChar*) -> RwTexture* { return nullptr; });

        // ---- a pool of named textures in the current dictionary --------------------------------------------------
        struct TexPair {
            u32 g;
            RwTexture* n;
        };
        std::vector<TexPair> textures;
        std::vector<std::string> names;
        std::map<u32, int> gTexId;
        std::map<const void*, int> nTexId;
        const u32 gDict = m.peek32(kDefaultDict);
        const auto writeString = [&](u32 at, const std::string& s) {
            for (std::size_t i = 0; i <= s.size(); ++i) m.write8(at + static_cast<u32>(i), i < s.size() ? static_cast<u8>(s[i]) : 0);
        };
        for (int i = 0; i < 12; ++i) {
            std::string n = "wtex";
            for (int k = 0; k < 1 + i % 5; ++k) n.push_back("abcdefghijklmnopqrstuvwxyz"[rng.next() % 26u]);
            names.push_back(n);
            instructions += g.call(kTextureCreate.entry, {0});
            const u32 gt = g.r3();
            writeString(kName, n);
            g.call(kSetName.entry, {gt, kName});
            g.call(kAddTexture.entry, {gDict, gt});
            RwTexture* nt = RwTextureCreate(nullptr);
            RwTextureSetName(nt, n.c_str());
            RwTexDictionaryAddTexture(RwTexDictionaryGetCurrent(), nt);
            gTexId[gt] = i, nTexId[nt] = i;
            textures.push_back({gt, nt});
        }
        const auto gtid = [&](u32 a) { return a == 0 ? -10 : gTexId.count(a) ? gTexId[a] : -1; };
        const auto ntid = [&](const void* p) { return p == nullptr ? -10 : nTexId.count(p) ? nTexId[p] : -1; };

        // ---- objects alive ---------------------------------------------------------------------------------------
        struct WorldPair {
            u32 g;
            RpWorld* n;
            int id;
        };
        struct LightPair {
            u32 g;
            RpLight* n;
            int id;
            int world;   // index into worlds' ids, -1: none
        };
        std::vector<WorldPair> worlds;
        std::vector<LightPair> lights;
        std::vector<std::pair<u32, RwFrame*>> frames;
        std::map<u32, int> gWorldId, gLightId;
        std::map<const void*, int> nWorldId, nLightId;
        int nextWorldId = 0, nextLightId = 0;
        u32 tagCounter = 1;
        const auto nextTag = [&]() { return static_cast<float>(tagCounter++); };

        // ---- comparison --------------------------------------------------------------------------------------------
        std::set<std::pair<int, u32>> gLive, nLive;
        unsigned gUnconstructed = 0, nUnconstructed = 0;
        const auto filter = [](std::vector<Seen>& v, std::set<std::pair<int, u32>>& live, unsigned& dropped) {
            std::vector<Seen> out;
            for (const Seen& s : v) {
                if (s.reg == 2) out.push_back(s);
                else if (s.kind == 0) live.insert({s.reg, s.tag}), out.push_back(s);
                else if (live.erase({s.reg, s.tag}) != 0) out.push_back(s);
                else ++dropped;
            }
            v = out;
        };
        const auto sameCallbacks = [&](const std::string& what) {
            filter(gseen, gLive, gUnconstructed);
            filter(g_nseen, nLive, nUnconstructed);
            require(nUnconstructed == 0, what + ": a native destructor on plugin data never constructed");
            if (gseen != g_nseen) {
                std::string d = what + ": callbacks differ\n  original:";
                for (const Seen& s : gseen) d += " " + std::to_string(s.kind) + "/" + std::to_string(s.reg) + "/" + std::to_string(s.tag) + "/" + std::to_string(s.extra);
                d += "\n  native:  ";
                for (const Seen& s : g_nseen) d += " " + std::to_string(s.kind) + "/" + std::to_string(s.reg) + "/" + std::to_string(s.tag) + "/" + std::to_string(s.extra);
                require(false, d);
            }
            gseen.clear(), g_nseen.clear();
            instructions += g.call(kErrorGet.entry, {kErr});
            RwError e{};
            RwErrorGet(&e);
            char buf[96];
            std::snprintf(buf, sizeof buf, ": errors (original %08X/%08X, native %08X/%08X)", m.peek32(kErr), m.peek32(kErr + 4),
                          static_cast<u32>(e.pluginID), static_cast<u32>(e.errorCode));
            require(m.peek32(kErr) == static_cast<u32>(e.pluginID) && m.peek32(kErr + 4) == static_cast<u32>(e.errorCode), what + buf);
        };
        unsigned sectorsCompared = 0, planesCompared = 0, meshesCompared = 0, indicesCompared = 0, tiesCompared = 0,
                 maxDepthCompared = 0;
        const auto word = [&](u32 ga, u32 n, const std::string& what) {
            if (m.peek32(ga) != n) {
                char values[96];
                std::snprintf(values, sizeof values, " (original %08X, native %08X)", m.peek32(ga), n);
                require(false, what + values);
            }
        };
        // a guest list's entries (link - offset) in order
        const auto guestList = [&](u32 head, u32 offset) {
            std::vector<u32> out;
            for (u32 l = m.peek32(head); l != head; l = m.peek32(l)) {
                require(out.size() < 4096, "a guest list does not end");
                out.push_back(l - offset);
            }
            return out;
        };
        const auto sameWorld = [&](const WorldPair& p, const std::string& whatWorld) {
            const u32 gw = p.g;
            const RpWorld& nw = *p.n;
            const std::string what = whatWorld + " world " + std::to_string(p.id);
            require(m.peek8(gw) == nw.object.type && m.peek8(gw + 1) == nw.object.subType && m.peek8(gw + 2) == nw.object.flags &&
                        m.peek8(gw + 3) == nw.object.privateFlags && m.peek32(gw + 4) == 0 && nw.object.parent == nullptr,
                    what + ": object");
            word(gw + 0x08, nw.flags, what + ": flags");
            word(gw + 0x0C, static_cast<u32>(nw.renderOrder), what + ": render order");
            // materials: each slot as the first slot holding the same material
            const u32 gn = m.peek32(gw + 0x14);
            require(gn == static_cast<u32>(nw.matList.numMaterials) && m.peek32(gw + 0x18) == static_cast<u32>(nw.matList.space),
                    what + ": material count / space (original " + std::to_string(gn) + " / " + std::to_string(m.peek32(gw + 0x18)) +
                        ", native " + std::to_string(nw.matList.numMaterials) + " / " + std::to_string(nw.matList.space) + ")");
            require((m.peek32(gw + 0x10) == 0) == (nw.matList.materials == nullptr), what + ": material array");
            std::vector<u32> gm(gn);
            for (u32 i = 0; i < gn; ++i) gm[i] = m.peek32(m.peek32(gw + 0x10) + 4 * i);
            const auto gIndex = [&](u32 a) -> int {
                for (u32 i = 0; i < gn; ++i) if (gm[i] == a) return static_cast<int>(i);
                return a == 0 ? -10 : -1;
            };
            const auto nIndex = [&](const RpMaterial* a) -> int {
                for (RwInt32 i = 0; i < nw.matList.numMaterials; ++i) if (nw.matList.materials[i] == a) return i;
                return a == nullptr ? -10 : -1;
            };
            for (u32 i = 0; i < gn; ++i) {
                const RpMaterial& nm = *nw.matList.materials[i];
                const u32 a = gm[i];
                require(gIndex(a) == nIndex(&nm), what + ": material slot " + std::to_string(i));
                require(gtid(m.peek32(a)) == ntid(nm.texture) && m.peek8(a + 4) == nm.color.red && m.peek8(a + 5) == nm.color.green &&
                            m.peek8(a + 6) == nm.color.blue && m.peek8(a + 7) == nm.color.alpha && m.peek32(a + 8) == 0 && nm.pipeline == nullptr,
                        what + ": material texture / colour / pipeline");
                require(m.peek32(a + 0xC) == bits(nm.surfaceProps.ambient) && m.peek32(a + 0x10) == bits(nm.surfaceProps.specular) &&
                            m.peek32(a + 0x14) == bits(nm.surfaceProps.diffuse) &&
                            static_cast<RwInt16>(m.peek32(a + 0x18) >> 16) == nm.refCount,
                        what + ": material surface / references");
            }
            word(gw + 0x20, static_cast<u32>(nw.numTexCoordSets), what + ": texture coordinate sets");
            word(gw + 0x24, static_cast<u32>(nw.numClumpsInWorld), what + ": clumps");
            require(m.peek32(gw + 0x28) == gw + 0x2C && nw.currentClumpLink == &nw.clumpList.link, what + ": current clump link");
            require(m.peek32(gw + 0x2C) == gw + 0x2C && m.peek32(gw + 0x30) == gw + 0x2C && nw.clumpList.link.next == &nw.clumpList.link &&
                        nw.clumpList.link.prev == &nw.clumpList.link,
                    what + ": clump list empty");
            for (u32 k = 0; k < 2; ++k) {   // positional, then global lights, in order
                const u32 head = gw + (k == 0 ? 0x34u : 0x3Cu);
                const RwLinkList& nl = k == 0 ? nw.lightList : nw.directionalLightList;
                std::vector<int> gi, ni;
                for (u32 l : guestList(head, 0x34)) gi.push_back(gLightId.count(l) ? gLightId[l] : -1);
                for (const RwLLLink* l = nl.link.next; l != &nl.link; l = l->next) {
                    require(ni.size() < 4096, "a native list does not end");
                    const auto* light = reinterpret_cast<const RpLight*>(reinterpret_cast<const char*>(l) - offsetof(RpLight, inWorld));
                    ni.push_back(nLightId.count(light) ? nLightId[light] : -1);
                }
                require(gi == ni, what + (k == 0 ? ": light list" : ": directional light list"));
            }
            for (u32 i = 0; i < 3; ++i) word(gw + 0x44 + 4 * i, bits((&nw.worldOrigin.x)[i]), what + ": origin");
            for (u32 i = 0; i < 3; ++i) word(gw + 0x50 + 4 * i, bits((&nw.boundingBox.sup.x)[i]), what + ": box sup");
            for (u32 i = 0; i < 3; ++i) word(gw + 0x5C + 4 * i, bits((&nw.boundingBox.inf.x)[i]), what + ": box inf");
            require(m.peek32(gw + 0x68) == kDefaultRenderCB && nw.renderCallBack == nullptr, what + ": render callback");
            require(m.peek32(gw + 0x6C) == 0 && nw.pipeline == nullptr, what + ": pipeline");
            word(gw + gWorldOff, *reinterpret_cast<const u32*>(reinterpret_cast<const char*>(&nw) + g_nWorldOff), what + ": plugin data");
            // the tree, pre-order
            struct Item {
                u32 g;
                const RpSector* n;
                unsigned depth;
            };
            std::vector<Item> stack{{m.peek32(gw + 0x1C), nw.rootSector, 0}};
            while (!stack.empty()) {
                const Item it = stack.back();
                stack.pop_back();
                require((it.g == 0) == (it.n == nullptr), what + ": a sector on one side only");
                if (it.g == 0) continue;
                maxDepthCompared = std::max(maxDepthCompared, it.depth);
                const u32 gs = it.g;
                const RwInt32 type = static_cast<RwInt32>(m.peek32(gs));
                require(type == it.n->type, what + ": sector type");
                if (type >= 0) {
                    const auto& np = *reinterpret_cast<const RpPlaneSector*>(it.n);
                    word(gs + 4, bits(np.value), what + ": plane value");
                    word(gs + 0x10, bits(np.leftValue), what + ": plane left value");
                    word(gs + 0x14, bits(np.rightValue), what + ": plane right value");
                    stack.push_back({m.peek32(gs + 0xC), np.rightSubTree, it.depth + 1});
                    stack.push_back({m.peek32(gs + 8), np.leftSubTree, it.depth + 1});
                    ++planesCompared;
                    continue;
                }
                const auto& ns = *reinterpret_cast<const RpWorldSector*>(it.n);
                const std::string sw = what + " sector " + std::to_string(m.peek32(gs + 0x6C));
                const u32 nv = m.peek16(gs + 0x82), nt = m.peek16(gs + 0x84);
                require(m.peek16(gs + 0x80) == ns.matListWindowBase && nv == ns.numVertices && nt == ns.numTriangles &&
                            m.peek16(gs + 0x86) == ns.pad,
                        sw + ": window / counts / pad");
                const u32 gv = m.peek32(gs + 8), gnr = m.peek32(gs + 0xC), gp = m.peek32(gs + 0x30), gt = m.peek32(gs + 4);
                require((gv == 0) == (ns.vertices == nullptr) && (gnr == 0) == (ns.normals == nullptr) && (gp == 0) == (ns.preLitLum == nullptr) &&
                            (gt == 0) == (ns.triangles == nullptr),
                        sw + ": arrays present");
                if (gv) for (u32 i = 0; i < nv * 3; ++i) word(gv + 4 * i, bits((&ns.vertices->x)[i]), sw + ": vertex");
                if (gnr) for (u32 i = 0; i < nv * 4; ++i) require(m.peek8(gnr + i) == reinterpret_cast<const u8*>(ns.normals)[i], sw + ": normal");
                if (gp) for (u32 i = 0; i < nv * 4; ++i) require(m.peek8(gp + i) == reinterpret_cast<const u8*>(ns.preLitLum)[i], sw + ": prelit");
                for (u32 s = 0; s < 8; ++s) {
                    const u32 gc = m.peek32(gs + 0x10 + 4 * s);
                    require((gc == 0) == (ns.texCoords[s] == nullptr), sw + ": texture coordinates present");
                    if (gc) for (u32 i = 0; i < nv * 2; ++i) word(gc + 4 * i, bits((&ns.texCoords[s]->u)[i]), sw + ": texture coordinate");
                }
                if (gt) {
                    for (u32 i = 0; i < nt; ++i) {
                        const RpTriangle& t = ns.triangles[i];
                        require(m.peek16(gt + 8 * i) == t.vertIndex[0] && m.peek16(gt + 8 * i + 2) == t.vertIndex[1] &&
                                    m.peek16(gt + 8 * i + 4) == t.vertIndex[2] && m.peek16(gt + 8 * i + 6) == t.matIndex,
                                sw + ": triangle " + std::to_string(i));
                    }
                }
                require(m.peek32(gs + 0x34) == 0 && ns.repEntry == nullptr, sw + ": repEntry");
                require(m.peek32(gs + 0x38) == gs + 0x38 && m.peek32(gs + 0x3C) == gs + 0x38 &&
                            ns.collAtomicsInWorldSector.link.next == &ns.collAtomicsInWorldSector.link,
                        sw + ": atomic ties empty");
                {   // light ties in order
                    std::vector<int> gi, ni;
                    for (u32 tie : guestList(gs + 0x40, 0)) {
                        const u32 l = m.peek32(tie + 8);
                        require(m.peek32(tie + 0x14) == gs, sw + ": a guest tie's sector");
                        gi.push_back(gLightId.count(l) ? gLightId[l] : -1);
                    }
                    for (const RwLLLink* l = ns.lightsInWorldSector.link.next; l != &ns.lightsInWorldSector.link; l = l->next) {
                        const auto* tie = reinterpret_cast<const rpLightTie*>(l);
                        require(tie->sect == &ns && ni.size() < 4096, sw + ": a native tie's sector");
                        ni.push_back(nLightId.count(tie->light) ? nLightId[tie->light] : -1);
                    }
                    require(gi == ni, sw + ": light ties");
                    tiesCompared += static_cast<unsigned>(gi.size());
                }
                for (u32 i = 0; i < 3; ++i) word(gs + 0x48 + 4 * i, bits((&ns.boundingBox.sup.x)[i]), sw + ": box sup");
                for (u32 i = 0; i < 3; ++i) word(gs + 0x54 + 4 * i, bits((&ns.boundingBox.inf.x)[i]), sw + ": box inf");
                for (u32 i = 0; i < 3; ++i) word(gs + 0x60 + 4 * i, bits((&ns.tightBoundingBox.sup.x)[i]), sw + ": tight box sup");
                for (u32 i = 0; i < 3; ++i) word(gs + 0x6C + 4 * i, bits((&ns.tightBoundingBox.inf.x)[i]), sw + ": tight box inf");
                require(m.peek32(gs + 0x7C) == 0 && ns.pipeline == nullptr, sw + ": pipeline");
                word(gs + gSectorOff, *reinterpret_cast<const u32*>(reinterpret_cast<const char*>(&ns) + g_nSectorOff), sw + ": plugin data");
                const u32 gh = m.peek32(gs + 0x78);
                const RpMeshHeader* nh = ns.mesh;
                require((gh == 0) == (nh == nullptr), sw + ": mesh present");
                if (gh) {
                    const u32 count = m.peek16(gh + 4);
                    require(m.peek32(gh) == nh->flags && count == nh->numMeshes && m.peek16(gh + 6) == nh->serialNum &&
                                m.peek32(gh + 8) == nh->totalIndicesInMesh && m.peek32(gh + 0xC) == nh->firstMeshOffset,
                            sw + ": mesh header (serial original " + std::to_string(m.peek16(gh + 6)) + ", native " +
                                std::to_string(nh->serialNum) + ")");
                    const u32 gStore = gh + 0x10 + count * 0xC;
                    const RpMesh* meshes = RpMeshHeaderGetMeshes(nh);
                    const auto* nStore = reinterpret_cast<const RwUInt16*>(meshes + count);
                    for (u32 k = 0; k < count; ++k) {
                        const u32 gm2 = gh + 0x10 + 0xC * k;
                        const RpMesh& nm = meshes[k];
                        const u32 gix = m.peek32(gm2), gni = m.peek32(gm2 + 4);
                        require(gix >= gStore && (gix - gStore) % 2 == 0 && (gix - gStore) / 2 == static_cast<u32>(nm.indices - nStore) &&
                                    gni == nm.numIndices && gIndex(m.peek32(gm2 + 8)) == nIndex(nm.material),
                                sw + ": mesh " + std::to_string(k));
                        for (u32 i = 0; i < gni; ++i) require(m.peek16(gix + 2 * i) == nm.indices[i], sw + ": mesh index");
                        indicesCompared += gni;
                    }
                    ++meshesCompared;
                }
                ++sectorsCompared;
            }
        };
        const auto sameLights = [&](const std::string& what) {
            for (const LightPair& p : lights) {
                const std::string lw = what + " light " + std::to_string(p.id);
                require(m.peek8(p.g + 1) == p.n->object.object.subType, lw + ": type");
                const u32 gwld = m.peek32(p.g + gLightExt);
                const RpWorld* nwld = rpWorldLightExtension(p.n)->world;
                require((gwld == 0 ? -10 : gWorldId.count(gwld) ? gWorldId[gwld] : -1) ==
                            (nwld == nullptr ? -10 : nWorldId.count(nwld) ? nWorldId[nwld] : -1),
                        lw + ": world");
                std::vector<u32> gi, ni;   // sector tags of its ties, in order
                for (u32 tie : guestList(p.g + 0x2C, 0xC)) {
                    require(m.peek32(tie + 8) == p.g, lw + ": a guest tie's light");
                    gi.push_back(m.peek32(m.peek32(tie + 0x14) + 0x6C));
                }
                for (const RwLLLink* l = p.n->WorldSectorsInLight.link.next; l != &p.n->WorldSectorsInLight.link; l = l->next) {
                    const auto* tie = reinterpret_cast<const rpLightTie*>(reinterpret_cast<const char*>(l) - offsetof(rpLightTie, WorldSectorInLight));
                    require(tie->light == p.n && ni.size() < 4096, lw + ": a native tie's light");
                    ni.push_back(bits(tie->sect->tightBoundingBox.inf.x));
                }
                require(gi == ni, lw + ": sector ties");
            }
        };
        const auto sameTextures = [&](const std::string& what) {
            for (const TexPair& t : textures) {
                require(m.peek32(t.g + 0x54) == static_cast<u32>(t.n->refCount),
                        what + ": texture " + std::to_string(gTexId[t.g]) + " references (original " + std::to_string(m.peek32(t.g + 0x54)) +
                            ", native " + std::to_string(t.n->refCount) + ")");
            }
        };
        const auto sameAll = [&](const std::string& what) {
            for (const WorldPair& p : worlds) sameWorld(p, what);
            sameLights(what);
            sameTextures(what);
        };

        // ---- stream generation -------------------------------------------------------------------------------------
        unsigned oldTriangles = 0, deepTrees = 0, overlapWorlds = 0, shortWorldStructs = 0, shortPlaneStructs = 0, texturedMaterials = 0,
                 sharedMaterials = 0;
        const auto stringChunk = [&](Writer& w, const std::string& text) {
            const u32 length = (static_cast<u32>(text.size()) + 4u) & ~3u;
            w.begin(rwID_STRING, 0x36003u, false);
            std::vector<u8> chars(length, 0);
            std::memcpy(chars.data(), text.data(), text.size());
            w.b.insert(w.b.end(), chars.begin(), chars.end());
            w.end();
        };
        const auto validVersion = [&]() {
            const u32 v[] = {0x35000u, 0x35002u, 0x36000u, 0x36001u, 0x36002u, 0x36003u, 0x37002u};
            return rng.next() % 3u == 0 ? v[rng.next() % std::size(v)] : 0x36003u;
        };
        const auto realIn = [&](float lo, float hi) { return lo + (hi - lo) * static_cast<float>(rng.next() & 0xFFFFFF) / 16777216.0f; };
        struct Counts {
            u32 planes = 0, sectors = 0, nv = 0, nt = 0;
        };
        // the bytes 0 .. length of 32-bit words as the reader sees them (zero beyond)
        const auto effective = [](const u32* words, u32 count, u32 length) {
            std::vector<u32> e(words, words + count);
            for (u32 i = 0; i < count; ++i) {
                for (u32 byte = 0; byte < 4; ++byte) {
                    if (4 * i + byte >= length) e[i] &= ~(0xFFu << (8 * byte));
                }
            }
            return e;
        };
        const auto pluginChunk = [&](Writer& w, u32 id) {
            w.begin(id, 0x36003u, false);
            w.put(rng.next() % 60u == 0 ? (kPluginFailMark | (rng.next() & 0xFFFFFFu)) : (rng.next() & 0x7FFFFFFFu));
            w.end();
        };
        const auto unknownChunk = [&](Writer& w) {
            w.begin(kUnknownChunk, 0x36003u, false);
            for (u32 i = rng.next() % 4u; i > 0; --i) w.put(rng.next());
            w.end();
        };
        // an atomic sector, its arrays and extension
        const auto atomicSector = [&](Writer& w, const Box& box, u32 format, u32 nTex, u32 numMaterials, Counts& c) {
            w.begin(rwID_ATOMICSECT, 0x36003u, true);
            const u32 nv = rng.next() % 6u == 0 ? 0u : 1u + rng.next() % 40u;
            const u32 nt = nv == 0 ? 0u : rng.next() % 5u == 0 ? 0u : rng.next() % 41u;
            const u32 version = validVersion();
            oldTriangles += nt != 0 && version < 0x36002u;
            w.begin(rwID_STRUCT, version, false);
            w.put(rng.next() % 4u == 0 ? rng.next() % 8u : 0u), w.put(nt), w.put(nv);
            w.putF(nextTag());                                                    // inf.x: the sector's tag
            w.putF(realIn(box.inf[1], box.sup[1])), w.putF(realIn(box.inf[2], box.sup[2]));
            for (u32 a = 0; a < 3; ++a) w.putF(realIn(box.inf[a], box.sup[a]));   // sup
            w.put(rng.next() & 1u), w.put(rng.next());                             // collSectorPresent, unused
            for (u32 i = 0; i < nv; ++i) {
                for (u32 a = 0; a < 3; ++a) w.put(rng.next() % 50u == 0 ? rng.next() : std::bit_cast<u32>(realIn(box.inf[a], box.sup[a])));
            }
            if (nv != 0 && (format & 0x10u) != 0) for (u32 i = 0; i < nv; ++i) w.put(rng.next());
            if (nv != 0 && (format & 0x08u) != 0) {
                const std::size_t at = w.b.size();
                for (u32 i = 0; i < nv; ++i) w.put(rng.next());
                w.noCut.push_back({at, w.b.size()});
            }
            if (nv != 0) for (u32 s = 0; s < nTex; ++s) for (u32 i = 0; i < 2 * nv; ++i) w.putF(rng.real(-8, 8));
            for (u32 i = 0; i < nt; ++i) {
                const u32 v0 = rng.next() % nv, v1 = rng.next() % nv, v2 = rng.next() % nv, mat = rng.next() % 8u;
                if (version < 0x36002u) w.put16(mat), w.put16(v0), w.put16(v1), w.put16(v2);
                else w.put16(v0), w.put16(v1), w.put16(v2), w.put16(mat);
            }
            w.end();
            w.begin(rwID_EXTENSION, 0x36003u, true);
            const bool plugin = rng.next() % 2u == 0, unknown = rng.next() % 5u == 0, twoMeshes = rng.next() % 40u == 0;
            const u32 meshAt = rng.next() % 3u;
            for (u32 slot = 0; slot < 3; ++slot) {
                if (slot == meshAt) {
                    for (u32 copy = 0; copy < (twoMeshes ? 2u : 1u); ++copy) {
                        w.begin(rwID_BINMESHPLUGIN, 0x36003u, false);
                        const std::size_t meshBody = w.b.size();
                        const u32 meshes = numMaterials == 0 ? 0u : rng.next() % 5u;
                        std::vector<u32> counts(meshes);
                        u32 total = 0;
                        for (u32& k : counts) total += k = rng.next() % 30u;
                        const bool fewer = meshes > 1 && rng.next() % 30u == 0;   // a header count below the meshes written
                        w.put(rng.next() % 4u == 0 ? 1u : rng.next() % 10u == 0 ? rng.next() : 0u);
                        w.put(fewer ? meshes - 1 : meshes);
                        w.put(total + (rng.next() % 4u == 0 ? rng.next() % 10u : 0u));
                        for (u32 k = 0; k < meshes; ++k) {
                            w.put(counts[k]), w.put(rng.next() % numMaterials);
                            for (u32 i = 0; i < counts[k]; ++i) w.put(rng.next() % 8u == 0 ? rng.next() : (nv ? rng.next() % nv : 0u));
                        }
                        w.wordCut.push_back({meshBody, w.b.size()});
                        w.end();
                    }
                } else if (slot == (meshAt + 1) % 3) {
                    if (plugin) pluginChunk(w, kSectorPlugin);
                } else if (unknown) {
                    unknownChunk(w);
                }
            }
            w.end();
            w.end();
            ++c.sectors, c.nv += nv, c.nt += nt;
        };
        // a plane sector and its subtrees (chain: one child a leaf at every level, down to the depth)
        std::function<void(Writer&, const Box&, u32, u32, u32, Counts&, u32, u32, bool)> planeSector;
        planeSector = [&](Writer& w, const Box& box, u32 format, u32 nTex, u32 numMaterials, Counts& c, u32 depth, u32 maxDepth, bool chain) {
            w.begin(rwID_PLANESECT, 0x36003u, true);
            const u32 axis = rng.next() % 3u;
            const float value = realIn(box.inf[axis], box.sup[axis]);
            const float span = box.sup[axis] - box.inf[axis];
            const bool leaves = depth + 1 >= maxDepth;
            u32 leftAtomic = leaves || (chain ? rng.next() % 2u == 0 : rng.next() % 3u == 0) ? 1u : 0u;
            u32 rightAtomic = leaves || (chain ? leftAtomic == 0 : rng.next() % 3u == 0) ? 1u : 0u;
            if (rng.next() % 9u == 0 && leftAtomic != 0) leftAtomic = rng.next() | 1u;   // any non-zero word: a leaf
            u32 length = 0x18;
            if (rng.next() % 8u == 0 && depth + 3 < maxDepth && !chain) length = rng.next() % 0x18u, ++shortPlaneStructs;
            const u32 words[6] = {axis * 4, bits(value), leftAtomic, rightAtomic, bits(value + realIn(0, span / 8)),
                                  bits(value - realIn(0, span / 8))};
            const std::vector<u32> e = effective(words, 6, length);
            w.begin(rwID_STRUCT, validVersion(), false);
            for (u32 i = 0; i < length; ++i) w.b.push_back(static_cast<u8>(words[i / 4] >> (8 * (i % 4))));
            w.end();
            const u32 ax = e[0] / 4;
            const float v = std::bit_cast<float>(e[1]);
            const bool overlap = (format & 0x40000000u) != 0;
            Box left = box, right = box;
            left.sup[ax] = overlap ? std::bit_cast<float>(e[4]) : v;
            right.inf[ax] = overlap ? std::bit_cast<float>(e[5]) : v;
            ++c.planes;
            for (u32 side = 0; side < 2; ++side) {
                const Box& b = side == 0 ? left : right;
                if (e[2 + side] != 0) atomicSector(w, b, format, nTex, numMaterials, c);
                else planeSector(w, b, format, nTex, numMaterials, c, depth + 1, maxDepth, chain);
            }
            w.end();
        };
        struct Stream {
            Writer w;
            Box box;
            const char* kind = "valid";
        };
        const auto makeWorld = [&]() {
            Stream st;
            Writer& w = st.w;
            u32 format = 0;
            for (u32 bit : {0x01u, 0x04u, 0x08u, 0x10u, 0x20u, 0x40u, 0x80u}) if (rng.next() % 2u) format |= bit;
            if (rng.next() % 4u == 0) format |= (rng.next() % 9u) << 16;
            if (rng.next() % 3u == 0) format |= 0x40000000u;
            if (rng.next() % 6u == 0) format |= (rng.next() & 0x3E00FF00u) & ~0x01000000u;   // other bits, kept as flags
            Box box;
            for (u32 a = 0; a < 3; ++a) {
                box.inf[a] = realIn(-2000, 0);
                box.sup[a] = box.inf[a] + realIn(10, 3000);
            }
            st.box = box;
            const bool rootAtomic = rng.next() % 6u == 0;
            u32 col = 0;
            if (rng.next() % 25u == 0) col = rng.next() % 2u ? 1u + rng.next() % 0x7Fu : 0x80u | (rng.next() & 0x7Fu);   // raw positive / not
            u32 words[16] = {rootAtomic ? (rng.next() % 5u == 0 ? rng.next() | 1u : 1u) : 0u,
                             bits(-nextTag()), bits(realIn(-100, 100)), bits(realIn(-100, 100)),
                             0, 0, 0, 0, col, format,
                             bits(box.sup[0]), bits(box.sup[1]), bits(box.sup[2]), bits(box.inf[0]), bits(box.inf[1]), bits(box.inf[2])};
            u32 length = 0x40;
            if (rng.next() % 10u == 0) length = 0x20 + rng.next() % 0x20u, ++shortWorldStructs;
            const std::vector<u32> e = effective(words, 16, length);
            const u32 eformat = e[9];
            overlapWorlds += (eformat & 0x40000000u) != 0;
            const u32 nTex = (eformat & 0x00FF0000u) ? (eformat & 0x00FF0000u) >> 16 : (eformat & 0x80u) ? 2u : (eformat >> 2) & 1u;
            w.begin(rwID_STRUCT, validVersion(), true);
            const std::size_t structAt = w.b.size();
            for (u32 i = 0; i < length; ++i) w.b.push_back(static_cast<u8>(words[i / 4] >> (8 * (i % 4))));
            w.end();
            // the material list
            const u32 materials = rng.next() % 7u;
            const u32 listed = materials + (rng.next() % 40u == 0 ? 1u + rng.next() % 3u : 0u);   // a count past the materials
            w.begin(rwID_MATLIST, 0x36003u, true);
            w.begin(rwID_STRUCT, validVersion(), false);
            const std::size_t listAt = w.b.size();
            w.put(listed);
            std::vector<int> refs(materials);
            for (u32 i = 0; i < materials; ++i) {
                refs[i] = i > 0 && rng.next() % 4u == 0 ? static_cast<int>(rng.next() % i) : -1;
                sharedMaterials += refs[i] >= 0;
                w.put(static_cast<u32>(refs[i]));
            }
            for (u32 i = materials; i < listed; ++i) w.put(~0u);
            w.wordCut.push_back({listAt, w.b.size()});
            w.end();
            for (u32 i = 0; i < materials; ++i) {
                if (refs[i] >= 0) continue;
                w.begin(rwID_MATERIAL, 0x36003u, true);
                w.begin(rwID_STRUCT, 0x36003u, false);
                const bool textured = rng.next() % 2u == 0;
                w.put(rng.next()), w.put(rng.next()), w.put(rng.next()), w.put(textured ? 1u : 0u);
                w.putF(nextTag()), w.putF(realIn(0, 1)), w.putF(realIn(0, 1));
                w.end();
                if (textured) {
                    ++texturedMaterials;
                    w.begin(rwID_TEXTURE, 0x36003u, false);
                    w.begin(rwID_STRUCT, 0x36003u, false);
                    w.put((rng.next() % 7u) | ((rng.next() % 5u) << 8) | ((rng.next() % 5u) << 12) | ((rng.next() & 1u) << 16));
                    w.end();
                    std::string name = rng.next() % 4u ? names[rng.next() % names.size()] : "missing" + std::to_string(rng.next() % 1000u);
                    for (char& ch : name) if (rng.next() % 3u == 0 && ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 32);
                    stringChunk(w, name);
                    stringChunk(w, "");
                    // the texture's plugin data must read: when it fails the original keeps the reference
                    // RwTextureStreamRead took (a texture contract, rpworld_material_native_tests)
                    w.noCut.push_back({w.b.size() - 1, w.b.size() + 12});
                    w.begin(rwID_EXTENSION, 0x36003u, false);
                    w.headers.back().versionable = false;
                    w.end();
                    w.end();
                }
                w.begin(rwID_EXTENSION, 0x36003u, false);
                w.end();
                w.end();
            }
            w.end();
            // the tree
            Counts c;
            if (e[0] != 0) {
                atomicSector(w, box, eformat, nTex, materials, c);
            } else {
                const bool chain = rng.next() % 25u == 0;
                const u32 maxDepth = chain ? 30u + rng.next() % 31u : 1u + rng.next() % 5u;
                deepTrees += chain;
                planeSector(w, box, eformat, nTex, materials, c, 0, maxDepth, chain);
            }
            // the world's extension
            w.begin(rwID_EXTENSION, 0x36003u, true);
            if (rng.next() % 2u) pluginChunk(w, kWorldPlugin);
            if (rng.next() % 5u == 0) unknownChunk(w);
            w.end();
            // the totals (above the tree's now and then)
            const auto more = [&]() { return rng.next() % 4u == 0 ? rng.next() % 50u : 0u; };
            w.patch(structAt + 0x10, c.nt + more()), w.patch(structAt + 0x14, c.nv + more());
            w.patch(structAt + 0x18, c.planes + more()), w.patch(structAt + 0x1C, c.sectors + more());
            if (col != 0 && length > 0x20) st.kind = "colSectorSize";
            if (listed != materials) st.kind = "material count";
            return st;
        };

        // ---- one stream through both readers ------------------------------------------------------------------------
        const auto readBoth = [&](const std::vector<u8>& bytes) -> std::pair<u32, RpWorld*> {
            require(bytes.size() <= kDataEnd - kData, "a stream too long for the guest's buffer");
            m.write_block(kData, bytes.data(), bytes.size());
            m.write32(kMem, kData), m.write32(kMem + 4, static_cast<u32>(bytes.size()));
            RwMemory memory{const_cast<u8*>(bytes.data()), bytes.size()};
            instructions += g.call(kStreamOpen.entry, {rwSTREAMMEMORY, rwSTREAMREAD, kMem});
            const u32 gs = g.r3();
            RwStream* ns = RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD, &memory);
            gFailLeft = g_failN, g_nFailLeft = g_failN;
            instructions += g.call(kWorldStreamRead.entry, {gs});
            const u32 gw = g.r3();
            RpWorld* nw = RpWorldStreamRead(ns);
            g_failHint = 0;
            instructions += g.call(kStreamClose.entry, {gs, 0});
            RwStreamClose(ns, nullptr);
            return {gw, nw};
        };
        const auto removeLight = [&](LightPair& p, const std::string& what) {
            const WorldPair* wp = nullptr;
            for (const WorldPair& w : worlds) if (w.id == p.world) wp = &w;
            require(wp != nullptr, what + ": the light's world");
            instructions += g.call(kRemoveLight.entry, {wp->g, p.g});
            require(g.r3() == wp->g && RpWorldRemoveLight(wp->n, p.n) == wp->n, what + ": RpWorldRemoveLight");
            p.world = -1;
        };
        const auto destroyWorld = [&](std::size_t k, const std::string& what) {
            for (LightPair& p : lights) if (p.world == worlds[k].id) removeLight(p, what);
            instructions += g.call(kWorldDestroy.entry, {worlds[k].g});
            require(g.r3() == 1 && RpWorldDestroy(worlds[k].n) == 1, what + ": RpWorldDestroy");
            gWorldId.erase(worlds[k].g), nWorldId.erase(worlds[k].n);
            worlds.erase(worlds.begin() + static_cast<long>(k));
        };

        unsigned checks = 0, read = 0, failed = 0, gotVisits = 0, visitStops = 0, lookups = 0, syncs = 0, adds = 0, removes = 0;
        std::map<std::string, unsigned> kinds, kindsFailed;
        std::string lastKind = "-";
        for (unsigned step = 0; step < 6000; ++step) {
            const u32 op = rng.next() % 100u;
            const std::string what = "op " + std::to_string(op) + " step " + std::to_string(step);
            if (op < 32) {                                                                      // read a world
                if (worlds.size() >= 4) destroyWorld(rng.next() % worlds.size(), what + " (room)"), sameCallbacks(what + " (room)");
                Stream st = makeWorld();
                std::vector<u8>& b = st.w.b;
                const u32 bad = rng.next() % 100u;
                if (bad < 10) {                                                                 // truncated
                    std::size_t cut = 0;
                    for (int tries = 0; tries < 100; ++tries) {
                        cut = rng.next() % b.size();
                        bool inside = false;
                        for (const auto& r : st.w.noCut) inside = inside || (cut > r.first && cut < r.second);
                        for (const auto& r : st.w.wordCut) inside = inside || (cut > r.first && cut < r.second && (cut - r.first) % 4 != 0);
                        if (!inside) break;
                        cut = b.size();
                    }
                    b.resize(cut);
                    st.kind = "truncated";
                } else if (bad < 18) {                                                          // a version outside the window
                    const Writer::Header* hp = &st.w.headers[rng.next() % st.w.headers.size()];
                    while (!hp->versionable) hp = &st.w.headers[rng.next() % st.w.headers.size()];
                    const auto& h = *hp;
                    st.w.patch(h.at + 8, libraryID(rng.next() % 2u ? 0x34000u : 0x37003u));
                    st.kind = "bad version";
                } else if (bad < 24) {                                                          // a container of the wrong type
                    std::vector<std::size_t> at;
                    for (const auto& h : st.w.headers) if (h.retypable) at.push_back(h.at);
                    const std::size_t h = at[rng.next() % at.size()];
                    const u32 types[] = {rwID_EXTENSION, rwID_MATERIAL, rwID_MATLIST, rwID_ATOMICSECT, rwID_PLANESECT, kUnknownChunk};
                    u32 type = types[rng.next() % std::size(types)];
                    if (type == static_cast<u32>(b[h] | (b[h + 1] << 8))) type = kUnknownChunk;
                    st.w.patch(h, type);
                    st.kind = "wrong type";
                } else if (bad < 30) {                                                          // an allocation fails
                    const u32 hints[] = {0x0003000Bu, 0x00030502u, 0x01030008u};
                    g_failHint = hints[rng.next() % 3u];
                    g_failN = static_cast<int>(rng.next() % 3u);
                    st.kind = g_failHint == 0x0003000Bu ? "no world block" : g_failHint == 0x00030502u ? "no mesh header" : "no material array";
                }
                const std::string kind = st.kind;
                lastKind = kind;
                context = what + " (" + kind + ")";
                const auto [gw, nw] = readBoth(b);
                ++kinds[kind];
                require((gw == 0) == (nw == nullptr), what + " (" + kind + "): RpWorldStreamRead succeeded on one side only (original " +
                                                          (gw ? "a world" : "NULL") + ")");
                if (nw != nullptr) {
                    ++read;
                    worlds.push_back({gw, nw, nextWorldId});
                    gWorldId[gw] = nextWorldId, nWorldId[nw] = nextWorldId;
                    ++nextWorldId;
                } else {
                    ++failed, ++kindsFailed[kind];
                }
            } else if (op < 38 && !worlds.empty()) {                                           // destroy a world
                destroyWorld(rng.next() % worlds.size(), what);
            } else if (op < 46) {                                                              // create / destroy a light
                if (lights.size() < 10 && (lights.empty() || rng.next() % 3u)) {
                    const u32 types[] = {rpLIGHTDIRECTIONAL, rpLIGHTAMBIENT, rpLIGHTPOINT, rpLIGHTSPOT, rpLIGHTSPOTSOFT};
                    const u32 type = types[rng.next() % std::size(types)];
                    instructions += g.call(kLightCreate.entry, {type});
                    RpLight* nl = RpLightCreate(static_cast<RwInt32>(type));
                    require(g.r3() != 0 && nl != nullptr, what + ": RpLightCreate");
                    lights.push_back({g.r3(), nl, nextLightId, -1});
                    gLightId[g.r3()] = nextLightId, nLightId[nl] = nextLightId;
                    ++nextLightId;
                } else {
                    std::vector<std::size_t> free;
                    for (std::size_t i = 0; i < lights.size(); ++i) if (lights[i].world < 0) free.push_back(i);
                    if (free.empty()) continue;
                    const std::size_t k = free[rng.next() % free.size()];
                    instructions += g.call(kLightDestroy.entry, {lights[k].g});
                    require(g.r3() == 1 && RpLightDestroy(lights[k].n) == 1, what + ": RpLightDestroy");
                    gLightId.erase(lights[k].g), nLightId.erase(lights[k].n);
                    lights.erase(lights.begin() + static_cast<long>(k));
                }
            } else if (op < 62 && !lights.empty()) {                                           // frames, positions, radii, syncs
                LightPair& p = lights[rng.next() % lights.size()];
                const u32 k = rng.next() % 4u;
                if (k == 0 || RpLightGetFrame(p.n) == nullptr) {
                    if (frames.size() < 8 || rng.next() % 4u == 0) {
                        instructions += g.call(kFrameCreate.entry, {});
                        frames.push_back({g.r3(), RwFrameCreate()});
                    }
                    const auto& f = frames[rng.next() % frames.size()];
                    instructions += g.call(kSetFrame.entry, {p.g, f.first});
                    RpLightSetFrame(p.n, f.second);
                } else if (k == 1) {
                    const float r = rng.next() % 5u == 0 ? realIn(0, 3000) : realIn(0, 300);
                    instructions += g.call(kSetRadius.entry, {p.g}, {double(r)});
                    require(g.r3() == p.g && RpLightSetRadius(p.n, r) == p.n, what + ": RpLightSetRadius");
                } else {   // the frame moved inside (or near) a world's box
                    RwV3d v{realIn(-2000, 1000), realIn(-2000, 1000), realIn(-2000, 1000)};
                    if (!worlds.empty()) {
                        const RpWorld* w = worlds[rng.next() % worlds.size()].n;
                        v = {realIn(w->boundingBox.inf.x, w->boundingBox.sup.x), realIn(w->boundingBox.inf.y, w->boundingBox.sup.y),
                             realIn(w->boundingBox.inf.z, w->boundingBox.sup.z)};
                    }
                    RwFrame* nf = RpLightGetFrame(p.n);
                    u32 gf = m.peek32(p.g + 4);
                    g.write_float(kVec, v.x), g.write_float(kVec + 4, v.y), g.write_float(kVec + 8, v.z);
                    instructions += g.call(kFrameTranslate.entry, {gf, kVec, rwCOMBINEREPLACE});
                    RwFrameTranslate(nf, &v, rwCOMBINEREPLACE);
                }
                if (rng.next() % 3u) {
                    instructions += g.call(kSyncDirty.entry, {});
                    _rwFrameSyncDirty();
                    ++syncs;
                }
            } else if (op < 74 && !lights.empty() && !worlds.empty()) {                       // add / remove a light
                LightPair& p = lights[rng.next() % lights.size()];
                if (p.world < 0) {
                    const WorldPair& w = worlds[rng.next() % worlds.size()];
                    instructions += g.call(kAddLight.entry, {w.g, p.g});
                    require(g.r3() == w.g && RpWorldAddLight(w.n, p.n) == w.n, what + ": RpWorldAddLight");
                    p.world = w.id;
                    ++adds;
                } else if (rng.next() % 2u) {
                    removeLight(p, what);
                    ++removes;
                }
            } else if (op < 82 && !worlds.empty()) {                                           // walk the sectors
                const WorldPair& w = worlds[rng.next() % worlds.size()];
                g_visitStop = rng.next() % 2u ? 0 : 1 + static_cast<int>(rng.next() % 6u);
                gvisited.clear(), g_nvisited.clear();
                instructions += g.call(kForAllSectors.entry, {w.g, kSectorCB, 0});
                require(g.r3() == w.g && RpWorldForAllWorldSectors(w.n, nSectorCB, nullptr) == w.n, what + ": RpWorldForAllWorldSectors");
                require(gvisited.size() == g_nvisited.size(), what + ": sectors visited");
                for (std::size_t i = 0; i < gvisited.size(); ++i) {
                    require(m.peek32(gvisited[i] + 0x6C) == bits(g_nvisited[i]->tightBoundingBox.inf.x), what + ": visit order");
                }
                gotVisits += static_cast<unsigned>(gvisited.size());
                visitStops += g_visitStop != 0 && static_cast<int>(gvisited.size()) == g_visitStop;
            } else if (op < 90) {                                                              // look-ups
                for (const WorldPair& w : worlds) {
                    g_visitStop = 0;
                    gvisited.clear(), g_nvisited.clear();
                    instructions += g.call(kForAllSectors.entry, {w.g, kSectorCB, 0});
                    RpWorldForAllWorldSectors(w.n, nSectorCB, nullptr);
                    for (std::size_t i = 0; i < gvisited.size() && i < g_nvisited.size(); ++i) {
                        if (rng.next() % 3u) continue;
                        instructions += g.call(kSectorGetWorld.entry, {gvisited[i]});
                        const RpWorld* nr = RpWorldSectorGetWorld(g_nvisited[i]);
                        require(gWorldId.count(g.r3()) && nWorldId.count(nr) && gWorldId[g.r3()] == nWorldId[nr] && nWorldId[nr] == w.id,
                                what + ": RpWorldSectorGetWorld");
                        ++lookups;
                    }
                }
                if (!lights.empty()) {   // an address in no world
                    const LightPair& p = lights[rng.next() % lights.size()];
                    instructions += g.call(kSectorGetWorld.entry, {p.g});
                    require(g.r3() == 0 && RpWorldSectorGetWorld(reinterpret_cast<const RpWorldSector*>(p.n)) == nullptr,
                            what + ": RpWorldSectorGetWorld of no sector");
                }
                for (const LightPair& p : lights) {
                    instructions += g.call(kLightGetWorld.entry, {p.g});
                    const RpWorld* nr = RpLightGetWorld(p.n);
                    require((g.r3() == 0 ? -10 : gWorldId[g.r3()]) == (nr == nullptr ? -10 : nWorldId[nr]), what + ": RpLightGetWorld");
                }
            } else {                                                                           // a sync alone
                instructions += g.call(kSyncDirty.entry, {});
                _rwFrameSyncDirty();
                ++syncs;
            }
            sameCallbacks(what + " (" + lastKind + ")");
            sameAll(what + " (" + lastKind + ")");
            lastKind = "-";
            ++checks;
        }
        while (!worlds.empty()) destroyWorld(0, "the end"), sameCallbacks("the end");
        sameAll("the end");
        std::printf("  worlds read %u, failed %u; by stream:", read, failed);
        for (const auto& [k, n] : kinds) std::printf(" %s %u (%u failed);", k.c_str(), n, kindsFailed[k]);
        std::printf("\n  compared: %u sectors, %u planes (deepest %u), %u meshes, %u indices, %u light ties; old triangle order %u, "
                    "chains %u, overlapping %u, short world structs %u, short plane structs %u, textured materials %u, shared %u\n",
                    sectorsCompared, planesCompared, maxDepthCompared, meshesCompared, indicesCompared, tiesCompared, oldTriangles,
                    deepTrees, overlapWorlds, shortWorldStructs, shortPlaneStructs, texturedMaterials, sharedMaterials);
        std::printf("  lights: %u syncs, %u adds, %u removes; %u sectors visited (%u stopped walks), %u look-ups; %u original "
                    "destructor calls on unconstructed plugin data (allowed)\n",
                    syncs, adds, removes, gotVisits, visitStops, lookups, gUnconstructed);
        require(read > 800 && failed > 300 && sectorsCompared > 20000 && planesCompared > 10000 && maxDepthCompared >= 45 &&
                    meshesCompared > 15000 && tiesCompared > 2000 && oldTriangles > 100 && shortPlaneStructs > 50 &&
                    shortWorldStructs > 50 && visitStops > 50 && lookups > 1000 && gUnconstructed > 0,
                "the sessions must reach every case");
        for (const char* k : {"truncated", "bad version", "wrong type", "no world block", "no mesh header", "no material array",
                              "colSectorSize", "material count"}) {
            require(kindsFailed[k] > 5, std::string("the sessions must fail streams of every kind: ") + k);
        }
        std::printf("rpworld_world_tests: %u operations identical to the original (%llu original instructions)\n", checks,
                    static_cast<unsigned long long>(instructions));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rpworld_world_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
