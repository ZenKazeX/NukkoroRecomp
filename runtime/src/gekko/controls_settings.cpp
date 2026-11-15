#include "gekko/controls_settings.h"

#include <system_error>

#include "gekko/config_file.h"

namespace gekko {

using shadow::peripheral::ControlsConfig;

namespace {

bool file_stamp(const std::filesystem::path& path, std::filesystem::file_time_type& stamp) {
    std::error_code ec;
    stamp = std::filesystem::last_write_time(path, ec);
    return !ec;
}

}  // namespace

void ControlsSettings::publish(const ControlsConfig& config) {
    current_ = std::make_shared<const ControlsConfig>(config);
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

void ControlsSettings::load(const std::filesystem::path& path, std::vector<std::string>* warnings) {
    std::string text, error;
    if (!config_file::read(path, text, &error) && warnings != nullptr) warnings->push_back(path.string() + ": " + error);
    ControlsConfig config = shadow::peripheral::default_controls();
    std::vector<std::string> problems;
    shadow::peripheral::parse_controls(text, config, &problems);
    if (warnings != nullptr) {
        for (const std::string& problem : problems) warnings->push_back(path.string() + " " + problem);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = path;
    have_stamp_ = file_stamp(path, stamp_);
    publish(config);
}

std::shared_ptr<const ControlsConfig> ControlsSettings::current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

std::filesystem::path ControlsSettings::path() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return path_;
}

bool ControlsSettings::apply(const ControlsConfig& config, std::string* error) {
    if (!shadow::peripheral::validate_controls(config, error)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    publish(config);
    return true;
}

void ControlsSettings::restore_defaults() {
    std::lock_guard<std::mutex> lock(mutex_);
    publish(shadow::peripheral::default_controls());
}

bool ControlsSettings::save(std::string* error) {
    std::shared_ptr<const ControlsConfig> config;
    std::filesystem::path path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config = current_;
        path = path_;
    }
    if (path.empty()) {
        if (error != nullptr) *error = "controls: no controls.cfg path (not loaded)";
        return false;
    }
    std::vector<config_file::KeyValue> values;
    for (const shadow::peripheral::ControlsSetting& row : shadow::peripheral::controls_settings(*config)) {
        values.push_back({{row.key}, row.value});
    }
    if (!config_file::update(path, values, error)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    have_stamp_ = file_stamp(path, stamp_);
    return true;
}

bool ControlsSettings::reload_if_changed(std::vector<std::string>* warnings) {
    std::filesystem::path path;
    std::filesystem::file_time_type known{};
    bool had = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = path_;
        known = stamp_;
        had = have_stamp_;
    }
    if (path.empty()) return false;
    std::filesystem::file_time_type now{};
    const bool exists = file_stamp(path, now);
    if (exists == had && (!exists || now == known)) return false;
    const auto before = current();
    load(path, warnings);
    return !(*before == *current());
}

ControlsSettings& controls_settings() {
    static ControlsSettings instance;
    return instance;
}

}  // namespace gekko
