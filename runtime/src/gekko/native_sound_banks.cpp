// The GCAX sound banks without ARAM. Contract and the original code's evidence: include/gekko/native_sound_banks.h,
// docs/ARAM.md and docs/GCAX_SOUND_MAP.md.

#include "gekko/native_sound_banks.h"

#include <atomic>
#include <string>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"

namespace gekko {

namespace {

// ---- the entry point the generator routes here (extract/native-owners.txt)
constexpr u32 kUpload = 0x8043BACCu;  // gcaxAramDma (r3 source, r4 sound address, r5 bytes, r6 callback, r7 argument)

const NativeSoundBanks::Address kAddresses[] = {
    {kUpload, "sound banks: bank upload (0x8043BACC)"},
};

constexpr u32 kSdaContext = 0x5FACu;
// The context's request bookkeeping (docs/GCAX_SOUND_MAP.md section 2).
constexpr u32 kCtxBusy = 0x505Cu;      // 64 request busy bytes
constexpr u32 kCtxCallback = 0x50A0u;  // 64 words
constexpr u32 kCtxArgument = 0x51A0u;  // 64 words
constexpr u32 kRequests = 64u;

std::atomic<NativeSoundBanks*> g_installed{nullptr};

NativeSoundBanks* installed(Cpu& c, const char* entry) {
    NativeSoundBanks* banks = g_installed.load(std::memory_order_acquire);
    if (banks == nullptr)
        c.request_stop(StopReason::ExecutionFault,
                       std::string("lowered sound-bank site (scaffold::") + entry + ") ran without the banks installed");
    return banks;
}

}  // namespace

const NativeSoundBanks::Address* NativeSoundBanks::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

void NativeSoundBanks::install() {
    g_installed.store(this, std::memory_order_release);
    cpu_.install_hle(kUpload, [this](Cpu& c) {
        upload(c);
        return true;  // an owner never declines
    }, kAddresses[0].name);
}

void NativeSoundBanks::stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "sound banks: " + why);
}

// The init's ARAlloc (0x8043F7A4, r3 the bytes requested): the arena reserved to that request, a power of two so a
// mask maps the sound address space onto it; nothing committed yet. Returns the base the game keeps (context
// +0x52B0) and builds every slot address from.
u32 NativeSoundBanks::reserve(Cpu& c, u32 bytes) {
    std::string why;
    if (!arena_.reserve(bytes, why)) {
        stop(c, why + " (the original takes one ARAM block for the program's life)");
        return 0u;
    }
    stats_.reserved = arena_.reserved();
    ++stats_.reserves;
    return kSoundBase;
}

// 0x8043F730 in the GCAX shutdown 0x8043F674 (from the AudioGCN destructor; never reached in play): ARFree(&size) -
// the banks' arena released, the block's base back and its size written where r3 points, as ARFree does.
u32 NativeSoundBanks::release(Cpu& c, u32 size_out) {
    const u32 size = arena_.requested();
    const u32 base = arena_.sound_base();
    std::string why;
    if (!arena_.release(why)) {
        stop(c, why);
        return 0u;
    }
    if (size_out != 0u)
        c.mem_write32(size_out, size);
    ++stats_.releases;
    return base;
}

// A copy into the banks as one ARQ request of the original: the first free request recorded with its callback and
// argument (as 0x8043BACC / 0x8043BBBC pick and fill it), the bytes copied, the request finished as its completion
// 0x8043BC9C finishes it (busy byte clear). A callback is never set: gcaxLoadMPB clears it for the game's only load
// (callback -1) and the zero block passes none; a set one would be a path this code has not been shown.
u32 NativeSoundBanks::copy_in(Cpu& c, u32 from, u32 to, u32 bytes, u32 callback, u32 argument, bool counted) {
    const u32 ctx = c.mem_read32(c.gpr[13] + kSdaContext);
    u32 request = 0;
    while (request < kRequests && c.mem_read8(ctx + kCtxBusy + request) != 0u)
        ++request;
    if (request == kRequests) {
        stop(c, "no free request (the original reports a panic and hangs)");
        return 0u;
    }
    if (callback != 0u) {
        stop(c, "a bank copy with a completion callback (not a path of the shipped game)");
        return 0u;
    }
    c.mem_write32(ctx + kCtxCallback + 4u * request, callback);
    c.mem_write32(ctx + kCtxArgument + 4u * request, argument);
    std::string why;
    if (!arena_.copy_in(to, from, bytes, why)) {
        stop(c, why);
        return 0u;
    }
    stats_.committed = arena_.committed();
    if (counted) {
        ++stats_.uploads;
        stats_.upload_bytes += bytes;
    }
    return request;
}

// 0x8043BACC (r3 source, r4 sound address, r5 bytes, r6 callback, r7 argument, r8 asynchronous): returns the
// request's index, as the original does once its wait ends.
void NativeSoundBanks::upload(Cpu& c) {
    const u32 request = copy_in(c, c.gpr[3], c.gpr[4], c.gpr[5], c.gpr[6], c.gpr[7], true);
    if (c.exiting())
        return;
    c.gpr[3] = request;
    take_interrupts(c);
}

// As in the motion store: the original's wait loop is where a due interrupt is taken.
void NativeSoundBanks::take_interrupts(Cpu& c) {
    if (c.needs_dispatch()) {
        ++stats_.interrupt_points;
        c.leave_to_dispatcher(c.lr);
    }
}

// ---- the lowered call sites (extract/lift-sites.txt; declared by the generator in recomp_table.cpp) -------------

namespace scaffold {

// 0x8043F7A4 in the GCAX init: ARAlloc(bytes).
u32 sound_banks_reserve(Cpu& c, u32 bytes) {
    NativeSoundBanks* banks = installed(c, "sound_banks_reserve");
    return banks != nullptr ? banks->reserve(c, bytes) : 0u;
}

// 0x8043F818 in the GCAX init: the zero block's upload (r3 source, r4 sound address, r5 bytes, r6 callback,
// r7 argument), which the original waits for.
u32 sound_banks_zero_block(Cpu& c, u32 from, u32 to, u32 bytes, u32 callback, u32 argument) {
    NativeSoundBanks* banks = installed(c, "sound_banks_zero_block");
    if (banks != nullptr)
        banks->copy_in(c, from, to, bytes, callback, argument, false);
    return 0u;
}

// 0x8043F730 in the GCAX shutdown: ARFree(&size).
u32 sound_banks_release(Cpu& c, u32 size_out) {
    NativeSoundBanks* banks = installed(c, "sound_banks_release");
    return banks != nullptr ? banks->release(c, size_out) : 0u;
}

}  // namespace scaffold

}  // namespace gekko
