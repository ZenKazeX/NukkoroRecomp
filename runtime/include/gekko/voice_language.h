#pragma once

// Japanese voices for the PAL game (the user's request of 2026-11-05).
//
// The PAL game keeps the original voice-language switch (the hidden Options item 5, 0x801DFA40: the sound manager's
// voice banks and the readers of sng_EV%04d_J.adx / PRS_VOICE_J.afs) and ships the in-game Japanese banks
// (sound/B3/B4/B5_*_J.mlt), but not the Japanese cutscene and dialogue voices. Those come from the player's own
// Japanese disc: PRS_VOICE_J.afs (4480 entries, as PRS_VOICE_E.afs) and the 78 sng_*_J.adx files that match the PAL
// disc's 78 sng_*_E.adx one to one. They are served beside the PAL tree under their own names
// (shadow::backends::StorageOptions::extra_files), never replacing a PAL file.
//
// Where they are looked for, in order: <game root>/voice_jp/ (a folder the player fills), then
// <game root>/../EXTRACTED_DISKS/Japan/files/ (this workspace's Japanese extract). Japanese is offered only when
// PRS_VOICE_J.afs and all of the sng_*_J.adx files the PAL tree's English ones call for are found.

#include <filesystem>
#include <string>
#include <vector>

namespace gekko {

// The Japanese disc's character voice banks sound/B3_SHADOW_J.mlt, B4_SUBHERO_J.mlt and B5_SUBDARK_J.mlt. The PAL
// disc's own *_J.mlt are NOT Japanese: they are byte-identical to the Japanese disc's *_E.mlt (an older revision,
// other program layout for B3), while the Japanese disc's *_J.mlt share the PAL *_E.mlt program layout. They replace
// the PAL files of the same name (AssetSystem::replace) when complete and structurally compatible. Filled by
// find_japanese_voice_files (empty when the three files are not all found).
struct JapaneseBank {
    std::string game_path;             // "sound/B3_SHADOW_J.mlt"
    std::filesystem::path host_file;   // the Japanese disc's file
};
const std::vector<JapaneseBank>& japanese_banks() noexcept;
// Whether a Japanese bank has the PAL English bank's program layout (same program unit size and head).
bool japanese_bank_compatible(const std::vector<unsigned char>& japanese, const std::vector<unsigned char>& pal_english);

// The Japanese voice files for the game root (its files/ is the PAL tree); empty when they are not complete.
std::vector<std::filesystem::path> find_japanese_voice_files(const std::filesystem::path& game_root);

// Whether the Japanese voices are served (the launcher sets it after the mount; the Options menu reads it).
void set_japanese_voices_available(bool available) noexcept;
bool japanese_voices_available() noexcept;

// The two dialogue-voice archives' host files (find_japanese_voice_files fills them; empty paths when not found).
struct VoiceArchivePaths {
    std::filesystem::path english;   // <game root>/files/PRS_VOICE_E.afs
    std::filesystem::path japanese;  // the Japanese disc's PRS_VOICE_J.afs
};
const VoiceArchivePaths& voice_archive_paths() noexcept;

// The dialogue-voice archive both languages share (the user's request of 2026-11-05: a voice change as seamless as
// the text change - no reload, no sound stop, the menu music goes on). PRS_VOICE_E.afs and PRS_VOICE_J.afs both hold
// 4480 entries but at different offsets and sizes, and the game reads the AFS table of contents once, when it opens
// the archive. So one composed archive is served under the English name: its table gives entry i a slot as large as
// the larger of the two clips (2048-aligned), and a read of slot i comes from the English or the Japanese file -
// whichever `japanese()` says at that read - with zeros past the clip. Nothing is copied; reads go to the disc files.
class VoiceArchive {
public:
    // Opens and indexes both files; false (with the reason) when either is missing or their tables differ in count.
    bool open(const VoiceArchivePaths& paths, std::string* error);
    unsigned long long size() const noexcept { return size_; }
    // `japanese`: serve the Japanese clips. Thread-safe.
    bool read(unsigned long long offset, unsigned long long length, unsigned char* dest, unsigned long long& got,
              bool japanese);
    ~VoiceArchive();

private:
    struct Entry {
        unsigned long long slot, slot_size;            // in the composed archive
        unsigned long long source[2], source_size[2];  // [0] English, [1] Japanese
    };
    std::vector<unsigned char> head_;   // the composed table of contents (offset 0)
    std::vector<unsigned char> names_;  // the English name table, after the last slot
    unsigned long long names_at_ = 0, size_ = 0;
    std::vector<Entry> entries_;
    void* files_[2] = {nullptr, nullptr};  // std::FILE*
    void* lock_ = nullptr;                 // std::mutex
};

}  // namespace gekko
