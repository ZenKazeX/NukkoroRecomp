#pragma once

// The memory card: a BLOCK-DEVICE MODEL of the GameCube memory card, not a native save system (NATIVE_AUDIT.md C1,
// lies 1-2; ES comment fix 2026-10-20). It keeps the card's contract - 8 KiB erase sectors, 512-byte read segments,
// 128-byte program pages, vendor ID 0xC221, the five system blocks, one completion per channel delivered through the
// card interrupt - cut at the CARD library's lowest functions, so that the translated Nintendo CARD file system runs
// unchanged on top of it. The EXI library and bus are gone; the card chip's contract is not. To be replaced at the
// CARD API boundary by host save files (NE-SAVE).
//
// docs/native-time-20261005/STAGE5_EXI.md. On the console the CARD library
// spoke the card chip's protocol over EXI channel 0: read a 512-byte segment,
// program a 128-byte page, erase an 8 KiB sector, read / clear the status,
// enable the chip's interrupt, the unlock handshake, the ID probe; every
// transfer ended in an EXI interrupt whose handler ran the library's callback
// chain. Here the card is a raw image in host memory and six CARD functions are
// native owners (extract/native-owners.txt):
//
//   game save module / CARD file system (translated guest code: directory,
//   allocation table, checksums, open / create / read / write / delete /
//   status / format / __CARDVerify)
//      |  __CARDRead, __CARDWrite, __CARDEraseSector  (the block device)
//      |  CARDProbeEx, CARDMountAsync, CARDUnmount     (identity, mount)
//   NativeCard: the image (host file policy below), the card's identity, one
//      |        completion per channel
//   NativeInterrupts (Source::Card): the completion in interrupt context
//
// With those owned, DoMount, __CARDMountCallback, DoUnmount, the unlock
// handshake, __CARDStart, the segment / page / erase / status / interrupt
// commands, Retry and the EXI / TX / EXT / unlocked / timeout handlers have no
// caller left and the generator prunes them, and with them every CARD call
// into the EXI library.
//
// THE CONTRACT (what the game observes; STAGE5_EXI.md sections 3-4)
//  * The card: slot A holds the attached image from power-on (slot B is
//    empty); its identity is the one the emulated card presented - ID word =
//    image bytes / 128 KiB (0x80 for 16 MiB), 8 KiB sectors, 128-byte pages,
//    vendor 0xC221, always unlocked - so CARDControl holds what it held
//    (size, pageSize, sectorSize, cBlock, vendorID, latency from the game's
//    own LatencyTable, cid). No insertion settle, no removal, no chip status.
//  * A read moves the bytes at once into the buffer (whole 512-byte segments,
//    at least one), a write stores them (whole pages, at least one), an erase
//    fills the 8 KiB sector with 0xFF; each leaves the transfer fields
//    (repeat, addr, buffer, xferred) and the callback fields where the
//    original's last segment / page left them, and posts its completion. The
//    chip command block (cmd, cmdlen, mode, retry) is not written: only the
//    pruned chip-level code read it.
//  * The completion runs in interrupt context the original's tail:
//      read   (BlockReadCallback's end): if no API callback, put the result in
//             the control block; take, clear and call the transfer callback;
//      write  (__CARDExiHandler, BlockWriteCallback's end): nothing when the
//             card is no longer attached; else the same as a read;
//      erase  (__CARDExiHandler): nothing when no longer attached; else take,
//             clear and call the EXI callback;
//      mount  (the five system-block reads' __CARDMountCallback): mountStep 7,
//             __CARDVerify(card), take and clear the API callback, put the
//             result, call it.
//    One completion per channel is in flight at a time (the library's BUSY
//    result serialises them); a second one stops the machine.
//  * CARDMountAsync runs DoMount's steps 0-1 at once (identity, the unlocked
//    path's SRAM flash-ID checksum through the OS SRAM block, the interrupt
//    enable) and reads the five system blocks (steps 2-6) into the work area;
//    its errors take DoUnmount's path. CARDProbeEx and CARDUnmount are the
//    originals over a card that is present and never removed.
//
// THE HOST FILE POLICY (the emulator's raw-card-image policy, kept as it was: the host file is a whole card image,
// not a save file)
//  * A .gci source is imported into a raw base card ("Memory Card A.raw"
//    beside it) when that base records an allocation for the save, else into
//    a freshly formatted 4-Mbit card (the GCI header as the directory entry,
//    one block chain from the first user block), and kept in a per-run
//    working image (default <source stem>.pc.raw beside the source, or the
//    caller's path) with a 32-byte <working>.source marker binding it to that
//    GCI; a working image that fails validation is renamed to
//    <working>.unusable[.N] (it may hold progress newer than the GCI) and
//    rebuilt. A .raw source is used in place; one that does not exist is
//    created as an empty, formatted 4-Mbit card. Both must be valid raw
//    cards (a power-of-two size IsCard accepts, 512 KiB to 16 MiB; header
//    checksum; a checksum-valid directory and allocation table).
//  * Writes and erases mark the image dirty; the whole image is written
//    durably once, after persist_quiet_ms of GAME time without a card write
//    (tick), or at flush() / set_persist_debounce(false) / destruction /
//    the next set_path. A failed write is counted, retried with a game-time
//    backoff, and never reaches the guest (a real card's failed program does
//    not either).
//
// State (savestate version 15): the pending completion of each channel. The
// image is the host file, flushed before a savestate is written.

#include <array>
#include <cstddef>
#include <filesystem>
#include <vector>

#include "gekko/types.h"

