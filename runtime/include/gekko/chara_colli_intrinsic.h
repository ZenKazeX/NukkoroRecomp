#pragma once

// The character-collision broad phase (SonicteamUSA::System::CharaColli; docs/CPU_FINAL_HAUNT.md), served
// natively where it rejects. Every ray query (0x800C5D20) and sphere sweep (0x800C51C8) of the ground behaviour
// walks every collision object in the world and calls its test virtual; on the Final Haunt tape that is 8.4 million
// tests in 120 s of game time, of which 3,242 hit. Each test first re-syncs the object from its owner and then
// rejects it at the bounding sphere. This serves those four test virtuals:
//
//   CharaColliBody::vf 0x60     0x800C15A0  ray    (this r3, segment r4, result r5, filter r6, filter arg r7)
//   CharaColliBody::vf 0x6C     0x800C1108  sweep  (this r3, sphere r4, motion r5, result r6, filter r7, arg r8)
//   CharaColliGeneral::vf 0x60  0x800C31EC  ray    (this r3, segment r4, result r5, filter r6, filter arg r7)
//   CharaColliGeneral::vf 0x6C  0x800C2854  sweep  (this r3, sphere r4, motion r5, result r6, filter r7, arg r8)
//   (Body: also BodyCharInfo; General: also GeneralCharInfo and GeneralBone, whose update is below)
//
// What each guest body does up to its bounding test:
//   update = vf 0x88 (BodyCharInfo 0x800C1A74, GeneralCharInfo 0x800C409C, the same code; plain Body 0x800C0C9C
//     and General 0x800C1D20 are a bare blr): lock the owner's weak_ptr at this+0x30
//     (px; its sp_counted_base pi at this+0x34) - if it is expired or px is null nothing changes - else copy the
//     owner's position (px+0, 3 floats) to this+0x40, its rotation (px+0xC) to this+0x4C and its word px+0x30 to
//     this+0x58, each only when it differs (fcmpu per component, the word by cmpw), setting the dirty byte this+0x28.
//     Every reference count the locks touch is incremented and decremented again in the same call.
//   centre = vf 0x14 = this+0x40; radius = this+8 (float)
//   d2 = 0x8003C688 (centre, segment {a, b}, null): squared distance from the centre to the segment through the
//     RenderWare vector helpers (RwV3dSub/Scale/Add/Dot/LengthSquared, every result stored as a single)
//   ray:   reject when d2 > radius * radius
//   sweep: segment {sphere.centre, sphere.centre + motion}; reject when d2 > ((radius + sphere.r) + sphere.r)^2
//   Body only: vf 0x28 (0x800C0BD0) reads the shape at this+0x5C, and a non-null filter is called before the test
//   (returning 0 rejects without it). The ground behaviour's filter, 0x80099454, is served natively (its contract at
//   ground_filter in the .cpp: a mask test, an exclusion list, a __dynamic_cast and a team check), and the five
//   others the enemies' queries pass (run_filter: three mask tests, an exclusion of two objects, a __dynamic_cast and
//   owner-id check); any other declines.
// A rejected test returns r3 = 0.
//
// Served (a rejected test): every operation is the guest's ppc_ops helper (fps_*, fp_compare) in the guest's order,
// on the guest's operand bits, and every stored float the guest's single conversion, so FPSCR and every byte written
// are the guest's. What a served call leaves: r3 = 0; the object's update writes; FPSCR as the guest's operations
// leave it; f31 restored through its paired-single save (ps1 rounded to single), every other non-volatile register,
// LR and memory at or above r1 unchanged. Not reproduced, by the ABI: volatile registers (r0, r4-r12, f0-f13, CTR,
// XER, CR0/1/5-7) and the dead stack below r1. The tests are reached only through their vtables (no direct call
// site in the DOL), so a caller can rely on nothing else; both query loops read r3 alone after the call.
//
// Declined (the guest body runs, nothing changed - FPSCR and CR restored): MSR[FP] clear; GQR0 quantising (the
// paired-single save); a vtable whose 0x88/0x14/0x28 slot is not the code above; another filter (Body); a reference
// count that the lock pair could drive to zero; a dirty GeneralBone; and every test that passes the bounding sphere
// (the narrow phase stays guest code; a GeneralBone test is handed to the guest test at its vf 0x88 return instead,
// its update's writes standing).

