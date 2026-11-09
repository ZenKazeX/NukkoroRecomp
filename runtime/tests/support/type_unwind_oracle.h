// Shared exception fixture: original instructions plus an optional caller driver.
#pragma once
// The generated preparation helper sets values/frame only; it never dispatches game code.
#include "object/type_cast.h"
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "native_type_catalog.inc"
#include "native_type_boundary.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace type_unwind_oracle {
using namespace gekko;
namespace object = shadow::object;
namespace fixture = type_cast_catalog_fixture;
constexpr u32 kCast = 0x803A1AFCu, kThrow = 0x803A2660u, kTerminate = 0x803A1A0Cu;
constexpr u32 kCaller = 0x8042711Cu, kAllocation = 0x803A1380u, kCatch = 0x80427174u;
constexpr u32 kStack = 0x817E0000u, kReturn = 0x817FF000u, kCallback = 0x817FD000u;
constexpr u32 kObject = 0x81500000u, kOwned = kObject + 0x1000u, kOutput = kOwned + 0x100u;
constexpr u32 kMetadata = 0x81600000u, kSda = 0x805EC500u, kToc = 0x805FA780u;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Result {
    std::vector<u32> unwind_trace;
    std::array<u32, 19> caught_gpr{};
    std::array<u64, 36> caught_fpr{};
    std::array<u32, 4> catch_record{};
    u32 caught_stack = 0, caught_cr = 0, caught_fpscr = 0;
    u32 terminal = 0, payload = 0, cleanup_value = 0;
    unsigned catches = 0, callbacks = 0, throws = 0;
};

struct Oracle {
    GuestMemory memory;
    Cpu cpu{memory};
    u32 cursor = kMetadata;
    u64 instructions = 0;
    std::array<u32, 153> cast_words{};
    struct MetadataBytes { u32 address; std::vector<u8> bytes; };
    struct Header { u32 address, descriptor; object::TypeId identity; };
    std::vector<MetadataBytes> metadata_bytes;
    std::vector<Header> headers;
    std::size_t metadata_size = 0;

    explicit Oracle(const char* path) {
        std::ifstream file(path, std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(file), {}};
        require(dol.size() >= 256, "missing DOL");
        u64 hash = 14695981039346656037ull;
        for (u8 byte : dol) hash = (hash ^ byte) * 1099511628211ull;
        // Pins exception code, its dispatch tables, unwind descriptors and the
        // original catch/cleanup/rethrow owner, not only the cast body.
        require(hash == 0xd2cfb4a1ca897f9bull, "original DOL exception contract changed");
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = load_be32(dol.data() + i * 4);
            const u32 address = load_be32(dol.data() + 0x48 + i * 4);
            const u32 size = load_be32(dol.data() + 0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory.write_block(address, dol.data() + offset, size);
        }
        for (std::size_t i = 0; i < cast_words.size(); ++i)
            cast_words[i] = memory.peek32(kCast + static_cast<u32>(i) * 4u);
        for (const auto& span : fixture::metadata_spans) {
            MetadataBytes saved{span.address, std::vector<u8>(span.size)};
            memory.read_block(span.address, saved.bytes.data(), saved.bytes.size());
            metadata_size += saved.bytes.size();
            metadata_bytes.push_back(std::move(saved));
        }
        for (const auto& table : fixture::vtables) {
            const auto descriptor = std::lower_bound(fixture::descriptors.begin(), fixture::descriptors.end(),
                table.descriptor, [](const auto& item, u32 address) { return item.address < address; });
            require(descriptor != fixture::descriptors.end() && descriptor->address == table.descriptor,
                    "unwind fixture has an unowned type descriptor");
            headers.push_back({table.address, table.descriptor, fixture::classes[descriptor->class_index].type});
        }
        // The owned object's virtual destructor is a fixture callback that
        // writes one observable field. Original cleanup code dispatches it.
        memory.write32(kCallback, 0x38000001u);      // li r0, 1
        memory.write32(kCallback + 4, 0x90030004u);  // stw r0, 4(r3)
        memory.write32(kCallback + 8, 0x4E800020u);  // blr
    }

