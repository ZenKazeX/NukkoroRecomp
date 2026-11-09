// Where the game's save file lives and the one-time import into it (gekko/save_import.h).

#include "gekko/save_import.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

#include "gekko/save_file.h"

namespace gekko::save_import {

namespace {

std::string lower_extension(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

bool same_file(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    if (std::filesystem::exists(a, ec) && std::filesystem::exists(b, ec)) return std::filesystem::equivalent(a, b, ec);
    return std::filesystem::weakly_canonical(a, ec) == std::filesystem::weakly_canonical(b, ec);
}

std::filesystem::path not_the_source(std::filesystem::path save, const std::optional<std::filesystem::path>& source) {
    if (source && same_file(save, *source)) save.replace_filename(save.stem().string() + "-pc" + save.extension().string());
    return save;
}

bool read_bytes(const std::filesystem::path& p, std::vector<std::uint8_t>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !in.bad();
}

// The game's file from one source: a GCI, or a card image (.raw / .pc.raw / anything else).
bool take(const std::filesystem::path& source, save_file::Gci& gci, std::string& why) {
    std::error_code ec;
    if (!std::filesystem::exists(source, ec)) {
        why = "absent";
        return false;
    }
    if (lower_extension(source) == ".gci") {
        if (!save_file::read_gci(source, gci, &why)) return false;
        if (gci.game_code().substr(0, 3) != "GUP" || gci.file_name() != "SHADOWTHEHEDGEHOG") {
            why = "not this game's save";
            return false;
        }
    } else {
        std::vector<std::uint8_t> card;
        if (!read_bytes(source, card)) {
            why = "cannot be read";
            return false;
        }
        // A card image of any region, this build's own first.
        bool found = false;
        for (const char* game : {"GUPP", "GUPE", "GUPJ"}) {
            if (save_file::extract_from_card(card, game, "8P", "SHADOWTHEHEDGEHOG", gci, &why)) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    if (gci.data.size() < save_file::kRecordOffset + save_file::kRecordBytes) {
        why = "too short";
        return false;
    }
    return true;
}

}  // namespace

Paths resolve(const std::optional<std::filesystem::path>& save_file_option,
              const std::optional<std::filesystem::path>& memory_card_option, const std::filesystem::path& root,
              const std::filesystem::path& save_data) {
    Paths p;
    p.card_source = memory_card_option;
    if (!p.card_source) {
        const std::filesystem::path completed = root / "GC" / "EUR" / "Card A" / "8P-GUPP-SHADOWTHEHEDGEHOG.gci";
        std::error_code ec;
        if (std::filesystem::exists(completed, ec)) p.card_source = completed;
    }
    if (p.card_source && lower_extension(*p.card_source) == ".gci")
        p.working_image = save_data / (p.card_source->stem().string() + ".pc.raw");
    if (save_file_option) {
        p.save = *save_file_option;
    } else if (!p.card_source) {
        p.save = save_data / "ShadowTheHedgehog.gci";
    } else if (lower_extension(*p.card_source) == ".gci") {
        p.save = save_data / (p.card_source->stem().string() + ".gci");
    } else {
        // A card image (.raw, .pc.raw): the save beside it, "card.raw" -> "card.gci", "x.pc.raw" -> "x.gci".
        std::string stem = p.card_source->stem().string();
        if (stem.size() > 3 && stem.compare(stem.size() - 3, 3, ".pc") == 0) stem.resize(stem.size() - 3);
        p.save = p.card_source->parent_path() / (stem + ".gci");
    }
    p.save = not_the_source(p.save, p.card_source);
    if (!p.working_image.empty()) p.save = not_the_source(p.save, p.working_image);
    return p;
}

Result import_if_missing(const Paths& paths) {
    Result r;
    std::error_code ec;
    if (std::filesystem::exists(paths.save, ec)) {
        r.existed = true;
        r.message = "save file " + paths.save.string();
        // An earlier import could leave another region's game code in the file; the save store looks for this build's.
        save_file::Gci existing;
        std::string why;
        const bool other_region = save_file::read_gci(paths.save, existing, &why) && existing.game_code() != "GUPP" &&
                                  existing.game_code().substr(0, 3) == "GUP" && existing.file_name() == "SHADOWTHEHEDGEHOG";
        if (other_region) {
            const std::string region = existing.game_code();
            const std::filesystem::path backup = paths.save.string() + ".before-region-fix";
            std::filesystem::copy_file(paths.save, backup, std::filesystem::copy_options::skip_existing, ec);
            if (ec) {
                r.message += " is a " + region + " save, left as it is: its backup failed";
            } else if (!save_file::to_canonical_pc_gci(existing, &why) || !save_file::write_gci(paths.save, existing, &why)) {
                r.message += " is a " + region + " save, left as it is: " + why;
            } else {
                r.message += " converted from " + region;
            }
        }
        return r;
    }
    std::vector<std::filesystem::path> sources;
    if (!paths.working_image.empty()) sources.push_back(paths.working_image);
    if (paths.card_source) sources.push_back(*paths.card_source);
    std::string skipped;
    for (const std::filesystem::path& source : sources) {
        save_file::Gci gci;
        std::string why;
        if (!take(source, gci, why)) {
            skipped += "; " + source.string() + ": " + why;
            continue;
        }
        const save_file::RecordCheck check = save_file::check_record(gci.data);
        if (!check.valid) {
            skipped += "; " + source.string() + ": its record fails the game's check";
            continue;
        }
        std::filesystem::create_directories(paths.save.parent_path(), ec);
        std::string error;
        // Another region's save: the file the save store looks for (the record itself is the same in every region).
        const std::string region = gci.game_code();
        if (region != "GUPP" && !save_file::to_canonical_pc_gci(gci, &error)) {
            skipped += "; " + source.string() + ": " + error;
            continue;
        }
        if (!save_file::write_gci(paths.save, gci, &error)) {
            r.message = "the save could not be written: " + error;
            return r;
        }
        {
            std::ofstream note(paths.save.string() + ".source", std::ios::binary | std::ios::trunc);
            note << "imported from " << source.string() << "\n";
        }
        r.imported = true;
        r.source = source.string();
        r.has_options = true;
        r.text_language = check.text_language;
        r.speaker_mode = check.speaker_mode;
        r.vibration = check.vibration != 0;
        r.message = "save file " + paths.save.string() + " imported from " + source.string() +
                    (region != "GUPP" ? " (converted from " + region + ")" : std::string()) +
                    (skipped.empty() ? std::string() : " (skipped" + skipped + ")");
        return r;
    }
    r.message = "no save to import" + (sources.empty() ? std::string(" (no card source)") : skipped) +
                ": the game starts without a save file";
    return r;
}

}  // namespace gekko::save_import
