// RenderWare materials and material lists (modules bamateri, bamatlst; see rw/world/material.h).
#include "rw/world/material.h"

#include <cstring>

#include "rw/core/binary.h"
#include "rw/core/error.h"
#include "rw/core/memory.h"

RwModuleInfo materialModule{};

namespace {

// The free list's creation parameters (bamateri statics 0x805EEE68 / 0x805EEE6C) and its in-place header
// (0x805E26B0); the default surface properties (0x8056E918).
constexpr RwInt32 kMaterialsPerBlock = 256;
constexpr RwInt32 kPreallocBlocks = 1;
constexpr RwUInt32 kMaterialHint = 0x00030000u | rwID_MATERIAL;              // rwMEMHINTDUR_EVENT
constexpr RwUInt32 kMatListHint = 0x01030000u | rwID_MATLIST;                // ... | rwMEMHINTFLAG_RESIZEABLE
constexpr RwUInt32 rwID_WORLDPLUGIN = 0x02;
constexpr RwInt32 kMatListGrowth = 20;
const RwSurfaceProperties kDefaultSurface{1.0f, 1.0f, 1.0f};
RwFreeList materialFreeListSpace{};
RwPluginRegistry materialTKList = rwPLUGINREGISTRYINIT(sizeof(RpMaterial));   // 0x8056E900
rpMaterialRights g_rights{};

RwFreeList*& materialFreeList() { return rwModuleGlobals<rpMaterialGlobals>(materialModule).materials; }

void badVersion() {
    RwError error{rwID_WORLDPLUGIN, _rwerror(static_cast<RwInt32>(0x80000004u))};   // E_RW_BADVERSION
    RwErrorSet(&error);
}

/// RpMaterialCreate's body (the stream read runs it inlined).
RpMaterial* createMaterial() {
    auto* material = static_cast<RpMaterial*>(rwSrcGlobals().memoryAlloc(materialFreeList(), kMaterialHint));
    if (material == nullptr) return nullptr;
    std::memset(material, 0, sizeof *material);
    material->refCount = 1;
    material->color = {255, 255, 255, 255};
    material->texture = nullptr;
    material->pipeline = nullptr;
    material->surfaceProps = kDefaultSurface;
    if (_rwPluginRegistryInitObject(&materialTKList, material) == nullptr) {
        // native: a failed plugin constructor fails the creation (the original returns the half-built material)
        rwSrcGlobals().memoryFree(materialFreeList(), material);
        return nullptr;
    }
    return material;
}

}  // namespace

RwPluginRegistry& rpMaterialRegistry() { return materialTKList; }
rpMaterialRights& rpMaterialStreamRights() { return g_rights; }

void* _rpMaterialOpen(void* instance, RwInt32 offset, RwInt32 /*size*/) {
    materialModule.globalsOffset = offset;
    materialFreeList() = RwFreeListCreateAndPreallocateSpace(materialTKList.sizeOfStruct, kMaterialsPerBlock,
                                                             alignof(RpMaterial), kPreallocBlocks, &materialFreeListSpace,
                                                             rwMEMHINTDUR_GLOBAL | rwID_MATERIAL);
    if (materialFreeList() == nullptr) return nullptr;
    ++materialModule.numInstances;
    return instance;
}

void* _rpMaterialClose(void* instance, RwInt32 /*offset*/, RwInt32 /*size*/) {
    if (materialFreeList() != nullptr) {
        RwFreeListDestroy(materialFreeList());
        materialFreeList() = nullptr;
    }
    --materialModule.numInstances;
    return instance;
}

RpMaterial* RpMaterialCreate() {
    if (materialModule.numInstances <= 0) {   // native: no material module open
        RwError error{rwID_COREPLUGIN, _rwerror(static_cast<RwInt32>(0x80000001u))};   // E_RW_BADENGINESTATE
        RwErrorSet(&error);
        return nullptr;
    }
    return createMaterial();
}

RwBool RpMaterialDestroy(RpMaterial* material) {
    if (material->refCount != 1) {
        --material->refCount;
        return 1;
    }
    _rwPluginRegistryDeInitObject(&materialTKList, material);
    RpMaterialSetTexture(material, nullptr);
    rwSrcGlobals().memoryFree(materialFreeList(), material);
    return 1;
}

RpMaterial* RpMaterialSetTexture(RpMaterial* material, RwTexture* texture) {
    if (texture != nullptr) ++texture->refCount;
    if (material->texture != nullptr) RwTextureDestroy(material->texture);
    material->texture = texture;
    return material;
}

RwInt32 RpMaterialRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB,
                                 RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB) {
    return _rwPluginRegistryAddPlugin(&materialTKList, size, pluginID, constructCB, destructCB, copyCB);
}

