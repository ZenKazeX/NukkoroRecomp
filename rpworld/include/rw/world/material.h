// RenderWare Graphics 3.7 world plugin: materials and material lists (modules bamateri, bamatlst), native x64.
//
// A material is a reference-counted colour, surface properties (ambient, specular, diffuse) and an optional texture
// it holds a reference to; geometry and world sectors name their materials through material lists (arrays of
// referenced materials). Materials are read from streams with their texture (RwTextureStreamRead: found in the
// current texture dictionary, as the converted stages provide them) and plugin data.
//
// Written from PAL main.dol bamateri 0x8045C098..0x8045C73C and bamatlst 0x8045C740..0x8045CF70. Proof:
// runtime/tests/rpworld_material_tests.cpp (oracle) and rpworld_material_native_tests.cpp. Deliberate differences:
// rwcore/README.md "Native contracts" -> Materials.
#pragma once

#include "rw/core/engine.h"
#include "rw/core/plugin.h"
#include "rw/core/stream.h"
#include "rw/core/texture.h"
#include "rw/core/types.h"

/// A colour of four bytes (rwplcore.h RwRGBA).
struct RwRGBA {
    RwUInt8 red;
    RwUInt8 green;
    RwUInt8 blue;
    RwUInt8 alpha;
};

/// How a surface reflects light (rwplcore.h RwSurfaceProperties).
struct RwSurfaceProperties {
    RwReal ambient;
    RwReal specular;
    RwReal diffuse;
};

struct RxPipeline;

/// A material (rpworld.h RpMaterial).
struct RpMaterial {
    RwTexture* texture;
    RwRGBA color;
    RxPipeline* pipeline;
    RwSurfaceProperties surfaceProps;
    RwInt16 refCount;
    RwInt16 pad;
};

/// An array of referenced materials (rpworld.h RpMaterialList).
struct RpMaterialList {
    RpMaterial** materials;
    RwInt32 numMaterials;
    RwInt32 space;
};

/// The material module's globals: the free list of materials.
struct rpMaterialGlobals {
    RwFreeList* materials;
};

extern RwModuleInfo materialModule;

inline constexpr RwUInt32 rwID_MATERIALMODULE = 0x501u;

/// The materials' plugin registry (materialTKList, 0x8056E900).
RwPluginRegistry& rpMaterialRegistry();

/// The "right to render" a material's plugin data may carry (the world-object extensions' material chunk 0x1F reads
/// it): the plugin whose rights RpMaterialStreamRead invokes after the material's plugin data, and its extra data.
/// The original's statics 0x805F2588 (r13 + 0x6088, data) and 0x805F258C (r13 + 0x608C, plugin).
struct rpMaterialRights {
    RwUInt32 pluginID;
    RwUInt32 extraData;
};
rpMaterialRights& rpMaterialStreamRights();

/// Opens the module: the material free list (its block size and preallocation from 0x805EEE68 / 0x805EEE6C, a static
/// header, hint rwMEMHINTDUR_GLOBAL | rwID_MATERIAL). NULL when it cannot be made. Original: 0x8045C098.
void* _rpMaterialOpen(void* instance, RwInt32 offset, RwInt32 size);

/// Destroys the material free list. Returns instance. Original: 0x8045C128.
void* _rpMaterialClose(void* instance, RwInt32 offset, RwInt32 size);

/// A white material (255, 255, 255, 255), surface properties (1, 1, 1), no texture or pipeline, one reference, the
/// plugins' constructors run. NULL when the allocation (native: or a plugin constructor) fails, or no material module
/// is open. Original: 0x8045C18C.
RpMaterial* RpMaterialCreate();

/// Drops a reference; the last one runs the plugin destructors, releases the texture and frees the material. TRUE.
/// Original: 0x8045C244.
RwBool RpMaterialDestroy(RpMaterial* material);

/// The texture: the new one gains a reference, the old one is released (RwTextureDestroy). Returns material.
/// Original: 0x8045C2C0.
RpMaterial* RpMaterialSetTexture(RpMaterial* material, RwTexture* texture);

/// A material plugin and its stream callbacks (before the engine opens). Its offset or -1. Originals: 0x8045C31C,
/// 0x8045C35C.
RwInt32 RpMaterialRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB,
                                 RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB);
RwInt32 RpMaterialRegisterPluginStream(RwUInt32 pluginID, RwPluginDataChunkReadCallBack readCB,
                                       RwPluginDataChunkWriteCallBack writeCB,
                                       RwPluginDataChunkGetSizeCallBack getSizeCB);

/// A material from a stream: its struct chunk {flags, colour (bytes in stream order), unused, textured, ambient,
/// specular, diffuse} (versions 3.5.0.0 .. 3.7.0.2, else E_RW_BADVERSION), its texture when textured (a texture chunk
/// read by RwTextureStreamRead; a texture that is not found leaves the material untextured), its plugin data, then
/// the rights a plugin claimed while they were read. NULL when a read fails (the material destroyed). Native: the
/// struct is read into its 28 bytes and the rest skipped (the original reads it all into a 28-byte stack buffer).
/// Original: 0x8045C398.
RpMaterial* RpMaterialStreamRead(RwStream* stream);

inline const RwRGBA* RpMaterialGetColor(const RpMaterial* m) { return &m->color; }
inline RpMaterial* RpMaterialSetColor(RpMaterial* m, const RwRGBA* color) {
    m->color = *color;
    return m;
}
inline RwTexture* RpMaterialGetTexture(const RpMaterial* m) { return m->texture; }
inline const RwSurfaceProperties* RpMaterialGetSurfaceProperties(const RpMaterial* m) { return &m->surfaceProps; }
inline RpMaterial* RpMaterialSetSurfaceProperties(RpMaterial* m, const RwSurfaceProperties* p) {
    m->surfaceProps = *p;
    return m;
}
inline RpMaterial* RpMaterialAddRef(RpMaterial* m) {
    ++m->refCount;
    return m;
}

// ---- material lists ------------------------------------------------------------------------------------------

/// An empty list. Returns matList. Original: 0x8045C7D8.
RpMaterialList* _rpMaterialListInitialize(RpMaterialList* matList);

/// Releases every material (RpMaterialDestroy) and frees the array; the list is empty again. Returns matList.
/// Original: 0x8045C740.
RpMaterialList* _rpMaterialListDeinitialize(RpMaterialList* matList);

/// The material at index (no bounds check, as the original). Original: 0x8045C7EC.
RpMaterial* _rpMaterialListGetMaterial(const RpMaterialList* matList, RwInt32 index);

/// Appends material (which gains a reference), growing the array by 20 entries when full (hint
/// rwMEMHINTDUR_EVENT | rwMEMHINTFLAG_RESIZEABLE | rwID_MATLIST). Its index, or -1 with E_RW_NOMEM. Original:
/// 0x8045C7FC.
RwInt32 _rpMaterialListAppendMaterial(RpMaterialList* matList, RpMaterial* material);

/// The last index holding material, or -1. Original: 0x8045C93C.
RwInt32 _rpMaterialListFindMaterialIndex(const RpMaterialList* matList, const RpMaterial* material);
