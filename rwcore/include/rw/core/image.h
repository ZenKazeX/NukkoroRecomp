// RenderWare Graphics 3.7 core: images (module baimage, with baimras's image / raster exchange), native x64.
//
// An image is device-independent pixels: depth 4 or 8 (one palette index a byte, a palette of 1 << depth RwRGBA) or
// 32 (RwRGBA a pixel), rows `stride` bytes apart. The module also keeps the image search path, the gamma tables
// (built at RwEngineStart from the device's gamma) and the list of registered image file formats, whose read
// callbacks RwImageReadMaskedImage / RwImageFindFileType try along the path.
//
// Written from PAL main.dol 0x80488964..0x8048AB7C (and _rwpathisabsolute 0x80484494). The game links only part of
// RenderWare's baimage: not RwImageRead (inlined into RwImageReadMaskedImage), RwImageResize, RwImageWrite,
// RwImageRegisterImageFormat, RwImageGetPath / GetGamma, RwRGBAToPixel, RwRGBAGammaCorrect, RwImageRegisterPlugin,
// the image streams - those have no original to prove against and are not provided. Without
// RwImageRegisterImageFormat nothing in the game adds a format, so its format list stays empty and every image read
// finds nothing; the list is the module's own (rwImageGlobals::imageFormatList), as in RenderWare. Native structures
// (64-bit pointers, size_t buffer sizes); results, errors, gamma tables and pixel conversions are the original's.
// Proof: runtime/tests/rwcore_image_tests.cpp (oracle) and rwcore_image_native_tests.cpp; rwcore/README.md.
#pragma once

#include <cstddef>

#include "rw/core/engine.h"
#include "rw/core/plugin.h"
#include "rw/core/raster.h"
#include "rw/core/types.h"

/// A colour (rwplcore.h RwRGBA).
struct RwRGBA {
    RwUInt8 red;
    RwUInt8 green;
    RwUInt8 blue;
    RwUInt8 alpha;
};

/// RwImageFlag (rwcore.h).
inline constexpr RwInt32 rwNAIMAGEFLAG = 0x00;
inline constexpr RwInt32 rwIMAGEALLOCATED = 0x1;        ///< the pixels (and palette) belong to the image
inline constexpr RwInt32 rwIMAGEGAMMACORRECTED = 0x2;

/// An image (rwcore.h RwImage).
struct RwImage {
    RwInt32 flags;
    RwInt32 width;
    RwInt32 height;
    RwInt32 depth;     ///< bits per pixel: 4, 8 (palettised), 32
    RwInt32 stride;    ///< bytes per row
    RwUInt8* cpPixels;
    RwRGBA* palette;
};

using RwImageCallBackRead = RwImage* (*)(const RwChar* imageName);
using RwImageCallBackWrite = RwImage* (*)(RwImage* image, const RwChar* imageName);

/// A registered image file format: the extension in lower and upper case (with the dot, at most 19 characters) and
/// its read / write callbacks. Descriptive field names: the structure is private to RenderWare's baimage.
struct rwImageFormat {
    RwChar lcExtension[20];
    RwChar ucExtension[20];
    RwImageCallBackRead imageRead;
    RwImageCallBackWrite imageWrite;
    rwImageFormat* nextFormat;
};

/// The image module's globals (RenderWare's rwImageGlobals; native: size_t buffer sizes).
struct rwImageGlobals {
    RwFreeList* imageFreeList;
    RwChar* imagePath;                  ///< the search path: directories separated by ';'
    std::size_t imagePathSize;          ///< bytes allocated for it
    RwUInt8 gammaTable[256];            ///< c -> 255 (c / 255)^(1 / gamma)
    RwUInt8 invGammaTable[256];         ///< c -> 255 (c / 255)^gamma
    RwReal gammaVal;
    RwChar* scratchMem;                 ///< the full names built along the path
    std::size_t scratchMemSize;
    RwFreeList* imageFormatFreeList;
    rwImageFormat* imageFormatList;     ///< the newest first
};

extern RwModuleInfo imageModule;

/// The images' plugin registry (imageTKList, 0x8056F0C0). The game links no RwImageRegisterPlugin; plugins come
/// through _rwPluginRegistryAddPlugin on it.
RwPluginRegistry& rwImageRegistry();

/// Opens the module: the image free list (128 to a block, one preallocated, hint rwMEMHINTDUR_GLOBAL | rwID_IMAGE),
/// the format free list (the same, hint rwMEMHINTDUR_GLOBAL | rwID_IMAGEMODULE), an empty 256-byte path, gamma 1, no
/// formats, a 256-byte scratch buffer. NULL when a step fails (everything undone; native: not counted as open - the
/// original has counted it before the scratch buffer). Original: 0x80488964.
void* _rwImageOpen(void* instance, RwInt32 offset, RwInt32 size);

