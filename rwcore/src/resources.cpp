// RenderWare resources arena and its heap (see rw/core/resources.h).
#include "rw/core/resources.h"

#include <cstdint>
#include <initializer_list>
#include <limits>

#include "rw/core/error.h"

RwModuleInfo resourcesModule{};

namespace {

constexpr std::uintptr_t kBlockAlign = 32;
constexpr RwUInt32 kArenaHint = rwMEMHINTDUR_GLOBAL | rwID_RESOURCESMODULE;   // 0x4040B

rwResHeapBlockHeader* blockOf(void* memory) {
    return reinterpret_cast<rwResHeapBlockHeader*>(static_cast<char*>(memory) - sizeof(rwResHeapBlockHeader));
}
bool isFree(const rwResHeapBlockHeader* block) { return (block->flags & rwRESHEAP_BLOCKUSED) == 0; }

void raise(RwInt32 code, RwInt32 value) {
    RwError error{rwID_COREPLUGIN, _rwerror(code, value)};
    RwErrorSet(&error);
}

void emptyList(RwLinkList& list) { list.link.next = list.link.prev = &list.link; }

}  // namespace

// ---- resmem --------------------------------------------------------------------------------------------------------

RwBool _rwResHeapInit(void* memory, std::size_t size) {
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(memory);
    const std::uintptr_t first = (base + sizeof(rwResHeap) + kBlockAlign - 1) & ~(kBlockAlign - 1);
    const std::uintptr_t end = (base + size) & ~(kBlockAlign - 1);
    if (end < first + 2 * sizeof(rwResHeapBlockHeader)) return 0;   // the original: fewer than 32 bytes after the header
    const std::uintptr_t bytes = end - first - sizeof(rwResHeapBlockHeader);
    if (bytes > std::numeric_limits<RwUInt32>::max() - (kBlockAlign - 1)) return 0;   // native: past RwUInt32 sizes
    auto* const heap = static_cast<rwResHeap*>(memory);
    auto* const block = reinterpret_cast<rwResHeapBlockHeader*>(first);
    block->heap = heap;
    block->next = nullptr;
    block->prev = nullptr;
    block->flags = 0;
    block->size = static_cast<RwUInt32>(bytes);
    heap->firstBlock = block;
    heap->firstFree = block;
    return 1;
}

RwBool _rwResHeapClose(void* /*heap*/) { return 1; }

void* _rwResHeapAlloc(void* heapMemory, std::size_t size) {
    if (size > std::numeric_limits<RwUInt32>::max() - (kBlockAlign - 1)) return nullptr;   // native: no wrap
    auto* const heap = static_cast<rwResHeap*>(heapMemory);
    const RwUInt32 need = static_cast<RwUInt32>((size + kBlockAlign - 1) & ~(kBlockAlign - 1));
    rwResHeapBlockHeader* block = nullptr;
    for (rwResHeapBlockHeader* b = heap->firstFree; b != nullptr && block == nullptr; b = b->next) {
        if (isFree(b) && b->size >= need) block = b;
    }
    if (block == nullptr) return nullptr;
    if (block->size > std::uint64_t{need} + 2 * sizeof(rwResHeapBlockHeader)) {   // splitBlock (inlined)
        auto* const rest = reinterpret_cast<rwResHeapBlockHeader*>(reinterpret_cast<char*>(block) +
                                                                   sizeof(rwResHeapBlockHeader) + need);
        rwResHeapBlockHeader* const after = block->next;
        if (after != nullptr && isFree(after)) {   // the rest takes in the free block after it
            rest->next = after->next;
            rest->size = after->size + (block->size - need);
        } else {
            rest->next = after;
            rest->size = block->size - need - static_cast<RwUInt32>(sizeof(rwResHeapBlockHeader));
        }
        block->next = rest;
        rest->flags = 0;
        rest->prev = block;
        if (rest->next != nullptr) rest->next->prev = rest;
        block->size = need;
        rest->heap = block->heap;
    }
    if (block == heap->firstFree) {
        do {
            heap->firstFree = heap->firstFree->next;
        } while (heap->firstFree != nullptr && !isFree(heap->firstFree));
    }
    block->flags = rwRESHEAP_BLOCKUSED;
    return reinterpret_cast<char*>(block) + sizeof(rwResHeapBlockHeader);
}

void _rwResHeapFree(void* memory) {
    rwResHeapBlockHeader* block = blockOf(memory);
    block->flags = 0;
    rwResHeap* const heap = block->heap;
    rwResHeapBlockHeader* const prev = block->prev;
    rwResHeapBlockHeader* const next = block->next;
    if (heap->firstFree == nullptr || block < heap->firstFree) heap->firstFree = block;
    if (prev != nullptr && isFree(prev)) {
        prev->next = next;
        if (next != nullptr) next->prev = prev;
        prev->size += block->size + static_cast<RwUInt32>(sizeof(rwResHeapBlockHeader));
        block = prev;
    }
    if (next != nullptr && isFree(next)) {
        block->next = next->next;
        if (next->next != nullptr) next->next->prev = block;
        block->size += next->size + static_cast<RwUInt32>(sizeof(rwResHeapBlockHeader));
    }
}

// ---- baresour ------------------------------------------------------------------------------------------------------

