// RenderWare palette quantisation (module palquant; see rw/core/palquant.h). Every arithmetic step is the original's,
// in its order: single-precision operations, its multiply-adds fused (fmadds: std::fmaf), float -> int as fctiwz
// (toward zero; NaN and too small give INT_MIN, too large INT_MAX), comparisons false for NaN as its branches are.
// The GameCube build inlines or unrolls some of the functions below (named by RenderWare's debug library); they are
// written once here, each where the PC build has it.
#include "rw/core/palquant.h"

#include <array>
#include <cmath>
#include <limits>

namespace {

constexpr RwInt32 QuantDepth = 6;   // QuantDepth (0x805EEFD0): RwPalQuantSetMaxDepth is not linked
constexpr RwInt32 kCells = 1 << QuantDepth;
constexpr RwUInt32 kNodeHint = 0x00030000u | 0x411u;   // rwMEMHINTDUR_EVENT | rwID_DRIVERMODULE, as the original passes it
constexpr RwInt32 kNodesPerBlock = 0x400;

constexpr RwReal kByteToReal = 0.003921568859368563f;   // 1 / 255 rounded (0x805FC3E4: 0x3B808081)
constexpr RwReal kRedWeight = 0.5093696713447571f;      // 0x805FC3D8: 0x3F02660D
constexpr RwReal kBlueWeight = 0.1942078322172165f;     // 0x805FC3DC: 0x3E46DE6B
constexpr RwReal kToByte = 255.99000549316406f;         // 0x805FC3F4: 0x437FFD71
constexpr RwReal kZero = 0.0f, kOne = 1.0f;

/// splice (0x805E3E18, filled by the original's RwPalQuantInit): bit k of a QuantDepth-bit channel value moved to bit
/// 4 x (QuantDepth - 1 - k), so the most significant bit lands in the first level's nibble.
constexpr std::array<RwUInt32, kCells> splice = [] {
    std::array<RwUInt32, kCells> t{};
    for (RwInt32 i = 0; i < kCells; ++i) {
        for (RwInt32 k = 0; k < QuantDepth; ++k) {
            if (i & (1 << k)) t[i] |= 1u << (4 * (QuantDepth - 1 - k));
        }
    }
    return t;
}();

RwInt32 fctiwz(RwReal v) {
    if (std::isnan(v) || v <= -2147483648.0f) return std::numeric_limits<RwInt32>::min();
    if (v >= 2147483648.0f) return std::numeric_limits<RwInt32>::max();
    return static_cast<RwInt32>(v);
}
/// A channel of a representative colour: 255.99 x c toward zero, 255 from 255 up, else its low 8 bits.
RwUInt8 toByte(RwReal c) {
    const RwInt32 v = fctiwz(kToByte * c);
    return v < 255 ? static_cast<RwUInt8>(static_cast<RwUInt32>(v)) : RwUInt8{255};
}

/// GetOctAdr: the tree address of a colour, QuantDepth nibbles from the first level.
RwUInt32 GetOctAdr(const RwUInt8* c) {
    constexpr RwInt32 shift = 8 - QuantDepth;
    return (splice[c[0] >> shift] << 3) | (splice[c[1] >> shift] << 2) | (splice[c[2] >> shift] << 1) |
           splice[c[3] >> shift];
}

/// FromMatchSpace (0x8048E8AC): red and blue back from match space.
void FromMatchSpace(RwRGBAReal* c) {
    c->red = c->red / kRedWeight;
    c->blue = c->blue / kBlueWeight;
}

/// InitLeaf (0x8048E8D0).
void InitLeaf(_rwPalQuantLeafNode* leaf) {
    leaf->palIndex = 0;
    leaf->weight = kZero;
    leaf->ac = {kZero, kZero, kZero, kZero};
    leaf->var = kZero;
}

/// InitBranch (0x8048EBF4).
void InitBranch(_rwPalQuantBranchNode* branch) {
    for (_rwPalQuantOctNode*& d : branch->dir) d = nullptr;
}

/// LeafCalcStats (0x8048E8F8; ToMatchSpace inlined): the leaf's sums made absolute - var less the squared sum over
/// the weight (at least 0), then the cell's corner (bytes, in match space) times the weight added to ac.
void LeafCalcStats(_rwPalQuantLeafNode* leaf, const RwUInt8* corner) {
    const RwReal sq = static_cast<RwReal>(leaf->ac.alpha * leaf->ac.alpha) +
                      (static_cast<RwReal>(leaf->ac.blue * leaf->ac.blue) +
                       static_cast<RwReal>(leaf->ac.red * leaf->ac.red + leaf->ac.green * leaf->ac.green));
    leaf->var = leaf->var - sq / leaf->weight;
    if (leaf->var < kZero) leaf->var = kZero;
    const RwReal w = leaf->weight;
    leaf->ac.red = leaf->ac.red + kByteToReal * static_cast<RwReal>(corner[0]) * kRedWeight * w;
    leaf->ac.green = leaf->ac.green + kByteToReal * static_cast<RwReal>(corner[1]) * w;
    leaf->ac.blue = leaf->ac.blue + kByteToReal * static_cast<RwReal>(corner[2]) * kBlueWeight * w;
    leaf->ac.alpha = leaf->ac.alpha + kByteToReal * static_cast<RwReal>(corner[3]) * w;
}

/// The between-group term of combining a (weight wa, inverse ia) with b: |a mean - b mean|^2 / (ia + ib).
RwReal between(const RwRGBAReal& a, RwReal ia, const RwRGBAReal& b, RwReal ib) {
    const RwReal dg = a.green * ia - b.green * ib;
    const RwReal dr = a.red * ia - b.red * ib;
    const RwReal db = a.blue * ia - b.blue * ib;
    const RwReal da = a.alpha * ia - b.alpha * ib;
    const RwReal s = std::fmaf(da, da, std::fmaf(db, db, std::fmaf(dr, dr, dg * dg)));
    return s / (ia + ib);
}

/// StatsAdd (0x8048EA18): dst = a combined with b (dst may be a).
void StatsAdd(_rwPalQuantLeafNode* dst, const _rwPalQuantLeafNode* a, const _rwPalQuantLeafNode* b) {
    dst->var = a->var + b->var;
    if (a->weight > kZero && b->weight > kZero) {
        dst->var = dst->var + between(a->ac, kOne / a->weight, b->ac, kOne / b->weight);
    }
    dst->ac.red = a->ac.red + b->ac.red;
    dst->ac.green = a->ac.green + b->ac.green;
    dst->ac.blue = a->ac.blue + b->ac.blue;
    dst->ac.alpha = a->ac.alpha + b->ac.alpha;
    dst->weight = a->weight + b->weight;
}

/// StatsSub (inlined in the original's FindBestCut): the statistics of whole without part.
_rwPalQuantLeafNode StatsSub(const _rwPalQuantLeafNode* whole, const _rwPalQuantLeafNode* part) {
    _rwPalQuantLeafNode rest{};
    rest.weight = whole->weight - part->weight;
    rest.ac.red = whole->ac.red - part->ac.red;
    rest.ac.green = whole->ac.green - part->ac.green;
    rest.ac.blue = whole->ac.blue - part->ac.blue;
    rest.ac.alpha = whole->ac.alpha - part->ac.alpha;
    rest.var = whole->var - part->var;
    if (rest.weight > kZero && part->weight > kZero) {
        rest.var = rest.var - between(part->ac, kOne / part->weight, rest.ac, kOne / rest.weight);
    }
    return rest;
}

/// RepresentativeColor (0x8048EB18; FromMatchSpace inlined): the mean colour of leaf as bytes.
void RepresentativeColor(RwRGBA* out, const _rwPalQuantLeafNode* leaf) {
    const RwReal inv = kOne / leaf->weight;
    RwRGBAReal c{leaf->ac.red * inv, leaf->ac.green * inv, leaf->ac.blue * inv, leaf->ac.alpha * inv};
    FromMatchSpace(&c);
    out->red = toByte(c.red);
    out->green = toByte(c.green);
    out->blue = toByte(c.blue);
    out->alpha = toByte(c.alpha);
}

/// AllocateToLeaf (0x8048EC3C): the node depth levels below root along addr (a nibble a level), the missing ones
/// allocated (InitBranch; InitLeaf for the leaf). Native: NULL when a node cannot be allocated.
_rwPalQuantOctNode* AllocateToLeaf(RwPalQuant* pq, _rwPalQuantOctNode* root, RwUInt32 addr, RwInt32 depth) {
    for (; depth != 0; --depth, addr >>= 4) {
        _rwPalQuantOctNode*& child = root->Branch.dir[addr & 15u];
        if (child == nullptr) {
            child = static_cast<_rwPalQuantOctNode*>(rwSrcGlobals().memoryAlloc(pq->cubefreelist, kNodeHint));
            if (child == nullptr) return nullptr;
            InitBranch(&child->Branch);
            if (depth == 1) InitLeaf(&child->Leaf);
        }
        root = child;
    }
    return root;
}

/// LeafAddPixel (inlined in the original's RwPalQuantAddImage): a pixel with weight added to its leaf, its colour
/// relative to the cell's corner (the low 8 - QuantDepth bits), in match space.
void LeafAddPixel(_rwPalQuantLeafNode* leaf, const RwUInt8* c, RwReal weight) {
    constexpr RwUInt8 mask = static_cast<RwUInt8>((1u << (8 - QuantDepth)) - 1u);
    leaf->weight = leaf->weight + weight;
    const RwReal r = kByteToReal * static_cast<RwReal>(c[0] & mask) * kRedWeight;
    const RwReal g = kByteToReal * static_cast<RwReal>(c[1] & mask);
    const RwReal b = kByteToReal * static_cast<RwReal>(c[2] & mask) * kBlueWeight;
    const RwReal a = kByteToReal * static_cast<RwReal>(c[3] & mask);
    leaf->var = std::fmaf(weight, std::fmaf(a, a, std::fmaf(b, b, std::fmaf(r, r, g * g))), leaf->var);
    leaf->ac.red = leaf->ac.red + r * weight;
    leaf->ac.green = leaf->ac.green + g * weight;
    leaf->ac.blue = leaf->ac.blue + b * weight;
    leaf->ac.alpha = leaf->ac.alpha + a * weight;
}

/// The child corner of corner (cells, or bytes) for child i at a cell size of 1 << shift.
template <typename T>
void childCorner(T* out, const T* corner, RwInt32 i, RwInt32 shift) {
    out[0] = static_cast<T>(corner[0] + (((i >> 3) & 1) << shift));
    out[1] = static_cast<T>(corner[1] + (((i >> 2) & 1) << shift));
    out[2] = static_cast<T>(corner[2] + (((i >> 1) & 1) << shift));
    out[3] = static_cast<T>(corner[3] + ((i & 1) << shift));
}

/// Whether the node's cube, corner .. corner + 2^level cells, overlaps box.
bool overlaps(const RwInt32* corner, RwInt32 level, const _rwPalQuantRGBABox* box) {
    const RwInt32 size = 1 << level;
    for (int c = 0; c < 4; ++c) {
        if (corner[c] + size - box->col0[c] <= 0) return false;
    }
    for (int c = 0; c < 4; ++c) {
        if (corner[c] - box->col1[c] >= 0) return false;
    }
    return true;
}

/// assignindex (0x8048F350): every leaf in box takes index.
void assignindex(_rwPalQuantOctNode* node, const RwInt32* corner, RwInt32 level, const _rwPalQuantRGBABox* box,
                 RwUInt8 index) {
    if (node == nullptr || !overlaps(corner, level, box)) return;
    if (level == 0) {
        node->Leaf.palIndex = index;
        return;
    }
    for (RwInt32 i = 0; i < 16; ++i) {
        RwInt32 child[4];
        childCorner(child, corner, i, level - 1);
        assignindex(node->Branch.dir[i], child, level - 1, box, index);
    }
}

/// addvolume (0x8048F4CC; StatsAdd inlined): the statistics of every node wholly in box added to out.
void addvolume(_rwPalQuantOctNode* node, const RwInt32* corner, RwInt32 level, const _rwPalQuantRGBABox* box,
               _rwPalQuantLeafNode* out) {
    if (node == nullptr || !overlaps(corner, level, box)) return;
    const RwInt32 size = 1 << level;
    bool inside = true;
    for (int c = 0; c < 4; ++c) inside = inside && corner[c] - box->col0[c] >= 0;
    for (int c = 0; c < 4; ++c) inside = inside && box->col1[c] - (corner[c] + size) >= 0;
    if (inside) {
        StatsAdd(out, out, &node->Leaf);
    } else if (level > 0) {
        for (RwInt32 i = 0; i < 16; ++i) {
            RwInt32 child[4];
            childCorner(child, corner, i, level - 1);
            addvolume(node->Branch.dir[i], child, level - 1, box, out);
        }
    }
}

/// BoxStats (inlined in the original): the statistics of the leaves in box.
void BoxStats(_rwPalQuantOctNode* root, const _rwPalQuantRGBABox* box, _rwPalQuantLeafNode* out) {
    InitLeaf(out);
    const RwInt32 corner[4] = {0, 0, 0, 0};
    addvolume(root, corner, QuantDepth, box, out);
}

/// FindBestCut (0x8048F798; BoxStats and StatsSub inlined): a binary search along axis dir for the cut of box whose
/// two halves have the least total variance; returns it (whole->var when none is less) and sets cut[dir] to its place.
RwReal FindBestCut(_rwPalQuantOctNode* root, const _rwPalQuantRGBABox* box, RwInt32 dir, RwInt32* cut,
                   const _rwPalQuantLeafNode* whole) {
    RwReal best = whole->var;
    _rwPalQuantRGBABox part = *box;
    RwInt32 lo = box->col0[dir], hi = box->col1[dir];
    while (hi - lo > 1) {
        const RwInt32 mid = lo + ((hi - lo) >> 1);
        part.col1[dir] = mid;
        _rwPalQuantLeafNode in;
        BoxStats(root, &part, &in);
        const _rwPalQuantLeafNode rest = StatsSub(whole, &in);
        if (kZero == in.weight) {
            lo = mid;
        } else if (kZero == rest.weight) {
            hi = mid;
        } else {
            const RwReal total = in.var + rest.var;
            if (total < best) {
                best = total;
                cut[dir] = part.col1[dir];
            }
            if (in.var < rest.var) lo = mid;
            else hi = mid;
        }
    }
    return best;
}

/// nCut (0x8048F9D8): box cut along the axis whose best cut has the least variance, when that is less than whole's:
/// newBox the upper part, box the lower. FALSE when no cut reduces it.
RwBool nCut(_rwPalQuantOctNode* root, const _rwPalQuantLeafNode* whole, _rwPalQuantRGBABox* box,
            _rwPalQuantRGBABox* newBox) {
    RwInt32 cut[4];
    RwReal v[4];
    for (RwInt32 d = 0; d < 4; ++d) v[d] = FindBestCut(root, box, d, cut, whole);
    RwInt32 i = 0;
    for (RwInt32 d = 1; d < 4; ++d) {
        if (v[d] < v[i]) i = d;
    }
    if (!(v[i] < whole->var)) return 0;
    *newBox = *box;
    const RwInt32 at = cut[i] & 0xFF;
    newBox->col0[i] = at;
    box->col1[i] = at;
    return 1;
}

/// CalcNodeWeights (0x8048FB74): every node's statistics, from its leaves (LeafCalcStats at the corner, bytes) up;
/// returns the node's, NULL for no node.
_rwPalQuantLeafNode* CalcNodeWeights(_rwPalQuantOctNode* node, const RwUInt8* corner, RwInt32 depth) {
    if (node == nullptr) return nullptr;
    if (depth > 0) {
        InitLeaf(&node->Leaf);
        for (RwInt32 i = 0; i < 16; ++i) {
            RwUInt8 child[4];
            childCorner(child, corner, i, depth - 1 + (8 - QuantDepth));
            const _rwPalQuantLeafNode* const sub = CalcNodeWeights(node->Branch.dir[i], child, depth - 1);
            if (sub != nullptr) StatsAdd(&node->Leaf, &node->Leaf, sub);
        }
    } else {
        LeafCalcStats(&node->Leaf, corner);
    }
    return &node->Leaf;
}

/// CountLeafs (0x8048FF4C).
RwInt32 CountLeafs(const _rwPalQuantOctNode* node, RwInt32 depth) {
    if (node == nullptr) return 0;
    if (depth <= 0) return 1;
    RwInt32 n = 0;
    for (const _rwPalQuantOctNode* d : node->Branch.dir) n += CountLeafs(d, depth - 1);
    return n;
}

/// ExtractNodes (0x80490234): each leaf's mean colour to palette[index], in tree order, the leaf taking the index;
/// returns the next index.
RwInt32 ExtractNodes(_rwPalQuantOctNode* node, RwRGBA* palette, RwInt32 index, RwInt32 depth) {
    if (node == nullptr) return index;
    if (depth > 0) {
        for (_rwPalQuantOctNode* d : node->Branch.dir) index = ExtractNodes(d, palette, index, depth - 1);
        return index;
    }
    RepresentativeColor(&palette[index], &node->Leaf);
    node->Leaf.palIndex = static_cast<RwUInt8>(index);
    return index + 1;
}

/// GetIndex (0x80490504): the palette index of the leaf at addr. Native: 0 where the tree has no node (the original
/// reads through NULL).
RwUInt8 GetIndex(const _rwPalQuantOctNode* node, RwUInt32 addr, RwInt32 depth) {
    for (; depth != 0; --depth, addr >>= 4) {
        node = node->Branch.dir[addr & 15u];
        if (node == nullptr) return 0;
    }
    return node->Leaf.palIndex;
}

/// DeleteOctTree (0x80491124): the node and those under it returned to the free list, children first.
void DeleteOctTree(RwPalQuant* pq, _rwPalQuantOctNode* node, RwInt32 depth) {
    if (node == nullptr) return;
    if (depth > 0) {
        for (_rwPalQuantOctNode* d : node->Branch.dir) DeleteOctTree(pq, d, depth - 1);
    }
    rwSrcGlobals().memoryFree(pq->cubefreelist, node);
}

/// The pixel at x of row: its palette entry for depths 4 and 8, itself for 32.
const RwUInt8* pixelColour(const RwImage* img, const RwUInt8* row, RwInt32 x) {
    return img->depth == 32 ? row + 4 * x : &img->palette[row[x]].red;
}

}  // namespace

