// liveness-roots: reference-interpreter-only
// CRI's server manager (gekko/native_svm.h) against the ORIGINAL words of the game's DOL, in the reference
// interpreter: one scripted sequence of SVM calls - registration (free slot, by id, overwrite, every illegal argument,
// a full type), the lock pair with traced hooks (balanced, nested, an unbalanced unlock), every server type with
// traced callbacks (results ORed; a callback that re-enters the SVM - registers a server and takes the lock - while
// its own server runs), deletion, the test-and-set on 0 / 1 / 2, user hooks (set, call, a null hook), Init / Finish
// reference counting - is run on a machine executing the original eighteen functions and on one whose SVM is
// NativeSvm. After every call both must agree on: the callback trace (which guest function ran, in which order, with
// which argument), the value the call returned (servers, test-and-set, SVM_SetCbSvr), and the SVM's state (the
// original's block S read from guest memory vs NativeSvm's getters: table with names, user hooks, lock hooks, lock
// depth and type, reference count, "inside" flags, pass counters). Also: NativeSvm::addresses() equals the native
// owners list for the SVM (extract/native-owners.txt section).
//
// usage: native_svm_tests <sys/main.dol> <extract/native-owners.txt>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_svm.h"

#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace gekko;

constexpr u32 kTestAndSet = 0x803CBFB8u, kFinish = 0x803CC0CCu, kInit = 0x803CC194u;
constexpr u32 kServer6 = 0x803CC24Cu, kServer5 = 0x803CC2DCu, kServer4 = 0x803CC36Cu, kServer2 = 0x803CC3FCu;
constexpr u32 kSetCbUnlock = 0x803CC48Cu, kSetCbLock = 0x803CC49Cu, kCallUsrHook = 0x803CC4ACu, kSetUsrHook = 0x803CC4F0u;
constexpr u32 kSetCbSvrId = 0x803CC5F4u, kDelCbSvr = 0x803CC7D0u, kSetCbSvr = 0x803CC970u;
constexpr u32 kUnlock = 0x803CCC5Cu, kLock = 0x803CCCECu;
constexpr u32 kS = NativeSvm::kBlock;

constexpr u32 kR13 = 0x805EC500u, kR2 = 0x805FA780u;
constexpr u32 kFinishPc = 0x817FF000u, kStack = 0x817E0000u;
constexpr u32 kCode = 0x81700000u, kTrace = 0x81710000u, kWords = 0x81720000u, kNames = 0x81730000u;

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
std::string hex(u32 v) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", v);
    return text;
}
int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// ---- PowerPC encodings for the scripted callbacks
u32 addi(u32 d, u32 a, s32 imm) { return (14u << 26) | (d << 21) | (a << 16) | (static_cast<u32>(imm) & 0xFFFFu); }
u32 addis(u32 d, u32 a, u32 imm) { return (15u << 26) | (d << 21) | (a << 16) | (imm & 0xFFFFu); }
u32 ori(u32 a, u32 s, u32 imm) { return (24u << 26) | (s << 21) | (a << 16) | (imm & 0xFFFFu); }
u32 oris(u32 a, u32 s, u32 imm) { return (25u << 26) | (s << 21) | (a << 16) | (imm & 0xFFFFu); }
u32 lwz(u32 d, s32 off, u32 a) { return (32u << 26) | (d << 21) | (a << 16) | (static_cast<u32>(off) & 0xFFFFu); }
u32 stw(u32 s, s32 off, u32 a) { return (36u << 26) | (s << 21) | (a << 16) | (static_cast<u32>(off) & 0xFFFFu); }
u32 stwu(u32 s, s32 off, u32 a) { return (37u << 26) | (s << 21) | (a << 16) | (static_cast<u32>(off) & 0xFFFFu); }
u32 mr(u32 a, u32 s) { return (31u << 26) | (s << 21) | (a << 16) | (s << 11) | (444u << 1); }
u32 add(u32 d, u32 a, u32 b) { return (31u << 26) | (d << 21) | (a << 16) | (b << 11) | (266u << 1); }
u32 rlwinm(u32 a, u32 s, u32 sh, u32 mb, u32 me) {
    return (21u << 26) | (s << 21) | (a << 16) | (sh << 11) | (mb << 6) | (me << 1);
}
u32 mflr(u32 d) { return (31u << 26) | (d << 21) | (8u << 16) | (339u << 1); }
u32 mtlr(u32 s) { return (31u << 26) | (s << 21) | (8u << 16) | (467u << 1); }
constexpr u32 kBlr = 0x4E800020u;
u32 bl(u32 from, u32 to) { return (18u << 26) | ((to - from) & 0x03FFFFFCu) | 1u; }

