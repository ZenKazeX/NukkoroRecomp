// RenderWare Graphics 3.7 base types (rwplcore.h), native x64.
//
// Names are RenderWare's. Widths: RenderWare's fixed-size types keep their size (RwInt32 is 32 bits on every target,
// as in the binary stream format); pointers and byte counts are native (64-bit pointers, size_t sizes), so nothing in
// the core limits the address space to 32 bits (rwcore/README.md "64-bit model").
#pragma once

#include <cstddef>
#include <cstdint>

using RwInt8 = std::int8_t;
using RwUInt8 = std::uint8_t;
using RwInt16 = std::int16_t;
using RwUInt16 = std::uint16_t;
using RwInt32 = std::int32_t;
using RwUInt32 = std::uint32_t;
using RwInt64 = std::int64_t;
using RwUInt64 = std::uint64_t;
using RwChar = char;
using RwReal = float;
using RwBool = RwInt32;  ///< RenderWare's boolean: 0 = FALSE, anything else TRUE

/// A 2D point or vector.
struct RwV2d {
    RwReal x;
    RwReal y;
};

/// A 3D point or vector.
struct RwV3d {
    RwReal x;
    RwReal y;
    RwReal z;
};

/// A colour of four reals (rwplcore.h RwRGBAReal).
struct RwRGBAReal {
    RwReal red;
    RwReal green;
    RwReal blue;
    RwReal alpha;
};

/// A link of RenderWare's intrusive doubly linked lists (RwLinkList). Native pointers.
struct RwLLLink {
    RwLLLink* next;
    RwLLLink* prev;
};

/// RenderWare's intrusive doubly linked list: a circular list whose head link is the list itself.
struct RwLinkList {
    RwLLLink link;
};
