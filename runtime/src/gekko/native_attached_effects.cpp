// Attached effects follow their object as it is this frame. Contract and evidence:
// include/gekko/native_attached_effects.h and docs/RENDER_FLICKER.md "Fourth bug".

#include "gekko/native_attached_effects.h"

#include <cstring>

#include "gekko/cpu.h"
#include "gekko/ppc_ops.h"

namespace gekko {

namespace {

constexpr u32 kSetMatrix = 0x80426EB8u;
constexpr u32 kFromAttach = 0x80167ADCu;  // the attach step 0x80167A00's call
constexpr u32 kFromUpdate = 0x80426CB4u;  // the effect's update 0x80426C84, following +0x48

// The effect (Effect::...::EffectParticle* base, 0x80426FB0 initialises it).
constexpr u32 kEffectFollowMatrix = 0x48u;    // a matrix to take at every update (0x80426CA4)
constexpr u32 kEffectFollowPosition = 0x4Cu;  // a position to take (0x80426CB8)
constexpr u32 kEffectFollowOther = 0x50u;     // 0x80426DE4's source
constexpr u32 kEffectBase = 0x84u;            // 3x4, rows (0x8040D198 copies 12 words)

// An attach entry (0x80167A00: stride 0x4C).
constexpr u32 kEntryFrame = 0x10u;
constexpr u32 kEntryMatrix = 0x14u;  // the frame's LTM, transposed into the effect's 3x4 rows

// RwFrame / RwMatrix.
constexpr u32 kFrameParent = 0x04u;
constexpr u32 kFrameModelling = 0x10u;
constexpr u32 kFrameLtm = 0x50u;
constexpr u32 kFrameRoot = 0xA0u;
constexpr u32 kFrameFlags = 0x03u;  // private flags byte
constexpr u32 kRootDirty = 0x01u;   // RwFrameGetLTM syncs the hierarchy (0x80488300)
constexpr u32 kSyncLtm = 0x04u;     // this frame's LTM is rebuilt (0x8048C644, inherited down: 0x8048C434)

float read_f32(Cpu& c, u32 address) {
    const u32 bits = c.mem_read32(address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

u64 wide(float value) { return double_to_bits(static_cast<double>(value)); }
float narrow(u64 bits) { return static_cast<float>(bits_to_double(bits)); }

void read_matrix(Cpu& c, u32 address, float out[16]) {
    for (u32 i = 0; i < 16u; ++i)
        out[i] = read_f32(c, address + 4u * i);
}

// RwMatrixMultiply 0x8047FC08: out = a x b (a the child's modelling matrix, b its parent's LTM), in the original's
// operation order - each product and fused multiply-add rounded to single as the paired-single unit does. Rows
// right (0), up (4), at (8): out[r][k] = ((b.right[k] * a[r][0]) + b.up[k] * a[r][1]) + b.at[k] * a[r][2];
// pos: out.pos[k] = ((b.right[k] * a.pos.x + b.pos[k]) + b.up[k] * a.pos.y) + b.at[k] * a.pos.z.
void multiply(Cpu& c, const float a[16], const float b[16], float out[16]) {
    for (u32 row = 0; row < 12u; row += 4u) {
        for (u32 k = 0; k < 3u; ++k) {
            u64 acc = ppc::fps_mul(c, wide(b[0 + k]), wide(a[row + 0]));
            acc = ppc::fps_madd(c, wide(b[4 + k]), wide(a[row + 1]), acc);
            acc = ppc::fps_madd(c, wide(b[8 + k]), wide(a[row + 2]), acc);
            out[row + k] = narrow(acc);
        }
        out[row + 3] = a[row + 3];  // padding words: not written by the original (unused here)
    }
    for (u32 k = 0; k < 3u; ++k) {
        u64 acc = ppc::fps_madd(c, wide(b[0 + k]), wide(a[12]), wide(b[12 + k]));
        acc = ppc::fps_madd(c, wide(b[4 + k]), wide(a[13]), acc);
        acc = ppc::fps_madd(c, wide(b[8 + k]), wide(a[14]), acc);
        out[12 + k] = narrow(acc);
    }
    out[15] = a[15];
}

}  // namespace

void NativeAttachedEffects::frame_ltm(Cpu& c, u32 frame, float out[16]) {
    const u32 root = c.mem_read32(frame + kFrameRoot);
    if (root == 0u || (c.mem_read8(root + kFrameFlags) & kRootDirty) == 0u) {
        read_matrix(c, frame + kFrameLtm, out);
        return;
    }
    // The path root .. frame; the sync (0x8048C628 / 0x8048C410) rebuilds each frame whose own or inherited flags
    // carry kSyncLtm: the root from its modelling matrix, a child as modelling x parent LTM.
    u32 path[64];
    u32 depth = 0;
    for (u32 f = frame; f != 0u && depth < 64u; f = c.mem_read32(f + kFrameParent))
        path[depth++] = f;
    // The arithmetic's sticky FPSCR bits are this computation's, not the game's.
    const u32 fpscr = c.fpscr;
    float ltm[16], local[16], next[16];
    u32 flags = 0;
    for (u32 i = depth; i-- > 0;) {
        const u32 f = path[i];
        flags |= c.mem_read8(f + kFrameFlags);
        if ((flags & kSyncLtm) == 0u) {
            read_matrix(c, f + kFrameLtm, ltm);
        } else if (i == depth - 1u) {
            read_matrix(c, f + kFrameModelling, ltm);
        } else {
            read_matrix(c, f + kFrameModelling, local);
            multiply(c, local, ltm, next);
            std::memcpy(ltm, next, sizeof(ltm));
        }
    }
    c.fpscr = fpscr;
    std::memcpy(out, ltm, sizeof(ltm));
}

void NativeAttachedEffects::install() {
    cpu_.install_hle(kSetMatrix, [this](Cpu& c) {
        set_matrix(c);
        return true;  // an owner never declines
    }, "attached effects: effect SetMatrix (0x80426EB8)");
}

// (r3 effect, r4 3x4 matrix): the effect's base matrix (+0x84) = the matrix.
void NativeAttachedEffects::set_matrix(Cpu& c) {
    const u32 effect = c.gpr[3];
    u32 source = c.gpr[4];
    if (c.lr == kFromUpdate) {
        // The effect's update asks for the matrix it follows: an attach entry's, this frame's transform.
        const auto found = pending_.find(effect);
        if (found != pending_.end()) {
            const Attach attach = found->second;
            pending_.erase(found);
            c.mem_write32(effect + kEffectFollowMatrix, 0u);
            if (source == attach.entry + kEntryMatrix && c.mem_read32(attach.entry + kEntryFrame) == attach.frame) {
                float ltm[16];
                frame_ltm(c, attach.frame, ltm);
                // The attach step's transposition (0x80167A70 .. 0x80167AD0): row r of the effect's matrix holds
                // column r of the LTM - right, up, at, pos components.
                for (u32 r = 0; r < 3u; ++r)
                    for (u32 col = 0; col < 4u; ++col) {
                        u32 bits;
                        std::memcpy(&bits, &ltm[col * 4u + r], sizeof(bits));
                        c.mem_write32(attach.entry + kEntryMatrix + 4u * (r * 4u + col), bits);
                    }
                ++stats_.followed;
            }
        }
    } else if (c.lr == kFromAttach) {
        ++stats_.attaches;
        const u32 entry = source - kEntryMatrix;
        // Only an effect that follows nothing else: then its update takes +0x48 where it would have found nothing.
        if (c.mem_read32(effect + kEffectFollowMatrix) == 0u && c.mem_read32(effect + kEffectFollowPosition) == 0u &&
            c.mem_read32(effect + kEffectFollowOther) == 0u) {
            pending_[effect] = Attach{entry, c.mem_read32(entry + kEntryFrame)};
            c.mem_write32(effect + kEffectFollowMatrix, source);
        } else {
            ++stats_.skipped;
        }
    } else {
        ++stats_.other;
    }
    // 0x8040D198: twelve words.
    for (u32 i = 0; i < 12u; ++i)
        c.mem_write32(effect + kEffectBase + 4u * i, c.mem_read32(source + 4u * i));
}

}  // namespace gekko