/// Frees the scratch buffer and the path, every format, both free lists. Returns instance. Original: 0x80488C00.
void* _rwImageClose(void* instance, RwInt32 offset, RwInt32 size);

/// An image of width x height x depth without pixels (flags 0), the plugins' constructors run. NULL when the
/// allocation (native: or a plugin constructor) fails, or no image module is open. Native: the stride starts at 0
/// (the original leaves the free list's bytes). Original: 0x80488D58.
RwImage* RwImageCreate(RwInt32 width, RwInt32 height, RwInt32 depth);

/// Frees the pixels when the image owns them, runs the plugin destructors, frees the image. TRUE. Original:
/// 0x80488E00.
RwBool RwImageDestroy(RwImage* image);

/// Allocates the pixels: stride = ((depth + 7) / 8) x width rounded up to 4, then stride x height bytes and, for
/// depths 4 and 8, the palette after them (one block, hint rwMEMHINTDUR_EVENT | rwID_IMAGE); the image owns them.
/// NULL with E_RW_NOMEM (the stride is set either way). Native: a negative size or a stride beyond RwInt32 fails
/// with E_RW_NOMEM before any allocation (the original's 32-bit arithmetic wraps). Original: 0x80488E68.
RwImage* RwImageAllocatePixels(RwImage* image);

/// Frees the pixels (whoever owns them: the original frees cpPixels unconditionally); no pixels, no palette, not
/// owned. Returns image. Original: 0x80488FA0.
RwImage* RwImageFreePixels(RwImage* image);

/// Each palette entry's (depths 4, 8) or pixel's (32) alpha becomes the largest of its red, green and blue; other
/// depths are left. Returns image. Original: 0x80488FF8.
RwImage* RwImageMakeMask(RwImage* image);

/// The mask's alpha into the image (sizes must match, else E_RW_INVIMAGEMASK): a palettised image is first converted
/// to depth 32 (its pixels reallocated); each pixel takes the alpha of the mask's pixel (its palette entry for a
/// palettised mask; other mask depths change nothing). NULL with E_RW_INVIMAGEFORMAT for image depths other than 4,
/// 8, 32, or when the conversion's image cannot be made. Native: when the image's new pixels cannot be allocated it
/// returns NULL (depth 32, no pixels; the original copies into the NULL pixels). Original: 0x804890D8.
RwImage* RwImageApplyMask(RwImage* image, const RwImage* mask);

/// The image search path (directories separated by ';', each ending in its separator), copied into the module's
/// buffer (grown with RwRealloc, hint rwMEMHINTDUR_GLOBAL | rwMEMHINTFLAG_RESIZEABLE | rwID_IMAGEMODULE). Returns
/// path, NULL with E_RW_NOMEM. Original: 0x8048962C.
const RwChar* RwImageSetPath(const RwChar* path);

/// The extension of the registered format with which imageName exists along the path: for each full name (the name
/// itself when it is absolute or there is no path, else each path directory + the name), each format's lower- then
/// upper-case extension appended, the first that RwFexist finds. NULL when none. Original: 0x80489BA8.
const RwChar* RwImageFindFileType(const RwChar* imageName);

/// The image read by the format its extension names (compared case-sensitively, after the last ':', '/' or '\\'),
/// along the path (the first full name that exists and reads); with a mask name (not empty), the mask read the same
/// way, made a mask (RwImageMakeMask) and applied (RwImageApplyMask), then destroyed. NULL when either is not read or
/// the mask does not apply (both destroyed). Original: 0x80489BE4 (RwImageRead inlined twice).
RwImage* RwImageReadMaskedImage(const RwChar* imageName, const RwChar* maskName);

/// The device's colour of a pixel value in a raster format (rwSTANDARDPIXELTORGB). Returns rgbOut. Original:
/// 0x8048A168.
RwRGBA* RwRGBASetFromPixel(RwRGBA* rgbOut, RwUInt32 pixelValue, RwInt32 rasterFormat);

/// Copies sourceImage's pixels into destImage (both allocated, the same size): equal depths row by row (and the
/// palette for depths up to 8, when both have one), 4 -> 8 the indices, 4 / 8 -> 32 through the source palette;
/// destImage takes the source's gamma-corrected flag. Other conversions fail with E_RW_INVIMAGEFORMAT: NULL (native:
/// destImage unchanged; the original then writes the flag through NULL). Original: 0x8048A408 (ImageConvertDepth
/// 0x8048A1AC).
RwImage* RwImageCopy(RwImage* destImage, const RwImage* sourceImage);

