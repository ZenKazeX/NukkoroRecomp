#if !defined(SHADOW_RENDER_LIFTED)  // G5
#include "gekko/rw_capture.h"
#endif
#include "gekko/savestate.h"

#include <cstdio>
#include <fstream>

#include "gekko/guest_memory.h"
#include "gekko/profiling.h"
#include "gekko/system.h"

// Development savestate. See gekko/savestate.h for the contract.
//
// The machine's own blocks are serialised by their owners (Cpu, the native
// platform layers, the capture); this translation unit orchestrates them and
// keeps the format's history.

namespace gekko {

namespace {

constexpr u32 kSaveMagic = 0x4B474B53u;  // 'SKGK'
// Bump whenever any serialised field is added, removed or reordered. A stale
// file then fails the header check instead of restoring a plausible-looking
// machine built from mis-parsed bytes.
// Version 6: the CPU block carries the lwarx/stwcx. reservation (same size).
// Version 7: Cpu::save_state appends its timeline block (hardware-service
// phase, data-fault latch); Cpu::load_state zeroes it for versions 3-6.
// Version 8: a trailing native-capture block (u8 present, u32 byte length, then
// RwCapture::save_state): the GX state the native renderer replays on load.
// Version 9: the DI block is gone with the disc-interface register model.
// Version 10 (native time, docs/native-time-20261005/DESIGN.md): the header's
// u64 is the game clock, not a retired-instruction count; the CPU block carries
// the time-base/decrementer views and the GameClock (Cpu::save_state); the AI
// and DSP register blocks are replaced by the native interrupt and native audio
// blocks. Older files are refused: their timelines were measured in retired
// instructions, which have no game-time equivalent.
// Version 11 (native video): the VI block is gone; the native video block
// follows the native interrupt block.
// Version 12 (native controller input, STAGE3_PAD_SI.md): the SI block is
// gone; the native pad block follows the native video block.
// Version 13 (native GX synchronisation, STAGE4_GX_SYNC.md): the CP and PE
// blocks and the PI's CPU FIFO words are gone; the native GX sync block
// follows the native audio block, and the native interrupt block carries the
// three GX sources.
// Version 14 (phase 4, the GX command data plane, GX_DATA_PLANE.md): the GX
// block - the decoder's BP / CP / XF register files, TMEM and the partially
// consumed command stream - is gone with the write-gather pipe; the native
// immediate geometry block (an open primitive) follows the native GX sync.
// Version 15 (stage 5, the external interface, STAGE5_EXI.md): the EXI block -
// the channel registers, the IPL chip's SRAM image and the card protocol - is
// gone with the device model; the native card block (each channel's pending
// completion) and the IPL services block (the 68-byte settings image) follow
// the native immediate geometry block, and the native interrupt block carries
// the card source. The card image is its host file (flushed before a save).
// Version 16 (stage 6, the processor and memory interfaces, STAGE6_PI_MI.md):
// the PI block (cause, mask, reset flag, counters) and the MI block (its
// register file) are gone with the models - no device has registers; the
// native interrupt block comes first.
// Version 17 (docs/memory-architecture-20261006, M1): guest memory is the
// game's address window - every region with its committed bytes (the program
// image, the M1 legacy OS heap range, the locked-cache scratch) - and the
// audio sample store, instead of the GameCube's MEM1 / ARAM / locked-cache
// images.
// Version 18 (docs/lift-20261006/L1_INPUT.md): the native pad block (the PAD
// library's enabled channels, spec, motors and latched samples) is replaced
// by the lifted peripheral layer's block: the latched controller samples in
// the game's controls and the rumble requests.
// Version 19 (docs/comparator/README.md): the header's game clock is followed
// by the executable's renderer kind (savestate.h RendererStateKind), and the
// trailing v8 capture block (u8 present, u32 length) becomes the renderer's
// block (u32 length, then the kind's payload: RwCapture's block, or the lifted
// R2 driver's). A file loads only into an executable of its kind; the lifted
// driver's handle words are saved as 0.
// Version 20: append decoder/SRC/compressor history after the renderer block.
// Version 19 remains readable, with an explicit missing-audio-history warning.
// Version 21: native output routing retains the preceding main surround block.
// Version 22: both retained matrix blocks and phase survive output-mode changes.
// Version 23: the native room effect's binding, parameters, tail and pending blocks follow the mixer history.
// Version 24 (docs/ARAM.md step 5): no ARAM image after the guest window - the sound samples live in native stores,
// regions of the window; the GCAX banks' and the stream ring's placement follows the audio block. Versions 19-23
// load: their ARAM image is taken over by the stores (adopt_aram_image).
constexpr u32 kSaveVersion = 24u;
constexpr u32 kFirstVersionWithoutAram = 24u;
constexpr u32 kAramImageBytes = 0x01000000u;  // the 16 MiB image versions 19-23 carry

// A state saved before ARAM was retired (versions 19-23) carries ARAM's 16 MiB after its guest window. The game
// state it restores still names ARAM addresses where it kept its sound data; each native store takes over its own
// block at those addresses (docs/ARAM.md "Old savestates"), read from the game's own records:
//   GCAX: the context [r13+0x5FAC], its block at +0x52B0 of +0x52A4 bytes (0x8043F744);
//   CRI's stream ring: the ring record 0x805C4D10, its block at +0x10 of +0xC bytes (fn_803D069C);
//   motion clips: the pool [r13+0x5F98], 6 MiB (0x804225B0).
// A store whose block already sits at its native base (a state from a build that had it) resumes that region. The
// image's other bytes - the SDK's reserve, the space nothing allocated - have no reader and are dropped.
constexpr u32 kSdaGcaxContext = 0x5FACu;
constexpr u32 kGcaxBase = 0x52B0u;
constexpr u32 kGcaxRequested = 0x52A4u;
constexpr u32 kCriRingRecord = 0x805C4D10u;
constexpr u32 kCriRingBytes = 0xCu;
constexpr u32 kCriRingBase = 0x10u;
constexpr u32 kSdaMotionPool = 0x5F98u;
constexpr u32 kMotionPoolBytes = 0x600000u;

bool adopt_block(SoundArena& arena, u32 native_base, u32 base, u32 bytes, const u8* image, const char* what,
                 std::string& why) {
    if (bytes == 0u) return true;
    if (base == native_base) return arena.resume(bytes, why);
    if (base >= kAramImageBytes || bytes > kAramImageBytes - base) {
        why = std::string(what) + ": a block outside ARAM and the native stores";
        return false;
    }
    return arena.adopt(base, bytes, image + base, why);
}

bool adopt_aram_image(System& system, const u8* image, std::string& why) {
    GuestMemory& memory = system.guest_memory();
    const u32 sda = system.cpu().gpr[13];
    system.sound_banks().arena().forget();
    system.stream_ring().arena().forget();
    if (const u32 context = memory.read32(sda + kSdaGcaxContext); context != 0u) {
        if (!adopt_block(system.sound_banks().arena(), NativeSoundBanks::kSoundBase, memory.read32(context + kGcaxBase),
                         memory.read32(context + kGcaxRequested), image, "GCAX banks", why))
            return false;
    }
    if (!adopt_block(system.stream_ring().arena(), NativeStreamRing::kSoundBase,
                     memory.read32(kCriRingRecord + kCriRingBase), memory.read32(kCriRingRecord + kCriRingBytes), image,
                     "stream ring", why))
        return false;
    if (const u32 pool = memory.read32(sda + kSdaMotionPool); pool != 0u && pool < kAramImageBytes) {
        const u32 bytes = pool + kMotionPoolBytes <= kAramImageBytes ? kMotionPoolBytes : kAramImageBytes - pool;
        if (!system.motion_store().adopt_pool(pool, bytes, image + pool, why)) return false;
    }
    return true;
}

RendererStateHooks& renderer_hooks() {
    static RendererStateHooks hooks;
    return hooks;
}

// PAL GUPP8P Rev.00 screen-effect layout recovered from the retail DOL:
//   fn_800A35D8: singleton = *(r13 + 0x2DD8)
//   fn_8042F16C: blur      = *(singleton + 0x40)
//   fn_8042E254: draw only when blur[0x78] != 0
// Read host-side from the restored guest memory (a repair, not a guest access).
constexpr u32 kScreenEffectSingletonR13Offset = 0x2DD8u;
constexpr u32 kScreenBlurBackendOffset = 0x40u;
constexpr u32 kScreenBlurHistoryValidOffset = 0x78u;

bool read_guest_be32(const GuestMemory& memory, u32 address, u32& value) noexcept {
    const u8* const at = memory.peek(GcAddr(address), sizeof(u32));
    if (at == nullptr) return false;
    value = load_be32(at);
    return true;
}

template <typename T>
void put_vector(std::vector<u8>& out, const std::vector<T>& values) {
    state::put<u64>(out, static_cast<u64>(values.size()));
    if (!values.empty()) {
        state::put_bytes(out, values.data(), values.size() * sizeof(T));
    }
}

template <typename T>
bool get_vector(const u8*& cursor, const u8* end, std::vector<T>& values) {
    u64 count = 0;
    if (!state::get<u64>(cursor, end, count)) return false;
    // A truncated or foreign file must not be able to request a huge alloc.
    if (count > (1ull << 26)) return false;
    values.resize(static_cast<std::size_t>(count));
    if (count == 0) return true;
    return state::get_bytes(cursor, end, values.data(), values.size() * sizeof(T));
}

void fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
}

}  // namespace

