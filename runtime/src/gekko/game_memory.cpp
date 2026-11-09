// The game's memory services, native. Contract: include/gekko/game_memory.h
// and docs/memory-architecture-20261006/README.md.

#include "gekko/game_memory.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/ppc_ops.h"

namespace gekko {

namespace {

// ---- the entry points the generator routes here (extract/native-owners.txt)

// The C runtime's "memcpy" (its symbol), a byte-wise memmove: forward when the
// source is not below the destination, backward from the end otherwise.
constexpr u32 kMemmove = 0x800054F4u;
constexpr u32 kMemmoveForwardHead = 0x8000550Cu;   // owner entries: where a hand-back resumes
constexpr u32 kMemmoveBackwardHead = 0x80005530u;
constexpr u32 kMemmoveForwardEdge = 0x80005518u;   // the back edges (reported as the hand-back site)
constexpr u32 kMemmoveBackwardEdge = 0x8000553Cu;

// SEGA PRS expansion: (src, dst) -> bytes written. Called by the ONE archive
// member load (0x8004C770) and the asynchronous expansion job (0x80043D5C).
constexpr u32 kPrsExpand = 0x80043FE0u;
constexpr u32 kPrsTokenHead = 0x80043FF0u;          // owner entries: where a hand-back resumes
constexpr u32 kPrsShortLengthHead = 0x800440ACu;
constexpr u32 kPrsCopyHead = 0x800440F8u;
constexpr u32 kPrsLiteralEdge = 0x80044028u;        // the back edges (reported as the hand-back site)
constexpr u32 kPrsShortLengthEdge = 0x800440D0u;
constexpr u32 kPrsEmptyCopyEdge = 0x800440F4u;
constexpr u32 kPrsCopyEdge = 0x8004410Cu;
constexpr u32 kPrsTokenEdge = 0x80044110u;

// NE-OS-0 (EV, 2026-10-20; findings-20261019-EV/EV_REPORT.md 22.5 / 23): the SDK's cache maintenance. On the console
// DCFlushRange (dcbf loop) / DCStoreRange (dcbst loop) wrote dirty data-cache lines back to memory before a device DMA
// read it, and each ended with `sc`, whose vector (PPCSync's head) set and restored HID0[ABE] around isync / sync;
// PPCSync is that `sc` alone. They are the only three `sc` in the DOL. The game's memory here has no cache and no
// device reads it behind the CPU's back, so what the game asks for - "make these bytes visible to the hardware" - is
// already true: the owners return. This deletes the system-call exception from the run (92,350 per Lava run before).
// Proof standard: GLE (NO_EMULATION_PLAN 2): the guest registers the bodies left behind (r3-r5, CTR, CR0, r9, r10,
// SRR0 / SRR1) and the exception count differ by construction and are attributed; game memory must be equal.
constexpr u32 kPpcSync = 0x80370B14u;       // PPCSync: sc; blr
constexpr u32 kDcFlushRange = 0x80372534u;  // DCFlushRange(addr, n): dcbf loop; sc; blr
constexpr u32 kDcStoreRange = 0x80372564u;  // DCStoreRange(addr, n): dcbst loop; sc; blr
// 2026-11-04: their two siblings without the `sc` were still translated - an empty counted loop, one iteration per
// 32-byte line with a dispatch test on each back edge, which the AX frame ran over its buffers every 5 ms
// (DCInvalidateRange 0.07 ms/frame of the game thread). Owned the same way, and stricter than the three above: the
// owner leaves exactly the registers the loop leaves (serve_cache_range_loop), so only the time differs.
constexpr u32 kDcInvalidateRange = 0x80372508u;   // DCInvalidateRange(addr, n): dcbi loop; blr
constexpr u32 kDcFlushRangeNoSync = 0x80372594u;  // DCFlushRangeNoSync(addr, n): dcbf loop; blr
// Their loop heads, owner entries: a state saved by a build that still translated the loops (or any resumption there)
// can stand at one, mid-loop, with the remaining line count in CTR.
constexpr u32 kDcInvalidateLoop = 0x80372524u;
constexpr u32 kDcFlushNoSyncLoop = 0x803725B0u;

const GameMemory::Address kAddresses[] = {
    {kMemmove, "memcpy (memmove)", false},
    {kMemmoveForwardHead, "memcpy: forward loop resumes", true},
    {kMemmoveBackwardHead, "memcpy: backward loop resumes", true},
    {kPrsExpand, "PRS expand", false},
    {kPrsTokenHead, "PRS expand: token loop resumes", true},
    {kPrsShortLengthHead, "PRS expand: short length resumes", true},
    {kPrsCopyHead, "PRS expand: match copy resumes", true},
    {kPpcSync, "PPCSync", false},
    {kDcFlushRange, "DCFlushRange", false},
    {kDcStoreRange, "DCStoreRange", false},
    {kDcInvalidateRange, "DCInvalidateRange", false},
    {kDcFlushRangeNoSync, "DCFlushRangeNoSync", false},
    {kDcInvalidateLoop, "DCInvalidateRange: line loop resumes", true},
    {kDcFlushNoSyncLoop, "DCFlushRangeNoSync: line loop resumes", true},
};

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "native game memory: " + why);
}

// DCInvalidateRange / DCFlushRangeNoSync's architected effect, from their words (0x80372508 / 0x80372594):
//   cmplwi r4,0; blelr; clrlwi r5,r3,27; add r4,r4,r5; addi r4,r4,31; srwi r4,r4,5; mtctr r4;
//   loop: dcbi|dcbf 0,r3; addi r3,r3,32; bdnz loop; blr
// The per-line cache operation has nothing to act on here, so the loop's only results are its registers: CR0 from
// the compare, r5, r4 = the line count, r3 advanced by 32 per line, CTR run down to 0. A count of 0 (only when
// r4 + r5 + 31 wraps) is 2^32 iterations on the console, which advance r3 by 2^37 = 0 mod 2^32: the same formula.
}  // namespace

void serve_cache_range_loop(Cpu& c) {
    ppc::cmpl(c, 0u, c.gpr[4], 0u);
    if (c.gpr[4] == 0u) return;
    const u32 start = c.gpr[3];
    c.gpr[5] = start & 0x1Fu;
    const u32 lines = (c.gpr[4] + c.gpr[5] + 0x1Fu) >> 5;
    c.gpr[4] = lines;
    c.ctr = 0u;
    c.gpr[3] = start + (lines << 5);
}

