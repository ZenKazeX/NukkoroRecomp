#include "gekko/native_immediate.h"

#include <cstdio>

#include "gekko/cpu.h"
#include "gekko/savestate.h"

namespace gekko {
namespace {

// __GXData = *(r2 + 0xFD8) (the GX library's own pointer; r2 = 0x805FA780).
constexpr u32 kSda2GxData = 0xFD8u;
constexpr u32 kGxVcdLow = 0x14u;
constexpr u32 kGxVcdHigh = 0x18u;
constexpr u32 kGxVatA = 0x1Cu;
constexpr u32 kGxVatB = 0x3Cu;
constexpr u32 kGxVatC = 0x5Cu;
constexpr u8 kDrawFirst = 0x80u;
constexpr u8 kDrawLast = 0xBFu;

}  // namespace

void NativeImmediate::refuse(u32 site, const char* what) {
    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "native immediate geometry: %s at GX site 0x%08X (opcode 0x%02X, %u vertices, %zu of %zu "
                  "payload bytes)",
                  what, site, opcode_, count_, payload_.size(), expected_);
    cpu_.request_stop(StopReason::ExecutionFault, detail);
}

void NativeImmediate::primitive_opcode(u32 site, u8 opcode) {
    if (phase_ != Phase::Idle) {
        refuse(site, phase_ == Phase::Payload ? "a primitive header while the previous primitive is incomplete"
                                              : "a primitive header after a header with no vertex count");
        return;
    }
    if (opcode < kDrawFirst || opcode > kDrawLast) {
        opcode_ = opcode;
        refuse(site, "a primitive header byte that is not a draw opcode");
        return;
    }
    opcode_ = opcode;
    count_ = 0;
    phase_ = Phase::Opcode;
}

void NativeImmediate::primitive_count(u32 site, u16 count) {
    if (phase_ != Phase::Opcode) {
        refuse(site, "a vertex count with no primitive header");
        return;
    }
    count_ = count;
    const u32 gx = cpu_.mem_read32(cpu_.gpr[2] + kSda2GxData);
    const u32 table = opcode_ & 7u;
    vcd_low_ = cpu_.mem_read32(gx + kGxVcdLow);
    vcd_high_ = cpu_.mem_read32(gx + kGxVcdHigh);
    vat_a_ = cpu_.mem_read32(gx + kGxVatA + 4u * table);
    vat_b_ = cpu_.mem_read32(gx + kGxVatB + 4u * table);
    vat_c_ = cpu_.mem_read32(gx + kGxVatC + 4u * table);
    open();
    if (expected_ == 0u) {
        complete();
    }
}

void NativeImmediate::open() {
    format_ = decode_vertex_format(vcd_low_, vcd_high_, vat_a_, vat_b_, vat_c_);
    expected_ = static_cast<std::size_t>(count_) * format_.stride;
    payload_.clear();
    if (payload_.capacity() < expected_) payload_.reserve(expected_);
    phase_ = Phase::Payload;
}

void NativeImmediate::vertex(u32 site, u32 value, u32 bytes) {
    if (phase_ != Phase::Payload) {
        refuse(site, "vertex data with no open primitive");
        return;
    }
    if (payload_.size() + bytes > expected_) {
        refuse(site, "vertex data past the primitive's end");
        return;
    }
    for (u32 shift = bytes * 8u; shift != 0u; shift -= 8u) {
        payload_.push_back(static_cast<u8>(value >> (shift - 8u)));
    }
    if (payload_.size() == expected_) {
        complete();
    }
}

void NativeImmediate::complete() {
    phase_ = Phase::Idle;
    ++stats_.primitives;
    stats_.vertices += count_;
    stats_.payload_bytes += expected_;
    if (expected_ == 0u) ++stats_.empty;
    if (sink_ != nullptr) {
        sink_->on_primitive(opcode_, count_, format_, vat_a_, vat_b_, vat_c_,
                            expected_ != 0u ? payload_.data() : nullptr, expected_);
    }
}

void NativeImmediate::save_state(std::vector<u8>& out) const {
    state::put<u8>(out, static_cast<u8>(phase_));
    state::put<u8>(out, opcode_);
    state::put<u32>(out, count_);
    state::put<u32>(out, vcd_low_);
    state::put<u32>(out, vcd_high_);
    state::put<u32>(out, vat_a_);
    state::put<u32>(out, vat_b_);
    state::put<u32>(out, vat_c_);
    state::put_block(out, payload_.data(), phase_ == Phase::Payload ? payload_.size() : 0u);
}

bool NativeImmediate::load_state(const u8*& cursor, const u8* end) {
    u8 phase = 0;
    u8 opcode = 0;
    u32 count = 0, vcd_low = 0, vcd_high = 0, vat_a = 0, vat_b = 0, vat_c = 0;
    u64 bytes = 0;
    if (!state::get<u8>(cursor, end, phase) || !state::get<u8>(cursor, end, opcode) ||
        !state::get<u32>(cursor, end, count) || !state::get<u32>(cursor, end, vcd_low) ||
        !state::get<u32>(cursor, end, vcd_high) || !state::get<u32>(cursor, end, vat_a) ||
        !state::get<u32>(cursor, end, vat_b) || !state::get<u32>(cursor, end, vat_c) ||
        !state::get<u64>(cursor, end, bytes)) {
        return false;
    }
    if (phase > static_cast<u8>(Phase::Payload)) return false;
    if (static_cast<u64>(end - cursor) < bytes) return false;
    phase_ = static_cast<Phase>(phase);
    opcode_ = opcode;
    count_ = count;
    vcd_low_ = vcd_low;
    vcd_high_ = vcd_high;
    vat_a_ = vat_a;
    vat_b_ = vat_b;
    vat_c_ = vat_c;
    payload_.clear();
    expected_ = 0;
    if (phase_ == Phase::Payload) {
        open();
        if (bytes >= expected_) return false;  // an open primitive is incomplete by definition
        payload_.assign(cursor, cursor + bytes);
    } else if (bytes != 0u) {
        return false;
    }
    cursor += bytes;
    return true;
}

void NativeImmediate::reset() noexcept {
    phase_ = Phase::Idle;
    opcode_ = 0;
    count_ = 0;
    expected_ = 0;
    payload_.clear();
}

}  // namespace gekko