    void restore_metadata() {
        for (const auto& span : metadata_bytes)
            memory.write_block(span.address, span.bytes.data(), span.bytes.size());
        for (const auto& header : headers) memory.write32(header.address, header.descriptor);
    }

    void remove_metadata() {
        // A fixture for the pending ownership change: native identities replace
        // the virtual table's type field; class names/descriptors/base lists no
        // longer exist. Exception range records and encoded exception types stay.
        for (const auto& span : metadata_bytes) memory.clear_block(span.address, span.bytes.size());
        for (const auto& header : headers) memory.write32(header.address, header.identity);
    }

    void require_metadata_removed() {
        for (const auto& span : metadata_bytes)
            for (std::size_t i = 0; i < span.bytes.size(); ++i)
                require(memory.peek8(span.address + static_cast<u32>(i)) == 0, "retired class metadata was rewritten");
        for (const auto& header : headers)
            require(memory.peek32(header.address) == header.identity, "native header identity was overwritten");
    }

    std::pair<u32, u32> match_exception_type(const char* target, bool recovered) {
        restore_metadata();
        if (recovered) remove_metadata();
        // Execute the original typed matcher as well as the catch-all cleanup
        // fixture below. The thrown type is the actual bad_cast encoding.
        constexpr u32 matcher = 0x803A17B8u, text = kMetadata + 0x8000u, output = text + 0x100u;
        if (target) memory.write_block(text, target, std::strlen(target) + 1);
        memory.write32(output, 0xA5A5A5A5u);
        cpu.reset();
        cpu.pc = matcher; cpu.lr = kReturn; cpu.gpr[1] = kStack;
        cpu.gpr[3] = 0x80514CC8u; cpu.gpr[4] = target ? text : 0; cpu.gpr[5] = output;
        const auto tick = cpu.game_clock().now();
        unsigned count = 0;
        while (cpu.pc != kReturn && count++ < 10000) step();
        require(cpu.pc == kReturn && cpu.game_clock().now() == tick, "original typed exception matcher did not return");
        if (recovered) require_metadata_removed();
        return {cpu.gpr[3], memory.peek32(output)};
    }

    void step() {
        cpu.run_steps(1);
        ++instructions;
        if (cpu.stopped()) {
            std::fprintf(stderr, "unwind oracle stopped at %08X: %s\n", cpu.pc, cpu.stop().detail.c_str());
            throw std::runtime_error("original unwind execution stopped unexpectedly");
        }
    }

    void register_fragment() {
        cpu.reset();
        memory.write32(0x805A61F8u, 0);
        cpu.gpr[1] = kStack; cpu.gpr[3] = 0x80008D14u; cpu.gpr[4] = kToc;
        cpu.pc = 0x803A3954u; cpu.lr = kReturn;
        unsigned count = 0;
        while (cpu.pc != kReturn && count++ < 2000) step();
        require(cpu.pc == kReturn && cpu.gpr[3] == 0, "original exception registration failed");
    }

    u32 reserve(u32 bytes) {
        const auto result = cursor;
        cursor += (bytes + 3u) & ~3u;
        require(cursor < kMetadata + 0x10000u, "metadata fixture overflow");
        return result;
    }

    u32 type(unsigned id) {
        const auto name = "UnwindType::" + std::to_string(id);
        const auto descriptor = reserve(8), text = reserve(static_cast<u32>(name.size() + 1));
        memory.write_block(text, name.c_str(), name.size() + 1);
        memory.write32(descriptor, text); memory.write32(descriptor + 4, 0);
        return descriptor;
    }

