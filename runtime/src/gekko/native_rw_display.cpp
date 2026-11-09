// The RenderWare GameCube driver's display swap (gekko/native_rw_display.h; docs/agents/vi.md "Step 12").
//
//   0x80495CBC ShowRaster                  the frame copy and the swap ring
//   0x80494A2C draw-sync callback          GXSetDrawSyncCallback's, advances the ring
//   0x80494B0C VI pre-retrace callback     flips the shown buffer when its token was reached
//   0x80494BE8 VI post-retrace callback    the ring advance seen from the retrace side
//   0x80494CD4 video mode setter           (descriptor, count)
//   0x80494EDC GcnSystem                   the device system function: 23 requests
//   0x804981E8 / 0x8049847C                render-state defaults / context teardown
//
// Every owner is the original's arithmetic over the same guest words. The SDK leaves it called (GX, VI, OS) are owners of the
// lifted build and are entered as owners (serve_owner); RenderWare core and the engine's memory functions are guest calls.
// The waits (ShowRaster's full ring, VIWaitForRetrace in the mode change, the busy wait of request 3) are the native wait of
// os.threads (OSSleepThread's owner / Cpu::idle_wait): the game clock runs to the next event, no spinning. Guest code runs in
// zero game time, so nothing becomes deliverable inside an owner except through a wait; the interrupt gates are therefore kept
// only where the original takes a pending interrupt (the entry, and the restore after a sleep).
#include "gekko/native_guest_call.h"
#include "gekko/native_rw_raster.h"
#include "gekko/ppc_ops.h"

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

namespace gekko::rw_raster {

namespace {

// ---- addresses
constexpr u32 kShowRaster = 0x80495CBCu, kShowRasterAfterRestore = 0x80495D38u;   // the row after OSRestoreInterrupts: an interrupt taken there resumes at it
constexpr u32 kDrawSyncCallback = 0x80494A2Cu, kPreRetraceCallback = 0x80494B0Cu, kPostRetraceCallback = 0x80494BE8u;
constexpr u32 kVideoMode = 0x80494CD4u, kGcnSystem = 0x80494EDCu;
constexpr u32 kStateDefaults = 0x804981E8u, kContextTeardown = 0x8049847Cu;

// OS / VI / GX leaves (owners of the lifted build)
constexpr u32 kSleepThread = 0x803792C4u, kWakeupThread = 0x803793B0u, kInitThreadQueue = 0x8037847Cu;
constexpr u32 kGxFlush = 0x80396098u, kGxDrawDone = 0x803952FCu, kGxGetDrawSync = 0x80394EA4u, kGxSetDrawSyncToken = 0x80394F80u,
              kGxStopDrawSync = 0x80395034u, kGxSetDrawSyncCallback = 0x80394F3Cu, kGxCopyDisp = 0x80397444u, kGxSetDrawSync = 0x80396428u,
              kGxReadDrawSync = 0x803964DCu, kGxSetCopyFilterMode = 0x80396E54u, kGxSetDispCopySrc = 0x80396BD4u,
              kGxSetDispCopyDst = 0x80396CCCu, kGxSetCopyFilter = 0x80397228u, kGxSetDispCopyYScale = 0x803970E4u,
              kGxSetDispCopyFrame = 0x80399FCCu, kGxCopyClear = 0x80399E58u, kGxSetDispCopyGamma = 0x80397430u,
              kGxFifoCreate = 0x80393A88u, kGxFifoBase = 0x80394AF4u, kGxFifoReadPtr = 0x80394BDCu, kGxFifoWritePtr = 0x80394D04u,
              kGxViewportJitter = 0x8039A444u, kGxViewport = 0x8039A49Cu, kGxScissor = 0x8039A4E4u, kGxScissorOffset = 0x8039A55Cu;
constexpr u32 kViConfigure = 0x8037F19Cu, kViFlush = 0x8037FD38u, kViSetNextFrameBuffer = 0x8037FE68u, kViSetBlack = 0x8037FEDCu,
              kViGetNextField = 0x80380004u, kViGetTvFormat = 0x80380138u, kViSetPreRetraceCallback = 0x8037E4F0u,
              kViSetPostRetraceCallback = 0x8037E534u;
constexpr u32 kTokenReached = 0x8049B7DCu, kCacheRange = 0x80372508u;
// state setters of 0x804981E8
constexpr u32 kGxState2 = 0x80399E24u, kGxState3 = 0x80399778u, kGxState4 = 0x80399D44u, kGxState5 = 0x80396B54u,
              kGxState6 = 0x80397CBCu, kGxState7 = 0x80397B98u, kGxState8 = 0x80399D98u, kGxState9 = 0x80399DC4u, kGxState10 = 0x803971B0u,
              kGxState11 = 0x8039A2CCu;
// RenderWare core / C library (guest_call)
constexpr u32 kErrorFormat = 0x8047EC78u, kErrorSet = 0x8047EBD4u,
              kContextCreate = 0x8048DD2Cu, kContextRelease = 0x8048DA50u, kContextDestroy = 0x8048DDDCu, kScratchCreate = 0x8048AEC4u,
              kScratchLock = 0x8048AFC0u, kScratchUnlock = 0x8048AB90u, kScratchFree = 0x8048AC2Cu, kMemset = 0x8000540Cu;

// ---- SDA words (r13-relative)
constexpr u32 kSdaRasterPlugin = 0x6200u, kSdaScratch = 0x6208u, kSdaContext = 0x620Cu, kSdaTexturePlugin = 0x6218u, kSdaEngine = 0x615Cu;
constexpr u32 kSdaColour = 0x6188u, kSdaFbCount = 0x618Cu, kSdaFbCount2 = 0x6190u, kSdaModeKind = 0x6194u, kSdaDoubleField = 0x6198u,
              kSdaArenaRaw = 0x619Cu, kSdaBase0 = 0x61A0u, kSdaBase1 = 0x61A4u, kSdaDraw = 0x61A8u, kSdaShown = 0x61ACu,
              kSdaRingRead = 0x61B0u, kSdaRingWrite = 0x61B4u, kSdaViWrite = 0x61B8u, kSdaViRead = 0x61BCu, kSdaCbInstalled = 0x61C0u,
              kSdaPending = 0x61C4u, kSdaHandshakeA = 0x61C8u, kSdaHandshakeB = 0x61CCu, kSdaField = 0x61D0u, kSdaFifoMade = 0x61D4u,
              kSdaQueue = 0x61D8u, kSdaViTokens = 0x61E0u, kSdaArena = 0x61E8u, kSdaFifo2 = 0x61ECu, kSdaHalfWidth = 0x61F0u,
              kSdaMode = 0x61F4u, kSdaDeviceTail = 0x61F8u, kSdaTokenRead = 0x6220u;
constexpr u32 kSdaBlackPending = 0x2AE0u, kSdaRingSize = 0x2AE4u, kSdaSinceSwap = 0x2AE8u, kSdaMinSwap = 0x2AE9u, kSdaToken = 0x2AF0u,
              kSdaFirstHalf = 0x2AD8u, kSdaExtra = 0x2ADCu, kSdaRetraceCount = 0x5BCCu, kSdaRetraceQueue = 0x5BD4u;
constexpr u32 kSwapTable = 0x805E4238u;        // 8 bytes per slot: { buffer base, token }
constexpr u32 kModeStore = 0x805E4250u;        // the caller's descriptor copied (0x3C bytes)
constexpr u32 kStaticModes = 0x8056F408u;      // 4 x 24 bytes of mode data; +0x48 / +0x4C / +0x50 are the mirror words
constexpr u32 kDeviceRecord = 0x8056F50Cu;     // the device record (0x804960F0 returns it)
constexpr u32 kStateRecord = 0x805E42D0u;
constexpr u32 kFallbackFn = 0x80494CCCu;       // "li r3, 0; blr": the default standard function
constexpr u32 kStdFunctionTable = 0x8051D400u;
constexpr u32 kMsrEE = 0x8000u;

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "native RW display: " + why);
}

