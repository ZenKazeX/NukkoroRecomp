#pragma once

// Development savestate support.
//
// This is a debugging and regression tool, not a user-facing feature. It exists
// so a rendering change can be A/B tested from an identical gameplay position
// without a human driving the controller through the menus: reaching Lava
// Shelter takes ~35.7e9 retired instructions, and the project deliberately has
// no scripted-input path (docs/NATIVE_RUNTIME_ARCHITECTURE.md).
//
// What is captured is the GAMECUBE MACHINE MODEL the game runs on, not the host renderer (NATIVE_AUDIT.md C23,
// lies list 17): CPU architected state, the MEM1 window, ARAM (sample memory), the locked cache, and the
// device models' state (VI field clock, interrupt latches, GX sync, the GX immediate builder, card, IPL, audio).
// The GX register files, TMEM and MMIO devices of earlier versions no longer exist. Host-side caches (texture/palette caches, DX12 resources, the
// EFB itself) are content-addressed or regenerated from the restored guest
// state and are deliberately not serialised.
//
// Version 20 also carries host decoder and mixer history, including fractional
// source position and filter/compressor tails. Version 21 adds preceding main
// surround history. Version 22 retains both matrix blocks and their phase.
// Versions 20/21 warn about missing history on output-mode changes; version 20
// also warns about the absent surround block.
// Version 19 loads with a warning:
// those files cannot reproduce history they never saved. Host audio-device queues
// are flushed by the caller; wall-clock playback latency is not checkpoint state.
//
// Save and load MUST happen at a guest-ordering boundary with all guest state
// committed and no guest function on the native stack. In practice that means
// between run_slice() calls, or from System's host-service callback.
// save_state()/load_state() check the preconditions they can and refuse rather
// than producing a state that only appears to work.
//
// The legacy GX register raster (D13-2, D13-3) exists only in the reference
// executable. When its launcher has one, it requires Renderer::quiescent()
// before save_state (a deferred copy would otherwise be saved as stale MEM1
// bytes and land over restored MEM1 after a load) and calls
// Renderer::on_state_loaded() after load_state (dropping host state keyed by
// guest addresses of the discarded timeline). The shipping executable has no
// renderer that writes guest memory, so neither precondition exists there.

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

#include "gekko/types.h"

namespace gekko {

class System;

// Shadow's motion-blur history is temporal host/GPU state even though the
// guest keeps its validity byte in MEM1.  A savestate restores the byte but
// cannot restore the matching native EFB-copy texture.  Clear that guest gate
// at a load boundary so SaveScreen produces a new resolution-correct history
// before DrawBlur is allowed to consume it.  Exposed for the regression test;
// normal callers get this automatically through load_state().
bool invalidate_restored_shadow_render_history(System& system) noexcept;

namespace state {

// Little-endian host-native POD serialisation. The file is a host development
// artefact and is never shipped or moved between machines, so no byte-order or
// alignment normalisation is performed; the version stamp in the header covers
// layout changes.

template <typename T>
void put(std::vector<u8>& out, const T& value) {
    static_assert(std::is_trivially_copyable<T>::value, "savestate fields must be POD");
    const auto* bytes = reinterpret_cast<const u8*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

inline void put_bytes(std::vector<u8>& out, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const u8*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

template <typename T>
bool get(const u8*& cursor, const u8* end, T& value) {
    static_assert(std::is_trivially_copyable<T>::value, "savestate fields must be POD");
    if (static_cast<std::size_t>(end - cursor) < sizeof(T)) return false;
    std::memcpy(&value, cursor, sizeof(T));
    cursor += sizeof(T);
    return true;
}

inline bool get_bytes(const u8*& cursor, const u8* end, void* data, std::size_t size) {
    if (static_cast<std::size_t>(end - cursor) < size) return false;
    std::memcpy(data, cursor, size);
    cursor += size;
    return true;
}

// A length-prefixed byte block, so a vector whose size changes between builds
// fails the version check instead of silently mis-parsing the rest of the file.
inline void put_block(std::vector<u8>& out, const void* data, std::size_t size) {
    put<u64>(out, static_cast<u64>(size));
    put_bytes(out, data, size);
}

inline bool get_block(const u8*& cursor, const u8* end, void* data, std::size_t size) {
    u64 stored = 0;
    if (!get<u64>(cursor, end, stored)) return false;
    if (stored != static_cast<u64>(size)) return false;
    return get_bytes(cursor, end, data, size);
}

}  // namespace state

// The version stamped in every savestate header (its history: savestate.cpp).
// The boot-contract fragments carry it too: their CPU and device blocks are
// the savestate's.
u32 save_state_version() noexcept;

// The renderer's part of a savestate (version 19). Each executable has one
// renderer, and a renderer can keep state the game observes outside guest
// memory: the legacy build's capture (RwCapture: the GX state its renderer
// replays), the lifted build's R2 driver (RenderWare's render-state values,
// which the lowered driver keeps natively, and the 2D layout's remembered
// anchors). The header names the kind, and a savestate loads only into an
// executable of the same kind: the two renderers also leave different words in
// the game's own RenderWare objects (the legacy driver's TMEM-region words, the
// lifted driver's native handles), so a file of the other kind is a different
// format - refused before anything is restored. The before / after workstation
// (tools/comparator) brings each build to a checkpoint with its own savestate.
enum class RendererStateKind : u32 { None = 0, Capture = 1, Lifted = 2 };
const char* renderer_state_kind_name(RendererStateKind kind) noexcept;

struct RendererStateHooks {
    RendererStateKind kind = RendererStateKind::None;
    std::function<void(std::vector<u8>&)> save;
    std::function<bool(const u8*&, const u8*)> load;
    // After a successful load: drop what the renderer derived from the replaced
    // timeline.
    std::function<void()> loaded;
    // Guest words the renderer writes for itself (the lifted driver's native
    // handles in RenderWare objects): saved as 0 - "nothing built yet", what the
    // translated factories leave there - so a savestate holds no value that
    // only meant something to the process that wrote it.
    std::function<void(std::vector<u32>&)> own_words;
};
// The lifted build's renderer registers its hooks before the game runs (one per
// process). The legacy build needs none: its kind and its block come from
// System::native_capture().
void set_renderer_state_hooks(RendererStateHooks hooks);
// This executable's kind.
RendererStateKind renderer_state_kind(System& system) noexcept;
// Runs `body` with the renderer's own guest words (RendererStateHooks::own_words)
// reading 0, as a savestate writes them, and puts them back afterwards. At a
// boundary only (no translated code running).
void with_renderer_words_cleared(System& system, const std::function<void()>& body);

// A savestate's header, read without restoring anything.
struct StateHeader {
    u32 version = 0;
    RendererStateKind kind = RendererStateKind::None;
    u64 game_ticks = 0;
};
bool read_state_header(const std::filesystem::path& path, StateHeader* out, std::string* error = nullptr);

// Writes the guest machine to `path`. Returns false and fills `error` on any
// failure, including a precondition violation (pending GX work in flight).
bool save_state(System& system, const std::filesystem::path& path,
                std::string* error = nullptr);

// Restores the guest machine from `path`. On failure the machine is left
// untouched when the failure is detected before any state is applied; a failure
// after that point is reported and the machine must be considered unusable.
bool load_state(System& system, const std::filesystem::path& path,
                std::string* error = nullptr);

}  // namespace gekko
