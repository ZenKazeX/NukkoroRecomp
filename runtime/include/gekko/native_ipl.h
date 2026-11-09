#pragma once

// The OS's other external-interface clients: a MODEL OF THE IPL CHIP'S SETTINGS MEMORY (the 64-byte SRAM and the
// 4-byte RTC counter image, the OSSram checksums, the card-A flash-ID pairing the CARD mount checks), the boot ROM
// reader (refused), the debug UART, the EXI bring-up and the debugger hand-off. The EXI library and channels are
// gone; the SRAM chip's content and pairing semantics are reproduced byte for byte. Not native settings
// (NATIVE_AUDIT.md C2, lie 3; ES comment fix 2026-10-20): to be replaced by host settings in NE-SAVE.
//
// docs/native-time-20261005/STAGE5_EXI.md (D4, D5). On the console the IPL
// chip on EXI channel 0 held the 64-byte SRAM (the OS's settings: language,
// sound mode, video offsets and flags, the cards' flash IDs), the boot ROM
// (the system fonts), the real-time clock and, on development hardware, a
// UART for OSReport; channel 2 carried the debugger's link. Here:
//
//   EXIInit 0x803B4CFC      masks OS interrupts 9-16 as the original did;
//                           nothing else (no handlers: nothing raises them;
//                           OSRegisterVersion is a report, as for PAD and VI)
//   __OSInitSram 0x803776F4 the SRAM read into the OS's control block (Scb
//                           0x80586FE0), then OSSetGbsMode(OSGetGbsMode())'s
//                           normalisation of the gbs word
//   UnlockSram 0x803778E8   the commit (flags clamp, the OSSram checksums, the
//                           gbs rule, the write of Scb.sram[offset..64) into
//                           the settings image, persisted when it changed),
//                           the lock release and OSRestoreInterrupts
//   __OSReadROM 0x80377C7C  refused (stop): this program has no boot ROM; only
//                           the disc-error screen's font reads it, and the
//                           native file API never reports a drive error
//   WriteUARTN 0x803B5848   the original's guest-visible effects: 2 when the
//                           UART is not enabled, every '\n' of the caller's
//                           buffer rewritten to '\r', 0; the line is counted
//   InitMetroTRK 0x8039F8E0, InitMetroTRK_BBA 0x8039F978
//                           refused (stop): __start enters them only when the
//                           boot information names a debug monitor, and this
//                           program has none
//
// __OSLockSram / __OSLockSramEx, the SRAM getters and setters and
// InitializeUART never touched the chip and stay game code. The real-time
// clock is never read (the image links no __OSGetRTC).
//
// THE SETTINGS IMAGE (68 bytes: the 4-byte RTC counter, then the 64 SRAM
// bytes; host policy unchanged from the emulated device). Without a card it is
// the factory default (English, stereo, interlaced) and nothing persists. With
// a card in slot A: an `SRAM.raw` (68 bytes) found in the card source's
// directory or up to five parents above it is used when its card-A flash ID
// matches the card; `SRAM.pc.raw` beside the card's working image holds the
// settings the game wrote (loaded when present, rewritten whenever a commit
// changes a byte); the card-A flash ID is always re-derived from the card's
// header (the IPL's algorithm), so a persisted image can never fail the mount.
//
// State (savestate version 15): the settings image.

#include <array>
#include <cstddef>
#include <filesystem>
#include <vector>

#include "gekko/types.h"

namespace gekko {

class GuestMemory;
class Cpu;
class NativeInterrupts;

class NativeIpl final {
public:
    static constexpr std::size_t kSettingsSize = 0x44u;

    NativeIpl(Cpu& cpu, NativeInterrupts& interrupts) noexcept;

    // Installs the owners. Before the guest boots.
    void install();

    // ---- the settings image ----------------------------------------------------
    // No card: the factory default, nothing persisted.
    void reset_settings();
    // The card now in slot A (its source as given, its working image, its
    // bytes): the companion SRAM, the persisted settings and the flash ID, as
    // described above.
    void bind_card(const std::filesystem::path& card_source, const std::filesystem::path& card_backing,
                   const std::vector<u8>& card_image);
    // The native save store (gekko/save_store.h) instead of a card: the settings the game writes persist in
    // <folder>/SRAM.pc.raw (read when present), with no card identity (nothing mounts a card).
    void bind_save_folder(const std::filesystem::path& folder);
    // The PC settings' language and sound mode over the image the game reads at boot (OSGetLanguage /
    // OSGetSoundMode), checksum kept: `language` in the OS's numbering (0 English, 1 German, 2 French, 3 Spanish,
    // 4 Italian; negative = keep), `stereo` 1 / 0 (negative = keep). Not persisted by itself.
    void override_console_settings(int language, int stereo);
    // The companion SRAM.raw in use, or empty (flash ID derived from the card).
    const std::filesystem::path& companion_path() const noexcept { return companion_path_; }
    // Where commits persist (SRAM.pc.raw), or empty.
    const std::filesystem::path& settings_path() const noexcept { return settings_path_; }
    const std::array<u8, kSettingsSize>& settings() const noexcept { return settings_; }

    struct Stats {
        u64 sram_reads = 0;        // __OSInitSram
        u64 sram_unlocks = 0;      // UnlockSram calls
        u64 sram_commits = 0;      // ... with commit set
        u64 sram_persists = 0;     // commits that changed the image (durable writes)
        u64 sram_unchanged = 0;    // commits of an unchanged image (no write)
        u64 sram_failures = 0;     // durable writes that failed
        u64 uart_lines = 0;        // WriteUARTN calls that wrote
        u64 uart_bytes = 0;
        u64 uart_disabled = 0;     // WriteUARTN calls with the UART not enabled
    };
    const Stats& stats() const noexcept { return stats_; }

    // Every guest address the generator must route to an owner here
    // (extract/native-owners.txt).
    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept;

    // Savestate (gekko/savestate.h, version 15): the settings image.
    void save_state(std::vector<u8>& out) const;
    bool load_state(const u8*& cursor, const u8* end);

private:
    void init_sram(Cpu& c);
    // UnlockSram(commit, offset): returns Scb.sync.
    u32 unlock_sram(Cpu& c, bool commit, u32 offset);
    void persist_settings();
    void write_uart(Cpu& c);

    Cpu& cpu_;
    NativeInterrupts& interrupts_;
    std::array<u8, kSettingsSize> settings_{};
    std::filesystem::path companion_path_;
    std::filesystem::path settings_path_;
    Stats stats_;
};

}  // namespace gekko
