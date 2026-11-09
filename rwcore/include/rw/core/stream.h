// RenderWare Graphics 3.7 core: streams (module bastream), native x64.
//
// A stream reads or writes a host file (one the caller opened, or one opened by name), a block of memory, or the
// application's own functions. The originals are PAL main.dol 0x80481CBC..0x8048263C; proof:
// runtime/tests/rwcore_stream_tests.cpp. Files go through RwFopen / RwFread / ... (RenderWare 3.7's RtFileSystem
// entry points), which here are the host's C files (rwFileInterface) instead of the GameCube's disc file system.
//
// 64-bit: a memory stream's position and size are size_t (RenderWare's are RwUInt32), so a stream written to memory
// can grow past 4 GiB; each read, write or skip still moves at most an RwUInt32, as in the binary format.
// Not provided (not linked by the game): RwStreamSetFreeListCreateParams.
#pragma once

#include <cstddef>

#include "rw/core/engine.h"
#include "rw/core/types.h"

enum RwStreamType : RwInt32 {
    rwNASTREAM = 0,
    rwSTREAMFILE = 1,       ///< a file the caller opened (RwFopen)
    rwSTREAMFILENAME = 2,   ///< a file the stream opens by name
    rwSTREAMMEMORY = 3,     ///< a memory block (RwMemory)
    rwSTREAMCUSTOM = 4,     ///< the application's functions (RwStreamCustom)
};

enum RwStreamAccessType : RwInt32 {
    rwNASTREAMACCESS = 0,
    rwSTREAMREAD = 1,
    rwSTREAMWRITE = 2,
    rwSTREAMAPPEND = 3,
};

/// A memory stream's state (rwplcore.h RwStreamMemory; size_t position and size).
struct RwStreamMemory {
    std::size_t position;   ///< the next byte, 0 first
    std::size_t nSize;      ///< the bytes in memBlock
    RwUInt8* memBlock;
};

union RwStreamFile {
    void* fpFile;
    const void* constfpFile;
};

using rwCustomStreamFnClose = RwBool (*)(void* data);
using rwCustomStreamFnRead = RwUInt32 (*)(void* data, void* buffer, RwUInt32 length);
using rwCustomStreamFnWrite = RwBool (*)(void* data, const void* buffer, RwUInt32 length);
using rwCustomStreamFnSkip = RwBool (*)(void* data, RwUInt32 offset);

/// The application's stream functions (rwplcore.h RwStreamCustom). sfnclose may be NULL.
struct RwStreamCustom {
    rwCustomStreamFnClose sfnclose;
    rwCustomStreamFnRead sfnread;
    rwCustomStreamFnWrite sfnwrite;
    rwCustomStreamFnSkip sfnskip;
    void* data;
};

union RwStreamUnion {
    RwStreamMemory memory;
    RwStreamFile file;
    RwStreamCustom custom;
};

/// A stream (rwplcore.h RwStream). position is unused by the core, as in the original.
struct RwStream {
    RwStreamType type;
    RwStreamAccessType accessType;
    RwInt32 position;
    RwStreamUnion Type;
    RwBool rwOwned;
};

/// A block of memory to stream from or into (rwplcore.h RwMemory; size_t length). For a written stream, close
/// stores the stream's block and its length here.
struct RwMemory {
    RwUInt8* start;
    std::size_t length;
};

/// The stream module's globals: the free list of streams.
struct rwStreamGlobals {
    RwFreeList* streamFreeList;
};

extern RwModuleInfo streamModule;

/// The file functions streams use (RenderWare 3.7's RwFopen family, rtfsyst.h). They default to the host's C
/// files; an application or a test may replace them. 64-bit: positions and seek offsets are RwInt64 (RenderWare's
/// are long / RwInt32, 32 bits on Windows), so files past 2 GiB seek and tell correctly. The table is the port's (descriptive name): RenderWare reaches
/// its file systems through RtFSManager, which the game does not use for its streams.
struct RwFileInterface {
    void* (*fopen)(const RwChar* name, const RwChar* access);
    RwInt32 (*fclose)(void* fptr);
    std::size_t (*fread)(void* addr, std::size_t size, std::size_t count, void* fptr);
    std::size_t (*fwrite)(const void* addr, std::size_t size, std::size_t count, void* fptr);
    RwInt32 (*fseek)(void* fptr, RwInt64 offset, RwInt32 origin);
    RwInt32 (*feof)(void* fptr);
    RwInt64 (*ftell)(void* fptr);
    /// Whether the file exists (RenderWare's rwfexist; the image search uses it). NULL: RwFexist opens the file for
    /// reading through fopen and closes it again.
    RwBool (*fexist)(const RwChar* name);
};
RwFileInterface& rwFileInterface();
RwFileInterface rwHostFileInterface();

void* RwFopen(const RwChar* name, const RwChar* access);
RwInt32 RwFclose(void* fptr);
std::size_t RwFread(void* addr, std::size_t size, std::size_t count, void* fptr);
std::size_t RwFwrite(const void* addr, std::size_t size, std::size_t count, void* fptr);
RwInt32 RwFseek(void* fptr, RwInt64 offset, RwInt32 origin);
RwInt32 RwFeof(void* fptr);
RwInt64 RwFtell(void* fptr);
/// Whether the file exists: the interface's fexist, else an fopen "rb" that succeeds (closed again). The GameCube's is
/// its disc file system's (0x8047B6CC, not ported).
RwBool RwFexist(const RwChar* name);

/// Opens the module in an engine instance: the free list of streams (16 to a block, one preallocated, a static
/// header, hint rwMEMHINTDUR_GLOBAL | rwID_STREAMMODULE). NULL when it cannot be made. Original: 0x80481CBC.
void* _rwStreamModuleOpen(void* instance, RwInt32 offset, RwInt32 size);

/// Destroys the free list of streams. Returns instance. Original: 0x80481D48.
void* _rwStreamModuleClose(void* instance, RwInt32 offset, RwInt32 size);

/// A stream of type for accessType over pData: the file (rwSTREAMFILE; refused when RwFtell says -1), its name
/// (rwSTREAMFILENAME: opened "rb" / "wb" / "ab"), an RwMemory (read: its bytes; write: a new block; append: its
/// bytes, positioned at the end), or an RwStreamCustom (copied). NULL with an RwError on failure. Original: 0x804823B8.
RwStream* RwStreamOpen(RwStreamType type, RwStreamAccessType accessType, const void* pData);

/// Closes stream (a name-opened file is closed; a written memory stream's block and length go to pData, an RwMemory)
/// and frees it. TRUE, FALSE when a named file fails to close or the type is invalid (then stream is not freed).
/// Original: 0x8048229C.
RwBool RwStreamClose(RwStream* stream, void* pData);

/// Reads up to length bytes into buffer. Returns the bytes read; fewer at the end (E_RW_ENDOFSTREAM) or on a file
/// error (E_RW_READ). Original: 0x80481D9C.
RwUInt32 RwStreamRead(RwStream* stream, void* buffer, RwUInt32 length);

/// Writes length bytes (a memory stream grows by 512 bytes, or by length when that is more). Returns stream, NULL on
/// failure (E_RW_WRITE / E_RW_NOMEM). Original: 0x80481F20.
RwStream* RwStreamWrite(RwStream* stream, const void* buffer, RwUInt32 length);

/// Skips offset bytes. Returns stream, NULL past the end (E_RW_ENDOFSTREAM; a memory stream stops at its end).
/// Original: 0x8048214C.
RwStream* RwStreamSkip(RwStream* stream, RwUInt32 offset);
