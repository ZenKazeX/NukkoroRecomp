// Native tests of the RenderWare core's limits on a 64-bit host (rwcore/README.md "64-bit model"): files past 4 GiB
// (a real sparse file - no disk space used - read through RwStreamOpen / RwStreamSkip / RwStreamFindChunk /
// RwStreamReadInt32, and skips of 2 GiB and more through the file interface, which a 32-bit long turned negative);
// the guards that replace the original's wrap-arounds (free-list parameters, RwSList counts at RwInt32's end, plugin
// offsets at RwInt32's end, negative plugin sizes).
//
// Usage: rwcore_limits_tests <scratch directory>
#include "rw/core/binary.h"
#include "rw/core/engine.h"
#include "rw/core/error.h"
#include "rw/core/list.h"
#include "rw/core/memory.h"
#include "rw/core/plugin.h"
#include "rw/core/stream.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#endif

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

RwError takeError() {
    RwError e{};
    RwErrorGet(&e);
    return e;
}

std::vector<RwInt64> g_seeks;
RwInt32 recordSeek(void*, RwInt64 offset, RwInt32) {
    g_seeks.push_back(offset);
    return 0;
}
RwInt64 tellZero(void*) { return 0; }
int g_reallocs = 0;
void* countingRealloc(void* p, std::size_t n, RwUInt32) {
    ++g_reallocs;
    return std::realloc(p, n);
}

