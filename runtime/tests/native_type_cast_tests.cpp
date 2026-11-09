// Recovered cast contract against the complete original instruction body.
// liveness-roots: reference-interpreter-only
// The generated preparation helper sets values/frame only; it never dispatches game code.
#include "object/type_cast.h"
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "native_type_catalog.inc"
#include "native_type_boundary.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace gekko;
namespace object = shadow::object;
constexpr u32 kEntry = 0x803A1AFCu, kThrow = 0x803A2660u;
constexpr u32 kStack = 0x817E0000u, kReturn = 0x817FF000u;
constexpr u32 kObject = 0x81500000u, kMetadata = 0x81600000u;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Base {
    object::TypeId type;
    u32 offset;
    bool restricted;
    std::vector<object::SourceSubobject> sources;
};

struct Oracle {
    GuestMemory memory;
    Cpu cpu{memory};
    u32 cursor = kMetadata;
    u64 instructions = 0;
    unsigned cases = 0, throws = 0, successes = 0, failures = 0;
    unsigned shipped_cases = 0;
    unsigned catalog_cases = 0, catalog_throws = 0;
    unsigned compiled_cases = 0;

    explicit Oracle(const char* path) {
        std::ifstream file(path, std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(file), {}};
        require(dol.size() >= 256, "missing DOL");
        for (unsigned i = 0; i < 18; ++i) {
            const auto word = [&](std::size_t at) { return load_be32(dol.data() + at); };
            const u32 off = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(off) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + off, size);
        }
        u64 hash = 14695981039346656037ull;
        for (u32 at = kEntry; at < kEntry + 612u; ++at)
            hash = (hash ^ memory.peek8(at)) * 1099511628211ull;
        require(hash == 0xa25f15561751807eull, "original cast body changed");
    }

    u32 reserve(u32 size) {
        const u32 result = cursor;
        cursor += (size + 3u) & ~3u;
        require(cursor < kMetadata + 0x10000u, "fixture metadata overflow");
        return result;
    }

    u32 type(object::TypeId id) {
        // Every call creates a distinct descriptor and name address. Name
        // identity, including a non-ASCII byte, must determine equivalence.
        const std::string name = "Class::" + std::to_string(id) + "::\xE9";
        const u32 descriptor = reserve(8), text = reserve(static_cast<u32>(name.size() + 1));
        memory.write_block(text, name.c_str(), name.size() + 1);
        memory.write32(descriptor, text);
        memory.write32(descriptor + 4, 0);
        return descriptor;
    }

    void check(object::TypeId dynamic, object::TypeId target, object::TypeId source,
               u32 source_offset, u32 vptr_offset, u32 reference,
               const std::vector<Base>& bases, bool null_object = false) {
        cursor = kMetadata;
        cpu.reset();
        cpu.pc = kEntry;
        cpu.lr = kReturn;
        cpu.gpr[1] = kStack;
        cpu.fpscr = 0xF1234567u;
        for (u32 i = 14; i < 32; ++i) cpu.gpr[i] = 0xABCD0000u + i;
        const u32 actual_object = kObject + source_offset;
        const u32 dynamic_descriptor = dynamic ? type(dynamic) : 0u;
        const u32 vtable = reserve(8);
        memory.write32(vtable, dynamic_descriptor);
        memory.write32(vtable + 4, 0u - source_offset);
        memory.write32(actual_object + vptr_offset, vtable);
        cpu.gpr[3] = null_object ? 0 : actual_object;
        cpu.gpr[4] = vptr_offset;
        cpu.gpr[5] = target ? type(target) : 0u;
        cpu.gpr[6] = type(source);
        cpu.gpr[7] = reference;

        std::vector<object::BaseSubobject> native_bases;
        const u32 list = reserve(512);
        u32 at = list;
        for (const auto& base : bases) {
            memory.write32(at, type(base.type));
            memory.write32(at + 4, base.offset | (base.restricted ? 0x80000000u : 0u));
            at += 8;
            if (base.restricted) {
                memory.write32(at, static_cast<u32>(base.sources.size()));
                at += 4;
                for (const auto& item : base.sources) {
                    memory.write32(at, type(item.type));
                    memory.write32(at + 4, static_cast<u32>(item.offset));
                    at += 8;
                }
            }
            native_bases.push_back({base.type, base.offset, base.sources.data(), base.sources.size(), base.restricted});
        }
        require(at + 4 <= list + 512, "fixture base list overflow");
        memory.write32(at, 0);
        if (dynamic && !bases.empty()) memory.write32(dynamic_descriptor + 4, list);
        const object::ClassInfo info{dynamic, native_bases.data(), native_bases.size()};
        const auto result = object::find_subobject(dynamic ? &info : nullptr, source_offset, target, source);
        const u32 expected = !null_object && result.found ? kObject + static_cast<u32>(result.offset) : 0;
        const bool throws_expected = !null_object && !result.found && (reference & 0xFFFFu) != 0;
        std::vector<u8> before(cursor - kMetadata);
        memory.read_block(kMetadata, before.data(), before.size());
        const auto ticks = cpu.game_clock().now();
        unsigned steps = 0;
        while (cpu.pc != kReturn && cpu.pc != kThrow && !cpu.stopped() && steps++ < 20000)
            cpu.run_steps(1);
        require(!cpu.stopped() && steps < 20000, "reference cast failed to terminate");
        if ((cpu.pc == kThrow) != throws_expected || (!throws_expected && cpu.gpr[3] != expected)) {
            std::fprintf(stderr, "case %u dynamic=%u target=%u source=%u offset=%u ref=%08X "
                                 "expected=%08X throw=%u actual=%08X pc=%08X\n",
                         cases, dynamic, target, source, source_offset, reference,
                         expected, throws_expected ? 1u : 0u, cpu.gpr[3], cpu.pc);
            throw std::runtime_error("native type lookup differs from original cast");
        }
        if (throws_expected) {
            require(cpu.gpr[3] == 0x80514CC8u && cpu.gpr[4] == kStack - 24u &&
                        cpu.gpr[5] == 0x803A1D60u && memory.peek32(kStack - 24u) == 0x80568DD4u,
                    "reference failure exception contract changed");
            ++throws;
        } else {
            require(cpu.pc == kReturn && cpu.gpr[1] == kStack, "reference cast changed return/stack");
            for (u32 i = 14; i < 32; ++i)
                require(cpu.gpr[i] == 0xABCD0000u + i, "reference cast changed nonvolatile register");
            if (expected) ++successes; else ++failures;
        }
        std::vector<u8> after(before.size());
        memory.read_block(kMetadata, after.data(), after.size());
        require(before == after && memory.peek32(actual_object + vptr_offset) == vtable,
                "reference cast wrote to class metadata or object");
        require(cpu.fpscr == 0xF1234567u && cpu.game_clock().now() == ticks,
                "reference cast changed floating status or game clock");
        instructions += steps;
        ++cases;
    }

    void check_shipped() {
        // Actual descriptors reached by the first 256 development-build cast
        // observations, plus the verified PlayerShadow and effect-system
        // vtables. This is an explicit seed set, not whole-game coverage.
        const std::vector<u32> descriptors{
            0x8056B4B8u, 0x8056B4CCu, 0x8056B548u, 0x8056B55Cu,
            0x8056BCD0u, 0x8056BCE4u, 0x8056BCF4u, 0x8056BD08u,
            0x8056BD10u, 0x8056BD24u, 0x8056BDB4u, 0x8056BDC8u,
            0x8056BEA0u, 0x8056BEB4u, 0x8056BF38u, 0x8056BF4Cu,
            0x8056BFB8u, 0x8056BFCCu, 0x8056C060u, 0x8056C074u,
            0x805E4620u, 0x805E4628u, 0x805E4660u, 0x805E4670u,
            0x805E46C8u, 0x805E46D0u, 0x805E5098u, 0x805E50A8u,
            0x805E5678u, 0x805E5680u, 0x805E5888u, 0x805E58A0u,
            0x805E590Cu, 0x805E5914u, 0x805E5AF0u, 0x805E5DD8u,
            0x805E5DE0u, 0x805E6A8Cu, 0x805E6A94u, 0x805E9950u,
            0x805E9958u, 0x805E9B60u, 0x805E9B68u, 0x805E9D98u
        };
        std::map<std::string, object::TypeId> identities;
        const auto identity = [&](u32 descriptor) {
            if (!descriptor) return object::TypeId{0};
            const u32 text = memory.peek32(descriptor);
            std::string name;
            for (unsigned i = 0; i < 1024; ++i) {
                const char byte = static_cast<char>(memory.peek8(text + i));
                if (!byte) break;
                name.push_back(byte);
            }
            require(!name.empty() && name.size() < 1024, "invalid shipped type name");
            auto found = identities.find(name);
            if (found != identities.end()) return found->second;
            const auto id = static_cast<object::TypeId>(identities.size() + 1);
            identities.emplace(name, id);
            return id;
        };
        std::vector<u32> targets{0};
        targets.insert(targets.end(), descriptors.begin(), descriptors.end());
        for (const auto descriptor : descriptors) {
            std::vector<object::BaseSubobject> bases;
            std::vector<std::vector<object::SourceSubobject>> sources;
            u32 table = memory.peek32(descriptor + 4);
            for (unsigned i = 0; table && memory.peek32(table) != 0; ++i) {
                require(i < 256, "unterminated shipped base list");
                const auto id = identity(memory.peek32(table));
                const u32 encoded = memory.peek32(table + 4);
                const bool restricted = (encoded & 0x80000000u) != 0;
                const u32 offset = encoded & 0x7FFFFFFFu;
                table += 8;
                sources.emplace_back();
                if (restricted) {
                    const u32 count = memory.peek32(table);
                    require(count < 256, "invalid shipped source list");
                    table += 4;
                    for (u32 j = 0; j < count; ++j) {
                        const auto source = identity(memory.peek32(table));
                        const auto location = static_cast<s32>(memory.peek32(table + 4));
                        sources.back().push_back({source, location});
                        table += 8;
                    }
                }
                bases.push_back({id, offset, sources.back().data(), sources.back().size(), restricted});
            }
            const object::ClassInfo info{identity(descriptor), bases.data(), bases.size()};
            for (const auto target : targets)
                for (const auto source : {descriptor, descriptors.front()})
                    for (u32 source_offset : {0u, 24u, 56u}) {
                        cpu.reset();
                        cpu.pc = kEntry; cpu.lr = kReturn; cpu.gpr[1] = kStack;
                        const u32 object_address = kObject + source_offset;
                        memory.write32(object_address + 0x38u, kMetadata);
                        memory.write32(kMetadata, descriptor);
                        memory.write32(kMetadata + 4, 0u - source_offset);
                        cpu.gpr[3] = object_address; cpu.gpr[4] = 0x38u;
                        cpu.gpr[5] = target; cpu.gpr[6] = source; cpu.gpr[7] = 1;
                        const auto result = object::find_subobject(&info, source_offset, identity(target), identity(source));
                        unsigned steps = 0;
                        while (cpu.pc != kReturn && cpu.pc != kThrow && !cpu.stopped() && steps++ < 20000)
                            cpu.run_steps(1);
                        require(!cpu.stopped() && steps < 20000, "shipped-table reference failed to terminate");
                        require(result.found ? cpu.pc == kReturn && cpu.gpr[3] == kObject + static_cast<u32>(result.offset)
                                             : cpu.pc == kThrow,
                                "native lookup differs on actual shipped metadata");
                        instructions += steps;
                        ++shipped_cases;
                    }
        }
    }

    void check_catalog() {
        namespace fixture = type_cast_catalog_fixture;
        static_assert(sizeof(object::ClassInfo) == 24 && sizeof(object::BaseSubobject) == 40 &&
                      sizeof(object::SourceSubobject) == 16, "catalog size estimates require the x64 layout");
        const auto info_for = [&](u32 descriptor) -> const object::ClassInfo& {
            const auto found = std::lower_bound(fixture::descriptors.begin(), fixture::descriptors.end(), descriptor,
                [](const auto& item, u32 address) { return item.address < address; });
            require(found != fixture::descriptors.end() && found->address == descriptor, "catalog descriptor missing");
            require(found->class_index < fixture::classes.size(), "catalog class index invalid");
            return fixture::classes[found->class_index];
        };
        std::map<object::TypeId, u32> representatives;
        for (const auto& descriptor : fixture::descriptors)
            representatives.emplace(info_for(descriptor.address).type, descriptor.address);
        const auto metadata_bytes = [&] {
            std::vector<u8> bytes;
            for (const auto& span : fixture::metadata_spans) {
                const auto begin = bytes.size();
                bytes.resize(begin + span.size);
                memory.read_block(span.address, bytes.data() + begin, span.size);
            }
            return bytes;
        };
        const auto before = metadata_bytes();
        const auto query = [&](u32 descriptor, u32 vtable, u32 source_offset, u32 target, u32 source, u32 reference,
                               const fixture::Query* compiled = nullptr) {
            const auto& info = info_for(descriptor);
            const auto target_id = target ? info_for(target).type : object::TypeId{0};
            const auto source_id = info_for(source).type;
            const auto result = compiled ? object::game_type_queries::find_subobject(info.type, compiled->native_query)
                                         : object::find_subobject(&info, source_offset, target_id, source_id);
            const bool throws_expected = !result.found && (reference & 0xffffu) != 0;
            require(source_offset < 0x10000u, "catalog object fixture would overlap metadata");
            const u32 pointer = kObject + source_offset;
            const u32 vptr_offset = std::array<u32,3>{0u, 0x38u, 0xA8u}[catalog_cases % 3u];
            memory.write32(pointer + vptr_offset, vtable);
            const auto prepare = [&] {
                for (u32 at = kStack - 32u; at < kStack + 8u; at += 4) memory.write32(at, 0xA5A5A5A5u);
                cpu.reset();
                cpu.pc = kEntry; cpu.lr = kReturn; cpu.gpr[1] = kStack;
                cpu.gpr[3] = pointer; cpu.gpr[4] = vptr_offset;
                cpu.gpr[5] = target; cpu.gpr[6] = source; cpu.gpr[7] = reference;
                cpu.fpscr = 0xF1234567u;
                for (unsigned r = 14; r < 32; ++r) cpu.gpr[r] = 0x98760000u + r;
            };
            prepare();
            const auto ticks = cpu.game_clock().now();
            unsigned steps = 0;
            while (cpu.pc != kReturn && cpu.pc != kThrow && !cpu.stopped() && steps++ < 20000)
                cpu.run_steps(1);
            require(!cpu.stopped() && steps < 20000, "generated catalog oracle failed to terminate");
            const u32 expected = result.found ? kObject + static_cast<u32>(result.offset) : 0;
            if ((cpu.pc == kThrow) != throws_expected || (!throws_expected && cpu.gpr[3] != expected)) {
                std::fprintf(stderr, "catalog dynamic=%08X vtable=%08X offset=%u target=%08X source=%08X "
                                     "reference=%u expected=%08X throw=%u actual=%08X pc=%08X\n",
                             descriptor, vtable, source_offset, target, source, reference,
                             expected, throws_expected ? 1u : 0u, cpu.gpr[3], cpu.pc);
                throw std::runtime_error("generated native catalog differs from original tables");
            }
            if (throws_expected) {
                require(cpu.gpr[3] == 0x80514CC8u && cpu.gpr[4] == kStack - 24u &&
                            cpu.gpr[5] == 0x803A1D60u && memory.peek32(kStack - 24u) == 0x80568DD4u,
                        "catalog exception decision changed throw payload");
                ++catalog_throws;
            } else {
                require(cpu.gpr[1] == kStack && cpu.lr == kReturn, "catalog cast changed stack or return");
                for (unsigned r = 14; r < 32; ++r)
                    require(cpu.gpr[r] == 0x98760000u + r, "catalog cast changed nonvolatile register");
            }
            require(cpu.fpscr == 0xF1234567u && cpu.game_clock().now() == ticks,
                    "catalog cast changed floating status or time");
            require(memory.peek32(pointer + vptr_offset) == vtable, "catalog cast changed object metadata");
            instructions += steps;
            ++catalog_cases;
            if (compiled) {
                std::array<u8,40> original_frame{}, native_frame{};
                if (throws_expected) memory.peek_block(kStack - 32u, original_frame.data(), original_frame.size());
                prepare();
                const auto tick = cpu.game_clock().now();
                const auto outcome = type_query_boundary::prepare(cpu, compiled->native_query, target);
                require(!cpu.stopped() && outcome != type_query_boundary::Outcome::Fault,
                        "generated call boundary rejected a catalog case");
                require((outcome == type_query_boundary::Outcome::Throw) == throws_expected,
                        "generated call boundary changed exception policy");
                if (throws_expected) {
                    memory.peek_block(kStack - 32u, native_frame.data(), native_frame.size());
                    require(native_frame == original_frame && cpu.pc == kThrow && cpu.lr == 0x803A1D48u &&
                                cpu.gpr[3] == 0x80514CC8u && cpu.gpr[4] == kStack - 24u && cpu.gpr[5] == 0x803A1D60u,
                            "generated call boundary changed exception frame or entry");
                } else {
                    require(cpu.gpr[3] == expected && cpu.pc == kReturn && cpu.lr == kReturn && cpu.gpr[1] == kStack,
                            "generated call boundary changed returned pointer or continuation");
                }
                for (unsigned r = 14; r < 32; ++r)
                    require(cpu.gpr[r] == 0x98760000u + r, "generated boundary changed nonvolatile register");
                require(cpu.fpscr == 0xF1234567u && cpu.game_clock().now() == tick &&
                            memory.peek32(pointer + vptr_offset) == vtable, "generated boundary changed time, FPSCR or object");
                ++compiled_cases;
            }
        };
        for (std::size_t i = 0; i < fixture::descriptors.size(); ++i) {
            const auto descriptor = fixture::descriptors[i].address;
            const auto& info = info_for(descriptor);
            std::vector<u32> targets{0, descriptor, representatives.at(info.type),
                                    fixture::descriptors[(i + 97) % fixture::descriptors.size()].address};
            std::vector<u32> offsets{0, 56};
            for (std::size_t b = 0; b < info.base_count; ++b) {
                targets.push_back(representatives.at(info.bases[b].type));
                require(info.bases[b].offset >= 0 && info.bases[b].offset < 0x10000,
                        "catalog fixture offset out of range");
                offsets.push_back(static_cast<u32>(info.bases[b].offset));
            }
            std::sort(targets.begin(), targets.end());
            targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
            std::sort(offsets.begin(), offsets.end());
            offsets.erase(std::unique(offsets.begin(), offsets.end()), offsets.end());
            for (u32 offset : offsets) {
                memory.write32(kMetadata, descriptor);
                memory.write32(kMetadata + 4, 0u - offset);
                for (u32 target : targets)
                    for (u32 reference : {0u, 1u})
                        query(descriptor, kMetadata, offset, target, descriptor, reference);
            }
            for (const auto& plan : fixture::queries) {
                const auto offset = static_cast<u32>((i + plan.target_descriptor) & 1u) * 56u;
                memory.write32(kMetadata, descriptor); memory.write32(kMetadata + 4, 0u - offset);
                query(descriptor, kMetadata, offset, plan.target_descriptor, descriptor, static_cast<u32>(i & 1u), &plan);
            }
        }
        // Also use every discovered original virtual table unchanged. This
        // exercises the actual complete-object adjustments, not re-encoded ones.
        std::size_t native_vtable_case = 0;
        for (const auto& vtable : fixture::vtables) {
            require(vtable.adjustment <= 0, "unexpected positive adjustment needs a separate object fixture");
            const auto offset = static_cast<u32>(-std::int64_t{vtable.adjustment});
            const auto& info = info_for(vtable.descriptor);
            const auto& plan = fixture::queries[native_vtable_case++ % fixture::queries.size()];
            query(vtable.descriptor, vtable.address, offset, plan.target_descriptor, vtable.descriptor,
                  static_cast<u32>(native_vtable_case & 1u), &plan);
            query(vtable.descriptor, vtable.address, offset, 0, vtable.descriptor, 1);
            query(vtable.descriptor, vtable.address, offset, representatives.at(info.type), vtable.descriptor, 1);
            for (std::size_t b = 0; b < info.base_count; ++b)
                query(vtable.descriptor, vtable.address, offset, representatives.at(info.bases[b].type), vtable.descriptor, 1);
        }
        require(before == metadata_bytes(), "catalog oracle wrote original class metadata");
    }
};

