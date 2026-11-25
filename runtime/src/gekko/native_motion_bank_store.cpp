// The game's motion store as a native registry (SubQ batch B003). Contract: include/gekko/native_motion_bank_store.h.
// Behaviour: recovered/game/motion/README.md and the readable reconstruction recovered/game/motion/store/*.h,
// recovered/game/motion/clip_records/*.h, each proved against its lifted original; this file is the native store over
// native structures, proved against the original words by runtime/tests/motion_bank_store_contract_tests.cpp.

#include "gekko/native_motion_bank_store.h"

#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_guest_call.h"
#include "gekko/native_motion_store.h"
#include "gekko/ppc_ops.h"

namespace gekko {

namespace {

// ---- the owner entries (extract/native-owners.txt) ----------------------------------------------------------------
constexpr u32 kDropLoaded = 0x80412764u;
constexpr u32 kNextRecord = 0x80412868u;
constexpr u32 kQuery = 0x80412E3Cu;
constexpr u32 kHandleOf = 0x8041342Cu;
constexpr u32 kTrack = 0x8041352Cu;
constexpr u32 kFrameRange = 0x80413644u;
constexpr u32 kIdOfName = 0x80413788u;
constexpr u32 kNameOf = 0x80413870u;
constexpr u32 kDataOf = 0x80413970u;
constexpr u32 kUnregister = 0x80413A24u;
constexpr u32 kRegister = 0x804140A8u;
constexpr u32 kRelease = 0x80414290u;
constexpr u32 kAcquire = 0x80414368u;
constexpr u32 kBuild = 0x80414588u;
constexpr u32 kBind = 0x8041ADACu;

const MotionBankStore::Address kAddresses[] = {
    {kDropLoaded, "motion banks: drop loaded clips (0x80412764)"},
    {kNextRecord, "motion banks: next record (0x80412868)"},
    {kQuery, "motion banks: record query (0x80412E3C)"},
    {kHandleOf, "motion banks: handle of (0x8041342C)"},
    {kTrack, "motion banks: track cursor (0x8041352C)"},
    {kFrameRange, "motion banks: frame range (0x80413644)"},
    {kIdOfName, "motion banks: id of name (0x80413788)"},
    {kNameOf, "motion banks: name of (0x80413870)"},
    {kDataOf, "motion banks: data of (0x80413970)"},
    {kUnregister, "motion banks: unregister (0x80413A24)"},
    {kRegister, "motion banks: register (0x804140A8)"},
    {kRelease, "motion banks: release (0x80414290)"},
    {kAcquire, "motion banks: acquire (0x80414368)"},
    {kBuild, "motion banks: build pool bank (0x80414588)"},
    {kBind, "motion banks: slot bind (0x8041ADAC)"},
    // The rows of the symbol map that start inside those bodies: unreachable `b` words MWCC leaves after a loop or a
    // call (recovered/game/motion/README.md, "Files"). No code enters them; an owner that stops keeps the generator from
    // translating them (they would run the original words of an owned body). Entering one is a fault.
    {0x80412978u, "motion banks: unreachable branch in 0x80412868 (0x80412978)"},
    {0x80412E20u, "motion banks: unreachable branches in 0x80412868 (0x80412E20)"},
    {0x80412F4Cu, "motion banks: unreachable branch in 0x80412E3C (0x80412F4C)"},
    {0x80413410u, "motion banks: unreachable branches in 0x80412E3C (0x80413410)"},
    {0x8041346Cu, "motion banks: unreachable branch in 0x8041342C (0x8041346C)"},
    {0x80413514u, "motion banks: unreachable branch in 0x8041342C (0x80413514)"},
    {0x80413564u, "motion banks: unreachable branch in 0x8041352C (0x80413564)"},
    {0x804136C8u, "motion banks: unreachable branch in 0x80413644 (0x804136C8)"},
    {0x80413770u, "motion banks: unreachable branch in 0x80413644 (0x80413770)"},
    {0x80413B9Cu, "motion banks: unreachable branch in 0x80413A24 (0x80413B9C)"},
    {0x804142ACu, "motion banks: unreachable branch in 0x80414290 (0x804142AC)"},
    {0x80414354u, "motion banks: unreachable branch in 0x80414290 (0x80414354)"},
};
constexpr std::size_t kFunctionCount = 15u;  // kAddresses[0 .. 15) are the functions, the rest the unreachable rows

// ---- the guest functions the store calls where the original calls them ---------------------------------------------
constexpr u32 kAlloc = 0x80412184u;         // (1, bytes) -> a zeroed game-heap block or 0 (RwCalloc through the hook)
constexpr u32 kFree = 0x8041214Cu;          // (block); also the destroy word of every pool bank
constexpr u32 kPoolAlloc = 0x804223B8u;     // (bytes) -> a pool address or 0
constexpr u32 kPoolFree = 0x80422350u;      // (pool block)
constexpr u32 kHandleFor = 0x80411710u;     // (clip data + 0x14) -> its handle
constexpr u32 kBindChannels = 0x8041C1C0u;  // (object, slot index, clip id) -> 0 on failure

// ---- the guest words that stay guest (read by translated code) -----------------------------------------------------
constexpr u32 kNameBuffer = 0x805E2260u;
constexpr u32 kDefaultsA = 0x805E2360u;  // the query's default window
constexpr u32 kDefaultsB = 0x805E2370u;  // next_record's
// The state block (published for checkpoints and state reports only).
constexpr u32 kStateHead = 0x805F2458u;
constexpr u32 kStateLastFound = 0x805F245Cu;
constexpr u32 kStateHeld = 0x805F2460u;
constexpr u32 kStateTrack = 0x805F2468u;
constexpr u32 kStateTrackReady = 0x805F246Cu;
constexpr u32 kStateEventsA = 0x805F2470u;
constexpr u32 kStateEventsB = 0x805F2474u;

// ---- SCAFFOLDING (B003): the original's return addresses, as the guest stack keeps them (frame link words and the
// LR a guest callee saves). Values only: nothing ever executes there. Deleted with the frame residue (see the header).
constexpr u32 kLinkAcquireAlloc = 0x804144C4u;       // liveness-roots: value-only
constexpr u32 kLinkReleaseFree = 0x8041432Cu;        // liveness-roots: value-only
constexpr u32 kLinkBuildAlloc = 0x804145B0u;         // liveness-roots: value-only
constexpr u32 kLinkBuildPoolOwns = 0x804146BCu;      // liveness-roots: value-only
constexpr u32 kLinkBuildFreeOwns = 0x804146D4u;      // liveness-roots: value-only
constexpr u32 kLinkBuildPool = 0x80414880u;          // liveness-roots: value-only
constexpr u32 kLinkBuildFree = 0x80414898u;          // liveness-roots: value-only
constexpr u32 kLinkBuildDestroyData = 0x804149CCu;   // liveness-roots: value-only
constexpr u32 kLinkBuildFreePack = 0x804149D4u;      // liveness-roots: value-only
constexpr u32 kLinkBuildDestroyPack = 0x804149ECu;   // liveness-roots: value-only
constexpr u32 kLinkRegisterHandle = 0x8041424Cu;     // liveness-roots: value-only
constexpr u32 kLinkRegisterBuild = 0x80414278u;      // liveness-roots: value-only
constexpr u32 kLinkUnregisterPool = 0x80413A94u;     // liveness-roots: value-only
constexpr u32 kLinkUnregisterNode = 0x80413AA4u;     // liveness-roots: value-only
constexpr u32 kLinkUnregisterData = 0x80413B78u;     // liveness-roots: value-only
constexpr u32 kLinkUnregisterBank = 0x80413B80u;     // liveness-roots: value-only
constexpr u32 kLinkUnregisterDestroy = 0x80413B9Cu;  // liveness-roots: value-only
constexpr u32 kLinkDropFree = 0x80412798u;           // liveness-roots: value-only
constexpr u32 kLinkHandleAcquire = 0x80413448u;      // liveness-roots: value-only
constexpr u32 kLinkHandleFree = 0x804134E8u;         // liveness-roots: value-only
constexpr u32 kLinkRangeAcquire = 0x80413668u;       // liveness-roots: value-only
constexpr u32 kLinkRangeFree = 0x80413744u;          // liveness-roots: value-only
constexpr u32 kLinkNameAcquire = 0x8041388Cu;        // liveness-roots: value-only
constexpr u32 kLinkFindName = 0x804137ECu;           // liveness-roots: value-only
constexpr u32 kLinkQueryAcquire = 0x80412E7Cu;       // liveness-roots: value-only
constexpr u32 kLinkQueryRange = 0x80412F30u;         // liveness-roots: value-only
constexpr u32 kLinkQueryHeld = 0x80412FC4u;          // liveness-roots: value-only
constexpr u32 kLinkQueryFabsWidth = 0x804130E4u;     // liveness-roots: value-only
constexpr u32 kLinkQueryFabsDelta = 0x8041310Cu;     // liveness-roots: value-only
constexpr u32 kLinkQueryMiss = 0x804133DCu;          // liveness-roots: value-only
constexpr u32 kLinkNextAcquire = 0x804128A8u;        // liveness-roots: value-only
constexpr u32 kLinkNextRange = 0x8041295Cu;          // liveness-roots: value-only
constexpr u32 kLinkNextHeld = 0x804129F0u;           // liveness-roots: value-only
constexpr u32 kLinkNextFabsWidth = 0x80412B00u;      // liveness-roots: value-only
constexpr u32 kLinkNextFabsDelta = 0x80412B28u;      // liveness-roots: value-only
constexpr u32 kLinkNextMiss = 0x80412DECu;           // liveness-roots: value-only
constexpr u32 kLinkBindAcquire = 0x8041ADD8u;        // liveness-roots: value-only
constexpr u32 kLinkBindReleaseOld = 0x8041ADECu;     // liveness-roots: value-only
constexpr u32 kLinkBindMemset = 0x8041ADFCu;         // liveness-roots: value-only
constexpr u32 kLinkBindChannels = 0x8041AEA4u;       // liveness-roots: value-only
constexpr u32 kLinkBindReleaseNew = 0x8041AEB4u;     // liveness-roots: value-only
constexpr u32 kLinkBindQueryInverted = 0x8041AEECu;  // liveness-roots: value-only
constexpr u32 kLinkBindQueryKind2 = 0x8041AF2Cu;     // liveness-roots: value-only
constexpr u32 kLinkBindQueryKind1 = 0x8041AF74u;     // liveness-roots: value-only

constexpr std::size_t kNone = ~std::size_t{0};
constexpr u32 kListBound = 0x10000u;  // a guest list longer than this is not a list (import only)

struct Stopped {};  // a guest call stopped the run: the owner returns at once

using Bank = MotionBankStore::Bank;
using Node = MotionBankStore::Node;
using Record = MotionBankStore::Record;

// x rounded up to 32 by its signed remainder, as the original rounds a length.
u32 round32(u32 x) {
    const s32 rem = static_cast<s32>(x) % 32;
    return rem == 0 ? x : x + static_cast<u32>(32 - rem);
}

// SCAFFOLDING (B003): an original function's frame as the guest stack sees it - the back chain, the link word in the
// caller's frame, the caller's registers the prologue saves - and the register image the original holds, which a
// guest callee saves into its own frame. Write-only (see the header).
struct Frame {
    Cpu& c;
    u32 entry_sp;
    u32 sp;
    u32 r[32];
    Frame(Cpu& cpu, u32 caller_sp, const u32* caller_image, u32 size, u32 save_first, u32 save_at, u32 return_address)
        : c(cpu), entry_sp(caller_sp), sp(caller_sp - size) {
        std::memcpy(r, caller_image, sizeof(r));
        c.mem_write32(sp, entry_sp);
        c.mem_write32(entry_sp + 4u, return_address);
        for (u32 k = save_first; k < 32u; ++k) c.mem_write32(sp + save_at + 4u * (k - save_first), caller_image[k]);
    }
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
};

// The entry image: the caller's registers.
struct EntryImage {
    u32 r[32];
    explicit EntryImage(const Cpu& c) { std::memcpy(r, c.gpr, sizeof(r)); }
};

// The stores installed in this process (one per System; the tests make several).
std::mutex g_instances_mutex;
struct Instance {
    const Cpu* cpu;
    const MotionBankStore* store;
};
Instance g_instances[8] = {};

}  // namespace

// ---- the native bodies -------------------------------------------------------------------------------------------------
struct MotionBankStore::Impl {
    MotionBankStore& s;
    Cpu& c;

