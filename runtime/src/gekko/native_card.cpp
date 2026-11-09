// The memory card, native. Contract and rationale: include/gekko/native_card.h
// and docs/native-time-20261005/STAGE5_EXI.md. Guest addresses are from the
// disassembly cited there (python -B tools/doldis.py <address>:<count>); the
// CARDControl layout is section 3's.

#include "gekko/native_card.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>

#include "gekko/cpu.h"
#include "gekko/host_files.h"
#include "gekko/guest_memory.h"
#include "gekko/native_interrupts.h"
#include "gekko/savestate.h"

namespace gekko {

namespace {

// ---- the entry points the generator routes here (extract/native-owners.txt)
constexpr u32 kCardEraseSector = 0x8038DD2Cu;  // __CARDEraseSector(chan, addr, callback)
constexpr u32 kCardRead = 0x8038F56Cu;         // __CARDRead(chan, addr, length, buffer, callback)
constexpr u32 kCardWrite = 0x8038F6B8u;        // __CARDWrite(chan, addr, length, buffer, callback)
constexpr u32 kCardProbeEx = 0x80390D90u;      // CARDProbeEx(chan, memSize *, sectorSize *)
constexpr u32 kCardMountAsync = 0x80391498u;   // CARDMountAsync(chan, workArea, detach, attach)
constexpr u32 kCardUnmount = 0x803916D4u;      // CARDUnmount(chan)

// ---- guest code the deliveries call

const NativeCard::Address kAddresses[] = {
    {kCardEraseSector, "__CARDEraseSector", false},
    {kCardRead, "__CARDRead", false},
    {kCardWrite, "__CARDWrite", false},
    {kCardProbeEx, "CARDProbeEx", false},
    {kCardMountAsync, "CARDMountAsync", false},
    {kCardUnmount, "CARDUnmount", false},
};

// ---- CARDControl (__CARDBlock[2], STAGE5_EXI.md section 3)
constexpr u32 kCardBlock = 0x805A2CA0u;
constexpr u32 kCardBlockStride = 0x110u;
constexpr u32 kAttached = 0x00u;
constexpr u32 kResult = 0x04u;
constexpr u32 kSize = 0x08u;           // u16, megabits
constexpr u32 kPageSize = 0x0Au;       // u16
constexpr u32 kSectorSize = 0x0Cu;
constexpr u32 kBlockCount = 0x10u;     // u16 (cBlock)
constexpr u32 kVendorId = 0x12u;       // u16
constexpr u32 kLatency = 0x14u;
constexpr u32 kMountStep = 0x24u;
constexpr u32 kWorkArea = 0x80u;
constexpr u32 kCurrentDir = 0x84u;
constexpr u32 kCurrentFat = 0x88u;
constexpr u32 kRepeat = 0xACu;
constexpr u32 kAddr = 0xB0u;
constexpr u32 kBuffer = 0xB4u;
constexpr u32 kXferred = 0xB8u;
constexpr u32 kExtCallback = 0xC4u;
constexpr u32 kTxCallback = 0xC8u;
constexpr u32 kExiCallback = 0xCCu;
constexpr u32 kApiCallback = 0xD0u;
constexpr u32 kXferCallback = 0xD4u;
constexpr u32 kUnlockCallback = 0xDCu;
constexpr u32 kCid = 0x108u;
constexpr u32 kDiskId = 0x10Cu;

// ---- the CARD library's data and small-data words
constexpr u32 kSectorSizeTable = 0x805684E0u;  // SectorSizeTable[8]
constexpr u32 kLatencyTable = 0x80568500u;     // LatencyTable[8]
constexpr u32 kSdaVendorId = 0x27D8u;          // u16 __CARDVendorID (IsCard's 4-Mbit rule)
constexpr u32 kSdaFastMode = 0x5DF2u;          // u16 the fast mode 0x8038E214 reads (512-byte pages)
constexpr u32 kGameChoice = 0x800030E3u;       // low memory: bit 0x80 forbids card access

// ---- the OS SRAM control block (Scb, __OSLockSramEx 0x8037788C)
constexpr u32 kSram = 0x80586FE0u;
constexpr u32 kSramFlashId = 0x14u;          // OSSramEx.flashID[2][12]
constexpr u32 kSramFlashIdChecksum = 0x3Au;  // OSSramEx.flashIDCheckSum[2]
constexpr u32 kSramEnabled = 0x44u;          // the level the lock saved
constexpr u32 kSramLocked = 0x48u;

// ---- CARD results
constexpr s32 kReady = 0;
constexpr s32 kBusy = -1;
constexpr s32 kWrongDevice = -2;
constexpr s32 kNoCard = -3;
constexpr s32 kIoError = -5;
constexpr s32 kFatalError = -128;

// ---- the card (D3): the identity the emulated card presented
constexpr u32 kSegment = 0x200u;       // a read's unit (CARD_SEG_SIZE)
constexpr u32 kPage = 0x80u;           // a write's unit (this card's page size)
constexpr u32 kSector = 0x2000u;       // an erase's unit
constexpr u16 kCardVendor = 0xC221u;   // what __CARDReadVendorID read
constexpr u32 kSystemBlocks = 5u;      // header, two directories, two allocation tables
constexpr u32 kMountSteps = 7u;        // CARD_MAX_MOUNT_STEP

// ---- the host image format
constexpr std::size_t kBlockSize = 8192u;
constexpr std::size_t kGciHeaderSize = 64u;
constexpr std::array<u8, 8> kGciSourceMarkerMagic{{'G', 'F', 'G', 'C', 'I', 'S', 'R', 'C'}};
// The sizes IsCard accepts: 4 to 128 Mbit (0.5 to 16 MiB) of 8 KiB sectors.
constexpr std::size_t kSmallestCardBytes = 0x80000u;
constexpr std::size_t kLargestCardBytes = 0x1000000u;
// A card this runtime creates: 4 Mbit, 59 user blocks (the game's save needs 2).
constexpr std::size_t kCreatedCardBytes = kSmallestCardBytes;
// The flash ID a created card is formatted against; the IPL derives it back
// from the header, so it only has to be the same on both sides.
constexpr std::array<u8, 12> kCreatedCardFlashId{{'S', 'H', 'A', 'D', 'O', 'W', 'P', 'C', 'S', 'A', 'V', 'E'}};
// OSTime counts the 40.5 MHz time base from 2000-01-01 00:00:00.
constexpr u64 kTimeBaseHz = 40500000u;
constexpr s64 kUnixSecondsAt2000 = 946684800;

std::string hex(u32 value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "native card: " + why);
}

u32 block(u32 chan) noexcept { return kCardBlock + chan * kCardBlockStride; }

// __CARDPutControlBlock (0x8038DFE0): an attached card takes the result; a
// detached one only replaces BUSY.
void put_control_block(Cpu& c, u32 card, s32 result) {
    if (c.mem_read32(card + kAttached) != 0u || static_cast<s32>(c.mem_read32(card + kResult)) == kBusy) {
        c.mem_write32(card + kResult, static_cast<u32>(result));
    }
}

// IsCard (0x80390CC4), over the ID word of the card in the slot.
bool is_card(Cpu& c, u32 id) {
    if ((id & 0xFFFF0000u) != 0u) {
        // Only the 4-Mbit 0x80000004, and only with a vendor ID configured.
        if (id != 0x80000004u || c.mem_read16(c.gpr[13] + kSdaVendorId) == 0xFFFFu) return false;
    }
    if ((id & 3u) != 0u) return false;
    const u32 size = id & 0xFCu;
    if (size != 4u && size != 8u && size != 16u && size != 32u && size != 64u && size != 128u) return false;
    const u32 sector = c.mem_read32(kSectorSizeTable + ((id >> 9) & 0x1Cu));
    if (sector == 0u) return false;
    return ((size << 17) & 0x1FFE0000u) / sector >= 8u;
}

// ---- the host image format (the policy the emulated device had) ------------

u16 card_checksum(const u8* bytes, std::size_t length, bool inverse) {
    u16 checksum = 0u;
    for (std::size_t offset = 0; offset < length; offset += 2u) {
        const u16 word = load_be16(bytes + offset);
        checksum = static_cast<u16>(checksum + (inverse ? static_cast<u16>(~word) : word));
    }
    return checksum == 0xffffu ? 0u : checksum;
}

bool card_checksum_valid(const u8* bytes, std::size_t length, const u8* stored) {
    return load_be16(stored) == card_checksum(bytes, length, false) &&
           load_be16(stored + 2u) == card_checksum(bytes, length, true);
}

struct CardFilesystemCopy {
    std::size_t directory = 0u;
    std::size_t bat = 0u;
    u16 update_counter = 0u;
};

CardFilesystemCopy active_card_filesystem(const std::vector<u8>& image, const std::filesystem::path& path) {
    struct MetadataCopy {
        std::size_t offset;
        u16 counter;
    };
    std::vector<MetadataCopy> directories;
    std::vector<MetadataCopy> bats;
    for (const std::size_t offset : {0x2000u, 0x4000u}) {
        if (card_checksum_valid(image.data() + offset, 0x1ffcu, image.data() + offset + 0x1ffcu)) {
            directories.push_back(MetadataCopy{offset, load_be16(image.data() + offset + 0x1ffau)});
        }
    }
    for (const std::size_t offset : {0x6000u, 0x8000u}) {
        if (card_checksum_valid(image.data() + offset + 4u, 0x1ffcu, image.data() + offset)) {
            bats.push_back(MetadataCopy{offset, load_be16(image.data() + offset + 4u)});
        }
    }
    if (directories.empty() || bats.empty()) {
        throw std::runtime_error("memory-card directory/BAT checksums have no consistent generation: " +
                                 path.string());
    }
    // CARDMount selects the newest checksum-valid directory and the newest
    // checksum-valid BAT independently. CARDWrite of an existing file bumps
    // only the directory counter; requiring them to match rejects a real
    // in-game save on the next launch.
    MetadataCopy directory = directories.front();
    for (const MetadataCopy& candidate : directories) {
        if (static_cast<s16>(candidate.counter - directory.counter) > 0) directory = candidate;
    }
    MetadataCopy bat = bats.front();
    for (const MetadataCopy& candidate : bats) {
        if (static_cast<s16>(candidate.counter - bat.counter) > 0) bat = candidate;
    }
    return CardFilesystemCopy{directory.offset, bat.offset, directory.counter};
}

CardFilesystemCopy validate_raw_card(const std::vector<u8>& image, const std::filesystem::path& path) {
    // Any other size would attach and then fail every mount as WRONGDEVICE
    // (IsCard), which the game reports as an unsupported device.
    if (image.size() < kSmallestCardBytes || image.size() > kLargestCardBytes ||
        (image.size() & (image.size() - 1u)) != 0u) {
        throw std::runtime_error("memory-card raw image has an invalid size (a card is 512 KiB to 16 MiB, a power "
                                 "of two): " + path.string());
    }
    if (!card_checksum_valid(image.data(), 0x1fcu, image.data() + 0x1fcu)) {
        throw std::runtime_error("memory-card header checksum is invalid: " + path.string());
    }
    return active_card_filesystem(image, path);
}

std::vector<u16> find_gci_blocks(const std::vector<u8>& raw, const std::vector<u8>& gci,
                                 const std::filesystem::path& gci_path) {
    if (gci.size() < kGciHeaderSize) {
        throw std::runtime_error("GCI is shorter than its directory header: " + gci_path.string());
    }
    const u16 block_count = load_be16(gci.data() + 0x38u);
    if (block_count == 0u || gci.size() != kGciHeaderSize + static_cast<std::size_t>(block_count) * kBlockSize) {
        throw std::runtime_error("GCI block count does not match its payload: " + gci_path.string());
    }
    const CardFilesystemCopy filesystem = active_card_filesystem(raw, gci_path);
    const u8* matched_entry = nullptr;
    for (std::size_t entry_index = 0; entry_index < 127u; ++entry_index) {
        const u8* entry = raw.data() + filesystem.directory + entry_index * kGciHeaderSize;
        const bool identity_matches =
            std::memcmp(entry, gci.data(), 6u) == 0 && std::memcmp(entry + 8u, gci.data() + 8u, 32u) == 0;
        if (identity_matches && load_be16(entry + 0x38u) == block_count) {
            matched_entry = entry;
            break;
        }
    }
    if (matched_entry == nullptr) {
        throw std::runtime_error("the base raw card has no matching game/maker/name allocation for: " +
                                 gci_path.string());
    }
    u16 current = load_be16(matched_entry + 0x36u);
    std::vector<bool> visited(raw.size() / kBlockSize, false);
    std::vector<u16> blocks;
    blocks.reserve(block_count);
    for (u16 payload_block = 0; payload_block < block_count; ++payload_block) {
        if (current < 5u || current >= visited.size() || visited[current]) {
            throw std::runtime_error("the base raw card has an invalid GCI allocation chain");
        }
        visited[current] = true;
        blocks.push_back(current);
        const std::size_t bat_entry = filesystem.bat + 0x0au + static_cast<std::size_t>(current - 5u) * 2u;
        if (bat_entry + 2u > raw.size()) throw std::runtime_error("the base raw card BAT is truncated");
        const u16 next = load_be16(raw.data() + bat_entry);
        if (payload_block + 1u == block_count) {
            if (next != 0xffffu) throw std::runtime_error("the base raw card allocation is longer than the GCI");
        } else if (next == 0xffffu) {
            throw std::runtime_error("the base raw card allocation is shorter than the GCI");
        }
        current = next;
    }
    return blocks;
}

// Import a standard 64-byte-header GCI into the allocation chain already owned
// by the same game in a raw card. The directory and BAT remain the card's real
// filesystem metadata; only the save's allocated data blocks are replaced.
void import_gci_payload(std::vector<u8>& raw, const std::vector<u8>& gci, const std::filesystem::path& gci_path) {
    const std::vector<u16> blocks = find_gci_blocks(raw, gci, gci_path);
    for (std::size_t payload_block = 0; payload_block < blocks.size(); ++payload_block) {
        const std::size_t target = static_cast<std::size_t>(blocks[payload_block]) * kBlockSize;
        const std::size_t source = kGciHeaderSize + payload_block * kBlockSize;
        std::copy_n(gci.data() + source, kBlockSize, raw.data() + target);
    }
}

bool gci_payload_matches(const std::vector<u8>& raw, const std::vector<u8>& gci,
                         const std::filesystem::path& gci_path) {
    const std::vector<u16> blocks = find_gci_blocks(raw, gci, gci_path);
    for (std::size_t payload_block = 0; payload_block < blocks.size(); ++payload_block) {
        const u8* source = gci.data() + kGciHeaderSize + payload_block * kBlockSize;
        const u8* stored = raw.data() + static_cast<std::size_t>(blocks[payload_block]) * kBlockSize;
        if (!std::equal(source, source + kBlockSize, stored)) return false;
    }
    return true;
}

std::vector<u8> gci_source_marker(const std::vector<u8>& gci) {
    u64 forward = 1469598103934665603ull;
    u64 reverse = 1099511628211ull;
    for (const u8 byte : gci) forward = (forward ^ byte) * 1099511628211ull;
    for (auto it = gci.rbegin(); it != gci.rend(); ++it) reverse = (reverse ^ *it) * 1469598103934665603ull;
    std::vector<u8> marker(32u, 0u);
    std::copy(kGciSourceMarkerMagic.begin(), kGciSourceMarkerMagic.end(), marker.begin());
    const auto store_u64 = [&](std::size_t offset, u64 value) {
        for (u32 byte = 0; byte < 8u; ++byte) marker[offset + byte] = static_cast<u8>(value >> (56u - byte * 8u));
    };
    store_u64(8u, gci.size());
    store_u64(16u, forward);
    store_u64(24u, reverse);
    return marker;
}

void store_checksum(std::vector<u8>& image, std::size_t data, std::size_t length, std::size_t stored) {
    store_be16(image.data() + stored, card_checksum(image.data() + data, length, false));
    store_be16(image.data() + stored + 2u, card_checksum(image.data() + data, length, true));
}

// An empty card, as __CARDFormatRegionAsync (0x803918C4) leaves one: the
// header's serial is the flash ID encoded with the format time, both directory
// copies empty, both allocation tables with every user block free; the copy
// with check code 1 is current. Encoding 0 (ANSI), as the PAL game's font.
std::vector<u8> format_card(std::size_t bytes, u64 format_time) {
    std::vector<u8> image(bytes, u8{0xFFu});
    u64 random = format_time;
    for (std::size_t index = 0; index < kCreatedCardFlashId.size(); ++index) {
        random = (random * 0x41c64e6dull + 0x3039ull) >> 16u;
        image[index] = static_cast<u8>(kCreatedCardFlashId[index] + static_cast<u8>(random & 0xffu));
        random = ((random * 0x41c64e6dull + 0x3039ull) >> 16u) & 0x7fffull;
    }
    store_be64(image.data() + 12u, format_time);
    store_be32(image.data() + 20u, 0u);  // the SRAM counter bias
    store_be32(image.data() + 24u, 0u);  // the SRAM language
    store_be32(image.data() + 28u, 0u);  // the VI DTV status
    store_be16(image.data() + 0x20u, 0u);
    store_be16(image.data() + 0x22u, static_cast<u16>(bytes / 131072u));
    store_be16(image.data() + 0x24u, 0u);
    store_checksum(image, 0u, 0x1fcu, 0x1fcu);
    const u16 user_blocks = static_cast<u16>(bytes / kBlockSize - kSystemBlocks);
    for (u16 copy = 0; copy < 2u; ++copy) {
        const std::size_t directory = (1u + copy) * kBlockSize;
        store_be16(image.data() + directory + 0x1ffau, copy);
        store_checksum(image, directory, 0x1ffcu, directory + 0x1ffcu);
        const std::size_t bat = (3u + copy) * kBlockSize;
        std::fill_n(image.begin() + static_cast<std::ptrdiff_t>(bat), kBlockSize, u8{0u});
        store_be16(image.data() + bat + 4u, copy);
        store_be16(image.data() + bat + 6u, user_blocks);
        store_be16(image.data() + bat + 8u, static_cast<u16>(kSystemBlocks - 1u));
        store_checksum(image, bat + 4u, 0x1ffcu, bat);
    }
    return image;
}

u64 host_format_time() {
    const s64 unix_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    return static_cast<u64>(std::max<s64>(0, unix_seconds - kUnixSecondsAt2000)) * kTimeBaseHz;
}

// A GCI's save on a freshly formatted card, as CARDCreate + CARDWrite would
// leave it: its directory entry (the GCI header) in the first slot of both
// copies, its blocks allocated in one chain from the first user block, the
// payload in them. No base card is needed.
std::vector<u8> card_with_gci(const std::vector<u8>& gci, const std::filesystem::path& gci_path) {
    if (gci.size() < kGciHeaderSize) {
        throw std::runtime_error("GCI is shorter than its directory header: " + gci_path.string());
    }
    const u16 block_count = load_be16(gci.data() + 0x38u);
    if (block_count == 0u || gci.size() != kGciHeaderSize + static_cast<std::size_t>(block_count) * kBlockSize) {
        throw std::runtime_error("GCI block count does not match its payload: " + gci_path.string());
    }
    std::size_t bytes = kCreatedCardBytes;
    while (bytes / kBlockSize - kSystemBlocks < block_count) bytes <<= 1u;
    if (bytes > kLargestCardBytes) throw std::runtime_error("GCI is larger than any memory card: " + gci_path.string());
    std::vector<u8> image = format_card(bytes, host_format_time());
    const u16 first = static_cast<u16>(kSystemBlocks);
    for (u16 copy = 0; copy < 2u; ++copy) {
        const std::size_t directory = (1u + copy) * kBlockSize;
        std::copy_n(gci.data(), kGciHeaderSize, image.data() + directory);
        store_be16(image.data() + directory + 0x36u, first);
        store_checksum(image, directory, 0x1ffcu, directory + 0x1ffcu);
        const std::size_t bat = (3u + copy) * kBlockSize;
        for (u16 index = 0; index < block_count; ++index) {
            const u16 block = static_cast<u16>(first + index);
            const u16 next = index + 1u == block_count ? u16{0xFFFFu} : static_cast<u16>(block + 1u);
            store_be16(image.data() + bat + 0x0au + static_cast<std::size_t>(block - kSystemBlocks) * 2u, next);
        }
        store_be16(image.data() + bat + 6u, static_cast<u16>(load_be16(image.data() + bat + 6u) - block_count));
        store_be16(image.data() + bat + 8u, static_cast<u16>(first + block_count - 1u));
        store_checksum(image, bat + 4u, 0x1ffcu, bat);
    }
    std::copy(gci.begin() + kGciHeaderSize, gci.end(),
              image.begin() + static_cast<std::ptrdiff_t>(first) * static_cast<std::ptrdiff_t>(kBlockSize));
    return image;
}

// The first free "<path>.unusable[.N]" name: where a working card that can no
// longer be mounted is kept before it is rebuilt.
std::filesystem::path unusable_copy_path(const std::filesystem::path& path) {
    std::filesystem::path candidate = path;
    candidate += ".unusable";
    for (unsigned index = 1; std::filesystem::exists(candidate); ++index) {
        candidate = path;
        candidate += ".unusable." + std::to_string(index);
    }
    return candidate;
}

}  // namespace

