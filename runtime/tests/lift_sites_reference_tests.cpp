// The generated lowered call sites (extract/lift-sites.txt, docs/lift-20261006/LIFT_PROGRAMME.md 2.1)
// against the lifted subsystems: what each site reads from its translated caller, what it asks the native
// subsystem, and what it writes back - through the same generated function the translated code, the
// dispatcher and the interpreter run. L1: controller input (docs/lift-20261006/L1_INPUT.md).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "gekko/lifted.h"
#include "gekko/recomp.h"
#include "gekko/system.h"

namespace {

using namespace gekko;
using shadow::peripheral::ControllerSample;
using shadow::peripheral::ManagerImpPc;

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

// The engine's peripheral manager and the other callers (extract/lift-sites.txt).
constexpr u32 kSiteGetPortCount = 0x8040E960u;
constexpr u32 kSiteRead = 0x8040EA68u;
constexpr u32 kSiteGetStickRange = 0x8040EB38u;
constexpr u32 kSitesRumble[] = {0x8034FCE8u, 0x8034FD80u, 0x8034FD90u};
constexpr u32 kSitesRumbleAll[] = {0x8032D2C4u};
constexpr u32 kSiteInit = 0x800517C4u;
constexpr u32 kSitesNothing[] = {0x80306A84u, 0x800511F0u, 0x8005126Cu};  // (OSInit's SIInit call 0x8037101C left with OSInit)
constexpr u32 kBuffer = 0x80100000u;  // a caller's buffer the tests own

void run_site(Cpu& cpu, u32 site) {
    const LiftSiteRecord* record = cpu.find_lift_site(site);
    if (record == nullptr) {
        check(false, "site " + std::to_string(site) + " is in the generated table");
        return;
    }
    record->lower(cpu);
}

void table_is_sorted_and_complete(Cpu& cpu) {
    constexpr u32 input_sites[] = {kSiteGetPortCount, kSiteRead, kSiteGetStickRange,
        kSitesRumble[0], kSitesRumble[1], kSitesRumble[2], kSitesRumbleAll[0], kSiteInit,
        kSitesNothing[0], kSitesNothing[1], kSitesNothing[2]};
    for (u32 address : input_sites)
        check(cpu.find_lift_site(address) != nullptr, "every one of the eleven L1 sites is generated");
    check(cpu.find_lift_site(0x8043F7A4u) != nullptr && cpu.find_lift_site(0x8043F818u) != nullptr,
          "both sound-bank sites of the GCAX init are generated");
    // The two ring sites inside the ADX output's owner (0x803CEF10..0x803CF260) are not generated: the native owner replaces
    // the whole body (extract/lift-sites-native.txt, docs/CRI_SERVERS.md 4c), so no lowered site exists there. The literals are
    // looked up only to pin that absence.
    check(cpu.find_lift_site(0x803CF0D0u) == nullptr,  // liveness-roots: value-only
          "the ring-feed site inside the native ADX output owner is not generated");
    check(cpu.find_lift_site(0x803CF21Cu) == nullptr,  // liveness-roots: value-only
          "the ring-fill site inside the native ADX output owner is not generated");
    check(cpu.find_lift_site(0x803D06E0u) != nullptr && cpu.find_lift_site(0x803D0658u) != nullptr,
          "the two stream-ring setup and finalize sites are generated");
    check(cpu.find_lift_site(0x8043F730u) != nullptr, "the sound-bank release site is generated");
    check(cpu.find_lift_site(0x801F2EA0u) != nullptr && cpu.find_lift_site(0x801F2EA4u) != nullptr,
          "the game constructor's ARInit and ARQInit sites are generated");
    check(recomp::kLiftSiteCount == 1046u,
          "eleven lift-sites.txt sites, 1018 lift-sites-math.txt and (this unlifted tree) 4 lift-sites-math-unlifted.txt "
          "sites, 10 lift-sites-native.txt sites and the three empty variadic leaves: 1046");
    unsigned math_sites = 0, copy_sites = 0, retired_copy_sites = 0;
    for (std::size_t i = 0; i < recomp::kLiftSiteCount; ++i) {
        math_sites += recomp::kLiftSiteWords[i] == 0x4BFFFD89u;
        copy_sites += recomp::kLiftSiteWords[i] == 0x4BFFFD01u;
        retired_copy_sites += recomp::kLiftSiteWords[i] == 0x480001C1u;
    }
    check(math_sites == 1u, "the additional site has the pinned scalar square-root call word");
    check(copy_sites == 1u, "the retained inverse copy site has its pinned call word");
    check(retired_copy_sites == 0u, "the retired composition's internal copy site is absent");
    // native_matrix_call_tests executes all 68 composition boundaries. Their
    // complete direct-edge list is pinned independently by verify_native_math.py.
    for (std::size_t i = 1; i < recomp::kLiftSiteCount; ++i) {
        check(recomp::kLiftSites[i - 1].address < recomp::kLiftSites[i].address, "the site table is sorted");
    }
    // The words themselves are pinned against the DOL by the generator and by
    // tools/lift/verify_l1_input.py; here: each is a bl or an unconditional bctrl.
    for (std::size_t i = 0; i < recomp::kLiftSiteCount; ++i) {
        const LiftSiteRecord& site = recomp::kLiftSites[i];
        const u32 word = recomp::kLiftSiteWords[i];
        const bool leaf = site.address == 0x8000D374u || site.address == 0x8001C574u || site.address == 0x8047EC78u;
        if (leaf) {
            check(word == (site.address == 0x8047EC78u ? 0x9421FF80u : 0x9421FF90u), "empty leaf has its pinned prologue");
            check(site.continuation == LiftSiteContinuation::Prepared, "empty leaf owns its return");
        } else {
            const bool bl = (word >> 26) == 18u && (word & 3u) == 1u;
            check(bl || word == 0x4E800421u, "each other site lowers a bl or a bctrl");
            check(site.continuation == LiftSiteContinuation::Sequential, "ordinary site continues after its call");
        }
        check(cpu.find_lift_site(site.address) == &site, "each site is found at its address");
    }
    check(cpu.find_lift_site(kSiteRead + 4u) == nullptr, "an address that is not a site is not found");
}

void read_writes_the_engine_records(System& system) {
    Cpu& cpu = system.cpu();
    ManagerImpPc& peripheral = system.peripheral();
    ControllerSample sample;
    sample.connected = true;
    sample.controls = shadow::peripheral::kControlA | shadow::peripheral::kControlR;
    sample.stick_x = -100;
    sample.stick_y = 64;
    sample.substick_x = 5;
    sample.substick_y = -7;
    peripheral.latch(0, sample);
    peripheral.latch(1, ControllerSample{});
    for (u32 i = 0; i < 64u; ++i) cpu.mem_write8(kBuffer + i, 0xEEu);
    cpu.gpr[3] = 0x12345678u;  // the implementation object: not used
    cpu.gpr[4] = kBuffer;
    cpu.gpr[5] = 4u;
    cpu.lr = 0u;
    run_site(cpu, kSiteRead);
    check(cpu.gpr[3] == 1u && cpu.lr == kSiteRead + 4u, "Read returns true and links after the site");
    check(cpu.mem_read32(kBuffer) == 0u && cpu.mem_read32(kBuffer + 4u) == (0x0001u | 0x2000u),
          "port 0: ready, the game's controls");
    check(static_cast<s16>(cpu.mem_read16(kBuffer + 8u)) == -99 && static_cast<s16>(cpu.mem_read16(kBuffer + 10u)) == -64 &&
              static_cast<s16>(cpu.mem_read16(kBuffer + 12u)) == 5 && static_cast<s16>(cpu.mem_read16(kBuffer + 14u)) == 6,
          "port 0: sticks in the engine's convention");
    for (u32 port = 1; port < 4u; ++port) {
        const u32 record = kBuffer + 16u * port;
        check(cpu.mem_read32(record) == 1u && cpu.mem_read32(record + 4u) == 0xEEEEEEEEu &&
                  cpu.mem_read32(record + 12u) == 0xEEEEEEEEu,
              "an absent port gets its state only, as the shipped implementation wrote it");
    }
    check(peripheral.stats().frames_read >= 1u, "the read reached the peripheral layer");
}

void stick_range_and_port_count(System& system) {
    Cpu& cpu = system.cpu();
    cpu.gpr[4] = 2u;
    cpu.gpr[5] = kBuffer;
    run_site(cpu, kSiteGetStickRange);
    check(cpu.gpr[3] == 1u && cpu.lr == kSiteGetStickRange + 4u, "GetStickRange returns true");
    check(cpu.mem_read16(kBuffer) == 15u && cpu.mem_read16(kBuffer + 2u) == 25u && cpu.mem_read16(kBuffer + 4u) == 42u &&
              cpu.mem_read16(kBuffer + 6u) == 39u,
          "dead zones 15 / 25, live ranges 42 / 39");
    cpu.gpr[3] = 0u;
    run_site(cpu, kSiteGetPortCount);
    check(cpu.gpr[3] == 4u && cpu.lr == kSiteGetPortCount + 4u, "four ports");
}

void rumble_sites(System& system) {
    Cpu& cpu = system.cpu();
    ManagerImpPc& peripheral = system.peripheral();
    std::vector<std::pair<std::size_t, bool>> events;
    peripheral.set_rumble_listener([&](std::size_t port, bool on) { events.emplace_back(port, on); });
    for (const u32 site : kSitesRumble) {
        cpu.gpr[3] = 2u;
        cpu.gpr[4] = 1u;  // rumble
        run_site(cpu, site);
        check(peripheral.rumble(2) && cpu.lr == site + 4u, "command 1 rumbles the port");
        cpu.gpr[3] = 2u;
        cpu.gpr[4] = 2u;  // hard stop
        run_site(cpu, site);
        check(!peripheral.rumble(2), "command 2 stops it");
        cpu.gpr[3] = 2u;
        cpu.gpr[4] = 1u;
        run_site(cpu, site);
        cpu.gpr[3] = 2u;
        cpu.gpr[4] = 0u;  // stop
        run_site(cpu, site);
        check(!peripheral.rumble(2), "command 0 stops it");
    }
    const u32 commands[4] = {1u, 0u, 2u, 1u};
    for (const u32 site : kSitesRumbleAll) {
        for (u32 port = 0; port < 4u; ++port) cpu.mem_write32(kBuffer + 4u * port, commands[port]);
        cpu.gpr[3] = kBuffer;
        run_site(cpu, site);
        check(peripheral.rumble(0) && !peripheral.rumble(1) && !peripheral.rumble(2) && peripheral.rumble(3) &&
                  cpu.lr == site + 4u,
              "the four commands reach the four ports");
        for (u32 port = 0; port < 4u; ++port) cpu.mem_write32(kBuffer + 4u * port, 0u);
        run_site(cpu, site);
        check(!peripheral.rumble(0) && !peripheral.rumble(3), "all stop");
    }
    check(!events.empty(), "the device layer heard the changes");
    peripheral.set_rumble_listener({});
}

void init_and_nothing_sites(System& system) {
    Cpu& cpu = system.cpu();
    cpu.gpr[3] = 0u;
    run_site(cpu, kSiteInit);
    check(cpu.gpr[3] == 1u && cpu.lr == kSiteInit + 4u, "PADInit's call reads TRUE: nothing to initialise");
    for (const u32 site : kSitesNothing) {
        cpu.gpr[3] = 0x11111111u;
        cpu.gpr[4] = 0x22222222u;
        run_site(cpu, site);
        check(cpu.gpr[3] == 0x11111111u && cpu.gpr[4] == 0x22222222u && cpu.lr == site + 4u,
              "a site with nothing to do only links past the call");
    }
}

// The reference interpreter runs the same generated function when it reaches a site, and continues after
// it - never the original call.
void interpreter_runs_the_site(System& system) {
    Cpu& cpu = system.cpu();
    cpu.set_msr(cpu.msr() | kMsrIR | kMsrDR);  // the game's code runs translated (no real mode)
    cpu.gpr[3] = 0u;
    cpu.pc = kSiteGetPortCount;
    const u64 before = cpu.lift_sites_dispatched();
    cpu.step();
    check(cpu.pc == kSiteGetPortCount + 4u && cpu.gpr[3] == 4u && cpu.lr == kSiteGetPortCount + 4u,
          "step() at a site runs its lowering and continues after it");
    check(cpu.lift_sites_dispatched() == before + 1u, "counted as a site the interpreter ran");
}

}  // namespace

int main() {
    System system;
    system.install_lift_sites(recomp::kLiftSites, recomp::kLiftSiteCount);
    system.start();
    table_is_sorted_and_complete(system.cpu());
    read_writes_the_engine_records(system);
    stick_range_and_port_count(system);
    rumble_sites(system);
    init_and_nothing_sites(system);
    interpreter_runs_the_site(system);
    if (g_failures == 0) {
        std::printf("lift_sites_reference_tests: all passed\n");
        return 0;
    }
    std::printf("lift_sites_reference_tests: %d failure(s)\n", g_failures);
    return 1;
}
