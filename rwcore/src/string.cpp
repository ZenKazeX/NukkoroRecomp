// RenderWare string functions (module bastring; see rw/core/engine.h): the engine's RwStringFunctions are the host
// C library's, as the original's are the GameCube's MSL (installed by _rwStringOpen, 0x80483E70). RenderWare's string
// API is unbounded by design (sprintf / strcpy / strcat take no destination size), so the library's deprecation
// warnings for those calls are disabled in this file only.
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "rw/core/engine.h"
#include "rw/core/binary.h"
#include "rw/core/error.h"

namespace {

int rwSprintf(RwChar* buffer, const RwChar* format, ...) {
    std::va_list args;
    va_start(args, format);
    const int n = std::vsprintf(buffer, format, args);
    va_end(args);
    return n;
}
int rwVsprintf(RwChar* buffer, const RwChar* format, std::va_list args) { return std::vsprintf(buffer, format, args); }
RwChar* rwStrcpy(RwChar* dest, const RwChar* srce) { return std::strcpy(dest, srce); }
RwChar* rwStrncpy(RwChar* dest, const RwChar* srce, std::size_t size) { return std::strncpy(dest, srce, size); }
RwChar* rwStrcat(RwChar* dest, const RwChar* srce) { return std::strcat(dest, srce); }
RwChar* rwStrncat(RwChar* dest, const RwChar* srce, std::size_t size) { return std::strncat(dest, srce, size); }
RwChar* rwStrrchr(const RwChar* string, int findThis) { return const_cast<RwChar*>(std::strrchr(string, findThis)); }
RwChar* rwStrchr(const RwChar* string, int findThis) { return const_cast<RwChar*>(std::strchr(string, findThis)); }
RwChar* rwStrstr(const RwChar* string, const RwChar* findThis) {
    return const_cast<RwChar*>(std::strstr(string, findThis));
}
int rwStrcmp(const RwChar* a, const RwChar* b) { return std::strcmp(a, b); }
int rwStrncmp(const RwChar* a, const RwChar* b, std::size_t n) { return std::strncmp(a, b, n); }
int rwStricmp(const RwChar* a, const RwChar* b) {
    for (;; ++a, ++b) {
        const int ca = std::tolower(static_cast<unsigned char>(*a)), cb = std::tolower(static_cast<unsigned char>(*b));
        if (ca != cb || ca == 0) return ca - cb;
    }
}
std::size_t rwStrlen(const RwChar* string) { return std::strlen(string); }
RwChar* rwStrupr(RwChar* string) {
    for (RwChar* c = string; *c != 0; ++c) *c = static_cast<RwChar>(std::toupper(static_cast<unsigned char>(*c)));
    return string;
}
RwChar* rwStrlwr(RwChar* string) {
    for (RwChar* c = string; *c != 0; ++c) *c = static_cast<RwChar>(std::tolower(static_cast<unsigned char>(*c)));
    return string;
}
RwChar* rwStrtok(RwChar* string, const RwChar* delimit) { return std::strtok(string, delimit); }
int rwSscanf(const RwChar* buffer, const RwChar* format, ...) {
    std::va_list args;
    va_start(args, format);
    const int n = std::vsscanf(buffer, format, args);
    va_end(args);
    return n;
}

}  // namespace

RwBool _rwStringOpen() {
    rwSrcGlobals().stringFuncs = {rwSprintf, rwVsprintf, rwStrcpy,  rwStrncpy, rwStrcat,  rwStrncat,
                                  rwStrrchr, rwStrchr,   rwStrstr,  rwStrcmp,  rwStrncmp, rwStricmp,
                                  rwStrlen,  rwStrupr,   rwStrlwr,  rwStrtok,  rwSscanf};
    return 1;
}

void _rwStringClose() {}

namespace {

constexpr RwUInt32 kStringHint = 0x00030000u | rwID_STRING;   // rwMEMHINTDUR_EVENT, as the original passes it

/// _rwStringStreamFindAndRead's body; capacity 0 copies the chunk's whole length (the original's contract).
RwChar* readString(RwChar* string, std::size_t capacity, RwStream* stream) {
    for (;;) {
        RwUInt32 type = 0, length = 0, version = 0;
        if (!_rwStreamReadChunkHeader(stream, &type, &length, &version, nullptr)) return nullptr;
        if (version < rwLIBRARYBASEVERSION || version > rwLIBRARYCURRENTVERSION) {
            RwError error{rwID_COREPLUGIN, _rwerror(static_cast<RwInt32>(0x80000004u))};   // E_RW_BADVERSION
            RwErrorSet(&error);
            return nullptr;
        }
        if (type != rwID_STRING && type != rwID_UNICODESTRING) {
            if (RwStreamSkip(stream, length) == nullptr) return nullptr;
            continue;
        }
        const bool unicode = type == rwID_UNICODESTRING;
        const std::size_t characters = unicode ? length / 2 : length;
        bool allocated = false;
        if (string == nullptr) {
            string = static_cast<RwChar*>(RwMalloc(length != 0 ? length : 1, kStringHint));
            if (string == nullptr) {
                RwError error{rwID_COREPLUGIN, _rwerror(E_RW_NOMEM, length)};
                RwErrorSet(&error);
                return nullptr;
            }
            allocated = true;
        }
        const std::size_t keep = capacity == 0 ? characters : (characters < capacity - 1 ? characters : capacity - 1);
        // in pieces of 64 bytes (ASCII) / 128 bytes (Unicode), as the original copies through its stack buffer
        RwUInt8 piece[128];
        std::size_t done = 0, left = length;
        while (left > 0) {
            const std::size_t take = left < (unicode ? 128u : 64u) ? left : (unicode ? 128u : 64u);
            if (RwStreamRead(stream, piece, static_cast<RwUInt32>(take)) != take) {
                if (allocated) RwFree(string);   // native: the original leaks it on the ASCII path
                return nullptr;
            }
            left -= take;
            const std::size_t count = unicode ? take / 2 : take;
            for (std::size_t i = 0; i < count; ++i, ++done) {
                if (done < keep) string[done] = static_cast<RwChar>(unicode ? piece[2 * i] : piece[i]);
            }
        }
        if (capacity != 0) string[keep] = 0;
        return string;
    }
}

}  // namespace

RwChar* _rwStringStreamFindAndRead(RwChar* string, RwStream* stream) { return readString(string, 0, stream); }

RwChar* _rwStringStreamFindAndReadCapped(RwChar* string, std::size_t capacity, RwStream* stream) {
    return capacity == 0 ? nullptr : readString(string, capacity, stream);
}
