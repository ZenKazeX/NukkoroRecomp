// RenderWare streams (see rw/core/stream.h).
#include "rw/core/stream.h"

#include <cstdio>
#include <cstring>

#include "rw/core/error.h"
#include "rw/core/memory.h"

RwModuleInfo streamModule{};

namespace {

// The free list's creation parameters (bastream statics 0x805EEF58 / 0x805EEF5C; RwStreamSetFreeListCreateParams,
// which would change them, is not linked) and its in-place header (0x805E3B68).
constexpr RwInt32 kStreamsPerBlock = 16;
constexpr RwInt32 kPreallocBlocks = 1;
constexpr RwUInt32 rwID_STREAMMODULE = 0x00000404u;
constexpr RwUInt32 kEventHint = 0x00030000u | rwID_STREAMMODULE;          // rwMEMHINTDUR_EVENT
constexpr RwUInt32 kResizeable = 0x01000000u;                             // rwMEMHINTFLAG_RESIZEABLE
constexpr std::size_t kMemoryGrowth = 0x200;
constexpr RwInt32 kSeekCur = 1;                                           // SEEK_CUR as RwFseek takes it
RwFreeList streamFreeListSpace{};

RwFreeList*& streamFreeList() { return rwModuleGlobals<rwStreamGlobals>(streamModule).streamFreeList; }

void streamError(RwInt32 code) {
    RwError error{rwID_COREPLUGIN, _rwerror(code)};
    RwErrorSet(&error);
}

constexpr RwInt32 E_RW_BADOPEN = static_cast<RwInt32>(0x80000002u);
constexpr RwInt32 E_RW_READ = static_cast<RwInt32>(0x8000001Au);
constexpr RwInt32 E_RW_WRITE = static_cast<RwInt32>(0x8000001Cu);
constexpr RwInt32 E_RW_ENDOFSTREAM = 0x05;
constexpr RwInt32 E_RW_INVSTREAMACCESSTYPE = 0x0D;
constexpr RwInt32 E_RW_INVSTREAMTYPE = 0x0E;

// ---- the host's C files ---------------------------------------------------------------------------------------
void* hostFopen(const RwChar* name, const RwChar* access) {
#if defined(_MSC_VER)
    std::FILE* file = nullptr;
    return fopen_s(&file, name, access) == 0 ? file : nullptr;
#else
    return std::fopen(name, access);
#endif
}
RwInt32 hostFclose(void* fptr) { return std::fclose(static_cast<std::FILE*>(fptr)); }
std::size_t hostFread(void* addr, std::size_t size, std::size_t count, void* fptr) {
    return std::fread(addr, size, count, static_cast<std::FILE*>(fptr));
}
std::size_t hostFwrite(const void* addr, std::size_t size, std::size_t count, void* fptr) {
    return std::fwrite(addr, size, count, static_cast<std::FILE*>(fptr));
}
RwInt32 hostFseek(void* fptr, RwInt64 offset, RwInt32 origin) {
#if defined(_MSC_VER)
    return _fseeki64(static_cast<std::FILE*>(fptr), offset, origin);
#else
    return fseeko(static_cast<std::FILE*>(fptr), static_cast<off_t>(offset), origin);
#endif
}
RwInt32 hostFeof(void* fptr) { return std::feof(static_cast<std::FILE*>(fptr)); }
RwInt64 hostFtell(void* fptr) {
#if defined(_MSC_VER)
    return _ftelli64(static_cast<std::FILE*>(fptr));
#else
    return static_cast<RwInt64>(ftello(static_cast<std::FILE*>(fptr)));
#endif
}
RwBool hostFexist(const RwChar* name) {
    void* file = hostFopen(name, "rb");
    if (file == nullptr) return 0;
    hostFclose(file);
    return 1;
}

RwFileInterface g_files = rwHostFileInterface();

}  // namespace

RwFileInterface rwHostFileInterface() {
    return {hostFopen, hostFclose, hostFread, hostFwrite, hostFseek, hostFeof, hostFtell, hostFexist};
}
RwFileInterface& rwFileInterface() { return g_files; }

