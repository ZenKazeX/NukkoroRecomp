#pragma once

// RenderWare GameCube driver entries nothing enters in the lifted build (docs/agents/axfx-effects.md, "Step K").
//
// RxPipelineExecute 0x8049C6C4 starts a pipeline: it is the ONLY call of a node definition's nodeBody method in the whole DOL
// (tools/platform/closure_proof.py, tools/platform/indirect_calls.py; docs/agents/axfx-effects.md). It has exactly five callers -
// AtomicDefaultRenderCallBack 0x80456F04 and the world-sector render callback 0x80460B60 (their call sites are lowered to
// rw_pipeline_execute_atomic / _sector) and the three RwIm3D entry points (owned natively) - no materialisation, no data word and no
// plain branch refers to it. So no pipeline, and with it no node body, runs in the lifted build. The owner stops the machine naming
// the entry, so a path the proof did not see says so instead of running GX code that has no consumer. The legacy tree keeps the body.
//
// Its two node bodies are stopped too: 0x80464AAC (the all-in-one node, nodeBody word 0x8056E990) and 0x80464FF4 (the world-sector
// node, word 0x8056E9F8) have no call, branch or materialisation, one data word each, and the DOL has exactly one call of a
// nodeBody method - the dispatch inside RxPipelineExecute. Limit: a read of the method slot with another instruction shape.
// 0x804A0B68 (SubmitNoLightNode) is the same: no call, branch or materialisation, one data word (0x8056FD84 = nodeBody slot of the
// node definition at 0x8056FD80, returned by 0x804A17B0). Owning it frees 0x80498854 (the Im bind) in the planner.
//
// (The render callbacks stored into the pipelines' nodes and the raster-render wrappers are NOT stopped: their
// closure over the readers of the stored pointers is not proven. See the Step K section.)
#include "gekko/types.h"

#include <cstddef>

namespace gekko {
class Cpu;
}

namespace gekko::rw_node_stops {

struct StopEntry {
    u32 address;
    const char* name;
};

const StopEntry* entries(std::size_t& count) noexcept;
void install_lifted(Cpu& cpu);

}  // namespace gekko::rw_node_stops