#include "gekko/types.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gekko {

class Cpu;
class DynamicCastIntrinsic;

class CharaColliIntrinsic {
public:
    // Manage: not a test - the frame a GeneralBone update stands in when the manager's pass calls it (vf 0x7C).
    enum class Test : u8 { BodyRay, BodySweep, GeneralRay, GeneralSweep, Manage };
    static constexpr u32 kBodyRay = 0x800C15A0u;
    static constexpr u32 kBodySweep = 0x800C1108u;
    static constexpr u32 kGeneralRay = 0x800C31ECu;
    static constexpr u32 kGeneralSweep = 0x800C2854u;

    // The hook: true when the call was served, false to run the guest body.
    bool serve(Cpu& cpu, Test test);

    // The three query loops (0x800C5D20 ray, 0x800C51C8 sweep, 0x800C615C the enemies' ray), served natively: the guest loop transcribed onto the
    // CPU's own register file and stack frame (every frame word and loop register the guest would hold), its small
    // helpers inline (a weak_ptr copy released with no guest code between changes no count, so none is written),
    // the four tests above called directly. A test that declines, or any other test virtual, is a nested guest call
    // with the guest's own return address in LR. The query is handed back to the guest body - the machine left at
    // the guest instruction it stands at, every register and frame word as the guest's would be there - when a test
    // returns a hit, an entry is expired (the guest erases it), a release would reach zero (the guest destroys), a
    // count could wrap, or a nested guest call is cut short by an interrupt (the guest resumes inside it and returns
    // into its own loop). A query served to its end returns as the guest's does without a hit: r3 = 0 (ray) or the
    // list argument r8 (sweep), r1 and r14-r31 restored, the frame's back chain, saved registers and LR save word
    // written.
    enum class Query : u8 { Ray, Sweep, Enemy };
    static constexpr u32 kRayQuery = 0x800C5D20u;
    static constexpr u32 kSweepQuery = 0x800C51C8u;
    static constexpr u32 kEnemyQuery = 0x800C615Cu;  // the enemies' ray query (0x80021FE4)
    bool serve_query(Cpu& cpu, Query query);
    u64 queries() const noexcept { return queries_; }
    u64 queries_finished() const noexcept { return queries_finished_; }  // served to the end natively
    // Handed back to the guest body: 0 hit, 1 expired entry, 2 release to zero / count, 3 nested call cut short.
    u64 handed_back(u32 reason) const noexcept { return handed_back_[reason]; }
    u64 nested_calls() const noexcept { return nested_calls_; }  // guest test calls made from a native loop

    // The manager's per-frame pass (fn_800C7068) calls every object's vf 0x7C: GeneralBone 0x800C3DD4 (its update),
    // Body 0x800C1788 / General 0x800C3594 (vf 0x88, then vf 0x8C: when dirty, the shape / children re-derived from
    // the owner). Served by the same native code as the tests' updates; declined (the guest's vf 0x7C runs, nothing
    // changed) outside that code's contract.
    enum class Manage : u8 { Bone, Body, General };
    bool serve_manage(Cpu& cpu, Manage kind);
    u64 manage_calls() const noexcept { return manage_calls_; }
    u64 manage_served(Manage kind) const noexcept { return manage_served_[static_cast<u32>(kind)]; }
    u64 manage_rederived() const noexcept { return manage_rederived_; }  // a dirty Body / General re-derived
    u64 manage_declined() const noexcept { return manage_declined_; }
    u64 manage_verified() const noexcept { return manage_verified_; }
    u64 manage_differed() const noexcept { return manage_differed_; }
    void set_manage(bool enabled) noexcept { manage_ = enabled; }  // false: the pass calls the guest's vf 0x7C

