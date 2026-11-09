// liveness-roots: reference-interpreter-only
// The guest-call seam (gekko/native_guest_call.h) in the reference build: the interpreter runs the nested call.
// The guest code is written into memory by the test (a few instructions, and the two leaf patterns of guest_call_cases.h). The
// translated-code path of the seam is the production library's: guest_call_translated_tests runs generated game leaves of the same
// patterns, found in the recompiled table by their words.
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/native_guest_call.h"
#include "guest_call_cases.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
using namespace gekko;
constexpr u32 kAdd = 0x81700000u, kClobber = 0x81700100u, kLoop = 0x81700200u, kLeaf = 0x81700300u, kLink = 0x81700F00u, kYielding = 0x81700400u, kScribble = 0x81700500u, kStack = 0x817E0000u;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void put(GuestMemory& m, u32 address, std::initializer_list<u32> words) {
    for (u32 w : words) { m.write32(address, w); address += 4; }
}
}  // namespace

int main() {
    try {
        GuestMemory memory;
        Cpu cpu{memory};
        put(memory, kAdd, {0x7C632214u, 0x4E800020u});                       // add r3,r3,r4 ; blr
        put(memory, kClobber, {0x7C0802A6u, 0x90010004u, 0x39C00005u, 0x38600009u,   // mflr r0 ; stw r0,4(r1) ; li r14,5 ; li r3,9
                               0x80010004u, 0x7C0803A6u, 0x4E800020u});      // lwz r0,4(r1) ; mtlr r0 ; blr
        put(memory, kLoop, {0x48000000u});                                   // b .
        const auto setup = [&]() {
            cpu.reset();
            cpu.gpr[1] = kStack; cpu.gpr[2] = 0x805FA780u; cpu.gpr[13] = 0x805EC500u; cpu.gpr[14] = 0xAAAA0014u; cpu.gpr[31] = 0xAAAA001Fu;
            cpu.lr = 0x81701234u; cpu.ctr = 0x81705678u; cpu.cr = 0x12345678u; cpu.pc = 0x81702000u; cpu.npc = 0x81702004u;
            memory.write32(kStack + 4u, 0xFEEDFACEu);  // the caller's own LR slot: the callee must not touch it
        };
        const auto unchanged = [&]() {
            return cpu.gpr[1] == kStack && cpu.gpr[14] == 0xAAAA0014u && cpu.gpr[31] == 0xAAAA001Fu && cpu.lr == 0x81701234u &&
                   cpu.ctr == 0x81705678u && cpu.cr == 0x12345678u && cpu.pc == 0x81702000u && memory.read32(kStack + 4u) == 0xFEEDFACEu;
        };
        u32 r = 0;
        setup();
        const bool ok = guest_call(cpu, kAdd, kLink, {7u, 35u}, r);
        if (!ok || r != 42u)
            std::fprintf(stderr, "ok=%d r=%u pc=%08X stopped=%d %s\n", ok, r, cpu.pc, cpu.stopped(), cpu.stop().detail.c_str());
        require(ok && r == 42u, "the nested add did not return 42");
        require(unchanged(), "the call changed the caller's registers or LR slot");
        setup();
        require(guest_call(cpu, kClobber, kLink, {}, r) && r == 9u, "the clobbering callee's result");
        require(unchanged(), "the callee's r14 or LR save leaked into the caller");
        // The same two leaf patterns as the translated-branch test (guest_call_translated_tests), here as words in the interpreter.
        for (u32 k : guest_call_cases::kImmediates) {
            memory.write32(kLeaf, guest_call_cases::kLiR3 | (k & 0xFFFFu));
            memory.write32(kLeaf + 4u, guest_call_cases::kBlr);
            setup();
            r = 0xDEADBEEFu;
            require(guest_call(cpu, kLeaf, kLink, {guest_call_cases::kFirstArgument, guest_call_cases::kSecondArgument}, r) &&
                        r == guest_call_cases::sign_extend16(k), "the interpreted `li r3,K ; blr` returned a different r3");
            require(unchanged(), "the interpreted call changed the caller's registers or LR slot");
        }
        memory.write32(kLeaf, guest_call_cases::kMrR3R4);
        memory.write32(kLeaf + 4u, guest_call_cases::kBlr);
        setup();
        r = 0xDEADBEEFu;
        require(guest_call(cpu, kLeaf, kLink, {guest_call_cases::kFirstArgument, guest_call_cases::kSecondArgument}, r) &&
                    r == guest_call_cases::kSecondArgument, "the interpreted `mr r3,r4 ; blr` returned a different r3");
        require(unchanged(), "the interpreted call changed the caller's registers or LR slot");
        // A callee that writes into its own frame (which lies just below the r1 guest_call hands it): stwu r1,-0x40(r1) ;
        // stw r4,0x20(r1) ; addi r1,r1,0x40 ; blr. The word it writes is at (caller r1 - 0x10) - 0x40 + 0x20 = caller r1 - 0x30.
        put(memory, kScribble, {0x9421FFC0u, 0x90810020u, 0x38210040u, 0x4E800020u});
        {
            // Negative control: a temporary just below the owner's r1 is overwritten by the callee (the exposure GuestFrame removes).
            setup();
            memory.write32(kStack - 0x30u, 0x11111111u);
            require(guest_call(cpu, kScribble, kLink, {0u, 0xBADBAD00u}, r), "the scribbling callee failed");
            require(memory.read32(kStack - 0x30u) == 0xBADBAD00u, "the negative control did not show the overwrite");
            // With a GuestFrame the temporary sits above the callee's frame and survives; r1 is restored when the frame ends.
            setup();
            {
                GuestFrame frame(cpu, 0x40u);
                for (u32 i = 0; i < 0x40u; i += 4u) memory.write32(frame.base() + i, 0xA5A50000u + i);
                require(frame.base() % 16u == 0u && frame.base() + 0x40u <= kStack && cpu.gpr[1] < frame.base(), "GuestFrame layout");
                require(memory.read32(cpu.gpr[1]) == kStack, "GuestFrame back chain");
                require(guest_call(cpu, kScribble, kLink, {0u, 0xBADBAD00u}, r), "the scribbling callee failed under a GuestFrame");
                for (u32 i = 0; i < 0x40u; i += 4u) require(memory.read32(frame.base() + i) == 0xA5A50000u + i, "a callee overwrote a GuestFrame temporary");
            }
            require(unchanged(), "GuestFrame did not restore r1");
        }
        // A callee that yields (leave_to_dispatcher, as OSYieldThread's hand-over to the dispatcher does) is the documented limit:
        // the call faults, loudly, and writes no result.
        cpu.install_hle(kYielding, [](Cpu& c) {
            c.leave_to_dispatcher(0x81700F80u, FallbackReason::Yield, c.lr);
            return true;
        }, "yielding callee");
        setup();
        r = 0xDEADBEEFu;
        require(!guest_call(cpu, kYielding, kLink, {1u}, r), "a callee that yielded returned");
        require(cpu.stopped() && cpu.stop().reason == StopReason::ExecutionFault, "a yielding callee did not raise an ExecutionFault");
        require(cpu.stop().detail.find("did not return to its link") != std::string::npos, "the yield fault names the wrong cause");
        require(r == 0xDEADBEEFu, "a yielding callee wrote a result");
        // A faulting call under a GuestFrame: r1 is restored by the destructor on that path too.
        setup();
        {
            GuestFrame frame(cpu, 0x10u);
            require(!guest_call(cpu, kYielding, kLink, {1u}, r), "the yielding callee returned under a GuestFrame");
        }
        require(cpu.gpr[1] == kStack, "GuestFrame did not restore r1 after a faulting call");
        // The bound is loud: the run is stopped with an ExecutionFault, the result register is not written.
        setup();
        r = 0xDEADBEEFu;
        require(!guest_call(cpu, kLoop, kLink, {}, r, 1000u), "an endless callee returned");
        require(cpu.stopped() && cpu.stop().reason == StopReason::ExecutionFault, "the step bound did not raise an ExecutionFault");
        require(r == 0xDEADBEEFu, "a failed call wrote a result");
        setup();
        r = 1u;
        require(!guest_call(cpu, kAdd, kLink, {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u}, r) && cpu.stopped(), "nine arguments were accepted");
        std::printf("PASS guest call: add / clobber / bound; registers, LR slot and pc restored; step bound (default %llu) -> ExecutionFault, no result\n",
                    static_cast<unsigned long long>(kGuestCallStepBound));
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL guest_call_tests: %s\n", e.what());
        return 1;
    }
}