u32 rd(Cpu& c, u32 sda) { return c.mem_read32(c.gpr[13] + sda); }
void wr(Cpu& c, u32 sda, u32 value) { c.mem_write32(c.gpr[13] + sda, value); }

// divw / mullw / subf: x mod n the way the compiler spells it (n == 0 is undefined in the original)
bool modulo(Cpu& c, u32 x, u32 n, u32& out) {
    if (n == 0u) {
        stop(c, "the swap ring size is 0 (the original divides by it)");
        return false;
    }
    const std::int64_t a = static_cast<std::int32_t>(x), d = static_cast<std::int32_t>(n);
    out = static_cast<u32>(a - (a / d) * d);
    return true;
}

bool done(Cpu& c) { return c.stopped() || c.stop_requested(); }

// The lifted build lowers two call sites of these bodies into the renderer (lift-sites-r2.txt, now retired with the bodies): the single-field
// GXCopyDisp of ShowRaster (render_submit_frame(xfb, clear)) and the GXSetZMode(1, LEQUAL, 1) of 0x804981E8 (rw_render_state_reset). The owner calls
// the same hooks; every other GX / VI call is an owner entry.
DisplayHooks g_hooks;

// single-precision value into FPR n (as lfs does)
void set_single(Cpu& c, unsigned n, u32 bits) { c.fpr[n].bits[0] = single_to_double_bits(bits); }
void set_float(Cpu& c, unsigned n, float value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    set_single(c, n, bits);
}

// ---- the draw-sync / retrace side of the swap ring ---------------------------------------------------------------------------
// 0x80494A60..0x80494AEC (and the same words at 0x80494C10..0x80494CA8): advance the ring read index, hand the next token to the GX
// owner (or stop the callback when the ring is empty), wake the waiter, and publish "a swap is pending".
bool ring_advance(Cpu& c) {
    u32 next = 0;
    if (!modulo(c, rd(c, kSdaRingRead) + 1u, rd(c, kSdaRingSize), next)) return false;
    if (next == rd(c, kSdaRingWrite)) {
        if (!c.serve_owner(kGxStopDrawSync) || done(c)) return false;
        wr(c, kSdaCbInstalled, 0u);
    } else {
        c.gpr[3] = c.mem_read32(kSwapTable + next * 8u + 4u);
        if (!c.serve_owner(kGxSetDrawSyncToken) || done(c)) return false;
    }
    wr(c, kSdaRingRead, next);
    c.gpr[3] = c.gpr[13] + kSdaQueue;
    if (!c.serve_owner(kWakeupThread) || done(c)) return false;
    if (rd(c, kSdaDoubleField) == 0u) {
        wr(c, kSdaPending, 1u);
    } else if (rd(c, kSdaField) == 1u) {
        wr(c, kSdaPending, 1u);
        wr(c, kSdaField, 0u);
    } else {
        wr(c, kSdaField, 1u);
    }
    return true;
}

// 0x80494A2C: the draw-sync callback.
void draw_sync_callback(Cpu& c) {
    const u32 read = rd(c, kSdaRingRead);
    if (c.mem_read32(kSwapTable + read * 8u) == rd(c, kSdaShown)) {
        wr(c, kSdaHandshakeA, 1u);
        return;
    }
    (void)ring_advance(c);
}

