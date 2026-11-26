#include "gekko/gameplay_camera.h"
#include "gekko/game_memory_end.h"

#include <bit>
#include <cstdio>
#include <cstdlib>

#include "gekko/cpu.h"

namespace gekko {

namespace {

constexpr u32 kPauseByte = 0x805EF27Cu;               // set by the pause manager (0x801EB674), cleared by 0x801EB668
constexpr u32 kCameraManager = 0x8056FED4u;           // SonicteamUSA::System::Camera::CameraManager singleton
constexpr u32 kUnitVectorData = kCameraManager + 0xCu;  // vector<shared_ptr<CameraManagerUnit>> at +4: data at its +8
constexpr u32 kUnitRequestListSize = 0xE0u;           // CameraManagerUnit: request list size

bool plausible(u32 address) noexcept { return address >= 0x80000000u && address < gekko::game_memory_end(); }

}  // namespace

bool gameplay_camera_active(Cpu& cpu) noexcept {
    if (cpu.mem_read8(kPauseByte) != 0u) return false;
    const u32 units = cpu.mem_read32(kUnitVectorData);
    if (!plausible(units)) return false;
    const u32 unit0 = cpu.mem_read32(units);  // element 0: the shared_ptr's object pointer
    if (!plausible(unit0)) return false;
    return cpu.mem_read32(unit0 + kUnitRequestListSize) != 0u;
}

bool gameplay_paused(Cpu& cpu) noexcept { return cpu.mem_read8(kPauseByte) != 0u; }

void camera_trace_record(Cpu& cpu, u64 game_ticks, const shadow::peripheral::ControllerSample& sample) {
    static std::FILE* const file = [] {
        const char* path = std::getenv("GEKKO_CAMERA_TRACE");
        std::FILE* f = path != nullptr ? std::fopen(path, "w") : nullptr;
        if (f != nullptr) std::fputs("tick,controls,sx,sy,cx,cy,active,ex,ey,ez,lx,ly,lz,px,py,pz,vx,vy,vz\n", f);
        return f;
    }();
    if (file == nullptr) return;
    const auto f32 = [&](u32 address) { return std::bit_cast<float>(cpu.mem_read32(address)); };
    const u32 units = cpu.mem_read32(kUnitVectorData);
    const u32 unit0 = plausible(units) ? cpu.mem_read32(units) : 0u;
    if (!plausible(unit0)) return;
    const u32 output = unit0 + 0x08u;                     // CameraManagerUnit::output: eye +0x00, look +0x0C
    const u32 target = cpu.mem_read32(unit0 + 0xF4u);     // CameraTargetInfo: position +0x00, velocity +0x0C
    if (!plausible(target)) return;
    std::fprintf(file, "%llu,%x,%d,%d,%d,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
                 static_cast<unsigned long long>(game_ticks), sample.controls, sample.stick_x, sample.stick_y,
                 sample.substick_x, sample.substick_y, gameplay_camera_active(cpu) ? 1 : 0, f32(output), f32(output + 4u),
                 f32(output + 8u), f32(output + 12u), f32(output + 16u), f32(output + 20u), f32(target), f32(target + 4u),
                 f32(target + 8u), f32(target + 12u), f32(target + 16u), f32(target + 20u));
    std::fflush(file);
}

}  // namespace gekko