// The same loop entered at its head (dcbi|dcbf 0,r3; addi r3,r3,32; bdnz): CTR more lines, CTR = 0 meaning 2^32.
void serve_cache_range_loop_resume(Cpu& c) {
    c.gpr[3] += c.ctr << 5;
    c.ctr = 0u;
}

namespace {

// ---- guest bytes -------------------------------------------------------------
//
// The CPU's own accessors serve an access from host memory exactly when data
// translation is on in supervisor mode and nothing must observe every access
// (the oracle's journal, the poll watch, an access trace; a latched data fault
// never reaches an owner). Under that same condition the owners read and write
// the same host memory directly; otherwise every byte goes through the CPU
// accessors a translated instruction uses, so faults, traces and journals see
// what they always saw.
class GuestBytes {
public:
    explicit GuestBytes(Cpu& c) noexcept
        : cpu_(c),
          memory_(c.guest_memory()),
          direct_((c.msr() & (kMsrDR | kMsrPR)) == kMsrDR && !memory_.journaling() && !memory_.access_traced()) {}

    bool direct() const noexcept { return direct_; }
    Cpu& cpu() noexcept { return cpu_; }
    GuestMemory& memory() noexcept { return memory_; }

    // A host span for [address, address + size) inside one committed region,
    // or nullptr (not direct, wrapping, unmapped, a device, a region boundary).
    u8* span(u32 address, u64 size) noexcept {
        if (!direct_ || size == 0u || static_cast<u64>(address) + size > 0x100000000ull) return nullptr;
        return memory_.host_pointer(address, static_cast<std::size_t>(size));
    }
    // The committed part of the region holding `address`, as host memory: guest [base, end) at host + (a - base).
    // Empty when not direct or nothing is committed there.
    struct Run {
        u8* host = nullptr;
        u64 base = 0, end = 0;
    };
    Run run_at(u32 address) const noexcept {
        if (!direct_) return {};
        const GuestMemory::Region* region = memory_.region_at(address);
        if (region == nullptr || region->host == nullptr || address - region->base >= region->committed) return {};
        return {region->host, region->base, static_cast<u64>(region->base) + region->committed};
    }

private:
    Cpu& cpu_;
    GuestMemory& memory_;
    const bool direct_;
};

// One sequential byte stream (the PRS source, its match source, its output):
// the host span of the page it is in, held across bytes and refilled only
// when the stream leaves the page. The hit path (one subtraction, one compare)
// is inlined into the decoder's loops; a page that does not resolve keeps an
// empty span, so every byte of it takes the CPU accessors (the cold path).
class Cursor {
public:
    GEKKO_ALWAYS_INLINE u8* at(GuestBytes& bytes, u32 address) noexcept {
        const u32 offset = address - base_;
        if (offset < size_) return host_ + offset;
        return refill(bytes, address);
    }

private:
    GEKKO_NOINLINE u8* refill(GuestBytes& bytes, u32 address) noexcept {
        const u32 page = address & ~(Cpu::kHostPageSize - 1u);
        u8* const host = bytes.span(page, Cpu::kHostPageSize);
        base_ = page;
        host_ = host;
        size_ = host != nullptr ? Cpu::kHostPageSize : 0u;
        return host != nullptr ? host + (address - page) : nullptr;
    }

    u32 base_ = 0;
    u32 size_ = 0;
    u8* host_ = nullptr;
};

u32 signed_field(u32 value, u32 so) noexcept {
    const s32 s = static_cast<s32>(value);
    return (s < 0 ? 8u : (s > 0 ? 4u : 2u)) | so;
}
u32 unsigned_field(u32 lhs, u32 rhs, u32 so) noexcept { return (lhs < rhs ? 8u : (lhs > rhs ? 4u : 2u)) | so; }

u64 fnv1a(GuestMemory& memory, u32 address, u32 size) {
    std::vector<u8> bytes(size);
    if (size != 0u) memory.peek_block(address, bytes.data(), size);
    u64 hash = 1469598103934665603ull;
    for (const u8 b : bytes) hash = (hash ^ b) * 1099511628211ull;
    return hash;
}

// ---- the boundary ------------------------------------------------------------
//
// Both owners are entered by TRANSLATED callers, which keep the game's own
// calling contract: arguments in r3-r5, the result in r3, and whatever else the
// original leaves in the volatile registers (r0, r4-r10, ctr, CR0, XER[CA]).
// A translated loop also hands control back to the dispatcher at each loop back
// edge while Cpu::needs_dispatch() holds (a stop, or an interrupt line with
// MSR[EE]), so an interrupt or a guest thread switch can land in the middle of
// a copy, and resumes at the loop head.
//
// The owners reproduce that contract exactly at their edges: the same result,
// the same final register file, the same hand-backs at the same back edges with
// the same register file, and an owner entry at every loop head a hand-back
// resumes at. Between those edges the work is plain native code. Two facts make
// the bulk paths exact (docs: EV_REPORT.md 7.1):
//   * needs_dispatch() reads interrupt_lines_ / MSR[EE], which only game-clock
//     events and native deliveries change - never a RAM access - and the stop
//     flag. So when a back edge does not hand back, no later back edge reached
//     through RAM-only accesses would either (only an asynchronous host stop
//     could, and the run ends then anyway).
//   * The byte-wise forward copy (source >= destination) and backward copy
//     (source < destination) both equal memmove, overlap included.

// "memcpy" (0x800054F4). r3 = dst (returned unchanged), r4 = src, r5 = n.
// Registers as the original leaves them (forward: r4 = src+n-1, r6 = dst+n-1;
// backward: r4 = src, r6 = dst; r5 = 0; r0 = the last byte moved, untouched for
// n = 0; CR0 = EQ from the final addic.; XER[CA] = 1).
class MoveEngine {
public:
    MoveEngine(Cpu& c, GameMemory::Stats& stats) noexcept
        : c_(c), bytes_(c), stats_(stats), so_((c.xer & kXerSO) != 0u ? 1u : 0u) {}

