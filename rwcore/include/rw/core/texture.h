// RenderWare Graphics 3.7 core: textures and texture dictionaries (module batextur, CPU side), native x64.
//
// A texture is a named, reference-counted raster with filtering and addressing modes; a dictionary is a list of
// textures. RwTextureRead finds a texture by name (in the current dictionary, or in every dictionary when there is no
// current one) and otherwise asks the read callback to load it; a loaded texture joins the current dictionary. The
// GameCube's default read callback loads image files from the disc and builds rasters (with mipmaps) through its
// driver; the port loads stage textures from offline-converted dictionaries instead, so its default read callback finds
// nothing (RwTextureSetReadCallBack installs a loader) - the dictionaries and lookups are the original's.
//
// Written from PAL main.dol batextur 0x8048D96C..0x8048E80C. Proof: runtime/tests/rwcore_texture_tests.cpp (oracle)
// and rwcore_texture_native_tests.cpp. Not provided: the image-file reading (TextureDefaultRead 0x8048D644 with
// 0x8048CDD4 / 0x8048D088, TextureRasterDefaultBuildMipmaps 0x8048D684, RwTextureGenerateMipmapName 0x8048E13C and
// its default 0x8048C6FC) - the converter makes the textures, mip levels included; the native texture stream read
// comes with the materials.
#pragma once

#include "rw/core/engine.h"
#include "rw/core/frame.h"
#include "rw/core/plugin.h"
#include "rw/core/raster.h"
#include "rw/core/types.h"

inline constexpr RwUInt8 rwTEXDICTIONARY = 6;   ///< RwObject::type of a texture dictionary

enum RwTextureFilterMode : RwUInt32 {
    rwFILTERNAFILTERMODE = 0,
    rwFILTERNEAREST = 1,
    rwFILTERLINEAR = 2,
    rwFILTERMIPNEAREST = 3,
    rwFILTERMIPLINEAR = 4,
    rwFILTERLINEARMIPNEAREST = 5,
    rwFILTERLINEARMIPLINEAR = 6,
};

enum RwTextureAddressMode : RwUInt32 {
    rwTEXTUREADDRESSNATEXTUREADDRESS = 0,
    rwTEXTUREADDRESSWRAP = 1,
    rwTEXTUREADDRESSMIRROR = 2,
    rwTEXTUREADDRESSCLAMP = 3,
    rwTEXTUREADDRESSBORDER = 4,
};

inline constexpr RwInt32 rwTEXTUREBASENAMELENGTH = 32;

struct RwTexDictionary;

/// A texture (rwcore.h RwTexture).
struct RwTexture {
    RwRaster* raster;
    RwTexDictionary* dict;
    RwLLLink lInDictionary;
    RwChar name[rwTEXTUREBASENAMELENGTH];
    RwChar mask[rwTEXTUREBASENAMELENGTH];
    RwUInt32 filterAddressing;   ///< filter mode (bits 0-7), U address (8-11), V address (12-15)
    RwInt32 refCount;
};

/// A texture dictionary (rwcore.h RwTexDictionary).
struct RwTexDictionary {
    RwObject object;
    RwLinkList texturesInDict;
    RwLLLink lInInstance;
};

using RwTextureCallBackRead = RwTexture* (*)(const RwChar* name, const RwChar* maskName);
using RwTextureCallBackFind = RwTexture* (*)(const RwChar* name);
using RwTextureCallBack = RwTexture* (*)(RwTexture* texture, void* pData);
using RwTexDictionaryCallBack = RwTexDictionary* (*)(RwTexDictionary* dict, void* data);

/// The texture module's globals (the GameCube's, without the image-file reading's buffer and mipmap callbacks).
struct rwTextureGlobals {
    RwLinkList texDictList;
    RwFreeList* textureFreeList;
    RwFreeList* texDictFreeList;
    RwTexDictionary* currentTexDict;
    RwTextureCallBackRead textureReadCB;
    RwTextureCallBackFind textureFindCB;
    RwBool haveTexMipmaps;
    RwBool generateTexMipmaps;
};

extern RwModuleInfo textureModule;

/// The registries of textures (textureTKList, 0x8056F0F0) and dictionaries (texDictTKList, 0x8056F108).
RwPluginRegistry& rwTextureRegistry();
RwPluginRegistry& rwTexDictionaryRegistry();

/// Opens the module: the texture free list (128 to a block, hint rwMEMHINTDUR_GLOBAL | rwID_TEXTURE), the
/// dictionary free list (5 to a block, hint rwMEMHINTDUR_GLOBAL | rwID_TEXTUREMODULE), an empty dictionary list, a
/// default dictionary that becomes the current one, mipmapping off, the default find and read callbacks. NULL when a
/// step fails (everything undone; native: the instance count too - the original leaves it counted). Original:
/// 0x8048E52C.
void* _rwTextureOpen(void* instance, RwInt32 offset, RwInt32 size);