// The representation may collapse classes that cannot match any compiled
// query, but it must recognize exactly the original descriptor domain. Check
// every byte position in represented prefixes, including holes/alignment/padding,
// plus an alias probe in every unrepresented high prefix.
u64 check_binding_domain() {
    namespace fixture = type_cast_catalog_fixture;
    std::map<u32, object::TypeId> known;
    std::set<u32> prefixes;
    std::set<object::TypeId> queried;
    for (const auto& descriptor : fixture::descriptors) {
        known.emplace(descriptor.address, fixture::classes[descriptor.class_index].type);
        prefixes.insert(descriptor.address >> 16);
    }
    for (const auto& query : fixture::queries) {
        const auto target = known.at(query.target_descriptor);
        for (const auto& info : fixture::classes)
            if (object::find_subobject(&info, 0, target, info.type).found) queried.insert(info.type);
    }
    u64 cases = 0;
    for (u32 prefix : prefixes) for (u32 suffix = 0; suffix <= 0xffffu; ++suffix) {
        const u32 address = (prefix << 16) | suffix;
        const auto found = known.find(address);
        const auto actual = type_query_boundary::detail::identity(address);
        require((actual != 0) == (found != known.end()), "binding domain accepted a hole/unaligned address or lost a descriptor");
        if (found != known.end()) {
            if (queried.count(found->second)) require(actual == found->second, "queried dynamic identity changed");
            else for (const auto& query : fixture::queries)
                require(!object::game_type_queries::find_subobject(actual, query.native_query).found,
                        "unqueried class became a successful native cast");
        }
        ++cases;
    }
    const u32 suffix = known.begin()->first & 0xffffu;
    for (u32 prefix = 0; prefix <= 0xffffu; ++prefix) if (!prefixes.count(prefix)) {
        require(type_query_boundary::detail::identity((prefix << 16) | suffix) == 0,
                "unknown high prefix aliased a known descriptor");
        ++cases;
    }
    return cases;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected original DOL path");
        Oracle oracle(argv[1]);
        // Explicit branch boundaries, including ambiguous restricted sources,
        // duplicate names at distinct addresses, first-match semantics and a
        // failed restricted candidate before a later unrestricted candidate.
        const std::vector<std::vector<Base>> shapes{
            {}, {{2, 32, false, {}}}, {{2, 32, true, {}}},
            {{2, 32, true, {{3, 32}}}}, {{2, 32, true, {{4, 32}, {3, 64}}}},
            {{2, 32, true, {{4, 32}, {3, 32}}}},
            {{2, 32, true, {{4, 32}}}, {2, 48, false, {}}},
            {{2, 32, false, {}}, {2, 64, false, {}}},
            {{2, 32, true, {{3, 32}}}, {2, 64, true, {{3, 64}}}},
            {{2, 32, true, {}}, {4, 96, false, {}}}
        };
        for (const auto& shape : shapes)
            for (u32 dynamic : {0u, 1u, 2u})
                for (u32 target : {0u, 1u, 2u, 4u, 7u})
                    for (u32 source : {3u, 4u})
                        for (u32 offset : {0u, 32u, 64u})
                            for (u32 reference : {0u, 1u, 0x8000u, 0x10000u})
                                oracle.check(dynamic, target, source, offset, 0x18u, reference, shape);
        for (u32 reference : {0u, 1u, 0xFFFFFFFFu})
            oracle.check(0, 7, 3, 0, 0, reference, {}, true);
        u32 random = 0x678123BCu;
        const auto next = [&] { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
        for (unsigned i = 0; i < 12000; ++i) {
            std::vector<Base> bases;
            const unsigned count = next() % 7;
            for (unsigned j = 0; j < count; ++j) {
                Base base{1 + next() % 6, (next() % 5) * 32u, (next() & 1) != 0, {}};
                const unsigned sources = next() % 5;
                for (unsigned k = 0; k < sources; ++k)
                    base.sources.push_back({1 + next() % 6, (next() % 5) * 32});
                bases.push_back(std::move(base));
            }
            const auto dynamic = next() % 7, target = next() % 8, source = 1 + next() % 7;
            const auto offset = (next() % 5) * 32u, vptr = (next() % 3) * 24u, reference = next();
            oracle.check(dynamic, target, source, offset, vptr, reference, bases);
        }
        oracle.check_shipped();
        oracle.check_catalog();
        const auto domain_cases = check_binding_domain();
        require(!object::game_type_queries::find_subobject(1,
                    static_cast<object::game_type_queries::Query>(0xffff)).found,
                "invalid native query index unexpectedly matched");
        {
            namespace fixture = type_cast_catalog_fixture;
            namespace boundary = type_query_boundary;
            const auto plan = fixture::queries.front();
            const u32 descriptor = fixture::descriptors.front().address;
            const auto prepare = [&](u32 dynamic, u32 reference = 0) {
                oracle.memory.write32(kObject + 0x38u, kMetadata);
                oracle.memory.write32(kMetadata, dynamic);
                oracle.memory.write32(kMetadata + 4u, 0);
                oracle.cpu.reset(); oracle.cpu.pc = kEntry; oracle.cpu.lr = kReturn;
                oracle.cpu.gpr[1] = kStack; oracle.cpu.gpr[3] = kObject; oracle.cpu.gpr[4] = 0x38u;
                oracle.cpu.gpr[5] = plan.target_descriptor; oracle.cpu.gpr[7] = reference;
            };
            prepare(descriptor, 1); oracle.cpu.gpr[3] = 0; oracle.cpu.gpr[5] = 0;
            require(boundary::prepare(oracle.cpu, plan.native_query, plan.target_descriptor) == boundary::Outcome::Returned &&
                        oracle.cpu.gpr[3] == 0 && !oracle.cpu.stopped(), "null input must ignore type arguments and reference flag");
            for (u32 flag : {0u, 1u, 0x10000u}) {
                prepare(0, flag);
                const auto outcome = boundary::prepare(oracle.cpu, plan.native_query, plan.target_descriptor);
                require(outcome == ((flag & 0xffffu) ? boundary::Outcome::Throw : boundary::Outcome::Returned),
                        "absent metadata changed low-16-bit failure policy");
            }
            for (u32 invalid : {descriptor + 1u, descriptor ^ 0x00010000u, 0xFFFFFFFFu}) {
                prepare(invalid);
                require(boundary::prepare(oracle.cpu, plan.native_query, plan.target_descriptor) == boundary::Outcome::Fault &&
                            oracle.cpu.stopped(), "unknown or aliased binding silently became a class");
            }
            prepare(descriptor, 1);
            oracle.cpu.gpr[5] = 0;
            require(boundary::prepare(oracle.cpu, static_cast<object::game_type_queries::Query>(0xffff), 0) ==
                        boundary::Outcome::Returned && oracle.cpu.gpr[3] == kObject,
                    "complete-object query incorrectly needs a compiled target");
            prepare(0, 1);
            for (u32 at = kStack - 32u; at < kStack + 8u; at += 4) oracle.memory.write32(at, 0x6C6C6C6Cu);
            std::array<u8,40> initial_frame{}, changed_frame{}, restored_frame{};
            oracle.memory.peek_block(kStack - 32u, initial_frame.data(), initial_frame.size());
            oracle.memory.journal_begin();
            const auto outcome = boundary::prepare(oracle.cpu, plan.native_query, plan.target_descriptor);
            oracle.memory.journal_end();
            oracle.memory.peek_block(kStack - 32u, changed_frame.data(), changed_frame.size());
            oracle.memory.journal_rollback();
            oracle.memory.peek_block(kStack - 32u, restored_frame.data(), restored_frame.size());
            require(outcome == boundary::Outcome::Throw && changed_frame != initial_frame && restored_frame == initial_frame,
                    "generated exception writes escaped memory journalling");
            prepare(descriptor); oracle.cpu.gpr[5] ^= 4u;
            require(boundary::prepare(oracle.cpu, plan.native_query, plan.target_descriptor) == boundary::Outcome::Fault &&
                        oracle.cpu.stopped(), "changed target contract was accepted");
            prepare(descriptor);
            require(boundary::prepare(oracle.cpu, static_cast<object::game_type_queries::Query>(0xffff), plan.target_descriptor) ==
                        boundary::Outcome::Fault && oracle.cpu.stopped(), "invalid generated query was accepted");
        }
        static_assert(sizeof(object::CompiledCastMatch) == 4, "compact query layout changed");
        const std::array<object::CompiledCastMatch, 2> compact{{{1, -32768}, {65535, 32767}}};
        for (const auto& entry : compact) {
            const auto result = object::find_compiled_subobject(entry.dynamic_type, compact.data(), compact.size());
            require(result.found && result.offset == entry.offset, "compact query changed signed offset");
        }
        for (const auto absent : {0u, 2u, 65536u, 65537u, 0xFFFFFFFFu})
            require(!object::find_compiled_subobject(absent, compact.data(), compact.size()).found,
                    "compact query narrowed or invented dynamic identity");
        require(!object::find_compiled_subobject(1, nullptr, 0).found, "empty compact query unexpectedly matched");
        // Native offsets and descriptor pointers are genuinely host-sized.
        const std::ptrdiff_t wide = std::ptrdiff_t{1} << 40;
        const object::SourceSubobject child{3, wide};
        const object::BaseSubobject base{2, wide, &child, 1, true};
        const object::ClassInfo info{1, &base, 1};
        const auto wide_result = object::find_subobject(&info, wide, 2, 3);
        require(wide_result.found && wide_result.offset == wide, "native offset narrowed to 32 bits");
        std::printf("PASS native type cast: %u original-body cases, %u successes, %u failures, %u throw decisions, "
                    "%u shipped-table cases, %u generated-catalog cases (%u compiled queries, %u throw decisions), "
                    "%zu descriptors, %zu original vtables, %llu original instructions; host-sized offset check; %zu bytes of native query data; %zu temporary binding bytes; %llu descriptor-domain cases; compiled calls exercise generated preparation\n",
                    oracle.cases, oracle.successes, oracle.failures, oracle.throws,
                    oracle.shipped_cases,
                    oracle.catalog_cases, oracle.compiled_cases, oracle.catalog_throws,
                    type_cast_catalog_fixture::descriptors.size(), type_cast_catalog_fixture::vtables.size(),
                    static_cast<unsigned long long>(oracle.instructions), object::game_type_queries::data_bytes, type_query_boundary::binding_bytes,
                    static_cast<unsigned long long>(domain_cases));
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL native type cast: %s\n", error.what());
        return 1;
    }
}
