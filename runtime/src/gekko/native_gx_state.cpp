// The GX library's state API as native owners (gekko/native_gx_state.h).

#include "gekko/native_gx_state.h"

#include "gekko/cpu.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace gekko::gx_state {

namespace {

// The three bodies that are not silent.
constexpr u32 kGetCpuFifo = 0x803952FCu;          // lwz r3, 0x5E20(r13); blr   (CPUFifo)
constexpr u32 kGetTexObjData = 0x803982B8u;       // lwz r3, 24(r3); blr
constexpr u32 kGetTexObjFmt = 0x8039822Cu;        // lwz r3, 20(r3); blr
constexpr u32 kGetTexObjMipMap = 0x80398234u;     // r3 = ((lbz 31(r3)) & 1) == 1; r0 = cntlzw((flags & 1) - 1)
constexpr u32 kSetBreakPtCallback = 0x80394F3Cu;  // old = BreakPointCB; (interrupts off) BreakPointCB = r3; return old
constexpr u32 kSdaCpuFifo = 0x5E20u;
// The display-copy scale pair (the Dolphin SDK's GXSetDispCopyYScale 0x803970E4 and GXGetYScaleFactor 0x80396EAC by shape).
constexpr u32 kSetDispCopyYScale = 0x803970E4u;
constexpr u32 kGetYScaleFactor = 0x80396EACu;
constexpr u32 kSda2GxData = 0xFD8u;      // r2-relative pointer to __GXData
constexpr u32 kGxBpSentNot = 0x02u;      // u16
constexpr u32 kGxCpDispSrcSize = 0x1E4u; // the display-copy size word: bits 10..19 = height - 1
constexpr u32 kGxCpDisp = 0x1ECu;        // bit 10 = the Y scale is not 1

// __cvt_fp2unsigned (0x803A1DC8): below 0 -> 0, 2^32 and above or NaN -> 0xFFFFFFFF, else truncation.
u32 cvt_fp2unsigned(double value) noexcept {
    if (value < 0.0) return 0u;
    if (!(value < 4294967296.0)) return 0xFFFFFFFFu;
    return static_cast<u32>(value);
}
u32 rotl(u32 value, unsigned amount) noexcept { return amount == 0u ? value : (value << amount) | (value >> (32u - amount)); }

// The inlined line count of both functions: numerator / scale + 1, +1 more for a scale between 128 and 256 whose odd
// part divides the height (so the copy has no remainder line), at most 1024. A zero scale divides to 0, as divwu does.
u32 xfb_lines(u32 numerator, u32 height, u32 scale) noexcept {
    u32 lines = (scale != 0u ? numerator / scale : 0u) + 1u;
    if (scale > 128u && scale < 256u) {
        u32 odd = scale;
        while ((odd & 1u) == 0u) odd >>= 1;
        const u32 quotient = height / odd;
        if (height - quotient * odd == 0u) ++lines;
    }
    return lines > 1024u ? 1024u : lines;
}

// GXSetDispCopyYScale(vscale) -> the number of XFB lines. scale = (u32)(256 / vscale) & 0x1FF; the shadow keeps
// bpSentNot = 0 and cpDisp bit 10 = (scale != 256); the lines come from the display-copy height in the shadow.
void set_disp_copy_y_scale(Cpu& c) {
    double vscale;
    std::memcpy(&vscale, &c.fpr[1].bits[0], sizeof(vscale));
    const float quotient = 256.0f / static_cast<float>(vscale);
    const u32 scale = cvt_fp2unsigned(static_cast<double>(quotient)) & 0x1FFu;
    const u32 gx = c.mem_read32(c.gpr[2] + kSda2GxData);
    c.mem_write16(gx + kGxBpSentNot, 0u);
    const u32 disp = c.mem_read32(gx + kGxCpDisp);
    c.mem_write32(gx + kGxCpDisp, (disp & ~0x400u) | (scale != 256u ? 0x400u : 0u));
    const u32 size = c.mem_read32(gx + kGxCpDispSrcSize);
    const u32 height = ((size >> 10) & 0x3FFu) + 1u;
    c.gpr[3] = xfb_lines(rotl(size, 30u) & 0x0003FF00u, height, scale);
}

// GXGetYScaleFactor(efbHeight, xfbHeight) -> f32: the largest vertical scale whose line count does not exceed the
// target. Start from xfb / efb, step the numerator down while the count is above the target, then up while it is
// below, and answer the ratio before the last step up.
void get_y_scale_factor(Cpu& c) {
    const u32 efb = c.gpr[3] & 0xFFFFu, target = c.gpr[4] & 0xFFFFu;
    const u32 numerator = rotl(efb - 1u, 8u) & 0xFFFFFF00u;
    const auto lines_for = [&](u32 x, float& ratio) {
        ratio = static_cast<float>(x) / static_cast<float>(efb);
        const float quotient = 256.0f / ratio;
        return xfb_lines(numerator, efb, cvt_fp2unsigned(static_cast<double>(quotient)) & 0x1FFu);
    };
    u32 x = target;
    float ratio = 0.0f, previous = 0.0f;
    u32 lines = lines_for(x, ratio);
    while (lines > target) {
        --x;
        lines = lines_for(x, ratio);
    }
    previous = ratio;
    while (lines < target) {
        ++x;
        previous = ratio;
        lines = lines_for(x, ratio);
    }
    const double result = static_cast<double>(previous);
    std::memcpy(&c.fpr[1].bits[0], &result, sizeof(result));
}
constexpr u32 kSdaBreakPointCb = 0x5E34u;

constexpr Address kAddresses[] = {
    {0x80395D48u, "GXSetTexCoordGen2"},  {0x80395FC8u, "GXSetNumTexGens"},     {0x80396B54u, "GXSetCullMode"},
    {0x80397A34u, "GXLoadLightObjImm"},  {0x80397AB0u, "GXSetChanAmbColor"},   {0x80397B98u, "GXSetChanMatColor"},
    {0x80397C80u, "GXSetNumChans"},      {0x80397CBCu, "GXSetChanCtrl"},       {0x80399778u, "GXSetAlphaCompare"},
    {0x80399A0Cu, "GXSetFog"},           {0x80399D44u, "GXSetBlendMode"},      {0x80399D98u, "GXSetColorUpdate"},
    {0x80399DC4u, "GXSetAlphaUpdate"},   {0x80399DF0u, "GXSetZMode"},          {0x80399E24u, "GXSetZCompLoc"},
    {0x80399F58u, "GXSetDstAlpha"},      {0x8039A0B4u, "GXSetProjection"},     {0x8039A158u, "GXSetProjectionv"},
    {0x8039A22Cu, "GXLoadPosMtxImm"},    {0x8039A27Cu, "GXLoadNrmMtxImm"},     {0x8039A2CCu, "GXSetCurrentMtx"},
    {0x8039A300u, "GXLoadTexMtxImm"},    {0x8039A444u, "GXSetViewportJitter"}, {0x8039A49Cu, "GXSetViewport"},
    {0x8039A4E4u, "GXSetScissor"},       {0x8039A55Cu, "GXSetScissorBoxOffset"},
    // gxC, common to both trees: the cache invalidate is one dead pipe store; the display-list call's pipe stores are
    // dead transport too (the retained geometry R2 draws is read from RenderWare's own data).
    {0x80395D38u, "GXInvalidateVtxCache"}, {0x8039A044u, "GXCallDisplayList"},
    // gxD: GXSetMisc(token, value) keeps only shadow flags (the vertex count words and the XF flush bits).
    {0x80396004u, "GXSetMisc"},
    // gxB: the TEV / indirect-texture state setters. Each writes __GXData shadow words and sends dead pipe stores; the
    // combiner the renderer runs is decided from RenderWare's own materials, never from TEV registers. The names are
    // the Dolphin SDK's where the shape matches; the unnamed ones carry their size.
    {0x80398828u, "TEV / indirect setter (124 B)"},
    {0x80398E14u, "GXSetTevIndirect"},
    {0x80398E80u, "indirect texture matrix setter (376 B)"},
    {0x80398FF8u, "GXSetIndTexCoordScale"},
    {0x8039913Cu, "GXSetIndTexOrder"},
    {0x80399228u, "GXSetNumIndStages"},
    {0x8039924Cu, "GXSetTevDirect"},
    {0x803992ECu, "GXSetTevOp"},
    {0x80399378u, "GXSetTevColorIn"},
    {0x803993BCu, "GXSetTevAlphaIn"},
    {0x80399400u, "GXSetTevColorOp"},
    {0x80399468u, "GXSetTevAlphaOp"},
    {0x80399530u, "TEV setter (100 B)"},
    {0x80399594u, "GXSetTevKColor"},
    {0x803995F8u, "GXSetTevKColorSel"},
    {0x80399654u, "GXSetTevKAlphaSel"},
    {0x80399848u, "GXSetTevOrder"},
    {0x803999E4u, "GXSetNumTevStages"},
    {0x80399294u, "empty body (blr only)"},
    // Three small exact bodies the structural matcher found (tools/platform/structure_match.py): two getters and the
    // breakpoint-callback swap, served for real below.
    {kGetCpuFifo, "GXGetCPUFifo"},
    {kGetTexObjData, "GXGetTexObjData shape"},
    {kGetTexObjFmt, "GXGetTexObjFmt shape"},
    {kGetTexObjMipMap, "GXGetTexObjMipMap"},
    {kSetBreakPtCallback, "GXSetBreakPtCallback"},
    // gxD's last two rows: the display-copy Y scale (computed from the shadow's copy height) and its pure search.
    {kSetDispCopyYScale, "GXSetDispCopyYScale"},
    {kGetYScaleFactor, "GXGetYScaleFactor"},
};

// Lifted build only (see the header). The unnamed setters carry the shape names of the Dolphin signature table.
constexpr u32 kGxBegin = 0x80396934u;
constexpr Address kLiftedAddresses[] = {
    {0x80395758u, "GXClearVtxDesc"},
    {0x8039530Cu, "vertex descriptor setter (switch on the attribute)"},
    {0x80395790u, "vertex attribute format setter (switch on the attribute)"},
    {0x80395CF4u, "vertex array setter (GXSetArray shape)"},
    {kGxBegin, "GXBegin"},
};

}  // namespace

