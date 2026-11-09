// RenderWare plugin registry (see rw/core/plugin.h).
#include "rw/core/plugin.h"

#include "rw/core/error.h"
#include "rw/core/memory.h"

namespace {

// The entry free list's creation parameters (batkreg statics 0x805EEF70 / 0x805EEF74;
// RwPluginRegistrySetFreeListCreateParams, which would change them, is not linked) and its in-place header
// (0x805E3B90).
constexpr RwInt32 kEntriesPerBlock = 64;
constexpr RwInt32 kPreallocBlocks = 1;
RwFreeList regEntryFreeListSpace{};
RwFreeList* regEntryFreeList = nullptr;

// With an application's own memoryAlloc the entries are not in the free list, so the registries that hold entries
// are remembered to free them at close (GameCube 0x805F2630 / 0x805F2634; descriptive names, the originals are
// static). The count is reset by _rwPluginRegistryOpen only, as in the original.
RwPluginRegistry** allocatedRegistries = nullptr;
RwUInt32 numAllocatedRegistries = 0;

bool usesFreeLists() { return rwSrcGlobals().memoryAlloc == _rwFreeListAllocReal; }

void pluginInitError() {
    RwError error{rwID_COREPLUGIN, _rwerror(E_RW_PLUGININIT)};
    RwErrorSet(&error);
}

void restore(RwPluginRegistry* reg) {
    reg->sizeOfStruct = reg->origSizeOfStruct;
    reg->firstRegEntry = nullptr;
    reg->lastRegEntry = nullptr;
}

// The callbacks a plugin gets when it gives none: they return the object (0x80482DDC / 0x80482DE0 / 0x80482DE4).
void* PluginDefaultConstructor(void* object, RwInt32, RwInt32) { return object; }
void* PluginDefaultDestructor(void* object, RwInt32, RwInt32) { return object; }
void* PluginDefaultCopy(void* dstObject, const void*, RwInt32, RwInt32) { return dstObject; }

/// RwFreeListForAllUsed callback: returns the entry's registry to its object's size and frees the entry into the free
/// list in data. Original: 0x80482C44.
void rwDestroyEntry(void* pMem, void* pData) {
    auto* entry = static_cast<RwPluginRegEntry*>(pMem);
    RwPluginRegistry* reg = entry->parentRegistry;
    if (reg->firstRegEntry != nullptr) restore(reg);
    rwSrcGlobals().memoryFree(static_cast<RwFreeList*>(pData), entry);
}

/// Remembers reg among the registries with entries (first time only). false when out of memory: the original does
/// not test its RwMalloc and writes through NULL.
bool rememberRegistry(RwPluginRegistry* reg) {
    for (RwUInt32 i = 0; i < numAllocatedRegistries; ++i) {
        if (allocatedRegistries[i] == reg) return true;
    }
    const std::size_t bytes = sizeof(RwPluginRegistry*) * (std::size_t{numAllocatedRegistries} + 1);
    auto* registries = static_cast<RwPluginRegistry**>(RwMalloc(bytes, rwMEMHINTDUR_GLOBAL));
    if (registries == nullptr) {
        RwError error{rwID_COREPLUGIN, _rwerror(E_RW_NOMEM, static_cast<RwInt32>(bytes))};
        RwErrorSet(&error);
        return false;
    }
    RwUInt32 count = 0;
    if (allocatedRegistries != nullptr) {
        for (; count < numAllocatedRegistries; ++count) registries[count] = allocatedRegistries[count];
        RwFree(allocatedRegistries);
        allocatedRegistries = nullptr;
    }
    registries[count] = reg;
    allocatedRegistries = registries;
    ++numAllocatedRegistries;
    return true;
}

}  // namespace

RwBool _rwPluginRegistryOpen() {
    regEntryFreeList = RwFreeListCreateAndPreallocateSpace(sizeof(RwPluginRegEntry), kEntriesPerBlock,
                                                           alignof(RwPluginRegEntry), kPreallocBlocks,
                                                           &regEntryFreeListSpace, rwMEMHINTDUR_GLOBAL);
    if (regEntryFreeList == nullptr) return 0;
    numAllocatedRegistries = 0;
    return 1;
}

RwBool _rwPluginRegistryClose() {
    if (regEntryFreeList == nullptr) return 1;
    RwFreeListForAllUsed(regEntryFreeList, rwDestroyEntry, regEntryFreeList);
    if (!usesFreeLists()) {
        for (RwUInt32 i = 0; i < numAllocatedRegistries; ++i) {
            RwPluginRegEntry* entry = allocatedRegistries[i]->firstRegEntry;
            RwPluginRegistry* reg = entry != nullptr ? entry->parentRegistry : nullptr;
            while (entry != nullptr) {
                RwPluginRegEntry* next = entry->nextRegEntry;
                rwSrcGlobals().memoryFree(nullptr, entry);
                entry = next;
            }
            if (reg != nullptr && reg->firstRegEntry != nullptr) restore(reg);
        }
        if (allocatedRegistries != nullptr) {
            RwFree(allocatedRegistries);
            allocatedRegistries = nullptr;
        }
    }
    RwFreeListDestroy(regEntryFreeList);
    regEntryFreeList = nullptr;
    return 1;
}