// 0x80494B0C: the VI pre-retrace callback.
void pre_retrace_callback(Cpu& c) {
    const u32 token = c.mem_read16(c.gpr[13] + kSdaViTokens + 2u * rd(c, kSdaViRead));
    const u32 since = (c.mem_read8(c.gpr[13] + kSdaSinceSwap) + 1u) & 0xFFu;
    c.mem_write8(c.gpr[13] + kSdaSinceSwap, static_cast<u8>(since));
    if (rd(c, kSdaPending) != 1u) return;
    c.gpr[3] = token;
    if (!c.serve_owner(kTokenReached) || done(c)) return;
    if (c.gpr[3] == 0u) return;
    if (c.mem_read8(c.gpr[13] + kSdaSinceSwap) < c.mem_read8(c.gpr[13] + kSdaMinSwap)) return;
    c.mem_write8(c.gpr[13] + kSdaSinceSwap, 0u);
    u32 buffer = rd(c, kSdaBase0);
    if (rd(c, kSdaShown) == buffer) buffer = rd(c, kSdaBase1);
    wr(c, kSdaShown, buffer);
    c.gpr[3] = buffer;
    if (!c.serve_owner(kViSetNextFrameBuffer) || done(c)) return;
    if (rd(c, kSdaBlackPending) != 0u) {
        c.gpr[3] = 0u;
        if (!c.serve_owner(kViSetBlack) || done(c)) return;
        wr(c, kSdaBlackPending, 0u);
    }
    if (!c.serve_owner(kViFlush) || done(c)) return;
    u32 next = 0;
    if (!modulo(c, rd(c, kSdaViRead) + 1u, rd(c, kSdaRingSize), next)) return;
    wr(c, kSdaHandshakeB, 1u);
    wr(c, kSdaViRead, next);
    wr(c, kSdaPending, 0u);
}

// 0x80494BE8: the VI post-retrace callback.
void post_retrace_callback(Cpu& c) {
    if (rd(c, kSdaHandshakeA) != 0u && rd(c, kSdaHandshakeB) != 0u) {
        if (!ring_advance(c)) return;
        wr(c, kSdaHandshakeA, 0u);
    }
    wr(c, kSdaHandshakeB, 0u);
}

// ---- ShowRaster ------------------------------------------------------------------------------------------------------------------
u32 next_token(u32 current) {   // (current + 1) mod 0xE000 by the original's multiply-high idiom
    const std::int64_t r5 = static_cast<std::int32_t>(current + 1u);
    const std::int64_t high = (static_cast<std::int64_t>(static_cast<std::int32_t>(0x92492493u)) * r5) >> 32;
    std::int32_t r3 = static_cast<std::int32_t>(high + r5) >> 15;
    r3 = r3 + static_cast<std::int32_t>(static_cast<u32>(r3) >> 31);
    return static_cast<u32>(static_cast<u32>(r5) - static_cast<u32>(r3) * 0xE000u);
}

bool toggle_draw(Cpu& c) {
    u32 other = rd(c, kSdaBase0);
    if (rd(c, kSdaDraw) == other) other = rd(c, kSdaBase1);
    wr(c, kSdaDraw, other);
    return true;
}

// Queue the buffer being drawn into with the token GXGetDrawSync-style leaf returned (sp + 0xC); `viToken` also records the draw-sync
// token the retrace side checks.
bool queue_frame(Cpu& c, u32 sp, bool viToken) {
    const u32 write = rd(c, kSdaRingWrite);
    c.mem_write32(kSwapTable + write * 8u, rd(c, kSdaDraw));
    c.mem_write32(kSwapTable + write * 8u + 4u, c.mem_read32(sp + 0xCu));
    if (viToken) c.mem_write16(c.gpr[13] + kSdaViTokens + 2u * rd(c, kSdaViWrite), static_cast<u16>(c.mem_read16(c.gpr[13] + kSdaToken)));
    u32 next = 0;
    if (!modulo(c, write + 1u, rd(c, kSdaRingSize), next)) return false;
    wr(c, kSdaRingWrite, next);
    if (viToken) {
        if (!modulo(c, rd(c, kSdaViWrite) + 1u, rd(c, kSdaRingSize), next)) return false;
        wr(c, kSdaViWrite, next);
    }
    return true;
}

bool install_callback_once(Cpu& c, u32 sp) {
    if (rd(c, kSdaCbInstalled) != 0u) return true;
    wr(c, kSdaCbInstalled, 1u);
    c.gpr[3] = c.mem_read32(sp + 0xCu);
    return c.serve_owner(kGxSetDrawSyncToken) && !done(c);
}

bool advance_token(Cpu& c) {   // GXSetDrawSync(token); token = (token + 1) mod 0xE000
    c.gpr[3] = c.mem_read16(c.gpr[13] + kSdaToken);
    if (!c.serve_owner(kGxSetDrawSync) || done(c)) return false;
    c.mem_write16(c.gpr[13] + kSdaToken, static_cast<u16>(next_token(c.mem_read16(c.gpr[13] + kSdaToken))));
    return true;
}

bool copy_frame(Cpu& c, u32 destination) {   // GXCopyDisp(destination, colour & 0xFF)
    c.gpr[3] = destination;
    c.gpr[4] = rd(c, kSdaColour) & 0xFFu;
    return c.serve_owner(kGxCopyDisp) && !done(c);
}

bool get_token(Cpu& c, u32 sp) {   // GXFlush; 0x803952FC; 0x80394EA4(r4 = sp + 0x10, r5 = sp + 0xC)
    if (!c.serve_owner(kGxFlush) || done(c)) return false;
    if (!c.serve_owner(kGxDrawDone) || done(c)) return false;
    c.gpr[4] = sp + 0x10u;
    c.gpr[5] = sp + 0xCu;
    return c.serve_owner(kGxGetDrawSync) && !done(c);
}

