// RenderWare memory and free lists (see rw/core/memory.h).
#include "rw/core/memory.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <new>
#include <set>
#include <unordered_map>
#include <utility>

#include "rw/core/error.h"

namespace {

bool g_preallocate = true;           // lbl_805EEF50 (_rwFreeListEnable): 1 in the game's data
bool g_memoryOpen = false;           // lbl_805F2610
RwFreeList* g_master = nullptr;      // _masterFreeListPtr
RwFreeList g_masterSpace{};          // lbl_805E3B40: the master list's in-place header
RwLinkList g_freeLists{};            // lbl_805F2618: every free list but the master

constexpr std::size_t kLinkSlack = sizeof(RwLLLink) - 1;   // the original's 7 (an 8-byte link), here 15
constexpr RwUInt32 kDurationMask = 0x00FF0000u;            // rwMEMHINTDUR_MASK

void linkInsertAfter(RwLLLink* head, RwLLLink* link) {    // RenderWare's rwLinkListAddLLLink: at the head
    link->next = head->next;
    link->prev = head;
    head->next->prev = link;
    head->next = link;
}

void linkRemove(RwLLLink* link) {
    link->prev->next = link->next;
    link->next->prev = link->prev;
}

std::uintptr_t alignDown(std::uintptr_t v, RwUInt32 alignment) { return v & ~std::uintptr_t{alignment - 1}; }

/// The first entry of a block: the end of the link and bitmap rounded up to the alignment (the original's
/// (block + heapSize + 7 + alignment) & ~(alignment - 1) = align_up(block + 8 + heapSize)).
std::uintptr_t entryBase(const RwFreeList* fl, const void* block) {
    return alignDown(reinterpret_cast<std::uintptr_t>(block) + fl->heapSize + kLinkSlack + fl->alignment, fl->alignment);
}

std::size_t blockBytes(const RwFreeList* fl) {
    return std::size_t{fl->heapSize} + std::size_t{fl->entriesPerBlock} * fl->entrySize + fl->alignment + kLinkSlack;
}

RwUInt8* bitmapOf(void* block) { return static_cast<RwUInt8*>(block) + sizeof(RwLLLink); }

/// Where a block's entry indexes count from: the unaligned end of its bitmap (the original's index base).
std::uintptr_t indexBase(const RwFreeList* fl, const void* block) {
    return reinterpret_cast<std::uintptr_t>(block) + sizeof(RwLLLink) + fl->heapSize;
}

/// A free list's block index. Blocks join the list at its head and never move, so list order is creation order
/// newest first: the non-full set ordered by creation number (descending) starts at the block the original's scan
/// would reach first.
struct BlockIndex {
    struct Info {
        std::uint64_t seq;
        RwUInt32 used;
    };
    std::map<std::uintptr_t, RwLLLink*> byBase;   // index base -> block
    std::set<std::pair<std::uint64_t, RwLLLink*>, std::greater<>> nonFull;
    std::unordered_map<RwLLLink*, Info> info;
    std::uint64_t nextSeq = 0;
};
RwInt32 g_liveIndexes = 0;

BlockIndex& indexOf(RwFreeList* fl) { return *static_cast<BlockIndex*>(fl->index); }

void indexAdd(RwFreeList* fl, RwLLLink* block) {
    BlockIndex& ix = indexOf(fl);
    const std::uint64_t seq = ix.nextSeq++;
    ix.info[block] = {seq, 0};
    ix.byBase[indexBase(fl, block)] = block;
    ix.nonFull.insert({seq, block});
}

void indexRemove(RwFreeList* fl, RwLLLink* block) {
    BlockIndex& ix = indexOf(fl);
    const auto it = ix.info.find(block);
    ix.nonFull.erase({it->second.seq, block});
    ix.byBase.erase(indexBase(fl, block));
    ix.info.erase(it);
}

void indexDelete(RwFreeList* fl) {
    if (fl->index == nullptr) return;
    delete static_cast<BlockIndex*>(fl->index);
    fl->index = nullptr;
    --g_liveIndexes;
}

/// Frees blocks and (unless in place) the header; the shared tail of RwFreeListDestroy, the create failure path
/// (0x80480C74, which does not unlink) and _rwMemoryClose.
void releaseBlocksAndHeader(RwFreeList* fl) {
    RwLLLink* head = &fl->blockList.link;
    while (head->next != head) {
        RwLLLink* block = head->next;
        linkRemove(block);
        RwFree(block);
    }
    indexDelete(fl);
    if (fl->flags & rwFREELISTFLAG_STATIC) return;
    if (g_master == nullptr || g_master == fl) {
        RwFree(fl);
    } else {
        rwSrcGlobals().memoryFree(g_master, fl);
    }
}

}  // namespace

void _rwFreeListEnable(RwBool enabled) { g_preallocate = enabled != 0; }

