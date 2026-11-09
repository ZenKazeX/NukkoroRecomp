// RenderWare Graphics 3.7 core: growable arrays (module balist, RwSList), native x64.
//
// An RwSList is an array of fixed-size entries that grows by RwRealloc (by a quarter of its size, or to fit a run of
// new entries). The originals are PAL main.dol 0x8047ECE0..0x8047F14C; proof: runtime/tests/rwcore_list_tests.cpp.
// 64-bit: counts keep RenderWare's RwInt32; byte sizes are computed in size_t (the original's 32-bit products would
// wrap past 2 GiB).
#pragma once

#include "rw/core/engine.h"
#include "rw/core/types.h"

/// A growable array (RenderWare's RwSList, internal).
struct RwSList {
    RwUInt8* listElements;
    RwInt32 numElementsFilled;
    RwInt32 numElementsAlloced;
    RwInt32 entrySize;
};

/// A list of entrySize-byte entries with room for 20 (RwMalloc: the header with hint 0x30000, the entries with hint |
/// rwMEMHINTFLAG_RESIZEABLE). NULL when an allocation fails (E_RW_NOMEM is set). Original: 0x8047ECE0.
RwSList* _rwSListCreate(RwInt32 size, RwUInt32 hint);

/// No entries (the memory stays). Returns list. Original: 0x8047EE00.
RwSList* _rwSListEmpty(RwSList* sList);

/// A new entry at the end, growing by a quarter when full. NULL when RwRealloc fails (E_RW_NOMEM). Original: 0x8047EE0C.
void* _rwSListGetNewEntry(RwSList* sList, RwUInt32 hint);

/// num new entries at the end (the first returned). NULL when RwRealloc fails. Original: 0x8047EEF0.
void* _rwSListGetNewEntries(RwSList* sList, RwInt32 num, RwUInt32 hint);

/// Drops the last numEntries entries. Original: 0x8047EFEC.
void _rwSListDestroyEndEntries(RwSList* sList, RwInt32 numEntries);

/// The entries, or NULL when there are none. Original: 0x8047EFFC.
void* _rwSListGetArray(RwSList* sList);

/// Frees the list and its entries. Returns TRUE. Original: 0x8047F018.
RwBool _rwSListDestroy(RwSList* sList);

/// The number of entries. Original: 0x8047F08C.
RwInt32 _rwSListGetNumEntries(const RwSList* sList);

/// Frees the list and hands over its entries (NULL, freed, when there are none). Original: 0x8047F094.
void* _rwSListToArray(RwSList* sList);

/// Entry entry (no bounds check). Original: 0x8047F11C.
void* _rwSListGetEntry(RwSList* sList, RwInt32 entry);

/// The first entry / one past the last. Originals: 0x8047F130, 0x8047F138.
void* _rwSListGetBegin(RwSList* sList);
void* _rwSListGetEnd(RwSList* sList);
