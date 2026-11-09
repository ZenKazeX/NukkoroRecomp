// Tests of the Options menu's settings registry (gekko/settings_registry.h) and its Display / Graphics providers
// (gekko/settings_display.h): routing by id, apply + save, the 15 s display trial (confirm, revert, expiry, applies
// refused while it runs), and that a Display change never undoes a Graphics one and the reverse.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "gekko/display_settings.h"
#include "gekko/native_options_display.h"
#include "gekko/settings_display.h"
#include "gekko/settings_registry.h"
#include "test_scratch.h"

namespace {

namespace gs = gekko::settings;
namespace od = gekko::options_display;
using gekko::u32;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

gs::Registry::Clock::time_point g_now{};
gs::Registry::Clock::time_point fake_now() { return g_now; }

int index_of(const gs::SettingView& v, const std::string& key) {
    for (std::size_t i = 0; i < v.choices.size(); ++i)
        if (v.choices[i].key == key) return static_cast<int>(i);
    return -1;
}

const gs::SettingView* find(const std::vector<gs::SettingView>& views, const std::string& id) {
    for (const gs::SettingView& v : views)
        if (v.id == id) return &v;
    return nullptr;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// A provider that only counts: the trial mechanics without the display service.
class FakeProvider final : public gs::Provider {
public:
    gs::Category category() const override { return gs::Category::System; }
    void begin() override { pending = applied; }
    std::vector<gs::SettingView> views() const override {
        gs::SettingView v;
        v.id = "system.thing";
        v.kind = gs::Kind::Toggle;
        v.choices = {{"off", "Off"}, {"on", "On"}};
        v.applied = applied;
        v.pending = pending;
        v.needs_confirm = trial;
        return {v};
    }
    bool step(std::string_view id, int delta) override {
        if (id != "system.thing") return false;
        const int next = std::clamp(pending + delta, 0, 1);
        if (next == pending) return false;
        pending = next;
        return true;
    }
    bool set(std::string_view id, int value) override { return step(id, value - pending); }
    bool dirty() const override { return pending != applied; }
    gs::ApplyResult apply(std::string*) override {
        before = applied;
        applied = pending;
        return trial ? gs::ApplyResult::TrialStarted : gs::ApplyResult::Applied;
    }
    void discard() override { pending = applied; }
    void reset_defaults() override { pending = 0; }
    bool save(std::string*) override {
        ++saves;
        return true;
    }
    void trial_keep() override { ++keeps; }
    void trial_revert() override {
        ++reverts;
        applied = pending = before;
    }

    bool trial = false;
    int applied = 0, pending = 0, before = 0, saves = 0, keeps = 0, reverts = 0;
};

void routing() {
    gs::Category c{};
    check(gs::category_of("graphics.msaa", &c) && c == gs::Category::Graphics, "graphics.* is Graphics");
    check(gs::category_of("display.resolution", &c) && c == gs::Category::Display, "display.* is Display");
    check(!gs::category_of("nothing", &c) && !gs::category_of("video.msaa", &c), "unknown prefixes name nothing");
    check(gs::category_prefix(gs::Category::Gameplay) == "gameplay", "the prefix of Gameplay");
}

void trial_mechanics() {
    gs::Registry r;
    r.set_clock(fake_now);
    auto owned = std::make_unique<FakeProvider>();
    FakeProvider& p = *owned;
    r.add(std::move(owned));
    check(r.has(gs::Category::System) && !r.has(gs::Category::Audio), "providers by category");

    // An ordinary change: applied and saved at once.
    r.begin(gs::Category::System);
    check(r.apply_pending(gs::Category::System) == gs::ApplyResult::Nothing, "nothing pending: nothing applied");
    check(r.step("system.thing", 1), "a step changes the pending value");
    check(r.dirty(gs::Category::System), "dirty while pending");
    check(r.apply_pending(gs::Category::System) == gs::ApplyResult::Applied && p.applied == 1 && p.saves == 1,
          "applied and saved");

    // A trial: confirm keeps it and saves; nothing is saved before.
    p.trial = true;
    r.step("system.thing", -1);
    check(r.apply_pending(gs::Category::System) == gs::ApplyResult::TrialStarted, "a trial starts");
    check(p.saves == 1, "nothing saved while the trial runs");
    gs::TrialState t = r.trial();
    check(t.active && t.category == gs::Category::System && t.seconds_left > 14.9 && t.seconds_left <= 15.0,
          "15 seconds left");
    r.step("system.thing", 1);
    std::string error;
    check(r.apply_pending(gs::Category::System, &error) == gs::ApplyResult::Failed && !error.empty(),
          "applies are refused while the trial runs");
    p.pending = p.applied;  // the refused change is still pending in the page; drop it
    g_now += std::chrono::seconds(5);
    check(r.trial().seconds_left > 9.9 && r.trial().seconds_left <= 10.0, "the countdown follows the clock");
    check(r.confirm(&error) && p.keeps == 1 && p.saves == 2 && !r.trial().active, "confirm keeps and saves");

    // A trial left alone: rolled back by tick() when the 15 s run out, not before.
    r.step("system.thing", 1);
    check(r.apply_pending(gs::Category::System) == gs::ApplyResult::TrialStarted && p.applied == 1, "a second trial");
    g_now += std::chrono::milliseconds(14999);
    r.tick();
    check(r.trial().active && p.reverts == 0, "not rolled back before 15 s");
    g_now += std::chrono::milliseconds(2);
    r.tick();
    check(!r.trial().active && p.reverts == 1 && p.applied == 0 && p.saves == 2, "rolled back after 15 s, unsaved");
    check(r.report().expiries == 1 && r.report().confirms == 1 && r.report().trials == 2, "the report counts them");

    // revert() at once.
    r.step("system.thing", 1);
    r.apply_pending(gs::Category::System);
    r.revert();
    check(!r.trial().active && p.reverts == 2 && p.applied == 0, "revert puts the old value back");
}

void display_and_graphics(const std::filesystem::path& cfg) {
    od::set_screen_size(1920, 1080);
    od::set_work_area(1904, 1001);
    od::set_msaa_supported(0x0Fu);  // 1, 2, 4, 8
    {
        std::ofstream out(cfg, std::ios::binary);
        out << "# test display.cfg\nwidth=1280\nheight=720\nfullscreen=false\n";
    }
    gekko::DisplaySettings start;
    start.window_width = 1280;
    start.window_height = 720;
    gekko::display_settings().initialize(start, cfg);

    gs::Registry r;
    r.set_clock(fake_now);
    r.add(gs::make_display_provider());
    r.add(gs::make_graphics_provider());

    r.begin(gs::Category::Display);
    std::vector<gs::SettingView> d = r.list(gs::Category::Display);
    check(d.size() == 3u && find(d, "display.mode") && find(d, "display.aspect") && find(d, "display.vsync"),
          "Display lists the mode, the ratio and V-Sync");
    std::vector<gs::SettingView> g = r.list(gs::Category::Graphics);
    check(g.size() == 1u && find(g, "graphics.render_scale") != nullptr, "Graphics lists the resolution scale");
    const gs::SettingView* mode = find(d, "display.mode");
    check(mode->needs_confirm && mode->pending == 0 && mode->choices.size() == 2u, "two modes, windowed in effect");

    // Fullscreen: a trial; 15 s without a confirm roll it back, nothing saved.
    check(r.set("display.mode", 1), "pick fullscreen");
    check(r.apply_pending(gs::Category::Display) == gs::ApplyResult::TrialStarted, "a mode change is a trial");
    check(gekko::display_settings().current().fullscreen && gekko::display_settings().provisional(), "in effect");
    {
        std::string save_error;
        check(gekko::display_settings().save(&save_error) && read_file(cfg).find("fullscreen=true") == std::string::npos,
              "a save during the trial writes nothing of it");
    }
    g_now += std::chrono::seconds(16);
    r.tick();
    check(!gekko::display_settings().current().fullscreen, "rolled back to the window after 15 s");
    check(read_file(cfg).find("fullscreen=true") == std::string::npos, "the rollback saves nothing");

    // A ratio picked while windowed: applied and saved at once, and the window takes its shape once.
    r.begin(gs::Category::Display);
    d = r.list(gs::Category::Display);
    const int i3210 = index_of(*find(d, "display.aspect"), "32:10");
    check(i3210 > 0, "32:10 is offered");
    r.set("display.aspect", i3210);
    check(r.apply_pending(gs::Category::Display) == gs::ApplyResult::Applied, "a ratio applies at once");
    gekko::DisplaySettings now = gekko::display_settings().current();
    check(std::abs(now.aspect - 3.2) < 1e-12 && now.window_width == 1280 && now.window_height == 400,
          "32:10 picked in a 1280x720 window: the window becomes 1280x400");
    check(read_file(cfg).find("aspect=32:10") != std::string::npos && read_file(cfg).find("height=400") != std::string::npos,
          "both saved");
    // The player drags the window to another shape (the UI loop commits it): the ratio stays, the picture boxes in.
    {
        gekko::DisplaySettings dragged = gekko::display_settings().current();
        dragged.window_width = 1000;
        dragged.window_height = 1000;
        check(gekko::display_settings().apply(dragged), "a dragged window size");
    }
    r.begin(gs::Category::Display);
    r.set("display.vsync", 0);
    check(r.apply_pending(gs::Category::Display) == gs::ApplyResult::Applied, "V-Sync off applied");
    now = gekko::display_settings().current();
    check(!now.vsync && now.window_width == 1000 && now.window_height == 1000 && std::abs(now.aspect - 3.2) < 1e-12,
          "another row's change never reshapes the window");
    // Picking the ratio again (away and back) shapes it again.
    r.begin(gs::Category::Display);
    r.step("display.aspect", -1);
    r.step("display.aspect", 1);
    check(r.apply_pending(gs::Category::Display) == gs::ApplyResult::Applied &&
              gekko::display_settings().current().window_width == 1000 &&
              gekko::display_settings().current().window_height == 313,
          "re-picked 32:10: the 1000x1000 window becomes 1000x313");
    // Auto picked: the window stays as it is.
    r.begin(gs::Category::Display);
    r.set("display.aspect", 0);
    check(r.apply_pending(gs::Category::Display) == gs::ApplyResult::Applied &&
              gekko::display_settings().current().aspect == 0.0 &&
              gekko::display_settings().current().window_height == 313,
          "Auto: the window keeps its size");

    // Graphics: the scale, saved; a Display page applied later keeps it (each provider writes only its fields).
    r.begin(gs::Category::Display);
    r.set("display.vsync", 1);  // V-Sync on, pending in the Display page
    r.begin(gs::Category::Graphics);
    g = r.list(gs::Category::Graphics);
    const int i150 = index_of(*find(g, "graphics.render_scale"), "150");
    check(i150 >= 0 && index_of(*find(g, "graphics.render_scale"), "400") < 0 &&
              index_of(*find(g, "graphics.render_scale"), "25") == 0,
          "25..200 % offered");
    r.set("graphics.render_scale", i150);
    check(r.apply_pending(gs::Category::Graphics) == gs::ApplyResult::Applied, "150 % applies");
    check(read_file(cfg).find("render_scale=150") != std::string::npos, "saved");
    check(read_file(cfg).find("e+") == std::string::npos, "no number is saved in scientific form (render_scale=1e+02)");
    check(r.apply_pending(gs::Category::Display) == gs::ApplyResult::Applied, "the Display page applies later");
    now = gekko::display_settings().current();
    check(now.vsync && now.render_scale == 150.0, "V-Sync on, and the scale kept");

    // Reset to defaults: into pending only.
    r.begin(gs::Category::Graphics);
    r.reset_defaults(gs::Category::Graphics);
    check(r.dirty(gs::Category::Graphics) && gekko::display_settings().current().render_scale == 150.0,
          "defaults are pending, not applied");
    r.apply_pending(gs::Category::Graphics);
    check(gekko::display_settings().current().render_scale == 100.0, "applied: 100 %");
    od::set_work_area(0, 0);
}

// The display settings' transactions: what is saved, what a session override is, Alt+Enter through the registry, a
// refused exclusive mode (display.cfg), and an open page after an outside change.
void transactions(const std::filesystem::path& cfg) {
    od::set_screen_size(1920, 1080);
    {
        std::ofstream out(cfg, std::ios::binary);
        out << "# saved by the player\nwidth=1280\nheight=720\nfullscreen=false\nrender_scale=150\n";
    }
    // The command line set the window 1600x900 and 50 % for this session (session overrides).
    gekko::DisplaySettings start;
    start.window_width = 1600;
    start.window_height = 900;
    start.render_scale = 50.0;
    gekko::DisplaySettingsService& service = gekko::display_settings();
    service.initialize(start, cfg,
                       gekko::display_settings_key("width") | gekko::display_settings_key("height") |
                           gekko::display_settings_key("render_scale"));
    check(service.overridden("render_scale") && service.session_overrides().find("width") != std::string::npos,
          "the command line's keys are session overrides");

    gs::Registry r;
    r.set_clock(fake_now);
    r.add(gs::make_display_provider());
    r.add(gs::make_graphics_provider());

    r.begin(gs::Category::Display);
    r.set("display.vsync", 0);
    check(r.apply_pending(gs::Category::Display) == gs::ApplyResult::Applied, "V-Sync off applied");
    std::string file = read_file(cfg);
    check(file.find("vsync=false") != std::string::npos && file.find("width=1280") != std::string::npos &&
              file.find("render_scale=150") != std::string::npos,
          "the session's 1600x900 / 50 % are not written over the player's 1280x720 / 150 %");
    r.begin(gs::Category::Graphics);
    r.set("graphics.render_scale", index_of(*find(r.list(gs::Category::Graphics), "graphics.render_scale"), "75"));
    check(r.apply_pending(gs::Category::Graphics) == gs::ApplyResult::Applied &&
              read_file(cfg).find("render_scale=75") != std::string::npos && !service.overridden("render_scale"),
          "a setting the player changes is saved and stops being an override");

    // Alt+Enter: windowed -> fullscreen, committed and saved through the registry.
    check(r.toggle_fullscreen() && service.current().fullscreen && !service.provisional(), "Alt+Enter to fullscreen");
    check(read_file(cfg).find("fullscreen=true") != std::string::npos, "saved");
    // The open page follows the outside change and keeps an edit made on it.
    r.begin(gs::Category::Display);
    r.set("display.vsync", 1);  // V-Sync on, pending
    check(r.toggle_fullscreen() && !service.current().fullscreen, "Alt+Enter back to windowed");
    {
        const std::vector<gs::SettingView> d = r.list(gs::Category::Display);
        const gs::SettingView* mode = find(d, "display.mode");
        check(mode != nullptr && mode->applied == 0 && mode->pending == 0, "the page shows windowed in effect");
    }
    check(r.apply_pending(gs::Category::Display) == gs::ApplyResult::Applied && !service.current().fullscreen &&
              service.current().vsync,
          "applying the page keeps Alt+Enter's mode and the edit made meanwhile");

    // Exclusive (display.cfg) by Alt+Enter: provisional until the window owner reports; refused -> rolled back.
    gekko::DisplaySettings ex = service.current();
    ex.exclusive = true;
    ex.fullscreen_width = 2560;
    ex.fullscreen_height = 1440;
    check(service.apply(ex), "exclusive set for fullscreen (windowed now)");
    service.save();
    const std::string before = read_file(cfg);
    check(r.toggle_fullscreen() && service.provisional() && r.trial().active && r.trial().verify_only,
          "Alt+Enter into exclusive is provisional");
    check(!r.toggle_fullscreen(), "a second Alt+Enter is refused while it is checked");
    {
        gekko::DisplayWindowState w;
        w.generation = service.generation();
        w.fullscreen = w.exclusive_wanted = true;
        w.note = "exclusive fullscreen: lists no 2560x1440 mode; showing borderless at the desktop's size";
        service.report_window(w);
    }
    r.tick();
    std::uint64_t serial = 0;
    check(!r.trial().active && !service.current().fullscreen && !service.provisional() &&
              r.last_failure(&serial).find("2560x1440") != std::string::npos && serial != 0u,
          "refused -> rolled back to windowed, the reason kept for the page");
    check(read_file(cfg) == before, "and nothing saved");
    ex = service.current();
    ex.fullscreen_width = 1920;
    ex.fullscreen_height = 1080;
    service.apply(ex);
    check(r.toggle_fullscreen() && service.provisional(), "Alt+Enter into exclusive 1920x1080");
    {
        gekko::DisplayWindowState w;
        w.generation = service.generation();
        w.fullscreen = w.exclusive_wanted = w.exclusive_active = true;
        service.report_window(w);
    }
    r.tick();
    check(!r.trial().active && service.current().fullscreen && !service.provisional() &&
              read_file(cfg).find("fullscreen_width=1920") != std::string::npos,
          "verified -> committed and saved");
    service.report_window({});
}

}  // namespace

int main() {
    routing();
    trial_mechanics();
    const std::filesystem::path cfg = gekko::test_support::scratch_directory("settings_registry_tests.cfg");
    display_and_graphics(cfg);
    transactions(cfg);
    std::error_code ec;
    std::filesystem::remove(cfg, ec);
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("settings_registry_tests: all passed\n");
    return 0;
}