    // Returns true when the call finished (pc = lr follows); false after a hand-back.
    bool run(u32 entry) {
        load();
        switch (entry) {
            case kMemmove:
                cr0_ = unsigned_field(r4_, r3_, so_);  // cmplw r4, r3
                if (r4_ < r3_) {
                    r4_ = r4_ + r5_;
                    r6_ = r3_ + r5_;
                    r5_ = r5_ + 1u;
                    goto backward_count;
                }
                r4_ = r4_ - 1u;
                r6_ = r3_ - 1u;
                r5_ = r5_ + 1u;
                goto forward_count;
            case kMemmoveForwardHead:
                goto forward_byte;
            case kMemmoveBackwardHead:
                goto backward_byte;
            default:
                return true;
        }
    forward_byte:
        r0_ = read(r4_ + 1u);
        r4_ = r4_ + 1u;
        write(r6_ + 1u, static_cast<u8>(r0_));
        r6_ = r6_ + 1u;
        ++stats_.move_bytes_single;
    forward_count:
        count();
        if (r5_ != 0u) {
            if (edge(kMemmoveForwardHead, kMemmoveForwardEdge)) return false;
            if (bulk(true)) goto done;
            goto forward_byte;
        }
        goto done;
    backward_byte:
        r0_ = read(r4_ - 1u);
        r4_ = r4_ - 1u;
        write(r6_ - 1u, static_cast<u8>(r0_));
        r6_ = r6_ - 1u;
        ++stats_.move_bytes_single;
    backward_count:
        count();
        if (r5_ != 0u) {
            if (edge(kMemmoveBackwardHead, kMemmoveBackwardEdge)) return false;
            if (bulk(false)) goto done;
            goto backward_byte;
        }
    done:
        store();
        return true;
    }

private:
    // addic. r5, r5, -1
    void count() noexcept {
        ca_ = r5_ != 0u;
        r5_ = r5_ - 1u;
        cr0_ = signed_field(r5_, so_);
    }
    bool edge(u32 head, u32 site) noexcept {
        c_.pc = head;
        if (!c_.needs_dispatch()) return false;
        store();
        ++stats_.move_yields;
        c_.leave_to_dispatcher(head, FallbackReason::Yield, site);
        return true;
    }
    // At a loop head with r5 = k bytes left and no hand-back pending: the k
    // remaining iterations as one host memmove, when both ranges are host memory.
    bool bulk(bool forward) noexcept {
        const u32 k = r5_;
        const u32 src = forward ? r4_ + 1u : r4_ - k;
        const u32 dst = forward ? r6_ + 1u : r6_ - k;
        if (!forward && (r4_ < k || r6_ < k)) return false;  // a wrapping range: one byte at a time
        u8* const from = bytes_.span(src, k);
        u8* const to = bytes_.span(dst, k);
        if (from == nullptr || to == nullptr) return false;
        // The last byte the loop would have moved, as it read it: the source
        // byte it reaches last is never written before that read in its
        // direction (forward: source >= destination; backward: source < destination).
        r0_ = forward ? from[k - 1u] : from[0];
        std::memmove(to, from, k);
        r4_ = forward ? r4_ + k : r4_ - k;
        r6_ = forward ? r6_ + k : r6_ - k;
        r5_ = 0u;
        ca_ = true;                         // the final addic. r5 = 1 -> 0
        cr0_ = 2u | so_;
        stats_.move_bytes_bulk += k;
        return true;
    }
    u8 read(u32 address) {
        if (u8* p = bytes_.span(address, 1u)) return *p;
        store();
        return c_.mem_read8(address);
    }
    void write(u32 address, u8 value) {
        if (u8* p = bytes_.span(address, 1u)) {
            *p = value;
            return;
        }
        store();
        c_.mem_write8(address, value);
    }
    void load() noexcept {
        r0_ = c_.gpr[0];
        r3_ = c_.gpr[3];
        r4_ = c_.gpr[4];
        r5_ = c_.gpr[5];
        r6_ = c_.gpr[6];
        cr0_ = (c_.cr >> 28) & 0xFu;
        ca_ = (c_.xer & kXerCA) != 0u;
    }
    void store() noexcept {
        c_.gpr[0] = r0_;
        c_.gpr[3] = r3_;
        c_.gpr[4] = r4_;
        c_.gpr[5] = r5_;
        c_.gpr[6] = r6_;
        c_.cr = (c_.cr & 0x0FFFFFFFu) | (cr0_ << 28);
        c_.xer = ca_ ? (c_.xer | kXerCA) : (c_.xer & ~kXerCA);
    }

