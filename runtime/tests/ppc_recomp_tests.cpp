// Equivalence tests for the semantics shared by the interpreter and the
// statically recompiled code.
//
// gekko/ppc_ops.h is what tools/ppcrecomp emits calls to. If it drifts from
// cpu.cpp by one carry bit or one condition-register encoding, recompiled game
// code diverges from interpreted game code in ways that surface thousands of
// instructions later. So every operation is run both ways here, over randomised
// register state, and the entire architected result is compared: all 32 general
// registers, all 32 floating-point registers including the second paired-single
// slot, CR, XER, LR, CTR and FPSCR.
//
// The interpreter is the reference: it executes the real encoding. The helper
// side is invoked exactly the way the generator emits it.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "support/os_entry.h"
#include "gekko/ppc_instruction_traits.h"
#include "gekko/ppc_ops.h"

using namespace gekko;

namespace {

int g_failures = 0;
int g_checks = 0;

constexpr u32 kCodeEffective = 0x80001000u;

// ---- instruction encoding --------------------------------------------------

u32 d_form(u32 op, u32 d, u32 a, u32 imm) {
    return (op << 26) | (d << 21) | (a << 16) | (imm & 0xffffu);
}

u32 x_form(u32 op, u32 d, u32 a, u32 b, u32 xo, bool rc, bool oe = false) {
    return (op << 26) | (d << 21) | (a << 16) | (b << 11) | ((oe ? 1u : 0u) << 10) | (xo << 1) |
           (rc ? 1u : 0u);
}

u32 m_form(u32 op, u32 s, u32 a, u32 sh, u32 mb, u32 me, bool rc) {
    return (op << 26) | (s << 21) | (a << 16) | (sh << 11) | (mb << 6) | (me << 1) | (rc ? 1u : 0u);
}

u32 a_form(u32 op, u32 d, u32 a, u32 b, u32 c, u32 xo, bool rc) {
    return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo << 1) | (rc ? 1u : 0u);
}

// ---- state comparison ------------------------------------------------------

struct State {
    u32 gpr[32];
    u64 fpr[32][2];
    u32 cr;
    u32 xer;
    u32 lr;
    u32 ctr;
    u32 fpscr;
};

State capture(const Cpu& cpu) {
    State state{};
    for (int index = 0; index < 32; ++index) {
        state.gpr[index] = cpu.gpr[index];
        state.fpr[index][0] = cpu.fpr[index].bits[0];
        state.fpr[index][1] = cpu.fpr[index].bits[1];
    }
    state.cr = cpu.cr;
    state.xer = cpu.xer;
    state.lr = cpu.lr;
    state.ctr = cpu.ctr;
    state.fpscr = cpu.fpscr;
    return state;
}

std::string describe(const State& a, const State& b) {
    char buffer[256];
    for (int index = 0; index < 32; ++index) {
        if (a.gpr[index] != b.gpr[index]) {
            std::snprintf(buffer, sizeof(buffer), "r%d: interpreter 0x%08X helper 0x%08X", index,
                          a.gpr[index], b.gpr[index]);
            return buffer;
        }
    }
    for (int index = 0; index < 32; ++index) {
        if (a.fpr[index][0] != b.fpr[index][0]) {
            std::snprintf(buffer, sizeof(buffer), "f%d ps0: interpreter 0x%016llX helper 0x%016llX",
                          index, static_cast<unsigned long long>(a.fpr[index][0]),
                          static_cast<unsigned long long>(b.fpr[index][0]));
            return buffer;
        }
        if (a.fpr[index][1] != b.fpr[index][1]) {
            std::snprintf(buffer, sizeof(buffer), "f%d ps1: interpreter 0x%016llX helper 0x%016llX",
                          index, static_cast<unsigned long long>(a.fpr[index][1]),
                          static_cast<unsigned long long>(b.fpr[index][1]));
            return buffer;
        }
    }
    if (a.cr != b.cr) {
        std::snprintf(buffer, sizeof(buffer), "CR: interpreter 0x%08X helper 0x%08X", a.cr, b.cr);
        return buffer;
    }
    if (a.xer != b.xer) {
        std::snprintf(buffer, sizeof(buffer), "XER: interpreter 0x%08X helper 0x%08X", a.xer, b.xer);
        return buffer;
    }
    if (a.lr != b.lr) {
        std::snprintf(buffer, sizeof(buffer), "LR: interpreter 0x%08X helper 0x%08X", a.lr, b.lr);
        return buffer;
    }
    if (a.ctr != b.ctr) {
        std::snprintf(buffer, sizeof(buffer), "CTR: interpreter 0x%08X helper 0x%08X", a.ctr, b.ctr);
        return buffer;
    }
    if (a.fpscr != b.fpscr) {
        std::snprintf(buffer, sizeof(buffer), "FPSCR: interpreter 0x%08X helper 0x%08X", a.fpscr,
                      b.fpscr);
        return buffer;
    }
    return std::string();
}

bool equal(const State& a, const State& b) { return describe(a, b).empty(); }

// ---- the harness -----------------------------------------------------------

struct Machine {
    GuestMemory bus;
    Cpu cpu{bus};
};

// Two machines are enough for the whole suite. A GuestMemory maps 24 MiB of
// game memory, so building a fresh pair per iteration would dominate the
// run time without testing anything extra.
Machine& interpreted_machine() {
    static Machine machine;
    return machine;
}

Machine& helper_machine() {
    static Machine machine;
    return machine;
}

void seed_state(Cpu& cpu, std::mt19937& random) {
    for (int index = 0; index < 32; ++index) {
        cpu.gpr[index] = random();
        // Values around the sign boundary and around zero exercise carry,
        // overflow and the comparison encodings far more than uniform noise.
        switch (index % 6) {
            case 0: cpu.gpr[index] = 0u; break;
            case 1: cpu.gpr[index] = 0xffffffffu; break;
            case 2: cpu.gpr[index] = 0x80000000u; break;
            case 3: cpu.gpr[index] = 0x7fffffffu; break;
            default: break;
        }
        const double value = static_cast<double>(static_cast<s32>(random())) / 65536.0;
        cpu.fpr[index].bits[0] = double_to_bits(value);
        cpu.fpr[index].bits[1] = double_to_bits(-value * 0.5);
    }
    cpu.cr = random();
    cpu.xer = random() & 0xe000007fu;
    cpu.lr = random() & ~3u;
    cpu.ctr = random();
    cpu.fpscr = random() & 0xfffff800u;
}

// Runs one encoding through the interpreter and through the helper form and
// compares. `apply` is written the way tools/ppcrecomp emits the instruction.
void check(const char* name, u32 encoding, const std::function<void(Cpu&)>& apply,
           unsigned iterations = 64, u32 fpscr_set = 0u) {
    ++g_checks;
    std::mt19937 random(0x5eed0000u ^ encoding);
    for (unsigned iteration = 0; iteration < iterations; ++iteration) {
        Machine& interpreted = interpreted_machine();
        Machine& helped = helper_machine();
        interpreted.cpu.reset();
        helped.cpu.reset();
        interpreted.cpu.clear_stop();
        helped.cpu.clear_stop();

        std::mt19937 shared = random;
        seed_state(interpreted.cpu, shared);
        shared = random;
        seed_state(helped.cpu, shared);
        static_cast<void>(random());
        interpreted.cpu.fpscr |= fpscr_set;
        helped.cpu.fpscr |= fpscr_set;

        interpreted.bus.write32(kCodeEffective, encoding);
        interpreted.cpu.set_entry(kCodeEffective);
        interpreted.cpu.step();
        if (interpreted.cpu.stopped()) {
            std::printf("FAIL %-12s interpreter refused the encoding: %s\n", name,
                        interpreted.cpu.stop().detail.c_str());
            ++g_failures;
            return;
        }

        apply(helped.cpu);

        const State a = capture(interpreted.cpu);
        const State b = capture(helped.cpu);
        if (!equal(a, b)) {
            std::printf("FAIL %-12s (0x%08X) iteration %u: %s\n", name, encoding, iteration,
                        describe(a, b).c_str());
            ++g_failures;
            return;
        }
    }
    std::printf("ok   %-12s 0x%08X\n", name, encoding);
}

// ---- cases -----------------------------------------------------------------