    // A guest function, called from the original's place: r1 at the original's frame, its register image in r14..r31,
    // `link` as the return address. The caller's registers are back on return.
    u32 call(Frame& f, u32 target, u32 link, std::initializer_list<u32> args) {
        u32 saved[18];
        std::memcpy(saved, &c.gpr[14], sizeof(saved));
        std::memcpy(&c.gpr[14], &f.r[14], sizeof(saved));
        const u32 sp_before = c.gpr[1];
        // guest_call builds a 16-byte frame below r1 for the callee's link word; starting 0x10 higher puts the callee's
        // r1 exactly at the original frame (native_mtn_pose.cpp `call` does the same).
        const u32 slot = f.sp & ~0xFu;
        const u32 slot_word = c.mem_read32(slot);
        c.gpr[1] = f.sp + 0x10u;
        u32 result = 0;
        const bool ok = guest_call(c, target, link, args, result);
        c.gpr[1] = sp_before;
        std::memcpy(&c.gpr[14], saved, sizeof(saved));
        if (!ok) throw Stopped{};
        if (slot != f.sp) c.mem_write32(slot, slot_word);
        c.mem_write32(f.sp, f.entry_sp);  // guest_call's back chain word lands on the original's
        return result;
    }

    void stop(const std::string& why) {
        c.request_stop(StopReason::ExecutionFault, "motion banks (B003): " + why);
        throw Stopped{};
    }

    // ---- the registry ----------------------------------------------------------------------------------------------
    // The first bank whose range holds key decides (first > key unsigned, key < first + count signed: equal for u16).
    std::size_t bank_of(u32 key) const {
        for (std::size_t i = 0; i < s.banks_.size(); ++i) {
            const Bank& b = s.banks_[i];
            if (b.first > key) continue;
            if (!(static_cast<s32>(key) < static_cast<s32>(b.first) + static_cast<s32>(b.count))) continue;
            return i;
        }
        return kNone;
    }
    std::size_t index_of(u32 header) const {
        for (std::size_t i = 0; i < s.banks_.size(); ++i)
            if (s.banks_[i].header == header) return i;
        return kNone;
    }
    static std::size_t node_of(const Bank& b, u32 key) {
        for (std::size_t k = 0; k < b.nodes.size(); ++k)
            if (b.nodes[k].id == key) return k;
        return kNone;
    }
    // The bank at a guest address taken as it is in guest memory (a registered pack; a restored checkpoint). A pool
    // bank's records and node list are read too. False: its node list does not end.
    bool adopt(u32 header, Bank& b) {
        b = Bank{};
        b.header = header;
        b.first = static_cast<u16>(c.mem_read16(header + 0x0u));
        b.count = static_cast<u16>(c.mem_read16(header + 0x2u));
        b.owns = static_cast<u8>(c.mem_read8(header + 0x4u));
        b.byte5 = static_cast<u8>(c.mem_read8(header + 0x5u));
        b.pooled = static_cast<u8>(c.mem_read8(header + 0x6u));
        b.byte7 = static_cast<u8>(c.mem_read8(header + 0x7u));
        b.destroy = c.mem_read32(header + 0x8u);
        b.table = c.mem_read32(header + 0x10u);
        if (b.pooled != 0u) {
            b.pool = c.mem_read32(header + 0x14u);
            b.records.resize(b.count);
            for (u32 i = 0; i < b.count; ++i) {
                const u32 r = header + 0x1Cu + 24u * i;
                b.records[i] = Record{c.mem_read32(r), c.mem_read32(r + 4u), c.mem_read32(r + 8u), c.mem_read32(r + 0xCu),
                                      c.mem_read32(r + 0x10u), c.mem_read32(r + 0x14u)};
            }
            u32 n = c.mem_read32(header + 0x18u);
            for (u32 guard = 0; n != 0u; n = c.mem_read32(n + 0x10u)) {
                if (++guard > kListBound) return false;
                b.nodes.push_back(Node{n, static_cast<u16>(c.mem_read16(n)), static_cast<s16>(c.mem_read16(n + 2u)),
                                       c.mem_read32(n + 4u), c.mem_read32(n + 8u), c.mem_read32(n + 0xCu)});
            }
        }
        return true;
    }
    // A link word of the list: the bank before `index` (its +0xC) or, for the first bank, the head (native).
    void link_before(std::size_t index, u32 value) {
        if (index > 0u) c.mem_write32(s.banks_[index - 1u].header + 0xCu, value);
    }

