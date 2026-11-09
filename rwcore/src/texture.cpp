// RenderWare textures and texture dictionaries (module batextur, CPU side; see rw/core/texture.h).
#include "rw/core/texture.h"

#include <cstring>

#include "rw/core/binary.h"
#include "rw/core/error.h"
#include "rw/core/memory.h"

RwModuleInfo textureModule{};

namespace {

// The free lists' creation parameters (batextur statics 0x805EEFBC .. 0x805EEFC8) and in-place headers (0x805E3DD0,
// 0x805E3DF4).
constexpr RwInt32 kTexturesPerBlock = 128, kTexturePrealloc = 1;
constexpr RwInt32 kDictsPerBlock = 5, kDictPrealloc = 1;
constexpr RwUInt32 rwID_TEXTUREMODULE = 0x408u;
constexpr RwUInt32 kTextureHint = 0x00030000u | rwID_TEXTURE;      // rwMEMHINTDUR_EVENT, as the original passes them
constexpr RwUInt32 kDictHint = 0x00030000u | rwID_TEXDICTIONARY;
constexpr RwInt32 E_RW_READTEXMASK = 0x16;                          // the core's own code, kept with its plugin id
constexpr RwInt32 E_RW_STRING_TRUNCATION = static_cast<RwInt32>(0x8000001Eu);
constexpr RwInt32 rwSTANDARDTEXTURESETRASTER = 8;
RwFreeList textureFreeListSpace{}, texDictFreeListSpace{};
RwPluginRegistry textureTKList = rwPLUGINREGISTRYINIT(sizeof(RwTexture));       // 0x8056F0F0
RwPluginRegistry texDictTKList = rwPLUGINREGISTRYINIT(sizeof(RwTexDictionary));  // 0x8056F108
RwTexDictionary* g_defaultTexDict = nullptr;   // the dictionary the open makes (r13 + 0x6178)

rwTextureGlobals& globals() { return rwModuleGlobals<rwTextureGlobals>(textureModule); }

void unlink(RwLLLink& link) {
    link.prev->next = link.next;
    link.next->prev = link.prev;
}
void linkAtHead(RwLinkList& list, RwLLLink& link) {
    link.next = list.link.next;
    link.prev = &list.link;
    list.link.next->prev = &link;
    list.link.next = &link;
}

RwTexture* textureOf(RwLLLink* link) {
    return reinterpret_cast<RwTexture*>(reinterpret_cast<char*>(link) - offsetof(RwTexture, lInDictionary));
}
RwTexDictionary* dictOf(RwLLLink* link) {
    return reinterpret_cast<RwTexDictionary*>(reinterpret_cast<char*>(link) - offsetof(RwTexDictionary, lInInstance));
}

/// ASCII upper case, as the original compares (only 'a' .. 'z' change).
int upper(RwChar c) { return (c >= 'a' && c <= 'z') ? c - 0x20 : c; }

/// The default find (0x8048D96C): the current dictionary, else every dictionary in list order.
RwTexture* TextureDefaultFind(const RwChar* name) {
    rwTextureGlobals& g = globals();
    if (g.currentTexDict != nullptr) return RwTexDictionaryFindNamedTexture(g.currentTexDict, name);
    for (RwLLLink* l = g.texDictList.link.next; l != &g.texDictList.link; l = l->next) {
        if (RwTexture* t = RwTexDictionaryFindNamedTexture(dictOf(l), name)) return t;
    }
    return nullptr;
}

/// The port's default read: the textures are in converted dictionaries, there are no image files to read.
RwTexture* TextureDefaultRead(const RwChar*, const RwChar*) { return nullptr; }

RwTexture* destroyOne(RwTexture* texture, void*) { return RwTextureDestroy(texture) ? texture : nullptr; }

/// RwTexDictionaryCreate's body, which the open also runs (inlined there).
RwTexDictionary* createDict() {
    rwTextureGlobals& g = globals();
    auto* dict = static_cast<RwTexDictionary*>(rwSrcGlobals().memoryAlloc(g.texDictFreeList, kDictHint));
    if (dict == nullptr) return nullptr;
    std::memset(dict, 0, sizeof *dict);
    dict->object = {rwTEXDICTIONARY, 0, 0, 0, nullptr};
    linkAtHead(g.texDictList, dict->lInInstance);
    dict->texturesInDict.link = {&dict->texturesInDict.link, &dict->texturesInDict.link};
    if (_rwPluginRegistryInitObject(&texDictTKList, dict) == nullptr) {   // native: a failed constructor fails it
        unlink(dict->lInInstance);
        rwSrcGlobals().memoryFree(g.texDictFreeList, dict);
        return nullptr;
    }
    return dict;
}

RwTexture* setName(RwTexture* texture, RwChar* field, const RwChar* name) {
    const RwStringFunctions& s = rwSrcGlobals().stringFuncs;
    s.vecStrncpy(field, name, rwTEXTUREBASENAMELENGTH);
    if (s.vecStrlen(name) >= static_cast<std::size_t>(rwTEXTUREBASENAMELENGTH)) {
        RwError error{rwID_COREPLUGIN, _rwerror(E_RW_STRING_TRUNCATION, name, rwTEXTUREBASENAMELENGTH,
                                                rwTEXTUREBASENAMELENGTH - 1, name[rwTEXTUREBASENAMELENGTH - 1])};
        RwErrorSet(&error);
        field[rwTEXTUREBASENAMELENGTH - 1] = 0;
    }
    return texture;
}

bool moduleOpen() {
    if (textureModule.numInstances > 0) return true;
    RwError error{rwID_COREPLUGIN, _rwerror(static_cast<RwInt32>(0x80000001u))};   // E_RW_BADENGINESTATE
    RwErrorSet(&error);
    return false;
}

}  // namespace

