#pragma once

// The game's address window (docs/memory-architecture-20261006/README.md).
//
// The translated game is 32-bit PowerPC code: the values it keeps in registers
// and in its own structures are 32-bit addresses. This class gives that code a
// flat 32-bit address window onto memory the native (64-bit) memory system
// owns - the same arrangement a 64-bit OS gives a 32-bit process. It is not a
// model of the GameCube's memory: there is no MEM1, no physical address, no
// cached / uncached mirror, no BAT and no real mode. An address means exactly
// one byte or nothing.
//
//   * Regions are the unit of mapping. Each one is a contiguous range of the
//     window backed by its own host reservation (anywhere in the 64-bit host
//     address space), committed on demand. Region kinds: the program image
//     (the code, data and BSS the game was linked at - their addresses are
//     baked into the translated code), the locked-cache scratch (an address
//     the game's code forms itself), and the arenas the native allocator maps
//     for the game's dynamic memory.
//   * The page table (16 KiB pages over the whole window) is the translation:
//     the CPU's inline accessors read it directly. A page without an entry is
//     unmapped, and every access to it fails closed: the CPU stops the machine
//     naming the address and the guest location, a host request is refused.
//   * Host code names guest addresses with GcAddr and reaches bytes only
//     through span()/peek()/read()/write(); nothing masks or offsets a guest
//     address into a host pointer.

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gekko/gc_addr.h"
#include "gekko/types.h"

namespace gekko {

// The locked cache's scratch address, a constant the game's own code forms
// (LCEnable, dcbz_l). It is mapped as a region like any other.
constexpr u32 kLockedCacheBase = 0xe0000000u;
constexpr u32 kLockedCacheSize = 0x00004000u;  // 16 KiB

// A reference-test fixture answering addresses the image's ORIGINAL SDK words
// touch when a test runs them in the interpreter (0xCC00xxxx). Production
// registers none: every GameCube device is native, and a production access to
// such an address stops the machine (stage 6).
class MmioDevice {
public:
    virtual ~MmioDevice() = default;
    virtual const char* name() const = 0;

    virtual u8 read8(u32 address);
    virtual u16 read16(u32 address);
    virtual u32 read32(u32 address);
    virtual void write8(u32 address, u8 value);
    virtual void write16(u32 address, u16 value);
    virtual void write32(u32 address, u32 value);
};

struct GuestMemoryDiagnostics {
    u64 unmapped_reads = 0;
    u64 unmapped_writes = 0;
    u32 first_unmapped_read_address = 0;
    u32 first_unmapped_write_address = 0;
    std::vector<std::string> messages;
};

class GuestMemory {
public:
    // ---- the window --------------------------------------------------------
    static constexpr u32 kPageShift = 14;  // the CPU's inline translation page
    static constexpr u32 kPageSize = 1u << kPageShift;
    static constexpr u32 kPageOffsetMask = kPageSize - 1u;
    static constexpr std::size_t kPageCount = static_cast<std::size_t>(1) << (32 - kPageShift);
    static constexpr u32 kGranule = kPageSize;     // region alignment and commit unit (one page)

    // The program image: OS low memory, the DOL's code / data / BSS and the
    // main thread's stack, at the addresses the game was linked at.
    static constexpr u32 kImageBase = 0x80000000u;
    static constexpr u32 kImageEnd = 0x80610000u;
    // M1 only (README section 4): the game's own OS heap still runs over the
    // range the GameCube OS gave it, mapped here as an ordinary region until
    // M2 hands every allocation to the native allocator and unmaps it.
    static constexpr u32 kLegacyHeapBase = kImageEnd;
    static constexpr u32 kLegacyHeapEnd = 0x81800000u;

    enum class RegionKind : u8 { Image = 1, LockedCache = 2, LegacyHeap = 3, Arena = 4 };
    static const char* kind_name(RegionKind kind) noexcept;
    struct Region {
        u32 base = 0;        // guest address, granule aligned
        u32 size = 0;        // reserved bytes, granule multiple
        u32 committed = 0;   // committed prefix, granule multiple
        RegionKind kind = RegionKind::Arena;
        std::string name;
        u8* host = nullptr;  // the reservation (64-bit host address)
        // FM (2026-10-20): the region's pages are a pagefile-backed section
        // mapped twice: `host` (the native view every span / peek / bulk user
        // reads) and fm_window() + base (the view the CPU's inline accessors
        // address as base + guest effective address). Same pages, so a store
        // through either is seen through both.
        void* section = nullptr;
    };

