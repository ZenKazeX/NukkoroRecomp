// The OS's other external-interface clients, native. Contract and rationale:
// include/gekko/native_ipl.h and docs/native-time-20261005/STAGE5_EXI.md
// (section 3: the disassembly these follow).

#include "gekko/native_ipl.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <system_error>

#include "gekko/cpu.h"
#include "gekko/host_files.h"
#include "gekko/guest_memory.h"
#include "gekko/native_interrupts.h"
#include "gekko/savestate.h"

namespace gekko {

namespace {

// ---- the entry points the generator routes here (extract/native-owners.txt)
constexpr u32 kInitSram = 0x803776F4u;        // __OSInitSram (from OSInit)
constexpr u32 kUnlockSram = 0x803778E8u;      // UnlockSram(commit, offset)
constexpr u32 kReadRom = 0x80377C7Cu;         // __OSReadROM(buffer, length, offset)
constexpr u32 kInitMetroTrk = 0x8039F8E0u;    // InitMetroTRK (__start, debug monitor 2 or 3)
// InitMetroTRK's resume point after its mtmsr: the generator makes it a row of
// its own (an interrupt taken there resumes at it), and the address census
// roots that row through the dead blr at its end. Owned, it can root nothing.
constexpr u32 kInitMetroTrkResume = 0x8039F928u;
constexpr u32 kInitMetroTrkBba = 0x8039F978u; // InitMetroTRK_BBA (__start, debug monitor 4)
constexpr u32 kExiInit = 0x803B4CFCu;         // EXIInit (from OSInit)
constexpr u32 kWriteUartN = 0x803B5848u;      // WriteUARTN(buffer, length)

const NativeIpl::Address kAddresses[] = {
    {kInitSram, "__OSInitSram", false},
    {kUnlockSram, "UnlockSram", false},
    {kReadRom, "__OSReadROM", false},
    {kInitMetroTrk, "InitMetroTRK", false},
    {kInitMetroTrkResume, "InitMetroTRK: the resume point after its mtmsr", false},
    {kInitMetroTrkBba, "InitMetroTRK_BBA", false},
    {kExiInit, "EXIInit", false},
    {kWriteUartN, "WriteUARTN", false},
};

// ---- the OS SRAM control block (Scb)
constexpr u32 kScb = 0x80586FE0u;
constexpr u32 kSramBytes = 0x40u;     // Scb.sram[64]: OSSram (0x14), then OSSramEx
constexpr u32 kScbOffset = 0x40u;     // the first byte not yet written back
constexpr u32 kScbEnabled = 0x44u;    // the interrupt level the lock saved
constexpr u32 kScbLocked = 0x48u;
constexpr u32 kScbSync = 0x4Cu;
constexpr u32 kSramChecksum = 0x00u;  // u16 checkSum, u16 checkSumInv
constexpr u32 kSramSummed = 0x0Cu;    // counterBias .. flags: the checksummed words
constexpr u32 kSramFlags = 0x13u;
constexpr u32 kSramExBase = 0x14u;    // OSSramEx
constexpr u32 kSramGbs = 0x3Cu;       // OSSramEx.gbs (u16)

// ---- the debug UART's enable word (InitializeUART 0x803B57D8)
constexpr u32 kSdaUartEnabled = 0x5EE0u;
constexpr u32 kUartEnabled = 0xA5FF005Au;

// ---- the settings image: the RTC counter, then the SRAM
constexpr std::size_t kSettingsSram = 4u;
constexpr std::size_t kSettingsCardAFlashId = kSettingsSram + 0x14u;
constexpr std::size_t kSettingsCardAFlashIdChecksum = kSettingsSram + 0x3Au;

// The OS interrupt masks of EXI channels 0-2 (OS interrupts 9-16).
constexpr u32 kExiInterruptMasks = 0x007F8000u;

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "native IPL services: " + why);
}

// The factory settings: the zeroed English / interlaced body with its normal
// checksum pair, and stereo set - a fresh console ships with stereo, and the
// game's Options -> Sound Mode reads it through OSGetSoundMode.
void initialise_default_settings(std::array<u8, NativeIpl::kSettingsSize>& settings) {
    settings.fill(0);
    settings[0x04] = 0x00;
    settings[0x05] = 0x00;
    settings[0x06] = 0xff;
    settings[0x07] = 0xfc;
    settings[0x16] = 0;     // language: English
    settings[0x17] = 0x04;  // flags: stereo, interlaced, 4:3
}

