#pragma once

// GX texture image decoders for the native renderer's TextureCache
// (docs/native-runtime-2026-09-16/ARCHITECTURE.md section 3).
//
// Pure functions over guest bytes: no D3D, no guest memory access, no global
// state, no allocation. The TextureCache copies a raster's image and palette
// out of MEM1 at capture time and decodes them here on a worker, once per
// content generation.
//
// Exactness reference: Renderer::decode_texel / decode_tlut_entry in
// runtime/src/gekko/renderer.cpp (the legacy decoder) for every format EXCEPT
// CMPR; the tests transcribe it literally and compare, synthetically and on
// real rasters from an in-stage MEM1 image. CMPR follows the GameCube palette
// rule (5/8-3/8 interpolants, averaged colour with alpha 0 in the c0 <= c1
// mode) as Dolphin decodes it; the legacy decoder implements PC DXT1 there and
// is not an oracle for CMPR.

#include "gekko/types.h"

#include <cstddef>

namespace gekko {

// GXTexFmt, as stored at raster+0x40 by the RwGC driver and in the native TXD
// header (RENDERER_RE/01 section 4.1 / 4.7).
constexpr u32 kGxTexFmtI4 = 0;
constexpr u32 kGxTexFmtI8 = 1;
constexpr u32 kGxTexFmtIA4 = 2;
constexpr u32 kGxTexFmtIA8 = 3;
constexpr u32 kGxTexFmtRGB565 = 4;
constexpr u32 kGxTexFmtRGB5A3 = 5;
constexpr u32 kGxTexFmtRGBA8 = 6;
constexpr u32 kGxTexFmtC4 = 8;
constexpr u32 kGxTexFmtC8 = 9;
constexpr u32 kGxTexFmtC14X2 = 10;
constexpr u32 kGxTexFmtCMPR = 14;

// GXTlutFmt, raster+0x44.
constexpr u32 kGxTlutFmtIA8 = 0;
constexpr u32 kGxTlutFmtRGB565 = 1;
constexpr u32 kGxTlutFmtRGB5A3 = 2;

// TX_SETIMAGE0 carries width-1 and height-1 in 10 bits each, so no texture the
// Flipper can sample is larger. Also keeps every size computation far from
// overflow on any host.
constexpr u32 kGxMaxTextureDimension = 1024;

// True for C4, C8 and C14X2: texels are TLUT indices.
bool gx_texture_format_is_palettised(u32 gx_format) noexcept;

// Bytes of ONE mip level in the GX tile layout, or 0 for an unknown format or a
// dimension outside 1..kGxMaxTextureDimension. Every tile is 32 bytes except
// RGBA8 (64); the tile is 8x8 for the 4-bit formats I4/C4 and for CMPR (four
// 4x4 DXT1 sub-blocks), 8 wide x 4 tall for the 8-bit formats I8/IA4/C8, and
// 4x4 for IA8/RGB565/RGB5A3/C14X2/RGBA8. This is the per-level size table of
// the guest's own allocator (0x80496478: depth 4 A8(w)*A8(h)/2, depth 8
// A8(w)*A4(h), depth 16 A4(w)*A4(h)*2, depth 32 A4(w)*A4(h)*4) and the legacy
// renderer's tiled_byte_size.
std::size_t gx_texture_data_size(u32 gx_format, u32 width, u32 height) noexcept;

// Decodes one mip level into `rgba8_out`: width*height*4 bytes, row-major,
// top row first, bytes R,G,B,A per texel. The caller owns that buffer.
//
// `data` is the level's image bytes exactly as the guest stores them (big
// endian, GX tile layout) and `data_size` how many of them are readable; it
// must be at least gx_texture_data_size(). `tlut_be` holds `tlut_entries`
// big-endian u16 palette entries in `tlut_format`; both are read only for the
// palettised formats and ignored otherwise.
//
// Returns false, WITHOUT writing a single output byte, when: the format is not
// one of the eleven above; width or height is 0 or above
// kGxMaxTextureDimension; `data` or `rgba8_out` is null; `data_size` is short;
// or, for a palettised format, `tlut_be` is null, `tlut_entries` is 0 or
// `tlut_format` is not 0..2. Nothing is ever read outside [data, data +
// gx_texture_data_size) or [tlut_be, tlut_be + 2*tlut_entries).
//
// Channel rules (legacy decoder, bit-for-bit):
//   4->8 v*0x11; 3->8 (v<<5)|(v<<2)|(v>>1); 5->8 (v<<3)|(v>>2);
//   6->8 (v<<2)|(v>>4).
//   I4/I8: R=G=B=A=intensity. IA4: high nibble alpha, low nibble intensity.
//   IA8 (texture and TLUT): first byte alpha, second byte intensity.
//   RGB5A3: bit 15 set RGB555 opaque, clear A3 RGB444.
//   RGBA8: each 4x4 tile is 16 (A,R) pairs then 16 (G,B) pairs - the layout the
//   RwGC unlock fix-up 0x8049677C writes and Dolphin decodes.
//   CMPR: sub-blocks TL,TR,BL,BR per 8x8 tile, colours c0/c1 big-endian RGB565,
//   selector bits MSB first in row-major order. c0 > c1: c2 = (2*c0+c1)/3,
//   c3 = (c0+2*c1)/3 per channel, integer truncation. c0 <= c1: c2 = (c0+c1)/2,
//   c3 = transparent black (0,0,0,0). Dolphin instead blends 3/8-5/8
//   ((3*a+5*b)>>3) and keeps the averaged colour with alpha 0 for c3.
//
// Palette index beyond `tlut_entries` (C14X2 indices reach 16383, and a C4/C8
// caller may pass a short palette): the texel decodes the entry value 0x0000
// through `tlut_format` - IA8 (0,0,0,0), RGB565 (0,0,0,255), RGB5A3 (0,0,0,0).
// That is what the legacy decode_tlut_entry yields for an entry it cannot
// read (beyond the end of its texture memory); the legacy decoder has no
// palette length of its own and otherwise reads whatever texture memory
// follows the palette, which a copied palette cannot reproduce.
bool gx_decode_texture(u32 gx_format, u32 width, u32 height, const u8* data,
                       std::size_t data_size, const u8* tlut_be, u32 tlut_format,
                       u32 tlut_entries, u8* rgba8_out) noexcept;

}  // namespace gekko