bool invalidate_restored_shadow_render_history(System& system) noexcept {
    const u32 r13 = system.cpu().gpr[13];
    const u32 singleton_slot = r13 + kScreenEffectSingletonR13Offset;
    // Reject wrapped arithmetic as well as values outside the game's MEM1
    // cached alias before following either restored pointer.
    if (singleton_slot < r13) return false;

    GuestMemory& memory = system.guest_memory();
    u32 screen_effect = 0;
    if (!read_guest_be32(memory, singleton_slot, screen_effect) || screen_effect == 0u) {
        return false;
    }

    const u32 backend_slot = screen_effect + kScreenBlurBackendOffset;
    if (backend_slot < screen_effect) return false;
    u32 blur_backend = 0;
    if (!read_guest_be32(memory, backend_slot, blur_backend) || blur_backend == 0u) {
        return false;
    }

    const u32 history_valid = blur_backend + kScreenBlurHistoryValidOffset;
    if (history_valid < blur_backend) return false;
    u8* const valid = memory.span(GcAddr(history_valid), sizeof(u8));
    if (valid == nullptr) return false;
    *valid = 0u;
    return true;
}

// ---------------------------------------------------------------------------
// Whole-machine orchestration
// ---------------------------------------------------------------------------

u32 save_state_version() noexcept { return kSaveVersion; }

