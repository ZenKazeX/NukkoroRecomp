// CRI's server manager (SVM/GC Ver.1.58) as native C++20. Contract and evidence: include/gekko/native_svm.h,
// docs/CRI_SERVERS.md section 1.

#include "gekko/native_svm.h"

#include <cstdio>
#include <initializer_list>
#include <string>

#include "gekko/cpu.h"
#include "gekko/native_guest_call.h"
#include "gekko/savestate.h"

namespace gekko {

namespace {

// The eighteen entry points (PAL DOL; names from the module's own messages where it has them).
constexpr u32 kTestAndSet = 0x803CBFB8u;
constexpr u32 kFinish = 0x803CC0CCu;
constexpr u32 kInit = 0x803CC194u;
constexpr u32 kServerIdle = 0x803CC24Cu;   // type 6
constexpr u32 kServerMain = 0x803CC2DCu;   // type 5
constexpr u32 kServer4 = 0x803CC36Cu;      // type 4
constexpr u32 kServerVsync = 0x803CC3FCu;  // type 2
constexpr u32 kSetCbUnlock = 0x803CC48Cu;
constexpr u32 kSetCbLock = 0x803CC49Cu;
constexpr u32 kCallUsrHook = 0x803CC4ACu;
constexpr u32 kSetUsrHook = 0x803CC4F0u;
constexpr u32 kSetCbSvrId = 0x803CC5F4u;
constexpr u32 kDelCbSvr = 0x803CC7D0u;
constexpr u32 kSetCbSvr = 0x803CC970u;
constexpr u32 kError = 0x803CCB2Cu;
constexpr u32 kErrorf = 0x803CCB84u;
constexpr u32 kUnlock = 0x803CCC5Cu;
constexpr u32 kLock = 0x803CCCECu;

constexpr u32 kUnknownName = 0x805186FCu;  // "Unknown", the name a registration without one gets
constexpr u32 kNoUserHook = 0x805C0EF8u;   // what the original's CallUsrHook leaves in r3 when the hook is null

// The owner frame on the guest stack (0x40 bytes; +4 is the callee's LR save word).
constexpr u32 kFrameSize = 0x40u;
constexpr u32 kTag = 0x4E53564Du;  // 'NSVM'
constexpr u32 kFrameTag = 0x08u, kFrameOp = 0x0Cu, kFramePhase = 0x10u, kFrameIndex = 0x14u, kFrameAcc = 0x18u,
              kFrameLr = 0x1Cu, kFrameArgs = 0x20u, kFrameResult = 0x38u;

enum Phase : u32 {
    kEnter = 0,       // svm_lock: the lock hook
    kEntered,         // ... returned: depth / type
    kBody,            // the operation under the lock
    kLeave,           // svm_unlock: depth / type, then the unlock hook
    kServerNext,      // a server: the next slot
    kServerReturned,  // ... its callback returned
    kCallHook,        // CallUsrHook
    kDone,
};

void stop(Cpu& c, const std::string& why) { c.request_stop(StopReason::ExecutionFault, "native SVM: " + why); }

bool is_type(u32 type) { return static_cast<s32>(type) >= 0 && static_cast<s32>(type) < 8; }
bool is_id(u32 id) { return static_cast<s32>(id) >= 0 && static_cast<s32>(id) < 6; }

const NativeSvm::Address kAddresses[] = {
    {kTestAndSet, "SVM test-and-set", false},
    {kFinish, "SVM_Finish", false},
    {kInit, "SVM_Init", false},
    {kServerIdle, "SVM server type 6 (Idle)", false},
    {kServerMain, "SVM server type 5 (Main)", false},
    {kServer4, "SVM server type 4", false},
    {kServerVsync, "SVM server type 2 (Vsync)", false},
    {kSetCbUnlock, "SVM_SetCbUnlock", false},
    {kSetCbLock, "SVM_SetCbLock", false},
    {kCallUsrHook, "SVM user hook call", false},
    {kSetUsrHook, "SVM user hook set", false},
    {kSetCbSvrId, "SVM_SetCbSvrId", false},
    {kDelCbSvr, "SVM_DelCbSvr", false},
    {kSetCbSvr, "SVM_SetCbSvr", false},
    {kError, "SVM error report", false},
    {kErrorf, "SVM formatted error report", false},
    {kUnlock, "svm_unlock", false},
    {kLock, "svm_lock", false},
    {NativeSvm::kReturn, "native SVM: a guest call returned", true},
};

}  // namespace

enum class NativeSvm::Op : u32 {
    Server = 1, Lock, Unlock, TestAndSet, SetUsrHook, CallUsrHook, SetCbSvrId, DelCbSvr, SetCbSvr
};

const NativeSvm::Address* NativeSvm::addresses(std::size_t& count) noexcept {
    count = std::size(kAddresses);
    return kAddresses;
}

void NativeSvm::report(const char* message, u32 a, u32 b) {
    ++stats_.errors;
#if !defined(GEKKO_SHIPPING)
    std::fprintf(stderr, "[native SVM] %s (0x%X, 0x%X)\n", message, a, b);
#else
    (void)message, (void)a, (void)b;
#endif
}

void NativeSvm::begin(Cpu& c, Op op, std::initializer_list<u32> args) {
    const u32 caller_sp = c.gpr[1];
    const u32 frame = caller_sp - kFrameSize;
    if ((caller_sp & 7u) != 0u || !c.guest_memory().mapped(GcAddr(frame), kFrameSize)) {
        stop(c, "the caller's stack cannot hold a frame (r1 " + std::to_string(caller_sp) + ")");
        return;
    }
    c.mem_write32(frame, caller_sp);
    c.mem_write32(frame + kFrameTag, kTag);
    c.mem_write32(frame + kFrameOp, static_cast<u32>(op));
    u32 phase = kEnter;
    if (op == Op::Server) phase = kServerNext;
    if (op == Op::Unlock) phase = kLeave;
    if (op == Op::CallUsrHook) phase = kCallHook;
    c.mem_write32(frame + kFramePhase, phase);
    c.mem_write32(frame + kFrameIndex, 0u);
    c.mem_write32(frame + kFrameAcc, 0u);
    c.mem_write32(frame + kFrameLr, c.lr);
    u32 i = 0;
    for (const u32 a : args) c.mem_write32(frame + kFrameArgs + 4u * i++, a);
    c.mem_write32(frame + kFrameResult, 0u);
    c.gpr[1] = frame;
    run(c, frame);
}

void NativeSvm::call(Cpu& c, u32 frame, u32 phase, u32 function, u32 argument) {
    c.mem_write32(frame + kFramePhase, phase);
    if (c.guest_call_depth() != 0u) {
        // Served inside gekko::guest_call (a native wait running CRI's Idle pass): no dispatcher to hand the continuation to.
        // The callee runs to completion here and the operation goes on from its result.
        c.gpr[1] = frame;
        u32 result = 0u;
        if (!guest_call(c, function, kReturn, {argument}, result)) return;  // the run is stopped
        c.gpr[3] = result;
        run(c, frame);
        return;
    }
    c.gpr[1] = frame;
    c.gpr[3] = argument;
    c.gpr[12] = function;  // the original's mtctr r12 / bctrl
    c.ctr = function;
    c.lr = kReturn;
    c.leave_to_dispatcher(function, FallbackReason::Yield, kReturn);
}

void NativeSvm::resume(Cpu& c) {
    const u32 frame = c.gpr[1];
    if ((frame & 3u) != 0u || !c.guest_memory().mapped(GcAddr(frame), kFrameSize) ||
        c.mem_read32(frame + kFrameTag) != kTag) {
        stop(c, "a guest call returned without its frame (r1 " + std::to_string(frame) + ")");
        return;
    }
    run(c, frame);
}

void NativeSvm::finish(Cpu& c, u32 frame, u32 result) {
    c.mem_write32(frame + kFrameTag, 0u);
    c.gpr[3] = result;
    c.lr = c.mem_read32(frame + kFrameLr);
    c.gpr[1] = c.mem_read32(frame);
}

void NativeSvm::run(Cpu& c, u32 frame) {
    const Op op = static_cast<Op>(c.mem_read32(frame + kFrameOp));
    const auto arg = [&](u32 i) { return c.mem_read32(frame + kFrameArgs + 4u * i); };
    u32 phase = c.mem_read32(frame + kFramePhase);
    for (;;) {
        switch (phase) {
            case kEnter:
                if (lock_hook_.function != 0u) {
                    ++stats_.locks;
                    call(c, frame, kEntered, lock_hook_.function, lock_hook_.argument);
                    return;
                }
                phase = op == Op::Lock ? kDone : kBody;
                break;
            case kEntered:
                if (lock_depth_ == 0u) lock_type_ = 1u;
                ++lock_depth_;
                phase = op == Op::Lock ? kDone : kBody;
                break;
            case kBody: {
                u32 result = 0u;
                switch (op) {
                    case Op::TestAndSet: {
                        ++stats_.test_and_sets;
                        const u32 word = arg(0);
                        const u32 old = c.mem_read32(word);
                        c.mem_write32(word, 1u);
                        result = old != 1u ? 1u : 0u;
                        break;
                    }
                    case Op::SetUsrHook: {
                        const u32 index = arg(0);
                        if (index >= kUserHooks) {  // the original would write into the server table
                            stop(c, "a user hook index " + std::to_string(index) + " outside the table");
                            return;
                        }
                        user_hooks_[index] = {arg(1), arg(2)};
                        break;
                    }
                    case Op::SetCbSvrId: {
                        const u32 type = arg(0), id = arg(1);
                        ++stats_.registrations;
                        if (!is_id(id)) {
                            report("1071201:SVM_SetCbSvrId:illegal id", type, id);
                        } else if (!is_type(type)) {
                            report("1071202:SVM_SetCbSvrId:illegal svtype", type, id);
                        } else {
                            Slot& slot = table_[type][id];
                            if (slot.function != 0u) report("2100801:SVM_SetCbSvrId:over write callback function.", type, id);
                            slot = {arg(2), arg(3), arg(4) != 0u ? arg(4) : kUnknownName};
                        }
                        break;
                    }
                    case Op::DelCbSvr: {
                        const u32 type = arg(0), id = arg(1);
                        if (!is_id(id)) {
                            report("1051002:SVM_DelCbSvr:illegal id", type, id);
                        } else if (!is_type(type)) {
                            report("1071206:SVM_SetCbSvrId:illegal svtype", type, id);
                        } else {
                            table_[type][id] = {};
                        }
                        break;
                    }
                    case Op::SetCbSvr: {
                        const u32 type = arg(0);
                        ++stats_.registrations;
                        result = ~0u;
                        if (!is_type(type)) {
                            report("1071205:SVM_SetCbSvrId:illegal svtype", type);
                        } else {
                            for (u32 id = 0; id < kSlots; ++id) {
                                Slot& slot = table_[type][id];
                                if (slot.function != 0u) continue;
                                slot = {arg(1), arg(2), arg(3) != 0u ? arg(3) : kUnknownName};
                                result = id;
                                break;
                            }
                            if (result == ~0u) report("1051001:SVM_SetCbSvr:too many server function", type);
                        }
                        break;
                    }
                    default:
                        stop(c, "an operation without a body under the lock");
                        return;
                }
                c.mem_write32(frame + kFrameResult, result);
                phase = kLeave;
                break;
            }
            case kLeave:
                if (unlock_hook_.function != 0u) {
                    ++stats_.unlocks;
                    if (--lock_depth_ == 0u) {
                        if (lock_type_ != 1u) report("2103102:SVM:svm_unlock:lock type miss match.", lock_type_, 1u);
                        lock_type_ = 0u;
                    }
                    call(c, frame, kDone, unlock_hook_.function, unlock_hook_.argument);
                    return;
                }
                phase = kDone;
                break;
            case kServerNext: {
                const u32 type = arg(0);
                u32 index = c.mem_read32(frame + kFrameIndex);
                for (; index < kSlots; ++index) {
                    const Slot& slot = table_[type][index];
                    if (slot.function == 0u) continue;
                    inside_[type] = 1u;
                    ++stats_.callbacks;
                    c.mem_write32(frame + kFrameIndex, index);
                    call(c, frame, kServerReturned, slot.function, slot.argument);
                    return;
                }
                ++passes_[type];
                c.mem_write32(frame + kFrameResult, c.mem_read32(frame + kFrameAcc));
                phase = kDone;
                break;
            }
            case kServerReturned: {
                const u32 type = arg(0);
                c.mem_write32(frame + kFrameAcc, c.mem_read32(frame + kFrameAcc) | c.gpr[3]);
                inside_[type] = 0u;
                c.mem_write32(frame + kFrameIndex, c.mem_read32(frame + kFrameIndex) + 1u);
                phase = kServerNext;
                break;
            }
            case kCallHook: {
                const Hook& hook = user_hooks_[arg(0)];
                if (hook.function == 0u) {
                    c.mem_write32(frame + kFrameResult, kNoUserHook);
                    phase = kDone;
                    break;
                }
                ++stats_.user_hook_calls;
                // The hook's own r3 is what the original returns (it does not touch r3 after the call).
                c.mem_write32(frame + kFrameResult, 0u);
                call(c, frame, kDone + 1u, hook.function, hook.argument);
                return;
            }
            case kDone + 1u:  // a user hook returned: its r3 passes through
                c.mem_write32(frame + kFrameResult, c.gpr[3]);
                phase = kDone;
                break;
            case kDone:
                finish(c, frame, c.mem_read32(frame + kFrameResult));
                return;
            default:
                stop(c, "a frame in an unknown phase " + std::to_string(phase));
                return;
        }
    }
}

void NativeSvm::install() {
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(
            address,
            [serve](Cpu& c) {
                serve(c);
                return true;
            },
            std::string("native SVM: ") + name);
    };
    const auto server = [this](u32 type) {
        return [this, type](Cpu& c) {
            ++stats_.server_calls;
            ++stats_.server_calls_by_type[type];
            begin(c, Op::Server, {type});
        };
    };
    own(kServerVsync, "server type 2", server(2u));
    own(kServer4, "server type 4", server(4u));
    own(kServerMain, "server type 5", server(5u));
    own(kServerIdle, "server type 6", server(6u));
    own(kInit, "SVM_Init", [this](Cpu&) {
        if (refcount_ == 0u) {
            inside_ = {};
            lock_hook_ = {};
            unlock_hook_ = {};
            for (u32 t = 0; t < 6u; ++t) passes_[t] = 0u;  // the original clears types 0..5 only
        }
        ++refcount_;
    });
    own(kFinish, "SVM_Finish", [this](Cpu&) {
        if (--refcount_ == 0u) {
            inside_ = {};
            lock_hook_ = {};
            unlock_hook_ = {};
            for (u32 t = 0; t < 6u; ++t) passes_[t] = 0u;
        }
    });
    own(kSetCbLock, "SVM_SetCbLock", [this](Cpu& c) { lock_hook_ = {c.gpr[3], c.gpr[4]}; });
    own(kSetCbUnlock, "SVM_SetCbUnlock", [this](Cpu& c) { unlock_hook_ = {c.gpr[3], c.gpr[4]}; });
    own(kLock, "svm_lock", [this](Cpu& c) { begin(c, Op::Lock, {}); });
    own(kUnlock, "svm_unlock", [this](Cpu& c) { begin(c, Op::Unlock, {}); });
    own(kTestAndSet, "test-and-set", [this](Cpu& c) { begin(c, Op::TestAndSet, {c.gpr[3]}); });
    own(kSetUsrHook, "user hook set", [this](Cpu& c) { begin(c, Op::SetUsrHook, {c.gpr[3], c.gpr[4], c.gpr[5]}); });
    own(kCallUsrHook, "user hook call", [this](Cpu& c) {
        if (c.gpr[3] >= kUserHooks) {
            stop(c, "a user hook index " + std::to_string(c.gpr[3]) + " outside the table");
            return;
        }
        begin(c, Op::CallUsrHook, {c.gpr[3]});
    });
    own(kSetCbSvrId, "SVM_SetCbSvrId", [this](Cpu& c) {
        begin(c, Op::SetCbSvrId, {c.gpr[3], c.gpr[4], c.gpr[5], c.gpr[6], c.gpr[7]});
    });
    own(kDelCbSvr, "SVM_DelCbSvr", [this](Cpu& c) { begin(c, Op::DelCbSvr, {c.gpr[3], c.gpr[4]}); });
    own(kSetCbSvr, "SVM_SetCbSvr", [this](Cpu& c) { begin(c, Op::SetCbSvr, {c.gpr[3], c.gpr[4], c.gpr[5], c.gpr[6]}); });
    own(kError, "error report", [this](Cpu& c) {
        std::string text;
        for (u32 i = 0; i < 0x7Fu; ++i) {
            const u8 byte = c.mem_read8(c.gpr[3] + i);
            if (byte == 0u) break;
            text.push_back(static_cast<char>(byte));
        }
        report(text.c_str());
    });
    own(kErrorf, "formatted error report", [this](Cpu& c) {
        std::string text;
        for (u32 i = 0; i < 0x7Fu; ++i) {
            const u8 byte = c.mem_read8(c.gpr[3] + i);
            if (byte == 0u) break;
            text.push_back(static_cast<char>(byte));
        }
        report(text.c_str(), c.gpr[4], c.gpr[5]);
    });
    own(kReturn, "a guest call returned", [this](Cpu& c) { resume(c); });
}

