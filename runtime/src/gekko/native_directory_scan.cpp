// The game's directory scan 0x80044ED4, native over the asset namespace. Contract: include/gekko/
// native_directory_scan.h and docs/DISC_SYSTEM_MAP.md ("Cut 2": the disassembly and the evidence).

#include "gekko/native_directory_scan.h"

#include <string>
#include <string_view>
#include <vector>

#include "gekko/asset_mount.h"
#include "gekko/cpu.h"
#include "gekko/recomp.h"

namespace gekko {

namespace {

constexpr u32 kScan = 0x80044ED4u;  // the directory scan (the entry the generator routes here)

const NativeDirectoryScan::Address kAddresses[] = {
    {kScan, "the game's directory scan", false},
};

// What the scan calls.
constexpr u32 kHeapAlloc = 0x80049BB8u;  // alloc(size, heap)
constexpr u32 kHeapFree = 0x80049B5Cu;   // free(block)
constexpr u32 kMemcpy = 0x800054F4u;     // memcpy(dst, src, n) (a native owner itself)
constexpr u32 kHeap = 2u;                // the heap both blocks come from
constexpr u32 kCapOffset = 0x7BF0u;      // the record capacity: the word at -0x7BF0(r13)

// One record, and the original's frame: stwu r1,-0x80(r1); stw r0,0x84(r1) (the caller's LR save word);
// stmw r26,0x68(r1).
constexpr u32 kRecord = 0x108u;
constexpr u32 kRecordLength = 0x100u;
constexpr u32 kRecordKind = 0x104u;
constexpr u32 kKindDirectory = 1u;
constexpr u32 kKindFile = 2u;
constexpr u32 kNoLength = 0xFFFFFFFFu;
constexpr u32 kFrame = 0x80u;
constexpr u32 kSavedRegisters = 0x68u;
constexpr u32 kMaxPath = 4096u;  // the file API's limit for a guest path (DVDOpenDir refused a longer one)

void stop(Cpu& c, const std::string& why) {
    c.request_stop(StopReason::ExecutionFault, "native directory scan: " + why);
}

// The guest string at `address`, up to its NUL; false when there is none within kMaxPath bytes.
bool guest_string(Cpu& c, u32 address, std::string& out) {
    out.clear();
    for (u32 i = 0u; i < kMaxPath; ++i) {
        const u8 byte = c.mem_read8(address + i);
        if (byte == 0u) return true;
        out.push_back(static_cast<char>(byte));
    }
    return false;
}

// A guest call returning here; the scan's own caller (LR) is left as it was.
bool call_guest(Cpu& c, u32 function, u32 r3, u32 r4, u32 r5) {
    const u32 link = c.lr;
    c.gpr[3] = r3;
    c.gpr[4] = r4;
    c.gpr[5] = r5;
    c.gpr[12] = function;
    c.ctr = function;
    recomp::call_indirect(c, function, link);
    if (c.exiting()) c.finish_transfers();
    if (c.exiting() || c.pc != link) {
        stop(c, "a heap call did not return");
        return false;
    }
    c.lr = link;
    return true;
}

}  // namespace

const NativeDirectoryScan::Address* NativeDirectoryScan::addresses(std::size_t& count) noexcept {
    count = sizeof(kAddresses) / sizeof(kAddresses[0]);
    return kAddresses;
}

void NativeDirectoryScan::install() {
    cpu_.install_hle(kScan, [this](Cpu& c) {
        ++stats_.scans;
        const u32 out = c.gpr[4], filter_address = c.gpr[6];
        // The original's prologue, on every path (its LR save is a word of the caller's frame): the heap calls
        // below then run where the original's ran.
        const u32 sp = c.gpr[1], frame = sp - kFrame;
        c.mem_write32(frame, sp);
        c.mem_write32(sp + 4u, c.lr);
        for (u32 r = 26u; r < 32u; ++r) c.mem_write32(frame + kSavedRegisters + 4u * (r - 26u), c.gpr[r]);
        std::string path, filter;
        if (filter_address != 0u && !guest_string(c, filter_address, filter)) {
            stop(c, "the filter has no NUL within 4096 bytes");
            return true;
        }
        const assets::AssetNamespace& names = assets_.names();
        // A path with no NUL within the limit did not open (the file API's DVDOpenDir refused it).
        const bool terminated = guest_string(c, c.gpr[5], path);
        const s32 found = terminated ? names.resolve(path) : assets::AssetNamespace::kNotFound;
        if (found < 0 || !names.is_directory(static_cast<u32>(found))) {
            ++stats_.missing;
            c.mem_write32(out, 0u);
            // (the original's epilogue restores r1; the owner never moved it)
            c.gpr[3] = 0u;
            return true;
        }

        // The matching children, in entry order (a directory's subtree is skipped, as DVDReadDir did).
        struct Match {
            u32 index;
            bool directory;
        };
        std::vector<Match> matches;
        const u32 directory = static_cast<u32>(found);
        for (u32 at = directory + 1u; at < names.entry(directory).end;) {
            const bool is_directory = names.is_directory(at);
            const std::string_view name = names.name(at);
            if (filter_address == 0u || name.find(filter) != std::string_view::npos) {
                if (name.size() >= kRecordLength) {
                    stop(c, "'" + std::string(name) + "' does not fit a record's 256-byte name");
                    return true;
                }
                matches.push_back({at, is_directory});
            }
            at = is_directory ? names.entry(at).end : at + 1u;
        }
        const u32 cap = c.mem_read32(c.gpr[13] - kCapOffset);
        c.gpr[1] = frame;
        if (matches.size() > cap) {
            stop(c, "'" + path + "' has " + std::to_string(matches.size()) + " matching entries, more than the " +
                        std::to_string(cap) + " records the game allocates");
            return true;
        }

        if (!call_guest(c, kHeapAlloc, cap * kRecord, kHeap, c.gpr[5])) return true;
        const u32 staging = c.gpr[3];
        if (staging == 0u) {
            stop(c, "the heap could not supply the scan's staging block");
            return true;
        }
        for (std::size_t i = 0; i < matches.size(); ++i) {
            const u32 record = staging + static_cast<u32>(i) * kRecord;
            const std::string_view name = names.name(matches[i].index);
            for (std::size_t k = 0; k < name.size(); ++k)
                c.mem_write8(record + static_cast<u32>(k), static_cast<u8>(name[k]));
            c.mem_write8(record + static_cast<u32>(name.size()), 0u);
            if (matches[i].directory) {
                c.mem_write32(record + kRecordKind, kKindDirectory);
                c.mem_write32(record + kRecordLength, kNoLength);
                continue;
            }
            c.mem_write32(record + kRecordKind, kKindFile);
            // The original opened the bare name, which resolves from the root.
            const s32 opened = names.resolve(name);
            u32 length = kNoLength;
            if (opened >= 0 && !names.is_directory(static_cast<u32>(opened))) {
                const u64 size = names.entry(static_cast<u32>(opened)).size;
                if (size > 0xFFFFFFFFull) {
                    stop(c, "'" + std::string(name) + "' is larger than a 32-bit length");
                    return true;
                }
                length = static_cast<u32>(size);
            }
            c.mem_write32(record + kRecordLength, length);
        }
        const u32 count = static_cast<u32>(matches.size());
        if (!call_guest(c, kHeapAlloc, count * kRecord, kHeap, c.gpr[5])) return true;
        const u32 result = c.gpr[3];
        c.mem_write32(out, result);
        if (!call_guest(c, kMemcpy, result, staging, count * kRecord)) return true;
        if (!call_guest(c, kHeapFree, staging, c.gpr[4], c.gpr[5])) return true;

        c.gpr[1] = sp;
        c.gpr[3] = count;
        stats_.records += count;
        return true;  // an owner never declines
    }, "native directory scan");
}

}  // namespace gekko