// The part of ShowRaster after its first OSRestoreInterrupts. r1 is the frame (sp0 - 0x20).
bool show_raster_rest(Cpu& c) {
    const u32 sp = c.gpr[1], sp0 = sp + 0x20u;
    if (rd(c, kSdaDoubleField) == 0u) {
        if (!get_token(c, sp) || !queue_frame(c, sp, true) || !install_callback_once(c, sp)) return false;
        (void)g_hooks.submit_frame(c, rd(c, kSdaDraw), rd(c, kSdaColour) & 0xFFu);   // the lowered GXCopyDisp site
        if (done(c) || !advance_token(c)) return false;
        if (!c.serve_owner(kGxFlush) || done(c)) return false;
        toggle_draw(c);
    } else if (rd(c, kSdaFirstHalf) != 0u) {
        if (!get_token(c, sp) || !queue_frame(c, sp, false) || !install_callback_once(c, sp)) return false;
        if (!copy_frame(c, rd(c, kSdaDraw)) || !advance_token(c)) return false;
        if (!c.serve_owner(kGxFlush) || done(c)) return false;
        wr(c, kSdaFirstHalf, 0u);
    } else {
        if (!get_token(c, sp) || !queue_frame(c, sp, true) || !install_callback_once(c, sp)) return false;
        const u32 mode = rd(c, kSdaMode);
        const u32 width = c.mem_read16(mode + 4u), height = c.mem_read16(mode + 6u);
        c.gpr[3] = 2u;
        if (!c.serve_owner(kGxSetCopyFilterMode) || done(c)) return false;
        c.gpr[3] = 0u; c.gpr[4] = 2u; c.gpr[5] = width; c.gpr[6] = (height - 2u) & 0xFFFFu;
        if (!c.serve_owner(kGxSetDispCopySrc) || done(c)) return false;
        const u32 rounded = (width + 15u) & 0xFFF0u;
        if (!copy_frame(c, rd(c, kSdaDraw) + ((rounded * (height - 2u)) << 1))) return false;
        if (!advance_token(c)) return false;
        c.gpr[3] = 3u;
        if (!c.serve_owner(kGxSetCopyFilterMode) || done(c)) return false;
        c.gpr[3] = 0u; c.gpr[4] = 0u; c.gpr[5] = width; c.gpr[6] = height;
        if (!c.serve_owner(kGxSetDispCopySrc) || done(c)) return false;
        if (!c.serve_owner(kGxFlush) || done(c)) return false;
        toggle_draw(c);
        wr(c, kSdaFirstHalf, 1u);
    }
    // the epilogue: r31, r1, LR; r3 = 1
    c.gpr[31] = c.mem_read32(sp + 0x1Cu);
    c.lr = c.mem_read32(sp0 + 4u);
    c.gpr[1] = sp0;
    c.gpr[3] = 1u;
    return true;
}

bool show_raster(Cpu& c) {
    if (c.interrupt_pending()) {   // OSDisableInterrupts takes a deliverable interrupt first and restarts
        c.leave_to_dispatcher(kShowRaster, FallbackReason::Yield, kShowRaster);
        return true;
    }
    const u32 sp0 = c.gpr[1], sp = sp0 - 0x20u;
    c.mem_write32(sp, sp0);
    c.mem_write32(sp0 + 4u, c.lr);
    c.mem_write32(sp + 0x1Cu, c.gpr[31]);
    c.gpr[1] = sp;
    const bool enabled = (c.msr() & kMsrEE) != 0u;   // OSDisableInterrupts: r31 = the previous EE
    c.set_msr(c.msr() & ~kMsrEE);
    c.gpr[31] = enabled ? 1u : 0u;
    const u32 read = rd(c, kSdaRingRead), write = rd(c, kSdaRingWrite), size = rd(c, kSdaRingSize);
    const bool full = write - read == 0xFFFFFFFFu || size - 1u == write - read;
    if (full) {
        c.gpr[3] = c.gpr[13] + kSdaQueue;
        if (!c.serve_owner(kSleepThread)) return false;
        if (done(c)) return true;
        if (c.exiting()) {
            stop(c, "OSSleepThread handed control back to the dispatcher inside the display swap");
            return true;
        }
    }
    if (enabled) c.set_msr(c.msr() | kMsrEE);   // OSRestoreInterrupts
    if (c.interrupt_pending()) {   // a source that became deliverable during the wait is taken here, then ShowRaster goes on
        c.leave_to_dispatcher(kShowRasterAfterRestore, FallbackReason::Yield, kShowRaster);
        return true;
    }
    return show_raster_rest(c);
}

// ---- the video mode and GcnSystem -------------------------------------------------------------------------------------------
bool report_error(Cpu& c, u32 sp, u32 slot, std::initializer_list<u32> format_args) {
    u32 message = 0, ignored = 0;
    c.mem_write32(sp + slot, 1u);
    ppc::set_cr_bit(c, 6u, 0u);
    if (!guest_call(c, kErrorFormat, kVideoMode + 4u, format_args, message)) return false;
    c.mem_write32(sp + slot + 4u, message);
    return guest_call(c, kErrorSet, kVideoMode + 4u, {sp + slot}, ignored);
}

// 0x80494CD4 (descriptor or 0, count).
bool set_video_mode(Cpu& c, u32 descriptor, u32 count) {
    const u32 sp0 = c.gpr[1], sp = sp0 - 0x20u;
    c.mem_write32(sp, sp0);
    c.gpr[1] = sp;
    bool ok = true;
    if (descriptor != 0u) {
        for (u32 offset = 0; offset < 0x3Cu; offset += 4u) c.mem_write32(kModeStore + offset, c.mem_read32(descriptor + offset));
        wr(c, kSdaMode, kModeStore);
        wr(c, kSdaModeKind, 0x2Au);
        u32 frames = count;
        if (c.mem_read8(kModeStore + 0x19u) != 0u) frames = 2u;
        wr(c, kSdaFbCount, frames);
        wr(c, kSdaFbCount2, frames);
        c.mem_write32(kStaticModes + 0x48u, c.mem_read16(kModeStore + 4u));
        c.mem_write32(kStaticModes + 0x4Cu, c.mem_read16(kModeStore + 6u));
        const std::int32_t signed_count = static_cast<std::int32_t>(count);
        if (count == 2u) c.mem_write32(kStaticModes + 0x50u, 0x10u);
        else if (signed_count < 2 && signed_count >= 0) c.mem_write32(kStaticModes + 0x50u, 0x18u);
        else ok = report_error(c, sp, 0x10u, {2u, kStaticModes + 0x60u});
    } else {
        c.gpr[3] = 0u;
        if (!c.serve_owner(kViGetTvFormat) || done(c)) {
            c.gpr[1] = sp0;
            return false;
        }
        const std::int32_t format = static_cast<std::int32_t>(c.gpr[3]);
        u32 record = 0, kind = 0;
        bool known = true;
        switch (format) {
        case 0: record = 0x805688C0u; kind = 0u; break;
        case 1: case 5: record = 0x80568938u; kind = 1u; break;
        case 2: record = 0x805688FCu; kind = 2u; break;
        default: known = false; break;
        }
        if (known) {
            wr(c, kSdaModeKind, kind);
            wr(c, kSdaMode, record);
        } else {
            ok = report_error(c, sp, 0x08u, {2u, kStaticModes + 0x78u});
        }
    }
    c.gpr[1] = sp0;
    return ok;
}