RwPluginRegistry& rwTextureRegistry() { return textureTKList; }
RwPluginRegistry& rwTexDictionaryRegistry() { return texDictTKList; }

void* _rwTextureOpen(void* instance, RwInt32 offset, RwInt32 /*size*/) {
    textureModule.globalsOffset = offset;
    rwTextureGlobals& g = globals();
    g.textureFreeList = RwFreeListCreateAndPreallocateSpace(textureTKList.sizeOfStruct, kTexturesPerBlock,
                                                            alignof(RwTexture), kTexturePrealloc, &textureFreeListSpace,
                                                            rwMEMHINTDUR_GLOBAL | rwID_TEXTURE);
    if (g.textureFreeList == nullptr) return nullptr;
    g.texDictFreeList = RwFreeListCreateAndPreallocateSpace(texDictTKList.sizeOfStruct, kDictsPerBlock,
                                                            alignof(RwTexDictionary), kDictPrealloc, &texDictFreeListSpace,
                                                            rwMEMHINTDUR_GLOBAL | rwID_TEXTUREMODULE);
    if (g.texDictFreeList == nullptr) {
        RwFreeListDestroy(g.textureFreeList);
        g.textureFreeList = nullptr;
        return nullptr;
    }
    g.texDictList.link = {&g.texDictList.link, &g.texDictList.link};
    g_defaultTexDict = createDict();
    g.currentTexDict = g_defaultTexDict;
    if (g.currentTexDict == nullptr) {
        RwFreeListDestroy(g.texDictFreeList);
        g.texDictFreeList = nullptr;
        RwFreeListDestroy(g.textureFreeList);
        g.textureFreeList = nullptr;
        return nullptr;   // native: not counted as open (the original has counted it already)
    }
    g.haveTexMipmaps = 0;
    g.generateTexMipmaps = 0;
    g.textureFindCB = TextureDefaultFind;
    g.textureReadCB = TextureDefaultRead;
    ++textureModule.numInstances;
    return instance;
}

void* _rwTextureClose(void* instance, RwInt32 /*offset*/, RwInt32 /*size*/) {
    rwTextureGlobals& g = globals();
    if (g.textureFreeList != nullptr && g.texDictFreeList != nullptr) {
        for (RwLLLink* l = g.texDictList.link.next; l != &g.texDictList.link; l = l->next) {
            if (dictOf(l) == g_defaultTexDict) {
                RwTexDictionaryDestroy(g_defaultTexDict);
                break;
            }
        }
        g_defaultTexDict = nullptr;
    }
    if (g.textureFreeList != nullptr) {
        RwFreeListDestroy(g.textureFreeList);
        g.textureFreeList = nullptr;
    }
    if (g.texDictFreeList != nullptr) {
        RwFreeListDestroy(g.texDictFreeList);
        g.texDictFreeList = nullptr;
    }
    --textureModule.numInstances;
    return instance;
}