RwBool RwPalQuantInit(RwPalQuant* pq) {
    pq->cubefreelist = RwFreeListCreate(static_cast<RwInt32>(sizeof(_rwPalQuantOctNode)), kNodesPerBlock,
                                        static_cast<RwInt32>(alignof(_rwPalQuantOctNode)), kNodeHint);
    if (pq->cubefreelist == nullptr) return 0;   // native
    pq->root = static_cast<_rwPalQuantOctNode*>(rwSrcGlobals().memoryAlloc(pq->cubefreelist, kNodeHint));
    if (pq->root == nullptr) {   // native
        RwFreeListDestroy(pq->cubefreelist);
        pq->cubefreelist = nullptr;
        return 0;
    }
    InitBranch(&pq->root->Branch);
    return 1;
}

void RwPalQuantTerm(RwPalQuant* pq) {
    DeleteOctTree(pq, pq->root, QuantDepth);
    pq->root = nullptr;
    RwFreeListDestroy(pq->cubefreelist);
}

void RwPalQuantAddImage(RwPalQuant* pq, RwImage* img, RwReal weight) {
    if (img->depth != 4 && img->depth != 8 && img->depth != 32) return;
    const RwUInt8* row = img->cpPixels;
    for (RwInt32 y = 0; y < img->height; ++y, row += img->stride) {
        for (RwInt32 x = 0; x < img->width; ++x) {
            const RwUInt8* const c = pixelColour(img, row, x);
            _rwPalQuantOctNode* const leaf = AllocateToLeaf(pq, pq->root, GetOctAdr(c), QuantDepth);
            if (leaf != nullptr) LeafAddPixel(&leaf->Leaf, c, weight);
        }
    }
}