// VIWaitForRetrace (0x8037ECD4, still guest in the lifted build): disable interrupts, sleep on the retrace queue until the
// retrace count changes, restore. The same words, with the native sleep.
bool wait_for_retrace(Cpu& c) {
    const u32 count = rd(c, kSdaRetraceCount);
    const bool enabled = (c.msr() & kMsrEE) != 0u;
    c.set_msr(c.msr() & ~kMsrEE);
    do {
        c.gpr[3] = c.gpr[13] + kSdaRetraceQueue;
        if (!c.serve_owner(kSleepThread) || done(c)) return false;
        if (c.exiting()) {
            stop(c, "OSSleepThread handed control back to the dispatcher inside VIWaitForRetrace");
            return false;
        }
    } while (rd(c, kSdaRetraceCount) == count);
    if (enabled) c.set_msr(c.msr() | kMsrEE);
    return true;
}

// request 2: bring the video and the GX copy path up for the current mode record.
bool start_display(Cpu& c) {
    const u32 sp0 = c.gpr[1], sp = sp0 - 0x180u;
    c.mem_write32(sp, sp0);
    c.gpr[1] = sp;
    const auto fail = [&]() {
        c.gpr[1] = sp0;
        return false;
    };
    if (rd(c, kSdaFifoMade) == 0u) {
        c.gpr[3] = rd(c, kSdaArena);
        c.gpr[4] = rd(c, kSdaExtra);
        if (!c.serve_owner(kGxFifoCreate) || done(c)) return fail();
        wr(c, kSdaFifo2, c.gpr[3]);
        wr(c, kSdaFifoMade, 1u);
    } else {
        c.gpr[3] = sp + 8u; c.gpr[4] = rd(c, kSdaArena); c.gpr[5] = rd(c, kSdaExtra);
        if (!c.serve_owner(kGxFifoBase) || done(c)) return fail();
        c.gpr[3] = sp + 8u;
        if (!c.serve_owner(kGxFifoReadPtr) || done(c)) return fail();
        c.gpr[3] = sp + 8u;
        if (!c.serve_owner(kGxFifoWritePtr) || done(c)) return fail();
        c.gpr[3] = rd(c, kSdaFifo2); c.gpr[4] = rd(c, kSdaArena); c.gpr[5] = rd(c, kSdaExtra);
        if (!c.serve_owner(kGxFifoBase) || done(c)) return fail();
        c.gpr[3] = rd(c, kSdaFifo2);
        if (!c.serve_owner(kGxFifoReadPtr) || done(c)) return fail();
        c.gpr[3] = rd(c, kSdaFifo2);
        if (!c.serve_owner(kGxFifoWritePtr) || done(c)) return fail();
    }
    // wait until the GX draw-sync token the previous frame issued has been read back
    c.gpr[3] = 0u;
    for (;;) {
        if (!c.serve_owner(kGxReadDrawSync) || done(c)) return fail();
        if (c.mem_read16(c.gpr[13] + kSdaTokenRead) == (c.gpr[3] & 0xFFFFu)) break;
        c.gpr[3] = c.mem_read16(c.gpr[13] + kSdaTokenRead);
        if (!c.serve_owner(kGxSetDrawSync) || done(c)) return fail();
    }
    const u32 mode = rd(c, kSdaMode), fb = rd(c, kSdaFbCount);
    c.gpr[3] = mode;
    if (!c.serve_owner(kViConfigure) || done(c)) return fail();
    c.gpr[3] = 1u;
    if (!c.serve_owner(kViSetBlack) || done(c)) return fail();
    if (!c.serve_owner(kViFlush) || done(c)) return fail();
    const u32 width = c.mem_read16(mode + 4u), height6 = c.mem_read16(mode + 6u), height8 = c.mem_read16(mode + 8u);
    const u32 interlace = c.mem_read8(mode + 0x18u);
    const u32 sdata2 = c.gpr[2];
    set_single(c, 1, c.mem_read32(sdata2 + 0x1CA8u));
    set_single(c, 2, c.mem_read32(sdata2 + 0x1CA8u));
    set_single(c, 5, c.mem_read32(sdata2 + 0x1CA8u));
    set_single(c, 6, c.mem_read32(sdata2 + 0x1CACu));
    set_float(c, 3, static_cast<float>(width));
    set_float(c, 4, static_cast<float>(height8));
    if (interlace != 0u) {
        if (!c.serve_owner(kViGetNextField) || done(c)) return fail();
        c.gpr[3] ^= 1u;
        set_single(c, 1, c.mem_read32(sdata2 + 0x1CA8u));
        set_single(c, 2, c.mem_read32(sdata2 + 0x1CA8u));
        set_single(c, 5, c.mem_read32(sdata2 + 0x1CA8u));
        set_single(c, 6, c.mem_read32(sdata2 + 0x1CACu));
        set_float(c, 3, static_cast<float>(width));
        set_float(c, 4, static_cast<float>(height8));
        if (!c.serve_owner(kGxViewportJitter) || done(c)) return fail();
    } else {
        if (!c.serve_owner(kGxViewport) || done(c)) return fail();
    }
    c.gpr[3] = 0u; c.gpr[4] = 0u; c.gpr[5] = width; c.gpr[6] = height6;
    if (!c.serve_owner(kGxScissor) || done(c)) return fail();
    c.gpr[3] = 0u; c.gpr[4] = 0u;
    if (!c.serve_owner(kGxScissorOffset) || done(c)) return fail();
    c.gpr[3] = 0u; c.gpr[4] = 0u; c.gpr[5] = width; c.gpr[6] = height6;
    if (!c.serve_owner(kGxSetDispCopySrc) || done(c)) return fail();
    c.gpr[3] = width; c.gpr[4] = height8;
    if (!c.serve_owner(kGxSetDispCopyDst) || done(c)) return fail();
    c.gpr[3] = c.mem_read8(mode + 0x19u); c.gpr[4] = mode + 0x1Au; c.gpr[5] = 1u; c.gpr[6] = mode + 0x32u;
    if (!c.serve_owner(kGxSetCopyFilter) || done(c)) return fail();
    if (c.mem_read8(mode + 0x19u) != 0u && height8 == c.mem_read16(mode + 0x10u)) {
        set_single(c, 1, c.mem_read32(sdata2 + 0x1CACu));
        if (!c.serve_owner(kGxSetDispCopyYScale) || done(c)) return fail();
        wr(c, kSdaDoubleField, 1u);
        wr(c, kSdaHalfWidth, height8 >> 1);
        wr(c, kSdaFirstHalf, 1u);
    } else {
        set_float(c, 1, static_cast<float>(height8) / static_cast<float>(height6));
        if (!c.serve_owner(kGxSetDispCopyYScale) || done(c)) return fail();
        wr(c, kSdaDoubleField, 0u);
    }
    c.gpr[3] = c.mem_read8(mode + 0x18u);
    c.gpr[4] = (height8 - c.mem_read16(mode + 0x10u)) >> 31;
    if (!c.serve_owner(kGxSetDispCopyFrame) || done(c)) return fail();
    c.gpr[3] = fb;
    c.gpr[4] = 0u;
    if (!c.serve_owner(kGxCopyClear) || done(c)) return fail();
    wr(c, kSdaBlackPending, 1u);
    if (!wait_for_retrace(c)) return fail();
    if ((c.mem_read32(rd(c, kSdaMode)) & 1u) != 0u && !wait_for_retrace(c)) return fail();
    c.gpr[3] = 0u;
    if (!c.serve_owner(kGxSetDispCopyGamma) || done(c)) return fail();
    c.mem_write8(c.gpr[13] + kSdaSinceSwap, c.mem_read8(c.gpr[13] + kSdaMinSwap));
    c.gpr[3] = kPreRetraceCallback;
    if (!c.serve_owner(kViSetPreRetraceCallback) || done(c)) return fail();
    c.gpr[3] = kPostRetraceCallback;
    if (!c.serve_owner(kViSetPostRetraceCallback) || done(c)) return fail();
    c.gpr[3] = kDrawSyncCallback;
    if (!c.serve_owner(kGxSetDrawSyncCallback) || done(c)) return fail();
    c.gpr[3] = c.gpr[13] + kSdaQueue;
    if (!c.serve_owner(kInitThreadQueue) || done(c)) return fail();
    c.gpr[1] = sp0;
    return true;
}