RwInt32 _rwPluginRegistryGetPluginOffset(const RwPluginRegistry* reg, RwUInt32 pluginID) {
    for (const RwPluginRegEntry* entry = reg->firstRegEntry; entry != nullptr; entry = entry->nextRegEntry) {
        if (entry->pluginID == pluginID) return entry->offset;
    }
    return -1;
}

RwInt32 _rwPluginRegistryAddPlugin(RwPluginRegistry* reg, RwInt32 size, RwUInt32 pluginID,
                                   RwPluginObjectConstructor constructCB, RwPluginObjectDestructor destructCB,
                                   RwPluginObjectCopy copyCB) {
    if (regEntryFreeList == nullptr) return -1;
    if (_rwGetNumEngineInstances() != 0) {
        pluginInitError();
        return -1;
    }
    if (!usesFreeLists() && !rememberRegistry(reg)) return -1;
    for (const RwPluginRegEntry* entry = reg->firstRegEntry; entry != nullptr; entry = entry->nextRegEntry) {
        if (entry->pluginID == pluginID) {
            pluginInitError();
            return entry->offset;
        }
    }
    // native contract: a slice has a size >= 0 and every offset fits RenderWare's RwInt32 (the original wraps)
    constexpr RwInt64 kAlign = rwPLUGINDATAALIGNMENT;
    const RwInt64 offset64 = (RwInt64{reg->sizeOfStruct} + kAlign - 1) & ~(kAlign - 1);
    const RwInt64 newSize64 = offset64 + ((RwInt64{size} + kAlign - 1) & ~(kAlign - 1));
    if (size < 0 || newSize64 > 0x7FFFFFFF) return -1;
    const auto offset = static_cast<RwInt32>(offset64);
    const auto newSize = static_cast<RwInt32>(newSize64);
    if (reg->maxSizeOfStruct != 0 && newSize > reg->maxSizeOfStruct) return -1;
    auto* entry = static_cast<RwPluginRegEntry*>(rwSrcGlobals().memoryAlloc(regEntryFreeList, rwMEMHINTDUR_GLOBAL));
    if (entry == nullptr) return -1;
    entry->offset = offset;
    reg->sizeOfStruct = newSize;
    entry->size = size;
    entry->pluginID = pluginID;
    entry->readCB = nullptr;
    entry->writeCB = nullptr;
    entry->getSizeCB = nullptr;
    entry->alwaysCB = nullptr;
    entry->rightsCB = nullptr;
    entry->constructCB = constructCB != nullptr ? constructCB : PluginDefaultConstructor;
    entry->destructCB = destructCB != nullptr ? destructCB : PluginDefaultDestructor;
    entry->copyCB = copyCB != nullptr ? copyCB : PluginDefaultCopy;
    entry->errStrCB = nullptr;
    entry->nextRegEntry = nullptr;
    entry->prevRegEntry = nullptr;
    entry->parentRegistry = reg;
    if (reg->firstRegEntry == nullptr) {
        reg->firstRegEntry = entry;
        reg->lastRegEntry = entry;
    } else {
        reg->lastRegEntry->nextRegEntry = entry;
        entry->prevRegEntry = reg->lastRegEntry;
        reg->lastRegEntry = entry;
    }
    return entry->offset;
}

const RwPluginRegistry* _rwPluginRegistryInitObject(const RwPluginRegistry* reg, void* object) {
    for (const RwPluginRegEntry* entry = reg->firstRegEntry; entry != nullptr; entry = entry->nextRegEntry) {
        if (entry->constructCB(object, entry->offset, entry->size) != nullptr) continue;
        for (entry = entry->prevRegEntry; entry != nullptr; entry = entry->prevRegEntry) {
            entry->destructCB(object, entry->offset, entry->size);
        }
        return nullptr;
    }
    return reg;
}

const RwPluginRegistry* _rwPluginRegistryDeInitObject(const RwPluginRegistry* reg, void* object) {
    for (const RwPluginRegEntry* entry = reg->lastRegEntry; entry != nullptr; entry = entry->prevRegEntry) {
        entry->destructCB(object, entry->offset, entry->size);
    }
    return reg;
}

const RwPluginRegistry* _rwPluginRegistryCopyObject(const RwPluginRegistry* reg, void* dstObject,
                                                    const void* srcObject) {
    for (const RwPluginRegEntry* entry = reg->firstRegEntry; entry != nullptr; entry = entry->nextRegEntry) {
        entry->copyCB(dstObject, srcObject, entry->offset, entry->size);
    }
    return reg;
}