NativeCard::NativeCard(Cpu& cpu, GuestMemory& memory, NativeInterrupts& interrupts) noexcept
    : cpu_(cpu), memory_(memory), interrupts_(interrupts) {}

NativeCard::~NativeCard() {
    // Bytes the debounce still holds were already reported to the guest as
    // programmed: commit them so a host exit cannot drop an in-game save.
    try {
        persist();
    } catch (...) {
    }
}

const NativeCard::Address* NativeCard::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

// ---------------------------------------------------------------------------
// The host store
// ---------------------------------------------------------------------------

void NativeCard::set_path(std::filesystem::path path, std::filesystem::path gci_working_path) {
    // The outgoing card's deferred bytes are committed before its image goes.
    flush();
    source_path_ = std::move(path);
    backing_path_.clear();
    image_.clear();
    present_ = false;
    dirty_ = false;
    quiet_cycles_ = 0;
    retry_shift_ = 0;
    if (source_path_.empty()) return;

    std::string extension = source_path_.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
    std::vector<u8> image;
    std::filesystem::path backing;
    if (extension == ".gci") {
        const std::vector<u8> gci = read_host_file(source_path_);
        const std::vector<u8> expected_marker = gci_source_marker(gci);
        std::filesystem::path working = std::move(gci_working_path);
        if (working.empty()) working = source_path_.parent_path() / (source_path_.stem().string() + ".pc.raw");
        std::filesystem::path marker_path = working;
        marker_path += ".source";
        if (!working.parent_path().empty()) {
            std::error_code directory_error;
            std::filesystem::create_directories(working.parent_path(), directory_error);
            if (directory_error) {
                throw std::runtime_error("could not create memory-card working directory: " +
                                         working.parent_path().string() + ": " + directory_error.message());
            }
        }
        const std::filesystem::path base = source_path_.parent_path() / "Memory Card A.raw";
        bool rebuild = true;
        if (std::filesystem::exists(working)) {
            image = read_host_file(working);
            if (std::filesystem::exists(marker_path) && read_host_file(marker_path) != expected_marker) {
                throw std::runtime_error("memory-card working image belongs to a different source GCI: " +
                                         working.string());
            }
            bool usable = false;
            try {
                validate_raw_card(image, working);
                usable = true;
            } catch (const std::runtime_error&) {
                usable = false;
            }
            if (usable) {
                // A working card may differ from the source payload after a
                // real in-game save. Bind it to the selected GCI once.
                if (!std::filesystem::exists(marker_path)) {
                    (void)gci_payload_matches(image, gci, source_path_);
                    write_host_file_atomically(marker_path, expected_marker);
                }
                rebuild = false;
            } else {
                // It may hold in-game progress newer than the GCI: keep it
                // before the rebuild replaces it.
                const std::filesystem::path kept = unusable_copy_path(working);
                std::filesystem::rename(working, kept);
                std::fprintf(stderr, "[card] %s cannot be mounted; kept as %s and rebuilt from %s\n",
                             working.string().c_str(), kept.string().c_str(), source_path_.string().c_str());
            }
        }
        if (rebuild) {
            // The GCI goes into the allocation a base card beside it already
            // records for this save; without such a base it is created on a
            // freshly formatted card.
            bool imported = false;
            if (std::filesystem::exists(base)) {
                try {
                    image = read_host_file(base);
                    validate_raw_card(image, base);
                    import_gci_payload(image, gci, source_path_);
                    imported = true;
                } catch (const std::runtime_error& error) {
                    std::fprintf(stderr, "[card] %s is not used (%s); the save is placed on a new card\n",
                                 base.string().c_str(), error.what());
                }
            }
            if (!imported) image = card_with_gci(gci, source_path_);
            write_host_file_atomically(working, image);
            write_host_file_atomically(marker_path, expected_marker);
        }
        backing = std::move(working);
    } else if (extension == ".raw") {
        if (!std::filesystem::exists(source_path_)) {
            // A card that does not exist yet is created formatted and empty:
            // the game offers to create its save on it.
            if (!source_path_.parent_path().empty()) {
                std::error_code directory_error;
                std::filesystem::create_directories(source_path_.parent_path(), directory_error);
                if (directory_error) {
                    throw std::runtime_error("could not create memory-card directory: " +
                                             source_path_.parent_path().string() + ": " + directory_error.message());
                }
            }
            write_host_file_atomically(source_path_, format_card(kCreatedCardBytes, host_format_time()));
            std::fprintf(stderr, "[card] created an empty memory card: %s\n", source_path_.string().c_str());
        }
        image = read_host_file(source_path_);
        backing = source_path_;
    } else {
        throw std::runtime_error("memory card must be a .gci or .raw file: " + source_path_.string());
    }
    validate_raw_card(image, backing);
    image_ = std::move(image);
    backing_path_ = std::move(backing);
    present_ = true;
}

