// The RenderWare driver's device render-state entries, native. Contract: include/gekko/native_rw_device_state.h.

#include "gekko/native_rw_device_state.h"

namespace gekko::rw_device_state {
namespace {

// 0x804987C8: the mode cache store.
void apply_mode(Cpu& c, u32 mode) noexcept {
    if (c.mem_read32(kModeCache) == mode) return;  // the original returns without touching anything
    c.mem_write32(kModeCache, mode);
    c.gpr[3] = mode & 0xFFu;  // the original's last argument to GXSetZCompLoc, left in r3 (the silent owner keeps it)
}
}  // namespace

void serve_view_matrix_upload(Cpu& cpu) noexcept {
    cpu.gpr[3] = 0u;  // the original's last call is GXSetCurrentMtx(0); nothing else is observable
}

void serve_mode_setter(Cpu& cpu) noexcept {
    apply_mode(cpu, cpu.gpr[3]);
}

void install(Cpu& cpu) {
    cpu.install_hle(kViewMatrixUpload, [](Cpu& c) {
        serve_view_matrix_upload(c);
        return true;
    }, "RenderWare device: camera view-matrix upload (silent)");
    cpu.install_hle(kModeSetter, [](Cpu& c) {
        serve_mode_setter(c);
        return true;
    }, "RenderWare device: alpha-test / z-compare-location mode setter");
}

}  // namespace gekko::rw_device_state
