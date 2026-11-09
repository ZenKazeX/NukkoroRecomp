// RenderWare images (module baimage and baimras's image / raster exchange; see rw/core/image.h).
#include "rw/core/image.h"

#include <cstdint>
#include <cstring>
#include <limits>

#include "math/msl.h"
#include "rw/core/error.h"
#include "rw/core/memory.h"
#include "rw/core/stream.h"

RwModuleInfo imageModule{};

namespace {

// The free lists' creation parameters (baimage statics 0x805EEF98 = 128 a block, 0x805EEF9C / 0x805EEFA0 = one
// preallocated block) and in-place headers (0x805E3D60, 0x805E3D84).
constexpr RwInt32 kPerBlock = 128, kImagePrealloc = 1, kFormatPrealloc = 1;
constexpr RwUInt32 rwID_IMAGE = 0x18u, rwID_IMAGEMODULE = 0x406u;
constexpr RwUInt32 rwMEMHINTDUR_EVENT = 0x00030000u, rwMEMHINTFLAG_RESIZEABLE = 0x01000000u;
constexpr RwUInt32 kImageHint = rwMEMHINTDUR_EVENT | rwID_IMAGE;                                  // images, pixels
constexpr RwUInt32 kPathHint = rwMEMHINTDUR_GLOBAL | rwMEMHINTFLAG_RESIZEABLE | rwID_IMAGEMODULE;  // the path
constexpr RwUInt32 kScratchHint = rwMEMHINTDUR_GLOBAL | rwMEMHINTFLAG_RESIZEABLE | rwID_IMAGE;     // full names
constexpr std::size_t kInitialPathSize = 256, kInitialScratchSize = 256;
constexpr std::size_t kTypeNameRoom = 20;   // RwImageFindFileType: an extension appended to the full name
constexpr std::size_t kReadNameRoom = 5;    // the image read

constexpr RwInt32 E_RW_BADENGINESTATE = static_cast<RwInt32>(0x80000001u);
constexpr RwInt32 E_RW_INVIMAGEDEPTH = static_cast<RwInt32>(0x80000008u);
constexpr RwInt32 E_RW_INVIMAGEFORMAT = static_cast<RwInt32>(0x80000009u);
constexpr RwInt32 E_RW_INVIMAGEMASK = static_cast<RwInt32>(0x8000000Au);
constexpr RwInt32 E_RW_NULLP = static_cast<RwInt32>(0x80000016u);

constexpr RwInt32 rwSTANDARDPIXELTORGB = 3, rwSTANDARDIMAGEGETRASTER = 6, rwSTANDARDRASTERSETIMAGE = 7,
                  rwSTANDARDIMAGEFINDRASTERFORMAT = 9;

RwFreeList imageFreeListSpace{}, imageFormatFreeListSpace{};
RwPluginRegistry imageTKList = rwPLUGINREGISTRYINIT(sizeof(RwImage));   // 0x8056F0C0

rwImageGlobals& globals() { return rwModuleGlobals<rwImageGlobals>(imageModule); }
const RwStringFunctions& strings() { return rwSrcGlobals().stringFuncs; }

void raise(RwInt32 code) {
    RwError error{rwID_COREPLUGIN, _rwerror(code)};
    RwErrorSet(&error);
}
void raiseNoMem(std::size_t size) {
    RwError error{rwID_COREPLUGIN, _rwerror(E_RW_NOMEM, size)};
    RwErrorSet(&error);
}

/// slw 1, n: the Gekko shifts by the low 6 bits, so 32..63 give 0.
RwUInt32 oneShifted(RwInt32 n) {
    const RwUInt32 shift = static_cast<RwUInt32>(n) & 0x3Fu;
    return shift >= 32u ? 0u : 1u << shift;
}

bool paletted(RwInt32 depth) { return depth == 4 || depth == 8; }

/// fctiwz: toward zero, saturating; NaN gives INT_MIN.
RwInt32 fctiwz(float v) {
    if (v != v) return std::numeric_limits<RwInt32>::min();
    if (v >= 2147483647.0f) return std::numeric_limits<RwInt32>::max();
    if (v <= -2147483648.0f) return std::numeric_limits<RwInt32>::min();
    return static_cast<RwInt32>(v);
}

/// _rwpathisabsolute (osintf, 0x80484494): a drive letter and ':', or a leading backslash. Native: an empty name is
/// not read past its NUL (the original reads name[1] first; the result is the same).
bool _rwpathisabsolute(const RwChar* name) {
    const RwChar c = name[0];
    if (c != 0 && name[1] == ':' && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return true;
    return c == '\\';
}

/// ImageGetScratchMem (inlined twice into ImagePathForAllFullNames): the scratch buffer, grown to size bytes (RwRealloc,
/// or RwMalloc when there is none). NULL with E_RW_NOMEM.
RwChar* ImageGetScratchMem(std::size_t size) {
    rwImageGlobals& g = globals();
    if (size > g.scratchMemSize) {
        void* grown = g.scratchMem != nullptr ? RwRealloc(g.scratchMem, size, kScratchHint) : RwMalloc(size, kScratchHint);
        if (grown == nullptr) {
            raiseNoMem(size);
            return nullptr;
        }
        g.scratchMem = static_cast<RwChar*>(grown);
        g.scratchMemSize = size;
    }
    return g.scratchMem;
}

using ImageFullNameCallBack = const RwChar* (*)(RwChar* fullName, void* data);

/// ImagePathForAllFullNames (0x8048971C): callBack for each full name of imageName with `room` bytes beyond the
/// name's characters - the name itself when it is absolute or there is no path, else each path directory followed by
/// the name - until one returns NULL. imageName, or NULL when the scratch buffer cannot grow.
const RwChar* ImagePathForAllFullNames(const RwChar* imageName, std::size_t room, ImageFullNameCallBack callBack,
                                       void* data) {
    const RwStringFunctions& s = strings();
    const RwChar* path = globals().imagePath;
    if (_rwpathisabsolute(imageName) || path == nullptr || path[0] == 0) {
        RwChar* full = ImageGetScratchMem(s.vecStrlen(imageName) + room);
        if (full == nullptr) return nullptr;
        s.vecStrcpy(full, imageName);
        callBack(full, data);
        return imageName;
    }
    while (path != nullptr && path[0] != 0) {
        // the original asks the string function for the separator twice and keeps the second answer
        const RwChar* separator = s.vecStrchr(path, ';');
        std::size_t length;
        const RwChar* next;
        if (separator != nullptr) {
            length = static_cast<std::size_t>(separator - path);
            next = separator + 1;
        } else {
            length = s.vecStrlen(path);
            next = nullptr;
        }
        RwChar* full = ImageGetScratchMem(length + (s.vecStrlen(imageName) + room));
        if (full == nullptr) return nullptr;
        std::memcpy(full, path, length);
        s.vecStrcpy(full + length, imageName);
        if (callBack(full, data) == nullptr) return imageName;
        path = next;
    }
    return imageName;
}

/// ImageDetermineExtender (0x80489AC0): each format's lower- then upper-case extension appended to the full name;
/// the first RwFexist finds is stored in *data and stops the walk (NULL).
const RwChar* ImageDetermineExtender(RwChar* fullName, void* data) {
    RwChar* end = fullName + strings().vecStrlen(fullName);
    for (rwImageFormat* format = globals().imageFormatList; format != nullptr; format = format->nextFormat) {
        strings().vecStrcpy(end, format->lcExtension);
        if (RwFexist(fullName)) {
            *static_cast<const RwChar**>(data) = format->lcExtension;
            return nullptr;
        }
        strings().vecStrcpy(end, format->ucExtension);
        if (RwFexist(fullName)) {
            *static_cast<const RwChar**>(data) = format->ucExtension;
            return nullptr;
        }
    }
    return fullName;
}

struct ImageReadAttempt {
    RwImageCallBackRead imageRead;
    RwImage* image;
};

/// ImageAttempRead (0x80489A54): a full name that exists is read; a read image stops the walk (NULL).
const RwChar* ImageAttempRead(RwChar* fullName, void* data) {
    auto* attempt = static_cast<ImageReadAttempt*>(data);
    if (RwFexist(fullName)) {
        attempt->image = attempt->imageRead(fullName);
        if (attempt->image != nullptr) return nullptr;
    }
    return fullName;
}

/// RwImageRead (inlined into RwImageReadMaskedImage): the format of the name's extension - after the last ':', '/'
/// or '\\', from the last '.', compared with both of each format's extensions - reads it along the path.
RwImage* ImageRead(const RwChar* imageName) {
    const RwStringFunctions& s = strings();
    const RwChar* name = s.vecStrrchr(imageName, ':');
    if (name == nullptr) name = imageName;
    if (const RwChar* slash = s.vecStrrchr(name, '/')) name = slash;
    const RwChar* backslash = s.vecStrrchr(name, '\\');
    if (backslash == nullptr) backslash = name;
    const RwChar* extension = s.vecStrrchr(backslash, '.');
    if (extension == nullptr) return nullptr;
    for (rwImageFormat* format = globals().imageFormatList; format != nullptr; format = format->nextFormat) {
        if (s.vecStrcmp(format->lcExtension, extension) == 0 || s.vecStrcmp(format->ucExtension, extension) == 0) {
            if (format->imageRead == nullptr) return nullptr;
            ImageReadAttempt attempt{format->imageRead, nullptr};
            ImagePathForAllFullNames(imageName, kReadNameRoom, ImageAttempRead, &attempt);
            return attempt.image;
        }
    }
    return nullptr;
}

/// ImageConvertDepth (0x8048A1AC), on (source depth << 8) | destination depth over the destination's size: equal
/// depths do nothing, 4 -> 8 copies the indices, 4 / 8 -> 32 looks them up in the source palette. FALSE with
/// E_RW_INVIMAGEFORMAT otherwise.
RwBool ImageConvertDepth(RwImage* dst, const RwImage* src) {
    const RwUInt32 key = (static_cast<RwUInt32>(src->depth) << 8) | static_cast<RwUInt32>(dst->depth);
    const RwInt32 width = dst->width, height = dst->height;
    RwUInt8* to = dst->cpPixels;
    const RwUInt8* from = src->cpPixels;
    switch (key) {
        case 0x808u:
        case 0x404u:
        case 0x2020u:
            return 1;
        case 0x408u:
            for (RwInt32 y = 0; y < height; ++y) {
                std::memcpy(to, from, static_cast<std::size_t>(width));
                from += src->stride;
                to += dst->stride;
            }
            return 1;
        case 0x420u:
        case 0x820u: {
            const RwRGBA* palette = src->palette;
            for (RwInt32 y = 0; y < height; ++y) {
                for (RwInt32 x = 0; x < width; ++x) std::memcpy(to + 4 * x, &palette[from[x]], 4);
                from += src->stride;
                to += dst->stride;
            }
            return 1;
        }
        default:
            raise(E_RW_INVIMAGEFORMAT);
            return 0;
    }
}

bool moduleOpen() {
    if (imageModule.numInstances > 0) return true;
    raise(E_RW_BADENGINESTATE);
    return false;
}

}  // namespace

RwPluginRegistry& rwImageRegistry() { return imageTKList; }

void* _rwImageOpen(void* instance, RwInt32 offset, RwInt32 /*size*/) {
    imageModule.globalsOffset = offset;
    rwImageGlobals& g = globals();
    g.imageFreeList = RwFreeListCreateAndPreallocateSpace(imageTKList.sizeOfStruct, kPerBlock, alignof(RwImage),
                                                          kImagePrealloc, &imageFreeListSpace,
                                                          rwMEMHINTDUR_GLOBAL | rwID_IMAGE);
    if (g.imageFreeList == nullptr) return nullptr;
    g.imageFormatFreeList = RwFreeListCreateAndPreallocateSpace(sizeof(rwImageFormat), kPerBlock, alignof(rwImageFormat),
                                                                kFormatPrealloc, &imageFormatFreeListSpace,
                                                                rwMEMHINTDUR_GLOBAL | rwID_IMAGEMODULE);
    if (g.imageFormatFreeList == nullptr) {
        RwFreeListDestroy(g.imageFreeList);
        g.imageFreeList = nullptr;
        return nullptr;
    }
    g.imagePathSize = kInitialPathSize;
    g.imagePath = static_cast<RwChar*>(RwMalloc(kInitialPathSize, kPathHint));
    if (g.imagePath == nullptr) {
        g.imagePathSize = 0;   // native: the original leaves the size it was going to allocate
        RwFreeListDestroy(g.imageFormatFreeList);
        g.imageFormatFreeList = nullptr;
        RwFreeListDestroy(g.imageFreeList);
        g.imageFreeList = nullptr;
        return nullptr;
    }
    g.imagePath[0] = 0;
    RwImageSetGamma(1.0f);
    g.imageFormatList = nullptr;
    g.scratchMemSize = kInitialScratchSize;
    g.scratchMem = static_cast<RwChar*>(RwMalloc(kInitialScratchSize, kScratchHint));
    if (g.scratchMem == nullptr) {
        g.scratchMemSize = 0;
        RwFree(g.imagePath);
        g.imagePath = nullptr;
        g.imagePathSize = 0;
        RwFreeListDestroy(g.imageFormatFreeList);
        g.imageFormatFreeList = nullptr;
        RwFreeListDestroy(g.imageFreeList);
        g.imageFreeList = nullptr;
        return nullptr;   // native: not counted as open (the original has counted it already)
    }
    ++imageModule.numInstances;
    return instance;
}

void* _rwImageClose(void* instance, RwInt32 /*offset*/, RwInt32 /*size*/) {
    rwImageGlobals& g = globals();
    if (g.scratchMem != nullptr) {
        RwFree(g.scratchMem);
        g.scratchMem = nullptr;
        g.scratchMemSize = 0;
    }
    if (g.imagePath != nullptr) {
        RwFree(g.imagePath);
        g.imagePath = nullptr;
        g.imagePathSize = 0;
    }
    while (rwImageFormat* format = g.imageFormatList) {
        g.imageFormatList = format->nextFormat;
        rwSrcGlobals().memoryFree(g.imageFormatFreeList, format);
    }
    if (g.imageFormatFreeList != nullptr) {
        RwFreeListDestroy(g.imageFormatFreeList);
        g.imageFormatFreeList = nullptr;
    }
    if (g.imageFreeList != nullptr) {
        RwFreeListDestroy(g.imageFreeList);
        g.imageFreeList = nullptr;
    }
    --imageModule.numInstances;
    return instance;
}

RwImage* RwImageCreate(RwInt32 width, RwInt32 height, RwInt32 depth) {
    if (!moduleOpen()) return nullptr;   // native: no image module open
    auto* image = static_cast<RwImage*>(rwSrcGlobals().memoryAlloc(globals().imageFreeList, kImageHint));
    if (image == nullptr) return nullptr;
    std::memset(image, 0, sizeof *image);   // native: the stride starts at 0
    image->width = width;
    image->height = height;
    image->depth = depth;
    image->cpPixels = nullptr;
    image->palette = nullptr;
    image->flags = 0;
    if (_rwPluginRegistryInitObject(&imageTKList, image) == nullptr) {
        // native: a failed plugin constructor fails the creation (the original returns the half-built image)
        rwSrcGlobals().memoryFree(globals().imageFreeList, image);
        return nullptr;
    }
    return image;
}

RwBool RwImageDestroy(RwImage* image) {
    if (image->flags & rwIMAGEALLOCATED) RwImageFreePixels(image);
    _rwPluginRegistryDeInitObject(&imageTKList, image);
    rwSrcGlobals().memoryFree(globals().imageFreeList, image);
    return 1;
}

RwImage* RwImageAllocatePixels(RwImage* image) {
    const bool withPalette = paletted(image->depth);
    const std::int64_t row = static_cast<std::int64_t>((image->depth + 7) >> 3) * image->width;
    if (image->width < 0 || image->height < 0 || row < 0 || row > std::numeric_limits<RwInt32>::max() - 3) {
        raiseNoMem(0);   // native: the original's 32-bit size arithmetic would wrap
        return nullptr;
    }
    image->stride = static_cast<RwInt32>((row + 3) & ~std::int64_t{3});
    const std::size_t pixelBytes = static_cast<std::size_t>(image->stride) * static_cast<std::size_t>(image->height);
    const std::size_t paletteBytes = withPalette ? std::size_t{4} << image->depth : 0;
    const std::size_t bytes = pixelBytes + paletteBytes;
    image->cpPixels = static_cast<RwUInt8*>(RwMalloc(bytes, kImageHint));
    if (image->cpPixels == nullptr) {
        raiseNoMem(bytes);
        return nullptr;
    }
    image->palette = withPalette ? reinterpret_cast<RwRGBA*>(image->cpPixels + pixelBytes) : nullptr;
    image->flags |= rwIMAGEALLOCATED;
    return image;
}

RwImage* RwImageFreePixels(RwImage* image) {
    RwFree(image->cpPixels);
    image->cpPixels = nullptr;
    image->palette = nullptr;
    image->flags &= ~rwIMAGEALLOCATED;
    return image;
}

RwImage* RwImageMakeMask(RwImage* image) {
    const auto largest = [](RwUInt8* c) {
        RwUInt8 m = c[0];
        if (c[1] > m) m = c[1];
        if (c[2] > m) m = c[2];
        c[3] = m;
    };
    switch (image->depth) {
        case 4:
        case 8: {
            auto* entry = reinterpret_cast<RwUInt8*>(image->palette);
            for (RwUInt32 n = oneShifted(image->depth); n > 0; --n, entry += 4) largest(entry);
            break;
        }
        case 32: {
            RwUInt8* row = image->cpPixels;
            for (RwInt32 y = 0; y < image->height; ++y, row += image->stride) {
                for (RwInt32 x = 0; x < image->width; ++x) largest(row + 4 * x);
            }
            break;
        }
        default:
            break;
    }
    return image;
}

RwImage* RwImageApplyMask(RwImage* image, const RwImage* mask) {
    if (image->width != mask->width || image->height != mask->height) {
        raise(E_RW_INVIMAGEMASK);
        return nullptr;
    }
    switch (image->depth) {
        case 4:
        case 8: {   // to depth 32 through a copy of the palettised pixels
            RwImage* copy = RwImageCreate(image->width, image->height, image->depth);
            if (copy == nullptr) return nullptr;
            if (RwImageAllocatePixels(copy) == nullptr) {
                RwImageDestroy(copy);
                return nullptr;
            }
            RwImageCopy(copy, image);
            if (image->flags & rwIMAGEALLOCATED) RwImageFreePixels(image);
            image->depth = 32;
            if (RwImageAllocatePixels(image) == nullptr) {   // native: the original goes on with no pixels
                RwImageDestroy(copy);
                return nullptr;
            }
            RwImageCopy(image, copy);
            RwImageFreePixels(copy);
            RwImageDestroy(copy);
            break;
        }
        case 32:
            break;
        default:
            raise(E_RW_INVIMAGEFORMAT);
            return nullptr;
    }
    const RwUInt8* from = mask->cpPixels;
    const auto* palette = reinterpret_cast<const RwUInt8*>(mask->palette);
    RwUInt8* to = image->cpPixels;
    for (RwInt32 y = 0; y < image->height; ++y) {
        if (paletted(mask->depth)) {
            for (RwInt32 x = 0; x < image->width; ++x) to[4 * x + 3] = palette[4 * from[x] + 3];
        } else if (mask->depth == 32) {
            for (RwInt32 x = 0; x < image->width; ++x) to[4 * x + 3] = from[4 * x + 3];
        }
        from += mask->stride;
        to += image->stride;
    }
    return image;
}

const RwChar* RwImageSetPath(const RwChar* path) {
    rwImageGlobals& g = globals();
    const std::size_t size = strings().vecStrlen(path) + 1;
    if (size > g.imagePathSize) {
        void* grown = RwRealloc(g.imagePath, size, kPathHint);
        if (grown == nullptr) {
            raiseNoMem(size);
            return nullptr;
        }
        g.imagePath = static_cast<RwChar*>(grown);
        g.imagePathSize = size;
    }
    std::memcpy(g.imagePath, path, size);
    return path;
}

const RwChar* RwImageFindFileType(const RwChar* imageName) {
    const RwChar* extension = nullptr;
    ImagePathForAllFullNames(imageName, kTypeNameRoom, ImageDetermineExtender, &extension);
    return extension;
}

RwImage* RwImageReadMaskedImage(const RwChar* imageName, const RwChar* maskName) {
    RwImage* image = ImageRead(imageName);
    if (image == nullptr) return nullptr;
    if (maskName != nullptr && maskName[0] != 0) {
        RwImage* mask = ImageRead(maskName);
        if (mask == nullptr) {
            RwImageDestroy(image);
            return nullptr;
        }
        if (RwImageMakeMask(mask) == nullptr || RwImageApplyMask(image, mask) == nullptr) {
            RwImageDestroy(image);
            RwImageDestroy(mask);
            return nullptr;
        }
        RwImageDestroy(mask);
    }
    return image;
}

RwRGBA* RwRGBASetFromPixel(RwRGBA* rgbOut, RwUInt32 pixelValue, RwInt32 rasterFormat) {
    rwSrcGlobals().stdFunc[rwSTANDARDPIXELTORGB](rgbOut, &pixelValue, rasterFormat);
    return rgbOut;
}

RwImage* RwImageCopy(RwImage* destImage, const RwImage* sourceImage) {
    if (destImage->depth == sourceImage->depth) {   // ImageStraightCopy, inlined
        if (destImage->palette != nullptr && sourceImage->palette != nullptr && sourceImage->depth <= 8) {
            std::memcpy(destImage->palette, sourceImage->palette, std::size_t{4} * oneShifted(sourceImage->depth));
        }
        const std::size_t row = static_cast<std::size_t>(static_cast<std::int64_t>((destImage->depth + 7) >> 3) *
                                                         destImage->width);
        RwUInt8* to = destImage->cpPixels;
        const RwUInt8* from = sourceImage->cpPixels;
        for (RwInt32 y = 0; y < destImage->height; ++y) {
            std::memcpy(to, from, row);
            from += sourceImage->stride;
            to += destImage->stride;
        }
    } else if (!ImageConvertDepth(destImage, sourceImage)) {
        return nullptr;   // native: the original then writes the flag below through NULL
    }
    destImage->flags = (destImage->flags & ~rwIMAGEGAMMACORRECTED) | (sourceImage->flags & rwIMAGEGAMMACORRECTED);
    return destImage;
}

RwImage* RwImageGammaCorrect(RwImage* image) {
    const RwUInt8* table = globals().gammaTable;
    switch (image->depth) {
        case 4:
        case 8: {
            if (image->palette == nullptr) {
                raise(E_RW_NULLP);
                return nullptr;
            }
            auto* entry = reinterpret_cast<RwUInt8*>(image->palette);
            for (RwUInt32 n = oneShifted(image->depth); n > 0; --n, entry += 4) {
                entry[0] = table[entry[0]];
                entry[1] = table[entry[1]];
                entry[2] = table[entry[2]];
            }
            break;
        }
        case 32: {
            if (image->cpPixels == nullptr) {
                raise(E_RW_NULLP);
                return nullptr;
            }
            RwUInt8* row = image->cpPixels;
            for (RwInt32 y = 0; y < image->height; ++y, row += image->stride) {
                for (RwInt32 x = 0; x < image->width; ++x) {
                    RwUInt8* p = row + 4 * x;
                    p[0] = table[p[0]];
                    p[1] = table[p[1]];
                    p[2] = table[p[2]];
                }
            }
            break;
        }
        default:
            raise(E_RW_INVIMAGEDEPTH);
            return nullptr;
    }
    image->flags |= rwIMAGEGAMMACORRECTED;
    return image;
}

RwBool RwImageSetGamma(RwReal gammaValue) {
    // The original's single-precision steps (fdivs, fmuls, fadds; the pow result rounded by frsp), unfused, and
    // MSL's double pow. The game runs with FPSCR[NI] set; the tables are the same bytes in both modes: a result
    // the mode would flush is below 2^-126, where 0.5 + 255 x stays below 1 either way.
    namespace msl = shadow::math::msl;
    constexpr shadow::math::SubnormalMode kMode = shadow::math::SubnormalMode::Preserve;
    rwImageGlobals& g = globals();
    const float inverse = 1.0f / gammaValue;
    g.gammaVal = gammaValue;
    g.gammaTable[0] = 0;
    g.invGammaTable[0] = 0;
    for (RwInt32 i = 1; i < 256; ++i) {
        const float x = static_cast<float>(i) / 255.0f;
        const float corrected = static_cast<float>(msl::pow(x, inverse, kMode));
        const float product = 255.0f * corrected;
        g.gammaTable[i] = static_cast<RwUInt8>(fctiwz(0.5f + product));
        const float uncorrected = static_cast<float>(msl::pow(x, gammaValue, kMode));
        const float product2 = 255.0f * uncorrected;
        g.invGammaTable[i] = static_cast<RwUInt8>(fctiwz(0.5f + product2));
    }
    return 1;
}

RwImage* RwImageSetFromRaster(RwImage* image, RwRaster* raster) {
    if (!rwSrcGlobals().stdFunc[rwSTANDARDIMAGEGETRASTER](image, raster, 0)) return nullptr;
    if (raster->privateFlags & 0x1u) image->flags |= rwIMAGEGAMMACORRECTED;
    return image;
}

RwRaster* RwRasterSetFromImage(RwRaster* raster, RwImage* image) {
    if (!rwSrcGlobals().stdFunc[rwSTANDARDRASTERSETIMAGE](raster, image, 0)) return nullptr;
    if (image->flags & rwIMAGEGAMMACORRECTED) raster->privateFlags |= 0x1u;
    return raster;
}

RwImage* RwImageFindRasterFormat(RwImage* ipImage, RwInt32 nRasterType, RwInt32* npWidth, RwInt32* npHeight,
                                 RwInt32* npDepth, RwInt32* npFormat) {
    RwRaster raster{};   // native: zeroed (the original's stack raster holds whatever the stack held)
    if (!rwSrcGlobals().stdFunc[rwSTANDARDIMAGEFINDRASTERFORMAT](&raster, ipImage, nRasterType)) return nullptr;
    *npFormat = static_cast<RwInt32>(raster.cType) | (static_cast<RwInt32>(raster.cFormat) << 8);
    *npWidth = raster.width;
    *npHeight = raster.height;
    *npDepth = raster.depth;
    return ipImage;
}