void NativeCard::mark_dirty() noexcept {
    dirty_ = true;
    quiet_cycles_ = 0;
}

// The guest saw its write or erase complete before any host file is touched;
// the file is not addressable by the guest. One durable write per burst: from
// tick() after persist_quiet_cycles_ of game time without a card write, or at
// flush() / set_persist_debounce(false) / destruction / the next set_path. A
// host crash inside the quiet window loses that burst - the outcome of a card
// pulled mid-write; a normal exit or a savestate save still commits it.
void NativeCard::schedule_persist() {
    if (!present_ || !dirty_) return;
    // The deferred write is drained only by tick(). Until the first tick has
    // proven it wired (it runs thousands of times before the guest first
    // touches the card), keep the immediate policy.
    if (!persist_debounce_ || !tick_seen_) {
        save_trace::mark("save.card.request", 0u, cpu_.lr, image_.size());
        persist();
        return;
    }
    save_trace::mark("save.card.request", 1u, cpu_.lr, image_.size());
    ++stats_.persists_deferred;
    quiet_cycles_ = 0;
}

// The persist runs on the game thread inside a guest call (or a clock
// advance): a failed host write is counted and retried with a backoff, never
// thrown into translated code - losing the running game because the card file
// could not be written would be worse than losing the write, and the guest is
// told nothing either way, as with a real card whose program failed.
void NativeCard::persist() {
    if (!present_ || !dirty_) return;
    const auto start = std::chrono::steady_clock::now();
    try {
        save_trace::Stage stage("save.card.persist", image_.size(), cpu_.lr);
        write_host_file_atomically(backing_path_, image_);
        dirty_ = false;
        retry_shift_ = 0;
        ++stats_.persists;
        stats_.persist_bytes += image_.size();
    } catch (const std::exception& error) {
        ++stats_.persist_failures;
        if (retry_shift_ < 6u) ++retry_shift_;
        std::fprintf(stderr, "[card] could not write memory card: %s\n", error.what());
    }
    const u64 elapsed = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    stats_.persist_ns += elapsed;
    stats_.persist_ns_max = std::max(stats_.persist_ns_max, elapsed);
    quiet_cycles_ = 0;
}