void* RwFopen(const RwChar* name, const RwChar* access) { return g_files.fopen(name, access); }
RwInt32 RwFclose(void* fptr) { return g_files.fclose(fptr); }
std::size_t RwFread(void* addr, std::size_t size, std::size_t count, void* fptr) {
    return g_files.fread(addr, size, count, fptr);
}
std::size_t RwFwrite(const void* addr, std::size_t size, std::size_t count, void* fptr) {
    return g_files.fwrite(addr, size, count, fptr);
}
RwInt32 RwFseek(void* fptr, RwInt64 offset, RwInt32 origin) { return g_files.fseek(fptr, offset, origin); }
RwInt32 RwFeof(void* fptr) { return g_files.feof(fptr); }
RwInt64 RwFtell(void* fptr) { return g_files.ftell(fptr); }
RwBool RwFexist(const RwChar* name) {
    if (g_files.fexist != nullptr) return g_files.fexist(name);
    void* file = g_files.fopen(name, "rb");
    if (file == nullptr) return 0;
    g_files.fclose(file);
    return 1;
}

void* _rwStreamModuleOpen(void* instance, RwInt32 offset, RwInt32 /*size*/) {
    streamModule.globalsOffset = offset;
    streamFreeList() = RwFreeListCreateAndPreallocateSpace(sizeof(RwStream), kStreamsPerBlock, alignof(RwStream),
                                                           kPreallocBlocks, &streamFreeListSpace,
                                                           rwMEMHINTDUR_GLOBAL | rwID_STREAMMODULE);
    if (streamFreeList() == nullptr) return nullptr;
    ++streamModule.numInstances;
    return instance;
}

void* _rwStreamModuleClose(void* instance, RwInt32 /*offset*/, RwInt32 /*size*/) {
    if (streamFreeList() != nullptr) RwFreeListDestroy(streamFreeList());
    --streamModule.numInstances;
    return instance;
}

RwUInt32 RwStreamRead(RwStream* stream, void* buffer, RwUInt32 length) {
    switch (stream->type) {
        case rwSTREAMFILE:
        case rwSTREAMFILENAME: {
            void* fp = stream->Type.file.fpFile;
            const auto read = static_cast<RwUInt32>(RwFread(buffer, 1, length, fp));
            if (read != length) streamError(RwFeof(fp) != 0 ? E_RW_ENDOFSTREAM : E_RW_READ);
            return read;
        }
        case rwSTREAMMEMORY: {
            RwStreamMemory& memory = stream->Type.memory;
            const std::size_t left = memory.nSize - memory.position;
            if (length > left) {
                length = static_cast<RwUInt32>(left);
                streamError(E_RW_ENDOFSTREAM);
            }
            if (length != 0) std::memcpy(buffer, memory.memBlock + memory.position, length);
            memory.position += length;
            return length;
        }
        case rwSTREAMCUSTOM:
            return stream->Type.custom.sfnread(stream->Type.custom.data, buffer, length);
        default:
            streamError(E_RW_INVSTREAMTYPE);
            return 0;
    }
}

RwStream* RwStreamWrite(RwStream* stream, const void* buffer, RwUInt32 length) {
    switch (stream->type) {
        case rwSTREAMFILE:
        case rwSTREAMFILENAME:
            if (RwFwrite(buffer, 1, length, stream->Type.file.fpFile) != length) {
                streamError(E_RW_WRITE);
                return nullptr;
            }
            return stream;
        case rwSTREAMMEMORY: {
            RwStreamMemory& memory = stream->Type.memory;
            if (memory.memBlock == nullptr) {
                memory.memBlock = static_cast<RwUInt8*>(RwMalloc(kMemoryGrowth, kEventHint));
                if (memory.memBlock == nullptr) {
                    RwError error{rwID_COREPLUGIN, _rwerror(E_RW_NOMEM, static_cast<RwInt32>(kMemoryGrowth))};
                    RwErrorSet(&error);
                    return nullptr;
                }
                memory.nSize = kMemoryGrowth;
            }
            if (memory.nSize - memory.position < length) {
                const std::size_t newSize = length < kMemoryGrowth ? memory.nSize + kMemoryGrowth : memory.nSize + length;
                auto* block = static_cast<RwUInt8*>(RwRealloc(memory.memBlock, newSize, kEventHint | kResizeable));
                if (block == nullptr) {
                    RwError error{rwID_COREPLUGIN, _rwerror(E_RW_NOMEM, static_cast<RwInt32>(newSize - memory.nSize))};
                    RwErrorSet(&error);
                    return nullptr;
                }
                memory.memBlock = block;
                memory.nSize = newSize;
            }
            std::memcpy(memory.memBlock + memory.position, buffer, length);
            memory.position += length;
            return stream;
        }
        case rwSTREAMCUSTOM:
            return stream->Type.custom.sfnwrite(stream->Type.custom.data, buffer, length) != 0 ? stream : nullptr;
        default:
            streamError(E_RW_INVSTREAMTYPE);
            return nullptr;
    }
}