RwFreeList* RwFreeListCreateAndPreallocateSpace(RwInt32 entrySize, RwInt32 entriesPerBlock, RwInt32 alignment,
                                               RwInt32 numBlocksToPreallocate, RwFreeList* inPlaceSpace,
                                               RwUInt32 hint) {
    // Native contract: the parameters must describe a usable list. The original takes anything (an entry size of 0
    // divides by zero in _rwFreeListFreeReal; an alignment that is not a power of two misplaces entries); here they
    // fail with E_RW_BADPARAM. The game passes none of these.
    const RwUInt32 align = alignment == 0 ? 32u : static_cast<RwUInt32>(alignment);
    if (entrySize <= 0 || entriesPerBlock <= 0 || alignment < 0 || numBlocksToPreallocate < 0 ||
        (align & (align - 1u)) != 0 || std::uint64_t{static_cast<RwUInt32>(entrySize)} + align - 1u > 0xFFFFFFFFull) {
        RwError error{rwID_COREPLUGIN, _rwerror(E_RW_BADPARAM, "invalid free list parameters")};
        RwErrorSet(&error);
        return nullptr;
    }
    if (!g_preallocate) numBlocksToPreallocate = 0;
    RwFreeList* fl = inPlaceSpace;
    if (fl == nullptr) {
        fl = static_cast<RwFreeList*>(g_master != nullptr ? rwSrcGlobals().memoryAlloc(g_master, hint & kDurationMask)
                                                          : RwMalloc(sizeof(RwFreeList), hint & kDurationMask));
        if (fl == nullptr) return nullptr;
        fl->flags = rwFREELISTFLAG_FREEBLOCKS;
    } else {
        fl->flags = rwFREELISTFLAG_FREEBLOCKS | rwFREELISTFLAG_STATIC;
    }
    const RwUInt32 perBlock = static_cast<RwUInt32>(entriesPerBlock);
    fl->entrySize = (static_cast<RwUInt32>(entrySize) + align - 1u) & ~(align - 1u);
    fl->entriesPerBlock = perBlock;
    fl->alignment = align;
    fl->heapSize = (perBlock + 7u) >> 3;
    fl->blockList.link.next = fl->blockList.link.prev = &fl->blockList.link;
    fl->index = new (std::nothrow) BlockIndex;
    if (fl->index == nullptr) {
        RwError error{rwID_COREPLUGIN, _rwerror(E_RW_NOMEM, static_cast<RwInt32>(sizeof(BlockIndex)))};
        RwErrorSet(&error);
        releaseBlocksAndHeader(fl);
        return nullptr;
    }
    ++g_liveIndexes;
    for (RwUInt32 n = static_cast<RwUInt32>(numBlocksToPreallocate); n != 0; --n) {
        void* block = RwMalloc(blockBytes(fl), hint);
        if (block == nullptr) {
            releaseBlocksAndHeader(fl);
            return nullptr;
        }
        linkInsertAfter(&fl->blockList.link, static_cast<RwLLLink*>(block));
        std::memset(bitmapOf(block), 0, fl->heapSize);
        indexAdd(fl, static_cast<RwLLLink*>(block));
    }
    if (g_freeLists.link.next == nullptr) g_freeLists.link.next = g_freeLists.link.prev = &g_freeLists.link;
    linkInsertAfter(&g_freeLists.link, &fl->link);
    return fl;
}

RwFreeList* RwFreeListCreate(RwInt32 entrySize, RwInt32 entriesPerBlock, RwInt32 alignment, RwUInt32 hint) {
    return RwFreeListCreateAndPreallocateSpace(entrySize, entriesPerBlock, alignment, 1, nullptr, hint);
}

RwBool RwFreeListDestroy(RwFreeList* freelist) {
    linkRemove(&freelist->link);
    releaseBlocksAndHeader(freelist);
    return 1;
}

void* _rwFreeListAllocReal(RwFreeList* freelist, RwUInt32 hint) {
    BlockIndex& ix = indexOf(freelist);
    RwLLLink* block = nullptr;
    if (!ix.nonFull.empty()) {
        block = ix.nonFull.begin()->second;   // the first block in list order with a free entry
    } else {
        block = static_cast<RwLLLink*>(RwMalloc(blockBytes(freelist), hint));
        if (block == nullptr) return nullptr;
        std::memset(bitmapOf(block), 0, freelist->heapSize);
        linkInsertAfter(&freelist->blockList.link, block);
        indexAdd(freelist, block);
    }
    // its first free entry (most significant bit first, as the original scans)
    RwUInt8* bits = bitmapOf(block);
    RwUInt32 byte = 0;
    while (bits[byte] == 0xFF) ++byte;
    RwUInt32 bit = 0;
    while (bits[byte] & (0x80u >> bit)) ++bit;
    bits[byte] = static_cast<RwUInt8>(bits[byte] | (0x80u >> bit));
    BlockIndex::Info& info = ix.info[block];
    if (++info.used == freelist->entriesPerBlock) ix.nonFull.erase({info.seq, block});
    return reinterpret_cast<void*>(entryBase(freelist, block) + std::uintptr_t{freelist->entrySize} * (byte * 8u + bit));
}

