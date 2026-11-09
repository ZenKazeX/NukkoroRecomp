// The game's ONE archive layer in C++. Contract: include/gekko/native_one.h, docs/ONE_LAYER.md.
//
// Each owner leaves guest memory exactly as the original words leave it, the stack included: the game copies
// uninitialised stack bytes into heap records (fn_8006BEF0's 0x2C-byte copies), so a frame the original writes and an
// owner skipped would change heap bytes. An owner therefore writes its original's frame - the back chain, the saved LR
// in the caller's frame, the saved registers, the locals (FindName's uppercase copy) - and makes its guest calls (the
// RenderWare free, operator delete, sscanf) from the original's frame, with the original call site as the return
// address and the non-volatile registers the original holds there, so the callee's own frame writes are the original's
// too. The leaves the originals call (Entry, strcmp, strstr, the byte swap 0x80043D34) write no stack. memset
// (0x8000540C) is reproduced in C++: its frame (back chain, LR, r31) and __fill_mem's stores (a leaf).

#include "gekko/native_one.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>

#include "gekko/cpu.h"
#include "gekko/recomp.h"

namespace gekko {

namespace {

constexpr u32 kFindSubstr = 0x8004C878u, kFindName = 0x8004C938u, kEntry = 0x8004CA50u, kAdopt = 0x8004CAC0u;
constexpr u32 kIsOpen = 0x8004CB20u, kClose = 0x8004CB48u, kParse = 0x8004CD38u;
constexpr u32 kDtor = 0x8004CF90u, kCtor = 0x8004CFD8u;
constexpr u32 kRwFree = 0x80049B5Cu;   // the RenderWare free (RwEngineInstance +0x10C)
constexpr u32 kDelete = 0x803A1334u;   // operator delete
constexpr u32 kSscanf = 0x803AC608u;   // MSL sscanf
constexpr u32 kVersionFormat = 0x804ACCB4u;  // "One Ver %f"
constexpr u32 kLegacyBound = 0x805F2D88u;    // kF32_0_59 (0.59f)
// The return sites of the originals' calls (the LR their callees save in the caller's frame).
constexpr u32 kCloseFreeBufferSite = 0x8004CBB8u, kCloseMemsetSite = 0x8004CBC8u, kCloseFreeAllocationSite = 0x8004CBF8u;
constexpr u32 kParseSscanfSite = 0x8004CD64u;
constexpr u32 kAdoptCloseSite = 0x8004CAE0u, kAdoptParseSite = 0x8004CB08u;
constexpr u32 kDtorCloseSite = 0x8004CFB0u, kDtorDeleteSite = 0x8004CFC0u;
constexpr u32 kCtorMemset1Site = 0x8004D014u, kCtorMemset2Site = 0x8004D024u, kCtorOpenSite = 0x8004D044u;
// Open (0x8004CE1C) and what it reaches.
constexpr u32 kOpen = 0x8004CE1Cu;
constexpr u32 kReadManagerGetter = 0x80037200u;  // the read manager's holder
constexpr u32 kStreamOpen = 0x80045014u;         // (manager, path): the path copy, RwStreamOpen(2, 1, copy), its free
constexpr u32 kCategoryPush = 0x80049B3Cu, kCategoryPop = 0x80049B1Cu;  // RW allocation category 0x40 / back to 0x20
constexpr u32 kRwAllocate = 0x80049BB8u;         // RwMalloc(size, hint)
constexpr u32 kStreamClose = 0x8048229Cu;        // RwStreamClose(stream, 0)
constexpr u32 kStreamOpenFn = 0x804823B8u;       // RwStreamOpen(type, access, data)
constexpr u32 kErrorCode = 0x8047EC78u;          // the RW error code builder (varargs)
constexpr u32 kErrorSet = 0x8047EBD4u;           // RwErrorSet(&error)
constexpr u32 kFileEof = 0x8047B6A0u;            // RwFeof(fp)
constexpr u32 kRtGcnFsRead = 0x8047BC60u;        // RtGCNFS's read: the file system's read slot (+0x30)
constexpr u32 kComplexTable = 0x8056EFA0u;       // fn_8047DD48's jump table (chunk type -> "li r3, n; blr")
constexpr u32 kOpenCloseSite1 = 0x8004CE8Cu, kOpenCloseSite2 = 0x8004CEB0u, kOpenGetterSite = 0x8004CEB4u;
constexpr u32 kOpenStreamSite = 0x8004CEBCu, kOpenHeaderSite = 0x8004CED4u, kOpenPushSite = 0x8004CEE0u;
constexpr u32 kOpenAllocSite = 0x8004CF18u, kOpenPopSite = 0x8004CF20u, kOpenReadSite = 0x8004CF40u;
constexpr u32 kOpenCloseStreamSite = 0x8004CF50u, kOpenParseSite = 0x8004CF78u;
constexpr u32 kHeaderReadSite = 0x8047E9C4u, kHeaderErrorCodeSite = 0x8047E9ECu, kHeaderErrorSetSite = 0x8047E9F8u;
constexpr u32 kStreamFreadSite = 0x80481DFCu, kFreadSlotSite = 0x8047B460u, kStreamEofSite = 0x80481E10u;
constexpr u32 kStreamEofCodeSite = 0x80481E2Cu, kStreamEofSetSite = 0x80481E38u;
constexpr u32 kStreamShortCodeSite = 0x80481E54u, kStreamShortSetSite = 0x80481E60u;
constexpr u32 kCurrentThread = 0x800000E4u, tPriority = 0x2D0u, kSdaRunQueueBits = 0x5AE0u;
// OpenAsync (0x8004CC94) and its completion OnRead (0x8004CC28).
constexpr u32 kOpenAsync = 0x8004CC94u, kOnRead = 0x8004CC28u;
constexpr u32 kSubmit = 0x8004506Cu;  // (manager, request, 1): the exists check (manager +4), then the queued node
constexpr u32 kAsyncCloseSite = 0x8004CCD4u, kAsyncGetterSite = 0x8004CCFCu, kAsyncSubmitSite = 0x8004CD08u;
constexpr u32 kOnReadAdoptSite = 0x8004CC54u, kOnReadCallbackSite = 0x8004CC68u;
constexpr u32 kOnReadReturn = 0x8004CC6Cu;  // after the callback's bctrl: OnRead's tail (an owner here)
constexpr u32 oCallback = 0x48u, oCallbackArg = 0x4Cu;
// The whole-file sync read 0x800450C4(manager, path, &size) (batch 4d).
constexpr u32 kWholeRead = 0x800450C4u;
constexpr u32 kFileOpen = 0x800514CCu, kFileClose = 0x8047B3D8u;  // RwFopen(path, mode), RwFclose(fp)
constexpr u32 kReadMode = 0x805F2C38u;                             // "rb"
constexpr u32 kWholeOpenSite = 0x800450E8u, kWholeAllocSite = 0x80045100u, kWholeReadSite = 0x80045114u;
constexpr u32 kWholeCloseSite = 0x80045138u;

// The six TOneFileAsync<T> loaders' updates (batch 4e): one template (STREAMING_DIFFERENTIAL_VALIDATION.md §15).
constexpr u32 kLoaderManager = 0x8004B164u;    // static_80574C2C::instance
constexpr u32 kLoaderCount = 0x8004B13Cu;      // (manager) -> the jobs registered
constexpr u32 kLoaderRegister = 0x8004D564u;   // ref_TOneFileAsyncManager(manager, task, one, index)
constexpr u32 kLoaderUnregister = 0x8004D424u; // (manager, task)
constexpr u32 kExtractFn = 0x8004C770u;        // the ONE extract (an owner in expand modes 3-5)
constexpr u32 kStreamFindChunk = 0x8047DFA0u;  // RwStreamFindChunk(stream, type, 0, 0)
constexpr u32 kTexDictSetCurrent = 0x8048E114u;
constexpr u32 kSameName = 0x805E4990u;         // the name word every loader compares its own with
constexpr u32 kLoaderJobs = 0x805EF234u;       // the jobs outstanding (state 4 after the callback: - 1)
constexpr u32 kLoaderJobLimit = 16u;
// The task (TOneFileAsync<T>): +4 flags, +0x28 state, +0x2C callback, +0x30 its argument, +0x34 index, +0x38
// buffer, +0x3C result, +0x40 the archive, +0x44 / +0x48 the memory stream's start / size (+0x48 also the sync
// extract's size), +0x4C stream, +0x50 async pending, +0x54 the texture dictionary, +0x58 a wait flag.
constexpr u32 kFlags = 0x4u, kState = 0x28u, kTaskCallback = 0x2Cu, kTaskArg = 0x30u, kIndex = 0x34u;
constexpr u32 kBuffer = 0x38u, kResult = 0x3Cu, kArchive = 0x40u, kMemory = 0x44u, kSize = 0x48u, kStream = 0x4Cu;
constexpr u32 kAsync = 0x50u, kTexDict = 0x54u, kWait = 0x58u;
constexpr u32 oArchiveJobs = 0x54u;  // the archive's +0x54: the loads outstanding on it

struct Loader {
    const char* name;
    u32 update;
    u32 done;          // the extract's callback (the async path)
    u32 chunk;         // 0: the raw loader (no chunk, no reader: the buffer is the result)
    bool texdict;      // RwTexDictionarySetCurrent(+0x54) before the reader (r30 = +0x54 from the find on)
    u32 reader;        // reader(stream), or reader(reader_arg, stream)
    u32 reader_arg;
    u32 after;         // a call with the result after the reader, or 0
    u32 name_word;     // strcmp(*name_word, *0x805E4990) != 0 frees the buffer at the end
    // Return sites: [0] instance, [1] count, [2] alloc, [3] free, [4] alloc, [5] alloc, [6] instance, [7] register,
    // [8] extract async, [9] extract sync, [10] free (failed), [11] instance, [12] unregister, [13] stream open,
    // [14] find chunk, [15] texdict, [16] reader, [17] after, [18] free (end), [19] stream close, [20] the callback.
    u32 sites[21];
};
constexpr Loader kLoaders[] = {
    {"RpDMorphAnimation", 0x8004AE60u, 0x8004B12Cu, 0x1Eu, false, 0x80443C74u, 0u, 0u, 0x805E49B8u,
     {0x8004AEACu, 0x8004AEB0u, 0x8004AEE0u, 0x8004AEF4u, 0x8004AF14u, 0x8004AF44u, 0x8004AF4Cu, 0x8004AF5Cu,
      0x8004AF84u, 0x8004AFA8u, 0x8004AFE4u, 0x8004B000u, 0x8004B008u, 0x8004B028u, 0x8004B040u, 0u, 0x8004B050u,
      0u, 0x8004B09Cu, 0x8004B0B8u, 0x8004B0E4u}},
    {"RpWorld", 0x8004B3A0u, 0x8004B678u, 0x0Bu, true, 0x80456744u, 0u, 0u, 0x805E49B0u,
     {0x8004B3ECu, 0x8004B3F0u, 0x8004B420u, 0x8004B434u, 0x8004B454u, 0x8004B484u, 0x8004B48Cu, 0x8004B49Cu,
      0x8004B4C4u, 0x8004B4E8u, 0x8004B524u, 0x8004B540u, 0x8004B548u, 0x8004B568u, 0x8004B584u, 0x8004B594u,
      0x8004B59Cu, 0u, 0x8004B5E8u, 0x8004B604u, 0x8004B630u}},
    {"RtDict", 0x8004B7BCu, 0x8004BA90u, 0x2Bu, false, 0x8047A2D4u, 0x8056E808u, 0u, 0x805E49A8u,
     {0x8004B808u, 0x8004B80Cu, 0x8004B83Cu, 0x8004B850u, 0x8004B870u, 0x8004B8A0u, 0x8004B8A8u, 0x8004B8B8u,
      0x8004B8E0u, 0x8004B904u, 0x8004B940u, 0x8004B95Cu, 0x8004B964u, 0x8004B984u, 0x8004B99Cu, 0u, 0x8004B9B4u,
      0u, 0x8004BA00u, 0x8004BA1Cu, 0x8004BA48u}},
    {"RpClump", 0x8004BBD4u, 0x8004BEACu, 0x10u, true, 0x804586E8u, 0u, 0u, 0x805E49A0u,
     {0x8004BC20u, 0x8004BC24u, 0x8004BC54u, 0x8004BC68u, 0x8004BC88u, 0x8004BCB8u, 0x8004BCC0u, 0x8004BCD0u,
      0x8004BCF8u, 0x8004BD1Cu, 0x8004BD58u, 0x8004BD74u, 0x8004BD7Cu, 0x8004BD9Cu, 0x8004BDB8u, 0x8004BDC8u,
      0x8004BDD0u, 0u, 0x8004BE1Cu, 0x8004BE38u, 0x8004BE64u}},
    {"RwTexDictionary", 0x8004BFF0u, 0x8004C2C4u, 0x16u, false, 0x8048500Cu, 0u, 0x802E0BB0u, 0x805E4998u,
     {0x8004C03Cu, 0x8004C040u, 0x8004C070u, 0x8004C084u, 0x8004C0A4u, 0x8004C0D4u, 0x8004C0DCu, 0x8004C0ECu,
      0x8004C114u, 0x8004C138u, 0x8004C174u, 0x8004C190u, 0x8004C198u, 0x8004C1B8u, 0x8004C1D0u, 0u, 0x8004C1E0u,
      0x8004C1ECu, 0x8004C234u, 0x8004C250u, 0x8004C27Cu}},
    {"raw", 0x8004C408u, 0x8004C6B4u, 0u, false, 0u, 0u, 0u, 0x805E4990u,
     {0x8004C454u, 0x8004C458u, 0x8004C488u, 0x8004C49Cu, 0x8004C4BCu, 0x8004C4ECu, 0x8004C4F4u, 0x8004C504u,
      0x8004C52Cu, 0x8004C550u, 0x8004C58Cu, 0x8004C5A8u, 0x8004C5B0u, 0x8004C5D0u, 0u, 0u, 0u, 0u, 0x8004C624u,
      0x8004C640u, 0x8004C66Cu}},
};
constexpr u32 kCrBit6 = 0x02000000u;  // cr1eq: cleared before a varargs call without FP arguments (crclr cr1eq)
// The archive object and its directory.
constexpr u32 oName = 0x00u, oBuffer = 0x2Cu, oBufferSize = 0x30u, oData = 0x34u, oAllocation = 0x38u;
constexpr u32 oDataSize = 0x3Cu, oAllocator = 0x40u, oPending = 0x44u, oByte50 = 0x50u, oWord54 = 0x54u;
constexpr u32 dCount = 0x10u, dEntries = 0x34u, kEntryBytes = 0x38u;
constexpr u32 eSize = 0x2Cu, ePointer = 0x30u, eFlags = 0x34u;
constexpr u32 kNameBuffer = 0x2Cu;  // FindName's uppercase copy: at most 44 bytes at its frame + 8
constexpr u32 kStopped = 0u;  // an owner's result after a guest call stopped the run (it no longer matters)

// The non-volatile registers an original holds (r26..r31; r14..r25 are never touched by these functions).
using Regs = std::array<u32, 6>;
Regs regs_of(const Cpu& c) { return {c.gpr[26], c.gpr[27], c.gpr[28], c.gpr[29], c.gpr[30], c.gpr[31]}; }

// A prologue: stwu r1, -size(r1); mflr r0; stw r0, size + 4(r1). Returns the frame.
u32 prologue(Cpu& c, u32 sp, u32 lr, u32 size) {
    const u32 frame = sp - size;
    c.mem_write32(frame, sp);
    c.mem_write32(sp + 4u, lr);
    return frame;
}
// stmw rN, at(r1): r(first)..r31.
void save_regs(Cpu& c, u32 at, u32 first, const Regs& regs) {
    for (u32 r = first; r <= 31u; ++r) c.mem_write32(at + (r - first) * 4u, regs[r - 26u]);
}

// A guest call from an original's frame: r1 = that frame, LR = the original's return site, r28..r31 = the original's
// values there. The caller's registers come back afterwards (the owner returns as the original's epilogue would).
bool call_guest_from(Cpu& c, u32 frame, u32 site, const Regs& regs, u32 function, u32 r3, u32 r4 = 0u, u32 r5 = 0u,
                     u32 r6 = 0u, u32 r7 = 0u) {
    const u32 sp = c.gpr[1], link = c.lr, entry_pc = c.pc;
    const Regs saved = regs_of(c);
    c.gpr[1] = frame;
    for (u32 r = 26u; r <= 31u; ++r) c.gpr[r] = regs[r - 26u];
    c.gpr[3] = r3;
    c.gpr[4] = r4;
    c.gpr[5] = r5;
    c.gpr[6] = r6;
    c.gpr[7] = r7;
    c.gpr[12] = function;
    c.ctr = function;
    recomp::call_indirect(c, function, site);
    if (c.exiting()) c.finish_transfers();
    if (c.exiting() || c.pc != site) {
        c.request_stop(StopReason::ExecutionFault,
                       "native ONE layer: a guest call (0x" + std::to_string(function) + ") did not return");
        return false;
    }
    c.gpr[1] = sp;
    c.lr = link;
    c.pc = entry_pc;  // an owner leaves pc at its entry (the verifier's owner pass then resumes at LR, as dispatch does)
    for (u32 r = 26u; r <= 31u; ++r) c.gpr[r] = saved[r - 26u];
    return true;
}

// A guest call whose original loads only r3: r4 and up are what they hold there - the earlier callees' or the
// function's last writes, which the owner keeps current - since a callee may hand r4 back unchanged.
bool call_guest_r3(Cpu& c, u32 frame, u32 site, const Regs& regs, u32 function, u32 r3) {
    return call_guest_from(c, frame, site, regs, function, r3, c.gpr[4], c.gpr[5], c.gpr[6], c.gpr[7]);
}

// memset 0x8000540C(dst, 0, n) called from `site` with the stack at `sp` and r31 = `r31`: its frame, then __fill_mem.
void memset_zero(Cpu& c, u32 sp, u32 site, u32 r31, u32 dst, u32 n) {
    const u32 frame = prologue(c, sp, site, 0x10u);
    c.mem_write32(frame + 0xCu, r31);
    for (u32 i = 0; i < n; ++i) c.mem_write8(dst + i, 0u);
    c.gpr[4] = 0u;  // __fill_mem: clrlwi r4, r4, 24 of the fill byte (fewer than 0x20 bytes: r4 is not used again)
}

bool is_open(Cpu& c, u32 one) { return c.mem_read32(one + oBuffer) != 0u || c.mem_read8(one + oName) != 0u; }

u32 count_of(Cpu& c, u32 one) { return c.mem_read32(c.mem_read32(one + oData) + dCount); }

// fn_8004CA50 (a leaf).
u32 entry(Cpu& c, u32 one, u32 index) {
    if (!is_open(c, one)) return 0u;
    if (static_cast<s32>(index) < 2) return 0u;
    if (static_cast<s32>(index) >= static_cast<s32>(count_of(c, one) + 2u)) return 0u;
    return c.mem_read32(one + oData) + dEntries + index * kEntryBytes;
}

// MSL strcmp (fn_803AD96C, a leaf) as its words run it: bytes until both strings are word-aligned, then words while
// none holds a byte < 0x01 or >= 0x81 ((w - 0x01010101) & 0x80808080), then bytes. The result r3 and where r4 stops,
// as the original leaves them (the callers' r4 is the verifier's contract).
struct Strcmp {
    u32 r3, r4;
};
Strcmp msl_strcmp(Cpu& c, u32 r3, u32 r4) {
    u32 r5 = c.mem_read8(r3), r0 = c.mem_read8(r4);
    if (r5 != r0) return {r5 - r0, r4};
    const auto bytes = [&]() -> Strcmp {  // .L_803ADA5C
        for (;;) {
            if (r5 == 0u) return {0u, r4};
            r5 = c.mem_read8(++r3);
            r0 = c.mem_read8(++r4);
            if (r5 != r0) return {r5 - r0, r4};
        }
    };
    const u32 a3 = r3 & 3u;
    if ((r4 & 3u) != a3) return bytes();
    if (a3 != 0u) {
        if (r5 == 0u) return {0u, r4};
        for (u32 n = 3u - a3; n != 0u; --n) {
            r5 = c.mem_read8(++r3);
            r0 = c.mem_read8(++r4);
            if (r5 != r0) return {r5 - r0, r4};
            if (r5 == 0u) return {0u, r4};
        }
        ++r3;
        ++r4;
    }
    u32 r7 = c.mem_read32(r3), r8 = c.mem_read32(r4);
    while (((r7 - 0x01010101u) & 0x80808080u) == 0u) {
        if (r7 != r8) return {r7 <= r8 ? 0xFFFFFFFFu : 1u, r4};
        r7 = c.mem_read32(r3 += 4u);
        r8 = c.mem_read32(r4 += 4u);
    }
    r5 = c.mem_read8(r3);
    r0 = c.mem_read8(r4);
    if (r5 != r0) return {r5 - r0, r4};
    return bytes();
}

bool equal_strings(Cpu& c, u32 a, u32 b) { return msl_strcmp(c, a, b).r3 == 0u; }

// strstr(s, sub) != 0 on guest strings (fn_803AD710, a leaf): an empty `sub` matches.
bool contains(Cpu& c, u32 s, u32 sub) {
    if (c.mem_read8(sub) == 0u) return true;
    for (u32 at = s; c.mem_read8(at) != 0u; ++at) {
        for (u32 k = 0;; ++k) {
            const u8 want = c.mem_read8(sub + k);
            if (want == 0u) return true;
            if (c.mem_read8(at + k) != want) break;
        }
    }
    return false;
}

// The byte swap 0x80043D34 of one word in place (a leaf: lwz; stwbrx).
// Returns the word it read: the swap's loop leaves it in r4.
u32 swap_word(Cpu& c, u32 at) {
    const u32 v = c.mem_read32(at);
    c.mem_write32(at, (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24));
    return v;
}

float f32_at(Cpu& c, u32 at) {
    const u32 bits = c.mem_read32(at);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// fn_8004CD38 entered with the stack at `sp`, LR `lr`, registers `regs`. False when the run stopped.
bool parse(Cpu& c, u32 sp, u32 lr, Regs regs, u32 one) {
    const u32 frame = prologue(c, sp, lr, 0x20u);
    save_regs(c, frame + 0x14u, 29u, regs);
    regs[29 - 26] = one;
    // sscanf(data, "One Ver %f", frame + 8): the game's own, from the original's frame.
    const u32 cr = c.cr;
    c.cr &= ~kCrBit6;
    const bool ok = call_guest_from(c, frame, kParseSscanfSite, regs, kSscanf, c.mem_read32(one + oData),
                                    kVersionFormat, frame + 8u);
    c.cr = cr;
    if (!ok) return false;
    const float version = f32_at(c, frame + 8u);
    const bool legacy = version <= f32_at(c, kLegacyBound);  // fcmpo; cror eq, lt, eq: false for a NaN
    u32 r4 = swap_word(c, c.mem_read32(one + oData) + dCount);
    for (u32 i = 0; static_cast<s32>(i) < static_cast<s32>(count_of(c, one) + 2u); ++i) {
        r4 = i;  // Entry's index
        const u32 e = entry(c, one, i);
        if (e == 0u) continue;
        // lfs f1, 8(r1): the version, both halves (the interpreter's lfs)
        c.fpr[1].bits[0] = c.fpr[1].bits[1] = single_to_double_bits(c.mem_read32(frame + 8u));
        if (legacy) {
            c.mem_write32(e + eFlags, c.mem_read32(e + 0x28u));
            c.mem_write32(e + ePointer, c.mem_read32(e + 0x24u));
            c.mem_write32(e + eSize, c.mem_read32(e + 0x20u));
        }
        swap_word(c, e + eSize);
        swap_word(c, e + ePointer);
        r4 = swap_word(c, e + eFlags);
        c.mem_write32(e + ePointer, c.mem_read32(one + oData) + c.mem_read32(e + ePointer));
    }
    c.gpr[3] = count_of(c, one);  // the loop's last bound read
    c.gpr[4] = r4;
    return true;
}

// fn_8004CB48 entered with the stack at `sp`, LR `lr`, registers `regs`; the archive has no allocator object. The
// result, or nothing when the run stopped.
std::optional<u32> close(Cpu& c, u32 sp, u32 lr, Regs regs, u32 one) {
    const u32 frame = prologue(c, sp, lr, 0x10u);
    c.mem_write32(frame + 0xCu, regs[31 - 26]);  // stw r31, 0xC(r1)
    regs[31 - 26] = one;
    const u32 buffer = c.mem_read32(one + oBuffer);
    c.gpr[4] = buffer;  // lwz r4, 0x2C(r3)
    if (!is_open(c, one)) return 0u;
    if (buffer != 0u) {
        if (!call_guest_from(c, frame, kCloseFreeBufferSite, regs, kRwFree, buffer, buffer)) return std::nullopt;
        memset_zero(c, frame, kCloseMemsetSite, one, one + oBuffer, 8u);
    } else {
        const u32 allocation = c.mem_read32(one + oAllocation);
        if (!call_guest_from(c, frame, kCloseFreeAllocationSite, regs, kRwFree, allocation, allocation)) return std::nullopt;
    }
    c.mem_write32(one + oData, 0u);
    c.mem_write32(one + oAllocation, 0u);
    c.mem_write8(one + oName, 0u);
    return 1u;
}

// A guest call to the RW error code builder (varargs, no FP arguments: crclr cr1eq).
bool error_code(Cpu& c, u32 frame, u32 site, const Regs& regs, u32 code) {
    const u32 cr = c.cr;
    c.cr &= ~kCrBit6;
    const bool ok = call_guest_from(c, frame, site, regs, kErrorCode, code);
    c.cr = cr;
    return ok;
}

// fn_8047DD48: whether a chunk type is complex (its jump table's "li r3, n" for types up to 0x1A, else 0).
u32 complex_type(Cpu& c, u32 type) {
    if (type > 0x1Au) return 0u;
    return c.mem_read32(c.mem_read32(kComplexTable + type * 4u)) & 0xFFFFu;
}

// RwFread 0x8047B430(buffer, size, count, fp) called from `site` with the stack at `sp` and the caller's registers:
// its frame (r31 saved, then r31 = size), the read slot of fp's file system - RtGCNFS's read, through `read` - with
// size * count bytes, the result divided by size. Nothing when the run stopped.
std::optional<u32> rw_fread(Cpu& c, const NativeOne::SyncRead& read, u32 sp, u32 site, const Regs& regs, u32 buffer,
                            u32 size, u32 count, u32 fp) {
    const u32 frame = prologue(c, sp, site, 0x10u);
    c.mem_write32(frame + 0xCu, regs[31 - 26]);
    const u32 fs = c.mem_read32(fp + 0x38u);
    if (c.mem_read32(fs + 0x30u) != kRtGcnFsRead) {
        c.request_stop(StopReason::ExecutionFault, "native ONE layer: a read through a file system that is not RtGCNFS");
        return std::nullopt;
    }
    Regs slot_regs = regs;
    slot_regs[31 - 26] = size;
    const u32 caller_sp = c.gpr[1], caller_lr = c.lr;
    const Regs saved = regs_of(c);
    c.gpr[1] = frame;
    c.lr = kFreadSlotSite;
    for (u32 r = 26u; r <= 31u; ++r) c.gpr[r] = slot_regs[r - 26u];
    c.gpr[3] = fp;  // the slot entered as RwFread enters it: (fp, buffer, size * count)
    c.gpr[4] = buffer;
    c.gpr[5] = size * count;
    const std::optional<u32> got = read(c, fp, buffer, size * count);
    c.gpr[1] = caller_sp;
    c.lr = caller_lr;
    for (u32 r = 26u; r <= 31u; ++r) c.gpr[r] = saved[r - 26u];
    if (!got) return std::nullopt;
    return size != 0u ? *got / size : 0u;  // divwu (by 0: no caller reads with size 0)
}

// RwStreamRead 0x80481D9C(stream, buffer, size) for a file stream, entered with the stack at `sp`, LR `lr`, registers
// `regs`: its frame, RwFread 0x8047B430's, then the file system's read slot (RtGCNFS's read, through `read`). The bytes
// read, or nothing when the run stopped.
std::optional<u32> stream_read(Cpu& c, const NativeOne::SyncRead& read, u32 sp, u32 lr, Regs regs, u32 stream,
                               u32 buffer, u32 size) {
    const u32 frame = prologue(c, sp, lr, 0x40u);
    c.mem_write32(frame + 0x3Cu, regs[31 - 26]);
    c.mem_write32(frame + 0x38u, regs[30 - 26]);
    c.mem_write32(frame + 0x34u, regs[29 - 26]);
    const u32 type = c.mem_read32(stream);
    if (type != 1u && type != 2u) {
        c.request_stop(StopReason::ExecutionFault, "native ONE layer: Open read a stream that is not a file stream");
        return std::nullopt;
    }
    const u32 fp = c.mem_read32(stream + 0xCu);
    regs[31 - 26] = fp;
    regs[30 - 26] = buffer;
    regs[29 - 26] = size;
    // RwFread(buffer, 1, size, fp).
    const std::optional<u32> got = rw_fread(c, read, frame, kStreamFreadSite, regs, buffer, 1u, size, fp);
    if (!got) return std::nullopt;
    const u32 count = *got;
    if (count != size) {
        // Short: RwFeof, then the RW error (as RwStreamRead raises it).
        regs[30 - 26] = count;
        if (!call_guest_r3(c, frame, kStreamEofSite, regs, kFileEof, fp)) return std::nullopt;
        if (c.gpr[3] != 0u) {
            c.mem_write32(frame + 0x20u, 1u);
            if (!error_code(c, frame, kStreamEofCodeSite, regs, 5u)) return std::nullopt;
            c.mem_write32(frame + 0x24u, c.gpr[3]);
            if (!call_guest_from(c, frame, kStreamEofSetSite, regs, kErrorSet, frame + 0x20u)) return std::nullopt;
        } else {
            c.mem_write32(frame + 0x18u, 1u);
            if (!error_code(c, frame, kStreamShortCodeSite, regs, 0x8000001Au)) return std::nullopt;
            c.mem_write32(frame + 0x1Cu, c.gpr[3]);
            if (!call_guest_from(c, frame, kStreamShortSetSite, regs, kErrorSet, frame + 0x18u)) return std::nullopt;
        }
    }
    return count;
}

// fn_8047E99C RwStreamReadChunkHeaderInfo(stream, info): the 12-byte header read and byte-swapped, the version and
// build decoded, the info written (type, length, version, build, isComplex). The result (stream, or 0 on a short
// header), or nothing when the run stopped.
std::optional<u32> chunk_header(Cpu& c, const NativeOne::SyncRead& read, u32 sp, u32 lr, Regs regs, u32 stream,
                                u32 info) {
    const u32 frame = prologue(c, sp, lr, 0x50u);
    c.mem_write32(frame + 0x4Cu, regs[31 - 26]);
    c.mem_write32(frame + 0x48u, regs[30 - 26]);
    regs[31 - 26] = info;
    regs[30 - 26] = stream;
    const std::optional<u32> got = stream_read(c, read, frame, kHeaderReadSite, regs, stream, frame + 0x20u, 12u);
    if (!got) return std::nullopt;
    if (*got != 12u) {
        c.mem_write32(frame + 0x18u, 1u);
        if (!error_code(c, frame, kHeaderErrorCodeSite, regs, 0x8000001Au)) return std::nullopt;
        c.mem_write32(frame + 0x1Cu, c.gpr[3]);
        if (!call_guest_from(c, frame, kHeaderErrorSetSite, regs, kErrorSet, frame + 0x18u)) return std::nullopt;
        return 0u;
    }
    for (u32 i = 0; i < 3u; ++i) swap_word(c, frame + 0x20u + i * 4u);
    const u32 type = c.mem_read32(frame + 0x20u), length = c.mem_read32(frame + 0x24u), id = c.mem_read32(frame + 0x28u);
    c.mem_write32(frame + 0x2Cu, type);
    c.mem_write32(frame + 0x30u, length);
    u32 version = 0u, build = 0u;
    if ((id & 0xFFFF0000u) == 0u) {
        version = id << 8;
    } else {
        version = ((((id << 18) | (id >> 14)) & 0x0003FF00u) + 0x30000u) | ((id >> 16) & 0x3Fu);
        build = id & 0xFFFFu;
    }
    c.mem_write32(frame + 0x34u, version);
    c.mem_write32(frame + 0x38u, build);
    c.mem_write32(frame + 0x3Cu, complex_type(c, type));
    c.mem_write32(frame + 0x14u, type);
    c.mem_write32(frame + 0x10u, length);
    c.mem_write32(frame + 0x08u, build);
    c.mem_write32(frame + 0x0Cu, version);
    c.mem_write32(info + 0x0u, type);
    c.mem_write32(info + 0x4u, length);
    c.mem_write32(info + 0x8u, version);
    c.mem_write32(info + 0xCu, build);
    c.mem_write32(info + 0x10u, complex_type(c, type));
    c.gpr[4] = version;  // lwz r4, 0xC(r1) last, then fn_8047DD48 (a leaf) leaves it
    return stream;
}

// Whether a sync open may read without its yields: no other thread of the current thread's priority is ready.
bool reads_without_yields(Cpu& c) {
    const u32 thread = c.mem_read32(kCurrentThread);
    if (thread == 0u) return false;
    const u32 priority = c.mem_read32(thread + tPriority);
    if (priority > 31u) return false;
    return (c.mem_read32(c.gpr[13] + kSdaRunQueueBits) & (1u << (31u - priority))) == 0u;
}

// fn_8004CE1C Open(one, path) entered with the stack at `sp`, LR `lr`, registers `regs` (no allocator object; reads
// without yields allowed). The result, or nothing when the run stopped.
std::optional<u32> open(Cpu& c, const NativeOne::SyncRead& read, u32 sp, u32 lr, Regs regs, u32 one, u32 path,
                        u64& bytes) {
    const u32 frame = prologue(c, sp, lr, 0x30u);
    save_regs(c, frame + 0x20u, 28u, regs);
    regs[31 - 26] = one;
    regs[28 - 26] = path;
    if (c.mem_read8(one + oPending) != 0u) return 0u;
    if (path == 0u) {
        if (is_open(c, one)) return 1u;
        if (!close(c, frame, kOpenCloseSite1, regs, one)) return std::nullopt;
        return 0u;
    }
    if (const Strcmp r = msl_strcmp(c, one + oName, path); r.r3 == 0u) {  // strcmp, a leaf: already loaded
        c.gpr[4] = r.r4;
        return 1u;
    }
    if (!close(c, frame, kOpenCloseSite2, regs, one)) return std::nullopt;
    if (!call_guest_r3(c, frame, kOpenGetterSite, regs, kReadManagerGetter, c.gpr[3])) return std::nullopt;
    if (!call_guest_from(c, frame, kOpenStreamSite, regs, kStreamOpen, c.gpr[3], path)) return std::nullopt;
    const u32 stream = c.gpr[3];
    regs[29 - 26] = stream;
    if (stream == 0u) return 0u;
    if (!chunk_header(c, read, frame, kOpenHeaderSite, regs, stream, frame + 8u)) return std::nullopt;
    const u32 length = c.mem_read32(frame + 0xCu);
    regs[30 - 26] = length;
    if (!call_guest_r3(c, frame, kOpenPushSite, regs, kCategoryPush, 0x40u)) return std::nullopt;
    if (!call_guest_from(c, frame, kOpenAllocSite, regs, kRwAllocate, length, 2u)) return std::nullopt;
    const u32 buffer = c.gpr[3];
    regs[30 - 26] = buffer;
    if (!call_guest_r3(c, frame, kOpenPopSite, regs, kCategoryPop, c.gpr[3])) return std::nullopt;
    if (buffer == 0u) return 0u;  // the original leaks the stream here, as this does
    const std::optional<u32> got = stream_read(c, read, frame, kOpenReadSite, regs, stream, buffer, length);
    if (!got) return std::nullopt;
    c.mem_write32(frame + 0xCu, *got);
    bytes += *got;
    if (!call_guest_from(c, frame, kOpenCloseStreamSite, regs, kStreamClose, stream, 0u)) return std::nullopt;
    for (u32 k = 0;; ++k) {  // strcpy (a leaf, unbounded as the original)
        const u8 b = c.mem_read8(path + k);
        c.mem_write8(one + oName + k, b);
        if (b == 0u) break;
    }
    c.mem_write32(one + oAllocation, buffer);
    c.mem_write32(one + oDataSize, c.mem_read32(frame + 0xCu));
    c.mem_write32(one + oData, buffer);
    if (!parse(c, frame, kOpenParseSite, regs, one)) return std::nullopt;
    return 1u;
}

}  // namespace

// cmpwi / cmpw into CR0: LT, GT or EQ, and SO copied from XER.
void set_cr0(Cpu& c, s32 a, s32 b) {
    const u32 field = (a < b ? 8u : a > b ? 4u : 2u) | ((c.xer >> 31) & 1u);
    c.cr = (c.cr & 0x0FFFFFFFu) | (field << 28);
}

// The in-place byte swaps the loaders run over little-endian disc data (fonts, sky2, light, ONE directories): leaves,
// no stack. Every register they write is left as the original leaves it.
//   0x80043D0C(p, bytes): r0 = (bytes + 1) >> 1, CTR = r0, cmpwi bytes, 0; for bytes > 0 each halfword lhz / sthbrx,
//                         r3 past the last, r4 the last halfword read, CTR 0.
//   0x80043D34(p, bytes): the same for words (r0 = (bytes + 3) >> 2, lwz / stwbrx).
void swap_run(Cpu& c, u32 unit) {
    const u32 bytes = c.gpr[4];
    const u32 n = unit == 2u ? ((bytes + 1u) >> 1) : ((bytes + 3u) >> 2);  // addi; srwi (the 32-bit wrap kept)
    c.gpr[0] = n;
    c.ctr = n;
    set_cr0(c, static_cast<s32>(bytes), 0);
    if (static_cast<s32>(bytes) <= 0) return;  // blelr
    u32 p = c.gpr[3], last = 0u;
    for (u32 k = 0; k < n; ++k, p += unit) {
        if (unit == 2u) {
            last = c.mem_read16(p);
            c.mem_write16(p, bswap16(static_cast<u16>(last)));
        } else {
            last = c.mem_read32(p);
            c.mem_write32(p, bswap32(last));
        }
    }
    c.gpr[3] = p;
    c.gpr[4] = last;
    c.ctr = 0u;
}

// 0x80043C80(p, format, repeat): `repeat` times over the NUL-terminated format, per character (extsb; c - 'C' as an
// unsigned index into the jump table 0x8051E158, 0x36 entries): 'c' / 'C' skip a byte; 's' / 'S' align p to 2 and
// swap a halfword; 'f' 'F' 'i' 'I' 'x' 'X' align to 4 and swap a word; any other character does nothing. Leaves r3 =
// p, r6 = the NUL, r7 = the last unit read, r8 = the repeat index, r9 = the NUL's address, r0 = 0 after a pass,
// CTR = the last case taken, CR0 = cmpw r8, r5.
void swap_format(Cpu& c) {
    const u32 format = c.gpr[4];
    const s32 repeat = static_cast<s32>(c.gpr[5]);
    u32 p = c.gpr[3];
    s32 i = 0;
    for (; i < repeat; ++i) {
        u32 at = format;
        for (;;) {
            const u8 ch = c.mem_read8(at);
            c.gpr[6] = ch;
            c.gpr[0] = static_cast<u32>(static_cast<s32>(static_cast<s8>(ch)));  // extsb. r0, r6
            if (ch == 0u) break;
            const u32 index = static_cast<u32>(static_cast<s32>(static_cast<s8>(ch))) - 0x43u;
            if (index <= 0x35u) {
                const u32 target = c.mem_read32(0x8051E158u + index * 4u);  // lwzx from the table; mtctr
                c.gpr[0] = target;
                c.ctr = target;
                if (target == 0x80043CB8u) {  // 'c': addi r3, r3, 1
                    p += 1u;
                } else if (target == 0x80043CC0u) {  // 's'
                    const u32 aligned = (p + 1u) & ~1u;
                    c.gpr[0] = p + 1u;
                    c.gpr[7] = c.mem_read16(aligned);
                    c.mem_write16(aligned, bswap16(static_cast<u16>(c.gpr[7])));
                    p = aligned + 2u;
                } else if (target == 0x80043CD8u) {  // 'i' / 'f' / 'x'
                    const u32 aligned = (p + 3u) & ~3u;
                    c.gpr[0] = p + 3u;
                    c.gpr[7] = c.mem_read32(aligned);
                    c.mem_write32(aligned, bswap32(c.gpr[7]));
                    p = aligned + 4u;
                }
            }
            ++at;
        }
        c.gpr[9] = at;
    }
    c.gpr[3] = p;
    c.gpr[8] = static_cast<u32>(i);
    set_cr0(c, i, repeat);
}

bool NativeOne::install() {
    // Default on (user decision 2026-11-08, S4); GEKKO_NATIVE_ONE=0 selects the original ONE layer code.
    const char* env = std::getenv("GEKKO_NATIVE_ONE");
    if (env != nullptr && (env[0] != '1' || env[1] != '\0')) return false;
    enabled_ = true;
    Cpu& cpu = cpu_;
    cpu.install_hle(0x80043D0Cu, [this](Cpu& c) {
        ++stats_.swap16;
        swap_run(c, 2u);
        return true;
    }, "native ONE layer: byte swap (halfwords)");
    cpu.install_hle(0x80043D34u, [this](Cpu& c) {
        ++stats_.swap32;
        swap_run(c, 4u);
        return true;
    }, "native ONE layer: byte swap (words)");
    cpu.install_hle(0x80043C80u, [this](Cpu& c) {
        ++stats_.swap_format;
        swap_format(c);
        return true;
    }, "native ONE layer: byte swap by format string");
    // Each owner returns to its caller with r1, LR and r14..r31 as it found them and the result in r3.
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(
            address,
            [serve](Cpu& c) {
                const u32 sp = c.gpr[1], link = c.lr;
                const std::optional<u32> result = serve(c, sp, link);
                if (!result) return false;  // declined: the original words run
                c.gpr[1] = sp;
                c.lr = link;
                c.gpr[3] = *result;
                return true;
            },
            std::string("native ONE layer: ") + name);
    };
    own(kEntry, "Entry", [this](Cpu& c, u32, u32) -> std::optional<u32> {
        ++stats_.entry;
        return entry(c, c.gpr[3], c.gpr[4]);
    });
    own(kIsOpen, "IsOpen", [this](Cpu& c, u32, u32) -> std::optional<u32> {
        ++stats_.is_open;
        return is_open(c, c.gpr[3]) ? 1u : 0u;
    });
    own(kFindName, "FindName", [this](Cpu& c, u32 sp, u32 lr) -> std::optional<u32> {
        const u32 one = c.gpr[3];
        u32 name = c.gpr[4];
        ++stats_.find_name;
        const u32 frame = prologue(c, sp, lr, 0x50u);
        save_regs(c, frame + 0x44u, 29u, regs_of(c));
        if (!is_open(c, one) || name == 0u) return 0xFFFFFFFFu;
        // The uppercase copy at frame + 8: up to 44 bytes, the NUL included when it comes; a longer name leaves it
        // unterminated, and strcmp then reads on into the frame - as here, from guest memory.
        const u32 query = frame + 8u;
        for (u32 k = 0; k < kNameBuffer; ++k, ++name) {
            const u8 b = c.mem_read8(name);
            const s8 s = static_cast<s8>(b);
            c.mem_write8(query + k, (s >= 'a' && s <= 'z') ? static_cast<u8>(b - 0x20u) : b);
            if (c.mem_read8(query + k) == 0u) break;
        }
        c.gpr[4] = name;  // the copy's source pointer, left at the NUL (or 44 bytes on)
        for (u32 i = 2; static_cast<s32>(i) < static_cast<s32>(count_of(c, one) + 2u); ++i) {
            c.gpr[4] = i;  // Entry's index
            const u32 e = entry(c, one, i);
            if (e == 0u) break;
            const Strcmp r = msl_strcmp(c, e, query);
            c.gpr[4] = r.r4;
            if (r.r3 == 0u) return i;
        }
        return 0xFFFFFFFFu;
    });
    own(kFindSubstr, "FindSubstr", [this](Cpu& c, u32 sp, u32 lr) -> std::optional<u32> {
        const u32 one = c.gpr[3], sub = c.gpr[4], start = c.gpr[5];
        ++stats_.find_substr;
        const u32 frame = prologue(c, sp, lr, 0x20u);
        save_regs(c, frame + 0x10u, 28u, regs_of(c));
        if (!is_open(c, one) || sub == 0u) return 0xFFFFFFFFu;
        const u32 data = c.mem_read32(one + oData);
        for (u32 i = start; static_cast<s32>(i) < static_cast<s32>(c.mem_read32(data + dCount) + 2u); ++i) {
            c.gpr[4] = i;  // Entry's index
            const u32 e = entry(c, one, i);
            if (e == 0u) return 0xFFFFFFFFu;
            c.gpr[4] = sub;  // strstr leaves r4 as given
            if (contains(c, e, sub)) return i;
        }
        return 0xFFFFFFFFu;
    });
    own(kParse, "ParseDirectory", [this](Cpu& c, u32 sp, u32 lr) -> std::optional<u32> {
        const u32 one = c.gpr[3];
        if (!is_open(c, one)) {  // the original reads through a null directory: not served
            ++stats_.declined;
            return std::nullopt;
        }
        ++stats_.parse;
        if (!parse(c, sp, lr, regs_of(c), one)) return kStopped;
        return c.gpr[3];  // void: r3 as the original leaves it (the count)
    });
    own(kClose, "Close", [this](Cpu& c, u32 sp, u32 lr) -> std::optional<u32> {
        const u32 one = c.gpr[3];
        if (is_open(c, one) && c.mem_read32(one + oAllocator) != 0u) {  // frees through the allocator's vtable
            ++stats_.declined;
            return std::nullopt;
        }
        ++stats_.close;
        return close(c, sp, lr, regs_of(c), one).value_or(kStopped);
    });
    own(kAdopt, "Adopt", [this](Cpu& c, u32 sp, u32 lr) -> std::optional<u32> {
        const u32 one = c.gpr[3], buffer = c.gpr[4], size = c.gpr[5];
        if (is_open(c, one) && c.mem_read32(one + oAllocator) != 0u) {
            ++stats_.declined;
            return std::nullopt;
        }
        ++stats_.adopt;
        Regs regs = regs_of(c);
        const u32 frame = prologue(c, sp, lr, 0x20u);
        save_regs(c, frame + 0x14u, 29u, regs);
        regs[29 - 26] = one;
        regs[30 - 26] = buffer;
        regs[31 - 26] = size;
        if (!close(c, frame, kAdoptCloseSite, regs, one)) return kStopped;
        c.mem_write32(one + oBuffer, buffer);
        c.mem_write32(one + oBufferSize, size);
        c.mem_write32(one + oAllocation, buffer + 0xCu);
        c.mem_write32(one + oDataSize, size - 0xCu);
        c.mem_write32(one + oData, buffer + 0xCu);
        regs[30 - 26] = buffer + 0xCu;
        if (!parse(c, frame, kAdoptParseSite, regs, one)) return kStopped;
        return 1u;
    });
    own(kCtor, "One::One", [this](Cpu& c, u32 sp, u32 lr) -> std::optional<u32> {
        const u32 one = c.gpr[3], path = c.gpr[4], allocator = c.gpr[5];
        // With a path the ctor opens synchronously: as the Open owner, or the original when that would decline.
        if (path != 0u && (!sync_read_ || allocator != 0u || !reads_without_yields(c))) {
            if (sync_read_ && allocator == 0u) ++stats_.open_thread_ready;
            ++stats_.declined;
            return std::nullopt;
        }
        ++stats_.ctor;
        Regs regs = regs_of(c);
        const u32 frame = prologue(c, sp, lr, 0x10u);
        save_regs(c, frame + 0x8u, 30u, regs);
        c.mem_write32(one + oWord54, 0u);
        c.mem_write8(one + oByte50, 0u);
        c.mem_write32(one + oAllocator, allocator);
        c.mem_write8(one + oName, 0u);
        memset_zero(c, frame, kCtorMemset1Site, path, one + oBuffer, 8u);  // r31 = path there
        memset_zero(c, frame, kCtorMemset2Site, path, one + oAllocation, 8u);
        c.mem_write32(one + oData, 0u);
        c.mem_write8(one + oPending, 0u);
        if (path != 0u) {
            regs[30 - 26] = one;
            regs[31 - 26] = path;
            ++stats_.open;
            if (!open(c, sync_read_, frame, kCtorOpenSite, regs, one, path, stats_.open_bytes)) return kStopped;
        }
        return one;
    });
    // OpenAsync: the request block on its frame (+8: path, destination, size, the archive, OnRead; +0x1C is never
    // written, as in the original), Close, the callback fields, then the read manager's getter and the submit (game
    // code: the exists check and the queued node), from its frame.
    own(kOpenAsync, "OpenAsync", [this](Cpu& c, u32 sp, u32 lr) -> std::optional<u32> {
        const u32 one = c.gpr[3], path = c.gpr[4], callback = c.gpr[5], arg = c.gpr[6], dst = c.gpr[7], size = c.gpr[8];
        if (c.mem_read8(one + oPending) == 0u && is_open(c, one) && c.mem_read32(one + oAllocator) != 0u) {
            ++stats_.declined;  // its Close frees through the allocator's vtable
            return std::nullopt;
        }
        ++stats_.open_async;
        Regs regs = regs_of(c);
        const u32 frame = prologue(c, sp, lr, 0x40u);
        save_regs(c, frame + 0x28u, 26u, regs);
        regs[26 - 26] = one;
        regs[27 - 26] = path;
        regs[28 - 26] = callback;
        regs[29 - 26] = arg;
        regs[30 - 26] = dst;
        regs[31 - 26] = size;
        if (c.mem_read8(one + oPending) != 0u) return 0u;
        if (!close(c, frame, kAsyncCloseSite, regs, one)) return kStopped;
        c.mem_write32(frame + 0x08u, path);
        c.mem_write32(frame + 0x0Cu, dst);
        c.mem_write32(frame + 0x10u, size);
        c.mem_write32(frame + 0x18u, kOnRead);
        c.mem_write32(frame + 0x14u, one);
        c.mem_write32(one + oCallback, callback);
        c.mem_write32(one + oCallbackArg, arg);
        if (!call_guest_r3(c, frame, kAsyncGetterSite, regs, kReadManagerGetter, c.gpr[3])) return kStopped;
        if (!call_guest_from(c, frame, kAsyncSubmitSite, regs, kSubmit, c.gpr[3], frame + 0x08u, 1u)) return kStopped;
        if ((c.gpr[3] & 0xFFu) == 0u) return 0u;
        c.mem_write8(one + oPending, 1u);
        return 1u;
    });
    // OnRead (the completion's callback, r3 = the request block): Adopt, then the requester's callback entered as the
    // original's bctrl enters it - from OnRead's frame, LR = the word after the bctrl - so whatever the callback does
    // (yield included) needs no return into this owner; the owner at that word (below) runs OnRead's tail. A request
    // without a buffer (the original parses a closed archive: through 0) and an allocator object are declined.
    cpu.install_hle(kOnRead, [this](Cpu& c) {
        const u32 sp = c.gpr[1], lr = c.lr;
        const u32 request = c.gpr[3];
        const u32 buffer = c.mem_read32(request + 4u), one = c.mem_read32(request + 0xCu);
        if (buffer == 0u || (is_open(c, one) && c.mem_read32(one + oAllocator) != 0u)) {
            ++stats_.declined;
            return false;
        }
        ++stats_.on_read;
        Regs regs = regs_of(c);
        const u32 frame = prologue(c, sp, lr, 0x10u);
        c.mem_write32(frame + 0xCu, regs[31 - 26]);  // stw r31, 0xC(r1)
        regs[31 - 26] = one;
        // Adopt(one, buffer, size) from this frame (its own frame and its Close's and parse's below).
        {
            const u32 size = c.mem_read32(request + 8u);
            const u32 adopt_frame = prologue(c, frame, kOnReadAdoptSite, 0x20u);
            save_regs(c, adopt_frame + 0x14u, 29u, regs);
            Regs adopt = regs;
            adopt[29 - 26] = one;
            adopt[30 - 26] = buffer;
            adopt[31 - 26] = size;
            if (!close(c, adopt_frame, 0x8004CAE0u, adopt, one)) return true;
            c.mem_write32(one + oBuffer, buffer);
            c.mem_write32(one + oBufferSize, size);
            c.mem_write32(one + oAllocation, buffer + 0xCu);
            c.mem_write32(one + oDataSize, size - 0xCu);
            c.mem_write32(one + oData, buffer + 0xCu);
            adopt[30 - 26] = buffer + 0xCu;
            if (!parse(c, adopt_frame, 0x8004CB08u, adopt, one)) return true;
        }
        const u32 callback = c.mem_read32(one + oCallback);
        if (callback == 0u) {  // the tail at once, back to OnRead's caller
            c.mem_write8(one + oPending, 0u);
            c.gpr[1] = sp;
            c.lr = lr;
            c.gpr[3] = 1u;  // Adopt's result (r4 and f1: the parse's)
            return true;
        }
        // The callback: r1 = OnRead's frame, r31 = the archive, r3 = its argument, LR = the word after the bctrl.
        c.gpr[1] = frame;
        c.gpr[31] = one;
        c.gpr[3] = c.mem_read32(one + oCallbackArg);
        c.gpr[12] = callback;
        c.ctr = callback;
        c.lr = kOnReadReturn;
        c.leave_to_dispatcher(callback, FallbackReason::Yield, kOnReadCallbackSite);
        return true;  // served: the dispatcher resumes at the callback (a leave keeps pc), not at LR
    }, "native ONE layer: OnRead");
    // OnRead's tail, where the callback returns: the pending byte cleared, then the epilogue (r31 and LR from the
    // frame) back to OnRead's caller.
    cpu.install_hle(
        kOnReadReturn,
        [this](Cpu& c) {
            ++stats_.on_read_tail;
            const u32 frame = c.gpr[1];
            c.mem_write8(c.gpr[31] + oPending, 0u);
            c.gpr[31] = c.mem_read32(frame + 0xCu);
            c.lr = c.mem_read32(frame + 0x14u);
            c.gpr[1] = frame + 0x10u;
            return true;  // resumes at LR: OnRead's caller
        },
        "native ONE layer: OnRead's tail (after the requester's callback)");