// ---- baresamp: resampling (0x8048B1C8..0x8048BE30; rwcore/src/resample.cpp) ------------------------------------
// A box filter in 16.16 fixed point: each destination pixel is the coverage-weighted average of the source pixels
// under it, channels as reals (byte x 1/255), back to bytes as (int)(255 x c + 0.5) with the multiply-add fused, as
// the original does. Both images 32 bits deep (RenderWare's debug build asserts it).

/// Resamples srcImage into dstImage's size (both 32 bits deep, dstImage's pixels allocated): steps (int)(65536 x
/// (float)src / (float)dst) per axis, each destination pixel averaged over its source box (ImageResampleGetAvgPixel,
/// 0x8048B55C, over ImageResampleGetSpan, 0x8048B1C8); dstImage takes srcImage's gamma-corrected flag. Returns
/// dstImage. Native: NULL, dstImage unchanged, for a depth other than 32, missing pixels, or a step below 1 with a
/// destination to fill (a source of size 0, or a destination over 65536 times larger: the original then loops on a
/// wrapped count), or a source over 32766 pixels wide or high (its 16.16 coordinates overflow); a source pixel past the image (the step's float rounding can reach one for images of hundreds of
/// pixels) is read from its last row / column (the original reads the memory after the row or the buffer).
/// Original: 0x8048B7E0.
RwImage* RwImageResample(RwImage* dstImage, const RwImage* srcImage);

/// A new 32-bit image of width x height resampled from srcImage (RwImageResample; a source of another depth through a
/// 32-bit copy, RwImageCopy). NULL when an image or its pixels cannot be allocated (whatever was made is freed).
/// Native: NULL for width or height < 1 (RenderWare's debug build asserts both > 0) and when the resample refuses.
/// Original: 0x8048B9BC (RwImageResample inlined twice; labelled TextureRasterDefaultBuildMipmaps__sub6 by the naming
/// project).
RwImage* RwImageCreateResample(const RwImage* srcImage, RwInt32 width, RwInt32 height);

/// Applies the gamma table to the red, green and blue of the palette (depths 4, 8) or every pixel (32), alpha kept,
/// and marks the image gamma-corrected. NULL with E_RW_NULLP without a palette / pixels, E_RW_INVIMAGEDEPTH for other
/// depths. Original: 0x8048A508.
RwImage* RwImageGammaCorrect(RwImage* image);

/// The gamma tables for gammaValue: entry c of the gamma table is (int)(0.5 + 255 (float)pow(c / 255, 1 / gamma))
/// and of the inverse table with pow(c / 255, gamma), entry 0 being 0 - the original's single-precision steps and
/// MSL's pow (math/msl.h), stored as the low byte of fctiwz. TRUE. Original: 0x8048A880.
RwBool RwImageSetGamma(RwReal gammaValue);

/// The image from the raster, through the device (rwSTANDARDIMAGEGETRASTER); gamma-corrected when the raster is.
/// image, or NULL when the device refuses. Original: 0x8048A9F8.
RwImage* RwImageSetFromRaster(RwImage* image, RwRaster* raster);

/// The raster from the image, through the device (rwSTANDARDRASTERSETIMAGE); the raster marked gamma-corrected
/// (privateFlags bit 0) when the image is. raster, or NULL when the device refuses. Original: 0x8048AA6C.
RwRaster* RwRasterSetFromImage(RwRaster* raster, RwImage* image);

/// The raster the device would make for the image (rwSTANDARDIMAGEFINDRASTERFORMAT on a raster of the stack): its
/// width, height, depth and format (type | format bits). image, or NULL when the device refuses (outputs untouched).
/// Native: the stack raster starts zeroed (the original's is uninitialised). Original: 0x8048AAE0.
RwImage* RwImageFindRasterFormat(RwImage* ipImage, RwInt32 nRasterType, RwInt32* npWidth, RwInt32* npHeight,
                                 RwInt32* npDepth, RwInt32* npFormat);

inline RwInt32 RwImageGetWidth(const RwImage* image) { return image->width; }
inline RwInt32 RwImageGetHeight(const RwImage* image) { return image->height; }
inline RwInt32 RwImageGetDepth(const RwImage* image) { return image->depth; }
inline RwInt32 RwImageGetStride(const RwImage* image) { return image->stride; }
inline RwUInt8* RwImageGetPixels(const RwImage* image) { return image->cpPixels; }
inline RwRGBA* RwImageGetPalette(const RwImage* image) { return image->palette; }
inline RwImage* RwImageSetStride(RwImage* image, RwInt32 stride) {
    image->stride = stride;
    return image;
}
inline RwImage* RwImageSetPixels(RwImage* image, RwUInt8* pixels) {
    image->cpPixels = pixels;
    return image;
}
inline RwImage* RwImageSetPalette(RwImage* image, RwRGBA* palette) {
    image->palette = palette;
    return image;
}