/// Destroys the default dictionary (its textures with it; the current dictionary is cleared when it was the default)
/// and both free lists. Returns instance. Original: 0x8048E350.
void* _rwTextureClose(void* instance, RwInt32 offset, RwInt32 size);

/// A texture of the raster: no name or mask, no dictionary, one reference, nearest filtering, wrap addressing
/// (0x1101), the plugins' constructors run. NULL when the allocation (native: or a plugin constructor) fails, or no
/// texture module is open. Original: 0x8048DD2C.
RwTexture* RwTextureCreate(RwRaster* raster);

/// Drops a reference; the last one runs the plugin destructors (with refCount back at 1 while they run), leaves the
/// dictionary, destroys the raster and frees the texture. TRUE. Original: 0x8048DDDC.
RwBool RwTextureDestroy(RwTexture* texture);

/// The name / mask name, copied with the engine's strncpy into 32 bytes; a longer name is cut at 31 characters with
/// E_RW_STRING_TRUNCATION. Returns texture. Originals: 0x8048DEA4, 0x8048DF50.
RwTexture* RwTextureSetName(RwTexture* texture, const RwChar* name);
RwTexture* RwTextureSetMaskName(RwTexture* texture, const RwChar* maskName);

/// The raster through the device (rwSTANDARDTEXTURESETRASTER); NULL clears it. texture, or NULL when the device
/// refuses. Original: 0x8048DA50.
RwTexture* RwTextureSetRaster(RwTexture* texture, RwRaster* raster);

/// Finds name (a found texture gains a reference) or reads it with the read callback; a read texture joins the
/// current dictionary. NULL with error 0x16 (E_RW_READTEXMASK: name, mask or "(null)") when neither finds it.
/// Original: 0x8048E180.
RwTexture* RwTextureRead(const RwChar* name, const RwChar* maskName);

/// A texture plugin (before the engine opens). Its offset or -1. Original: 0x8048E2D0.
RwInt32 RwTextureRegisterPlugin(RwInt32 size, RwUInt32 pluginID, RwPluginObjectConstructor constructCB,
                                RwPluginObjectDestructor destructCB, RwPluginObjectCopy copyCB);

/// Whether rasters made for read textures have / generate mip levels. TRUE. Originals: 0x8048D9F8, 0x8048DA10,
/// 0x8048DA24, 0x8048DA3C.
RwBool RwTextureSetMipmapping(RwBool enable);
RwBool RwTextureGetMipmapping();
RwBool RwTextureSetAutoMipmapping(RwBool enable);
RwBool RwTextureGetAutoMipmapping();

/// The callbacks RwTextureRead uses (RenderWare API; native: the port's loaders install a read callback).
RwBool RwTextureSetReadCallBack(RwTextureCallBackRead callBack);
RwTextureCallBackRead RwTextureGetReadCallBack();
RwBool RwTextureSetFindCallBack(RwTextureCallBackFind callBack);
RwTextureCallBackFind RwTextureGetFindCallBack();

inline RwRaster* RwTextureGetRaster(const RwTexture* t) { return t->raster; }
inline const RwChar* RwTextureGetName(const RwTexture* t) { return t->name; }
inline const RwChar* RwTextureGetMaskName(const RwTexture* t) { return t->mask; }
inline RwTextureFilterMode RwTextureGetFilterMode(const RwTexture* t) {
    return static_cast<RwTextureFilterMode>(t->filterAddressing & 0xFFu);
}
inline RwTexture* RwTextureSetFilterMode(RwTexture* t, RwTextureFilterMode mode) {
    t->filterAddressing = (t->filterAddressing & ~0xFFu) | mode;
    return t;
}
inline RwTextureAddressMode RwTextureGetAddressingU(const RwTexture* t) {
    return static_cast<RwTextureAddressMode>((t->filterAddressing >> 8) & 0x0Fu);
}
inline RwTextureAddressMode RwTextureGetAddressingV(const RwTexture* t) {
    return static_cast<RwTextureAddressMode>((t->filterAddressing >> 12) & 0x0Fu);
}
inline RwTexture* RwTextureSetAddressingU(RwTexture* t, RwTextureAddressMode mode) {
    t->filterAddressing = (t->filterAddressing & ~0x0F00u) | (mode << 8);
    return t;
}
inline RwTexture* RwTextureSetAddressingV(RwTexture* t, RwTextureAddressMode mode) {
    t->filterAddressing = (t->filterAddressing & ~0xF000u) | (mode << 12);
    return t;
}
inline RwTexture* RwTextureAddRef(RwTexture* t) {
    ++t->refCount;
    return t;
}