    // The TOneFileAsync<T> loaders' updates (task, r4): one state per call, frame-exact, game code for the manager,
    // the heap, the extract, the RW streams and readers; the requester's callback entered as the original's bctrl
    // (LR = the word after it; the owner there runs the tail).
    for (const Loader& loader : kLoaders) {
        const Loader* const l = &loader;
        cpu.install_hle(l->update, [this, l](Cpu& c) {
            const u32 task = c.gpr[3], arg = c.gpr[4], sp = c.gpr[1], link = c.lr;
            ++stats_.loader_updates;
            Regs regs = regs_of(c);
            const u32 frame = prologue(c, sp, link, 0x10u);
            save_regs(c, frame + 0x8u, 30u, regs);
            regs[31 - 26] = task;
            regs[30 - 26] = arg;
            const auto back = [&](u32 result) {
                c.gpr[1] = sp;
                c.lr = link;
                c.gpr[3] = result;
                return true;
            };
            const auto call = [&](u32 site, u32 function, u32 r3, u32 r4 = 0u, u32 r5 = 0u, u32 r6 = 0u, u32 r7 = 0u) {
                return call_guest_from(c, frame, site, regs, function, r3, r4, r5, r6, r7);
            };
            const auto call1 = [&](u32 site, u32 function, u32 r3) {  // the original loads only r3
                return call_guest_r3(c, frame, site, regs, function, r3);
            };
            const s32 state = static_cast<s32>(c.mem_read32(task + kState));
            if (state == 0) {
                if (!call1(l->sites[0], kLoaderManager, c.gpr[3])) return true;
                if (!call1(l->sites[1], kLoaderCount, c.gpr[3])) return true;
                if (static_cast<s32>(c.gpr[3]) >= static_cast<s32>(kLoaderJobLimit)) return back(0u);
                const auto member_size = [&] {  // Entry (a leaf) +0x2C, read even when there is no entry
                    return c.mem_read32(entry(c, c.mem_read32(task + kArchive), c.mem_read32(task + kIndex)) + eSize);
                };
                if (!call(l->sites[2], kRwAllocate, (member_size() + 0x401Fu) & ~0x1Fu, 2u)) return true;
                c.mem_write32(task + kBuffer, c.gpr[3]);
                if (c.mem_read32(task + kBuffer) != 0u) {
                    if (!call1(l->sites[3], kRwFree, c.mem_read32(task + kBuffer))) return true;
                    if (!call(l->sites[4], kRwAllocate, (member_size() + 0x1Fu) & ~0x1Fu, 2u)) return true;
                    c.mem_write32(task + kBuffer, c.gpr[3]);
                }
                if (c.mem_read32(task + kBuffer) == 0u) {
                    if (!call(l->sites[5], kRwAllocate, (member_size() + 0x1Fu) & ~0x1Fu, 1u)) return true;
                    c.mem_write32(task + kBuffer, c.gpr[3]);
                }
                if (!call1(l->sites[6], kLoaderManager, c.gpr[3])) return true;
                if (!call(l->sites[7], kLoaderRegister, c.gpr[3], task, c.mem_read32(task + kArchive),
                          c.mem_read32(task + kIndex)))
                    return true;
                if (c.mem_read32(task + kTaskCallback) != 0u) {
                    if (!call(l->sites[8], kExtractFn, c.mem_read32(task + kArchive), c.mem_read32(task + kIndex),
                              c.mem_read32(task + kBuffer), l->done, task))
                        return true;
                    c.mem_write8(task + kAsync, 1u);
                } else {
                    if (!call(l->sites[9], kExtractFn, c.mem_read32(task + kArchive), c.mem_read32(task + kIndex),
                              c.mem_read32(task + kBuffer), 0u, 0u))
                        return true;
                    c.mem_write32(task + kSize, c.gpr[3]);
                    c.mem_write8(task + kAsync, 0u);
                }
                c.mem_write32(task + kState, 1u);
                return back(0u);
            }
            if (state == 1) {
                if (c.mem_read8(task + kAsync) != 0u) return back(0u);
                if (c.mem_read32(task + kSize) == 0u) {
                    if (!call1(l->sites[10], kRwFree, c.mem_read32(task + kBuffer))) return true;
                    c.mem_write32(task + kState, 4u);
                    return back(0u);
                }
                c.mem_write32(task + kState, 2u);
                if (!call1(l->sites[11], kLoaderManager, c.gpr[3])) return true;
                if (!call(l->sites[12], kLoaderUnregister, c.gpr[3], task)) return true;
                return back(0u);
            }
            if (state == 2) {
                c.mem_write32(task + kMemory, c.mem_read32(task + kBuffer));
                if (!call(l->sites[13], kStreamOpenFn, 3u, 1u, task + kMemory)) return true;
                c.mem_write32(task + kStream, c.gpr[3]);
                if (l->chunk == 0u) {  // raw: the buffer is the result
                    c.mem_write32(task + kResult, c.mem_read32(task + kBuffer));
                    c.mem_write32(task + kState, 4u);
                    return back(0u);
                }
                if (l->texdict) regs[30 - 26] = c.mem_read32(task + kTexDict);
                if (!call(l->sites[14], kStreamFindChunk, c.mem_read32(task + kStream), l->chunk, 0u, 0u)) return true;
                if (c.gpr[3] != 0u) {
                    if (l->texdict && !call1(l->sites[15], kTexDictSetCurrent, regs[30 - 26])) return true;
                    const bool ok = l->reader_arg != 0u
                                        ? call(l->sites[16], l->reader, l->reader_arg, c.mem_read32(task + kStream))
                                        : call1(l->sites[16], l->reader, c.mem_read32(task + kStream));
                    if (!ok) return true;
                    c.mem_write32(task + kResult, c.gpr[3]);
                    if (l->after != 0u && !call1(l->sites[17], l->after, c.mem_read32(task + kResult))) return true;
                }
                c.mem_write32(task + kState, 4u);
                return back(0u);
            }
            if (state == 3) {
                if (c.mem_read8(task + kWait) != 0u) c.mem_write32(task + kState, 4u);
                return back(0u);
            }
            if (state != 4) return back(0u);
            const Strcmp same = msl_strcmp(c, c.mem_read32(l->name_word), c.mem_read32(kSameName));
            c.gpr[4] = same.r4;
            if (same.r3 != 0u && c.mem_read32(task + kBuffer) != 0u) {
                if (!call1(l->sites[18], kRwFree, c.mem_read32(task + kBuffer))) return true;
                c.mem_write32(task + kBuffer, 0u);
            }
            if (const u32 stream = c.mem_read32(task + kStream); stream != 0u) {
                if (!call(l->sites[19], kStreamClose, stream, 0u)) return true;
            }
            if (arg != 0u) c.mem_write32(arg, c.mem_read32(task + kResult));
            const u32 callback = c.mem_read32(task + kTaskCallback);
            if (callback == 0u) {
                c.mem_write16(task + kFlags, static_cast<u16>(c.mem_read16(task + kFlags) | 1u));
                return back(1u);
            }
            // The callback: r1 = the frame, r30 / r31 as the original's, r3 = the result, r4 = its argument.
            c.gpr[1] = frame;
            c.gpr[30] = regs[30 - 26];
            c.gpr[31] = task;
            c.gpr[3] = c.mem_read32(task + kResult);
            c.gpr[4] = c.mem_read32(task + kTaskArg);
            c.gpr[12] = callback;
            c.ctr = callback;
            c.lr = l->sites[20];
            ++stats_.loader_callbacks;
            c.leave_to_dispatcher(callback, FallbackReason::Yield, l->sites[20] - 4u);
            return true;
        }, std::string("native ONE layer: TOneFileAsync<") + l->name + ">::update");
        // The tail after the callback: the archive's and the global count - 1, the task done, -> 1.
        cpu.install_hle(l->sites[20], [this](Cpu& c) {
            const u32 frame = c.gpr[1], task = c.gpr[31];
            const u32 one = c.mem_read32(task + kArchive);
            c.gpr[4] = one;  // lwz r4, 0x40(r31)
            c.mem_write32(one + oArchiveJobs, c.mem_read32(one + oArchiveJobs) - 1u);
            c.mem_write32(kLoaderJobs, c.mem_read32(kLoaderJobs) - 1u);
            c.mem_write16(task + kFlags, static_cast<u16>(c.mem_read16(task + kFlags) | 1u));
            c.gpr[3] = 1u;
            c.gpr[30] = c.mem_read32(frame + 0x8u);
            c.gpr[31] = c.mem_read32(frame + 0xCu);
            c.lr = c.mem_read32(frame + 0x14u);
            c.gpr[1] = frame + 0x10u;
            ++stats_.loader_tails;
            return true;
        }, std::string("native ONE layer: TOneFileAsync<") + l->name + ">::update's tail");
    }
    // 0x800450C4(manager, path, &size): RwFopen, RwMalloc of its length, the whole file by RwFread (through read_sync),
    // the size stored, RwFclose; -> the buffer (0 when the file is not there).
    own(kWholeRead, "whole-file read", [this](Cpu& c, u32 sp, u32 lr) -> std::optional<u32> {
        const u32 path = c.gpr[4], size_at = c.gpr[5];
        if (!sync_read_ || !reads_without_yields(c)) {
            if (sync_read_) ++stats_.whole_thread_ready;
            ++stats_.declined;
            return std::nullopt;
        }
        ++stats_.whole_reads;
        Regs regs = regs_of(c);
        const u32 frame = prologue(c, sp, lr, 0x20u);
        save_regs(c, frame + 0x14u, 29u, regs);
        regs[29 - 26] = size_at;
        regs[30 - 26] = 0u;
        if (!call_guest_from(c, frame, kWholeOpenSite, regs, kFileOpen, path, kReadMode)) return kStopped;
        const u32 fp = c.gpr[3];
        regs[31 - 26] = fp;
        if (fp == 0u) return 0u;
        const u32 length = c.mem_read32(fp + 4u);
        if (!call_guest_from(c, frame, kWholeAllocSite, regs, kRwAllocate, length, 2u)) return kStopped;
        const u32 buffer = c.gpr[3];
        regs[30 - 26] = buffer;
        const std::optional<u32> count = rw_fread(c, sync_read_, frame, kWholeReadSite, regs, buffer, length, 1u, fp);
        if (!count) return kStopped;
        const u32 size = c.mem_read32(fp + 4u) * *count;
        stats_.whole_bytes += size;
        if (size_at != 0u) c.mem_write32(size_at, size);
        if (!call_guest_r3(c, frame, kWholeCloseSite, regs, kFileClose, fp)) return kStopped;
        return buffer;
    });
    own(kOpen, "Open", [this](Cpu& c, u32 sp, u32 lr) -> std::optional<u32> {
        const u32 one = c.gpr[3], path = c.gpr[4];
        if (!sync_read_ || c.mem_read32(one + oAllocator) != 0u || !reads_without_yields(c)) {
            if (sync_read_ && c.mem_read32(one + oAllocator) == 0u) ++stats_.open_thread_ready;
            ++stats_.declined;
            return std::nullopt;
        }
        ++stats_.open;
        return open(c, sync_read_, sp, lr, regs_of(c), one, path, stats_.open_bytes).value_or(kStopped);
    });
    own(kDtor, "One::~One", [this](Cpu& c, u32 sp, u32 lr) -> std::optional<u32> {
        const u32 one = c.gpr[3], flags = c.gpr[4];
        if (one != 0u && is_open(c, one) && c.mem_read32(one + oAllocator) != 0u) {
            ++stats_.declined;
            return std::nullopt;
        }
        ++stats_.dtor;
        Regs regs = regs_of(c);
        const u32 frame = prologue(c, sp, lr, 0x10u);
        save_regs(c, frame + 0x8u, 30u, regs);
        if (one == 0u) return 0u;
        regs[30 - 26] = one;
        regs[31 - 26] = flags;
        if (!close(c, frame, kDtorCloseSite, regs, one)) return kStopped;
        if (static_cast<s16>(flags) > 0 && !call_guest_r3(c, frame, kDtorDeleteSite, regs, kDelete, one)) return kStopped;
        return one;
    });
    return true;
}

}  // namespace gekko