bool engine_call(Cpu& c, u32 slot, std::initializer_list<u32> args, u32& result) {
    return guest_call(c, c.mem_read32(rd(c, kSdaEngine) + slot), kGcnSystem + 4u, args, result);
}

// request 0: choose the mode and allocate the frame buffers.
bool create_buffers(Cpu& c, u32 argument) {
    const u32 block = c.mem_read32(argument);
    if (block != 0u) {
        if (!set_video_mode(c, c.mem_read32(block), c.mem_read32(block + 4u))) return false;
        wr(c, kSdaExtra, c.mem_read32(block + 8u));
    } else {
        if (!set_video_mode(c, 0u, rd(c, kSdaFbCount))) return false;
    }
    const u32 mode = rd(c, kSdaMode);
    const u32 pixels = (((c.mem_read16(mode + 4u) + 0xFu) & 0xFFF0u) * c.mem_read16(mode + 8u));
    u32 pointer = 0, ignored = 0;
    if (!engine_call(c, 0x108u, {rd(c, kSdaExtra) + (pixels << 2) + 0x1Fu, 0x40411u}, pointer)) return false;
    wr(c, kSdaArenaRaw, pointer);
    const u32 aligned = (pointer + 0x1Fu) & ~0x1Fu;
    wr(c, kSdaArena, aligned);
    c.gpr[3] = aligned;
    c.gpr[4] = rd(c, kSdaExtra);
    if (!c.serve_owner(kCacheRange) || done(c)) return false;
    (void)ignored;
    const u32 first = aligned + rd(c, kSdaExtra), second = first + (pixels << 1);
    wr(c, kSdaShown, first);
    wr(c, kSdaBase0, first);
    wr(c, kSdaBase1, second);
    wr(c, kSdaDraw, second);
    return true;
}

// request 11: install the standard functions: every slot of the caller's array names the default, then the table's entries below the count.
void install_standard_functions(Cpu& c, u32 array, u32 count) {
    const std::int32_t n = static_cast<std::int32_t>(count);
    for (std::int32_t i = 0; i < n; ++i) c.mem_write32(array + 4u * static_cast<u32>(i), kFallbackFn);
    const u32 first_id = c.mem_read32(kStdFunctionTable);   // the original compares the FIRST pair's id on every iteration
    for (int pair = 26; pair >= 0; --pair) {
        const u32 id = c.mem_read32(kStdFunctionTable + 8u * static_cast<u32>(pair));
        const u32 function = c.mem_read32(kStdFunctionTable + 8u * static_cast<u32>(pair) + 4u);
        if (static_cast<std::int32_t>(first_id) >= n || static_cast<std::int32_t>(first_id) < 0) continue;
        c.mem_write32(array + (id << 2), function);
    }
}

