// RenderWare Graphics 3.7 world plugin (rpworld), native x64: the world and its sectors (modules baworld, basector,
// babinwor, bamesh's reading, the world-object light extension), and attaching the plugin to the engine.
//
// A world is a BSP tree: plane sectors split space on an axis; atomic world sectors at the leaves hold the static
// geometry - vertices, normals, prelit colours, texture coordinates, triangles - and a mesh header grouping the
// triangles' vertex indices by material (the world's material list). A world read from a stream lives in one block,
// as the original lays it out (the world and its plugin data, then the tree in pre-order, each atomic sector followed
// by its arrays). Lights added to a world are either global (ambient / directional: the world's directional list) or
// positional (point / spot: the world's light list, tied to every atomic sector their radius box reaches, rebuilt each
// time their frame syncs).
//
// RpWorldPluginAttach (PAL main.dol 0x8046185C) registers the modules in the original's order and ids: material 0x501,
// mesh 0x502, geometry 0x503 (not yet), clump 0x504 (not yet), light 0x505, 0x50A, world 0x507, 0x50B; then the
// world-object extensions it needs (the light's world extension 0x509, the sector mesh stream 0x50E). The offsets are
// ORed: any negative one fails the attach.
//
// Written from PAL main.dol (rwcore/README.md "rpworld" lists every function and address; the reading of the original
// is .build/dolrecon/world_read_spec.md). Proof: runtime/tests/rpworld_world_tests.cpp (oracle against the original
// RpWorldStreamRead on converted worlds) and rpworld_world_native_tests.cpp. Deliberate differences: rwcore/README.md
// "Native contracts" -> Worlds.
#pragma once

#include "rw/core/bbox.h"
#include "rw/core/engine.h"
#include "rw/core/frame.h"
#include "rw/core/plugin.h"
#include "rw/core/stream.h"
#include "rw/core/types.h"
#include "rw/world/light.h"
#include "rw/world/material.h"

inline constexpr RwUInt32 rwID_MESHMODULE = 0x502u;
inline constexpr RwUInt32 rwID_LIGHTMODULE = 0x505u;
inline constexpr RwUInt32 rwID_WORLDMODULE = 0x507u;
inline constexpr RwUInt32 rwID_WORLDLIGHTEXT = 0x509u;   ///< the light's world extension (rpworld's own plugin id)
inline constexpr RwUInt32 rwID_BINMESHPLUGIN = 0x50Eu;   ///< the sector / geometry mesh chunk
inline constexpr RwUInt8 rpWORLD = 7;                    ///< RwObject::type of a world

/// World flags (rpworld.h RpWorldFlag; the stream's format word).
enum RpWorldFlag : RwUInt32 {
    rpWORLDTRISTRIP = 0x01,
    rpWORLDTEXTURED = 0x04,
    rpWORLDPRELIT = 0x08,
    rpWORLDNORMALS = 0x10,
    rpWORLDLIGHT = 0x20,
    rpWORLDMODULATEMATERIALCOLOR = 0x40,
    rpWORLDTEXTURED2 = 0x80,
    rpWORLDNATIVE = 0x01000000,
    rpWORLDSECTORSOVERLAP = 0x40000000,
};

inline constexpr RwInt32 rwSECTORATOMIC = -1;   ///< RpSector::type of an atomic world sector
inline constexpr RwInt32 rwSECTORBUILD = -2;

/// A triangle of a world sector (rpworld.h RpTriangle): three vertex indices and a material index (into the sector's
/// window of the world's material list).
struct RpTriangle {
    RwUInt16 vertIndex[3];
    RwUInt16 matIndex;
};

/// A packed normal (rpworld.h RpVertexNormal): three signed bytes over 128 and a pad.
struct RpVertexNormal {
    RwInt8 x;
    RwInt8 y;
    RwInt8 z;
    RwUInt8 pad;
};

struct RwTexCoords {
    RwReal u;
    RwReal v;
};

/// A mesh (rpworld.h RpMesh): the vertex indices of one material's triangles (lists, or a strip when the header says).
struct RpMesh {
    RwUInt16* indices;
    RwUInt32 numIndices;
    RpMaterial* material;
};

