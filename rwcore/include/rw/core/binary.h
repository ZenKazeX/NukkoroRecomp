// RenderWare Graphics 3.7 core: binary chunks (module babinary), native x64.
//
// RenderWare's binary format is a tree of chunks, each with a 12-byte little-endian header {type, length,
// library ID}; the library ID packs the RenderWare version and build. The originals are PAL main.dol
// 0x8047DD48..0x8047EB5C; proof: runtime/tests/rwcore_binary_tests.cpp.
//
// Byte order: the stream format is little-endian. The GameCube swaps every word it reads or writes (RwMemNative32 /
// RwMemLittleEndian32 are stwbrx loops there); on a little-endian host the same functions leave memory as it is, and
// values compare equal to the original's (the oracle compares the integers, not the bytes in memory).
// The writers: the GameCube copies each 256 bytes to a stack buffer, swaps it to little-endian and writes it; on a
// little-endian host the data already is the stream's, so it is written in one call (the same bytes; a memory stream
// may then grow in one step where the original grows per 256 bytes - capacity only, not data).
#pragma once

#include "rw/core/stream.h"
#include "rw/core/types.h"

/// A chunk header as RwStreamReadChunkHeaderInfo returns it (rwplcore.h RwChunkHeaderInfo).
struct RwChunkHeaderInfo {
    RwUInt32 type;
    RwUInt32 length;
    RwUInt32 version;
    RwUInt32 buildNum;
    RwBool isComplex;
};

/// The chunk IDs of the core and world objects (rwplcore.h RwCorePluginID; the ones babinary tells apart).
enum RwCorePluginID : RwUInt32 {
    rwID_NAOBJECT = 0x00,
    rwID_STRUCT = 0x01,
    rwID_STRING = 0x02,
    rwID_EXTENSION = 0x03,
    rwID_CAMERA = 0x05,
    rwID_TEXTURE = 0x06,
    rwID_MATERIAL = 0x07,
    rwID_MATLIST = 0x08,
    rwID_ATOMICSECT = 0x09,
    rwID_PLANESECT = 0x0A,
    rwID_WORLD = 0x0B,
    rwID_SPLINE = 0x0C,
    rwID_MATRIX = 0x0D,
    rwID_FRAMELIST = 0x0E,
    rwID_GEOMETRY = 0x0F,
    rwID_CLUMP = 0x10,
    rwID_LIGHT = 0x12,
    rwID_UNICODESTRING = 0x13,
    rwID_ATOMIC = 0x14,
    rwID_TEXTURENATIVE = 0x15,
    rwID_TEXDICTIONARY = 0x16,
    rwID_ANIMDATABASE = 0x17,
    rwID_IMAGE = 0x18,
    rwID_SKINANIMATION = 0x19,
    rwID_GEOMETRYLIST = 0x1A,
};

/// The versions RwStreamFindChunk accepts (3.5.0.0 to 3.7.0.2, as the binary checks them).
inline constexpr RwUInt32 rwLIBRARYBASEVERSION = 0x35000u;
inline constexpr RwUInt32 rwLIBRARYCURRENTVERSION = 0x37002u;

/// Converts size bytes of little-endian 32-bit / 16-bit words at mem to host order, in place (no-op on a
/// little-endian host). Returns mem. Originals: 0x8047E2A4, 0x8047E314.
void* RwMemNative32(void* mem, RwUInt32 size);
void* RwMemNative16(void* mem, RwUInt32 size);

/// Converts size bytes of host-order 32-bit / 16-bit words at mem to little-endian, in place. Returns mem.
/// Originals: 0x8047E1A8, 0x8047E218.
void* RwMemLittleEndian32(void* mem, RwUInt32 size);
void* RwMemLittleEndian16(void* mem, RwUInt32 size);

/// Whether a chunk of header->type holds other chunks (the original's table: camera, texture, material, matlist,
/// atomic / plane sector, world, frame list, geometry, clump, light, atomic, geometry list). Original: 0x8047DD48.
RwBool _rwChunkIsComplex(const RwChunkHeaderInfo* header);