struct Machine {
    GuestMemory bus;
    Cpu cpu{bus};
    NativeSvm svm{cpu};
    bool native;
    u32 code_end = kCode;
    u32 cb_plain = 0, cb_lock = 0, cb_unlock = 0, cb_reenter = 0, cb_hook = 0;

    Machine(const std::vector<u8>& dol, bool native_svm) : native(native_svm) {
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (u32 i = 0; i < 18u; ++i) {
            const u32 offset = word(i * 4u), address = word(0x48u + i * 4u), size = word(0x90u + i * 4u);
            if (!size) continue;
            require(std::uint64_t(offset) + size <= dol.size(), "invalid DOL extent");
            bus.write_block(address, dol.data() + offset, size);
        }
        require(bus.read32(kServer2) == 0x9421FFD0u && bus.read32(kLock) == 0x9421FFF0u,
                "unexpected SVM instruction evidence");
        cpu.reset();
        cpu.gpr[2] = kR2;
        cpu.gpr[13] = kR13;
        cpu.set_msr(cpu.msr() | kMsrDR | kMsrIR | kMsrFP);
        bus.write32(kTrace, 0u);
        if (native) svm.install();
        build_callbacks();
    }

    std::vector<u32> w;
    u32 at = 0;
    u32 here() const { return at + static_cast<u32>(w.size()) * 4u; }
    void begin() {
        w.clear();
        at = code_end;
    }
    u32 end() {
        for (std::size_t i = 0; i < w.size(); ++i) bus.write32(at + static_cast<u32>(i) * 4u, w[i]);
        code_end = at + static_cast<u32>(w.size()) * 4u;
        return at;
    }
    void load(u32 reg, u32 value) {
        w.push_back(addis(reg, 0, value >> 16));
        w.push_back(ori(reg, reg, value & 0xFFFFu));
    }
    // trace[count++] = (tag << 24) | (reg & 0xFFFFFF); uses r4..r6 (and r3 is kept).
    void record(u32 reg, u32 tag) {
        load(4, kTrace);
        w.push_back(lwz(5, 0, 4));
        w.push_back(addi(6, 5, 1));
        w.push_back(stw(6, 0, 4));
        w.push_back(rlwinm(5, 5, 2, 0, 29));
        w.push_back(add(5, 5, 4));
        w.push_back(rlwinm(6, reg, 0, 8, 31));
        w.push_back(oris(6, 6, tag << 8));
        w.push_back(stw(6, 4, 5));
    }
    void build_callbacks() {
        // A server callback: records its argument, returns its argument's top half.
        begin();
        record(3, 0x01);
        w.push_back(rlwinm(3, 3, 16, 16, 31));
        w.push_back(kBlr);
        cb_plain = end();
        begin();
        record(3, 0x02);
        w.push_back(kBlr);
        cb_lock = end();
        begin();
        record(3, 0x03);
        w.push_back(kBlr);
        cb_unlock = end();
        begin();
        record(3, 0x04);
        load(3, 0x00770000u);
        w.push_back(kBlr);
        cb_hook = end();
        // A callback that re-enters the SVM while its server runs: registers a type 4 server (recording the id),
        // takes and leaves the lock, returns 0x40.
        begin();
        w.push_back(mflr(0));
        w.push_back(stw(0, 4, 1));
        w.push_back(stwu(1, -0x20, 1));
        w.push_back(stw(31, 0x1C, 1));
        w.push_back(mr(31, 3));
        record(3, 0x05);
        w.push_back(addi(3, 0, 4));
        load(4, cb_plain);
        load(5, 0x00080009u);
        w.push_back(addi(6, 0, 0));
        w.push_back(bl(here(), kSetCbSvr));
        record(3, 0x06);
        w.push_back(bl(here(), kLock));
        w.push_back(bl(here(), kUnlock));
        w.push_back(addi(3, 0, 0x40));
        w.push_back(lwz(31, 0x1C, 1));
        w.push_back(addi(1, 1, 0x20));
        w.push_back(lwz(0, 4, 1));
        w.push_back(mtlr(0));
        w.push_back(kBlr);
        cb_reenter = end();
    }