u64 NativeCard::cycles_until_persist() const noexcept {
    if (!dirty_ || !persist_debounce_ || !tick_seen_) return ~0ull;
    const u64 threshold = persist_quiet_cycles_ << retry_shift_;
    return quiet_cycles_ >= threshold ? 1u : threshold - quiet_cycles_;
}

void NativeCard::tick(u64 cycles) {
    save_trace::claim_owner();
    tick_seen_ = true;
    if (!dirty_ || !persist_debounce_) return;
    quiet_cycles_ += cycles;
    if (quiet_cycles_ < (persist_quiet_cycles_ << retry_shift_)) return;
    persist();
}

void NativeCard::set_persist_debounce(bool enabled) noexcept {
    persist_debounce_ = enabled;
    // The immediate policy must not leave bytes the deferred one still holds.
    if (!enabled) persist();
}

void NativeCard::set_persist_quiet_ms(u32 milliseconds) noexcept {
    persist_quiet_cycles_ = std::max<u64>(1u, (kCoreClockHz / 1000u) * static_cast<u64>(milliseconds));
}

void NativeCard::flush() { persist(); }

// ---------------------------------------------------------------------------
// The card
// ---------------------------------------------------------------------------

// The ID word EXIGetID read: the size in megabits (image bytes / 128 KiB); the
// sector-size and latency codes are 0 (8 KiB sectors, the table's first
// latency).
u32 NativeCard::card_id() const noexcept { return static_cast<u32>(image_.size() / 131072u); }