const char* renderer_state_kind_name(RendererStateKind kind) noexcept {
    switch (kind) {
        case RendererStateKind::None: return "no renderer";
        case RendererStateKind::Capture: return "legacy capture renderer";
        case RendererStateKind::Lifted: return "lifted R2 renderer";
    }
    return "unknown renderer";
}

void set_renderer_state_hooks(RendererStateHooks hooks) {
    renderer_hooks() = std::move(hooks);
}

RendererStateKind renderer_state_kind(System& system) noexcept {
#if !defined(SHADOW_RENDER_LIFTED)  // G5
    if (system.native_capture() != nullptr) return RendererStateKind::Capture;
#endif
    return renderer_hooks().kind;
}

void with_renderer_words_cleared(System& system, const std::function<void()>& body) {
    std::vector<u32> words;
    if (renderer_state_kind(system) == RendererStateKind::Lifted && renderer_hooks().own_words) {
        renderer_hooks().own_words(words);
    }
    std::vector<std::pair<u32, u32>> kept;
    kept.reserve(words.size());
    for (const u32 word : words) {
        u8* const at = system.guest_memory().span(GcAddr(word), sizeof(u32));
        if (at == nullptr) continue;
        kept.emplace_back(word, load_be32(at));
        std::memset(at, 0, sizeof(u32));
    }
    body();
    for (const auto& [word, value] : kept) {
        u8* const at = system.guest_memory().span(GcAddr(word), sizeof(u32));
        at[0] = static_cast<u8>(value >> 24);
        at[1] = static_cast<u8>(value >> 16);
        at[2] = static_cast<u8>(value >> 8);
        at[3] = static_cast<u8>(value);
    }
}

