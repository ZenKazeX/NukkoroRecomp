#pragma once

// Forensic resource-lineage tracing: FILE -> READ -> GUEST MEMORY -> PARSER ->
// GAME STRUCTURE -> RESOURCE -> HOST BACKEND.
//
// Why this exists. Function-entry counts, dispatch-table sizes and "native
// execution" labels prove that translated code EXISTS. They do not prove that
// the original program is the thing making the decisions. This tracer answers
// the only question that does: when the game asks for a real asset, whose code
// picked the name, whose code read the bytes, whose code parsed them, and where
// did they live while that happened.
//
// Every event records who decided it - `guest` when the value came out of
// recompiled guest code (a guest register, guest memory, a guest call site) and
// `host` when the runtime supplied it. Host-supplied fields are reporting
// conveniences and are labelled as such: the offset->filename resolution in
// particular is a host reverse lookup over the FST for the benefit of the
// reader, and nothing in the emulated path ever consults it.
//
// Cost when disabled: one null-pointer test on the disc read path. The tracer
// is off unless --asset-trace names an output file.

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "gekko/types.h"

namespace gekko {

class Cpu;

// One field of a traced event, carrying its own provenance. The whole point of
// the audit is that "who produced this value" is not guessable from the value,
// so it is never left implicit.
enum class Provenance : u8 {
    Guest,  // read out of guest registers, guest memory, or a guest call site
    Host,   // produced by the runtime (host reporting aid, or a hardware model)
};

class AssetTracer {
public:
    // Opens path for writing. Returns nullptr if it cannot be created.
    static std::unique_ptr<AssetTracer> create(const std::string& path,
                                               const std::string& filter);
    ~AssetTracer();

    // The single global instance, or nullptr when tracing is off. The native
    // file API tests this on its read path, so it must stay a plain pointer
    // load.
#if defined(GEKKO_SHIPPING)
    // GEKKO_SHIPPING (ES): no lineage trace in the user's exe; the read path's test folds away.
    static constexpr AssetTracer* active() noexcept { return nullptr; }
#else
    static AssetTracer* active() noexcept { return active_.load(std::memory_order_relaxed); }
#endif
    static void set_active(AssetTracer* tracer) noexcept {
        active_.store(tracer, std::memory_order_relaxed);
    }

    // --- stage 1: a read the native file API served -------------------------
    // Called by NativeFileApi::read after the bytes landed in guest memory.
    // `token` is the guest token the API decoded (DVDFileInfo.startAddr +
    // offset) and `destination` the guest address the caller named;
    // file/file_offset are what the token resolved to. issue_pc/issue_lr/
    // issue_chain name the guest code that asked for the bytes: the read
    // completes inside the call, so they are sampled at the call itself.
    // Record "file_read" (archived traces from the DI device model spell it
    // "di_read" with "disc_offset"; the readers accept both).
    void file_read(u32 issue_pc, u32 issue_lr, const std::string& issue_chain, u64 token,
                   u32 length, u32 destination, const std::string& resolved_file, u64 file_offset,
                   const u8* bytes, bool ok);

    // --- stage 2: a guest function on the asset path ------------------------
    // Installed as observe-only HLE hooks (the handler returns false, so the
    // recompiled body still runs). `args` are guest register values; `strings`
    // are guest memory read through the normal translation path.
    void guest_fn(const Cpu& cpu, const char* role, u32 address);

    // --- stage 3: bytes landing in guest-visible memory ---------------------
    void guest_memory(const Cpu& cpu, const char* role, u32 guest_address, u32 length);

    // Free-form note tying stages together (host-owned, always labelled host).
    void note(const char* role, const std::string& text);

    u64 events() const noexcept { return events_; }
    bool wants(const std::string& file) const;
    // The issuer's guest call path, for a record made on its behalf.
    static std::string call_chain_of(Cpu* cpu) { return call_chain(cpu); }

private:
    AssetTracer(std::FILE* out, std::string filter);
    void write_line(const std::string& line);
    // Guest call chain, deepest last. Empty when the CPU has no history.
    // The guest call stack at this instant: LR, then each caller's saved LR
    // up the EABI back chain (at most 8), read with side-effect-free peeks.
    static std::string call_chain(Cpu* cpu);

    std::FILE* out_ = nullptr;
    std::string filter_;
    std::mutex mutex_;
    u64 seq_ = 0;
    u64 events_ = 0;
    static std::atomic<AssetTracer*> active_;
};

}  // namespace gekko
