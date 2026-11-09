// The native owners of the GX display-copy and FIFO-setup leaves (gekko/native_gx_display.h).

#include "gekko/native_gx_display.h"

#include "gekko/cpu.h"

#include <cstdio>
#include <string>

namespace gekko::gx_display {

namespace {

constexpr Address kAddresses[] = {
    {kGxInitFifoBase, "GXInitFifoBase"},
    {0x80396098u, "GXFlush"},
    {0x80396BD4u, "GXSetDispCopySrc"},
    {0x80396CCCu, "GXSetDispCopyDst"},
    {0x80396E54u, "GX display copy clamp"},
    {0x80397228u, "GXSetCopyFilter"},
    {0x80397430u, "GX display copy gamma"},
    {0x80399E58u, "GX pixel format setter"},
    {0x80399FCCu, "GX BP mask setter"},
    {0x803971B0u, "GX BP 0x4F-0x51 setter"},
    {0x803992C8u, "GX BP state register setter"},
};

constexpr Address kLiftedAddresses[] = {
    {kGxCopyDisp, "GXCopyDisp (refused)"},
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

void serve_init_fifo_base(Cpu& cpu) {
    const u32 fifo = cpu.gpr[3], base = cpu.gpr[4], size = cpu.gpr[5];
    cpu.mem_write32(fifo + 0x00u, base);
    cpu.mem_write32(fifo + 0x04u, base + (size - 4u));
    cpu.mem_write32(fifo + 0x08u, size);
    // GXInitFifoLimits(fifo, size - 0x4000, (size >> 1) & ~0x1F)
    cpu.mem_write32(fifo + 0x0Cu, size - 0x4000u);
    cpu.mem_write32(fifo + 0x10u, (size >> 1) & 0x7FFFFFE0u);
    // GXInitFifoPtrs(fifo, base, base): read pointer, write pointer, and the byte count between them (base - base = 0,
    // so the "add the size when negative" branch never runs). Interrupts are disabled around it and restored.
    cpu.mem_write32(fifo + 0x14u, base);
    cpu.mem_write32(fifo + 0x18u, base);
    cpu.mem_write32(fifo + 0x1Cu, 0u);
}

void install(Cpu& cpu) {
    for (const Address& entry : kAddresses) {
        if (entry.address == kGxInitFifoBase) {
            cpu.install_hle(entry.address, [](Cpu& c) {
                serve_init_fifo_base(c);
                return true;
            }, "native GX FIFO setup: GXInitFifoBase");
            continue;
        }
        // Served and silent: the shadow these functions kept is retired (see the header).
        cpu.install_hle(entry.address, [](Cpu&) { return true; }, std::string("native GX display state: ") + entry.name);
    }
}

void install_lifted(Cpu& cpu) {
    for (const Address& entry : kLiftedAddresses) {
        cpu.install_hle(entry.address, [](Cpu& c) {
            char detail[160];
            std::snprintf(detail, sizeof(detail),
                          "GXCopyDisp was entered from 0x%08X: in the lifted build the display copy is the lowered "
                          "render_submit_frame and every GX copy trigger is a dead site", c.lr);
            c.request_stop(StopReason::ExecutionFault, detail);
            return true;
        }, std::string("native GX display copy: ") + entry.name);
    }
}

}  // namespace gekko::gx_display