bool NativeCard::image_range(u32 address, u64 bytes) const noexcept {
    return static_cast<u64>(address) + bytes <= image_.size();
}

void NativeCard::read_image(Cpu& c, u32 address, u32 bytes, u32 buffer) {
    if (!image_range(address, bytes)) {
        stop(c, "a card read of " + hex(bytes) + " bytes at " + hex(address) + " leaves the " +
                    std::to_string(image_.size() >> 20) + " MiB card");
        return;
    }
    // The SDK's transfer buffers are 32-byte aligned addresses of the game's
    // window.
    if ((buffer & 31u) != 0u || !memory_.mapped(GcAddr(buffer), bytes)) {
        stop(c, "a card read into " + hex(buffer) + " (" + hex(bytes) +
                    " bytes): not a 32-byte aligned buffer of the game's mapped memory");
        return;
    }
    memory_.write_block(buffer, image_.data() + address, bytes);
    ++stats_.reads;
    stats_.read_bytes += bytes;
}

void NativeCard::write_image(Cpu& c, u32 address, u32 bytes, u32 buffer) {
    if (!image_range(address, bytes)) {
        stop(c, "a card write of " + hex(bytes) + " bytes at " + hex(address) + " leaves the " +
                    std::to_string(image_.size() >> 20) + " MiB card");
        return;
    }
    if ((buffer & 31u) != 0u || !memory_.mapped(GcAddr(buffer), bytes)) {
        stop(c, "a card write from " + hex(buffer) + " (" + hex(bytes) +
                    " bytes): not a 32-byte aligned buffer of the game's mapped memory");
        return;
    }
    // The file system erases every sector before it writes it; a byte written
    // over one it did not erase is counted (the flash would have ANDed it).
    for (u32 i = 0; i < bytes; ++i) {
        if (image_[address + i] != 0xFFu) ++stats_.overwritten_bytes;
    }
    memory_.read_block(buffer, image_.data() + address, bytes);
    ++stats_.writes;
    stats_.written_bytes += bytes;
    mark_dirty();
    schedule_persist();
}

void NativeCard::erase_image(Cpu& c, u32 address) {
    // The erase unit is the 8 KiB sector holding the address.
    const u32 base = address & ~(kSector - 1u);
    if (!image_range(base, kSector)) {
        stop(c, "a card erase at " + hex(address) + " leaves the " + std::to_string(image_.size() >> 20) +
                    " MiB card");
        return;
    }
    std::fill_n(image_.begin() + static_cast<std::ptrdiff_t>(base), kSector, u8{0xFFu});
    ++stats_.erases;
    mark_dirty();
    schedule_persist();
}

void NativeCard::post(Cpu& c, u32 chan, Completion kind, s32 result) {
    if (pending_[chan].kind != Completion::None) {
        stop(c, "a card operation on channel " + std::to_string(chan) +
                    " started while the previous one's completion was still pending");
        return;
    }
    pending_[chan] = Pending{kind, result};
    interrupts_.raise(NativeInterrupts::Source::Card);
}

// __CARDRead -> __CARDReadSegment, segment by segment through BlockReadCallback.
void NativeCard::card_read(Cpu& c) {
    const u32 chan = c.gpr[3];
    if (chan >= kChannels) {
        stop(c, "__CARDRead on channel " + std::to_string(chan));
        return;
    }
    const u32 card = block(chan);
    if (c.mem_read32(card + kAttached) == 0u) {
        c.gpr[3] = static_cast<u32>(kNoCard);
        return;
    }
    const u32 address = c.gpr[4];
    const u32 buffer = c.gpr[6];
    c.mem_write32(card + kXferCallback, c.gpr[7]);
    // repeat = length >> 9 segments; the last one ends the transfer, and the
    // first runs even when repeat is 0.
    const u32 repeat = c.gpr[5] >> 9;
    const u32 segments = repeat >= 1u ? repeat : 1u;
    const u32 bytes = segments * kSegment;
    read_image(c, address, bytes, buffer);
    if (c.stopped()) return;
    // __CARDStart: unlockCallback set, then cleared once the lock was taken;
    // txCallback = BlockReadCallback, cleared by each segment's TX handler.
    c.mem_write32(card + kUnlockCallback, 0u);
    c.mem_write32(card + kTxCallback, 0u);
    c.mem_write32(card + kRepeat, repeat >= 1u ? 0u : 0xFFFFFFFFu);
    c.mem_write32(card + kAddr, address + bytes);
    c.mem_write32(card + kBuffer, buffer + bytes);
    c.mem_write32(card + kXferred, c.mem_read32(card + kXferred) + bytes);
    post(c, chan, Completion::Read, kReady);
    c.gpr[3] = static_cast<u32>(kReady);
}