    // The held ids inside bank b's range are cleared (held < first unsigned, held < first + count signed).
    void clear_held(const Bank& b) {
        for (u16& held : s.held_) {
            if (held < b.first) continue;
            if (!(static_cast<s32>(held) < static_cast<s32>(b.first) + static_cast<s32>(b.count))) continue;
            held = 0u;
        }
    }

    // ---- 0x80414368 acquire: clip `id` resident, one user more ---------------------------------------------------
    void acquire(const u32* image, u32 caller_sp, u32 return_address, u32 id) {
        Frame f(c, caller_sp, image, 0x40u, 20u, 16u, return_address);
        ++s.stats_.acquires;
        const u32 key = id & 0xFFFFu;
        if (key == 0u) return;
        std::size_t bi = bank_of(key);
        if (bi == kNone) return;
        Bank& b = s.banks_[bi];
        if (b.pooled == 0u) return;
        u32 last = 0u;
        for (Node& n : b.nodes) {
            if (n.id == key) {
                n.uses = static_cast<s16>(n.uses + 1);
                c.mem_write16(n.block + 2u, static_cast<u16>(n.uses));
                return;
            }
            last = n.block;
        }
        const u32 index = key - b.first;
        const Record r = b.records[index];
        const u32 header = b.header;
        const u32 name_skew = r.name & 31u, data_skew = r.data & 31u;
        const u32 name_span = round32(name_skew + r.name_bytes);
        const u32 data_span = round32(data_skew + r.data_bytes);
        const u32 events_skew = r.events != 0u ? r.events & 31u : 0u;
        const u32 events_span = round32(r.events_bytes != 0u ? events_skew + r.events_bytes : 0u);
        f.r[21] = id;
        f.r[22] = events_span;
        f.r[23] = data_span;
        f.r[24] = data_skew;
        f.r[25] = name_span;
        f.r[26] = name_skew;
        f.r[27] = header + 0x1Cu + 24u * index;
        f.r[28] = last;
        f.r[29] = header;
        f.r[30] = events_skew;
        const u32 n = call(f, kAlloc, kLinkAcquireAlloc, {1u, name_span + data_span + events_span + 0x40u});
        if (n == 0u) {
            ++s.stats_.load_failures;
            return;
        }
        // The node is the list's new end only if its link word came zeroed: the hook allocator is RwCalloc (README.md
        // section 4 item 8, settled from the allocator's code). Anything else is a path this store does not model.
        if (c.mem_read32(n + 0x10u) != 0u) stop("the allocator returned a node whose link word is not zero");
        bi = index_of(header);
        if (bi == kNone) stop("a bank left the registry during an allocation");
        c.mem_write16(n + 2u, 1u);
        c.mem_write16(n + 0u, static_cast<u16>(id));
        Node node{n, static_cast<u16>(key), 1, 0u, 0u, 0u};
        u32 at = n + 0x20u;
        if ((at & 31u) != 0u) at += 32u - (at & 31u);
        transfer(at, r.name - name_skew, name_span);
        node.name = at + name_skew;
        c.mem_write32(n + 4u, node.name);
        at += name_span;
        transfer(at, r.data - data_skew, data_span);
        node.data = at + data_skew;
        c.mem_write32(n + 8u, node.data);
        if (r.events != 0u) {
            at += data_span;
            transfer(at, r.events - events_skew, events_span);
            node.events = at + events_skew;
            c.mem_write32(n + 0xCu, node.events);
        } else {
            node.events = c.mem_read32(n + 0xCu);  // not written: what the allocation held
        }
        Bank& owner = s.banks_[bi];
        if (last != 0u) c.mem_write32(last + 0x10u, n);
        else c.mem_write32(owner.header + 0x18u, n);
        owner.nodes.push_back(node);
        ++s.stats_.loads;
    }
    void transfer(u32 to, u32 pool_from, u32 bytes) {
        if (!s.pool_.fetch_span(c, to, pool_from, bytes)) throw Stopped{};
        if (c.needs_dispatch()) ++s.stats_.deferred_interrupts;
    }

    // ---- release of one user (0x80414290 and its inline copies), in frame f, the free called back to `link` ------
    void release_id(Frame& f, u32 id, u32 link) {
        const u32 key = id & 0xFFFFu;
        if (key == 0u) return;
        const std::size_t bi = bank_of(key);
        if (bi == kNone) return;
        Bank& b = s.banks_[bi];
        if (b.pooled == 0u) return;
        const std::size_t k = node_of(b, key);
        if (k == kNone) return;
        Node& n = b.nodes[k];
        n.uses = static_cast<s16>(n.uses - 1);
        c.mem_write16(n.block + 2u, static_cast<u16>(n.uses));
        if (n.uses != 0) return;
        const u32 next = k + 1u < b.nodes.size() ? b.nodes[k + 1u].block : 0u;
        if (k > 0u) c.mem_write32(b.nodes[k - 1u].block + 0x10u, next);
        else c.mem_write32(b.header + 0x18u, next);
        const u32 block = n.block;
        b.nodes.erase(b.nodes.begin() + static_cast<std::ptrdiff_t>(k));
        ++s.stats_.frees;
        call(f, kFree, link, {block});
    }
    void release(const u32* image, u32 caller_sp, u32 return_address, u32 id) {
        Frame f(c, caller_sp, image, 0x10u, 32u, 0u, return_address);
        release_id(f, id, kLinkReleaseFree);
    }