// The IPL's card association: the first 12 bytes of the card header decoded
// with the serial seed in bytes 12-19 give card A's flash ID.
void set_card_a_flash_id(std::array<u8, NativeIpl::kSettingsSize>& settings, const std::vector<u8>& card_image) {
    if (card_image.size() < 20u) throw std::runtime_error("memory-card image is too short to derive its flash ID");
    u64 random = load_be64(card_image.data() + 12u);
    u8 checksum = 0;
    for (std::size_t index = 0; index < 12u; ++index) {
        random = (random * 0x41c64e6dull + 0x3039ull) >> 16u;
        const u8 flash_id = static_cast<u8>(card_image[index] - static_cast<u8>(random & 0xffu));
        settings[kSettingsCardAFlashId + index] = flash_id;
        checksum = static_cast<u8>(checksum + flash_id);
        random = (random * 0x41c64e6dull + 0x3039ull) >> 16u;
        random &= 0x7fffull;
    }
    settings[kSettingsCardAFlashIdChecksum] = static_cast<u8>(checksum ^ 0xffu);
}

bool card_a_matches(const std::vector<u8>& sram, const std::vector<u8>& card_image) {
    if (sram.size() != NativeIpl::kSettingsSize) return false;
    std::array<u8, NativeIpl::kSettingsSize> derived{};
    initialise_default_settings(derived);
    set_card_a_flash_id(derived, card_image);
    return std::equal(derived.begin() + kSettingsCardAFlashId, derived.begin() + kSettingsCardAFlashId + 12u,
                      sram.begin() + kSettingsCardAFlashId) &&
           derived[kSettingsCardAFlashIdChecksum] == sram[kSettingsCardAFlashIdChecksum];
}

std::filesystem::path find_companion(const std::filesystem::path& card_path) {
    std::filesystem::path directory = card_path.parent_path();
    for (unsigned depth = 0; depth < 6u && !directory.empty(); ++depth) {
        const std::filesystem::path candidate = directory / "SRAM.raw";
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            const auto size = std::filesystem::file_size(candidate, error);
            if (!error && size == NativeIpl::kSettingsSize) return candidate;
        }
        const std::filesystem::path parent = directory.parent_path();
        if (parent == directory) break;
        directory = parent;
    }
    return {};
}

// The gbs word's rule (OSSetGbsMode 0x803781E4 and UnlockSram's commit): two
// value families read as 0.
u16 normalised_gbs(u16 gbs) noexcept {
    return ((gbs & 0x7C00u) == 0x5000u || (gbs & 0xC0u) == 0xC0u) ? u16{0} : gbs;
}

}  // namespace

NativeIpl::NativeIpl(Cpu& cpu, NativeInterrupts& interrupts) noexcept : cpu_(cpu), interrupts_(interrupts) {
    initialise_default_settings(settings_);
}

const NativeIpl::Address* NativeIpl::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

// ---------------------------------------------------------------------------
// The settings image
// ---------------------------------------------------------------------------

void NativeIpl::reset_settings() {
    companion_path_.clear();
    settings_path_.clear();
    initialise_default_settings(settings_);
}

void NativeIpl::bind_save_folder(const std::filesystem::path& folder) {
    reset_settings();
    settings_path_ = folder / "SRAM.pc.raw";
    if (std::filesystem::exists(settings_path_)) {
        const std::vector<u8> persisted = read_host_file(settings_path_);
        if (persisted.size() == settings_.size()) std::copy(persisted.begin(), persisted.end(), settings_.begin());
    }
}

void NativeIpl::override_console_settings(int language, int stereo) {
    if (language >= 0 && language <= 5) settings_[kSettingsSram + 0x12u] = static_cast<u8>(language);
    if (stereo >= 0) {
        u8& flags = settings_[kSettingsSram + kSramFlags];
        flags = static_cast<u8>(stereo != 0 ? (flags | 0x04u) : (flags & ~0x04u));
    }
    // The OS checksum pair over counterBias .. flags (as the commit above writes it).
    u16 sum = 0u, inverse = 0u;
    for (u32 p = kSramSummed; p < kSramExBase; p += 2u) {
        const u16 word = static_cast<u16>(settings_[kSettingsSram + p] << 8 | settings_[kSettingsSram + p + 1u]);
        sum = static_cast<u16>(sum + word);
        inverse = static_cast<u16>(inverse + static_cast<u16>(~word));
    }
    settings_[kSettingsSram + kSramChecksum] = static_cast<u8>(sum >> 8);
    settings_[kSettingsSram + kSramChecksum + 1u] = static_cast<u8>(sum);
    settings_[kSettingsSram + kSramChecksum + 2u] = static_cast<u8>(inverse >> 8);
    settings_[kSettingsSram + kSramChecksum + 3u] = static_cast<u8>(inverse);
}