// __CARDWrite -> __CARDWritePage, page by page through BlockWriteCallback.
void NativeCard::card_write(Cpu& c) {
    const u32 chan = c.gpr[3];
    if (chan >= kChannels) {
        stop(c, "__CARDWrite on channel " + std::to_string(chan));
        return;
    }
    const u32 card = block(chan);
    if (c.mem_read32(card + kAttached) == 0u) {
        c.gpr[3] = static_cast<u32>(kNoCard);
        return;
    }
    const u32 page = c.mem_read16(card + kPageSize);
    if (page != kPage) {
        stop(c, "__CARDWrite with a page size of " + std::to_string(page) + " bytes (this card's pages are 128)");
        return;
    }
    const u32 address = c.gpr[4];
    const u32 buffer = c.gpr[6];
    c.mem_write32(card + kXferCallback, c.gpr[7]);
    const s32 repeat = static_cast<s32>(c.gpr[5]) / static_cast<s32>(page);  // divw
    const u32 pages = repeat >= 1 ? static_cast<u32>(repeat) : 1u;
    const u32 bytes = pages * page;
    write_image(c, address, bytes, buffer);
    if (c.stopped()) return;
    // __CARDStart: unlockCallback as for a read; exiCallback =
    // BlockWriteCallback, cleared by each page's EXI handler.
    c.mem_write32(card + kUnlockCallback, 0u);
    c.mem_write32(card + kExiCallback, 0u);
    c.mem_write32(card + kRepeat, static_cast<u32>(repeat >= 1 ? 0 : repeat - 1));
    c.mem_write32(card + kAddr, address + bytes);
    c.mem_write32(card + kBuffer, buffer + bytes);
    c.mem_write32(card + kXferred, c.mem_read32(card + kXferred) + bytes);
    post(c, chan, Completion::Write, kReady);
    c.gpr[3] = static_cast<u32>(kReady);
}

// __CARDEraseSector -> __CARDStart(chan, 0, callback).
void NativeCard::card_erase(Cpu& c) {
    const u32 chan = c.gpr[3];
    if (chan >= kChannels) {
        stop(c, "__CARDEraseSector on channel " + std::to_string(chan));
        return;
    }
    const u32 card = block(chan);
    const u32 page = c.mem_read16(card + kPageSize);
    if (page > kPage) {
        // A 512-byte-page card needs no erase: the original calls back at once.
        stop(c, "__CARDEraseSector on a card with " + std::to_string(page) +
                    "-byte pages (this card's pages are 128)");
        return;
    }
    if (c.mem_read32(card + kAttached) == 0u) {
        c.gpr[3] = static_cast<u32>(kNoCard);
        return;
    }
    const u32 callback = c.gpr[5];
    if (callback != 0u) c.mem_write32(card + kExiCallback, callback);
    c.mem_write32(card + kUnlockCallback, 0u);
    erase_image(c, c.gpr[4]);
    if (c.stopped()) return;
    post(c, chan, Completion::Erase, kReady);
    c.gpr[3] = static_cast<u32>(kReady);
}

// CARDProbeEx over a card present from power-on: EXIProbeEx answers 1 (slot
// A) or -1 (slot B), never 0 - there is no insertion to settle (D3).
void NativeCard::probe(Cpu& c) {
    ++stats_.probes;
    const s32 chan = static_cast<s32>(c.gpr[3]);
    const u32 mem_size = c.gpr[4];
    const u32 sector_size = c.gpr[5];
    if (chan < 0 || chan >= static_cast<s32>(kChannels)) {
        c.gpr[3] = static_cast<u32>(kFatalError);
        return;
    }
    if ((c.mem_read8(kGameChoice) & 0x80u) != 0u) {
        c.gpr[3] = static_cast<u32>(kNoCard);
        return;
    }
    const u32 card = block(static_cast<u32>(chan));
    s32 result = kReady;
    if (!slot_present(static_cast<u32>(chan))) {
        result = kNoCard;
    } else if (c.mem_read32(card + kAttached) != 0u) {
        if (static_cast<s32>(c.mem_read32(card + kMountStep)) < 1) {
            result = kBusy;
        } else {
            if (mem_size != 0u) c.mem_write32(mem_size, c.mem_read16(card + kSize));
            if (sector_size != 0u) c.mem_write32(sector_size, c.mem_read32(card + kSectorSize));
        }
    } else {
        // EXIGetState(chan) & ATTACHED is never set here: only CARDMountAsync
        // attached the EXI channel, together with the card. EXIGetID: the ID.
        const u32 id = card_id();
        if (is_card(c, id)) {
            if (mem_size != 0u) c.mem_write32(mem_size, id & 0xFCu);
            if (sector_size != 0u) c.mem_write32(sector_size, c.mem_read32(kSectorSizeTable + ((id >> 9) & 0x1Cu)));
        } else {
            result = kWrongDevice;
        }
    }
    c.gpr[3] = static_cast<u32>(result);
}

// DoUnmount (0x80391638): the card detached with `result`.
void NativeCard::do_unmount(Cpu& c, u32 chan, s32 result) {
    const u32 card = block(chan);
    if (c.mem_read32(card + kAttached) == 0u) return;
    // EXISetExiCallback(chan, 0), EXIDetach(chan): no EXI; OSCancelAlarm: the
    // CARD alarm is never armed (only the pruned chip-level code armed it).
    c.mem_write32(card + kAttached, 0u);
    c.mem_write32(card + kResult, static_cast<u32>(result));
    c.mem_write32(card + kMountStep, 0u);
}

