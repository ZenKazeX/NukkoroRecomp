// FM (EV 2026-10-20): the flat native memory map's fault path - the vectored
// exception handler that resolves a guest access the flat window did not serve
// (EV_REPORT 17). Its own translation unit so <windows.h> stays out of cpu.cpp.
#include "gekko/cpu.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdlib.h>
#endif

namespace gekko {

// ---------------------------------------------------------------------------
// FM: the flat native memory map's fault path (EV 2026-10-20, EV_REPORT 17).
//
// The inline accessors address guest memory as fm_base_ + effective. An access
// the old page-table fast half would have declined faults instead: an address
// outside every region or past a region's committed prefix (the window keeps
// those reserved), or any access while fm_base_ is the null window (MSR[DR] off
// or user mode; the journal, the poll watch, an access trace or a latched data
// fault: select_memory_window). This handler decides each such access with the
// SAME *_slow body that decided it before, then completes the instruction the
// way the old accessor returned: a read's destination register receives the
// value (in the register form the instruction defines), a store has been
// performed or suppressed by *_slow, and execution resumes after the
// instruction. The x64 forms are the closed set the volatile accessor
// primitives compile to; anything else is refused loudly (never guessed).
// ---------------------------------------------------------------------------
namespace {
#if defined(_WIN32)
constexpr int kFmMachines = 16;
std::atomic<Cpu*> g_fm_machines[kFmMachines];
std::atomic<int> g_fm_registered{0};
PVOID g_fm_handler = nullptr;
std::atomic<u64> g_fm_faults{0};
// Handler entries by class (integrator 15:1x): the null window (a mode: MSR[DR]
// off / user mode / journal / poll watch / access trace / latched fault), the
// GameCube device-register window 0xCC000000-0xCDFFFFFF, an access split across
// a page boundary into a page the window does not serve, anything else unmapped.
std::atomic<u64> g_fm_null{0}, g_fm_mmio{0}, g_fm_split{0}, g_fm_unmapped{0};
#if !defined(GEKKO_SHIPPING)
// The first distinct faulting instructions (image offsets; dev builds only).
constexpr int kFmSites = 8;
std::atomic<u64> g_fm_sites[kFmSites];
#endif

void fm_report_at_exit() {
    const u64 total = g_fm_faults.load();
    std::printf("[fm] flat-map handler entries: %llu (null window %llu, device window %llu, page split %llu, "
                "unmapped %llu)\n",
                static_cast<unsigned long long>(total), static_cast<unsigned long long>(g_fm_null.load()),
                static_cast<unsigned long long>(g_fm_mmio.load()), static_cast<unsigned long long>(g_fm_split.load()),
                static_cast<unsigned long long>(g_fm_unmapped.load()));
#if !defined(GEKKO_SHIPPING)
    for (auto& site : g_fm_sites) {
        if (const u64 rva = site.load()) std::printf("[fm]   faulting instruction at image offset 0x%llX\n",
                                                     static_cast<unsigned long long>(rva));
    }
#endif
    std::fflush(stdout);
}

LONG CALLBACK fm_vectored_entry(EXCEPTION_POINTERS* info) {
    return static_cast<LONG>(Cpu::fm_fault(info));
}

u64* fm_gpr(CONTEXT& context, unsigned index) {
    switch (index) {
        case 0: return &context.Rax;
        case 1: return &context.Rcx;
        case 2: return &context.Rdx;
        case 3: return &context.Rbx;
        case 4: return &context.Rsp;
        case 5: return &context.Rbp;
        case 6: return &context.Rsi;
        case 7: return &context.Rdi;
        case 8: return &context.R8;
        case 9: return &context.R9;
        case 10: return &context.R10;
        case 11: return &context.R11;
        case 12: return &context.R12;
        case 13: return &context.R13;
        case 14: return &context.R14;
        default: return &context.R15;
    }
}

// One decoded guest access.
struct FmAccess {
    bool ok = false;
    bool write = false;
    unsigned width = 0;      // bytes: 1, 2, 4, 8
    unsigned length = 0;     // instruction bytes
    u64 address = 0;         // host address of the operand
    enum class Form { Mov, Movbe, Movzx, Movsx, MovImm } form = Form::Mov;
    unsigned reg = 0;        // ModRM.reg (with REX.R)
    bool rex = false;
    bool dest64 = false;     // REX.W / the destination's operand size is 64
    bool dest16 = false;     // 0x66: a 16-bit destination (upper bits kept)
    u64 immediate = 0;
};

FmAccess fm_decode(const u8* ip, CONTEXT& context) {
    FmAccess a;
    unsigned at = 0;
    bool opsize16 = false;
    unsigned rex = 0;
    for (;;) {
        const u8 b = ip[at];
        if (b == 0x66) { opsize16 = true; ++at; continue; }
        if (b >= 0x40 && b <= 0x4F) { rex = b; ++at; break; }
        break;
    }
    a.rex = rex != 0;
    const bool rex_w = (rex & 8u) != 0;
    u8 op = ip[at++];
    unsigned op2 = 0;
    if (op == 0x0F) {
        op2 = ip[at++];
        if (op2 == 0x38) {
            const u8 op3 = ip[at++];
            if (op3 != 0xF0 && op3 != 0xF1) return a;
            a.form = FmAccess::Form::Movbe;
            a.write = op3 == 0xF1;
            a.width = rex_w ? 8 : opsize16 ? 2 : 4;
        } else if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) {
            a.form = (op2 == 0xB6 || op2 == 0xB7) ? FmAccess::Form::Movzx : FmAccess::Form::Movsx;
            a.width = (op2 == 0xB6 || op2 == 0xBE) ? 1 : 2;
        } else {
            return a;
        }
    } else if (op == 0x88 || op == 0x8A) {
        a.form = FmAccess::Form::Mov;
        a.write = op == 0x88;
        a.width = 1;
    } else if (op == 0x89 || op == 0x8B) {
        a.form = FmAccess::Form::Mov;
        a.write = op == 0x89;
        a.width = rex_w ? 8 : opsize16 ? 2 : 4;
    } else if (op == 0xC6 || op == 0xC7) {
        a.form = FmAccess::Form::MovImm;
        a.write = true;
        a.width = op == 0xC6 ? 1 : rex_w ? 8 : opsize16 ? 2 : 4;
    } else {
        return a;
    }
    a.dest64 = rex_w;
    a.dest16 = opsize16;
    const u8 modrm = ip[at++];
    const unsigned mod = modrm >> 6;
    a.reg = ((modrm >> 3) & 7u) | ((rex & 4u) ? 8u : 0u);
    unsigned rm = modrm & 7u;
    if (mod == 3) return a;  // a register operand: not a memory access
    u64 address = 0;
    if (rm == 4) {
        const u8 sib = ip[at++];
        const unsigned scale = 1u << (sib >> 6);
        const unsigned index = ((sib >> 3) & 7u) | ((rex & 2u) ? 8u : 0u);
        const unsigned base = (sib & 7u) | ((rex & 1u) ? 8u : 0u);
        if (index != 4) address += *fm_gpr(context, index) * scale;
        if ((sib & 7u) == 5 && mod == 0) {
            address += static_cast<u64>(static_cast<std::int64_t>(*reinterpret_cast<const std::int32_t*>(ip + at)));
            at += 4;
        } else {
            address += *fm_gpr(context, base);
        }
    } else if (rm == 5 && mod == 0) {
        return a;  // RIP-relative: host data, never a guest access
    } else {
        address += *fm_gpr(context, rm | ((rex & 1u) ? 8u : 0u));
    }
    if (mod == 1) {
        address += static_cast<u64>(static_cast<std::int64_t>(static_cast<std::int8_t>(ip[at])));
        at += 1;
    } else if (mod == 2) {
        address += static_cast<u64>(static_cast<std::int64_t>(*reinterpret_cast<const std::int32_t*>(ip + at)));
        at += 4;
    }
    if (a.form == FmAccess::Form::MovImm) {
        if (a.width == 1) {
            a.immediate = ip[at];
            at += 1;
        } else if (a.width == 2) {
            a.immediate = *reinterpret_cast<const u16*>(ip + at);
            at += 2;
        } else {
            a.immediate = static_cast<u64>(static_cast<std::int64_t>(*reinterpret_cast<const std::int32_t*>(ip + at)));
            at += 4;
        }
    }
    a.address = address;
    a.length = at;
    a.ok = true;
    return a;
}

u64 fm_swap(u64 v, unsigned width) {
    switch (width) {
        case 2: return _byteswap_ushort(static_cast<u16>(v));
        case 4: return _byteswap_ulong(static_cast<u32>(v));
        case 8: return _byteswap_uint64(v);
        default: return v & 0xFFu;
    }
}

// Writes `value` (already in the instruction's register form) into register
// `reg` as an instruction of that destination size does.
void fm_set_register(CONTEXT& context, unsigned reg, bool rex, unsigned bits, u64 value) {
    if (bits == 8 && !rex && reg >= 4 && reg < 8) {  // AH, CH, DH, BH
        u64& r = *fm_gpr(context, reg - 4);
        r = (r & ~0xFF00ull) | ((value & 0xFFull) << 8);
        return;
    }
    u64& r = *fm_gpr(context, reg);
    switch (bits) {
        case 8: r = (r & ~0xFFull) | (value & 0xFFull); break;
        case 16: r = (r & ~0xFFFFull) | (value & 0xFFFFull); break;
        case 32: r = value & 0xFFFFFFFFull; break;  // a 32-bit destination zero-extends
        default: r = value; break;
    }
}

u64 fm_get_register(CONTEXT& context, unsigned reg, bool rex, unsigned width) {
    if (width == 1 && !rex && reg >= 4 && reg < 8) return (*fm_gpr(context, reg - 4) >> 8) & 0xFFu;
    const u64 r = *fm_gpr(context, reg);
    return width == 8 ? r : r & ((1ull << (8 * width)) - 1u);
}
#endif
}  // namespace

