// The engine instance and the host memory functions (see rw/core/engine.h).
#include "rw/core/engine.h"

#include <cstdlib>

void* RwEngineInstance = nullptr;
RwInt32 rwNumEngineInstances = 0;

namespace {

void* hostMalloc(std::size_t size, RwUInt32 /*hint*/) { return std::malloc(size); }
void hostFree(void* mem) { std::free(mem); }
void* hostRealloc(void* mem, std::size_t size, RwUInt32 /*hint*/) { return std::realloc(mem, size); }
void* hostCalloc(std::size_t num, std::size_t size, RwUInt32 /*hint*/) { return std::calloc(num, size); }

}  // namespace

RwMemoryFunctions rwHostMemoryFunctions() { return {hostMalloc, hostFree, hostRealloc, hostCalloc}; }
