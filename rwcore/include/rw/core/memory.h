// RenderWare Graphics 3.7 core: memory and free lists (module bamemory), native x64.
//
// A free list hands out fixed-size entries from blocks it allocates with RwMalloc: each block is {link, used bitmap,
// slack, entries}, the bitmap one bit per entry (most significant bit first), the entries aligned. The originals are
// PAL main.dol 0x80480A78..0x80481580; proof: runtime/tests/rwcore_memory_tests.cpp, which compares every list's
// fields, block order, bitmaps, the entry each allocation returns (as block and index), and every RwMalloc / RwFree.
//
// Speed: the original finds an entry's block by scanning the block list on every free and the newest block with room
// by scanning on every allocation - quadratic in the number of blocks (200,000 frames took 5 s to destroy). Here each
// list keeps an index (blocks by address, non-full blocks newest first, a used count per block), so both are
// O(log blocks); the blocks, their order, the bitmaps and every entry returned are exactly the original's (the oracle
// compares them after every operation).
//
// 64-bit: the block header is a native RwLLLink (16 bytes), so where the original reserves sizeof(link) - 1 = 7 bytes
// of slack this reserves 15; entry addresses follow the same rule from it. Byte counts are size_t (a block of
// entriesPerBlock x entrySize may exceed 4 GiB); the header's fields keep RenderWare's RwUInt32 widths, as the
// stream format and the originals use them.
#pragma once

#include "rw/core/engine.h"
#include "rw/core/types.h"

/// A free list (rwplcore.h RwFreeList).
struct RwFreeList {
    RwUInt32 entrySize;        ///< rounded up to alignment
    RwUInt32 entriesPerBlock;
    RwUInt32 heapSize;         ///< bytes of a block's used bitmap: (entriesPerBlock + 7) / 8
    RwUInt32 alignment;
    RwLinkList blockList;      ///< the blocks, newest first
    RwUInt32 flags;            ///< rwFREELISTFLAG_*
    RwLLLink link;             ///< in the list of all free lists
    void* index;               ///< native: the blocks by address and the non-full blocks by age (memory.cpp)
};

inline constexpr RwUInt32 rwFREELISTFLAG_STATIC = 0x00000001;      ///< the RwFreeList itself is the caller's
inline constexpr RwUInt32 rwFREELISTFLAG_FREEBLOCKS = 0x00000002;  ///< a block is freed when it empties

using RwFreeListCallBack = void (*)(void* pMem, void* pData);

/// Enables or disables preallocation (FreeListCreate allocates no block while disabled). Original: 0x80480A78.
void _rwFreeListEnable(RwBool enabled);

/// A free list of entries of entrySize bytes, entriesPerBlock to a block, aligned to alignment (0: 32), with
/// numBlocksToPreallocate blocks allocated now (none while preallocation is disabled). inPlaceSpace: the caller's
/// RwFreeList (then never freed), else one from the master free list (or RwMalloc before there is one). Returns the
/// list or NULL. Native contract: entrySize and entriesPerBlock > 0, alignment 0 or a power of two, else NULL with
/// E_RW_BADPARAM (the original accepts them and fails later). Originals: 0x80480A80 (FreeListCreate), 0x80480C54
/// (RwFreeListCreateAndPreallocateSpace).
RwFreeList* RwFreeListCreateAndPreallocateSpace(RwInt32 entrySize, RwInt32 entriesPerBlock, RwInt32 alignment,
                                               RwInt32 numBlocksToPreallocate, RwFreeList* inPlaceSpace,
                                               RwUInt32 hint);

/// RwFreeListCreateAndPreallocateSpace with one block and no in-place space. Original: 0x80480C28.
RwFreeList* RwFreeListCreate(RwInt32 entrySize, RwInt32 entriesPerBlock, RwInt32 alignment, RwUInt32 hint);

/// Frees the list's blocks and, unless it is in place, the list. Returns TRUE. Original: 0x80480D2C.
RwBool RwFreeListDestroy(RwFreeList* freelist);

/// An entry: the first free one, blocks newest first, else entry 0 of a new block (RwMalloc with hint). NULL when
/// that allocation fails. Original: 0x80480E00.
void* _rwFreeListAllocReal(RwFreeList* freelist, RwUInt32 hint);

/// Returns an entry; under rwFREELISTFLAG_FREEBLOCKS an emptied block is freed. Returns freelist, or NULL when pMem
/// is in none of its blocks. Original: 0x80480FA0.
RwFreeList* _rwFreeListFreeReal(RwFreeList* freelist, void* pMem);

/// Calls fpCallBack(entry, pData) for every used entry, blocks newest first, entries in order. Each block's bitmap is
/// copied first (RwMalloc), so the callback may free entries. Returns freelist, or NULL when the copy cannot be
/// allocated. Original: 0x8048112C.
RwFreeList* RwFreeListForAllUsed(RwFreeList* freelist, RwFreeListCallBack fpCallBack, void* pData);

/// Opens memory: creates the master free list (it holds the free lists' headers) and installs memoryFuncs, or the
/// host allocator when NULL. Returns FALSE when the master free list cannot be made. Original: 0x804812C4.
RwBool _rwMemoryOpen(const RwMemoryFunctions* memoryFuncs);

/// Destroys every free list, then the master one. Original: 0x804813DC.
void _rwMemoryClose();

/// The engine's memory functions. Original: 0x80481580.
RwMemoryFunctions* RwOsGetMemoryInterface();

/// The master free list (NULL before _rwMemoryOpen) and the list of all other free lists, for inspection.
RwFreeList* _rwFreeListMaster();
RwLinkList* _rwFreeListAll();

/// Native: the number of free lists whose block index is allocated (one per live list), for inspection.
RwInt32 _rwFreeListLiveIndexes();