/// Reads a chunk header; each non-NULL output gets its field. FALSE when 12 bytes cannot be read (E_RW_READ).
/// Original: 0x8047DE04.
RwBool _rwStreamReadChunkHeader(RwStream* stream, RwUInt32* type, RwUInt32* length, RwUInt32* version,
                                RwUInt32* buildNum);

/// Writes a chunk header with version and buildNum packed into the library ID. Returns RwStreamWrite's result.
/// Original: 0x8047DF40.
RwStream* _rwStreamWriteVersionedChunkHeader(RwStream* stream, RwInt32 type, RwInt32 size, RwUInt32 version,
                                             RwUInt32 buildNum);

/// Skips chunks until one of type; its length and version go to the non-NULL outputs. FALSE at a read failure
/// (E_RW_READ), a failed skip, or a version outside rwLIBRARYBASEVERSION..rwLIBRARYCURRENTVERSION
/// (E_RW_BADVERSION). Original: 0x8047DFA0.
RwBool RwStreamFindChunk(RwStream* stream, RwUInt32 type, RwUInt32* lengthOut, RwUInt32* versionOut);

/// Reads numBytes of 16-bit / real / 32-bit values into data and converts them to host order. NULL only when no
/// byte at all can be read (E_RW_READ): a short read still converts all numBytes, as the original does. Returns
/// stream. Originals: 0x8047E6D4, 0x8047E7D4, 0x8047E8B8.
RwStream* RwStreamReadInt16(RwStream* stream, RwInt16* data, RwUInt32 numBytes);
RwStream* RwStreamReadReal(RwStream* stream, RwReal* data, RwUInt32 numBytes);
RwStream* RwStreamReadInt32(RwStream* stream, RwInt32* data, RwUInt32 numBytes);

/// Writes numBytes of 32-bit / real / 16-bit values from data as little-endian. Returns stream, NULL when a write
/// fails (E_RW_WRITE / E_RW_NOMEM from RwStreamWrite). Originals: 0x8047E4A8, 0x8047E3A0, 0x8047E5B0.
RwStream* RwStreamWriteInt32(RwStream* stream, const RwInt32* data, RwUInt32 numBytes);
RwStream* RwStreamWriteReal(RwStream* stream, const RwReal* data, RwUInt32 numBytes);
RwStream* RwStreamWriteInt16(RwStream* stream, const RwInt16* data, RwUInt32 numBytes);

/// Reads a chunk header into headerInfo. NULL when 12 bytes cannot be read (E_RW_READ). Original: 0x8047E99C.
RwStream* RwStreamReadChunkHeaderInfo(RwStream* stream, RwChunkHeaderInfo* headerInfo);

/// The next string chunk's characters (skipping other chunks): rwID_STRING (2) bytes as they are, rwID_UNICODESTRING
/// (0x13) 16-bit little-endian units narrowed to their low byte, as RenderWare does on a little-endian host (the
/// GameCube's build loads each unit big-endian and keeps the wrong byte). With string == NULL a buffer of the chunk's
/// length is allocated (hint rwMEMHINTDUR_EVENT | rwID_STRING; the caller frees it). NULL at a header or read failure,
/// a version outside 3.5.0.0 .. 3.7.0.2 (E_RW_BADVERSION) or no memory (E_RW_NOMEM); native: a buffer it allocated is
/// freed then (the original leaks it on an ASCII read failure). The chunk's whole length is copied, as in the original:
/// a caller's buffer must hold it - _rwStringStreamFindAndReadCapped is the bounded form. Original: 0x804840B0.
RwChar* _rwStringStreamFindAndRead(RwChar* string, RwStream* stream);

/// The same into a caller's buffer of capacity bytes (>= 1): at most capacity - 1 characters, the rest of the chunk
/// skipped, always NUL-terminated after what was read (native: the original's callers pass 128-byte stack buffers and
/// read the whole chunk into them).
RwChar* _rwStringStreamFindAndReadCapped(RwChar* string, std::size_t capacity, RwStream* stream);
