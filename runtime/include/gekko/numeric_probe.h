#pragma once

// Observation-only, bounded evidence reducer. Never supplies execution state.
// Overflow means incomplete evidence, not permission to assume a constant.
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>

namespace gekko::measurement {
struct NumericKey {
    std::array<std::uint32_t, 8> formats{};
    std::uint32_t status = 0;
    bool operator==(const NumericKey& other) const noexcept {
        return formats == other.formats && status == other.status;
    }
};

struct EntryArgumentKey {
    std::uint32_t value = 0;
    bool operator==(const EntryArgumentKey& other) const noexcept { return value == other.value; }
};

// Actual reference-entry observations only, never a state restoration input.
struct OrderedNumericEvent {
    std::uint64_t sequence = 0, instructions = 0, guest_frame = 0;
    std::uint32_t site = 0, lr = 0, vi = 0, msr = 0, fpscr = 0;
    std::array<std::uint64_t, 4> f18_f19{}; // f18.ps0/ps1, f19.ps0/ps1 raw bits
    static constexpr std::array<unsigned, 12> root_registers{1,3,4,5,6,7,13,27,28,29,30,31};
    std::array<std::uint32_t, 12> roots{}; // raw tags, never dereferenced as sources
    std::array<std::uint32_t, 8> formats{};
    std::uint32_t current_context = 0, fp_owner = 0, thread = 0;
    std::uint32_t context_fields_observed = 0; // bits 0/1/2: current/FP owner/thread
    // Producer output, saved backing values and interior instruction ordering
    // are deliberately unobserved by an entry-only hook.
};

class OrderedNumericEvidence {
public:
    // Explicit diagnostic ceiling; allocation happens once before execution.
    // No growth, wrap, overwrite or sampling after exhaustion. The existing
    // validated cold routes have ~20K context restores plus ~10K producers;
    // users must inspect drops rather than inflate capacity to claim closure.
    static constexpr std::size_t max_events = 65536;
    explicit OrderedNumericEvidence(std::size_t capacity = 0) : capacity_(capacity) {
        if (capacity > max_events) throw std::invalid_argument("ordered evidence capacity exceeded");
        if (capacity) events_ = std::make_unique<OrderedNumericEvent[]>(capacity);
    }
    template<class Snapshot> void observe(Snapshot&& snapshot) noexcept {
        if (!capacity_) return;
        if (hits_ == std::numeric_limits<std::uint64_t>::max()) { counter_overflow_ = true; return; }
        ++hits_;
        if (used_ == capacity_) { ++dropped_; return; }
        auto event = snapshot(); // no guest reads when disabled/full
        event.sequence = hits_;
        events_[used_++] = event;
    }
    const OrderedNumericEvent& operator[](std::size_t index) const noexcept { return events_[index]; }
    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t used() const noexcept { return used_; }
    std::size_t retained_bytes() const noexcept { return capacity_ * sizeof(OrderedNumericEvent); }
    std::uint64_t hits() const noexcept { return hits_; }
    std::uint64_t dropped() const noexcept { return dropped_; }
    bool counter_overflow() const noexcept { return counter_overflow_; }
    bool complete() const noexcept { return hits_ != 0 && dropped_ == 0 && !counter_overflow_; }
private:
    std::unique_ptr<OrderedNumericEvent[]> events_;
    std::size_t capacity_ = 0, used_ = 0;
    std::uint64_t hits_ = 0, dropped_ = 0;
    bool counter_overflow_ = false;
};

template <std::size_t Capacity, class Key = NumericKey> struct NumericEvidence {
    static_assert(Capacity > 0, "evidence must have storage");
    struct Slot { Key key{}; std::uint64_t hits = 0; };
    std::array<Slot, Capacity> slots{};
    std::size_t used = 0;
    std::uint64_t hits = 0, unclassified = 0;

    void observe(const Key& key) noexcept {
        ++hits;
        for (std::size_t i = 0; i < used; ++i) {
            if (slots[i].key == key) { ++slots[i].hits; return; }
        }
        if (used == Capacity) { ++unclassified; return; }
        slots[used++] = Slot{key, 1};
    }
    bool complete() const noexcept { return hits != 0 && unclassified == 0; }
};
} // namespace gekko::measurement