    // The pass itself (fn_800C7068): its loops over every object pair, vf 0x7C, the radius / centre getters and the
    // distance test native; contact events, records and the narrow phase (vf 0x80) the guest's, called with the
    // guest's own return addresses (the contract and the hand-backs at run_pass in the .cpp).
    static constexpr u32 kPass = 0x800C7068u;
    bool serve_pass(Cpu& cpu);
    u64 pass_calls() const noexcept { return pass_calls_; }
    u64 pass_finished() const noexcept { return pass_finished_; }
    // Handed back: 0 expired entry / null contact, 1 release to zero / count, 2 nested call cut short, 3 getters.
    u64 pass_back(u32 reason) const noexcept { return pass_back_[reason]; }
    u64 pass_pairs() const noexcept { return pass_pairs_; }        // distance tests
    u64 pass_narrow() const noexcept { return pass_narrow_; }      // narrow phases (vf 0x80)
    u64 pass_contacts() const noexcept { return pass_contacts_; }  // narrow phases in contact
    u64 pass_verified() const noexcept { return pass_verified_; }
    u64 pass_differed() const noexcept { return pass_differed_; }

    // --trace-stale-contacts (observation only; every guest access then takes the traced slow path): the narrow
    // phase copies its stack contact results R (0x8001FF00 initialises three words) into the heap contact objects
    // (0x8002686C: R at +0xC, R +0xC again at +0x44). The bytes of R no write reached are stale stack; every later
    // guest read of their copies is counted, by reader LR and R offset, until the byte is overwritten.
    void trace_stale_contacts(Cpu& cpu);
    bool stale_init(Cpu& cpu);  // 0x8001FF00 entry (declines)
    void stale_note(Cpu& cpu, u32 result);  // a result initialised (the native loops' 0x8001FF00 too)
    bool stale_copy(Cpu& cpu);  // 0x8002686C entry (declines)
    bool stale_copy_block(Cpu& cpu, u32 size, u32 last);  // a copy routine's entry (declines)
    std::string stale_summary() const;

    // CharaColliGeneralBone (update 0x800C3D6C: 0x800C409C, then every child Range shape re-posed on its bone and the
    // bounding radius recomputed; the contract at bone_inputs in the .cpp). Its tests are served too: the update is
    // a deterministic function of its read set, so a run whose every input word equals a recorded guest run's is
    // that run again - its recorded output words are written, FPCC set as it left it, and the FPRs its callees save
    // with psq_st rounded (ps1 to single) as their restores leave them. A run with no record is the guest's own,
    // nested with the guest test's prologue built and the test's own return address, then recorded (when it raised
    // no FPSCR exception) and the test handed to the guest at that return. A dirty object (vf 0x8C re-derives every
    // child) declines.
    u64 bone_hits() const noexcept { return bone_hits_; }
    u64 bone_misses() const noexcept { return bone_misses_; }
    u64 bone_recorded() const noexcept { return bone_recorded_; }
    u64 bone_dirty() const noexcept { return bone_dirty_; }
    u64 bone_unkeyed() const noexcept { return bone_unkeyed_; }  // a child class or layout outside the contract
    // Computed natively (gekko/chara_colli_bone.h) when not reused: clean / dirty updates, and those outside its
    // contract (run by the guest). --chara-colli-bone-guest: every update not reused run by the guest (A/B).
    void set_bone_native(bool enabled) noexcept { bone_native_ = enabled; }
    // --chara-colli-bone-no-memo: every update computed (diagnostic: the cost of the computation alone).
    void set_bone_memo(bool enabled) noexcept { bone_memo_on_ = enabled; }
    u64 bone_native_clean() const noexcept { return bone_native_clean_; }
    u64 bone_native_dirty() const noexcept { return bone_native_dirty_; }
    u64 bone_native_declined() const noexcept { return bone_native_declined_; }
    u64 bone_checked() const noexcept { return bone_checked_; }    // native updates checked against the guest's
    u64 bone_differed() const noexcept { return bone_differed_; }
    // With verification on, every 64th checked update is also run on changed inputs (NaN / infinite / denormal / -0
    // angles, a zero or NaN axis, other rotation orders, NaN positions and radii, a singular or NaN bone matrix,
    // dirty states) by both and compared, everything put back after.
    u64 bone_fuzzed() const noexcept { return bone_fuzzed_; }
    u64 bone_fuzz_differed() const noexcept { return bone_fuzz_differed_; }
    u64 bone_fuzz_declined() const noexcept { return bone_fuzz_declined_; }
    // The native __dynamic_cast the ground filter's cast uses (without it a filtered test declines).
    void set_dynamic_cast(DynamicCastIntrinsic* cast) noexcept { cast_ = cast; }