    // ---- lookups ---------------------------------------------------------------------------------------------------
    // The clip table entry of key: a resident bank's table + index * 12 (0 when that is 0), a loaded node's +4 (as an
    // entry), or 0. Returns the entry's three words through name / data / events when it exists.
    bool entry_of(u32 key, u32* name, u32* data, u32* events) {
        if (s.banks_.empty()) return false;
        const std::size_t bi = bank_of(key);
        if (bi == kNone) return false;
        const Bank& b = s.banks_[bi];
        if (b.pooled == 0u) {
            const u32 entry = b.table + (key - b.first) * 12u;
            if (entry == 0u) return false;
            if (name) *name = c.mem_read32(entry);
            if (data) *data = c.mem_read32(entry + 4u);
            if (events) *events = c.mem_read32(entry + 8u);
            return true;
        }
        const std::size_t k = node_of(b, key);
        if (k == kNone) return false;
        const Node& n = b.nodes[k];
        if (name) *name = n.name;
        if (data) *data = n.data;
        if (events) *events = n.events;
        return true;
    }
    // 0x80413970 (a leaf: no frame)
    u32 data_of(u32 id) {
        u32 data = 0u;
        return entry_of(id & 0xFFFFu, nullptr, &data, nullptr) ? data : 0u;
    }
    // 0x8041342C
    u32 handle_of(const u32* image, u32 caller_sp, u32 return_address, u32 id) {
        Frame f(c, caller_sp, image, 0x10u, 30u, 8u, return_address);
        f.r[30] = id;
        f.r[31] = 0u;
        acquire(f.r, f.sp, kLinkHandleAcquire, id);
        const u32 data = data_of(id);
        const u32 handle = data != 0u ? c.mem_read32(data + 4u) : 0u;
        f.r[31] = handle;
        release_id(f, id, kLinkHandleFree);
        return handle;
    }
    // 0x80413644 (out, id) -> found: out[0] = out[3] = first frame, out[1] = last frame, as floats
    u32 frame_range(const u32* image, u32 caller_sp, u32 return_address, u32 out, u32 id) {
        Frame f(c, caller_sp, image, 0x30u, 29u, 36u, return_address);
        f.r[29] = out;
        f.r[30] = id;
        f.r[31] = 0u;
        acquire(f.r, f.sp, kLinkRangeAcquire, id);
        const u32 data = data_of(id);
        if (data != 0u) {
            const u32 first = c.mem_read16(data + 0x10u);
            // SCAFFOLDING (B003): the original's integer-to-float conversion words in its frame.
            c.mem_write32(f.sp + 8u, 0x43300000u);
            c.mem_write32(f.sp + 12u, first);
            c.mem_write32(f.sp + 16u, 0x43300000u);
            const u32 first_bits = float_to_bits(static_cast<float>(first));
            c.mem_write32(out + 0xCu, first_bits);
            c.mem_write32(out + 0x0u, first_bits);
            const u32 last = c.mem_read16(data + 0x12u);
            c.mem_write32(f.sp + 20u, last);
            c.mem_write32(out + 0x4u, float_to_bits(static_cast<float>(last)));
            f.r[31] = 1u;
        }
        release_id(f, id, kLinkRangeFree);
        return f.r[31];
    }
    // 0x80413870: the name copied to 0x805E2260; the acquire is never released (quirk)
    u32 name_of(const u32* image, u32 caller_sp, u32 return_address, u32 id) {
        Frame f(c, caller_sp, image, 0x10u, 30u, 8u, return_address);
        f.r[30] = id;
        f.r[31] = 0u;
        acquire(f.r, f.sp, kLinkNameAcquire, id);
        u32 name = 0u;
        if (!entry_of(id & 0xFFFFu, &name, nullptr, nullptr) || name == 0u) return 0u;
        for (u32 i = 0;; ++i) {  // strcpy
            const u32 byte = c.mem_read8(name + i);
            c.mem_write8(kNameBuffer + i, static_cast<u8>(byte));
            if (byte == 0u) break;
        }
        return kNameBuffer;
    }
    bool same_string(u32 a, u32 b) {  // strcmp(a, b) == 0
        for (u32 i = 0;; ++i) {
            const u32 x = c.mem_read8(a + i), y = c.mem_read8(b + i);
            if (x != y) return false;
            if (x == 0u) return true;
        }
    }
    // 0x80413788: banks in list order, clips in id order; pool banks through name_of (each clip passed stays loaded)
    u32 id_of_name(const u32* image, u32 caller_sp, u32 return_address, u32 name) {
        Frame f(c, caller_sp, image, 0x20u, 28u, 16u, return_address);
        f.r[28] = name;
        if (name == 0u || s.banks_.empty()) return 0u;
        for (std::size_t bi = 0; bi < s.banks_.size(); ++bi) {
            f.r[29] = s.banks_[bi].header;
            f.r[30] = 0u;
            f.r[31] = 0u;
            for (u32 i = 0; static_cast<s32>(i) < static_cast<s32>(s.banks_[bi].count); ++i) {
                f.r[30] = i;
                f.r[31] = 12u * i;
                const Bank& b = s.banks_[bi];
                const u32 id = (b.first + i) & 0xFFFFu;
                const u32 other = b.pooled != 0u ? name_of(f.r, f.sp, kLinkFindName, id) : c.mem_read32(b.table + 12u * i);
                if (same_string(name, other)) return (i + s.banks_[bi].first) & 0xFFFFu;
            }
        }
        return 0u;
    }

    // 0x8041352C (a leaf): the track cursor
    u32 track(u32 id) {
        if (static_cast<s8>(s.track_ready_) == 0) {
            s.track_ = 0u;
            s.track_ready_ = 1u;
        }
        const u32 key = id & 0xFFFFu;
        if (key != 0u) {
            u32 data = 0u;
            s.track_ = entry_of(key, nullptr, &data, nullptr) ? data + 0x34u : 0u;
        } else if (s.track_ != 0u) {
            const u32 cursor = s.track_;
            s.track_ = static_cast<s8>(c.mem_read8(cursor + 7u)) != 0 ? cursor + c.mem_read32(cursor) : 0u;
        }
        return s.track_;
    }

    // ---- 0x80412E3C query / 0x80412868 next_record --------------------------------------------------------------------
    // Float compares go through the guest's own ordered compare (fcmpo: CR0 and FPSCR[FPCC]; VXVC for a NaN, VXSNAN for a
    // signalling one) in the original's order and operand order, so the FPSCR the next code sees is the original's.
    static u64 dbl(u32 single) { return single_to_double_bits(single); }
    u32 fcmp(u64 a, u64 b) {
        ppc::fp_compare(c, 0u, a, b, true);
        return (c.fpscr >> 12) & 0xFu;  // 8 lt, 4 gt, 2 eq, 1 unordered
    }
    bool lt(u64 a, u64 b) { return (fcmp(a, b) & 8u) != 0u; }
    bool gt(u64 a, u64 b) { return (fcmp(a, b) & 4u) != 0u; }
    bool le(u64 a, u64 b) { return (fcmp(a, b) & 0xAu) != 0u; }
    bool ge(u64 a, u64 b) { return (fcmp(a, b) & 0x6u) != 0u; }
    u64 fabs_(Frame& f, u64 x, u32 link) {
        // 0x8000A980 has a frame: its back chain and its link word in the caller's frame (SCAFFOLDING (B003)).
        c.mem_write32(f.sp - 0x10u, f.sp);
        c.mem_write32(f.sp + 4u, link);
        return ppc::fps_round(c, x & 0x7FFFFFFFFFFFFFFFull);
    }
    u64 sub(u64 a, u64 b) { return ppc::fps_sub(c, a, b); }
    u64 add(u64 a, u64 b) { return ppc::fps_add(c, a, b); }
    u64 rf(u32 a) { return dbl(c.mem_read32(a)); }