void* _rwResourcesOpen(void* instance, RwInt32 offset, RwInt32 /*size*/) {
    resourcesModule.globalsOffset = offset;
    rwResources& res = rwResourcesArena();
    const RwUInt32 arenaSize = rwSrcGlobals().resArenaInitSize;
    if (arenaSize != 0) {
        res.memHeap = RwMalloc(arenaSize, kArenaHint);
        if (res.memHeap == nullptr) {
            RwError error{rwID_COREPLUGIN, _rwerror(E_RW_NOMEM, arenaSize)};
            RwErrorSet(&error);
            return nullptr;
        }
        if (!_rwResHeapInit(res.memHeap, arenaSize)) {
            RwFree(res.memHeap);
            res.memHeap = nullptr;   // native: the original leaves the freed pointer
            raise(E_RW_INSUFFICIENTRESOURCES, 0);
            return nullptr;
        }
    } else {
        res.memHeap = nullptr;
    }
    emptyList(res.entriesA);
    emptyList(res.entriesB);
    res.usedEntries = &res.entriesA;
    res.freeEntries = &res.entriesB;
    res.maxSize = static_cast<RwInt32>(arenaSize);
    res.currentSize = 0;
    res.reusageSize = 0;
    ++resourcesModule.numInstances;
    return instance;
}

void* _rwResourcesClose(void* instance, RwInt32 /*offset*/, RwInt32 /*size*/) {
    RwResourcesEmptyArena();
    rwResources& res = rwResourcesArena();
    if (res.memHeap != nullptr) {
        _rwResHeapClose(res.memHeap);
        RwFree(res.memHeap);
        res.memHeap = nullptr;
    }
    --resourcesModule.numInstances;
    return instance;
}

RwBool RwResourcesFreeResEntry(RwResEntry* entry) {
    if (entry->destroyNotify != nullptr) entry->destroyNotify(entry);
    if (entry->ownerRef != nullptr) *entry->ownerRef = nullptr;
    if (entry->link.next != nullptr) {
        entry->link.prev->next = entry->link.next;
        entry->link.next->prev = entry->link.prev;
        rwResourcesArena().currentSize -= entry->size;
        _rwResHeapFree(entry);
    } else {
        RwFree(entry);
    }
    return 1;
}

void _rwResourcesPurge() {
    if (resourcesModule.numInstances <= 0) return;   // native: no arena to purge
    rwResources& res = rwResourcesArena();
    RwLinkList* const wasFree = res.freeEntries;
    RwLinkList* const wasUsed = res.usedEntries;
    RwLLLink* const freeHead = &wasFree->link;
    RwLLLink* const usedHead = &wasUsed->link;
    if (freeHead->next != freeHead) {   // the free entries go after the used ones
        if (usedHead->next == usedHead) {
            usedHead->next = freeHead->next;
            usedHead->next->prev = usedHead;
            usedHead->prev = freeHead->prev;
            usedHead->prev->next = usedHead;
        } else {
            RwLLLink* const usedTail = usedHead->prev;
            usedTail->next = freeHead->next;
            freeHead->next->prev = usedTail;
            RwLLLink* const freeTail = freeHead->prev;
            freeTail->next = usedHead;
            usedHead->prev = freeTail;
        }
        emptyList(*wasFree);
    }
    res.usedEntries = wasFree;
    res.freeEntries = wasUsed;
    res.reusageSize = 0;
}

RwResEntry* RwResourcesAllocateResEntry(void* owner, RwResEntry** ownerRef, RwInt32 size,
                                        RwResEntryDestroyNotify destroyNotify) {
    rwResources& res = rwResourcesArena();
    if (size >= 0 && res.memHeap != nullptr) {   // native: see the header
        const std::size_t request = static_cast<std::size_t>(size) + sizeof(RwResEntry);
        for (;;) {
            if (auto* const entry = static_cast<RwResEntry*>(_rwResHeapAlloc(res.memHeap, request))) {
                RwLLLink* const head = &res.usedEntries->link;
                entry->link.next = head->next;
                entry->link.prev = head;
                head->next->prev = &entry->link;
                head->next = &entry->link;
                entry->owner = owner;
                entry->size = size;
                entry->ownerRef = ownerRef;
                entry->destroyNotify = destroyNotify;
                res.currentSize += size;
                if (ownerRef != nullptr) *ownerRef = entry;
                return entry;
            }
            RwLLLink* oldest = res.freeEntries->link.prev;
            if (oldest == &res.freeEntries->link) {
                oldest = res.usedEntries->link.prev;
                if (oldest == &res.usedEntries->link) break;   // the arena is empty and still too small
                res.reusageSize += reinterpret_cast<RwResEntry*>(oldest)->size;
            }
            RwResourcesFreeResEntry(reinterpret_cast<RwResEntry*>(oldest));
        }
    }
    if (ownerRef != nullptr) *ownerRef = nullptr;
    raise(E_RW_INSUFFICIENTRESOURCES, size);
    return nullptr;
}

RwBool RwResourcesEmptyArena() {
    rwResources& res = rwResourcesArena();
    for (RwLinkList* list : {&res.entriesA, &res.entriesB}) {
        RwLLLink* const head = &list->link;
        for (RwLLLink* link = head->next; link != head;) {
            RwLLLink* const next = link->next;
            RwResourcesFreeResEntry(reinterpret_cast<RwResEntry*>(link));
            link = next;
        }
    }
    emptyList(res.entriesA);
    emptyList(res.entriesB);
    res.reusageSize = 0;
    return 1;
}
