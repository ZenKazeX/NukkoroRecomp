#pragma once

// GAME OWNERS' VIEW OF RENDERWARE (NE-RW1, HW_REPORT W.2). The game-level owners (runtime/game: natively owned
// functions of the GAME - not of RenderWare and not of the Nintendo SDK) reach RenderWare only through this interface:
// the RenderWare API calls their game code made (render-state get / set, an Im2D primitive), the game's
// world-to-screen projection, and the part of the game's screen the output shows. Today the RenderWare GameCube driver
// scaffold (runtime/scaffold/rw_driver) implements it; the RenderWare lifts (NE-RW2 / NE-RW3) replace that
// implementation, and the owners do not change.

#include <cstdint>

#include "gekko/types.h"
#include "render/frame.h"
#include "render/view.h"

namespace gekko {
class Cpu;
}

namespace gekko::game {

class RwApi {
public:
    virtual ~RwApi() = default;

    // ---- RenderWare's render states (RwRenderStateSet; the five the game's draws save and restore) ------------------
    struct SavedStates {
        u32 z_test = 1, z_write = 1, fog_enable = 0, src_blend = 5, dst_blend = 6;
    };
    virtual SavedStates saved_states() const = 0;
    virtual void state_set(Cpu& c, u32 state, u32 value) = 0;
    // ---- RwIm2DRenderPrimitive of `count` vertices made by the owner, as values (x, y canvas units, z the screen
    // depth, RGBA8 with R in the lowest byte, u, v; NE-RW2); `anchor` places the draw on the canvas -----------------
    virtual void im2d_draw(Cpu& c, u32 type, const shadow::render::ImmediateVertex* vertices, u32 count,
                           shadow::render::Anchor anchor) = 0;
    // ---- the game's screen ------------------------------------------------------------------------------------------
    // A world point on a player's view in the game's screen coordinates, as the game's own projection gives it
    // (false: behind the eye or no camera).
    struct Projected {
        float x = 0.0f, y = 0.0f, depth = 0.0f;
    };
    virtual bool project(Cpu& c, u32 world, u32 player, Projected* out) = 0;
    // A viewport's rectangle of the game's 640 x 480 screen, and the part of it the output shows.
    struct ScreenRect {
        float x = 0.0f, y = 0.0f, w = 640.0f, h = 480.0f;
    };
    virtual ScreenRect viewport(Cpu& c, s32 index) = 0;
    virtual shadow::render::CanvasRect shown_screen(const ScreenRect& r) const = 0;
    // The output's width / height (the window's client area, else the captures').
    virtual float presented_aspect() const = 0;
    // How every view's picture frames the camera (render/view.h: square pixels unless --framing game).
    virtual shadow::render::Framing framing() const noexcept = 0;
    // The frame being built (diagnostics).
    virtual u64 frame_number() const = 0;
};

}  // namespace gekko::game