bool read_state_header(const std::filesystem::path& path, StateHeader* out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        fail(error, "savestate: cannot open " + path.string());
        return false;
    }
    u8 bytes[20] = {};
    file.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
    const std::streamsize got = file.gcount();
    u32 magic = 0, version = 0, kind = 0;
    u64 ticks = 0;
    if (got >= 16) {
        std::memcpy(&magic, bytes, 4);
        std::memcpy(&version, bytes + 4, 4);
        std::memcpy(&ticks, bytes + 8, 8);
    }
    if (got < 16 || magic != kSaveMagic) {
        fail(error, "savestate: not a GekkoForge savestate");
        return false;
    }
    if (version >= 19u) {
        if (got < 20) {
            fail(error, "savestate: truncated header");
            return false;
        }
        std::memcpy(&kind, bytes + 16, 4);
    }
    out->version = version;
    out->kind = static_cast<RendererStateKind>(kind);
    out->game_ticks = ticks;
    return true;
}

bool save_state(System& system, const std::filesystem::path& path, std::string* error) {
    // Refusals below are marked on the timeline: the zone starts after the
    // preconditions, so a refused save would otherwise produce no zone, no
    // plot and no mark at all. (An open immediate primitive needs no
    // precondition: its block is saved with the machine.)
    // The native renderer never writes guest memory, so no renderer work can
    // be in flight against the MEM1 bytes serialised below. The reference
    // executable's legacy GX raster can; its launcher establishes
    // Renderer::quiescent() before it calls this function.

    // Same precondition, one layer up: a gameplay traversal that has dispatched
    // a task and is waiting for it keeps part of the frame's walk in host
    // state. MEM1 alone cannot express "halfway through the Player layer", so a
    // file written here would restore into a frame that cannot be finished.
    // Every task call is a boundary, so the next service pass finds it idle.
    if (system.gameplay_scheduler() != nullptr && system.gameplay_scheduler()->busy()) {
        GEKKO_PROFILE_MESSAGE_LITERAL("mem/savestate refused: traversal busy");
        fail(error,
             "savestate refused: a gameplay traversal is in progress (a task has been "
             "dispatched and has not returned). Save between traversals.");
        return false;
    }

    // Everything below runs on the calling thread in one go, so the whole save
    // is one stall. The inner zones say which block spent the time, and their
    // values say which block is BIG - the two questions a growing file raises -
    // without editing the device files that own the blocks themselves.
    GEKKO_PROFILE_ZONE("mem/savestate save");
    if (GEKKO_PROFILE_CONNECTED()) {
        // One named local. Two path.string() temporaries in a single macro
        // expansion would hand Tracy the pointer of one and the length of
        // another, the first of which is already destroyed.
        const std::string target = path.string();
        GEKKO_PROFILE_TEXT(target.c_str(), target.size());
    }

    // The card's working image is the host file: commit what the persistence
    // debounce still holds, so the file matches the machine saved below.
    system.card().flush();

    std::vector<u8> blob;
    blob.reserve(static_cast<std::size_t>(system.guest_memory().committed_bytes()) + (48u << 20));

    const RendererStateKind kind = renderer_state_kind(system);
    state::put<u32>(blob, kSaveMagic);
    state::put<u32>(blob, kSaveVersion);
    state::put<u64>(blob, system.game_ticks());
    state::put<u32>(blob, static_cast<u32>(kind));

    {
        GEKKO_PROFILE_ZONE("mem/savestate cpu");
        [[maybe_unused]] const std::size_t before = blob.size();
        system.cpu().save_state(blob);
        GEKKO_PROFILE_VALUE(blob.size() - before);
    }

    {
        // The game's address window (every region and its committed bytes)
        // and the audio sample store. The renderer's own words are written as
        // 0 (RendererStateHooks::own_words) and put back right after.
        GEKKO_PROFILE_ZONE("mem/savestate ram");
        [[maybe_unused]] const std::size_t before = blob.size();
        with_renderer_words_cleared(system, [&] { system.guest_memory().save_state(blob); });
        GEKKO_PROFILE_VALUE(blob.size() - before);
    }

    {
        // The native interrupt sources, the native video, the native
        // controller input, the native audio platform, the GX sync, the
        // immediate geometry, the memory card and the IPL services (the SRAM
        // settings image). No device model is left (stage 6).
        GEKKO_PROFILE_ZONE("mem/savestate devices");
        [[maybe_unused]] const std::size_t before = blob.size();
        system.interrupts().save_state(blob);
        system.video().save_state(blob);
        system.peripheral().save(blob);
        system.audio().save_state(blob);
        system.sound_banks().arena().save_state(blob);
        system.stream_ring().arena().save_state(blob);
        system.gx_sync().save_state(blob);
#if !defined(SHADOW_RENDER_LIFTED)  // G5
        system.immediate().save_state(blob);
#else
        System::save_idle_immediate_state(blob);
#endif
        system.card().save_state(blob);
        system.ipl().save_state(blob);
        GEKKO_PROFILE_VALUE(blob.size() - before);
    }
    {
        // The renderer's block (version 19): the kind named in the header.
        GEKKO_PROFILE_ZONE("mem/savestate renderer");
        std::vector<u8> block;
#if !defined(SHADOW_RENDER_LIFTED)  // G5
        if (kind == RendererStateKind::Capture) {
            system.native_capture()->save_state(block);
        } else
#endif
        if (kind == RendererStateKind::Lifted && renderer_hooks().save) {
            renderer_hooks().save(block);
        }
        GEKKO_PROFILE_VALUE(block.size());
        state::put<u32>(blob, static_cast<u32>(block.size()));
        state::put_bytes(blob, block.data(), block.size());
    }

    {
        GEKKO_PROFILE_ZONE("mem/savestate audio history");
        std::vector<u8> block;
        system.save_host_audio_state(block);
        state::put<u32>(blob, static_cast<u32>(block.size()));
        state::put_bytes(blob, block.data(), block.size());
    }

    std::error_code ec;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    // Write to a sibling temporary first: a savestate half-written because the
    // process died mid-write must not replace a good one.
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        // 40+ MiB in one synchronous write on this thread: exactly the shape
        // the UART/SRAM misroute investigation proved a load-time stutter has.
        GEKKO_PROFILE_ZONE("mem/savestate write");
        GEKKO_PROFILE_VALUE(blob.size());
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            fail(error, "savestate refused: cannot open " + temporary.string());
            return false;
        }
        file.write(reinterpret_cast<const char*>(blob.data()),
                   static_cast<std::streamsize>(blob.size()));
        if (!file) {
            fail(error, "savestate refused: write failed for " + temporary.string());
            return false;
        }
    }
    {
        // On Windows a rename over a file another process has open or is
        // scanning blocks for milliseconds and can take the remove+rename
        // fallback below, neither of which anything records today.
        GEKKO_PROFILE_ZONE("mem/savestate rename");
        std::filesystem::rename(temporary, path, ec);
        if (ec) {
            std::filesystem::remove(path, ec);
            std::filesystem::rename(temporary, path, ec);
            if (ec) {
                fail(error, "savestate refused: cannot replace " + path.string());
                return false;
            }
        }
    }
    GEKKO_PROFILE_VALUE(blob.size());
    if (GEKKO_PROFILE_CONNECTED()) {
        static bool configured = false;
        if (!configured) {
            configured = true;
            GEKKO_PROFILE_PLOT_CONFIG("mem/savestate bytes", GEKKO_PLOT_MEMORY, true, true,
                                      0x4fa8ff);
            GEKKO_PROFILE_PLOT_CONFIG("mem/savestate blob capacity", GEKKO_PLOT_MEMORY, true, true,
                                      0xffc04f);
        }
        GEKKO_PROFILE_PLOT("mem/savestate bytes", static_cast<double>(blob.size()));
        // Read against the size above: the reserve is 24 MiB + 48 MiB, and a
        // future block that outgrows it makes the vector reallocate and copy
        // 72 MiB inside this zone with nothing else to attribute the rise to.
        GEKKO_PROFILE_PLOT("mem/savestate blob capacity", static_cast<double>(blob.capacity()));
    }
    GEKKO_PROFILE_MESSAGE_LITERAL("mem/savestate saved");
    return true;
}

