#pragma once

// The migration boundary of the native player (docs/SHADOW_PLAYER_NATIVE_RECONSTRUCTION.md, boundary option A,
// approved 2026-11-03): native player code reads and writes the ONE player object the game allocated
// (Player::PlayerShadow, operator new 0x29C at 0x800AE40C) through this view. The view holds no state of its own - a
// second authoritative player state is forbidden - only the object's guest address.
//
// Contract, enforced by runtime/tests/player_view_tests.cpp:
//  - every accessor is exactly ONE guest access of the field's width at base + offset, in the order the native code
//    calls it: the original's observable write order is the native code's job, and nothing here batches, caches,
//    reorders or widens an access;
//  - accesses go through GuestMemory (journalled, traced, watched like the CPU's): --verify-player and --watch-guest
//    see the native code's traffic exactly as they see a translated row's;
//  - a field exists here only with binary evidence of its offset, width and kind (test_data/player/
//    player_shadow_fields.csv, from tools/dolrecon/player_layout.py); bytes read with two widths or kinds are ONE field
//    of the widest observed shape plus typed reinterpretations, never two members;
//  - floats are the guest's big-endian IEEE single bits, moved as bits (no host conversion, no NaN quieting).
// Names say only what the evidence proves; a field's meaning is named when the batch that uses it proves it.
// TEMPORARY: retired with the last translated reader of the object (batch 13).

#include <cstdint>
#include <cstring>

#include "gekko/guest_memory.h"
#include "gekko/types.h"

namespace gekko::player {

enum class FieldKind : u8 { Int, Float };

struct FieldShape {
    u32 offset;
    u32 width;  // bytes: 1, 2 or 4
    FieldKind kind;
};

// The view's fields: (offset, width, kind) as the methods of PlayerShadow and its bases access them (player_layout.py
// over pal-v30). Extended batch by batch, each addition with its evidence rows.
inline constexpr FieldShape kPlayerShadowFields[] = {
    {0x0C, 4, FieldKind::Float}, {0x10, 4, FieldKind::Float}, {0x14, 4, FieldKind::Float},
    {0x30, 4, FieldKind::Int},   {0x34, 4, FieldKind::Int},   {0x38, 4, FieldKind::Int},
    {0x4C, 4, FieldKind::Int},   {0x50, 4, FieldKind::Int},   {0x54, 4, FieldKind::Int},
    {0x58, 4, FieldKind::Int},   {0x5C, 4, FieldKind::Int},   {0x60, 4, FieldKind::Int},
    {0x64, 4, FieldKind::Int},   {0x80, 4, FieldKind::Int},   {0x84, 4, FieldKind::Int},
    {0x88, 4, FieldKind::Int},   {0x8C, 4, FieldKind::Float}, {0x90, 4, FieldKind::Int},
    {0x94, 4, FieldKind::Float}, {0x98, 4, FieldKind::Float}, {0x9C, 4, FieldKind::Float},
    {0x218, 4, FieldKind::Int},  {0x224, 4, FieldKind::Int},  {0x228, 4, FieldKind::Int},
    {0x22C, 4, FieldKind::Int},  {0x230, 4, FieldKind::Int},  {0x234, 4, FieldKind::Int},
    {0x238, 4, FieldKind::Int},  {0x23C, 4, FieldKind::Int},  {0x240, 4, FieldKind::Int},
    {0x244, 4, FieldKind::Int},  {0x248, 4, FieldKind::Int},  {0x24C, 4, FieldKind::Int},
    {0x250, 4, FieldKind::Int},  {0x254, 4, FieldKind::Int},  {0x268, 4, FieldKind::Int},
    {0x26C, 4, FieldKind::Int},  {0x274, 4, FieldKind::Float}, {0x278, 4, FieldKind::Int},
    {0x27C, 4, FieldKind::Float}, {0x298, 4, FieldKind::Int},
};
inline constexpr u32 kPlayerShadowSize = 0x29C;

constexpr bool known_field(u32 offset, u32 width, FieldKind kind) {
    for (const FieldShape& f : kPlayerShadowFields)
        if (f.offset == offset && f.width == width && f.kind == kind) return true;
    return false;
}

class PlayerShadowView {
public:
    PlayerShadowView(GuestMemory& memory, u32 object) noexcept : memory_(&memory), object_(object) {}
    u32 address() const noexcept { return object_; }

    template <u32 Offset>
    u32 word() const {
        static_assert(known_field(Offset, 4, FieldKind::Int), "no evidence for a 32-bit integer field here");
        return memory_->read32(object_ + Offset);
    }
    template <u32 Offset>
    void set_word(u32 value) {
        static_assert(known_field(Offset, 4, FieldKind::Int), "no evidence for a 32-bit integer field here");
        memory_->write32(object_ + Offset, value);
    }
    // A guest single as its bits; float_value() is the same bits as a host float (copied: payloads kept).
    template <u32 Offset>
    u32 float_bits() const {
        static_assert(known_field(Offset, 4, FieldKind::Float), "no evidence for a float field here");
        return memory_->read32(object_ + Offset);
    }
    template <u32 Offset>
    float float_value() const {
        const u32 bits = float_bits<Offset>();
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    template <u32 Offset>
    void set_float_bits(u32 bits) {
        static_assert(known_field(Offset, 4, FieldKind::Float), "no evidence for a float field here");
        memory_->write32(object_ + Offset, bits);
    }
    template <u32 Offset>
    void set_float_value(float value) {
        u32 bits;
        std::memcpy(&bits, &value, sizeof(bits));
        set_float_bits<Offset>(bits);
    }

private:
    GuestMemory* memory_;
    u32 object_;
};

}  // namespace gekko::player
