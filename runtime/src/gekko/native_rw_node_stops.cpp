#include "gekko/native_rw_node_stops.h"

#include "gekko/cpu.h"

#include <cstdio>
#include <string>

namespace gekko::rw_node_stops {
namespace {

constexpr StopEntry kEntries[] = {
    {0x8049C6C4u, "RxPipelineExecute"},
    {0x80464AACu, "RwGC all-in-one pipeline node body"},
    {0x80464FF4u, "RwGC world-sector pipeline node body"},
    {0x804A0B68u, "RwGC SubmitNoLight pipeline node body"},
};

}  // namespace

const StopEntry* entries(std::size_t& count) noexcept {
    count = sizeof(kEntries) / sizeof(kEntries[0]);
    return kEntries;
}

void install_lifted(Cpu& cpu) {
    for (const StopEntry& entry : kEntries) {
        cpu.install_hle(entry.address, [entry](Cpu& c) {
            char address[16];
            std::snprintf(address, sizeof(address), "0x%08X", entry.address);
            c.request_stop(StopReason::ExecutionFault,
                           std::string("RenderWare entry ") + address + " (" + entry.name +
                               ") was entered: the lifted build lowers the pipeline execution and the raster render, "
                               "no stage or recording reaches this entry (docs/agents/axfx-effects.md, Step K)");
            return true;
        }, std::string("RenderWare entry nothing enters: ") + entry.name);
    }
}

}  // namespace gekko::rw_node_stops