RwTexture* RwTextureCreate(RwRaster* raster) {
    if (!moduleOpen()) return nullptr;   // native: no texture module open
    auto* texture = static_cast<RwTexture*>(rwSrcGlobals().memoryAlloc(globals().textureFreeList, kTextureHint));
    if (texture == nullptr) return nullptr;
    std::memset(texture, 0, sizeof *texture);
    texture->dict = nullptr;
    texture->name[0] = 0;
    texture->mask[0] = 0;
    texture->raster = raster;
    texture->refCount = 1;
    texture->filterAddressing = 0;
    texture->filterAddressing = (texture->filterAddressing & 0xFFFF00FFu) | 0x1100u;   // wrap U and V
    texture->filterAddressing = (texture->filterAddressing & 0xFFFFFF00u) | rwFILTERNEAREST;
    if (_rwPluginRegistryInitObject(&textureTKList, texture) == nullptr) {
        // native: a failed plugin constructor fails the creation (the original returns the half-built texture)
        rwSrcGlobals().memoryFree(globals().textureFreeList, texture);
        return nullptr;
    }
    return texture;
}

RwBool RwTextureDestroy(RwTexture* texture) {
    if (--texture->refCount > 0) return 1;
    ++texture->refCount;   // the plugins' destructors see the last reference
    _rwPluginRegistryDeInitObject(&textureTKList, texture);
    if (texture->dict != nullptr) unlink(texture->lInDictionary);
    if (texture->raster != nullptr) {
        RwRasterDestroy(texture->raster);
        texture->raster = nullptr;
    }
    --texture->refCount;
    rwSrcGlobals().memoryFree(globals().textureFreeList, texture);
    return 1;
}

RwTexture* RwTextureSetName(RwTexture* texture, const RwChar* name) { return setName(texture, texture->name, name); }

RwTexture* RwTextureSetMaskName(RwTexture* texture, const RwChar* maskName) {
    return setName(texture, texture->mask, maskName);
}

RwTexture* RwTextureSetRaster(RwTexture* texture, RwRaster* raster) {
    if (raster == nullptr) {
        texture->raster = nullptr;
        return texture;
    }
    return rwSrcGlobals().stdFunc[rwSTANDARDTEXTURESETRASTER](texture, raster, 0) ? texture : nullptr;
}

RwTexture* RwTextureRead(const RwChar* name, const RwChar* maskName) {
    rwTextureGlobals& g = globals();
    if (RwTexture* found = g.textureFindCB(name)) {
        ++found->refCount;
        return found;
    }
    RwTexture* texture = g.textureReadCB(name, maskName);
    if (texture == nullptr) {
        RwError error{rwID_COREPLUGIN, _rwerror(E_RW_READTEXMASK, name, maskName != nullptr ? maskName : "(null)")};
        RwErrorSet(&error);
        return nullptr;
    }
    if (g.currentTexDict != nullptr) RwTexDictionaryAddTexture(g.currentTexDict, texture);
    return texture;
}

RwInt32 RwTextureRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB,
                                RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB) {
    return _rwPluginRegistryAddPlugin(&textureTKList, size, pluginID, constructCB, destructCB, copyCB);
}

RwBool RwTextureSetMipmapping(RwBool enable) {
    globals().haveTexMipmaps = enable;
    return 1;
}
RwBool RwTextureGetMipmapping() { return globals().haveTexMipmaps; }
RwBool RwTextureSetAutoMipmapping(RwBool enable) {
    globals().generateTexMipmaps = enable;
    return 1;
}
RwBool RwTextureGetAutoMipmapping() { return globals().generateTexMipmaps; }

RwBool RwTextureSetReadCallBack(RwTextureCallBackRead callBack) {
    globals().textureReadCB = callBack != nullptr ? callBack : TextureDefaultRead;
    return 1;
}
RwTextureCallBackRead RwTextureGetReadCallBack() { return globals().textureReadCB; }
RwBool RwTextureSetFindCallBack(RwTextureCallBackFind callBack) {
    globals().textureFindCB = callBack != nullptr ? callBack : TextureDefaultFind;
    return 1;
}
RwTextureCallBackFind RwTextureGetFindCallBack() { return globals().textureFindCB; }

