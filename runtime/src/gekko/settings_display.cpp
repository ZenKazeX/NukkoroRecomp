// The Display and Graphics categories of the settings registry (gekko/settings_display.h).

#include "gekko/settings_display.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "gekko/display_geometry.h"
#include "gekko/native_options_display.h"

namespace gekko::settings {

namespace od = gekko::options_display;

namespace {

struct RowId {
    const char* id;
    od::Row row;
};

// Five settings in all (docs/DISPLAY_SETTINGS_AUDIT.md section 6): Display - the mode, the ratio, the resolution and
// V-Sync; Graphics - the resolution scale.
constexpr RowId kDisplayRows[] = {{"display.mode", od::kDisplayMode},
                                  {"display.aspect", od::kScreenRatio},
                                  {"display.resolution", od::kResolution},
                                  {"display.vsync", od::kVsync}};
constexpr RowId kGraphicsRows[] = {{"graphics.render_scale", od::kRenderResolution}};

std::string format(const char* f, double v) {
    char text[48];
    std::snprintf(text, sizeof(text), f, v);
    return text;
}

std::string aspect_key(double a) {
    if (!(a > 0.0)) return "auto";
    if (const char* name = aspect_name(a)) return name;
    return format("%.4g", a);
}

std::string narrow(const std::u16string& s) {
    std::string out;
    for (const char16_t c : s) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

// V-Sync is a toggle {off, on}; the model holds it as 0 = ON, 1 = OFF.
int vsync_index(u32 model_value) { return model_value == 0u ? 1 : 0; }

// Copies one category's fields of `from` onto `onto`.
void copy_display_fields(DisplaySettings& onto, const DisplaySettings& from) {
    onto.fullscreen = from.fullscreen;
    onto.aspect = from.aspect;
    onto.vsync = from.vsync;
}
void copy_graphics_fields(DisplaySettings& onto, const DisplaySettings& from) { onto.render_scale = from.render_scale; }

// The fields the player edited on the page (pending differs from what was in effect when the page took it) copied
// onto `onto`: an outside change (Alt+Enter, a window drag, a rollback) is taken, the player's edits are kept.
template <typename T>
void keep_edit(T& onto, const T& pending, const T& applied) {
    if (!(pending == applied)) onto = pending;
}

const od::Texts& english() { return od::texts(od::Language::EN); }
std::string text_of(const od::Model& m, od::Row row) { return narrow(m.value_text(row, english(), u"On", u"Off")); }

// The shared machinery: the page's model (pending) over the settings in effect when the page opened, re-read when
// they changed outside the page (sync).
class ModelProvider : public Provider {
public:
    void begin() override { take(); }
    void discard() override { take(); }
    void sync() override {
        if (display_settings().generation() == generation_) return;
        const DisplaySettings pending = model_.settings(base_), applied = applied_.settings(base_);
        const od::Model::Size pending_window = model_.window, applied_window = applied_.window;
        const bool ratio_picked = ratio_picked_, resolution_picked = resolution_picked_;
        take();
        DisplaySettings merged = base_;
        keep_edits(merged, pending, applied);
        model_ = od::Model::from(merged);
        ratio_picked_ = ratio_picked;
        resolution_picked_ = resolution_picked;
        if (pending_window.width != applied_window.width || pending_window.height != applied_window.height)
            model_.window = pending_window;  // the player's pending size, not the size the window was dragged to
    }

    bool step(std::string_view id, int delta) override {
        const RowId* r = find(id);
        if (r == nullptr || delta == 0) return false;
        const DisplaySettings before = model_.settings(base_);
        const od::Model::Size window_before = model_.window;
        model_.step(r->row, r->row == od::kVsync ? -delta : delta);
        if (r->row == od::kScreenRatio) {
            ratio_picked_ = true;
            resolution_picked_ = false;  // a new ratio decides the size again
        }
        if (r->row == od::kResolution) resolution_picked_ = true;
        if (r->row == od::kScreenRatio || r->row == od::kDisplayMode) shape_window();
        return model_.settings(base_) != before || model_.window.width != window_before.width ||
               model_.window.height != window_before.height;
    }

    bool set(std::string_view id, int value) override {
        const RowId* r = find(id);
        if (r == nullptr) return false;
        const int now = index_of(*r, model_);
        return value != now && step(id, value - now);
    }

    bool save(std::string* error) override { return display_settings().save(error); }

protected:
    virtual const RowId* rows(std::size_t* count) const = 0;
    virtual void keep_edits(DisplaySettings& onto, const DisplaySettings& pending, const DisplaySettings& applied) const = 0;

    const RowId* find(std::string_view id) const {
        std::size_t n = 0;
        const RowId* list = rows(&n);
        for (std::size_t i = 0; i < n; ++i)
            if (id == list[i].id) return &list[i];
        return nullptr;
    }

    void take() {
        generation_ = display_settings().generation();
        base_ = display_settings().current();
        model_ = od::Model::from(base_);
        applied_ = model_;
        ratio_picked_ = false;
        resolution_picked_ = false;
    }

    // A fixed ratio picked on the page while windowed gives the window its shape, once (gekko/display_geometry.h
    // window_for_ratio), from the size it had when the page opened, and the Resolution row follows at once. Afterwards
    // the window is free again and the picture is letterboxed in whatever size the player drags it to.
    void shape_window() {
        if (resolution_picked_ || !ratio_picked_) return;
        model_.window = applied_.window;
        if (model_.value[od::kDisplayMode] != 0u || !(model_.aspect > 0.0)) return;
        const PixelSize shaped = window_for_ratio({applied_.window.width, applied_.window.height}, model_.aspect,
                                                  {model_.work.width, model_.work.height});
        model_.window = {std::max(kMinPictureWidth, shaped.width), std::max(kMinPictureHeight, shaped.height)};
    }

    static int index_of(const RowId& r, const od::Model& m) {
        if (r.row == od::kVsync) return vsync_index(m.value[od::kVsync]);
        return static_cast<int>(m.index(r.row));
    }

    // The view of one row: its list from the pending model, the applied value from the model taken at begin().
    SettingView view(const RowId& r) const {
        SettingView v;
        v.id = r.id;
        v.pending = index_of(r, model_);
        v.applied = index_of(r, applied_);
        switch (r.row) {
        case od::kDisplayMode:
            // The words are the menu text table's (options_menu_texts.inc word.windowed / word.borderless).
            v.english_label = "Display mode";
            v.english_help = "Play in a window or fullscreen. A change\nmust be confirmed within 15 seconds.";
            v.choices = {{"windowed", "Windowed"}, {"borderless", "Fullscreen"}};
            v.needs_confirm = true;
            break;
        case od::kScreenRatio: {
            v.english_label = "Screen ratio";
            v.english_help = "The shape of the picture. Auto fills the\nscreen, a fixed ratio may add black bars.";
            const std::vector<double> list = model_.aspects();
            for (const double a : list) {
                od::Model m = model_;
                m.aspect = a;
                v.choices.push_back({aspect_key(a), text_of(m, od::kScreenRatio)});
            }
            // The applied ratio by its key (a custom one may be listed only while it is in effect).
            v.applied = -1;
            for (std::size_t i = 0; i < v.choices.size(); ++i)
                if (v.choices[i].key == aspect_key(applied_.aspect)) v.applied = static_cast<int>(i);
            break;
        }
        case od::kResolution: {
            v.english_label = "Resolution";
            v.english_help = "The size of the game's picture. Only sizes\nthat fit the screen ratio are offered.";
            for (const od::Model::Size s : model_.resolutions()) {
                const std::string text = std::to_string(s.width) + "x" + std::to_string(s.height);
                v.choices.push_back({text, text});
            }
            // The applied size by its key: the list is the pending ratio's, and may not hold it.
            const std::string applied_key = text_of(applied_, od::kResolution);
            v.applied = -1;
            for (std::size_t i = 0; i < v.choices.size(); ++i)
                if (v.choices[i].key == applied_key) v.applied = static_cast<int>(i);
            break;
        }
        case od::kVsync:
            v.kind = Kind::Toggle;
            v.english_label = "V-Sync";
            v.english_help = "Show each frame in step with the display's\nrefresh, which prevents tearing.";
            v.choices = {{"off", "Off"}, {"on", "On"}};
            break;
        case od::kRenderResolution: {
            v.english_label = "Resolution scale";
            v.english_help = "The 3D picture's sharpness: higher is\nsharper, lower is faster.";
            const std::vector<double> list = model_.scales();
            for (const double p : list) {
                od::Model m = model_;
                m.scale = p;
                v.choices.push_back({format("%g", p), text_of(m, od::kRenderResolution)});
            }
            v.applied = -1;
            for (std::size_t i = 0; i < v.choices.size(); ++i)
                if (v.choices[i].key == format("%g", applied_.scale)) v.applied = static_cast<int>(i);
            break;
        }
        default:
            break;
        }
        return v;
    }

    DisplaySettings base_{};
    od::Model model_{};
    od::Model applied_{};
    std::uint64_t generation_ = 0;
    bool ratio_picked_ = false;  // the ratio row was changed on the page since it opened (a window takes its shape)
    bool resolution_picked_ = false;  // the player chose a size after that: a mode change must not undo it
};

class DisplayProvider final : public ModelProvider {
public:
    Category category() const override { return Category::Display; }

    std::vector<SettingView> views() const override {
        std::vector<SettingView> out;
        for (const RowId& r : kDisplayRows) out.push_back(view(r));
        return out;
    }

    bool dirty() const override { return wanted() != display_settings().current(); }

    ApplyResult apply(std::string* error) override {
        const DisplaySettings now = display_settings().current();
        const DisplaySettings want = wanted();
        if (want == now) {
            take();
            return ApplyResult::Nothing;
        }
        // Only a mode change can leave the screen showing nothing: it is tried out. A ratio, V-Sync or a window
        // size are committed at once.
        const bool risky = want.fullscreen != now.fullscreen;
        if (!display_settings().apply(want, error, risky)) return ApplyResult::Failed;
        take();
        return risky ? ApplyResult::TrialStarted : ApplyResult::Applied;
    }

    void reset_defaults() override {
        DisplaySettings d = display_settings().current();
        copy_display_fields(d, default_display_settings());
        model_ = od::Model::from(d);
        const DisplaySettings defaults = default_display_settings();
        model_.window = {static_cast<u32>(defaults.window_width), static_cast<u32>(defaults.window_height)};
        ratio_picked_ = true;  // Auto: no reshaping, but a fixed default would be applied as picked
        resolution_picked_ = true;  // the default size stands
    }

    void trial_keep() override {
        display_settings().commit();
        take();
    }
    void trial_revert() override {
        display_settings().rollback();  // the committed values: the trial never reaches display.cfg
        take();
    }

protected:
    const RowId* rows(std::size_t* count) const override {
        *count = sizeof(kDisplayRows) / sizeof(kDisplayRows[0]);
        return kDisplayRows;
    }
    void keep_edits(DisplaySettings& onto, const DisplaySettings& pending, const DisplaySettings& applied) const override {
        keep_edit(onto.fullscreen, pending.fullscreen, applied.fullscreen);
        keep_edit(onto.aspect, pending.aspect, applied.aspect);
        keep_edit(onto.vsync, pending.vsync, applied.vsync);
    }

private:
    // The settings the page asks for: its rows, and the window's size when the Resolution row or a ratio changed it.
    DisplaySettings wanted() const {
        const DisplaySettings now = display_settings().current();
        DisplaySettings want = now;
        copy_display_fields(want, model_.settings(now));
        if (!want.fullscreen &&
            (model_.window.width != applied_.window.width || model_.window.height != applied_.window.height)) {
            want.window_width = static_cast<int>(model_.window.width);
            want.window_height = static_cast<int>(model_.window.height);
        }
        return want;
    }
};

class GraphicsProvider final : public ModelProvider {
public:
    Category category() const override { return Category::Graphics; }

    std::vector<SettingView> views() const override {
        std::vector<SettingView> out;
        for (const RowId& r : kGraphicsRows) out.push_back(view(r));
        return out;
    }

    bool dirty() const override {
        const DisplaySettings now = display_settings().current();
        DisplaySettings want = now;
        copy_graphics_fields(want, model_.settings(now));
        return want != now;
    }

    ApplyResult apply(std::string* error) override {
        const DisplaySettings now = display_settings().current();
        DisplaySettings want = now;
        copy_graphics_fields(want, model_.settings(now));
        if (want == now) {
            take();
            return ApplyResult::Nothing;
        }
        if (!display_settings().apply(want, error)) return ApplyResult::Failed;
        take();
        return ApplyResult::Applied;
    }

    void reset_defaults() override {
        DisplaySettings d = display_settings().current();
        copy_graphics_fields(d, default_display_settings());
        model_ = od::Model::from(d);
    }

protected:
    const RowId* rows(std::size_t* count) const override {
        *count = sizeof(kGraphicsRows) / sizeof(kGraphicsRows[0]);
        return kGraphicsRows;
    }
    void keep_edits(DisplaySettings& onto, const DisplaySettings& pending, const DisplaySettings& applied) const override {
        keep_edit(onto.render_scale, pending.render_scale, applied.render_scale);
    }
};

}  // namespace

DisplaySettings default_display_settings() { return DisplaySettings{}; }

std::unique_ptr<Provider> make_display_provider() { return std::make_unique<DisplayProvider>(); }
std::unique_ptr<Provider> make_graphics_provider() { return std::make_unique<GraphicsProvider>(); }

}  // namespace gekko::settings
