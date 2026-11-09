// RenderWare Graphics 3.7 core: the resources arena (modules baresour and resmem), native x64.
//
// The arena caches instanced data - what a pipeline builds from an object to draw it - in one fixed block of memory
// the size RwEngineInit was given (RwGlobals::resArenaInitSize). Each RwResEntry is a header and its data; the entry
// knows its owner and the owner's pointer to it (cleared when the entry goes) and has a callback run just before it
// is freed. Entries used since the last frame are on the used list, the others on the free list: the end of each
// frame (_rwResourcesPurge, from RwRasterShowRaster) moves every entry to the free list, and using an entry
// (RwResourcesUseResEntry) puts it back at the head of the used list. When the arena is full, an allocation frees
// the least recently used entries - free list first, then used list (counted in reusageSize) - until it fits.
// The memory inside the arena is resmem's first-fit heap: 32-byte blocks with a 32-byte header, split and merged.
//
// Written from PAL main.dol 0x8048158C..0x80481CBC (baresour) and 0x804839D4..0x80483C10 (resmem's init, close, free
// and alloc with its block split), the same code as the RenderWare 3.7 PC libraries' (rwcore.lib baresour.obj /
// resmem.obj). The game links no RwResourcesSetArenaSize / GetArenaSize / GetArenaUsage and no _rwResHeapCalloc /
// Realloc / GetStats: those are not provided. Native structures (64-bit pointers); the heap's block header stays 32
// bytes (RwUInt32 size and flags), so the heap lays blocks out as the original does. Proof:
// runtime/tests/rwcore_resources_tests.cpp (oracle) and rwcore_resources_native_tests.cpp; rwcore/README.md.
#pragma once

#include <cstddef>

#include "rw/core/engine.h"
#include "rw/core/types.h"

struct RwResEntry;

/// Called by RwResourcesFreeResEntry (and the arena's evictions and RwResourcesEmptyArena) just before the entry's
/// memory is released (rwplcore.h RwResEntryDestroyNotify).
using RwResEntryDestroyNotify = void (*)(RwResEntry* resEntry);

/// An entry of the arena (rwplcore.h RwResEntry); its data follows the structure.
struct RwResEntry {
    RwLLLink link;                          ///< in the used or free list; next is NULL for an entry outside the arena
    RwInt32 size;                           ///< the data's size (the header not counted)
    void* owner;
    RwResEntry** ownerRef;                  ///< the owner's pointer to this entry: cleared when the entry is freed
    RwResEntryDestroyNotify destroyNotify;  ///< run just before the entry is freed
};

/// The arena (rwplcore.h rwResources).
struct rwResources {
    RwInt32 maxSize;        ///< the arena size it was opened with (RwGlobals::resArenaInitSize)
    RwInt32 currentSize;    ///< the data bytes of the entries in it
    RwInt32 reusageSize;    ///< the data bytes of used entries evicted since the last purge
    void* memHeap;          ///< the arena memory (a resmem heap); NULL for an arena of size 0
    RwLinkList entriesA;
    RwLinkList entriesB;
    RwLinkList* freeEntries;   ///< entriesA or entriesB: not used since the last purge (least recent at the tail)
    RwLinkList* usedEntries;   ///< the other one: used since the last purge (most recent at the head)
};

/// The resources module's globals (rwplcore.h rwResourcesGlobals).
struct rwResourcesGlobals {
    rwResources res;
};

/// The resources module's registration (rwplcore.h resourcesModule).
extern RwModuleInfo resourcesModule;

inline constexpr RwUInt32 rwID_RESOURCESMODULE = 0x40Bu;  ///< rwID_RESOURCESMODULE (rwcore module id)

/// RWRESOURCESGLOBAL(res): the arena of the engine instance.
inline rwResources& rwResourcesArena() { return rwModuleGlobals<rwResourcesGlobals>(resourcesModule).res; }

/// Opens the resources module: an arena of RwGlobals::resArenaInitSize bytes (RwMalloc, hint rwMEMHINTDUR_GLOBAL |
/// rwID_RESOURCESMODULE; none for 0), empty lists. Fails with E_RW_NOMEM (the size) when the memory is not there and
/// E_RW_INSUFFICIENTRESOURCES (0) when it is too small for a heap (the memory freed; native: memHeap cleared, the
/// original leaves it pointing at the freed memory). Returns instance. Original: 0x8048158C.
void* _rwResourcesOpen(void* instance, RwInt32 offset, RwInt32 size);

