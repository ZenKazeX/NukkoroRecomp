// The GX default region callbacks and the RenderWare raster-render wrappers as native owners (gekko/native_driver_callbacks.h).

#include "gekko/native_driver_callbacks.h"

#include "gekko/cpu.h"

#include <string>

namespace gekko::driver_callbacks {

namespace {

constexpr u32 kSda2GxData = 0xFD8u;  // r2-relative pointer to __GXData
constexpr u32 kTexRegionsFirst = 0x208u;   // eight 16-byte records each: no mipmaps, mipmaps, colour-index mipmaps
constexpr u32 kTexRegionsSecond = 0x288u;
constexpr u32 kTexRegionsThird = 0x308u;
constexpr u32 kTlutRegions = 0x388u;       // twenty 16-byte records
constexpr u32 kTlutRegionCount = 0x14u;

constexpr Address kAddresses[] = {
    {kTexRegion, "GX default texture region callback"},
    {kTlutRegion, "GX default TLUT region callback"},
};
constexpr Address kLiftedAddresses[] = {
    {kRasterRenderScaled0Fast1, "RwRasterRender wrapper (scaled 0, fast 1)"},
    {kRasterRenderScaled0Fast0, "RwRasterRender wrapper (scaled 0, fast 0)"},
    {kRasterRenderScaled1Fast1, "RwRasterRender wrapper (scaled 1, fast 1)"},
};

// scaled (r5) and fast (r6) of each wrapper, parallel to kLiftedAddresses.
constexpr u32 kWrapperFlags[][2] = {{0u, 1u}, {0u, 0u}, {1u, 1u}};

void tex_region(Cpu& c) {
    const u32 object = c.gpr[3];
    const s32 id = static_cast<s32>(c.gpr[4]);
    const s32 format = static_cast<s32>(c.mem_read32(object + 0x14u));  // GXGetTexObjFmt
    const bool mipmap = (c.mem_read8(object + 0x1Fu) & 1u) != 0u;       // GXGetTexObjMipMap
    u32 base;
    if (format == 6) base = mipmap ? kTexRegionsThird : kTexRegionsSecond;
    else if (format >= 8 && format <= 10) base = kTexRegionsFirst;
    else base = mipmap ? kTexRegionsSecond : kTexRegionsFirst;
    const s32 index = id - (id / 8) * 8;  // srawi 3 / addze / slwi 3 / subfc: the signed remainder
    c.gpr[3] = c.mem_read32(c.gpr[2] + kSda2GxData) + base + (static_cast<u32>(index) << 4);
}

void tlut_region(Cpu& c) {
    const u32 id = c.gpr[3];
    c.gpr[3] = id < kTlutRegionCount ? c.mem_read32(c.gpr[2] + kSda2GxData) + kTlutRegions + (id << 4) : 0u;
}

void raster_render_wrapper(Cpu& c, u32 scaled, u32 fast) {
    c.gpr[5] = scaled;
    c.gpr[6] = fast;
    if (!c.serve_owner(kRasterRenderCore)) {
        c.request_stop(StopReason::ExecutionFault, "RwRasterRender wrapper: the raster render core has no owner installed");
    }
}

}  // namespace

const Address* addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

const Address* lifted_addresses(std::size_t& count) noexcept {
    count = sizeof(kLiftedAddresses) / sizeof(kLiftedAddresses[0]);
    return kLiftedAddresses;
}

void serve(Cpu& c, u32 address) {
    if (address == kAddresses[0].address) {
        tex_region(c);
        return;
    }
    if (address == kAddresses[1].address) {
        tlut_region(c);
        return;
    }
    for (std::size_t i = 0; i < sizeof(kLiftedAddresses) / sizeof(kLiftedAddresses[0]); ++i) {
        if (address == kLiftedAddresses[i].address) {
            raster_render_wrapper(c, kWrapperFlags[i][0], kWrapperFlags[i][1]);
            return;
        }
    }
    c.request_stop(StopReason::ExecutionFault, "driver callback owner called at an address it does not own");
}

void install(Cpu& cpu) {
    for (const Address& entry : kAddresses) {
        const u32 address = entry.address;
        cpu.install_hle(address, [address](Cpu& c) {
            serve(c, address);
            return true;
        }, std::string("native driver callback: ") + entry.name);
    }
}

void install_lifted(Cpu& cpu) {
    for (const Address& entry : kLiftedAddresses) {
        const u32 address = entry.address;
        cpu.install_hle(address, [address](Cpu& c) {
            serve(c, address);
            return true;
        }, std::string("native driver callback: ") + entry.name);
    }
}

}  // namespace gekko::driver_callbacks