/// A sparse file of size bytes with data at offset; false where the file system cannot make it sparse.
bool makeSparse(const std::string& path, RwInt64 size, RwInt64 offset, const std::vector<unsigned char>& data) {
#if defined(_WIN32)
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD returned = 0;
    bool ok = DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != 0;
    LARGE_INTEGER at{};
    at.QuadPart = offset;
    DWORD written = 0;
    ok = ok && SetFilePointerEx(h, at, nullptr, FILE_BEGIN) &&
         WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
    at.QuadPart = size;
    ok = ok && SetFilePointerEx(h, at, nullptr, FILE_BEGIN) && SetEndOfFile(h);
    CloseHandle(h);
    return ok;
#else
    (void)path, (void)size, (void)offset, (void)data;
    return false;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: rwcore_limits_tests <scratch directory>");
        RwEngineOpenParams params{nullptr};
        require(RwEngineInit(nullptr, 0, 0) && RwEngineOpen(&params) && RwEngineStart(), "engine start");
        unsigned checks = 0;

        // ---- skips of 2 GiB and more reach the file layer as positive 64-bit offsets ----------------------------
        {
            const RwFileInterface host = rwFileInterface();
            rwFileInterface().fseek = recordSeek;
            rwFileInterface().ftell = tellZero;
            int dummy = 0;
            RwStream* s = RwStreamOpen(rwSTREAMFILE, rwSTREAMREAD, &dummy);
            require(s != nullptr, "a caller's file stream");
            for (RwUInt32 offset : {0x7FFFFFFFu, 0x80000000u, 0xF0000000u, 0xFFFFFFFFu}) {
                require(RwStreamSkip(s, offset) == s, "a large skip");
            }
            require((g_seeks == std::vector<RwInt64>{0x7FFFFFFF, 0x80000000LL, 0xF0000000LL, 0xFFFFFFFFLL}),
                    "skip offsets reach RwFseek unchanged (a 32-bit long made 0x80000000 and up negative)");
            RwStreamClose(s, nullptr);
            rwFileInterface() = host;
            checks += 4;
        }

        // ---- a real 6 GiB sparse file: skip past 4 GiB, find a chunk there, read it ----------------------------------
        {
            const std::string path = std::string(argv[1]) + "/rwcore_limits_sparse.bin";
            struct Remove {
                const std::string& path;
                ~Remove() { std::remove(path.c_str()); }   // also when a check throws
            } remove{path};
            const RwInt64 at = (RwInt64{1} << 32) + (RwInt64{1} << 29) + 12;   // 4.5 GiB + 12
            std::vector<unsigned char> chunk;
            const RwUInt32 words[4] = {rwID_STRUCT, 4, (((0x37002u - 0x30000u) << 14) & 0xFFC00000u) | (2u << 16) | 0x37u,
                                       0x12345678u};
            for (RwUInt32 w : words) for (int i = 0; i < 4; ++i) chunk.push_back(static_cast<unsigned char>(w >> (8 * i)));
            if (makeSparse(path, RwInt64{6} << 30, at, chunk)) {
                RwStream* s = RwStreamOpen(rwSTREAMFILENAME, rwSTREAMREAD, path.c_str());
                require(s != nullptr, "opening the sparse file");
                require(RwStreamSkip(s, 0xFFFFFFFFu) == s && RwStreamSkip(s, static_cast<RwUInt32>(at - 0xFFFFFFFFLL)) == s,
                        "skipping past 4 GiB");
                require(RwFtell(s->Type.file.fpFile) == at, "RwFtell past 4 GiB");
                RwUInt32 length = 0, version = 0;
                RwInt32 value = 0;
                require(RwStreamFindChunk(s, rwID_STRUCT, &length, &version) && length == 4 && version == 0x37002u,
                        "a chunk 4.5 GiB into a file");
                require(RwStreamReadInt32(s, &value, 4) == s && value == 0x12345678, "its value");
                require(RwStreamClose(s, nullptr) == 1, "close");
                std::remove(path.c_str());
                checks += 5;
            } else {
                std::remove(path.c_str());
                std::printf("  (no sparse file support here: the real large-file check did not run)\n");
            }
        }

        // ---- free-list parameters ---------------------------------------------------------------------------------
        {
            takeError();
            require(RwFreeListCreate(0, 16, 4, 0) == nullptr && takeError().errorCode == E_RW_BADPARAM, "entry size 0");
            require(RwFreeListCreate(16, 0, 4, 0) == nullptr && takeError().errorCode == E_RW_BADPARAM, "0 per block");
            require(RwFreeListCreate(16, 16, 3, 0) == nullptr && takeError().errorCode == E_RW_BADPARAM, "alignment 3");
            require(RwFreeListCreate(16, 16, -8, 0) == nullptr && takeError().errorCode == E_RW_BADPARAM, "alignment -8");
            RwFreeList* fl = RwFreeListCreate(24, 16, 0, 0);
            require(fl != nullptr && fl->alignment == 32 && fl->entrySize == 32, "alignment 0 means 32");
            void* e = _rwFreeListAllocReal(fl, 0);
            require(e != nullptr && reinterpret_cast<std::uintptr_t>(e) % 32 == 0, "an aligned entry");
            require(_rwFreeListFreeReal(fl, e) == fl && RwFreeListDestroy(fl) == 1, "free / destroy");
            checks += 6;
        }

        // ---- RwSList counts at RwInt32's end: no wrap, no reallocation, E_RW_NOMEM -------------------------------
        {
            const RwMemoryFunctions saved = rwSrcGlobals().memoryFuncs;
            rwSrcGlobals().memoryFuncs.rwrealloc = countingRealloc;
            RwSList* list = _rwSListCreate(1, 0);
            require(list != nullptr, "a list");
            RwUInt8* const elements = list->listElements;
            list->numElementsAlloced = 0x7FFFFFF0;
            list->numElementsFilled = 0x7FFFFFF0;
            takeError();
            require(_rwSListGetNewEntry(list, 0) == nullptr && takeError().errorCode == E_RW_NOMEM && g_reallocs == 0,
                    "growing past RwInt32 fails without reallocating");
            list->numElementsFilled = 4;
            require(_rwSListGetNewEntries(list, 0x7FFFFFF0, 0) == nullptr && takeError().errorCode == E_RW_NOMEM &&
                        g_reallocs == 0,
                    "a run of entries past RwInt32 fails without reallocating");
            list->numElementsAlloced = 20, list->numElementsFilled = 0, list->listElements = elements;
            require(_rwSListDestroy(list) == 1, "destroy");
            rwSrcGlobals().memoryFuncs = saved;
            checks += 2;
        }

        // ---- plugin offsets at RwInt32's end, negative sizes -------------------------------------------------------
        {
            RwEngineStop();
            RwEngineClose();   // plugins register only before an engine is open
            RwPluginRegistry reg = rwPLUGINREGISTRYINIT(16);
            require(_rwPluginRegistryAddPlugin(&reg, -1, 1, nullptr, nullptr, nullptr) == -1, "a negative size");
            require(_rwPluginRegistryAddPlugin(&reg, 0x7FFFFFF8, 2, nullptr, nullptr, nullptr) == -1 && reg.sizeOfStruct == 16,
                    "a slice past RwInt32");
            require(_rwPluginRegistryAddPlugin(&reg, 0x7FFFFFF0 - 16, 3, nullptr, nullptr, nullptr) == 16 &&
                        reg.sizeOfStruct == 0x7FFFFFF0,
                    "the last slice that fits");
            require(_rwPluginRegistryAddPlugin(&reg, 9, 4, nullptr, nullptr, nullptr) == -1, "nothing after it");
            checks += 4;
            require(RwEngineTerm(), "term");
        }

        std::printf("rwcore_limits_tests: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "rwcore_limits_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
