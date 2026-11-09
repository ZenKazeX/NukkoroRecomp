// CRI's ADX output to the AX voices as native C++20. Contract and evidence: include/gekko/native_adx_output.h,
// docs/CRI_SERVERS.md section 4c. Each step follows its original's order of guest reads and writes.

#include "gekko/native_adx_output.h"

#include <cstdio>
#include <string>

#include "gekko/cpu.h"
#include "gekko/native_sj.h"

namespace gekko {

namespace {

constexpr u32 kExecAll = 0x803CEEB4u, kExec = 0x803CEF10u, kSilenceDone = 0x803CF260u, kPlayDone = 0x803CF32Cu,
              kConsume = 0x803CF408u;

constexpr u32 kHandles = 0x805C3E4Cu, kHandleSize = 0xECu, kHandleCount = 16u;
constexpr u32 kZeroBuffer = 0x805C2D7Cu;                          // the word holding the silence source
constexpr u32 kHistoryIndex = 0x805C2D88u, kHistory = 0x805C2D8Cu;  // 32 play positions
constexpr u32 kChunkBytes = 0x2000u;
constexpr u32 kCursorWrap = 0x1000u;

// handle fields
constexpr u32 kFlags = 0x01u, kChannels = 0x03u, kCursor = 0x04u, kVoice = 0x08u, kRingBase = 0x18u, kOwner = 0x28u,
              kInput = 0x30u, kOutput = 0x38u, kSourceChunk = 0x40u, kRingChunk = 0x50u, kBusy = 0x60u,
              kHalfBytes = 0x68u, kPlayed = 0x6Cu, kSilenceBusy = 0x70u, kSilenceHalf = 0x78u, kSilenceDoneTotal = 0x7Cu,
              kRingLength = 0x20u, kRequests = 0xA8u, kPosition = 0xE8u;
// ARQPostRequest's record
constexpr u32 kRequestNext = 0x0u, kRequestOwner = 0x4u, kRequestType = 0x8u, kRequestSource = 0x10u,
              kRequestDestination = 0x14u, kRequestLength = 0x18u, kRequestCallback = 0x1Cu;

const NativeAdxOutput::Address kAddresses[] = {
    {kExecAll, "ADX output: every handle", false},
    {kExec, "ADX output: one handle", false},
    {kSilenceDone, "ADX output: silence transfer completion", false},
    {kPlayDone, "ADX output: transfer completion", false},
    {kConsume, "ADX output: consume what the voice played", false},
};

s32 sw(u32 v) { return static_cast<s32>(v); }
s32 smin(s32 a, s32 b) { return b < a ? b : a; }
s32 channels(Cpu& c, u32 handle) { return static_cast<s8>(c.mem_read8(handle + kChannels)); }
// srawi 5 / addze / slwi 5: rounded toward zero to 32 bytes
u32 down32(s32 v) { return static_cast<u32>((v / 32) * 32); }

}  // namespace

const NativeAdxOutput::Address* NativeAdxOutput::addresses(std::size_t& count) noexcept {
    count = std::size(kAddresses);
    return kAddresses;
}

bool NativeAdxOutput::fail(Cpu& c, const char* what, u32 handle) {
    char text[64];
    std::snprintf(text, sizeof(text), " (handle 0x%08X)", handle);
    c.request_stop(StopReason::ExecutionFault, std::string("native ADX output: ") + what + text);
    return false;
}

// 0x803CF408
void NativeAdxOutput::consume(Cpu& c, u32 handle) {
    ++stats_.consumes;
    const s32 count = channels(c, handle);
    u32 cursor = c.mem_read32(handle + kCursor);
    const u32 last = handle + 4u * static_cast<u32>(count);  // the last channel's words (+0x08 / +0x18 arrays)
    const u32 voice = c.mem_read32(last + kVoice - 4u);
    if (voice == 0u) return;
    const u32 base = c.mem_read32(last + kRingBase - 4u);
    // lwz at +0x1B2 of the parameter block: a halfword-aligned word
    const u32 address = voice + 0x1B2u;
    const u32 current = (static_cast<u32>(c.mem_read16(address)) << 16) | c.mem_read16(address + 2u);
    const s32 position = sw(current - base);
    c.mem_write32(handle + kPosition, static_cast<u32>(position));
    const u32 index = c.mem_read32(kHistoryIndex);
    c.mem_write32(kHistoryIndex, index + 1u);
    c.mem_write32(kHistory + (index << 2), static_cast<u32>(position));
    if (index + 1u == 32u) c.mem_write32(kHistoryIndex, 0u);
    if (position < 0 || position > sw(c.mem_read32(handle + kRingLength))) {
        fail(c, "the voice plays outside its ring (the original hangs at 0x803CF494)", handle);  // liveness-roots: value-only
        return;
    }
    s32 played = 0;
    if (sw(cursor) == -1) {
        if (position == 0) return;  // nothing played yet
        c.mem_write32(handle + kCursor, 0u);
        cursor = 0u;
    }
    played = position > sw(cursor) ? position - sw(cursor) : static_cast<s32>(kCursorWrap) - (sw(cursor) - position);
    const s32 samples = (played / 2048) * 2048;
    if (samples <= 0) return;
    for (s32 ch = 0; ch < count; ++ch) {
        const u32 sj = c.mem_read32(handle + kOutput + 4u * static_cast<u32>(ch));
        NativeSj::Chunk chunk;
        if (!sj_.get_chunk(c, sj, 1u, static_cast<u32>(samples) << 1, chunk) || !sj_.put_chunk(c, sj, 0u, chunk)) {
            fail(c, "an output joint the native SJ cannot serve", handle);
            return;
        }
    }
    stats_.consumed_samples += static_cast<u32>(samples);
    u32 next = c.mem_read32(handle + kCursor) + static_cast<u32>(samples);
    c.mem_write32(handle + kCursor, next);
    next = c.mem_read32(handle + kCursor);
    if (sw(next) >= static_cast<s32>(kCursorWrap)) c.mem_write32(handle + kCursor, next - kCursorWrap);
}

// 0x803CF32C (play) / 0x803CF260 (silence): the handle and channel from the record's owner word (index * 2 + channel)
void NativeAdxOutput::complete(Cpu& c, u32 request, bool silence) {
    const u32 owner = c.mem_read32(request + kRequestOwner);
    const u32 channel = owner & 1u;
    const u32 handle = kHandles + ((owner & 0x7FFFFFFFu) >> 1) * kHandleSize;
    const u32 busy = handle + (silence ? kSilenceBusy : kBusy) + 4u * channel;
    if (c.mem_read32(busy) != 1u) return;
    NativeSj::Chunk chunk;
    if (!silence) {
        const u32 at = handle + kSourceChunk + 8u * channel;
        chunk = {c.mem_read32(at), c.mem_read32(at + 4u)};
        if (!sj_.put_chunk(c, c.mem_read32(handle + kInput + 4u * channel), 0u, chunk)) {
            fail(c, "an input joint the native SJ cannot serve", handle);
            return;
        }
    }
    const u32 at = handle + kRingChunk + 8u * channel;
    chunk = {c.mem_read32(at), c.mem_read32(at + 4u)};
    if (!sj_.put_chunk(c, c.mem_read32(handle + kOutput + 4u * channel), 1u, chunk)) {
        fail(c, "an output joint the native SJ cannot serve", handle);
        return;
    }
    c.mem_write32(busy, 0u);
    if (sw(channel) == channels(c, handle) - 1) {
        const u32 total = silence ? kSilenceDoneTotal : kPlayed, half = silence ? kSilenceHalf : kHalfBytes;
        c.mem_write32(handle + total, c.mem_read32(handle + total) + c.mem_read32(handle + half));
    }
}

// 0x803CEF10
void NativeAdxOutput::exec(Cpu& c, u32 handle) {
    if (handle == 0u) return;
    ++stats_.handles;
    if ((c.mem_read8(handle + kFlags) >> 1) & 1u) consume(c, handle);
    if (c.stopped()) return;
    const u32 flags = c.mem_read8(handle + kFlags);
    // ARQPostRequest's record, the copy, then the completion the original waited for
    const auto transfer = [&](u32 channel, u32 source, u32 destination, u32 bytes, bool silence) {
        const u32 request = handle + kRequests + 0x20u * channel;
        c.mem_write32(request + kRequestNext, 0u);
        c.mem_write32(request + kRequestOwner, c.mem_read32(handle + kOwner + 4u * channel));
        c.mem_write32(request + kRequestType, 0u);
        c.mem_write32(request + kRequestSource, source);
        c.mem_write32(request + kRequestDestination, destination);
        c.mem_write32(request + kRequestLength, bytes);
        c.mem_write32(request + kRequestCallback, silence ? kSilenceDone : kPlayDone);
        std::string why;
        if (!copy_(c, source, destination, bytes, why)) return fail(c, why.c_str(), handle);
        complete(c, request, silence);
        return !c.stopped();
    };
    if (flags & 1u) {
        for (s32 ch = 0; ch < channels(c, handle); ++ch) {
            const u32 k = static_cast<u32>(ch);
            if (c.mem_read32(handle + kVoice + 4u * k) == 0u || c.mem_read32(handle + kBusy + 4u * k) != 0u) continue;
            const u32 out = c.mem_read32(handle + kOutput + 4u * k), in = c.mem_read32(handle + kInput + 4u * k);
            NativeSj::Chunk ring, rest_ring, data, rest_data;
            if (!sj_.get_chunk(c, out, 0u, kChunkBytes, ring) || !sj_.get_chunk(c, in, 1u, ring.length, data)) {
                fail(c, "a joint the native SJ cannot serve", handle);
                return;
            }
            const u32 bytes = down32(smin(sw(data.length), sw(ring.length)));
            sj_.split_chunk(ring, bytes, ring, rest_ring);
            sj_.split_chunk(data, bytes, data, rest_data);
            if (!sj_.unget_chunk(c, out, 0u, rest_ring) || !sj_.unget_chunk(c, in, 1u, rest_data)) {
                fail(c, "a joint the native SJ cannot serve", handle);
                return;
            }
            if (bytes == 0u) return;  // nothing to move ends the whole call, as in the original
            if (data.length != ring.length) {
                fail(c, "unequal chunk lengths (the original hangs at 0x803CF070)", handle);  // liveness-roots: value-only
                return;
            }
            c.mem_write32(handle + kSourceChunk + 8u * k, data.data);
            c.mem_write32(handle + kSourceChunk + 8u * k + 4u, data.length);
            c.mem_write32(handle + kRingChunk + 8u * k, ring.data);
            c.mem_write32(handle + kRingChunk + 8u * k + 4u, ring.length);
            c.mem_write32(handle + kHalfBytes, bytes >> 1);
            c.mem_write32(handle + kBusy + 4u * k, 1u);
            ++stats_.transfers;
            stats_.transfer_bytes += bytes;
            if (!transfer(k, data.data, ring.data, bytes, false)) return;
        }
        return;
    }
    if (!((flags >> 1) & 1u) || sw(c.mem_read32(handle + kSilenceDoneTotal)) >= sw(c.mem_read32(handle + kRingLength)))
        return;
    for (s32 ch = 0; ch < channels(c, handle); ++ch) {
        const u32 k = static_cast<u32>(ch);
        if (c.mem_read32(handle + kSilenceBusy + 4u * k) != 0u) continue;
        const u32 out = c.mem_read32(handle + kOutput + 4u * k);
        NativeSj::Chunk ring, rest;
        if (!sj_.get_chunk(c, out, 0u, kChunkBytes, ring)) {
            fail(c, "an output joint the native SJ cannot serve", handle);
            return;
        }
        const u32 bytes = down32(sw(ring.length));
        sj_.split_chunk(ring, bytes, ring, rest);
        if (!sj_.unget_chunk(c, out, 0u, rest)) {
            fail(c, "an output joint the native SJ cannot serve", handle);
            return;
        }
        if (bytes == 0u) return;
        c.mem_write32(handle + kRingChunk + 8u * k, ring.data);
        c.mem_write32(handle + kRingChunk + 8u * k + 4u, ring.length);
        c.mem_write32(handle + kSilenceHalf, bytes >> 1);
        c.mem_write32(handle + kSilenceBusy + 4u * k, 1u);
        ++stats_.silence_transfers;
        stats_.silence_bytes += bytes;
        if (!transfer(k, c.mem_read32(kZeroBuffer), ring.data, bytes, true)) return;
    }
}

void NativeAdxOutput::install() {
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(
            address,
            [serve](Cpu& c) {
                serve(c);
                return true;
            },
            std::string("native ADX output: ") + name);
    };
    own(kExecAll, "every handle", [this](Cpu& c) {
        ++stats_.passes;
        for (u32 i = 0; i < kHandleCount && !c.stopped(); ++i) {
            const u32 handle = kHandles + i * kHandleSize;
            if (c.mem_read8(handle) == 1u) exec(c, handle);
        }
    });
    own(kExec, "one handle", [this](Cpu& c) { exec(c, c.gpr[3]); });
    own(kConsume, "consume", [this](Cpu& c) { consume(c, c.gpr[3]); });
    own(kPlayDone, "transfer completion", [this](Cpu& c) { complete(c, c.gpr[3], false); });
    own(kSilenceDone, "silence transfer completion", [this](Cpu& c) { complete(c, c.gpr[3], true); });
}

}  // namespace gekko