    // The probe's time window against the record's time (clip_records/query.h `window`).
    bool window(Frame& f, u32 record, u32 probe, u32 exact, u32 link_width, u32 link_delta) {
        const u64 zero = 0u;
        if (le(fabs_(f, rf(probe + 8u), link_width), zero)) {
            if (exact == 0u) return false;
            const u64 d = fabs_(f, sub(rf(probe + 0xCu), rf(record + 4u)), link_delta);
            return lt(d, dbl(0x38D1B717u));  // 1e-4
        }
        const u64 w = rf(probe + 8u);
        if (gt(w, zero)) {
            const u64 hi = rf(probe + 4u), lo = rf(probe);
            if (ge(w, sub(hi, lo))) {
                const u64 p = rf(record + 4u);
                return le(lo, p) && ge(hi, p);
            }
            const u64 t = rf(probe + 0xCu), back = sub(t, w);
            if (lt(back, lo)) {
                const u64 p = rf(record + 4u);
                if (le(lo, p) && ge(t, p)) return true;
                return lt(sub(hi, sub(lo, back)), p) && ge(hi, p);
            }
            const u64 p = rf(record + 4u);
            return lt(back, p) && ge(t, p);
        }
        const u64 lo = rf(probe), hi = rf(probe + 4u);
        if (le(w, sub(lo, hi))) {
            const u64 p = rf(record + 4u);
            return le(lo, p) && ge(hi, p);
        }
        const u64 t = rf(probe + 0xCu), ahead = sub(t, w);
        if (gt(ahead, hi)) {
            const u64 p = rf(record + 4u);
            if (le(t, p) && ge(hi, p)) return true;
            const u64 beyond = sub(ahead, hi);
            if (!le(lo, p)) return false;
            return gt(add(lo, beyond), p);
        }
        const u64 p = rf(record + 4u);
        return le(t, p) && gt(ahead, p);
    }
    // The window of the 0x8000 kind: the probe's time inside [record +4, record +8] (a negative bound: the default).
    bool inside(u32 record, u32 probe, u32 defaults) {
        const u64 zero = 0u;
        u64 lo = rf(record + 4u);
        if (lt(lo, zero)) lo = rf(defaults);
        u64 hi = rf(record + 8u);
        if (lt(hi, zero)) hi = rf(defaults + 4u);
        const u64 t = rf(probe + 0xCu);
        return ge(t, lo) && le(t, hi);
    }
    // memcpy 0x800054F4 (memmove semantics, byte by byte)
    void move(u32 to, u32 from, u32 bytes) {
        if (from >= to) {
            for (u32 i = 0; i < bytes; ++i) c.mem_write8(to + i, c.mem_read8(from + i));
        } else {
            for (u32 i = bytes; i-- > 0u;) c.mem_write8(to + i, c.mem_read8(from + i));
        }
    }

    // `second` selects next_record (held 0x805F2462, cursor 0x805F2474, defaults 0x805E2370); its `kind_out` is the
    // query's `flags`.
    u32 records(const u32* image, u32 caller_sp, u32 return_address, bool second, u32 id, u32 flags_or_kind_out,
                u32 probe, u32 out, u32 exact) {
        Frame f(c, caller_sp, image, 0x30u, second ? 24u : 22u, second ? 16u : 8u, return_address);
        ++s.stats_.queries;
        u32& cursor = second ? s.events_b_ : s.events_a_;
        u16& held = s.held_[second ? 1 : 0];
        const u32 defaults = second ? kDefaultsB : kDefaultsA;
        f.r[31] = id;
        f.r[26] = flags_or_kind_out;
        f.r[27] = probe;
        f.r[28] = out;
        f.r[29] = exact;
        f.r[30] = 0u;
        s.last_found_ = 0u;
        if ((id & 0xFFFFu) != 0u) {
            cursor = 0u;
            acquire(f.r, f.sp, second ? kLinkNextAcquire : kLinkQueryAcquire, id);
            // The entry (r25): a resident table's entry or a loaded node's +4. Its +8 is read AFTER the frame range,
            // through that address: when the frame range's release frees the node (a use count that wrapped), the
            // original reads the freed block, and so does this.
            const u32 entry = entry_of(id & 0xFFFFu, nullptr, nullptr, nullptr) ? entry_address(id & 0xFFFFu) : 0u;
            f.r[25] = entry;
            if (entry != 0u) {
                frame_range(f.r, f.sp, second ? kLinkNextRange : kLinkQueryRange, defaults, id);
                const u32 before = held;
                const u32 events = c.mem_read32(entry + 8u);
                cursor = events;
                if (before != 0u) release_id(f, before, second ? kLinkNextHeld : kLinkQueryHeld);
                held = static_cast<u16>(id);
            }
        }
        u32 record = cursor;
        if (record == 0u) return 0u;
        f.r[31] = record;
        f.r[25] = defaults;
        if (!second) {
            f.r[23] = flags_or_kind_out & 0xFFFFu;
            f.r[24] = flags_or_kind_out & 0x8000u;
            f.r[26] = flags_or_kind_out & 0x4000u;
        }
        const u32 kind_out = flags_or_kind_out;
        for (;;) {
            const u32 length = c.mem_read16(record + 2u);
            if (static_cast<s16>(length) == -1) break;
            u32 kind;
            bool candidate;
            if (second) {
                c.mem_write16(kind_out, c.mem_read16(record));
                kind = c.mem_read16(kind_out);
                candidate = true;
            } else {
                kind = flags_or_kind_out;
                candidate = c.mem_read16(record) == (flags_or_kind_out & 0xFFFFu);
            }
            if (candidate) {
                u32 header = 0u;
                bool taken = false, stop_walk = false;
                if ((kind & 0x8000u) != 0u) {
                    if (probe == 0u) stop_walk = true;
                    else if (inside(record, probe, defaults)) { taken = true; header = 12u; }
                } else if ((kind & 0x4000u) == 0u) {
                    taken = true;
                    header = 4u;
                } else {
                    if (probe == 0u) stop_walk = true;
                    else {
                        f.r[second ? 24 : 22] = 0u;
                        if (window(f, record, probe, exact, second ? kLinkNextFabsWidth : kLinkQueryFabsWidth,
                                   second ? kLinkNextFabsDelta : kLinkQueryFabsDelta)) {
                            f.r[second ? 24 : 22] = 1u;
                            taken = true;
                            header = 8u;
                        }
                    }
                }
                if (stop_walk) break;
                if (taken) {
                    const u32 at = cursor;
                    if (second) {
                        c.mem_write32(out, 0u);
                        if (c.mem_read16(record + 2u) != header) c.mem_write32(out, at + header);
                    } else {
                        const u32 bytes = header == 8u ? c.mem_read16(record + 2u) : length;
                        if (bytes != header && out != 0u) move(out, at + header, bytes - header);
                    }
                    s.last_found_ = cursor;
                    cursor = cursor + c.mem_read16(record + 2u);
                    return 1u;
                }
            }
            record = cursor + c.mem_read16(record + 2u);
            cursor = record;
            f.r[31] = record;
        }
        const u32 before = held;
        cursor = 0u;
        if (before != 0u) release_id(f, before, second ? kLinkNextMiss : kLinkQueryMiss);
        held = 0u;
        return 0u;
    }
    // The address of key's entry as the original holds it in r25 (resident: table + index * 12; pool: the node + 4).
    u32 entry_address(u32 key) {
        const std::size_t bi = bank_of(key);
        if (bi == kNone) return 0u;
        const Bank& b = s.banks_[bi];
        if (b.pooled == 0u) return b.table + (key - b.first) * 12u;
        const std::size_t k = node_of(b, key);
        return k == kNone ? 0u : b.nodes[k].block + 4u;
    }

