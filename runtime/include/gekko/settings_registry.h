#pragma once

// The PC Options menu's settings registry (docs/OPTIONS_MENU_OVERHAUL.md section 14): one place where every setting
// the menu shows is defined, listed, changed, applied, persisted, reset and - for display changes - tried out for
// 15 seconds and rolled back unless confirmed.
//
// Split of responsibilities:
//   * a Provider per category (Display, Graphics, Audio, Controls, Gameplay, System) owns its settings: their
//     definitions, the values each may take (with the hardware's limits), the pending values while a page is open,
//     how they are applied to the engine, persisted and reset. Providers hold no menu state and draw nothing;
//   * the Registry routes the menu's calls to the providers by category or by setting id ("display.resolution": the
//     part before the dot names the category) and owns the display trial (the 15 s confirmation and its rollback);
//   * the menu (gekko/native_options_menu.*) draws, localises (it maps setting ids and stable value keys to the six
//     languages) and calls the Registry. It never parses or writes configuration files and never touches the
//     renderer, the window or the audio runtime itself.
//
// Threads: every call takes the Registry's lock. The menu calls from the game thread; tick() runs on the UI thread
// once per loop iteration so that an expired trial is rolled back even when the menu is not on screen (or the new
// display mode shows nothing). Providers are called under the lock and must not call back into the Registry.

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace gekko::settings {

enum class Category : unsigned { Display, Graphics, Audio, Controls, Gameplay, System };
constexpr unsigned kCategoryCount = 6u;
// "display", "graphics", "audio", "controls", "gameplay", "system": the id prefix of the category's settings.
std::string_view category_prefix(Category category) noexcept;
// The category of a setting id ("graphics.msaa" -> Graphics); false when the prefix names none.
bool category_of(std::string_view id, Category* category) noexcept;

enum class Kind : unsigned {
    Choice,  // one of `choices` (left / right)
    Toggle,  // `choices` = {off, on}
    Slider,  // an integer in [slider_min, slider_max] by slider_step
    Action,  // a command (A runs it through Registry::run); `choices` may name it
};

// One value a Choice / Toggle setting can take: a stable key (persisted, compared, localised by the menu - e.g.
// "1920x1080", "windowed", "16:9", "4") and its English text (the menu's fallback and the reference wording).
struct ChoiceView {
    std::string key;
    std::string english;
};

struct SettingView {
    std::string id;             // "display.resolution"
    Kind kind = Kind::Choice;
    std::string english_label;  // "Resolution"
    std::string english_help;   // the description panel's text: says only what the setting really does
    std::vector<ChoiceView> choices;
    int slider_min = 0, slider_max = 0, slider_step = 1;
    int applied = 0;            // Choice / Toggle: index into choices; Slider: the value. What is in effect.
    int pending = 0;            // the same while the page is open (== applied when nothing is pending)
    bool available = true;      // false: shown disabled, `reason` says why (English)
    std::string reason;
    bool needs_confirm = false;     // applying a change of it starts the 15 s trial
    bool restart_required = false;  // applied at the next start only (the menu says so)
};

enum class ApplyResult {
    Nothing,       // nothing was pending
    Applied,       // in effect and saved
    TrialStarted,  // in effect for the trial: confirm() keeps it, revert() or 15 s without a confirm undo it
    Failed,        // refused (the error says why); nothing changed
};

// A category's settings (see the top of this file).
class Provider {
public:
    virtual ~Provider() = default;
    virtual Category category() const = 0;
    // The page opens: pending := in effect (the lists are rebuilt from what is in effect now).
    virtual void begin() = 0;
    // The settings, their lists computed from the pending values (a list may follow another setting's value).
    virtual std::vector<SettingView> views() const = 0;
    // Changes a pending value: by steps (clamped at the list's ends) or to a value. False: no such setting, not
    // available, or no change.
    virtual bool step(std::string_view id, int delta) = 0;
    virtual bool set(std::string_view id, int value) = 0;
    virtual bool dirty() const = 0;  // a pending value differs from what is in effect
    // Applies the pending values to the engine. TrialStarted only for changes of needs_confirm settings: the
    // provider keeps what it needs to undo them (trial_revert). The Registry saves after Applied (save()).
    virtual ApplyResult apply(std::string* error) = 0;
    virtual void discard() = 0;         // pending := in effect
    virtual void reset_defaults() = 0;  // pending := the defaults (apply makes them so)
    virtual bool save(std::string* error) = 0;  // persists what is in effect
    // The trial's end: keep (then the Registry saves) or put back what was in effect before it.
    virtual void trial_keep() {}
    virtual void trial_revert() {}
    // The settings in effect may have changed outside the page (Alt+Enter, a rolled-back trial, the window owner):
    // a provider holding a snapshot re-reads it. The Registry calls this before every list / step / apply.
    virtual void sync() {}
    // Kind::Action settings: runs one. False: no such action.
    virtual bool run(std::string_view id, std::string* error) {
        (void)id;
        if (error != nullptr) *error = "no such action";
        return false;
    }
};

struct TrialState {
    bool active = false;
    Category category = Category::Display;
    double seconds_left = 0.0;
    bool verify_only = false;  // Alt+Enter's: kept when the window owner reports success, no confirmation asked
};

class Registry {
public:
    static constexpr std::chrono::seconds kTrialLength{15};
    using Clock = std::chrono::steady_clock;

    // Startup, once per category (a later one replaces the earlier).
    void add(std::unique_ptr<Provider> provider);
    bool has(Category category) const;

    std::vector<SettingView> list(Category category) const;  // empty when the category has no provider
    void begin(Category category);
    bool step(std::string_view id, int delta);
    bool set(std::string_view id, int value);
    bool run(std::string_view id, std::string* error = nullptr);
    bool dirty(Category category) const;

    // Applies the category's pending values. Applied: saved too (a save failure is Failed, with the change kept in
    // effect). TrialStarted: the display trial runs; nothing is saved until confirm(). While a trial runs every
    // apply is refused (Failed) until confirm() or revert().
    ApplyResult apply_pending(Category category, std::string* error = nullptr);
    TrialState trial() const;
    bool confirm(std::string* error = nullptr);  // keeps the trial's change and saves it
    void revert();                               // puts back what was in effect before the trial
    // Alt+Enter: windowed <-> fullscreen (borderless or exclusive, as set) through the same transaction as the page:
    // refused while a trial runs; windowed and borderless are committed and saved at once; exclusive is provisional
    // until the window owner reports the mode set (then kept and saved) or refused (then rolled back).
    bool toggle_fullscreen(std::string* error = nullptr);
    // The last display change the window owner could not make (a refused exclusive mode), rolled back; empty: none.
    // `serial` changes with each new failure.
    std::string last_failure(std::uint64_t* serial = nullptr) const;
    void discard_pending(Category category);
    void reset_defaults(Category category);      // into pending: apply_pending makes them so
    bool save(Category category, std::string* error = nullptr);

    // The UI thread, every loop iteration: rolls back a trial whose 15 s have run out.
    void tick(Clock::time_point now = Clock::now());
    // For tests: the clock the trial measures against (default: Clock::now()).
    void set_clock(Clock::time_point (*now)()) noexcept { clock_ = now; }

    struct Report {
        std::uint64_t applies = 0, apply_failures = 0, trials = 0, confirms = 0, reverts = 0, expiries = 0,
                      saves = 0, save_failures = 0, refusals = 0, toggles = 0;
    };
    Report report() const;

private:
    Provider* provider_for(std::string_view id) const;
    void end_trial(bool keep);

    mutable std::recursive_mutex mutex_;
    std::unique_ptr<Provider> providers_[kCategoryCount];
    bool trial_active_ = false;
    Category trial_category_ = Category::Display;
    Clock::time_point trial_deadline_{};
    bool trial_verify_only_ = false;
    std::uint64_t trial_generation_ = 0;  // the display settings' generation the trial's change made
    std::string last_failure_;
    std::uint64_t failure_serial_ = 0;
    void check_display_trial();  // under the lock: a refused exclusive mode ends the trial at once
    Clock::time_point (*clock_)() = nullptr;
    Report report_{};
};

// The process's registry: the launcher adds the providers; the menu and the UI loop use it.
Registry& registry();

}  // namespace gekko::settings
