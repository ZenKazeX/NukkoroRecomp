// RenderWare Graphics 3.7 core: palette quantisation (module palquant), native x64.
//
// Makes a palette of at most 256 colours for images and the palette index of each pixel. An octree over RGBA, 4 bits
// a level (the next bit of red, green, blue and alpha, the most significant first), QuantDepth = 6 levels: a leaf is a
// cell of 4 x 4 x 4 x 4 byte values and holds the weight of the pixels added in it, their weighted colour sum and
// weighted squared sum. Colours are in "match space": the byte / 255, red x 0.5093697 and blue x 0.19420783 (green
// and alpha x 1). RwPalQuantAddImage counts each pixel in its leaf (relative to the cell's corner);
// RwPalQuantResolvePalette sums the tree (the corners added, each node the statistics of the leaves under it) and
// then makes the palette: one entry a leaf when they fit, else boxes of the 64 x 64 x 64 x 64 cube, split along the
// axis and at the place that most reduce their variance (the box of largest variance first), until there are maxcols;
// each entry is its leaf's or box's mean colour and every leaf takes its index. RwPalQuantMatchImage writes each
// pixel's index. Arithmetic is the original's, in its order (rwcore/src/palquant.cpp).
//
// Written from PAL main.dol 0x8048E8AC..0x8049148C, the same code as the RenderWare 3.7 PC libraries' (rwcore.lib
// palquant.obj; its static functions named by the debug library). The game links no RwPalQuantSetMaxDepth /
// GetMaxDepth (not provided): QuantDepth stays 6, its initial value. Native structures (64-bit pointers); a node is
// 160 bytes (GameCube 0x5C). Proof: runtime/tests/rwcore_palquant_tests.cpp (oracle) and
// rwcore_palquant_native_tests.cpp; rwcore/README.md.
#pragma once

#include "rw/core/image.h"
#include "rw/core/memory.h"
#include "rw/core/types.h"

inline constexpr RwInt32 RWPALQUANT_MAXDEPTH = 8;                          ///< rwcore.h RWPALQUANT_MAXDEPTH
inline constexpr RwInt32 RWPALQUANT_MAXCOLOR = 1 << RWPALQUANT_MAXDEPTH;   ///< rwcore.h RWPALQUANT_MAXCOLOR (256)

struct _rwPalQuantOctNode;

/// A box of the cube (rwcore.h _rwPalQuantRGBABox): cells col0 (inclusive) to col1 (exclusive) per channel.
struct _rwPalQuantRGBABox {
    RwInt32 col0[4];
    RwInt32 col1[4];
};

/// The statistics of a leaf, a node or a box (rwcore.h _rwPalQuantLeafNode).
struct _rwPalQuantLeafNode {
    RwReal weight;     ///< accumulated weight
    RwRGBAReal ac;     ///< accumulated weight x colour (match space)
    RwReal var;        ///< weight x variance
    RwUInt8 palIndex;  ///< the palette index (leaves)
};

/// The children of a node, indexed by (red bit << 3) | (green << 2) | (blue << 1) | alpha (rwcore.h
/// _rwPalQuantBranchNode).
struct _rwPalQuantBranchNode {
    _rwPalQuantOctNode* dir[16];
};

/// A node of the octree (rwcore.h _rwPalQuantOctNode).
struct _rwPalQuantOctNode {
    _rwPalQuantLeafNode Leaf;
    _rwPalQuantBranchNode Branch;
};

/// The quantiser's working data (rwcore.h RwPalQuant): the median cut's boxes, their variances and statistics, the
/// tree and the free list its nodes come from.
struct RwPalQuant {
    _rwPalQuantRGBABox Mcube[RWPALQUANT_MAXCOLOR];
    RwReal Mvv[RWPALQUANT_MAXCOLOR];
    _rwPalQuantLeafNode Mvol[RWPALQUANT_MAXCOLOR];
    _rwPalQuantOctNode* root;
    RwFreeList* cubefreelist;
};

/// Starts a quantiser: the node free list (RwFreeListCreate(sizeof node, 1024, ..., rwMEMHINTDUR_EVENT |
/// rwID_DRIVERMODULE)) and an empty root. Returns TRUE. Native: FALSE when the free list or the root cannot be allocated
/// (what was made released; the original returns TRUE and writes through NULL). Original: 0x80490F28.
RwBool RwPalQuantInit(RwPalQuant* pq);

/// Frees the tree and the free list; root NULL. Original: 0x80491448.
void RwPalQuantTerm(RwPalQuant* pq);

/// Adds every pixel of img (depth 4 or 8: its palette entry, one index a byte; 32: RwRGBA) with weight to the tree,
/// allocating the leaves its colours need; other depths add nothing. Native: sizes below 1 add nothing (the original
/// loops on a wrapped count for negative ones), and a pixel whose node cannot be allocated is left out (the original
/// writes through NULL). Original: 0x8048EF00 (LeafAddPixel inlined).
void RwPalQuantAddImage(RwPalQuant* pq, RwImage* img, RwReal weight);

/// Makes the palette of at most maxcols colours from what was added: palette[0 .. maxcols - 1] cleared, then one
/// entry a leaf (tree order) when the leaves are no more than maxcols - their number returned - else maxcols boxes
/// (median cut) - maxcols returned. Every leaf takes the index of its entry. Native: maxcols over 256 is taken as 256
/// (the original overruns its 256 boxes; indices are bytes); when no box can be split further before there are
/// maxcols (each left without variance, or a cut that the float sums cannot show to reduce it), the boxes not made
/// keep their cleared entries and take no leaf - what the original gives on a zeroed RwPalQuant (it reads the boxes
/// it did not set this time: an earlier resolve's, or whatever the memory held). Original: 0x80490620.
RwInt32 RwPalQuantResolvePalette(RwRGBA* palette, RwInt32 maxcols, RwPalQuant* pq);

/// Writes the palette index of every pixel of img (depths 4, 8, 32; others write nothing) to dstpixels, rows dststride
/// bytes apart: for dstdepth 4 with dstPacked exactly TRUE two pixels a byte (the first in the low 4 bits, the other
/// bits kept), else one a byte. Native: sizes below 1 write nothing, and a pixel whose colour was never added matches
/// index 0 (the original reads through NULL). Original: 0x80490B00.
void RwPalQuantMatchImage(RwUInt8* dstpixels, RwInt32 dststride, RwInt32 dstdepth, RwBool dstPacked, RwPalQuant* pq,
                          RwImage* img);
