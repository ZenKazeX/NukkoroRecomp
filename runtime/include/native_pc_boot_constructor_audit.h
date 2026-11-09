#pragma once
#include "native_pc_boot_original_contract.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace shadow::pc_bootstrap {

// Original-oracle observation only. Does not invoke, replace or omit a ctor.
// Kept independent of Cpu/System so its ownership rules can be tested offline.
class ConstructorAudit {
public:
    static constexpr std::uint32_t kTableAddress = 0x804AAC60u;
    static constexpr std::uint32_t kWalkerReturn = 0x803796FCu;
    static constexpr std::size_t kExpected = original::kConstructorTable.size() - 1u;
    struct Record {
        std::uint32_t index = 0;
        std::uint32_t target = 0;
        std::uint32_t table_slot = 0;
        std::uint32_t return_address = 0;
        std::uint64_t game_ticks = 0;  // the game clock at the call (never decreases)
    };
    void begin(bool original_table_matches) noexcept {
        if (started_) return;
        started_ = true;
        table_matches_ = original_table_matches;
    }
    void observe(std::uint32_t target, std::uint32_t table_slot,
                 std::uint32_t lr, std::uint64_t game_ticks) noexcept {
        // Nested ctor calls and later ordinary calls are not walker entries.
        if (!started_ || sealed_ || lr != kWalkerReturn) return;
        if (count_ == records_.size()) {
            if (overflow_ != std::numeric_limits<std::uint32_t>::max()) ++overflow_;
            return;
        }
        const auto delta = std::uint64_t(table_slot) - kTableAddress;
        const auto index = table_slot >= kTableAddress && delta < kExpected * 4u && (delta & 3u) == 0u
            ? static_cast<std::uint32_t>(delta / 4u) : std::numeric_limits<std::uint32_t>::max();
        records_[count_++] = {index, target, table_slot, lr, game_ticks};
    }
    void seal(bool original_table_still_matches) noexcept {
        if (sealed_) return;
        table_matches_ = table_matches_ && original_table_still_matches;
        sealed_ = true;
    }
    bool valid() const noexcept {
        if (!started_ || !sealed_ || !table_matches_ || overflow_ != 0u || count_ != kExpected) return false;
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& record = records_[i];
            if (record.index != i || record.target != original::kConstructorTable[i] ||
                record.table_slot != kTableAddress + i * 4u || record.return_address != kWalkerReturn)
                return false;
            if (i != 0u && record.game_ticks < records_[i - 1u].game_ticks) return false;
        }
        return true;
    }
    std::size_t count() const noexcept { return count_; }
    std::uint32_t overflow() const noexcept { return overflow_; }
    bool table_matches() const noexcept { return table_matches_; }
    bool started() const noexcept { return started_; }
    bool sealed() const noexcept { return sealed_; }
    const std::array<Record, kExpected>& records() const noexcept { return records_; }
private:
    std::array<Record, kExpected> records_{};
    std::size_t count_ = 0;
    std::uint32_t overflow_ = 0;
    bool started_ = false;
    bool sealed_ = false;
    bool table_matches_ = false;
};

} // namespace shadow::pc_bootstrap