void Cpu::fm_register(Cpu* cpu) noexcept {
#if defined(_WIN32)
    for (auto& slot : g_fm_machines) {
        Cpu* expected = nullptr;
        if (slot.compare_exchange_strong(expected, cpu)) break;
    }
    if (g_fm_registered.fetch_add(1) == 0 && g_fm_handler == nullptr) {
        g_fm_handler = AddVectoredExceptionHandler(1, &fm_vectored_entry);
        std::atexit(&fm_report_at_exit);
    }
#else
    (void)cpu;
#endif
}

void Cpu::fm_unregister(Cpu* cpu) noexcept {
#if defined(_WIN32)
    for (auto& slot : g_fm_machines) {
        Cpu* expected = cpu;
        if (slot.compare_exchange_strong(expected, nullptr)) break;
    }
    g_fm_registered.fetch_sub(1);
#else
    (void)cpu;
#endif
}

u64 Cpu::fm_fault_count() noexcept {
#if defined(_WIN32)
    return g_fm_faults.load();
#else
    return 0;
#endif
}

long Cpu::fm_fault(void* exception_pointers) noexcept {
#if defined(_WIN32)
    EXCEPTION_POINTERS* const info = static_cast<EXCEPTION_POINTERS*>(exception_pointers);
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        info->ExceptionRecord->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const u64 fault = static_cast<u64>(info->ExceptionRecord->ExceptionInformation[1]);
    Cpu* owner = nullptr;
    u8* window = nullptr;
    for (auto& slot : g_fm_machines) {
        Cpu* const cpu = slot.load(std::memory_order_acquire);
        if (cpu == nullptr) continue;
        for (u8* w : {cpu->fm_window_, cpu->fm_null_window_}) {
            const u64 base = reinterpret_cast<u64>(w);
            if (fault >= base && fault - base < GuestMemory::kFmWindowBytes) {
                owner = cpu;
                window = w;
            }
        }
        if (owner != nullptr) break;
    }
    if (owner == nullptr) return EXCEPTION_CONTINUE_SEARCH;  // not a guest window: a host fault
    CONTEXT& context = *info->ContextRecord;
    const FmAccess a = fm_decode(reinterpret_cast<const u8*>(context.Rip), context);
    const u64 base = reinterpret_cast<u64>(window);
    if (!a.ok || a.address < base || a.address - base >= GuestMemory::kFmWindowBytes ||
        a.address - base + a.width > GuestMemory::kFmWindowBytes) {
        std::fprintf(stderr,
                     "[fm] access violation at 0x%016llX in a guest window from an instruction the flat-map "
                     "handler does not decode (rip 0x%016llX): refusing to guess\n",
                     static_cast<unsigned long long>(fault), static_cast<unsigned long long>(context.Rip));
        std::fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_fm_faults.fetch_add(1, std::memory_order_relaxed);
    const u32 effective = static_cast<u32>(a.address - base);
    if (window == owner->fm_null_window_) {
        g_fm_null.fetch_add(1, std::memory_order_relaxed);
    } else if (effective >= 0xCC000000u && effective < 0xCE000000u) {
        g_fm_mmio.fetch_add(1, std::memory_order_relaxed);
    } else if ((a.address & 0xFFFu) + a.width > 0x1000u) {
        g_fm_split.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_fm_unmapped.fetch_add(1, std::memory_order_relaxed);
    }
#if !defined(GEKKO_SHIPPING)
    {
        const u64 rva = static_cast<u64>(context.Rip) - reinterpret_cast<u64>(GetModuleHandleW(nullptr));
        for (auto& site : g_fm_sites) {
            u64 seen = site.load(std::memory_order_relaxed);
            if (seen == rva) break;
            if (seen == 0 && site.compare_exchange_strong(seen, rva)) break;
            if (seen == rva) break;
        }
    }
#endif
    if (a.write) {
        // The guest value the accessor was storing: MOVBE stores the register
        // big-endian itself; MOV / MOV imm store the already-swapped bytes.
        u64 raw = a.form == FmAccess::Form::MovImm ? a.immediate : fm_get_register(context, a.reg, a.rex, a.width);
        const u64 value = a.form == FmAccess::Form::Movbe ? raw : fm_swap(raw, a.width);
        // The unlisted-site report names the generated code that stored (the
        // *_cold halves recorded their return address; here it is the
        // instruction after the store).
        owner->cold_store_return_ = reinterpret_cast<const void*>(context.Rip + a.length);
        switch (a.width) {
            case 1: owner->mem_write8_slow(effective, static_cast<u8>(value)); break;
            case 2: owner->mem_write16_slow(effective, static_cast<u16>(value)); break;
            case 4: owner->mem_write32_slow(effective, static_cast<u32>(value)); break;
            default: owner->mem_write64_slow(effective, value); break;
        }
    } else {
        u64 value = 0;
        switch (a.width) {
            case 1: value = owner->mem_read8_slow(effective); break;
            case 2: value = owner->mem_read16_slow(effective); break;
            case 4: value = owner->mem_read32_slow(effective); break;
            default: value = owner->mem_read64_slow(effective); break;
        }
        // The register receives what the instruction would have loaded from
        // memory holding the guest's big-endian bytes.
        switch (a.form) {
            case FmAccess::Form::Movbe:
                fm_set_register(context, a.reg, a.rex, a.dest64 ? 64 : a.dest16 ? 16 : 32, value);
                break;
            case FmAccess::Form::Movzx:
                fm_set_register(context, a.reg, a.rex, a.dest64 ? 64 : a.dest16 ? 16 : 32,
                                a.width == 1 ? value : fm_swap(value, 2));
                break;
            case FmAccess::Form::Movsx: {
                const u64 raw = a.width == 1 ? value : fm_swap(value, 2);
                const u64 extended = a.width == 1
                                         ? static_cast<u64>(static_cast<std::int64_t>(static_cast<std::int8_t>(raw)))
                                         : static_cast<u64>(static_cast<std::int64_t>(static_cast<std::int16_t>(raw)));
                fm_set_register(context, a.reg, a.rex, a.dest64 ? 64 : a.dest16 ? 16 : 32, extended);
                break;
            }
            default:  // Mov
                fm_set_register(context, a.reg, a.rex, a.width * 8, fm_swap(value, a.width));
                break;
        }
    }
    context.Rip += a.length;
    return EXCEPTION_CONTINUE_EXECUTION;
#else
    (void)exception_pointers;
    return 0;
#endif
}

}  // namespace gekko
