// RenderWare Graphics 3.7 core: the plugin registry (module batkreg), native x64.
//
// Every extensible RenderWare object (frame, camera, texture, geometry, atomic, world, ...) has a registry: plugins
// registered before the engine opens each get a slice of the object, at an offset after the object's own structure,
// and their constructor / destructor / copy callbacks run with every object. The originals are PAL main.dol
// 0x80482BE8..0x804831FC; proof: runtime/tests/rwcore_plugin_tests.cpp.
//
// 64-bit: plugin slices are aligned to rwPLUGINDATAALIGNMENT (8: pointers and doubles) where the GameCube rounds to
// 4, so a slice holding a pointer is never misaligned; offsets are otherwise computed as the original computes them.
// Not provided (not linked by the game): _rwPluginRegistrySetStaticPluginsSize, RwPluginRegistrySetFreeListCreateParams
// and the debug-only validation codes.
#pragma once

#include "rw/core/engine.h"
#include "rw/core/types.h"

struct RwStream;

using RwPluginDataChunkWriteCallBack = RwStream* (*)(RwStream* stream, RwInt32 binaryLength, const void* object,
                                                     RwInt32 offsetInObject, RwInt32 sizeInObject);
using RwPluginDataChunkReadCallBack = RwStream* (*)(RwStream* stream, RwInt32 binaryLength, void* object,
                                                    RwInt32 offsetInObject, RwInt32 sizeInObject);
using RwPluginDataChunkGetSizeCallBack = RwInt32 (*)(const void* object, RwInt32 offsetInObject, RwInt32 sizeInObject);
using RwPluginDataChunkAlwaysCallBack = RwBool (*)(void* object, RwInt32 offsetInObject, RwInt32 sizeInObject);
using RwPluginDataChunkRightsCallBack = RwBool (*)(void* object, RwInt32 offsetInObject, RwInt32 sizeInObject,
                                                   RwUInt32 extraData);
using RwPluginObjectConstructor = void* (*)(void* object, RwInt32 offsetInObject, RwInt32 sizeInObject);
using RwPluginObjectCopy = void* (*)(void* dstObject, const void* srcObject, RwInt32 offsetInObject,
                                     RwInt32 sizeInObject);
using RwPluginObjectDestructor = void* (*)(void* object, RwInt32 offsetInObject, RwInt32 sizeInObject);
using RwPluginErrorStrCallBack = void* (*)(void*);

struct RwPluginRegEntry;

/// One object type's registry (rwplcore.h RwPluginRegistry). sizeOfStruct grows with each plugin; maxSizeOfStruct 0
/// means unbounded.
struct RwPluginRegistry {
    RwInt32 sizeOfStruct;
    RwInt32 origSizeOfStruct;
    RwInt32 maxSizeOfStruct;
    RwInt32 staticAlloc;
    RwPluginRegEntry* firstRegEntry;
    RwPluginRegEntry* lastRegEntry;
};

/// One plugin of a registry (rwplcore.h RwPluginRegEntry). The stream callbacks are batkbin's.
struct RwPluginRegEntry {
    RwInt32 offset;
    RwInt32 size;
    RwUInt32 pluginID;
    RwPluginDataChunkReadCallBack readCB;
    RwPluginDataChunkWriteCallBack writeCB;
    RwPluginDataChunkGetSizeCallBack getSizeCB;
    RwPluginDataChunkAlwaysCallBack alwaysCB;
    RwPluginDataChunkRightsCallBack rightsCB;
    RwPluginObjectConstructor constructCB;
    RwPluginObjectDestructor destructCB;
    RwPluginObjectCopy copyCB;
    RwPluginErrorStrCallBack errStrCB;
    RwPluginRegEntry* nextRegEntry;
    RwPluginRegEntry* prevRegEntry;
    RwPluginRegistry* parentRegistry;
};

/// A registry for objects of size bytes (RenderWare's static initializer for one).
#define rwPLUGINREGISTRYINIT(size) {static_cast<RwInt32>(size), static_cast<RwInt32>(size), 0, 0, nullptr, nullptr}

/// Plugin slices start at multiples of this (the GameCube's 4 widened for 64-bit data).
inline constexpr RwInt32 rwPLUGINDATAALIGNMENT = 8;
static_assert(rwPLUGINDATAALIGNMENT >= alignof(void*) && rwPLUGINDATAALIGNMENT >= alignof(double));

/// The registry module: the free list of registry entries (64 to a block, one preallocated, a static header, hint
/// rwMEMHINTDUR_GLOBAL). FALSE when it cannot be made. Original: 0x80482BE8.
RwBool _rwPluginRegistryOpen();

/// Frees every registry entry and returns every registry to its object's own size. Returns TRUE. Original: 0x80482CA8.
RwBool _rwPluginRegistryClose();

