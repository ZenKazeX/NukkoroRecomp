#pragma once

// The renderer's Tracy zones (HW 2026-10-20, HW_REPORT L.6; ES limit 6). With SHADOW_TRACY (the trace build: CMake
// gives shadow_render / shadow_render_lifted the define, Tracy and runtime/include only inside
// if(SHADOW_ENABLE_TRACY)) these are gekko/profiling.h's macros; otherwise they compile to nothing, and no include
// path changes. Names follow profiling.h: "subsystem/what", per frame or per resource, never per element.

#if defined(SHADOW_TRACY)
#include "gekko/profiling.h"
#else
#define GEKKO_PROFILE_ZONE(name)
#define GEKKO_PROFILE_VALUE(value)
#define GEKKO_PROFILE_THREAD(name)
#define GEKKO_PROFILE_PLOT(name, value)
#endif
