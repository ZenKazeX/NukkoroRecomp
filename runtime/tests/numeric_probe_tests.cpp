#include "gekko/numeric_probe.h"
#include "gekko/measure_probes.h"
#include "gekko/system.h"
#include <iostream>
#include <stdexcept>
#include <memory>

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    using namespace gekko;
    using namespace gekko::measurement;
    try {
        NumericEvidence<2> evidence;
        NumericKey a, b, c;
        b.formats[6] = 0x060E060Eu;
        c.status = 4;
        check(!evidence.complete(), "unexecuted is not complete");
        evidence.observe(a); evidence.observe(a); evidence.observe(b);
        check(evidence.complete() && evidence.used == 2 && evidence.hits == 3 &&
              evidence.slots[0].hits == 2 && evidence.slots[1].hits == 1, "exact grouping");
        evidence.observe(c); evidence.observe(c); evidence.observe(a);
        check(!evidence.complete() && evidence.hits == 6 && evidence.unclassified == 2 &&
              evidence.slots[0].hits == 3, "overflow retained; known counts continue");
        NumericEvidence<8> lanes;
        for (std::size_t i = 0; i < 8; ++i) {
            auto key = a; key.formats[i] = 1; lanes.observe(key);
        }
        check(lanes.used == 8 && lanes.complete(), "each format participates in identity");
        NumericEvidence<2, EntryArgumentKey> arguments;
        arguments.observe({0x01100000u}); arguments.observe({0x01000000u});
        arguments.observe({0x01100000u}); arguments.observe({0xffffffffu});
        check(arguments.hits == 4 && arguments.used == 2 && arguments.unclassified == 1 &&
              arguments.slots[0].hits == 2 && !arguments.complete(), "argument overflow remains explicit");
        unsigned snapshots = 0;
        auto snapshot = [&]() noexcept {
            ++snapshots;
            OrderedNumericEvent event;
            event.site = 0x8044C260u;
            event.instructions = 10; // identical coordinates still have strict sequence
            event.f18_f19[3] = 0xfff8000000000001ull;
            return event;
        };
        OrderedNumericEvidence disabled;
        disabled.observe(snapshot);
        check(snapshots == 0 && disabled.retained_bytes() == 0 && disabled.hits() == 0,
              "disabled log allocates and observes nothing");
        OrderedNumericEvidence ordered(2);
        ordered.observe(snapshot); ordered.observe(snapshot); ordered.observe(snapshot);
        check(snapshots == 2 && ordered.hits() == 3 && ordered.used() == 2 && ordered.dropped() == 1 &&
              ordered[0].sequence == 1 && ordered[1].sequence == 2 && !ordered.complete() &&
              ordered[0].f18_f19[3] == 0xfff8000000000001ull && ordered[1].context_fields_observed == 0,
              "ordered log retains exact bits, sequence, unobserved fields and lazy overflow");
        check(ordered.retained_bytes() == 2 * sizeof(OrderedNumericEvent), "fixed exact byte bound");
        bool rejected = false;
        try { OrderedNumericEvidence excessive(OrderedNumericEvidence::max_events + 1); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "programmatic oversized log rejected");
        MeasureProbes::Config config;
        std::string error;
        check(MeasureProbes::parse_spec("sets=none;numeric=0x8044C260,0x8044E738;first=0", config, error) &&
              config.numeric_entries.size() == 2 && config.extra_entries.empty(), "numeric selection");
        check(!MeasureProbes::parse_spec("numeric=0x8044C261", config, error), "alignment checked");
        check(!MeasureProbes::parse_spec("numeric=0", config, error), "zero rejected");
        std::string many = "numeric=";
        for (unsigned i = 0; i < 33; ++i) many += (i ? "," : "") + std::to_string(0x80000000u + i * 4u);
        check(!MeasureProbes::parse_spec(many, config, error), "capacity rejected, never truncated by parser");
        check(MeasureProbes::parse_spec("sets=none;first=0", config, error) && config.numeric_entries.empty(),
              "numeric evidence opt-in only");
        check(config.ordered_events == 0, "ordered evidence off by default");
        check(MeasureProbes::parse_spec("sets=none;numeric=0x80005000;ordered=65536", config, error) &&
              config.ordered_events == 65536, "ordered parser accepts explicit bounded ceiling");
        check(!MeasureProbes::parse_spec("ordered=65537", config, error) &&
              !MeasureProbes::parse_spec("ordered=-1", config, error), "ordered parser rejects bad capacity");
        check(!MeasureProbes::parse_spec("ordered=1", config, error), "ordered needs selected numeric owners");
        check(MeasureProbes::parse_spec("sets=none;sample=0x80001000:4:494,0x80001004:4:1;first=0;pb=none", config, error) &&
              config.samples.size() == 2 && config.samples[0].log_capacity == 494 && config.samples[1].log_capacity == 1,
              "per-owner measured sample capacities");
        for (const char* bad : {"sample=0x80001000:4:0", "sample=0x80001000:4:65537", "sample=0x80001000:4:-1",
                               "sample=0x80001000:4:", "sample=0x80001000::494", "sample=0x80001000:4:1:2"}) {
            check(!MeasureProbes::parse_spec(bad, config, error), "malformed or oversized sample capacity refused");
        }
        check(MeasureProbes::parse_spec("sample=0x80001000:4", config, error) && config.samples[0].log_capacity == 256,
              "old sample syntax keeps unchanged default");
        auto system = std::make_unique<System>();
        RecompiledCode code;
        const RecompiledCode::Entry entry{0x80005000u, 4u, [](Cpu&) {}, "test-entry"};
        const RecompiledCode::Range range{0x80005000u, 0x80005004u, [](Cpu&, u32) {}};
        code.install(&entry, 1); code.install_ranges(&range, 1);
        auto& cpu = system->cpu(); cpu.attach_recompiled(&code);
        check(MeasureProbes::parse_spec("sets=none;sample=none;pb=none;numeric=0x80005000;first=3;ordered=2", config, error),
              "fixture config");
        MeasureProbes probes(*system, config); probes.install();
        for (u32 i = 0; i < 33; ++i) {
            cpu.spr[SPR_GQR0] = i; cpu.fpscr = 4;
            cpu.gpr[4] = i;
            cpu.fpr[18].bits[0] = 0xfff8000000000001ull;
            cpu.fpr[18].bits[1] = i;
            cpu.fpr[19].bits[0] = 0x3ff0000000000000ull;
            cpu.fpr[19].bits[1] = 0x8000000000000000ull;
            std::vector<u8> before, after;
            cpu.save_state(before);
            check(!cpu.try_hle(entry.address), "observer never serves the entry");
            cpu.save_state(after);
            check(before == after, "numeric observer never changes CPU/continuation state");
        }
        const auto report = probes.json();
        check(report.find("\"capacity\":2") != std::string::npos &&
              report.find("\"stored\":2,\"dropped\":31") != std::string::npos &&
              report.find("\"sequence\":2") != std::string::npos &&
              report.find("\"0xFFF8000000000001\"") != std::string::npos &&
              report.find("\"0x8000000000000000\"") != std::string::npos &&
              report.find("\"context_fields_observed\":7") != std::string::npos,
              "ordered runtime evidence is bounded, exact and context reads identified");
        check(report.find("\"unclassified\":1") != std::string::npos &&
              report.find("\"complete_for_observed_entries\":false") != std::string::npos,
              "runtime overflow refuses completeness");
        check(report.find("\"r7\":\"0x00000000\",\"fpscr\":\"0x00000004\",\"gqr\":[\"0x00000002\"") != std::string::npos,
              "each first sample has its own numeric evidence");
        check(report.find("\"entry_r4_evidence\":{\"hits\":33,\"unclassified\":1,\"complete_for_observed_entries\":false")
                  != std::string::npos && report.find("\"r4\":\"0x0000001F\",\"hits\":1") != std::string::npos,
              "all-hit argument histogram goes beyond first samples without silent truncation");
        check(MeasureProbes::parse_spec("sets=none;sample=0x80001000:4:494,0x80001004:4:1;pb=none;first=0", config, error),
              "sampler fixture config");
        MeasureProbes sampled(*system, config); sampled.install();
        // One VI frame (two native fields) per sample: the IPL's PAL display.
        system->video().boot(true);
        system->video().install();
        for (u32 i = 0; i < 301; ++i) {
            system->guest_memory().write32(0x80001000, i); system->guest_memory().write32(0x80001004, i);
            system->video().on_retrace(0u);
            system->video().on_retrace(0u);
            std::vector<u8> before, after; cpu.save_state(before);
            sampled.on_host_service(); sampled.on_host_service(); // same VI must not append twice
            cpu.save_state(after); check(before == after, "memory sampling never changes CPU state");
            check(system->guest_memory().peek32(0x80001000) == i && system->guest_memory().peek32(0x80001004) == i,
                  "memory sampling never changes observed words");
        }
        const auto sampled_report = sampled.json();
        check(sampled_report.find("\"log_capacity\":494,\"log_retained_capacity\":494,\"log_retained_bytes\":3952,\"samples\":301,\"changes\":300") != std::string::npos &&
              sampled_report.find("\"log_overflow\":0") != std::string::npos &&
              sampled_report.find("\"log_overflow\":300") != std::string::npos,
              "301 changes exceed old256 history; per-owner bounds and overflow remain explicit");
        config.samples[0].log_capacity = 65537;
        rejected = false;
        try { MeasureProbes bad_capacity(*system, config); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "programmatic oversized sample history refused");
        std::cout << "numeric_probe_tests: bounded grouping, overflow and parser passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