RwTexDictionary* RwTexDictionaryCreate() {
    if (!moduleOpen()) return nullptr;
    return createDict();
}

RwBool RwTexDictionaryDestroy(RwTexDictionary* dict) {
    rwTextureGlobals& g = globals();
    if (g.currentTexDict == dict) g.currentTexDict = nullptr;
    RwTexDictionaryForAllTextures(dict, destroyOne, nullptr);
    // native: a texture still referenced elsewhere leaves the dictionary (the original leaves it linked into the freed
    // dictionary, so its later destruction writes freed memory)
    while (dict->texturesInDict.link.next != &dict->texturesInDict.link) {
        RwTexture* survivor = textureOf(dict->texturesInDict.link.next);
        unlink(survivor->lInDictionary);
        survivor->lInDictionary = {nullptr, nullptr};
        survivor->dict = nullptr;
    }
    _rwPluginRegistryDeInitObject(&texDictTKList, dict);
    unlink(dict->lInInstance);
    rwSrcGlobals().memoryFree(g.texDictFreeList, dict);
    return 1;
}

RwTexture* RwTexDictionaryAddTexture(RwTexDictionary* dict, RwTexture* texture) {
    if (texture->dict != nullptr) unlink(texture->lInDictionary);
    texture->dict = dict;
    linkAtHead(dict->texturesInDict, texture->lInDictionary);
    return texture;
}

RwTexture* RwTexDictionaryFindNamedTexture(RwTexDictionary* dict, const RwChar* name) {
    for (RwLLLink* l = dict->texturesInDict.link.next; l != &dict->texturesInDict.link; l = l->next) {
        RwTexture* t = textureOf(l);
        const RwChar* a = t->name;
        const RwChar* b = name;
        while (*a != 0 && *b != 0 && upper(*a) == upper(*b)) ++a, ++b;
        if (*a == 0 && *b == 0) return t;   // a mismatch stops the walk on two non-NUL characters
    }
    return nullptr;
}

const RwTexDictionary* RwTexDictionaryForAllTextures(const RwTexDictionary* dict, RwTextureCallBack fpCallBack,
                                                     void* pData) {
    const RwLLLink* end = &dict->texturesInDict.link;
    for (RwLLLink* l = dict->texturesInDict.link.next; l != end;) {
        RwLLLink* next = l->next;
        if (fpCallBack(textureOf(l), pData) == nullptr) break;
        l = next;
    }
    return dict;
}

RwBool RwTexDictionaryForAllTexDictionaries(RwTexDictionaryCallBack fpCallBack, void* pData) {
    rwTextureGlobals& g = globals();
    for (RwLLLink* l = g.texDictList.link.next; l != &g.texDictList.link;) {
        RwLLLink* next = l->next;
        if (fpCallBack(dictOf(l), pData) == nullptr) break;
        l = next;
    }
    return 1;
}

RwTexDictionary* RwTexDictionarySetCurrent(RwTexDictionary* dict) {
    globals().currentTexDict = dict;
    return dict;
}

RwTexDictionary* RwTexDictionaryGetCurrent() { return globals().currentTexDict; }

RwInt32 RwTextureRegisterPluginStream(RwUInt32 pluginID, RwPluginDataChunkReadCallBack readCB,
                                      RwPluginDataChunkWriteCallBack writeCB, RwPluginDataChunkGetSizeCallBack getSizeCB) {
    return _rwPluginRegistryAddPluginStream(&textureTKList, pluginID, readCB, writeCB, getSizeCB);
}