// 0x804981E8: the render-state defaults.
bool state_defaults(Cpu& c) {
    const u32 sp0 = c.gpr[1], sp = sp0 - 0x20u;
    c.mem_write32(sp, sp0);
    c.gpr[1] = sp;
    const auto fail = [&]() {
        c.gpr[1] = sp0;
        return false;
    };
    const u32 sdata2 = c.gpr[2];
    const u32 word = c.mem_read32(sdata2 + 0x1CC8u);
    c.mem_write32(sp + 0x1Cu, word);
    const u32 record = kStateRecord;
    c.mem_write32(record + 0x10u, 0u); c.mem_write32(record + 0x14u, 1u); c.mem_write32(record + 0x18u, 0u);
    for (u32 b = 0x1Cu; b <= 0x1Fu; ++b) c.mem_write8(record + b, 0u);
    c.mem_write32(record + 0x24u, c.mem_read32(sdata2 + 0x1CCCu));
    c.mem_write32(record + 0x28u, c.mem_read32(sdata2 + 0x1CD0u));
    c.mem_write32(record + 0x2Cu, c.mem_read32(sdata2 + 0x1CD4u));
    c.mem_write32(record + 0x30u, c.mem_read32(sdata2 + 0x1CD0u));
    c.mem_write32(record + 0x20u, 0u);
    c.mem_write32(record + 0x00u, 1u); c.mem_write32(record + 0x04u, 1u); c.mem_write32(record + 0x3Cu, 1u);
    c.mem_write32(record + 0x34u, 5u); c.mem_write32(record + 0x38u, 6u); c.mem_write32(record + 0x0Cu, 2u); c.mem_write32(record + 0x08u, 3u);
    (void)g_hooks.state_reset(c, 1u, 3u, 1u);   // the lowered GXSetZMode(1, LEQUAL, 1) site
    if (done(c)) return fail();
    c.gpr[3] = 1u;
    if (!c.serve_owner(kGxState2) || done(c)) return fail();
    c.mem_write32(record + 0x40u, 4u); c.mem_write8(record + 0x4Cu, 0u); c.mem_write32(record + 0x48u, 0u);
    c.mem_write32(record + 0x44u, 7u); c.mem_write8(record + 0x4Du, 0u); c.mem_write8(record + 0x4Eu, 0u);
    c.gpr[3] = 7u; c.gpr[4] = 0u; c.gpr[5] = 0u; c.gpr[6] = 7u; c.gpr[7] = 0u;
    if (!c.serve_owner(kGxState3) || done(c)) return fail();
    c.gpr[3] = 1u; c.gpr[4] = 4u; c.gpr[5] = 5u; c.gpr[6] = 0u;
    if (!c.serve_owner(kGxState4) || done(c)) return fail();
    c.gpr[3] = 1u;
    if (!c.serve_owner(kGxState5) || done(c)) return fail();
    c.gpr[3] = 1u; c.gpr[4] = 4u; c.gpr[5] = 5u; c.gpr[6] = 0u;
    if (!c.serve_owner(kGxState4) || done(c)) return fail();
    for (const u32 index : {2u, 3u, 0u, 1u}) {
        c.gpr[3] = index; c.gpr[4] = 0u; c.gpr[5] = 0u; c.gpr[6] = 0u; c.gpr[7] = 0u; c.gpr[8] = 0u; c.gpr[9] = 2u;
        if (!c.serve_owner(kGxState6) || done(c)) return fail();
    }
    const std::pair<u32, u32> colours[] = {{2u, 0x18u}, {3u, 0x14u}, {0u, 0x10u}, {1u, 0x0Cu}};
    for (const auto& entry : colours) {
        c.mem_write32(sp + entry.second, c.mem_read32(sp + 0x1Cu));
        c.gpr[3] = entry.first; c.gpr[4] = sp + entry.second;
        if (!c.serve_owner(kGxState7) || done(c)) return fail();
    }
    c.gpr[3] = 1u;
    if (!c.serve_owner(kGxState8) || done(c)) return fail();
    c.gpr[3] = 1u;
    if (!c.serve_owner(kGxState9) || done(c)) return fail();
    c.mem_write32(sp + 8u, c.mem_read32(sp + 0x1Cu));
    c.gpr[3] = sp + 8u; c.gpr[4] = 0x00FFFFFFu;
    if (!c.serve_owner(kGxState10) || done(c)) return fail();
    c.gpr[3] = 0u;
    if (!c.serve_owner(kGxState11) || done(c)) return fail();
    // 0x8049AC18: clear eight words of the driver's state (0x8056FCDC .. 0x8056FCF8)
    for (u32 address = 0x8056FCDCu; address <= 0x8056FCF8u; address += 4u) c.mem_write32(address, 0u);
    u32 result = 0, ignored = 0;
    constexpr u32 kLink = kStateDefaults + 4u;
    if (!guest_call(c, kContextCreate, kLink, {0u}, result)) return fail();
    wr(c, kSdaContext, result);
    u32 flags = c.mem_read32(result + 0x50u);
    flags = (flags & 0xFFFFFF00u) | 2u;
    c.mem_write32(result + 0x50u, flags);
    const u32 context = rd(c, kSdaContext);
    flags = c.mem_read32(context + 0x50u);
    flags = (flags & 0xFFFF00FFu) | 0x1100u;
    c.mem_write32(context + 0x50u, flags);
    if (!guest_call(c, kScratchCreate, kLink, {4u, 4u, 0x10u, 0x204u}, result)) return fail();
    wr(c, kSdaScratch, result);
    u32 locked = 0;
    if (!guest_call(c, kScratchLock, kLink, {result, 0u, 9u}, locked)) return fail();
    if (!guest_call(c, kMemset, kLink, {locked, 0xFFu, 0x20u}, ignored)) return fail();
    if (!guest_call(c, kScratchUnlock, kLink, {rd(c, kSdaScratch)}, ignored)) return fail();
    c.gpr[1] = sp0;
    return true;
}