    Cpu& c_;
    GuestBytes bytes_;
    GameMemory::Stats& stats_;
    const u32 so_;
    u32 r0_ = 0, r3_ = 0, r4_ = 0, r5_ = 0, r6_ = 0, cr0_ = 0;
    bool ca_ = false;
};

// The PRS decoder's register file (PrsEngine) between tokens.
struct PrsRegisters {
    u32 r0, r3, r4, r5, r6, r7, r8, r9, r10, ctr, cr0;
    bool ca;
};

enum class PrsRaw { Done, Boundary, AtCopy };

// PRS tokens on host memory, while the stream and the output stay inside their committed runs and no hand-back can
// be due (PrsEngine::run decides). The same arithmetic as the token loop, on plain locals:
// - r0, r3, r5, r6, CTR and XER[CA] stay exact, because the loop publishes them at a cold access before it
//   recomputes them;
// - CR0 is set only at the terminator (every other value of it is overwritten before the loop's next edge or access).
// It returns:
// - at a token boundary, when a run ends within one token's reach or `budget` tokens have run (the stop-request poll);
// - at the match copy (AtCopy, registers ready), when the match source is not in the output's run;
// - Done at the terminator.
PrsRaw prs_raw(PrsRegisters& reg, const GuestBytes::Run& in, const GuestBytes::Run& out, u32 so, u32 budget) noexcept {
    // A token reads at most one flag byte and three stream bytes, and writes at most 256 bytes (the extended long
    // length, byte + 1).
    constexpr u64 kStreamReach = 4u, kOutputReach = 256u;
    u32 r0 = reg.r0, r3 = reg.r3, r4 = reg.r4, r5 = reg.r5, r6 = reg.r6, r8 = reg.r8, r9 = reg.r9, r10 = reg.r10,
        ctr = reg.ctr;
    bool ca = reg.ca;
    const u32 in_base = static_cast<u32>(in.base), out_base = static_cast<u32>(out.base);
#define SHADOW_PRS_IN(address) in.host[static_cast<u32>((address) - in_base)]
#define SHADOW_PRS_OUT(address) out.host[static_cast<u32>((address) - out_base)]
// addic. r10, r10, -1; on zero: lbz r9,0(r8); li r10,8; addi r8,r8,1
#define SHADOW_PRS_FLAG()            \
    do {                             \
        ca = r10 != 0u;              \
        r10 = r10 - 1u;              \
        if (r10 == 0u) {             \
            r9 = SHADOW_PRS_IN(r8);  \
            r10 = 8u;                \
            r8 = r8 + 1u;            \
        }                            \
    } while (false)
    PrsRaw result = PrsRaw::Boundary;
    for (u32 left = budget; left != 0u; --left) {
        if (r8 < in.base || r8 + kStreamReach > in.end || r4 < out.base || r4 + kOutputReach > out.end) break;
        SHADOW_PRS_FLAG();
        r3 = r9 & 1u;
        r9 >>= 1u;
        if (r3 == 1u) {  // literal
            r0 = SHADOW_PRS_IN(r8);
            r8 = r8 + 1u;
            SHADOW_PRS_OUT(r4) = static_cast<u8>(r0);
            r4 = r4 + 1u;
            continue;
        }
        SHADOW_PRS_FLAG();
        r0 = r9 & 1u;
        r9 >>= 1u;
        if (r0 == 1u) {  // long match
            r0 = SHADOW_PRS_IN(r8 + 1u);
            r3 = SHADOW_PRS_IN(r8);
            r8 = r8 + 2u;
            r0 = ((r0 << 8) & 0xFF00u) | r3;
            if (r0 == 0u) {  // the terminator
                r3 = r4 - reg.r7;
                reg.cr0 = signed_field(r0, so);
                result = PrsRaw::Done;
                break;
            }
            r6 = r3 & 7u;
            r3 = r0 >> 3;
            r0 = 0xFFFFE000u;
            r5 = r3 | r0;
            if (r6 == 0u) {
                r3 = SHADOW_PRS_IN(r8);
                r8 = r8 + 1u;
                r6 = r3 + 1u;
            } else {
                r6 = r6 + 2u;
            }
            r5 = r5 + r4;
        } else {  // short match: two flag bits, MSB first
            r0 = 2u;
            r6 = 0u;
            ctr = 2u;
            for (u32 bit = 0; bit < 2u; ++bit) {
                SHADOW_PRS_FLAG();
                r3 = r9 & 1u;
                r0 = r6 << 1;
                r9 >>= 1u;
                r6 = r0 | r3;
                ctr = ctr - 1u;
            }
            r3 = SHADOW_PRS_IN(r8);
            r0 = 0xFFFFFF00u | (r3 & 0xFFu);
            r6 = r6 + 2u;
            r8 = r8 + 1u;
            r5 = r0 + r4;
        }
        // The copy: r6 bytes from r5, 1..8192 bytes behind r4 unless the address wrapped below zero.
        ctr = r6;
        if (r6 == 0u) continue;
        if (r5 < out.base || r5 >= r4) {
            result = PrsRaw::AtCopy;
            break;
        }
        const u32 n = r6, distance = r4 - r5;
        u8* const to = &SHADOW_PRS_OUT(r4);
        const u8* const from = &SHADOW_PRS_OUT(r5);
        if (distance >= n) {
            std::memcpy(to, from, n);  // disjoint runs
        } else {
            for (u32 i = 0; i < n; ++i) to[i] = from[i];  // repeats the last `distance` bytes, forward
        }
        r0 = to[n - 1u];
        r5 = r5 + n;
        r4 = r4 + n;
        r6 = 0u;
        ctr = 0u;
    }
#undef SHADOW_PRS_FLAG
#undef SHADOW_PRS_OUT
#undef SHADOW_PRS_IN
    reg.r0 = r0;
    reg.r3 = r3;
    reg.r4 = r4;
    reg.r5 = r5;
    reg.r6 = r6;
    reg.r8 = r8;
    reg.r9 = r9;
    reg.r10 = r10;
    reg.ctr = ctr;
    reg.ca = ca;
    return result;
}

// PRS expand (0x80043FE0). r3 = src, r4 = dst; returns r3 = bytes written. No
// bounds: the stream ends at its zero word. The format: flag bits LSB first,
// eight per flag byte, reloaded on demand; 1 = a literal byte; 0 1 = a long
// match (little-endian word w: w == 0 ends; distance (w >> 3) | ~0x1FFF,
// length (w & 7) + 2, or the next byte + 1 when w & 7 == 0); 0 0 = a short
// match (two more flag bits, MSB first, + 2; distance byte | ~0xFF). Matches
// copy forward byte by byte from the output's own past (they may overlap it
// and may reach before dst).
//
// The original keeps the decoder's state in r0, r3-r10, CTR, CR0 and XER[CA], and a hand-back leaves it there, so
// the decoder works on exactly those values - as locals, which the compiler keeps in host registers (members of an
// object reached through the CPU would be reloaded around every guest access). Its back edges poll for an
// interrupt; a hand-back can only be due where Cpu::needs_dispatch() can be true:
//   * at entry (an interrupt line already up with MSR[EE], or a stop request): then the poll runs at every edge,
//     and the first edge the original reaches hands back, with the register file the original has there;
//   * after a guest access through the CPU's accessors (the cold path: a device, a trace or a journal), which may
//     raise a line: the poll is re-armed from it;
//   * a stop request from another thread, which no instruction boundary orders against the guest: polled every
//     kStopPollTokens tokens.
// Interrupt lines are the game thread's state and game time does not move during the call, so nothing else can
// make a hand-back due. docs/FRAME_PACING.md "The remaining long intervals" (why this matters: the stage-start
// stall) and tests/game_memory_owner_tests.cpp (register-exact against the original words, with hand-backs).
class PrsEngine {
public:
    PrsEngine(Cpu& c, GameMemory::Stats& stats) noexcept : c_(c), bytes_(c), stats_(stats) {}

    bool yielded() const noexcept { return yielded_; }
    u32 source_start() const noexcept { return src0_; }
    u32 source_end() const noexcept { return source_end_; }

