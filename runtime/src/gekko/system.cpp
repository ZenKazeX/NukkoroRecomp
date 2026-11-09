#include "gekko/system.h"
#include "gekko/thread_name.h"  // ES-T1

#include "gekko/camera.h"
#include "gekko/world_anchor.h"
#include "gekko/audio_audit.h"
#include "gekko/ax_adpcm_decode.h"
#include "gekko/ax_voice_select.h"
#include "gekko/cpu_diag.h"
#if !defined(SHADOW_RENDER_LIFTED)  // G5
#include "gekko/rw_capture.h"
#else
#include "gekko/savestate.h"  // G5: the idle immediate block
#endif

#include "gekko/display.h"
#include "gekko/native_save_text.h"
#include "gekko/profiling.h"
#include "gekko/process_profile.h"
#include "render/view.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <chrono>
#include <array>
#include <cstdio>
#include <functional>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <utility>

#if defined(SHADOW_TRACY)
// Only the profiling build reads the guest clock constant or the native memory
// accounting; the shipped build must not even carry the includes.
#include "gekko/host_timing.h"
#include "native_memory.h"
#endif

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace gekko {
namespace {

constexpr u32 kBi2GuestAddress = 0x817fe000u;

// A pointer a hook may follow: `bytes` of the game's mapped memory.
bool guest_pointer(Cpu& cpu, u32 pointer, u32 bytes = 4u) {
    return cpu.guest_memory().mapped(GcAddr(pointer), bytes);
}

// The AX microcode contract (command lists, PB links, the output buffer)
// carries OSCachedToPhysical values - the DSP read main memory physically -
// and the window address itself where an SDK path passed it unconverted.
// Anything else is no address of the contract.
bool ax_contract_address(u32 value, u32& address) {
    if (value < 0x40000000u) {
        address = GuestMemory::from_os_physical(value)->value();
        return true;
    }
    if (value >= GuestMemory::kConventionBase && value < GuestMemory::kConventionEnd) {
        address = value;
        return true;
    }
    return false;
}

// The memory card's persistence debounce (NativeCard::tick, host policy)
// counts 486 MHz core cycles; the game clock counts time-base ticks, a twelfth
// of that (core / (bus / 4)).
constexpr u64 kDeviceCyclesPerTick = 12u;

#if defined(SHADOW_TRACY)
// PER-VI-FRAME PUBLISHING STATE (profiling builds only). Sampled at the host
// service (GameEvent::Frame) and published once per VI frame, so a series
// means what it says per frame rather than per service.
struct TracyMachineSeries {
    bool anchored = false;
    u32 last_frame = 0;
    std::chrono::steady_clock::time_point last_publish{};
    u64 last_ticks = 0;
    u64 last_interpreted = 0;
    u64 last_allocations = 0;
    u64 last_divide_calls = 0;
    u64 last_divide_declines = 0;
};
TracyMachineSeries g_tracy_series;
#endif

// ---------------------------------------------------------------------------
// Widescreen: which logical HUD group a recovered glyph run belongs to
//
// fn_800BCBA4 is the game's HUD number/text renderer:
//
//     fn_800BCBA4(r3 = number-font object, r4 = C string, r5 = alignment,
//                 f1 = X, f2 = Y)          X and Y in 640x480 canvas pixels
//
// It walks the string and submits one four-vertex quad per glyph through
// RwIm2DRenderPrimitive (0x800BCE3C), rewriting only the X/Y of a pre-built
// quad at fontObj + 88 + digit*96. Every glyph is therefore an independent GX
// primitive with nothing tying it to its neighbours, which is why classifying
// 2D by primitive bounding box tears a string apart: "00:26:14" is eight quads
// spread over ~90 canvas pixels and the host was free to send the first two to
// the left edge and the last four to the right.
//
// The alignment reference X the game passes is exactly the anchor point of the
// whole run, so one classification per call is both correct and complete. The
// eight X values the gameplay HUD actually uses, recovered from the 27 call
// sites, are:
//
//     54, 78, 84, 120   left-hand group   (score, lives, rings, weapon ammo)
//     320               centre group      (timer and score, alignment 1)
//     477, 520, 560     right-hand group  (score, 2P rings, weapon ammo)
//
// Thirds of the canvas separate those three clusters with a wide margin on
// both boundaries (120 against 213.3, and 477 against 426.7), so the rule
// needs no per-widget table and no per-resolution constant. Classification
// itself lives in the host display layer (classify_group_anchor), so the widget
// path and the glyph path cannot drift apart and an animating run can be held
// steady across a boundary; this hook only reports the X and the call site.

// The recovered draw sites of the gameplay HUD strip.
//
//     LEFT | HERO GAUGE | TIMER + SCORE | DARK GAUGE | RIGHT
//
// Chao::CSD::Project::Render draws one .gncp per call, so its return address
// names the widget. Only the widgets that belong to that strip are anchored to
// a screen edge; every other CSD draw - the pause and results screens, the
// hint arrow that hovers over a world object, and anything not yet identified -
// is treated as scene space and keeps the centre anchor, which is exactly the
// transform the 3D projection received. That default is the safe one: a menu
// stays a centred 4:3 composition and a world marker stays attached to the
// object it points at, whereas an edge anchor would tear both away.
//
// Each entry is the instruction after the `bl 0x8036e794`, recovered by
// scanning extract/functions.jsonl for that call, and confirmed against the
// virtual-canvas boxes measured through --hud-report on the Westopolis
// savestate (the box for each site is in the comment).
constexpr u32 kHudStripDrawSites[] = {
    0x8003FE44u,  // CsdBase::Draw, shared by TGiHissatu (Hissatu_Hero /
                  // Hissatu_Dark, the meter icons capping the two gauges),
                  // NpcNameDisplay, StageTitleTask and CsdOneShot. The shared
                  // return address is safe here because the anchor comes from
                  // the group's own bounds: the Hissatu icons lie wholly in one
                  // half of the canvas (measured x 197.5..229.5 and
                  // 415.5..447.5) and take that side, while a stage title or an
                  // NPC name straddles the middle and stays centred.
    0x800BD3C8u,  // TGiHdGage::Render - a loop of two, HeroGauge.gncp then
                  // DarkGauge.gncp, so both meters arrive with this same return
                  // address and are told apart by their ordinal in the frame.
                  // (measured x 43.5..231.5 and 410.5..599.0)
    0x801FEC84u,  // TGiCommandWindowImpl::Render - the weapon / command window
                  // panels (measured x 639.5..721.5, parked off-canvas in 1P)
    0x8020015Cu,  // TGiRingChallengeImpl::Render, 2P branch - 2P_Ring.gncp.
                  // One call draws both the Ring1P and Ring2P scenes, so the
                  // group spans both halves and settles on the centre; see the
                  // split-screen gap in docs/WIDESCREEN_ARCHITECTURE.md 0.7.
    0x802002E8u,  // TGiRingChallengeImpl::Render, 1P branch - InfoWindow.gncp,
                  // the ring and life icons (measured x -46.5..97.5)
    0x800B8AFCu,  // The MISSION OBJECTIVE WINDOW (TWindow.gncp), drawn from
                  // fn_800B8ACC as the widget at `this+120`, gated on the HUD
                  // flag byte at 0x8057E760+328.
                  //
                  // It has to be here because its own counters already are.
                  // fn_800B8ACC draws the widget and then calls fn_800BC384,
                  // which loops TWO objective entries (stride 28) into
                  // fn_800BC74C -> fn_800BC7D4, reaching the glyph renderer at
                  // return address 0x800BC898 - which is in kHudStripTextSites,
                  // so each counter is announced as Text and takes Left or
                  // Right by the canvas thirds. Leaving the window art
                  // Ungrouped gave it the scene-space centre anchor instead,
                  // and a parent on Center with children on Left is exactly the
                  // mixed-ownership split this layer exists to prevent: at 16:9
                  // the "n/m" counter stays at the frame edge while the panel it
                  // is printed on slides inboard, so the counter ends up outside
                  // its own window. User-reported with a screenshot; the
                  // geometry confirms it - a counter at output canvas ~17..47
                  // can only come from a Left anchor, because Center would need
                  // a guest X of about -83.
                  //
                  // Safe by construction for the two-objective case: the anchor
                  // comes from the group's own bounds, so a window whose casts
                  // straddle the middle still classifies Center, exactly as it
                  // does today. This can only change the single-sided case,
                  // which is the broken one.
    0x8032F108u,  // The EGG DEALER slot display (CsdFiles/EggBoss, the boss
                  // stages stg0612 / 0614 / 0615; the object made by
                  // fn_8032F074 - docs/renderer-re-20261007/07_2D.md 2.8.6),
                  // drawn at L21 during play. ONE Project::Render draws its two
                  // displays, never at the same time: DisplaySml with its three
                  // reel scenes sits under the right gauge (measured canvas
                  // x ~466..605, y ~100..145 - wholly in the right half, so it
                  // takes the right edge with the gauge above it), DisplayLg is
                  // the big centred announcement (x ~156..484, straddling the
                  // middle, so the same bounds rule keeps it centred: the
                  // hysteresis lets a centred group return to Center at once).
                  // It used to be in the "centred safe areas" list below as a
                  // menu, which left the small display floating at the 4:3
                  // box's edge on any wider output (user-reported 2026-10-07).
                  // Per-cast anchoring would tear DisplayLg apart (its outer
                  // reel windows each lie in one half), so the project is the
                  // unit. As for every group, the first frame of a switch
                  // between the displays keeps the previous anchor.
};

// Deliberately NOT in that list, with the reason, because each of them looks
// like gameplay HUD and is not:
//
//   0x800BE884  TGiHPGaugeBossImpl - the boss health gauge is a centred
//               composition, not an edge-anchored strip widget.
//   0x800BF770  GiHPGaugeEnemyImpl - the enemy health bar is WORLD FOLLOWING:
//               fn_800bf9d8 projects the enemy's world position every frame
//               (fn_8000f654) and writes it with CNode::SetPosition. Edge
//               anchoring it would tear it off the enemy it belongs to; the
//               centre anchor is exactly the transform the 3D projection got.
//   0x80136FBC  Gong::CSD::GongCSD::Render - the PR0302RINGANM CSD drawn in
//               canvas space: no camera, no projection (the 2026-10-07 census,
//               docs/boundary-20261007/CENSUS.md 4; this line used to call it
//               the world-space 2D system - it is not).
//   0x8033BC4C  VehicleArrowImpl::Render - VHArrow.gncp, the hint arrow.
//               World following.
//   0x80201B2C  TGiScoreDisplayImpl::Render - fires only in the 2P branch;
//               in 1P the timer and score are pure glyph runs. Centred.
//   0x802BE7E0  the loading-screen widget (csdFiles/Loading + language suffix).
//               Centred safe area.
//   0x800B8B0C  the second widget drawn by fn_800B8ACC (`this+44`, followed by
//               fn_800B9F14, which is gated on a different HUD flag at
//               0x8057E760+293 and walks a message rather than a counter pair).
//               Not enough evidence yet that it is edge-hugging; left centred.
//               NOTE: this pair used to be labelled TGiPause::Render here. That
//               attribution was wrong - fn_800B8ACC sits in the gameplay-HUD
//               translation unit beside TGiHdGage (fn_800BD38C), the boss gauge
//               (fn_800BE850) and the enemy HP bar (fn_800BF634), and its
//               counters reach the gameplay glyph site 0x800BC898. The pause
//               screen is elsewhere.
//   0x802038BC  ResultTask::Draw,
//   0x802C44F8  SelectMode / BattleMode, 0x802E310C GiGameOver::Draw,
//   0x803327B0 / 0x803327D0 / 0x803327F4 SavePointSelect (0x8032F108, the
//               Egg Dealer slot display, moved to the list above 2026-10-07),
//   0x80349774, 0x803516E4, 0x80352BC4 StageRouteDisplay - all menus and
//               banners, all centred safe areas.

// The gameplay HUD's own glyph runs, by call site. All 27 `bl 0x800BCBA4`
// sites in the game resolve to four owners:
//
//   fn_801FEC40  weapon / CarMeter ammo          3 sites   HUD strip
//   fn_802000DC  TGiRingChallenge rings + lives  5 sites   HUD strip
//   fn_80201620  TGiScoreDisplay timer + score   7 sites   HUD strip
//   fn_800BC7D4                                  1 site    HUD strip (its own helper)
//   fn_80202F24..fn_80203368  results rows       7 sites   centred composition
//   fn_802C4DBC  Select menu info overlay        4 sites   centred composition
//
// Only the HUD strip is anchored to a screen edge. A results row or the Select
// menu's score columns are part of a centred 4:3 composition, and the canvas
// thirds would split one row of them across two anchors - the columns sit at
// canvas X 270, 380 and 490, so the third one alone would be dragged to the
// right edge while the panel art behind it stayed centred.
constexpr u32 kHudStripTextSites[] = {
    0x800BC898u,
    0x801FED70u, 0x801FEE30u, 0x801FEEF4u,
    0x80200200u, 0x802002A4u, 0x802003A0u, 0x80200458u, 0x80200490u,
    0x8020175Cu, 0x802017F0u, 0x80201BDCu, 0x80201C08u, 0x80201C34u,
    0x80201C60u, 0x80201D50u,
};

bool is_hud_strip_text_site(u32 return_address) {
    for (const u32 site : kHudStripTextSites) {
        if (site == return_address) {
            return true;
        }
    }
    return false;
}

// Draw sites whose ONE project draws several spatially separate panels, so the
// project is the wrong unit to anchor. See HudGroupKind::CsdWidgetPerCast.
constexpr u32 kHudPerCastAnchorSites[] = {
    0x800B8AFCu,  // the mission objective window - one Project::Render, two
                  // panels, whose counters sit at X = 24 and X = 545.
};

bool is_per_cast_anchor_site(u32 return_address) {
    for (const u32 site : kHudPerCastAnchorSites) {
        if (site == return_address) {
            return true;
        }
    }
    return false;
}

bool is_hud_strip_draw_site(u32 return_address) {
    for (const u32 site : kHudStripDrawSites) {
        if (site == return_address) {
            return true;
        }
    }
    return false;
}

u32 leading_zeroes_64(u64 value) {
    if (value == 0u) return 64u;
#ifdef _MSC_VER
    unsigned long index = 0;
    _BitScanReverse64(&index, value);
    return 63u - static_cast<u32>(index);
#else
    return static_cast<u32>(__builtin_clzll(value));
#endif
}

u32 population_count_64(u64 value) {
#ifdef _MSC_VER
    return static_cast<u32>(__popcnt64(value));
#else
    return static_cast<u32>(__builtin_popcountll(value));
#endif
}

// ---- Metrowerks 64-bit divide helpers: 0x803A1F54 (unsigned), 0x803A2040 (signed)
//
// The Gekko has no 64-bit divide instruction, so the game's compiler emitted a
// restoring shift-subtract loop: up to 64 passes of 32-bit adde/subfc/subfe over
// register pairs. Served here, quotient and remainder come from ONE host 64-bit
// division. The only 32-bit surface left is the interface the compiled callers
// depend on - arguments and results in the r3:r4 / r5:r6 register pairs - plus
// the exact guest state the recovered body leaves behind (scratch registers,
// CR0, XER[CA], CTR, stack words). A zero divisor declines: the recovered body
// runs.

// Final state of the division loop both helpers share:
//         li r10,-1 / addic r7,r7,0
//   loop: adde r4,r4,r4 / adde r3,r3,r3 / adde r8,r8,r8 / adde r7,r7,r7
//         subfc r0,r6,r8 / subfe. r9,r5,r7 / blt skip
//         mr r8,r0 / mr r7,r9 / addic r0,r10,1
//   skip: bdnz loop
//         adde r4,r4,r4 / adde r3,r3,r3
struct DivideLoopState {
    u64 quotient = 0;    // r3:r4 after the trailing adde pair
    u64 remainder = 0;   // r7:r8
    u32 r0 = 0;
    u32 r9 = 0;          // last subfe. result; it also set CR0
    u32 subtracted = 0;  // passes that fell through blt (three more instructions each)
    bool carry = false;  // XER[CA] after the trailing adde pair
};

// `magnitude` is the (sign-stripped) dividend and `iterations` =
// clz64(divisor) - clz64(magnitude) + 1, in [1, 64].
DivideLoopState run_divide_loop(u64 magnitude, u64 divisor, u32 iterations) {
    DivideLoopState state;
    if (divisor <= (u64{1} << 62u)) {
        // The loop subtracts when the 64-bit difference is non-negative as a
        // SIGNED value. With the divisor at most 2^62 every shifted remainder
        // stays below 2^63, so that test is exactly remainder >= divisor and the
        // loop computes the true quotient: one host division reproduces it.
        state.quotient = magnitude / divisor;
        state.remainder = magnitude % divisor;
        state.subtracted = population_count_64(state.quotient);
        const bool last_subtracted = (state.quotient & 1u) != 0u;
        const u64 difference = state.remainder - divisor;
        state.r0 = last_subtracted ? 0u : static_cast<u32>(difference);
        state.r9 = static_cast<u32>((last_subtracted ? state.remainder : difference) >> 32u);
        // The accumulator holds quotient >> 1 before the trailing shift, so
        // nothing carries out of bit 63.
        state.carry = false;
        return state;
    }
    // Above 2^62 the signed test and the 64-bit shifts no longer agree with a
    // true division; follow the loop's own arithmetic, still on 64-bit words.
    u64 accumulator = iterations == 64u ? magnitude : magnitude << (64u - iterations);
    u64 remainder = iterations == 64u ? 0u : magnitude >> iterations;
    bool carry = false;
    for (u32 pass = 0; pass < iterations; ++pass) {
        const bool shifted_out = (accumulator >> 63u) != 0u;
        accumulator = (accumulator << 1u) | (carry ? 1u : 0u);
        remainder = (remainder << 1u) | (shifted_out ? 1u : 0u);
        const u64 difference = remainder - divisor;
        state.r0 = static_cast<u32>(difference);
        state.r9 = static_cast<u32>(difference >> 32u);
        carry = remainder >= divisor;
        if (static_cast<s32>(state.r9) >= 0) {
            remainder = difference;
            state.r0 = 0u;
            carry = true;
            ++state.subtracted;
        }
    }
    state.carry = (accumulator >> 63u) != 0u;
    state.quotient = (accumulator << 1u) | (carry ? 1u : 0u);
    state.remainder = remainder;
    return state;
}

u32 cr0_signed(u32 value, const Cpu& cpu) {
    const s32 v = static_cast<s32>(value);
    return (v < 0 ? 8u : (v > 0 ? 4u : 2u)) | ((cpu.xer & kXerSO) != 0u ? 1u : 0u);
}

bool unsigned_divide_64_intrinsic(Cpu& cpu) {
    const u64 dividend = (static_cast<u64>(cpu.gpr[3]) << 32u) | cpu.gpr[4];
    const u64 divisor = (static_cast<u64>(cpu.gpr[5]) << 32u) | cpu.gpr[6];
    if (divisor == 0u) return false;

    const u32 dividend_leading = leading_zeroes_64(dividend);
    const u32 divisor_leading = leading_zeroes_64(divisor);
    if (dividend_leading > divisor_leading) {
        // subfic r10,r0,64 always carries (the count is in [0, 64]); cmpw r0,r9
        // left GT. r7, r8 and CTR are untouched.
        cpu.gpr[0] = dividend_leading;
        cpu.gpr[9] = divisor_leading;
        cpu.gpr[10] = 64u - dividend_leading;
        cpu.xer |= kXerCA;
        cpu.gpr[3] = 0u;
        cpu.gpr[4] = 0u;
        cpu.set_cr_field(0, 4u | ((cpu.xer & kXerSO) != 0u ? 1u : 0u));
        return true;
    }
    const u32 iterations = divisor_leading - dividend_leading + 1u;
    const DivideLoopState loop = run_divide_loop(dividend, divisor, iterations);
    cpu.gpr[0] = loop.r0;
    cpu.gpr[3] = static_cast<u32>(loop.quotient >> 32u);
    cpu.gpr[4] = static_cast<u32>(loop.quotient);
    cpu.gpr[7] = static_cast<u32>(loop.remainder >> 32u);
    cpu.gpr[8] = static_cast<u32>(loop.remainder);
    cpu.gpr[9] = loop.r9;
    cpu.gpr[10] = 0xffffffffu;
    cpu.ctr = 0u;
    cpu.xer = loop.carry ? (cpu.xer | kXerCA) : (cpu.xer & ~kXerCA);
    cpu.set_cr_field(0, cr0_signed(loop.r9, cpu));
    return true;
}

bool signed_divide_64_intrinsic(Cpu& cpu) {
    const u64 dividend_bits = (static_cast<u64>(cpu.gpr[3]) << 32u) | cpu.gpr[4];
    const u64 divisor_bits = (static_cast<u64>(cpu.gpr[5]) << 32u) | cpu.gpr[6];
    if (divisor_bits == 0u) return false;

    const bool dividend_negative = (dividend_bits >> 63u) != 0u;
    const bool divisor_negative = (divisor_bits >> 63u) != 0u;
    const u64 dividend = dividend_negative ? (~dividend_bits + 1u) : dividend_bits;
    const u64 divisor = divisor_negative ? (~divisor_bits + 1u) : divisor_bits;
    const u32 dividend_leading = leading_zeroes_64(dividend);
    const u32 divisor_leading = leading_zeroes_64(divisor);
    const bool early_out = dividend_leading > divisor_leading;
    const bool signs_differ = dividend_negative != divisor_negative;
    DivideLoopState loop;
    if (!early_out) {
        loop = run_divide_loop(dividend, divisor, divisor_leading - dividend_leading + 1u);
    }

    const u32 dividend_sign = dividend_negative ? 0x80000000u : 0u;
    const u32 divisor_sign = divisor_negative ? 0x80000000u : 0u;
    const u32 old_stack = cpu.gpr[1];
    cpu.mem_write32(old_stack - 16u, old_stack);
    cpu.mem_write32(old_stack - 8u, dividend_sign);
    cpu.mem_write32(old_stack - 4u, divisor_sign);
    // subfic/subfze negate the divisor pair in place.
    cpu.gpr[5] = static_cast<u32>(divisor >> 32u);
    cpu.gpr[6] = static_cast<u32>(divisor);
    const u32 summary_overflow = (cpu.xer & kXerSO) != 0u ? 1u : 0u;
    if (early_out) {
        cpu.gpr[0] = dividend_leading;
        cpu.gpr[9] = divisor_leading;
        cpu.gpr[10] = 64u - dividend_leading;
        cpu.xer |= kXerCA;
        cpu.gpr[3] = 0u;
        cpu.gpr[4] = 0u;
        cpu.set_cr_field(0, 4u | summary_overflow);
    } else {
        const u64 quotient = signs_differ ? (~loop.quotient + 1u) : loop.quotient;
        cpu.gpr[0] = loop.r0;
        cpu.gpr[3] = static_cast<u32>(quotient >> 32u);
        cpu.gpr[4] = static_cast<u32>(quotient);
        cpu.gpr[7] = dividend_sign ^ divisor_sign;
        cpu.gpr[8] = static_cast<u32>(loop.remainder);
        cpu.gpr[9] = dividend_sign;
        cpu.gpr[10] = divisor_sign;
        cpu.ctr = 0u;
        // xor. leaves EQ when the signs agree; otherwise cmpwi r9,0 decides and
        // subfic/subfze carry only when the magnitude quotient is zero.
        const bool carry = signs_differ ? loop.quotient == 0u : loop.carry;
        cpu.xer = carry ? (cpu.xer | kXerCA) : (cpu.xer & ~kXerCA);
        cpu.set_cr_field(0, (signs_differ && dividend_negative ? 8u : 2u) | summary_overflow);
    }
    return true;
}

std::string hex32(u32 value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    return buffer;
}

}  // namespace

AxVoiceAdvanceResult advance_ax_voice_position(u32 current, u32 end_address,
                                               u32 loop_address, bool looping,
                                               u64 units) noexcept {
    AxVoiceAdvanceResult result;
    result.next = current;
    if (current > end_address || (looping && loop_address > end_address)) return result;
    result.valid = true;
    result.running = true;
    const u64 next = static_cast<u64>(current) + units;
    if (next <= end_address) {
        result.next = static_cast<u32>(next);
    } else if (looping) {
        const u64 span = static_cast<u64>(end_address) - loop_address + 1u;
        result.next = static_cast<u32>(loop_address + ((next - loop_address) % span));
        result.looped = true;
    } else {
        result.next = end_address;
        result.running = false;
    }
    return result;
}

// The lowered GX pipe sites' calls (gekko/gx_sites.h, extract/gx-sites.txt):
// the draw-sync token and draw-done to the native GX synchronisation, GXBegin's
// header and the vertex data to the immediate geometry builder, the copy
// trigger to the capture (a count: the copy itself is the RenderWare driver's).
class System::GxSiteRouter final : public GxSiteSink {
public:
    explicit GxSiteRouter(System& system) noexcept : system_(system) {}
    void draw_sync_token(u32, u16 token, bool interrupt) override {
        system_.gx_sync_->on_draw_sync_token(token, interrupt);
    }
    void draw_done(u32, u32 value) override {
        // BP 0x45 with low byte 2 requests the finish interrupt.
        if ((value & 0xffu) == 0x02u) system_.gx_sync_->on_draw_done();
    }
#if !defined(SHADOW_RENDER_LIFTED)  // G5
    void copy_trigger(u32, u32 value) override {
        if (RwCapture* capture = system_.native_capture_.get()) capture->on_copy_trigger(value);
    }
    void primitive_opcode(u32 site, u8 opcode) override { system_.immediate_->primitive_opcode(site, opcode); }
    void primitive_count(u32 site, u16 count) override { system_.immediate_->primitive_count(site, count); }
    void vertex(u32 site, u32 value, u32 bytes) override { system_.immediate_->vertex(site, value, bytes); }
#else
    // G5 (HW_REPORT T): the copy trigger only fed the legacy capture's counter (nothing here, as without it); the
    // GX immediate sites are dead in the lifted build (0 primitives in every lifted run) and fail closed.
    void copy_trigger(u32, u32) override {}
    void primitive_opcode(u32 site, u8) override { dead(site, "GXBegin header"); }
    void primitive_count(u32 site, u16) override { dead(site, "GXBegin vertex count"); }
    void vertex(u32 site, u32, u32) override { dead(site, "immediate vertex"); }
#endif

private:
#if defined(SHADOW_RENDER_LIFTED)
    void dead(u32 site, const char* what) {
        char detail[160];
        std::snprintf(detail, sizeof(detail),
                      "GX pipe site 0x%08X (%s) was reached: the lifted build has no GX immediate path", site, what);
        system_.cpu_.request_stop(StopReason::ExecutionFault, detail);
    }
#endif
    System& system_;
};

System::System() : cpu_(memory_) {
    // No device is registered: every GameCube device is native and has no
    // registers - the video interface (NativeVideo), controller input (the
    // lifted peripheral layer, docs/lift-20261006/L1_INPUT.md), the audio interface and the DSP
    // (NativeAudio), the command processor and the pixel engine (NativeGxSync),
    // the external interface's clients - the memory card, the SRAM, the boot
    // ROM, the debug UART (NativeCard, NativeIpl) -, the processor interface's
    // interrupt bookkeeping and reset controls (NativeInterrupts, NativeReset)
    // and the memory interface (nothing: no memory-protection hardware;
    // docs/native-time-20261005/STAGE6_PI_MI.md). The write-gather pipe has no
    // device at all - every store the image makes to it is a lowered GX site
    // (phase 4, docs/native-time-20261005/GX_DATA_PLANE.md; an unlisted one
    // stops the machine, Cpu::unlisted_gx_store). So any other guest access
    // outside memory stops the machine too, naming the retired page (fail
    // closed, Cpu::set_unmapped_access_stops).
    cpu_.set_unmapped_access_stops(true);

    // The native platform layers (docs/native-time-20261005/DESIGN.md).
    interrupts_ = std::make_unique<NativeInterrupts>(cpu_);
    video_ = std::make_unique<NativeVideo>(cpu_, *interrupts_);
    // The lifted subsystems the lowered call sites reach (docs/lift-20261006).
    cpu_.set_lifted(&lifted_);
    audio_ = std::make_unique<NativeAudio>(cpu_, memory_, *interrupts_);
    audio_->set_mixer([this](u32 command_list) { run_ax_mix(command_list); });
    frame_limiter_ = std::make_unique<NativeFrameLimiter>(
        cpu_, [this](u64 target) { return advance_game_time(target); },
        [this] { return advance_to_next_event(); });
    // The layout objects' range: the game's draw distance (docs/DRAW_DISTANCE.md).
    object_range_ = std::make_unique<NativeObjectRange>(cpu_);
    motion_store_ = std::make_unique<NativeMotionStore>(cpu_, memory_);
    sound_banks_ = std::make_unique<NativeSoundBanks>(cpu_, memory_);
    stream_ring_ = std::make_unique<NativeStreamRing>(cpu_, memory_, *audio_);
    // Attached effects follow their object as it is this frame (docs/RENDER_FLICKER.md "Fourth bug").
    attached_effects_ = std::make_unique<NativeAttachedEffects>(cpu_);
    // The GX library's draw sync, breakpoint and FIFO API, and the immediate
    // geometry builder; the lowered GX sites reach both through the router.
    gx_sync_ = std::make_unique<NativeGxSync>(cpu_, *interrupts_);
#if !defined(SHADOW_RENDER_LIFTED)  // G5
    immediate_ = std::make_unique<NativeImmediate>(cpu_);
#endif
    // The memory card (the PC's save storage under the game's CARD file
    // system) and the OS's other external-interface clients.
    card_ = std::make_unique<NativeCard>(cpu_, memory_, *interrupts_);
    ipl_ = std::make_unique<NativeIpl>(cpu_, *interrupts_);
    // The console's reset and boot controls (OSResetSystem ends the program).
    reset_controls_ = std::make_unique<NativeReset>(cpu_);
    // The game's disc-error screen module, with no drive (DISC_SYSTEM_MAP.md cut 1).
    disc_error_ = std::make_unique<NativeDiscError>(cpu_);
    // The layout manager's link-group lists in one pass (GPU_FILL.md "stage start").
    link_groups_ = std::make_unique<NativeLinkGroups>(cpu_);
    // The game's directory scan, over the asset namespace (DISC_SYSTEM_MAP.md cut 2).
    directory_scan_ = std::make_unique<NativeDirectoryScan>(cpu_, assets_);
    game_memory_ = std::make_unique<GameMemory>(cpu_);
    gx_site_router_ = std::make_unique<GxSiteRouter>(*this);
    // The router is installed with an empty table: the generated one is the
    // executable's (install_gx_sites).
    cpu_.set_gx_sites(nullptr, 0u, gx_site_router_.get());
    // One 128 KiB allocation, once: the AX contract always runs (the voices
    // move whether or not a host device plays the result).
    ax_adpcm_ring_ = std::make_unique<AxAdpcmRingStore>();
    ax_source_windows_ = std::make_unique<AxSourceWindowStore>();
    audio_mixer_ = std::make_unique<shadow::audio::Mixer>();
    ax_report_.enabled = true;

    // The OS idle loop is a wait point (Cpu::idle_wait).
    cpu_.set_idle_handler([this](Cpu&) { idle_wait(); });
}

System::~System() {
    watchdog_quit_.store(true);
    if (watchdog_.joinable()) watchdog_.join();
    cpu_.set_gx_sites(nullptr, 0u, nullptr);
}

void System::install_gx_sites(const GxSiteRecord* sites, std::size_t count) noexcept {
    cpu_.set_gx_sites(sites, count, gx_site_router_.get());
}

void System::mark_hud_group(HudGroupKind kind, u32 group_id, float canvas_x, u32 instance) {
#if !defined(SHADOW_RENDER_LIFTED)  // G5
    if (native_capture_ != nullptr) native_capture_->on_hud_group(kind, group_id, canvas_x, instance);
#endif
    if (hud_listener_ != nullptr) hud_listener_->on_hud_group(kind, group_id, canvas_x, instance);
}

void System::mark_hud_scene(u32 node, u32 cast) {
#if !defined(SHADOW_RENDER_LIFTED)  // G5
    if (native_capture_ != nullptr) native_capture_->on_hud_scene(node, cast);
#endif
    if (hud_listener_ != nullptr) hud_listener_->on_hud_scene(node, cast);
}

namespace {

// Geometry helpers for the visibility probe. Double precision throughout: this
// is host-side observation, never guest arithmetic, so there is no reason to
// reproduce the guest's single-precision rounding here.
struct ProbeVec3 { double x = 0.0, y = 0.0, z = 0.0; };
struct ProbePlane { ProbeVec3 n; double d = 0.0; };

inline double probe_dot(const ProbeVec3& a, const ProbeVec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline ProbeVec3 probe_cross(const ProbeVec3& a, const ProbeVec3& b) {
    return ProbeVec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline ProbeVec3 probe_scale(const ProbeVec3& a, double s) {
    return ProbeVec3{a.x * s, a.y * s, a.z * s};
}
inline double probe_len(const ProbeVec3& a) { return std::sqrt(probe_dot(a, a)); }

// Rodrigues rotation of n about a unit axis.
inline ProbeVec3 probe_rotate(const ProbeVec3& n, const ProbeVec3& axis, double theta) {
    const double c = std::cos(theta);
    const double s = std::sin(theta);
    const ProbeVec3 cr = probe_cross(axis, n);
    const double dt = probe_dot(axis, n);
    return ProbeVec3{n.x * c + cr.x * s + axis.x * dt * (1.0 - c),
                     n.y * c + cr.y * s + axis.y * dt * (1.0 - c),
                     n.z * c + cr.z * s + axis.z * dt * (1.0 - c)};
}

// Intersection point of three planes; false if near-singular.
inline bool probe_apex(const ProbePlane& a, const ProbePlane& b, const ProbePlane& c,
                       ProbeVec3& out) {
    const ProbeVec3 bc = probe_cross(b.n, c.n);
    const double det = probe_dot(a.n, bc);
    if (std::fabs(det) < 1e-9) return false;
    const ProbeVec3 ca = probe_cross(c.n, a.n);
    const ProbeVec3 ab = probe_cross(a.n, b.n);
    out = ProbeVec3{(bc.x * a.d + ca.x * b.d + ab.x * c.d) / det,
                    (bc.y * a.d + ca.y * b.d + ab.y * c.d) / det,
                    (bc.z * a.d + ca.z * b.d + ab.z * c.d) / det};
    return true;
}

constexpr double kProbePi = 3.14159265358979323846;

}  // namespace

// See the hook comment at RwCameraTestSphere. Pure observation: reads guest
// memory, writes only host counters, and the handler returns false so the
// recovered body runs untouched.
//
// The layout is VERIFIED rather than assumed. Every plane normal must be unit
// length, and exactly four of the six planes must share a common apex - that is
// what a perspective frustum looks like and nothing else does. A structure that
// fails either check is counted as layout_bad instead of producing a number.
void System::observe_frustum_test(Cpu& cpu) {
    const u32 camera = cpu.gpr[3];
    const u32 sphere = cpu.gpr[4];
    const auto sane = [&cpu](u32 p) { return guest_pointer(cpu, p); };
    if (!sane(camera) || !sane(sphere)) return;

    const auto rf = [&cpu](u32 address) {
        const u32 bits = cpu.mem_read32(address);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return static_cast<double>(value);
    };

    ++visibility_probe_.tests;
    // Direct read of the guest's own view window: viewWindow = (tan(hFOV/2),
    // tan(vFOV/2)), the single input the frustum builder uses.
    visibility_probe_.camera_address = camera;
    {
        const double vwx = rf(camera + 0x68u);
        const double vwy = rf(camera + 0x6Cu);
        if (vwx > 1e-6 && vwy > 1e-6 && vwx < 100.0 && vwy < 100.0) {
            visibility_probe_.view_window_x = vwx;
            visibility_probe_.view_window_y = vwy;
            if (vwx < visibility_probe_.view_window_x_min)
                visibility_probe_.view_window_x_min = vwx;
            if (vwx > visibility_probe_.view_window_x_max)
                visibility_probe_.view_window_x_max = vwx;
        }
    }

    // RwCamera::frustumPlanes[6] at +0x94. Each RwFrustumPlane is an RwPlane
    // (normal xyz, distance) followed by three closest-corner bytes and a pad,
    // so the stride is 20. Nothing below depends on the plane ORDER.
    ProbePlane planes[6];
    for (int i = 0; i < 6; ++i) {
        const u32 base = camera + 0x94u + static_cast<u32>(i) * 20u;
        planes[i].n = ProbeVec3{rf(base + 0u), rf(base + 4u), rf(base + 8u)};
        planes[i].d = rf(base + 12u);
        const double len = probe_len(planes[i].n);
        if (!(len > 0.5 && len < 2.0)) {
            ++visibility_probe_.layout_bad;
            return;
        }
    }

    // The four side planes of a perspective frustum meet at the camera apex;
    // near and far do not. Recovering the apex both locates the camera and
    // proves the layout guess.
    ProbeVec3 apex{};
    bool have_apex = false;
    for (int a = 0; a < 6 && !have_apex; ++a) {
        for (int b = a + 1; b < 6 && !have_apex; ++b) {
            for (int c = b + 1; c < 6 && !have_apex; ++c) {
                ProbeVec3 candidate{};
                if (!probe_apex(planes[a], planes[b], planes[c], candidate)) continue;
                int on = 0;
                for (int i = 0; i < 6; ++i) {
                    if (std::fabs(probe_dot(planes[i].n, candidate) - planes[i].d) < 1e-2) ++on;
                }
                if (on == 4) {
                    apex = candidate;
                    have_apex = true;
                }
            }
        }
    }
    if (!have_apex) {
        ++visibility_probe_.layout_bad;
        return;
    }
    ++visibility_probe_.layout_ok;

    int side[4];
    int sides = 0;
    for (int i = 0; i < 6 && sides < 4; ++i) {
        if (std::fabs(probe_dot(planes[i].n, apex) - planes[i].d) < 1e-2) side[sides++] = i;
    }
    if (sides != 4) {
        ++visibility_probe_.layout_bad;
        return;
    }

    // Of the two opposing pairs among the side planes, the horizontal one is the
    // wider: the game runs a 66.833 degree hFOV against a 53.130 degree vFOV.
    // narrow_* is retained for the report so the recovered vFOV can be checked
    // against that expectation.
    int wide_i = -1;
    int wide_j = -1;
    int narrow_i = -1;
    int narrow_j = -1;
    double wide_half = -1.0;
    double narrow_half = -1.0;
    for (int a = 0; a < 4; ++a) {
        for (int b = a + 1; b < 4; ++b) {
            const double c = probe_dot(planes[side[a]].n, planes[side[b]].n);
            if (c > -0.2) continue;  // not an opposing pair
            const double between = std::acos(std::min(1.0, std::max(-1.0, c)));
            const double half = kProbePi * 0.5 - between * 0.5;
            if (half > wide_half) {
                narrow_half = wide_half;
                narrow_i = wide_i;
                narrow_j = wide_j;
                wide_half = half;
                wide_i = side[a];
                wide_j = side[b];
            } else if (half > narrow_half) {
                narrow_half = half;
                narrow_i = side[a];
                narrow_j = side[b];
            }
        }
    }
    if (wide_i < 0 || narrow_i < 0 || narrow_half <= 0.0) {
        ++visibility_probe_.layout_bad;
        return;
    }

    const ProbeVec3 centre{rf(sphere + 0u), rf(sphere + 4u), rf(sphere + 8u)};
    const double radius = rf(sphere + 12u);
    if (!(radius >= 0.0) || radius > 1.0e7) {
        ++visibility_probe_.layout_bad;
        return;
    }

    const auto outside = [&](const ProbePlane* set) {
        for (int i = 0; i < 6; ++i) {
            if (probe_dot(set[i].n, centre) - set[i].d > radius) return true;
        }
        return false;
    };

    const bool guest_out = outside(planes);
    if (guest_out) ++visibility_probe_.rejected_guest;

    // Widen by exactly what the host does, taken from the renderer rather than
    // recomputed: load_matrices multiplies the horizontal perspective
    // coefficient m00 by horplus_scale(), and m00 = 1/tan(half hFOV), so the
    // widened half-angle is atan(tan(half) / scale). Reading the live value
    // means the probe and the projection cannot drift apart.
    const double scale = static_cast<double>(display_.coverage_scale());
    if (!(scale > 0.05) || !(scale <= 1.0)) {
        ++visibility_probe_.layout_bad;
        return;
    }
    const double widened_half = std::atan(std::tan(wide_half) / scale);
    visibility_probe_.guest_half_hfov_deg = wide_half * 180.0 / kProbePi;
    visibility_probe_.widened_half_hfov_deg = widened_half * 180.0 / kProbePi;
    visibility_probe_.guest_half_vfov_deg = narrow_half * 180.0 / kProbePi;

    ProbePlane widened[6];
    for (int i = 0; i < 6; ++i) widened[i] = planes[i];
    if (widened_half > wide_half) {
        const double delta = widened_half - wide_half;
        ProbeVec3 axis = probe_cross(planes[wide_i].n, planes[wide_j].n);
        const double axis_len = probe_len(axis);
        if (axis_len > 1e-6) {
            axis = probe_scale(axis, 1.0 / axis_len);
            for (int which = 0; which < 2; ++which) {
                const int index = which == 0 ? wide_i : wide_j;
                const int other = which == 0 ? wide_j : wide_i;
                const ProbeVec3 a1 = probe_rotate(planes[index].n, axis, delta);
                const ProbeVec3 a2 = probe_rotate(planes[index].n, axis, -delta);
                // For two inward side-plane normals of a symmetric frustum with
                // half-angle t, dot(nL, nR) = -cos(2t): it RISES as the frustum
                // opens (-1 at t=0, +1 at t=90). So the widening rotation is the
                // one that increases the dot product, not the one that makes the
                // normals more opposed. Getting this backwards narrows the
                // frustum and shows MORE rejections at wider aspect, which is
                // how it was caught.
                const ProbeVec3 chosen =
                    probe_dot(a1, planes[other].n) > probe_dot(a2, planes[other].n) ? a1 : a2;
                widened[index].n = chosen;
                widened[index].d = probe_dot(chosen, apex);
            }
        }
    }
    const bool widened_out = outside(widened);
    if (widened_out) ++visibility_probe_.rejected_widened;

    if (guest_out && !widened_out) {
        ++visibility_probe_.lost;
        const ProbeVec3 rel{centre.x - apex.x, centre.y - apex.y, centre.z - apex.z};
        const double dist = probe_len(rel);
        if (dist > std::max(radius, 1e-3)) {
            const double ang = std::asin(radius / dist) * 180.0 / kProbePi;
            if (ang > visibility_probe_.lost_max_halfangle_deg) {
                visibility_probe_.lost_max_halfangle_deg = ang;
            }
        }
    }

    // ---------------------------------------------------------------------
    // Independent clip-space reference test
    // ---------------------------------------------------------------------
    //
    // Everything above works in the guest plane space. This block answers a
    // different question, the one a popping prop actually poses: is the prop
    // still inside the frustum the host is DISPLAYING?
    //
    // It shares no arithmetic with the block above. The camera basis is
    // recovered from the frustum own geometry (the apex, plus the fact that the
    // four side planes outward normals sum to a vector pointing back along the
    // view axis), the sphere conservative world-space AABB is transformed into
    // that basis, and the eight corners are tested against the DISPLAYED
    // camera clip bounds. A prop is called invisible only when all eight
    // corners are outside the SAME clip plane.
    const double near_plane = rf(camera + 0x80u);
    const double far_plane = rf(camera + 0x84u);
    visibility_probe_.near_plane = near_plane;
    visibility_probe_.far_plane = far_plane;
    if (!(near_plane > 0.0) || !(far_plane > near_plane)) return;

    // View axis: the four side outward normals sum to a backward-pointing
    // vector for any frustum, symmetric or not.
    ProbeVec3 backward{0.0, 0.0, 0.0};
    for (int i = 0; i < 4; ++i) {
        backward.x += planes[side[i]].n.x;
        backward.y += planes[side[i]].n.y;
        backward.z += planes[side[i]].n.z;
    }
    const double backward_len = probe_len(backward);
    if (!(backward_len > 1e-6)) return;
    const ProbeVec3 forward = probe_scale(backward, -1.0 / backward_len);

    // Horizontal axis: the wide pair normals differ along it.
    ProbeVec3 right{planes[wide_j].n.x - planes[wide_i].n.x,
                    planes[wide_j].n.y - planes[wide_i].n.y,
                    planes[wide_j].n.z - planes[wide_i].n.z};
    const double right_len = probe_len(right);
    if (!(right_len > 1e-6)) return;
    right = probe_scale(right, 1.0 / right_len);
    ProbeVec3 up = probe_cross(forward, right);
    const double up_len = probe_len(up);
    if (!(up_len > 1e-6)) return;
    up = probe_scale(up, 1.0 / up_len);

    // The DISPLAYED camera: the guest own vertical coverage, and the horizontal
    // coverage the host is actually rendering with.
    const double display_tan_y = std::tan(narrow_half);
    const double display_tan_x = std::tan(widened_half);
    if (!(display_tan_x > 0.0) || !(display_tan_y > 0.0)) return;

    // The reference tests the SAME bound production tests - the sphere - against
    // the DISPLAYED frustum. Testing an inflated bound here would make the
    // reference more permissive for reasons unrelated to the camera and would
    // manufacture disagreements at every boundary.
    const ProbeVec3 v{centre.x - apex.x, centre.y - apex.y, centre.z - apex.z};
    const double vx = probe_dot(v, right);
    const double vy = probe_dot(v, up);
    const double vd = probe_dot(v, forward);  // depth, positive in front
    // A side plane of half-angle a has unit normal at angle a to the axis, so
    // the perpendicular distance from a point is the axis-aligned overshoot
    // divided by sqrt(1 + tan^2 a).
    const double inv_x = 1.0 / std::sqrt(1.0 + display_tan_x * display_tan_x);
    const double inv_y = 1.0 / std::sqrt(1.0 + display_tan_y * display_tan_y);
    const double over_right = (vx - display_tan_x * vd) * inv_x;
    const double over_left = (-vx - display_tan_x * vd) * inv_x;
    const double over_top = (vy - display_tan_y * vd) * inv_y;
    const double over_bottom = (-vy - display_tan_y * vd) * inv_y;
    const double over_near = near_plane - vd;
    const double over_far = vd - far_plane;
    const bool reference_out = over_right > radius || over_left > radius ||
                               over_top > radius || over_bottom > radius ||
                               over_near > radius || over_far > radius;
    // How much room the sphere still had against the tightest DISPLAYED plane.
    double tightest = over_right;
    tightest = std::max(tightest, over_left);
    tightest = std::max(tightest, over_top);
    tightest = std::max(tightest, over_bottom);
    tightest = std::max(tightest, over_near);
    tightest = std::max(tightest, over_far);
    const float worst_slack = static_cast<float>(radius - tightest);

    if (guest_out && !reference_out) {
        ++visibility_probe_.reference_visible_production_rejected;
        // Which production plane threw it away, identified by GEOMETRY. The
        // guest plane ORDER is not established anywhere, so an array index
        // carries no meaning; classify by which recovered axis the normal lies
        // along and in which direction.
        int blame = -1;
        double blame_margin = 0.0;
        int blame_role = 6;
        for (int i = 0; i < 6; ++i) {
            const double over = probe_dot(planes[i].n, centre) - planes[i].d - radius;
            if (over > blame_margin) {
                blame_margin = over;
                blame = i;
            }
        }
        if (blame >= 0) {
            const ProbeVec3 n = planes[blame].n;
            const double along_right = probe_dot(n, right);
            const double along_up = probe_dot(n, up);
            const double along_forward = probe_dot(n, forward);
            const double abs_right = std::fabs(along_right);
            const double abs_up = std::fabs(along_up);
            const double abs_forward = std::fabs(along_forward);
            if (abs_forward >= abs_right && abs_forward >= abs_up) {
                // Outward normal along -forward is the near plane; along
                // +forward is the far plane.
                blame_role = along_forward < 0.0 ? 4 : 5;
            } else if (abs_right >= abs_up) {
                blame_role = along_right < 0.0 ? 0 : 1;  // left : right
            } else {
                blame_role = along_up < 0.0 ? 2 : 3;  // bottom : top
            }
            ++visibility_probe_.disagree_by_plane[blame_role];
        }
        if (worst_slack > visibility_probe_.worst_disagreement_margin) {
            visibility_probe_.worst_disagreement_margin = worst_slack;
            visibility_probe_.worst_disagreement_sphere = sphere;
        }
        if (visibility_disagreements_.size() < 64u) {
            VisibilityDisagreement record;
            record.sphere_address = sphere;
            record.centre[0] = static_cast<float>(centre.x);
            record.centre[1] = static_cast<float>(centre.y);
            record.centre[2] = static_cast<float>(centre.z);
            record.radius = static_cast<float>(radius);
            record.rejecting_plane = static_cast<u32>(blame_role);
            record.production_margin = static_cast<float>(blame_margin);
            record.reference_slack = worst_slack;
            record.view_x = static_cast<float>(vx);
            record.view_y = static_cast<float>(vy);
            record.view_z = static_cast<float>(vd);
            visibility_disagreements_.push_back(record);
        }
    } else if (!guest_out && reference_out) {
        // The other direction: production kept something the displayed frustum
        // has no room for. Counted separately; it would mean the two volumes
        // disagree in BOTH directions.
        ++visibility_probe_.reference_rejected_production_visible;
    }
}

// ---------------------------------------------------------------------------
// Native visibility: OUR frustum test, replacing the original's.
// ---------------------------------------------------------------------------
//
// See the declaration in system.h for why this replaces rather than observes.
// Returns true when it answered; false leaves the decision to the recovered
// body.
bool System::native_frustum_test(Cpu& cpu) {
    const u32 camera = cpu.gpr[3];
    const u32 sphere = cpu.gpr[4];
    const auto sane = [&cpu](u32 p) { return guest_pointer(cpu, p); };
    if (!sane(camera) || !sane(sphere)) return false;

    // The native renderer draws each view's picture with render/view.h
    // displayed_window() (canonical_camera): the authored picture - square
    // pixels from the game's vertical view unless the game's framing is asked
    // for - fitted to the view. The guest's own cone is its horizontal view
    // window (the game's real one: docs/boundary-20261007/ORIGINAL.md 2 - the
    // "stuck" reading was a cheat in the reference emulator), which is narrower
    // than the picture, so there is nothing to defer to.
    const bool width_follows_height = display_.width_follows_height();
    if (width_follows_height && !display_.configured()) {
        ++native_visibility_stats_.deferred_unconfigured;
        return false;
    }

    // The legacy renderer: the widening the host is actually displaying with.
    // horplus_scale() is the multiplier applied to the projection coefficient
    // m00, which is the reciprocal of the horizontal extent, so the extent scale
    // is its inverse.
    const float m00_scale = display_.coverage_scale();
    if (!width_follows_height && (!(m00_scale > 0.05f) || !(m00_scale <= 1.0f))) return false;
    const float extent_scale = 1.0f / m00_scale;

    // At unity the displayed cone IS the original's cone, so the native answer
    // and the original's are the same function. Defer, so the bit-exact 4:3
    // reference keeps the original's own fused arithmetic and the byte-identity
    // gate cannot move by a ULP.
    if (!width_follows_height && extent_scale == 1.0f) {
        // TWO DIFFERENT THINGS used to share this counter, and only one of them
        // is benign. "The display really is 4:3, so the cones are identical" is
        // correct and intended. "The renderer has not been told its output size
        // yet, so horplus_scale() is still its initial 1.0" is a FIRST-FRAME
        // POP: culling silently reverts to the guest's narrow cone for however
        // many frames that lasts, and the old single counter made it
        // indistinguishable from the 4:3 case. Deferring is still the safe
        // answer in both cases; what was missing was being able to see which.
        if (display_.configured()) {
            ++native_visibility_stats_.deferred_unity;
        } else {
            ++native_visibility_stats_.deferred_unconfigured;
        }
        return false;
    }

    // ONE CAMERA MODEL. This used to recover its own camera from the six
    // frustum planes - an apex search over plane triples, a basis inferred from
    // the difference of two normals, and NINE separate bail-outs that each fell
    // back to the guest's own narrow cone without saying so.
    //
    // The one that was genuinely fragile is the apex search's acceptance test:
    // a candidate had to lie on exactly four planes within an ABSOLUTE residual
    // of 1e-2, measured on plane equations whose world coordinates run to
    // thousands. At Final Haunt's ~3,400 the float32 ULP on that residual is
    // 4.9e-4 and the three-plane solve lands near 3e-3 - inside the tolerance,
    // but only just. Near the 20,000 far plane the ULP is 1.95e-3 and the
    // residual reaches the threshold itself. So the failure was silent,
    // coordinate-magnitude dependent and therefore stage dependent, which is
    // the worst shape a bug can have.
    //
    // NOT the opposing-pair threshold. An earlier draft of this comment blamed
    // the `dot > -0.2` gate for imposing a 78.463 deg ceiling below the
    // displayed 78.777 deg at 16:9. That is wrong and it is worth recording
    // why, because the arithmetic looks convincing: the gate only ever saw the
    // GUEST planes at camera+0x94, which are the 63.2501 deg cone (dot =
    // -0.45010, nowhere near the gate). The widening was applied afterwards, to
    // cam.extents, so the widened cone never existed as plane data for the gate
    // to reject. It could not fire.
    //
    // It now takes the same exact camera as everything else, so world culling
    // and world-anchored UI cannot disagree about where the screen ends.
    camera::Camera cam;
    if (!canonical_camera(cpu, camera, cam)) {
        ++native_visibility_stats_.deferred_layout;
        return false;
    }
    const camera::Frustum world = camera::world_frustum(cam);

    const auto rfs = [&cpu](u32 address) {
        const u32 bits = cpu.mem_read32(address);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    camera::Sphere s;
    s.x = rfs(sphere + 0u);
    s.y = rfs(sphere + 4u);
    s.z = rfs(sphere + 8u);
    s.radius = rfs(sphere + 12u);
    if (!(s.radius >= 0.0f) || s.radius > 1.0e7f) {
        ++native_visibility_stats_.deferred_layout;
        return false;
    }

    const camera::VisibilityResult r = camera::classify_sphere(world, s);
    // The original's return convention, read at 0x80486718.
    u32 result = 2u;  // rwSPHEREINSIDE
    switch (r.visibility) {
        case camera::Visibility::Outside:
            result = 0u;
            ++native_visibility_stats_.outside;
            break;
        case camera::Visibility::Intersecting:
            result = 1u;
            ++native_visibility_stats_.boundary;
            break;
        case camera::Visibility::Inside:
            result = 2u;
            ++native_visibility_stats_.inside;
            break;
    }
    // Would the original have thrown this away? Counting it is what proves the
    // fix did something - but it costs 24 guest reads on EVERY visible sphere,
    // which on a 133,564-test route is ~2.5M reads bought entirely for a
    // diagnostic. It belongs behind the probe, not on the shipping path.
    if (visibility_probe_enabled_ && r.visible()) {
        bool original_out = false;
        const auto rf = [&cpu](u32 address) {
            const u32 bits = cpu.mem_read32(address);
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return static_cast<double>(value);
        };
        for (int i = 0; i < 6 && !original_out; ++i) {
            const u32 base = camera + 0x94u + static_cast<u32>(i) * 20u;
            const double nx = rf(base + 0u), ny = rf(base + 4u), nz = rf(base + 8u);
            const double d = rf(base + 12u);
            if (nx * s.x + ny * s.y + nz * s.z - d > s.radius) original_out = true;
        }
        if (original_out) ++native_visibility_stats_.admitted;
    }

    cpu.gpr[3] = result;
    ++native_visibility_stats_.decided;
    return true;
}

// ---------------------------------------------------------------------------
// The camera's visibility volume, built from the displayed picture.
// ---------------------------------------------------------------------------
//
// RenderWare's perspective frustum build (_rwCameraValUpdatePerspectiveFrustum
// 0x80485318, run by _rwCameraSync 0x804862DC whenever the camera's frame is
// dirty at a camera begin - docs/renderer-re-20261007/02_RW_CORE.md 3.6.4)
// derives the camera's visibility volume - eight corners and six planes; the
// sync then bounds the corners - from the camera's view window, because on the
// console the window WAS the picture. With the native renderer it is not: the
// camera carries the authored picture, the canvas mapping every canvas-unit
// reader of the camera needs (docs/boundary-20261007/DESIGN.md 8), while the
// output shows displayed_window(), wider on a wider view. Built from the window,
// the volume rejected world sectors whose boxes the picture's planes accept -
// RenderWare's sector collection 0x804626A8 tests the planes and the corners'
// box directly, and the atomics tied to a rejected sector never reach the
// sphere test (DESIGN.md 7, measured). Built here from the one camera model -
// the frustum the renderer draws and native_frustum_test decides with - so
// every RenderWare visibility test reading the camera decides against the
// actual viewport.
//
// RenderWare's layout and order: corners +0x124 (near: top-left, top-right,
// bottom-right, bottom-left as the picture shows them) and +0x154 (far, the
// same order); planes +0x94, 0x14 each - far, near, right, top, left, bottom -
// an outward normal, the distance (a point is outside when n.p - d > 0) and,
// per axis, the box corner the plane test takes (+0x10..+0x12: 1 = the box's
// inf where the normal's component has its sign bit clear, else 0 = its sup).
// The legacy renderer keeps RenderWare's own build (its widening is
// load_matrices'; the 4:3 reference stays the original's arithmetic).
bool System::native_frustum_build(Cpu& cpu) {
    if (!native_visibility_ || !display_.width_follows_height()) return false;
    const u32 camera_ptr = cpu.gpr[3];
    const auto rf = [&cpu](u32 address) {
        const u32 bits = cpu.mem_read32(address);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    if (!guest_pointer(cpu, camera_ptr)) {
        ++frustum_build_stats_.declined_camera;
        return false;
    }
    // RenderWare offsets the eye by the view offset (+0x78 / +0x7C); the canonical camera has no shear and this game
    // sets none (02_RW_CORE.md 3.6.3), so an offset is counted and left to RenderWare rather than dropped.
    if (rf(camera_ptr + 0x78u) != 0.0f || rf(camera_ptr + 0x7Cu) != 0.0f) {
        ++frustum_build_stats_.declined_offset;
        return false;
    }
    camera::Camera cam;
    if (!canonical_camera(cpu, camera_ptr, cam)) {
        ++frustum_build_stats_.declined_camera;
        return false;
    }
    const auto wf = [&cpu](u32 address, float value) {
        u32 bits;
        std::memcpy(&bits, &value, sizeof(bits));
        cpu.mem_write32(address, bits);
    };
    const std::array<camera::Corner, 4> near_corners = camera::frustum_section(cam, cam.near_plane);
    const std::array<camera::Corner, 4> far_corners = camera::frustum_section(cam, cam.far_plane);
    for (u32 i = 0; i < 4u; ++i) {
        for (u32 axis = 0; axis < 3u; ++axis) {
            wf(camera_ptr + 0x124u + i * 12u + axis * 4u, near_corners[i][axis]);
            wf(camera_ptr + 0x154u + i * 12u + axis * 4u, far_corners[i][axis]);
        }
    }
    const camera::Frustum world = camera::world_frustum(cam);
    static constexpr camera::FrustumPlane kRenderWareOrder[6] = {
        camera::FrustumPlane::Far, camera::FrustumPlane::Near, camera::FrustumPlane::Right,
        camera::FrustumPlane::Top, camera::FrustumPlane::Left, camera::FrustumPlane::Bottom};
    for (u32 i = 0; i < 6u; ++i) {
        const camera::Plane& p = world.planes[static_cast<u32>(kRenderWareOrder[i])];
        const u32 base = camera_ptr + 0x94u + i * 0x14u;
        wf(base + 0x00u, p.nx);
        wf(base + 0x04u, p.ny);
        wf(base + 0x08u, p.nz);
        wf(base + 0x0Cu, p.d);
        cpu.mem_write8(base + 0x10u, static_cast<u8>(std::signbit(p.nx) ? 0u : 1u));
        cpu.mem_write8(base + 0x11u, static_cast<u8>(std::signbit(p.ny) ? 0u : 1u));
        cpu.mem_write8(base + 0x12u, static_cast<u8>(std::signbit(p.nz) ? 0u : 1u));
    }
    ++frustum_build_stats_.built;
    cpu.gpr[3] = camera_ptr;
    return true;
}

// ---------------------------------------------------------------------------
// The canonical native camera. ONE source, read exactly.
// ---------------------------------------------------------------------------
//
// Everything that depends on camera coverage - world visibility, world-anchored
// UI, the health bar - takes its camera from here, so there is exactly one
// implementation and they cannot disagree.
//
// THIS USED TO BE ARCHAEOLOGY. It intersected triples of the six RwCamera
// frustum planes looking for a point lying on exactly four of them, with an
// ABSOLUTE 1e-2 tolerance on plane equations whose world coordinates run to
// thousands, then inferred a basis from the plane normals. That method had to
// guess three things the camera already stores - the apex, the axis signs and
// the handedness - and it got the first one from a search, the second from the
// difference of two normals (which yields the RwCamera frame's right axis, the
// NEGATION of the guest's projection right axis, point-reflecting every
// world-anchored position) and the third from a guard that is unreachable.
//
// The camera is sitting in memory. RwCamera+0x04 is its RwFrame and the frame's
// local-to-world matrix is at frame+0x50, proved by the guest's own frustum
// builder at 0x80485AFC, which loads exactly +0x50/+0x60/+0x70/+0x80 after
// `lwz r5,4(r31)`. RwFrameGetLTM (0x804882E8) returns frame+0x50 and syncs the
// dirty bit first, and the camera sync (0x80271F9C) writes that frame from the
// game camera object every frame, so the two cameras share an eye by
// construction.
//
// AXIS SIGNS, derived from both projection routes and agreeing:
//   - the gauge's own projector 0x8000F654 uses camobj+0x3C, whose rows are
//     (R, U, F) with F pointing BACKWARD;
//   - RenderWare's route loads diag(-1, +1, -1) * view into GX
//     (GXLoadPosMtxImm at 0x804961D4, with fneg on rows 0 and 2), and its
//     own viewMatrix builder applies -0.5 to the x row at 0x80485B04.
//   Both give:  screen +X = -ltm.right,  screen up = +ltm.up,
//               increasing depth = +ltm.at.
//   Equivalently (R, U, F) = (-A, +B, -C) for the frame's (right, up, at).
//   A projector that takes `right` straight from the frame draws the MIRROR of
//   the guest gauge, which is why the sign is spelled out here rather than
//   left to a decoder's convention.
//
// Cached on the values it actually reads. It deliberately does NOT share the
// visibility path's cache: that cache is validated by a different function
// which never writes the camera, so a hit there returned a DEFAULT-CONSTRUCTED
// camera - identity basis, zero origin - which silently turns depth into
// -world_z. On Final Haunt that is ~3,400 where the true axial depth is ~195,
// so every world-anchored element failed its authored distance rule and
// nothing drew, with no error reported anywhere.
bool System::canonical_camera(Cpu& cpu, camera::Camera& out) {
    // The in-stage camera, published at 0x8056FF1C. Callers that already hold a
    // camera pointer - the frustum test is handed one in r3 - pass it instead,
    // so the two can never silently be different objects.
    return canonical_camera(cpu, cpu.mem_read32(0x8056FF1Cu), out);
}

bool System::canonical_camera(Cpu& cpu, u32 camera_ptr, camera::Camera& out) {
    const auto sane = [&cpu](u32 p) { return guest_pointer(cpu, p); };
    if (!sane(camera_ptr)) return false;
    const u32 frame_ptr = cpu.mem_read32(camera_ptr + 0x04u);
    if (!sane(frame_ptr)) return false;

    const float m00_scale = display_.coverage_scale();
    const camera::Coverage coverage = camera::Coverage::from_affine_scale(m00_scale);
    // The native renderer (display_.width_follows_height()) draws a view's
    // picture with render/view.h displayed_window(): the view's aspect is the
    // output's times the camera raster's share of its top-level raster (a
    // split-screen half has its own shape), the canvas's authored aspect the
    // 4:3 picture's share the same way - the camera that culls is the camera it
    // draws (docs/lift-20261006/R2_DRIVER.md 4.12, docs/boundary-20261007). The
    // lifted build's camera already carries the authored window (its camera
    // sync, scaffold projection.cpp camera_picture_window): displayed_window
    // gives the same cone from it, bit for bit.
    const bool width_follows_height = display_.width_follows_height();
    float view_aspect = 0.0f;
    float canvas_aspect = 4.0f / 3.0f;
    if (width_follows_height) {
        float w = 0.0f, h = 0.0f, top_w = 0.0f, top_h = 0.0f;
        const u32 raster = cpu.mem_read32(camera_ptr + 0x60u);
        if (sane(raster)) {
            u32 top = cpu.mem_read32(raster + 0x00u);
            if (!sane(top)) top = raster;
            w = static_cast<float>(cpu.mem_read32(raster + 0x0Cu));
            h = static_cast<float>(cpu.mem_read32(raster + 0x10u));
            top_w = static_cast<float>(cpu.mem_read32(top + 0x0Cu));
            top_h = static_cast<float>(cpu.mem_read32(top + 0x10u));
        }
        const shadow::render::RasterView view = shadow::render::raster_view(w, h, top_w, top_h, display_.aspect());
        view_aspect = view.view_aspect;
        canvas_aspect = view.canvas_aspect;
        if (!(view_aspect > 0.0f) || !std::isfinite(view_aspect)) return false;
    }

    const auto rf = [&cpu](u32 address) {
        const u32 bits = cpu.mem_read32(address);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };

    // The cache key is every word the result depends on: the frame's 12 LTM
    // floats, the two view-window tangents, near, far, and the coverage. One
    // writer, one key, one value - and a miss rebuilds everything.
    // The key is every word the result depends on, INCLUDING which camera it
    // came from: two callers holding different camera objects must not share
    // one cached answer.
    std::array<u32, 18> key{};
    key[17] = camera_ptr;
    for (u32 i = 0; i < 12u; ++i) {
        key[i] = cpu.mem_read32(frame_ptr + 0x50u + (i / 3u) * 16u + (i % 3u) * 4u);
    }
    key[12] = cpu.mem_read32(camera_ptr + 0x68u);
    key[13] = cpu.mem_read32(camera_ptr + 0x6Cu);
    key[14] = cpu.mem_read32(camera_ptr + 0x80u);
    key[15] = cpu.mem_read32(camera_ptr + 0x84u);
    // The horizontal rule's input: the legacy coverage, or the view's aspect (negated, so the two rules can
    // never share a key; the framing is fixed for the process, the canvas aspect follows the raster words).
    const float horizontal_key = width_follows_height ? -view_aspect : coverage.extent_scale;
    std::memcpy(&key[16], &horizontal_key, sizeof(float));
    if (native_camera_valid_ && key == native_camera_key_) {
        out = native_camera_;
        return true;
    }

    const float view_x = rf(camera_ptr + 0x68u);
    const float view_y = rf(camera_ptr + 0x6Cu);
    const float near_plane = rf(camera_ptr + 0x80u);
    const float far_plane = rf(camera_ptr + 0x84u);
    if (!(view_x > 1.0e-6f) || !(view_y > 1.0e-6f) || !(near_plane > 0.0f) ||
        !(far_plane > near_plane)) {
        return false;
    }

    // frame+0x50 right, +0x60 up, +0x70 at, +0x80 pos. RwMatrix pads each
    // vector to 16 bytes, which is why the stride is 16 and not 12.
    const float right[3] = {-rf(frame_ptr + 0x50u), -rf(frame_ptr + 0x54u),
                            -rf(frame_ptr + 0x58u)};
    const float up[3] = {rf(frame_ptr + 0x60u), rf(frame_ptr + 0x64u), rf(frame_ptr + 0x68u)};
    const float at[3] = {rf(frame_ptr + 0x70u), rf(frame_ptr + 0x74u), rf(frame_ptr + 0x78u)};
    const float pos[3] = {rf(frame_ptr + 0x80u), rf(frame_ptr + 0x84u), rf(frame_ptr + 0x88u)};

    camera::Camera cam;
    cam.type = camera::ProjectionType::Perspective;
    // Hor+ scales the EXTENT, so the encoded matrix and the analytic planes are
    // both rebuilt from the changed physical quantity and cannot disagree.
    if (width_follows_height) {
        const shadow::render::ViewWindow shown = shadow::render::displayed_window(
            shadow::render::ViewWindow{view_x, view_y}, canvas_aspect, view_aspect,
            display_.game_framing() ? shadow::render::Framing::GameWindow
                                    : shadow::render::Framing::SquarePixels);
        cam.extents = camera::FrustumExtents::symmetric(shown.x, shown.y);
    } else {
        cam.extents = camera::FrustumExtents::symmetric(view_x * coverage.extent_scale, view_y);
    }
    cam.near_plane = near_plane;
    cam.far_plane = far_plane;
    cam.view_to_world = camera::decode::transform_from_renderware_ltm(right, up, at, pos);

    // Refuse anything that is not a rotation rather than publish a camera whose
    // depth is not a world distance.
    const std::array<float, 9>& b = cam.view_to_world.basis;
    for (int j = 0; j < 3; ++j) {
        for (int k = j; k < 3; ++k) {
            const float dot = b[static_cast<std::size_t>(j)] * b[static_cast<std::size_t>(k)] +
                              b[static_cast<std::size_t>(j) + 3u] *
                                  b[static_cast<std::size_t>(k) + 3u] +
                              b[static_cast<std::size_t>(j) + 6u] *
                                  b[static_cast<std::size_t>(k) + 6u];
            if (!(std::fabs(dot - ((j == k) ? 1.0f : 0.0f)) <= 1.0e-3f)) return false;
        }
    }

    native_camera_ = cam;
    native_camera_key_ = key;
    native_camera_valid_ = true;
    ++native_visibility_stats_.rebuilds;
    out = cam;
    return true;
}

// ---------------------------------------------------------------------------
// The native health bar. Replaces the guest draw callback 0x800BF634 outright.
// ---------------------------------------------------------------------------
//
// The handler returns true, so the whole guest body is suppressed: no gate, no
// projection into a 640x480 canvas, no string-keyed cast lookups, no CSD scene
// step, and none of the ~148 GX commands per gauge of which 8 carried
// information. Nothing is re-entered and nothing is filtered - this is the
// implementation, not a wrapper around one.
//
// The guest's PRODUCER is untouched. EnemyDispCommon::Update still composes the
// world anchor (frame position, plus the attach transform and the half-height
// lift when an attach matrix resolves), still computes the HP rate, and still
// evaluates the per-enemy-type show/hide policy. That is enemy behaviour and it
// stays with the enemy; this layer is a pure consumer of the record it writes.
//
// Because the handler runs at the function ENTRY, before the prologue, r3 is
// the gauge object and the callee-saved registers still belong to the CALLER.
// Nothing here reads them.
bool System::native_health_bar_draw(Cpu& cpu) {
#if defined(SHADOW_RENDER_LIFTED)
    // G5: only the legacy renderer had a frame segment to submit into; the lifted build declines, as the legacy
    // build did without its capture.
    (void)cpu;
    return false;
#else
    RwCapture* capture = native_capture_.get();
    // Only the native renderer has a frame segment to submit into. Under the
    // legacy backend, decline and let the original draw itself.
    if (capture == nullptr) return false;

    const auto sane = [&cpu](u32 p) { return guest_pointer(cpu, p); };
    const u32 gauge = cpu.gpr[3];
    if (!sane(gauge)) return false;

    camera::Camera cam;
    if (!canonical_camera(cpu, cam)) {
        ++health_bar_camera_declines_;
        return false;
    }

    // The presentation rectangle. The guest raster IS that rectangle here: the
    // canonical camera is already widened, so |ndc| <= 1 covers the full raster
    // width, and the presenter fits the raster into the window at present time.
    // Taking the numbers from the shared canvas constants rather than writing
    // 640 and 480 keeps one definition.
    const world_anchor::ContentViewport viewport{0.0f, 0.0f, kVirtualHudWidth, kVirtualHudHeight};

    // The shared global HUD-visible flag - the original's first gate.
    const bool hud_visible = cpu.mem_read8(0x8057E8A8u) != 0u;
    if (!health_bar_layer_.begin_frame(cam, viewport, hud_visible)) return false;

    const auto rf = [&cpu](u32 address) {
        const u32 bits = cpu.mem_read32(address);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };

    health_bar::Gauge object;
    object.signal.world_x = rf(gauge + 0x20u);
    object.signal.world_y = rf(gauge + 0x24u);
    object.signal.world_z = rf(gauge + 0x28u);
    object.signal.rate = rf(gauge + 0x10u);
    object.signal.max_hp = rf(gauge + 0x40u);
    object.signal.enabled = cpu.mem_read8(gauge + 0x0Cu) != 0u;
    // Authored, PER INSTANCE. 20 / 200 / 50 are the constructor's defaults, not
    // the rule: Devil Doom's wing gauges author 500 and 100, so reading them
    // from the object is the difference between the final boss's bars working
    // and being clipped 300 units early at half size.
    object.authored.min_depth = rf(gauge + 0x18u);
    object.authored.max_depth = rf(gauge + 0x1Cu);
    object.authored.base_scale = rf(gauge + 0x14u);

    const health_bar::Placement placement = health_bar_layer_.place(object, health_bar_composition_);
    health_bar_layer_.record(placement);
    // Rejection is total, and it is HANDLED: the original submits nothing and
    // steps no animation on a rejected gauge, which is exactly what returning
    // true with no submission does.
    if (!placement.visible) return true;

    health_bar::Vertex local[health_bar::kMaxVerticesPerGauge];
    const u32 count =
        health_bar_layer_.build(placement, health_bar_composition_, local,
                                health_bar::kMaxVerticesPerGauge);
    if (count == 0u) return true;

    std::array<shadow::render::RenderVertex, health_bar::kMaxVerticesPerGauge> vertices{};
    for (u32 i = 0; i < count; ++i) {
        shadow::render::RenderVertex& v = vertices[i];
        v.position[0] = local[i].x;
        v.position[1] = local[i].y;
        // No z. Depth is genuinely absent from this layer; the intent lives in
        // the pipeline state (compare ALWAYS, write off), never in a value.
        v.position[2] = 0.0f;
        v.color0[0] = local[i].rgba[0];
        v.color0[1] = local[i].rgba[1];
        v.color0[2] = local[i].rgba[2];
        v.color0[3] = local[i].rgba[3];
    }
    if (capture->submit_native_ui(vertices.data(), count, viewport.width, viewport.height)) {
        health_bar_layer_.stats().quads += count / 4u;
    }
    return true;
#endif  // G5
}

void System::set_memory_card_path(std::filesystem::path path,
                                  std::filesystem::path gci_working_path) {
    // The settings fall back to the factory image first, so an unusable card
    // (set_path throws) leaves no stale identity behind.
    ipl_->reset_settings();
    card_->set_path(std::move(path), std::move(gci_working_path));
    if (card_->present()) ipl_->bind_card(card_->source_path(), card_->backing_path(), card_->image());
}

bool System::memory_card_present() const noexcept {
    return card_->present();
}

const std::filesystem::path& System::memory_card_backing_path() const noexcept {
    return card_->backing_path();
}

const std::filesystem::path& System::memory_card_sram_path() const noexcept {
    return ipl_->companion_path();
}

// A menu sound as the page plays it: PlaySimple(GetSoundManager(), id, 0, 0) through the native SE manager, the
// caller's argument registers kept.
void System::play_menu_sound(void* system, Cpu& cpu, u32 id) {
    auto* self = static_cast<System*>(system);
    const u32 manager = cpu.mem_read32(se_manager::kManagerPointer);
    if (manager == 0u) return;
    const std::array<u32, 4> saved{cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6]};
    cpu.gpr[3] = manager;
    cpu.gpr[4] = id;
    cpu.gpr[5] = 0u;
    cpu.gpr[6] = 0u;
    se_manager::serve_play_simple(cpu, self->se_services_);
    cpu.gpr[3] = saved[0];
    cpu.gpr[4] = saved[1];
    cpu.gpr[5] = saved[2];
    cpu.gpr[6] = saved[3];
}

bool System::mount_assets(const std::filesystem::path& files_root, std::vector<u8> identity,
                          std::string& error) {
    if (pc_save_text_) {
        assets_.replace(std::string(save_text::kEnglishTable),
                        [](const assets::AssetSystem::TreeReader& read, std::vector<u8>& bytes,
                           std::string& build_error) {
                            std::vector<u8> table;
                            std::vector<u8> glyphs;
                            if (!read(save_text::kEnglishTable, table) || !read(save_text::kEnglishGlyphs, glyphs)) {
                                build_error = "the English save-message table or its glyph list is missing";
                                return false;
                            }
                            return save_text::rewrite_table(table, glyphs, save_text::pc_english_messages(), bytes,
                                                            build_error);
                        });
    }
    // The Options menu's Display tile (gekko/native_options_display.h), per language: the tile's art in Options2,
    // and the front-end table with the tile's help line - the English one also with the save screen's PC words (its
    // copies of the save messages, the boot's autosave notice among them, found by their original text). Reading the
    // table's glyph sheets here hands them to the page's overlay.
    for (u32 index = 0; index < options_display::kLanguageSuffix.size(); ++index) {
        const auto language = static_cast<options_display::Language>(index);
        const std::string suffix(options_display::kLanguageSuffix[index]);
        assets_.replace("csdFiles/Options_" + suffix + ".txd",
                        [suffix](const assets::AssetSystem::TreeReader& read, std::vector<u8>& bytes,
                                 std::string& build_error) {
                            std::vector<u8> txd;
                            if (!read("csdFiles/Options_" + suffix + ".txd", txd)) {
                                build_error = "csdFiles/Options_" + suffix + ".txd is missing";
                                return false;
                            }
                            return options_display::place_tile_art(txd, bytes, build_error);
                        });
        const std::string base = "FONTS/ADVERTISE/Advertise_" + suffix;
        const bool english_save_words = pc_save_text_ && language == options_display::Language::EN;
        assets_.replace(base + ".fnt", [this, base, language, english_save_words](
                                           const assets::AssetSystem::TreeReader& read, std::vector<u8>& bytes,
                                           std::string& build_error) {
            std::vector<u8> table, sheets;
            if (!read(base + ".fnt", table) || !read(base + ".txd", sheets)) {
                build_error = base + ".fnt or its glyph sheets are missing";
                return false;
            }
            std::vector<std::vector<u8>> glyph_lists;
            for (u32 n = 0; n < 100u; ++n) {
                char name[16];
                std::snprintf(name, sizeof(name), "%02u.met", n);
                std::vector<u8> met;
                if (!read(base + name, met)) break;
                glyph_lists.push_back(std::move(met));
            }
            if (glyph_lists.empty()) {
                build_error = base + " has no glyph list";
                return false;
            }
            std::u16string on, off;
            save_text::message_text(table, 346u, on);  // the game's own ON / OFF
            save_text::message_text(table, 347u, off);
            options_display_.set_font_files(language, std::move(sheets), glyph_lists, on, off);
            if (english_save_words) {
                std::vector<u8> save_table, reworded;
                std::vector<save_text::Rewording> rewordings;
                u32 count = 0;
                if (!read(save_text::kEnglishTable, save_table) ||
                    !save_text::rewordings_of(save_table, save_text::pc_english_messages(), rewordings,
                                              build_error) ||
                    !save_text::reword_table(table, glyph_lists[0], rewordings, reworded, count, build_error)) {
                    if (build_error.empty()) build_error = "the English save-message table is missing";
                    return false;
                }
                table = std::move(reworded);
            }
            return options_display::reword_help(table, glyph_lists, language, bytes, build_error);
        });
    }
    if (!assets_.mount(files_root, std::move(identity), error)) return false;
    // The prompts' controller buttons: patched into the button sheets as the game reads them.
    button_glyphs_.attach(assets_);
    return true;
}

void System::enable_file_api() {
    if (file_api_ != nullptr || !assets_.mounted()) return;
    file_api_ = std::make_unique<NativeFileApi>(cpu_, memory_, assets_);
    file_api_->install();
    // PRS archive members decoded on workers as soon as their archive is read (prs_ahead.h); the expand owner
    // serves a call from one only when the guest's stream equals it (GameMemory::serve_prs_ahead). Diagnostic
    // builds: GEKKO_PRS_AHEAD=0 decodes every call as before.
#if !defined(GEKKO_SHIPPING)
    const char* ahead_setting = std::getenv("GEKKO_PRS_AHEAD");
    const bool ahead = ahead_setting == nullptr || ahead_setting[0] != '0';
#else
    const bool ahead = true;
#endif
    if (ahead) {
        prs_ahead_ = std::make_unique<PrsAhead>(assets_.resources());
        PrsAhead* const decoder = prs_ahead_.get();
        file_api_->set_read_observer([decoder](assets::AssetId id, const std::string& path, u64 offset,
                                               u32 address, u64 length) {
            decoder->announce(id, path, offset, address, length);
        });
        game_memory_->set_prs_ahead(
            [decoder](const u8* first, const std::function<bool(const PrsDecoded&)>& accept) {
                return decoder->take(PrsAhead::key_of(first), accept);
            });
    }
    cri_file_device_ = std::make_unique<NativeCriFileDevice>(cpu_, assets_, *file_api_);
    cri_file_device_->install();
    rtgcnfs_ = std::make_unique<NativeRtGcnFs>(cpu_, assets_, *file_api_);
    rtgcnfs_->install();
}

void System::enable_gameplay_scheduler() {
    if (gameplay_scheduler_ != nullptr) return;
    gameplay_scheduler_ = std::make_unique<GameplayScheduler>(cpu_, memory_);
    gameplay_scheduler_->install();
    gameplay_scheduler_->set_boundary_hook([this] {
        if (!boundary_stop_pending_) return;
        boundary_stop_pending_ = false;
        cpu_.request_stop(boundary_stop_reason_, boundary_stop_detail_);
    });
}

void System::request_stop_between_traversals(StopReason reason, std::string detail) {
    if (gameplay_scheduler_ == nullptr || !gameplay_scheduler_->busy()) {
        boundary_stop_pending_ = false;
        cpu_.request_stop(reason, std::move(detail));
        return;
    }
    if (boundary_stop_pending_) return;
    boundary_stop_pending_ = true;
    boundary_stop_reason_ = reason;
    boundary_stop_detail_ = std::move(detail);
}

void System::load_dol(const DolProgram& program) {
    // The image (and, in M1, the range the game's OS heap still runs over)
    // starts from zero, as on every boot.
    for (const GuestMemory::Region& region : memory_.regions()) {
        if (region.kind == GuestMemory::RegionKind::Image || region.kind == GuestMemory::RegionKind::LegacyHeap) {
            memory_.clear_block(region.base, region.committed);
        }
    }
    // The DOL BSS extent deliberately spans the small-data sections that follow
    // it, so it has to be cleared before the sections are placed. Clearing it
    // afterwards would erase initialised .sdata/.sdata2 - including the SDA
    // function-pointer table the startup code calls through.
    if (program.bss_size != 0) {
        memory_.clear_block(program.bss_address, program.bss_size);
    }
    for (const auto& section : program.sections) {
        if (section.address < 0x80000000u) {
            continue;
        }
        memory_.write_block(section.address, program.bytes.data() + section.file_offset, section.size);
    }
    cpu_.set_entry(program.entry_point);
}

void System::apply_boot_configuration(const BootConfiguration& configuration) {
    // The IPL leaves the low-memory boot block in this state before the game
    // entry point runs. Values follow YAGCD section 4.2.
    if (configuration.disc_header.size() >= 0x20) {
        memory_.write_block(0x80000000u, configuration.disc_header.data(), 0x20);
    }
    if (!configuration.bi2.empty()) {
        const u32 size = static_cast<u32>(std::min<std::size_t>(configuration.bi2.size(), 0x2000));
        memory_.write_block(kBi2GuestAddress, configuration.bi2.data(), size);
    }

    // M1 (docs/memory-architecture-20261006/README.md section 4): the OS is
    // still told the size of the range its own heap runs over; M2 replaces
    // the OS heap and with it what these words mean.
    constexpr u32 kLegacyOsMemorySize = GuestMemory::kLegacyHeapEnd - GuestMemory::kImageBase;
    memory_.write32(0x80000020u, 0x0d15ea5eu);              // booted from the boot ROM
    memory_.write32(0x80000028u, kLegacyOsMemorySize);      // memory size the OS reports
    memory_.write32(0x8000002cu, configuration.console_type);
    memory_.write32(0x800000ccu, configuration.pal ? 1u : 0u);  // VI mode left by the IPL

    // ... and the display that word describes (NativeVideo). VIInit keeps the
    // IPL's mode only when the display is already running; a display that is
    // off gets the SDK's cold start with a hardcoded VI_NTSC, which rewrites
    // 0x800000cc to 0 and starts NTSC timing - every PAL-gated screen, the
    // 50/60 Hz selector first, then correctly refuses to run. The PAL display
    // runs from boot (one retrace per field), so the SDK's first
    // VIWaitForRetrace returns before VIConfigure has run.
    video_->boot(configuration.pal);
    memory_.write32(0x800000f0u, kLegacyOsMemorySize);      // the same, as the "simulated" size
    memory_.write32(0x800000f4u, kBi2GuestAddress);         // pointer to the BI2 block
    memory_.write32(0x800000f8u, 0x09a7ec80u);              // bus clock: 162 MHz
    memory_.write32(0x800000fcu, 0x1cf7c580u);              // core clock: 486 MHz

    if (assets_.mounted()) {
        // The arena limit the game has always been given: just below the BI2
        // block, minus the size the disc's directory table had (one 12-byte
        // record per entry plus every name - the boundary it was mastered
        // with). Nothing lives in that region: no directory table or table
        // address exists in guest memory (0x80000038/0x8000003C stay zero),
        // and no API hands out names from it (the game's only directory scan
        // is native, docs/DISC_SYSTEM_MAP.md cut 2).
        const u64 size = assets_.names().reservation_bytes();
        const u32 aligned = static_cast<u32>((size + 31u) & ~u64{31u});
        arena_high_ = (kBi2GuestAddress - aligned) & ~31u;
        memory_.write32(0x80000034u, arena_high_);  // arena high
    }
}

void System::start() {
    // Called once, from the boot path, and nothing wraps it there. Without this
    // the HLE installation below reads as an unexplained gap before the first
    // cpu/host-service zone. The boot spine's other zones live in the caller
    // under a boot/ prefix; this one keeps the subsystem prefix, and if the
    // spine should read as one group it is renamed once, not spelled both ways.
    GEKKO_PROFILE_ZONE("cpu/start");
    cpu_.clear_stop();

    // The Metrowerks 64-bit divide helpers (guest software, not hardware),
    // served by one host division; a zero divisor declines to the recovered
    // body. --no-divide-intrinsics runs the recovered loops for A/B.
    if (divide_intrinsics_) {
        cpu_.install_hle(
            0x803A1F54u,
            [this](Cpu& cpu) {
                if (!unsigned_divide_64_intrinsic(cpu)) {
                    ++divide_intrinsic_declines_;
                    return false;
                }
                ++unsigned_divide_intrinsic_calls_;
                return true;
            },
            "unsigned 64-bit compiler divide intrinsic");
        cpu_.install_hle(
            0x803A2040u,
            [this](Cpu& cpu) {
                if (!signed_divide_64_intrinsic(cpu)) {
                    ++divide_intrinsic_declines_;
                    return false;
                }
                ++signed_divide_intrinsic_calls_;
                return true;
            },
            "signed 64-bit compiler divide intrinsic");
    }

    // The Metrowerks __dynamic_cast (0x803A1AFC): the same RTTI walk, natively and once per (vtable, target, source);
    // a reference cast that fails declines, and the guest body throws its bad_cast. --no-dynamic-cast-intrinsic: A/B.
    if (dynamic_cast_intrinsic_) {
        cpu_.install_hle(
            0x803A1AFCu, [this](Cpu& cpu) { return dynamic_cast_.serve(cpu); }, "Metrowerks __dynamic_cast");
    }

    // RenderWare's RwMatrixMultiply (0x8047FC08): the guest body's operations and memory accesses in its order, the
    // register file in host registers. --no-matrix-multiply-intrinsic: A/B.
    if (matrix_multiply_intrinsic_) {
        cpu_.install_hle(
            0x8047FC08u, [this](Cpu& cpu) { return matrix_multiply_.serve(cpu); }, "RenderWare RwMatrixMultiply");
    }
    // The character-collision broad phase's four bounding tests (gekko/chara_colli_intrinsic.h): a rejected test
    // served natively, every other declined to the guest body. --no-chara-colli-intrinsic: A/B.
    if (chara_colli_intrinsic_) {
        using Test = CharaColliIntrinsic::Test;
        chara_colli_.set_dynamic_cast(&dynamic_cast_);
        chara_colli_.set_bone_native(chara_colli_bone_native_);
        chara_colli_.set_bone_memo(chara_colli_bone_memo_);
        cpu_.install_hle(
            CharaColliIntrinsic::kBodyRay, [this](Cpu& cpu) { return chara_colli_.serve(cpu, Test::BodyRay); },
            "CharaColliBody ray test");
        cpu_.install_hle(
            CharaColliIntrinsic::kBodySweep, [this](Cpu& cpu) { return chara_colli_.serve(cpu, Test::BodySweep); },
            "CharaColliBody sweep test");
        cpu_.install_hle(
            CharaColliIntrinsic::kGeneralRay, [this](Cpu& cpu) { return chara_colli_.serve(cpu, Test::GeneralRay); },
            "CharaColliGeneral ray test");
        cpu_.install_hle(
            CharaColliIntrinsic::kGeneralSweep,
            [this](Cpu& cpu) { return chara_colli_.serve(cpu, Test::GeneralSweep); }, "CharaColliGeneral sweep test");
        if (chara_colli_loops_) {  // --no-chara-colli-loops: the tests only (A/B of the loops)
            using Query = CharaColliIntrinsic::Query;
            cpu_.install_hle(
                CharaColliIntrinsic::kRayQuery, [this](Cpu& cpu) { return chara_colli_.serve_query(cpu, Query::Ray); },
                "CharaColli ray query");
            cpu_.install_hle(
                CharaColliIntrinsic::kSweepQuery,
                [this](Cpu& cpu) { return chara_colli_.serve_query(cpu, Query::Sweep); }, "CharaColli sweep query");
            cpu_.install_hle(
                CharaColliIntrinsic::kEnemyQuery,
                [this](Cpu& cpu) { return chara_colli_.serve_query(cpu, Query::Enemy); }, "CharaColli enemy query");
        }
        if (chara_colli_manage_) {  // --no-chara-colli-manage: the manager pass's vf 0x7C in the guest (A/B)
            using Manage = CharaColliIntrinsic::Manage;
            cpu_.install_hle(
                0x800C3DD4u, [this](Cpu& cpu) { return chara_colli_.serve_manage(cpu, Manage::Bone); },
                "CharaColliGeneralBone vf 0x7C");
            cpu_.install_hle(
                0x800C1788u, [this](Cpu& cpu) { return chara_colli_.serve_manage(cpu, Manage::Body); },
                "CharaColliBody vf 0x7C");
            cpu_.install_hle(
                0x800C3594u, [this](Cpu& cpu) { return chara_colli_.serve_manage(cpu, Manage::General); },
                "CharaColliGeneral vf 0x7C");
        }
        chara_colli_.set_manage(chara_colli_manage_);
        if (trace_stale_contacts_) {  // --trace-stale-contacts: observation only
            cpu_.install_hle(0x8001FF00u, [this](Cpu& cpu) { return chara_colli_.stale_init(cpu); },
                             "contact result init (stale trace)");
            cpu_.install_hle(0x8002686Cu, [this](Cpu& cpu) { return chara_colli_.stale_copy(cpu); },
                             "contact object (stale trace)");
            const struct {
                u32 entry, size, last;
            } copies[] = {{0x800267F8u, 0x38u, 0x34u}, {0x800C2B8Cu, 0x38u, 0x34u}, {0x800091D0u, 12u, 8u},
                          {0x800091ECu, 12u, 8u}};
            // (The twelve-word copy 0x8040D198 is not hooked: its body is pruned - every direct call is the native
            // affine copy, docs/NATIVE_MATRIX_MATH.md - so a hook there would never run, and its literal would
            // root the body the matrix gate requires gone.)
            for (const auto& copy : copies)
                cpu_.install_hle(
                    copy.entry,
                    [this, copy](Cpu& cpu) { return chara_colli_.stale_copy_block(cpu, copy.size, copy.last); },
                    "copy (stale trace)");
            chara_colli_.trace_stale_contacts(cpu_);
        }
        if (chara_colli_pass_) {  // --chara-colli-pass (opt-in): the manager's pair pass natively
            cpu_.install_hle(
                CharaColliIntrinsic::kPass, [this](Cpu& cpu) { return chara_colli_.serve_pass(cpu); },
                "CharaColli manager pair pass");
        }
    }

    // mwPlyGetStat at 0x804086D8. Direct native calls skip the dispatcher, so
    // the generated entry thunk invokes Cpu::try_hle first. Returning false
    // leaves the original GetStat body to read the handle we just updated.
    cpu_.install_hle(
        0x804086D8u,
        [this](Cpu& cpu) {
            if (movie_trace_enabled_) {
                ++movie_counts_.get_stat;
                const u32 handle = cpu.gpr[3];
                const u32 status = guest_pointer(cpu, handle)
                                       ? cpu.mem_read32(handle + 4u)
                                       : 0xffffffffu;
                note_movie_event(cpu, 0x804086D8u, handle, status);
            }
            // Everything after this point exists only to drive the PLAYEND
            // stub, and this hook is installed on mwPlyGetStat - which the CRI
            // server polls. The gate stays above the lookup: this used to walk
            // every file the session had ever touched (up to 3,480 strings,
            // growing) on the guest thread, per poll, on a DEFAULT run where
            // sofdec_playend_stub_ is false. The asset system now records the
            // answer as reads happen, so there is no scan left to hoist.
            if (!sofdec_playend_stub_) {
                return false;
            }
            const std::string& latest_sfd = assets_.last_movie_path();
            if (latest_sfd.empty()) {
                return false;
            }
            const u32 frames = static_cast<u32>(video_->frames());
            if (latest_sfd != sofdec_current_sfd_) {
                sofdec_current_sfd_ = latest_sfd;
                sofdec_start_frame_ = frames;
            }
            // About one PAL second after the current movie file first appears.
            constexpr u32 kMinFrames = 50u;
            if (frames - sofdec_start_frame_ < kMinFrames) {
                return false;
            }
            const u32 handle = cpu.gpr[3];
            if (!guest_pointer(cpu, handle)) {
                return false;
            }
            const u32 magic = cpu.mem_read32(handle);
            if (magic != 1u) {
                return false;
            }
            const u32 status = cpu.mem_read32(handle + 4u);
            // 1 = PREP, 2 = PLAYING. Leave STOP/PLAYEND/ERROR alone.
            if (status != 1u && status != 2u) {
                return false;
            }
            // sofdec_playend_stub_ was tested at the top of this hook.
            // --no-sofdec-stub leaves the guest's own
            // status alone, which is what a real decoder path has to work
            // against: nothing else in the runtime writes this word.
            cpu.mem_write32(handle + 4u, 3u);  // MWS_PLY_STAT_PLAYEND
            const u32 inner = cpu.mem_read32(handle + 0x3cu);
            if (guest_pointer(cpu, inner)) {
                cpu.mem_write32(inner + 0x48u, 0xffffffffu);
            }
            if (sofdec_completed_sfd_ != latest_sfd) {
                sofdec_completed_sfd_ = latest_sfd;
                std::printf("[sofdec] PLAYEND %s after %u VI frames (handle 0x%08X was %u)\n",
                            latest_sfd.c_str(), frames - sofdec_start_frame_, handle, status);
                std::fflush(stdout);
            }
            return false;
        },
        "mwPlyGetStat");

    // RwCameraTestSphere at 0x80486718 - READ-ONLY OBSERVER.
    //
    // Widescreen in this port is host-side only: load_matrices widens the
    // projection AFTER the guest has already decided what to submit. The guest
    // culls every atomic against its OWN frustum: six planes RenderWare builds
    // from the camera's view window (its frame-sync callback at each camera
    // begin, 0x80485318 - docs/renderer-re-20261007/02_RW_CORE.md), the window
    // the camera sync derives from the game camera's FOV (63.25 x 53.13 deg by
    // default; the lifted build gives the camera the authored window -
    // docs/boundary-20261007/DESIGN.md 8). On a wider output the host shows
    // more, so content the guest rejected outside its own cone is simply absent
    // - and the host cannot see it, because the guest never submits it.
    //
    // This handler reconstructs the guest's frustum from the planes it is about
    // to use and asks: would this sphere have survived a frustum widened to the
    // host's actual aspect? The difference is the exact size of the defect.
    // Returns false: the recovered body runs untouched.
    cpu_.install_hle(
        0x80486718u,
        [this](Cpu& cpu) {
            if (visibility_probe_enabled_) observe_frustum_test(cpu);
            // The native implementation IS the frustum test when it is enabled
            // and the displayed cone differs from the original's. Returning
            // true means the recovered body never runs.
            if (native_visibility_) return native_frustum_test(cpu);
            return false;
        },
        "RwCameraTestSphere (native visibility)");

    // The planes that test - and RenderWare's world-sector collection - read:
    // the camera's frustum built from the displayed picture with the native
    // renderer (native_frustum_build; the legacy renderer keeps RenderWare's).
    cpu_.install_hle(
        0x80485318u, [this](Cpu& cpu) { return native_frustum_build(cpu); },
        "_rwCameraValUpdatePerspectiveFrustum (native visibility: the displayed picture)");

    // THE HEALTH BAR. This replaces the guest's draw callback outright.
    //
    // ENTRY POINTS ONLY: this runtime dispatches HLE hooks at declared function
    // entries and never mid-function, so an interior address is silently never
    // reached (measured: 18,731 calls to the gauge's gate and zero hits on nine
    // interior sites). 0x800BF634 is a declared entry spanning 0x800BF634..
    // 0x800BF790, and it is the only address the gauge's bound pointer-to-member
    // can reach, so every route - including the indirect call the functor
    // actually uses - passes through the hook exactly once.
    //
    // Returning true suppresses the ENTIRE guest body: the gate, the projection
    // into a 640x480 canvas, the string-keyed cast lookups, the CSD scene step
    // and its eight separate GX pipeline brackets. Nothing re-enters the
    // original and nothing filters its output.
    //
    // What used to be installed here was the "world-anchored scope end" latch,
    // one half of a pair that was already dead: its partner on 0x8036E794 was
    // registered first and then destroyed by a second registration at the same
    // address for the widescreen widget group, because install_hle replaces by
    // address. Both halves are gone; classification now travels on the draw
    // record itself, which is what makes a stale global latch impossible.
    cpu_.install_hle(
        0x800BF634u,
        [this](Cpu& cpu) {
            if (!native_health_bar_) return false;  // --legacy-health-bar
            return native_health_bar_draw(cpu);
        },
        "GiHPGaugeEnemy draw (native health bar)");

    // The Options menu's Display tile (gekko/native_options_display.h): the sixth slot shown and owned natively. The
    // setup and page entries only take notes and decline; the row draw and the three button queries are served for
    // item 5 alone, every other caller and item runs the game's own code.
    options_display_.set_sound(&System::play_menu_sound, this);
    cpu_.install_hle(
        options_display::kSetup,
        [this](Cpu& cpu) {
            // The front-end's language: the JP disc's own, else the console's (OSGetLanguage: the SRAM byte).
            static constexpr options_display::Language kByConsole[] = {
                options_display::Language::EN, options_display::Language::GE, options_display::Language::FR,
                options_display::Language::SP, options_display::Language::IT, options_display::Language::EN};
            const std::vector<u8>& identity = assets_.identity_block();
            const u8 console = ipl_ != nullptr ? ipl_->settings()[0x16] : 0u;
            options_display_.set_language(identity.size() > 3u && identity[3] == 'J'
                                              ? options_display::Language::JP
                                              : kByConsole[console < 6u ? console : 0u]);
            return options_display_.setup(cpu);
        },
        "Options setup (Display tile shown)");
    cpu_.install_hle(
        options_display::kPage, [this](Cpu& cpu) { return options_display_.page(cpu); },
        "Options page input (Display tile notes)");
    cpu_.install_hle(
        options_display::kDrawRows, [this](Cpu& cpu) { return options_display_.draw_rows(cpu); },
        "Options page rows (Display tile)");
    cpu_.install_hle(
        options_display::kRepeatPress, [this](Cpu& cpu) { return options_display_.repeat_press(cpu); },
        "Options repeat press (Display tile)");
    cpu_.install_hle(
        options_display::kEdgePress, [this](Cpu& cpu) { return options_display_.edge_press(cpu); },
        "Options edge press (Display tile)");

    // fn_801C28A8: `addi r3,r3,28; blr`. Called from fn_80271F9C at 0x80272094
    // with r3 = the camera-mode object; the caller then reads (fovX, fovY) as
    // degrees from +28/+32. Filter on LR so unrelated callers are ignored.
    cpu_.install_hle(
        0x801C28A8u,
        [this](Cpu& cpu) {
            if (fov_probe_enabled_ && cpu.lr == 0x80272094u) {
                const u32 object = cpu.gpr[3];
                if (guest_pointer(cpu, object)) {
                    const auto rf = [&cpu](u32 a) {
                        const u32 bits = cpu.mem_read32(a);
                        float v;
                        std::memcpy(&v, &bits, sizeof(v));
                        return static_cast<double>(v);
                    };
                    ++fov_probe_.pair_reads;
                    fov_probe_.mode_object = object;
                    fov_probe_.fov_x = rf(object + 28u);
                    fov_probe_.fov_y = rf(object + 32u);
                    if (fov_probe_.fov_x < fov_probe_.fov_x_min)
                        fov_probe_.fov_x_min = fov_probe_.fov_x;
                    if (fov_probe_.fov_x > fov_probe_.fov_x_max)
                        fov_probe_.fov_x_max = fov_probe_.fov_x;
                }
            }
            return false;
        },
        "camera FOV pair getter (fov probe)");

    // fn_802731A4(out, index): the scale pair. index == -1 and index == 0 take
    // different paths, and a non-zero index indexes a 48-byte table whose +40
    // entry is the horizontal scale. Recording which path runs says whether the
    // horizontal scale is even being looked up.
    cpu_.install_hle(
        0x802731A4u,
        [this](Cpu& cpu) {
            if (fov_probe_enabled_) {
                const int index = static_cast<int>(cpu.gpr[4]);
                ++fov_probe_.scale_calls;
                fov_probe_.scale_index = index;
                if (index == -1) ++fov_probe_.scale_index_negative;
                else if (index == 0) ++fov_probe_.scale_index_zero;
                else ++fov_probe_.scale_index_other;
            }
            return false;
        },
        "camera FOV scale pair (fov probe)");

    // Widescreen: the guest names the logical 2D group it is about to draw.
    //
    // Both handlers return false, so the recovered bodies run untouched and no
    // guest state is modified - they only publish which screen-side group the
    // primitives that follow belong to, so the host applies one anchor per
    // group instead of one per GX primitive.
    cpu_.install_hle(
        0x800BCBA4u,
        [this](Cpu& cpu) {
            {
                // Report the run's alignment reference X and which call site
                // drew it. The host decides the anchor: keeping the rule there
                // means the glyph path and the widget path cannot drift apart,
                // and it lets the host hold a run's anchor steady while the
                // run animates across a classification boundary.
                // A run outside the gameplay HUD strip is part of a centred
                // composition, so it is announced as scene space. Announcing it
                // at all still matters: it closes whatever group was latched
                // before it, which is what stops menu artwork drawn between
                // two runs from inheriting a HUD anchor.
                mark_hud_group(is_hud_strip_text_site(cpu.lr)
                                        ? HudGroupKind::Text
                                        : HudGroupKind::Ungrouped,
                                    cpu.lr,
                                    static_cast<float>(bits_to_double(cpu.fpr[1].bits[0])));
                // A glyph run is drawn by the HUD font renderer, not by a CSD
                // cast; leaving the cast channel latched attributed the Select
                // menu's glyph quads to cast 0x80b5fba8.
                mark_hud_scene(0u, 0u);
            }
            return false;
        },
        "HUD glyph run (widescreen group anchor)");

    // The fade curtain.
    //
    // Fade::Draw at 0x800448C8 is the single fade submission in the game: one
    // process-wide Fade object (0x80573E54, cached at r13+11468) drives every
    // stage and menu transition, drawn by FadeDrawTask through `bl 0x800448C8`
    // at 0x80044E24. It emits the curtain as TEN separate four-vertex tri-strips
    // of 64x480 canvas units, tiling 0..640 x 0..480, plus - while the loading
    // graphic is up - one textured 109x135 quad at (456,274)-(565,409).
    //
    // Ten 64-wide columns is why the curtain was being anchored: no single
    // primitive is anywhere near full width, so each column took the scene-space
    // centre anchor and the curtain covered only canvas 80..560 at 16:9,
    // measured, leaving an eighth of the frame unfaded at each edge.
    //
    // Announcing the call lets the host recognise the tiling: inside a
    // ScreenCover group a primitive that spans the canvas height is one of the
    // curtain's columns and keeps its X, while the loading graphic - which does
    // not - is laid out normally and keeps its shape.
    cpu_.install_hle(
        0x800448C8u,
        [this](Cpu&) {
            {
                mark_hud_group(HudGroupKind::ScreenCover, 0x800448C8u);
                // The fade is RenderWare Im2D, not Chao::CSD, so no cast draws
                // it. Without this the curtain's ten columns inherit whichever
                // cast drew last - measured: fade columns were being reported
                // under menu cast ids 0x80b507ac / 0x8099bc0c / 0x80f506d8.
                mark_hud_scene(0u, 0u);
            }
            return false;
        },
        "Fade::Draw (widescreen screen-cover group)");

    // Chao::CSD::Project::Render draws one .gncp - one logical widget - so the
    // glyph run's anchor must not leak into it. Its own sprites carry their
    // group in their geometry and are classified from it.
    cpu_.install_hle(
        0x8036E794u,
        [this](Cpu& cpu) {
            {
                // Project::Render has two early exits that jump straight to its
                // epilogue at 0x8036E850 WITHOUT running the refcount release
                // at 0x8036E84C - the project's own visible flag at +8, and a
                // null render context at +44 (0x8036E7A8-0x8036E7D0). The
                // release is what the group-end hook is attached to, so a
                // hidden widget used to open a group that nothing ever closed,
                // and everything drawn after it - world-projected marks, effect
                // billboards, another task's sprites - was folded into that
                // empty widget and took its remembered anchor. Deciding the
                // same predicate here means a widget that will not draw never
                // opens a group in the first place.
                const u32 project = cpu.gpr[3];
                const u32 visible = cpu.mem_read32(project + 8u);
                const u32 context = cpu.mem_read32(project + 44u);
                const bool will_draw =
                    visible != 0u && context != 0u && cpu.mem_read32(context) != 0u;
                if (!will_draw) {
                    mark_hud_group(HudGroupKind::Ungrouped, cpu.lr);
                    mark_hud_scene(0u, 0u);
                    return false;
                }
                // The return address names the recovered task that drew this
                // widget. Widgets of the gameplay HUD strip are anchored to a
                // screen edge; everything else keeps the scene-space centre.
                // r3 is the Chao::CSD project - the guest's own identity for
                // this widget instance. It is what tells the Hero gauge from
                // the Dark gauge, and the stage title card from the Hissatu cap
                // icons, all of which share a draw site.
                const HudGroupKind kind =
                    is_per_cast_anchor_site(cpu.lr)  ? HudGroupKind::CsdWidgetPerCast
                    : is_hud_strip_draw_site(cpu.lr) ? HudGroupKind::CsdWidget
                                                     : HudGroupKind::Ungrouped;
                mark_hud_group(kind, cpu.lr, 0.0f, cpu.gpr[3]);
            }
            return false;
        },
        "Chao::CSD::Project::Render (widescreen widget group)");

    // Chao::CSD per-cast draw dispatch - OBSERVATION ONLY.
    //
    // fn_8035F840 is the draw pass's per-node dispatcher: fn_8035F8BC walks the
    // scene's draw list and calls it once per node, in draw order, with r4 =
    // the authored item and r7 = the 324-byte runtime node record; it then
    // dispatches on the authored type at item+4 (1 = sprite -> fn_8035F640,
    // 3 = text -> fn_8035F3AC), each of which submits that node's geometry.
    // Marking here therefore names the cast that drew every primitive.
    //
    // The transform pass (Project::Render -> vtable +0x24 fn_80367800 ->
    // fn_8035EFA4 -> the recursive walker fn_8035ECA8) is a SEPARATE pass that
    // runs before any geometry is submitted, so a mark taken there would put
    // every announcement ahead of every primitive and describe nothing.
    //
    // Nothing in the layout reads this channel yet; it exists so the achievable
    // decomposition can be measured before any policy is keyed to it.
    cpu_.install_hle(
        0x8035F840u,
        [this](Cpu& cpu) {
            {
                mark_hud_scene(cpu.gpr[7], cpu.gpr[4]);
            }
            return false;
        },
        "Chao::CSD cast draw (2D decomposition observation)");

    // Effect particle draw, 0x80434824. DIAGNOSTIC ONLY - inert unless
    // SHADOW_EFFECT_PROBE is set in the environment.
    //
    // This function walks a particle list (head at this+0xFC, next at +0x68)
    // and, per particle, wraps a rotation at +0x18 into [-pi, pi] with a LOOP
    // rather than a remainder:
    //
    //     while (a > pi || a < -pi) a = (a >= 0.0f) ? a - 2*pi : a + 2*pi;
    //
    // The three constants are DOL data and were read directly: pi at
    // 0x805FBDB0 = 3.14159274, the compare threshold at 0x8051C790 = 0.0, and
    // the multiplier at 0x8051C798 = 2.0.
    //
    // That loop is only bounded while the rotation stays small. In float32 the
    // ULP passes 2*pi at |a| >= 2^27, so a - 2*pi ROUNDS BACK TO a and the loop
    // can never terminate; well below that it still costs |a|/(2*pi) iterations
    // per particle per frame. The guest then never returns from the draw, which
    // is why GX submission stops dead while instructions keep retiring.
    //
    // The probe reports the largest-magnitude rotation in the list so the value
    // can be caught while it is still growing, rather than after it wedges.
    cpu_.install_hle(
        0x80434824u,
        [this](Cpu& cpu) {
            static const bool enabled = std::getenv("SHADOW_EFFECT_PROBE") != nullptr;
            if (!enabled) return false;
            static u64 entries = 0;
            static u64 drawn = 0;
            ++entries;
            const u32 self = cpu.gpr[3];
            const u32 flags = cpu.mem_read32(self + 0x18u);
            if ((flags & 1u) == 0u || (flags & 8u) != 0u) return false;
            if (cpu.mem_read32(self + 0x110u) == 0u) return false;
            ++drawn;
            static float reported = 4.0f;   // above pi, so a wrapped list is silent
            u32 node = cpu.mem_read32(self + 0xfcu);
            u32 walked = 0;
            float worst = 0.0f;
            u32 worst_node = 0;
            while (node != 0u && walked < 8192u) {
                const u32 bits = cpu.mem_read32(node + 0x18u);
                float angle = 0.0f;
                std::memcpy(&angle, &bits, sizeof(angle));
                if (std::fabs(angle) > std::fabs(worst)) {
                    worst = angle;
                    worst_node = node;
                }
                node = cpu.mem_read32(node + 0x68u);
                ++walked;
            }
            static const u64 verbose_from = [] {
                const char* from = std::getenv("SHADOW_EFFECT_PROBE_FROM");
                return from != nullptr ? std::strtoull(from, nullptr, 0)
                                       : std::numeric_limits<u64>::max();
            }();
            static u32 longest = 0;
            if (cpu.game_clock().now() >= verbose_from) {
                std::fprintf(stderr,
                             "[effect-trace] ticks=%llu entries=%llu emitter=0x%08X "
                             "list_len=%u head=0x%08X msr=0x%08X FP=%d vis=0x%08X stream=0x%08X "
                             "worst_rot=%.9g (0x%08X) at 0x%08X iters=%.4g\nchain=",
                             static_cast<unsigned long long>(cpu.game_clock().now()),
                             static_cast<unsigned long long>(entries), self, walked,
                             cpu.mem_read32(self + 0xfcu), cpu.msr(),
                             (cpu.msr() & 0x2000u) != 0u ? 1 : 0,
                             cpu.mem_read32(self + 0x110u), cpu.mem_read32(self + 0x13cu),
                             static_cast<double>(worst),
                             [&] { u32 b = 0; std::memcpy(&b, &worst, sizeof(b)); return b; }(),
                             worst_node, static_cast<double>(std::fabs(worst) / 6.2831853));
                u32 n = cpu.mem_read32(self + 0xfcu);
                for (u32 i = 0; i < 4u && n != 0u; ++i) {
                    std::fprintf(stderr, "0x%08X->", n);
                    n = cpu.mem_read32(n + 0x68u);
                }
                std::fprintf(stderr, "0x%08X%s\n", n,
                             walked >= 8192u ? "  *** WALK CAPPED ***" : "");
                // The five attribute streams the draw is about to lock, resolved
                // exactly as FUN_80449458 would: stream = *(object + *(r13+0x5FF0)),
                // then a (base, stride) pair per mask, with an override at +0x9C.
                // The wrap loop's three constants, live. If the pi at 0x805FBDB0
                // is ever 0 the loop is provably non-terminating: the condition
                // 0 < a stays true while the body computes a = -(2*0 - a) = a.
                {
                    const u32 pi_bits = cpu.mem_read32(0x805fbdb0u);
                    const u32 thr_bits = cpu.mem_read32(0x8051c790u);
                    const u32 mul_bits = cpu.mem_read32(0x8051c798u);
                    float pi = 0.0f, thr = 0.0f, mul = 0.0f;
                    std::memcpy(&pi, &pi_bits, sizeof(pi));
                    std::memcpy(&thr, &thr_bits, sizeof(thr));
                    std::memcpy(&mul, &mul_bits, sizeof(mul));
                    std::fprintf(stderr,
                                 "               wrap: pi=%.9g (0x%08X) thr=%.9g mul=%.9g%s\n",
                                 static_cast<double>(pi), pi_bits, static_cast<double>(thr),
                                 static_cast<double>(mul),
                                 (pi_bits != 0x40490fdbu) ? "   *** PI CORRUPTED ***" : "");
                }
                const u32 object = cpu.mem_read32(self + 0x13cu);
                const u32 sda_offset = cpu.mem_read32(cpu.gpr[13] + 0x5ff0u);
                const u32 stream = object != 0u ? cpu.mem_read32(object + sda_offset) : 0u;
                if (stream != 0u) {
                    const u32 present = cpu.mem_read32(stream + 0xa0u);
                    const u32 override_stride = cpu.mem_read32(stream + 0x9cu);
                    struct { u32 mask; u32 base_off; u32 stride_off; const char* what; } kAttrs[] = {
                        {0x01u, 0x4cu, 0x50u, "pos"},   {0x02u, 0x6cu, 0x70u, "col"},
                        {0x04u, 0x64u, 0x68u, "uv"},    {0x20u, 0x7cu, 0x80u, "rot"},
                        {0x80u, 0x84u, 0x88u, "xtra"},
                    };
                    std::fprintf(stderr, "               stream=0x%08X present=0x%08X ovr=%u",
                                 stream, present, override_stride);
                    for (const auto& a : kAttrs) {
                        const bool ok = (present & a.mask) != 0u;
                        const u32 base = ok ? cpu.mem_read32(stream + a.base_off) : 0u;
                        u32 stride = ok ? cpu.mem_read32(stream + a.stride_off) : 0u;
                        if (override_stride != 0u) stride = override_stride;
                        std::fprintf(stderr, "  %s=%s0x%08X/%u", a.what, ok ? "" : "FAIL:", base,
                                     stride);
                    }
                    std::fprintf(stderr, "\n");
                }
                std::fflush(stderr);
            }
            if (walked > longest || std::fabs(worst) > reported) {
                if (std::fabs(worst) > reported) reported = std::fabs(worst) * 2.0f;
                if (walked > longest) longest = walked;
                u32 bits = 0;
                std::memcpy(&bits, &worst, sizeof(bits));
                std::fprintf(stderr,
                             "[effect-probe] ticks=%llu entries=%llu emitter=0x%08X "
                             "list_len=%u head=0x%08X worst_rot=%.6g (0x%08X) node=0x%08X%s\n",
                             static_cast<unsigned long long>(cpu.game_clock().now()),
                             static_cast<unsigned long long>(entries), self, walked,
                             cpu.mem_read32(self + 0xfcu), static_cast<double>(worst), bits,
                             worst_node, walked >= 8192u ? "  *** WALK CAPPED ***" : "");
                std::fflush(stderr);
            }
            return false;
        },
        "Effect particle draw (rotation-range probe)");

    // Vertex-stream attribute lock, 0x80449458. DIAGNOSTIC ONLY.
    //
    //   lock(object r3, out int[2] r4, mask r5, flags r6)
    //     stream = *(object + *(r13 + 0x5FF0))
    //     if ((stream[0xA0] & mask) == 0) { out[0] = 0; out[1] = 0; return 0; }
    //     out[0] = base, out[1] = stride, from a per-mask pair of fields
    //
    // The effect particle draw at 0x80434824 calls this five times (masks 1, 2,
    // 4, 0x20, 0x80) and IGNORES THE RETURN VALUE every time, then stores
    // through out[0] and advances by out[1] once per particle. A failed lock
    // therefore leaves it storing through a null base with a zero stride.
    // The outcome is fully determined by state readable at entry, so this
    // reports it without perturbing anything.
    cpu_.install_hle(
        0x80449458u,
        [this](Cpu& cpu) {
            static const bool enabled = std::getenv("SHADOW_EFFECT_PROBE") != nullptr;
            if (!enabled) return false;
            const u32 object = cpu.gpr[3];
            const u32 mask = cpu.gpr[5];
            const u32 sda_offset = cpu.mem_read32(cpu.gpr[13] + 0x5ff0u);
            const u32 stream = cpu.mem_read32(object + sda_offset);
            if (stream == 0u) {
                std::fprintf(stderr,
                             "[stream-lock] ticks=%llu object=0x%08X mask=0x%X "
                             "STREAM NULL (sda_off=0x%X)\n",
                             static_cast<unsigned long long>(cpu.game_clock().now()), object,
                             mask, sda_offset);
                std::fflush(stderr);
                return false;
            }
            const u32 present = cpu.mem_read32(stream + 0xa0u);
            if ((present & mask) == 0u) {
                static u64 failures = 0;
                ++failures;
                if (failures <= 40u || (failures % 5000u) == 0u) {
                    std::fprintf(stderr,
                                 "[stream-lock] ticks=%llu FAIL #%llu object=0x%08X "
                                 "stream=0x%08X mask=0x%X present=0x%08X lr=0x%08X\n",
                                 static_cast<unsigned long long>(cpu.game_clock().now()),
                                 static_cast<unsigned long long>(failures), object, stream,
                                 mask, present, cpu.lr);
                    std::fflush(stderr);
                }
            }
            return false;
        },
        "RW vertex-stream attribute lock (failure probe)");

    // The refcount release Chao::CSD::Project::Render calls last (0x8036E84C)
    // closes the widget, so anything drawn afterwards is scene-space again
    // rather than part of the widget that happened to be drawn most recently.
    //
    // fn_8035f934 pairs the same two helpers around a smaller section *inside*
    // a widget, so the release has to be attributed to its caller: without the
    // return-address test the widget ends early and its remaining sprites -
    // the ring icon next to its counter, the parked weapon panels - are left
    // ungrouped and drift away from the rest of their group.
    cpu_.install_hle(
        0x8035FD08u,
        [this](Cpu& cpu) {
            constexpr u32 kProjectRenderReturn = 0x8036E850u;
            if (cpu.lr == kProjectRenderReturn) {
                mark_hud_group(HudGroupKind::Ungrouped);
                // ... and so does the cast that drew last inside it. The cast
                // channel has no exit hook of its own (fn_8035F840 is entered
                // per node and never returns to a hookable address), so this is
                // where a project's casts stop being current.
                mark_hud_scene(0u, 0u);
            }
            return false;
        },
        "Chao::CSD render release (widescreen widget group end)");

    // The native video first: the probes below may wrap its owners.
    video_->install();
    install_movie_trace_hooks();
    install_ui_probe_hooks();
    install_boot_stage_hooks();

    // The native platform: the OS's interrupt bookkeeping and delivery, the
    // video library (installed above), the audio libraries, the controller
    // libraries, the frame limiter, the GX draw sync / breakpoint / FIFO API,
    // the memory card, the OS's IPL-chip clients and the reset controls. Their
    // owners serve the guest entry points extract/native-owners.txt routes to
    // them.
    interrupts_->install();
    audio_->install();
    // The game's room reverb: the mixer's native effect, run by the game's own aux callback dispatch
    // (gekko/native_reverb_owner.h).
    install_reverb_owner();
    // The sound-effect table lookup (gekko/native_sound_tables.h): the original lookup and its helper are not generated.
    cpu_.install_hle(sound_tables::kLookup, [this](Cpu& cpu) {
        sound_tables::serve_lookup(cpu, sound_table_report_);
        return true;  // an owner never declines
    }, "sound-effect table lookup (native)");
    // GCAX's MIDI ring push (gekko/native_gcax_midi.h) and the SE manager (gekko/native_se_manager.h): the
    // manager's internals are not generated.
    cpu_.install_hle(gcax_midi::kPortPush, [this](Cpu& cpu) {
        gcax_midi::serve_push(cpu, midi_report_);
        return true;
    }, "GCAX MIDI port push (native)");
    cpu_.install_hle(se_manager::kPlaySimple, [this](Cpu& cpu) {
        se_manager::serve_play_simple(cpu, se_services_);
        return true;
    }, "SE PlaySimple (native)");
    cpu_.install_hle(se_manager::kPlayFlags, [this](Cpu& cpu) {
        se_manager::serve_play_flags(cpu, se_services_);
        return true;
    }, "SE PlayFlags (native)");
    cpu_.install_hle(se_manager::kPlay3D, [this](Cpu& cpu) {
        se_manager::serve_play_3d(cpu, se_services_);
        return true;
    }, "SE Play3D (native)");
    cpu_.install_hle(se_manager::kPlayRing, [this](Cpu& cpu) {
        se_manager::serve_play_ring(cpu, se_services_);
        return true;
    }, "SE PlayRing (native)");
    for (const u32 gate : {se_manager::kGateClearIfZero, se_manager::kGateSetIfZero, se_manager::kGateClear,
                           se_manager::kGateSet})
        cpu_.install_hle(gate, [gate](Cpu& cpu) {
            se_manager::serve_gate(cpu, gate);
            return true;
        }, "SE request gate (native)");
    cpu_.install_hle(se_manager::kFrame, [this](Cpu& cpu) {
        se_manager::serve_frame(cpu, se_services_);
        return true;
    }, "SE FrameSE (native)");
    frame_limiter_->install();
    object_range_->install();
    motion_store_->install();
    sound_banks_->install();
    stream_ring_->install();
    attached_effects_->install();
    gx_sync_->install();
    card_->install();
    ipl_->install();
    reset_controls_->install();
    disc_error_->install();
    link_groups_->install();
    directory_scan_->install();
    game_memory_->install();
    // Every listed platform site goes to the subsystem whose table lists it.
    cpu_.set_platform_sites(
        [this](Cpu& cpu, u32 site, u32 effective) { return platform_read16(cpu, site, effective); },
        [this](Cpu& cpu, u32 site, u32 effective, u16 value) { platform_write16(cpu, site, effective, value); });
    cpu_.set_platform_values([this](Cpu& cpu, u32 site) { return platform_value(cpu, site); });
    // The host service runs every kHostServiceTicks of game time from now on, and the pacing at every kPacingTicks.
    cpu_.game_clock().schedule(GameEvent::Frame, next_frame_event(cpu_.game_clock().now()));

#if !defined(SHADOW_RENDER_LIFTED)  // G5
    // Last: the native capture observes in front of every handler above.
    if (native_capture_) {
        native_capture_->install();
    }
#endif
}

// The only console constants left in game code are the video field's (NativeVideo).
u32 System::platform_value(Cpu& cpu, u32 site) { return video_->platform_value(cpu, site); }

u16 System::platform_read16(Cpu& cpu, u32 site, u32 effective) {
    if (NativeGxSync::is_platform_site(site)) return gx_sync_->platform_read16(cpu, site, effective);
    return video_->platform_read16(cpu, site, effective);
}

void System::platform_write16(Cpu& cpu, u32 site, u32 effective, u16 value) {
    // The only written platform sites left are GX's performance selects (the
    // reset path's DCR writes went with stage 6; dead sites emit nothing).
    if (NativeGxSync::is_platform_site(site)) {
        gx_sync_->platform_write16(cpu, site, effective, value);
        return;
    }
    char detail[160];
    std::snprintf(detail, sizeof(detail), "unlisted platform site 0x%08X wrote 0x%04X to 0x%08X", site,
                  static_cast<unsigned>(value), effective);
    cpu.request_stop(StopReason::ExecutionFault, detail);
}

#if !defined(SHADOW_RENDER_LIFTED)  // G5
RwCapture& System::enable_native_capture() {
    if (!native_capture_) {
        native_capture_ = std::make_unique<RwCapture>(*this);
    }
    return *native_capture_;
}
#else
// G5: NativeImmediate::save_state's block while idle: phase Idle, opcode 0, count 0, VCD low / high 0, VAT A / B / C
// 0, an empty payload block (its u64 length 0).
void System::save_idle_immediate_state(std::vector<u8>& out) {
    state::put<u8>(out, 0u);
    state::put<u8>(out, 0u);
    for (int i = 0; i < 6; ++i) state::put<u32>(out, 0u);
    state::put_block(out, nullptr, 0u);
}

bool System::load_idle_immediate_state(const u8*& cursor, const u8* end) {
    u8 phase = 0, opcode = 0;
    u32 words[6] = {};
    u64 bytes = 0;
    if (!state::get<u8>(cursor, end, phase) || !state::get<u8>(cursor, end, opcode)) return false;
    for (u32& word : words)
        if (!state::get<u32>(cursor, end, word)) return false;
    if (!state::get<u64>(cursor, end, bytes)) return false;
    // Only an idle builder: an open primitive cannot continue in a build without the immediate path.
    return phase == 0u && bytes == 0u;
}
#endif

void System::add_hle_observer(u32 address, std::function<void(Cpu&)> observer, std::string name) {
    Cpu::HleHandler previous;
    std::string previous_name;
    for (const auto& entry : cpu_.hle_entries()) {
        if (entry.address == address) {
            previous = entry.handler;
            previous_name = entry.name;
        }
    }
    if (previous) {
        cpu_.install_hle(
            address,
            [observer = std::move(observer), previous](Cpu& cpu) {
                observer(cpu);
                return previous(cpu);
            },
            name + " + " + previous_name);
    } else {
        cpu_.install_hle(
            address,
            [observer = std::move(observer)](Cpu& cpu) {
                observer(cpu);
                return false;
            },
            std::move(name));
    }
}

void System::install_chained_hle(u32 address, Cpu::HleHandler handler, std::string name) {
    Cpu::HleHandler previous;
    for (const auto& entry : cpu_.hle_entries()) {
        if (entry.address == address) previous = entry.handler;
    }
    if (previous) {
        std::fprintf(stderr, "[boot-stage] chaining existing hook '%s' at 0x%08X\n",
                     cpu_.hle_name(address) != nullptr ? cpu_.hle_name(address)->c_str() : "?",
                     address);
        cpu_.install_hle(
            address,
            [previous, handler](Cpu& cpu) { return previous(cpu) || handler(cpu); },
            std::move(name));
    } else {
        cpu_.install_hle(address, std::move(handler), std::move(name));
    }
}

// --boot-stage: button-free developer navigation into any Select Mode stage.
//
// Everything below was traced from the DOL and proven end to end by applying
// the same writes to cold-boot savestates (docs/tools/BOOT_STAGE.md):
//
//   GameSystem manager 0x805F0A6C  +0 next system (5 = StageSequence::Manager)
//   StageSequence kind  0x805ED220  (0 = PlayOnce)
//   PlayOnce stage id   0x80585788  plain decimal (604 = Final Haunt), read
//                                   only by PlayOnce's builder 0x802D11CC
//   play flags          0x8057E8A8..AA = 1 (the confirm's memset)
//   AdvReal controller  0x80584B34  RequestPhase 0x802B7934(ctrl, id)
//
// Select Mode's confirm writes the first four; its exit requests AdvReal phase
// 0, which builds nothing, so Advertise ends and the GameSystem factory builds
// the Manager. At cold boot AdvFolder's constructor asks for phase 9
// (MemoryCheck) from 0x802B6718; the hook makes that the same hand-off. The
// memory card is therefore never read (autosave stays off) and the PAL 50/60 Hz
// screen, which runs earlier, is ended through its own code keeping the SRAM
// preselection, so SRAM is never rewritten.
void System::install_boot_stage_hooks() {
    if (!boot_stage_.requested) return;
    constexpr u32 kRequestPhase = 0x802B7934u;
    constexpr u32 kAdvFolderCallLr = 0x802B6718u;
    constexpr u32 kAdvRealController = 0x80584B34u;
    constexpr u32 kSequenceKindCurrent = 0x805ED224u;
    constexpr u32 kGameSystemHolder = 0x805F03E0u;
    constexpr u32 kGameSystemManager = 0x805F0A6Cu;
    constexpr u32 kCurrentSystem = 0x805F0A50u;
    constexpr u32 kAdvertiseVtable = 0x8053DE14u;
    constexpr u32 kSequenceKindRequested = 0x805ED220u;
    constexpr u32 kPlayOnceStageId = 0x80585788u;
    // PlayEvent::Phase's constructor 0x802D0DF0 reads the event id 8 bytes on;
    // PlayStory::Phase's constructor 0x802D2618 its start mode 4 bytes on.
    constexpr u32 kPlayEventId = 0x80585790u;
    constexpr u32 kPlayStoryMode = 0x8058578Cu;
    constexpr u32 kStageTable = 0x804C5AE8u;
    constexpr u32 kStageRecords = 59u;
    constexpr u32 kStageRecordSize = 0x50u;

    install_chained_hle(
        kRequestPhase,
        [this](Cpu& cpu) {
            if (boot_stage_.fired || cpu.lr != kAdvFolderCallLr ||
                cpu.gpr[3] != kAdvRealController || cpu.gpr[4] != 9u) {
                return false;
            }
            const auto fail = [&](const char* why) {
                boot_stage_.fired = true;
                cpu.request_stop(StopReason::HostRequest,
                                 std::string("boot-stage: cold-boot preconditions not met (") +
                                     why + "); nothing was written");
                return false;
            };
            if (cpu.read_guest_u32(kSequenceKindCurrent) != 0xFFFFFFFFu) {
                return fail("stage sequence already used");
            }
            if (cpu.read_guest_u32(kGameSystemHolder) != kGameSystemManager ||
                cpu.read_guest_u32(kGameSystemManager) != 0u) {
                return fail("GameSystem manager state");
            }
            const u32 current = cpu.read_guest_u32(kCurrentSystem);
            if (current < 0x80000000u || cpu.read_guest_u32(current) != kAdvertiseVtable) {
                return fail("current system is not Advertise");
            }
            if (cpu.read_guest_u32(kAdvRealController + 4u) != 0u ||
                cpu.read_guest_u32(kAdvRealController + 8u) != 0u) {
                return fail("AdvReal controller not at its constructed state");
            }
            if (boot_stage_.kind == 0u) {
                bool known = false;
                for (u32 record = 0; record < kStageRecords; ++record) {
                    if (cpu.read_guest_u32(kStageTable + record * kStageRecordSize) ==
                        boot_stage_.id) {
                        known = true;
                    }
                }
                if (!known) return fail("stage id not in the guest stage table");
            }

            cpu.mem_write32(kGameSystemManager, 5u);
            cpu.mem_write32(kSequenceKindRequested, boot_stage_.kind);
            if (boot_stage_.kind == 1u) {
                cpu.mem_write32(kPlayStoryMode, 0u);  // a new game
            } else if (boot_stage_.kind == 7u) {
                // PlayFlashback takes no id: its phase reads its own state.
            } else {
                cpu.mem_write32(boot_stage_.kind == 2u ? kPlayEventId : kPlayOnceStageId, boot_stage_.id);
            }
            cpu.mem_write8(0x8057E8A8u, 1u);
            cpu.mem_write8(0x8057E8A9u, 1u);
            cpu.mem_write8(0x8057E8AAu, 1u);
            cpu.gpr[4] = 0u;  // phase 0: builds nothing, Advertise ends
            boot_stage_.fired = true;
            boot_stage_.fired_at_ticks = cpu.game_clock().now();
            std::fprintf(stderr,
                         boot_stage_.kind == 2u
                             ? "[boot-stage] event%04u handed to StageSequence::PlayEvent at %.3f s "
                               "of game time\n"
                         : boot_stage_.kind == 1u
                             ? "[boot-stage] story %u (new game) handed to StageSequence::PlayStory at "
                               "%.3f s of game time\n"
                         : boot_stage_.kind == 7u
                             ? "[boot-stage] flashback %u handed to StageSequence::PlayFlashback at "
                               "%.3f s of game time\n"
                             : "[boot-stage] stg%04u handed to StageSequence::PlayOnce at %.3f s "
                               "of game time\n",
                         boot_stage_.id,
                         static_cast<double>(cpu.game_clock().now()) /
                             static_cast<double>(kGameTicksPerSecond));
            return false;
        },
        "boot-stage: Select Mode hand-off at AdvFolder's first phase request");

    // PAL 50/60 Hz screen (0x80306638 exec, called from 0x80307000; object
    // fields +0x10 cursor, +0x14 mode, +0x18 fade state, +0x1C timer). A
    // preselected 60 Hz is accepted through the screen's own test-timeout
    // branch with the mode argument swapped to keep 60 Hz (0x80306888 from
    // 0x80306840), then faded out; the dtor then commits the unchanged choice.
    // A 50 Hz preselect stops the run instead of guessing.
    install_chained_hle(
        0x80306638u,
        [this](Cpu& cpu) {
            if (boot_stage_.fired || boot_stage_hz_step_ >= 3 || cpu.lr != 0x80307000u) {
                return false;
            }
            const u32 self = cpu.gpr[3];
            if (!guest_pointer(cpu, self, 0x20u) ||
                cpu.read_guest_u32(self + 0x18u) != 1u || cpu.read_guest_u32(self + 0x14u) != 0u) {
                return false;
            }
            if (boot_stage_hz_step_ == 0) {
                const u32 cursor = cpu.read_guest_u32(self + 0x10u);
                if (cursor == 0u) {
                    cpu.mem_write32(self + 0x14u, 1u);
                    cpu.mem_write32(self + 0x1Cu, 180u);
                    boot_stage_hz_step_ = 1;
                } else {
                    boot_stage_hz_step_ = 3;
                    cpu.request_stop(StopReason::HostRequest,
                                     "boot-stage: the PAL screen preselects 50 Hz; boot with "
                                     "--tv-format ntsc (or choose 60 Hz once in a normal run)");
                }
            } else if (boot_stage_hz_step_ == 2) {
                cpu.mem_write32(self + 0x18u, 2u);
                cpu.mem_write32(self + 0x1Cu, 25u);
                boot_stage_hz_step_ = 3;
                boot_stage_.hz_choice = 60u;
                std::fprintf(stderr, "[boot-stage] PAL 50/60 Hz screen: kept the 60 Hz preselect\n");
            }
            return false;
        },
        "boot-stage: PAL 50/60 Hz screen exec");
    install_chained_hle(
        0x80306888u,
        [this](Cpu& cpu) {
            if (boot_stage_hz_step_ == 1 && cpu.lr == 0x80306840u) {
                cpu.gpr[4] = cpu.read_guest_u32(cpu.gpr[3] + 8u);
                boot_stage_hz_step_ = 2;
            }
            return false;
        },
        "boot-stage: PAL 60 Hz confirm");
    // A loud stop instead of a silent hang: the per-frame GameSystem driver
    // runs from the start, so a hand-off that has not happened within two
    // minutes of game time (the PAL screen and WaitInitialize take seconds)
    // never will.
    install_chained_hle(
        0x801E2210u,
        [this](Cpu& cpu) {
            if (!boot_stage_.fired && cpu.game_clock().now() > 120u * kGameTicksPerSecond) {
                boot_stage_.fired = true;
                cpu.request_stop(StopReason::HostRequest,
                                 "boot-stage: the stage hand-off point was never reached");
            }
            return false;
        },
        "boot-stage: hand-off deadline");
}

// The PAL 50/60 Hz selection screen, observed at its own call boundaries.
//
// Chain, all recovered statically: __start -> RsRwInitialize -> fn_8030717c ->
// fn_80306b48 -> sprintf("palSel/PalSel%s.txd", suffix) -> RwFopen ->
// RwTexDictionaryStreamRead. The single gate is VIGetTvFormat() == VI_PAL(1)
// at 0x800517d4, and VIGetTvFormat (fn_80380138) reads HorVer.tv at
// 0x805f2114, which VIInit fills from the IPL word at 0x800000cc - a word this
// host writes as 1 unconditionally (apply_boot_configuration), so the gate
// should pass. It does not appear, so the probe answers which of the three
// remaining possibilities is true: the gate is false, the loader never runs,
// or the loader runs and RwFopen fails silently (fn_80306b48 returns 0 with no
// diagnostic, and the asset system only names a file after a successful read,
// so the file never appearing in a trace proves nothing).
void System::install_ui_probe_hooks() {
    if (!ui_probe_enabled_) {
        return;
    }
    cpu_.install_hle(
        0x80380138u,
        [this](Cpu& cpu) {
            // c = the IPL video-mode word this host writes as 1 in
            // apply_boot_configuration; d = whether the native display runs.
            // If HorVer.tv is 0 while 0x800000cc is 1, VIInit did not take
            // the IPL word.
            note_movie_event(cpu, 0x80380138u, cpu.mem_read32(0x805F2114u), cpu.lr,
                             cpu.mem_read32(0x800000CCu), video_->enabled() ? 1u : 0u);
            return false;
        },
        "VIGetTvFormat (ui probe)");
    // VIInit derives HorVer.tv from the display it finds (NativeVideo::vi_init:
    // the running PAL display, or the NTSC cold start) and only promotes 0 to 5
    // (VI_EURGB60) when the IPL word at 0x800000cc is 1. VIInit is a native
    // owner, so the probe runs in front of it and the owner serves the call.
    {
        Cpu::HleHandler owner;
        for (const auto& entry : cpu_.hle_entries()) {
            if (entry.address == 0x8037E824u) owner = entry.handler;
        }
        cpu_.install_hle(
            0x8037E824u,
            [this, owner](Cpu& cpu) {
                note_movie_event(cpu, 0x8037E824u, video_->enabled() ? 1u : 0u,
                                 video_->half_lines_per_field(), cpu.mem_read32(0x800000CCu), cpu.lr);
                return owner ? owner(cpu) : false;
            },
            "VIInit (ui probe, then the native owner)");
    }
    cpu_.install_hle(
        0x8030717Cu,
        [this](Cpu& cpu) {
            note_movie_event(cpu, 0x8030717Cu, cpu.gpr[3], cpu.lr);
            return false;
        },
        "PAL-select gate fn_8030717c (ui probe)");
    cpu_.install_hle(
        0x80306B48u,
        [this](Cpu& cpu) {
            const u32 object = cpu.gpr[3];
            const u32 language = guest_pointer(cpu, object)
                                     ? cpu.mem_read32(object + 12u)
                                     : 0xffffffffu;
            note_movie_event(cpu, 0x80306B48u, object, language, cpu.lr);
            return false;
        },
        "PAL-select loader fn_80306b48 (ui probe)");
    // RwFopen. Its name argument is the sprintf result, so logging the first
    // eight bytes of the string says exactly which path the guest asks for -
    // which is what separates "never asked" from "asked and was refused".
    cpu_.install_hle(
        0x8047B1C0u,
        [this](Cpu& cpu) {
            const u32 name = cpu.gpr[3];
            u32 first = 0;
            u32 second = 0;
            if (guest_pointer(cpu, name)) {
                first = cpu.mem_read32(name);
                second = cpu.mem_read32(name + 4u);
            }
            note_movie_event(cpu, 0x8047B1C0u, name, first, second, cpu.lr);
            return false;
        },
        "RwFopen (ui probe)");
    // fn_80306638 is where the 50/60 Hz screen actually reads input:
    // PadManager::Update, then per pad, skip unless pad+40 (status) is 0, then
    // read pad+8 - an EDGE word, new & (old ^ new). The screen only acts on
    // D-pad LEFT/RIGHT (0x100/0x200) and A/START (0x001/0x400).
    // The manager singleton is at 0x8056FE0C: Pad[4] of 44 bytes, +176 =
    // numPads, +180 = driver.
    cpu_.install_hle(
        0x80306638u,
        [this](Cpu& cpu) {
            note_movie_event(cpu, 0x80306638u,
                             lifted_.peripheral.connected_mask(),  // connected controller ports
                             cpu.mem_read32(0x8056FE34u),   // Pad[0].status
                             cpu.mem_read32(0x8056FE0Cu),   // Pad[0] held buttons
                             cpu.mem_read32(0x8056FE14u));  // Pad[0] edge word
            return false;
        },
        "PAL-select input read (ui probe)");
}

// The recovered Sofdec pipeline, observed at the guest's own call boundaries.
//
// Every handler here returns false and writes nothing, so the recovered bodies
// run untouched: a traced run and an untraced run execute the same guest code.
// The sites, in the order the guest reaches them:
//
//   fn_80036d68  the movie object is created and published to r13+0x2c50
//   fn_80036b0c  the per-frame movie tick, reached through the pointer at
//                data address 0x8051df08
//   0x80403e3c   CRI get-frame(handle, MWFRAME* out) - out[0] == 0 means the
//                decoder has nothing ready
//   fn_800367a8  the game's frame handler. Its ONLY call site is 0x80036b48,
//                three instructions after the `bc 12,2` that skips it when
//                out[0] is zero, so entering it PROVES a frame was produced.
//                r4 is the 144-byte frame record: +0 picture, +12 width,
//                +16 height.
//   0x804096d0   converts that picture into the locked RwRaster (r5 = the
//                locked pixels, i.e. the address the GX texture will be read
//                from)
//   0x80403a20   CRI release-frame
//   fn_80036570  the Im2D blit of the movie quad, gated on both r13+0x2c50
//                (the movie object) and r13+0x2c48 (the current raster)
void System::install_movie_trace_hooks() {
    if (!movie_trace_enabled_) {
        return;
    }
    cpu_.install_hle(
        0x80036D68u,
        [this](Cpu& cpu) {
            ++movie_counts_.open;
            note_movie_event(cpu, 0x80036D68u, cpu.gpr[3], cpu.gpr[4]);
            return false;
        },
        "movie open (trace)");

    cpu_.install_hle(
        0x80036B0Cu,
        [this](Cpu& cpu) {
            ++movie_counts_.tick;
            // 0x80572248 is the movie module's global block; its first word is
            // the CRI player handle the tick passes to every CRI call.
            const u32 handle = cpu.mem_read32(0x80572248u);
            note_movie_event(cpu, 0x80036B0Cu, cpu.gpr[3], handle);
            return false;
        },
        "movie tick (trace)");

    cpu_.install_hle(
        0x80403E3Cu,
        [this](Cpu& cpu) {
            ++movie_counts_.get_frame;
            // The CRI player is a nine-module pipeline. 0x803fed94 dispatches
            // into it as vtable[method] where vtable = *(inner + 7996 + 68*module),
            // and 0x803fa6a4 asks module 6 for method 11 - "hand me a decoded
            // picture". Those vtables only exist at run time, so recording them
            // here is the only way to learn which recovered function actually
            // decides that a frame is not ready, and then read it statically.
            const u32 handle = cpu.gpr[3];
            if (!guest_pointer(cpu, handle)) {
                return false;
            }
            const u32 inner = cpu.mem_read32(handle + 0x3cu);
            if (!guest_pointer(cpu, inner)) {
                return false;
            }
            // 0x803ff710 - module 6's method 11, the "hand me a picture" entry -
            // reports "nothing ready" unless *(inner + 0x48) is 3 or 4:
            //     lwz r7,72(r3) / addi r0,r7,-3 / cmplwi r0,1 / bc 4,1,work
            // so that word IS the decoder's readiness state, and it is also the
            // word the PLAYEND stub overwrites. Logging it every poll shows
            // whether the decoder ever leaves the ready state and what it sits
            // in. inner+0x58 gates 0x803fa6a4; inner+0x968/+0x96c are the frame
            // counters it compares.
            // 0x803ff710 then calls 0x803f0374(inner, *(inner+8408), out), which
            // refuses with "no picture" unless *(inner + sub*116 + 4884) == 0,
            // and otherwise chains into module *(inner + sub*116 + 4956).
            // Logging the whole chain says exactly which gate is shut.
            const u32 sub = cpu.mem_read32(inner + 8408u);
            const u32 base = inner + sub * 116u;
            note_movie_event(cpu, 0x80403E3Cu, cpu.mem_read32(inner + 0x48u), sub,
                             sub < 64u ? cpu.mem_read32(base + 4884u) : 0xdeadbeefu,
                             sub < 64u ? cpu.mem_read32(base + 4956u) : 0xdeadbeefu);
            return false;
        },
        "CRI get-frame (trace)");

    // The nine-module poll: 0x803fede8 walks modules 0..8 calling method 0 on
    // each. Recording every module's vtable once names every stage of the
    // recovered player - including whichever one is the ADX audio module.
    cpu_.install_hle(
        0x803FEDE8u,
        [this](Cpu& cpu) {
            ++movie_counts_.module_poll;
            // r4 is the METHOD index the chain is pumped with, not a module.
            // 0x803fede8 walks modules 0..8 calling vtable[r4] on each and
            // stops at the first non-zero return, so which methods the player
            // actually pumps is what says whether the decoder is ever driven.
            if (cpu.gpr[4] < 16u) {
                ++movie_counts_.poll_method[cpu.gpr[4]];
            }
            if (movie_module_vtables_logged_) {
                return false;
            }
            const u32 inner = cpu.gpr[3];
            if (!guest_pointer(cpu, inner)) {
                return false;
            }
            movie_module_vtables_logged_ = true;
            for (u32 module = 0; module < 9u; ++module) {
                const u32 vtable = cpu.mem_read32(inner + 7996u + 68u * module);
                u32 first = 0;
                if (guest_pointer(cpu, vtable)) {
                    first = cpu.mem_read32(vtable);
                }
                note_movie_event(cpu, 0x803FEDE8u, inner, module, vtable, first);
            }
            return false;
        },
        "CRI module poll (trace)");

    // Module 2 is the MPEG video decoder. Its method 0 is what the nine-module
    // poll calls every service pass, so counting it separates "the decoder is
    // never asked to work" from "it is asked and produces nothing".
    cpu_.install_hle(
        0x803F9398u,
        [this](Cpu&) {
            ++movie_counts_.video_tick;
            return false;
        },
        "CRI video module tick (trace)");

    // Module 1's method 0 - the stage that feeds module 2.
    cpu_.install_hle(
        0x803F449Cu,
        [this](Cpu&) {
            ++movie_counts_.demux_tick;
            return false;
        },
        "CRI demux module tick (trace)");

    cpu_.install_hle(
        0x80036E88u, [this](Cpu&) { ++movie_counts_.service; return false; },
        "movie service fn_80036e88 (trace)");

    // Where the decoding actually lives. mwPlyInitSfdFx registers the three
    // MWSFSVR procs as CRI SVM servers on svtype 2 (Vsync), 5 (Main) and
    // 6 (Idle); the game passes iprm[3] = 0, so the Main proc is a no-op and
    // every decode runs in the Idle proc on the lowest-priority (24) CRI OS
    // thread. If that thread never runs, a second frame cannot exist.
    cpu_.install_hle(
        0x803C5330u, [this](Cpu&) { ++movie_counts_.cri_thread_prio24; return false; },
        "CRI prio-24 decode thread body (trace)");
    cpu_.install_hle(
        0x803CC24Cu, [this](Cpu&) { ++movie_counts_.svm_exec_idle; return false; },
        "SVM_ExecSvr svtype 6 idle server (trace)");
    cpu_.install_hle(
        0x80408C44u, [this](Cpu&) { ++movie_counts_.idle_thrd_proc; return false; },
        "MWSFSVR_IdleThrdProc (trace)");
    cpu_.install_hle(
        0x80408A68u, [this](Cpu&) { ++movie_counts_.exec_svr_hndl; return false; },
        "mwPlyExecSvrHndl (trace)");
    cpu_.install_hle(
        0x80408B30u, [this](Cpu&) { ++movie_counts_.sfd_exec_svr; return false; },
        "mwlSfdExecSvr (trace)");
    cpu_.install_hle(
        0x80408D80u,
        [this](Cpu& cpu) {
            ++movie_counts_.sfd_exec_dec;
            // Keyed on the MwePlyStat word at handle+4; log it so the state the
            // decode server keeps refusing on is visible.
            const u32 handle = cpu.gpr[3];
            const u32 stat = guest_pointer(cpu, handle)
                                 ? cpu.mem_read32(handle + 4u)
                                 : 0xffffffffu;
            note_movie_event(cpu, 0x80408D80u, handle, stat);
            return false;
        },
        "mwlSfdExecDecSvr (trace)");
    cpu_.install_hle(
        0x803FC394u, [this](Cpu&) { ++movie_counts_.sfd_exec_server; return false; },
        "SFD_ExecServer (trace)");
    cpu_.install_hle(
        0x803F2898u, [this](Cpu&) { ++movie_counts_.m2_module0; return false; },
        "module 0 method 2 (trace)");
    cpu_.install_hle(
        0x803F4458u, [this](Cpu&) { ++movie_counts_.m2_module1; return false; },
        "module 1 method 2 (trace)");
    cpu_.install_hle(
        0x803F92E0u, [this](Cpu&) { ++movie_counts_.m2_module2; return false; },
        "module 2 method 2 - MPEG video decoder tick (trace)");
    cpu_.install_hle(
        0x803CC3FCu, [this](Cpu&) { ++movie_counts_.svm_exec_vsync; return false; },
        "SVM_ExecSvr svtype 2 vsync server (trace)");
    cpu_.install_hle(
        0x80408CECu, [this](Cpu&) { ++movie_counts_.vsync_thrd_proc; return false; },
        "MWSFSVR_VsyncThrdProc (trace)");
    cpu_.install_hle(
        0x803C5150u, [this](Cpu&) { ++movie_counts_.adxgc_init; return false; },
        "ADXGC init, creates the four CRI threads (trace)");
    cpu_.install_hle(
        0x80378ECCu,
        [this](Cpu& cpu) {
            ++movie_counts_.os_resume_thread;
            note_movie_event(cpu, 0x80378ECCu, cpu.gpr[3], cpu.lr);
            return false;
        },
        "OSResumeThread (trace)");
    cpu_.install_hle(
        0x8040534Cu, [this](Cpu&) { ++movie_counts_.cri_service; return false; },
        "CRI player service fn_8040534c (trace)");

    // The four pumps of the module chain. Counting them says which driver is
    // missing; the reach log cannot answer this because it only records calls
    // the interpreter took, so its negatives carry no information.
    cpu_.install_hle(
        0x803FA858u, [this](Cpu&) { ++movie_counts_.pump_a858; return false; },
        "CRI pump 0x803fa858 method 4 (trace)");
    cpu_.install_hle(
        0x803FADACu, [this](Cpu&) { ++movie_counts_.pump_adac; return false; },
        "CRI pump 0x803fadac method 4 (trace)");
    cpu_.install_hle(
        0x803FB014u, [this](Cpu&) { ++movie_counts_.pump_b014; return false; },
        "CRI pump 0x803fb014 method 3 (trace)");
    cpu_.install_hle(
        0x803FC0E0u, [this](Cpu&) { ++movie_counts_.pump_c0e0; return false; },
        "CRI pump 0x803fc0e0 method 2 (trace)");

    // fn_803f4aa8 asks this for the head of the decoded-picture queue and
    // reports "no frame" when it returns null.
    cpu_.install_hle(
        0x803F9C80u,
        [this](Cpu&) {
            ++movie_counts_.queue_head;
            return false;
        },
        "CRI picture-queue head (trace)");

    cpu_.install_hle(
        0x800367A8u,
        [this](Cpu& cpu) {
            ++movie_counts_.frame_ready;
            const u32 frame = cpu.gpr[4];
            u32 picture = 0;
            u32 width = 0;
            u32 height = 0;
            if (guest_pointer(cpu, frame)) {
                picture = cpu.mem_read32(frame);
                width = cpu.mem_read32(frame + 12u);
                height = cpu.mem_read32(frame + 16u);
                movie_frame_width_ = width;
                movie_frame_height_ = height;
            }
            note_movie_event(cpu, 0x800367A8u, picture, width, height, cpu.gpr[3]);
            return false;
        },
        "movie frame ready (trace)");

    cpu_.install_hle(
        0x804096D0u,
        [this](Cpu& cpu) {
            ++movie_counts_.convert;
            // r5 is the locked RwRaster's pixel pointer, i.e. where the CFT
            // kernels are about to write the converted picture.
            movie_convert_dest_ = cpu.gpr[5];
            note_movie_event(cpu, 0x804096D0u, cpu.gpr[3], cpu.gpr[4], cpu.gpr[5], cpu.gpr[6]);
            return false;
        },
        "movie picture convert (trace)");

    // RwRasterUnlock. The movie path calls it at 0x80036af4, immediately after
    // the conversion, and the pixels are still linear at that point - the GX
    // tiling happens inside. Gating on the return address keeps this to the
    // movie's own raster and off every other unlock in the game.
    cpu_.install_hle(
        0x8048AB90u,
        [this](Cpu& cpu) {
            if (!movie_frame_dump_.empty() && cpu.lr == 0x80036AF8u) {
                dump_movie_frame(cpu);
            }
            return false;
        },
        "RwRasterUnlock (movie frame dump)");

    cpu_.install_hle(
        0x80403A20u,
        [this](Cpu&) {
            ++movie_counts_.release;
            return false;
        },
        "CRI release-frame (trace)");

    cpu_.install_hle(
        0x80036570u,
        [this](Cpu& cpu) {
            ++movie_counts_.blit;
            const u32 sda = cpu.gpr[13];
            const u32 object = cpu.mem_read32(sda + 11344u);   // r13+0x2c50
            const u32 raster = cpu.mem_read32(sda + 11336u);   // r13+0x2c48
            if (object == 0u || raster == 0u) {
                ++movie_counts_.blit_skipped;
            }
            note_movie_event(cpu, 0x80036570u, object, raster, cpu.gpr[3], cpu.lr);
            return false;
        },
        "movie Im2D blit (trace)");
}

// The decoded movie picture, straight out of the CRI colour-format transform
// and before RenderWare tiles it for GX. Written as four PPMs, one per byte
// lane, plus the natural RGB reading - the 32-bit RenderWare raster's channel
// order on this platform is not something to assume, and a picture that is
// merely channel-swapped looks very different from one that is genuinely black.
void System::dump_movie_frame(Cpu& cpu) {
    // Spread the captures across the movie rather than taking the first few:
    // an opening that fades in from black makes consecutive early frames
    // indistinguishable, which is exactly the ambiguity this dump exists to
    // resolve.
    constexpr u32 kMaxDumps = 8;
    constexpr u32 kEvery = 30;
    const u32 seen = movie_frames_seen_++;
    if (movie_frames_dumped_ >= kMaxDumps || (seen % kEvery) != 0u) {
        return;
    }
    const u32 base = movie_convert_dest_;
    const u32 width = movie_frame_width_;
    const u32 height = movie_frame_height_;
    if (!memory_.mapped(GcAddr(base), 4u) || width == 0 || height == 0 ||
        width > 1024u || height > 1024u) {
        return;
    }
    // The raster is created at the picture size rounded up to powers of two
    // (RwRasterCreate(pow2(w), pow2(h), 32, 4) in fn_800367a8), so the row
    // stride follows the rounded width, not the picture width.
    u32 stride_pixels = 1;
    while (stride_pixels < width) {
        stride_pixels <<= 1;
    }
    const u32 index = movie_frames_dumped_++;
    char name[512];
    std::snprintf(name, sizeof(name), "%s_%02u.ppm", movie_frame_dump_.c_str(), index);
    std::ofstream out(name, std::ios::binary);
    if (!out) {
        return;
    }
    out << "P6\n" << width << " " << height << "\n255\n";
    // RwRasterLock hands back the GX texture memory itself, not a linear
    // staging copy, so the CFT kernels write straight into GX_TF_RGBA8 tiles:
    // 4x4 texels per tile, 32 bytes of (A,R) pairs followed by 32 bytes of
    // (G,B) pairs. Reading it linearly produces a regular stripe pattern that
    // looks like a decoder fault and is not one.
    const auto byte_at = [&cpu](u32 address) {
        const u32 word = cpu.mem_read32(address & ~3u);
        return static_cast<u8>((word >> ((3u - (address & 3u)) * 8u)) & 0xffu);
    };
    const u32 tiles_across = stride_pixels / 4u;
    std::vector<u8> row(static_cast<std::size_t>(width) * 3u);
    u64 nonzero = 0;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u32 tile = ((y / 4u) * tiles_across + (x / 4u)) * 64u;
            const u32 texel = ((y % 4u) * 4u + (x % 4u)) * 2u;
            const u8 red = byte_at(base + tile + texel + 1u);
            const u8 green = byte_at(base + tile + 32u + texel);
            const u8 blue = byte_at(base + tile + 32u + texel + 1u);
            if ((red | green | blue) != 0u) {
                ++nonzero;
            }
            row[x * 3u + 0u] = red;
            row[x * 3u + 1u] = green;
            row[x * 3u + 2u] = blue;
        }
        out.write(reinterpret_cast<const char*>(row.data()),
                  static_cast<std::streamsize>(row.size()));
    }
    out.close();
    std::printf("[movie-frame] raw bytes at 0x%08X:", base);
    for (u32 offset = 0; offset < 48u; offset += 4u) {
        std::printf(" %08X", cpu.mem_read32(base + offset));
    }
    std::printf("\n[movie-frame] row 1 at +%u:", stride_pixels * 4u);
    for (u32 offset = 0; offset < 32u; offset += 4u) {
        std::printf(" %08X", cpu.mem_read32(base + stride_pixels * 4u + offset));
    }
    std::printf("\n");
    std::printf("[movie-frame] %s  %ux%u stride %u from 0x%08X, %llu/%u pixels non-zero\n", name,
                width, height, stride_pixels, base,
                static_cast<unsigned long long>(nonzero),
                static_cast<unsigned>(width * height));
    std::fflush(stdout);
}

// One AX frame, as the DSP microcode would advance it.
//
// Run immediately before the guest enters fn_80386bc0, the per-frame AX driver
// reached from the AI DMA callback. That function DCInvalidateRanges the
// DSP-visible parameter blocks and then, per voice, runs fn_80386474 - which in
// its steady state copies exactly four halfwords back from the parameter block
// into the guest's own AXVPB:
//
//     803864A4  lhz r0,14(r5)  / sth r0,14(r6)     state
//     803864AC  lhz r0,100(r5) / sth r0,100(r6)    ve.currentVolume
//     803864B4  lhz r0,122(r5) / sth r0,122(r6)    currentAddressHi
//     803864BC  lhz r0,124(r5) / sth r0,124(r6)    currentAddressLo
//
// Those four are the entire surface the guest observes, so writing the play
// position into the parameter block is exactly what makes the guest believe
// audio is progressing. Writing the AXVPB copy instead would be wrong: the very
// next frame copies it back from the parameter block.
//
// Addresses here are sound addresses (a native sound store's, docs/ARAM.md) in FORMAT-DEPENDENT units, and endAddress
// is inclusive:
//     format 0  ADPCM  nibble address   (16 nibbles carry 14 samples)
//     format 10 PCM16  16-bit sample index
//     format 25 PCM8   byte address
// Advancing a PCM16 voice in bytes would run it at twice speed and put the
// Sofdec clock out by the same factor.
void System::reset_host_audio_state() noexcept {
    ax_voice_pb_.fill(0u);
    ax_pb_order_.fill(0u);
    ax_pb_count_ = 0u;
    ax_compressor_table_verified_ = 0u;
    ax_position_fraction_.fill(0u);
    ax_voice_was_running_.fill(false);
    ax_voice_restarted_.fill(false);
    ax_span_start_.fill(0u);
    ax_span_frac_.fill(0u);
    ax_span_active_.fill(false);
    ax_adpcm_yn1_.fill(0);
    ax_adpcm_yn2_.fill(0);
    ax_adpcm_position_.fill(std::numeric_limits<u32>::max());
    // A cold restart invalidates decoder history. Checkpoint loads restore the
    // saved history instead. Ring entries outside the cursor's decoded window
    // are never read and need no clearing.
    ax_adpcm_cursor_state_.fill(AxAdpcmVoiceCursor{});
    if (audio_mixer_) audio_mixer_->reset();
    reverb_binding_ = reverb::Binding{};
}

void System::install_reverb_owner() {
    // ReverbHiInit and ReverbHiShutdown stay translated SDK code (they allocate and free the delay lines the
    // native effect no longer uses); their entries only tell the effect which parameters and object it serves.
    cpu_.install_hle(reverb::kInit, [this](Cpu& cpu) {
        reverb::observe_init(cpu, reverb_binding_, audio_mixer_->effect());
        return false;  // the original initialization runs
    }, "room reverb: ReverbHiInit (parameters to the native effect)");
    cpu_.install_hle(reverb::kShutdown, [this](Cpu& cpu) {
        reverb::observe_shutdown(cpu, reverb_binding_);
        return false;  // the original release runs
    }, "room reverb: ReverbHiShutdown");
    // The callback itself is native: the translated body, its crossfeed and HandleReverb are not generated
    // (extract/native-owners.txt).
    cpu_.install_hle(reverb::kCallback, [this](Cpu& cpu) {
        const auto start = std::chrono::steady_clock::now();
        auto result = reverb::serve_callback(cpu, reverb_binding_, audio_mixer_->effect());
        if (result == reverb::CallbackResult::Unbound && !reverb_binding_.configured) {
            // A checkpoint older than version 23 carries no effect history: the game initialized this object
            // before the save, so its parameters are in guest memory. The tail starts empty.
            shadow::audio::Reverb::Parameters parameters;
            reverb_binding_.context = cpu.gpr[4];
            reverb_binding_.configured = reverb::read_parameters(memory_, reverb_binding_.context, parameters) &&
                                         audio_mixer_->effect().configure(parameters);
            std::fprintf(stderr, "[reverb] effect bound to 0x%08X from the game's parameters (%s)\n",
                         reverb_binding_.context, reverb_binding_.configured ? "tail starts empty" : "refused");
            result = reverb::serve_callback(cpu, reverb_binding_, audio_mixer_->effect());
        }
        switch (result) {
            case reverb::CallbackResult::Processed: ++ax_report_.effect_processed; break;
            case reverb::CallbackResult::Bypassed: ++ax_report_.effect_bypassed; break;
            case reverb::CallbackResult::Idle: ++ax_report_.effect_idle; break;
            case reverb::CallbackResult::Unbound: {
                char detail[128];
                std::snprintf(detail, sizeof(detail),
                              "room reverb callback for 0x%08X, but the native effect belongs to 0x%08X%s",
                              cpu.gpr[4], reverb_binding_.context, reverb_binding_.configured ? "" : " (unconfigured)");
                cpu.request_stop(StopReason::ExecutionFault, detail);
                break;
            }
        }
        ax_report_.effect_ns += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        return true;  // an owner never declines
    }, "room reverb: ReverbHiCallback (native effect)");
}

void System::reset_ax_voice(u32 voice) noexcept {
    ax_position_fraction_[voice] = 0u;
    ax_span_frac_[voice] = 0u;
    ax_voice_was_running_[voice] = ax_span_active_[voice] = false;
    ax_voice_restarted_[voice] = false;
    ax_adpcm_position_[voice] = std::numeric_limits<u32>::max();
    ax_adpcm_yn1_[voice] = ax_adpcm_yn2_[voice] = 0;
    ax_adpcm_cursor_state_[voice] = AxAdpcmVoiceCursor{};
}

bool System::select_ax_parameter_blocks(u32 head) {
    std::array<u32, kAxVoiceSlots> addresses{}, links{};
    u32 count = 0;
    // Validate the ENTIRE chain before advancing any voice or replacing host
    // decoder ownership. PB+0/2 is nextHi/nextLo (guest init 80386FF0..80387004).
    while (head != 0u) {
        u32 address = 0u;
        if (!ax_contract_address(head, address)) break;
        if ((address & 1u) != 0u || !memory_.mapped(GcAddr(address), kAxParameterBlockBytes) ||
            count == addresses.size())
            break;
        links[count] = head;
        addresses[count++] = address;
        head = memory_.read32(address);
    }
    // Normally the previous frame's chain again: already free of overlaps, and every block keeps its slot.
    const bool unchanged = ax_chain_unchanged(addresses.data(), count, ax_voice_pb_, ax_pb_order_, ax_pb_count_);
    if (!unchanged) {
        // The chain ends at the first block overlapping an earlier one (a cycle repeats a block); the walk above
        // stops at the capacity instead, so the same block, link and count are reported.
        const u32 overlap = first_ax_block_overlap(addresses.data(), count);
        if (overlap != count) {
            head = links[overlap];
            count = overlap;
        }
    }
    if (head != 0u) {
        char detail[160];
        std::snprintf(detail, sizeof(detail),
                      "AX invalid PB chain at 0x%08X after %u blocks (range, overlap/cycle, or capacity)",
                      head, count);
        cpu_.request_stop(StopReason::ExecutionFault, detail);
        return false;
    }
    const u64 reset = unchanged ? ax_empty_slots(ax_voice_pb_)
                                : assign_ax_voices(addresses.data(), count, ax_voice_pb_, ax_pb_order_);
    for (u32 voice = 0; voice < kAxVoiceSlots; ++voice) {
        if ((reset >> voice) & 1u) reset_ax_voice(voice);
    }
    ax_pb_count_ = count;
    return true;
}

void System::advance_ax_accepted_voices() {
    const auto ax_advance_start = std::chrono::steady_clock::now();
    struct AdvanceTimer {
        System* system;
        std::chrono::steady_clock::time_point start;
        ~AdvanceTimer() {
            system->ax_report_.advance_ns += static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
        }
    } advance_timer{this, ax_advance_start};
    // run_ax_mix has admitted the whole descriptor chain for this job. Never
    // recover an older mailbox list or reselect identities across that boundary.
    // 160 output samples per 5 ms frame at 32 kHz; the ratio is 16.16 source
    // samples per output sample, clamped by AXSetVoiceSrcRatio to 4.0.
    constexpr u64 kSamplesPerFrame = 160u;

    ++ax_report_.frames;
    for (u32 index = 0; index < ax_pb_count_; ++index) {
        const u32 voice = ax_pb_order_[index];
        const u32 pb = ax_voice_pb_[voice];
        if (memory_.read16(pb + 14u) != 1u) {
            ax_voice_was_running_[voice] = false;
            ax_span_active_[voice] = false;
            continue;
        }
        if (!ax_voice_was_running_[voice]) {
            ax_voice_restarted_[voice] = true;  // the native mixer takes the SDK-primed SRC history (words 86..89)
            ax_position_fraction_[voice] = 0u;
            ax_adpcm_position_[voice] = std::numeric_limits<u32>::max();
            ax_adpcm_yn1_[voice] = 0;
            ax_adpcm_yn2_[voice] = 0;
            // A (re)started voice decodes from the history the guest primed
            // at pb+162/164, not from wherever the last sound on this slot
            // stopped. Host state only; nothing guest-visible changes.
            ax_adpcm_cursor_state_[voice].valid = false;
            ax_voice_was_running_[voice] = true;
        }
        const u32 ratio = (static_cast<u32>(memory_.read16(pb + 166u)) << 16) |
                          memory_.read16(pb + 168u);
        if (ratio == 0u) {
            continue;
        }
        const u32 format = memory_.read16(pb + 112u);
        const u32 loop_flag = memory_.read16(pb + 110u);
        const u32 loop_address = (static_cast<u32>(memory_.read16(pb + 114u)) << 16) |
                                 memory_.read16(pb + 116u);
        const u32 end_address = (static_cast<u32>(memory_.read16(pb + 118u)) << 16) |
                                memory_.read16(pb + 120u);
        const u32 current = (static_cast<u32>(memory_.read16(pb + 122u)) << 16) |
                            memory_.read16(pb + 124u);
        // The loop address is meaningful only for a looping voice. One-shots
        // commonly leave it equal to or above endAddress; rejecting them here
        // prevents state from ever reaching stopped and leaves the guest waiting
        // forever at the authored end time. A looping voice may also have an
        // intro before loopAddress, so current < loopAddress is valid.
        if (current > end_address ||
            (loop_flag != 0u && loop_address > end_address)) {
            ++ax_report_.skipped_insane;
            continue;
        }

        const u64 stepped = kSamplesPerFrame * static_cast<u64>(ratio) +
                            ax_position_fraction_[voice];
        ax_span_frac_[voice] = ax_position_fraction_[voice];
        ax_position_fraction_[voice] = static_cast<u32>(stepped & 0xffffu);
        u64 units = stepped >> 16;  // source samples consumed this frame

        if (format == 0u && units != 0u) {
            // A DSP ADPCM read consumes one sample nibble and skips each
            // two-nibble frame header. The address step therefore depends on
            // the current position inside the 14-sample frame. Scaling the
            // sample count by 16/14 loses that phase and can land on a header,
            // duplicating or skipping a sample at successive AX boundaries.
            const u64 within = current & 15u;
            const u64 sample_in_frame = within < 2u ? 0u : within - 2u;
            const u64 destination = sample_in_frame + units;
            const u64 nibble_in_frames = (destination / 14u) * 16u +
                                         2u + destination % 14u;
            units = nibble_in_frames - within;
        }

        const AxVoiceAdvanceResult advance = advance_ax_voice_position(
            current, end_address, loop_address, loop_flag != 0u, units);
        if (!advance.valid) {
            ++ax_report_.skipped_insane;
            continue;
        }
        const u32 next = advance.next;
        const u16 state = advance.running ? 1u : 0u;
        if (format == 0u && audio_audit::enabled()) {
            // The trace records source samples before their conversion to
            // nibble units. A reducer can independently check header skipping
            // and loop wrap without mutating the guest's PB or decoder.
            audio_audit::event(audio_audit::Lane::Guest, "adpcm-advance",
                cpu_.game_clock().now(), pb, current, next, loop_address,
                end_address, stepped >> 16, loop_flag);
        }
        if (advance.looped) ++ax_report_.loops;
        if (!advance.running) ++ax_report_.finished;

        ax_span_start_[voice] = current;
        ax_span_active_[voice] = true;
        memory_.write16(pb + 122u, static_cast<u16>((next >> 16) & 0xffffu));
        memory_.write16(pb + 124u, static_cast<u16>(next & 0xffffu));
        if (state == 0u) {
            memory_.write16(pb + 14u, 0u);
            ax_voice_was_running_[voice] = false;
        }
        ++ax_report_.voice_advances;
    }
}

// Execute one AX command list: work out where the frame's audio goes, mix the
// running voices into it, and leave 160 stereo samples behind.
//
// This runs from the DSP mail handler, which is the moment the guest hands the
// list over - the same instant real hardware would start executing it. The
// output buffer must come from the list itself (opcode 0x0E's second address):
// it is built one frame before it is pushed and rotates through three slots, so
// assuming a slot would write the wrong one.
//
// The list is read here and turned into the native mixer's frame settings; nothing else of it reaches the mixer.
//
// Grammar: the microcode's own (05_DSP_MICROCODE_SEMANTICS.md section 5; dispatch table at IRAM 0x0C9D): every word
// is a big-endian u16, every address <hi><lo>, parameter words per id below; an id above 0x13 halts the DSP (mail
// 0xBAAD00id) and stops the run here with the same meaning; the list ends at 0x000F (what follows is stale data from
// an earlier frame). Shapes: the SDK builder 0x803850AC emits, in this order,
//     00 setup, [07 | 11] surround fold (AXSetMode 0 | 1), 02 PB head, 03 process,
//     [13 send aux A + mix] (mode 2), [10 aux B L/R matrix] (mode 2), [04 aux A] [05 aux B] (modes 0/1),
//     [12 compressor], 0E output (surround address, stereo address), 0F end.
// The ids the builder never emits (01 06 08 09 0D) and any other order are refused with the id named, never skipped:
// a list this boundary cannot express as frame settings must not be mixed as if it were another one.
// Op 00's record (depop ramps built from the PB depop words, which only the DSP writes and this mixer does not) is
// all zero by construction and is not read.
namespace {
constexpr u8 kAxCommandWords[0x14] = {2, 5, 2, 0, 4, 4, 2, 2, 4, 2, 0, 0, 0, 3, 4, 0, 4, 2, 4, 12};
int ax_command_rank(u16 id) noexcept {
    switch (id) {
        case 0x00: return 0;
        case 0x07: case 0x11: return 1;
        case 0x02: return 2;
        case 0x03: return 3;
        case 0x13: return 4;
        case 0x10: return 5;
        case 0x04: return 6;
        case 0x05: return 7;
        case 0x12: return 8;
        case 0x0E: return 9;
        case 0x0F: return 10;
        default: return -1;
    }
}
}  // namespace

bool System::parse_ax_list(u32 command_list, u32& head, u32& output, shadow::audio::FrameSettings& settings) {
    const auto valid_address = [this](u32 value, u32 size, u32& address) {
        return ax_contract_address(value, address) && (address & 1u) == 0u &&
               memory_.mapped(GcAddr(address), size);
    };
    settings = shadow::audio::FrameSettings{};
    u32 compressor_table = 0u;
    bool have_head = false, have_output = false;
    char why[96] = "list out of bounds";
    u32 list = 0u;
    if (valid_address(command_list, 384u, list)) {
        std::snprintf(why, sizeof(why), "no end command");
        u32 cursor = list;
        const u32 limit = cursor + 384u;
        int last_rank = -1;
        while (cursor + 2u <= limit) {
            const u16 id = memory_.read16(cursor);
            cursor += 2u;
            if (id > 0x13u) {
                std::snprintf(why, sizeof(why), "command 0x%04X above 0x13 (the DSP halts)", id);
                break;
            }
            const u32 bytes = 2u * kAxCommandWords[id];
            if (bytes > limit - cursor) {
                std::snprintf(why, sizeof(why), "command 0x%02X runs past the list", id);
                break;
            }
            const int rank = ax_command_rank(id);
            if (rank < 0) {
                std::snprintf(why, sizeof(why), "command 0x%02X is never built by the SDK (0x803850AC)", id);
                break;
            }
            if (rank < last_rank) {
                std::snprintf(why, sizeof(why), "command 0x%02X out of the SDK's order", id);
                break;
            }
            last_rank = rank;
            switch (id) {
                case 0x02: head = memory_.read32(cursor); have_head = true; break;
                case 0x07: settings.mode = shadow::audio::OutputMode::StereoFold; break;
                case 0x11: settings.mode = shadow::audio::OutputMode::OppositeFold; break;
                case 0x10: settings.mode = shadow::audio::OutputMode::DolbyMatrix; break;
                case 0x04: case 0x13: settings.aux_a = true; break;
                case 0x05: settings.aux_b = true; break;
                case 0x12:
                    settings.compressor = true;
                    settings.compressor_threshold = memory_.read16(cursor);
                    settings.compressor_release_frames = memory_.read16(cursor + 2u);
                    compressor_table = memory_.read32(cursor + 4u);
                    break;
                case 0x0E: output = memory_.read32(cursor + 4u); have_output = true; break;
                default: break;
            }
            cursor += bytes;
            if (id == 0x12u && settings.compressor_release_frames > 10u) {
                // Release rows are 11 + counter: more than 10 frames would read past the 21-row gain table.
                std::snprintf(why, sizeof(why), "compressor release of %u frames (the table has 10 release rows)",
                              static_cast<unsigned>(settings.compressor_release_frames));
                break;
            }
            if (id == 0x12u && compressor_table != ax_compressor_table_verified_) {
                // The gains are the native copy of the game's table (main.dol 0x80563420); the list names the
                // guest's copy, which must hold the same 21 x 160 big-endian u16 (checked once per address).
                u32 table = 0u;
                bool same = ax_contract_address(compressor_table, table) &&
                            memory_.mapped(GcAddr(table), 21u * 160u * 2u);
                const auto& native = shadow::audio::compressor_gain_table();
                for (u32 k = 0; same && k < native.size(); ++k) same = memory_.read16(table + k * 2u) == native[k];
                if (!same) {
                    std::snprintf(why, sizeof(why), "compressor table at 0x%08X differs from the game's (0x80563420)",
                                  compressor_table);
                    break;
                }
                ax_compressor_table_verified_ = compressor_table;
            }
            if (id == 0x0Fu) {
                u32 output_address = 0u;
                if (have_head && have_output && valid_address(output, 640u, output_address)) {
                    output = output_address;
                    return true;
                }
                std::snprintf(why, sizeof(why), "no PB head or no valid output buffer");
                break;
            }
        }
    }
    ++ax_report_.lists_unparsed;
    char detail[160];
    std::snprintf(detail, sizeof(detail), "AX invalid command list at 0x%08X: %s", command_list, why);
    cpu_.request_stop(StopReason::ExecutionFault, detail);
    return false;
}

void System::run_ax_mix(u32 command_list) {
    audio_audit::Scope audit_mix("mix", "mix");
    audio_audit::event(audio_audit::Lane::Guest, "mix", cpu_.game_clock().now(), command_list);
    const auto ax_mix_start = std::chrono::steady_clock::now();
    struct MixTimer {
        System* system;
        std::chrono::steady_clock::time_point start;
        ~MixTimer() {
            system->ax_report_.mix_ns += static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count());
        }
    } mix_timer{this, ax_mix_start};
    ++ax_report_.lists;
    u32 head = 0, output = 0;
    shadow::audio::FrameSettings settings;
    if (!parse_ax_list(command_list, head, output, settings) || !select_ax_parameter_blocks(head)) return;
    if (settings.aux_a) ++ax_report_.effect_scheduled;
    // Parsing and complete chain validation precede every source/state effect.
    // This executes synchronously on the accepting producer thread: neither
    // the output thread nor a later SDK sync may mutate the descriptor between
    // advancement and mixing. The completed position is then available for
    // the next sync's readback, rather than being fabricated at its entry.
    advance_ax_accepted_voices();

    constexpr u32 kSamples = shadow::audio::kFrameSamples;   // one 5 ms frame at 32 kHz
    // This function is the boundary: it reads the guest's parameter blocks and the sound stores and hands the native mixer
    // plain per-voice values (source samples, ratio, gains); the mixer owns the arithmetic.
    std::array<shadow::audio::VoiceFrame, shadow::audio::kMaxVoices> voice_frames{};
    u32 submitted = 0;

    const SoundArena::Source banks = sound_banks_->arena().source();
    const SoundArena::Source ring = stream_ring_->arena().source();

    for (u32 index = 0; index < ax_pb_count_; ++index) {
        const u32 voice = ax_pb_order_[index];
        if (!ax_span_active_[voice]) {
            continue;
        }
        ax_span_active_[voice] = false;
        const u32 pb = ax_voice_pb_[voice];
        const u32 format = memory_.read16(pb + 112u);
        const u32 loop_flag = memory_.read16(pb + 110u);
        const u32 ratio = (static_cast<u32>(memory_.read16(pb + 166u)) << 16) |
                          memory_.read16(pb + 168u);
        const u32 loop_address = (static_cast<u32>(memory_.read16(pb + 114u)) << 16) |
                                 memory_.read16(pb + 116u);
        const u32 end_address = (static_cast<u32>(memory_.read16(pb + 118u)) << 16) |
                                memory_.read16(pb + 120u);
        const u32 envelope = memory_.read16(pb + 100u);
        const s32 volume_left = static_cast<s32>(memory_.read16(pb + 18u));
        const s32 volume_right = static_cast<s32>(memory_.read16(pb + 22u));
        if (audio_audit::enabled()) {
            audio_audit::event(audio_audit::Lane::Guest, "voice-mix", cpu_.game_clock().now(),
                pb, ax_span_start_[voice], memory_.peek32(pb + 122u),
                loop_address, end_address, ratio, format);
            audio_audit::event(audio_audit::Lane::Guest, "voice-config",
                cpu_.game_clock().now(), pb, format, memory_.read16(pb + 8u),
                memory_.read16(pb + 16u), static_cast<u32>(volume_left),
                static_cast<u32>(volume_right),
                static_cast<u64>(envelope) | (static_cast<u64>(memory_.read16(pb + 102u)) << 16u));
        }
        if (loop_flag != 0u && loop_address > end_address) {
            continue;
        }
        const bool main_audible = (volume_left | volume_right) != 0 && envelope != 0u;
        const u64 span = loop_flag != 0u
                             ? static_cast<u64>(end_address) - loop_address + 1u
                             : 0u;

        // Where the voice's samples are, by where its range starts: a native sound store (docs/ARAM.md) - the GCAX
        // banks or CRI's stream ring. There is no ARAM: a voice addressing no store, or reaching past its store's
        // committed bytes, is a defect to stop on, never a read to guess.
        const u64 start = std::min<u64>(ax_span_start_[voice], loop_flag != 0u ? loop_address : end_address);
        const u64 first_byte = format == 0u ? start / 2u : format == 10u ? start * 2u : start;
        const u64 last_byte = format == 0u ? end_address / 2u + 1u                       // the end nibble's byte
                              : format == 10u ? static_cast<u64>(end_address) * 2u + 2u  // the end sample's two
                                              : static_cast<u64>(end_address) + 1u;
        const u32 voice_byte = static_cast<u32>(std::min<u64>(first_byte, 0xFFFFFFFFull));
        const SoundArena* store = sound_banks_->arena().holds(voice_byte)   ? &sound_banks_->arena()
                                  : stream_ring_->arena().holds(voice_byte) ? &stream_ring_->arena()
                                                                            : nullptr;
        if (store == nullptr) {
            cpu_.request_stop(StopReason::ExecutionFault,
                              "AX voice addresses no native sound store (pb " + std::to_string(pb) + ")");
            continue;
        }
        const SoundArena::Source& source = store == &sound_banks_->arena() ? banks : ring;
        if (source.bytes == nullptr || last_byte > source.end) {
            cpu_.request_stop(StopReason::ExecutionFault,
                              "AX voice reads past a native sound store (pb " + std::to_string(pb) + ")");
            continue;
        }
        const u8* const samples = source.bytes;
        const u32 sample_mask = source.mask;

        // ADPCM is decoded sequentially: every sample depends on the two
        // before it, so it cannot be point-sampled the way PCM can. Three
        // arms, selected by the two switches in system.h (both default ON):
        //   cursor  - continuous per-voice decoder; the frame decodes forward
        //             from where the previous one stopped and the samples come
        //             from the voice's ring (D07-F3/F4);
        //   sample  - the 2026-08 decode-from-the-frame-boundary run, indexed
        //             in SAMPLE units (D07-F2 alone, set_ax_adpcm_cursor(false));
        //   legacy  - the 2026-08 code verbatim, nibble-unit indexing
        //             (set_ax_sample_resampling(false)); the A/B reference.
        // Why the guest cannot observe any of this: the only thing that
        // changes is the sample values written into the AI output buffer,
        // which the guest never reads back - the AI DMA engine is its only
        // consumer. Source/state advancement belongs to this accepted job.
        //
        // NOT implemented here, deliberately (D07-F5): volume envelope ramps.
        // The real DSP ramps ve.currentVolume (pb+100) by ve.currentDelta
        // (pb+102) per sample and the CPU copies the ramped value back into
        // the AXVPB, so implementing it means a NEW guest-visible write to
        // pb+100 whose per-sample/per-frame semantics (and the mix vDeltaL/R
        // at pb+20/24) have to be read out of the AX microcode at 0x80564E60
        // first. Until that is verified the envelope stays a per-frame
        // constant, as it has been since 2026-08.
        const bool adpcm_sample_units = ax_sample_resampling_;
        const bool adpcm_cursor =
            adpcm_sample_units && ax_adpcm_cursor_ && ax_adpcm_ring_ != nullptr;
        std::vector<s16> decoded;          // sample/legacy arms only; never touched by cursor
        u32 adpcm_frame_start = 0;         // sample/legacy arms: where `decoded` begins
        u64 adpcm_k0 = 0;                  // sample arm: span start's index into `decoded`
        const s16* adpcm_ring = nullptr;   // cursor arm
        u64 adpcm_origin = 0;              // cursor arm: ring index of the span start
        u64 adpcm_available = 0;           // cursor arm: valid samples from the origin
        // A 16-nibble frame carries a 2-nibble header and 14 samples, so a
        // nibble address does not index the decoded run directly.
        const auto sample_index_of = [](u64 relative_nibble) -> hw::AudioFrameIndex64 {
            const u64 frame = relative_nibble / 16u;
            const u64 within = relative_nibble % 16u;
            return frame * 14u + (within < 2u ? 0u : within - 2u);
        };
        if (format == 0u && adpcm_cursor) {
            ++ax_report_.adpcm_voices;
            ++ax_report_.adpcm_sample_unit_voices;
            ++ax_report_.adpcm_cursor_voices;
            s16 coefficients[16];
            for (u32 c = 0; c < 16u; ++c) {
                coefficients[c] = static_cast<s16>(memory_.read16(pb + 126u + c * 2u));
            }
            AdpcmDecodeContext ctx;
            ctx.samples = samples;
            ctx.sample_mask = sample_mask;
            ctx.coefficients = coefficients;
            ctx.end_address = end_address;
            ctx.loop_address = loop_address;
            ctx.looping = loop_flag != 0u;
            ctx.loop_pred_scale = memory_.read16(pb + 180u);
            ctx.loop_yn1 = static_cast<s16>(memory_.read16(pb + 182u));
            ctx.loop_yn2 = static_cast<s16>(memory_.read16(pb + 184u));

            AxAdpcmVoiceCursor& cursor = ax_adpcm_cursor_state_[voice];
            s16* ring = ax_adpcm_ring_->voice[voice].data();
            constexpr hw::AudioFrameIndex64 kRingMask = kAxAdpcmRingSamples - 1u;
            // How far the cursor may be from the span start and still be
            // treated as continuous. advance_ax_accepted_voices advances the guest-visible
            // nibble address by units*16/14 with a carried remainder, which
            // lands within +-2 samples of where the mix stopped; anything
            // larger is a guest restart or seek and gets a fresh cursor.
            constexpr u64 kForwardTolerance = 64u;
            constexpr u64 kBackTolerance = 64u;
            const u64 start_frac = ax_span_frac_[voice];
            // Every sample the frame reads: the microcode's SRC reads up to the sample before the frame's end
            // position (frac + 160 * ratio), its window update the 4 before it; +1 keeps that sample itself.
            const u64 end_pos = static_cast<u64>(kSamples) * ratio + start_frac;
            u64 needed = (end_pos >> 16) + 1u;
            if (needed > kAxAdpcmRingSamples) {
                // A ratio above ~6.4 (the SDK clamps at 4.0) outruns the ring;
                // the samples beyond it read as silence rather than aliasing.
                needed = kAxAdpcmRingSamples;
                ++ax_report_.adpcm_span_clamped;
            }

            const u32 span_start = ax_span_start_[voice];
            const u64 span_abs = adpcm_nibble_to_sample(span_start);
            const u64 loop_abs = adpcm_nibble_to_sample(loop_address);
            const u64 end_abs = adpcm_nibble_to_sample(end_address);
            bool continuous = false;
            u64 skip = 0;  // samples to decode and discard before the origin
            u64 back = 0;  // samples already in the ring before the cursor
            if (cursor.valid) {
                const u64 cursor_abs = adpcm_nibble_to_sample(cursor.nibble);
                if (span_abs >= cursor_abs) {
                    const u64 ahead = span_abs - cursor_abs;
                    if (ahead <= kForwardTolerance) {
                        continuous = true;
                        skip = ahead;
                    } else if (ctx.looping && cursor_abs >= loop_abs && span_abs <= end_abs) {
                        // The cursor has wrapped, the span start has not yet:
                        // the samples are the tail of the loop, still in the ring.
                        const u64 behind = (cursor_abs - loop_abs) + (end_abs + 1u - span_abs);
                        if (behind <= kBackTolerance && behind <= cursor.decoded) {
                            continuous = true;
                            back = behind;
                        }
                    }
                } else {
                    const u64 behind = cursor_abs - span_abs;
                    if (behind <= kBackTolerance && behind <= cursor.decoded) {
                        continuous = true;
                        back = behind;
                    } else if (ctx.looping && cursor_abs <= end_abs + 1u &&
                               span_abs >= loop_abs && span_abs <= end_abs) {
                        // The span start has wrapped, the cursor is about to.
                        const u64 ahead = (end_abs + 1u - cursor_abs) + (span_abs - loop_abs);
                        if (ahead <= kForwardTolerance) {
                            continuous = true;
                            skip = ahead;
                        }
                    }
                }
            }
            if (!continuous) {
                // Establish the cursor at the containing frame so the header
                // byte is read, with the history the guest primed for this
                // start (pb+162/164), and walk up to the span start.
                cursor.nibble = span_start & ~(kAdpcmNibblesPerFrame - 1u);
                cursor.predictor = 0;
                cursor.scale = 0;
                cursor.yn1 = static_cast<s16>(memory_.read16(pb + 162u));
                cursor.yn2 = static_cast<s16>(memory_.read16(pb + 164u));
                cursor.decoded = 0;
                cursor.valid = true;
                skip = span_abs - adpcm_nibble_to_sample(cursor.nibble);
                ++ax_report_.adpcm_cursor_reloads;
            }
            AdpcmDecodeStats stats;
            if (skip != 0u) {
                adpcm_decode_forward(cursor, ring, kRingMask, ctx, skip, stats);
            }
            adpcm_origin = cursor.decoded - back;
            if (adpcm_origin + needed > cursor.decoded) {
                adpcm_decode_forward(cursor, ring, kRingMask, ctx,
                                     adpcm_origin + needed - cursor.decoded, stats);
            }
            ax_report_.adpcm_samples_decoded += stats.samples;
            ax_report_.adpcm_loop_context_reloads += stats.loop_context_reloads;
            if (!main_audible) ax_report_.muted_adpcm_samples_decoded += stats.samples;
            if (audio_audit::enabled()) {
                audio_audit::event(audio_audit::Lane::Guest, "adpcm-cursor",
                    cpu_.game_clock().now(), pb, span_start, cursor.nibble,
                    adpcm_origin, needed, continuous, (skip << 32u) | back);
            }
            adpcm_ring = ring;
            adpcm_available = needed;
        } else if (format == 0u) {
            ++ax_report_.adpcm_voices;
            if (adpcm_sample_units) ++ax_report_.adpcm_sample_unit_voices;
            const u64 last =
                (static_cast<u64>(kSamples - 1u) * ratio + ax_span_frac_[voice]) >> 16;
            // Start at the containing frame so the header byte is available,
            // and carry the predictor history only if it belongs to this
            // position; otherwise take what the guest primed.
            adpcm_frame_start = ax_span_start_[voice] & ~15u;
            const u32 frame_start = adpcm_frame_start;
            s32 yn1 = ax_adpcm_yn1_[voice];
            s32 yn2 = ax_adpcm_yn2_[voice];
            if (ax_adpcm_position_[voice] != frame_start) {
                yn1 = static_cast<s16>(memory_.read16(pb + 162u));
                yn2 = static_cast<s16>(memory_.read16(pb + 164u));
            }
            s16 coefficients[16];
            for (u32 c = 0; c < 16u; ++c) {
                coefficients[c] = static_cast<s16>(memory_.read16(pb + 126u + c * 2u));
            }
            // Enough decoded samples to cover the furthest position the
            // resampler will ask for, plus a frame of slack. In sample units
            // that is the span start's own offset into the frame plus the
            // resampler's reach; in nibble units (legacy) the reach is
            // mis-measured in nibbles, which is the D07-F2 defect itself.
            hw::AudioFrameIndex64 wanted = 0;
            if (adpcm_sample_units) {
                adpcm_k0 = sample_index_of(ax_span_start_[voice] - frame_start);
                u64 reach = adpcm_k0 + last + 1u + 14u;
                // Bound the run: a garbage ratio must not become a huge
                // allocation on this per-frame path (samples past it are 0).
                constexpr u64 kMaxRun = 4096u;
                if (reach > kMaxRun) {
                    reach = kMaxRun;
                    ++ax_report_.adpcm_span_clamped;
                }
                wanted = reach;
            } else {
                const u64 furthest = last + (ax_span_start_[voice] - frame_start);
                wanted = sample_index_of(furthest) + 14u;
            }
            decoded.reserve(static_cast<std::size_t>(wanted));
            u32 nibble = frame_start;
            u32 scale = 0;
            u32 predictor = 0;
            while (decoded.size() < wanted) {
                if (nibble > end_address) {
                    if (loop_flag != 0u) {
                        nibble = loop_address;
                    } else {
                        decoded.push_back(0);
                        continue;
                    }
                }
                const u32 offset = nibble & 15u;
                if (offset < 2u) {
                    // A 16-nibble frame opens with one header byte: the top
                    // nibble selects the coefficient pair, the bottom one the
                    // shift.
                    const u8 header = samples[(nibble / 2u) & sample_mask];
                    predictor = (header >> 4) & 7u;
                    scale = header & 0xfu;
                    nibble += 2u - offset;
                    continue;
                }
                const u8 byte = samples[(nibble / 2u) & sample_mask];
                const s32 raw = ((nibble & 1u) == 0u) ? (byte >> 4) : (byte & 0xfu);
                const s32 nibble_value = (raw ^ 8) - 8;  // sign-extend 4 bits
                const s32 predicted = coefficients[predictor * 2u] * yn1 +
                                      coefficients[predictor * 2u + 1u] * yn2;
                s32 value = ((nibble_value * (1 << scale)) * 2048 + 1024 + predicted) >> 11;
                if (value > 32767) value = 32767;
                if (value < -32768) value = -32768;
                yn2 = yn1;
                yn1 = value;
                decoded.push_back(static_cast<s16>(value));
                ++nibble;
            }
            ax_adpcm_yn1_[voice] = yn1;
            ax_adpcm_yn2_[voice] = yn2;
            ax_adpcm_position_[voice] = nibble & ~15u;
        }

        // DSP 0x0310..0x031D gates SRC only on running==1, then executes
        // SRC before reading the envelope or channel gains. Muting the main
        // output does not retire the source decoder or its predictor history.
        // PCM has no host decode history; the guest position was already
        // advanced. A voice muted on the main buses is still handed to the
        // mixer: it may send to the aux buses, and from step 3 the mixer's
        // SRC history advances for every running voice (its main contribution
        // is zero either way).
        if (!main_audible && format == 0u) ++ax_report_.muted_adpcm_voices;

        const u32 src_type = memory_.read16(pb + 8u);
        const u32 start_frac = ax_span_frac_[voice];
        const auto addressed_at = [&](u64 step) -> s32 {
            u64 position = static_cast<u64>(ax_span_start_[voice]) + step;
            if (position > end_address) {
                if (loop_flag != 0u && span != 0u) {
                    position = loop_address + ((position - loop_address) % span);
                } else {
                    return 0;
                }
            }
            if (format == 0u) {
                const hw::AudioFrameIndex64 at =
                    position >= adpcm_frame_start
                        ? sample_index_of(position - adpcm_frame_start)
                        : decoded.size();
                return at < decoded.size() ? decoded[static_cast<std::size_t>(at)] : 0;
            }
            if (format == 10u) {
                const hw::AudioByteOffset64 byte_index = position * 2u;
                const u32 high = samples[static_cast<u32>(byte_index & sample_mask)];
                const u32 low = samples[static_cast<u32>((byte_index + 1u) & sample_mask)];
                return static_cast<s16>((high << 8) | low);
            }
            return static_cast<s8>(samples[static_cast<u32>(position & sample_mask)]) * 256;
        };

        // The voice's source samples for this frame, as values: window[k] is the k-th source sample from the
        // span start. The three decode arms differ only in how they produce that sequence.
        s16* const window = ax_source_windows_->voice[submitted].data();
        constexpr u64 kWindow = kAxSourceWindowSamples;
        // A clamp is reported on stderr (first 8 voice-frames) so that a run proves its absence by the line's absence.
        const auto clamp_window = [&](u64& reach) {
            if (reach <= kWindow) return;
            if (ax_report_.source_window_clamped < 8u) {
                std::fprintf(stderr, "[ax-mix] source window clamped: pb 0x%08X format %u ratio 0x%08X reach %llu\n",
                             pb, format, ratio, static_cast<unsigned long long>(reach));
            }
            reach = kWindow;
            ++ax_report_.source_window_clamped;
        };
        u64 count = 0;
        if (format == 0u && adpcm_sample_units) {
            if (adpcm_cursor) {
                count = adpcm_available;  // <= the decode ring (1024) <= the window
                for (u64 k = 0; k < count; ++k) {
                    window[k] = adpcm_ring[static_cast<std::size_t>(
                        (adpcm_origin + k) & (kAxAdpcmRingSamples - 1u))];
                }
            } else {
                count = decoded.size() > adpcm_k0 ? decoded.size() - adpcm_k0 : 0u;
                clamp_window(count);
                for (u64 k = 0; k < count; ++k) {
                    window[k] = decoded[static_cast<std::size_t>(adpcm_k0 + k)];
                }
            }
        } else {
            // Everything the frame can reach: up to the sample at the frame's end position (frac + 160 * ratio); the
            // microcode's SRC and its window update read no further.
            count = ((static_cast<u64>(kSamples) * ratio + start_frac) >> 16) + 1u;
            clamp_window(count);
            for (u64 k = 0; k < count; ++k) {
                window[k] = static_cast<s16>(addressed_at(k));
            }
        }

        // The voice's parameters as values (PB layout: 05_DSP_MICROCODE_SEMANTICS.md section 6).
        shadow::audio::VoiceFrame& frame = voice_frames[submitted++];
        frame.slot = voice;
        frame.restart = ax_voice_restarted_[voice];
        ax_voice_restarted_[voice] = false;  // consumed by this frame's mix
        for (u32 k = 0; k < 4u; ++k) {
            frame.start_history[k] = static_cast<s16>(memory_.read16(pb + 172u + k * 2u));  // words 86..89
        }
        frame.source.samples = window;
        frame.source.count = static_cast<u32>(count);
        frame.ratio = ratio;
        frame.frac = start_frac;
        frame.src = src_type == 2u   ? shadow::audio::SrcMode::None
                    : src_type == 0u ? shadow::audio::SrcMode::Polyphase
                                     : shadow::audio::SrcMode::Linear;
        frame.envelope = static_cast<u16>(envelope);
        frame.envelope_delta = memory_.read16(pb + 102u);  // word 51
        frame.mixer_control = memory_.read16(pb + 12u);    // word 6
        // Words 9..26: {volume, delta} for main L, main R, aux A L, aux A R, aux B L, aux B R, aux B S, main S, aux A S.
        constexpr shadow::audio::Bus kPbBusOrder[9] = {
            shadow::audio::kMainL, shadow::audio::kMainR, shadow::audio::kAuxAL, shadow::audio::kAuxAR,
            shadow::audio::kAuxBL, shadow::audio::kAuxBR, shadow::audio::kAuxBS, shadow::audio::kMainS,
            shadow::audio::kAuxAS};
        for (u32 b = 0; b < 9u; ++b) {
            frame.volume[kPbBusOrder[b]].value = memory_.read16(pb + 18u + b * 4u);
            frame.volume[kPbBusOrder[b]].delta = memory_.read16(pb + 20u + b * 4u);
        }
    }

    shadow::audio::StereoFrame mixed;
    shadow::audio::MixStats stats;
    audio_mixer_->mix(settings, voice_frames.data(), submitted, mixed, stats);
    // Volume / envelope ramps are written back into the PB by the DSP every ms (+32*delta); this boundary does not
    // write them back (no route uses them: AU_REPORT.md section 2). Their first occurrence is reported on stderr so a
    // run proves their absence by the line's absence.
    if (stats.ramped_voices != 0u && ax_report_.ramped_voices == 0u) {
        std::fprintf(stderr, "[ax-mix] volume ramp requested (mixer control ramp bit): ramps are per frame only\n");
    }
    if (stats.enveloped_voices != 0u && ax_report_.enveloped_voices == 0u) {
        std::fprintf(stderr, "[ax-mix] envelope delta requested: the envelope is not written back to the PB\n");
    }
    ax_report_.ramped_voices += stats.ramped_voices;
    ax_report_.enveloped_voices += stats.enveloped_voices;
    if (stats.compressor_triggered) {
        if (ax_report_.compressor_triggers == 0u) {
            std::fprintf(stderr, "[ax-mix] compressor first triggered (list 0x%08X)\n", command_list);
        }
        ++ax_report_.compressor_triggers;
    }
    if (stats.compressor_row >= 0) ++ax_report_.compressor_frames;
    if (settings.mode == shadow::audio::OutputMode::DolbyMatrix) {
        if (ax_report_.dolby_frames == 0u) {
            std::fprintf(stderr, "[ax-mix] Dolby matrix output program (AXSetMode 2) from list 0x%08X\n", command_list);
        }
        ++ax_report_.dolby_frames;
    }
    ax_report_.mixed_voices += stats.voices_mixed;
    if (stats.peak > ax_report_.peak) ax_report_.peak = stats.peak;
    // The guest's audio interface buffer: 160 frames of {first, second} big-endian s16, first = the main bus mixed
    // with pb+18 (the order this runtime's AI consumer has always read).
    for (u32 index = 0; index < kSamples; ++index) {
        memory_.write16(output + (index * 2u + 0u) * 2u, static_cast<u16>(mixed.left[index]));
        memory_.write16(output + (index * 2u + 1u) * 2u, static_cast<u16>(mixed.right[index]));
    }
    ++ax_report_.mixed_frames;
    audio_audit::event(audio_audit::Lane::Guest, "mix-done", cpu_.game_clock().now(),
        output, ax_report_.mixed_frames);
}

void System::note_movie_event(Cpu& cpu, u32 site, u32 a, u32 b, u32 c, u32 d) {
    // Bounded: a movie runs for thousands of frames and the interesting part is
    // always the start of one, plus the transition out of it.
    constexpr std::size_t kMaxEvents = 20000;
    if (movie_events_.size() >= kMaxEvents) {
        return;
    }
    MovieTraceEvent event;
    event.game_ticks = cpu.game_clock().now();
    event.vi_frame = static_cast<u32>(video_->frames());
    event.site = site;
    event.a = a;
    event.b = b;
    event.c = c;
    event.d = d;
    movie_events_.push_back(event);
}

std::string System::movie_trace_report() const {
    if (!movie_trace_enabled_) {
        return {};
    }
    std::ostringstream out;
    out << "Sofdec movie pipeline (observation only):\n";
    out << "  movie objects created (fn_80036d68)   " << movie_counts_.open << "\n";
    out << "  per-frame ticks       (fn_80036b0c)   " << movie_counts_.tick << "\n";
    out << "  CRI get-frame calls   (0x80403e3c)    " << movie_counts_.get_frame << "\n";
    out << "  FRAMES PRODUCED       (fn_800367a8)   " << movie_counts_.frame_ready << "\n";
    out << "  picture conversions   (0x804096d0)    " << movie_counts_.convert << "\n";
    out << "  CRI release-frame     (0x80403a20)    " << movie_counts_.release << "\n";
    out << "  Im2D blits            (fn_80036570)   " << movie_counts_.blit << " ("
        << movie_counts_.blit_skipped << " returned early on a null latch)\n";
    out << "  mwPlyGetStat polls    (0x804086d8)    " << movie_counts_.get_stat << "\n";
    out << "  nine-module polls     (0x803fede8)    " << movie_counts_.module_poll << "\n";
    out << "  demux module ticks    (fn_803f449c)   " << movie_counts_.demux_tick << "\n";
    out << "  VIDEO module ticks    (fn_803f9398)   " << movie_counts_.video_tick << "\n";
    out << "  picture-queue reads   (fn_803f9c80)   " << movie_counts_.queue_head << "\n";
    out << "  movie SERVICE         (fn_80036e88)   " << movie_counts_.service << "\n";
    out << "  mwPlyCreateSofdec     (fn_8040534c)   " << movie_counts_.cri_service << "\n";
    out << "  ADXGC init            (0x803c5150)   " << movie_counts_.adxgc_init << "\n";
    out << "  OSResumeThread        (0x80378ecc)   " << movie_counts_.os_resume_thread << "\n";
    out << "  CRI prio-24 DECODER   (0x803c5330)   " << movie_counts_.cri_thread_prio24 << "\n";
    out << "  SVM_ExecSvr idle      (0x803cc24c)   " << movie_counts_.svm_exec_idle << "\n";
    out << "  MWSFSVR_IdleThrdProc  (0x80408c44)   " << movie_counts_.idle_thrd_proc << "\n";
    out << "  mwPlyExecSvrHndl      (0x80408a68)   " << movie_counts_.exec_svr_hndl << "\n";
    out << "  mwlSfdExecSvr         (0x80408b30)   " << movie_counts_.sfd_exec_svr << "\n";
    out << "  mwlSfdExecDecSvr      (0x80408d80)   " << movie_counts_.sfd_exec_dec << "\n";
    out << "  SFD_ExecServer        (0x803fc394)   " << movie_counts_.sfd_exec_server << "\n";
    out << "  method-2 chain depth  module0=" << movie_counts_.m2_module0
        << " module1=" << movie_counts_.m2_module1 << " module2(MPEG)="
        << movie_counts_.m2_module2 << "\n";
    out << "  SVM_ExecSvr vsync     (0x803cc3fc)   " << movie_counts_.svm_exec_vsync << "\n";
    out << "  MWSFSVR_VsyncThrdProc (0x80408cec)   " << movie_counts_.vsync_thrd_proc << "\n";
    out << "  chain pumps           fn_803fa858(m4)=" << movie_counts_.pump_a858
        << " fn_803fadac(m4)=" << movie_counts_.pump_adac
        << " fn_803fb014(m3)=" << movie_counts_.pump_b014
        << " fn_803fc0e0(m2)=" << movie_counts_.pump_c0e0 << "\n";
    out << "  poll methods pumped  ";
    for (std::size_t method = 0; method < movie_counts_.poll_method.size(); ++method) {
        if (movie_counts_.poll_method[method] != 0) {
            out << " [" << method << "]=" << movie_counts_.poll_method[method];
        }
    }
    out << "\n";
    out << "  playend stub          " << (sofdec_playend_stub_ ? "ON" : "OFF") << "\n";
    return out.str();
}

void System::run() {
    note_guest_running(true);
    cpu_.run();
    note_guest_running(false);
}

namespace {
s64 steady_ns() noexcept {
    return static_cast<s64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}
}  // namespace

void System::note_guest_running(bool running) noexcept {
    if (watchdog_wall_ms_ == 0u) return;
    guest_running_since_ns_.store(running ? steady_ns() : 0, std::memory_order_relaxed);
}

void System::set_wait_watchdog(u32 wall_ms) {
    if (watchdog_.joinable()) {
        watchdog_quit_.store(true);
        watchdog_.join();
        watchdog_quit_.store(false);
    }
    watchdog_wall_ms_ = wall_ms;
    guest_running_since_ns_.store(0, std::memory_order_relaxed);
    if (wall_ms == 0u) return;
    watchdog_ = std::thread([this, wall_ms] {
        name_this_thread("wait-watchdog");  // ES-T1
        const s64 limit = static_cast<s64>(wall_ms) * 1000000;
        while (!watchdog_quit_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const s64 since = guest_running_since_ns_.load(std::memory_order_relaxed);
            if (since == 0) continue;
            const s64 ran = steady_ns() - since;
            if (ran < limit) continue;
            char detail[224];
            std::snprintf(detail, sizeof(detail),
                          "wait-point watchdog: the guest executed for %lld ms of wall time without "
                          "reaching a game-clock wait point (limit %u ms)",
                          static_cast<long long>(ran / 1000000), wall_ms);
            cpu_.request_stop_from_host(StopReason::ExecutionFault, detail);
            // One request per stretch of guest execution.
            while (!watchdog_quit_.load() &&
                   guest_running_since_ns_.load(std::memory_order_relaxed) == since) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    });
}

// ---------------------------------------------------------------------------
// Game time (docs/native-time-20261005/DESIGN.md)
// ---------------------------------------------------------------------------

void System::move_clock_to(u64 when) {
    GameClock& clock = cpu_.game_clock();
    const u64 now = clock.now();
    if (when <= now) return;
    clock.set_now(when);
    card_->tick((when - now) * kDeviceCyclesPerTick);
}

void System::schedule_devices() {
    const u64 cycles = card_->cycles_until_persist();
    GameClock& clock = cpu_.game_clock();
    if (cycles == ~0ull) {
        clock.cancel(GameEvent::Devices);
        return;
    }
    clock.schedule(GameEvent::Devices,
                   clock.now() + (cycles + kDeviceCyclesPerTick - 1u) / kDeviceCyclesPerTick);
}

void System::deliver_event(GameEvent event) {
    GameClock& clock = cpu_.game_clock();
    const u64 now = clock.now();
    switch (event) {
        case GameEvent::Frame: {
            clock.schedule(GameEvent::Frame, next_frame_event(now));
            if (now % kHostServiceTicks != 0u) {
                // A pacing tick between two host services: real-time pacing only.
                GEKKO_PROFILE_ZONE("cpu/host-pacing");
                if (host_pacing_) host_pacing_(now);
                break;
            }
            ++host_services_;
            {
                GEKKO_PROFILE_ZONE("cpu/host-service");
                if (host_service_) host_service_(now);
            }
#if defined(SHADOW_TRACY)
            publish_process_resources();
            if (GEKKO_PROFILE_CONNECTED()) {
                TracyMachineSeries& tracy = g_tracy_series;
                const u32 frame = static_cast<u32>(video_->frames());
                const auto wall = std::chrono::steady_clock::now();
                const u64 divide_calls = unsigned_divide_intrinsic_calls_ + signed_divide_intrinsic_calls_;
                const auto gameplay_allocations = [] {
                    return shadow::memory::system()
                        .snapshot()
                        .domains[static_cast<std::size_t>(shadow::memory::Domain::Gameplay)];
                };
                if (!tracy.anchored) {
                    GEKKO_PROFILE_PLOT_CONFIG("cpu/gameplay-bytes", GEKKO_PLOT_MEMORY, true, true, 0);
                    GEKKO_PROFILE_PLOT_CONFIG("cpu/gameplay-allocs", GEKKO_PLOT_NUMBER, true, true, 0);
                    GEKKO_PROFILE_PLOT_CONFIG("cpu/gameplay-alloc-churn", GEKKO_PLOT_NUMBER, true, true, 0);
                    tracy.anchored = true;
                    tracy.last_frame = frame;
                    tracy.last_publish = wall;
                    tracy.last_ticks = now;
                    tracy.last_interpreted = cpu_.interpreted_instructions();
                    tracy.last_divide_calls = divide_calls;
                    tracy.last_divide_declines = divide_intrinsic_declines_;
                    tracy.last_allocations = gameplay_allocations().allocations;
                } else if (frame != tracy.last_frame) {
                    const double wall_s =
                        std::chrono::duration<double>(wall - tracy.last_publish).count();
                    const double game_s = static_cast<double>(now - tracy.last_ticks) /
                                          static_cast<double>(kGameTicksPerSecond);
                    // Game seconds per wall second over the frame: 1.0 is real
                    // time; below it the host is the bottleneck.
                    if (wall_s > 0.0) GEKKO_PROFILE_PLOT("cpu/game-speed", game_s / wall_s);
                    GEKKO_PROFILE_PLOT("cpu/interpreted-instr",
                                       static_cast<double>(cpu_.interpreted_instructions() -
                                                           tracy.last_interpreted));
                    const auto gameplay = gameplay_allocations();
                    GEKKO_PROFILE_PLOT("cpu/gameplay-bytes", static_cast<double>(gameplay.active));
                    GEKKO_PROFILE_PLOT("cpu/gameplay-allocs", static_cast<double>(gameplay.live_allocations));
                    GEKKO_PROFILE_PLOT("cpu/gameplay-alloc-churn",
                                       static_cast<double>(gameplay.allocations - tracy.last_allocations));
                    GEKKO_PROFILE_PLOT("cpu/divide-intrinsics",
                                       static_cast<double>(divide_calls - tracy.last_divide_calls));
                    GEKKO_PROFILE_PLOT("cpu/divide-declines",
                                       static_cast<double>(divide_intrinsic_declines_ - tracy.last_divide_declines));
                    GEKKO_PROFILE_FRAME("cpu/vi-frame");
                    tracy.last_frame = frame;
                    tracy.last_publish = wall;
                    tracy.last_ticks = now;
                    tracy.last_interpreted = cpu_.interpreted_instructions();
                    tracy.last_divide_calls = divide_calls;
                    tracy.last_divide_declines = divide_intrinsic_declines_;
                    tracy.last_allocations = gameplay.allocations;
                }
            }
#endif
            break;
        }
        case GameEvent::Retrace:
            video_->on_retrace(now);
            break;
        case GameEvent::Audio:
            audio_->on_pass_end(now);
            break;
        case GameEvent::Decrementer:
            cpu_.latch_decrementer();
            break;
        case GameEvent::Devices:
            // move_clock_to aged the card's debounce to now and ran a due
            // persist; nothing for the guest.
            clock.cancel(GameEvent::Devices);
            break;
        case GameEvent::Count:
            break;
    }
}

bool System::advance_game_time(u64 target) {
    // The watchdog measures guest execution between wait points: this one
    // starts now and the guest runs again when it returns.
    struct WaitScope {
        System& system;
        explicit WaitScope(System& s) : system(s) { system.note_guest_running(false); }
        ~WaitScope() { system.note_guest_running(true); }
    } wait_scope(*this);
    cpu_.end_poll_tracking();
    GameClock& clock = cpu_.game_clock();
    for (;;) {
        if (cpu_.stopped() || cpu_.stop_requested()) return false;
        // A deliverable interrupt is the guest's to take before time moves on.
        if (cpu_.interrupt_pending()) return false;
        schedule_devices();
        GameEvent event = GameEvent::Count;
        const u64 when = clock.next(event);
        if (when > target) {
            if (target == kNoDeadline) {
                cpu_.request_stop(StopReason::ExecutionFault,
                                  "the game is waiting for time to pass and nothing is scheduled");
                return false;
            }
            move_clock_to(target);
            return true;
        }
        move_clock_to(when);
        deliver_event(event);
    }
}

void System::idle_wait() {
    // A wait point (Cpu::idle_wait): a polling loop that only an event can
    // end. One event at a time, not "until an interrupt": the loop may run
    // with interrupts masked, polling device state an event changes, and must
    // re-read it after every event. A deliverable interrupt still returns at
    // once, before the clock moves (advance_game_time).
    ++idle_waits_;
    (void)advance_to_next_event();
}

bool System::advance_to_next_event() {
    for (;;) {
        schedule_devices();
        GameEvent event = GameEvent::Count;
        const u64 when = cpu_.game_clock().next(event);
        if (when == kNoDeadline) {
            cpu_.request_stop(StopReason::ExecutionFault,
                              "the game is waiting for an event and nothing is scheduled");
            return false;
        }
        // A pacing-only tick (GameEvent::Frame off the host-service grid, docs/FRAME_PACING.md "Pacing on the frame
        // grid") is host work, not an event of the game's: the wait delivers it (the pacer sleeps on time) and goes
        // on to the next real one, so every wait still ends at the event it ended at without pacing ticks - a
        // polling loop runs exactly as many iterations. A real event due on the same tick (a retrace on a frame
        // boundary) ends the wait there, as it did.
        if (event == GameEvent::Frame && when % kHostServiceTicks != 0u) {
            bool shared = false;
            const auto& deadlines = cpu_.game_clock().deadlines();
            for (std::size_t i = 0; i < deadlines.size(); ++i)
                shared = shared || (i != static_cast<std::size_t>(GameEvent::Frame) && deadlines[i] == when);
            if (!shared) {
                if (!advance_game_time(when)) return false;
                continue;
            }
        }
        return advance_game_time(when);
    }
}

void System::add_symbol(u32 address, std::string name) {
    symbols_.emplace(address, std::move(name));
}

std::string System::describe_address(u32 address) const {
    if (symbols_.empty()) {
        return hex32(address);
    }
    auto it = symbols_.upper_bound(address);
    if (it == symbols_.begin()) {
        return hex32(address);
    }
    --it;
    const u32 delta = address - it->first;
    if (delta > 0x8000u) {
        return hex32(address);
    }
    std::ostringstream out;
    out << hex32(address) << " (" << it->second;
    if (delta != 0) {
        out << "+0x" << std::hex << delta;
    }
    out << ")";
    return out.str();
}

std::string System::stop_report() const {
    std::ostringstream out;
    const CpuStop& stop = cpu_.stop();
    out << "Stop reason: ";
    switch (stop.reason) {
        case StopReason::Running: out << "still running"; break;
        case StopReason::HostRequest: out << "host request"; break;
        case StopReason::UnimplementedInstruction: out << "unimplemented instruction"; break;
        case StopReason::TranslationFault: out << "address translation fault"; break;
        case StopReason::ExecutionFault: out << "execution fault"; break;
        case StopReason::Breakpoint: out << "breakpoint"; break;
        case StopReason::HleExit: out << "guest requested exit"; break;
    }
    out << "\n";
    if (!stop.detail.empty()) {
        out << "Detail: " << stop.detail << "\n";
    }
    out << "PC: " << describe_address(cpu_.pc) << "  LR: " << describe_address(cpu_.lr) << "\n";
    {
        char time_line[96];
        std::snprintf(time_line, sizeof(time_line), "Game time: %.6f s (%llu ticks)\n",
                      static_cast<double>(cpu_.game_clock().now()) /
                          static_cast<double>(kGameTicksPerSecond),
                      static_cast<unsigned long long>(cpu_.game_clock().now()));
        out << time_line;
    }

    char reg[160];
    for (u32 row = 0; row < 8; ++row) {
        std::snprintf(reg, sizeof(reg), "  r%-2u %08X  r%-2u %08X  r%-2u %08X  r%-2u %08X",
                      row, cpu_.gpr[row], row + 8, cpu_.gpr[row + 8], row + 16,
                      cpu_.gpr[row + 16], row + 24, cpu_.gpr[row + 24]);
        out << reg << "\n";
    }
    std::snprintf(reg, sizeof(reg),
                  "  CR %08X  XER %08X  CTR %08X  MSR %08X  DEC %08X  L2CR %08X  HID0 %08X",
                  cpu_.cr, cpu_.xer, cpu_.ctr, cpu_.msr(), cpu_.decrementer(), cpu_.spr[SPR_L2CR],
                  cpu_.spr[SPR_HID0]);
    out << reg << "\n";

    const auto history = cpu_.history();
    const std::size_t shown = std::min<std::size_t>(history.size(), 16);
    if (shown != 0) {
        out << "Last " << shown << " instructions:\n";
        for (std::size_t index = history.size() - shown; index < history.size(); ++index) {
            char line[128];
            std::snprintf(line, sizeof(line), "  %08X: %08X", history[index].address,
                          history[index].instruction);
            out << line << "  " << describe_address(history[index].address) << "\n";
        }
    }

    const auto& diagnostics = memory_.diagnostics();
    if (diagnostics.unmapped_reads != 0 || diagnostics.unmapped_writes != 0) {
        out << "Unmapped bus accesses: " << diagnostics.unmapped_reads << " reads, "
            << diagnostics.unmapped_writes << " writes\n";
        for (const auto& message : diagnostics.messages) {
            out << "  " << message << "\n";
        }
    }
    return out.str();
}

std::string System::progress_report() const {
    std::ostringstream out;
    char line[384];
    {
        const NativeVideo::Stats& video = video_->stats();
        std::snprintf(line, sizeof(line),
                      "  Video: %s, %llu fields (%llu half-lines, %llu ticks each%s), %llu retraces "
                      "delivered, %llu commits (%llu held for the first field)",
                      video_->enabled() ? "running" : "off",
                      static_cast<unsigned long long>(video.fields),
                      static_cast<unsigned long long>(video_->half_lines_per_field()),
                      static_cast<unsigned long long>(video_->field_ticks()),
                      video_->progressive_clock() ? ", 54 MHz" : "",
                      static_cast<unsigned long long>(video.deliveries),
                      static_cast<unsigned long long>(video.commits),
                      static_cast<unsigned long long>(video.gated_commits));
        out << line << "\n";
    }
    {
        const NativeReset::Stats& reset = reset_controls_->stats();
        std::snprintf(line, sizeof(line),
                      "  Reset controls: %llu reset-button polls (no reset button), %llu reset-code reads "
                      "(cold boot); unserved guest accesses stopped: %llu",
                      static_cast<unsigned long long>(reset.reset_button_polls),
                      static_cast<unsigned long long>(reset.reset_code_reads),
                      static_cast<unsigned long long>(cpu_.unmapped_accesses_stopped()));
        out << line << "\n";
    }
    {
        // docs/memory-architecture-20261006: the game's address window.
        std::snprintf(line, sizeof(line),
                      "  Game memory: %zu regions, %llu bytes committed of %llu reserved; %llu OS exception "
                      "entries (native, no real mode); %llu BAT configurations (nothing to configure)",
                      memory_.regions().size(), static_cast<unsigned long long>(memory_.committed_bytes()),
                      static_cast<unsigned long long>(memory_.reserved_bytes()),
                      static_cast<unsigned long long>(cpu_diag().os_exception_entries),
                      static_cast<unsigned long long>(game_memory_->stats().bat_configurations));
        out << line << "\n";
    }

    {
        const auto& peripheral = lifted_.peripheral.stats();
        std::snprintf(line, sizeof(line),
                      "  Peripheral (lifted): %llu frames read, connected 0x%X, %llu rumble changes, "
                      "%llu lowered sites run by the dispatcher or interpreter",
                      static_cast<unsigned long long>(peripheral.frames_read),
                      lifted_.peripheral.connected_mask(),
                      static_cast<unsigned long long>(peripheral.rumble_changes),
                      static_cast<unsigned long long>(cpu_.lift_sites_dispatched()));
        out << line << "\n";
    }
    {
        const NativeGxSync::Stats& sync = gx_sync_->stats();
        std::snprintf(line, sizeof(line),
                      "  GX sync: %llu tokens (%llu with an interrupt request), %llu draw-done "
                      "(%llu delivered), %llu breakpoints (%llu delivered, %llu callbacks, %llu disabled), "
                      "%llu FIFO binds, %llu FIFO reads, %llu draw-sync reads, %llu shutdown calls, "
                      "%llu performance selects",
                      static_cast<unsigned long long>(sync.tokens),
                      static_cast<unsigned long long>(sync.token_interrupts),
                      static_cast<unsigned long long>(sync.draw_dones),
                      static_cast<unsigned long long>(sync.finish_deliveries),
                      static_cast<unsigned long long>(sync.breakpoints),
                      static_cast<unsigned long long>(sync.breakpoint_deliveries),
                      static_cast<unsigned long long>(sync.breakpoint_callbacks),
                      static_cast<unsigned long long>(sync.breakpoints_disabled),
                      static_cast<unsigned long long>(sync.fifo_binds),
                      static_cast<unsigned long long>(sync.fifo_reads),
                      static_cast<unsigned long long>(sync.draw_sync_reads),
                      static_cast<unsigned long long>(sync.shutdown_calls),
                      static_cast<unsigned long long>(sync.platform_writes));
        out << line << "\n";
    }
    {
        const NativeCard::Stats& card = card_->stats();
        std::snprintf(line, sizeof(line),
                      "  Memory card A: %s, %llu probes, %llu mounts (%llu failed), %llu unmounts; "
                      "%llu reads (%llu bytes), %llu writes (%llu bytes, %llu over unerased bytes), "
                      "%llu erases; %llu completions (%llu callbacks, %llu after a detach)",
                      card_->present() ? "inserted" : "empty", static_cast<unsigned long long>(card.probes),
                      static_cast<unsigned long long>(card.mounts),
                      static_cast<unsigned long long>(card.mount_failures),
                      static_cast<unsigned long long>(card.unmounts), static_cast<unsigned long long>(card.reads),
                      static_cast<unsigned long long>(card.read_bytes), static_cast<unsigned long long>(card.writes),
                      static_cast<unsigned long long>(card.written_bytes),
                      static_cast<unsigned long long>(card.overwritten_bytes),
                      static_cast<unsigned long long>(card.erases),
                      static_cast<unsigned long long>(card.completions),
                      static_cast<unsigned long long>(card.callbacks),
                      static_cast<unsigned long long>(card.dropped_detached));
        out << line << "\n";
        const NativeIpl::Stats& ipl = ipl_->stats();
        std::snprintf(line, sizeof(line),
                      "  IPL services: SRAM read %llu, %llu unlocks (%llu commits: %llu persisted, %llu "
                      "unchanged, %llu failed); debug UART %llu lines (%llu bytes, %llu while disabled)",
                      static_cast<unsigned long long>(ipl.sram_reads),
                      static_cast<unsigned long long>(ipl.sram_unlocks),
                      static_cast<unsigned long long>(ipl.sram_commits),
                      static_cast<unsigned long long>(ipl.sram_persists),
                      static_cast<unsigned long long>(ipl.sram_unchanged),
                      static_cast<unsigned long long>(ipl.sram_failures),
                      static_cast<unsigned long long>(ipl.uart_lines),
                      static_cast<unsigned long long>(ipl.uart_bytes),
                      static_cast<unsigned long long>(ipl.uart_disabled));
        out << line << "\n";
    }
    {
#if !defined(SHADOW_RENDER_LIFTED)  // G5
        const NativeImmediate::Stats& immediate = immediate_->stats();
        std::snprintf(line, sizeof(line),
                      "  GX immediate: %llu primitives (%llu vertices, %llu payload bytes, %llu empty), %s; "
                      "unlisted pipe stores %llu",
                      static_cast<unsigned long long>(immediate.primitives),
                      static_cast<unsigned long long>(immediate.vertices),
                      static_cast<unsigned long long>(immediate.payload_bytes),
                      static_cast<unsigned long long>(immediate.empty),
                      immediate_->idle() ? "none open" : "one open",
                      static_cast<unsigned long long>(cpu_.unlisted_gx_stores()));
#else
        std::snprintf(line, sizeof(line),
                      "  GX immediate: none (the lifted build has no GX immediate path); unlisted pipe stores %llu",
                      static_cast<unsigned long long>(cpu_.unlisted_gx_stores()));
#endif
        out << line << "\n";
    }
    std::snprintf(line, sizeof(line),
                  "  Native compiler intrinsics: %llu unsigned and %llu signed 64-bit divides, "
                  "%llu declined to the recovered body",
                  static_cast<unsigned long long>(unsigned_divide_intrinsic_calls_),
                  static_cast<unsigned long long>(signed_divide_intrinsic_calls_),
                  static_cast<unsigned long long>(divide_intrinsic_declines_));
    out << line << "\n";
    std::snprintf(line, sizeof(line),
                  "  Native __dynamic_cast: %llu calls, %llu RTTI walks (the rest remembered), %llu declined (bad_cast)",
                  static_cast<unsigned long long>(dynamic_cast_.calls()),
                  static_cast<unsigned long long>(dynamic_cast_.computed()),
                  static_cast<unsigned long long>(dynamic_cast_.declines()));
    out << line << "\n";
    std::snprintf(line, sizeof(line),
                  "  Native RwMatrixMultiply: %llu calls (%llu four lanes at a time; ordered: %llu overlapping, %llu off "
                  "the fast path), %llu declined, %llu checked against the guest body, %llu differed",
                  static_cast<unsigned long long>(matrix_multiply_.calls()),
                  static_cast<unsigned long long>(matrix_multiply_.wide()),
                  static_cast<unsigned long long>(matrix_multiply_.overlapping()),
                  static_cast<unsigned long long>(matrix_multiply_.off_fast_path()),
                  static_cast<unsigned long long>(matrix_multiply_.declines()),
                  static_cast<unsigned long long>(matrix_multiply_.verified()),
                  static_cast<unsigned long long>(matrix_multiply_.differed()));
    out << line << "\n";
    std::snprintf(line, sizeof(line),
                  "  Native CharaColli bounding tests: %llu calls, %llu rejected natively; declined: %llu inside the "
                  "sphere, %llu with another filter, %llu another class or state (%llu filters run natively); %llu checked against the guest body (%llu "
                  "on all memory, %llu cut short by an interrupt), %llu differed",
                  static_cast<unsigned long long>(chara_colli_.calls()),
                  static_cast<unsigned long long>(chara_colli_.served()),
                  static_cast<unsigned long long>(chara_colli_.passed()),
                  static_cast<unsigned long long>(chara_colli_.filtered()),
                  static_cast<unsigned long long>(chara_colli_.other()),
                  static_cast<unsigned long long>(chara_colli_.filter_runs()),
                  static_cast<unsigned long long>(chara_colli_.verified()),
                  static_cast<unsigned long long>(chara_colli_.full_checks()),
                  static_cast<unsigned long long>(chara_colli_.incomplete()),
                  static_cast<unsigned long long>(chara_colli_.differed()));
    out << line << "\n";
    std::snprintf(line, sizeof(line),
                  "  Native CharaColli queries: %llu, %llu served to the end; handed back to the guest: %llu hit, "
                  "%llu expired entry, %llu count, %llu cut short; %llu nested guest test calls; %llu checked against the "
                  "guest body, %llu differed",
                  static_cast<unsigned long long>(chara_colli_.queries()),
                  static_cast<unsigned long long>(chara_colli_.queries_finished()),
                  static_cast<unsigned long long>(chara_colli_.handed_back(0)),
                  static_cast<unsigned long long>(chara_colli_.handed_back(1)),
                  static_cast<unsigned long long>(chara_colli_.handed_back(2)),
                  static_cast<unsigned long long>(chara_colli_.handed_back(3)),
                  static_cast<unsigned long long>(chara_colli_.nested_calls()),
                  static_cast<unsigned long long>(chara_colli_.queries_verified()),
                  static_cast<unsigned long long>(chara_colli_.queries_differed()));
    out << line << "\n";
    std::snprintf(line, sizeof(line),
                  "  Native CharaColli vf 0x7C (the manager pass): %llu calls, served %llu bone / %llu body / %llu "
                  "general (%llu re-derived), %llu declined; %llu checked against the guest's, %llu differed",
                  static_cast<unsigned long long>(chara_colli_.manage_calls()),
                  static_cast<unsigned long long>(chara_colli_.manage_served(CharaColliIntrinsic::Manage::Bone)),
                  static_cast<unsigned long long>(chara_colli_.manage_served(CharaColliIntrinsic::Manage::Body)),
                  static_cast<unsigned long long>(chara_colli_.manage_served(CharaColliIntrinsic::Manage::General)),
                  static_cast<unsigned long long>(chara_colli_.manage_rederived()),
                  static_cast<unsigned long long>(chara_colli_.manage_declined()),
                  static_cast<unsigned long long>(chara_colli_.manage_verified()),
                  static_cast<unsigned long long>(chara_colli_.manage_differed()));
    out << line << "\n";
    std::snprintf(line, sizeof(line),
                  "  Native CharaColli pair pass: %llu calls, %llu served to the end; handed back to the guest: %llu "
                  "expired, %llu count, %llu cut short, %llu other getters; %llu distance tests, %llu narrow phases "
                  "(%llu in contact); %llu checked against the guest body, %llu differed",
                  static_cast<unsigned long long>(chara_colli_.pass_calls()),
                  static_cast<unsigned long long>(chara_colli_.pass_finished()),
                  static_cast<unsigned long long>(chara_colli_.pass_back(0)),
                  static_cast<unsigned long long>(chara_colli_.pass_back(1)),
                  static_cast<unsigned long long>(chara_colli_.pass_back(2)),
                  static_cast<unsigned long long>(chara_colli_.pass_back(3)),
                  static_cast<unsigned long long>(chara_colli_.pass_pairs()),
                  static_cast<unsigned long long>(chara_colli_.pass_narrow()),
                  static_cast<unsigned long long>(chara_colli_.pass_contacts()),
                  static_cast<unsigned long long>(chara_colli_.pass_verified()),
                  static_cast<unsigned long long>(chara_colli_.pass_differed()));
    out << line << "\n";
    if (trace_stale_contacts_)
        out << chara_colli_.stale_summary();
    std::snprintf(line, sizeof(line),
                  "  Native CharaColliGeneralBone updates: %llu from a recorded run, %llu run by the guest (%llu recorded), "
                  "declined: %llu dirty, %llu outside the contract",
                  static_cast<unsigned long long>(chara_colli_.bone_hits()),
                  static_cast<unsigned long long>(chara_colli_.bone_misses()),
                  static_cast<unsigned long long>(chara_colli_.bone_recorded()),
                  static_cast<unsigned long long>(chara_colli_.bone_dirty()),
                  static_cast<unsigned long long>(chara_colli_.bone_unkeyed()));
    out << line << "\n";
    std::snprintf(line, sizeof(line),
                  "    computed natively: %llu clean, %llu dirty, %llu outside the native contract (guest); %llu "
                  "checked against the guest update, %llu differed",
                  static_cast<unsigned long long>(chara_colli_.bone_native_clean()),
                  static_cast<unsigned long long>(chara_colli_.bone_native_dirty()),
                  static_cast<unsigned long long>(chara_colli_.bone_native_declined()),
                  static_cast<unsigned long long>(chara_colli_.bone_checked()),
                  static_cast<unsigned long long>(chara_colli_.bone_differed()));
    out << line << "\n";
    std::snprintf(line, sizeof(line), "    changed inputs: %llu compared, %llu differed, %llu outside the native contract",
                  static_cast<unsigned long long>(chara_colli_.bone_fuzzed()),
                  static_cast<unsigned long long>(chara_colli_.bone_fuzz_differed()),
                  static_cast<unsigned long long>(chara_colli_.bone_fuzz_declined()));
    out << line << "\n";
    for (u32 t = 0; t < 4u; ++t) {
        const auto test = static_cast<CharaColliIntrinsic::Test>(t);
        std::snprintf(line, sizeof(line), "    test %u declined as other: %llu MSR/GQR, %llu vtable, %llu counts", t,
                      static_cast<unsigned long long>(chara_colli_.other_by(test, 0)),
                      static_cast<unsigned long long>(chara_colli_.other_by(test, 1)),
                      static_cast<unsigned long long>(chara_colli_.other_by(test, 2)));
        out << line << "\n";
    }
    {
        const NativeAudio::Stats& audio = audio_->stats();
        std::snprintf(line, sizeof(line),
                      "  Audio: %llu passes (%llu silent), %llu AX lists, %llu DSP mails, %llu DSP DMA completions",
                      static_cast<unsigned long long>(audio.passes),
                      static_cast<unsigned long long>(audio.silent_passes),
                      static_cast<unsigned long long>(audio.command_lists),
                      static_cast<unsigned long long>(audio.dsp_mails),
                      static_cast<unsigned long long>(audio.transfer_completions));
        out << line << "\n";
    }
    // The asset system, which is what a read now goes through: no commands,
    // no opcodes and no device.
    const gekko::assets::AssetSystem::Stats reads = assets_.stats();
    std::snprintf(line, sizeof(line),
                  "  Assets: %llu reads, %llu bytes, %llu unmapped, %llu out of range, %llu failed",
                  static_cast<unsigned long long>(reads.reads),
                  static_cast<unsigned long long>(reads.bytes),
                  static_cast<unsigned long long>(reads.unmapped),
                  static_cast<unsigned long long>(reads.out_of_range),
                  static_cast<unsigned long long>(reads.failed));
    out << line << "\n";
    return out.str();
}



}  // namespace gekko
