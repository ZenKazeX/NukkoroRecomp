// RenderWare image resampling (module baresamp; see rw/core/image.h). Every arithmetic step is the original's, in its
// order: int -> float conversions single-rounded, fmuls / fadds / fdivs as single operations, the byte conversion's
// multiply-add fused (fmadds: std::fmaf), float -> int as fctiwz (toward zero; NaN and too small give INT_MIN, too
// large INT_MAX), bytes the low 8 bits.
#include "rw/core/image.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace {

constexpr RwReal kFixedToReal = 1.52587890625e-05f;   // 2^-16 (0x805FC3A0)
constexpr RwReal kByteToReal = 0.003921568859368563f;  // 1 / 255 rounded (0x805FC3A4: 0x3B808081)
constexpr RwReal kOne = 1.0f, kFixedOne = 65536.0f, kHalf = 0.5f, k255 = 255.0f;
constexpr RwInt32 kMaxSource = 32766;   // larger sizes overflow the 16.16 coordinates (pixel 32767 + 1 is 2^31)

RwInt32 fctiwz(RwReal v) {
    if (std::isnan(v) || v <= -2147483648.0f) return std::numeric_limits<RwInt32>::min();
    if (v >= 2147483648.0f) return std::numeric_limits<RwInt32>::max();
    return static_cast<RwInt32>(v);
}
RwUInt8 toByte(RwReal c) { return static_cast<RwUInt8>(static_cast<RwUInt32>(fctiwz(std::fmaf(k255, c, kHalf)))); }

/// The pixel at (ix, iy): the original's address (row iy x stride + ix x 4) while it lies in the pixel buffer; past it
/// (native) the last row, then the last column.
const RwUInt8* pixelAt(const RwImage* image, RwInt32 ix, RwInt32 iy) {
    const std::size_t size = static_cast<std::size_t>(image->stride) * static_cast<std::size_t>(image->height);
    const auto at = [&](RwInt32 x, RwInt32 y) {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(image->stride) + static_cast<std::size_t>(x) * 4u;
    };
    std::size_t offset = at(ix, iy);
    if (offset + 4u > size) {
        if (iy >= image->height) iy = image->height - 1;
        offset = at(ix, iy);
        if (offset + 4u > size) offset = at(image->width - 1, iy);
    }
    return image->cpPixels + offset;
}

void fromPixel(RwRGBAReal* out, const RwUInt8* p) {
    out->red = static_cast<RwReal>(p[0]) * kByteToReal;
    out->green = static_cast<RwReal>(p[1]) * kByteToReal;
    out->blue = static_cast<RwReal>(p[2]) * kByteToReal;
    out->alpha = static_cast<RwReal>(p[3]) * kByteToReal;
}
void scale(RwRGBAReal* c, RwReal s) {
    c->red = c->red * s;
    c->green = c->green * s;
    c->blue = c->blue * s;
    c->alpha = c->alpha * s;
}

/// ImageResampleGetSpan (0x8048B1C8): the average of row y >> 16 from x0 to x1 (16.16, inclusive): the first and last
/// pixels weighted by their covered fraction, those between by 1, the sum divided by (x1 - x0) / 65536.
void ImageResampleGetSpan(const RwImage* image, RwInt32 x0, RwInt32 x1, RwInt32 y, RwRGBAReal* out) {
    const RwReal width = static_cast<RwReal>(x1 - x0) * kFixedToReal;
    const RwInt32 iy = y >> 16, first = x0 >> 16, last = x1 >> 16;
    fromPixel(out, pixelAt(image, first, iy));
    if (first == last) {
        scale(out, width);
    } else {
        RwInt32 next = (first + 1) << 16;
        scale(out, static_cast<RwReal>(next - x0) * kFixedToReal);
        for (RwInt32 ix = first + 1; ix != last; ++ix, next += 0x10000) {
            const RwUInt8* p = pixelAt(image, ix, iy);
            out->red = out->red + static_cast<RwReal>(p[0]) * kByteToReal;
            out->green = out->green + static_cast<RwReal>(p[1]) * kByteToReal;
            out->blue = out->blue + static_cast<RwReal>(p[2]) * kByteToReal;
            out->alpha = out->alpha + static_cast<RwReal>(p[3]) * kByteToReal;
        }
        const RwReal weight = static_cast<RwReal>(x1 - next) * kFixedToReal;
        const RwUInt8* p = pixelAt(image, last, iy);
        out->red = out->red + static_cast<RwReal>(p[0]) * kByteToReal * weight;
        out->green = out->green + static_cast<RwReal>(p[1]) * kByteToReal * weight;
        out->blue = out->blue + static_cast<RwReal>(p[2]) * kByteToReal * weight;
        out->alpha = out->alpha + static_cast<RwReal>(p[3]) * kByteToReal * weight;
    }
    scale(out, kOne / width);
}