    GuestMemory();
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    // ---- regions -----------------------------------------------------------
    // Reserves `size` bytes of host memory for the window range [base,
    // base + size) and commits the first `commit` bytes (zero-filled); false
    // on overlap, misalignment, a wrap or a host refusal.
    bool map(u32 base, u32 size, u32 commit, RegionKind kind, std::string name);
    // Commits more of a region (its committed prefix grows to `committed`).
    bool commit(u32 base, u32 committed);
    // Removes the region that starts at `base`; its pages leave the page table
    // first and its host memory is released.
    bool unmap(u32 base);
    const std::vector<Region>& regions() const noexcept { return regions_; }
    // The region holding this address (committed or not), or nullptr.
    const Region* region_at(u32 address) const noexcept;
    u64 reserved_bytes() const noexcept;
    u64 committed_bytes() const noexcept;

    // Savestate (version 17): every region (bounds, kind, name, committed
    // bytes). A load restores the saved regions in place where they exist,
    // maps the ones that do not and unmaps the ones the file does not name.
    void save_state(std::vector<u8>& out) const;
    bool load_state(const u8*& cursor, const u8* end, std::string& why);

    // ---- guest addresses -----------------------------------------------------
    // Bytes of a committed range inside one region, or nullptr. span() is a
    // guest-visible data flow (reported to an installed access trace as a
    // read); peek() is a host-side look nothing records.
    u8* span(GcAddr address, std::size_t size);
    const u8* peek(GcAddr address, std::size_t size) const;
    bool mapped(GcAddr address, std::size_t size) const { return peek(address, size) != nullptr; }
    // A guest-visible bulk write / read (journalled and traced like the CPU's
    // stores). False, with nothing done, when the range is not mapped.
    bool write(GcAddr address, const void* source, std::size_t size);
    bool read(GcAddr address, void* destination, std::size_t size);

    // The SDK's DMA-style contracts - the AX microcode's command lists and
    // parameter-block links, a few transfer APIs - carry OSCachedToPhysical
    // values: the game's own `address - 0x80000000`. This inverts that
    // arithmetic. It is exact because every region of the window except the
    // locked-cache scratch lies in [0x80000000, 0xC0000000) (a placement rule
    // of the native allocator, taken from this dependency of the game's code),
    // and no such contract names the scratch. A value the arithmetic cannot
    // have produced is refused.
    static std::optional<GcAddr> from_os_physical(u32 value) noexcept {
        if (value >= 0x40000000u) return std::nullopt;
        return GcAddr(value | 0x80000000u);
    }
    // The window's addressable range for that convention.
    static constexpr u32 kConventionBase = 0x80000000u;
    static constexpr u32 kConventionEnd = 0xC0000000u;

    // The page table the CPU's inline accessors read: one host pointer per
    // 16 KiB page of the window, nullptr where nothing is committed.
    // FM (2026-10-20), the flat native memory map. fm_window() is a 4 GiB host
    // reservation in which every region's pages appear at window + guest
    // address; everything else in it (holes, uncommitted tails, 0xCC000000,
    // real-mode 0x00000000) is reserved and faults. fm_null_window() is a
    // second 4 GiB reservation with nothing mapped: every access faults. The
    // CPU addresses guest memory as base + effective with base one of the two
    // (Cpu::set_msr); a fault is resolved by Cpu's vectored handler through
    // the same *_slow paths that decide every unserved access today.
    u8* fm_window() const noexcept { return fm_window_; }
    u8* fm_null_window() const noexcept { return fm_null_window_; }
    static constexpr u64 kFmWindowBytes = 0x100000000ull;

    // ---- guest-address accesses (the CPU slow path, instruments, fixtures) ---
    void register_mmio(u32 address, u32 size, MmioDevice* device);
    bool device_at(u32 address) const noexcept { return mmio_for(address) != nullptr; }
    // Host pointer of a committed range inside one region, or nullptr; reported
    // to an installed access trace as a read. peek_pointer is never reported.
    u8* host_pointer(u32 address, std::size_t size);
    const u8* host_pointer(u32 address, std::size_t size) const;
    const u8* peek_pointer(u32 address, std::size_t size) const { return resolve(address, size); }