    // Exercise the actual generated failure-entry component. The surrounding
    // production caller/transfer integration remains pending.
    void recovered_failure_entry(u32 flag, bool corrupt_return) {
        require(type_query_boundary::enter_reference_failure(cpu, flag) == type_query_boundary::Outcome::Throw,
                "generated type-query failure entry faulted");
        if (corrupt_return) cpu.lr = kReturn;
    }

    using CallerDriver = u32 (*)(Oracle&, u32, bool);

    Result run(bool recovered, bool caught, unsigned shape, u32 flag, unsigned seed, bool corrupt_return = false,
               CallerDriver caller_driver = nullptr) {
        restore_metadata();
        register_fragment();
        for (std::size_t i = 0; i < cast_words.size(); ++i)
            memory.write32(kCast + static_cast<u32>(i) * 4u, cast_words[i]);
        cursor = kMetadata;
        const u32 dynamic = type(1), target = type(2), source = type(3), wrong_source = type(4);
        const u32 vtable = reserve(12), list = reserve(32);
        const u32 source_offset = shape >= 2 ? 32u : 0u;
        memory.write32(vtable, shape == 0 ? 0 : dynamic);
        memory.write32(vtable + 4, 0u - source_offset);
        memory.write32(vtable + 8, kCallback);
        memory.write32(kObject + source_offset + 0x38u, vtable);
        const object::SourceSubobject wrong{4, 32};
        const object::BaseSubobject base{2, 32, &wrong, shape == 2 ? 0u : 1u, true};
        const object::ClassInfo info{1, &base, shape >= 2 ? 1u : 0u};
        if (shape >= 2) {
            memory.write32(dynamic + 4, list);
            memory.write32(list, target); memory.write32(list + 4, 0x80000020u);
            memory.write32(list + 8, shape == 2 ? 0 : 1);
            memory.write32(list + 12, shape == 2 ? 0 : wrong_source);
            memory.write32(list + 16, 32); memory.write32(list + 20, 0);
        }
        require(!object::find_subobject(shape == 0 ? nullptr : &info, source_offset, 2, 3).found,
                "failure fixture unexpectedly matched native type query");
        for (u32 at = kStack - 0x2000u; at < kStack + 64u; at += 4) memory.write32(at, 0xA5A5A5A5u);
        memory.write32(kOwned, vtable); memory.write32(kOwned + 4, 0);
        memory.write32(kOutput, 0xFFFFFFFFu);
        cpu.reset();
        cpu.gpr[1] = kStack; cpu.gpr[2] = kToc; cpu.gpr[13] = kSda;
        for (unsigned r = 14; r < 32; ++r) cpu.gpr[r] = 0x12340000u + seed * 256u + r;
        for (unsigned r = 14; r < 32; ++r) for (unsigned lane = 0; lane < 2; ++lane) {
            const double value = (r + seed + lane * 37u) * 0.25;
            std::memcpy(&cpu.fpr[r].bits[lane], &value, sizeof(value));
        }
        cpu.cr = 0x72A5B6C9u ^ seed; cpu.xer = seed & 1u ? 0x80000000u : 0;
        cpu.fpscr = 0;
        cpu.lr = kReturn;
        if (caught) {
            cpu.pc = kCaller;
            cpu.gpr[3] = kOutput; cpu.gpr[4] = kOwned; cpu.gpr[5] = kOutput + 16;
            unsigned count = 0;
            while (cpu.pc != kAllocation && count++ < 100) step();
            require(cpu.pc == kAllocation && cpu.lr == 0x80427150u && cpu.gpr[1] == kStack - 64u,
                    "original catch owner prologue changed");
        }
        // Inject the failed cast as the allocator's throwing callee. The caller,
        // its unwind tables, cleanup function and rethrow execute unchanged.
        const auto handler_stack = cpu.gpr[1];
        auto caller_stack = handler_stack;
        cpu.gpr[3] = kObject + source_offset; cpu.gpr[4] = 0x38u;
        cpu.gpr[5] = target; cpu.gpr[6] = source; cpu.gpr[7] = flag;
        cpu.pc = kCast;
        if (recovered) {
            // Native cases must never execute the superseded cast or read its
            // original class metadata, including when a generated caller enters.
            for (std::size_t i = 0; i < cast_words.size(); ++i)
                memory.write32(kCast + static_cast<u32>(i) * 4u, 0);
            remove_metadata();
        }
        if (caller_driver) {
            caller_stack = caller_driver(*this, handler_stack, recovered);
            if (corrupt_return) cpu.lr = kReturn;
        } else if (recovered) {
            recovered_failure_entry(flag, corrupt_return);
        }
        Result result;
        const auto tick = cpu.game_clock().now();
        bool unwinding = false;
        unsigned steps = 0;
        while (cpu.pc != kTerminate && steps++ < 50000) {
            if (cpu.pc == kThrow) {
                ++result.throws;
                unwinding = true;
                require(result.throws == 1 ? cpu.gpr[3] == 0x80514CC8u && cpu.gpr[4] == caller_stack - 24u &&
                                             cpu.gpr[5] == 0x803A1D60u
                                           : cpu.gpr[3] == 0 && cpu.gpr[4] == 0 && cpu.gpr[5] == 0,
                        "original initial throw/rethrow arguments changed");
            }
            if (unwinding) result.unwind_trace.push_back(cpu.pc);
            if (cpu.pc == kCatch) {
                ++result.catches;
                require(caught && result.catches == 1, "unexpected handler selection");
                for (unsigned r = 13; r < 32; ++r) result.caught_gpr[r - 13] = cpu.gpr[r];
                for (unsigned r = 14; r < 32; ++r) for (unsigned lane = 0; lane < 2; ++lane)
                    result.caught_fpr[(r - 14) * 2 + lane] = cpu.fpr[r].bits[lane];
                result.caught_cr = cpu.cr; result.caught_stack = cpu.gpr[1]; result.caught_fpscr = cpu.fpscr;
                for (unsigned i = 0; i < 4; ++i) result.catch_record[i] = memory.peek32(handler_stack + 12 + i * 4);
                require(cpu.gpr[31] == handler_stack && cpu.gpr[2] == kToc,
                        "handler frame or TOC was not recovered");
                require(result.catch_record[0] == caller_stack - 24u && result.catch_record[1] == 0x80514CC8u &&
                        result.catch_record[2] == 0x803A1D60u && result.catch_record[3] == caller_stack - 24u,
                        "handler did not retain the cast exception object");
            }
            if (cpu.pc == kCallback) {
                ++result.callbacks;
                require(cpu.gpr[3] == kOwned && cpu.gpr[4] == 1, "original cleanup callback arguments changed");
            }
            step();
        }
        require(cpu.pc == kTerminate && steps < 50000, "unwind failed to reach unhandled-exception dispatch");
        require(cpu.game_clock().now() == tick, "exception unwinding advanced game time");
        result.terminal = cpu.pc;
        result.payload = memory.peek32(caller_stack - 24u);
        require(result.payload == 0x80568DD4u, "exception payload did not survive cleanup/rethrow");
        result.cleanup_value = memory.peek32(kOwned + 4);
        if (recovered) require_metadata_removed();
        return result;
    }
};

bool equal(const Result& a, const Result& b) {
    return a.unwind_trace == b.unwind_trace && a.caught_gpr == b.caught_gpr && a.caught_fpr == b.caught_fpr &&
           a.catch_record == b.catch_record && a.caught_stack == b.caught_stack && a.caught_cr == b.caught_cr &&
           a.caught_fpscr == b.caught_fpscr && a.terminal == b.terminal && a.payload == b.payload &&
           a.cleanup_value == b.cleanup_value && a.catches == b.catches && a.callbacks == b.callbacks && a.throws == b.throws;
}
}  // namespace

