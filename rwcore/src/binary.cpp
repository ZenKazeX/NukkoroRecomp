// RenderWare binary chunks (see rw/core/binary.h).
#include "rw/core/binary.h"

#include <bit>
#include <cstring>

#include "rw/core/error.h"

namespace {

constexpr RwInt32 E_RW_READ = static_cast<RwInt32>(0x8000001Au);
constexpr RwInt32 E_RW_BADVERSION = static_cast<RwInt32>(0x80000004u);
constexpr RwUInt32 kHeaderSize = 12;

void binaryError(RwInt32 code) {
    RwError error{rwID_COREPLUGIN, _rwerror(code)};
    RwErrorSet(&error);
}

template <class Word> void swapWords(void* mem, RwUInt32 size) {
    if constexpr (std::endian::native == std::endian::big) {
        auto* bytes = static_cast<unsigned char*>(mem);
        for (RwUInt32 i = 0; i + sizeof(Word) <= size; i += sizeof(Word)) {
            for (std::size_t lo = 0, hi = sizeof(Word) - 1; lo < hi; ++lo, --hi) {
                const unsigned char b = bytes[i + lo];
                bytes[i + lo] = bytes[i + hi];
                bytes[i + hi] = b;
            }
        }
    } else {
        (void)mem, (void)size;
    }
}

/// The header's fields from its three little-endian words; the library ID unpacked: an old one (no high half) is
/// the version shifted by 8, build 0; else version 0x30000 + bits 14..23 and bits 16..21, build the low half.
RwChunkHeaderInfo decode(const RwUInt32 words[3]) {
    RwChunkHeaderInfo info{words[0], words[1], 0, 0, 0};
    const RwUInt32 id = words[2];
    if ((id & 0xFFFF0000u) == 0) {
        info.version = id << 8;
        info.buildNum = 0;
    } else {
        info.version = (((id >> 14) & 0x3FF00u) + 0x30000u) | ((id >> 16) & 0x3Fu);
        info.buildNum = id & 0xFFFFu;
    }
    info.isComplex = _rwChunkIsComplex(&info);
    return info;
}

/// Reads and decodes a header; false (E_RW_READ) when 12 bytes do not arrive.
bool readHeader(RwStream* stream, RwChunkHeaderInfo& info) {
    RwUInt32 words[3];
    if (RwStreamRead(stream, words, kHeaderSize) != kHeaderSize) {
        binaryError(E_RW_READ);
        return false;
    }
    RwMemNative32(words, kHeaderSize);
    info = decode(words);
    return true;
}

}  // namespace

void* RwMemNative32(void* mem, RwUInt32 size) {
    swapWords<RwUInt32>(mem, size);
    return mem;
}
void* RwMemNative16(void* mem, RwUInt32 size) {
    swapWords<RwUInt16>(mem, size);
    return mem;
}
void* RwMemLittleEndian32(void* mem, RwUInt32 size) {
    swapWords<RwUInt32>(mem, size);
    return mem;
}
void* RwMemLittleEndian16(void* mem, RwUInt32 size) {
    swapWords<RwUInt16>(mem, size);
    return mem;
}

RwBool _rwChunkIsComplex(const RwChunkHeaderInfo* header) {
    switch (header->type) {
        case rwID_CAMERA:
        case rwID_TEXTURE:
        case rwID_MATERIAL:
        case rwID_MATLIST:
        case rwID_ATOMICSECT:
        case rwID_PLANESECT:
        case rwID_WORLD:
        case rwID_FRAMELIST:
        case rwID_GEOMETRY:
        case rwID_CLUMP:
        case rwID_LIGHT:
        case rwID_ATOMIC:
        case rwID_GEOMETRYLIST:
            return 1;
        default:
            return 0;
    }
}

RwBool _rwStreamReadChunkHeader(RwStream* stream, RwUInt32* type, RwUInt32* length, RwUInt32* version,
                                RwUInt32* buildNum) {
    RwChunkHeaderInfo info;
    if (!readHeader(stream, info)) return 0;
    if (type != nullptr) *type = info.type;
    if (length != nullptr) *length = info.length;
    if (buildNum != nullptr) *buildNum = info.buildNum;
    if (version != nullptr) *version = info.version;
    return 1;
}