RwTexture* RwTextureStreamRead(RwStream* stream) {
    RwUInt32 length = 0, version = 0;
    if (!RwStreamFindChunk(stream, rwID_STRUCT, &length, &version)) return nullptr;
    if (version < rwLIBRARYBASEVERSION || version > rwLIBRARYCURRENTVERSION) {
        RwError error{rwID_COREPLUGIN, _rwerror(static_cast<RwInt32>(0x80000004u))};   // E_RW_BADVERSION
        RwErrorSet(&error);
        return nullptr;
    }
    RwUInt32 word = 0;
    const RwUInt32 take = length < 4 ? length : 4;
    if (RwStreamRead(stream, &word, take) != take) return nullptr;
    if (length > take && RwStreamSkip(stream, length - take) == nullptr) return nullptr;   // native: the rest skipped
    RwMemNative32(&word, 4);
    const RwUInt32 filter = word & 0xFFu;
    if (((word >> 12) & 0xFu) == 0) word |= ((word >> 8) & 0xFu) << 12;   // V addressing defaults to U's
    const RwUInt32 flags = (word >> 16) & 0xFFu;
    const RwBool mipmapping = RwTextureGetMipmapping(), autoMipmapping = RwTextureGetAutoMipmapping();
    if ((filter >= rwFILTERMIPNEAREST && filter <= rwFILTERLINEARMIPNEAREST) || filter == rwFILTERLINEARMIPLINEAR) {
        RwTextureSetMipmapping(1);
        RwTextureSetAutoMipmapping((flags & 1u) ? 0 : 1);
    } else {
        RwTextureSetMipmapping(0);
        RwTextureSetAutoMipmapping(0);
    }
    const auto restore = [&] {
        RwTextureSetMipmapping(mipmapping);
        RwTextureSetAutoMipmapping(autoMipmapping);
    };
    RwChar name[128], mask[128];
    if (_rwStringStreamFindAndReadCapped(name, sizeof name, stream) == nullptr ||
        _rwStringStreamFindAndReadCapped(mask, sizeof mask, stream) == nullptr) {
        restore();
        return nullptr;
    }
    RwTexture* texture = RwTextureRead(name, mask);
    restore();
    if (texture == nullptr) {
        _rwPluginRegistrySkipDataChunks(&textureTKList, stream);
        return nullptr;
    }
    if (texture->refCount == 1) {
        texture->filterAddressing = word & 0xFFFFu;
        if (_rwPluginRegistryReadDataChunks(&textureTKList, stream, texture) == nullptr) {
            RwTextureDestroy(texture);   // native: the original keeps the reference it took
            return nullptr;
        }
    } else if (_rwPluginRegistrySkipDataChunks(&textureTKList, stream) == nullptr) {
        RwTextureDestroy(texture);   // native: drops the reference RwTextureRead added
        return nullptr;
    }
    return texture;
}

RwTexDictionary* RwTexDictionaryStreamRead(RwStream* stream) {
    RwUInt32 length = 0, version = 0;
    if (!RwStreamFindChunk(stream, rwID_STRUCT, &length, &version)) return nullptr;
    if (version < rwLIBRARYBASEVERSION || version > rwLIBRARYCURRENTVERSION) {
        RwError error{rwID_COREPLUGIN, _rwerror(static_cast<RwInt32>(0x80000004u))};   // E_RW_BADVERSION
        RwErrorSet(&error);
        return nullptr;
    }
    RwUInt32 word = 0;
    const RwUInt32 take = length < 4 ? length : 4;
    if (RwStreamRead(stream, &word, take) != take) return nullptr;
    if (length > take && RwStreamSkip(stream, length - take) == nullptr) return nullptr;   // native: the rest skipped
    RwMemNative32(&word, 4);
    RwUInt16 count = static_cast<RwUInt16>(word & 0xFFFFu);
    const RwUInt16 deviceID = static_cast<RwUInt16>(word >> 16);
    RwUInt16 openDevice = 0;
    _rwDeviceSystemRequest(&rwSrcGlobals().dOpenDevice, 0x16 /* rwDEVICESYSTEMGETID */, &openDevice, nullptr, 0);
    openDevice &= 0xFFu;
    if (openDevice != 0 && deviceID != 0 && openDevice != deviceID) return nullptr;
    RwTexDictionary* dict = RwTexDictionaryCreate();
    if (dict == nullptr) return nullptr;
    const auto fail = [&]() -> RwTexDictionary* {
        RwTexDictionaryForAllTextures(dict, destroyOne, nullptr);
        RwTexDictionaryDestroy(dict);
        return nullptr;
    };
    while (count-- != 0) {
        RwUInt32 textureLength = 0, textureVersion = 0;
        if (!RwStreamFindChunk(stream, rwID_TEXTURENATIVE, &textureLength, &textureVersion)) return fail();
        if (textureVersion < rwLIBRARYBASEVERSION || textureVersion > rwLIBRARYCURRENTVERSION) {
            fail();
            RwError error{rwID_COREPLUGIN, _rwerror(static_cast<RwInt32>(0x80000004u))};
            RwErrorSet(&error);
            return nullptr;
        }
        RwTexture* texture = nullptr;
        if (!rwSrcGlobals().stdFunc[26](stream, &texture, static_cast<RwInt32>(textureLength))) return fail();
        if (texture == nullptr) return fail();
        if (_rwPluginRegistryReadDataChunks(&textureTKList, stream, texture) == nullptr) {
            RwTextureDestroy(texture);   // native: the original leaks it (not yet in the dictionary)
            return fail();
        }
        RwTexDictionaryAddTexture(dict, texture);
    }
    if (_rwPluginRegistryReadDataChunks(&texDictTKList, stream, dict) == nullptr) return fail();
    return dict;
}

