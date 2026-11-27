// EXIT in the main menu: the owner of the menu's frame (0x802BDF0C). Contract: include/gekko/main_menu_exit.h;
// the original words it reproduces and the caution manager it drives: docs/re/main-menu-exit/README.md.

#include "gekko/main_menu_exit.h"

#include <bit>
#include <iostream>

#include "gekko/cpu.h"
#include "gekko/native_guest_call.h"

namespace gekko::main_menu_exit {

namespace {

// The menu's frame and the original functions it calls (Title::Phase, state 3).
constexpr u32 kMenuFrame = 0x802BDF0Cu;
constexpr u32 kLink = 0x802BE220u;            // liveness-roots: value-only; the frame's epilogue: the return address of the calls made here
constexpr u32 kAskCaution = 0x802E5E2Cu;      // (handle, window, port, flag) -> 0 open, else the answer
constexpr u32 kMenuInput = 0x802B8168u;       // (id, 4) -> pressed (any controller), only while no caution is open
constexpr u32 kTwoControllers = 0x802BD088u;  // () -> two controllers are connected (2P GAME)
constexpr u32 kSoundManager = 0x8005328Cu;    // () -> the SE manager
constexpr u32 kPlaySound = 0x801D59ACu;       // (manager, id, 0, 0)
constexpr u32 kSetState = 0x802BE4E8u;        // (phase, state, restart)
constexpr u32 kSceneOf = 0x8003CED4u;         // (phase + 0x10, scene name) -> scene
constexpr u32 kPlayMotion = 0x8003DA80u;      // (scene, motion name, 1)
constexpr u32 kSceneVisible = 0x8003C900u;    // (phase + 0x10, scene name, on)
constexpr u32 kTakeSnapshot = 0x802BDBD0u;    // (block)

// The menu's data.
constexpr u32 kMenuBlock = 0x805855A0u;       // liveness-roots: value-only; +4 the cursor, +0x10 its snapshot, +0x20 the refresh byte
constexpr u32 kCursor = 0x04u, kSnapshot = 0x10u, kRefresh = 0x20u;
constexpr u32 kPhaseSub = 0x08u, kPhaseScenes = 0x10u;
constexpr u32 kTitleSceneName = 0x805F9BA0u;  // liveness-roots: value-only; r2 - 0xBE0: char* "Title"
constexpr u32 kMenuSceneName = 0x805F9BA4u;   // liveness-roots: value-only; r2 - 0xBDC: char* "Menu"
constexpr u32 kMotionTable = 0x80558A48u;     // liveness-roots: value-only; char* [3]: Menu_1P, Menu_2P, Menu_Options
constexpr u32 kInputIds = 0x804FF6F8u;        // liveness-roots: value-only; menu input id -> button id
constexpr u32 kButtonMasks = 0x805F1EE4u;     // liveness-roots: value-only; r13 + 0x59E4: button id -> three masks
constexpr u32 kPadManager = 0x805EF29Cu;      // liveness-roots: value-only; r13 + 0x2D9C: the pad manager (0x8005185C's)
constexpr u32 kCautionManager = 0x805EFD68u;  // liveness-roots: value-only; r13 + 0x3868: the handle 0x801178FC returns
constexpr u32 kCautionHandleSet = 0x805EFD6Cu;  // liveness-roots: value-only; r13 + 0x386C: 0x801178FC's cached handle
constexpr u32 kClosedFraction = 0x805FA1F0u;  // liveness-roots: value-only; f32 0.0 (r2 - 0x590)
constexpr u32 kOpenFraction = 0x805FA204u;    // liveness-roots: value-only; f32 1.0 (r2 - 0x57C)
constexpr u32 kTextLanguage = 0x8057E7B8u;    // settings + 0x58: 0 JP, 1 EN, 2 FR, 3 SP, 4 GE, 5 IT
constexpr u32 kCautionTable = 0x805074A0u;    // liveness-roots: value-only; window descriptors, 0x24 bytes each

// Pad word and caution manager fields.
constexpr u32 kPadWords = 0x30u, kPadStride = 0x28u, kAnyPad = 4u;
constexpr u32 kManagerFont = 0x00u, kManagerShown = 0x0Cu, kManagerFraction = 0x18u, kManagerClosing = 0x2Cu;
constexpr u32 kFontPages = 10u, kFontPageList = 0x08u;  // {page, 0-ended character list} pairs from font + 4
constexpr u32 kFontMessages = 0x5Cu;          // {count, then 20-byte entries {text, key, ...}}

// The menu.
constexpr u32 kSubMenu = 1u, kSubDecided = 2u, kSubTwoPlayers = 3u;
constexpr u32 kSubAsk = 4u, kSubLeaving = 5u;  // EXIT's own sub-states (the original does nothing for 4 and up)
constexpr u32 kExitEntry = 3u;
constexpr u32 kInputDecide = 1u, kInputBack = 2u, kInputUp = 3u, kInputDown = 4u;
constexpr u32 kSoundCursor = 0xE002u, kSoundDecide = 0xE003u, kSoundBack = 0xE004u;
constexpr char kExitMotion[] = "Menu_Exit";

// The question: window 0x2A (memory card format: Yes / No, vertical, starts on No), message 6019 (key 6019100),
// answered with the results its descriptor lists for Yes and No.
constexpr u32 kWindow = 0x2Au;
constexpr u32 kWindowMessage = 6019u, kYesMessage = 6047u, kNoMessage = 6048u;
constexpr u32 kQuestionKey = kWindowMessage * 1000u + 100u;

// The question in the text language (UTF-16; every glyph is in both the Advertise and the Save atlas of its language).
const std::u16string& question(u32 language) {
    static const std::u16string texts[] = {
        u"\u30B2\u30FC\u30E0\u3092\u7D42\u4E86\u3057\u307E\u3059\u304B\uFF1F",  // JP
        u"Exit the game?",                                                        // EN
        u"Quitter le jeu ?",                                                      // FR
        u"\u00BFSalir del juego?",                                                // SP
        u"Spiel beenden?",                                                        // GE
        u"Esci dal gioco?",                                                       // IT
    };
    return texts[language < 6u ? language : 1u];
}

State* g_state = nullptr;

u32 call(Cpu& c, u32 target, std::initializer_list<u32> args, bool& ok) {
    u32 result = 0;
    if (ok && !guest_call(c, target, kLink, args, result)) ok = false;
    return result;
}

bool sound(Cpu& c, u32 id) {
    bool ok = true;
    const u32 manager = call(c, kSoundManager, {}, ok);
    call(c, kPlaySound, {manager, id, 0u, 0u}, ok);
    return ok;
}

// 0x80357068's test, read without running it: one of the button's masks matches the pad word whole, or by its 0x3C0
// bits, or by its 0x3C0000 bits.
bool pressed_now(Cpu& c, u32 input, u32 port) {
    const u32 masks = c.mem_read32(kButtonMasks), pads = c.mem_read32(kPadManager);
    if (masks == 0u || pads == 0u) return false;
    const u32 button = c.mem_read32(kInputIds + 4u * input);
    const u32 state = c.mem_read32(pads + kPadWords + kPadStride * port);
    for (u32 j = 0; j < 3u; ++j) {
        const u32 mask = c.mem_read32(masks + 0xCu * button + 4u * j);
        if (mask == 0u) continue;
        const u32 a = mask & 0x3C0u, b = mask & 0x3C0000u;
        if ((mask & state) == mask || (a != 0u && (a & state) == a) || (b != 0u && (b & state) == b)) return true;
    }
    return false;
}

bool caution_closed_now(Cpu& c) {
    const u32 manager = c.mem_read32(kCautionManager);
    return manager != 0u &&
           std::bit_cast<float>(c.mem_read32(manager + kManagerFraction)) == std::bit_cast<float>(c.mem_read32(kClosedFraction));
}

// Puts message 6019's text back once the question's window has closed (or orce).
void restore_text(Cpu& c, State& state, bool force = false) {
    if (state.saved_text == 0u) return;
    const u32 manager = c.mem_read32(kCautionManager);
    if (!force && manager != 0u && c.mem_read32(manager + kManagerShown) == kWindow) return;
    for (std::size_t i = 0; i < state.saved_units.size(); ++i)
        c.mem_write16(state.saved_text + 2u * static_cast<u32>(i), state.saved_units[i]);
    state.saved_text = 0u;
    state.saved_units.clear();
}

// Writes the question over message 6019's text in the caution manager's message table (the original units kept to be
// put back). False when the table, the message or the window are not the ones expected.
bool place_question(Cpu& c, State& state) {
    const u32 descriptor = kCautionTable + 0x24u * kWindow;
    if (c.mem_read32(descriptor) != kWindowMessage || c.mem_read32(descriptor + 4u) != 2u ||
        c.mem_read32(descriptor + 0x10u) != kYesMessage || c.mem_read32(descriptor + 0x14u) != kNoMessage)
        return false;
    const u32 manager = c.mem_read32(kCautionManager);
    const u32 font = manager != 0u ? c.mem_read32(manager + kManagerFont) : 0u;
    const u32 table = font != 0u ? c.mem_read32(font + kFontMessages) : 0u;
    if (table == 0u) return false;
    const u32 count = c.mem_read32(table);
    u32 text = 0u;
    for (u32 i = 0; i < count && i < 0x4000u; ++i) {
        if (c.mem_read32(table + 4u + 20u * i + 4u) == kQuestionKey) {
            text = c.mem_read32(table + 4u + 20u * i);
            break;
        }
    }
    if (text < 0x80000000u) return false;
    // Every character must be on one of the font's glyph pages (0x8004A4A4: up to ten {page, character list} pairs at
    // font + 4); a character on none is not drawn. The language's question, else the English one.
    const auto drawable = [&c, font](const std::u16string& s) {
        for (const char16_t ch : s) {
            bool found = ch == u'\n';
            for (u32 page = 0; page < kFontPages && !found; ++page) {
                const u32 list = c.mem_read32(font + kFontPageList + 8u * page);
                for (u32 k = 0; list != 0u && k < 0x4000u && !found; ++k) {
                    const u32 code = c.mem_read32(list + 4u * k);
                    if (code == 0u) break;
                    found = code == static_cast<u32>(ch);
                }
            }
            if (!found) {
                std::cerr << "[main-menu-exit] the caution font has no glyph U+" << std::hex
                          << static_cast<unsigned>(ch) << std::dec << '\n';
                return false;
            }
        }
        return true;
    };
    const std::u16string* chosen = &question(c.mem_read32(kTextLanguage));
    if (!drawable(*chosen)) chosen = &question(1u);
    if (!drawable(*chosen)) return false;
    const std::u16string& q = *chosen;
    u32 length = 0;
    while (length < 0x1000u && c.mem_read16(text + 2u * length) != 0u) ++length;
    if (length < q.size()) return false;
    restore_text(c, state, true);
    state.saved_text = text;
    state.saved_units.resize(q.size() + 1u);
    for (std::size_t i = 0; i <= q.size(); ++i) {
        state.saved_units[i] = c.mem_read16(text + 2u * static_cast<u32>(i));
        c.mem_write16(text + 2u * static_cast<u32>(i), i < q.size() ? static_cast<u16>(q[i]) : 0u);
    }
    return true;
}

// The menu's frame (sub-state 1) with four entries: 0x802BDF0C's own steps, through the same functions.
bool menu_frame(Cpu& c, State& state, u32 phase) {
    bool ok = true;
    // 0x801178FC (the handle, made on first use: it is, frame_entry checked) and 0x802E5D88 (its open fraction is 0.0),
    // read as they compute it.
    const bool closed = caution_closed_now(c);
    const u32 block = kMenuBlock;
    if (ok && closed) {
        const u32 cursor = c.mem_read32(block + kCursor);
        if ((call(c, kMenuInput, {kInputDecide, kAnyPad}, ok) & 0xFFu) != 0u) {
            if (cursor == kExitEntry) {
                if (place_question(c, state)) {
                    c.mem_write32(phase + kPhaseSub, kSubAsk);
                    ++state.stats.questions;
                    ok = ok && sound(c, kSoundDecide);
                } else {
                    ++state.stats.text_failures;
                    std::cerr << "[main-menu-exit] the question's text could not be placed; EXIT is not asked\n";
                    ok = ok && sound(c, kSoundBack);
                }
            } else if (cursor == 1u && (call(c, kTwoControllers, {phase}, ok) & 0xFFu) == 0u) {
                c.mem_write32(phase + kPhaseSub, kSubTwoPlayers);
                ok = ok && sound(c, kSoundBack);
            } else {
                c.mem_write32(phase + kPhaseSub, kSubDecided);
                ok = ok && sound(c, kSoundDecide);
            }
        } else if ((call(c, kMenuInput, {kInputBack, kAnyPad}, ok) & 0xFFu) != 0u) {
            call(c, kSetState, {phase, 2u, 1u}, ok);
            ok = ok && sound(c, kSoundBack);
        } else if ((call(c, kMenuInput, {kInputUp, kAnyPad}, ok) & 0xFFu) != 0u) {
            if (static_cast<s32>(cursor) > 0) {
                c.mem_write32(block + kCursor, cursor - 1u);
                ok = ok && sound(c, kSoundCursor);
            }
        } else if ((call(c, kMenuInput, {kInputDown, kAnyPad}, ok) & 0xFFu) != 0u) {
            if (static_cast<s32>(cursor) < static_cast<s32>(kExitEntry)) {
                c.mem_write32(block + kCursor, cursor + 1u);
                ok = ok && sound(c, kSoundCursor);
            }
        }
    }
    if (!ok) return true;  // the run is stopped (guest_call asked for it)
    bool changed = c.mem_read8(block + kRefresh) != 0u;
    for (u32 i = 0; i < 0x10u && !changed; ++i) changed = c.mem_read8(block + i) != c.mem_read8(block + kSnapshot + i);
    if (changed) {
        const u32 cursor = c.mem_read32(block + kCursor);
        const u32 scenes = phase + kPhaseScenes;
        const u32 scene = call(c, kSceneOf, {scenes, c.mem_read32(kMenuSceneName)}, ok);
        if (cursor == kExitEntry) {
            GuestFrame name(c, sizeof(kExitMotion));
            for (u32 i = 0; i < sizeof(kExitMotion); ++i) c.mem_write8(name.base() + i, static_cast<u8>(kExitMotion[i]));
            call(c, kPlayMotion, {scene, name.base(), 1u}, ok);
        } else {
            call(c, kPlayMotion, {scene, c.mem_read32(kMotionTable + 4u * cursor), 1u}, ok);
        }
        call(c, kSceneVisible, {scenes, c.mem_read32(kTitleSceneName), 0u}, ok);
        call(c, kSceneVisible, {scenes, c.mem_read32(kMenuSceneName), 1u}, ok);
        call(c, kTakeSnapshot, {block}, ok);
    }
    return true;
}

// The question (sub-state 4): asked every frame until answered; B cancels once the window is fully open.
bool ask_frame(Cpu& c, State& state, u32 phase) {
    if (c.mem_read32(phase + kPhaseSub) == kSubLeaving) return true;  // Yes was chosen: the host is stopping the game
    bool ok = true;
    const u32 handle = kCautionManager;  // what 0x801178FC returns once made (frame_entry checked)
    const u32 answer = call(c, kAskCaution, {handle, kWindow, 0u, 0u}, ok);
    if (!ok) return true;
    const u32 results = c.mem_read32(kCautionTable + 0x24u * kWindow + 0x1Cu);
    const u32 yes = c.mem_read32(results), no = c.mem_read32(results + 4u);
    if (answer != 0u && answer == yes) {
        ++state.stats.answered_yes;
        c.mem_write32(phase + kPhaseSub, kSubLeaving);
        std::cerr << "[main-menu-exit] Exit chosen: closing the game\n";
        if (state.on_exit) state.on_exit();
        return true;
    }
    if (answer != 0u && answer == no) {
        ++state.stats.answered_no;
        c.mem_write32(phase + kPhaseSub, kSubMenu);
        return true;
    }
    const u32 manager = c.mem_read32(handle);
    if (answer == 0u && manager != 0u && c.mem_read32(manager + kManagerShown) == kWindow &&
        c.mem_read32(manager + kManagerClosing) == 0u &&
        std::bit_cast<float>(c.mem_read32(manager + kManagerFraction)) >= std::bit_cast<float>(c.mem_read32(kOpenFraction)) &&
        pressed_now(c, kInputBack, 0u)) {  // 0x80357068(back, the window's port 0), read as it computes it
        ++state.stats.answered_no;
        c.mem_write32(phase + kPhaseSub, kSubMenu);  // no more requests: the window closes
        ok = ok && sound(c, kSoundBack);
    }
    return true;
}

bool frame_entry(Cpu& c) {
    State& state = *g_state;
    const u32 phase = c.gpr[3];
    if (c.mem_read32(kCautionHandleSet) != kCautionManager) {  // the caution manager is not made yet: the original makes it
        ++state.stats.declined;
        return false;
    }
    const u32 sub = c.mem_read32(phase + kPhaseSub);
    // While the question is asked the window is not yet (or still) shown: the text stays until it has closed.
    if (sub != kSubAsk && sub != kSubLeaving) restore_text(c, state);
    if (sub == kSubAsk || sub == kSubLeaving) {
        ++state.stats.owned;
        return ask_frame(c, state, phase);
    }
    if (sub == kSubMenu && state.entry_ready.load(std::memory_order_relaxed)) {
        const u32 cursor = c.mem_read32(kMenuBlock + kCursor);
        // EXIT is in the frame: the cursor is on it, or it is about to be (Down alone on OPTIONS, as the original reads
        // its inputs in order: decide, back, up, down).
        if (cursor == kExitEntry ||
            (cursor == kExitEntry - 1u && caution_closed_now(c) && !pressed_now(c, kInputDecide, kAnyPad) &&
             !pressed_now(c, kInputBack, kAnyPad) && !pressed_now(c, kInputUp, kAnyPad) &&
             pressed_now(c, kInputDown, kAnyPad))) {
            ++state.stats.owned;
            return menu_frame(c, state, phase);
        }
    }
    ++state.stats.declined;
    return false;
}

}  // namespace

void install(Cpu& cpu, State& state) {
    if (!enabled()) return;
    g_state = &state;
    cpu.install_hle(kMenuFrame, [](Cpu& c) { return frame_entry(c); },
                    "main menu: EXIT, the fourth entry (0x802BDF0C; declines unless EXIT is in the frame)");
}

}  // namespace gekko::main_menu_exit
