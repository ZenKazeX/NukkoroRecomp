// The game's saves on the host (gekko/save_store.h).

#include "gekko/save_store.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <system_error>
#include <vector>

#include "gekko/cpu.h"
#include "gekko/gc_addr.h"
#include "gekko/guest_memory.h"
#include "gekko/pc_settings.h"

namespace gekko {

namespace {

// ---- the runner (static RE 2026-11-05, docs/SAVE_REROUTE_PLAN.md)
constexpr u32 kStart = 0x8029BF84u;   // (R, job) -> 1 started / 0 busy
constexpr u32 kTick = 0x8029BA8Cu;    // (R) -> busy
// R (0x38 bytes)
constexpr u32 kCtx = 0x00u;
constexpr u32 kCardState = 0x08u;     // the latest probe: 0 present, 3 no card
constexpr u32 kOp = 0x0Cu;
constexpr u32 kIssued = 0x10u;
constexpr u32 kBusy = 0x14u;
constexpr u32 kJob = 0x18u;
constexpr u32 kResult = 0x1Cu;
constexpr u32 kFlags = 0x20u;         // 1 mounted, 2 open, 4 status (the close path reads them)
constexpr u32 kLength = 0x24u;
constexpr u32 kBuffer = 0x28u;
constexpr u32 kFreeBytes = 0x30u;
constexpr u32 kFreeEntries = 0x34u;
// ctx (0xBC bytes)
constexpr u32 kCtxName = 0x08u;       // char[32]
constexpr u32 kCtxCount = 0xACu;      // the scan's matches
constexpr u32 kCtxCompany = 0xB4u;    // "8P"
constexpr u32 kCtxGame = 0xB6u;       // "GUPP"
// jobs
enum Job : u32 { kFormat = 1, kCreate = 2, kDelete = 3, kRead = 4, kOverwrite = 5, kFreeBlocks = 6, kScan = 7 };
// results (R + 0x1C)
constexpr s32 kDone = 0, kGeneric = 2, kIoError = 5, kNoSpace = 7, kInterrupted = 8, kNoFile = 9, kExists = 10;

// ---- the CARD calls left outside the runner
constexpr u32 kCardProbeEx = 0x80390D90u;
constexpr u32 kCardGetResultCode = 0x8038E044u;
constexpr u32 kCardInit = 0x8038DE3Cu;        // CARDInit(): the game's init (0x801E258C)
constexpr u32 kDspInit = 0x8038C2C0u;         // owned by the audio layer; the one shared effect of CARDInit
constexpr u32 kCardMountAsync = 0x80391498u;
constexpr u32 kCardUnmount = 0x803916D4u;
constexpr u32 kCardRead = 0x8038F56Cu;        // __CARDRead (as native_card.cpp owns it)
constexpr u32 kCardWrite = 0x8038F6B8u;       // __CARDWrite
constexpr u32 kCardEraseSector = 0x8038DD2Cu; // __CARDEraseSector

// ---- the options
constexpr u32 kGameDataPointer = 0x2BB4u;  // r13 + : GameData (0x800BBB34's singleton), the record at + 0
constexpr u32 kAfterBootLoad = 0x8035AA28u;
constexpr u32 kAfterOptionsLoad = 0x8035B414u;
constexpr u32 kOptionsExit = 0x802B90B0u;  // Options state 5 (impl): substate impl + 8, motion busy impl + 0x1C
constexpr u32 kSaveEnabled = 0x8057E7DDu;  // G + 0x7D (byte)
constexpr u32 kVoiceLanguage = 0x8057E7BCu;  // G + 0x5C: 0 Japanese, 1 English
constexpr u32 kTextLanguage = 0x8057E7B8u;   // G + 0x58: 0 JP, 1 EN, 2 FR, 3 SP, 4 GE, 5 IT
constexpr u32 kTextLanguageSda = 0x2D20u;    // r13 + : the same value, which the text readers use
// The boot language setter (reached through a vtable; static RE 2026-11-05): when its object's config text (+ 0x2E04)
// has a "key: value" entry, the language is OSGetLanguage's (the SRAM byte, Scb 0x80586FE0 + 0x12) in the game's
// numbering. Before any save loads, so the front end's text follows it.
constexpr u32 kBootLanguage = 0x801DFA98u;
constexpr u32 kScbLanguage = 0x80586FE0u + 0x12u;

// A 4-Mbit card's 59 user blocks, the file's 2 among them when it exists; 127 directory entries.
constexpr u32 kUserBlocks = 59u;

u32 seconds_since_2000() {
    using namespace std::chrono;
    const auto now = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    constexpr long long k2000 = 946684800;  // 2000-01-01 00:00:00 UTC
    return static_cast<u32>(now > k2000 ? now - k2000 : 0);
}

void put_be16(u8* p, u16 v) {
    p[0] = static_cast<u8>(v >> 8);
    p[1] = static_cast<u8>(v);
}
void put_be32(u8* p, u32 v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<u8>(v >> (24 - 8 * i));
}
u32 get_be32(const u8* p) { return static_cast<u32>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

}  // namespace

SaveStore::SaveStore(Cpu& cpu, GuestMemory& memory) noexcept : cpu_(cpu), memory_(memory) {}

void SaveStore::set_path(std::filesystem::path path) {
    path_ = std::move(path);
    file_.reset();
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) return;
    save_file::Gci gci;
    std::string error;
    if (save_file::read_gci(path_, gci, &error)) {
        file_ = std::move(gci);
    } else {
        // Not readable as a save: the game sees no file (it offers to create one); the file is left as it is.
        last_error_ = path_.string() + ": " + error;
        std::cerr << "Save file unreadable, ignored: " << last_error_ << "\n";
    }
}

std::string SaveStore::guest_text(u32 address, std::size_t max) const {
    std::string s;
    for (std::size_t i = 0; i < max; ++i) {
        const u8 b = cpu_.mem_read8(address + static_cast<u32>(i));
        if (b == 0u) break;
        s.push_back(static_cast<char>(b));
    }
    return s;
}

s32 SaveStore::write_file(const save_file::Gci& gci) {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    const std::filesystem::space_info space = std::filesystem::space(path_.parent_path(), ec);
    if (!ec && space.available < gci.data.size() + save_file::kGciHeaderBytes + 4096u) {
        ++stats_.write_failures;
        last_error_ = path_.string() + ": not enough space on the drive";
        return kNoSpace;
    }
    std::string error;
    if (!save_file::write_gci(path_, gci, &error)) {
        ++stats_.write_failures;
        last_error_ = error;
        std::cerr << "Save write failed: " << error << "\n";
        return kIoError;
    }
    ++stats_.writes;
    file_ = gci;
    return kDone;
}

s32 SaveStore::run_job(u32 runner, u32 job) {
    const u32 ctx = cpu_.mem_read32(runner + kCtx);
    const std::string name = guest_text(ctx + kCtxName, 32);
    const std::string game = guest_text(ctx + kCtxGame, 4);
    const std::string company = guest_text(ctx + kCtxCompany, 2);
    const u32 length = cpu_.mem_read32(runner + kLength);
    const u32 buffer = cpu_.mem_read32(runner + kBuffer);
    // The one file, as the CARD library would find it by name in this game's directory entries.
    const bool ours = file_.has_value() && file_->game_code() == game && file_->maker_code() == company;
    const bool named = ours && file_->file_name() == name;
    const bool interrupted = named && get_be32(file_->header.data() + 0x2C) == 0xFFFFFFFFu;
    const auto buffer_ok = [&](u32 bytes) { return bytes != 0u && memory_.mapped(GcAddr(buffer), bytes); };

    switch (job) {
    case kFormat: {
        ++stats_.formats;
        std::error_code ec;
        if (std::filesystem::exists(path_, ec) && !std::filesystem::remove(path_, ec)) {
            last_error_ = path_.string() + ": cannot be removed";
            return kIoError;
        }
        file_.reset();
        return kDone;
    }
    case kCreate: {
        ++stats_.creates;
        if (named) return kExists;
        if (length == 0u || length % save_file::kBlockBytes != 0u || !buffer_ok(length)) return kGeneric;
        // The directory entry the CARD library leaves for this file (create + write + the status fill, 0x8029A9FC):
        // checked against the user's own GCI, 2026-11-05.
        save_file::Gci gci;
        u8* h = gci.header.data();
        std::memcpy(h, game.data(), std::min<std::size_t>(game.size(), 4));
        std::memcpy(h + 4, company.data(), std::min<std::size_t>(company.size(), 2));
        h[6] = 0xFF;
        h[7] = 0x02;                                          // banner RGB5A3
        std::memcpy(h + 8, name.data(), std::min<std::size_t>(name.size(), 32));
        put_be32(h + 0x28, seconds_since_2000());             // modified
        put_be32(h + 0x2C, 0x40u);                            // iconAddr
        put_be16(h + 0x30, 0x0002u);                          // icon 0 RGB5A3
        put_be16(h + 0x32, 0x0002u);                          // icon 0 middle speed, icon 1 the end
        h[0x34] = 0x04;                                       // public
        h[0x35] = 0x00;                                       // copies
        put_be16(h + 0x36, 5u);                               // first block (a fresh card's)
        put_be16(h + 0x38, static_cast<u16>(length / save_file::kBlockBytes));
        put_be16(h + 0x3A, 0xFFFFu);
        put_be32(h + 0x3C, 0u);                               // commentAddr
        gci.data.resize(length);
        memory_.read_block(buffer, gci.data.data(), length);
        return write_file(gci);
    }
    case kDelete: {
        ++stats_.deletes;
        if (!named) return kNoFile;
        std::error_code ec;
        if (!std::filesystem::remove(path_, ec) && ec) {
            last_error_ = path_.string() + ": cannot be removed";
            return kIoError;
        }
        file_.reset();
        return kDone;
    }
    case kRead: {
        ++stats_.reads;
        if (!named) return kNoFile;
        if (interrupted) return kInterrupted;
        if (length > file_->data.size()) return kGeneric;  // CARD_RESULT_LIMIT
        if (!buffer_ok(length)) return kGeneric;
        memory_.write_block(buffer, file_->data.data(), length);
        return kDone;
    }
    case kOverwrite: {
        ++stats_.overwrites;
        if (!named) return kNoFile;
        if (interrupted) return kInterrupted;
        if (length > file_->data.size() || !buffer_ok(length)) return kGeneric;
        save_file::Gci gci = *file_;
        memory_.read_block(buffer, gci.data.data(), length);
        put_be32(gci.header.data() + 0x28, seconds_since_2000());
        return write_file(gci);
    }
    case kFreeBlocks: {
        ++stats_.free_queries;
        const u32 used = file_ ? file_->block_count() : 0u;
        cpu_.mem_write32(runner + kFreeBytes, (kUserBlocks - std::min(used, kUserBlocks)) * save_file::kBlockBytes);
        cpu_.mem_write32(runner + kFreeEntries, file_ ? 126u : 127u);
        return kDone;
    }
    case kScan: {
        ++stats_.scans;
        // strstr(stat.fileName, name) over this game's entries.
        const u32 count = ours && file_->file_name().find(name) != std::string::npos ? 1u : 0u;
        cpu_.mem_write32(ctx + kCtxCount, count);
        return kDone;
    }
    default:
        return kGeneric;
    }
}

void SaveStore::start(Cpu& c) {
    const u32 runner = c.gpr[3];
    const u32 job = c.gpr[4];
    if (c.mem_read32(runner + kBusy) != 0u) {
        ++stats_.refused_starts;
        c.gpr[3] = 0u;
        return;
    }
    c.mem_write32(runner + kCardState, 0u);
    c.mem_write32(runner + kBusy, 1u);
    c.mem_write32(runner + kJob, job);
    c.mem_write32(runner + kResult, 1u);  // in progress
    ++stats_.starts;
    c.gpr[3] = 1u;
}

void SaveStore::tick(Cpu& c) {
    const u32 runner = c.gpr[3];
    ++stats_.ticks;
    restore_save_flag();
    // The player's Autosave off (PcSettings autosave 0, the Save Data page): the game's own "save enabled" (G + 0x7D,
    // which its boot check, loads and saves set) is kept clear, so no stage-clear or Options save runs. On / not set:
    // the game's value, as always (forcing it on without a save file would make every save fail).
    if (pc_settings().current().autosave == 0 && c.mem_read8(kSaveEnabled) != 0u) c.mem_write8(kSaveEnabled, 0u);
    c.mem_write32(runner + kCardState, 0u);  // the host file's "card" is always present
    if (c.mem_read32(runner + kBusy) != 0u) {
        const u32 job = c.mem_read32(runner + kJob);
        const s32 result = run_job(runner, job);
        if (result != kDone) ++stats_.failures;
        stats_.last_result = result;
        // The finish paths (0x8029B9C0 done / 0x8029B94C failed): idle, closed, unmounted.
        c.mem_write32(runner + kBusy, 0u);
        c.mem_write32(runner + kJob, 0u);
        c.mem_write32(runner + kResult, static_cast<u32>(result));
        c.mem_write32(runner + kFlags, 0u);
        c.mem_write32(runner + kOp, result == kDone ? 1u : 2u);
        c.mem_write32(runner + kIssued, 1u);
    }
    c.gpr[3] = c.mem_read32(runner + kBusy);
}

// After a load, before the game applies the record's options: the PC settings' values over the record's, or - not
// set yet - the record's taken into the PC settings once.
void SaveStore::after_load(Cpu& c) {
    const u32 data = c.mem_read32(c.gpr[13] + kGameDataPointer);
    if (data == 0u) return;
    PcSettings pc = pc_settings().current();
    bool taken = false;
    if (pc.language < 0) pc.language = static_cast<int>(c.mem_read32(data + 0x18)), taken = true;
    if (pc.speaker < 0) pc.speaker = static_cast<int>(c.mem_read32(data + 0x20)), taken = true;
    if (pc.vibration < 0) pc.vibration = c.mem_read8(data + 0x24) != 0u ? 1 : 0, taken = true;
    if (pc.voice < 0) pc.voice = c.mem_read32(data + 0x1C) == 0u ? 0 : 1, taken = true;
    if (taken) {
        std::string error;
        if (pc_settings().apply(pc, &error) && pc_settings().save(&error)) ++stats_.options_taken;
        else std::cerr << "Options from the save could not be kept: " << error << "\n";
    }
    // 6 = Hungarian: the game runs in English (1), its fonts / texts swapped at the mount; the save keeps 1 so it
    // stays valid on a console or in Dolphin.
    if (pc.language >= 0 && pc.language <= 6)
        c.mem_write32(data + 0x18, static_cast<u32>(pc.language == 6 ? 1 : pc.language));
    if (pc.speaker >= 0 && pc.speaker <= 2) c.mem_write32(data + 0x20, static_cast<u32>(pc.speaker));
    if (pc.vibration >= 0) c.mem_write8(data + 0x24, pc.vibration != 0 ? 1u : 0u);
    // The voice language (0 Japanese, 1 English): the record's + 0x1C and the game's G + 0x5C, which the loaders
    // never set on PAL (the event voices sng_EV*_J / _E follow G + 0x5C when they play).
    if (pc.voice >= 0 && pc.voice <= 1) {
        c.mem_write32(data + 0x1C, static_cast<u32>(pc.voice));
        c.mem_write32(kVoiceLanguage, static_cast<u32>(pc.voice));
    }
    // Autosave (the Save Data page): set over the game's "save enabled" once a save has loaded (so "on" never asks
    // for saves to a missing file); not set = the game's own value. "Off" is also held every frame (tick).
    if (pc.autosave >= 0) c.mem_write8(kSaveEnabled, pc.autosave != 0 ? 1u : 0u);
    ++stats_.options_applied;
}

// The boot language setter 0x801DFA98, owned: its own condition (a ':' in the object's config text followed, after
// any spaces, by something other than the end of the line), then the PC settings' language when set - Japanese too,
// which the console's SRAM cannot hold - else OSGetLanguage's mapped as the original maps it (EN 1, DE 4, FR 2,
// SP 3, IT 5, anything else 1). OSGetLanguage's SRAM lock and unlock change nothing the game sees.
void SaveStore::boot_language(Cpu& c) {
    const u32 object = c.gpr[3];
    u32 p = c.mem_read32(object + 0x2E04u);
    bool found = false;
    for (;;) {
        const s8 ch = static_cast<s8>(c.mem_read8(p));
        if (ch == 0) break;
        if (ch == ':') {
            ++p;
            for (;;) {
                const s8 d = static_cast<s8>(c.mem_read8(p));
                if (d == 0 || d == '\n') break;
                ++p;
                if (c.mem_read8(p) != static_cast<u8>(' ')) {
                    found = true;
                    break;
                }
            }
            break;
        }
        ++p;
    }
    if (!found) return;
    const int pc = pc_settings().current().language;
    u32 language = 1u;
    if (pc >= 0 && pc <= 6) {
        language = static_cast<u32>(pc == 6 ? 1 : pc);  // 6 = Hungarian: the game's English, fonts swapped at mount
    } else {
        static constexpr u32 kFromOs[5] = {1u, 4u, 2u, 3u, 5u};
        const u32 os = c.mem_read8(kScbLanguage);
        language = os < 5u ? kFromOs[os] : 1u;
    }
    c.mem_write32(c.gpr[13] + kTextLanguageSda, language);
    c.mem_write32(kTextLanguage, language);
    ++stats_.boot_languages;
}

// Options' exit (state 5): at the frame it decides whether to save (substate 1, motions done), "save enabled" reads
// clear, so it fades out without the card save. Restored at the next frame (the runner's tick or this entry).
void SaveStore::options_exit(Cpu& c) {
    restore_save_flag();
    const u32 impl = c.gpr[3];
    if (c.mem_read32(impl + 8u) != 1u || c.mem_read8(impl + 0x1Cu) != 0u) return;
    save_flag_ = c.mem_read8(kSaveEnabled);
    if (save_flag_ == 0u) return;
    c.mem_write8(kSaveEnabled, 0u);
    save_flag_cleared_ = true;
    ++stats_.options_exit_saves_skipped;
}

void SaveStore::restore_save_flag() {
    if (!save_flag_cleared_) return;
    cpu_.mem_write8(kSaveEnabled, save_flag_);
    save_flag_cleared_ = false;
}

namespace {
const SaveStore::Address kOwnedAddresses[] = {
    {kStart, "card job runner: start", false},
    {kTick, "card job runner: tick", false},
    {kCardInit, "CARDInit", false},
    {kCardGetResultCode, "CARDGetResultCode", false},
};
}  // namespace

const SaveStore::Address* SaveStore::addresses(std::size_t& count) noexcept {
    count = sizeof(kOwnedAddresses) / sizeof(kOwnedAddresses[0]);
    return kOwnedAddresses;
}

void SaveStore::install() {
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, std::string("save store: ") + name);
    };
    const auto observe = [&cpu](u32 address, const char* name, auto&& see) {
        cpu.install_hle(address, [see](Cpu& c) {
            see(c);
            return false;  // the game's own body runs
        }, std::string("save store: ") + name);
    };
    own(kStart, "card job start", [this](Cpu& c) { start(c); });
    own(kTick, "card job runner", [this](Cpu& c) { tick(c); });
    own(kCardProbeEx, "CARDProbeEx", [this](Cpu& c) {
        // CheckModule's probe of slot A (chan, memSize *, sectorSize *): present.
        if (c.gpr[4] != 0u) c.mem_write32(c.gpr[4], 4u);       // 4 Mbit
        if (c.gpr[5] != 0u) c.mem_write32(c.gpr[5], 0x2000u);  // 8 KiB sectors
        c.gpr[3] = 0u;
    });
    // CARDGetResultCode(chan): the original returns -128 (FATAL_ERROR) outside channels 0..1, else the channel's
    // block result (0x805A2CA0 + 0x110 * chan + 4). The store is an idle card in slot A (READY: a job ends inside
    // its tick, so the BUSY (-1) the one caller - the reset path, 0x8032D304 - waits on never shows) and nothing in
    // slot B (NOCARD, what CARDInit leaves there).
    own(kCardGetResultCode, "CARDGetResultCode", [](Cpu& c) {
        const u32 chan = c.gpr[3];
        c.gpr[3] = chan == 0u ? 0u : (chan == 1u ? static_cast<u32>(-3) : static_cast<u32>(-128));
    });
    // CARDInit: the card library's one-time set-up. Everything it writes is the library's private state (the two
    // __CARDBlock records at 0x805A2CA0, the encoding word at r13 + 0x5DF0, a reset-function record) or text (the
    // version line OSRegisterVersion reports), which nothing left reads (docs/agents/card-saves.md); OSInitAlarm is
    // a no-op after OSInit's call. What stays is DSPInit, whose first call this was: the audio layer's owner.
    own(kCardInit, "CARDInit", [](Cpu& c) { c.try_hle(kDspInit); });
    const auto stray = [this](const char* what) {
        return [this, what](Cpu& c) {
            if (stats_.stray_card_calls++ == 0u) std::cerr << "save store: unexpected " << what << " call\n";
            c.gpr[3] = static_cast<u32>(-3);  // CARD_RESULT_NOCARD
        };
    };
    own(kCardMountAsync, "CARDMountAsync", stray("CARDMountAsync"));
    own(kCardUnmount, "CARDUnmount", [](Cpu& c) { c.gpr[3] = 0u; });
    own(kCardRead, "__CARDRead", stray("__CARDRead"));
    own(kCardWrite, "__CARDWrite", stray("__CARDWrite"));
    own(kCardEraseSector, "__CARDEraseSector", stray("__CARDEraseSector"));
    own(kBootLanguage, "the boot language (the PC settings' when set)", [this](Cpu& c) { boot_language(c); });
    observe(kAfterBootLoad, "options after the boot load", [this](Cpu& c) { after_load(c); });
    observe(kAfterOptionsLoad, "options after Options' load", [this](Cpu& c) { after_load(c); });
    observe(kOptionsExit, "Options' exit without the card save", [this](Cpu& c) { options_exit(c); });
}

}  // namespace gekko