    u8 read8(u32 address);
    u16 read16(u32 address);
    u32 read32(u32 address);
    u64 read64(u32 address);
    void write8(u32 address, u8 value);
    void write16(u32 address, u16 value);
    void write32(u32 address, u32 value);
    void write64(u32 address, u64 value);
    void read_block(u32 address, void* destination, std::size_t size);
    void write_block(u32 address, const void* source, std::size_t size);
    void clear_block(u32 address, std::size_t size);
    // A bulk write whose bytes a host producer copies straight into guest
    // memory (the native file API's read path). Reported and journalled like
    // write_block; false, with nothing written, when the range is not one
    // committed span.
    template <class Fill>
    bool write_in_place(u32 address, std::size_t size, Fill&& fill) {
        u8* const destination = resolve(address, size);
        if (destination == nullptr) return false;
        if (access_trace_) trace_access(address, size, true);
        if (journaling_) {
            for (std::size_t index = 0; index < size; ++index) journal_note(address + static_cast<u32>(index), 1u);
        }
        fill(destination);
        return true;
    }
    u8 peek8(u32 address) const;
    u16 peek16(u32 address) const;
    u32 peek32(u32 address) const;
    // Bulk host-side reads may cross region boundaries: every committed run is
    // copied, every other byte reads as zero.
    void peek_block(u32 address, void* destination, std::size_t size) const;
    // A copy of the window range [begin, end) (host-side; unmapped bytes 0).
    std::vector<u8> window_bytes(u32 begin, u32 end) const;

    GuestMemoryDiagnostics& diagnostics() noexcept { return diagnostics_; }
    const GuestMemoryDiagnostics& diagnostics() const noexcept { return diagnostics_; }
    void set_trace_unmapped(bool enabled) noexcept { trace_unmapped_ = enabled; }

    // ---- access trace (verification instrument) -------------------------------
    // Observation only: reports, in program order, every committed extent a
    // guest data access reaching a CPU slow path, a read / write here, or a
    // host_pointer / span call touches. While a trace is installed the CPU
    // serves no access from the page table. The shipping executable never
    // installs one.
    using AccessTrace = std::function<void(u32 address, std::size_t size, bool writing)>;
    void set_access_trace(AccessTrace trace);
    bool access_traced() const noexcept { return static_cast<bool>(access_trace_); }
    void trace_access(u32 address, std::size_t size, bool writing);

    // ---- translation listeners ----------------------------------------------
    // The CPU serves accesses from the page table only while nothing needs to
    // see them: journalling and the access trace make it take the slow path.
    // `change` is raised when either starts or stops. One registration per
    // owner; every listener is notified in registration order.
    using TranslationChange = std::function<void()>;
    void add_translation_listener(const void* owner, TranslationChange change);
    void remove_translation_listener(const void* owner) noexcept;
    std::size_t translation_listener_count() const noexcept { return translation_listeners_.size(); }
    u64 translation_invalidations() const noexcept { return translation_invalidations_; }

    // ---- write journal (the --verify-recomp oracle) and the poll watch -------
    struct JournalEntry {
        u32 address;
        u32 size;
        u64 previous;
    };
    void journal_begin();
    void journal_end() noexcept {
        journaling_ = false;
        notify_translation_change();
    }
    void journal_rollback();
    const std::vector<JournalEntry>& journal() const noexcept { return journal_; }
    std::size_t journal_capacity() const noexcept { return journal_.capacity(); }
    bool journal_touched_device() const noexcept { return journal_device_; }
    void journal_note_host_effect() noexcept {
        if (journaling_) journal_device_ = true;
    }
    bool journal_overflowed() const noexcept { return journal_overflow_; }
    static constexpr std::size_t kJournalLimit = 1u << 20;
    bool journaling() const noexcept { return journaling_; }
    void watch_begin(std::size_t limit);
    void watch_end() noexcept {
        if (watch_limit_ != 0u) {
            watch_limit_ = 0u;
            journal_end();
        }
    }
    bool watch_active() const noexcept { return watch_limit_ != 0u && journaling_; }
    bool watch_unchanged() const;
    u64 watch_aborts() const noexcept { return watch_aborts_; }

private:
    u8* resolve(u32 address, std::size_t size) const noexcept;
    void notify_translation_change() const;
    void journal_note(u32 address, u32 size);
    MmioDevice* mmio_for(u32 address) const;
    void note_unmapped_read(u32 address, u32 size);
    void note_unmapped_write(u32 address, u32 size, u64 value);

    u8* fm_window_ = nullptr;
    u8* fm_null_window_ = nullptr;
    std::vector<Region> regions_;
    static constexpr u32 kFixtureBase = 0xcc000000u;
    static constexpr u32 kFixtureSize = 0x00010000u;
    std::vector<MmioDevice*> fixture_pages_;
    GuestMemoryDiagnostics diagnostics_;
    bool trace_unmapped_ = true;
    bool journaling_ = false;
    bool journal_device_ = false;
    bool journal_overflow_ = false;
    std::size_t watch_limit_ = 0;
    u64 watch_aborts_ = 0;
    std::vector<JournalEntry> journal_;
    mutable u64 translation_invalidations_ = 0;
    std::vector<std::pair<const void*, TranslationChange>> translation_listeners_;
    AccessTrace access_trace_;
};

}  // namespace gekko