RwInt32 RwPalQuantResolvePalette(RwRGBA* palette, RwInt32 maxcols, RwPalQuant* pq) {
    if (maxcols > RWPALQUANT_MAXCOLOR) maxcols = RWPALQUANT_MAXCOLOR;   // native
    const RwUInt8 origin[4] = {0, 0, 0, 0};
    CalcNodeWeights(pq->root, origin, QuantDepth);
    for (RwInt32 i = 0; i < maxcols; ++i) palette[i] = {0, 0, 0, 0};
    const RwInt32 leaves = CountLeafs(pq->root, QuantDepth);
    if (leaves <= maxcols) {
        ExtractNodes(pq->root, palette, 0, QuantDepth);
        return leaves;
    }
    pq->Mcube[0] = {{0, 0, 0, 0}, {kCells, kCells, kCells, kCells}};
    BoxStats(pq->root, &pq->Mcube[0], &pq->Mvol[0]);
    pq->Mvv[0] = pq->Mvol[0].var;
    RwInt32 k = 1;
    for (; k < maxcols; ++k) {
        RwReal largest = kZero;
        RwInt32 i = -1;
        for (RwInt32 j = 0; j < k; ++j) {
            if (pq->Mvv[j] > largest) largest = pq->Mvv[j], i = j;
        }
        if (i == -1) break;
        if (nCut(pq->root, &pq->Mvol[i], &pq->Mcube[i], &pq->Mcube[k])) {
            BoxStats(pq->root, &pq->Mcube[i], &pq->Mvol[i]);
            BoxStats(pq->root, &pq->Mcube[k], &pq->Mvol[k]);
            pq->Mvv[i] = pq->Mvol[i].var;
            pq->Mvv[k] = pq->Mvol[k].var;
        } else {
            pq->Mvv[i] = kZero;
            --k;
        }
    }
    // AssignPalIndex (inlined in the original), for j < maxcols. Native: not past the k boxes made (the original goes
    // on to maxcols when its cut stops short).
    const RwInt32 corner[4] = {0, 0, 0, 0};
    for (RwInt32 j = 0; j < k && j < maxcols; ++j) {
        assignindex(pq->root, corner, QuantDepth, &pq->Mcube[j], static_cast<RwUInt8>(j));
        RepresentativeColor(&palette[j], &pq->Mvol[j]);
    }
    return maxcols;
}

void RwPalQuantMatchImage(RwUInt8* dstpixels, RwInt32 dststride, RwInt32 dstdepth, RwBool dstPacked, RwPalQuant* pq,
                          RwImage* img) {
    if (img->depth != 4 && img->depth != 8 && img->depth != 32) return;
    const bool packed = dstdepth == 4 && dstPacked == 1;
    const RwUInt8* row = img->cpPixels;
    for (RwInt32 y = 0; y < img->height; ++y, row += img->stride, dstpixels += dststride) {
        RwUInt8* d = dstpixels;
        for (RwInt32 x = 0; x < img->width; ++x) {
            const RwUInt8 index = GetIndex(pq->root, GetOctAdr(pixelColour(img, row, x)), QuantDepth);
            if (!packed) {
                *d++ = index;
            } else if (x & 1) {
                *d = static_cast<RwUInt8>((*d & 0x0Fu) | ((index & 0x0Fu) << 4));
                ++d;
            } else {
                *d = static_cast<RwUInt8>((*d & 0xF0u) | (index & 0x0Fu));
            }
        }
    }
}
