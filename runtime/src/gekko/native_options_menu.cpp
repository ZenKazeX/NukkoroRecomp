// The PC Options menu. Contract: include/gekko/native_options_menu.h.

#include "gekko/native_options_menu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "gekko/cpu.h"
#include "gekko/game_clock.h"
#include "gekko/recomp.h"

namespace gekko::options_menu {

namespace od = options_display;
namespace st = settings;

namespace {

// The screen's menu sounds (0x801D59AC ids).
constexpr u32 kSoundCursor = 0x1E002u;
constexpr u32 kSoundDecide = 0x1E003u;
constexpr u32 kSoundBack = 0x1E004u;

constexpr u32 kButtonA = 1u, kButtonB = 2u, kButtonUp = 3u, kButtonDown = 4u, kButtonLeft = 5u, kButtonRight = 6u;

// impl fields (AdvReal::Option::Phase impl, docs/OPTIONS_MENU_ARCHITECTURE.md).
constexpr u32 kState = 0x00u, kSubState = 0x08u;
constexpr u32 kHiddenSubtitle = 0x24u, kNoJapanese = 0x25u, kHiddenAudio = 0x26u;
constexpr u32 kTextOn = 0x1Eu, kTrackPlayed = 0x30u, kRepeatCountdown = 0x34u;
// The original items' value slots (cursor + 0x20 + 4 * item) the game's apply 0x802B85D8 reads.
constexpr u32 kSlotVibration = kCursor + 0x24u, kSlotSpeaker = kCursor + 0x28u, kSlotLanguage = kCursor + 0x2Cu;
constexpr u32 kSlotVoice = kCursor + 0x34u;  // item 5, the voice language (0 Japanese, 1 English)
constexpr u32 kSnapshotLanguage = kCursor + 0x64u;  // the language slot's snapshot (cursor + 0x58 + 4 * item 3)
constexpr u32 kSnapshotVoice = kCursor + 0x6Cu;     // the voice slot's snapshot (cursor + 0x58 + 4 * item 5)
constexpr u32 kStateChanged = kCursor + 0x70u;
// The game's functions this menu calls (each as the game's own page calls it).
constexpr u32 kFadeOut = 0x80173C5Cu;        // (0, 1; f1 = 1.0): the fade the Subtitle page starts
// The sound test reads the game's data natively and plays through the game's own page handler (see edge_press): no
// nested guest call. kSaveDataPointer: r13 + 0x2BB4 = 0x800BBB34's singleton; kTrackTotal: 0x801E5348's constant.
constexpr u32 kSaveDataPointer = 0x2BB4u;
constexpr u32 kTrackTotal = 0x37u;
constexpr u32 kSoundTestSlot = kCursor + 0x20u;  // item 0's value slot: the rank of the track 0x802B9850 plays
// The game's speaker output names (the System page's values).
constexpr u32 kSpeakerNames[] = {5417u, 5418u, 5419u};

constexpr const char* kDefaultsId = "menu.defaults";
constexpr u32 kDefaultsIndex = kRowsPerCategory - 1u;

// The menu's texts in the six languages (tools/options_menu_sim/i18n, emitted by emit_menu_texts_inc.py): every label,
// description and dialog word the player sees, keyed by setting id ("audio.voice.label", "audio.voice.help") or by
// menu key. The registry's English texts are the fallback for a key the table lacks.
#include "options_menu_texts.inc"

// The text keys of the six categories, and their setting ids' prefixes (the fifth is the registry's "gameplay"
// category, shown as Save Data).
const char* const kCategoryNames[kCategoryCount] = {"display", "graphics", "audio", "controls", "savedata", "system"};
const char* const kCategoryPrefixes[kCategoryCount] = {"display", "graphics", "audio", "controls", "gameplay", "system"};
bool has_defaults_row(u32 category) { return category != 5u; }  // System has "Restore all defaults" instead

// The texts table's column: the front-end language's, or 6 for Hungarian (which runs on the English slot).
u32 column_of(od::Language language, bool hungarian) {
    return language == od::Language::EN && hungarian ? 6u : static_cast<u32>(language) % 6u;
}

// A text of the table in `column`, else in English; nullptr when the table has no such key.
const char* menu_text(u32 column, std::string_view key) {
    for (const MenuText& t : kMenuTexts) {
        if (key != t.key) continue;
        const char* own = t.text[column % 7u];
        return own != nullptr ? own : t.text[0];
    }
    return nullptr;
}

std::u16string text16(u32 column, std::string_view key, const std::string& fallback) {
    const char* t = menu_text(column, key);
    return od::utf16(t != nullptr ? t : fallback.c_str());
}

std::u16string ascii16(const std::string& s) { return std::u16string(s.begin(), s.end()); }

// The front-end language of the game's text language (settings + 0x58: 0 JP, 1 EN, 2 FR, 3 SP, 4 GE, 5 IT).
od::Language language_of_game(u32 text_language) {
    static constexpr od::Language kByGame[] = {od::Language::JP, od::Language::EN, od::Language::FR,
                                               od::Language::SP, od::Language::GE, od::Language::IT};
    return kByGame[text_language < 6u ? text_language : 1u];
}

struct Tables {
    std::mutex mutex;
    std::array<std::vector<u8>, 6> tables;
    std::vector<u8> hungarian;  // the Hungarian variant of the English table
};
Tables& tables() {
    static Tables t;
    return t;
}

std::u16string game_message(u32 column, u32 id) {
    std::u16string text;
    std::lock_guard<std::mutex> lock(tables().mutex);
    const std::vector<u8>& table =
        column == 6u && !tables().hungarian.empty() ? tables().hungarian : tables().tables[column % 6u];
    od::message_by_id(table, id, text);
    return text;
}

st::SettingView defaults_row(u32 category) {
    st::SettingView v;
    v.id = kDefaultsId;
    v.kind = st::Kind::Action;
    v.english_label = "Restore defaults";
    const char* help = menu_text(0u, std::string(kCategoryPrefixes[category % kCategoryCount]) + ".defaults.help");
    v.english_help = help != nullptr ? help : "";
    return v;
}

}  // namespace

std::vector<std::pair<u32, std::u16string>> help_messages(od::Language language, bool hungarian) {
    // The descriptions in `language` (English for a key it lacks), inserted into that language's front-end table.
    const u32 column = column_of(language, hungarian);
    std::vector<std::pair<u32, std::u16string>> out;
    for (u32 c = 0; c < kCategoryCount; ++c) {
        out.emplace_back(kCategoryHelpBase + c,
                         text16(column, std::string("category.") + kCategoryNames[c] + ".help", ""));
        const std::vector<st::SettingView> list = st::registry().list(static_cast<st::Category>(c));
        for (u32 i = 0; i < list.size() && i < kDefaultsIndex; ++i)
            out.emplace_back(help_id(c, i), text16(column, list[i].id + ".help", list[i].english_help));
        if (has_defaults_row(c))
            out.emplace_back(help_id(c, kDefaultsIndex),
                             text16(column, std::string(kCategoryPrefixes[c]) + ".defaults.help", ""));
    }
    out.emplace_back(kConfirmHelp, text16(column, "menu.confirm.help", ""));
    out.emplace_back(kSoundTestHelp, text16(column, "menu.sound_test.help", ""));
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

void set_front_end_table(od::Language language, std::vector<u8> table) {
    std::lock_guard<std::mutex> lock(tables().mutex);
    tables().tables[static_cast<u32>(language) % 6u] = std::move(table);
}

void set_hungarian_front_end_table(std::vector<u8> table) {
    std::lock_guard<std::mutex> lock(tables().mutex);
    tables().hungarian = std::move(table);
}

// ---------------------------------------------------------------------------------------------------------------

struct Menu::Attach {
    Menu& menu;
    Attach(Menu& m, Cpu& cpu) : menu(m) { menu.cpu_ = &cpu; }
    ~Attach() { menu.cpu_ = nullptr; }
};

Menu::Menu() { st::set_game_options(this); }
Menu::~Menu() { st::set_game_options(nullptr); }

void Menu::set_font_files(od::Language language, std::vector<u8> txd, std::vector<std::vector<u8>> glyph_lists,
                          std::u16string on, std::u16string off) {
    FontFiles& f = files_[static_cast<u32>(language) % 6u];
    f.txd = std::move(txd);
    f.glyph_lists = std::move(glyph_lists);
    f.on = std::move(on);
    f.off = std::move(off);
}

void Menu::set_track_names(od::Language language, std::vector<std::u16string> names) {
    tracks_[static_cast<u32>(language) % 6u] = std::move(names);
}

void Menu::set_hungarian_font_files(std::vector<u8> txd, std::vector<std::vector<u8>> glyph_lists, std::u16string on,
                                    std::u16string off, std::vector<std::u16string> track_names) {
    std::lock_guard<std::mutex> lock(hungarian_mutex_);
    hungarian_files_.txd = std::move(txd);
    hungarian_files_.glyph_lists = std::move(glyph_lists);
    hungarian_files_.on = std::move(on);
    hungarian_files_.off = std::move(off);
    hungarian_tracks_ = std::move(track_names);
}

bool Menu::hungarian() const {
    if (language_ != od::Language::EN || !st::hungarian_mounted()) return false;
    std::lock_guard<std::mutex> lock(hungarian_mutex_);
    return !hungarian_files_.glyph_lists.empty();
}

u32 Menu::column() const { return column_of(language_, hungarian()); }

bool Menu::call(Cpu& cpu, u32 function, u32 r3, u32 r4) {
    const u32 link = cpu.lr;
    cpu.gpr[3] = r3;
    cpu.gpr[4] = r4;
    cpu.gpr[12] = function;
    cpu.ctr = function;
    recomp::call_indirect(cpu, function, link);
    if (cpu.exiting()) cpu.finish_transfers();
    const bool ok = !cpu.exiting() && cpu.pc == link;
    cpu.lr = link;
    if (!ok) last_error_ = "a guest call did not return";
    return ok;
}

u32 Menu::current_item(Cpu& cpu) const {
    const u32 row = cpu.mem_read32(kCursor), column = cpu.mem_read32(kCursor + 4u);
    if (row >= 3u || column >= 2u) return ~0u;
    return cpu.mem_read32(kGrid + row * 8u + column * 4u);
}

std::vector<st::SettingView> Menu::rows() const {
    std::vector<st::SettingView> list = st::registry().list(static_cast<st::Category>(item_));
    if (list.size() > kDefaultsIndex) list.resize(kDefaultsIndex);
    if (has_defaults_row(item_)) list.push_back(defaults_row(item_));
    return list;
}

std::u16string Menu::label_text(const st::SettingView& v) const {
    return text16(column(), v.id + ".label", v.english_label);  // "menu.defaults.label" for the defaults row
}

std::u16string Menu::value_text(const st::SettingView& v, bool pending) const {
    if (v.kind == st::Kind::Action) return {};
    if (v.kind == st::Kind::Slider) return ascii16(std::to_string(pending ? v.pending : v.applied) + "%");
    const int index = pending ? v.pending : v.applied;
    if (index < 0 || index >= static_cast<int>(v.choices.size())) return {};
    const st::ChoiceView& c = v.choices[static_cast<std::size_t>(index)];
    // The game's own words where it has them (ON / OFF, the speaker output names), the menu's table for the others.
    if (c.key == "on") return on_off_[0];
    if (c.key == "off") return on_off_[1];
    if (v.id == "audio.speaker" && index < 3) {
        const std::u16string s = game_message(column(), kSpeakerNames[index]);
        if (!s.empty()) return s;
    }
    if (v.id == "display.mode" && (c.key == "windowed" || c.key == "borderless" || c.key == "exclusive"))
        return text16(column(), "word." + c.key, c.english);
    if (c.key == "auto") return text16(column(), "word.auto", c.english);
    // Language names in the menu's own language (lang.<key>: Hungarian "Angol", "Japán"...; the other menu languages
    // keep the native names of the English column).
    if (v.id == "audio.subtitles" || v.id == "audio.voice") return text16(column(), "lang." + c.key, c.english);
    if (v.id == "graphics.msaa" || v.id == "graphics.anisotropy") {
        if (c.key == "1") return on_off_[1];
    }
    return od::utf16(c.english.c_str());
}

// ---------------------------------------------------------------------------------------------------------------
// settings::GameOptions

unsigned Menu::speaker() const { return cpu_ != nullptr ? std::min<u32>(cpu_->mem_read32(kSettings + 0x78u), 2u) : 0u; }
bool Menu::vibration() const { return cpu_ != nullptr && cpu_->mem_read8(kSettings + 0x7Cu) != 0u; }
unsigned Menu::language() const {
    return cpu_ != nullptr ? std::min<u32>(cpu_->mem_read32(kSettings + 0x58u), 5u) : 1u;
}

bool Menu::autosave() const { return cpu_ != nullptr && cpu_->mem_read8(kSettings + 0x7Du) != 0u; }

bool Menu::set_autosave(bool on, std::string* error) {
    if (cpu_ == nullptr) {
        if (error != nullptr) *error = "the Options screen is not running";
        return false;
    }
    cpu_->mem_write8(kSettings + 0x7Du, on ? 1u : 0u);  // the game's own switch (its boot check sets it the same way)
    return true;
}

unsigned Menu::voice() const { return cpu_ != nullptr ? std::min<u32>(cpu_->mem_read32(kSettings + 0x5Cu), 1u) : 1u; }

bool Menu::apply(unsigned speaker_mode, bool vibration_on, unsigned language_id, unsigned voice_id,
                 std::string* error) {
    if (cpu_ == nullptr || impl_ == 0u) {
        if (error != nullptr) *error = "the Options screen is not running";
        return false;
    }
    Cpu& cpu = *cpu_;
    // Whether anything but the voice changes (the game's own apply is needed only then).
    const bool others_change = reload_text_ || speaker_mode != cpu.mem_read32(kSettings + 0x78u) ||
                               vibration_on != (cpu.mem_read8(kSettings + 0x7Cu) != 0u) ||
                               language_id != cpu.mem_read32(kSettings + 0x58u);
    // The screen's value slots, as its own pages leave them (vibration: 0 = ON).
    cpu.mem_write32(kSlotVibration, vibration_on ? 0u : 1u);
    cpu.mem_write32(kSlotSpeaker, speaker_mode);
    cpu.mem_write32(kSlotLanguage, language_id);
    cpu.mem_write32(kSlotVoice, voice_id);
    if (seamless_voice_ && voice_id != cpu.mem_read32(kSettings + 0x5Cu)) {
        // A seamless voice change (the user's request of 2026-11-05; the dialogue archive is the switchable one,
        // gekko/voice_language.h VoiceArchive): the game's own path (0x801DFA40: StopAllSE, then the archive reload)
        // would stop the menu music. Instead the voice goes straight into the settings (G + 0x5C, what the voice banks
        // and jingles are routed by) and into the slot's snapshot (cursor + 0x6C), so the game's apply - if it runs for
        // another option - sees no voice change. The archive's clips follow options.cfg's voice at their next read.
        cpu.mem_write32(kSettings + 0x5Cu, voice_id);
        cpu.mem_write32(kSnapshotVoice, voice_id);
        std::fprintf(stderr, "[options] voice %u applied seamlessly (no reload, no sound stop)\n", voice_id);
        if (!others_change) {
            ++report_.game_applies;
            return true;  // nothing else to apply: no fade, no state 3
        }
    }
    if (reload_text_) {
        // English <-> Magyar (settings::GameOptions::reload_text_on_next_apply): the game's language value stays 1, and
        // its apply compares the language slot with the snapshot (cursor + 0x58 + 0x0C, branch at 0x802B8678) and skips
        // the reload when they match. A different snapshot value makes it run: it writes the slot's value (still 1) to
        // G + 0x58 and re-reads the front end's text files, which now come out in the other language.
        reload_text_ = false;
        cpu.mem_write32(kSnapshotLanguage, language_id == 1u ? 0u : 1u);
        std::fprintf(stderr, "[options] text reload forced (English <-> Magyar): snapshot 0x%08X set to %u, slot %u\n",
                     kSnapshotLanguage, language_id == 1u ? 0u : 1u, language_id);
    }
    ++report_.game_applies;
    {
        // Every game-option change runs in the game's own flow, never as a nested call from this hook: the game's apply
        // 0x802B85D8 can block. A voice change calls 0x801DFA40 -> StopAllSE 0x801D68EC, G+0x5C, 0x801D690C ->
        // 0x801D844C, which re-reads PRS_VOICE_[EJ].afs (0x803C7B40 plus a wait loop on 0x803C7770); a nested call
        // never came back to the link ("a guest call did not return", execution fault). The Subtitle page's route is
        // state 3 (0x802B9230): sub 0 waits for the fade, sub 1 calls 0x802B85D8 (it compares every slot with the
        // snapshot at cursor + 0x58, so speaker, vibration, language and voice are all applied), copies the snapshot
        // (0x802B9330), fades in, and sub 3 returns to the grid. The fade starter 0x80173C5C (-> 0x8004427C,
        // 0x800446F8) only sets fields and never waits.
        cpu.mem_write32(impl_ + kState, 3u);
        cpu.mem_write32(impl_ + kSubState, 0u);
        cpu.mem_write8(kStateChanged, 1u);
        const double one = 1.0;
        u64 bits;
        std::memcpy(&bits, &one, sizeof(bits));
        cpu.fpr[1].bits[0] = cpu.fpr[1].bits[1] = bits;
        if (!call(cpu, kFadeOut, 0u, 1u)) {
            if (error != nullptr) *error = last_error_;
            return false;
        }
        page_closes_ = true;
        return true;
    }
}

bool Menu::run(std::string_view action, std::string* error) {
    if (cpu_ == nullptr || impl_ == 0u) {
        if (error != nullptr) *error = "the Options screen is not running";
        return false;
    }
    Cpu& cpu = *cpu_;
    if (action == "audio.sound_test") {
        // The unlocked tracks, read from the game's memory with no guest call: 0x800BBB34 is the singleton r13 + 0x2BB4,
        // 0x801E5348 returns 0x37, and 0x801E5A84 tests bit (i & 31) of the word at (save + 0x1714) + (i >> 5) * 4.
        unlocked_.clear();
        const u32 save = cpu.mem_read32(cpu.gpr[13] + kSaveDataPointer);
        if (save != 0u)
            for (u32 i = 0; i < kTrackTotal; ++i)
                if ((cpu.mem_read32(save + 0x1714u + (i >> 5) * 4u) >> (i & 31u) & 1u) != 0u) unlocked_.push_back(i);
        sound_test_ = true;
        list_cursor_ = list_scroll_ = 0u;
        return true;
    }
    if (action == "gameplay.load_game") {
        // The game's own Load: state 4 (0x802B9394) runs the load cycle and returns to the grid.
        cpu.mem_write32(impl_ + kState, 4u);
        cpu.mem_write32(impl_ + kSubState, 0u);
        cpu.mem_write8(kStateChanged, 1u);
        ++report_.loads;
        page_closes_ = true;
        return true;
    }
    if (error != nullptr) *error = "no such game command";
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// The hooks

bool Menu::setup(Cpu& cpu) {
    const u32 impl = cpu.gpr[3];
    // Every tile shown and reachable: the voice-language slot (+0x26) and the subtitle slot (+0x24) are categories now;
    // +0x25 would drop Japanese from the language list and shift its values.
    cpu.mem_write8(impl + kHiddenAudio, 0u);
    cpu.mem_write8(impl + kHiddenSubtitle, 0u);
    cpu.mem_write8(impl + kNoJapanese, 0u);
    ++report_.setups;
    open_ = sound_test_ = page_closes_ = false;
    impl_ = impl;
    return false;
}

bool Menu::page(Cpu&) {
    page_ran_ = true;
    return false;
}

bool Menu::set_text(Cpu& cpu) {
    if (cpu.lr != kHelpLineCall && cpu.lr != kHelpLineCall2) return false;
    if (!messages_ready_) return false;
    const u32 item = current_item(cpu);
    if (item >= kCategoryCount) return false;
    u32 message = kCategoryHelpBase + item;
    if (open_ && item == item_) {
        if (st::registry().trial().active) {
            message = kConfirmHelp;
        } else if (sound_test_) {
            message = kSoundTestHelp;
        } else {
            const std::vector<st::SettingView> list = rows();
            if (cursor_ < list.size()) message = list[cursor_].id == kDefaultsId ? help_id(item, kDefaultsIndex)
                                                                                  : help_id(item, cursor_);
        }
    }
    if (message != logged_help_) {
        // Evidence for the help band: the message set, and what the mounted table (as the menu holds it) has for it.
        logged_help_ = message;
        const std::u16string text = game_message(column(), message);
        std::string ascii;
        for (const char16_t c : text.substr(0, 48)) ascii.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        std::fprintf(stderr, "[options] help line: message %u (was %u), table text %zu chars \"%s\"\n", message,
                     cpu.gpr[4], text.size(), ascii.c_str());
    }
    cpu.gpr[4] = message;
    return false;  // the game draws it
}

// The game's option values as the settings object holds them, printed when they change (and at the first draw): the
// evidence of what is really in effect (docs: settings validation), read from the game's memory, not from the menu.
void Menu::log_engine_state(Cpu& cpu) {
    const u32 now[6] = {cpu.mem_read32(kSettings + 0x78u), cpu.mem_read8(kSettings + 0x7Cu),
                        cpu.mem_read32(kSettings + 0x58u), cpu.mem_read32(kSettings + 0x5Cu),
                        cpu.mem_read8(kSettings + 0x7Du), cpu.mem_read32(cpu.gpr[13] + 0x2D20u)};
    if (logged_engine_valid_ && std::memcmp(now, logged_engine_, sizeof(now)) == 0) return;
    std::memcpy(logged_engine_, now, sizeof(now));
    logged_engine_valid_ = true;
    std::fprintf(stderr, "[options] engine state: speaker %u, vibration %u, text language %u (r13 copy %u), voice %u, autosave %u\n",
                 now[0], now[1], now[2], now[5], now[3], now[4]);
}

bool Menu::draw_rows(Cpu& cpu) {
    const u32 impl = cpu.gpr[3];
    const u32 item = cpu.gpr[4];
    if (item >= kCategoryCount) return false;
    Attach attach(*this, cpu);
    impl_ = impl;
    set_language(language_of_game(cpu.mem_read32(kSettings + 0x58u)));  // follows a Subtitles change at once
    ++report_.row_draws;
    const bool focused = page_ran_;
    page_ran_ = false;
    log_engine_state(cpu);
    st::registry().tick();  // a display trial whose time ran out goes back now
    if (cpu.mem_read8(impl + kTextOn) == 0u) {
        hide(cpu);
        return true;
    }
    const bool trial = st::registry().trial().active;
    if (!focused || item != item_) {
        if (!trial) {
            if (open_) st::registry().discard_pending(static_cast<st::Category>(item_));
            open_ = sound_test_ = page_closes_ = false;
            if (item != item_ || cursor_ != 0u) {
                item_ = item;
                cursor_ = scroll_ = 0u;
            }
            st::registry().begin(static_cast<st::Category>(item_));  // the hovered category's values, dimmed
        }
    } else if (!open_ && !page_closes_) {
        open_ = true;
        st::registry().begin(static_cast<st::Category>(item_));
        cursor_ = scroll_ = 0u;
        sound_test_ = false;
    }
    publish(cpu, focused && item == item_ && !page_closes_);
    return true;
}

bool Menu::repeat_press(Cpu& cpu) {
    if (cpu.lr != kPageUpQuery && cpu.lr != kPageDownQuery) return false;
    if (current_item(cpu) >= kCategoryCount) return false;
    Attach attach(*this, cpu);
    if (cpu.lr == kPageUpQuery) page_input(cpu, cpu.gpr[31]);
    cpu.gpr[3] = 0u;  // the game's own rows never move
    return true;
}

bool Menu::edge_press(Cpu& cpu) {
    if (cpu.lr != kPageAQuery && cpu.lr != kPageBQuery) return false;
    if (current_item(cpu) >= kCategoryCount) return false;
    Attach attach(*this, cpu);
    impl_ = cpu.gpr[31];
    if (cpu.lr == kPageAQuery) {
        play_rank_ = -1;
        if (open_ && !page_closes_ && od::button_pressed(cpu, kButtonA)) press_a(cpu, impl_);
        if (play_rank_ >= 0) {
            // The sound test's play (play_track): the game's own A path for item 0 runs in its handler's frame.
            cpu.mem_write32(kSoundTestSlot, static_cast<u32>(play_rank_));
            cpu.gpr[29] = 0u;  // the handler's item register (0x802B9690), read again at 0x802B97FC
            cpu.gpr[3] = 1u;
            play_rank_ = -1;
            return true;
        }
        cpu.gpr[3] = 0u;  // the game's own A (its per-item save / load / sound test) never runs
        return true;
    }
    // B: the game's own (restore and back to the grid) unless the confirmation or the sound test list is open.
    const bool trial = st::registry().trial().active;
    if (open_ && !trial && sound_test_ && cpu.mem_read8(impl_ + kTrackPlayed) != 0u && od::button_pressed(cpu, kButtonB)) {
        // A track is playing (impl + 0x30, set by the game's own play path): the game's B path restores the menu music
        // (0x802B9988: impl + 0x30 != 0 -> 0x802C88F4 (bgm, 3), clear it), the cursor snapshot and the grid, as in
        // the original Sound Test page. It runs in the game's flow: decline.
        sound_test_ = false;
        return false;
    }
    if (open_ && (trial || sound_test_) && od::button_pressed(cpu, kButtonB)) {
        if (trial) {
            st::registry().revert();
            ++report_.reverts;
            st::registry().begin(static_cast<st::Category>(item_));
        } else {
            sound_test_ = false;
        }
        sound(cpu, kSoundBack);
        publish(cpu, true);
        cpu.gpr[3] = 0u;
        return true;
    }
    return false;
}

void Menu::press_a(Cpu& cpu, u32) {
    if (st::registry().trial().active) {
        std::string error;
        if (st::registry().confirm(&error)) ++report_.confirms, ++report_.saves;
        else ++report_.save_failures, last_error_ = error;
        st::registry().begin(static_cast<st::Category>(item_));
        sound(cpu, kSoundDecide);
        publish(cpu, true);
        return;
    }
    if (sound_test_) {
        if (list_cursor_ < unlocked_.size()) play_track(cpu, unlocked_[list_cursor_]);
        publish(cpu, true);
        return;
    }
    const std::vector<st::SettingView> list = rows();
    if (cursor_ >= list.size()) return;
    const st::SettingView& v = list[cursor_];
    std::string error;
    if (v.id == kDefaultsId) {
        restore_defaults(cpu, false);
    } else if (v.id == "system.restore_all") {
        restore_defaults(cpu, true);
    } else if (v.kind == st::Kind::Action) {
        if (!v.available) return;
        if (st::registry().run(v.id, &error)) sound(cpu, kSoundDecide);
        else ++report_.apply_failures, last_error_ = error;
    } else if (v.available) {
        apply_category(cpu);
    }
    publish(cpu, !page_closes_);
}

void Menu::apply_category(Cpu& cpu) {
    std::string error;
    const st::Category c = static_cast<st::Category>(item_);
    switch (st::registry().apply_pending(c, &error)) {
    case st::ApplyResult::Nothing: break;
    case st::ApplyResult::Applied: ++report_.applies, ++report_.saves; break;
    case st::ApplyResult::TrialStarted: ++report_.applies; break;
    case st::ApplyResult::Failed: ++report_.apply_failures, last_error_ = error; break;
    }
    if (!page_closes_ && !st::registry().trial().active) st::registry().begin(c);
    sound(cpu, kSoundDecide);
}

void Menu::restore_defaults(Cpu& cpu, bool every) {
    // The display category last: its trial (a window change) refuses every other apply until it ends.
    static constexpr st::Category kOrder[] = {st::Category::Graphics, st::Category::Audio, st::Category::Controls,
                                              st::Category::Gameplay, st::Category::System, st::Category::Display};
    std::string error;
    for (const st::Category c : kOrder) {
        if (!every && static_cast<u32>(c) != item_) continue;
        // Fresh pending values first: a category not opened since an earlier change would otherwise reset (and apply)
        // against its stale copy - a Restore all defaults that missed what changed elsewhere, or took it back.
        if (!st::registry().trial().active) st::registry().begin(c);
        st::registry().reset_defaults(c);
        const st::ApplyResult r = st::registry().apply_pending(c, &error);
        if (r == st::ApplyResult::Failed) ++report_.apply_failures, last_error_ = error;
        else if (r != st::ApplyResult::Nothing) ++report_.applies;
        if (!st::registry().trial().active) st::registry().begin(c);
    }
    sound(cpu, kSoundDecide);
}

void Menu::play_track(Cpu&, u32) {
    // The music change runs in the game's own flow, never nested in this hook: its stop / start of the BGM reaches the
    // ADX stream state machines (0x802C88F4, 0x801D55EC -> 0x803C21C4 / 0x803C25BC / 0x803C3D94), which may wait or
    // switch guest threads - a nested call there faulted the run ("a guest call did not return"). The game's A path
    // for item 0 (0x802B9850, inside the page handler 0x802B964C) does exactly this: it walks the unlocked tracks,
    // stops the BGM (0x801E5FB8 -> 0x802C88F4 (bgm, 0)), plays the one whose rank equals the item's slot
    // (0x801E52F0, 0x8005328C, 0x801D55EC) and sets impl + 0x30. edge_press makes the handler take that path when the
    // game asks "was A pressed" in this very frame: the answer is yes, the item it reads (r29) is 0, the slot is the
    // list rank. The rank among the unlocked tracks is this list's cursor (both count unlocked tracks in order).
    play_rank_ = static_cast<int>(list_cursor_);
    ++report_.tracks_played;
}

void Menu::page_input(Cpu& cpu, u32 impl) {
    impl_ = impl;
    if (!open_ || page_closes_ || st::registry().trial().active) return;
    int move = 0, change = 0;
    if (od::button_repeats(cpu, kButtonUp)) move = -1;
    else if (od::button_repeats(cpu, kButtonDown)) move = 1;
    else if (od::button_repeats(cpu, kButtonLeft)) change = -1;
    else if (od::button_repeats(cpu, kButtonRight)) change = 1;
    if (sound_test_) {
        if (move == 0) return;
        const s32 next = static_cast<s32>(list_cursor_) + move;
        if (next < 0 || next >= static_cast<s32>(unlocked_.size())) return;
        list_cursor_ = static_cast<u32>(next);
        if (list_cursor_ < list_scroll_) list_scroll_ = list_cursor_;
        if (list_cursor_ >= list_scroll_ + kVisibleRows) list_scroll_ = list_cursor_ - kVisibleRows + 1u;
    } else {
        const std::vector<st::SettingView> list = rows();
        if (move != 0) {
            const s32 next = static_cast<s32>(cursor_) + move;
            if (next < 0 || next >= static_cast<s32>(list.size())) return;  // the game's lists stop at their ends
            cursor_ = static_cast<u32>(next);
            if (cursor_ < scroll_) scroll_ = cursor_;
            if (cursor_ >= scroll_ + kVisibleRows) scroll_ = cursor_ - kVisibleRows + 1u;
            ++report_.moves;
        } else if (change != 0) {
            if (cursor_ >= list.size()) return;
            const st::SettingView& v = list[cursor_];
            if (!v.available || v.kind == st::Kind::Action) return;
            if (!st::registry().step(v.id, change)) return;
            ++report_.changes;
        } else {
            return;
        }
    }
    cpu.mem_write32(impl + kRepeatCountdown, 2u);  // as the game's own moves
    sound(cpu, kSoundCursor);
    publish(cpu, true);
}

void Menu::sound(Cpu& cpu, u32 id) {
    if (sound_ != nullptr) sound_(sound_context_, cpu, id);
}

bool Menu::ensure_font() {
    // Magyar switches live: the font follows it (the Hungarian sheets while hungarian(), the English ones otherwise).
    const bool want_hungarian = hungarian();
    if (want_hungarian != font_hungarian_) {
        font_hungarian_ = want_hungarian;
        font_ready_ = font_failed_ = false;
        std::fprintf(stderr, "[options] rows now drawn in %s\n", want_hungarian ? "Hungarian" : "the game's language");
    }
    if (font_ready_ || font_failed_) return font_ready_;
    FontFiles hungarian_copy;
    if (want_hungarian) {
        std::lock_guard<std::mutex> lock(hungarian_mutex_);
        hungarian_copy = hungarian_files_;
    }
    std::vector<std::pair<const std::vector<u8>*, const std::vector<std::vector<u8>>*>> files;
    if (want_hungarian) files.emplace_back(&hungarian_copy.txd, &hungarian_copy.glyph_lists);
    files.emplace_back(&files_[static_cast<u32>(language_)].txd, &files_[static_cast<u32>(language_)].glyph_lists);
    if (language_ != od::Language::EN) files.emplace_back(&files_[0].txd, &files_[0].glyph_lists);
    const FontFiles& own = want_hungarian ? hungarian_copy : files_[static_cast<u32>(language_)];
    on_off_[0] = own.on.empty() ? files_[0].on : own.on;
    on_off_[1] = own.off.empty() ? files_[0].off : own.off;
    od::Font font;
    if (!od::build_font(language_, files, font)) {
        font_failed_ = true;
        ++report_.font_failures;
        last_error_ = "the front-end glyph sheets could not be read";
        return false;
    }
    std::lock_guard<std::mutex> lock(od::shared().mutex);
    font.serial = od::shared().font.serial + 1u;
    od::shared().font = std::move(font);
    font_ready_ = true;
    return true;
}

void Menu::hide(Cpu& cpu) {
    od::Page page;
    page.game_ticks = cpu.game_clock().now();
    std::lock_guard<std::mutex> lock(od::shared().mutex);
    const od::Page& last = od::shared().pages[(od::shared().next + od::Shared::kHistory - 1u) % od::Shared::kHistory];
    if (!last.visible) return;
    od::shared().push(std::move(page));
}

void Menu::publish(Cpu& cpu, bool focused) {
    if (!ensure_font()) return;
    od::Page page;
    page.visible = true;
    page.focused = focused;
    page.game_ticks = cpu.game_clock().now();
    u32 total = 0, first = 0;
    const st::TrialState trial = st::registry().trial();
    {
        // A refused display change (an exclusive mode the monitor does not set): said on the page for 4 s, in the
        // window owner's own words, after its rollback.
        std::uint64_t serial = 0;
        const std::string failure = st::registry().last_failure(&serial);
        if (serial != failure_seen_) {
            failure_seen_ = serial;
            failure_text_ = failure;
            failure_until_ticks_ = page.game_ticks + 4u * kGameTicksPerSecond;
        }
    }
    if (!trial.active && focused && !failure_text_.empty() && page.game_ticks < failure_until_ticks_) {
        page.dialog.push_back(ascii16("Display change refused:"));
        std::string rest = failure_text_;
        while (!rest.empty()) {  // the panel's width: about 40 characters a line, at most four lines
            std::size_t cut = rest.size() <= 40u ? rest.size() : rest.rfind(' ', 40u);
            if (cut == std::string::npos || cut == 0u) cut = std::min<std::size_t>(40u, rest.size());
            page.dialog.push_back(ascii16(rest.substr(0, cut)));
            rest.erase(0, cut);
            while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
            if (page.dialog.size() >= 4u) break;
        }
    } else if (trial.active && focused && trial.verify_only) {
        page.dialog.push_back(ascii16("Setting the display mode..."));
    } else if (trial.active && focused) {
        const int seconds = static_cast<int>(std::ceil(trial.seconds_left));
        page.dialog.push_back(text16(column(), "word.confirm.question", "Keep these display settings?"));
        {
            std::u16string returning = text16(column(), "word.confirm.returning", "Returning in %d s");
            const std::size_t at = returning.find(u"%d");
            if (at != std::u16string::npos) returning.replace(at, 2, ascii16(std::to_string(seconds)));
            page.dialog.push_back(std::move(returning));
        }
        page.dialog.push_back(text16(column(), "word.confirm.keep", "A  Keep"));
        page.dialog.push_back(text16(column(), "word.confirm.return", "B  Return"));
    } else if (sound_test_ && focused) {
        std::vector<std::u16string> hungarian_names;
        if (hungarian()) {
            std::lock_guard<std::mutex> lock(hungarian_mutex_);
            hungarian_names = hungarian_tracks_;
        }
        const std::vector<std::u16string>& names =
            !hungarian_names.empty()                     ? hungarian_names
            : tracks_[static_cast<u32>(language_)].empty() ? tracks_[0]
                                                           : tracks_[static_cast<u32>(language_)];
        for (const u32 track : unlocked_) {
            od::Page::Line line;
            line.label = track < names.size() ? names[track] : ascii16(std::to_string(track + 1u));
            page.lines.push_back(std::move(line));
        }
        page.cursor = list_cursor_;
        page.scroll = first = list_scroll_;
        total = static_cast<u32>(page.lines.size());
    } else {
        for (const st::SettingView& v : rows()) {
            od::Page::Line line;
            line.label = label_text(v);
            line.value = value_text(v, true);
            if (v.restart_required)  // takes effect at the next start (Hungarian text): said next to the value
                line.value += u" " + text16(column(), "word.restart", "(restart)");
            line.info = !v.available;
            page.lines.push_back(std::move(line));
        }
        page.cursor = cursor_;
        page.scroll = first = scroll_;
        total = static_cast<u32>(page.lines.size());
    }
    if (total > kVisibleRows) {
        page.counter = ascii16(std::to_string(page.cursor + 1u) + "/" + std::to_string(total));
        page.more_above = first > 0u;
        page.more_below = first + kVisibleRows < total;
    }
    std::lock_guard<std::mutex> lock(od::shared().mutex);
    od::shared().push(std::move(page));
}

}  // namespace gekko::options_menu
