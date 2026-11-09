/* GekkoForge native runtime — guest memory, endian, compatibility status.
 * No Nintendo SDK, no game assets. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;

typedef enum CompatStatus {
    UNIMPLEMENTED = 0,
    IMPLEMENTED_UNTESTED = 1,
    PARTIALLY_VALIDATED = 2,
    VALIDATED = 3,
    PARITY_CONFIRMED = 4
} CompatStatus;

#define GUEST_BASE 0x80000000u
#define GUEST_SIZE 0x00800000u

typedef struct RecompContext {
    u32 gpr[32];
    u32 pc, lr, ctr, cr, xer, msr;
    double fpr[32];
    u8 *mem;
    u32 mem_base;
    u32 mem_size;
} RecompContext;

static inline u16 ReadU16BE(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static inline u32 ReadU32BE(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}
static inline void WriteU16BE(u8 *p, u16 v) {
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}
static inline void WriteU32BE(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static inline u32 GuestOff(u32 addr) { return addr - GUEST_BASE; }

static inline u32 ctx_readU32(RecompContext *ctx, u32 addr) {
    u32 off = GuestOff(addr);
    if (off + 4 > ctx->mem_size) return 0;
    return ReadU32BE(ctx->mem + off);
}

static inline void ctx_writeU32(RecompContext *ctx, u32 addr, u32 value) {
    u32 off = GuestOff(addr);
    if (off + 4 > ctx->mem_size) return;
    WriteU32BE(ctx->mem + off, value);
}

#define GEKKO_UNIMPLEMENTED(name)                                                                      \
    do {                                                                                               \
        fprintf(stderr, "UNIMPLEMENTED %s\n", (name));                                                 \
        abort();                                                                                       \
    } while (0)

static inline void gekko_ctx_init(RecompContext *ctx, u8 *mem, u32 size) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->mem = mem;
    ctx->mem_base = GUEST_BASE;
    ctx->mem_size = size;
    ctx->gpr[1] = 0x80700000u;
    ctx->gpr[2] = 0x80008000u;
    ctx->gpr[13] = 0x80008000u;
}

#ifdef __cplusplus
}
#endif