    // Returns true when the call finished (pc = lr follows); false after a hand-back.
    bool run(u32 entry) {
        Cpu& c = c_;
        const u32 so = (c.xer & kXerSO) != 0u ? 1u : 0u;
        u32 r0 = c.gpr[0], r3 = c.gpr[3], r4 = c.gpr[4], r5 = c.gpr[5], r6 = c.gpr[6], r7 = c.gpr[7],
            r8 = c.gpr[8], r9 = c.gpr[9], r10 = c.gpr[10], ctr = c.ctr, cr0 = (c.cr >> 28) & 0xFu;
        bool ca = (c.xer & kXerCA) != 0u;
        bool poll = c.needs_dispatch();
        u32 head_pc = c.pc;  // the original publishes its loop head at every back edge
        u32 tokens = 0;
        Cursor source, match, output;

        const auto store = [&]() noexcept {
            c.gpr[0] = r0;
            c.gpr[3] = r3;
            c.gpr[4] = r4;
            c.gpr[5] = r5;
            c.gpr[6] = r6;
            c.gpr[7] = r7;
            c.gpr[8] = r8;
            c.gpr[9] = r9;
            c.gpr[10] = r10;
            c.ctr = ctr;
            c.cr = (c.cr & 0x0FFFFFFFu) | (cr0 << 28);
            c.xer = ca ? (c.xer | kXerCA) : (c.xer & ~kXerCA);
        };
        // Not host memory (or not served from it now): the translated code's own accessor, with the register file
        // published first so a fault reports the same state; the access may raise a line.
        const auto read = [&](u32 address, Cursor& cursor) -> u32 {
            if (u8* p = cursor.at(bytes_, address)) return *p;
            store();
            c.pc = head_pc;
            ++stats_.accessor_bytes;
            const u32 value = c.mem_read8(address);
            poll = poll || c.needs_dispatch();
            return value;
        };
        const auto write = [&](u32 address, u8 value) {
            if (u8* p = output.at(bytes_, address)) {
                *p = value;
                return;
            }
            store();
            c.pc = head_pc;
            ++stats_.accessor_bytes;
            c.mem_write8(address, value);
            poll = poll || c.needs_dispatch();
        };
        // addic. r10, r10, -1; on zero: lbz r9,0(r8); li r10,8; addi r8,r8,1
        const auto next_flag = [&]() {
            ca = r10 != 0u;
            r10 = r10 - 1u;
            cr0 = signed_field(r10, so);
            if (r10 == 0u) {
                r9 = read(r8, source);
                r10 = 8u;
                r8 = r8 + 1u;
            }
        };
        // An original back edge: the translated loop's needs_dispatch() poll, where one can be due.
        const auto edge = [&](u32 head, u32 site) -> bool {
            head_pc = head;
            if (!poll) return false;
            c.pc = head;
            if (!c.needs_dispatch()) {
                poll = false;
                return false;
            }
            store();
            yielded_ = true;
            source_end_ = r8;
            ++stats_.expansion_yields;
            c.leave_to_dispatcher(head, FallbackReason::Yield, site);
            return true;
        };

        switch (entry) {
            case kPrsExpand:
                src0_ = r3;
                r9 = read(r3, source);  // the first flag byte
                r8 = r3 + 1u;
                r10 = 9u;
                r7 = r4;
                goto token;
            case kPrsTokenHead:
                goto token;
            case kPrsShortLengthHead:
                goto short_length_bit;
            case kPrsCopyHead:
                goto copy_byte;
            default:
                return true;
        }
    token:
        if (++tokens == kStopPollTokens) {
            tokens = 0;
            poll = poll || c.needs_dispatch();
        }
        // Whole tokens on host memory while no hand-back can be due (prs_raw); the loop below takes the token where
        // it stopped (a run's end within one token's reach) or the match copy (a source outside the output's run).
        if (!poll) {
            const GuestBytes::Run in = bytes_.run_at(r8), out = bytes_.run_at(r4);
            if (in.host != nullptr && out.host != nullptr) {
                PrsRegisters reg{r0, r3, r4, r5, r6, r7, r8, r9, r10, ctr, cr0, ca};
                const PrsRaw raw = prs_raw(reg, in, out, so, kStopPollTokens);
                const bool moved = reg.r8 != r8 || reg.r4 != r4;
                r0 = reg.r0, r3 = reg.r3, r4 = reg.r4, r5 = reg.r5, r6 = reg.r6, r8 = reg.r8, r9 = reg.r9;
                r10 = reg.r10, ctr = reg.ctr, cr0 = reg.cr0, ca = reg.ca;
                if (moved) head_pc = kPrsTokenHead;  // the last back edge passed is a token edge
                if (raw == PrsRaw::Done) {
                    store();
                    source_end_ = r8;
                    return true;
                }
                if (raw == PrsRaw::AtCopy) goto copy;
                tokens = 0;
                poll = c.needs_dispatch();  // the stop-request poll between batches
            }
        }
        next_flag();
        r3 = r9 & 1u;
        r9 >>= 1u;
        cr0 = unsigned_field(r3, 1u, so);
        if (r3 == 1u) {
            r0 = read(r8, source);
            r8 = r8 + 1u;
            write(r4, static_cast<u8>(r0));
            r4 = r4 + 1u;
            if (edge(kPrsTokenHead, kPrsLiteralEdge)) return false;
            goto token;
        }
        next_flag();
        r0 = r9 & 1u;
        r9 >>= 1u;
        cr0 = unsigned_field(r0, 1u, so);
        if (r0 == 1u) {
            // long match: lbz r0,1(r8); lbz r3,0(r8)
            r0 = read(r8 + 1u, source);
            r3 = read(r8, source);
            r8 = r8 + 2u;
            r0 = (r0 << 8) & 0xFF00u;
            r0 = r3 | r0;
            cr0 = signed_field(r0, so);
            if (r0 == 0u) {  // the terminator
                r3 = r4 - r7;
                store();
                source_end_ = r8;
                return true;
            }
            r6 = r3 & 7u;
            cr0 = signed_field(r6, so);
            r3 = r0 >> 3;
            r0 = 0xFFFFE000u;
            r5 = r3 | r0;
            if (r6 == 0u) {
                r3 = read(r8, source);
                r8 = r8 + 1u;
                r6 = r3 + 1u;
            } else {
                r6 = r6 + 2u;
            }
            r5 = r5 + r4;
            goto copy;
        }
        // short match
        r0 = 2u;
        r6 = 0u;
        ctr = 2u;
    short_length_bit:
        next_flag();
        r3 = r9 & 1u;
        r0 = r6 << 1;
        r9 >>= 1u;
        r6 = r0 | r3;
        ctr = ctr - 1u;
        if (ctr != 0u) {
            if (edge(kPrsShortLengthHead, kPrsShortLengthEdge)) return false;
            goto short_length_bit;
        }
        r3 = read(r8, source);
        r0 = 0xFFFFFF00u;
        r6 = r6 + 2u;
        r8 = r8 + 1u;
        r0 = (r0 & 0xFFFFFF00u) | (r3 & 0xFFu);
        r5 = r0 + r4;
    copy:
        ctr = r6;
        cr0 = unsigned_field(r6, 0u, so);
        if (r6 == 0u) {
            if (edge(kPrsTokenHead, kPrsEmptyCopyEdge)) return false;
            goto token;
        }
        // The whole match at once, where none of its back edges can hand back and both runs are host memory: the
        // same bytes in the same order as the byte loop, which leaves r0 = the last byte, r5 / r4 advanced by the
        // length and r6 = CTR = 0. The match starts 1..8192 bytes behind the output: a distance of at least the
        // length is a copy between disjoint runs; a shorter one repeats the last `distance` bytes, byte by byte
        // forward as the loop does.
        if (!poll) {
            const u32 n = r6, distance = r4 - r5;
            u8* const from = bytes_.span(r5, n);
            u8* const to = bytes_.span(r4, n);
            if (from != nullptr && to != nullptr) {
                if (distance >= n) {
                    std::memcpy(to, from, n);
                } else {
                    for (u32 i = 0; i < n; ++i) to[i] = from[i];
                }
                r0 = to[n - 1u];
                r5 = r5 + n;
                r4 = r4 + n;
                r6 = 0u;
                ctr = 0u;
                head_pc = kPrsTokenHead;  // the last back edge the loop passes (kPrsTokenEdge)
                goto token;
            }
        }
    copy_byte:
        r0 = read(r5, match);
        r5 = r5 + 1u;
        r6 = r6 - 1u;
        write(r4, static_cast<u8>(r0));
        r4 = r4 + 1u;
        ctr = ctr - 1u;
        if (ctr != 0u) {
            if (edge(kPrsCopyHead, kPrsCopyEdge)) return false;
            goto copy_byte;
        }
        if (edge(kPrsTokenHead, kPrsTokenEdge)) return false;
        goto token;
    }

private:
    // A stop request from another thread is polled this often (tokens); an interrupt cannot become due mid-call.
    static constexpr u32 kStopPollTokens = 4096u;