void integer_arithmetic() {
    const u32 d = 5, a = 7, b = 9;

    check("add", x_form(31, d, a, b, 266, false),
          [=](Cpu& c) { c.gpr[d] = c.gpr[a] + c.gpr[b]; });
    check("add.", x_form(31, d, a, b, 266, true), [=](Cpu& c) {
        c.gpr[d] = c.gpr[a] + c.gpr[b];
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("addo.", x_form(31, d, a, b, 266, true, true), [=](Cpu& c) {
        c.gpr[d] = ppc::add_oe(c, c.gpr[a], c.gpr[b]);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("addc.", x_form(31, d, a, b, 10, true), [=](Cpu& c) {
        c.gpr[d] = ppc::addc(c, c.gpr[a], c.gpr[b], false);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("addco.", x_form(31, d, a, b, 10, true, true), [=](Cpu& c) {
        c.gpr[d] = ppc::addc(c, c.gpr[a], c.gpr[b], true);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("adde.", x_form(31, d, a, b, 138, true), [=](Cpu& c) {
        c.gpr[d] = ppc::adde(c, c.gpr[a], c.gpr[b], false);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("addeo.", x_form(31, d, a, b, 138, true, true), [=](Cpu& c) {
        c.gpr[d] = ppc::adde(c, c.gpr[a], c.gpr[b], true);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("addze.", x_form(31, d, a, 0, 202, true), [=](Cpu& c) {
        c.gpr[d] = ppc::addze(c, c.gpr[a], false);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("addzeo.", x_form(31, d, a, 0, 202, true, true), [=](Cpu& c) {
        c.gpr[d] = ppc::addze(c, c.gpr[a], true);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("addme.", x_form(31, d, a, 0, 234, true), [=](Cpu& c) {
        c.gpr[d] = ppc::addme(c, c.gpr[a], false);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("addmeo.", x_form(31, d, a, 0, 234, true, true), [=](Cpu& c) {
        c.gpr[d] = ppc::addme(c, c.gpr[a], true);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("subf.", x_form(31, d, a, b, 40, true), [=](Cpu& c) {
        c.gpr[d] = c.gpr[b] - c.gpr[a];
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("subfo.", x_form(31, d, a, b, 40, true, true), [=](Cpu& c) {
        c.gpr[d] = ppc::subf_oe(c, c.gpr[a], c.gpr[b]);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("subfc.", x_form(31, d, a, b, 8, true), [=](Cpu& c) {
        c.gpr[d] = ppc::subfc(c, c.gpr[a], c.gpr[b], false);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("subfco.", x_form(31, d, a, b, 8, true, true), [=](Cpu& c) {
        c.gpr[d] = ppc::subfc(c, c.gpr[a], c.gpr[b], true);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("subfe.", x_form(31, d, a, b, 136, true), [=](Cpu& c) {
        c.gpr[d] = ppc::subfe(c, c.gpr[a], c.gpr[b], false);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("subfeo.", x_form(31, d, a, b, 136, true, true), [=](Cpu& c) {
        c.gpr[d] = ppc::subfe(c, c.gpr[a], c.gpr[b], true);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("subfze.", x_form(31, d, a, 0, 200, true), [=](Cpu& c) {
        c.gpr[d] = ppc::subfze(c, c.gpr[a], false);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("subfzeo.", x_form(31, d, a, 0, 200, true, true), [=](Cpu& c) {
        c.gpr[d] = ppc::subfze(c, c.gpr[a], true);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("subfme.", x_form(31, d, a, 0, 232, true), [=](Cpu& c) {
        c.gpr[d] = ppc::subfme(c, c.gpr[a], false);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("subfmeo.", x_form(31, d, a, 0, 232, true, true), [=](Cpu& c) {
        c.gpr[d] = ppc::subfme(c, c.gpr[a], true);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("neg.", x_form(31, d, a, 0, 104, true), [=](Cpu& c) {
        c.gpr[d] = 0u - c.gpr[a];
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("nego.", x_form(31, d, a, 0, 104, true, true), [=](Cpu& c) {
        c.gpr[d] = ppc::neg_oe(c, c.gpr[a]);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("mullw.", x_form(31, d, a, b, 235, true), [=](Cpu& c) {
        c.gpr[d] = ppc::mullw(c, c.gpr[a], c.gpr[b], false);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("mullwo.", x_form(31, d, a, b, 235, true, true), [=](Cpu& c) {
        c.gpr[d] = ppc::mullw(c, c.gpr[a], c.gpr[b], true);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("mulhw.", x_form(31, d, a, b, 75, true), [=](Cpu& c) {
        c.gpr[d] = ppc::mulhw(c.gpr[a], c.gpr[b]);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("mulhwu.", x_form(31, d, a, b, 11, true), [=](Cpu& c) {
        c.gpr[d] = ppc::mulhwu(c.gpr[a], c.gpr[b]);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("divw.", x_form(31, d, a, b, 491, true), [=](Cpu& c) {
        c.gpr[d] = ppc::divw(c, c.gpr[a], c.gpr[b], false);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("divwo.", x_form(31, d, a, b, 491, true, true), [=](Cpu& c) {
        c.gpr[d] = ppc::divw(c, c.gpr[a], c.gpr[b], true);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("divwu.", x_form(31, d, a, b, 459, true), [=](Cpu& c) {
        c.gpr[d] = ppc::divwu(c, c.gpr[a], c.gpr[b], false);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("divwuo.", x_form(31, d, a, b, 459, true, true), [=](Cpu& c) {
        c.gpr[d] = ppc::divwu(c, c.gpr[a], c.gpr[b], true);
        ppc::update_cr0(c, c.gpr[d]);
    });

    check("addi", d_form(14, d, a, 0xf001u),
          [=](Cpu& c) { c.gpr[d] = c.gpr[a] + 0xfffff001u; });
    check("addis", d_form(15, d, a, 0x1234u),
          [=](Cpu& c) { c.gpr[d] = c.gpr[a] + 0x12340000u; });
    check("addic", d_form(12, d, a, 0xf001u),
          [=](Cpu& c) { c.gpr[d] = ppc::addic(c, c.gpr[a], 0xfffff001u); });
    check("addic.", d_form(13, d, a, 0x0123u), [=](Cpu& c) {
        c.gpr[d] = ppc::addic(c, c.gpr[a], 0x00000123u);
        ppc::update_cr0(c, c.gpr[d]);
    });
    check("subfic", d_form(8, d, a, 0xf001u),
          [=](Cpu& c) { c.gpr[d] = ppc::subfic(c, c.gpr[a], 0xfffff001u); });
    check("mulli", d_form(7, d, a, 0xfffbu),
          [=](Cpu& c) { c.gpr[d] = ppc::mulli(c.gpr[a], -5); });
}

void logical_and_shift() {
    const u32 s = 5, a = 7, b = 9;

    auto simple = [&](const char* name, u32 encoding, std::function<u32(Cpu&)> value) {
        check(name, encoding, [=](Cpu& c) {
            c.gpr[a] = value(c);
            ppc::update_cr0(c, c.gpr[a]);
        });
    };

    simple("and.", x_form(31, s, a, b, 28, true), [=](Cpu& c) { return c.gpr[s] & c.gpr[b]; });
    simple("andc.", x_form(31, s, a, b, 60, true), [=](Cpu& c) { return c.gpr[s] & ~c.gpr[b]; });
    simple("or.", x_form(31, s, a, b, 444, true), [=](Cpu& c) { return c.gpr[s] | c.gpr[b]; });
    simple("orc.", x_form(31, s, a, b, 412, true), [=](Cpu& c) { return c.gpr[s] | ~c.gpr[b]; });
    simple("xor.", x_form(31, s, a, b, 316, true), [=](Cpu& c) { return c.gpr[s] ^ c.gpr[b]; });
    simple("nand.", x_form(31, s, a, b, 476, true), [=](Cpu& c) { return ~(c.gpr[s] & c.gpr[b]); });
    simple("nor.", x_form(31, s, a, b, 124, true), [=](Cpu& c) { return ~(c.gpr[s] | c.gpr[b]); });
    simple("eqv.", x_form(31, s, a, b, 284, true), [=](Cpu& c) { return ~(c.gpr[s] ^ c.gpr[b]); });
    simple("slw.", x_form(31, s, a, b, 24, true),
           [=](Cpu& c) { return ppc::slw(c.gpr[s], c.gpr[b]); });
    simple("srw.", x_form(31, s, a, b, 536, true),
           [=](Cpu& c) { return ppc::srw(c.gpr[s], c.gpr[b]); });
    simple("cntlzw.", x_form(31, s, a, 0, 26, true),
           [=](Cpu& c) { return ppc::cntlzw(c.gpr[s]); });
    simple("extsb.", x_form(31, s, a, 0, 954, true), [=](Cpu& c) {
        return static_cast<u32>(static_cast<s32>(static_cast<s8>(c.gpr[s])));
    });
    simple("extsh.", x_form(31, s, a, 0, 922, true), [=](Cpu& c) {
        return static_cast<u32>(static_cast<s32>(static_cast<s16>(c.gpr[s])));
    });

    check("sraw.", x_form(31, s, a, b, 792, true), [=](Cpu& c) {
        c.gpr[a] = ppc::sraw(c, c.gpr[s], c.gpr[b]);
        ppc::update_cr0(c, c.gpr[a]);
    });
    for (u32 shift : {0u, 1u, 15u, 31u}) {
        check("srawi.", x_form(31, s, a, shift, 824, true), [=](Cpu& c) {
            c.gpr[a] = ppc::srawi(c, c.gpr[s], shift);
            ppc::update_cr0(c, c.gpr[a]);
        });
    }

    // rA == rS (audit sqrt-ca-lead-1/-2). The generator emits
    // `c.gpr[a] = ppc::srawi(c, c.gpr[a], n)`, which reads rS by value; the
    // interpreter used to write rA first and then take CA from the already
    // shifted register. r11/r10 are seeded with uniform random values, so
    // both the sign and the shifted-out bits vary per iteration.
    {
        const u32 same = 11, shift_reg = 10;
        check("sraw. rA=rS", x_form(31, same, same, shift_reg, 792, true), [=](Cpu& c) {
            c.gpr[same] = ppc::sraw(c, c.gpr[same], c.gpr[shift_reg]);
            ppc::update_cr0(c, c.gpr[same]);
        });
        for (u32 shift : {0u, 1u, 15u, 31u}) {
            check("srawi. rA=rS", x_form(31, same, same, shift, 824, true), [=](Cpu& c) {
                c.gpr[same] = ppc::srawi(c, c.gpr[same], shift);
                ppc::update_cr0(c, c.gpr[same]);
            });
        }
    }

    check("ori", d_form(24, s, a, 0xbeefu), [=](Cpu& c) { c.gpr[a] = c.gpr[s] | 0xbeefu; });
    check("oris", d_form(25, s, a, 0xbeefu), [=](Cpu& c) { c.gpr[a] = c.gpr[s] | 0xbeef0000u; });
    check("xori", d_form(26, s, a, 0xbeefu), [=](Cpu& c) { c.gpr[a] = c.gpr[s] ^ 0xbeefu; });
    check("xoris", d_form(27, s, a, 0xbeefu), [=](Cpu& c) { c.gpr[a] = c.gpr[s] ^ 0xbeef0000u; });
    check("andi.", d_form(28, s, a, 0xbeefu), [=](Cpu& c) {
        c.gpr[a] = c.gpr[s] & 0xbeefu;
        ppc::update_cr0(c, c.gpr[a]);
    });
    check("andis.", d_form(29, s, a, 0xbeefu), [=](Cpu& c) {
        c.gpr[a] = c.gpr[s] & 0xbeef0000u;
        ppc::update_cr0(c, c.gpr[a]);
    });

    // Rotate-and-mask: the mask is what the generator folds to a literal, so
    // every interesting mb/me relationship is worth covering, including the
    // wrapped case where me < mb.
    const u32 patterns[][3] = {{0, 0, 31}, {4, 0, 27}, {16, 16, 31}, {8, 20, 5}, {31, 31, 31}};
    for (const auto& pattern : patterns) {
        const u32 sh = pattern[0], mb = pattern[1], me = pattern[2];
        const u32 mask = ppc::mask_from(mb, me);
        check("rlwinm.", m_form(21, s, a, sh, mb, me, true), [=](Cpu& c) {
            c.gpr[a] = ppc::rotl32(c.gpr[s], sh) & mask;
            ppc::update_cr0(c, c.gpr[a]);
        });
        check("rlwimi.", m_form(20, s, a, sh, mb, me, true), [=](Cpu& c) {
            c.gpr[a] = (ppc::rotl32(c.gpr[s], sh) & mask) | (c.gpr[a] & ~mask);
            ppc::update_cr0(c, c.gpr[a]);
        });
        check("rlwnm.", m_form(23, s, a, b, mb, me, true), [=](Cpu& c) {
            c.gpr[a] = ppc::rotl32(c.gpr[s], c.gpr[b] & 31u) & mask;
            ppc::update_cr0(c, c.gpr[a]);
        });
    }
}

void condition_register() {
    const u32 a = 7, b = 9, d = 5;

    check("cmp", x_form(31, 3 << 2, a, b, 0, false), [=](Cpu& c) {
        ppc::cmp(c, 3u, static_cast<s32>(c.gpr[a]), static_cast<s32>(c.gpr[b]));
    });
    check("cmpl", x_form(31, 5 << 2, a, b, 32, false),
          [=](Cpu& c) { ppc::cmpl(c, 5u, c.gpr[a], c.gpr[b]); });
    check("cmpi", d_form(11, 2 << 2, a, 0xfff0u),
          [=](Cpu& c) { ppc::cmp(c, 2u, static_cast<s32>(c.gpr[a]), -16); });
    check("cmpli", d_form(10, 6 << 2, a, 0xfff0u),
          [=](Cpu& c) { ppc::cmpl(c, 6u, c.gpr[a], 0xfff0u); });

    check("mfcr", x_form(31, d, 0, 0, 19, false), [=](Cpu& c) { c.gpr[d] = c.cr; });
    check("mtcrf", (31u << 26) | (d << 21) | (0xa5u << 12) | (144u << 1), [=](Cpu& c) {
        const u32 mask = ppc::field_mask8(0xa5u);
        c.cr = (c.cr & ~mask) | (c.gpr[d] & mask);
    });
    check("mcrf", (19u << 26) | ((2u << 2) << 21) | ((5u << 2) << 16),
          [=](Cpu& c) { ppc::set_cr_field(c, 2u, ppc::cr_field(c, 5u)); });
    check("mcrxr", x_form(31, 4 << 2, 0, 0, 512, false), [=](Cpu& c) {
        ppc::set_cr_field(c, 4u, (c.xer >> 28) & 0xfu);
        c.xer &= 0x0fffffffu;
    });

    struct CrOp {
        const char* name;
        u32 xo;
        std::function<u32(u32, u32)> apply;
    };
    const std::vector<CrOp> operations = {
        {"crnor", 33, [](u32 x, u32 y) { return ~(x | y) & 1u; }},
        {"crandc", 129, [](u32 x, u32 y) { return x & ~y & 1u; }},
        {"crxor", 193, [](u32 x, u32 y) { return x ^ y; }},
        {"crnand", 225, [](u32 x, u32 y) { return ~(x & y) & 1u; }},
        {"crand", 257, [](u32 x, u32 y) { return x & y; }},
        {"creqv", 289, [](u32 x, u32 y) { return ~(x ^ y) & 1u; }},
        {"crorc", 417, [](u32 x, u32 y) { return (x | ~y) & 1u; }},
        {"cror", 449, [](u32 x, u32 y) { return x | y; }},
    };
    for (const CrOp& operation : operations) {
        const u32 bt = 6, ba = 11, bb = 20;
        const auto apply = operation.apply;
        check(operation.name, x_form(19, bt, ba, bb, operation.xo, false), [=](Cpu& c) {
            const u32 x = (c.cr >> (31u - ba)) & 1u;
            const u32 y = (c.cr >> (31u - bb)) & 1u;
            ppc::set_cr_bit(c, bt, apply(x, y));
        });
    }
}

void floating_point() {
    const u32 d = 3, a = 6, b = 8, c_reg = 11;

    // Written the way tools/ppcrecomp emits each instruction (audit fpu-1/-2/-3:
    // every arithmetic form goes through a ppc::fp_* / fps_* helper). Run once
    // with the seeded FPSCR (NI clear) and once with FPSCR[NI] forced on.
    struct Op {
        const char* name;
        u32 primary;
        u32 xo;
        std::function<u64(Cpu&)> value;
    };
    const auto A = [=](Cpu& c) { return c.fpr[a].bits[0]; };
    const auto B = [=](Cpu& c) { return c.fpr[b].bits[0]; };
    const auto C = [=](Cpu& c) { return c.fpr[c_reg].bits[0]; };
    const std::vector<Op> singles = {
        {"fdivs", 59, 18, [=](Cpu& c) { return ppc::fps_div(c, A(c), B(c)); }},
        {"fsubs", 59, 20, [=](Cpu& c) { return ppc::fps_sub(c, A(c), B(c)); }},
        {"fadds", 59, 21, [=](Cpu& c) { return ppc::fps_add(c, A(c), B(c)); }},
        {"fsqrts", 59, 22, [=](Cpu& c) { return ppc::fps_sqrt(c, B(c)); }},
        {"fres", 59, 24, [=](Cpu& c) { return ppc::fps_res(c, B(c)); }},
        {"fmuls", 59, 25, [=](Cpu& c) { return ppc::fps_mul(c, A(c), C(c)); }},
        {"fmsubs", 59, 28, [=](Cpu& c) { return ppc::fps_msub(c, A(c), C(c), B(c)); }},
        {"fmadds", 59, 29, [=](Cpu& c) { return ppc::fps_madd(c, A(c), C(c), B(c)); }},
        {"fnmsubs", 59, 30, [=](Cpu& c) { return ppc::fps_nmsub(c, A(c), C(c), B(c)); }},
        {"fnmadds", 59, 31, [=](Cpu& c) { return ppc::fps_nmadd(c, A(c), C(c), B(c)); }},
    };
    for (const u32 ni : {0u, kFpscrNI}) {
        for (const Op& operation : singles) {
            const auto value = operation.value;
            check(operation.name, a_form(operation.primary, d, a, b, c_reg, operation.xo, false),
                  [=](Cpu& c) {
                      const u64 result = value(c);
                      c.fpr[d].bits[0] = result;
                      c.fpr[d].bits[1] = result;
                  },
                  64, ni);
        }
    }

    const std::vector<Op> doubles = {
        {"fdiv", 63, 18, [=](Cpu& c) { return ppc::fp_div(c, A(c), B(c)); }},
        {"fsub", 63, 20, [=](Cpu& c) { return ppc::fp_sub(c, A(c), B(c)); }},
        {"fadd", 63, 21, [=](Cpu& c) { return ppc::fp_add(c, A(c), B(c)); }},
        {"fsqrt", 63, 22, [=](Cpu& c) { return ppc::fp_sqrt(c, B(c)); }},
        {"fmul", 63, 25, [=](Cpu& c) { return ppc::fp_mul(c, A(c), C(c)); }},
        {"frsqrte", 63, 26, [=](Cpu& c) { return ppc::fp_rsqrte(c, B(c)); }},
        {"fmsub", 63, 28, [=](Cpu& c) { return ppc::fp_msub(c, A(c), C(c), B(c)); }},
        {"fmadd", 63, 29, [=](Cpu& c) { return ppc::fp_madd(c, A(c), C(c), B(c)); }},
        {"fnmsub", 63, 30, [=](Cpu& c) { return ppc::fp_nmsub(c, A(c), C(c), B(c)); }},
        {"fnmadd", 63, 31, [=](Cpu& c) { return ppc::fp_nmadd(c, A(c), C(c), B(c)); }},
        {"fsel", 63, 23, [=](Cpu& c) {
             const double va = bits_to_double(c.fpr[a].bits[0]);
             return double_to_bits((va >= 0.0 && !std::isnan(va)) ? bits_to_double(c.fpr[c_reg].bits[0])
                                                                  : bits_to_double(c.fpr[b].bits[0]));
         }},
    };
    for (const u32 ni : {0u, kFpscrNI}) {
        for (const Op& operation : doubles) {
            const auto value = operation.value;
            check(operation.name, a_form(operation.primary, d, a, b, c_reg, operation.xo, false),
                  [=](Cpu& c) { c.fpr[d].bits[0] = value(c); }, 64, ni);
        }
        check("frsp", x_form(63, d, 0, b, 12, false),
              [=](Cpu& c) { c.fpr[d].bits[0] = ppc::fps_round(c, c.fpr[b].bits[0]); }, 64, ni);
        check("fctiwz", x_form(63, d, 0, b, 15, false),
              [=](Cpu& c) { c.fpr[d].bits[0] = ppc::fctiw(c, c.fpr[b].bits[0], true); }, 64, ni);
        check("fctiw", x_form(63, d, 0, b, 14, false),
              [=](Cpu& c) { c.fpr[d].bits[0] = ppc::fctiw(c, c.fpr[b].bits[0], false); }, 64, ni);
    }
    check("fmr", x_form(63, d, 0, b, 72, false),
          [=](Cpu& c) { c.fpr[d].bits[0] = c.fpr[b].bits[0]; });
    check("fneg", x_form(63, d, 0, b, 40, false),
          [=](Cpu& c) { c.fpr[d].bits[0] = c.fpr[b].bits[0] ^ 0x8000000000000000ull; });
    check("fabs", x_form(63, d, 0, b, 264, false),
          [=](Cpu& c) { c.fpr[d].bits[0] = c.fpr[b].bits[0] & 0x7fffffffffffffffull; });
    check("fnabs", x_form(63, d, 0, b, 136, false),
          [=](Cpu& c) { c.fpr[d].bits[0] = c.fpr[b].bits[0] | 0x8000000000000000ull; });
    check("fcmpu", x_form(63, 4 << 2, a, b, 0, false), [=](Cpu& c) {
        ppc::fp_compare(c, 4u, c.fpr[a].bits[0], c.fpr[b].bits[0], false);
    });
    check("fcmpo", x_form(63, 4 << 2, a, b, 32, false), [=](Cpu& c) {
        ppc::fp_compare(c, 4u, c.fpr[a].bits[0], c.fpr[b].bits[0], true);
    });
    check("mffs", x_form(63, d, 0, 0, 583, false),
          [=](Cpu& c) { c.fpr[d].bits[0] = 0xfff8000000000000ull | c.fpscr; });
}

void paired_singles() {
    const u32 d = 3, a = 6, b = 8, c_reg = 11;

    using Half = std::function<u64(Cpu&)>;
    auto pair_case = [&](const char* name, u32 xo, Half first, Half second, u32 ni) {
        check(name, a_form(4, d, a, b, c_reg, xo, false), [=](Cpu& c) {
            const u64 v0 = first(c);
            const u64 v1 = second(c);
            c.fpr[d].bits[0] = v0;
            c.fpr[d].bits[1] = v1;
        }, 64, ni);
    };
    const auto A0 = [=](Cpu& c) { return c.fpr[a].bits[0]; };
    const auto A1 = [=](Cpu& c) { return c.fpr[a].bits[1]; };
    const auto B0 = [=](Cpu& c) { return c.fpr[b].bits[0]; };
    const auto B1 = [=](Cpu& c) { return c.fpr[b].bits[1]; };
    const auto C0 = [=](Cpu& c) { return c.fpr[c_reg].bits[0]; };
    const auto C1 = [=](Cpu& c) { return c.fpr[c_reg].bits[1]; };

    for (const u32 ni : {0u, kFpscrNI}) {
        pair_case("ps_add", 21, [=](Cpu& c) { return ppc::fps_add(c, A0(c), B0(c)); },
                  [=](Cpu& c) { return ppc::fps_add(c, A1(c), B1(c)); }, ni);
        pair_case("ps_sub", 20, [=](Cpu& c) { return ppc::fps_sub(c, A0(c), B0(c)); },
                  [=](Cpu& c) { return ppc::fps_sub(c, A1(c), B1(c)); }, ni);
        pair_case("ps_mul", 25, [=](Cpu& c) { return ppc::fps_mul(c, A0(c), C0(c)); },
                  [=](Cpu& c) { return ppc::fps_mul(c, A1(c), C1(c)); }, ni);
        pair_case("ps_div", 18, [=](Cpu& c) { return ppc::fps_div(c, A0(c), B0(c)); },
                  [=](Cpu& c) { return ppc::fps_div(c, A1(c), B1(c)); }, ni);
        pair_case("ps_res", 24, [=](Cpu& c) { return ppc::fps_res(c, B0(c)); },
                  [=](Cpu& c) { return ppc::fps_res(c, B1(c)); }, ni);
        pair_case("ps_rsqrte", 26, [=](Cpu& c) { return ppc::fps_rsqrte(c, B0(c)); },
                  [=](Cpu& c) { return ppc::fps_rsqrte(c, B1(c)); }, ni);
        pair_case("ps_madd", 29, [=](Cpu& c) { return ppc::fps_madd(c, A0(c), C0(c), B0(c)); },
                  [=](Cpu& c) { return ppc::fps_madd(c, A1(c), C1(c), B1(c)); }, ni);
        pair_case("ps_msub", 28, [=](Cpu& c) { return ppc::fps_msub(c, A0(c), C0(c), B0(c)); },
                  [=](Cpu& c) { return ppc::fps_msub(c, A1(c), C1(c), B1(c)); }, ni);
        pair_case("ps_nmadd", 31, [=](Cpu& c) { return ppc::fps_nmadd(c, A0(c), C0(c), B0(c)); },
                  [=](Cpu& c) { return ppc::fps_nmadd(c, A1(c), C1(c), B1(c)); }, ni);
        pair_case("ps_nmsub", 30, [=](Cpu& c) { return ppc::fps_nmsub(c, A0(c), C0(c), B0(c)); },
                  [=](Cpu& c) { return ppc::fps_nmsub(c, A1(c), C1(c), B1(c)); }, ni);
        pair_case("ps_madds0", 14, [=](Cpu& c) { return ppc::fps_madd(c, A0(c), C0(c), B0(c)); },
                  [=](Cpu& c) { return ppc::fps_madd(c, A1(c), C0(c), B1(c)); }, ni);
        pair_case("ps_madds1", 15, [=](Cpu& c) { return ppc::fps_madd(c, A0(c), C1(c), B0(c)); },
                  [=](Cpu& c) { return ppc::fps_madd(c, A1(c), C1(c), B1(c)); }, ni);
        pair_case("ps_muls0", 12, [=](Cpu& c) { return ppc::fps_mul(c, A0(c), C0(c)); },
                  [=](Cpu& c) { return ppc::fps_mul(c, A1(c), C0(c)); }, ni);
        pair_case("ps_muls1", 13, [=](Cpu& c) { return ppc::fps_mul(c, A0(c), C1(c)); },
                  [=](Cpu& c) { return ppc::fps_mul(c, A1(c), C1(c)); }, ni);
        // ps_sum0/ps_sum1/ps_sel move frC/frB halves as bits (PAGE 528/530/531);
        // written the way tools/ppcrecomp emits them.
        check("ps_sum0", a_form(4, d, a, b, c_reg, 10, false), [=](Cpu& c) {
            const u64 sum = ppc::fps_add(c, A0(c), B1(c));
            c.fpr[d].bits[1] = c.fpr[c_reg].bits[1];
            c.fpr[d].bits[0] = sum;
        }, 64, ni);
        check("ps_sum1", a_form(4, d, a, b, c_reg, 11, false), [=](Cpu& c) {
            const u64 sum = ppc::fps_add(c, A0(c), B1(c));
            c.fpr[d].bits[0] = c.fpr[c_reg].bits[0];
            c.fpr[d].bits[1] = sum;
        }, 64, ni);
    }
    check("ps_sel", a_form(4, d, a, b, c_reg, 23, false), [=](Cpu& c) {
        const u64 first = bits_to_double(c.fpr[a].bits[0]) >= 0.0 ? c.fpr[c_reg].bits[0] : c.fpr[b].bits[0];
        c.fpr[d].bits[1] = bits_to_double(c.fpr[a].bits[1]) >= 0.0 ? c.fpr[c_reg].bits[1] : c.fpr[b].bits[1];
        c.fpr[d].bits[0] = first;
    });

    check("ps_merge01", x_form(4, d, a, b, 560, false), [=](Cpu& c) {
        const u64 first = c.fpr[a].bits[0];
        c.fpr[d].bits[1] = c.fpr[b].bits[1];
        c.fpr[d].bits[0] = first;
    });
    check("ps_merge10", x_form(4, d, a, b, 592, false), [=](Cpu& c) {
        const u64 first = c.fpr[a].bits[1];
        c.fpr[d].bits[1] = c.fpr[b].bits[0];
        c.fpr[d].bits[0] = first;
    });
    check("ps_mr", x_form(4, d, 0, b, 72, false), [=](Cpu& c) {
        const u64 first = c.fpr[b].bits[0];
        c.fpr[d].bits[1] = c.fpr[b].bits[1];
        c.fpr[d].bits[0] = first;
    });
    check("ps_cmpu0", x_form(4, 2 << 2, a, b, 0, false), [=](Cpu& c) {
        ppc::fp_compare(c, 2u, c.fpr[a].bits[0], c.fpr[b].bits[0], false);
    });
    check("ps_cmpo0", x_form(4, 2 << 2, a, b, 32, false), [=](Cpu& c) {
        ppc::fp_compare(c, 2u, c.fpr[a].bits[0], c.fpr[b].bits[0], true);
    });
    check("ps_cmpu1", x_form(4, 2 << 2, a, b, 64, false), [=](Cpu& c) {
        ppc::fp_compare(c, 2u, c.fpr[a].bits[1], c.fpr[b].bits[1], false);
    });
    check("ps_cmpo1", x_form(4, 2 << 2, a, b, 96, false), [=](Cpu& c) {
        ppc::fp_compare(c, 2u, c.fpr[a].bits[1], c.fpr[b].bits[1], true);
    });
}

constexpr u64 kQNaN = 0x7FF8000000000000ull;
constexpr u64 kNegQNaN = 0xFFF8000000000000ull;
constexpr u64 kSNaN = 0x7FF4000000000000ull;

// Audit fpu-5 / paired-single-9: every FP compare against the manual's RTL
// (PAGE 397/398 fcmpo/fcmpu, 503-506 ps_cmpo0/1, ps_cmpu0/1), through the
// reference interpreter AND ppc::fp_compare. Expected FPSCR/CR values are
// written out by hand; engine agreement alone let the old defects survive
// (FX on a QNaN fcmpu, no VXSNAN/VXVC, FPSCR[C] cleared, ps_cmp* never writing
// FPCC).
void fp_compare_against_manual() {
    enum class Op { Fcmpu, Fcmpo, PsCmpu0, PsCmpo0, PsCmpu1, PsCmpo1 };
    struct Case {
        const char* name;
        Op op;
        u64 a0, a1, b0, b1;  // ps0/ps1 of frA and frB
        u32 fpscr_in;
        u32 code;
        u32 fpscr_out;
    };
    const u64 one = double_to_bits(1.0), two = double_to_bits(2.0);
    const u64 neg_zero = 0x8000000000000000ull, pos_zero = 0u;
    const Case cases[] = {
        // The audit's scenario: C (0x00010000) survives, FPCC = FU, no FX.
        {"fcmpu QNaN keeps C, no exception", Op::Fcmpu, kQNaN, 0, one, 0, 0x00010000u, 1u, 0x00011000u},
        {"fcmpo QNaN sets VXVC FX VX", Op::Fcmpo, kQNaN, 0, one, 0, 0u, 1u, 0xA0081000u},
        {"fcmpu SNaN sets VXSNAN FX VX", Op::Fcmpu, one, 0, kSNaN, 0, 0u, 1u, 0xA1001000u},
        {"fcmpo SNaN VE=0 sets VXSNAN VXVC", Op::Fcmpo, kSNaN, 0, one, 0, 0u, 1u, 0xA1081000u},
        {"fcmpo SNaN VE=1: VXSNAN only, FEX", Op::Fcmpo, kSNaN, 0, one, 0, 0x00000080u, 1u, 0xE1001080u},
        {"fcmpo QNaN, VXVC already set: FX stays 0", Op::Fcmpo, kQNaN, 0, kQNaN, 0, 0x20080000u, 1u,
         0x20081000u},
        {"fcmpo QNaN with OX&OE: FEX from OE", Op::Fcmpo, kNegQNaN, 0, one, 0, 0x10000040u, 1u, 0xF0081040u},
        {"fcmpu less keeps C, replaces FPCC", Op::Fcmpu, one, 0, two, 0, 0x0001F000u, 8u, 0x00018000u},
        {"fcmpo greater", Op::Fcmpo, two, 0, one, 0, 0x80000000u, 4u, 0x80004000u},
        {"fcmpo -0 == +0", Op::Fcmpo, neg_zero, 0, pos_zero, 0, 0u, 2u, 0x00002000u},
        // paired-single-9: ps_cmpo0 1.0 < 2.0 must write FPCC = FL.
        {"ps_cmpo0 less writes FPCC", Op::PsCmpo0, one, kQNaN, two, one, 0u, 8u, 0x00008000u},
        {"ps_cmpu0 SNaN frB", Op::PsCmpu0, one, one, kSNaN, one, 0x00010000u, 1u, 0xA1011000u},
        {"ps_cmpu0 QNaN no exception", Op::PsCmpu0, kQNaN, one, one, one, 0u, 1u, 0x00001000u},
        {"ps_cmpo0 QNaN VXVC", Op::PsCmpo0, kQNaN, one, one, one, 0u, 1u, 0xA0081000u},
        {"ps_cmpu1 uses ps1 (greater)", Op::PsCmpu1, kQNaN, two, kQNaN, one, 0u, 4u, 0x00004000u},
        {"ps_cmpo1 QNaN in ps1", Op::PsCmpo1, one, kQNaN, two, one, 0u, 1u, 0xA0081000u},
        {"ps_cmpo1 SNaN VE=1", Op::PsCmpo1, one, kSNaN, one, one, 0x00000080u, 1u, 0xE1001080u},
    };
    const u32 a = 6, b = 8, field = 3;
    for (const Case& test : cases) {
        ++g_checks;
        const bool paired = test.op != Op::Fcmpu && test.op != Op::Fcmpo;
        const u32 xo = (test.op == Op::Fcmpo || test.op == Op::PsCmpo0) ? 32u
                     : test.op == Op::PsCmpu1                             ? 64u
                     : test.op == Op::PsCmpo1                             ? 96u
                                                                         : 0u;
        const bool ordered = xo == 32u || xo == 96u;
        const bool high = xo >= 64u;
        Machine& interpreted = interpreted_machine();
        Machine& helped = helper_machine();
        for (Machine* machine : {&interpreted, &helped}) {
            Cpu& c = machine->cpu;
            c.reset();
            c.clear_stop();
            c.fpr[a].bits[0] = test.a0;
            c.fpr[a].bits[1] = test.a1;
            c.fpr[b].bits[0] = test.b0;
            c.fpr[b].bits[1] = test.b1;
            c.fpscr = test.fpscr_in;
            c.cr = 0x12345678u;
        }
        interpreted.bus.write32(kCodeEffective, x_form(paired ? 4u : 63u, field << 2, a, b, xo, false));
        interpreted.cpu.set_entry(kCodeEffective);
        interpreted.cpu.step();
        ppc::fp_compare(helped.cpu, field, high ? helped.cpu.fpr[a].bits[1] : helped.cpu.fpr[a].bits[0],
                        high ? helped.cpu.fpr[b].bits[1] : helped.cpu.fpr[b].bits[0], ordered);
        const u32 expected_cr = (0x12345678u & ~(0xFu << (28u - 4u * field))) |
                                (test.code << (28u - 4u * field));
        bool ok = true;
        for (Machine* machine : {&interpreted, &helped}) {
            const Cpu& c = machine->cpu;
            if (c.stopped() || c.fpscr != test.fpscr_out || c.cr != expected_cr ||
                c.fpr[a].bits[0] != test.a0 || c.fpr[b].bits[1] != test.b1) {
                std::printf("FAIL %s (%s): fpscr 0x%08X cr 0x%08X, expected fpscr 0x%08X cr 0x%08X\n",
                            test.name, machine == &interpreted ? "interpreter" : "helper", c.fpscr,
                            c.cr, test.fpscr_out, expected_cr);
                ++g_failures;
                ok = false;
            }
        }
        if (ok) std::printf("ok   %s\n", test.name);
    }
}

// Audit paired-single-6: ps_sel and the copied halves of ps_sum0/ps_sum1 are
// moves (PAGE 528, 530, 531). A signalling NaN and a double that is not a
// single must come through bit for bit; the old engines quieted the SNaN
// (0x7FF4... -> 0x7FFC...) and rounded 0.1 to 0x3FB99999A0000000.
void paired_moves_against_manual() {
    const u64 point_one = 0x3FB999999999999Aull;  // 0.1, not representable as a single
    const u64 one = double_to_bits(1.0), two = double_to_bits(2.0), half = double_to_bits(0.5);
    const u64 neg_zero = 0x8000000000000000ull, minus_one = double_to_bits(-1.0);
    const u64 single_sum = double_to_bits(static_cast<double>(static_cast<float>(1.0 + 0.1)));
    struct Case {
        const char* name;
        u32 xo, d, a, b, c;
        u64 fa[2], fb[2], fc[2];
        u64 expect[2];
    };
    const Case cases[] = {
        // ps_sum0 f3,f1,f5,f2: ps0 = round(f1.ps0 + f2.ps1), ps1 = f5.ps1 moved.
        {"ps_sum0 moves an SNaN", 10, 3, 1, 2, 5, {one, 0}, {0, half}, {0, kSNaN}, {double_to_bits(1.5), kSNaN}},
        {"ps_sum0 moves a non-single", 10, 3, 1, 2, 5, {one, 0}, {0, point_one}, {0, point_one},
         {single_sum, point_one}},
        {"ps_sum0 frD = frB", 10, 2, 1, 2, 5, {one, 0}, {two, half}, {0, point_one},
         {double_to_bits(1.5), point_one}},
        {"ps_sum1 moves an SNaN", 11, 3, 1, 2, 5, {one, 0}, {0, half}, {kSNaN, 0}, {kSNaN, double_to_bits(1.5)}},
        {"ps_sum1 frD = frC", 11, 5, 1, 2, 5, {one, 0}, {0, point_one}, {point_one, two},
         {point_one, single_sum}},
        // ps_sel f3,f1,f5,f2: frA >= 0 (incl. -0) picks frC, < 0 or NaN picks frB.
        {"ps_sel moves SNaN / non-single", 23, 3, 1, 2, 5, {one, minus_one}, {two, point_one},
         {kSNaN, half}, {kSNaN, point_one}},
        {"ps_sel -0 picks frC, NaN picks frB", 23, 3, 1, 2, 5, {neg_zero, kQNaN}, {two, point_one},
         {point_one, half}, {point_one, point_one}},
        {"ps_sel frD = frA", 23, 1, 1, 2, 5, {minus_one, one}, {kSNaN, two}, {half, point_one},
         {kSNaN, point_one}},
    };
    Machine& machine = interpreted_machine();
    Cpu& c = machine.cpu;
    for (const Case& test : cases) {
        ++g_checks;
        c.reset();
        c.clear_stop();
        // Register order matters when frD aliases an operand: frA first, then
        // frB, then frC, each overwriting the previous for a shared register.
        c.fpr[test.a].bits[0] = test.fa[0];
        c.fpr[test.a].bits[1] = test.fa[1];
        if (test.b != test.a) {
            c.fpr[test.b].bits[0] = test.fb[0];
            c.fpr[test.b].bits[1] = test.fb[1];
        }
        if (test.c != test.a && test.c != test.b) {
            c.fpr[test.c].bits[0] = test.fc[0];
            c.fpr[test.c].bits[1] = test.fc[1];
        }
        machine.bus.write32(kCodeEffective, a_form(4, test.d, test.a, test.b, test.c, test.xo, false));
        c.set_entry(kCodeEffective);
        c.step();
        if (c.stopped() || c.fpr[test.d].bits[0] != test.expect[0] ||
            c.fpr[test.d].bits[1] != test.expect[1]) {
            std::printf("FAIL %s: f%u = 0x%016llX 0x%016llX, expected 0x%016llX 0x%016llX\n", test.name,
                        test.d, static_cast<unsigned long long>(c.fpr[test.d].bits[0]),
                        static_cast<unsigned long long>(c.fpr[test.d].bits[1]),
                        static_cast<unsigned long long>(test.expect[0]),
                        static_cast<unsigned long long>(test.expect[1]));
            ++g_failures;
            continue;
        }
        std::printf("ok   %s\n", test.name);
    }
}

// The exact failures behind the __ieee754_sqrt oracle stop (audit
// sqrt-ca-lead-1/-2), as fixed two-instruction programs so they fail
// deterministically: `srawi rX,rX,n ; addze rX,rX` is the signed divide by 2^n
// idiom (420 of the DOL's 1,234 srawi have rA == rS, 104 followed by addze).
void aliased_shift_carry_programs() {
    struct Case {
        const char* name;
        u32 first;
        u32 second;
        u32 r0, r3, r4;
        u32 expect_r0, expect_r3;
        bool expect_carry;
    };
    const u32 srawi_r0_r0_1 = x_form(31, 0, 0, 1, 824, false);
    const u32 addze_r0_r0 = x_form(31, 0, 0, 0, 202, false);
    const u32 sraw_r3_r3_r4 = x_form(31, 3, 3, 4, 792, false);
    const u32 nop = d_form(24, 0, 0, 0);  // ori r0,r0,0
    const Case cases[] = {
        // 0x803B2F34 in __ieee754_sqrt: r0 = 0x3F9 - 0x3FF. The shifted-out bit
        // of 0xFFFFFFFA is 0, so CA = 0; the interpreter used to test bit 0 of
        // the shifted result 0xFFFFFFFD and set CA.
        {"srawi r0,r0,1 (sqrt)", srawi_r0_r0_1, nop, 0xFFFFFFFAu, 0, 0, 0xFFFFFFFDu, 0, false},
        // -2 / 2 = -1: CA must be 0, so addze leaves -1 (was 0).
        {"srawi+addze -2/2", srawi_r0_r0_1, addze_r0_r0, 0xFFFFFFFEu, 0, 0, 0xFFFFFFFFu, 0, false},
        // -3 / 2 rounds toward zero: CA = 1, addze gives -1 (was -2).
        {"srawi+addze -3/2", srawi_r0_r0_1, addze_r0_r0, 0xFFFFFFFDu, 0, 0, 0xFFFFFFFFu, 0, true},
        {"sraw r3,r3,r4 CA=0", sraw_r3_r3_r4, nop, 0, 0xFFFFFFFAu, 1u, 0, 0xFFFFFFFDu, false},
        {"sraw r3,r3,r4 CA=1", sraw_r3_r3_r4, nop, 0, 0xFFFFFFFDu, 1u, 0, 0xFFFFFFFEu, true},
    };
    for (const Case& test : cases) {
        ++g_checks;
        Machine& machine = interpreted_machine();
        machine.cpu.reset();
        machine.cpu.clear_stop();
        machine.cpu.xer = 0;
        machine.cpu.gpr[0] = test.r0;
        machine.cpu.gpr[3] = test.r3;
        machine.cpu.gpr[4] = test.r4;
        machine.bus.write32(kCodeEffective, test.first);
        machine.bus.write32(kCodeEffective + 4u, test.second);
        machine.cpu.set_entry(kCodeEffective);
        machine.cpu.step();
        const bool carry_after_shift = (machine.cpu.xer & kXerCA) != 0u;
        machine.cpu.step();
        const bool second_is_addze = test.second == addze_r0_r0;
        const bool ok = !machine.cpu.stopped() && carry_after_shift == test.expect_carry &&
                        machine.cpu.gpr[0] == test.expect_r0 &&
                        machine.cpu.gpr[3] == test.expect_r3 &&
                        (second_is_addze || ((machine.cpu.xer & kXerCA) != 0u) == test.expect_carry);
        if (!ok) {
            std::printf("FAIL %-24s r0=0x%08X r3=0x%08X xer=0x%08X (CA after shift %d)\n",
                        test.name, machine.cpu.gpr[0], machine.cpu.gpr[3], machine.cpu.xer,
                        carry_after_shift ? 1 : 0);
            ++g_failures;
            continue;
        }
        std::printf("ok   %s\n", test.name);
    }
}

// Audit int-arith-3/-4/-5: engine-vs-engine agreement hid wrong OV formulas
// shared by both engines, so every OE form is checked here against exact
// 64-bit arithmetic. Each form is the sum x + y + carry_in of manual Chapter 12;
// OV is signed overflow of that sum, CA its unsigned carry, SO sticky OV.
void overflow_against_reference() {
    const u32 d = 5, a = 7, b = 9;
    enum class X { A, NotA };
    enum class Y { B, Zero, MinusOne };
    enum class Cin { Zero, One, Ca };
    struct Form {
        const char* name;
        u32 xo;
        X x;
        Y y;
        Cin cin;
        bool sets_carry;
        std::function<u32(Cpu&, u32, u32, bool)> helper;  // as the generator emits it
    };
    const std::vector<Form> forms = {
        {"add", 266, X::A, Y::B, Cin::Zero, false,
         [](Cpu& c, u32 ra, u32 rb, bool oe) { return oe ? ppc::add_oe(c, ra, rb) : ra + rb; }},
        {"addc", 10, X::A, Y::B, Cin::Zero, true,
         [](Cpu& c, u32 ra, u32 rb, bool oe) { return ppc::addc(c, ra, rb, oe); }},
        {"adde", 138, X::A, Y::B, Cin::Ca, true,
         [](Cpu& c, u32 ra, u32 rb, bool oe) { return ppc::adde(c, ra, rb, oe); }},
        {"addze", 202, X::A, Y::Zero, Cin::Ca, true,
         [](Cpu& c, u32 ra, u32, bool oe) { return ppc::addze(c, ra, oe); }},
        {"addme", 234, X::A, Y::MinusOne, Cin::Ca, true,
         [](Cpu& c, u32 ra, u32, bool oe) { return ppc::addme(c, ra, oe); }},
        {"subf", 40, X::NotA, Y::B, Cin::One, false,
         [](Cpu& c, u32 ra, u32 rb, bool oe) { return oe ? ppc::subf_oe(c, ra, rb) : rb - ra; }},
        {"subfc", 8, X::NotA, Y::B, Cin::One, true,
         [](Cpu& c, u32 ra, u32 rb, bool oe) { return ppc::subfc(c, ra, rb, oe); }},
        {"subfe", 136, X::NotA, Y::B, Cin::Ca, true,
         [](Cpu& c, u32 ra, u32 rb, bool oe) { return ppc::subfe(c, ra, rb, oe); }},
        {"subfze", 200, X::NotA, Y::Zero, Cin::Ca, true,
         [](Cpu& c, u32 ra, u32, bool oe) { return ppc::subfze(c, ra, oe); }},
        {"subfme", 232, X::NotA, Y::MinusOne, Cin::Ca, true,
         [](Cpu& c, u32 ra, u32, bool oe) { return ppc::subfme(c, ra, oe); }},
        {"neg", 104, X::NotA, Y::Zero, Cin::One, false,
         [](Cpu& c, u32 ra, u32, bool oe) { return oe ? ppc::neg_oe(c, ra) : 0u - ra; }},
    };
    const u32 edges[] = {0u,          1u,          2u,          0x3FFFFFFFu, 0x40000000u,
                         0x7FFFFFFEu, 0x7FFFFFFFu, 0x80000000u, 0x80000001u, 0xBFFFFFFFu,
                         0xC0000000u, 0xFFFFFFFEu, 0xFFFFFFFFu, 0x12345678u, 0xEDCBA987u};
    for (const Form& form : forms) {
        ++g_checks;
        Machine& interpreted = interpreted_machine();
        Machine& helped = helper_machine();
        interpreted.cpu.reset();
        helped.cpu.reset();
        unsigned cases = 0;
        bool failed = false;
        for (bool oe : {false, true}) {
            const u32 encoding = x_form(31, d, a, form.y == Y::B ? b : 0u, form.xo, true, oe);
            interpreted.bus.write32(kCodeEffective, encoding);
            for (u32 ra : edges) for (u32 rb : edges) for (bool ca : {false, true})
                for (u32 flags : {0u, kXerSO, kXerOV, kXerSO | kXerOV}) {
                    if (failed || (form.y != Y::B && rb != 0u)) continue;
                    const u32 x = form.x == X::A ? ra : ~ra;
                    const u32 y = form.y == Y::B ? rb : (form.y == Y::Zero ? 0u : 0xFFFFFFFFu);
                    const u64 cin = form.cin == Cin::Zero ? 0u : (form.cin == Cin::One ? 1u : (ca ? 1u : 0u));
                    const u64 wide = static_cast<u64>(x) + y + cin;
                    const s64 exact = static_cast<s64>(static_cast<s32>(x)) +
                                      static_cast<s64>(static_cast<s32>(y)) + static_cast<s64>(cin);
                    const bool overflow = exact < -2147483648ll || exact > 2147483647ll;
                    const u32 value = static_cast<u32>(wide);
                    const bool carry = form.sets_carry ? (wide >> 32) != 0u : ca;
                    const bool ov = oe ? overflow : (flags & kXerOV) != 0u;
                    const bool so = (flags & kXerSO) != 0u || (oe && overflow);
                    const u32 xer_in = 0x0000007Fu | flags | (ca ? kXerCA : 0u);
                    const u32 xer = 0x0000007Fu | (so ? kXerSO : 0u) | (ov ? kXerOV : 0u) |
                                    (carry ? kXerCA : 0u);
                    const s32 signed_value = static_cast<s32>(value);
                    const u32 cr0 = (signed_value < 0 ? 8u : (signed_value > 0 ? 4u : 2u)) | (so ? 1u : 0u);

                    interpreted.cpu.clear_stop();
                    interpreted.cpu.gpr[a] = helped.cpu.gpr[a] = ra;
                    interpreted.cpu.gpr[b] = helped.cpu.gpr[b] = rb;
                    interpreted.cpu.xer = helped.cpu.xer = xer_in;
                    interpreted.cpu.cr = helped.cpu.cr = 0u;
                    interpreted.cpu.set_entry(kCodeEffective);
                    interpreted.cpu.step();
                    helped.cpu.gpr[d] = form.helper(helped.cpu, ra, rb, oe);
                    ppc::update_cr0(helped.cpu, helped.cpu.gpr[d]);

                    for (Machine* machine : {&interpreted, &helped}) {
                        const Cpu& c = machine->cpu;
                        if (c.gpr[d] != value || c.xer != xer || (c.cr >> 28) != cr0) {
                            std::printf("FAIL %s%s. (%s) rA=0x%08X rB=0x%08X xer_in=0x%08X: "
                                        "r%u=0x%08X xer=0x%08X cr0=%X, expected 0x%08X xer=0x%08X cr0=%X\n",
                                        form.name, oe ? "o" : "",
                                        machine == &interpreted ? "interpreter" : "helper", ra, rb,
                                        xer_in, d, c.gpr[d], c.xer, c.cr >> 28, value, xer, cr0);
                            ++g_failures;
                            failed = true;
                            break;
                        }
                    }
                    ++cases;
                }
        }
        if (!failed) std::printf("ok   %-8s %u OE=0/1 cases against exact arithmetic\n", form.name, cases);
    }
}

// Audit int-arith-6: ppc::cntlzw (compiled here WITHOUT the project /arch flag,
// so this is the BSR path; native_fallback_tests covers the /arch build) and
// the interpreter against the manual's bit loop, over every single bit, every
// prefix/suffix mask and mixed low bits.
void count_leading_zeros_patterns() {
    ++g_checks;
    const auto reference = [](u32 value) {
        u32 n = 0;
        while (n < 32u && (value & (0x80000000u >> n)) == 0u) ++n;
        return n;
    };
    std::vector<u32> patterns = {0u, 0xFFFFFFFFu};
    for (u32 bit = 0; bit < 32u; ++bit) {
        const u32 single = 1u << bit;
        patterns.push_back(single);
        patterns.push_back(single - 1u);
        patterns.push_back(0xFFFFFFFFu << bit);
        patterns.push_back(0xFFFFFFFFu >> bit);
        patterns.push_back(single | ((single - 1u) & 0x5A5A5A5Au));
        patterns.push_back(single | ((single - 1u) & 0xA5A5A5A5u));
    }
    Machine& interpreted = interpreted_machine();
    interpreted.cpu.reset();
    interpreted.bus.write32(kCodeEffective, x_form(31, 4, 3, 0, 26, false));
    for (u32 value : patterns) {
        interpreted.cpu.gpr[4] = value;
        interpreted.cpu.set_entry(kCodeEffective);
        interpreted.cpu.step();
        const u32 expected = reference(value);
        if (ppc::cntlzw(value) != expected || interpreted.cpu.gpr[3] != expected) {
            std::printf("FAIL cntlzw 0x%08X: helper %u interpreter %u expected %u\n", value,
                        ppc::cntlzw(value), interpreted.cpu.gpr[3], expected);
            ++g_failures;
            return;
        }
    }
    std::printf("ok   cntlzw   %zu boundary patterns\n", patterns.size());
}

// Audit load-store-1/-3/-4 in the reference interpreter, as single-instruction
// programs with values computed from the manual's RTL.
void update_and_reservation_programs() {
    const u32 data = 0x80300000u;
    Machine& machine = interpreted_machine();
    Cpu& c = machine.cpu;
    const auto run = [&](const char* name, std::initializer_list<u32> words) {
        u32 offset = 0;
        for (u32 word : words) {
            machine.bus.write32(kCodeEffective + offset, word);
            offset += 4u;
        }
        c.set_entry(kCodeEffective);
        for (std::size_t index = 0; index < words.size(); ++index) c.step();
        if (c.stopped()) {
            std::printf("FAIL %s: interpreter stopped: %s\n", name, c.stop().detail.c_str());
            ++g_failures;
            return false;
        }
        return true;
    };
    const auto expect = [&](const char* name, bool ok) {
        ++g_checks;
        if (ok) {
            std::printf("ok   %s\n", name);
        } else {
            std::printf("FAIL %s\n", name);
            ++g_failures;
        }
    };

    c.reset();
    c.gpr[1] = 0x8055FF38u;
    c.gpr[11] = 0xFFFFFF68u;
    if (run("stwux r1,r1,r11", {x_form(31, 1, 1, 11, 183, false)})) {
        expect("stwux r1,r1,r11 stores the original r1",
               c.gpr[1] == 0x8055FEA0u && machine.bus.read32(0x8055FEA0u) == 0x8055FF38u);
    }
    c.reset();
    c.gpr[5] = data + 0x10u;
    c.gpr[6] = 0x10u;
    if (run("sthux r5,r5,r6", {x_form(31, 5, 5, 6, 439, false)})) {
        expect("sthux r5,r5,r6 stores the original r5",
               c.gpr[5] == data + 0x20u && machine.bus.read16(data + 0x20u) == 0x0010u);
    }
    c.reset();
    c.gpr[5] = data + 0x11u;
    c.gpr[6] = 0x20u;
    if (run("stbux r5,r5,r6", {x_form(31, 5, 5, 6, 247, false)})) {
        expect("stbux r5,r5,r6 is implemented and stores the original r5",
               c.gpr[5] == data + 0x31u && machine.bus.read8(data + 0x31u) == 0x11u);
    }

    // lwarx r6,0,r3; stwcx. r4,0,r3; mfcr r7; stwcx. r5,0,r3; mfcr r8
    c.reset();
    c.gpr[3] = data + 0x204u;
    c.gpr[4] = 0x11111111u;
    c.gpr[5] = 0x22222222u;
    c.xer = kXerSO;
    machine.bus.write32(data + 0x204u, 0xCAFEF00Du);
    if (run("lwarx/stwcx. pair", {x_form(31, 6, 0, 3, 20, false), x_form(31, 4, 0, 3, 150, true),
                                  x_form(31, 7, 0, 0, 19, false), x_form(31, 5, 0, 3, 150, true),
                                  x_form(31, 8, 0, 0, 19, false)})) {
        expect("lwarx/stwcx.: store once, fail the unreserved retry, copy SO",
               c.gpr[6] == 0xCAFEF00Du && (c.gpr[7] >> 28) == 3u && (c.gpr[8] >> 28) == 1u &&
                   machine.bus.read32(data + 0x204u) == 0x11111111u && !c.reservation &&
                   c.reservation_address == data + 0x200u);
    }
}

// Quantised loads and stores reach memory, so they are checked on a machine
// whose RAM starts identical and is compared afterwards as well.
void quantised_memory() {
    const u32 d = 4, a = 9;
    const u32 data_effective = 0x80002000u;

    for (u32 type : {0u, 4u, 5u, 6u, 7u}) {
        for (u32 scale : {0u, 3u, 0x3du}) {
            const u32 gqr_value = (type << 16) | (scale << 24) | type | (scale << 8);
            const u32 encoding_load = (56u << 26) | (d << 21) | (a << 16) | (0u << 15) |
                                      (0u << 12) | 0x10u;
            const u32 encoding_store = (60u << 26) | (d << 21) | (a << 16) | (0u << 15) |
                                       (0u << 12) | 0x10u;

            ++g_checks;
            Machine& interpreted = interpreted_machine();
            Machine& helped = helper_machine();
            std::mt19937 random(0xbeef0000u ^ (type << 8) ^ scale);
            for (Machine* machine : {&interpreted, &helped}) {
                machine->cpu.reset();
                std::mt19937 local(0x1234u);
                seed_state(machine->cpu, local);
                machine->cpu.gpr[a] = data_effective;
                machine->cpu.spr[SPR_GQR0] = gqr_value;
                for (u32 offset = 0; offset < 64; offset += 4) {
                    machine->bus.write32(data_effective + offset, random());
                }
                random.seed(0xbeef0000u ^ (type << 8) ^ scale);
            }

            interpreted.bus.write32(kCodeEffective, encoding_load);
            interpreted.cpu.set_entry(kCodeEffective);
            interpreted.cpu.step();
            ppc::psq_load(helped.cpu, d, helped.cpu.gpr[a] + 0x10u, false, 0u);
            if (!equal(capture(interpreted.cpu), capture(helped.cpu))) {
                std::printf("FAIL psq_l type %u scale %u: %s\n", type, scale,
                            describe(capture(interpreted.cpu), capture(helped.cpu)).c_str());
                ++g_failures;
                continue;
            }

            interpreted.bus.write32(kCodeEffective, encoding_store);
            interpreted.cpu.set_entry(kCodeEffective);
            interpreted.cpu.step();
            ppc::psq_store(helped.cpu, d, helped.cpu.gpr[a] + 0x10u, false, 0u);
            bool memory_equal = true;
            for (u32 offset = 0; offset < 64; ++offset) {
                if (interpreted.bus.read8(data_effective + offset) !=
                    helped.bus.read8(data_effective + offset)) {
                    memory_equal = false;
                    break;
                }
            }
            if (!memory_equal) {
                std::printf("FAIL psq_st type %u scale %u: stored bytes differ\n", type, scale);
                ++g_failures;
                continue;
            }
            std::printf("ok   psq_l/st   type %u scale %u\n", type, scale);
        }
    }
}

// Audit paired-single-2 / paired-single-4: the quantised load/store against
// the manual, through the reference interpreter AND ppc::psq_load/psq_store.
//  - the power-of-two scale tables equal std::ldexp for every 6-bit field;
//  - type 0 ignores both scale fields (PAGE 493/497) and moves the word;
//  - F = I * 2^-S for every LD_SCALE (PAGE 110, Table 2-44) and
//    I = ROUND(F * 2^S) with clamping for every ST_SCALE (PAGE 112);
//  - +Inf and NaN of either sign store the POSITIVE overflow value, -Inf the
//    negative one (PAGE 111). A NaN used to reach static_cast<u8/u16> (UB) and
//    store zero.
void quantised_against_manual() {
    const u32 d = 4, a = 9, gqr_index = 5;
    const u32 data_effective = 0x80002000u;
    const auto psq = [&](u32 primary, bool w) {
        return (primary << 26) | (d << 21) | (a << 16) | ((w ? 1u : 0u) << 15) | (gqr_index << 12) | 0x10u;
    };
    Machine& interpreted = interpreted_machine();
    Machine& helped = helper_machine();
    const auto prepare = [&](u32 gqr, u64 ps0, u64 ps1) {
        for (Machine* machine : {&interpreted, &helped}) {
            machine->cpu.reset();
            machine->cpu.clear_stop();
            machine->cpu.gpr[a] = data_effective;
            machine->cpu.spr[SPR_GQR0 + gqr_index] = gqr;
            machine->cpu.fpr[d].bits[0] = ps0;
            machine->cpu.fpr[d].bits[1] = ps1;
            for (u32 offset = 0; offset < 0x20u; ++offset) machine->bus.write8(data_effective + offset, 0xAAu);
        }
    };
    const auto run = [&](u32 encoding, bool store, bool w) {
        interpreted.bus.write32(kCodeEffective, encoding);
        interpreted.cpu.set_entry(kCodeEffective);
        interpreted.cpu.step();
        if (store) {
            ppc::psq_store(helped.cpu, d, data_effective + 0x10u, w, gqr_index);
        } else {
            ppc::psq_load(helped.cpu, d, data_effective + 0x10u, w, gqr_index);
        }
        return !interpreted.cpu.stopped();
    };

    ++g_checks;
    bool tables_ok = true;
    for (u32 raw = 0; raw < 64u; ++raw) {
        const int s = static_cast<int>(raw & 0x1fu) - static_cast<int>(raw & 0x20u);
        if (double_to_bits(ppc::kGqrLoadScale.factor[raw]) != double_to_bits(std::ldexp(1.0, -s)) ||
            double_to_bits(ppc::kGqrStoreScale.factor[raw]) != double_to_bits(std::ldexp(1.0, s))) {
            std::printf("FAIL GQR scale table raw 0x%02X\n", raw);
            ++g_failures;
            tables_ok = false;
        }
    }
    if (tables_ok) std::printf("ok   GQR scale tables == ldexp for all 64 fields\n");

    // Type 0 with both scale fields at -1: the word moves unchanged.
    ++g_checks;
    {
        const u32 gqr = 0x3F003F00u;
        prepare(gqr, 0, 0);
        for (Machine* machine : {&interpreted, &helped}) {
            machine->bus.write32(data_effective + 0x10u, 0x40490FDBu);
            machine->bus.write32(data_effective + 0x14u, 0xC1200000u);
        }
        bool ok = run(psq(56, false), false, false);
        const u64 expect0 = double_to_bits(static_cast<double>(bits_to_float(0x40490FDBu)));
        const u64 expect1 = double_to_bits(-10.0);
        for (Machine* machine : {&interpreted, &helped}) {
            ok = ok && machine->cpu.fpr[d].bits[0] == expect0 && machine->cpu.fpr[d].bits[1] == expect1;
            machine->bus.write32(data_effective + 0x10u, 0u);
            machine->bus.write32(data_effective + 0x14u, 0u);
        }
        ok = ok && run(psq(60, false), true, false);
        for (Machine* machine : {&interpreted, &helped}) {
            ok = ok && machine->bus.read32(data_effective + 0x10u) == 0x40490FDBu &&
                 machine->bus.read32(data_effective + 0x14u) == 0xC1200000u &&
                 machine->bus.read8(data_effective + 0x18u) == 0xAAu;
        }
        std::printf("%s psq_l/psq_st type 0 ignores the scale fields\n", ok ? "ok  " : "FAIL");
        if (!ok) ++g_failures;
    }

    // Every LD_SCALE: u16 1 loads as 2^-S (W=1, ps1 = 1.0). Every ST_SCALE: s16
    // store of 1.0 is 2^S truncated toward zero and clamped to 32767.
    ++g_checks;
    {
        bool ok = true;
        for (u32 raw = 0; raw < 64u && ok; ++raw) {
            const int s = static_cast<int>(raw & 0x1fu) - static_cast<int>(raw & 0x20u);
            prepare((5u << 16) | (raw << 24) | 7u | (raw << 8), 0, 0);
            for (Machine* machine : {&interpreted, &helped}) machine->bus.write16(data_effective + 0x10u, 1u);
            ok = run(psq(56, true), false, true);
            // 2^-S written without ldexp: an exact integer power, or its reciprocal.
            const double power = s <= 0 ? static_cast<double>(1ull << -s)
                                        : 1.0 / static_cast<double>(1ull << s);
            const u64 expect = double_to_bits(static_cast<double>(static_cast<float>(power)));
            for (Machine* machine : {&interpreted, &helped}) {
                ok = ok && machine->cpu.fpr[d].bits[0] == expect &&
                     machine->cpu.fpr[d].bits[1] == double_to_bits(1.0);
                machine->cpu.fpr[d].bits[0] = double_to_bits(1.0);
            }
            ok = ok && run(psq(60, true), true, true);
            const u16 stored = s >= 15 ? 0x7FFFu : (s >= 0 ? static_cast<u16>(1u << s) : 0u);
            for (Machine* machine : {&interpreted, &helped}) {
                ok = ok && machine->bus.read16(data_effective + 0x10u) == stored &&
                     machine->bus.read8(data_effective + 0x12u) == 0xAAu;
            }
            if (!ok) std::printf("FAIL psq scale raw 0x%02X (S = %d)\n", raw, s);
        }
        std::printf("%s psq_l u16 / psq_st s16 over every scale field\n", ok ? "ok  " : "FAIL");
        if (!ok) ++g_failures;
    }

    // Saturation, W=0 so both halves go through the same element conversion.
    struct Row {
        const char* name;
        u64 value;
        u32 u8v, u16v, s8v, s16v;
    };
    const Row rows[] = {
        {"+QNaN", kQNaN, 0xFFu, 0xFFFFu, 0x7Fu, 0x7FFFu},
        {"-QNaN", kNegQNaN, 0xFFu, 0xFFFFu, 0x7Fu, 0x7FFFu},
        {"SNaN", kSNaN, 0xFFu, 0xFFFFu, 0x7Fu, 0x7FFFu},
        {"+Inf", 0x7FF0000000000000ull, 0xFFu, 0xFFFFu, 0x7Fu, 0x7FFFu},
        {"-Inf", 0xFFF0000000000000ull, 0x00u, 0x0000u, 0x80u, 0x8000u},
        {"300.75", double_to_bits(300.75), 0xFFu, 0x012Cu, 0x7Fu, 0x012Cu},
        {"-129.5", double_to_bits(-129.5), 0x00u, 0x0000u, 0x80u, 0xFF7Fu},
        {"2.9", double_to_bits(2.9), 0x02u, 0x0002u, 0x02u, 0x0002u},
        {"-2.9", double_to_bits(-2.9), 0x00u, 0x0000u, 0xFEu, 0xFFFEu},
    };
    for (const Row& row : rows) {
        for (u32 type = 4; type <= 7u; ++type) {
            ++g_checks;
            prepare(type, row.value, row.value);
            bool ok = run(psq(60, false), true, false);
            const u32 size = (type & 1u) + 1u;
            const u32 expect = type == 4u ? row.u8v : type == 5u ? row.u16v : type == 6u ? row.s8v : row.s16v;
            for (Machine* machine : {&interpreted, &helped}) {
                for (u32 element = 0; element < 2u; ++element) {
                    const u32 at = data_effective + 0x10u + element * size;
                    const u32 got = size == 1u ? machine->bus.read8(at) : machine->bus.read16(at);
                    ok = ok && got == expect;
                }
                ok = ok && machine->bus.read8(data_effective + 0x10u + 2u * size) == 0xAAu;
            }
            if (!ok) {
                std::printf("FAIL psq_st %s to type %u: interpreter 0x%04X helper 0x%04X, expected 0x%04X\n",
                            row.name, type,
                            size == 1u ? interpreted.bus.read8(data_effective + 0x10u)
                                       : interpreted.bus.read16(data_effective + 0x10u),
                            size == 1u ? helped.bus.read8(data_effective + 0x10u)
                                       : helped.bus.read16(data_effective + 0x10u),
                            expect);
                ++g_failures;
                continue;
            }
            std::printf("ok   psq_st %-6s to type %u\n", row.name, type);
        }
    }
}

// ---- audit batch b7: floating-point semantics -------------------------------

// fpu-4 / paired-single-8: the single <-> double conversions against a literal
// transcription of the PowerPC Programming Environments manual D.6 (load) and
// D.7 (store), with gekko_user_manual.txt PAGE 108 Table 2-41 for the
// exponent < 874 case and PAGE 112 for the psq type-0 denorm flush. Written
// bit by bit, sharing nothing with types.h.
u64 pem_load_single(u32 word) {
    const auto word_bit = [word](int n) -> u64 { return (word >> (31 - n)) & 1u; };
    u64 fr = 0;
    const auto set = [&fr](int n, u64 value) { fr |= (value & 1u) << (63 - n); };
    const u32 exponent = (word >> 23) & 0xffu;
    if (exponent > 0u && exponent < 255u) {
        set(0, word_bit(0));
        set(1, word_bit(1));
        for (int n = 2; n <= 4; ++n) set(n, word_bit(1) ^ 1u);
        for (int n = 2; n <= 31; ++n) set(n + 3, word_bit(n));
    } else if (exponent == 255u || (word & 0x7fffffffu) == 0u) {
        set(0, word_bit(0));
        set(1, word_bit(1));
        for (int n = 2; n <= 4; ++n) set(n, word_bit(1));
        for (int n = 2; n <= 31; ++n) set(n + 3, word_bit(n));
    } else {
        // frac[0-52] = 0b0 || WORD[9-31] || 29 zeros, exp = -126; normalise.
        int exp = -126;
        u64 frac = static_cast<u64>(word & 0x7fffffu) << 29;  // frac[0] is bit 52
        do {
            frac <<= 1;
            exp -= 1;
        } while ((frac & (1ull << 52)) == 0u);
        fr = (word_bit(0) << 63) | (static_cast<u64>(exp + 1023) << 52) | (frac & ((1ull << 52) - 1u));
    }
    return fr;
}

u32 pem_store_single(u64 bits, bool flush) {
    const auto fr_bit = [bits](int n) -> u32 { return static_cast<u32>((bits >> (63 - n)) & 1u); };
    u32 word = 0;
    const auto set = [&word](int n, u32 value) { word |= (value & 1u) << (31 - n); };
    const u32 exponent = static_cast<u32>(bits >> 52) & 0x7ffu;
    if (exponent > 896u || (bits & 0x7fffffffffffffffull) == 0u) {
        set(0, fr_bit(0));
        set(1, fr_bit(1));
        for (int n = 2; n <= 31; ++n) set(n, fr_bit(n + 3));
    } else if (!flush && exponent >= 874u) {
        int exp = static_cast<int>(exponent) - 1023;
        u64 frac = (1ull << 52) | (bits & ((1ull << 52) - 1u));  // 0b1 || frS[12-63]
        while (exp < -126) {
            frac >>= 1;
            exp += 1;
        }
        set(0, fr_bit(0));
        word |= static_cast<u32>((frac >> 29) & 0x7fffffu);  // WORD[9-31] = frac[1-23]
    } else {
        set(0, fr_bit(0));  // Table 2-41 "Store zero"; PAGE 112 "the value 0.0"
    }
    return word;
}

void fp_conversions_against_manual() {
    std::mt19937 random(0xb7c0u);
    {
        ++g_checks;
        u64 failures = 0, cases = 0;
        const auto probe = [&](u32 word) {
            ++cases;
            if (single_to_double_bits(word) != pem_load_single(word) && failures++ < 5u) {
                std::printf("FAIL lfs conversion 0x%08X: 0x%016llX, D.6 0x%016llX\n", word,
                            static_cast<unsigned long long>(single_to_double_bits(word)),
                            static_cast<unsigned long long>(pem_load_single(word)));
            }
        };
        for (u32 exponent = 0; exponent < 256u; ++exponent) {
            for (u32 sign : {0u, 0x80000000u}) {
                for (u32 fraction : {0u, 1u, 2u, 0x3fffffu, 0x400000u, 0x400001u, 0x7ffffeu, 0x7fffffu}) {
                    probe(sign | (exponent << 23) | fraction);
                }
                const u32 draws = (exponent == 0u || exponent == 255u) ? 65536u : 2048u;
                for (u32 index = 0; index < draws; ++index) {
                    probe(sign | (exponent << 23) | (random() & 0x7fffffu));
                }
            }
        }
        std::printf("%s single_to_double_bits == D.6 over %llu words\n", failures == 0u ? "ok  " : "FAIL",
                    static_cast<unsigned long long>(cases));
        if (failures != 0u) ++g_failures;
    }
    {
        ++g_checks;
        u64 failures = 0, cases = 0;
        const auto probe = [&](u64 bits) {
            ++cases;
            const u32 store = double_to_single_bits(bits);
            const u32 flush = double_to_single_bits_flush(bits);
            if ((store != pem_store_single(bits, false) || flush != pem_store_single(bits, true)) &&
                failures++ < 5u) {
                std::printf("FAIL stfs conversion 0x%016llX: store 0x%08X (D.7 0x%08X) flush 0x%08X (0x%08X)\n",
                            static_cast<unsigned long long>(bits), store, pem_store_single(bits, false),
                            flush, pem_store_single(bits, true));
            }
        };
        for (u64 exponent = 0; exponent < 2048u; ++exponent) {
            for (u64 sign : {0ull, 0x8000000000000000ull}) {
                for (u64 mantissa : {0ull, 1ull, 0x0000000020000000ull, 0x000fffffe0000000ull,
                                     0x0008000000000000ull, 0x000fffffffffffffull, 0x0000000010000000ull}) {
                    probe(sign | (exponent << 52) | mantissa);
                }
                for (u32 index = 0; index < 256u; ++index) {
                    const u64 mantissa = ((static_cast<u64>(random()) << 32) | random()) & 0x000fffffffffffffull;
                    probe(sign | (exponent << 52) | mantissa);
                    probe(sign | (exponent << 52) | (mantissa & ~0x1fffffffull));  // single-representable
                }
            }
        }
        std::printf("%s double_to_single_bits(_flush) == D.7 over %llu doubles\n",
                    failures == 0u ? "ok  " : "FAIL", static_cast<unsigned long long>(cases));
        if (failures != 0u) ++g_failures;
    }

    // Through the reference interpreter's real encodings and the emitted
    // helper calls: lfs f1,0x10(r9); stfs f1,0x18(r9); lfsux f2,r9,r10;
    // stfsx f2,r9,r11 and psq_st f1,0x20(r9),0,0 / psq_l f3,0x20(r9),0,0.
    struct Row {
        const char* name;
        u32 loaded;        // word at +0x10 (lfs)
        u64 load_expect;   // f1 after lfs
        u64 store_source;  // f2 for stfsx, f1 ps1 for psq_st
        u32 store_expect;  // stfsx word
        u32 psq_expect;    // psq_st second word
    };
    const Row rows[] = {
        // fpu-4: SNaN round trip; the host casts gave 0x7FF8000020000000 / 0x7FC00001.
        {"SNaN copy", 0x7F800001u, 0x7FF0000020000000ull, 0x7FF0000020000000ull, 0x7F800001u, 0x7F800001u},
        {"-SNaN", 0xFFA00000u, 0xFFF4000000000000ull, 0xFFF4000000000000ull, 0xFFA00000u, 0xFFA00000u},
        // fpu-4: stfs truncates 0.1 (the host rounded to 0x3DCCCCCD).
        {"0.1 truncates", 0x3F800000u, 0x3FF0000000000000ull, 0x3FB999999999999Aull, 0x3DCCCCCCu, 0x3DCCCCCCu},
        // paired-single-8: psq_st of a denormal stores zero, stfs denormalises.
        {"1e-40f", 0x000116C2u, 0x37A16C2000000000ull, 0x37A16C2000000000ull, 0x000116C2u, 0x00000000u},
        {"-1e-40f", 0x800116C2u, 0xB7A16C2000000000ull, 0xB7A16C2000000000ull, 0x800116C2u, 0x80000000u},
        {"min denormal", 0x00000001u, 0x36A0000000000000ull, 0x0000000000000001ull, 0x00000000u, 0x00000000u},
        {"2^-127 * 1.5 truncates", 0x00000000u, 0x0000000000000000ull, 0x3808000020000001ull, 0x00600000u,
         0x00000000u},
        {"FLT_MIN", 0x00800000u, 0x3810000000000000ull, 0x3810000000000000ull, 0x00800000u, 0x00800000u},
        {"beyond FLT_MAX: bit selection", 0x7F7FFFFFu, 0x47EFFFFFE0000000ull, 0x4800000000000000ull,
         0x40000000u, 0x40000000u},
    };
    const u32 data_effective = 0x80002000u;
    for (const Row& row : rows) {
        ++g_checks;
        bool ok = true;
        for (bool native : {false, true}) {
            Machine& machine = native ? helper_machine() : interpreted_machine();
            Cpu& c = machine.cpu;
            c.reset();
            c.clear_stop();
            c.gpr[9] = data_effective;
            c.gpr[10] = 0x40u;
            c.gpr[11] = 0x18u;
            c.fpr[2].bits[0] = row.store_source;
            for (u32 offset = 0; offset < 0x60u; offset += 4u) machine.bus.write32(data_effective + offset, 0xAAAAAAAAu);
            machine.bus.write32(data_effective + 0x10u, row.loaded);
            machine.bus.write32(data_effective + 0x40u, row.loaded);
            const auto run = [&](u32 encoding, const std::function<void()>& helper) {
                if (native) {
                    helper();
                } else {
                    machine.bus.write32(kCodeEffective, encoding);
                    c.set_entry(kCodeEffective);
                    c.step();
                    ok = ok && !c.stopped();
                }
            };
            run(d_form(48, 1, 9, 0x10), [&] {
                const u64 value = single_to_double_bits(c.mem_read32(c.gpr[9] + 0x10u));
                c.fpr[1].bits[0] = value;
                c.fpr[1].bits[1] = value;
            });
            ok = ok && c.fpr[1].bits[0] == row.load_expect && c.fpr[1].bits[1] == row.load_expect;
            run(d_form(52, 1, 9, 0x18), [&] { c.mem_write32(c.gpr[9] + 0x18u, double_to_single_bits(c.fpr[1].bits[0])); });
            ok = ok && machine.bus.read32(data_effective + 0x18u) == row.loaded;
            run(x_form(31, 3, 9, 10, 567, false), [&] {
                const u32 ea = c.gpr[9] + c.gpr[10];
                const u64 value = single_to_double_bits(c.mem_read32(ea));
                c.fpr[3].bits[0] = value;
                c.fpr[3].bits[1] = value;
                c.gpr[9] = ea;
            });
            ok = ok && c.fpr[3].bits[1] == row.load_expect && c.gpr[9] == data_effective + 0x40u;
            c.gpr[9] = data_effective;
            run(x_form(31, 2, 9, 11, 663, false), [&] { c.mem_write32(c.gpr[9] + c.gpr[11], double_to_single_bits(c.fpr[2].bits[0])); });
            ok = ok && machine.bus.read32(data_effective + 0x18u) == row.store_expect;
            c.fpr[1].bits[1] = row.store_source;
            const u32 psq_st = (60u << 26) | (1u << 21) | (9u << 16) | 0x20u;
            run(psq_st, [&] { ppc::psq_store(c, 1u, c.gpr[9] + 0x20u, false, 0u); });
            ok = ok && machine.bus.read32(data_effective + 0x20u) == pem_store_single(row.load_expect, true) &&
                 machine.bus.read32(data_effective + 0x24u) == row.psq_expect;
            const u32 psq_l = (56u << 26) | (5u << 21) | (9u << 16) | 0x20u;
            run(psq_l, [&] { ppc::psq_load(c, 5u, c.gpr[9] + 0x20u, false, 0u); });
            ok = ok && c.fpr[5].bits[1] == pem_load_single(row.psq_expect);
        }
        std::printf("%s lfs/stfs/lfsux/stfsx/psq type 0: %s\n", ok ? "ok  " : "FAIL", row.name);
        if (!ok) ++g_failures;
    }
}

// fpu-1/-2/-3, paired-single-3: exact expected results from
// tools/ppcrecomp/fp_semantics_oracle.py (Python Fractions, rounding done bit by
// bit), through the emitted helpers AND every encoding of the operation in the
// reference interpreter: the scalar group-59/63 form and, for single results,
// the paired form with both halves equal. frD = f4, frA = f1, frB = f2, frC = f3.
struct FpVector {
    u32 op;
    u32 ni;
    u64 a, b, c, result;
};

u64 fp_helper(Cpu& c, u32 op, u64 a, u64 b, u64 cc) {
    const bool single = (op & ppc::kFpSingle) != 0u;
    switch (op & 0xfu) {
        case ppc::kFpAdd: return single ? ppc::fps_add(c, a, b) : ppc::fp_add(c, a, b);
        case ppc::kFpSub: return single ? ppc::fps_sub(c, a, b) : ppc::fp_sub(c, a, b);
        case ppc::kFpMul: return single ? ppc::fps_mul(c, a, cc) : ppc::fp_mul(c, a, cc);
        case ppc::kFpDiv: return single ? ppc::fps_div(c, a, b) : ppc::fp_div(c, a, b);
        case ppc::kFpSqrt: return single ? ppc::fps_sqrt(c, b) : ppc::fp_sqrt(c, b);
        case ppc::kFpRes: return ppc::fps_res(c, b);
        case ppc::kFpRsqrte: return single ? ppc::fps_rsqrte(c, b) : ppc::fp_rsqrte(c, b);
        case ppc::kFpRound: return ppc::fps_round(c, b);
        case ppc::kFpMAdd: return single ? ppc::fps_madd(c, a, cc, b) : ppc::fp_madd(c, a, cc, b);
        case ppc::kFpMSub: return single ? ppc::fps_msub(c, a, cc, b) : ppc::fp_msub(c, a, cc, b);
        case ppc::kFpNMAdd: return single ? ppc::fps_nmadd(c, a, cc, b) : ppc::fp_nmadd(c, a, cc, b);
        default: return single ? ppc::fps_nmsub(c, a, cc, b) : ppc::fp_nmsub(c, a, cc, b);
    }
}

// fdivs fsubs fadds fmuls fsqrts fres ... A-form extended opcodes by FpOp.
constexpr u32 kFpExtended[12] = {21u, 20u, 25u, 18u, 22u, 24u, 26u, 0u, 29u, 28u, 31u, 30u};

std::vector<u32> fp_encodings(u32 op) {
    const bool single = (op & ppc::kFpSingle) != 0u;
    const u32 kind = op & 0xfu;
    std::vector<u32> encodings;
    if (kind == ppc::kFpRound) {
        encodings.push_back(x_form(63, 4, 0, 2, 12, false));  // frsp
        return encodings;
    }
    const bool unary = kind == ppc::kFpSqrt || kind == ppc::kFpRes || kind == ppc::kFpRsqrte;
    const u32 xo = kFpExtended[kind];
    const u32 a = unary ? 0u : 1u, c = (unary || kind == ppc::kFpAdd || kind == ppc::kFpSub ||
                                          kind == ppc::kFpDiv) ? 0u : 3u;
    const u32 b = kind == ppc::kFpMul ? 0u : 2u;
    // No double fres and no single frsqrte exist as scalar instructions.
    if (single ? kind != ppc::kFpRsqrte : kind != ppc::kFpRes) {
        encodings.push_back(a_form(single ? 59u : 63u, 4, a, b, c, xo, false));
    }
    if (single && kind != ppc::kFpSqrt) {
        encodings.push_back(a_form(4, 4, a, b, c, xo, false));
    }
    return encodings;
}

bool check_fp_vector(const FpVector& v, std::string& why) {
    char row[256];
    Cpu& helper = helper_machine().cpu;
    helper.fpscr = v.ni != 0u ? kFpscrNI : 0u;
    const u64 helped = fp_helper(helper, v.op, v.a, v.b, v.c);
    if (helped != v.result) {
        std::snprintf(row, sizeof(row), "helper 0x%016llX", static_cast<unsigned long long>(helped));
        why = row;
        return false;
    }
    Machine& machine = interpreted_machine();
    Cpu& c = machine.cpu;
    const bool single = (v.op & ppc::kFpSingle) != 0u;
    for (const u32 encoding : fp_encodings(v.op)) {
        c.clear_stop();
        c.fpscr = v.ni != 0u ? kFpscrNI : 0u;
        for (u32 half = 0; half < 2u; ++half) {
            c.fpr[1].bits[half] = v.a;
            c.fpr[2].bits[half] = v.b;
            c.fpr[3].bits[half] = v.c;
            c.fpr[4].bits[half] = 0x5555u;
        }
        machine.bus.write32(kCodeEffective, encoding);
        c.set_entry(kCodeEffective);
        c.step();
        const bool writes_ps1 = single && (encoding >> 26) != 63u;
        if (c.stopped() || c.pc != kCodeEffective + 4u || c.fpr[4].bits[0] != v.result ||
            c.fpr[4].bits[1] != (writes_ps1 ? v.result : 0x5555u)) {
            std::snprintf(row, sizeof(row), "interpreter 0x%08X: 0x%016llX 0x%016llX%s", encoding,
                          static_cast<unsigned long long>(c.fpr[4].bits[0]),
                          static_cast<unsigned long long>(c.fpr[4].bits[1]), c.stopped() ? " (stopped)" : "");
            why = row;
            return false;
        }
    }
    return true;
}

void run_fp_vectors(const FpVector* vectors, std::size_t count, const char* label) {
    ++g_checks;
    interpreted_machine().cpu.reset();
    helper_machine().cpu.reset();
    unsigned failures = 0;
    for (std::size_t index = 0; index < count; ++index) {
        std::string why;
        if (!check_fp_vector(vectors[index], why)) {
            if (failures++ < 12u) {
                const FpVector& v = vectors[index];
                std::printf("FAIL %s #%zu op 0x%02X NI %u A 0x%016llX B 0x%016llX C 0x%016llX: expected "
                            "0x%016llX, %s\n",
                            label, index, v.op, v.ni, static_cast<unsigned long long>(v.a),
                            static_cast<unsigned long long>(v.b), static_cast<unsigned long long>(v.c),
                            static_cast<unsigned long long>(v.result), why.c_str());
            }
        }
    }
    std::printf("%s %s: %zu vectors, %u failures\n", failures == 0u ? "ok  " : "FAIL", label, count, failures);
    if (failures != 0u) ++g_failures;
}

using ppc::kFpAdd;
using ppc::kFpDiv;
using ppc::kFpMAdd;
using ppc::kFpMSub;
using ppc::kFpMul;
using ppc::kFpNMAdd;
using ppc::kFpNMSub;
using ppc::kFpRes;
using ppc::kFpRound;
using ppc::kFpRsqrte;
using ppc::kFpSingle;
using ppc::kFpSqrt;
using ppc::kFpSub;

const FpVector kFpVectors[] = {
    // 511 of 25370 oracle vectors (tools/ppcrecomp/fp_semantics_oracle.py)
    {kFpSqrt, 0u, 0x0000000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x1E60000000000000ull},
    {kFpSqrt, 0u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpSqrt, 0u, 0x0000000000000000ull, 0xBFF8000000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpSqrt, 0u, 0x0000000000000000ull, 0xFFF0000000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpSqrt, 0u, 0x0000000000000000ull, 0xFFF8000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpSqrt, 0u, 0x0000000000000000ull, 0xFFF0000000000001ull, 0x0000000000000000ull, 0xFFF8000000000001ull},
    {kFpRsqrte, 0u, 0x0000000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x617FFE8000000000ull},
    {kFpRsqrte, 0u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpRsqrte, 0u, 0x0000000000000000ull, 0xBFF8000000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpRsqrte, 0u, 0x0000000000000000ull, 0xFFF0000000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpRsqrte, 0u, 0x0000000000000000ull, 0xFFF8000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpRsqrte, 0u, 0x0000000000000000ull, 0xFFF0000000000001ull, 0x0000000000000000ull, 0xFFF8000000000001ull},
    {kFpAdd, 0u, 0x0000000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x0000000000000001ull},
    {kFpAdd, 0u, 0x0000000000000000ull, 0x7FF8000000000123ull, 0x0000000000000000ull, 0x7FF8000000000123ull},
    {kFpAdd, 0u, 0x8000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull},
    {kFpAdd, 0u, 0x0000000000000001ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x0000000000000002ull},
    {kFpAdd, 0u, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull},
    {kFpAdd, 0u, 0x800FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x801FFFFFFFFFFFFEull},
    {kFpAdd, 0u, 0x0010000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x0010000000000001ull},
    {kFpAdd, 0u, 0x3F2FFFF800000000ull, 0xFFF0000000000001ull, 0x0000000000000000ull, 0xFFF8000000000001ull},
    {kFpAdd, 0u, 0xFFF8000000000000ull, 0xBFF8000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpAdd, 0u, 0xFFF0000000000001ull, 0xFFF0000000000001ull, 0x0000000000000000ull, 0xFFF8000000000001ull},
    {kFpSub, 0u, 0x0000000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x8000000000000001ull},
    {kFpSub, 0u, 0x0000000000000000ull, 0x7FF8000000000123ull, 0x0000000000000000ull, 0x7FF8000000000123ull},
    {kFpSub, 0u, 0x8000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x000FFFFFFFFFFFFFull},
    {kFpSub, 0u, 0x0000000000000001ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0010000000000000ull},
    {kFpSub, 0u, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull},
    {kFpSub, 0u, 0x800FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x0000000000000000ull, 0x801FFFFFFFFFFFFFull},
    {kFpSub, 0u, 0x0010000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x001FFFFFFFFFFFFFull},
    {kFpSub, 0u, 0x3F2FFFF800000000ull, 0xFFF0000000000001ull, 0x0000000000000000ull, 0xFFF8000000000001ull},
    {kFpSub, 0u, 0xFFF8000000000000ull, 0xBFF8000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpSub, 0u, 0xFFF0000000000001ull, 0xFFF0000000000001ull, 0x0000000000000000ull, 0xFFF8000000000001ull},
    {kFpDiv, 0u, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpDiv, 0u, 0x0000000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpDiv, 0u, 0x0000000000000001ull, 0xB7A16C2000000000ull, 0x0000000000000000ull, 0x851D633453329A63ull},
    {kFpDiv, 0u, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0000000000000000ull, 0xFFF0000000000000ull},
    {kFpDiv, 0u, 0x800FFFFFFFFFFFFFull, 0x3B90000000000000ull, 0x0000000000000000ull, 0x846FFFFFFFFFFFFEull},
    {kFpDiv, 0u, 0x0010000000000000ull, 0x3FF0000020000000ull, 0x0000000000000000ull, 0x000FFFFFE0000040ull},
    {kFpDiv, 0u, 0x39B0000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x7CD0000000000000ull},
    {kFpDiv, 0u, 0x3F2FFFF800000000ull, 0xFFF0000000000001ull, 0x0000000000000000ull, 0xFFF8000000000001ull},
    {kFpDiv, 0u, 0xFFF8000000000000ull, 0x3FB999999999999Aull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpMul, 0u, 0x0000000000000000ull, 0x0000000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull},
    {kFpMul, 0u, 0x0000000000000001ull, 0x0000000000000000ull, 0x3FF0000000000000ull, 0x0000000000000001ull},
    {kFpMul, 0u, 0x0000000000000001ull, 0x0000000000000000ull, 0xFFF0000000000000ull, 0xFFF0000000000000ull},
    {kFpMul, 0u, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x4C70000000000000ull, 0x8C8FFFFFFFFFFFFEull},
    {kFpMul, 0u, 0x3FF0000000000000ull, 0x0000000000000000ull, 0x0000000000000001ull, 0x0000000000000001ull},
    {kFpMul, 0u, 0x3FB999999999999Aull, 0x0000000000000000ull, 0x0020000000000000ull, 0x0003333333333333ull},
    {kFpMul, 0u, 0x3F2FFFF800000000ull, 0x0000000000000000ull, 0x7FF4000000000000ull, 0x7FFC000000000000ull},
    {kFpMul, 0u, 0x4C70000000000000ull, 0x0000000000000000ull, 0x0000000000000001ull, 0x0950000000000000ull},
    {kFpMul, 0u, 0xFFF8000000000000ull, 0x0000000000000000ull, 0xBFF8000000000000ull, 0xFFF8000000000000ull},
    {kFpMAdd, 0u, 0x0000000000000000ull, 0x0000000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull},
    {kFpMAdd, 0u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull},
    {kFpMAdd, 0u, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x3820000000000000ull, 0x8000000000000000ull},
    {kFpMAdd, 0u, 0x800FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x3820000000000000ull, 0x800FFFFFFFFFFFFFull},
    {kFpMAdd, 0u, 0x800FFFFFFFFFFFFFull, 0x3FB999999999999Aull, 0x7FF0000000000000ull, 0xFFF0000000000000ull},
    {kFpMAdd, 0u, 0x3FF0000000000000ull, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull},
    {kFpMAdd, 0u, 0x3FF0000000000000ull, 0x3820000000000000ull, 0xFFF8000000000000ull, 0xFFF8000000000000ull},
    {kFpMAdd, 0u, 0x3FB999999999999Aull, 0x3FB999999999999Aull, 0x3FB999999999999Aull, 0x3FBC28F5C28F5C29ull},
    {kFpMAdd, 0u, 0x47EFFFFFE0000000ull, 0x800FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x880FFFFFDFFFFFFEull},
    {kFpMAdd, 0u, 0xFFF8000000000000ull, 0x0000000000000000ull, 0x47EFFFFFE0000000ull, 0xFFF8000000000000ull},
    {kFpMSub, 0u, 0x0000000000000000ull, 0x0000000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull},
    {kFpMSub, 0u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x000FFFFFFFFFFFFFull},
    {kFpMSub, 0u, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x3FF0000000000000ull, 0x800FFFFFFFFFFFFFull},
    {kFpMSub, 0u, 0x800FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x3820000000000000ull, 0x000FFFFFFFFFFFFFull},
    {kFpMSub, 0u, 0x800FFFFFFFFFFFFFull, 0x3FB999999999999Aull, 0x7FF0000000000000ull, 0xFFF0000000000000ull},
    {kFpMSub, 0u, 0x3FF0000000000000ull, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull},
    {kFpMSub, 0u, 0x3FF0000000000000ull, 0x47EFFFFFE0000000ull, 0xFFF8000000000000ull, 0xFFF8000000000000ull},
    {kFpMSub, 0u, 0x3FB999999999999Aull, 0x3FB999999999999Aull, 0x3FB999999999999Aull, 0xBFB70A3D70A3D70Bull},
    {kFpMSub, 0u, 0x47EFFFFFE0000000ull, 0x800FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x880FFFFFDFFFFFFEull},
    {kFpMSub, 0u, 0xFFF8000000000000ull, 0x0000000000000000ull, 0x3FB999999999999Aull, 0xFFF8000000000000ull},
    {kFpNMAdd, 0u, 0x0000000000000000ull, 0x0000000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull},
    {kFpNMAdd, 0u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x000FFFFFFFFFFFFFull},
    {kFpNMAdd, 0u, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x3820000000000000ull, 0x0000000000000000ull},
    {kFpNMAdd, 0u, 0x800FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x3820000000000000ull, 0x000FFFFFFFFFFFFFull},
    {kFpNMAdd, 0u, 0x800FFFFFFFFFFFFFull, 0x3FB999999999999Aull, 0x7FF0000000000000ull, 0x7FF0000000000000ull},
    {kFpNMAdd, 0u, 0x3FF0000000000000ull, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x000FFFFFFFFFFFFFull},
    {kFpNMAdd, 0u, 0x3FF0000000000000ull, 0x3820000000000000ull, 0xFFF8000000000000ull, 0xFFF8000000000000ull},
    {kFpNMAdd, 0u, 0x3FB999999999999Aull, 0x3FB999999999999Aull, 0x3FB999999999999Aull, 0xBFBC28F5C28F5C29ull},
    {kFpNMAdd, 0u, 0x47EFFFFFE0000000ull, 0x800FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x080FFFFFDFFFFFFEull},
    {kFpNMAdd, 0u, 0xFFF8000000000000ull, 0x0000000000000000ull, 0x47EFFFFFE0000000ull, 0xFFF8000000000000ull},
    {kFpNMSub, 0u, 0x0000000000000000ull, 0x0000000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull},
    {kFpNMSub, 0u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull},
    {kFpNMSub, 0u, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x3FF0000000000000ull, 0x000FFFFFFFFFFFFFull},
    {kFpNMSub, 0u, 0x800FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x3820000000000000ull, 0x800FFFFFFFFFFFFFull},
    {kFpNMSub, 0u, 0x800FFFFFFFFFFFFFull, 0x3FB999999999999Aull, 0x7FF0000000000000ull, 0x7FF0000000000000ull},
    {kFpNMSub, 0u, 0x3FF0000000000000ull, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x000FFFFFFFFFFFFFull},
    {kFpNMSub, 0u, 0x3FF0000000000000ull, 0x47EFFFFFE0000000ull, 0xFFF8000000000000ull, 0xFFF8000000000000ull},
    {kFpNMSub, 0u, 0x3FB999999999999Aull, 0x3FB999999999999Aull, 0x3FB999999999999Aull, 0x3FB70A3D70A3D70Bull},
    {kFpNMSub, 0u, 0x47EFFFFFE0000000ull, 0x800FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x080FFFFFDFFFFFFEull},
    {kFpNMSub, 0u, 0xFFF8000000000000ull, 0x0000000000000000ull, 0x3FB999999999999Aull, 0xFFF8000000000000ull},
    {kFpSqrt, 1u, 0x0000000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpSqrt, 1u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpSqrt, 1u, 0x0000000000000000ull, 0xBFF8000000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpRsqrte, 1u, 0x0000000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x617FFE8000000000ull},
    {kFpRsqrte, 1u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpRsqrte, 1u, 0x0000000000000000ull, 0xBFF8000000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpAdd, 1u, 0x0000000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpAdd, 1u, 0x8000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpAdd, 1u, 0x0000000000000001ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpAdd, 1u, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpAdd, 1u, 0x800FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpAdd, 1u, 0x0010000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x0010000000000000ull},
    {kFpAdd, 1u, 0x3F2FFFF800000000ull, 0x7FF4000000000000ull, 0x0000000000000000ull, 0x7FFC000000000000ull},
    {kFpAdd, 1u, 0xFFF8000000000000ull, 0x3FF0000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpSub, 1u, 0x0000000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpSub, 1u, 0x8000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpSub, 1u, 0x0000000000000001ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpSub, 1u, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpSub, 1u, 0x800FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x0000000000000000ull, 0x8010000000000000ull},
    {kFpSub, 1u, 0x0010000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0010000000000000ull},
    {kFpSub, 1u, 0x3F2FFFF800000000ull, 0x7FF4000000000000ull, 0x0000000000000000ull, 0x7FFC000000000000ull},
    {kFpSub, 1u, 0xFFF8000000000000ull, 0x3FF0000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpDiv, 1u, 0x0000000000000000ull, 0xFFF8000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpDiv, 1u, 0x0000000000000001ull, 0x0010000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpDiv, 1u, 0x0000000000000001ull, 0xBFF8000000000000ull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpDiv, 1u, 0x800FFFFFFFFFFFFFull, 0x3810000000000000ull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpDiv, 1u, 0x800FFFFFFFFFFFFFull, 0x3FF0000020000000ull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpDiv, 1u, 0x36A0000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0xFFF0000000000000ull},
    {kFpDiv, 1u, 0x3B90000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x7FF0000000000000ull},
    {kFpDiv, 1u, 0x3FB999999999999Aull, 0x7FF8000000000123ull, 0x0000000000000000ull, 0x7FF8000000000123ull},
    {kFpDiv, 1u, 0xFFF8000000000000ull, 0x3810000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpMul, 1u, 0x0000000000000000ull, 0x0000000000000000ull, 0x7FF8000000000123ull, 0x7FF8000000000123ull},
    {kFpMul, 1u, 0x0000000000000001ull, 0x0000000000000000ull, 0x3FF0000000000000ull, 0x0000000000000000ull},
    {kFpMul, 1u, 0x0000000000000001ull, 0x0000000000000000ull, 0xFFE0000000000000ull, 0x8000000000000000ull},
    {kFpMul, 1u, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x3FF0000020000000ull, 0x8000000000000000ull},
    {kFpMul, 1u, 0x0020000000000000ull, 0x0000000000000000ull, 0x3FB999999999999Aull, 0x0000000000000000ull},
    {kFpMul, 1u, 0x3FF0000002000000ull, 0x0000000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull},
    {kFpMul, 1u, 0x3F2FFFF800000000ull, 0x0000000000000000ull, 0x0010000000000000ull, 0x0000000000000000ull},
    {kFpMul, 1u, 0x3F2FFFF800000000ull, 0x0000000000000000ull, 0x7FF8000000000123ull, 0x7FF8000000000123ull},
    {kFpMul, 1u, 0xFFF8000000000000ull, 0x0000000000000000ull, 0xB7A16C2000000000ull, 0xFFF8000000000000ull},
    {kFpMAdd, 1u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpMAdd, 1u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x3FB999999999999Aull, 0x0000000000000000ull},
    {kFpMAdd, 1u, 0x0000000000000000ull, 0x36A0000000000000ull, 0xFFF8000000000000ull, 0xFFF8000000000000ull},
    {kFpMAdd, 1u, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x3FB999999999999Aull, 0x0000000000000000ull},
    {kFpMAdd, 1u, 0x800FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x3820000000000000ull, 0x8000000000000000ull},
    {kFpMAdd, 1u, 0x36A0000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpMAdd, 1u, 0x3FF0000000000000ull, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull},
    {kFpMAdd, 1u, 0x3FF0000000000000ull, 0x800FFFFFFFFFFFFFull, 0xFFF8000000000000ull, 0xFFF8000000000000ull},
    {kFpMAdd, 1u, 0x3FB999999999999Aull, 0x3FB999999999999Aull, 0x3FB999999999999Aull, 0x3FBC28F5C28F5C29ull},
    {kFpMAdd, 1u, 0x7FF0000000000000ull, 0xFFF8000000000000ull, 0x47EFFFFFE0000000ull, 0xFFF8000000000000ull},
    {kFpMSub, 1u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpMSub, 1u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x3FF0000000000000ull, 0x0000000000000000ull},
    {kFpMSub, 1u, 0x0000000000000000ull, 0x3FF0000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull},
    {kFpMSub, 1u, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x3FB999999999999Aull, 0x8000000000000000ull},
    {kFpMSub, 1u, 0x800FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x36A0000000000000ull, 0x0000000000000000ull},
    {kFpMSub, 1u, 0x36A0000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpMSub, 1u, 0x3FF0000000000000ull, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x8000000000000000ull},
    {kFpMSub, 1u, 0x3FF0000000000000ull, 0x47EFFFFFE0000000ull, 0xFFF8000000000000ull, 0xFFF8000000000000ull},
    {kFpMSub, 1u, 0x3FB999999999999Aull, 0x3FB999999999999Aull, 0x3FB999999999999Aull, 0xBFB70A3D70A3D70Bull},
    {kFpMSub, 1u, 0x7FF0000000000000ull, 0xFFF8000000000000ull, 0x47EFFFFFE0000000ull, 0xFFF8000000000000ull},
    {kFpNMAdd, 1u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpNMAdd, 1u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x3FB999999999999Aull, 0x8000000000000000ull},
    {kFpNMAdd, 1u, 0x0000000000000000ull, 0x36A0000000000000ull, 0xFFF8000000000000ull, 0xFFF8000000000000ull},
    {kFpNMAdd, 1u, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x3FB999999999999Aull, 0x8000000000000000ull},
    {kFpNMAdd, 1u, 0x800FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x3820000000000000ull, 0x0000000000000000ull},
    {kFpNMAdd, 1u, 0x36A0000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpNMAdd, 1u, 0x3FF0000000000000ull, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x8000000000000000ull},
    {kFpNMAdd, 1u, 0x3FF0000000000000ull, 0x800FFFFFFFFFFFFFull, 0xFFF8000000000000ull, 0xFFF8000000000000ull},
    {kFpNMAdd, 1u, 0x3FB999999999999Aull, 0x3FB999999999999Aull, 0x3FB999999999999Aull, 0xBFBC28F5C28F5C29ull},
    {kFpNMAdd, 1u, 0x7FF0000000000000ull, 0xFFF8000000000000ull, 0x47EFFFFFE0000000ull, 0xFFF8000000000000ull},
    {kFpNMSub, 1u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpNMSub, 1u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x3FF0000000000000ull, 0x8000000000000000ull},
    {kFpNMSub, 1u, 0x0000000000000000ull, 0x3FF0000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull},
    {kFpNMSub, 1u, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x3FB999999999999Aull, 0x0000000000000000ull},
    {kFpNMSub, 1u, 0x800FFFFFFFFFFFFFull, 0x800FFFFFFFFFFFFFull, 0x36A0000000000000ull, 0x8000000000000000ull},
    {kFpNMSub, 1u, 0x36A0000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpNMSub, 1u, 0x3FF0000000000000ull, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull},
    {kFpNMSub, 1u, 0x3FF0000000000000ull, 0x47EFFFFFE0000000ull, 0xFFF8000000000000ull, 0xFFF8000000000000ull},
    {kFpNMSub, 1u, 0x3FB999999999999Aull, 0x3FB999999999999Aull, 0x3FB999999999999Aull, 0x3FB70A3D70A3D70Bull},
    {kFpNMSub, 1u, 0x7FF0000000000000ull, 0xFFF8000000000000ull, 0x47EFFFFFE0000000ull, 0xFFF8000000000000ull},
    {kFpSqrt | kFpSingle, 0u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpSqrt | kFpSingle, 0u, 0x0000000000000000ull, 0x36A0000000000000ull, 0x0000000000000000ull, 0x3B46A09E60000000ull},
    {kFpSqrt | kFpSingle, 0u, 0x0000000000000000ull, 0xBFF8000000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpSqrt | kFpSingle, 0u, 0x0000000000000000ull, 0xFFF0000000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpSqrt | kFpSingle, 0u, 0x0000000000000000ull, 0xFFF8000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpSqrt | kFpSingle, 0u, 0x0000000000000000ull, 0xFFF0000000000001ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpRes | kFpSingle, 0u, 0x0000000000000000ull, 0x47EFFFFFE0000000ull, 0x0000000000000000ull, 0x37F0001000000000ull},
    {kFpRes | kFpSingle, 0u, 0x0000000000000000ull, 0x7FF8000000000123ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpRes | kFpSingle, 0u, 0x0000000000000000ull, 0xFFF8000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpRes | kFpSingle, 0u, 0x0000000000000000ull, 0x7FF4000000000000ull, 0x0000000000000000ull, 0x7FFC000000000000ull},
    {kFpRes | kFpSingle, 0u, 0x0000000000000000ull, 0xFFF0000000000001ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpRsqrte | kFpSingle, 0u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpRsqrte | kFpSingle, 0u, 0x0000000000000000ull, 0x36A0000000000000ull, 0x0000000000000000ull, 0x44969FA000000000ull},
    {kFpRsqrte | kFpSingle, 0u, 0x0000000000000000ull, 0xBFF8000000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpRsqrte | kFpSingle, 0u, 0x0000000000000000ull, 0xFFF0000000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpRsqrte | kFpSingle, 0u, 0x0000000000000000ull, 0xFFF8000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpRsqrte | kFpSingle, 0u, 0x0000000000000000ull, 0xFFF0000000000001ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpRound | kFpSingle, 0u, 0x0000000000000000ull, 0x36A0000000000000ull, 0x0000000000000000ull, 0x36A0000000000000ull},
    {kFpRound | kFpSingle, 0u, 0x0000000000000000ull, 0xB7A16C2000000000ull, 0x0000000000000000ull, 0xB7A16C2000000000ull},
    {kFpRound | kFpSingle, 0u, 0x0000000000000000ull, 0x7FF8000000000123ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpRound | kFpSingle, 0u, 0x0000000000000000ull, 0xFFF8000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpRound | kFpSingle, 0u, 0x0000000000000000ull, 0x7FF4000000000000ull, 0x0000000000000000ull, 0x7FFC000000000000ull},
    {kFpRound | kFpSingle, 0u, 0x0000000000000000ull, 0xFFF0000000000001ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpAdd | kFpSingle, 0u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpAdd | kFpSingle, 0u, 0x0000000000000000ull, 0x7FF8000000000123ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpAdd | kFpSingle, 0u, 0x0000000000000001ull, 0x36A0000000000000ull, 0x0000000000000000ull, 0x36A0000000000000ull},
    {kFpAdd | kFpSingle, 0u, 0x0010000000000000ull, 0x36A0000000000000ull, 0x0000000000000000ull, 0x36A0000000000000ull},
    {kFpAdd | kFpSingle, 0u, 0x36A0000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x36A0000000000000ull},
    {kFpAdd | kFpSingle, 0u, 0x36A0000000000000ull, 0x3810000000000000ull, 0x0000000000000000ull, 0x3810000020000000ull},
    {kFpAdd | kFpSingle, 0u, 0xB7A16C2000000000ull, 0x0020000000000000ull, 0x0000000000000000ull, 0xB7A16C2000000000ull},
    {kFpAdd | kFpSingle, 0u, 0x3F2FFFF800000000ull, 0xFFF0000000000001ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpAdd | kFpSingle, 0u, 0xFFF8000000000000ull, 0xBFF8000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpAdd | kFpSingle, 0u, 0xFFF0000000000001ull, 0xFFF0000000000001ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpSub | kFpSingle, 0u, 0x0000000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpSub | kFpSingle, 0u, 0x0000000000000000ull, 0x7FF8000000000123ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpSub | kFpSingle, 0u, 0x8000000000000000ull, 0xB7A16C2000000000ull, 0x0000000000000000ull, 0x37A16C2000000000ull},
    {kFpSub | kFpSingle, 0u, 0x800FFFFFFFFFFFFFull, 0x36A0000000000000ull, 0x0000000000000000ull, 0xB6A0000000000000ull},
    {kFpSub | kFpSingle, 0u, 0x0020000000000000ull, 0xB7A16C2000000000ull, 0x0000000000000000ull, 0x37A16C2000000000ull},
    {kFpSub | kFpSingle, 0u, 0x36A0000000000000ull, 0x0020000000000000ull, 0x0000000000000000ull, 0x36A0000000000000ull},
    {kFpSub | kFpSingle, 0u, 0xB7A16C2000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0xB7A16C2000000000ull},
    {kFpSub | kFpSingle, 0u, 0x3F2FFFF800000000ull, 0xFFF0000000000001ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpSub | kFpSingle, 0u, 0xFFF8000000000000ull, 0xBFF8000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpSub | kFpSingle, 0u, 0xFFF0000000000001ull, 0xFFF0000000000001ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpDiv | kFpSingle, 0u, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpDiv | kFpSingle, 0u, 0x0000000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpDiv | kFpSingle, 0u, 0x0000000000000001ull, 0x0010000000000000ull, 0x0000000000000000ull, 0x3CB0000000000000ull},
    {kFpDiv | kFpSingle, 0u, 0x0010000000000000ull, 0x0010000000000000ull, 0x0000000000000000ull, 0x3FF0000000000000ull},
    {kFpDiv | kFpSingle, 0u, 0x36A0000000000000ull, 0x0010000000000000ull, 0x0000000000000000ull, 0x7FF0000000000000ull},
    {kFpDiv | kFpSingle, 0u, 0xB7A16C2000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0xFFF0000000000000ull},
    {kFpDiv | kFpSingle, 0u, 0x3810000000000000ull, 0x36A0000000000000ull, 0x0000000000000000ull, 0x4160000000000000ull},
    {kFpDiv | kFpSingle, 0u, 0x47EFFFFFE0000000ull, 0xFFF0000000000001ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpDiv | kFpSingle, 0u, 0x7FF4000000000000ull, 0x0010000000000000ull, 0x0000000000000000ull, 0x7FFC000000000000ull},
    {kFpMul | kFpSingle, 0u, 0x0000000000000000ull, 0x0000000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull},
    {kFpMul | kFpSingle, 0u, 0x0000000000000001ull, 0x0000000000000000ull, 0xFFE0000000000000ull, 0xBCC0000000000000ull},
    {kFpMul | kFpSingle, 0u, 0x36A0000000000000ull, 0x0000000000000000ull, 0x3FF0000000000000ull, 0x36A0000000000000ull},
    {kFpMul | kFpSingle, 0u, 0xB7A16C2000000000ull, 0x0000000000000000ull, 0x3FB999999999999Aull, 0xB76BE00000000000ull},
    {kFpMul | kFpSingle, 0u, 0x3B90000000000000ull, 0x0000000000000000ull, 0x3B90000000000000ull, 0x3730000000000000ull},
    {kFpMul | kFpSingle, 0u, 0x3F2FFFF800000000ull, 0x0000000000000000ull, 0x3820000000000000ull, 0x3760000000000000ull},
    {kFpMul | kFpSingle, 0u, 0x3FF0000020000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull, 0xFFF8000000000000ull},
    {kFpMul | kFpSingle, 0u, 0xFFE0000000000000ull, 0x0000000000000000ull, 0xB7A16C2000000000ull, 0x7FF0000000000000ull},
    {kFpMul | kFpSingle, 0u, 0xFFF8000000000000ull, 0x0000000000000000ull, 0x4C70000000000000ull, 0xFFF8000000000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x0000000000000000ull, 0x0000000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x47EFFFFFE0000000ull, 0x8000000000000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x36A0000000000000ull, 0x0000000000000000ull, 0x3FF0000000000000ull, 0x36A0000000000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x36A0000000000000ull, 0x3FF0000000000000ull, 0x47EFFFFFE0000000ull, 0x3FF0000080000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x3FF0000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x3FF0000000000000ull, 0x47EFFFFFE0000000ull, 0xFFF8000000000000ull, 0xFFF8000000000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x47EFFFFFE0000000ull, 0x36A0000000000000ull, 0x0000000000000000ull, 0x36A0000000000000ull},
    {kFpMAdd | kFpSingle, 0u, 0xFFF8000000000000ull, 0x800FFFFFFFFFFFFFull, 0x3820000000000000ull, 0xFFF8000000000000ull},
    {kFpMSub | kFpSingle, 0u, 0x0000000000000000ull, 0x0000000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull},
    {kFpMSub | kFpSingle, 0u, 0x0000000000000000ull, 0x36A0000000000000ull, 0x0000000000000000ull, 0xB6A0000000000000ull},
    {kFpMSub | kFpSingle, 0u, 0x800FFFFFFFFFFFFFull, 0x36A0000000000000ull, 0x3FF0000000000000ull, 0xB6A0000000000000ull},
    {kFpMSub | kFpSingle, 0u, 0x36A0000000000000ull, 0x800FFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0x7FF0000000000000ull},
    {kFpMSub | kFpSingle, 0u, 0x36A0000000000000ull, 0x3FB999999999999Aull, 0x7FF0000000000000ull, 0x7FF0000000000000ull},
    {kFpMSub | kFpSingle, 0u, 0x3FF0000000000000ull, 0x36A0000000000000ull, 0x3820000000000000ull, 0x381FFFFFE0000000ull},
    {kFpMSub | kFpSingle, 0u, 0x3FF0000000000000ull, 0x7FF0000000000000ull, 0xFFF8000000000000ull, 0xFFF8000000000000ull},
    {kFpMSub | kFpSingle, 0u, 0x3FB999999999999Aull, 0x47EFFFFFE0000000ull, 0x47EFFFFFE0000000ull, 0xC7ECCCCCA0000000ull},
    {kFpMSub | kFpSingle, 0u, 0x47EFFFFFE0000000ull, 0x3820000000000000ull, 0x36A0000000000000ull, 0x3E9FFFFFE0000000ull},
    {kFpMSub | kFpSingle, 0u, 0x47EFFFFFE0000000ull, 0x47EFFFFFE0000000ull, 0x3FB999999999999Aull, 0xC7ECCCCCA0000000ull},
    {kFpMSub | kFpSingle, 0u, 0xFFF8000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x0000000000000000ull, 0x0000000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x47EFFFFFE0000000ull, 0x0000000000000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x36A0000000000000ull, 0x0000000000000000ull, 0x3FF0000000000000ull, 0xB6A0000000000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x36A0000000000000ull, 0x3FF0000000000000ull, 0x47EFFFFFE0000000ull, 0xBFF0000080000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x3FF0000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x3FF0000000000000ull, 0x47EFFFFFE0000000ull, 0xFFF8000000000000ull, 0xFFF8000000000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x47EFFFFFE0000000ull, 0x36A0000000000000ull, 0x0000000000000000ull, 0xB6A0000000000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0xFFF8000000000000ull, 0x800FFFFFFFFFFFFFull, 0x3820000000000000ull, 0xFFF8000000000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x0000000000000000ull, 0x0000000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x0000000000000000ull, 0x36A0000000000000ull, 0x0000000000000000ull, 0x36A0000000000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x800FFFFFFFFFFFFFull, 0x36A0000000000000ull, 0x3FF0000000000000ull, 0x36A0000000000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x36A0000000000000ull, 0x800FFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0xFFF0000000000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x36A0000000000000ull, 0x3FB999999999999Aull, 0x7FF0000000000000ull, 0xFFF0000000000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x3FF0000000000000ull, 0x36A0000000000000ull, 0x3820000000000000ull, 0xB81FFFFFE0000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x3FF0000000000000ull, 0x7FF0000000000000ull, 0xFFF8000000000000ull, 0xFFF8000000000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x3FB999999999999Aull, 0x47EFFFFFE0000000ull, 0x47EFFFFFE0000000ull, 0x47ECCCCCA0000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x47EFFFFFE0000000ull, 0x3820000000000000ull, 0x36A0000000000000ull, 0xBE9FFFFFE0000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x47EFFFFFE0000000ull, 0x47EFFFFFE0000000ull, 0x3FB999999999999Aull, 0x47ECCCCCA0000000ull},
    {kFpNMSub | kFpSingle, 0u, 0xFFF8000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpSqrt | kFpSingle, 1u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpSqrt | kFpSingle, 1u, 0x0000000000000000ull, 0x36A0000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpSqrt | kFpSingle, 1u, 0x0000000000000000ull, 0xB7A16C2000000000ull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpSqrt | kFpSingle, 1u, 0x0000000000000000ull, 0xFFE0000000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpRes | kFpSingle, 1u, 0x0000000000000000ull, 0x47EFFFFFE0000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpRes | kFpSingle, 1u, 0x0000000000000000ull, 0x7FF8000000000123ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpRes | kFpSingle, 1u, 0x0000000000000000ull, 0xFFF8000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpRsqrte | kFpSingle, 1u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpRsqrte | kFpSingle, 1u, 0x0000000000000000ull, 0x36A0000000000000ull, 0x0000000000000000ull, 0x44969FA000000000ull},
    {kFpRsqrte | kFpSingle, 1u, 0x0000000000000000ull, 0xB7A16C2000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpRsqrte | kFpSingle, 1u, 0x0000000000000000ull, 0xFFE0000000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpRound | kFpSingle, 1u, 0x0000000000000000ull, 0x36A0000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpRound | kFpSingle, 1u, 0x0000000000000000ull, 0xB7A16C2000000000ull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpRound | kFpSingle, 1u, 0x0000000000000000ull, 0x7FF8000000000123ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpRound | kFpSingle, 1u, 0x0000000000000000ull, 0xFFF8000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpAdd | kFpSingle, 1u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpAdd | kFpSingle, 1u, 0x0000000000000001ull, 0x36A0000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpAdd | kFpSingle, 1u, 0x0010000000000000ull, 0x36A0000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpAdd | kFpSingle, 1u, 0x36A0000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpAdd | kFpSingle, 1u, 0x36A0000000000000ull, 0x3810000000000000ull, 0x0000000000000000ull, 0x3810000000000000ull},
    {kFpAdd | kFpSingle, 1u, 0xB7A16C2000000000ull, 0x0020000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpAdd | kFpSingle, 1u, 0x3F2FFFF800000000ull, 0x7FF4000000000000ull, 0x0000000000000000ull, 0x7FFC000000000000ull},
    {kFpAdd | kFpSingle, 1u, 0xFFF8000000000000ull, 0x3FF0000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpSub | kFpSingle, 1u, 0x0000000000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpSub | kFpSingle, 1u, 0x8000000000000000ull, 0xB7A16C2000000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpSub | kFpSingle, 1u, 0x800FFFFFFFFFFFFFull, 0x36A0000000000000ull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpSub | kFpSingle, 1u, 0x0020000000000000ull, 0xB7A16C2000000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpSub | kFpSingle, 1u, 0x36A0000000000000ull, 0x0020000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpSub | kFpSingle, 1u, 0xB7A16C2000000000ull, 0x0000000000000001ull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpSub | kFpSingle, 1u, 0x3F2FFFF800000000ull, 0x7FF4000000000000ull, 0x0000000000000000ull, 0x7FFC000000000000ull},
    {kFpSub | kFpSingle, 1u, 0xFFF8000000000000ull, 0x3FF0000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpDiv | kFpSingle, 1u, 0x0000000000000001ull, 0x0020000000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpDiv | kFpSingle, 1u, 0x36A0000000000000ull, 0x3810000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpDiv | kFpSingle, 1u, 0x36A0000000000000ull, 0x3FF0000002000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpDiv | kFpSingle, 1u, 0xB7A16C2000000000ull, 0x39B0000000000000ull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpDiv | kFpSingle, 1u, 0xB7A16C2000000000ull, 0x3F2FFFF800000000ull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpDiv | kFpSingle, 1u, 0x3810000000000000ull, 0x3FF0000020000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpDiv | kFpSingle, 1u, 0x3B90000000000000ull, 0xB7A16C2000000000ull, 0x0000000000000000ull, 0xFFF0000000000000ull},
    {kFpDiv | kFpSingle, 1u, 0x3B90000000000000ull, 0x7FF8000000000123ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpDiv | kFpSingle, 1u, 0xFFF8000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpMul | kFpSingle, 1u, 0x8000000000000000ull, 0x0000000000000000ull, 0xFFF0000000000001ull, 0xFFF8000000000000ull},
    {kFpMul | kFpSingle, 1u, 0x0000000000000001ull, 0x0000000000000000ull, 0xFFE0000000000000ull, 0x8000000000000000ull},
    {kFpMul | kFpSingle, 1u, 0x36A0000000000000ull, 0x0000000000000000ull, 0x47EFFFFFE0000000ull, 0x0000000000000000ull},
    {kFpMul | kFpSingle, 1u, 0xB7A16C2000000000ull, 0x0000000000000000ull, 0x3FF0000020000000ull, 0x8000000000000000ull},
    {kFpMul | kFpSingle, 1u, 0x3B90000000000000ull, 0x0000000000000000ull, 0x3B90000000000000ull, 0x0000000000000000ull},
    {kFpMul | kFpSingle, 1u, 0x3FB999999999999Aull, 0x0000000000000000ull, 0x3810000000000000ull, 0x0000000000000000ull},
    {kFpMul | kFpSingle, 1u, 0x3FF0000020000000ull, 0x0000000000000000ull, 0x7FF8000000000123ull, 0x7FF8000000000000ull},
    {kFpMul | kFpSingle, 1u, 0x47EFFFFFE0000000ull, 0x0000000000000000ull, 0xB7A16C2000000000ull, 0x8000000000000000ull},
    {kFpMul | kFpSingle, 1u, 0xFFF8000000000000ull, 0x0000000000000000ull, 0x36A0000000000000ull, 0xFFF8000000000000ull},
    {kFpMAdd | kFpSingle, 1u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpMAdd | kFpSingle, 1u, 0x0000000000000000ull, 0x36A0000000000000ull, 0x47EFFFFFE0000000ull, 0x0000000000000000ull},
    {kFpMAdd | kFpSingle, 1u, 0x0000000000000000ull, 0x7FF0000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull},
    {kFpMAdd | kFpSingle, 1u, 0x800FFFFFFFFFFFFFull, 0x36A0000000000000ull, 0x3FB999999999999Aull, 0x0000000000000000ull},
    {kFpMAdd | kFpSingle, 1u, 0x36A0000000000000ull, 0x36A0000000000000ull, 0x3FF0000000000000ull, 0x0000000000000000ull},
    {kFpMAdd | kFpSingle, 1u, 0x3820000000000000ull, 0x36A0000000000000ull, 0x36A0000000000000ull, 0x0000000000000000ull},
    {kFpMAdd | kFpSingle, 1u, 0x3FF0000000000000ull, 0x3FB999999999999Aull, 0xFFF8000000000000ull, 0xFFF8000000000000ull},
    {kFpMAdd | kFpSingle, 1u, 0x3FB999999999999Aull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpMAdd | kFpSingle, 1u, 0x7FF0000000000000ull, 0xFFF8000000000000ull, 0x3FB999999999999Aull, 0xFFF8000000000000ull},
    {kFpMSub | kFpSingle, 1u, 0x0000000000000000ull, 0x36A0000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpMSub | kFpSingle, 1u, 0x0000000000000000ull, 0x7FF0000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull},
    {kFpMSub | kFpSingle, 1u, 0x800FFFFFFFFFFFFFull, 0x36A0000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull},
    {kFpMSub | kFpSingle, 1u, 0x36A0000000000000ull, 0x800FFFFFFFFFFFFFull, 0x47EFFFFFE0000000ull, 0x0000000000000000ull},
    {kFpMSub | kFpSingle, 1u, 0x36A0000000000000ull, 0x3FF0000000000000ull, 0x47EFFFFFE0000000ull, 0xBFF0000000000000ull},
    {kFpMSub | kFpSingle, 1u, 0x3820000000000000ull, 0x36A0000000000000ull, 0x3FB999999999999Aull, 0x0000000000000000ull},
    {kFpMSub | kFpSingle, 1u, 0x3FF0000000000000ull, 0x7FF0000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull},
    {kFpMSub | kFpSingle, 1u, 0x3FB999999999999Aull, 0x36A0000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpMSub | kFpSingle, 1u, 0x3FB999999999999Aull, 0x47EFFFFFE0000000ull, 0x47EFFFFFE0000000ull, 0xC7ECCCCCA0000000ull},
    {kFpMSub | kFpSingle, 1u, 0x47EFFFFFE0000000ull, 0x47EFFFFFE0000000ull, 0x3FB999999999999Aull, 0xC7ECCCCCA0000000ull},
    {kFpMSub | kFpSingle, 1u, 0x7FF0000000000000ull, 0xFFF8000000000000ull, 0x36A0000000000000ull, 0xFFF8000000000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x0000000000000000ull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x0000000000000000ull, 0x36A0000000000000ull, 0x47EFFFFFE0000000ull, 0x8000000000000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x0000000000000000ull, 0x7FF0000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x800FFFFFFFFFFFFFull, 0x36A0000000000000ull, 0x3FB999999999999Aull, 0x8000000000000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x36A0000000000000ull, 0x36A0000000000000ull, 0x3FF0000000000000ull, 0x8000000000000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x3820000000000000ull, 0x36A0000000000000ull, 0x36A0000000000000ull, 0x8000000000000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x3FF0000000000000ull, 0x3FB999999999999Aull, 0xFFF8000000000000ull, 0xFFF8000000000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x3FB999999999999Aull, 0x800FFFFFFFFFFFFFull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x7FF0000000000000ull, 0xFFF8000000000000ull, 0x3FB999999999999Aull, 0xFFF8000000000000ull},
    {kFpNMSub | kFpSingle, 1u, 0x0000000000000000ull, 0x36A0000000000000ull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpNMSub | kFpSingle, 1u, 0x0000000000000000ull, 0x7FF0000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull},
    {kFpNMSub | kFpSingle, 1u, 0x800FFFFFFFFFFFFFull, 0x36A0000000000000ull, 0x800FFFFFFFFFFFFFull, 0x8000000000000000ull},
    {kFpNMSub | kFpSingle, 1u, 0x36A0000000000000ull, 0x800FFFFFFFFFFFFFull, 0x47EFFFFFE0000000ull, 0x8000000000000000ull},
    {kFpNMSub | kFpSingle, 1u, 0x36A0000000000000ull, 0x3FF0000000000000ull, 0x47EFFFFFE0000000ull, 0x3FF0000000000000ull},
    {kFpNMSub | kFpSingle, 1u, 0x3820000000000000ull, 0x36A0000000000000ull, 0x3FB999999999999Aull, 0x8000000000000000ull},
    {kFpNMSub | kFpSingle, 1u, 0x3FF0000000000000ull, 0x7FF0000000000000ull, 0x7FF0000000000000ull, 0x7FF8000000000000ull},
    {kFpNMSub | kFpSingle, 1u, 0x3FB999999999999Aull, 0x36A0000000000000ull, 0x0000000000000000ull, 0x8000000000000000ull},
    {kFpNMSub | kFpSingle, 1u, 0x3FB999999999999Aull, 0x47EFFFFFE0000000ull, 0x47EFFFFFE0000000ull, 0x47ECCCCCA0000000ull},
    {kFpNMSub | kFpSingle, 1u, 0x47EFFFFFE0000000ull, 0x47EFFFFFE0000000ull, 0x3FB999999999999Aull, 0x47ECCCCCA0000000ull},
    {kFpNMSub | kFpSingle, 1u, 0x7FF0000000000000ull, 0xFFF8000000000000ull, 0x36A0000000000000ull, 0xFFF8000000000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x412FFFFFC0000000ull, 0x4034C99F60000000ull, 0x3D70000020000000ull, 0xC034C99F60000000ull},
    {kFpMAdd | kFpSingle, 0u, 0xBF3FFFE000000000ull, 0x41B5BD27A0000000ull, 0xC0E0001000000000ull, 0x41B5BD27A0000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x402FFFF800000000ull, 0xC0AC537520000000ull, 0x3EE0000400000000ull, 0xC0AC537520000000ull},
    {kFpNMSub | kFpSingle, 1u, 0x3F0FFFFF00000000ull, 0xC0AA6C03E0000000ull, 0x4000000080000000ull, 0xC0AA6C03E0000000ull},
    {kFpMSub | kFpSingle, 1u, 0xC12FFFFF00000000ull, 0xBF08226CA0000000ull, 0xBC40000080000000ull, 0x3F08226CA0000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x406FFFFF80000000ull, 0x4141DFC660000000ull, 0x3F40000040000000ull, 0xC141DFC660000000ull},
    {kFpMAdd | kFpSingle, 0u, 0xC02FFFFF80000000ull, 0x4151B0E360000000ull, 0xBF90000040000000ull, 0x4151B0E360000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x402FFFFFC0000000ull, 0x3F33186DE0000000ull, 0x3D70000020000000ull, 0x3F33186DE0000000ull},
    {kFpMAdd | kFpSingle, 0u, 0xBFDFFFFF80000000ull, 0x40853A4660000000ull, 0xBF10000040000000ull, 0x40853A4660000000ull},
    {kFpMSub | kFpSingle, 0u, 0x403FFFE000000000ull, 0xC074C24FE0000000ull, 0x3EA0001000000000ull, 0x4074C24FE0000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0xC0DFFFC000000000ull, 0xC16F9D0020000000ull, 0xBEF0002000000000ull, 0x416F9D0020000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0xBDFFFFFFC0000000ull, 0x3F90B72AE0000000ull, 0xC000000020000000ull, 0xBF90B72AE0000000ull},
    {kFpMAdd | kFpSingle, 1u, 0xC02FFFFE00000000ull, 0x41C7A30F60000000ull, 0xC000000100000000ull, 0x41C7A30F60000000ull},
    {kFpMSub | kFpSingle, 0u, 0xBEAFFFFE00000000ull, 0x3E2BA92920000000ull, 0xBDE0000100000000ull, 0xBE2BA92920000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0xBE3FFFF800000000ull, 0xC098124B20000000ull, 0xC0C0000400000000ull, 0x4098124B20000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x3F0FFFFE00000000ull, 0xC19622A560000000ull, 0x40F0000100000000ull, 0x419622A560000000ull},
    {kFpNMSub | kFpSingle, 1u, 0x3DDFFFFF80000000ull, 0xC084485460000000ull, 0x4110000040000000ull, 0xC084485460000000ull},
    {kFpMAdd | kFpSingle, 1u, 0x3F5FFFC000000000ull, 0x41ABA971E0000000ull, 0x40B0002000000000ull, 0x41ABA971E0000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0xBD8FFFFF00000000ull, 0x40381D5BE0000000ull, 0xC110000080000000ull, 0xC0381D5BE0000000ull},
    {kFpMSub | kFpSingle, 1u, 0x3D2FFFE000000000ull, 0xBF52966AE0000000ull, 0x4090001000000000ull, 0x3F52966AE0000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0xBEDFFFFE00000000ull, 0xC110E9DB20000000ull, 0xC0A0000100000000ull, 0x4110E9DB20000000ull},
    {kFpNMSub | kFpSingle, 1u, 0x3FFFFFFF80000000ull, 0xC063C733A0000000ull, 0x3ED0000040000000ull, 0xC063C733A0000000ull},
    {kFpMSub | kFpSingle, 1u, 0xBFBFFFFC00000000ull, 0x41840239A0000000ull, 0xC030000200000000ull, 0xC1840239A0000000ull},
    {kFpMAdd | kFpSingle, 1u, 0x3D7FFFFE00000000ull, 0xBFBD037260000000ull, 0x40A0000100000000ull, 0xBFBD037260000000ull},
    {kFpNMSub | kFpSingle, 0u, 0xC0CFFFC000000000ull, 0x41452EF320000000ull, 0xBEE0002000000000ull, 0x41452EF320000000ull},
    {kFpMAdd | kFpSingle, 1u, 0xC03FFFF800000000ull, 0xC117CE84A0000000ull, 0xBF40000400000000ull, 0xC117CE84A0000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0xC00FFFFC00000000ull, 0xC00B1B60A0000000ull, 0xBE60000200000000ull, 0x400B1B60A0000000ull},
    {kFpMAdd | kFpSingle, 0u, 0xBF1FFFF800000000ull, 0x408895E9E0000000ull, 0xBFD0000400000000ull, 0x408895E9E0000000ull},
    {kFpMAdd | kFpSingle, 0u, 0xC11FFFFC00000000ull, 0xBFB6B3A2E0000000ull, 0xBD00000200000000ull, 0xBFB6B3A2E0000000ull},
    {kFpMAdd | kFpSingle, 1u, 0xBDAFFFF800000000ull, 0x3FE2A72E60000000ull, 0xC0A0000400000000ull, 0x3FE2A72E60000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x3EEFFFFE00000000ull, 0xBF161C3420000000ull, 0x3E90000100000000ull, 0x3F161C3420000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x3EDFFFE000000000ull, 0x41406E1920000000ull, 0x40D0001000000000ull, 0xC1406E1920000000ull},
    {kFpMAdd | kFpSingle, 0u, 0xBD5FFFFF00000000ull, 0xBFFE11BA60000000ull, 0xC100000080000000ull, 0xBFFE11BA60000000ull},
    {kFpMSub | kFpSingle, 0u, 0xC05FFFFF80000000ull, 0xBFC6C7E7A0000000ull, 0xBDD0000040000000ull, 0x3FC6C7E7A0000000ull},
    {kFpMAdd | kFpSingle, 1u, 0xBF9FFFC000000000ull, 0xC1CADD72E0000000ull, 0xC090002000000000ull, 0xC1CADD72E0000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x3C5FFFFFC0000000ull, 0xBF475DA260000000ull, 0x4150000020000000ull, 0x3F475DA260000000ull},
    {kFpMAdd | kFpSingle, 1u, 0xBE7FFFFF80000000ull, 0x402CF2B7A0000000ull, 0xC010000040000000ull, 0x402CF2B7A0000000ull},
    {kFpMAdd | kFpSingle, 1u, 0x404FFFFF00000000ull, 0x40DE3D4360000000ull, 0x3EF0000080000000ull, 0x40DE3D4360000000ull},
    {kFpNMSub | kFpSingle, 0u, 0xBFEFFFFFC0000000ull, 0xBF766255E0000000ull, 0xBDF0000020000000ull, 0xBF766255E0000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x3D3FFFF000000000ull, 0xBF8CF1C6E0000000ull, 0x40B0000800000000ull, 0x3F8CF1C6E0000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x407FFFC000000000ull, 0x40B260E5E0000000ull, 0x3EA0002000000000ull, 0x40B260E5E0000000ull},
    {kFpNMSub | kFpSingle, 1u, 0xBFDFFFF800000000ull, 0x3E1A64B0A0000000ull, 0xBCA0000400000000ull, 0x3E1A64B0A0000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x400FFFF800000000ull, 0x3FF3D170A0000000ull, 0x3E50000400000000ull, 0xBFF3D170A0000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0xBEAFFFFF00000000ull, 0xBEF74BFEA0000000ull, 0xBEB0000080000000ull, 0x3EF74BFEA0000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x40AFFFF000000000ull, 0xC09ED93D20000000ull, 0x3E50000800000000ull, 0x409ED93D20000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x3CCFFFE000000000ull, 0x3E117C2760000000ull, 0x3FB0001000000000ull, 0xBE117C2760000000ull},
    {kFpNMSub | kFpSingle, 0u, 0xBE2FFFFE00000000ull, 0xC0BC6E46E0000000ull, 0xC0F0000100000000ull, 0xC0BC6E46E0000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x3E4FFFC000000000ull, 0xC044CAA520000000ull, 0x4060002000000000ull, 0xC044CAA520000000ull},
    {kFpMAdd | kFpSingle, 1u, 0x406FFFFE00000000ull, 0x41DB4AEBE0000000ull, 0x3FD0000100000000ull, 0x41DB4AEBE0000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x3E1FFFFE00000000ull, 0xC061E57820000000ull, 0x40B0000100000000ull, 0xC061E57820000000ull},
    {kFpMAdd | kFpSingle, 0u, 0xC01FFFFF80000000ull, 0xC039DBD6E0000000ull, 0xBE80000040000000ull, 0xC039DBD6E0000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x3FEFFFC000000000ull, 0xBE49911120000000ull, 0x3CC0002000000000ull, 0x3E49911120000000ull},
    {kFpMAdd | kFpSingle, 0u, 0xC01FFFF000000000ull, 0x40FE829BE0000000ull, 0xBF40000800000000ull, 0x40FE829BE0000000ull},
    {kFpNMSub | kFpSingle, 1u, 0xB0F0000002200000ull, 0xC0542B356FFFFFFFull, 0x3940000000000000ull, 0xC0542B3560000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x3100000800000000ull, 0x407472B750000000ull, 0x3800408000000002ull, 0x407472B760000000ull},
    {kFpNMSub | kFpSingle, 1u, 0x3140000000000000ull, 0x3F83912230000000ull, 0xBAE0800002080000ull, 0x3F83912240000000ull},
    {kFpMSub | kFpSingle, 1u, 0xAEC0020000044000ull, 0xBF6EC60C6FFFFFFFull, 0xB374000801000000ull, 0x3F6EC60C60000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x3050000000000020ull, 0xBF7BBECF4FFFFFFFull, 0x41C0000000000000ull, 0x3F7BBECF40000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x2DA0000000000008ull, 0x424E38D050000000ull, 0xBAF0000040000000ull, 0xC24E38D040000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0xB2F0000000000000ull, 0xC1C3CD8BEFFFFFFFull, 0xB520000000000000ull, 0x41C3CD8BE0000000ull},
    {kFpMAdd | kFpSingle, 1u, 0xAEB0000000000000ull, 0x3FE02BCCEFFFFFFFull, 0xB581000000010000ull, 0x3FE02BCCE0000000ull},
    {kFpMAdd | kFpSingle, 1u, 0xAD40800000080000ull, 0x411A68EB50000000ull, 0x4390000000000000ull, 0x411A68EB40000000ull},
    {kFpMSub | kFpSingle, 1u, 0xAF50000000000000ull, 0xC0572BF830000001ull, 0x3700000000000002ull, 0x40572BF840000000ull},
    {kFpMAdd | kFpSingle, 0u, 0xAF20010200004000ull, 0xC232EC98AFFFFFFFull, 0x35F0000000000000ull, 0xC232EC98A0000000ull},
    {kFpMAdd | kFpSingle, 1u, 0x3110000800000000ull, 0x3DA3D86550000000ull, 0x3398000000000000ull, 0x3DA3D86540000000ull},
    {kFpMSub | kFpSingle, 1u, 0x30D0000000C00800ull, 0xC2404C8950000001ull, 0xBC50000240000000ull, 0x42404C8960000000ull},
    {kFpMAdd | kFpSingle, 1u, 0xB2D0000000000000ull, 0x41DC995E70000001ull, 0x4430000000000000ull, 0x41DC995E80000000ull},
    {kFpMSub | kFpSingle, 0u, 0xB130000004000000ull, 0x41CB5571AFFFFFFFull, 0xB0B0000000000400ull, 0xC1CB5571A0000000ull},
    {kFpMAdd | kFpSingle, 1u, 0x3198000100000020ull, 0xBFA64D2D50000000ull, 0xC350004000000000ull, 0xBFA64D2D40000000ull},
    {kFpMSub | kFpSingle, 0u, 0x3190000000000000ull, 0xBE00E7DB30000001ull, 0x3270000000080000ull, 0x3E00E7DB40000000ull},
    {kFpNMSub | kFpSingle, 1u, 0xB0B0080040000400ull, 0x40CB5F986FFFFFFFull, 0xC260000000000008ull, 0x40CB5F9860000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x2F7000000000000Cull, 0x41ED6BDCD0000001ull, 0xB4D0000000000000ull, 0xC1ED6BDCE0000000ull},
    {kFpMSub | kFpSingle, 1u, 0x2DD0000000000000ull, 0x40016DC810000001ull, 0x3A00000000000000ull, 0xC0016DC820000000ull},
    {kFpMAdd | kFpSingle, 0u, 0xB0F0000000000090ull, 0x4109B5D470000000ull, 0xC0F0020100000000ull, 0x4109B5D480000000ull},
    {kFpMSub | kFpSingle, 1u, 0x2F10000004800000ull, 0x402E1D67AFFFFFFFull, 0xB460000040002000ull, 0xC02E1D67A0000000ull},
    {kFpMSub | kFpSingle, 0u, 0xB240008880000000ull, 0xBE2EF493F0000001ull, 0xC130400010008000ull, 0x3E2EF49400000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x2FD0000000000000ull, 0xBE39452AB0000001ull, 0x4290100800000020ull, 0x3E39452AC0000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x31B0000000000000ull, 0x40AC87E1B0000000ull, 0x2FF0008000000000ull, 0x40AC87E1C0000000ull},
    {kFpMSub | kFpSingle, 0u, 0x31D0000400200010ull, 0xC26B81522FFFFFFFull, 0xC630000000400000ull, 0x426B815220000000ull},
    {kFpMSub | kFpSingle, 1u, 0x31F0000000000000ull, 0xBFF9FDFFD0000001ull, 0xC130200040000000ull, 0x3FF9FDFFE0000000ull},
    {kFpNMSub | kFpSingle, 0u, 0xB1B0000000040400ull, 0xC0307B000FFFFFFFull, 0xB840100000000003ull, 0xC0307B0000000000ull},
    {kFpMAdd | kFpSingle, 0u, 0xADE4020000000000ull, 0x41F4686D2FFFFFFFull, 0xBD10000002000000ull, 0x41F4686D20000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x2D60010001001000ull, 0x417C3D2B90000000ull, 0x3EA0000040000000ull, 0xC17C3D2BA0000000ull},
    {kFpMSub | kFpSingle, 1u, 0x3140000000000000ull, 0x3F494F982FFFFFFFull, 0x3D00100000000000ull, 0xBF494F9820000000ull},
    {kFpMAdd | kFpSingle, 0u, 0xAEB0000000000000ull, 0xBDC990AB2FFFFFFFull, 0xB538000008000000ull, 0xBDC990AB20000000ull},
    {kFpMSub | kFpSingle, 1u, 0x2E20000000000000ull, 0xBE04176010000001ull, 0x2EC0000000008004ull, 0x3E04176020000000ull},
    {kFpMAdd | kFpSingle, 1u, 0xAD72800004000000ull, 0xBEE5FBD9F0000000ull, 0x4050000000000000ull, 0xBEE5FBDA00000000ull},
    {kFpNMSub | kFpSingle, 0u, 0xAD90A00000000000ull, 0x41EB6300B0000000ull, 0x39E0000200000001ull, 0x41EB6300C0000000ull},
    {kFpMAdd | kFpSingle, 1u, 0x32E0000000200041ull, 0xC1D29938AFFFFFFFull, 0xB390000000000000ull, 0xC1D29938A0000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x2EA1000000100040ull, 0xC0CA962DB0000001ull, 0x42F0000000000000ull, 0xC0CA962DC0000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x2FE0000000000000ull, 0x41E1999E50000000ull, 0xB5C0000000000000ull, 0x41E1999E40000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x30A0000400018000ull, 0x3EA230D7D0000001ull, 0x4440000000002100ull, 0xBEA230D7E0000000ull},
    {kFpMSub | kFpSingle, 1u, 0xAE70402040000000ull, 0x420ABDBFAFFFFFFFull, 0x3940000000000000ull, 0xC20ABDBFA0000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0xAD40000000006000ull, 0xC0EA2645D0000001ull, 0xBAC0000000010000ull, 0x40EA2645E0000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0xADE0000000000000ull, 0xBFCAB10690000001ull, 0xB790000000000000ull, 0x3FCAB106A0000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x3282000002800000ull, 0x3E9DBE5FD0000000ull, 0xC630000000000000ull, 0xBE9DBE5FC0000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0xAF42800000100000ull, 0x3F5DD597AFFFFFFFull, 0xBA10000000802000ull, 0xBF5DD597A0000000ull},
    {kFpMAdd | kFpSingle, 1u, 0xB110000004004000ull, 0xC236C03DF0000000ull, 0x2D70001000000401ull, 0xC236C03E00000000ull},
    {kFpMAdd | kFpSingle, 1u, 0xAF10000000000000ull, 0x3F18DCAB70000000ull, 0xB390008000000400ull, 0x3F18DCAB80000000ull},
    {kFpMAdd | kFpSingle, 0u, 0xAEC0000010000200ull, 0xC1BCE61E4FFFFFFFull, 0xBE50000000012000ull, 0xC1BCE61E40000000ull},
    {kFpMAdd | kFpSingle, 0u, 0xAF90200000000220ull, 0x401EE14B8FFFFFFFull, 0xB2D0004300000000ull, 0x401EE14B80000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x3190000020040000ull, 0x3EADBC3A4FFFFFFFull, 0x3870008000000200ull, 0x3EADBC3A40000000ull},
    {kFpMSub | kFpSingle, 1u, 0x30B4000000000020ull, 0x3E673D4890000001ull, 0xB710008000000000ull, 0xBE673D48A0000000ull},
    {kFpMSub | kFpSingle, 1u, 0x2EC0000000200000ull, 0x4105B7EE30000001ull, 0x3580020000040001ull, 0xC105B7EE40000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0xB1E0000000000000ull, 0x3F71F4E410000000ull, 0x42A0400020000000ull, 0xBF71F4E400000000ull},
    {kFpNMSub | kFpSingle, 0u, 0xB060000040008004ull, 0xBE92E2B7EFFFFFFFull, 0x3EA0000000000000ull, 0xBE92E2B7E0000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x3320000000020000ull, 0x3F2155BC0FFFFFFFull, 0x32B0000011000080ull, 0x3F2155BC00000000ull},
    {kFpMAdd | kFpSingle, 1u, 0xAF80000010001000ull, 0xC04FE14D4FFFFFFFull, 0xBB20000000000040ull, 0xC04FE14D40000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x30C0200000000210ull, 0xBE6FAEAB90000000ull, 0x4550000100030000ull, 0xBE6FAEAB80000000ull},
    {kFpMSub | kFpSingle, 0u, 0x3120000000000000ull, 0x3E31BA90CFFFFFFFull, 0xC330008000100100ull, 0xBE31BA90C0000000ull},
    {kFpMAdd | kFpSingle, 1u, 0xAEC0004500000000ull, 0xC15CB36590000000ull, 0xB600000000000000ull, 0xC15CB36580000000ull},
    {kFpNMSub | kFpSingle, 1u, 0xB040000000008000ull, 0x3F33AB32F0000001ull, 0x4371000100000200ull, 0x3F33AB3300000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x2FA0000000010000ull, 0xBE7BDE4C70000000ull, 0xC4D0000000004000ull, 0xBE7BDE4C60000000ull},
    {kFpMSub | kFpSingle, 1u, 0xB090000000000000ull, 0xBEE55B9F10000000ull, 0x4090000000000000ull, 0x3EE55B9F00000000ull},
    {kFpMSub | kFpSingle, 0u, 0xB260000000C00000ull, 0x3F29D152B0000000ull, 0x4270000402000000ull, 0xBF29D152C0000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x2DA0080000000040ull, 0x3F7CAFE4D0000000ull, 0xC0E0000000000000ull, 0x3F7CAFE4C0000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x2D60000000000000ull, 0x3FF684A590000001ull, 0x4620000020000000ull, 0x3FF684A5A0000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x2EB0000000000000ull, 0xBEDD86476FFFFFFFull, 0xB620010000800040ull, 0x3EDD864760000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x2F70000000000000ull, 0x40322023D0000000ull, 0xB722000000800000ull, 0x40322023E0000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x2DA0001000000000ull, 0xC23139B830000000ull, 0xBE04000004200000ull, 0xC23139B840000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x2F30000000000000ull, 0xBDB88639F0000000ull, 0x4500000000000000ull, 0x3DB8863A00000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0x3361000000000000ull, 0x4109AD5ACFFFFFFFull, 0xB550000000000080ull, 0xC109AD5AC0000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x3064200000000000ull, 0xC2627F5F90000000ull, 0xBEA0400000000000ull, 0xC2627F5F80000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x2FE4000000000820ull, 0x40492D602FFFFFFFull, 0xC360000000000000ull, 0xC0492D6020000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x3210000000248000ull, 0xC176A31950000000ull, 0x33A0000000000000ull, 0xC176A31940000000ull},
    {kFpMAdd | kFpSingle, 0u, 0xB170009001000000ull, 0xC265AF9E90000001ull, 0x2E78000020000040ull, 0xC265AF9EA0000000ull},
    {kFpMAdd | kFpSingle, 1u, 0xAEC4000000000021ull, 0xC2509AE10FFFFFFFull, 0xBED0000001000001ull, 0xC2509AE100000000ull},
    {kFpNMAdd | kFpSingle, 0u, 0xAE60000000000000ull, 0x41ABEB9D10000001ull, 0x3758000000400800ull, 0xC1ABEB9D20000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x2E70020000000020ull, 0xBE080F258FFFFFFFull, 0x32A0000200800000ull, 0xBE080F2580000000ull},
    {kFpNMSub | kFpSingle, 0u, 0xADF0000000000000ull, 0x418DEDC950000001ull, 0x2E10000000000800ull, 0x418DEDC960000000ull},
    {kFpMSub | kFpSingle, 0u, 0x3190000000000000ull, 0xC1E5D009B0000001ull, 0xC000000200000000ull, 0x41E5D009C0000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x31D0000000000000ull, 0x415EF28BD0000000ull, 0x45B0000000000000ull, 0x415EF28BE0000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x3110000000000000ull, 0xBF6C819370000001ull, 0xB3B0000000000000ull, 0xBF6C819380000000ull},
    {kFpMSub, 0u, 0x3FF0000002000000ull, 0x3FF0000000000000ull, 0x3FF0000002000000ull, 0x3E50000001000000ull},
    {kFpMAdd | kFpSingle, 0u, 0x3F2FFFF800000000ull, 0x3FF0000020000000ull, 0x3F30000400000000ull, 0x3FF0000020000000ull},
    {kFpMAdd | kFpSingle, 1u, 0xB9B0000000000000ull, 0x3810000000000000ull, 0x39B0000000000000ull, 0x0000000000000000ull},
    {kFpMAdd | kFpSingle, 0u, 0xB9B0000000000000ull, 0x3810000000000000ull, 0x39B0000000000000ull, 0x3810000000000000ull},
    {kFpMAdd, 1u, 0x9E30000000000000ull, 0x0010000000000000ull, 0x1E30000000000000ull, 0x0000000000000000ull},
    {kFpMul, 1u, 0x3FEFFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0010000000000000ull, 0x0000000000000000ull},
    {kFpMul, 0u, 0x3FEFFFFFFFFFFFFFull, 0x0000000000000000ull, 0x0010000000000000ull, 0x0010000000000000ull},
    {kFpDiv, 1u, 0x3FEFFFFFFFFFFFFFull, 0x7FD0000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull},
    {kFpDiv, 0u, 0x3FEFFFFFFFFFFFFFull, 0x7FD0000000000000ull, 0x0000000000000000ull, 0x0010000000000000ull},
    {kFpNMSub, 0u, 0x7FF8000000000123ull, 0xFFF4000000000000ull, 0x7FF0000000000001ull, 0x7FF8000000000123ull},
    {kFpNMSub | kFpSingle, 0u, 0x7FF8000000000123ull, 0xFFF4000000000000ull, 0x7FF0000000000001ull, 0x7FF8000000000000ull},
    {kFpNMAdd, 0u, 0x3FF0000000000000ull, 0xFFF4000000000000ull, 0x7FF0000000000001ull, 0xFFFC000000000000ull},
    {kFpNMAdd | kFpSingle, 1u, 0x3FF0000000000000ull, 0x4000000000000000ull, 0xFFF0000000000001ull, 0xFFF8000000000000ull},
    {kFpMAdd, 0u, 0x7FF0000000000000ull, 0xFFF8000000000000ull, 0x0000000000000000ull, 0xFFF8000000000000ull},
    {kFpDiv | kFpSingle, 0u, 0x8000000000000000ull, 0x8000000000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpSqrt, 0u, 0x0000000000000000ull, 0xBFF0000000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpSub, 0u, 0x7FF0000000000000ull, 0x7FF0000000000000ull, 0x0000000000000000ull, 0x7FF8000000000000ull},
    {kFpNMSub | kFpSingle, 0u, 0x69B0008000040000ull, 0xBFE0010010000001ull, 0x8940000000000000ull, 0xBFE0010020000000ull},
};

void fp_vectors_against_oracle() {
    run_fp_vectors(kFpVectors, sizeof(kFpVectors) / sizeof(kFpVectors[0]), "FP oracle vectors");
}

// The fast paths (class keys, never-taken branches) against the reference
// interpreter over operand classes and sparse random doubles, both NI modes,
// every operation and encoding. No expected values: this is the differential
// half, the oracle vectors above the absolute half.
void fp_helpers_match_reference(u32 iterations = 4000u, u64 seed = 0xb7d1u) {
    const u64 classes[] = {
        0x0000000000000000ull, 0x8000000000000000ull, 0x0000000000000001ull, 0x800FFFFFFFFFFFFFull,
        0x0010000000000000ull, 0x0020000000000000ull, 0x36A0000000000000ull, 0x3810000000000000ull,
        0x3820000000000000ull, 0x3B30000000000000ull, 0x3B90000000000000ull, 0x3FF0000000000000ull,
        0xBFF8000000000000ull, 0x3FF0000002000000ull, 0x3FB999999999999Aull, 0x47EFFFFFE0000000ull,
        0x47EFFFFFF0000000ull, 0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
        0x7FF8000000000123ull, 0xFFF8000000000000ull, 0x7FF4000000000000ull, 0xFFF0000000000001ull,
        0x3CB0000000000000ull, 0x0350000000000000ull,
    };
    std::mt19937_64 random(seed);
    const auto draw = [&]() -> u64 {
        const u64 pick = random() % 4u;
        if (pick == 0u) return classes[random() % (sizeof(classes) / sizeof(classes[0]))];
        u64 mantissa = 0;
        for (u64 bit = random() % 4u; bit != 0u; --bit) mantissa |= 1ull << (random() % 52u);
        if (pick == 1u) mantissa |= random() & 0x000fffffe0000000ull;
        const u64 exponent = pick == 3u ? (random() % 2048u) : (1023u - 60u + random() % 120u);
        return ((random() & 1u) << 63) | (exponent << 52) | mantissa;
    };
    ++g_checks;
    interpreted_machine().cpu.reset();
    helper_machine().cpu.reset();
    unsigned failures = 0;
    u64 cases = 0;
    for (u32 kind = 0; kind < 12u; ++kind) {
        for (u32 single : {0u, 1u}) {
            if (kind == ppc::kFpRes && single == 0u) continue;
            if (kind == ppc::kFpRound && single == 0u) continue;
            for (u32 ni : {0u, 1u}) {
                for (u32 iteration = 0; iteration < iterations; ++iteration) {
                    FpVector v{kind | (single != 0u ? static_cast<u32>(ppc::kFpSingle) : 0u), ni, draw(), draw(), draw(), 0u};
                    // The reference result is the expectation here.
                    Machine& machine = interpreted_machine();
                    Cpu& c = machine.cpu;
                    c.clear_stop();
                    c.fpscr = ni != 0u ? kFpscrNI : 0u;
                    c.fpr[1].bits[0] = v.a;
                    c.fpr[2].bits[0] = v.b;
                    c.fpr[3].bits[0] = v.c;
                    machine.bus.write32(kCodeEffective, fp_encodings(v.op).front());
                    c.set_entry(kCodeEffective);
                    c.step();
                    v.result = c.fpr[4].bits[0];
                    std::string why;
                    ++cases;
                    if (!check_fp_vector(v, why) && failures++ < 12u) {
                        std::printf("FAIL helper/reference op 0x%02X NI %u A 0x%016llX B 0x%016llX C 0x%016llX: "
                                    "reference 0x%016llX, %s\n",
                                    v.op, ni, static_cast<unsigned long long>(v.a),
                                    static_cast<unsigned long long>(v.b), static_cast<unsigned long long>(v.c),
                                    static_cast<unsigned long long>(v.result), why.c_str());
                    }
                }
            }
        }
    }
    std::printf("%s FP helpers == reference over %llu class/sparse operand sets\n", failures == 0u ? "ok  " : "FAIL",
                static_cast<unsigned long long>(cases));
    if (failures != 0u) ++g_failures;
}

// fres / frsqrte / ps_res / ps_rsqrte against the hardware-verified definition
// in hwtests Common/FloatUtils.h (fres_expected, frsqrte_expected), transcribed
// here in its own form (test-only: none of it ships). hwtests compares those
// against the real instructions for every operand whose low 32 bits are zero;
// this sweep covers that operand set sparsely, every interpolation index at
// the exponents where a rule changes, and the interpreter on a subset.
namespace hwtests_estimate {
const int kRsqBase[] = {
    0x3ffa000, 0x3c29000, 0x38aa000, 0x3572000, 0x3279000, 0x2fb7000, 0x2d26000, 0x2ac0000,
    0x2881000, 0x2665000, 0x2468000, 0x2287000, 0x20c1000, 0x1f12000, 0x1d79000, 0x1bf4000,
    0x1a7e800, 0x17cb800, 0x1552800, 0x130c000, 0x10f2000, 0x0eff000, 0x0d2e000, 0x0b7c000,
    0x09e5000, 0x0867000, 0x06ff000, 0x05ab800, 0x046a000, 0x0339800, 0x0218800, 0x0105800,
};
const int kRsqDec[] = {
    0x7a4, 0x700, 0x670, 0x5f2, 0x584, 0x524, 0x4cc, 0x47e, 0x43a, 0x3fa, 0x3c2,
    0x38e, 0x35e, 0x332, 0x30a, 0x2e6, 0x568, 0x4f3, 0x48d, 0x435, 0x3e7, 0x3a2,
    0x365, 0x32e, 0x2fc, 0x2d0, 0x2a8, 0x283, 0x261, 0x243, 0x226, 0x20b,
};
const s32 kResBase[] = {
    0xfff000, 0xf07000, 0xe1d400, 0xd41000, 0xc71000, 0xbac400, 0xaf2000, 0xa41000,
    0x999000, 0x8f9400, 0x861000, 0x7d0000, 0x745800, 0x6c1000, 0x642800, 0x5c9400,
    0x555000, 0x4e5800, 0x47ac00, 0x413c00, 0x3b1000, 0x352000, 0x2f5c00, 0x29f000,
    0x248800, 0x1f7c00, 0x1a7000, 0x15bc00, 0x110800, 0x0ca000, 0x083800, 0x041800,
};
const s32 kResDec[] = {
    -0x3e1, -0x3a7, -0x371, -0x340, -0x313, -0x2ea, -0x2c4, -0x2a0, -0x27f, -0x261, -0x245,
    -0x22a, -0x212, -0x1fb, -0x1e5, -0x1d1, -0x1be, -0x1ac, -0x19b, -0x18b, -0x17c, -0x16e,
    -0x15b, -0x15b, -0x143, -0x143, -0x12d, -0x12d, -0x11a, -0x11a, -0x108, -0x106,
};

u64 frsqrte(u64 vali) {
    s64 mantissa = static_cast<s64>(vali & 0x000FFFFFFFFFFFFFull);
    const s64 sign = static_cast<s64>(vali & 0x8000000000000000ull);
    s64 exponent = static_cast<s64>(vali & 0x7FF0000000000000ull);
    if (mantissa == 0 && exponent == 0) return static_cast<u64>(sign) | 0x7FF0000000000000ull;
    if (exponent == static_cast<s64>(0x7FF0000000000000ull)) {
        if (mantissa == 0) return sign != 0 ? 0x7FF8000000000000ull : 0u;
        return vali | 0x0008000000000000ull;
    }
    if (sign != 0) return 0x7FF8000000000000ull;
    if (exponent == 0) {
        do {
            exponent -= 1LL << 52;
            mantissa <<= 1;
        } while (!(mantissa & (1LL << 52)));
        mantissa &= 0x000FFFFFFFFFFFFFll;
        exponent += 1LL << 52;
    }
    const bool odd_exponent = !(exponent & (1LL << 52));
    exponent = ((0x3FFLL << 52) - ((exponent - (0x3FELL << 52)) / 2)) & (0x7FFLL << 52);
    const int i = static_cast<int>(mantissa >> 37);
    const int index = i / 2048 + (odd_exponent ? 16 : 0);
    return static_cast<u64>(exponent) | (static_cast<u64>(kRsqBase[index] - kRsqDec[index] * (i % 2048)) << 26);
}

u64 fres(u64 full_bits, bool ni) {
    const u32 mantissa = static_cast<u32>((full_bits & 0x000FFFFFFFFFFFFFull) >> 29);
    const u32 sign = static_cast<u32>(full_bits >> 32) & 0x80000000u;
    s32 exponent = static_cast<s32>(((full_bits & 0x7FF0000000000000ull) >> 52) - 0x380);
    if ((full_bits & 0x7FF0000000000000ull) <= 0x37e0000000000000ull) {
        if ((full_bits & ~0x8000000000000000ull) == 0) return (full_bits & 0x8000000000000000ull) | 0x7FF0000000000000ull;
        return (full_bits & 0x8000000000000000ull) | 0x47EFFFFFE0000000ull;
    }
    const u64 max_float = ni ? 0x47d0000000000000ull : 0x4940000000000000ull;
    if ((full_bits & 0x7FF0000000000000ull) >= max_float) {
        if ((full_bits << 1) > 0xFFE0000000000000ull) return full_bits | 0x0008000000000000ull;
        return full_bits & 0x8000000000000000ull;
    }
    exponent = 253 - exponent;
    const u32 key = mantissa >> 18;
    const u32 new_mantissa =
        static_cast<u32>(kResBase[key] + kResDec[key] * static_cast<s32>((mantissa >> 8) & 0x3ff)) >> 1;
    u32 vali;
    if (exponent <= 0) {
        vali = ni ? sign : sign | (((1u << 23) | new_mantissa) >> (1u + static_cast<u32>(-exponent)));
    } else {
        vali = sign | static_cast<u32>(exponent << 23) | new_mantissa;
    }
    float valf;
    std::memcpy(&valf, &vali, sizeof(valf));
    return double_to_bits(static_cast<double>(valf));
}

// ps_rsqrte: the double estimate rounded to single, a denormal flushed under NI.
u64 ps_rsqrte(u64 bits, bool ni) {
    const u64 estimate = frsqrte(bits);
    const double rounded = static_cast<double>(static_cast<float>(bits_to_double(estimate)));
    if (ni && rounded != 0.0 && std::fabs(rounded) < 0x1p-126) return double_to_bits(rounded) & 0x8000000000000000ull;
    return double_to_bits(rounded);
}
}  // namespace hwtests_estimate

void estimates_against_hwtests() {
    ++g_checks;
    Cpu& helper = helper_machine().cpu;
    helper.reset();
    interpreted_machine().cpu.reset();
    unsigned failures = 0;
    u64 helper_cases = 0, interpreter_cases = 0;
    const auto check = [&](u64 operand, bool with_interpreter) {
        if ((operand << 1) > 0xFFE0000000000000ull) return;  // NaNs: rule 1, covered by kFpVectors
        for (u32 ni : {0u, 1u}) {
            helper.fpscr = ni != 0u ? kFpscrNI : 0u;
            const struct {
                u32 op;
                u64 expected;
            } cases[] = {
                {ppc::kFpRes | ppc::kFpSingle, hwtests_estimate::fres(operand, ni != 0u)},
                {ppc::kFpRsqrte, hwtests_estimate::frsqrte(operand)},
                {ppc::kFpRsqrte | ppc::kFpSingle, hwtests_estimate::ps_rsqrte(operand, ni != 0u)},
            };
            for (const auto& item : cases) {
                ++helper_cases;
                const u64 got = fp_helper(helper, item.op, 0u, operand, 0u);
                std::string why;
                bool ok = got == item.expected;
                if (!ok) {
                    char row[64];
                    std::snprintf(row, sizeof(row), "helper 0x%016llX", static_cast<unsigned long long>(got));
                    why = row;
                } else if (with_interpreter) {
                    ++interpreter_cases;
                    ok = check_fp_vector(FpVector{item.op, ni, 0u, operand, 0u, item.expected}, why);
                    helper.fpscr = ni != 0u ? kFpscrNI : 0u;
                }
                if (!ok && failures++ < 12u) {
                    std::printf("FAIL estimate op 0x%02X NI %u B 0x%016llX: hwtests 0x%016llX, %s\n", item.op, ni,
                                static_cast<unsigned long long>(operand),
                                static_cast<unsigned long long>(item.expected), why.c_str());
                }
            }
        }
    };
    // Every interpolation index (top 16 fraction bits) at the exponents where
    // a rule or a table half changes, both signs, plus one low-bit pattern.
    const u32 exponents[] = {0x000u, 0x001u, 0x002u, 0x37Eu, 0x37Fu, 0x380u, 0x3FEu, 0x3FFu, 0x400u,
                             0x47Cu, 0x47Du, 0x47Eu, 0x493u, 0x494u, 0x495u, 0x7FEu, 0x7FFu};
    for (const u32 exponent : exponents) {
        for (u64 index = 0; index < 0x10000u; ++index) {
            for (const u64 sign : {0ull, 0x8000000000000000ull}) {
                const u64 operand = sign | (static_cast<u64>(exponent) << 52) | (index << 36);
                check(operand, (index & 0x3ffu) == 0u);
                check(operand | 0x0000000FFFFFFFFFull, false);
            }
        }
    }
    // The hwtests operand set (low 32 bits zero), every 997th top word.
    for (u64 word = 0; word < 0x100000000ull; word += 997u) check(word << 32, (word % (997u * 4096u)) == 0u);
    std::printf("%s estimates == hwtests definition: %llu helper cases, %llu interpreter cases, %u failures\n",
                failures == 0u ? "ok  " : "FAIL", static_cast<unsigned long long>(helper_cases),
                static_cast<unsigned long long>(interpreter_cases), failures);
    if (failures != 0u) ++g_failures;
}

// fpu-1: fctiw/fctiwz in non-IEEE mode place 0x80000000 for every invalid
// conversion (PAGE 85 Table 2-22); IEEE mode saturates positive overflow.
void fctiw_against_manual() {
    struct Case {
        double value;
        bool truncate;
        u32 ieee, non_ieee;
    };
    const Case cases[] = {
        {2147483648.0, false, 0x7FFFFFFFu, 0x80000000u},  {2147483648.0, true, 0x7FFFFFFFu, 0x80000000u},
        {2147483647.4, false, 0x7FFFFFFFu, 0x7FFFFFFFu},  {2147483647.5, false, 0x7FFFFFFFu, 0x80000000u},
        {2147483647.9, true, 0x7FFFFFFFu, 0x7FFFFFFFu},   {2147483647.9, false, 0x7FFFFFFFu, 0x80000000u},
        {HUGE_VAL, false, 0x7FFFFFFFu, 0x80000000u},      {-HUGE_VAL, true, 0x80000000u, 0x80000000u},
        {-2147483648.4, false, 0x80000000u, 0x80000000u}, {-2147483649.0, true, 0x80000000u, 0x80000000u},
        {std::nan(""), false, 0x80000000u, 0x80000000u},   {-1.5, false, 0xFFFFFFFEu, 0xFFFFFFFEu},
        {-1.5, true, 0xFFFFFFFFu, 0xFFFFFFFFu},           {2.5, false, 0x00000002u, 0x00000002u},
        {1e-310, false, 0x00000000u, 0x00000000u},
    };
    for (const Case& test : cases) {
        for (bool ni : {false, true}) {
            ++g_checks;
            const u64 expect = 0xFFF8000000000000ull | (ni ? test.non_ieee : test.ieee);
            Machine& machine = interpreted_machine();
            Cpu& c = machine.cpu;
            c.reset();
            c.clear_stop();
            c.fpscr = ni ? kFpscrNI : 0u;
            c.fpr[2].bits[0] = double_to_bits(test.value);
            machine.bus.write32(kCodeEffective, x_form(63, 4, 0, 2, test.truncate ? 15u : 14u, false));
            c.set_entry(kCodeEffective);
            c.step();
            Cpu& h = helper_machine().cpu;
            h.fpscr = c.fpscr;
            const u64 helped = ppc::fctiw(h, double_to_bits(test.value), test.truncate);
            const bool ok = !c.stopped() && c.fpr[4].bits[0] == expect && helped == expect;
            if (!ok) {
                std::printf("FAIL fctiw%s %.17g NI %d: interpreter 0x%016llX helper 0x%016llX, expected 0x%016llX\n",
                            test.truncate ? "z" : "", test.value, ni ? 1 : 0,
                            static_cast<unsigned long long>(c.fpr[4].bits[0]),
                            static_cast<unsigned long long>(helped), static_cast<unsigned long long>(expect));
                ++g_failures;
            }
        }
    }
    std::printf("ok   fctiw/fctiwz IEEE and non-IEEE invalid conversions (unless FAIL above)\n");
}

// paired-single-10: HID2 PSE/LSQE/LCE gating in the reference interpreter
// (the translated guard is exercised by native_fallback_tests). PAGE 90: dcbz_l
// needs LCE, psq_l/psq_lu/psq_st/psq_stu need PSE and LSQE, every other paired
// single instruction PSE. The illegal-instruction program exception outranks
// floating-point unavailable (PAGE 162).
void hid2_gating_against_manual() {
    const u32 dcbz_l = (4u << 26) | (0u << 16) | (5u << 11) | (1014u << 1);
    const u32 ps_add = a_form(4, 1, 2, 3, 0, 21, false);
    const u32 psq_l = (56u << 26) | (1u << 21) | (5u << 16) | 0x8u;
    const u32 psq_st = (60u << 26) | (1u << 21) | (5u << 16) | 0x8u;
    const u32 psq_lx = (4u << 26) | (1u << 21) | (5u << 16) | (6u << 11) | (6u << 1);
    const u32 fadds = a_form(59, 1, 2, 3, 0, 21, false);
    struct Case {
        const char* name;
        u32 instruction;
        u32 hid2;
        bool fp;
        u32 vector;  // 0: executes
    };
    // HID2 LSQE|PSE are machine invariants (change C): every reachable HID2
    // has both, so only LCE (dcbz_l) and MSR[FP] decide.
    const Case cases[] = {
        {"dcbz_l with reset HID2 (LCE clear)", dcbz_l, 0xE0000000u, true, 0x700u},
        {"dcbz_l with LCE", dcbz_l, 0xF0000000u, true, 0u},
        {"dcbz_l LCE clear, MSR[FP] clear", dcbz_l, 0xE0000000u, false, 0x700u},
        {"dcbz_l LCE set, MSR[FP] clear (not an FP instruction)", dcbz_l, 0xF0000000u, false, 0u},
        {"ps_add with PSE", ps_add, 0xA0000000u, true, 0u},
        {"ps_add MSR[FP] clear: FP unavailable", ps_add, 0xA0000000u, false, 0x800u},
        {"psq_l with PSE|LSQE", psq_l, 0xA0000000u, true, 0u},
        {"psq_st with PSE|LSQE", psq_st, 0xE0000000u, true, 0u},
        {"psq_lx with PSE|LSQE", psq_lx, 0xA0000000u, true, 0u},
        {"fadds is not a paired-single instruction", fadds, 0xA0000000u, true, 0u},
    };
    const u32 data_effective = 0x80002000u;
    // The OS's exception entry as __OSExceptionInit / OSSetCurrentContext
    // leave it: a taken exception continues at its handler (no code there),
    // the interrupted SRR0 / SRR1 saved in the current OSContext.
    constexpr u32 kProgramHandler = 0x80007000u, kFpHandler = 0x80007100u, kContext = 0x80008000u;
    const auto handler_of = [](u32 vector) { return vector == 0x700u ? kProgramHandler : kFpHandler; };
    for (const Case& test : cases) {
        ++g_checks;
        Machine& machine = interpreted_machine();
        Cpu& c = machine.cpu;
        c.reset();
        c.clear_stop();
        test_support::install_os_exception(machine.bus, Exception::Program, 6u, kProgramHandler);
        test_support::install_os_exception(machine.bus, Exception::FpUnavailable, 7u, kFpHandler);
        machine.bus.write32(test_support::kOsCurrentContext, kContext);
        c.spr[SPR_HID2] = test.hid2;
        if (!test.fp) c.set_msr(c.msr() & (~kMsrFP));
        c.gpr[5] = data_effective;
        c.gpr[6] = 0u;
        for (u32 offset = 0; offset < 0x40u; offset += 4u) machine.bus.write32(data_effective + offset, 0xAAAAAAAAu);
        machine.bus.write32(kCodeEffective, test.instruction);
        c.set_entry(kCodeEffective);
        // The interpreter's completed-instruction count (nothing else counts
        // instructions): an instruction that executes completes, one whose
        // exception is taken instead does not.
        const u64 completed = c.interpreted_instructions();
        const u32 srr1_before = c.spr[SPR_SRR1];
        c.step();
        bool ok = !c.stopped();
        if (test.vector == 0u) {
            ok = ok && c.pc == kCodeEffective + 4u && c.interpreted_instructions() == completed + 1u;
            if (test.instruction == dcbz_l) ok = ok && machine.bus.read32(data_effective) == 0u;
        } else {
            ok = ok && c.pc == handler_of(test.vector) && machine.bus.read32(kContext + 0x198u) == kCodeEffective &&
                 c.interpreted_instructions() == completed &&
                 ((machine.bus.read32(kContext + 0x19Cu) & kSrr1IllegalInstruction) != 0u) ==
                     (test.vector == 0x700u) &&
                 machine.bus.read32(data_effective) == 0xAAAAAAAAu;
        }
        static_cast<void>(srr1_before);
        std::printf("%s HID2 gating: %s\n", ok ? "ok  " : "FAIL", test.name);
        if (!ok) {
            std::printf("     pc 0x%08X SRR0 0x%08X SRR1 0x%08X completed %llu\n", c.pc, c.spr[SPR_SRR0],
                        c.spr[SPR_SRR1],
                        static_cast<unsigned long long>(c.interpreted_instructions() - completed));
            ++g_failures;
        }
    }
    // An HID2 write that would clear LSQE or PSE is refused (the machine
    // stops, HID2 keeps its value); one keeping both is accepted.
    const struct {
        const char* name;
        u32 value;
        bool accepted;
    } writes[] = {
        {"HID2 write clearing PSE refused", 0xD0000000u, false},
        {"HID2 write clearing LSQE refused", 0x60000000u, false},
        {"HID2 write of LCE alone refused", 0x10000000u, false},
        {"HID2 write setting LCE accepted", 0xF0000000u, true},
        {"HID2 write clearing WPE accepted", 0xA0000000u, true},
    };
    for (const auto& write : writes) {
        ++g_checks;
        Machine& machine = interpreted_machine();
        Cpu& c = machine.cpu;
        c.reset();
        c.clear_stop();
        c.write_hid2(write.value);
        const bool ok = write.accepted ? (!c.stopped() && c.spr[SPR_HID2] == write.value)
                                       : (c.stopped() && c.spr[SPR_HID2] == 0xE0000000u);
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", write.name);
        if (!ok) ++g_failures;
        c.clear_stop();
    }
    // The predicate the translator and the interpreter share.
    ++g_checks;
    const bool traits_ok = instruction_checked_hid2(dcbz_l) == kHid2LCE &&
                           instruction_checked_hid2(ps_add) == 0u &&
                           instruction_checked_hid2(psq_l) == 0u &&
                           instruction_checked_hid2(fadds) == 0u &&
                           instruction_required_hid2(dcbz_l) == kHid2LCE &&
                           instruction_required_hid2(ps_add) == kHid2PSE &&
                           instruction_required_hid2(psq_l) == (kHid2PSE | kHid2LSQE) &&
                           instruction_required_hid2((61u << 26)) == (kHid2PSE | kHid2LSQE) &&
                           instruction_required_hid2(psq_lx) == kHid2PSE &&
                           instruction_required_hid2(fadds) == 0u &&
                           instruction_required_hid2(d_form(48, 1, 2, 0)) == 0u;
    std::printf("%s instruction_required_hid2 masks\n", traits_ok ? "ok  " : "FAIL");
    if (!traits_ok) ++g_failures;
}

// `ppc_recomp_tests --fp-vectors FILE` checks every line of
// `fp_semantics_oracle.py --plain` (op ni A B C R, hex operands).
int run_fp_vector_file(const char* path) {
#if defined(_MSC_VER)
    std::FILE* file = nullptr;
    if (fopen_s(&file, path, "r") != 0) file = nullptr;
#else
    std::FILE* file = std::fopen(path, "r");
#endif
    if (file == nullptr) {
        std::printf("cannot open %s\n", path);
        return 2;
    }
    std::vector<FpVector> vectors;
    unsigned op = 0, ni = 0;
    unsigned long long a = 0, b = 0, c = 0, r = 0;
#if defined(_MSC_VER)
    while (fscanf_s(file, "%u %u %llx %llx %llx %llx", &op, &ni, &a, &b, &c, &r) == 6) {
#else
    while (std::fscanf(file, "%u %u %llx %llx %llx %llx", &op, &ni, &a, &b, &c, &r) == 6) {
#endif
        vectors.push_back(FpVector{op, ni, a, b, c, r});
    }
    std::fclose(file);
    run_fp_vectors(vectors.data(), vectors.size(), path);
    return g_failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--fp-vectors") == 0) {
        return run_fp_vector_file(argv[2]);
    }
    if (argc == 3 && std::strcmp(argv[1], "--fp-stress") == 0) {  // iterations per operation
        fp_helpers_match_reference(static_cast<u32>(std::strtoul(argv[2], nullptr, 10)), 0x5eedb7ull);
        return g_failures == 0 ? 0 : 1;
    }
    integer_arithmetic();
    logical_and_shift();
    condition_register();
    floating_point();
    paired_singles();
    fp_compare_against_manual();
    paired_moves_against_manual();
    aliased_shift_carry_programs();
    overflow_against_reference();
    count_leading_zeros_patterns();
    update_and_reservation_programs();
    quantised_memory();
    quantised_against_manual();
    fp_conversions_against_manual();
    fp_vectors_against_oracle();
    fp_helpers_match_reference();
    estimates_against_hwtests();
    fctiw_against_manual();
    hid2_gating_against_manual();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
