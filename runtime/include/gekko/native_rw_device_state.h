#pragma once

// The RenderWare GameCube driver's device render-state entries (gx-rw-plan cluster rwR1), owned exactly
// (docs/agents/native-boot.md Step 7). The device record is the engine's GX state block at 0x805E42D0.
//
//   0x804960FC  camera view-matrix upload (r3 = camera frame or 0, r4 = also load the normal matrix).
//               The original builds a 4x4 view matrix on its own stack (RwMatrixMultiply 0x8047FC08, whose only store
//               goes through its out pointer, here the stack) and hands it to GXLoadPosMtxImm / GXLoadNrmMtxImm /
//               GXSetCurrentMtx. In this program those three are owned silent (gx_state: the matrices go to a pipe
//               nothing reads), so the body has NO guest-visible effect except r3 = 0 (the last call's argument): the
//               owner is a silent return with r3 = 0. Contract: the original's complete write set outside its stack is
//               empty.
//   0x804987C8  mode setter (r3 = mode). When the cached mode record+0x3C (0x805E430C) differs from r3 the original
//               calls GXSetAlphaCompare / GXSetZCompLoc (silent) and stores r3 into the cache: the owner stores the
//               cache and leaves r3 as the original does (r3 & 0xFF after a change, r3 unchanged otherwise).
//   (0x80498854 SubmitNoLightNode is NOT owned here: it ends in the texture bind 0x8049AC88, which is RW GameCube driver
//   code, not RenderWare core, so the guest-call seam does not apply; it follows when the bind is native.)

#include "gekko/cpu.h"

#include <cstddef>

namespace gekko::rw_device_state {

constexpr u32 kViewMatrixUpload = 0x804960FCu;
constexpr u32 kModeSetter = 0x804987C8u;
constexpr u32 kModeCache = 0x805E430Cu;       // device record + 0x3C

struct Owners {
    struct Address {
        u32 address;
        const char* name;
    };
    static const Address* addresses(std::size_t& count) noexcept {
        static constexpr Address kAddresses[] = {
            {kViewMatrixUpload, "camera view-matrix upload"},
            {kModeSetter, "device alpha-test / z-compare-location mode setter"},
        };
        count = sizeof(kAddresses) / sizeof(kAddresses[0]);
        return kAddresses;
    }
};

// The served bodies (for the contract test): each serves one entry.
void serve_view_matrix_upload(Cpu& cpu) noexcept;
void serve_mode_setter(Cpu& cpu) noexcept;

// Installs the two owners (LiftedRender::install_owners).
void install(Cpu& cpu);

}  // namespace gekko::rw_device_state
