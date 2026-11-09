// RenderWare growable arrays (see rw/core/list.h).
#include "rw/core/list.h"

#include "rw/core/error.h"

namespace {

constexpr RwUInt32 kHeaderHint = 0x00030000u;      // rwMEMHINTDUR_EVENT, as the original passes it
constexpr RwUInt32 kResizeable = 0x01000000u;      // rwMEMHINTFLAG_RESIZEABLE
constexpr RwInt32 kInitialEntries = 20;

std::size_t bytes(RwInt32 entrySize, RwInt32 count) {
    return static_cast<std::size_t>(entrySize) * static_cast<std::size_t>(count);
}

void noMemory(std::size_t size) {
    RwError error{rwID_COREPLUGIN, _rwerror(E_RW_NOMEM, static_cast<RwInt32>(size))};
    RwErrorSet(&error);
}

/// The capacity after growing alloced by extra, or -1 past RwInt32 (the counts are RenderWare's RwInt32; the
/// original wraps there).
RwInt32 grown(RwInt32 alloced, RwInt64 extra) {
    const RwInt64 n = RwInt64{alloced} + extra;
    return n > 0x7FFFFFFF ? -1 : static_cast<RwInt32>(n);
}

/// Grows sList to newAlloced entries; FALSE (E_RW_NOMEM set) when RwRealloc fails or the count overflows.
bool grow(RwSList* sList, RwInt32 newAlloced, RwUInt32 hint) {
    if (newAlloced < 0) {
        noMemory(~std::size_t{0});
        return false;
    }
    const std::size_t size = bytes(sList->entrySize, newAlloced);
    void* elements = RwRealloc(sList->listElements, size, hint);
    if (elements == nullptr) {
        noMemory(size);
        return false;
    }
    sList->listElements = static_cast<RwUInt8*>(elements);
    sList->numElementsAlloced = newAlloced;
    return true;
}

}  // namespace

RwSList* _rwSListCreate(RwInt32 size, RwUInt32 hint) {
    auto* sList = static_cast<RwSList*>(RwMalloc(sizeof(RwSList), kHeaderHint));
    if (sList == nullptr) {
        noMemory(sizeof(RwSList));
        return nullptr;
    }
    sList->numElementsFilled = 0;
    sList->numElementsAlloced = kInitialEntries;
    sList->entrySize = size;
    sList->listElements = static_cast<RwUInt8*>(RwMalloc(bytes(size, kInitialEntries), hint | kResizeable));
    if (sList->listElements == nullptr) {
        noMemory(bytes(size, kInitialEntries));
        RwFree(sList);
        return nullptr;
    }
    return sList;
}

RwSList* _rwSListEmpty(RwSList* sList) {
    sList->numElementsFilled = 0;
    return sList;
}

void* _rwSListGetNewEntry(RwSList* sList, RwUInt32 hint) {
    if (sList->numElementsFilled >= sList->numElementsAlloced &&
        !grow(sList, grown(sList->numElementsAlloced, sList->numElementsAlloced / 4), hint)) {
        return nullptr;
    }
    void* entry = sList->listElements + bytes(sList->entrySize, sList->numElementsFilled);
    ++sList->numElementsFilled;
    return entry;
}

void* _rwSListGetNewEntries(RwSList* sList, RwInt32 num, RwUInt32 hint) {
    if (RwInt64{sList->numElementsFilled} + num >= sList->numElementsAlloced &&
        !grow(sList, grown(sList->numElementsAlloced, RwInt64{num} + sList->numElementsAlloced / 4), hint)) {
        return nullptr;
    }
    void* entries = sList->listElements + bytes(sList->entrySize, sList->numElementsFilled);
    sList->numElementsFilled += num;
    return entries;
}

void _rwSListDestroyEndEntries(RwSList* sList, RwInt32 numEntries) { sList->numElementsFilled -= numEntries; }

void* _rwSListGetArray(RwSList* sList) { return sList->numElementsFilled != 0 ? sList->listElements : nullptr; }

RwBool _rwSListDestroy(RwSList* sList) {
    if (sList->listElements != nullptr && sList->numElementsAlloced != 0) {
        RwFree(sList->listElements);
        sList->listElements = nullptr;
        sList->numElementsAlloced = 0;
    }
    RwFree(sList);
    return 1;
}

RwInt32 _rwSListGetNumEntries(const RwSList* sList) { return sList->numElementsFilled; }

void* _rwSListToArray(RwSList* sList) {
    void* array = nullptr;
    if (sList->numElementsFilled != 0) {
        array = sList->listElements;
    } else if (sList->listElements != nullptr) {
        RwFree(sList->listElements);
        sList->listElements = nullptr;
    }
    RwFree(sList);
    return array;
}

void* _rwSListGetEntry(RwSList* sList, RwInt32 entry) { return sList->listElements + bytes(sList->entrySize, entry); }

void* _rwSListGetBegin(RwSList* sList) { return sList->listElements; }

void* _rwSListGetEnd(RwSList* sList) {
    return sList->listElements + bytes(sList->entrySize, sList->numElementsFilled);
}