void NativeIpl::bind_card(const std::filesystem::path& card_source, const std::filesystem::path& card_backing,
                          const std::vector<u8>& card_image) {
    reset_settings();
    const std::filesystem::path companion = find_companion(card_source);
    if (!companion.empty()) {
        const std::vector<u8> bytes = read_host_file(companion);
        if (bytes.size() != settings_.size()) {
            throw std::runtime_error("SRAM companion must be exactly 68 bytes: " + companion.string());
        }
        if (card_a_matches(bytes, card_image)) {
            std::copy(bytes.begin(), bytes.end(), settings_.begin());
            companion_path_ = companion;
        } else {
            // A real SRAM from another console / card pairing would make the
            // mount report the card as damaged: derive this card's identity
            // with the IPL's algorithm instead.
            set_card_a_flash_id(settings_, card_image);
        }
    } else {
        set_card_a_flash_id(settings_, card_image);
    }
    // Settings the game writes - sound mode, language, video flags - persist
    // beside the working card, as on the console's flash; the card identity
    // is re-derived afterwards, so a persisted image cannot invalidate the mount.
    settings_path_ = card_backing.parent_path() / "SRAM.pc.raw";
    if (std::filesystem::exists(settings_path_)) {
        const std::vector<u8> persisted = read_host_file(settings_path_);
        if (persisted.size() == settings_.size()) std::copy(persisted.begin(), persisted.end(), settings_.begin());
    }
    set_card_a_flash_id(settings_, card_image);
}

// A failed write is counted and reported, never thrown into translated code:
// the guest sees its commit succeed either way, as with a real SRAM write.
void NativeIpl::persist_settings() {
    if (settings_path_.empty()) return;
    try {
        std::vector<u8> image;
        {
            save_trace::Stage stage("save.sram.serialize", settings_.size());
            image.assign(settings_.begin(), settings_.end());
        }
        write_host_file_atomically(settings_path_, image);
    } catch (const std::exception& error) {
        ++stats_.sram_failures;
        std::fprintf(stderr, "[ipl] could not write SRAM: %s\n", error.what());
    }
}

// ---------------------------------------------------------------------------
// The owners
// ---------------------------------------------------------------------------

// __OSInitSram: Scb.locked = Scb.enabled = FALSE; ReadSram (inlined: 64
// bytes into Scb.sram, Scb.sync = success); Scb.offset = 64;
// OSSetGbsMode(OSGetGbsMode()).
void NativeIpl::init_sram(Cpu& c) {
    ++stats_.sram_reads;
    c.mem_write32(kScb + kScbLocked, 0u);
    c.mem_write32(kScb + kScbEnabled, 0u);
    for (u32 i = 0; i < kSramBytes; ++i) c.mem_write8(kScb + i, settings_[kSettingsSram + i]);
    c.mem_write32(kScb + kScbSync, 1u);
    c.mem_write32(kScb + kScbOffset, kSramBytes);
    // OSGetGbsMode then OSSetGbsMode: each locks (Scb.enabled = the current
    // level) and unlocks; the setter commits only a changed word.
    const u32 level = (c.msr() & kMsrEE) != 0u ? 1u : 0u;
    const u16 gbs = c.mem_read16(kScb + kSramGbs);
    const u16 normalised = normalised_gbs(gbs);
    c.mem_write32(kScb + kScbEnabled, level);
    if (normalised != gbs) {
        c.mem_write16(kScb + kSramGbs, normalised);
        (void)unlock_sram(c, true, kSramExBase);
    }
}