/// An empty dictionary, linked at the head of the module's dictionary list, its plugins' constructors run. NULL when
/// the allocation (native: or a plugin constructor) fails, or no texture module is open. Original: 0x8048DAB4.
RwTexDictionary* RwTexDictionaryCreate();

/// Destroys every texture in it (RwTextureDestroy: drops a reference each), runs the plugin destructors, leaves the
/// module's list and frees it; the current dictionary is cleared when it was this one. TRUE. Original: 0x8048DB88.
RwBool RwTexDictionaryDestroy(RwTexDictionary* dict);

/// Moves the texture into the dictionary (out of the one it was in), at its head. Returns texture. Original:
/// 0x8048DFFC.
RwTexture* RwTexDictionaryAddTexture(RwTexDictionary* dict, RwTexture* texture);

/// The first texture whose name equals name ignoring ASCII case, or NULL. Original: 0x8048E04C.
RwTexture* RwTexDictionaryFindNamedTexture(RwTexDictionary* dict, const RwChar* name);

/// fpCallBack for every texture (from the head) until one returns NULL. Returns dict. Original: 0x8048DC30.
const RwTexDictionary* RwTexDictionaryForAllTextures(const RwTexDictionary* dict, RwTextureCallBack fpCallBack,
                                                     void* pData);

/// fpCallBack for every dictionary of the module (from the head) until one returns NULL. TRUE. Original: 0x8048DCA4.
RwBool RwTexDictionaryForAllTexDictionaries(RwTexDictionaryCallBack fpCallBack, void* pData);

/// The current dictionary (NULL: RwTextureRead searches them all). Originals: 0x8048E114, 0x8048E128.
RwTexDictionary* RwTexDictionarySetCurrent(RwTexDictionary* dict);
RwTexDictionary* RwTexDictionaryGetCurrent();

/// A texture plugin's stream callbacks. Its offset or -1. Original: 0x80484B4C.
RwInt32 RwTextureRegisterPluginStream(RwUInt32 pluginID, RwPluginDataChunkReadCallBack readCB,
                                      RwPluginDataChunkWriteCallBack writeCB, RwPluginDataChunkGetSizeCallBack getSizeCB);

/// A texture from a stream: its struct chunk (the filter-addressing word: filter bits 0-7, U 8-11, V 12-15 - V
/// copies U when 0 -, bit 16 "no automatic mip levels"; versions 3.5.0.0 .. 3.7.0.2, else E_RW_BADVERSION), its name
/// and mask (string chunks), then RwTextureRead with mipmapping switched on for the mip filters (3 .. 6) and off
/// otherwise, the previous switches restored. A newly read texture (one reference) takes the stream's filter-addressing
/// word and its plugin data; a found one skips them. NULL when a read fails or no texture is found (its plugin data
/// skipped). Native: the struct is read into its 4 bytes and the rest skipped, names into 128-byte buffers (the
/// original reads both unbounded into 4- and 128-byte stack buffers), and a failed plugin read drops the reference the
/// read took (the original leaves it). Original: 0x80484D78.
RwTexture* RwTextureStreamRead(RwStream* stream);

/// A texture dictionary from a stream: its struct {texture count, device id} (the open device's id must match unless
/// either is 0), then each texture's native chunk (rwID_TEXTURENATIVE, read by the device's rwSTANDARDNATIVETEXTUREREAD)
/// and plugin data, added to the dictionary; then the dictionary's plugin data. NULL when a read fails (the dictionary
/// and its textures destroyed; native: also the texture whose plugin data failed, which the original leaks; the struct
/// read into its 4 bytes and the rest skipped - the original reads the whole chunk into a 4-byte stack buffer).
/// Original: 0x8048500C.
RwTexDictionary* RwTexDictionaryStreamRead(RwStream* stream);

/// The host device's native texture (the platform the offline converter writes; 'HOST', 0x54534F48): one struct chunk
/// {platform, filter-addressing word, name[32], mask[32], width, height, level count, then per level its byte count and
/// R G B A pixels (rows packed)}. rwSTANDARDNATIVETEXTUREREAD of the host device reads it into a texture of an 8888
/// raster (with mip levels when there are more than one). Native: RenderWare's PC drivers read their own formats here.
inline constexpr RwUInt32 rwHOSTTEXTUREPLATFORM = 0x54534F48u;
RwBool rwHostNativeTextureRead(void* stream, void* textureOut, RwInt32 length);
