#include "object/type_cast.h"

namespace shadow::object {

CastResult find_subobject(const ClassInfo* dynamic_type, std::ptrdiff_t source_offset,
                          TypeId target, TypeId source) noexcept {
    if (dynamic_type == nullptr)
        return {};
    if (target == 0 || dynamic_type->type == target)
        return {true, 0};
    for (std::size_t i = 0; i < dynamic_type->base_count; ++i) {
        const BaseSubobject& base = dynamic_type->bases[i];
        if (base.type != target)
            continue;
        if (!base.source_restricted)
            return {true, base.offset};
        if (base.offset != source_offset)
            continue;
        for (std::size_t j = 0; j < base.source_count; ++j) {
            const SourceSubobject& candidate = base.sources[j];
            if (candidate.offset == source_offset && candidate.type == source)
                return {true, candidate.offset};
        }
        // The first matching restricted entry determines failure; a later
        // entry cannot make an ambiguous source into a successful cast.
        return {};
    }
    return {};
}

CastResult find_compiled_subobject(TypeId dynamic_type, const CompiledCastMatch* matches,
                                   std::size_t count) noexcept {
    std::size_t first = 0, end = count;
    while (first != end) {
        const auto middle = first + (end - first) / 2;
        if (matches[middle].dynamic_type < dynamic_type)
            first = middle + 1;
        else
            end = middle;
    }
    if (first < count && matches[first].dynamic_type == dynamic_type)
        return {true, matches[first].offset};
    return {};
}

}  // namespace shadow::object