u32 NativeIpl::unlock_sram(Cpu& c, bool commit, u32 offset) {
    ++stats_.sram_unlocks;
    if (commit) {
        ++stats_.sram_commits;
        if (offset == 0u) {
            const u8 flags = c.mem_read8(kScb + kSramFlags);
            if ((flags & 3u) > 2u) c.mem_write8(kScb + kSramFlags, static_cast<u8>(flags & ~3u));
            u16 sum = 0u;
            u16 inverse = 0u;
            for (u32 p = kSramSummed; p < kSramExBase; p += 2u) {
                const u16 word = c.mem_read16(kScb + p);
                sum = static_cast<u16>(sum + word);
                inverse = static_cast<u16>(inverse + static_cast<u16>(~word));
            }
            c.mem_write16(kScb + kSramChecksum, sum);
            c.mem_write16(kScb + kSramChecksum + 2u, inverse);
        }
        if (offset < c.mem_read32(kScb + kScbOffset)) c.mem_write32(kScb + kScbOffset, offset);
        if (c.mem_read32(kScb + kScbOffset) <= kSramExBase) {
            const u16 gbs = c.mem_read16(kScb + kSramGbs);
            if (normalised_gbs(gbs) != gbs) c.mem_write16(kScb + kSramGbs, 0u);
        }
        // WriteSram(Scb.sram + offset, offset, 64 - offset), inlined: the
        // bytes into the settings image (a host write when one changed).
        const u32 from = c.mem_read32(kScb + kScbOffset);
        bool changed = false;
        for (u32 i = from; i < kSramBytes; ++i) {
            const u8 value = c.mem_read8(kScb + i);
            if (settings_[kSettingsSram + i] != value) {
                settings_[kSettingsSram + i] = value;
                changed = true;
            }
        }
        if (changed) {
            ++stats_.sram_persists;
            save_trace::Stage request("save.sram.request", settings_.size(), c.lr);
            persist_settings();
        } else {
            ++stats_.sram_unchanged;
        }
        c.mem_write32(kScb + kScbSync, 1u);
        c.mem_write32(kScb + kScbOffset, kSramBytes);
    }
    c.mem_write32(kScb + kScbLocked, 0u);
    // OSRestoreInterrupts(Scb.enabled).
    const u32 msr = c.msr();
    c.set_msr(c.mem_read32(kScb + kScbEnabled) != 0u ? (msr | kMsrEE) : (msr & ~kMsrEE));
    return c.mem_read32(kScb + kScbSync);
}

// WriteUARTN: the enable check, the '\n' -> '\r' rewrite of the caller's
// buffer, success. (The EXI lock it took always succeeds: nothing else uses
// the channel.)
void NativeIpl::write_uart(Cpu& c) {
    const u32 buffer = c.gpr[3];
    const u32 length = c.gpr[4];
    if (c.mem_read32(c.gpr[13] + kSdaUartEnabled) != kUartEnabled) {
        ++stats_.uart_disabled;
        c.gpr[3] = 2u;
        return;
    }
    for (u32 i = 0; i < length; ++i) {
        if (c.mem_read8(buffer + i) == '\n') c.mem_write8(buffer + i, '\r');
    }
    ++stats_.uart_lines;
    stats_.uart_bytes += length;
    c.gpr[3] = 0u;
}

void NativeIpl::install() {
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, std::string("native IPL services: ") + name);
    };
    own(kExiInit, "EXIInit", [this](Cpu& c) {
        // The channels idle, their registers cleared, handlers registered for
        // OS interrupts 9-16 - none of which exists; only the OS mask the
        // original set is the OS's state.
        (void)interrupts_.mask_os_interrupts(c, kExiInterruptMasks);
    });
    own(kInitSram, "__OSInitSram", [this](Cpu& c) { init_sram(c); });
    own(kUnlockSram, "UnlockSram", [this](Cpu& c) { c.gpr[3] = unlock_sram(c, c.gpr[3] != 0u, c.gpr[4]); });
    own(kReadRom, "__OSReadROM", [](Cpu& c) {
        stop(c, "__OSReadROM(" + std::to_string(c.gpr[4]) + " bytes): this program has no boot ROM (only the "
                "disc-error screen's font reads it, and the native file API never reports a drive error)");
    });
    own(kWriteUartN, "WriteUARTN", [this](Cpu& c) { write_uart(c); });
    own(kInitMetroTrk, "InitMetroTRK", [](Cpu& c) {
        stop(c, "InitMetroTRK: the boot information names a debug monitor, and this program has none");
    });
    own(kInitMetroTrkResume, "InitMetroTRK resume", [](Cpu& c) {
        stop(c, "InitMetroTRK's interior was entered (the resume point after its mtmsr): the debug-monitor "
                "hand-off is owned and never runs its original body");
    });
    own(kInitMetroTrkBba, "InitMetroTRK_BBA", [](Cpu& c) {
        stop(c, "InitMetroTRK_BBA: the boot information names a debug monitor, and this program has none");
    });
}

void NativeIpl::save_state(std::vector<u8>& out) const { state::put_block(out, settings_.data(), settings_.size()); }

bool NativeIpl::load_state(const u8*& cursor, const u8* end) {
    return state::get_block(cursor, end, settings_.data(), settings_.size());
}

}  // namespace gekko