RwBool rwHostNativeTextureRead(void* streamIn, void* textureOut, RwInt32 /*length*/) {
    auto* stream = static_cast<RwStream*>(streamIn);
    RwUInt32 structLength = 0, version = 0;
    if (!RwStreamFindChunk(stream, rwID_STRUCT, &structLength, &version)) return 0;
    struct Header {
        RwUInt32 platform, filterAddressing;
        RwChar name[32], mask[32];
        RwUInt32 width, height, levels;
    } h{};
    if (structLength < sizeof h || RwStreamRead(stream, &h, sizeof h) != sizeof h) return 0;
    RwMemNative32(&h.platform, 8);
    RwMemNative32(&h.width, 12);
    if (h.platform != rwHOSTTEXTUREPLATFORM || h.width == 0 || h.height == 0 || h.levels == 0 || h.width > 16384 ||
        h.height > 16384 || h.levels > 15) {
        return 0;
    }
    h.name[31] = 0, h.mask[31] = 0;
    const RwInt32 flags = rwRASTERTYPETEXTURE | rwRASTERFORMAT8888 | (h.levels > 1 ? rwRASTERFORMATMIPMAP : 0);
    RwRaster* raster = RwRasterCreate(static_cast<RwInt32>(h.width), static_cast<RwInt32>(h.height), 32, flags);
    if (raster == nullptr) return 0;
    std::size_t consumed = sizeof h;
    const RwInt32 levels = RwRasterGetNumLevels(raster);
    for (RwUInt32 level = 0; level < h.levels; ++level) {
        RwUInt32 bytes = 0;
        if (RwStreamRead(stream, &bytes, 4) != 4) return RwRasterDestroy(raster), 0;
        RwMemNative32(&bytes, 4);
        consumed += 4;
        if (static_cast<RwInt32>(level) >= levels) {   // more levels than the raster holds: skip them
            if (RwStreamSkip(stream, bytes) == nullptr) return RwRasterDestroy(raster), 0;
        } else {
            RwUInt8* pixels = RwRasterLock(raster, static_cast<RwUInt8>(level), rwRASTERLOCKWRITE);
            const std::size_t expected = pixels != nullptr ? static_cast<std::size_t>(raster->stride) * raster->height : 0;
            const bool ok = pixels != nullptr && bytes == expected && RwStreamRead(stream, pixels, bytes) == bytes;
            RwRasterUnlock(raster);
            if (!ok) return RwRasterDestroy(raster), 0;
        }
        consumed += bytes;
    }
    if (structLength > consumed && RwStreamSkip(stream, static_cast<RwUInt32>(structLength - consumed)) == nullptr) {
        return RwRasterDestroy(raster), 0;
    }
    RwTexture* texture = RwTextureCreate(raster);
    if (texture == nullptr) return RwRasterDestroy(raster), 0;
    RwTextureSetName(texture, h.name);
    RwTextureSetMaskName(texture, h.mask);
    texture->filterAddressing = h.filterAddressing;
    *static_cast<RwTexture**>(textureOut) = texture;
    return 1;
}
