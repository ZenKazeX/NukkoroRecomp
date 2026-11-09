/* Vertical slice: add r3, r3, r4 ; blr — no game code. */
#include "gekko_rt.h"

void fn_80004000(RecompContext *ctx) { ctx->gpr[3] = ctx->gpr[3] + ctx->gpr[4]; }

int main(void) {
    u8 mem[16];
    RecompContext ctx;
    gekko_ctx_init(&ctx, mem, 16);
    ctx.gpr[3] = 40;
    ctx.gpr[4] = 2;
    fn_80004000(&ctx);
    printf("%u\n", ctx.gpr[3]);
    return ctx.gpr[3] == 42 ? 0 : 1;
}
