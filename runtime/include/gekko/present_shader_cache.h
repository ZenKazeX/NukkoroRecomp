#pragma once

// Build-time-baked presenter programs. tools/shaderbake (set "present")
// compiles runtime/src/gekko/present_shader.h to DXBC during the build and
// generates present_shader_cache.cpp defining this table. These three entry
// points are the only shaders the shipping executable embeds; the legacy GX
// raster programs live in the reference-only gx_shader_cache table. A missing
// entry means the bake is incomplete and is a hard initialisation failure,
// never a silent runtime compile.

#include <cstddef>

namespace gekko {
namespace presentshaders {

struct EmbeddedShader {
    const char* entry;
    const char* target;
    const unsigned char* dxbc;
    std::size_t size;
};

extern const EmbeddedShader kEmbeddedShaders[];
extern const std::size_t kEmbeddedShaderCount;
// FNV-1a over the present HLSL source; recorded for diagnostics.
extern const unsigned long long kShaderSourceHash;

}  // namespace presentshaders
}  // namespace gekko