// 0x8049847C: the teardown.
bool context_teardown(Cpu& c) {
    u32 ignored = 0;
    constexpr u32 kLink = kContextTeardown + 4u;
    if (!guest_call(c, kScratchFree, kLink, {rd(c, kSdaScratch)}, ignored)) return false;
    wr(c, kSdaScratch, 0u);
    if (!guest_call(c, kContextRelease, kLink, {rd(c, kSdaContext), 0u}, ignored)) return false;
    if (!guest_call(c, kContextDestroy, kLink, {rd(c, kSdaContext)}, ignored)) return false;
    wr(c, kSdaContext, 0u);
    return true;
}

// 0x80494EDC GcnSystem(request, out, argument, count): the device system function.
void gcn_system(Cpu& c) {
    const u32 request = c.gpr[3], out = c.gpr[4], argument = c.gpr[5], count = c.gpr[6];
    c.gpr[3] = 0u;   // the default: unknown request
    if (request > 0x16u) return;
    switch (request) {
    case 0:
        if (create_buffers(c, argument)) c.gpr[3] = 1u;
        return;
    case 1:
        if (!c.serve_owner(kGxFlush) || done(c)) return;
        {
            u32 ignored = 0;
            if (!engine_call(c, 0x10Cu, {rd(c, kSdaArenaRaw)}, ignored)) return;
        }
        for (const u32 offset : {kSdaShown, kSdaBase0, kSdaBase1, kSdaDraw, kSdaArena, kSdaArenaRaw}) wr(c, offset, 0u);
        c.gpr[3] = 1u;
        return;
    case 2:
        if (start_display(c)) c.gpr[3] = 1u;
        return;
    case 3: {   // wait until no swap is pending and the VI ring has caught up
        std::uint32_t idle = 0;
        while (rd(c, kSdaPending) == 1u || rd(c, kSdaViWrite) != rd(c, kSdaViRead)) {
            if (done(c)) return;
            if (c.interrupt_pending()) {   // a deliverable interrupt is taken first; the request restarts afterwards
                c.gpr[3] = request;
                c.leave_to_dispatcher(kGcnSystem, FallbackReason::Yield, kGcnSystem);
                return;
            }
            if (++idle > 10000000u) {
                stop(c, "the display swap wait can never finish (no event arrives)");
                return;
            }
            c.idle_wait();
        }
        c.gpr[3] = 1u;
        return;
    }
    case 4: {   // copy the device record out and remember the third argument
        for (u32 offset = 0; offset < 0x38u; offset += 4u) c.mem_write32(out + offset, c.mem_read32(kDeviceRecord + offset));
        c.mem_write32(c.gpr[13] + kSdaDeviceTail + 4u, argument);
        c.gpr[3] = 1u;
        return;
    }
    case 5:
        c.mem_write32(out, 1u);
        c.gpr[3] = 1u;
        return;
    case 6: {
        if (static_cast<std::int32_t>(count) >= 1) return;   // r3 = 0
        u32 source = 0;
        bool copy = true;
        switch (rd(c, kSdaModeKind)) {
        case 0: source = 0u; break;
        case 1: case 5: source = 0x18u; break;
        case 2: source = 0x30u; break;
        case 0x2A: source = 0x48u; break;
        default: copy = false; break;
        }
        if (copy)
            for (u32 offset = 0; offset < 0x18u; offset += 4u) c.mem_write32(out + offset, c.mem_read32(kStaticModes + source + offset));
        c.gpr[3] = 1u;
        return;
    }
    case 7: {
        const u32 x = 1u ^ count;
        const std::int32_t half = static_cast<std::int32_t>(x) >> 1;
        const std::int32_t result = half - static_cast<std::int32_t>(x & 1u);
        c.gpr[3] = static_cast<u32>(result) >> 31;
        return;
    }
    case 8:
    case 9:
        c.gpr[3] = 1u;
        return;
    case 10:
        c.mem_write32(out, 0u);
        c.gpr[3] = 1u;
        return;
    case 11:
        install_standard_functions(c, out, count);
        c.gpr[3] = 1u;
        return;
    case 17: {   // GcnSystem keeps its own 0x180-byte frame under the callee: the state setters take pointers into the callee's frame
        const u32 sp0 = c.gpr[1];
        c.mem_write32(sp0 - 0x180u, sp0);
        c.gpr[1] = sp0 - 0x180u;
        const bool ok = state_defaults(c);
        c.gpr[1] = sp0;
        if (ok) c.gpr[3] = 1u;
        return;
    }
    case 18:
        if (context_teardown(c)) c.gpr[3] = 1u;
        return;
    case 19:
        c.mem_write32(out, 0x400u);
        c.gpr[3] = 1u;
        return;
    case 22:
        c.mem_write16(out, 3u);
        c.gpr[3] = 1u;
        return;
    default:
        return;   // r3 = 0
    }
}

}  // namespace

void install_display_owners(Cpu& cpu, const DisplayHooks& hooks) {
    g_hooks = hooks;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;
        }, std::string("native RW display: ") + name);
    };
    own(kShowRaster, "ShowRaster", [](Cpu& c) { (void)show_raster(c); });
    own(kShowRasterAfterRestore, "ShowRaster after its first OSRestoreInterrupts", [](Cpu& c) { (void)show_raster_rest(c); });
    own(kDrawSyncCallback, "draw-sync callback", [](Cpu& c) { draw_sync_callback(c); });
    own(kPreRetraceCallback, "VI pre-retrace callback", [](Cpu& c) { pre_retrace_callback(c); });
    own(kPostRetraceCallback, "VI post-retrace callback", [](Cpu& c) { post_retrace_callback(c); });
    own(kVideoMode, "video mode setter", [](Cpu& c) {
        if (set_video_mode(c, c.gpr[3], c.gpr[4])) c.gpr[3] = 0u;
    });
    own(kGcnSystem, "GcnSystem", [](Cpu& c) { gcn_system(c); });
    own(kFallbackFn, "the default standard function (li r3, 0; blr)", [](Cpu& c) { c.gpr[3] = 0u; });
    own(kStateDefaults, "render-state defaults", [](Cpu& c) { (void)state_defaults(c); });
    own(kContextTeardown, "context teardown", [](Cpu& c) { (void)context_teardown(c); });
}

}  // namespace gekko::rw_raster
