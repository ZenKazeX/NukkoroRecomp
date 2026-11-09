/* Clean-room OS stubs. GetTime/interrupts: IMPLEMENTED_UNTESTED. Others: UNIMPLEMENTED. */
#include "gekko_rt.h"

void OSInit(void) { /* PARTIALLY_VALIDATED: no-op init */ }

u32 OSDisableInterrupts(RecompContext *ctx) {
    u32 prev = ctx->msr & 0x8000u;
    ctx->msr &= ~0x8000u;
    return prev;
}

void OSRestoreInterrupts(RecompContext *ctx, u32 prev) {
    if (prev)
        ctx->msr |= 0x8000u;
    else
        ctx->msr &= ~0x8000u;
}

u32 OSGetTick(RecompContext *ctx) { return ctx->pc ^ 0x1; }

void OSGetTime(RecompContext *ctx) {
    ctx->gpr[3] = 0;
    ctx->gpr[4] = 0;
}

void VIWaitForRetrace(void) { GEKKO_UNIMPLEMENTED("VIWaitForRetrace"); }
void GXBegin(u32 prim, u32 vtxfmt, u16 nverts) {
    (void)prim;
    (void)vtxfmt;
    (void)nverts;
    GEKKO_UNIMPLEMENTED("GXBegin");
}
void DVDOpen(const char *path) {
    (void)path;
    GEKKO_UNIMPLEMENTED("DVDOpen");
}
void PADRead(void) { GEKKO_UNIMPLEMENTED("PADRead"); }
void AXInit(void) { GEKKO_UNIMPLEMENTED("AXInit"); }