    // ---- 0x804140A8 register / 0x80414588 build -------------------------------------------------------------------
    u32 register_bank(const u32* image, u32 caller_sp, u32 return_address, u32 pack) {
        Frame f(c, caller_sp, image, 0x20u, 29u, 20u, return_address);
        f.r[31] = pack;
        if (index_of(pack) != kNone) return 0u;
        const u32 first = c.mem_read16(pack + 0x0u);
        if (first != 0u) {
            for (const Bank& b : s.banks_) {
                const u32 start = b.first;
                if (start > first && static_cast<s32>(start - c.mem_read16(pack + 0x2u)) < static_cast<s32>(first)) return 0u;
                if (start < first && static_cast<s32>(start + b.count) > static_cast<s32>(first)) return 0u;
            }
        } else if (s.banks_.empty()) {
            c.mem_write16(pack + 0x0u, 1u);
        } else {
            u32 candidate = 1u, end = 0u;
            for (;;) {
                bool inside_bank = false;
                for (const Bank& b : s.banks_) {
                    const u32 start = b.first;
                    if (static_cast<s32>(start) > static_cast<s32>(candidate)) continue;
                    const u32 stop_at = start + b.count;
                    if (!(static_cast<s32>(stop_at) > static_cast<s32>(candidate))) continue;
                    end = (stop_at - 1u) & 0xFFFFu;
                    candidate = end;
                    inside_bank = true;
                    break;
                }
                if (!inside_bank && !(static_cast<s32>(candidate - end) < static_cast<s32>(c.mem_read16(pack + 0x2u)))) {
                    c.mem_write16(pack + 0x0u, static_cast<u16>(end + 1u));
                    break;
                }
                if (candidate == 0xFFFFu) return 0u;
                ++candidate;
            }
        }
        // Appended at the end of the list. The pack's own link word ends the list only when it is zero (a loaded pack's
        // is); a pack that points on is a list this store does not model.
        if (c.mem_read32(pack + 0xCu) != 0u) stop("a pack registered with a non-zero link word");
        Bank adopted;
        if (!adopt(pack, adopted)) stop("a pack registered as a pool bank whose node list does not end");
        if (!s.banks_.empty()) c.mem_write32(s.banks_.back().header + 0xCu, pack);
        s.banks_.push_back(std::move(adopted));
        ++s.stats_.registered;
        u32 i = 0u;
        for (; static_cast<s32>(i) < static_cast<s32>(c.mem_read16(pack + 0x2u)); ++i) {
            f.r[29] = i;
            f.r[30] = 12u * i;
            const u32 handle = call(f, kHandleFor, kLinkRegisterHandle,
                                    {c.mem_read32(c.mem_read32(pack + 0x10u) + 12u * i + 4u) + 0x14u});
            c.mem_write32(c.mem_read32(c.mem_read32(pack + 0x10u) + 12u * i + 4u) + 4u, handle);
        }
        f.r[29] = i;
        f.r[30] = 12u * i;
        const u32 bank = build(f.r, f.sp, kLinkRegisterBuild, pack);
        return c.mem_read16(bank);
    }

    u32 build(const u32* image, u32 caller_sp, u32 return_address, u32 pack) {
        Frame f(c, caller_sp, image, 0x30u, 24u, 16u, return_address);
        f.r[26] = pack;
        const u32 count = c.mem_read16(pack + 0x2u);
        const u32 created = call(f, kAlloc, kLinkBuildAlloc, {1u, static_cast<u32>(static_cast<s32>(count) * 24) + 0x1Cu});
        f.r[30] = created;
        if (created == 0u) return pack;
        const u32 recs = created + 0x1Cu;
        f.r[29] = recs;
        const auto table = [&] { return c.mem_read32(pack + 0x10u); };
        const auto record_word = [&](u32 i, u32 offset) { return c.mem_read32(recs + 24u * i + offset); };
        u32 pool;
        if (c.mem_read8(pack + 0x4u) != 0u) {
            if (c.mem_read16(pack + 0x2u) != 1u) return pack;  // the header just allocated leaks (quirk)
            const u32 name = c.mem_read32(table());
            const u32 name_base = name - (name & 31u);
            f.r[28] = name_base;
            u32 length = 0u;
            while (c.mem_read8(name + length) != 0u) ++length;  // strlen
            c.mem_write32(recs + 0x4u, length + 1u);
            c.mem_write32(recs + 0x0u, c.mem_read32(table()) - name_base);
            const u32 t = table();
            const u32 name_span = round32(c.mem_read32(t) + record_word(0u, 0x4u) - name_base);
            const u32 data = c.mem_read32(t + 4u);
            const u32 data_base = data - (data & 31u);
            f.r[27] = data_base;
            f.r[31] = name_span;
            c.mem_write32(recs + 0xCu, c.mem_read32(data + 8u));
            c.mem_write32(recs + 0x8u, name_span + (c.mem_read32(table() + 4u) - data_base));
            const u32 data_span = round32(c.mem_read32(table() + 4u) + record_word(0u, 0xCu) - data_base);
            f.r[24] = data_span;
            c.mem_write32(created + 0x14u, call(f, kPoolAlloc, kLinkBuildPoolOwns, {name_span + data_span}));
            pool = c.mem_read32(created + 0x14u);
            if (pool == 0u) {
                call(f, kFree, kLinkBuildFreeOwns, {created});
                return pack;
            }
            upload(pool, name_base, name_span);
            upload(c.mem_read32(created + 0x14u) + name_span, data_base, data_span);
        } else {
            const u32 first_name = c.mem_read32(table());
            const u32 base = first_name - (first_name & 31u);
            f.r[27] = base;
            u32 end = 0u, i = 0u;
            for (; static_cast<s32>(i) < static_cast<s32>(c.mem_read16(pack + 0x2u)); ++i) {
                const u32 entry = 12u * i, r = recs + 24u * i;
                const u32 name = c.mem_read32(table() + entry);
                if (name != 0u) {
                    u32 length = 0u;
                    while (c.mem_read8(name + length) != 0u) ++length;  // strlen
                    c.mem_write32(r + 0x4u, length + 1u);
                    c.mem_write32(r + 0x0u, c.mem_read32(table() + entry) - base);
                    const u32 at = c.mem_read32(table() + entry) + c.mem_read32(r + 0x4u);
                    if (end < at) end = at;
                }
                const u32 data = c.mem_read32(table() + entry + 4u);
                if (data != 0u) {
                    c.mem_write32(r + 0xCu, c.mem_read32(data + 8u));
                    c.mem_write32(r + 0x8u, c.mem_read32(table() + entry + 4u) - base);
                    const u32 at = c.mem_read32(table() + entry + 4u) + c.mem_read32(r + 0xCu);
                    if (end < at) end = at;
                }
                const u32 events = c.mem_read32(table() + entry + 8u);
                if (events != 0u) {
                    c.mem_write32(r + 0x14u, 4u);
                    for (u32 e = events;; e += c.mem_read16(e + 2u)) {
                        const u32 length = c.mem_read16(e + 2u);
                        if (static_cast<s16>(length) == -1) break;
                        c.mem_write32(r + 0x14u, c.mem_read32(r + 0x14u) + length);
                    }
                    c.mem_write32(r + 0x10u, c.mem_read32(table() + entry + 8u) - base);
                    const u32 at = c.mem_read32(table() + entry + 8u) + c.mem_read32(r + 0x14u);
                    if (end < at) end = at;
                }
            }
            const u32 span = round32(end - base);
            f.r[24] = span;
            f.r[25] = 24u * i;
            f.r[28] = end;
            f.r[31] = i;
            c.mem_write32(created + 0x14u, call(f, kPoolAlloc, kLinkBuildPool, {span}));
            pool = c.mem_read32(created + 0x14u);
            if (pool == 0u) {
                call(f, kFree, kLinkBuildFree, {created});
                return pack;
            }
            upload(pool, base, span);
        }
        // The header: the pack's fields, then the pool bank's own.
        c.mem_write16(created + 0x0u, c.mem_read16(pack + 0x0u));
        c.mem_write16(created + 0x2u, c.mem_read16(pack + 0x2u));
        for (u32 k = 4u; k < 8u; ++k) c.mem_write8(created + k, c.mem_read8(pack + k));
        for (u32 k = 8u; k < 0x14u; k += 4u) c.mem_write32(created + k, c.mem_read32(pack + k));
        c.mem_write32(created + 0x10u, 0u);
        c.mem_write8(created + 0x6u, 1u);
        c.mem_write8(created + 0x4u, 0u);
        c.mem_write32(created + 0x8u, kFree);
        Bank bank;
        bank.header = created;
        bank.first = static_cast<u16>(c.mem_read16(created + 0x0u));
        bank.count = static_cast<u16>(c.mem_read16(created + 0x2u));
        bank.owns = 0u;
        bank.byte5 = static_cast<u8>(c.mem_read8(created + 0x5u));
        bank.pooled = 1u;
        bank.byte7 = static_cast<u8>(c.mem_read8(created + 0x7u));
        bank.destroy = kFree;
        bank.table = 0u;
        bank.pool = c.mem_read32(created + 0x14u);
        bank.records.resize(bank.count);
        for (u32 i = 0; static_cast<s32>(i) < static_cast<s32>(bank.count); ++i) {
            const u32 r = recs + 24u * i;
            const u32 pool_now = c.mem_read32(created + 0x14u);
            c.mem_write32(r + 0x0u, pool_now + c.mem_read32(r + 0x0u));
            c.mem_write32(r + 0x8u, pool_now + c.mem_read32(r + 0x8u));
            const u32 events = c.mem_read32(r + 0x10u);
            if (events != 0u) c.mem_write32(r + 0x10u, pool_now + events);
            // The record as it now stands: words the build wrote, and what the allocation held where it wrote none.
            bank.records[i] = Record{c.mem_read32(r), c.mem_read32(r + 4u), c.mem_read32(r + 8u), c.mem_read32(r + 0xCu),
                                     c.mem_read32(r + 0x10u), c.mem_read32(r + 0x14u)};
        }
        // The new bank takes the pack's place in the list.
        const std::size_t at = index_of(pack);
        if (at != kNone) {
            link_before(at, created);
            s.banks_[at] = std::move(bank);
        }
        ++s.stats_.built;
        const u32 owns = c.mem_read8(pack + 0x4u);
        const u32 destroy = c.mem_read32(pack + 0x8u);
        if (owns != 0u) {
            if (destroy != 0u) call(f, destroy, kLinkBuildDestroyData, {c.mem_read32(c.mem_read32(pack + 0x10u) + 4u)});
            call(f, kFree, kLinkBuildFreePack, {pack});
        } else if (destroy != 0u) {
            call(f, destroy, kLinkBuildDestroyPack, {pack});
        }
        return created;
    }
    void upload(u32 pool_to, u32 from, u32 bytes) {
        if (!s.pool_.upload_span(c, pool_to, from, bytes)) throw Stopped{};
        if (c.needs_dispatch()) ++s.stats_.deferred_interrupts;
    }