/// The offset of pluginID's slice in reg's objects, or -1. Original: 0x80482DE8.
RwInt32 _rwPluginRegistryGetPluginOffset(const RwPluginRegistry* reg, RwUInt32 pluginID);

/// Registers a plugin of size bytes with reg (NULL callbacks: ones that do nothing). Returns its offset; the existing
/// offset when pluginID is already registered (E_RW_PLUGININIT is set); -1 while an engine is open (E_RW_PLUGININIT),
/// before _rwPluginRegistryOpen, past reg->maxSizeOfStruct, or when out of memory. Original: 0x80482E18.
RwInt32 _rwPluginRegistryAddPlugin(RwPluginRegistry* reg, RwInt32 size, RwUInt32 pluginID,
                                   RwPluginObjectConstructor constructCB, RwPluginObjectDestructor destructCB,
                                   RwPluginObjectCopy copyCB);

/// Runs every plugin's constructor on object, in registration order; when one fails (returns NULL), the destructors
/// of the plugins before it, in reverse, and returns NULL. Returns reg. Original: 0x804830EC.
const RwPluginRegistry* _rwPluginRegistryInitObject(const RwPluginRegistry* reg, void* object);

/// Runs every plugin's destructor on object, last registered first. Returns reg. Original: 0x80483194.
const RwPluginRegistry* _rwPluginRegistryDeInitObject(const RwPluginRegistry* reg, void* object);

/// Runs every plugin's copy callback (dst, src), in registration order. Returns reg. Original: 0x80483200.
const RwPluginRegistry* _rwPluginRegistryCopyObject(const RwPluginRegistry* reg, void* dstObject,
                                                    const void* srcObject);

// ---- plugin data in streams (module batkbin; originals 0x80482640..0x80482BE4) -----------------------------------
// An object's plugin data follows it in a stream as one rwID_EXTENSION chunk holding a chunk per plugin
// ({pluginID, length, library ID} + the plugin's bytes).

/// Gives pluginID's entry in reg its stream callbacks. Returns the plugin's offset, or -1 when it is not registered.
/// Original: 0x80482640.
RwInt32 _rwPluginRegistryAddPluginStream(RwPluginRegistry* reg, RwUInt32 pluginID,
                                         RwPluginDataChunkReadCallBack readCB, RwPluginDataChunkWriteCallBack writeCB,
                                         RwPluginDataChunkGetSizeCallBack getSizeCB);

/// The callback run for pluginID after every read of an object's plugin data, chunk or not. Original: 0x80482684.
RwInt32 _rwPluginRegistryAddPlgnStrmlwysCB(RwPluginRegistry* reg, RwUInt32 pluginID,
                                           RwPluginDataChunkAlwaysCallBack alwaysCB);

/// The rights callback of pluginID. Original: 0x804826C0.
RwInt32 _rwPluginRegistryAddPlgnStrmRightsCB(RwPluginRegistry* reg, RwUInt32 pluginID,
                                             RwPluginDataChunkRightsCallBack rightsCB);

/// Reads object's extension chunk: each plugin chunk to its plugin's read callback (skipped when it has none or is
/// unknown), then every plugin's always callback. Returns reg; NULL when a chunk, a callback or the version
/// (3.5.0.0 .. 3.7.0.2, E_RW_BADVERSION) fails. Original: 0x804826FC.
const RwPluginRegistry* _rwPluginRegistryReadDataChunks(const RwPluginRegistry* reg, RwStream* stream, void* object);

/// Runs pluginID's rights callback on object with extraData. reg when it returns TRUE; NULL when it fails, or the
/// plugin or its callback is missing. Original: 0x804828C0.
const RwPluginRegistry* _rwPluginRegistryInvokeRights(const RwPluginRegistry* reg, RwUInt32 pluginID, void* object,
                                                      RwUInt32 extraData);

/// The bytes object's plugin chunks take in a stream (each plugin's positive size + its 12-byte header), without
/// the extension chunk's own header. Original: 0x8048294C.
RwInt32 _rwPluginRegistryGetSize(const RwPluginRegistry* reg, const void* object);

/// Writes object's extension chunk (version 3.7.0.2, build 0x37) and a chunk per plugin with data. Returns reg, NULL
/// when a write fails. Original: 0x804829D0.
const RwPluginRegistry* _rwPluginRegistryWriteDataChunks(const RwPluginRegistry* reg, RwStream* stream,
                                                         const void* object);

/// Skips an extension chunk, plugin chunk by plugin chunk. Returns reg, NULL on a read or skip failure.
/// Original: 0x80482B24.
const RwPluginRegistry* _rwPluginRegistrySkipDataChunks(const RwPluginRegistry* reg, RwStream* stream);