bool load_state(System& system, const std::filesystem::path& path, std::string* error) {
    GEKKO_PROFILE_ZONE("mem/savestate load");
    if (GEKKO_PROFILE_CONNECTED()) {
        const std::string source = path.string();
        GEKKO_PROFILE_TEXT(source.c_str(), source.size());
    }
    // blob is declared outside the scope below because it owns the bytes every
    // block is parsed from; only the file handle and the read belong to the
    // zone. resize() on an empty vector value-initialises exactly as the
    // previous `std::vector<u8> blob(size)` did.
    std::vector<u8> blob;
    {
        // Whole-file read plus a 40+ MiB allocation before a single byte is
        // parsed: the first candidate for the hitch a hotkey load produces.
        GEKKO_PROFILE_ZONE("mem/savestate read");
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            fail(error, "savestate: cannot open " + path.string());
            return false;
        }
        const std::streamsize size = file.tellg();
        if (size <= 0) {
            fail(error, "savestate: empty file " + path.string());
            return false;
        }
        file.seekg(0);
        blob.resize(static_cast<std::size_t>(size));
        file.read(reinterpret_cast<char*>(blob.data()), size);
        if (!file) {
            fail(error, "savestate: short read on " + path.string());
            return false;
        }
        GEKKO_PROFILE_VALUE(size);
    }
    if (GEKKO_PROFILE_CONNECTED()) {
        static bool configured = false;
        if (!configured) {
            configured = true;
            GEKKO_PROFILE_PLOT_CONFIG("mem/savestate bytes", GEKKO_PLOT_MEMORY, true, true,
                                      0x4fa8ff);
        }
        GEKKO_PROFILE_PLOT("mem/savestate bytes", static_cast<double>(blob.size()));
    }

    const u8* cursor = blob.data();
    const u8* const end = blob.data() + blob.size();

    u32 magic = 0;
    u32 version = 0;
    u64 game_ticks = 0;
    u32 file_kind = 0;
    if (!state::get<u32>(cursor, end, magic) || magic != kSaveMagic) {
        GEKKO_PROFILE_MESSAGE_LITERAL("mem/savestate refused: load");
        fail(error, "savestate: not a GekkoForge savestate");
        return false;
    }
    if (!state::get<u32>(cursor, end, version) || (version < 19u || version > kSaveVersion)) {
        GEKKO_PROFILE_MESSAGE_LITERAL("mem/savestate refused: load");
        fail(error, "savestate: version mismatch (file " + std::to_string(version) +
                        ", runtime " + std::to_string(kSaveVersion) +
                        "). Files before version 10 measured time in retired instructions, "
                        "version 10 still had the emulated video interface, version 11 the "
                        "emulated serial interface, version 12 the emulated command processor "
                        "and pixel engine, version 13 the GX command stream decoder, version 14 "
                        "the emulated external interface, version 17 the PAD library's state "
                        "(controller input is now the lifted peripheral layer); re-save from a "
                        "current build.");
        return false;
    }
    if (!state::get<u64>(cursor, end, game_ticks) || !state::get<u32>(cursor, end, file_kind)) {
        GEKKO_PROFILE_MESSAGE_LITERAL("mem/savestate refused: load");
        fail(error, "savestate: truncated header");
        return false;
    }
    const RendererStateKind kind = renderer_state_kind(system);
    if (file_kind != static_cast<u32>(kind)) {
        GEKKO_PROFILE_MESSAGE_LITERAL("mem/savestate refused: load");
        fail(error, std::string("savestate: written by the ") +
                        renderer_state_kind_name(static_cast<RendererStateKind>(file_kind)) +
                        " build; this executable is the " + renderer_state_kind_name(kind) +
                        " build. Each build loads only its own savestates (their RenderWare objects hold "
                        "different renderer words); bring this build to the same point with its own "
                        "savestate (tools/comparator).");
        return false;
    }

    // Everything from here mutates the machine. A failure past this point
    // leaves it unusable, which is why the header is validated first and every
    // block is length-checked.
    const auto bad = [&](const char* which) {
        GEKKO_PROFILE_MESSAGE_LITERAL("mem/savestate refused: load");
        fail(error, std::string("savestate: truncated or mismatched ") + which +
                        " block; the machine is now inconsistent and the process should exit");
        return false;
    };
    const u8* aram_image = nullptr;  // versions 19-23: ARAM's image, for adopt_aram_image

    {
        // Cpu::restore_state also invalidates the host page table, so this
        // zone covers a whole-table translation drop as well as a parse.
        GEKKO_PROFILE_ZONE("mem/savestate restore cpu");
        if (!system.cpu().load_state(cursor, end, version)) return bad("CPU");
    }

    {
        // Regions that exist are restored in place (the CPU reads the page
        // table directly and the runtime keeps no other pointer into them);
        // regions the file names that do not exist are mapped, the others
        // unmapped.
        GEKKO_PROFILE_ZONE("mem/savestate restore ram");
        std::string why;
        if (!system.guest_memory().load_state(cursor, end, why)) return bad(("guest memory: " + why).c_str());
        if (version < kFirstVersionWithoutAram) {
            u64 count = 0;
            if (!state::get<u64>(cursor, end, count) || count != kAramImageBytes) return bad("ARAM image");
            if (static_cast<std::size_t>(end - cursor) < kAramImageBytes) return bad("ARAM image");
            aram_image = cursor;
            cursor += kAramImageBytes;
        }
    }

    {
        // The consumed byte count is the number that makes a block-size
        // defect visible instead of silently attributing it to GX.
        GEKKO_PROFILE_ZONE("mem/savestate restore devices");
        [[maybe_unused]] const u8* const before = cursor;
        if (!system.interrupts().load_state(cursor, end)) return bad("native interrupts");
        if (!system.video().load_state(cursor, end)) return bad("native video");
        if (!system.peripheral().load(cursor, end)) return bad("peripheral (lifted)");
        if (!system.audio().load_state(cursor, end)) return bad("native audio");
        std::string why;
        if (aram_image != nullptr) {
            if (!adopt_aram_image(system, aram_image, why)) return bad(("ARAM image: " + why).c_str());
        } else if (!system.sound_banks().arena().load_state(cursor, end, why) ||
                   !system.stream_ring().arena().load_state(cursor, end, why)) {
            return bad(("sound stores: " + why).c_str());
        }
        if (!system.gx_sync().load_state(cursor, end)) return bad("native GX sync");
#if !defined(SHADOW_RENDER_LIFTED)  // G5
        if (!system.immediate().load_state(cursor, end)) return bad("native immediate geometry");
#else
        if (!System::load_idle_immediate_state(cursor, end)) return bad("native immediate geometry (an open primitive)");
#endif
        if (!system.card().load_state(cursor, end)) return bad("native card");
        if (!system.ipl().load_state(cursor, end)) return bad("native IPL services");
        if (game_ticks != system.game_ticks()) return bad("game clock header");
        GEKKO_PROFILE_VALUE(cursor - before);
    }

    // Texture and palette caches are content-addressed and the EFB is
    // regenerated by the restored command stream.  Temporal EFB-copy history
    // is different: its guest validity byte was serialised, but its matching
    // native texture was not.  Invalidate the byte so the first restored frame
    // captures fresh history at the current internal resolution instead of
    // sampling the 320x240 MEM1 compatibility image once.
    {
        GEKKO_PROFILE_ZONE("mem/savestate history invalidate");
        invalidate_restored_shadow_render_history(system);
    }

    // D13-3. The reference executable's legacy GX raster keeps host state
    // keyed by guest addresses from the discarded timeline; its launcher calls
    // Renderer::on_state_loaded() after this function succeeds. The native
    // renderer's own state is restored or invalidated below.

    // Native renderer: replay the saved GX state, or invalidate when the file
    // carries none. A block saved by a native session and loaded by a legacy
    // one is skipped by its length.
    {
        // The renderer's block, of the kind the header named (checked above).
        GEKKO_PROFILE_ZONE("mem/savestate restore renderer");
        u32 length = 0;
        if (!state::get<u32>(cursor, end, length) || static_cast<std::size_t>(end - cursor) < length) {
            return bad("renderer");
        }
        const u8* block = cursor;
        cursor += length;
        const u8* block_cursor = block;
#if !defined(SHADOW_RENDER_LIFTED)  // G5
        if (kind == RendererStateKind::Capture) {
            RwCapture* capture = system.native_capture();
            if (length != 0u) {
                if (!capture->load_state(block_cursor, block + length)) return bad("native capture");
            } else {
                capture->on_state_loaded();
            }
        } else
#else
        if (kind == RendererStateKind::Capture) return bad("legacy capture renderer state in the lifted build");
#endif
        if (kind == RendererStateKind::Lifted) {
            const RendererStateHooks& hooks = renderer_hooks();
            if (hooks.load && !hooks.load(block_cursor, block + length)) return bad("lifted renderer");
            if (block_cursor != block + length) return bad("lifted renderer (length)");
            if (hooks.loaded) hooks.loaded();
        }
        GEKKO_PROFILE_VALUE(length);
    }

    // The caller has drained pending work. Decoder/SRC/compressor history
    // belongs to the saved timeline just as much as source positions do.
    {
        GEKKO_PROFILE_ZONE("mem/savestate restore audio history");
        if (version >= 20u) {
            u32 length = 0;
            if (!state::get(cursor, end, length) || static_cast<std::size_t>(end - cursor) < length)
                return bad("audio history");
            const u8* block_end = cursor + length;
            if (!system.load_host_audio_state(cursor, block_end, version) || cursor != block_end)
                return bad("audio history");
            if (version == 20u) std::fprintf(stderr, "[savestate] version 20 has no surround history; "
                "the first stereo output block after restore may differ.\n");
            if (version < 22u) std::fprintf(stderr, "[savestate] versions 20/21 have only one matrix block; "
                "switching output modes after restore may differ until both blocks are refreshed.\n");
            if (version < 23u) std::fprintf(stderr, "[savestate] versions 20-22 have no room effect history; "
                "the reverb tail restarts empty after restore.\n");
        } else {
            system.reset_host_audio_state();
            std::fprintf(stderr, "[savestate] version 19 has no decoder/mixer history; "
                                 "audio continuation may differ until history is rebuilt.\n");
        }
        if (cursor != end) return bad("trailing data");
    }
    {
        GEKKO_PROFILE_ZONE("mem/savestate host reset");
        // Any gameplay traversal still on this object belongs to the machine that
        // was just replaced. The restored guest resumes wherever its own pc says,
        // and a save was only allowed between traversals, so there is nothing to
        // carry over.
        if (system.gameplay_scheduler() != nullptr) {
            system.gameplay_scheduler()->discard_traversals();
        }
    }
    GEKKO_PROFILE_VALUE(blob.size());
    // A load is a discontinuity for every plot and counter slope in every
    // subsystem: this mark is where the in-stage benchmark route begins.
    GEKKO_PROFILE_MESSAGE_LITERAL("mem/savestate loaded");
    return true;
}

}  // namespace gekko