    // ---- 0x80413A24 unregister / 0x80412764 drop loaded clips ----------------------------------------------------
    void unregister(const u32* image, u32 caller_sp, u32 return_address, u32 first) {
        Frame f(c, caller_sp, image, 0x20u, 28u, 16u, return_address);
        const u32 key = first & 0xFFFFu;
        std::size_t bi = kNone;
        for (std::size_t i = 0; i < s.banks_.size(); ++i)
            if (s.banks_[i].first == key) {
                bi = i;
                break;
            }
        if (bi == kNone) return;
        Bank b = std::move(s.banks_[bi]);
        link_before(bi, bi + 1u < s.banks_.size() ? s.banks_[bi + 1u].header : 0u);
        s.banks_.erase(s.banks_.begin() + static_cast<std::ptrdiff_t>(bi));
        ++s.stats_.unregistered;
        f.r[31] = b.header;
        f.r[30] = b.destroy;
        if (b.pooled != 0u) {
            f.r[29] = b.nodes.empty() ? 0u : b.nodes.front().block;
            call(f, kPoolFree, kLinkUnregisterPool, {b.pool});
            for (std::size_t k = 0; k < b.nodes.size(); ++k) {
                f.r[29] = b.nodes[k].block;
                f.r[28] = k + 1u < b.nodes.size() ? b.nodes[k + 1u].block : 0u;
                ++s.stats_.frees;
                call(f, kFree, kLinkUnregisterNode, {b.nodes[k].block});
            }
            f.r[29] = 0u;
            clear_held(b);
        }
        if (b.owns != 0u) {
            if (b.destroy != 0u) call(f, b.destroy, kLinkUnregisterData, {c.mem_read32(b.table + 4u)});
            call(f, kFree, kLinkUnregisterBank, {b.header});
        } else if (b.destroy != 0u) {
            call(f, b.destroy, kLinkUnregisterDestroy, {b.header});
        }
    }
    void drop_loaded(const u32* image, u32 caller_sp, u32 return_address) {
        Frame f(c, caller_sp, image, 0x10u, 30u, 8u, return_address);
        for (std::size_t bi = 0; bi < s.banks_.size(); ++bi) {
            if (s.banks_[bi].pooled == 0u) continue;
            f.r[31] = s.banks_[bi].header;
            std::vector<Node> nodes = std::move(s.banks_[bi].nodes);
            s.banks_[bi].nodes.clear();
            for (std::size_t k = 0; k < nodes.size(); ++k) {
                f.r[30] = k + 1u < nodes.size() ? nodes[k + 1u].block : 0u;
                ++s.stats_.frees;
                call(f, kFree, kLinkDropFree, {nodes[k].block});
            }
            c.mem_write32(s.banks_[bi].header + 0x18u, 0u);
            clear_held(s.banks_[bi]);
        }
    }

