// The RenderWare world-sector pipeline entries (gx-rw-plan cluster rwR4) as native owners.
//
// The GameCube driver's world-sector pipeline (node definitions, instance / render callbacks, the stream callbacks that
// SIZE and WRITE native world data) is executed by RxPipelineExecute, whose six call sites are lowered into the native
// renderer (rw_pipeline_execute_atomic / _sector / ...), so none of its node bodies runs in the lifted build. Measured on
// the Westopolis, Final Haunt and Lethal Highway recordings (docs/agents/vi.md "Step 5"): of the 22 frontier rows only
// four are entered at all; the other 18 have 0 hits on all three.
//
//  * 0x804651D8 (the pipeline's node-definition init) and 0x80465210 (an accessor) are served exactly: the same words
//    written, the same value returned (reference test over the original bodies).
//  * The five native-data stream callbacks (size / write / read of the geometry and the world sector, native_rw_registry.cpp)
//    and the four vertex-format decoders (native_rw_world_decode.cpp) are exact owners as well: contract tests against the
//    original body (rw_world_stream_contract_tests, rw_world_decode_tests).
//  * The 9 entries nothing enters are owners that STOP the machine naming the entry: if a path the recordings do not
//    reach ever enters one, the run says so instead of executing GX code that has no consumer.
//  * 0x80465818 (a plugin attach calling the translated plugin registry) and 0x804662B0 (the native world-sector
//    stream reader, executed at every stage load) stay translated: serving them is the RenderWare reconstruction's job.
#pragma once

#include "gekko/cpu.h"

#include <cstddef>

namespace gekko {

struct RwWorldStopEntry {
    u32 address;
    const char* name;
};

// The entries that stop the run when entered (for the reference test and the pin test).
const RwWorldStopEntry* rw_world_stop_entries(std::size_t* count) noexcept;

// Installs the owners (LiftedRender::install_owners).
void install_native_rw_world_owners(Cpu& cpu);

// rwR3, the Im3D entry points (RwIm3DTransform 0x8049B87C, RwIm3DEnd 0x8049B95C, RwIm3DRenderIndexedPrimitive 0x8049B9AC,
// RwIm3DRenderPrimitive 0x8049BB48). Their bodies keep the immediate-mode state in the engine's plugin-globals slot and run the
// Im3D pipeline; the pipeline execution is the native renderer's (scaffold rw_pipeline_execute_im3d_*), passed in so the
// reference test can observe the call. `execute(cpu, pipeline, data, option)` returns the original's r3.
using Im3dExecuteFn = u32 (*)(Cpu&, u32 pipeline, u32 data, u32 option);
void install_native_rw_im3d_owners(Cpu& cpu, Im3dExecuteFn transform, Im3dExecuteFn render);

}  // namespace gekko
