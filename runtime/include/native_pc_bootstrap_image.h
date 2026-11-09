#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace shadow::pc_bootstrap {

// Independent process-image owner. Original image addresses are linker
// metadata, not a CPU/bus/device model. Not wired into the shipping bootstrap.
struct InitializedImage {
    std::uint32_t link_base = 0;
    std::uint32_t original_entry = 0;
    std::vector<std::uint8_t> storage;
    std::span<std::uint8_t> range(std::uint32_t linked_address, std::size_t size);
    std::uint32_t word(std::uint32_t linked_address);
};

// Caller supplies actual host memory budget; demand comes from section/BSS
// extents, with no MEM1 reservation or hardware state.
InitializedImage load_initialized_image(std::span<const std::uint8_t> dol,
                                       std::uint64_t available_host_bytes);

// Original-word-verified CRT memory contract only. No register, FP, exception,
// timing, constructor or event semantics. Validate all inputs before clearing.
std::uint64_t apply_verified_crt_initializers(InitializedImage& image);
}  // namespace shadow::pc_bootstrap
