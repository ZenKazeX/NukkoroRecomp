#pragma once

#include <cstddef>
#include <cstdint>

namespace shadow::object {

// Identities are interned class names, not addresses or name hashes. The
// metadata owner must give equal names the same identity and distinct names
// different identities. Zero names the complete-object (void*) query.
using TypeId = std::uint32_t;

struct SourceSubobject {
    TypeId type;
    std::ptrdiff_t offset;  // from the complete object
};

struct BaseSubobject {
    TypeId type;
    std::ptrdiff_t offset;
    const SourceSubobject* sources = nullptr;
    std::size_t source_count = 0;
    // Some repeated-base entries require an exact source subobject match.
    // An empty restricted entry fails; it is not an unrestricted entry.
    bool source_restricted = false;
};

struct ClassInfo {
    TypeId type;
    const BaseSubobject* bases = nullptr;
    std::size_t base_count = 0;
};

struct CastResult {
    bool found = false;
    std::ptrdiff_t offset = 0;  // from the complete object, meaningful when found
};

// Immutable metadata is owned outside this lookup and must outlive the call.
// The input offset locates the source subobject in the complete object. The
// caller handles null object pointers and reference-cast failure policy.
// No allocation, string comparison, pointer conversion or object access occurs.
CastResult find_subobject(const ClassInfo* dynamic_type, std::ptrdiff_t source_offset,
                          TypeId target, TypeId source) noexcept;

// An immutable query compiled for one target, with source-independent matches
// sorted by dynamic identity. Compact storage is allowed only after its owner
// proves the identity/offset bounds; input identities and result offsets remain
// full-width. No pointer or address is stored in the compact fields.
struct CompiledCastMatch {
    std::uint16_t dynamic_type;
    std::int16_t offset;
};

CastResult find_compiled_subobject(TypeId dynamic_type, const CompiledCastMatch* matches,
                                   std::size_t count) noexcept;

}  // namespace shadow::object