void NativeSvm::save_state(std::vector<u8>& out) const {
    state::put<u32>(out, refcount_);
    state::put<u32>(out, lock_depth_);
    state::put<u32>(out, lock_type_);
    for (const Hook* h : {&lock_hook_, &unlock_hook_}) {
        state::put<u32>(out, h->function);
        state::put<u32>(out, h->argument);
    }
    for (const Hook& h : user_hooks_) {
        state::put<u32>(out, h.function);
        state::put<u32>(out, h.argument);
    }
    for (const auto& type : table_)
        for (const Slot& s : type) {
            state::put<u32>(out, s.function);
            state::put<u32>(out, s.argument);
            state::put<u32>(out, s.name);
        }
    for (const u32 v : inside_) state::put<u32>(out, v);
    for (const u32 v : passes_) state::put<u32>(out, v);
}

bool NativeSvm::load_state(const u8*& cursor, const u8* end) {
    const auto get = [&](u32& v) { return state::get<u32>(cursor, end, v); };
    if (!get(refcount_) || !get(lock_depth_) || !get(lock_type_)) return false;
    for (Hook* h : {&lock_hook_, &unlock_hook_})
        if (!get(h->function) || !get(h->argument)) return false;
    for (Hook& h : user_hooks_)
        if (!get(h.function) || !get(h.argument)) return false;
    for (auto& type : table_)
        for (Slot& s : type)
            if (!get(s.function) || !get(s.argument) || !get(s.name)) return false;
    for (u32& v : inside_)
        if (!get(v)) return false;
    for (u32& v : passes_)
        if (!get(v)) return false;
    return true;
}

