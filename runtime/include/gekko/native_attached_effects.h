#pragma once

// Effects attached to an object follow the object as it is THIS frame - a deliberate deviation from the original,
// which places them one frame of motion behind (docs/RENDER_FLICKER.md "Fourth bug").
//
// The original (measured on Final Haunt's wall boxes, docs/RENDER_FLICKER.md):
//  * every object's update starts with the shared base step 0x800E5280, which runs the attach step 0x80167A00 over
//    the object's attached effects (+0x40): for each entry (stride 0x4C) it takes RwFrameGetLTM(entry +0x10), copies
//    it transposed into entry +0x14 .. +0x40 and hands that to the effect's SetMatrix 0x80426EB8 (copy into the
//    effect's base matrix, +0x84);
//  * only AFTER that does the object move: its own code places its model (0x801637C0 -> RwFrameTransform, 175 call
//    sites), so the attach step read last frame's transform;
//  * the effect's update 0x80426C84 runs later in the frame and builds its emitter from +0x84, and its model is
//    drawn there. A glow shell attached to a moving box is thus drawn 0.4-0.5 units behind the box along its motion;
//    the faces they share tie in depth by rounding and the glow flickers in and out.
//
// The fix, owned here: SetMatrix (0x80426EB8). Called by the attach step, it does what the original did and also
// points the effect's own follow-matrix field (+0x48, which the effect's update already honours: 0x80426CA4 calls
// SetMatrix with it) at the attach entry. The effect's update then asks for that matrix after the object has moved;
// the owner takes the attached frame's local-to-world matrix as RenderWare's sync would produce it then (the root's
// modelling matrix, children multiplied by 0x8047FC08 - ported operation for operation, fused single precision), so
// the shell is placed exactly where the object is drawn. The field is cleared again at once, so an effect whose
// owner stops attaching it behaves as before.

#include <cstddef>
#include <unordered_map>

#include "gekko/types.h"

namespace gekko {

class Cpu;

class NativeAttachedEffects {
public:
    explicit NativeAttachedEffects(Cpu& cpu) noexcept : cpu_(cpu) {}

    void install();

    struct Stats {
        u64 attaches = 0;   // SetMatrix from the attach step
        u64 followed = 0;   // ... and taken again at the effect's update with this frame's transform
        u64 skipped = 0;    // attach entries not followed (the effect already follows something, or the entry changed)
        u64 other = 0;      // SetMatrix from anywhere else
    };
    const Stats& stats() const noexcept { return stats_; }

    // RenderWare's local-to-world matrix of `frame` as RwFrameGetLTM returns it now (16 floats, RwMatrix layout),
    // without writing anything.
    static void frame_ltm(Cpu& c, u32 frame, float out[16]);

private:
    void set_matrix(Cpu& c);

    Cpu& cpu_;
    Stats stats_;
    struct Attach {
        u32 entry = 0;
        u32 frame = 0;
    };
    std::unordered_map<u32, Attach> pending_;  // effect -> the attach entry it follows this frame
};

}  // namespace gekko