// DoMount (0x80390F0C) from step 0, and the five system-block reads its
// __CARDMountCallback chain made (steps 2-6). Returns DoMount's result.
s32 NativeCard::do_mount(Cpu& c, u32 chan) {
    const u32 card = block(chan);
    const u32 sda = c.gpr[13];
    s32 result = kReady;
    if (c.mem_read32(card + kMountStep) == 0u) {
        // EXIGetID(chan, 0): the card's ID word; an empty slot reads none.
        const u32 id = slot_present(chan) ? card_id() : 0u;
        if (id == 0u) {
            result = kNoCard;
        } else if (!is_card(c, id)) {
            result = kWrongDevice;
        }
        if (result < 0) {
            do_unmount(c, chan, result);
            return result;
        }
        c.mem_write32(card + kCid, id);
        const u32 size = id & 0xFCu;
        c.mem_write16(card + kSize, static_cast<u16>(size));
        const u32 sector = c.mem_read32(kSectorSizeTable + ((id >> 9) & 0x1Cu));
        c.mem_write32(card + kSectorSize, sector);
        // cBlock = (size << 20 >> 3) / sectorSize, as the code divides.
        const s32 bytes = static_cast<s32>(size << 20) / 8;
        c.mem_write16(card + kBlockCount, static_cast<u16>(bytes / static_cast<s32>(sector)));
        c.mem_write32(card + kLatency, c.mem_read32(kLatencyTable + ((id >> 6) & 0x1Cu)));
        // __CARDReadVendorID, and the page size it selects (512 only for a
        // vendor-0xEC chip in fast mode).
        c.mem_write16(card + kVendorId, kCardVendor);
        const bool large_pages = c.mem_read16(sda + kSdaFastMode) != 0u && (kCardVendor >> 8) == 0xECu;
        c.mem_write16(card + kPageSize, static_cast<u16>(large_pages ? 0x200u : kPage));
        // __CARDClearStatus, __CARDReadStatus (ready, unlocked: bit 0x40),
        // EXIProbe: the unlocked path.
        c.mem_write32(card + kMountStep, 1u);
        // The SRAM's flash-ID checksum for this slot, through __OSLockSramEx /
        // __OSUnlockSramEx(FALSE): the lock records the interrupt level, the
        // unlock restores it.
        if (c.mem_read32(kSram + kSramLocked) != 0u) {
            stop(c, "DoMount found the SRAM locked (__OSLockSramEx returns NULL and DoMount reads through it)");
            return kFatalError;
        }
        u32 sum = 0u;
        for (u32 i = 0; i < 12u; ++i) sum += c.mem_read8(kSram + kSramFlashId + chan * 12u + i);
        c.mem_write32(kSram + kSramEnabled, (c.msr() & kMsrEE) != 0u ? 1u : 0u);
        if (c.mem_read8(kSram + kSramFlashIdChecksum + chan) != static_cast<u8>(~sum)) {
            do_unmount(c, chan, kIoError);
            return kIoError;
        }
    }
    if (c.mem_read32(card + kMountStep) == 1u) {
        if (c.mem_read32(card + kCid) == 0x80000004u) {
            // The 4-Mbit card's vendor rule: SRAM flash-ID word vs __CARDVendorID.
            if (c.mem_read32(kSram + kSramLocked) != 0u) {
                stop(c, "DoMount found the SRAM locked (__OSLockSramEx returns NULL and DoMount reads through it)");
                return kFatalError;
            }
            const u16 flash = c.mem_read16(kSram + kSramFlashId + chan * 12u);
            c.mem_write32(kSram + kSramEnabled, (c.msr() & kMsrEE) != 0u ? 1u : 0u);
            const u16 vendor = c.mem_read16(sda + kSdaVendorId);
            if (vendor != 0xFFFFu && flash != vendor) {
                do_unmount(c, chan, kWrongDevice);
                return kWrongDevice;
            }
        }
        // __CARDEnableInterrupt, EXISetExiCallback(__CARDExiHandler),
        // EXIUnlock: no chip, no EXI; DCInvalidateRange: no data cache.
        c.mem_write32(card + kMountStep, 2u);
    }
    // Steps 2-6: __CARDRead(chan, sectorSize * n, 8 KiB, workArea + 8 KiB * n,
    // __CARDMountCallback), each completion advancing the step.
    const u32 sector = c.mem_read32(card + kSectorSize);
    const u32 work = c.mem_read32(card + kWorkArea);
    for (u32 n = c.mem_read32(card + kMountStep) - 2u; n < kSystemBlocks; ++n) {
        read_image(c, sector * n, kSector, work + kSector * n);
        if (c.stopped()) return kFatalError;
        // As __CARDRead leaves them, the transfer callback taken by the step's
        // completion.
        c.mem_write32(card + kXferCallback, 0u);
        c.mem_write32(card + kUnlockCallback, 0u);
        c.mem_write32(card + kTxCallback, 0u);
        c.mem_write32(card + kRepeat, 0u);
        c.mem_write32(card + kAddr, sector * n + kSector);
        c.mem_write32(card + kBuffer, work + kSector * n + kSector);
        c.mem_write32(card + kXferred, c.mem_read32(card + kXferred) + kSector);
    }
    post(c, chan, Completion::Mount, kReady);
    return result;
}

void NativeCard::mount(Cpu& c) {
    const s32 chan = static_cast<s32>(c.gpr[3]);
    const u32 work = c.gpr[4];
    const u32 detach = c.gpr[5];
    const u32 attach = c.gpr[6];
    if (chan < 0 || chan >= static_cast<s32>(kChannels)) {
        c.gpr[3] = static_cast<u32>(kFatalError);
        return;
    }
    if ((c.mem_read8(kGameChoice) & 0x80u) != 0u) {
        c.gpr[3] = static_cast<u32>(kNoCard);
        return;
    }
    const u32 card = block(static_cast<u32>(chan));
    if (static_cast<s32>(c.mem_read32(card + kResult)) == kBusy) {
        c.gpr[3] = static_cast<u32>(kBusy);
        return;
    }
    // (!attached && EXIGetState(chan) & ATTACHED -> WRONGDEVICE never holds:
    // only this function attached the EXI channel, together with the card.)
    c.mem_write32(card + kResult, static_cast<u32>(kBusy));
    c.mem_write32(card + kWorkArea, work);
    c.mem_write32(card + kExtCallback, detach);
    c.mem_write32(card + kApiCallback, attach != 0u ? attach : default_api_callback_);
    c.mem_write32(card + kExiCallback, 0u);
    if (c.mem_read32(card + kAttached) == 0u && !slot_present(static_cast<u32>(chan))) {
        // EXIAttach failed: nothing in the slot.
        c.mem_write32(card + kResult, static_cast<u32>(kNoCard));
        c.gpr[3] = static_cast<u32>(kNoCard);
        return;
    }
    c.mem_write32(card + kMountStep, 0u);
    c.mem_write32(card + kAttached, 1u);
    // EXISetExiCallback(chan, 0): no EXI; OSCancelAlarm: never armed.
    c.mem_write32(card + kCurrentDir, 0u);
    c.mem_write32(card + kCurrentFat, 0u);
    // unlockCallback = __CARDMountCallback, cleared once EXILock succeeded.
    c.mem_write32(card + kUnlockCallback, 0u);
    ++stats_.mounts;
    const s32 result = do_mount(c, static_cast<u32>(chan));
    if (result < 0) ++stats_.mount_failures;
    c.gpr[3] = static_cast<u32>(result);
}

void NativeCard::unmount(Cpu& c) {
    const s32 chan = static_cast<s32>(c.gpr[3]);
    // __CARDGetControlBlock (0x8038DF28).
    if (chan < 0 || chan >= static_cast<s32>(kChannels) || c.mem_read32(block(static_cast<u32>(chan)) + kDiskId) == 0u) {
        c.gpr[3] = static_cast<u32>(kFatalError);
        return;
    }
    const u32 card = block(static_cast<u32>(chan));
    if (c.mem_read32(card + kAttached) == 0u) {
        c.gpr[3] = static_cast<u32>(kNoCard);
        return;
    }
    if (static_cast<s32>(c.mem_read32(card + kResult)) == kBusy) {
        c.gpr[3] = static_cast<u32>(kBusy);
        return;
    }
    c.mem_write32(card + kResult, static_cast<u32>(kBusy));
    c.mem_write32(card + kApiCallback, 0u);
    // The detach: as DoUnmount, with NOCARD.
    ++stats_.unmounts;
    do_unmount(c, static_cast<u32>(chan), kNoCard);
    c.gpr[3] = static_cast<u32>(kReady);
}