    // Call a function as a game thread; true when it returned. The native machine serves its owners as the
    // dispatcher does (a handler that did not leave resumes at LR).
    bool call(u32 entry, std::initializer_list<u32> args, u32* result = nullptr) {
        u32 r = 3u;
        for (const u32 v : args) cpu.gpr[r++] = v;
        cpu.gpr[1] = kStack;
        cpu.lr = kFinishPc;
        cpu.pc = entry;
        u64 steps = 0;
        while (cpu.pc != kFinishPc && !cpu.stopped() && steps++ < 5'000'000ull) {
            decltype(&cpu.hle_entries().front()) hook = nullptr;
            if (native)
                for (const auto& e : cpu.hle_entries())
                    if (e.address == cpu.pc) hook = &e;
            if (hook != nullptr) {
                const u32 before = cpu.pc;
                if (!hook->handler(cpu)) return false;  // owners never decline
                if (cpu.pc == before) cpu.pc = cpu.npc = cpu.lr;
                continue;
            }
            cpu.run_steps(1u);
        }
        if (result) *result = cpu.gpr[3];
        return cpu.pc == kFinishPc && !cpu.stopped() && cpu.gpr[1] == kStack;
    }

    std::vector<u32> trace() {
        std::vector<u32> out(bus.read32(kTrace));
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = bus.read32(kTrace + 4u + static_cast<u32>(i) * 4u);
        return out;
    }

    // The SVM's state in one comparable form.
    std::string state() {
        std::ostringstream s;
        const auto put = [&](const char* k, u32 v) { s << k << '=' << hex(v) << ' '; };
        if (native) {
            put("ref", svm.refcount());
            put("depth", svm.lock_depth());
            put("type", svm.lock_type());
            put("lock", svm.lock_hook().function);
            put("lockarg", svm.lock_hook().argument);
            put("unlock", svm.unlock_hook().function);
            put("unlockarg", svm.unlock_hook().argument);
            for (u32 i = 0; i < 8u; ++i) {
                put("uh", svm.user_hook(i).function);
                put("ua", svm.user_hook(i).argument);
            }
            for (u32 t = 0; t < 8u; ++t)
                for (u32 id = 0; id < 6u; ++id) {
                    put("fn", svm.slot(t, id).function);
                    put("arg", svm.slot(t, id).argument);
                    put("name", svm.slot(t, id).name);
                }
            for (u32 t = 0; t < 8u; ++t) put("inside", svm.inside(t));
            for (u32 t = 0; t < 8u; ++t) put("pass", svm.passes(t));
        } else {
            put("ref", bus.read32(kS));
            put("depth", bus.read32(kS + 4u));
            put("type", bus.read32(kS + 8u));
            put("lock", bus.read32(kS + 0xD0u));
            put("lockarg", bus.read32(kS + 0xD4u));
            put("unlock", bus.read32(kS + 0xC8u));
            put("unlockarg", bus.read32(kS + 0xCCu));
            for (u32 i = 0; i < 8u; ++i) {
                put("uh", bus.read32(kS + 0xD8u + 8u * i));
                put("ua", bus.read32(kS + 0xDCu + 8u * i));
            }
            for (u32 t = 0; t < 8u; ++t)
                for (u32 id = 0; id < 6u; ++id) {
                    const u32 a = kS + 0x118u + 0x48u * t + 0xCu * id;
                    put("fn", bus.read32(a));
                    put("arg", bus.read32(a + 4u));
                    put("name", bus.read32(a + 8u));
                }
            for (u32 t = 0; t < 8u; ++t) put("inside", bus.read32(kS + 0x358u + 4u * t));
            for (u32 t = 0; t < 8u; ++t) put("pass", bus.read32(kS + 0x378u + 4u * t));
        }
        return s.str();
    }
};