// The original's layout of S (section 1 of docs/CRI_SERVERS.md).
void NativeSvm::import_guest(Cpu& c) {
    refcount_ = c.mem_read32(kBlock + 0x00u);
    lock_depth_ = c.mem_read32(kBlock + 0x04u);
    lock_type_ = c.mem_read32(kBlock + 0x08u);
    unlock_hook_ = {c.mem_read32(kBlock + 0xC8u), c.mem_read32(kBlock + 0xCCu)};
    lock_hook_ = {c.mem_read32(kBlock + 0xD0u), c.mem_read32(kBlock + 0xD4u)};
    for (u32 i = 0; i < kUserHooks; ++i)
        user_hooks_[i] = {c.mem_read32(kBlock + 0xD8u + 8u * i), c.mem_read32(kBlock + 0xDCu + 8u * i)};
    for (u32 t = 0; t < kTypes; ++t)
        for (u32 id = 0; id < kSlots; ++id) {
            const u32 at = kBlock + 0x118u + 0x48u * t + 0xCu * id;
            table_[t][id] = {c.mem_read32(at), c.mem_read32(at + 4u), c.mem_read32(at + 8u)};
        }
    for (u32 t = 0; t < kTypes; ++t) {
        inside_[t] = c.mem_read32(kBlock + 0x358u + 4u * t);
        passes_[t] = c.mem_read32(kBlock + 0x378u + 4u * t);
    }
}

}  // namespace gekko