// ---------------------------------------------------------------------------
// The completion, in interrupt context (NativeInterrupts::Source::Card)
// ---------------------------------------------------------------------------
//
// Scratch word 0: channel | kind << 8; word 1: the result.
bool NativeCard::completion_handler(Cpu& c, u32 frame, u32 step) {
    u32 chan = 0;
    Completion kind = Completion::None;
    s32 result = kReady;
    if (step == 0u) {
        while (chan < kChannels && pending_[chan].kind == Completion::None) ++chan;
        if (chan == kChannels) return false;
        kind = pending_[chan].kind;
        result = pending_[chan].result;
        pending_[chan] = Pending{};
        // Another channel's completion is delivered after this one.
        for (u32 other = chan + 1u; other < kChannels; ++other) {
            if (pending_[other].kind != Completion::None) interrupts_.raise(NativeInterrupts::Source::Card);
        }
        c.mem_write32(NativeInterrupts::scratch(frame, 0u), chan | (static_cast<u32>(kind) << 8));
        c.mem_write32(NativeInterrupts::scratch(frame, 1u), static_cast<u32>(result));
        ++stats_.completions;
    } else {
        const u32 word = c.mem_read32(NativeInterrupts::scratch(frame, 0u));
        chan = word & 0xFFu;
        kind = static_cast<Completion>((word >> 8) & 0xFFu);
        result = static_cast<s32>(c.mem_read32(NativeInterrupts::scratch(frame, 1u)));
    }
    const u32 card = block(chan);
    const auto call = [&](u32 function, u32 r3, u32 r4) {
        ++stats_.callbacks;
        interrupts_.call_guest(c, frame, function, r3, r4);
        return true;
    };
    switch (kind) {
        case Completion::Write:
            // __CARDExiHandler: nothing for a card no longer attached.
            if (step == 0u && c.mem_read32(card + kAttached) == 0u) {
                ++stats_.dropped_detached;
                return false;
            }
            [[fallthrough]];
        case Completion::Read: {
            // The last segment's / page's callback tail.
            if (step != 0u) return false;
            if (c.mem_read32(card + kApiCallback) == 0u) put_control_block(c, card, result);
            const u32 callback = c.mem_read32(card + kXferCallback);
            if (callback == 0u) return false;
            c.mem_write32(card + kXferCallback, 0u);
            return call(callback, chan, static_cast<u32>(result));
        }
        case Completion::Erase: {
            if (step != 0u) return false;
            if (c.mem_read32(card + kAttached) == 0u) {
                ++stats_.dropped_detached;
                return false;
            }
            const u32 callback = c.mem_read32(card + kExiCallback);
            if (callback == 0u) return false;
            c.mem_write32(card + kExiCallback, 0u);
            return call(callback, chan, static_cast<u32>(result));
        }
        case Completion::Mount: {
            if (step == 0u) {
                // The fifth system block's __CARDMountCallback: step 7, verify.
                c.mem_write32(card + kMountStep, kMountSteps);
                // The file system's mount check (__CARDVerify) is guest code of the CARD library, which the game no
                // longer compiles: only the reference tests, which run the original words on this model, name its
                // entry (set_verify_entry); without one the mount stops the machine rather than answer for it.
                if (verify_entry_ == 0u) {
                    stop(c, "the card model's mount check (__CARDVerify) has no entry: the CARD library is retired");
                    return false;
                }
                return call(verify_entry_, card, 0u);
            }
            if (step == 1u) {
                const s32 verified = static_cast<s32>(c.gpr[3]);
                const u32 callback = c.mem_read32(card + kApiCallback);
                c.mem_write32(card + kApiCallback, 0u);
                put_control_block(c, card, verified);
                if (callback == 0u) {
                    stop(c, "the mount's API callback is NULL (the original calls through it)");
                    return false;
                }
                return call(callback, chan, static_cast<u32>(verified));
            }
            return false;
        }
        case Completion::None:
            break;
    }
    return false;
}

void NativeCard::install() {
    Cpu& cpu = cpu_;
    const auto own = [&cpu](u32 address, const char* name, auto&& serve) {
        cpu.install_hle(address, [serve](Cpu& c) {
            serve(c);
            return true;  // an owner never declines
        }, std::string("native card: ") + name);
    };
    interrupts_.set_handler(NativeInterrupts::Source::Card,
                            [this](Cpu& c, u32 frame, u32 step) { return completion_handler(c, frame, step); });
    own(kCardEraseSector, "__CARDEraseSector", [this](Cpu& c) { card_erase(c); });
    own(kCardRead, "__CARDRead", [this](Cpu& c) { card_read(c); });
    own(kCardWrite, "__CARDWrite", [this](Cpu& c) { card_write(c); });
    own(kCardProbeEx, "CARDProbeEx", [this](Cpu& c) { probe(c); });
    own(kCardMountAsync, "CARDMountAsync", [this](Cpu& c) { mount(c); });
    own(kCardUnmount, "CARDUnmount", [this](Cpu& c) { unmount(c); });
}

bool NativeCard::deliver_pending(Cpu& c, u32 frame, u32 callback_return, const std::function<void()>& step_one) {
    if (!interrupts_.status(NativeInterrupts::Source::Card)) return false;
    interrupts_.clear(NativeInterrupts::Source::Card);
    for (u32 step = 0;; ++step) {
        if (!completion_handler(c, frame, step)) return true;
        // The handler called a guest function (call_guest left pc at it and LR at the delivery's continuation).
        u64 guard = 0;
        while (c.pc != callback_return && !c.stopped() && guard++ < 50000000ull) step_one();
        if (c.pc != callback_return) return true;  // stopped: the caller sees it
    }
}

void NativeCard::save_state(std::vector<u8>& out) const {
    state::put<u8>(out, static_cast<u8>(kChannels));
    for (const Pending& pending : pending_) {
        state::put<u8>(out, static_cast<u8>(pending.kind));
        state::put<s32>(out, pending.result);
    }
}

bool NativeCard::load_state(const u8*& cursor, const u8* end) {
    u8 channels = 0;
    if (!state::get<u8>(cursor, end, channels) || channels != kChannels) return false;
    std::array<Pending, kChannels> loaded{};
    for (Pending& pending : loaded) {
        u8 kind = 0;
        if (!state::get<u8>(cursor, end, kind) || !state::get<s32>(cursor, end, pending.result)) return false;
        if (kind > static_cast<u8>(Completion::Mount)) return false;
        pending.kind = static_cast<Completion>(kind);
    }
    pending_ = loaded;
    return true;
}

void NativeCard::reset() noexcept { pending_ = {}; }

}  // namespace gekko