std::vector<u8> read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return std::vector<u8>{std::istreambuf_iterator<char>(input), {}};
}

struct Step {
    std::string what;
    u32 entry;
    std::vector<u32> args;
    bool returns;  // the call's r3 is part of the contract
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::printf("usage: native_svm_tests <sys/main.dol> <extract/native-owners.txt>\n");
        return 2;
    }
    try {
        const std::vector<u8> dol = read_file(argv[1]);
        require(dol.size() > 0x100u, "cannot read the DOL");
        Machine original(dol, false), native(dol, true);
        require(original.cb_reenter == native.cb_reenter, "callback layout differs");
        const u32 plain = original.cb_plain, lockf = original.cb_lock, unlockf = original.cb_unlock,
                  reenter = original.cb_reenter, hook = original.cb_hook;
        for (Machine* m : {&original, &native}) {
            m->bus.write32(kWords + 0u, 0u);
            m->bus.write32(kWords + 4u, 1u);
            m->bus.write32(kWords + 8u, 2u);
        }
        const std::vector<Step> steps = {
            {"Init", kInit, {}, false},
            {"Init (refcount 2)", kInit, {}, false},
            {"server 2, empty table", kServer2, {}, true},
            {"lock with no hook (no depth change)", kLock, {}, false},
            {"unlock with no hook", kUnlock, {}, false},
            {"SetCbLock", kSetCbLock, {lockf, 0xAu}, false},
            {"SetCbUnlock", kSetCbUnlock, {unlockf, 0xBu}, false},
            {"SetCbSvr type 2 (id 0)", kSetCbSvr, {2u, plain, 0x00010001u, kNames}, true},
            {"SetCbSvr type 2, no name (id 1)", kSetCbSvr, {2u, plain, 0x00000002u, 0u}, true},
            {"SetCbSvrId type 2 id 4", kSetCbSvrId, {2u, 4u, plain, 0x00020003u, kNames + 8u}, false},
            {"SetCbSvrId type 2 id 4 again (overwrite)", kSetCbSvrId, {2u, 4u, plain, 0x00040004u, 0u}, false},
            {"SetCbSvrId illegal type 9", kSetCbSvrId, {9u, 0u, plain, 1u, 0u}, false},
            {"SetCbSvrId illegal id 7", kSetCbSvrId, {2u, 7u, plain, 1u, 0u}, false},
            {"SetCbSvrId illegal id -1", kSetCbSvrId, {2u, 0xFFFFFFFFu, plain, 1u, 0u}, false},
            {"SetCbSvr illegal type -2", kSetCbSvr, {0xFFFFFFFEu, plain, 1u, 0u}, true},
            {"server 2: ids 0, 1, 4", kServer2, {}, true},
            {"SetCbSvr type 6 re-entrant", kSetCbSvr, {6u, reenter, 0x00000005u, 0u}, true},
            {"server 6: callback registers a type 4 server and locks", kServer6, {}, true},
            {"server 4: the server registered from inside server 6", kServer4, {}, true},
            {"SetCbSvr type 5 #1", kSetCbSvr, {5u, plain, 0x01000001u, 0u}, true},
            {"SetCbSvr type 5 #2", kSetCbSvr, {5u, plain, 0x02000002u, 0u}, true},
            {"SetCbSvr type 5 #3", kSetCbSvr, {5u, plain, 0x04000003u, 0u}, true},
            {"SetCbSvr type 5 #4", kSetCbSvr, {5u, plain, 0x08000004u, 0u}, true},
            {"SetCbSvr type 5 #5", kSetCbSvr, {5u, plain, 0x10000005u, 0u}, true},
            {"SetCbSvr type 5 #6", kSetCbSvr, {5u, plain, 0x20000006u, 0u}, true},
            {"SetCbSvr type 5 #7 (too many: -1)", kSetCbSvr, {5u, plain, 0x40000007u, 0u}, true},
            {"server 5: six callbacks", kServer5, {}, true},
            {"DelCbSvr type 2 id 0", kDelCbSvr, {2u, 0u}, false},
            {"DelCbSvr illegal id 9", kDelCbSvr, {2u, 9u}, false},
            {"DelCbSvr illegal type 8", kDelCbSvr, {8u, 0u}, false},
            {"server 2 after deletion", kServer2, {}, true},
            {"lock", kLock, {}, false},
            {"lock nested", kLock, {}, false},
            {"unlock", kUnlock, {}, false},
            {"unlock", kUnlock, {}, false},
            {"unlock unbalanced", kUnlock, {}, false},
            {"lock after the unbalanced unlock", kLock, {}, false},
            {"test-and-set on 0", kTestAndSet, {kWords + 0u}, true},
            {"test-and-set on 1", kTestAndSet, {kWords + 4u}, true},
            {"test-and-set on 2", kTestAndSet, {kWords + 8u}, true},
            {"test-and-set on (now) 1", kTestAndSet, {kWords + 0u}, true},
            {"SetUsrHook 6", kSetUsrHook, {6u, hook, 0x66u}, false},
            {"CallUsrHook 6", kCallUsrHook, {6u}, true},
            {"CallUsrHook 3 (null hook)", kCallUsrHook, {3u}, true},
            {"Finish (refcount 1)", kFinish, {}, false},
            {"Finish (refcount 0: clears)", kFinish, {}, false},
            {"server 2 after Finish (table kept)", kServer2, {}, true},
            {"Init again", kInit, {}, false},
        };
        for (const Step& step : steps) {
            u32 r_original = 0, r_native = 0;
            const auto run = [&](Machine& m, u32& r) {
                bool ok = false;
                switch (step.args.size()) {
                    case 0: ok = m.call(step.entry, {}, &r); break;
                    case 1: ok = m.call(step.entry, {step.args[0]}, &r); break;
                    case 2: ok = m.call(step.entry, {step.args[0], step.args[1]}, &r); break;
                    case 3: ok = m.call(step.entry, {step.args[0], step.args[1], step.args[2]}, &r); break;
                    case 4: ok = m.call(step.entry, {step.args[0], step.args[1], step.args[2], step.args[3]}, &r); break;
                    default:
                        ok = m.call(step.entry, {step.args[0], step.args[1], step.args[2], step.args[3], step.args[4]}, &r);
                }
                return ok;
            };
            const bool ok_original = run(original, r_original);
            const bool ok_native = run(native, r_native);
            check(ok_original, step.what + ": the original returned");
            check(ok_native, step.what + ": the native SVM returned");
            if (step.returns) check(r_original == r_native, step.what + ": result " + hex(r_original) + " vs " + hex(r_native));
            const std::vector<u32> t_original = original.trace(), t_native = native.trace();
            check(t_original == t_native, step.what + ": callback trace");
            check(original.state() == native.state(), step.what + ": SVM state");
            for (u32 i = 0; i < 3u; ++i)
                check(original.bus.read32(kWords + 4u * i) == native.bus.read32(kWords + 4u * i), step.what + ": words");
            std::printf("%-58s r3 %s  trace %zu\n", step.what.c_str(), hex(r_native).c_str(), t_native.size());
        }
        // The owners list.
        std::set<u32> listed;
        {
            std::ifstream owners(argv[2]);
            std::string line;
            bool in_section = false;
            while (std::getline(owners, line)) {
                if (line.find("runtime/src/gekko/native_svm.cpp") != std::string::npos) in_section = true;
                else if (in_section && !line.empty() && line[0] == '#' && line.find("native_svm") == std::string::npos &&
                         line.size() > 2 && line.find("runtime/src/") != std::string::npos) in_section = false;
                if (!in_section || line.empty() || line[0] == '#') continue;
                listed.insert(static_cast<u32>(std::stoul(line.substr(0, 10), nullptr, 16)));
            }
        }
        std::size_t count = 0;
        const NativeSvm::Address* a = NativeSvm::addresses(count);
        std::set<u32> owned;
        for (std::size_t i = 0; i < count; ++i) owned.insert(a[i].address);
        check(listed == owned, "NativeSvm::addresses() equals the SVM section of native-owners.txt");
    } catch (const std::exception& error) {
        std::printf("ERROR: %s\n", error.what());
        return 1;
    }
    std::printf("native SVM: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
