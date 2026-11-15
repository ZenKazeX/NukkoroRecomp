#pragma once

// CodeMemory over the game's address window: the stores use GuestMemory's accessors, the same pages the translated
// code's inline accessors address (guest_memory.h, "FM"), so a write is visible to the game exactly like a store made by
// the game itself. Game thread only.
#include "gekko/code_engine.h"
#include "gekko/guest_memory.h"

namespace gekko::codes {

class GuestCodeMemory final : public CodeMemory {
public:
    explicit GuestCodeMemory(GuestMemory& memory) : memory_(memory) {}
    bool mapped(u32 address, std::size_t size) const override { return memory_.mapped(GcAddr(address), size); }
    u8 read8(u32 address) override { return memory_.read8(address); }
    u16 read16(u32 address) override { return memory_.read16(address); }
    u32 read32(u32 address) override { return memory_.read32(address); }
    void write8(u32 address, u8 value) override { memory_.write8(address, value); }
    void write16(u32 address, u16 value) override { memory_.write16(address, value); }
    void write32(u32 address, u32 value) override { memory_.write32(address, value); }

private:
    GuestMemory& memory_;
};

}  // namespace gekko::codes
