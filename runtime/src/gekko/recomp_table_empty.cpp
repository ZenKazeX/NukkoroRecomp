// The recompiled-function table for a build with no generated code.
//
// CMake compiles this file when SHADOW_RECOMP_DOL is not set, so the runtime
// links and runs as a pure interpreter. When a game image is supplied the
// generated table replaces it and nothing else in the runtime changes.

#include "gekko/recomp.h"

namespace gekko {
namespace recomp {

const RecompiledCode::Entry kFunctions[] = {{0, 0, nullptr, nullptr}};
const std::size_t kFunctionCount = 0;
const RecompiledCode::Range kRanges[] = {{0, 0, nullptr}};
const std::size_t kRangeCount = 0;
const u32 kNativeOwners[] = {0u};
const std::size_t kNativeOwnerCount = 0;
const GxSiteRecord kGxSites[] = {{0u, 0u, GxSiteFamily::Dead}};
const std::size_t kGxSiteCount = 0;
const PlatformSiteRecord kPlatformSites[] = {{0u, 0u, false}};
const std::size_t kPlatformSiteCount = 0;
const LiftSiteRecord kLiftSites[] = {{0u, LiftSiteContinuation::Sequential, nullptr}};
const std::size_t kLiftSiteCount = 0;
#if !defined(GEKKO_SHIPPING)
const u32 kLiftSiteWords[] = {0u};
#endif

}  // namespace recomp
}  // namespace gekko