    // ---- 0x8041ADAC bind: a motion slot takes its clip ------------------------------------------------------------
    void bind(const u32* image, u32 caller_sp, u32 return_address, u32 object, u32 index) {
        Frame f(c, caller_sp, image, 0x20u, 29u, 20u, return_address);
        const u32 slot = c.mem_read32(object + 0x10u) + static_cast<u32>(static_cast<s32>(index) * 304);
        f.r[29] = object;
        f.r[30] = index;
        f.r[31] = slot;
        const auto flags = [&] { return c.mem_read32(slot + 0x90u); };
        const auto set = [&](u32 bits) { c.mem_write32(slot + 0x90u, flags() | bits); };
        const auto clear = [&](u32 bits) { c.mem_write32(slot + 0x90u, flags() & ~bits); };
        acquire(f.r, f.sp, kLinkBindAcquire, c.mem_read16(slot + 0x110u));
        if ((flags() & 4u) != 0u) release(f.r, f.sp, kLinkBindReleaseOld, c.mem_read16(slot + 0x128u));
        // memset 0x8000540C (slot + 0xD0, 0, 12): its frame (back chain, link word, the saved r31) and the bytes
        c.mem_write32(f.sp - 0x10u, f.sp);
        c.mem_write32(f.sp + 4u, kLinkBindMemset);
        c.mem_write32(f.sp - 0x4u, f.r[31]);
        for (u32 i = 0; i < 12u; ++i) c.mem_write8(slot + 0xD0u + i, 0u);
        if ((c.mem_read32(slot + 0x94u) & 2u) == 0u) copy3(slot + 0xB0u, object + 0xF0u);
        else set(0x100000u);
        if ((c.mem_read32(slot + 0x94u) & 4u) == 0u) {
            copy3(slot + 0xC0u, object + 0x100u);
            for (u32 i = 0; i < 12u; ++i) c.mem_write32(slot + 0x60u + 4u * i, c.mem_read32(object + 0x90u + 4u * i));
        } else {
            set(0x200000u);
        }
        set(0x4u | 0x10000u | 0x20000u | 0x80000u | 0x1000000u);
        if (call(f, kBindChannels, kLinkBindChannels, {object, index, c.mem_read16(slot + 0x110u)}) == 0u) {
            release(f.r, f.sp, kLinkBindReleaseNew, c.mem_read16(slot + 0x110u));
            c.mem_write16(slot + 0x128u, 0u);
            return;
        }
        c.mem_write16(slot + 0x128u, c.mem_read16(slot + 0x110u));
        if ((c.mem_read32(slot + 0x94u) & 0x800u) != 0u) {
            if (records(f.r, f.sp, kLinkBindQueryInverted, false, c.mem_read16(slot + 0x128u), 2u, 0u, 0u, 0u) != 0u) clear(8u);
            else set(8u);
        } else {
            if (records(f.r, f.sp, kLinkBindQueryKind2, false, c.mem_read16(slot + 0x128u), 2u, 0u, 0u, 0u) != 0u) set(8u);
            else clear(8u);
        }
        if ((c.mem_read32(slot + 0x94u) & 0x1000u) == 0u) {
            if (records(f.r, f.sp, kLinkBindQueryKind1, false, c.mem_read16(slot + 0x128u), 1u, 0u, 0u, 0u) != 0u) set(2u);
            else clear(2u);
        }
    }
    // 0x800091D0: three words through the FPRs (load, load, store, load, store, store)
    void copy3(u32 to, u32 from) {
        const u32 x = c.mem_read32(from), y = c.mem_read32(from + 4u);
        c.mem_write32(to, x);
        const u32 z = c.mem_read32(from + 8u);
        c.mem_write32(to + 4u, y);
        c.mem_write32(to + 8u, z);
    }
};

// ---- the store -----------------------------------------------------------------------------------------------------
MotionBankStore::MotionBankStore(Cpu& cpu, GuestMemory& memory, NativeMotionStore& pool) noexcept
    : cpu_(cpu), memory_(memory), pool_(pool) {
    (void)memory_;
}

MotionBankStore::~MotionBankStore() {
    std::lock_guard<std::mutex> lock(g_instances_mutex);
    for (Instance& instance : g_instances)
        if (instance.store == this) instance = Instance{};
}

const MotionBankStore::Address* MotionBankStore::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

const MotionBankStore* MotionBankStore::of(const Cpu& cpu) noexcept {
    std::lock_guard<std::mutex> lock(g_instances_mutex);
    for (const Instance& instance : g_instances)
        if (instance.cpu == &cpu) return instance.store;
    return nullptr;
}

void MotionBankStore::install() {
    {
        std::lock_guard<std::mutex> lock(g_instances_mutex);
        bool placed = false;
        for (Instance& instance : g_instances)
            if (instance.cpu == &cpu_ || (!placed && instance.store == nullptr)) {
                if (placed) {
                    instance = Instance{};
                    continue;
                }
                instance = Instance{&cpu_, this};
                placed = true;
            }
    }
    // Every owner: the original's registers at entry are its caller's image; r1 and the non-volatile registers are as
    // they were on return; r3 is the original's result (void functions leave r3 as the last callee left it: callers
    // never read it).
    const auto own = [this](u32 address, const char* name, auto body) {
        cpu_.install_hle(address, [this, body](Cpu& c) {
            const u32 sp = c.gpr[1];
            try {
                Impl impl{*this, c};
                const EntryImage image(c);
                body(impl, image.r, sp, c.lr);
            } catch (const Stopped&) {
            }
            c.gpr[1] = sp;
            return true;  // an owner never declines
        }, name);
    };
    own(kDropLoaded, kAddresses[0].name, [](Impl& i, const u32* r, u32 sp, u32 lr) { i.drop_loaded(r, sp, lr); });
    own(kNextRecord, kAddresses[1].name, [](Impl& i, const u32* r, u32 sp, u32 lr) {
        i.c.gpr[3] = i.records(r, sp, lr, true, r[3], r[4], r[5], r[6], r[7]);
    });
    own(kQuery, kAddresses[2].name, [](Impl& i, const u32* r, u32 sp, u32 lr) {
        i.c.gpr[3] = i.records(r, sp, lr, false, r[3], r[4], r[5], r[6], r[7]);
    });
    own(kHandleOf, kAddresses[3].name, [](Impl& i, const u32* r, u32 sp, u32 lr) { i.c.gpr[3] = i.handle_of(r, sp, lr, r[3]); });
    own(kTrack, kAddresses[4].name, [](Impl& i, const u32* r, u32, u32) { i.c.gpr[3] = i.track(r[3]); });
    own(kFrameRange, kAddresses[5].name, [](Impl& i, const u32* r, u32 sp, u32 lr) {
        i.c.gpr[3] = i.frame_range(r, sp, lr, r[3], r[4]);
    });
    own(kIdOfName, kAddresses[6].name, [](Impl& i, const u32* r, u32 sp, u32 lr) { i.c.gpr[3] = i.id_of_name(r, sp, lr, r[3]); });
    own(kNameOf, kAddresses[7].name, [](Impl& i, const u32* r, u32 sp, u32 lr) { i.c.gpr[3] = i.name_of(r, sp, lr, r[3]); });
    own(kDataOf, kAddresses[8].name, [](Impl& i, const u32* r, u32, u32) { i.c.gpr[3] = i.data_of(r[3]); });
    own(kUnregister, kAddresses[9].name, [](Impl& i, const u32* r, u32 sp, u32 lr) { i.unregister(r, sp, lr, r[3]); });
    own(kRegister, kAddresses[10].name, [](Impl& i, const u32* r, u32 sp, u32 lr) {
        i.c.gpr[3] = i.register_bank(r, sp, lr, r[3]);
    });
    own(kRelease, kAddresses[11].name, [](Impl& i, const u32* r, u32 sp, u32 lr) { i.release(r, sp, lr, r[3]); });
    own(kAcquire, kAddresses[12].name, [](Impl& i, const u32* r, u32 sp, u32 lr) { i.acquire(r, sp, lr, r[3]); });
    own(kBuild, kAddresses[13].name, [](Impl& i, const u32* r, u32 sp, u32 lr) { i.c.gpr[3] = i.build(r, sp, lr, r[3]); });
    own(kBind, kAddresses[14].name, [](Impl& i, const u32* r, u32 sp, u32 lr) { i.bind(r, sp, lr, r[3], r[4]); });
    for (std::size_t k = kFunctionCount; k < sizeof(kAddresses) / sizeof(kAddresses[0]); ++k) {
        const u32 address = kAddresses[k].address;
        cpu_.install_hle(address, [address](Cpu& c) {
            c.request_stop(StopReason::ExecutionFault,
                           "motion banks (B003): entered at an unreachable branch inside an owned function, " +
                               std::to_string(address));
            c.leave_to_dispatcher(address, FallbackReason::NotRecompiled, address);
            return true;
        }, kAddresses[k].name);
    }
}

void MotionBankStore::publish_to_guest() {
    Cpu& c = cpu_;
    c.mem_write32(kStateHead, banks_.empty() ? 0u : banks_.front().header);
    c.mem_write32(kStateLastFound, last_found_);
    for (u32 i = 0; i < 4u; ++i) c.mem_write16(kStateHeld + 2u * i, held_[i]);
    c.mem_write32(kStateTrack, track_);
    c.mem_write8(kStateTrackReady, track_ready_);
    c.mem_write32(kStateEventsA, events_a_);
    c.mem_write32(kStateEventsB, events_b_);
}

bool MotionBankStore::import_from_guest() {
    Cpu& c = cpu_;
    Impl impl{*this, c};
    std::vector<Bank> banks;
    u32 guard = 0;
    for (u32 b = c.mem_read32(kStateHead); b != 0u; b = c.mem_read32(b + 0xCu)) {
        Bank bank;
        if (++guard > kListBound || !impl.adopt(b, bank)) return false;
        banks.push_back(std::move(bank));
    }
    banks_ = std::move(banks);
    last_found_ = c.mem_read32(kStateLastFound);
    for (u32 i = 0; i < 4u; ++i) held_[i] = static_cast<u16>(c.mem_read16(kStateHeld + 2u * i));
    track_ = c.mem_read32(kStateTrack);
    track_ready_ = static_cast<u8>(c.mem_read8(kStateTrackReady));
    events_a_ = c.mem_read32(kStateEventsA);
    events_b_ = c.mem_read32(kStateEventsB);
    return true;
}

}  // namespace gekko