RwFreeList* _rwFreeListFreeReal(RwFreeList* freelist, void* pMem) {
    const auto entry = reinterpret_cast<std::uintptr_t>(pMem);
    BlockIndex& ix = indexOf(freelist);
    // the block whose index base is the last at or below the entry (blocks never overlap, so it is the one the
    // original's scan finds); the original's index base is the unaligned end of the bitmap; entries are at least an
    // alignment step long, so the padding never reaches the next index
    auto it = ix.byBase.upper_bound(entry);
    if (it == ix.byBase.begin()) return nullptr;
    --it;
    const std::uintptr_t start = it->first;
    if (entry > start + std::uintptr_t{freelist->entriesPerBlock} * freelist->entrySize) return nullptr;
    RwLLLink* block = it->second;
    const std::uintptr_t index = (entry - start) / freelist->entrySize;
    RwUInt8* bits = bitmapOf(block);
    const RwUInt8 mask = static_cast<RwUInt8>(0x80u >> (index & 7u));
    BlockIndex::Info& info = ix.info[block];
    if (index < freelist->entriesPerBlock && (bits[index >> 3] & mask)) {
        if (info.used-- == freelist->entriesPerBlock) ix.nonFull.insert({info.seq, block});
    }
    bits[index >> 3] = static_cast<RwUInt8>(bits[index >> 3] & ~mask);
    // the original frees the block when its bitmap sums to 0: no entry in use
    if ((freelist->flags & rwFREELISTFLAG_FREEBLOCKS) && info.used == 0) {
        indexRemove(freelist, block);
        linkRemove(block);
        RwFree(block);
    }
    return freelist;
}

RwFreeList* RwFreeListForAllUsed(RwFreeList* freelist, RwFreeListCallBack fpCallBack, void* pData) {
    RwLLLink* head = &freelist->blockList.link;
    for (RwLLLink* block = head->next; block != head;) {
        auto* copy = static_cast<RwUInt8*>(RwMalloc(freelist->heapSize, 0x00010000u));   // rwMEMHINTDUR_FUNCTION
        if (copy == nullptr) return nullptr;
        std::memcpy(copy, bitmapOf(block), freelist->heapSize);
        RwLLLink* next = block->next;
        const std::uintptr_t base = entryBase(freelist, block);
        for (RwUInt32 byte = 0; byte < freelist->heapSize; ++byte) {
            if (copy[byte] == 0) continue;
            for (RwUInt32 bit = 0; bit < 8; ++bit) {
                if (copy[byte] & (0x80u >> bit)) {
                    fpCallBack(reinterpret_cast<void*>(base + std::uintptr_t{freelist->entrySize} * (byte * 8u + bit)),
                               pData);
                }
            }
        }
        RwFree(copy);
        block = next;
    }
    return freelist;
}

RwBool _rwMemoryOpen(const RwMemoryFunctions* memoryFuncs) {
    g_freeLists.link.next = g_freeLists.link.prev = &g_freeLists.link;
    g_memoryOpen = true;
    g_master = RwFreeListCreateAndPreallocateSpace(sizeof(RwFreeList), 0x10, 0x20, 0, &g_masterSpace,
                                                   0x00040000u);   // rwMEMHINTDUR_GLOBAL
    if (g_master == nullptr) {
        g_memoryOpen = false;
        return 0;
    }
    linkRemove(&g_master->link);                               // the master is not among the free lists it holds
    rwSrcGlobals().memoryFuncs = memoryFuncs != nullptr ? *memoryFuncs : rwHostMemoryFunctions();
    return 1;
}

void _rwMemoryClose() {
    RwLLLink* head = &g_freeLists.link;
    while (head->next != head) {
        auto* fl = reinterpret_cast<RwFreeList*>(reinterpret_cast<char*>(head->next) - offsetof(RwFreeList, link));
        linkRemove(&fl->link);
        releaseBlocksAndHeader(fl);
    }
    RwFreeList* master = g_master;
    linkRemove(&master->link);
    releaseBlocksAndHeader(master);
    g_master = nullptr;
    g_memoryOpen = false;
}

RwMemoryFunctions* RwOsGetMemoryInterface() { return &rwSrcGlobals().memoryFuncs; }

RwFreeList* _rwFreeListMaster() { return g_master; }
RwInt32 _rwFreeListLiveIndexes() { return g_liveIndexes; }
RwLinkList* _rwFreeListAll() { return &g_freeLists; }