/// A mesh header (rpworld.h RpMeshHeader), followed in its block by numMeshes RpMesh records and then the indices.
struct RpMeshHeader {
    RwUInt32 flags;            ///< rpMESHHEADERTRISTRIP 0x01, ...
    RwUInt16 numMeshes;
    RwUInt16 serialNum;
    RwUInt32 totalIndicesInMesh;
    RwUInt32 firstMeshOffset;
};
inline RpMesh* RpMeshHeaderGetMeshes(RpMeshHeader* h) { return reinterpret_cast<RpMesh*>(h + 1); }
inline const RpMesh* RpMeshHeaderGetMeshes(const RpMeshHeader* h) { return reinterpret_cast<const RpMesh*>(h + 1); }

struct RpSector {
    RwInt32 type;   ///< rwSECTORATOMIC, rwSECTORBUILD, or a plane's axis as a byte offset into RwV3d (0, 4, 8)
};

/// A plane sector (rpworld.h RpPlaneSector): space split at value on an axis; leftValue / rightValue are where the
/// left (less) child ends and the right one starts (equal to value unless the world's sectors overlap).
struct RpPlaneSector {
    RwInt32 type;
    RwReal value;
    RpSector* leftSubTree;
    RpSector* rightSubTree;
    RwReal leftValue;
    RwReal rightValue;
};

struct RxPipeline;

/// An atomic world sector (rpworld.h RpWorldSector).
struct RpWorldSector {
    RwInt32 type;   ///< rwSECTORATOMIC
    RpTriangle* triangles;
    RwV3d* vertices;
    RpVertexNormal* normals;
    RwTexCoords* texCoords[8];
    RwRGBA* preLitLum;
    void* repEntry;            ///< the GameCube's display lists (native: never)
    RwLinkList collAtomicsInWorldSector;
    RwLinkList lightsInWorldSector;   ///< light ties (rpLightTie::lightInWorldSector)
    RwBBox boundingBox;        ///< from the BSP planes
    RwBBox tightBoundingBox;   ///< around its vertices (from the stream)
    RpMeshHeader* mesh;
    RxPipeline* pipeline;
    RwUInt16 matListWindowBase;
    RwUInt16 numVertices;
    RwUInt16 numTriangles;
    RwUInt16 pad;
};

struct RpWorld;
using RpWorldSectorCallBackRender = RpWorldSector* (*)(RpWorldSector* sector);
using RpWorldSectorCallBack = RpWorldSector* (*)(RpWorldSector* sector, void* data);

/// A world (rpworld.h RpWorld).
struct RpWorld {
    RwObject object;           ///< type rpWORLD; privateFlags bit 0: one block (read from a stream)
    RwUInt32 flags;            ///< RpWorldFlag
    RwInt32 renderOrder;       ///< 2 (back to front)
    RpMaterialList matList;
    RpSector* rootSector;
    RwInt32 numTexCoordSets;
    RwInt32 numClumpsInWorld;
    RwLLLink* currentClumpLink;
    RwLinkList clumpList;
    RwLinkList lightList;              ///< positional lights (RpLight::inWorld)
    RwLinkList directionalLightList;   ///< ambient and directional lights
    RwV3d worldOrigin;
    RwBBox boundingBox;
    RpWorldSectorCallBackRender renderCallBack;   ///< native: NULL (the renderer draws sectors; GameCube 0x80460B60)
    RxPipeline* pipeline;
};

/// A light's tie to an atomic sector its radius reaches (the original's 0x18-byte tie).
struct rpLightTie {
    RwLLLink lightInWorldSector;   ///< in RpWorldSector::lightsInWorldSector
    RpLight* light;
    RwLLLink WorldSectorInLight;   ///< in RpLight::WorldSectorsInLight
    RpWorldSector* sect;
};

/// The light's world extension (plugin 0x509): its world and the sync callback the extension replaced.
struct rpWorldLightExt {
    RpWorld* world;
    RwObjectHasFrameSyncFunction oldSync;
};

/// The world module's globals: the registry of worlds (for a sector's world) and the light ties. Native: the original
/// keeps the ties' free list in another module's globals (r13 + 0x60C0, not identified).
struct rpWorldGlobals {
    RwFreeList* registryEntries;
    RwLinkList worlds;
    RwFreeList* lightTies;
};

struct rpMeshGlobals {
    RwUInt16 nextSerialNum;
};

extern RwModuleInfo worldModule, meshModule;

/// The registries of worlds (worldTKList, 0x8056E958) and atomic sectors (sectorTKList, 0x8056E940).
RwPluginRegistry& rpWorldRegistry();
RwPluginRegistry& rpWorldSectorRegistry();

/// Registers the world plugin's modules and extensions (call between RwEngineInit and RwEngineOpen). TRUE, or FALSE
/// when a registration failed. Original: 0x8046185C.
RwBool RpWorldPluginAttach();