    u64 calls() const noexcept { return calls_; }
    u64 served() const noexcept { return served_; }
    u64 passed() const noexcept { return passed_; }      // declined: inside the bounding sphere
    u64 filtered() const noexcept { return filtered_; }  // declined: another filter
    u64 filter_runs() const noexcept { return filter_runs_; }  // a Body filter computed natively
    u64 other() const noexcept { return other_; }        // declined: another class or state
    // other() by test and reason (0 MSR/GQR, 1 vtable, 2 counts)
    u64 other_by(Test test, u32 reason) const noexcept { return other_by_[static_cast<u32>(test)][reason]; }

    // --verify-chara-colli: every call also runs the guest body from the same state (the native result rolled back
    // first). A served call is compared on the contract above: r1-r3, r13-r31, f14-f31 (both halves), LR, CR2-4,
    // FPSCR and the bytes of the object, its owner's data and counts and the inputs; every 1024th also on every
    // committed byte of guest memory outside the stack below r1. A declined call must have changed nothing at all.
    // The guest body's state is the one kept.
    void set_verify(bool enabled) noexcept { verify_ = enabled; }
    u64 verified() const noexcept { return verified_; }
    u64 full_checks() const noexcept { return full_checks_; }
    u64 differed() const noexcept { return differed_; }
    u64 incomplete() const noexcept { return incomplete_; }  // the guest body cut short by an interrupt
    // With verification on, every query served to its end is also run by the guest body from the same state, every
    // test in that run by its guest body too, and compared on r1-r3, r13-r31, f14-f31, LR, CR2-4, FPSCR and every
    // committed byte of guest memory outside the stack below the caller's r1.
    u64 queries_verified() const noexcept { return queries_verified_; }
    u64 queries_differed() const noexcept { return queries_differed_; }

private:
    bool execute(Cpu& cpu, Test test);
    bool verify(Cpu& cpu, Test test);
    bool rejected(Cpu& cpu, u32 cr);  // r3 = 0 and the rest of a served call's leftovers
    int run_filter(Cpu& cpu, u32 filter, u32 shape, u32 self, u32 arg);
    int ground_filter(Cpu& cpu, u32 shape, u32 self, u32 arg);
    // A query's test virtual at `target` with the guest's arguments in r3-r8 and link `link`: 1 when it returned
    // normally (r3 its result), 0 when a nested guest call was cut short (the machine stands inside it).
    int call_test(Cpu& cpu, u32 target, u32 link);
    int call_guest(Cpu& cpu, u32 target, u32 link);  // 1 returned to link, 0 cut short
    u32 bone_test_frame(Cpu& cpu, Test test);
    bool bone_inputs(Cpu& cpu, u32 self, std::vector<u32>& key, std::vector<u32>& outputs);
    int bone_update(Cpu& cpu, Test test, u32 cr, u32 fpscr);
    struct BoneRun {
        std::vector<u32> key, values;
        u32 fpcc = 0;
        u32 exceptions = 0;  // the FPSCR exception bits set when it ran: they must still be set
        bool wide = false;  // f21-f31 rounded (a capsule or cylinder re-posed), else f30-f31
    };
    std::unordered_map<u32, BoneRun> bone_memo_;
    struct BoneState {  // the update's writes and changed state, for --verify-chara-colli
        std::vector<u32> addresses, values;
        u8 dirty = 0, posed = 0;
        u32 fpscr = 0;
        u64 fpr[32][2] = {};
    };
    void bone_snapshot(Cpu& cpu, u32 self, BoneState& state);
    int verify_bone(Cpu& cpu, Test test, u32 self);
    void fuzz_bone(Cpu& cpu, Test test, u32 self);
    BoneState bone_fuzz_keep_;
    u64 bone_fuzz_rounds_ = 0, bone_fuzzed_ = 0, bone_fuzz_differed_ = 0, bone_fuzz_declined_ = 0;
    BoneState bone_pre_, bone_native_state_, bone_guest_state_;
    bool bone_native_ = true;
    bool bone_memo_on_ = true;
    u64 bone_native_clean_ = 0, bone_native_dirty_ = 0, bone_native_declined_ = 0, bone_checked_ = 0,
        bone_differed_ = 0;
    u64 bone_verified_ = 0;
    std::vector<u32> bone_key_, bone_outputs_;
    bool bone_oriented_ = false;
    u64 bone_hits_ = 0, bone_misses_ = 0, bone_recorded_ = 0, bone_dirty_ = 0, bone_unkeyed_ = 0;
    bool run_ray(Cpu& cpu);    // true: served (finished or handed back)
    bool run_sweep(Cpu& cpu);
    bool run_enemy(Cpu& cpu);
    bool run_query(Cpu& cpu, Query query);
    bool hand_back(Cpu& cpu, u32 pc, u32 reason);
    bool verify_query(Cpu& cpu, Query query);
    bool run_manage(Cpu& cpu, Manage kind);
    bool verify_manage(Cpu& cpu, Manage kind);
    u64 manage_calls_ = 0, manage_served_[3] = {}, manage_rederived_ = 0, manage_declined_ = 0, manage_verified_ = 0,
        manage_differed_ = 0;
    std::vector<std::pair<u32, u32>> manage_windows_;
    std::vector<u8> manage_before_, manage_native_, manage_guest_;
    bool manage_ = true;
    void stale_access(u32 address, u32 size, bool writing);
    void stale_mark(u32 address, int delta);
    void stale_taint(u32 address, u8 offset);
    void stale_clean(u32 address);
    void stale_arm(u32 dst, u32 src, u32 size, u32 last);
    struct StaleCopy {
        u32 src, size, last;
        std::vector<std::pair<u32, u8>> carried;
    };
    Cpu* stale_cpu_ = nullptr;
    std::vector<u32> stale_pages_;
    std::unordered_set<u32> stale_results_;       // R bases on the stack
    std::unordered_map<u32, u8> stale_bytes_;     // stale byte -> R offset
    std::unordered_map<u64, u64> stale_readers_;  // (lr << 9 | stack << 8 | R offset) -> reads
    std::vector<StaleCopy> stale_copies_;
    u64 stale_inits_ = 0, stale_contacts_ = 0, stale_reads_ = 0;
    u64 stale_carried_[0x38] = {};
    int call_manage(Cpu& cpu, u32 self, u32 link);
    bool run_pass(Cpu& cpu);
    bool verify_pass(Cpu& cpu);
    u64 pass_calls_ = 0, pass_finished_ = 0, pass_back_[4] = {}, pass_pairs_ = 0, pass_narrow_ = 0,
        pass_contacts_ = 0, pass_verified_ = 0, pass_differed_ = 0;

    DynamicCastIntrinsic* cast_ = nullptr;

    bool verify_ = false;
    bool reference_ = false;  // verification's guest run: every hook declines
    u64 queries_ = 0, queries_finished_ = 0, handed_back_[4] = {}, nested_calls_ = 0;
    u64 queries_verified_ = 0, queries_differed_ = 0;
    u64 calls_ = 0, served_ = 0, passed_ = 0, filtered_ = 0, other_ = 0, filter_runs_ = 0;
    u64 other_by_[4][3] = {};
    u64 verified_ = 0, full_checks_ = 0, differed_ = 0, incomplete_ = 0;
    std::vector<u8> full_before_, full_native_, full_guest_;
};

}  // namespace gekko