    Cpu& c_;
    GuestBytes bytes_;
    GameMemory::Stats& stats_;
    bool yielded_ = false;
    u32 src0_ = 0, source_end_ = 0;
};

}  // namespace

bool prs_decode_ahead(const u8* stream, std::size_t available, u32 size_hint, PrsDecoded& out, bool* hint_short) {
    if (hint_short != nullptr) *hint_short = false;
    // Arbitrary guest-shaped bases: every address register the decoder leaves is a base plus an offset.
    constexpr u32 kIn = 0x10000000u, kOut = 0x40000000u;
    if (available == 0u || available > 0x20000000u || size_hint > 0x10000000u) return false;
    out = PrsDecoded{};
    out.output.assign(static_cast<std::size_t>(size_hint) + 512u, 0u);
    const GuestBytes::Run in{const_cast<u8*>(stream), kIn, u64{kIn} + available};
    const GuestBytes::Run dst{out.output.data(), kOut, u64{kOut} + out.output.size()};
    // The expand entry (PrsEngine::run, kPrsExpand): the first flag byte, r8 past it, r10 = 9, r7 = dst. r6 starts
    // at a value no match leaves behind (a copy ends with r6 = 0; its length is never 0), so r6 == 0 at the end says
    // a match ran.
    PrsRegisters reg{0u, kIn, kOut, 0u, 0xFFFFFFFFu, kOut, kIn + 1u, stream[0], 9u, 0xFFFFFFFFu, 0u, false};
    for (;;) {
        const u32 r8 = reg.r8, r4 = reg.r4;
        const PrsRaw raw = prs_raw(reg, in, dst, 0u, 0x40000000u);
        if (raw == PrsRaw::Done) break;
        if (raw == PrsRaw::AtCopy) return false;            // a copy from before the output
        if (reg.r8 == r8 && reg.r4 == r4) {                 // a run's end within one token's reach
            // The output's reach is 256 bytes past the cursor: a cursor that close to the buffer's end ran out of
            // output (a larger hint helps); otherwise the input ended.
            if (hint_short != nullptr && u64{reg.r4} + 0x100u + 2u >= u64{kOut} + out.output.size()) *hint_short = true;
            return false;
        }
    }
    out.stream = stream;
    out.consumed = reg.r8 - kIn;
    out.output.resize(reg.r4 - kOut);
    out.r9 = reg.r9;
    out.r10 = reg.r10;
    out.ca = reg.ca;
    out.any_match = reg.r6 == 0u;
    out.r5_from_dst = reg.r5 - kOut;
    return true;
}

const GameMemory::Address* GameMemory::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

GameMemory::~GameMemory() {
    if (journal_ != nullptr) {
        std::fprintf(journal_,
                     "# end: moves %llu (resumes %llu, yields %llu, bulk bytes %llu, single bytes %llu); expansions "
                     "%llu (resumes %llu, yields %llu, bytes %llu, in place %llu, accessor bytes %llu)\n",
                     static_cast<unsigned long long>(stats_.moves),
                     static_cast<unsigned long long>(stats_.move_resumes),
                     static_cast<unsigned long long>(stats_.move_yields),
                     static_cast<unsigned long long>(stats_.move_bytes_bulk),
                     static_cast<unsigned long long>(stats_.move_bytes_single),
                     static_cast<unsigned long long>(stats_.expansions),
                     static_cast<unsigned long long>(stats_.expansion_resumes),
                     static_cast<unsigned long long>(stats_.expansion_yields),
                     static_cast<unsigned long long>(stats_.expanded_bytes),
                     static_cast<unsigned long long>(stats_.expansions_in_place),
                     static_cast<unsigned long long>(stats_.accessor_bytes));
        std::fclose(journal_);
        journal_ = nullptr;
    }
}