/// ImageResampleGetAvgPixel (0x8048B55C; written inline in the PC build's RwImageResample, named by its debug build):
/// the average over rows y0 .. y1 (16.16, inclusive) of the spans x0 .. x1, the first and last rows weighted by their
/// covered fraction, the sum divided by (y1 - y0) / 65536.
void ImageResampleGetAvgPixel(const RwImage* image, RwInt32 x0, RwInt32 x1, RwInt32 y0, RwInt32 y1, RwRGBAReal* out) {
    const RwReal height = static_cast<RwReal>(y1 - y0) * kFixedToReal;
    if ((y0 >> 16) == (y1 >> 16)) {
        ImageResampleGetSpan(image, x0, x1, y0, out);
        scale(out, height);
    } else {
        RwInt32 next = ((y0 >> 16) + 1) << 16;
        ImageResampleGetSpan(image, x0, x1, y0, out);
        scale(out, static_cast<RwReal>(next - y0) * kFixedToReal);
        RwRGBAReal row;
        for (RwInt32 iy = next >> 16; iy != (y1 >> 16); ++iy, next += 0x10000) {
            ImageResampleGetSpan(image, x0, x1, next, &row);
            out->red = row.red + out->red;
            out->green = row.green + out->green;
            out->blue = row.blue + out->blue;
            out->alpha = row.alpha + out->alpha;
        }
        ImageResampleGetSpan(image, x0, x1, next, &row);
        scale(&row, static_cast<RwReal>(y1 - next) * kFixedToReal);
        out->red = out->red + row.red;
        out->green = out->green + row.green;
        out->blue = out->blue + row.blue;
        out->alpha = out->alpha + row.alpha;
    }
    scale(out, kOne / height);
}

}  // namespace

RwImage* RwImageResample(RwImage* dstImage, const RwImage* srcImage) {
    const RwInt32 width = dstImage->width, height = dstImage->height;
    const RwInt32 xStep = fctiwz(kFixedOne * (static_cast<RwReal>(srcImage->width) / static_cast<RwReal>(width)));
    const RwInt32 yStep = fctiwz(kFixedOne * (static_cast<RwReal>(srcImage->height) / static_cast<RwReal>(height)));
    if (width > 0 && height > 0) {   // native: what the original cannot resample (see the header)
        if (dstImage->depth != 32 || srcImage->depth != 32 || dstImage->cpPixels == nullptr ||
            srcImage->cpPixels == nullptr || xStep < 1 || yStep < 1 || srcImage->width > kMaxSource ||
            srcImage->height > kMaxSource) {
            return nullptr;
        }
    }
    dstImage->flags |= srcImage->flags & rwIMAGEGAMMACORRECTED;
    RwInt32 y = 0;
    for (RwInt32 row = 0; row < height; ++row, y += yStep) {
        RwUInt8* p = dstImage->cpPixels + static_cast<std::ptrdiff_t>(dstImage->stride) * row;
        const RwInt32 y1 = y + (yStep - 1);
        RwInt32 x = 0;
        for (RwInt32 column = 0; column < width; ++column, x += xStep, p += 4) {
            RwRGBAReal c;
            ImageResampleGetAvgPixel(srcImage, x, x + (xStep - 1), y, y1, &c);
            p[0] = toByte(c.red);
            p[1] = toByte(c.green);
            p[2] = toByte(c.blue);
            p[3] = toByte(c.alpha);
        }
    }
    return dstImage;
}

RwImage* RwImageCreateResample(const RwImage* srcImage, RwInt32 width, RwInt32 height) {
    if (width < 1 || height < 1) return nullptr;   // native: RenderWare's debug build asserts both > 0
    RwImage* const image = RwImageCreate(width, height, 32);
    if (image == nullptr) return nullptr;
    if (RwImageAllocatePixels(image) == nullptr) {
        RwImageDestroy(image);
        return nullptr;
    }
    if (srcImage->depth == 32) {
        if (RwImageResample(image, srcImage) != nullptr) return image;
        RwImageFreePixels(image);
        RwImageDestroy(image);
        return nullptr;
    }
    RwImage* const copy = RwImageCreate(srcImage->width, srcImage->height, 32);
    if (copy == nullptr) {
        RwImageFreePixels(image);
        RwImageDestroy(image);
        return nullptr;
    }
    if (RwImageAllocatePixels(copy) == nullptr) {
        RwImageDestroy(copy);
        RwImageFreePixels(image);
        RwImageDestroy(image);
        return nullptr;
    }
    // native: a copy that fails (a depth RwImageCopy does not convert) fails the whole (the original resamples the
    // copy's uninitialised pixels)
    const bool ok = RwImageCopy(copy, srcImage) != nullptr && RwImageResample(image, copy) != nullptr;
    RwImageFreePixels(copy);
    RwImageDestroy(copy);
    if (ok) return image;
    RwImageFreePixels(image);
    RwImageDestroy(image);
    return nullptr;
}