RwStream* RwStreamSkip(RwStream* stream, RwUInt32 offset) {
    if (offset == 0) return stream;
    switch (stream->type) {
        case rwSTREAMFILE:
        case rwSTREAMFILENAME: {
            void* fp = stream->Type.file.fpFile;
            if (RwFseek(fp, static_cast<RwInt64>(offset), kSeekCur) != 0) {   // never negative
                if (RwFeof(fp) != 0) streamError(E_RW_ENDOFSTREAM);
                return nullptr;
            }
            return stream;
        }
        case rwSTREAMMEMORY: {
            RwStreamMemory& memory = stream->Type.memory;
            const std::size_t position = memory.position + offset;
            if (position > memory.nSize) {
                memory.position = memory.nSize;
                streamError(E_RW_ENDOFSTREAM);
                return nullptr;
            }
            memory.position = position;
            return stream;
        }
        case rwSTREAMCUSTOM:
            return stream->Type.custom.sfnskip(stream->Type.custom.data, offset) != 0 ? stream : nullptr;
        default:
            streamError(E_RW_INVSTREAMTYPE);
            return nullptr;
    }
}

RwBool RwStreamClose(RwStream* stream, void* pData) {
    RwBool result = 1;
    switch (stream->type) {
        case rwSTREAMFILE:
            break;
        case rwSTREAMFILENAME:
            result = RwFclose(stream->Type.file.fpFile) == 0;
            break;
        case rwSTREAMMEMORY:
            if (stream->accessType != rwSTREAMREAD && pData != nullptr) {
                auto* memory = static_cast<RwMemory*>(pData);
                memory->start = stream->Type.memory.memBlock;
                memory->length = stream->Type.memory.position;
            }
            break;
        case rwSTREAMCUSTOM:
            if (stream->Type.custom.sfnclose != nullptr) stream->Type.custom.sfnclose(stream->Type.custom.data);
            break;
        default:
            streamError(E_RW_INVSTREAMTYPE);
            return 0;
    }
    if (stream->rwOwned != 0) rwSrcGlobals().memoryFree(streamFreeList(), stream);
    return result;
}

RwStream* RwStreamOpen(RwStreamType type, RwStreamAccessType accessType, const void* pData) {
    auto* stream = static_cast<RwStream*>(rwSrcGlobals().memoryAlloc(streamFreeList(), kEventHint));
    if (stream == nullptr) return nullptr;
    stream->type = type;
    stream->accessType = accessType;
    stream->rwOwned = 1;
    bool opened = false;
    switch (type) {
        case rwSTREAMFILE:
            if (RwFtell(const_cast<void*>(pData)) != -1) {
                stream->Type.file.constfpFile = pData;
                opened = true;
            }
            break;
        case rwSTREAMFILENAME: {
            const auto* name = static_cast<const RwChar*>(pData);
            void* fp = nullptr;
            switch (accessType) {
                case rwSTREAMREAD: fp = RwFopen(name, "rb"); break;
                case rwSTREAMWRITE: fp = RwFopen(name, "wb"); break;
                case rwSTREAMAPPEND: fp = RwFopen(name, "ab"); break;
                default: streamError(E_RW_INVSTREAMACCESSTYPE); break;
            }
            if (fp != nullptr) {
                stream->Type.file.fpFile = fp;
                opened = true;
            } else {
                streamError(E_RW_BADOPEN);
            }
            break;
        }
        case rwSTREAMMEMORY: {
            const auto* memory = static_cast<const RwMemory*>(pData);
            RwStreamMemory& state = stream->Type.memory;
            opened = true;
            switch (accessType) {
                case rwSTREAMREAD: state = {0, memory->length, memory->start}; break;
                case rwSTREAMWRITE: state = {0, 0, nullptr}; break;
                case rwSTREAMAPPEND: state = {memory->length, memory->length, memory->start}; break;
                default:
                    streamError(E_RW_INVSTREAMACCESSTYPE);
                    opened = false;
                    break;
            }
            break;
        }
        case rwSTREAMCUSTOM:
            stream->Type.custom = *static_cast<const RwStreamCustom*>(pData);
            opened = true;
            break;
        default:
            streamError(E_RW_INVSTREAMTYPE);
            break;
    }
    if (!opened) {
        rwSrcGlobals().memoryFree(streamFreeList(), stream);
        return nullptr;
    }
    return stream;
}