const Address* addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

const Address* lifted_addresses(std::size_t& count) noexcept {
    count = sizeof(kLiftedAddresses) / sizeof(kLiftedAddresses[0]);
    return kLiftedAddresses;
}

void install_lifted(Cpu& cpu) {
    for (const Address& entry : kLiftedAddresses) {
        if (entry.address == kGxBegin) {
            cpu.install_hle(entry.address, [](Cpu& c) {
                char detail[160];
                std::snprintf(detail, sizeof(detail),
                              "GXBegin was entered from 0x%08X: the lifted build has no GX immediate path (a primitive "
                              "header is a dead site there)", c.lr);
                c.request_stop(StopReason::ExecutionFault, detail);
                return true;
            }, "native GX vertex path: GXBegin (refused)");
            continue;
        }
        cpu.install_hle(entry.address, [](Cpu&) { return true; }, std::string("native GX vertex path: ") + entry.name);
    }
}

void install(Cpu& cpu) {
    for (const Address& entry : kAddresses) {
        if (entry.address == kGetCpuFifo) {
            cpu.install_hle(entry.address, [](Cpu& c) {
                c.gpr[3] = c.mem_read32(c.gpr[13] + kSdaCpuFifo);
                return true;
            }, std::string("native GX state: ") + entry.name);
        } else if (entry.address == kGetTexObjData) {
            cpu.install_hle(entry.address, [](Cpu& c) {
                c.gpr[3] = c.mem_read32(c.gpr[3] + 24u);
                return true;
            }, std::string("native GX state: ") + entry.name);
        } else if (entry.address == kGetTexObjFmt) {
            cpu.install_hle(entry.address, [](Cpu& c) {
                c.gpr[3] = c.mem_read32(c.gpr[3] + 20u);
                return true;
            }, std::string("native GX state: ") + entry.name);
        } else if (entry.address == kGetTexObjMipMap) {
            cpu.install_hle(entry.address, [](Cpu& c) {
                const u32 bit = c.mem_read8(c.gpr[3] + 31u) & 1u;   // lbz r0,31(r3); rlwinm r3,r0,0,31,31
                c.gpr[0] = bit != 0u ? 32u : 0u;                     // addi r0,r3,-1; cntlzw r0,r0 (bit 1: 0 -> 32; bit 0: -1 -> 0)
                c.gpr[3] = bit;                                      // rlwinm r3,r0,27,31,31 = bit 5 of r0
                return true;
            }, std::string("native GX state: ") + entry.name);
        } else if (entry.address == kSetDispCopyYScale) {
            cpu.install_hle(entry.address, [](Cpu& c) {
                set_disp_copy_y_scale(c);
                return true;
            }, std::string("native GX state: ") + entry.name);
        } else if (entry.address == kGetYScaleFactor) {
            cpu.install_hle(entry.address, [](Cpu& c) {
                get_y_scale_factor(c);
                return true;
            }, std::string("native GX state: ") + entry.name);
        } else if (entry.address == kSetBreakPtCallback) {
            // The original swapped the word with interrupts off and restored them: no net change to MSR.
            cpu.install_hle(entry.address, [](Cpu& c) {
                const u32 previous = c.mem_read32(c.gpr[13] + kSdaBreakPointCb);
                c.mem_write32(c.gpr[13] + kSdaBreakPointCb, c.gpr[3]);
                c.gpr[3] = previous;
                return true;
            }, std::string("native GX state: ") + entry.name);
        } else {
            // Served and silent: the shadow these functions kept is retired (see the header).
            cpu.install_hle(entry.address, [](Cpu&) { return true; }, std::string("native GX state: ") + entry.name);
        }
    }
}

}  // namespace gekko::gx_state