namespace gekko {

class GuestMemory;
class Cpu;
class NativeInterrupts;

class NativeCard final {
public:
    NativeCard(Cpu& cpu, GuestMemory& memory, NativeInterrupts& interrupts) noexcept;
    // Commits what the persistence debounce still holds.
    ~NativeCard();
    NativeCard(const NativeCard&) = delete;
    NativeCard& operator=(const NativeCard&) = delete;

    // Installs the six owners and the completion delivery. Before the guest boots.
    void install();

    // ---- the host store ------------------------------------------------------
    // Detaches the current card (committing its un-persisted bytes) and
    // attaches `path` to slot A as described above (creating a missing .raw);
    // an empty path leaves the slot empty. Throws std::runtime_error when the
    // card cannot be used.
    void set_path(std::filesystem::path path, std::filesystem::path gci_working_path = {});
    bool present() const noexcept { return present_; }
    // The source as given (.gci or .raw), and the file the image persists to.
    const std::filesystem::path& source_path() const noexcept { return source_path_; }
    const std::filesystem::path& backing_path() const noexcept { return backing_path_; }
    const std::vector<u8>& image() const noexcept { return image_; }

    // ---- the persistence debounce (host file policy) --------------------------
    // Game-clock advance in 486 MHz cycles (System::move_clock_to).
    void tick(u64 cycles);
    // Cycles until a deferred persist is due; ~0 when none is pending.
    u64 cycles_until_persist() const noexcept;
    // ON (default): one durable write per quiet interval. OFF: a durable write
    // after every card write or erase.
    void set_persist_debounce(bool enabled) noexcept;
    bool persist_debounce() const noexcept { return persist_debounce_; }
    void set_persist_quiet_ms(u32 milliseconds) noexcept;
    // Durable write of any un-persisted bytes, now (savestate save, detach).
    void flush();
    bool dirty() const noexcept { return dirty_; }

    struct Stats {
        u64 probes = 0;               // CARDProbeEx calls
        u64 mounts = 0;               // CARDMountAsync calls that started a mount
        u64 mount_failures = 0;       // mounts that ended in DoUnmount's path at once
        u64 unmounts = 0;             // CARDUnmount calls that detached the card
        u64 reads = 0;                // __CARDRead calls (and the mount's system-block reads)
        u64 read_bytes = 0;
        u64 writes = 0;               // __CARDWrite calls
        u64 written_bytes = 0;
        u64 overwritten_bytes = 0;    // written over bytes that were not erased (0xFF)
        u64 erases = 0;               // __CARDEraseSector calls (8 KiB each)
        u64 completions = 0;          // completions delivered
        u64 callbacks = 0;            // guest calls the deliveries made
        u64 dropped_detached = 0;     // write / erase completions after a detach
        // host persistence
        u64 persists = 0;             // durable whole-image writes
        u64 persist_bytes = 0;
        u64 persist_ns = 0;
        u64 persist_ns_max = 0;
        u64 persists_deferred = 0;    // card writes the debounce absorbed into a later persist
        u64 persist_failures = 0;
    };
    const Stats& stats() const noexcept { return stats_; }
    // False: tick() never ran, so the debounce is inert (immediate persists).
    bool persist_tick_wired() const noexcept { return tick_seen_; }

    // Every guest address the generator must route to an owner here
    // (extract/native-owners.txt).
    struct Address {
        u32 address;
        const char* name;
        bool continuation;
    };
    static const Address* addresses(std::size_t& count) noexcept;

    // Savestate (gekko/savestate.h, version 15): each channel's pending
    // completion. A savestate is taken after flush(): the image is the file.
    void save_state(std::vector<u8>& out) const;
    bool load_state(const u8*& cursor, const u8* end);
    void reset() noexcept;

private:
    enum class Completion : u8 { None = 0, Read, Write, Erase, Mount };
    struct Pending {
        Completion kind = Completion::None;
        s32 result = 0;
    };
    static constexpr std::size_t kChannels = 2;

    // The card's identity (D3) and the image operations.
    bool slot_present(u32 chan) const noexcept { return chan == 0u && present_; }
    u32 card_id() const noexcept;
    bool image_range(u32 address, u64 bytes) const noexcept;
    void read_image(Cpu& c, u32 address, u32 bytes, u32 buffer);
    void write_image(Cpu& c, u32 address, u32 bytes, u32 buffer);
    void erase_image(Cpu& c, u32 address);
    void mark_dirty() noexcept;
    void schedule_persist();
    void persist();

    // The owners and the original procedures they keep.
    void card_read(Cpu& c);
    void card_write(Cpu& c);
    void card_erase(Cpu& c);
    void probe(Cpu& c);
    void mount(Cpu& c);
    void unmount(Cpu& c);
    s32 do_mount(Cpu& c, u32 chan);
    void do_unmount(Cpu& c, u32 chan, s32 result);
    void post(Cpu& c, u32 chan, Completion kind, s32 result);
    bool completion_handler(Cpu& c, u32 frame, u32 step);

    Cpu& cpu_;
    GuestMemory& memory_;
    NativeInterrupts& interrupts_;
    std::array<Pending, kChannels> pending_{};
    Stats stats_;

    std::filesystem::path source_path_;
    std::filesystem::path backing_path_;
    std::vector<u8> image_;
    bool present_ = false;
    bool dirty_ = false;
    bool persist_debounce_ = true;
    bool tick_seen_ = false;
    static constexpr u64 kCoreClockHz = 486000000ull;
    u64 persist_quiet_cycles_ = kCoreClockHz / 4u;  // 250 ms of game time
    u64 quiet_cycles_ = 0;                           // game cycles since the last card write
    u32 retry_shift_ = 0;                            // failed persists back off: quiet << shift
};

}  // namespace gekko