/// The modules (originals: _rpMeshOpen 0x8045D00C / _rpMeshClose 0x8045CFB4, WorldOpen 0x80460AA0 / WorldClose
/// 0x80460A38, the counters of 0x50A at 0x804602F0 / 0x80460304 and 0x50B at 0x80456D48 / 0x80456D34).
void* _rpMeshOpen(void* instance, RwInt32 offset, RwInt32 size);
void* _rpMeshClose(void* instance, RwInt32 offset, RwInt32 size);
void* WorldOpen(void* instance, RwInt32 offset, RwInt32 size);
void* WorldClose(void* instance, RwInt32 offset, RwInt32 size);

/// A world from a stream (positioned after its world chunk's header): its struct (version 3.5.0.0 .. 3.7.0.2), material
/// list, sector tree (plane sectors and atomic sectors with their arrays and plugin data - the sector mesh among them)
/// and plugin data, in one block. NULL when a read fails (the world destroyed). Worlds of GameCube display lists
/// (rpWORLDNATIVE) are not read natively: the offline converter writes platform-independent sectors. Original: 0x80456744
/// (with AtomicSectorRead 0x80455D34, PlaneSectorRead 0x804562FC, _rpWorldSetupSectorBoundingBoxes 0x80460BF4).
RpWorld* RpWorldStreamRead(RwStream* stream);

/// Unregisters the world, destroys its sectors' meshes, its material list, its sectors and their plugin data, its plugin
/// data, and frees it. TRUE. Native: lights still in the world are taken out of it first (the original leaves their
/// world pointers dangling). Original: 0x80460FF0.
RwBool RpWorldDestroy(RpWorld* world);

/// The world a sector belongs to, or NULL. Original: 0x80460EF0.
RpWorld* RpWorldSectorGetWorld(const RpWorldSector* sector);

/// callBack for every atomic sector (left before right) until one returns NULL. Returns world. Original: 0x80461740.
RpWorld* RpWorldForAllWorldSectors(RpWorld* world, RpWorldSectorCallBack callBack, void* data);

/// A world or sector plugin (before the engine opens). Its offset or -1. Originals: 0x804617E0, 0x80460348 (and their
/// stream forms 0x80461820, 0x80460388).
RwInt32 RpWorldRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB,
                              RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB);
RwInt32 RpWorldSectorRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB,
                                    RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB);
RwInt32 RpWorldRegisterPluginStream(RwUInt32 pluginID, RwPluginDataChunkReadCallBack readCB,
                                    RwPluginDataChunkWriteCallBack writeCB, RwPluginDataChunkGetSizeCallBack getSizeCB);
RwInt32 RpWorldSectorRegisterPluginStream(RwUInt32 pluginID, RwPluginDataChunkReadCallBack readCB,
                                          RwPluginDataChunkWriteCallBack writeCB,
                                          RwPluginDataChunkGetSizeCallBack getSizeCB);

/// A material list from a stream: its struct {count, then per material -1 (read next) or the index of an earlier one}
/// and the materials read. NULL when a read fails (the list emptied). Native: an index that is not an earlier one is a
/// failure (the original reads past the list). Original: 0x8045C974.
RpMaterialList* _rpMaterialListStreamRead(RwStream* stream, RpMaterialList* matList);

/// A mesh header from a stream for object (a world or a geometry: native objects carry no indices) whose materials are
/// in matList. NULL when a read fails (native: the header freed - the original leaks it; indices and material indices
/// checked). Original: 0x8045D7C0. Destroyed by _rpMeshDestroy (0x8045D324).
RpMeshHeader* _rpMeshRead(RwStream* stream, const void* object, const RpMaterialList* matList);
RwBool _rpMeshDestroy(RpMeshHeader* mesh);

/// Global lights (types below rpLIGHTPOINT) join the world's directional list; positional ones its light list (their
/// frame marked, so the next sync ties them to sectors). Native: a light already in a world is moved (the original
/// corrupts both lists). Returns world. Original: 0x804635C8.
RpWorld* RpWorldAddLight(RpWorld* world, RpLight* light);

/// Takes the light out of its world: its sector ties freed, its link removed. Returns world. Original: 0x80463668.
RpWorld* RpWorldRemoveLight(RpWorld* world, RpLight* light);

/// The world the light is in, or NULL. Original: 0x8046373C.
RpWorld* RpLightGetWorld(const RpLight* light);

/// The light's world extension (its slot in the light), for inspection.
rpWorldLightExt* rpWorldLightExtension(RpLight* light);