void GameMemory::install() {
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, std::string("native game memory: ") + name);
    };
    // RealMode (0x80376B48) and the BAT configurers it was given are gone: __OSInitMemoryProtection, their only
    // caller, is part of the native OSInit (gekko/native_boot.cpp), which configures nothing (the window is not
    // translated by BATs, GuestMemory).
    // NE-OS-0: cache maintenance has nothing to do (see kPpcSync above); the call returns to LR.
    own(kPpcSync, "PPCSync", [this](Cpu&) { ++stats_.cache_syncs; });
    own(kDcFlushRange, "DCFlushRange", [this](Cpu&) { ++stats_.cache_flushes; });
    own(kDcStoreRange, "DCStoreRange", [this](Cpu&) { ++stats_.cache_stores; });
    own(kDcInvalidateRange, "DCInvalidateRange", [this](Cpu& c) {
        ++stats_.cache_invalidates;
        serve_cache_range_loop(c);
    });
    own(kDcFlushRangeNoSync, "DCFlushRangeNoSync", [this](Cpu& c) {
        ++stats_.cache_flushes_no_sync;
        serve_cache_range_loop(c);
    });
    own(kDcInvalidateLoop, "DCInvalidateRange: line loop resumes", [](Cpu& c) { serve_cache_range_loop_resume(c); });
    own(kDcFlushNoSyncLoop, "DCFlushRangeNoSync: line loop resumes",
        [](Cpu& c) { serve_cache_range_loop_resume(c); });

    if (const char* path = std::getenv("SHADOW_GAME_MEMORY_JOURNAL"); path != nullptr && path[0] != '\0') {
        journal_ = std::fopen(path, "w");
        if (journal_ != nullptr) {
            std::fprintf(journal_, "# kind lr dst src n|written fnv1a64(dst..) [in_place] - complete calls only\n");
        }
    }

    // memcpy (memmove). The journal records each call that ran start to end
    // without a hand-back: lr, dst, src, n and the hash of what dst holds after.
    const auto move = [this](Cpu& c, u32 entry) {
        const u32 lr = c.lr, dst = c.gpr[3], src = c.gpr[4], n = c.gpr[5];
        if (entry == kMemmove) ++stats_.moves; else ++stats_.move_resumes;
        MoveEngine engine(c, stats_);
        const bool finished = engine.run(entry);
        if (journal_ != nullptr && finished && entry == kMemmove) {
            std::fprintf(journal_, "mm %08X %08X %08X %08X %016llX\n", lr, dst, src, n,
                         static_cast<unsigned long long>(fnv1a(c.guest_memory(), dst, n)));
        }
    };
    own(kMemmove, "memcpy (memmove)", [move](Cpu& c) { move(c, kMemmove); });
    own(kMemmoveForwardHead, "memcpy: forward loop resumes", [move](Cpu& c) { move(c, kMemmoveForwardHead); });
    own(kMemmoveBackwardHead, "memcpy: backward loop resumes", [move](Cpu& c) { move(c, kMemmoveBackwardHead); });

    // PRS expand. Expansions that ran start to end without a hand-back are
    // counted (and journalled) with their size and whether the consumed source
    // overlapped the written output (an in-place expansion).
    const auto expand = [this](Cpu& c, u32 entry) {
        const u32 lr = c.lr, src = c.gpr[3], dst = c.gpr[4];
        const auto started = std::chrono::steady_clock::now();
        const auto spent = [&started]() {
            return static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
        };
        if (entry == kPrsExpand) ++stats_.expansions; else ++stats_.expansion_resumes;
        // A stream decoded ahead (gekko/prs_ahead.h), when every condition of serve_prs_ahead holds.
        if (entry == kPrsExpand && prs_ahead_ && journal_ == nullptr && !c.needs_dispatch()) {
            if (const u8* first = prs_ahead_key_bytes(c, src)) {
                const std::shared_ptr<const PrsDecoded> decoded =
                    prs_ahead_(first, [this, &c](const PrsDecoded& d) { return serve_prs_ahead(c, d); });
                if (decoded != nullptr) {
                    stats_.expanded_bytes += static_cast<u32>(decoded->output.size());
                    const u64 ns = spent();
                    ++stats_.prs_ahead_served;
                    stats_.prs_ahead_ns += ns;
                    stats_.prs_ahead_ns_max = std::max(stats_.prs_ahead_ns_max, ns);
                    return;
                }
            }
        }
#if !defined(GEKKO_SHIPPING)
        // Measurement: why an expand runs the engine here instead of taking a stream decoded ahead (stderr, the first
        // 64 per run).
        if (entry == kPrsExpand) {
            static int logged = 0;
            if (logged < 64) {
                ++logged;
                const char* why = !prs_ahead_                         ? "decode-ahead off"
                                  : journal_ != nullptr                ? "journal on"
                                  : c.needs_dispatch()                 ? "a dispatch was due"
                                  : prs_ahead_key_bytes(c, src) == nullptr ? "no key bytes"
                                                                       : "not decoded ahead / refused";
                std::fprintf(stderr, "[prs-ahead miss] lr %08X src %08X dst %08X: %s\n", lr, src, dst, why);
            }
        }
#endif
        PrsEngine engine(c, stats_);
        const bool finished = engine.run(entry);
        {
            const u64 ns = spent();
            ++stats_.prs_engine_slices;
            stats_.prs_engine_ns += ns;
            stats_.prs_engine_ns_max = std::max(stats_.prs_engine_ns_max, ns);
        }
        if (!finished || entry != kPrsExpand || engine.yielded()) return;
        const u32 written = c.gpr[3];
        stats_.expanded_bytes += written;
        const u64 out_begin = dst, out_end = static_cast<u64>(dst) + written;
        const u64 in_begin = engine.source_start(), in_end = engine.source_end();
        const bool in_place = written != 0u && in_begin < out_end && out_begin < in_end;
        if (in_place) ++stats_.expansions_in_place;
        if (journal_ != nullptr) {
            std::fprintf(journal_, "prs %08X %08X %08X %08X %016llX %d\n", lr, dst, src, written,
                         static_cast<unsigned long long>(fnv1a(c.guest_memory(), dst, written)), in_place ? 1 : 0);
        }
    };
    own(kPrsExpand, "PRS expand", [expand](Cpu& c) { expand(c, kPrsExpand); });
    own(kPrsTokenHead, "PRS expand: token loop resumes", [expand](Cpu& c) { expand(c, kPrsTokenHead); });
    own(kPrsShortLengthHead, "PRS expand: short length resumes",
        [expand](Cpu& c) { expand(c, kPrsShortLengthHead); });
    own(kPrsCopyHead, "PRS expand: match copy resumes", [expand](Cpu& c) { expand(c, kPrsCopyHead); });
}

const u8* GameMemory::prs_ahead_key_bytes(Cpu& c, u32 src) {
    GuestBytes bytes(c);
    return bytes.span(src, 16u);
}

