// The PC Options menu's settings registry (gekko/settings_registry.h).

#include "gekko/settings_registry.h"

#include <array>

#include "gekko/display_settings.h"

namespace gekko::settings {

namespace {
constexpr std::array<std::string_view, kCategoryCount> kPrefixes = {"display", "graphics", "audio",
                                                                    "controls", "gameplay", "system"};
constexpr unsigned index(Category c) noexcept { return static_cast<unsigned>(c); }
}  // namespace

std::string_view category_prefix(Category category) noexcept {
    return index(category) < kCategoryCount ? kPrefixes[index(category)] : std::string_view{};
}

bool category_of(std::string_view id, Category* category) noexcept {
    const std::size_t dot = id.find('.');
    if (dot == std::string_view::npos) return false;
    const std::string_view prefix = id.substr(0, dot);
    for (unsigned i = 0; i < kCategoryCount; ++i) {
        if (kPrefixes[i] == prefix) {
            if (category != nullptr) *category = static_cast<Category>(i);
            return true;
        }
    }
    return false;
}

void Registry::add(std::unique_ptr<Provider> provider) {
    if (!provider) return;
    std::lock_guard lock(mutex_);
    const unsigned i = index(provider->category());
    if (i < kCategoryCount) providers_[i] = std::move(provider);
}

bool Registry::has(Category category) const {
    std::lock_guard lock(mutex_);
    return index(category) < kCategoryCount && providers_[index(category)] != nullptr;
}

Provider* Registry::provider_for(std::string_view id) const {
    Category c{};
    if (!category_of(id, &c)) return nullptr;
    return providers_[index(c)].get();
}

std::vector<SettingView> Registry::list(Category category) const {
    std::lock_guard lock(mutex_);
    if (index(category) >= kCategoryCount || !providers_[index(category)]) return {};
    providers_[index(category)]->sync();
    return providers_[index(category)]->views();
}

void Registry::begin(Category category) {
    std::lock_guard lock(mutex_);
    if (index(category) < kCategoryCount && providers_[index(category)]) providers_[index(category)]->begin();
}

bool Registry::step(std::string_view id, int delta) {
    std::lock_guard lock(mutex_);
    Provider* p = provider_for(id);
    if (p != nullptr) p->sync();
    return p != nullptr && p->step(id, delta);
}

bool Registry::set(std::string_view id, int value) {
    std::lock_guard lock(mutex_);
    Provider* p = provider_for(id);
    if (p != nullptr) p->sync();
    return p != nullptr && p->set(id, value);
}

bool Registry::run(std::string_view id, std::string* error) {
    std::lock_guard lock(mutex_);
    Provider* p = provider_for(id);
    if (p == nullptr) {
        if (error != nullptr) *error = "no such setting";
        return false;
    }
    return p->run(id, error);
}

bool Registry::dirty(Category category) const {
    std::lock_guard lock(mutex_);
    if (index(category) >= kCategoryCount || !providers_[index(category)]) return false;
    providers_[index(category)]->sync();
    return providers_[index(category)]->dirty();
}

ApplyResult Registry::apply_pending(Category category, std::string* error) {
    std::lock_guard lock(mutex_);
    if (index(category) >= kCategoryCount || !providers_[index(category)]) {
        if (error != nullptr) *error = "no such category";
        return ApplyResult::Failed;
    }
    Provider& p = *providers_[index(category)];
    p.sync();
    if (!p.dirty()) return ApplyResult::Nothing;
    if (trial_active_) {
        ++report_.apply_failures;
        if (error != nullptr) *error = "a display change is waiting for confirmation";
        return ApplyResult::Failed;
    }
    std::string why;
    const ApplyResult result = p.apply(&why);
    switch (result) {
    case ApplyResult::Applied:
        ++report_.applies;
        if (!p.save(&why)) {
            ++report_.save_failures;
            if (error != nullptr) *error = why;
            return ApplyResult::Failed;
        }
        ++report_.saves;
        return ApplyResult::Applied;
    case ApplyResult::TrialStarted:
        ++report_.applies;
        ++report_.trials;
        trial_active_ = true;
        trial_category_ = category;
        trial_verify_only_ = false;
        trial_generation_ = display_settings().generation();
        trial_deadline_ = (clock_ != nullptr ? clock_() : Clock::now()) + kTrialLength;
        return ApplyResult::TrialStarted;
    case ApplyResult::Failed:
        ++report_.apply_failures;
        if (error != nullptr) *error = why;
        return ApplyResult::Failed;
    case ApplyResult::Nothing:
    default:
        return result;
    }
}

TrialState Registry::trial() const {
    std::lock_guard lock(mutex_);
    TrialState t;
    if (!trial_active_) return t;
    t.active = true;
    t.category = trial_category_;
    t.verify_only = trial_verify_only_;
    const auto now = clock_ != nullptr ? clock_() : Clock::now();
    t.seconds_left = now >= trial_deadline_
                         ? 0.0
                         : std::chrono::duration<double>(trial_deadline_ - now).count();
    return t;
}

void Registry::end_trial(bool keep) {
    Provider* p = providers_[index(trial_category_)].get();
    trial_active_ = false;
    if (trial_verify_only_) {  // Alt+Enter's: no provider snapshot holds it
        trial_verify_only_ = false;
        if (keep) display_settings().commit();
        else display_settings().rollback();
        return;
    }
    if (p == nullptr) return;
    if (keep) p->trial_keep();
    else p->trial_revert();
}

bool Registry::confirm(std::string* error) {
    std::lock_guard lock(mutex_);
    if (!trial_active_) return true;
    Provider* p = providers_[index(trial_category_)].get();
    end_trial(true);
    ++report_.confirms;
    if (p != nullptr) {
        std::string why;
        if (!p->save(&why)) {
            ++report_.save_failures;
            if (error != nullptr) *error = why;
            return false;
        }
        ++report_.saves;
    }
    return true;
}

void Registry::revert() {
    std::lock_guard lock(mutex_);
    if (!trial_active_) return;
    end_trial(false);
    ++report_.reverts;
}

void Registry::discard_pending(Category category) {
    std::lock_guard lock(mutex_);
    if (index(category) < kCategoryCount && providers_[index(category)]) providers_[index(category)]->discard();
}

void Registry::reset_defaults(Category category) {
    std::lock_guard lock(mutex_);
    if (index(category) < kCategoryCount && providers_[index(category)])
        providers_[index(category)]->reset_defaults();
}

bool Registry::save(Category category, std::string* error) {
    std::lock_guard lock(mutex_);
    if (index(category) >= kCategoryCount || !providers_[index(category)]) return false;
    if (trial_active_ && trial_category_ == category) {
        if (error != nullptr) *error = "a display change is waiting for confirmation";
        return false;
    }
    std::string why;
    if (!providers_[index(category)]->save(&why)) {
        ++report_.save_failures;
        if (error != nullptr) *error = why;
        return false;
    }
    ++report_.saves;
    return true;
}

bool Registry::toggle_fullscreen(std::string* error) {
    std::lock_guard lock(mutex_);
    if (trial_active_) {
        ++report_.apply_failures;
        if (error != nullptr) *error = "a display change is waiting for confirmation";
        return false;
    }
    DisplaySettingsService& service = display_settings();
    DisplaySettings want = service.current();
    want.fullscreen = !want.fullscreen;
    ++report_.toggles;
    if (want.fullscreen && want.exclusive) {
        // A display mode change may fail on this monitor: provisional until the window owner reports.
        if (!service.apply(want, error, true)) return false;
        trial_active_ = true;
        trial_category_ = Category::Display;
        trial_verify_only_ = true;
        trial_generation_ = service.generation();
        trial_deadline_ = (clock_ != nullptr ? clock_() : Clock::now()) + kTrialLength;
        ++report_.trials;
        return true;
    }
    if (!service.apply(want, error)) return false;
    std::string why;
    if (!service.save(&why)) {
        ++report_.save_failures;
        if (error != nullptr) *error = why;
        return false;
    }
    ++report_.saves;
    return true;
}

std::string Registry::last_failure(std::uint64_t* serial) const {
    std::lock_guard lock(mutex_);
    if (serial != nullptr) *serial = failure_serial_;
    return last_failure_;
}

void Registry::check_display_trial() {
    if (!trial_active_ || trial_category_ != Category::Display) return;
    const DisplaySettings want = display_settings().current();
    if (!(want.fullscreen && want.exclusive)) return;
    const DisplayWindowState state = display_settings().window_state();
    if (state.generation < trial_generation_) return;  // the window owner has not applied it yet
    if (!state.exclusive_active) {
        last_failure_ = state.note.empty() ? std::string("exclusive fullscreen was refused") : state.note;
        ++failure_serial_;
        ++report_.refusals;
        const bool verify_only = trial_verify_only_;
        if (verify_only) {
            trial_active_ = false;
            display_settings().rollback();
        } else {
            end_trial(false);
        }
        ++report_.reverts;
        return;
    }
    if (trial_verify_only_) {
        trial_active_ = false;
        display_settings().commit();
        std::string why;
        if (display_settings().save(&why)) ++report_.saves;
        else ++report_.save_failures;
        ++report_.confirms;
    }
}

void Registry::tick(Clock::time_point now) {
    std::lock_guard lock(mutex_);
    check_display_trial();
    if (!trial_active_) return;
    if (clock_ != nullptr) now = clock_();
    if (now < trial_deadline_) return;
    end_trial(false);
    ++report_.expiries;
}

Registry::Report Registry::report() const {
    std::lock_guard lock(mutex_);
    return report_;
}

Registry& registry() {
    static Registry r;
    return r;
}

}  // namespace gekko::settings
