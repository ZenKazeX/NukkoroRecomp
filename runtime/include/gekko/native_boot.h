#pragma once

// The program's start-up, native: what the GameCube C runtime's entry point did before the game's main() ran.
//
// docs/agents/native-boot.md has the disassembly and the evidence for every statement here. The original start-up
// (`__start`, 0x80003154) was, in order:
//
//   __init_registers 0x800032B0   r1 / r2 / r13 (the stack and the two small-data bases), every other register 0
//   __init_hardware 0x80003400    MSR[FP], the paired-single unit (HID2, GQR0-7, the 32 FPRs), the caches
//   __init_data 0x80003340        ROM-to-RAM copies of ten sections (source = destination here: none moves) and the
//                                 clearing of three BSS ranges (already zero in the loaded image)
//   the boot information          the debug-monitor kind (BI2 + 0xC: InitMetroTRK / InitMetroTRK_BBA when 2, 3 or 4)
//                                 and the arguments (BI2 + 8) the loader passed
//   DBInit 0x80370BF0             __DBInterface = 0x80000040, its hook word, the verbosity flag
//   OSInit 0x80370E68             the operating system's start-up
//   the reset-flag check, the constructors (0x803796CC), main(argc, argv) 0x800510C0, then exit() 0x803A3B04
//
// Here `__start` and OSInit are native owners. `__start` sets the state the rest of the program reads, runs OSInit
// natively (os_init: the OS's globals, the exception vectors and table, the default thread and context, the arena, the
// interrupt mask, MSR / FPSCR / HID0; the services that are already native owners - __OSInterruptInit, EXIInit,
// __OSInitSram, __OSInitAudioSystem - are served in the original order), then runs the guest functions that remain in
// the original order with the original link register: the constructors, main. The game calls OSInit again (0x80051028):
// the owner returns at once, as the original did. What is not done is retired, each with the reader proof in the
// report: the debug monitor (refused, as InitMetroTRK already is: the disc names none), the argument parsing (refused
// when the loader passes any), the ROM copies and BSS clears (no-ops on this image), the hardware setup the CPU's reset
// state already is (HID0's ICFI and SPD bits, FPSCR[NI], MSR are set natively), DBInit's hook word and flag, the
// console banner prints (and the libc state they touch), the version lists, the debugger stub, the memory-interface
// setup and the error-handler registration (no reader).
//
// The return points of the original __start that remain are owner entries, so a callee that reads its link register
// sees the original values: 0x800032A0 (the constructors returned), 0x800032AC (main returned: the original
// tail-called exit()).

#include <cstddef>

#include "gekko/types.h"

namespace gekko {

class Cpu;
class NativeInterrupts;

class NativeBoot final {
public:
    explicit NativeBoot(Cpu& cpu) noexcept : cpu_(cpu) {}

    // Installs the owners. Before the guest boots.
    void install();

    // Every guest address the generator must route to an owner here (extract/native-owners.txt).
    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept;

    // Reference runner only (ShadowTheHedgehogReference --original-boot): every owner here declines, so the interpreter
    // runs the original __start and the original OSInit. The contract tests compare that boot with this one.
    void use_original_boot() noexcept;
    // The interrupt system whose mask OSInit's __OSInitMemoryProtection changed (MEM_ADDRESS unmasked).
    void bind_interrupts(NativeInterrupts& interrupts) noexcept { interrupts_ = &interrupts; }
    bool original_boot() const noexcept { return original_; }

    enum class Phase : u8 { Idle, OsInit, Constructors, Main, Exit };
    Phase phase() const noexcept { return phase_; }

private:
    void start(Cpu& c);
    void os_init(Cpu& c);
    void after_os_init(Cpu& c);
    void after_constructors(Cpu& c);
    // The constructor table walk (the SDK's __init_cpp, 0x803796CC): native, calling the same functions in the same order with the
    // original link register (the return point 0x803796FC is an owner entry).
    void constructors_begin(Cpu& c);
    void constructors_next(Cpu& c);
    void after_constructor(Cpu& c);
    void after_main(Cpu& c);

    Cpu& cpu_;
    NativeInterrupts* interrupts_ = nullptr;
    Phase phase_ = Phase::Idle;
    bool original_ = false;
};

}  // namespace gekko