RwInt32 RpMaterialRegisterPluginStream(RwUInt32 pluginID, RwPluginDataChunkReadCallBack readCB,
                                       RwPluginDataChunkWriteCallBack writeCB,
                                       RwPluginDataChunkGetSizeCallBack getSizeCB) {
    return _rwPluginRegistryAddPluginStream(&materialTKList, pluginID, readCB, writeCB, getSizeCB);
}

RpMaterial* RpMaterialStreamRead(RwStream* stream) {
    RwUInt32 length = 0, version = 0;
    if (!RwStreamFindChunk(stream, rwID_STRUCT, &length, &version)) return nullptr;
    if (version < rwLIBRARYBASEVERSION || version > rwLIBRARYCURRENTVERSION) {
        badVersion();
        return nullptr;
    }
    // {flags, colour, unused, textured, ambient, specular, diffuse}: 28 bytes; the colour's bytes stay in stream order
    RwUInt32 data[7] = {};
    const RwUInt32 take = length < sizeof data ? length : static_cast<RwUInt32>(sizeof data);
    if (RwStreamRead(stream, data, take) != take) return nullptr;
    if (length > take && RwStreamSkip(stream, length - take) == nullptr) return nullptr;   // native: the rest skipped
    const RwUInt32 colour = data[1];
    RwMemNative32(data, sizeof data);
    data[1] = colour;
    RpMaterial* material = materialModule.numInstances > 0 ? createMaterial() : nullptr;
    if (material == nullptr) return nullptr;
    std::memcpy(&material->color, &data[1], sizeof material->color);
    std::memcpy(&material->surfaceProps.ambient, &data[4], 4);
    std::memcpy(&material->surfaceProps.specular, &data[5], 4);
    std::memcpy(&material->surfaceProps.diffuse, &data[6], 4);
    material->texture = nullptr;
    if (data[3] != 0) {
        RwUInt32 textureVersion = 0;
        if (!RwStreamFindChunk(stream, rwID_TEXTURE, nullptr, &textureVersion)) {
            RpMaterialDestroy(material);
            return nullptr;
        }
        if (textureVersion < rwLIBRARYBASEVERSION || textureVersion > rwLIBRARYCURRENTVERSION) {
            RpMaterialDestroy(material);
            badVersion();
            return nullptr;
        }
        material->texture = RwTextureStreamRead(stream);   // NULL leaves it untextured, as the original
    }
    g_rights = {0, 0};
    if (_rwPluginRegistryReadDataChunks(&materialTKList, stream, material) == nullptr) {
        RpMaterialDestroy(material);
        return nullptr;
    }
    if (g_rights.pluginID != 0) {
        _rwPluginRegistryInvokeRights(&materialTKList, g_rights.pluginID, material, g_rights.extraData);
    }
    return material;
}

RpMaterialList* _rpMaterialListInitialize(RpMaterialList* matList) {
    matList->space = 0;
    matList->materials = nullptr;
    matList->numMaterials = 0;
    return matList;
}

RpMaterialList* _rpMaterialListDeinitialize(RpMaterialList* matList) {
    if (RpMaterial** materials = matList->materials) {
        for (RwInt32 i = 0; i < matList->numMaterials; ++i) {
            RpMaterialDestroy(materials[i]);
            materials[i] = nullptr;
        }
        RwFree(materials);
        matList->materials = nullptr;
    }
    matList->numMaterials = 0;
    matList->space = 0;
    return matList;
}

RpMaterial* _rpMaterialListGetMaterial(const RpMaterialList* matList, RwInt32 index) { return matList->materials[index]; }

RwInt32 _rpMaterialListAppendMaterial(RpMaterialList* matList, RpMaterial* material) {
    if (matList->space <= matList->numMaterials) {
        const std::size_t bytes = static_cast<std::size_t>(matList->space + kMatListGrowth) * sizeof(RpMaterial*);
        void* grown = matList->materials != nullptr ? RwRealloc(matList->materials, bytes, kMatListHint)
                                                    : RwMalloc(bytes, kMatListHint);
        if (grown == nullptr) {
            RwError error{rwID_WORLDPLUGIN, _rwerror(E_RW_NOMEM, bytes)};
            RwErrorSet(&error);
            return -1;
        }
        matList->materials = static_cast<RpMaterial**>(grown);
        matList->space += kMatListGrowth;
    }
    matList->materials[matList->numMaterials] = material;
    ++material->refCount;
    ++matList->numMaterials;
    return matList->numMaterials - 1;
}

RwInt32 _rpMaterialListFindMaterialIndex(const RpMaterialList* matList, const RpMaterial* material) {
    for (RwInt32 i = matList->numMaterials - 1; i >= 0; --i) {
        if (matList->materials[i] == material) return i;
    }
    return -1;
}