RwStream* _rwStreamWriteVersionedChunkHeader(RwStream* stream, RwInt32 type, RwInt32 size, RwUInt32 version,
                                             RwUInt32 buildNum) {
    RwUInt32 words[3] = {static_cast<RwUInt32>(type), static_cast<RwUInt32>(size),
                         (((version - 0x30000u) << 14) & 0xFFC00000u) | ((version & 0x3Fu) << 16) | (buildNum & 0xFFFFu)};
    RwMemLittleEndian32(words, kHeaderSize);
    return RwStreamWrite(stream, words, kHeaderSize);
}

RwBool RwStreamFindChunk(RwStream* stream, RwUInt32 type, RwUInt32* lengthOut, RwUInt32* versionOut) {
    for (;;) {
        RwChunkHeaderInfo info;
        if (!readHeader(stream, info)) return 0;
        if (info.type == type) {
            if (info.version < rwLIBRARYBASEVERSION || info.version > rwLIBRARYCURRENTVERSION) {
                binaryError(E_RW_BADVERSION);
                return 0;
            }
            if (lengthOut != nullptr) *lengthOut = info.length;
            if (versionOut != nullptr) *versionOut = info.version;
            return 1;
        }
        if (RwStreamSkip(stream, info.length) == nullptr) return 0;
    }
}

namespace {

/// The writers' shared body: little-endian on the host, the data is written as it is; else it goes out in 256-byte
/// swapped copies, as the original does it.
template <class Word> RwStream* writeWords(RwStream* stream, const void* data, RwUInt32 numBytes) {
    if constexpr (std::endian::native == std::endian::little) {
        if (numBytes == 0) return stream;
        return RwStreamWrite(stream, data, numBytes) != nullptr ? stream : nullptr;
    } else {
        const auto* bytes = static_cast<const unsigned char*>(data);
        unsigned char buffer[256];
        while (numBytes != 0) {
            const RwUInt32 n = numBytes < sizeof buffer ? numBytes : static_cast<RwUInt32>(sizeof buffer);
            std::memcpy(buffer, bytes, n);
            swapWords<Word>(buffer, n);
            if (RwStreamWrite(stream, buffer, n) == nullptr) return nullptr;
            numBytes -= n;
            bytes += n;
        }
        return stream;
    }
}

}  // namespace

RwStream* RwStreamWriteInt32(RwStream* stream, const RwInt32* data, RwUInt32 numBytes) {
    return writeWords<RwUInt32>(stream, data, numBytes);
}
RwStream* RwStreamWriteReal(RwStream* stream, const RwReal* data, RwUInt32 numBytes) {
    return writeWords<RwUInt32>(stream, data, numBytes);   // RwMemRealToFloat32 is the identity (IEEE single)
}
RwStream* RwStreamWriteInt16(RwStream* stream, const RwInt16* data, RwUInt32 numBytes) {
    return writeWords<RwUInt16>(stream, data, numBytes);
}

RwStream* RwStreamReadInt16(RwStream* stream, RwInt16* data, RwUInt32 numBytes) {
    if (RwStreamRead(stream, data, numBytes) == 0) {
        binaryError(E_RW_READ);
        return nullptr;
    }
    RwMemNative16(data, numBytes);
    return stream;
}

RwStream* RwStreamReadReal(RwStream* stream, RwReal* data, RwUInt32 numBytes) {
    if (RwStreamRead(stream, data, numBytes) == 0) {
        binaryError(E_RW_READ);
        return nullptr;
    }
    RwMemNative32(data, numBytes);   // RwMemFloat32ToReal is the identity: RwReal is the stream's IEEE single
    return stream;
}

RwStream* RwStreamReadInt32(RwStream* stream, RwInt32* data, RwUInt32 numBytes) {
    if (RwStreamRead(stream, data, numBytes) == 0) {
        binaryError(E_RW_READ);
        return nullptr;
    }
    RwMemNative32(data, numBytes);
    return stream;
}

RwStream* RwStreamReadChunkHeaderInfo(RwStream* stream, RwChunkHeaderInfo* headerInfo) {
    RwChunkHeaderInfo info;
    if (!readHeader(stream, info)) return nullptr;
    *headerInfo = info;
    return stream;
}