bool GameMemory::serve_prs_ahead(Cpu& c, const PrsDecoded& decoded) {
    const u32 src = c.gpr[3], dst = c.gpr[4];
    const u32 written = static_cast<u32>(decoded.output.size());
    const u64 in_end = u64{src} + decoded.consumed, out_end = u64{dst} + written;
    GuestBytes bytes(c);
    u8* const from = bytes.span(src, decoded.consumed);
    u8* const to = written != 0u ? bytes.span(dst, written) : nullptr;
    // Host memory both, not overlapping (an in-place stream reads bytes its own output rewrote), and the guest's
    // stream byte for byte the one decoded.
    if (from == nullptr || (written != 0u && to == nullptr) || in_end > 0x100000000ull || out_end > 0x100000000ull ||
        (written != 0u && u64{src} < out_end && u64{dst} < in_end) ||
        std::memcmp(from, decoded.stream, decoded.consumed) != 0) {
        ++prs_ahead_refused_;
        return false;
    }
    if (written != 0u) std::memcpy(to, decoded.output.data(), written);
    // The register file PrsEngine::run publishes at the terminator.
    const u32 so = (c.xer & kXerSO) != 0u ? 1u : 0u;
    c.gpr[0] = 0u;
    c.gpr[3] = written;
    c.gpr[4] = dst + written;
    if (decoded.any_match) {
        c.gpr[5] = dst + decoded.r5_from_dst;
        c.gpr[6] = 0u;
        c.ctr = 0u;
    }
    c.gpr[7] = dst;
    c.gpr[8] = src + decoded.consumed;
    c.gpr[9] = decoded.r9;
    c.gpr[10] = decoded.r10;
    c.cr = (c.cr & 0x0FFFFFFFu) | (signed_field(0u, so) << 28);
    c.xer = decoded.ca ? (c.xer | kXerCA) : (c.xer & ~kXerCA);
    ++prs_ahead_served_;
    return true;
}

std::optional<u32> GameMemory::expand_for_native(Cpu& c, u32 src, u32 dst, const PrsDecoded* prepared,
                                                 bool decode_here) {
    const auto started = std::chrono::steady_clock::now();
    const auto done = [&](std::optional<u32> result) {
        const u64 ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
        native_expand_stats_.ns += ns;
        native_expand_stats_.ns_max = std::max(native_expand_stats_.ns_max, ns);
        if (!result && decode_here) ++native_expand_stats_.declined;
        return result;
    };
    GuestBytes bytes(c);
    if (journal_ != nullptr || !bytes.direct()) return done(std::nullopt);
    // The output in guest memory, when it is host memory and does not overlap the consumed stream (serve_prs_ahead's
    // conditions, without the register file: a native caller reads only the count).
    const auto place = [&](const PrsDecoded& d) {
        const u32 written = static_cast<u32>(d.output.size());
        const u64 in_end = u64{src} + d.consumed, out_end = u64{dst} + written;
        u8* const from = bytes.span(src, d.consumed);
        u8* const to = written != 0u ? bytes.span(dst, written) : nullptr;
        if (from == nullptr || (written != 0u && to == nullptr) || in_end > 0x100000000ull ||
            out_end > 0x100000000ull || (written != 0u && u64{src} < out_end && u64{dst} < in_end) ||
            std::memcmp(from, d.stream, d.consumed) != 0)
            return false;
        if (written != 0u) std::memcpy(to, d.output.data(), written);
        return true;
    };
    // 1. Decoded ahead on the host workers.
    if (prs_ahead_) {
        if (const u8* first = bytes.span(src, 16u)) {
            if (const auto decoded = prs_ahead_(first, place)) {
                ++native_expand_stats_.served_ahead;
                stats_.expanded_bytes += static_cast<u32>(decoded->output.size());
                return done(static_cast<u32>(decoded->output.size()));
            }
        }
    }
    // 2. Decoded by the caller off the game thread from a copy of these bytes (Phase 3): placed when every consumed
    // byte still equals the copy.
    if (prepared != nullptr && place(*prepared)) {
        ++native_expand_stats_.served_prepared;
        stats_.expanded_bytes += static_cast<u32>(prepared->output.size());
        return done(static_cast<u32>(prepared->output.size()));
    }
    // 3. Decoded here from the guest's bytes (the source's region bounds what may be read).
    if (!decode_here) return done(std::nullopt);
    const GuestBytes::Run run = bytes.run_at(src);
    if (run.host == nullptr) return done(std::nullopt);
    const u8* stream = run.host + (src - run.base);
    const std::size_t available = static_cast<std::size_t>(run.end - src);
    for (const u32 hint : {0x10000u, 0x100000u, 0x800000u}) {
        // A decline is either an output past the hint (a larger hint helps) or a reason no hint changes (a copy from
        // before the output, a stream ending at the region's end): the next hint, then the guest path.
        PrsDecoded d;
        bool hint_short = false;
        if (!prs_decode_ahead(stream, available, hint, d, &hint_short)) {
            if (!hint_short) break;  // no larger hint changes this decline: the guest path, without decoding again
            continue;
        }
        if (!place(d)) return done(std::nullopt);
        ++native_expand_stats_.decoded_here;
        stats_.expanded_bytes += static_cast<u32>(d.output.size());
        return done(static_cast<u32>(d.output.size()));
    }
    return done(std::nullopt);
}

bool GameMemory::ahead_has_stream(Cpu& c, u32 src) const {
    if (!prs_ahead_ready_ || journal_ != nullptr) return false;
    GuestBytes bytes(c);
    if (!bytes.direct()) return false;
    const u8* const first = bytes.span(src, 16u);
    if (first == nullptr) return false;
    return prs_ahead_ready_(first, [&](const PrsDecoded& d) {
        const u8* const from = bytes.span(src, d.consumed);
        return from != nullptr && std::memcmp(from, d.stream, d.consumed) == 0;
    });
}

bool GameMemory::move_for_native(Cpu& c, u32 dst, u32 src, u32 n) const {
    GuestBytes bytes(c);
    if (journal_ != nullptr || !bytes.direct()) return false;
    if (n == 0u) return true;
    u8* const from = bytes.span(src, n);
    u8* const to = bytes.span(dst, n);
    if (from == nullptr || to == nullptr) return false;
    std::memmove(to, from, n);
    return true;
}

bool GameMemory::snapshot_stream(Cpu& c, u32 src, std::size_t max_bytes, std::vector<u8>& out) const {
    GuestBytes bytes(c);
    if (journal_ != nullptr || !bytes.direct()) return false;
    const GuestBytes::Run run = bytes.run_at(src);
    if (run.host == nullptr) return false;
    const std::size_t length = std::min<std::size_t>(max_bytes, static_cast<std::size_t>(run.end - src));
    const u8* const from = run.host + (src - run.base);
    out.assign(from, from + length);
    return true;
}

}  // namespace gekko