/// Empties the arena (RwResourcesEmptyArena) and frees its memory. Returns instance. Original: 0x804816E0.
void* _rwResourcesClose(void* instance, RwInt32 offset, RwInt32 size);

/// Frees an entry: its destroy callback, its owner's pointer cleared, then out of its list and the arena (currentSize
/// down by its size) - or RwFree for an entry outside the arena (link.next NULL). Returns TRUE. Original: 0x8048176C.
RwBool RwResourcesFreeResEntry(RwResEntry* entry);

/// The end of a frame: every entry becomes free, the used ones (most recent first) ahead of those already free; the
/// lists swap roles, reusageSize returns to 0. Native: does nothing while no resources module is open (the original
/// works on the module's unset globals). Original: 0x8048181C.
void _rwResourcesPurge();

/// An entry of size data bytes for owner, at the head of the used list, *ownerRef set to it (when ownerRef is not
/// NULL), currentSize up by size. When the arena has no room, the free list's then the used list's least recent
/// entries are freed (RwResourcesFreeResEntry; used ones counted in reusageSize) until it has. NULL, *ownerRef NULL
/// and E_RW_INSUFFICIENTRESOURCES (size) when even the empty arena has no room. Native: a negative size, and an arena of
/// size 0, fail the same way at once (the original allocates a block too small for the header for the first and
/// reads through NULL for the second). Original: 0x804818C8.
RwResEntry* RwResourcesAllocateResEntry(void* owner, RwResEntry** ownerRef, RwInt32 size,
                                        RwResEntryDestroyNotify destroyNotify);

/// Frees every entry, those of entriesA then those of entriesB, in list order (RwResourcesFreeResEntry); both lists
/// empty, reusageSize 0. Returns TRUE. Original: 0x80481B58.
RwBool RwResourcesEmptyArena();

/// An entry was used this frame: moved to the head of the used list (an entry outside the arena stays where it is).
/// Returns entry. RenderWare's release macro (rwplcore.h), inlined into its callers: there is no original function.
inline RwResEntry* RwResourcesUseResEntry(RwResEntry* entry) {
    if (entry->link.next != nullptr) {
        entry->link.prev->next = entry->link.next;
        entry->link.next->prev = entry->link.prev;
        RwLLLink* const head = &rwResourcesArena().usedEntries->link;
        entry->link.next = head->next;
        entry->link.prev = head;
        head->next->prev = &entry->link;
        head->next = &entry->link;
    }
    return entry;
}

// ---- resmem: the arena's heap ----------------------------------------------------------------------------------

/// rwRESHEAP_BLOCKUSED: the block is allocated.
inline constexpr RwUInt32 rwRESHEAP_BLOCKUSED = 0x1u;

struct rwResHeap;

/// A block of the heap (resmem.c rwResHeapBlockHeader); its memory follows the 32-byte header.
struct rwResHeapBlockHeader {
    rwResHeap* heap;
    rwResHeapBlockHeader* next;   ///< the block after it in memory
    rwResHeapBlockHeader* prev;   ///< the block before it
    RwUInt32 size;                ///< the bytes after the header, a multiple of 32
    RwUInt32 flags;               ///< rwRESHEAP_BLOCKUSED
};
static_assert(sizeof(rwResHeapBlockHeader) == 32, "the heap's block header is 32 bytes, as the original's");

/// The heap, at the start of its memory (resmem.c; firstBlock is resmem's name, firstFree descriptive: the libraries
/// do not name it).
struct rwResHeap {
    rwResHeapBlockHeader* firstBlock;
    rwResHeapBlockHeader* firstFree;   ///< where allocations start looking: at or before the first free block
};

/// Makes memory..memory + size a heap of one free block: the first 32-byte boundary that leaves room for rwResHeap
/// to the last one inside. FALSE when less than 32 bytes would be left after the block's header. Original: 0x804839D4.
RwBool _rwResHeapInit(void* memory, std::size_t size);

/// Nothing to release (the memory is the caller's). Returns TRUE. Original: 0x80483A24.
RwBool _rwResHeapClose(void* heap);

/// size bytes (rounded up to 32) from the first free block that holds them, split when more than 64 bytes would be
/// left over (the rest merged with a free block after it). NULL when none holds them. Original: 0x80483AE4.
void* _rwResHeapAlloc(void* heap, std::size_t size);

/// Returns memory from _rwResHeapAlloc, merged with free blocks before and after it. Original: 0x80483A2C.
void _rwResHeapFree(void* memory);
