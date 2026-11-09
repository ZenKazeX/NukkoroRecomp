// The native file API: every entry point of the file API the game links
// against, served from the mounted asset tree.
//
// What this suite defends is the contract the game observes (results, the
// fields of its DVDFileInfo / command blocks / DVDDir records, the bytes, the
// callback and its continuation, the failure behaviour) and the token decode
// that replaced the disc: a token that names no asset, or a request that
// reaches past what the API's own bounds check permits, is a COUNTED failure
// rather than a neighbour's bytes. The whole-surface differential against the
// original SDK words is native_file_api_reference_tests.

#include "gekko/native_file_api.h"
#include "shadow/backends/storage_backends.h"
#include "gekko/asset_mount.h"
#include "gekko/native_axfx_effects.h"
#include "gekko/native_os_arena_clock.h"
#include "gekko/native_os_debug.h"
#include "gekko/native_os_report.h"
#include "gekko/native_trk.h"
#include "gekko/native_gx_display.h"
#include "gekko/native_adx_output.h"
#include "gekko/native_sj.h"
#include "gekko/native_svm.h"
#include "gekko/native_ax_frame.h"
#include "gekko/native_rw_device_state.h"
#include "gekko/native_rw_registry.h"
#include "gekko/native_gx_texobj.h"
#include "test_scratch.h"
#include "gekko/cpu.h"
#include "gekko/guest_memory.h"
#include "gekko/system.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace gekko;

namespace {
constexpr u32 kReadAbs = 0x8037CF40u;
constexpr u32 kCallbackReturn = 0x8037CF44u;
constexpr u32 kOpen = 0x8037AAA0u, kClose = 0x8037AB68u, kReadAsync = 0x8037AF04u;
constexpr u32 kReadSync = 0x8037AFF4u;
constexpr u32 kTransferred = 0x8037B294u, kInit = 0x8037B304u, kReadDiskId = 0x8037D0ECu;
constexpr u32 kCancelStream = 0x8037D1C0u, kBlockStatus = 0x8037D390u, kDriveStatus = 0x8037D3DCu;
constexpr u32 kAutoInvalidation = 0x8037D488u, kCancelAsync = 0x8037D4E8u, kCancel = 0x8037D764u;
constexpr u32 kDiskId = 0x8037D834u, kCheckDisk = 0x8037D83Cu, kPrepareReset = 0x8037D934u;
constexpr u32 kTestAlarm = 0x8037DA50u, kAutoFatal = 0x8037DF9Cu, kDriveConfig = 0x80371768u;

constexpr u32 callback_entry = 0x80008000u;
constexpr u32 caller_return = 0x80008100u;
constexpr u32 block = 0x80001000u;
constexpr u32 destination = 0x80002000u;
constexpr u32 path_text = 0x80004000u;
constexpr u32 dir_record = 0x80004200u;
constexpr u32 dir_entry = 0x80004240u;
constexpr u32 stack = 0x8060F000u;  // free image memory past the DOL's sections

// The fixture tree: asset.bin (1 KiB, first 32 bytes 1..32), dir/b.bin,
// dir/sub/c.bin, Z.TXT.
constexpr u32 kAssetSize = 0x400u;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::vector<u8> identity_bytes() {
    std::vector<u8> bytes(0x20u);
    for (u32 i = 0; i < 32u; ++i) bytes[i] = static_cast<u8>(i + 1u);
    return bytes;
}

std::vector<u8> asset_bytes(u32 size, u32 seed) {
    std::vector<u8> bytes(size);
    for (u32 i = 0; i < size; ++i) bytes[i] = i < 32u && seed == 7u ? static_cast<u8>(i + 1u)
                                                                    : static_cast<u8>((i * seed) & 0xFFu);
    return bytes;
}

void write_file(const std::filesystem::path& path, const std::vector<u8>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::filesystem::path mount_root() {
    const auto dir = gekko::test_support::scratch_directory("gekko-native-file-api-tests");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    write_file(dir / "asset.bin", asset_bytes(kAssetSize, 7u));
    write_file(dir / "dir" / "b.bin", asset_bytes(0x123u, 3u));
    write_file(dir / "dir" / "sub" / "c.bin", asset_bytes(0x9000u, 5u));
    write_file(dir / "Z.TXT", asset_bytes(0x20u, 11u));
    return dir;
}

void callback(Cpu& c) {
    for (u32 at = 8u; at < 40u; at += 4u) c.mem_write32(c.gpr[1] + at, at);
    c.mem_write32(0x80003000u, c.gpr[3]);
    c.mem_write32(0x80003004u, c.gpr[4]);
    c.leave_to_dispatcher(callback_entry + 4u, FallbackReason::Yield, callback_entry);
}

void callback_tail(Cpu& c) {
    c.mem_write32(0x80003008u, 0xC0FFEEu);
    c.gpr[3] = 0xBADu;
    c.pc = c.lr;
}

void caller(Cpu& c) {
    c.mem_write32(0x8000300Cu, c.gpr[3]);
    c.request_stop(StopReason::HostRequest, "test completed");
}

u32 g_asset_base = 0;

void nested_parent(Cpu& c) {
    c.mem_write32(c.gpr[1] + 4u, c.lr);
    c.gpr[3] = block + 0x80u;
    c.gpr[4] = destination + 0x80u;
    c.gpr[5] = 16u;
    c.gpr[6] = g_asset_base;
    c.gpr[7] = callback_entry;
    c.lr = 0x80008204u;
    c.try_hle(kReadAbs);
}

void nested_tail(Cpu& c) {
    c.lr = c.mem_read32(c.gpr[1] + 4u);
    c.mem_write32(0x80003010u, 0x12345678u);
    c.pc = c.lr;
}

void read_native(Cpu& c) { c.try_hle(kReadAbs); }
void read_range(Cpu& c, u32 address) { c.try_hle(address); }
void async_native(Cpu& c) { c.try_hle(kReadAsync); }
void cancel_native(Cpu& c) { c.try_hle(kCancelAsync); }

struct Fixture {
    GuestMemory bus;
    Cpu cpu{bus};
    assets::AssetSystem assets;
    NativeFileApi api{cpu, bus, assets};
    u32 base = 0;
    std::array<RecompiledCode::Entry, 8> entries{{
        {callback_entry, 4u, callback, "callback"},
        {callback_entry + 4u, 4u, callback_tail, "callback-tail"},
        {caller_return, 4u, caller, "caller"},
        {0x80008200u, 4u, nested_parent, "nested"},
        {0x80008204u, 4u, nested_tail, "nested-tail"},
        {kReadAsync, 192u, async_native, "DVDReadAsyncPrio"},
        {kReadAbs, 220u, read_native, "DVDReadAbsAsyncPrio"},
        {kCancelAsync, 80u, cancel_native, "DVDCancelAsync"},
    }};
    RecompiledCode::Range range{kReadAbs, kReadAbs + 220u, read_range};
    RecompiledCode code;

    Fixture() {
        std::string error;
        require(shadow::backends::mount_host_directory(assets, mount_root(), identity_bytes(), error), "the fixture's assets did not mount");
        base = assets.space().base_of(assets.registry().find("asset.bin"));
        require(base != 0u, "the fixture's asset has no token");
        g_asset_base = base;
        cpu.reset();
        code.install(entries.data(), entries.size());
        code.install_ranges(&range, 1u);
        cpu.attach_recompiled(&code);
        cpu.gpr[1] = stack;
        cpu.lr = caller_return;
        api.install();
    }

    void put_string(u32 at, const std::string& text) {
        for (std::size_t i = 0; i < text.size(); ++i) cpu.mem_write8(at + static_cast<u32>(i), static_cast<u8>(text[i]));
        cpu.mem_write8(at + static_cast<u32>(text.size()), 0u);
    }
    std::string get_string(u32 at) {
        std::string text;
        for (u8 b; (b = cpu.mem_read8(at + static_cast<u32>(text.size()))) != 0u;) text.push_back(static_cast<char>(b));
        return text;
    }

    void call(u32 entry, std::initializer_list<u32> args) {
        u32 r = 3u;
        for (const u32 value : args) cpu.gpr[r++] = value;
        cpu.pc = entry;
        cpu.lr = caller_return;
        require(cpu.try_hle(entry), "an owner was not served");
    }

    // A raw token read through DVDReadAbsAsyncPrio.
    void read(u32 addr = destination, u32 length = 32u, u32 offset = 0u, u32 cb = 0u) {
        call(kReadAbs, {block, addr, length, base + offset, cb, 2u});
    }
    void read_raw_token(u32 token, u32 length = 32u) { call(kReadAbs, {block, destination, length, token, 0u, 2u}); }

    // Runs the translated fixture until the machine stops. Cpu::run has no
    // budget: it returns at `caller`'s request, or with a fault where native
    // execution cannot continue, which every stop-reason check rejects.
    void run_until_stop() { cpu.run(); }
};

void effective_address() {
    auto f = std::make_unique<Fixture>();
    const std::vector<u8> expected = asset_bytes(kAssetSize, 7u);
    f->read();
    require(std::equal(expected.begin(), expected.begin() + 32, f->bus.window_bytes(destination, destination + 32u).begin()),
            "read bytes did not reach the effective destination");
    require(f->bus.diagnostics().unmapped_writes == 0u, "read wrote unmapped bus addresses");
    require(f->cpu.mem_read32(block + 0x20u) == 32u, "transfer count was not published");
    require(f->cpu.mem_read32(block + 0x0Cu) == 0u, "completion state was not END");
    require(f->cpu.gpr[3] == 1u && f->cpu.pc == caller_return, "read return contract changed");
}

void offset_within_asset() {
    auto f = std::make_unique<Fixture>();
    const std::vector<u8> expected = asset_bytes(kAssetSize, 7u);
    f->read(destination, 64u, 128u);
    require(std::equal(expected.begin() + 128, expected.begin() + 192,
                       f->bus.window_bytes(destination, destination + 64u).begin()),
            "token + offset did not select the right bytes");
    require(f->api.stats().reads_unmapped == 0u && f->api.stats().reads_out_of_range == 0u,
            "a legal offset was misdecoded");
}

void tail_past_end_of_asset_zero_fills() {
    // Every live issuer rounds its length up to a multiple of 32, so the last
    // read of an asset always asks for bytes that are not there; the API's own
    // assertion bounds that overrun at 32 bytes and the token space reserves it.
    auto f = std::make_unique<Fixture>();
    f->read(destination, 32u, kAssetSize);
    require(f->cpu.mem_read32(block + 0x0Cu) == 0u, "a read of the 32-byte tail was refused");
    require(f->cpu.mem_read32(block + 0x20u) == 32u, "the tail read reported no bytes");
    for (u32 i = 0; i < 32u; ++i)
        require(f->bus.peek8(destination + i) == 0u, "the tail past the asset did not read as zero");
    require(f->api.stats().reads_out_of_range == 0u, "the API's own legal overrun was refused");
}

void unmapped_token_is_counted_not_aliased() {
    auto f = std::make_unique<Fixture>();
    f->cpu.mem_write32(destination, 0xDEADBEEFu);
    f->read_raw_token(0u);  // an uninitialised startAddr
    require(f->api.stats().reads_unmapped == 1u, "an unmapped token was not counted");
    require(f->cpu.mem_read32(block + 0x0Cu) == 0xFFFFFFFFu, "an unmapped token reported success");
    require(f->cpu.mem_read32(destination) == 0xDEADBEEFu, "an unmapped token wrote the destination");
}

void a_request_leaving_its_asset_is_refused() {
    auto f = std::make_unique<Fixture>();
    f->cpu.mem_write32(destination, 0xDEADBEEFu);
    f->read(destination, 256u, kAssetSize + 32u);
    require(f->api.stats().reads_out_of_range == 1u, "an out-of-range request was not counted");
    require(f->cpu.mem_read32(block + 0x0Cu) == 0xFFFFFFFFu, "an out-of-range request reported success");
    require(f->cpu.mem_read32(destination) == 0xDEADBEEFu, "an out-of-range request wrote the destination");
}

void callback_continuation() {
    auto f = std::make_unique<Fixture>();
    f->read(destination, 32u, 0u, callback_entry);
    f->run_until_stop();
    require(f->cpu.stop().reason == StopReason::HostRequest, "callback did not return to the requester");
    require(f->cpu.mem_read32(0x80003000u) == 32u, "read callback received zero instead of the bytes read");
    require(f->cpu.mem_read32(0x80003004u) == block, "callback received the wrong block");
    require(f->cpu.mem_read32(0x80003008u) == 0xC0FFEEu, "callback continuation was discarded");
    require(f->cpu.mem_read32(0x8000300Cu) == 1u, "callback overwrote the API return value");
    require(f->cpu.gpr[1] == stack, "callback stack was not restored");
}

void nested_request() {
    auto f = std::make_unique<Fixture>();
    f->read(destination, 32u, 0u, 0x80008200u);
    f->run_until_stop();
    require(f->cpu.stop().reason == StopReason::HostRequest, "nested callback did not return");
    require(f->cpu.mem_read32(0x80003000u) == 16u, "nested callback length incorrect");
    require(f->cpu.mem_read32(0x80003010u) == 0x12345678u, "parent callback did not resume");
    require(f->cpu.gpr[1] == stack && f->api.stats().callbacks == 2u, "nested callback lifetime corrupted");
}

void identity_arguments() {
    auto f = std::make_unique<Fixture>();
    f->call(kReadDiskId, {block, destination, 0u});
    require(f->cpu.mem_read32(destination) == 0x01020304u, "identity read ignored its arguments");
    require(f->cpu.mem_read32(block + 0x20u) == 32u, "identity length was not published");
    require(f->cpu.mem_read32(block + 0x08u) == 5u, "identity command kind was not retained");
    require(f->api.stats().reads_unmapped == 0u && f->api.stats().identity_reads == 1u,
            "the identity block went through the token decode");
    require(f->cpu.gpr[3] == 1u, "identity read was not issued");
}

void failed_read() {
    auto f = std::make_unique<Fixture>();
    f->cpu.mem_write32(destination, 0xDEADBEEFu);
    f->read(destination, 32u, 0x100000u);
    require(f->api.stats().reads_failed == 1u, "a failed read was not recorded");
    require(f->cpu.mem_read32(block + 0x0Cu) == 0xFFFFFFFFu, "a failed read reported success");
    require(f->cpu.mem_read32(block + 0x20u) == 0u, "a failed read reported transferred bytes");
    require(f->cpu.mem_read32(destination) == 0xDEADBEEFu, "a failed read overwrote the destination");
}

void invalid_range() {
    auto f = std::make_unique<Fixture>();
    f->read(0x817FFFF0u, 32u);
    require(f->cpu.gpr[3] == 0u, "a cross-boundary destination was accepted");
    require(f->api.stats().reads_refused == 1u && f->api.stats().read_bytes == 0u, "an invalid range reached I/O");
    require(f->bus.diagnostics().unmapped_writes == 0u, "an invalid range wrote unmapped memory");
}

void zero_length() {
    auto f = std::make_unique<Fixture>();
    f->read(destination, 0u);
    require(f->cpu.gpr[3] == 1u, "a zero-byte request was rejected");
    require(f->cpu.mem_read32(block + 0x20u) == 0u, "a zero-byte count was not published");
}

void init_touches_nothing() {
    auto f = std::make_unique<Fixture>();
    const std::vector<u8> before = f->bus.window_bytes(GuestMemory::kImageBase, GuestMemory::kLegacyHeapEnd);
    f->cpu.gpr[13] = 0x805EC500u;
    f->call(kInit, {});
    require(f->bus.window_bytes(GuestMemory::kImageBase, GuestMemory::kLegacyHeapEnd) == before,
            "DVDInit wrote guest memory (there is nothing to initialise)");
    f->call(kDriveConfig, {});
    require(f->cpu.gpr[3] == 0u, "the drive configuration is not the retail value");
    require(f->bus.diagnostics().unmapped_reads == 0u && f->bus.diagnostics().unmapped_writes == 0u,
            "initialisation touched hardware registers");
}

void no_disc_device() {
    auto system = std::make_unique<System>();
    std::string error;
    require(system->mount_assets(shadow::backends::create_storage("host-directory", {mount_root(), {}}, error), identity_bytes(), error), "assets did not mount");
    system->enable_file_api();
    require(system->file_api() != nullptr, "the native file API is missing");
    // No directory table is published: the boot leaves the FST low-memory
    // words zero and keeps only the arena limit below BI2.
    gekko::BootConfiguration boot;
    boot.disc_header.assign(0x20u, 0u);
    system->apply_boot_configuration(boot);
    require(system->guest_memory().read32(0x80000038u) == 0u && system->guest_memory().read32(0x8000003Cu) == 0u,
            "the boot published a directory table location");
    {
        const u64 reserved = system->assets().names().reservation_bytes();
        const u32 expected = (0x817FE000u - static_cast<u32>((reserved + 31u) & ~u64{31u})) & ~31u;
        require(system->guest_memory().read32(0x80000034u) == expected,
                "the arena limit is not BI2 less the directory table's size");
    }
    // Nothing answers at the disc interface's register address: a read there
    // must fall through to the unmapped path.
    const auto before = system->guest_memory().diagnostics().unmapped_reads;
    (void)system->guest_memory().read32(0xCC006000u);
    require(system->guest_memory().diagnostics().unmapped_reads == before + 1u,
            "something is registered at the disc interface's MMIO base");
}

void failed_callback() {
    auto f = std::make_unique<Fixture>();
    f->read(destination, 32u, 0x100000u, callback_entry);
    f->run_until_stop();
    require(f->cpu.mem_read32(0x80003000u) == 0xFFFFFFFFu, "the callback was not told of the failure");
    require(f->cpu.mem_read32(0x80003008u) == 0xC0FFEEu, "the failure callback was truncated");
}

void invalid_callback() {
    auto f = std::make_unique<Fixture>();
    f->read(destination, 32u, 0u, 0x81234567u);
    require(f->cpu.gpr[3] == 0u && f->api.stats().read_bytes == 0u, "an invalid callback was accepted");
}

void open_file_directory_and_miss() {
    auto f = std::make_unique<Fixture>();
    const u32 info = block;
    f->put_string(path_text, "/DIR/sub/C.BIN");
    f->call(kOpen, {path_text, info});
    const s32 entry = f->assets.names().resolve("dir/sub/c.bin");
    require(entry > 0, "the fixture namespace lost dir/sub/c.bin");
    require(f->cpu.gpr[3] == 1u, "DVDOpen of an existing file failed");
    require(f->cpu.mem_read32(info + 0x30u) == f->assets.token_of(static_cast<u32>(entry)),
            "DVDOpen did not publish the file's token");
    require(f->cpu.mem_read32(info + 0x34u) == 0x9000u, "DVDOpen did not publish the file's length");
    require(f->cpu.mem_read32(info + 0x38u) == 0u && f->cpu.mem_read32(info + 0x0Cu) == 0u,
            "DVDOpen left the callback or state set");
    f->cpu.mem_write32(info + 0x30u, 0x5A5A5A5Au);
    f->put_string(path_text, "dir");
    f->call(kOpen, {path_text, info});
    require(f->cpu.gpr[3] == 0u && f->cpu.mem_read32(info + 0x30u) == 0x5A5A5A5Au,
            "DVDOpen accepted a directory or wrote its file info");
    f->put_string(path_text, "dir/missing.bin");
    f->call(kOpen, {path_text, info});
    require(f->cpu.gpr[3] == 0u && f->cpu.mem_read32(info + 0x30u) == 0x5A5A5A5Au,
            "DVDOpen of a missing file succeeded or wrote its file info");
    require(f->api.stats().open_misses == 1u && f->api.missed_paths().size() == 1u &&
                f->api.missed_paths()[0] == "dir/missing.bin",
            "the missing path was not recorded");
    f->put_string(path_text, "./dir/../dir/b.bin");
    f->call(kOpen, {path_text, info});
    require(f->cpu.gpr[3] == 1u && f->cpu.mem_read32(info + 0x34u) == 0x123u, "dot components did not resolve");
}

void read_async_through_file_info() {
    auto f = std::make_unique<Fixture>();
    const u32 info = block;
    f->put_string(path_text, "dir/b.bin");
    f->call(kOpen, {path_text, info});
    require(f->cpu.gpr[3] == 1u, "open failed");
    f->call(kReadAsync, {info, destination, 0x40u, 0x20u, callback_entry, 2u});
    f->run_until_stop();
    const std::vector<u8> expected = asset_bytes(0x123u, 3u);
    require(std::equal(expected.begin() + 0x20, expected.begin() + 0x60,
                       f->bus.window_bytes(destination, destination + 0x40u).begin()),
            "DVDReadAsyncPrio delivered the wrong bytes");
    require(f->cpu.mem_read32(info + 0x38u) == callback_entry, "the file info does not carry the callback");
    require(f->cpu.mem_read32(0x80003000u) == 0x40u && f->cpu.mem_read32(0x80003004u) == info,
            "the callback did not receive (bytes, file info)");
    require(f->cpu.mem_read32(0x8000300Cu) == 1u, "DVDReadAsyncPrio did not return TRUE");
    // Synchronous read: bytes transferred as the result.
    f->cpu.clear_stop();
    f->call(kReadSync, {info, destination, 0x20u, 0x100u, 2u});
    require(f->cpu.gpr[3] == 0x20u, "DVDReadPrio did not return the bytes transferred");
    f->call(kTransferred, {info});
    require(f->cpu.gpr[3] == 0x20u, "DVDGetTransferredSize disagrees with the read");
    // A read outside the file is the SDK's assertion: the machine stops.
    f->call(kReadAsync, {info, destination, 0x40u, 0x200u, 0u, 2u});
    require(f->cpu.stopped() && f->cpu.stop().reason == StopReason::ExecutionFault,
            "a read outside the file did not stop the machine");
}

void status_close_and_cancel() {
    auto f = std::make_unique<Fixture>();
    f->cpu.mem_write32(block + 0x0Cu, 3u);
    f->call(kBlockStatus, {block});
    require(f->cpu.gpr[3] == 1u, "COVER_CLOSED did not fold to BUSY");
    f->cpu.mem_write32(block + 0x0Cu, 0xFFFFFFFFu);
    f->call(kBlockStatus, {block});
    require(f->cpu.gpr[3] == 0xFFFFFFFFu, "FATAL was not reported");
    f->call(kDriveStatus, {});
    require(f->cpu.gpr[3] == 0u, "the drive is not idle");
    f->call(kClose, {block});
    require(f->cpu.gpr[3] == 1u, "DVDClose did not return TRUE");
    f->cpu.mem_write32(block + 0x0Cu, 0u);
    f->call(kCancel, {block});
    require(f->cpu.gpr[3] == 0u, "DVDCancel of a finished block did not return 0");
    require(f->api.stats().anomalies == 0u, "a finished block counted as an anomaly");
    f->call(kCancelAsync, {block, callback_entry});
    f->run_until_stop();
    require(f->cpu.mem_read32(0x80003000u) == 0u && f->cpu.mem_read32(0x80003004u) == block,
            "the cancel callback did not receive (0, block)");
    require(f->cpu.mem_read32(0x8000300Cu) == 1u && f->cpu.gpr[1] == stack, "DVDCancelAsync did not return TRUE");
    f->cpu.clear_stop();
    f->cpu.mem_write32(block + 0x0Cu, 8u);
    f->call(kCancelAsync, {block, callback_entry});
    require(f->cpu.gpr[3] == 1u && !f->cpu.stopped(), "state 8 made a cancel callback");
    require(f->api.stats().anomalies == 1u, "a state the API never produces was not counted");
}

void configuration_flags() {
    auto f = std::make_unique<Fixture>();
    f->call(kAutoFatal, {1u});
    require(f->cpu.gpr[3] == 0u, "fatal messaging did not start off");
    f->call(kAutoFatal, {0u});
    require(f->cpu.gpr[3] == 1u, "fatal messaging lost its setting");
    f->call(kAutoInvalidation, {0u});
    require(f->cpu.gpr[3] == 1u, "auto invalidation did not start on (the SDK's default)");
    f->call(kDiskId, {});
    require(f->cpu.gpr[3] == 0x80000000u, "the disk ID is not the boot block's");
    f->call(kCheckDisk, {});
    require(f->cpu.gpr[3] == 1u, "the disk is not reported present");
    f->call(kTestAlarm, {0x80587B70u});
    require(f->cpu.gpr[3] == 0u, "an alarm was claimed by the file API, which owns none");
}

void console_reboot_stops() {
    for (const u32 entry : {kPrepareReset, kCancelStream}) {
        auto f = std::make_unique<Fixture>();
        f->call(entry, {0u, 0u});
        require(f->cpu.stopped() && f->cpu.stop().reason == StopReason::ExecutionFault,
                "the console reboot path did not stop the machine");
    }
}

// extract/native-owners.txt must list every address an owner installs: the file
// API, the native interrupt delivery, the native video, the native controller
// input, the native audio platform, the frame limiter, the native GX sync, the
// native memory card, the native IPL services and the reset controls - and
// nothing else.
template <class Owner>
void add_owner_addresses(std::set<u32>& installed, std::size_t& total) {
    std::size_t count = 0;
    const auto* addresses = Owner::addresses(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (!installed.insert(addresses[i].address).second) {
            char text[96];
            std::snprintf(text, sizeof(text), "an address is owned twice: 0x%08X %s", addresses[i].address, addresses[i].name);
            require(false, text);
        }
    }
    total += count;
}

void owner_list_matches(const char* owners_path) {
    std::ifstream in(owners_path);
    require(static_cast<bool>(in), "cannot read the native owners list");
    std::set<u32> listed;
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        listed.insert(static_cast<u32>(std::strtoul(line.c_str(), nullptr, 0)));
    }
    std::set<u32> installed;
    std::size_t total = 0;
    add_owner_addresses<NativeFileApi>(installed, total);
    add_owner_addresses<NativeInterrupts>(installed, total);
    add_owner_addresses<NativeVideo>(installed, total);
    add_owner_addresses<NativeAudio>(installed, total);
    add_owner_addresses<NativeFrameLimiter>(installed, total);
    add_owner_addresses<NativeObjectRange>(installed, total);
    add_owner_addresses<NativeMotionStore>(installed, total);
    add_owner_addresses<NativeSoundBanks>(installed, total);
    add_owner_addresses<NativeGxSync>(installed, total);
    add_owner_addresses<gx_state::Owners>(installed, total);
    add_owner_addresses<rw_raster::Owners>(installed, total);
    add_owner_addresses<NativeCard>(installed, total);
    add_owner_addresses<SaveStore>(installed, total);
    add_owner_addresses<NativeIpl>(installed, total);
    add_owner_addresses<NativeReset>(installed, total);
    add_owner_addresses<NativeBoot>(installed, total);
    add_owner_addresses<NativeDiscError>(installed, total);
    add_owner_addresses<NativeLinkGroups>(installed, total);
    add_owner_addresses<NativeDirectoryScan>(installed, total);
    add_owner_addresses<NativeCriFileDevice>(installed, total);
    add_owner_addresses<NativeRtGcnFs>(installed, total);
    add_owner_addresses<GameMemory>(installed, total);
    add_owner_addresses<reverb::Owners>(installed, total);
    add_owner_addresses<axfx_effects::Owners>(installed, total);
    add_owner_addresses<ax_registration::Owners>(installed, total);
    add_owner_addresses<ax_voice::Owners>(installed, total);
    add_owner_addresses<ax_sync::Owners>(installed, total);
    add_owner_addresses<ax_frame::Owners>(installed, total);
    add_owner_addresses<os_arena_clock::Owners>(installed, total);
    add_owner_addresses<os_debug::Owners>(installed, total);
    add_owner_addresses<os_report::Owners>(installed, total);
    add_owner_addresses<trk::Owners>(installed, total);
    add_owner_addresses<gx_display::Owners>(installed, total);
    add_owner_addresses<gx_texobj::Owners>(installed, total);
    add_owner_addresses<os_settings::Owners>(installed, total);
    add_owner_addresses<os_heap::Owners>(installed, total);
    add_owner_addresses<os_panic::Owners>(installed, total);
    add_owner_addresses<os_misc::Owners>(installed, total);
    add_owner_addresses<os_machine_check::Owners>(installed, total);
    add_owner_addresses<ax_mix_alloc::Owners>(installed, total);
    add_owner_addresses<math_affine::Owners>(installed, total);
    add_owner_addresses<rw_image::Owners>(installed, total);
    add_owner_addresses<rw_pixel::Owners>(installed, total);
    add_owner_addresses<os_threads::Owners>(installed, total);
    add_owner_addresses<os_font::Owners>(installed, total);
    add_owner_addresses<vec_math::Owners>(installed, total);
    add_owner_addresses<mix_entries::Owners>(installed, total);
    add_owner_addresses<sound_tables::Owners>(installed, total);
    add_owner_addresses<gcax_midi::Owners>(installed, total);
    add_owner_addresses<se_manager::Owners>(installed, total);
    add_owner_addresses<NativeAdxOutput>(installed, total);
    add_owner_addresses<NativeSj>(installed, total);
    add_owner_addresses<NativeSvm>(installed, total);
    add_owner_addresses<rw_device_state::Owners>(installed, total);
    add_owner_addresses<rw_registry::Owners>(installed, total);
    // The return stubs of the native workers: installed by NativeJobs / NativeCri, which keep no owner table.
    for (const u32 stub : {0x80376218u, 0x8037621Cu, 0x80376220u}) require(installed.insert(stub).second, "a return stub is also an owner");
    // Owners of the lifted build only live in the second list (the reference build keeps those guest bodies); most of its
    // rows belong to modules with no address table, so the first list alone is compared with what remains.
    {
        std::string second(owners_path);
        const std::size_t slash = second.find_last_of("/\\");
        second = second.substr(0, slash + 1u) + "native-owners-r2.txt";
        std::ifstream r2(second);
        require(static_cast<bool>(r2), "cannot read the second native owners list");
        for (std::string line; std::getline(r2, line);) {
            if (line.empty() || line[0] == '#') continue;
            installed.erase(static_cast<u32>(std::strtoul(line.c_str(), nullptr, 0)));
        }
    }
    if (installed != listed) {
        // Name the rows: a bare "differs" leaves the reader to bisect 200+ owners.
        for (const u32 address : listed)
            if (installed.count(address) == 0u) std::printf("  listed, not installed: 0x%08X\n", address);
        for (const u32 address : installed)
            if (listed.count(address) == 0u) std::printf("  installed, not listed: 0x%08X\n", address);
    }
    require(installed == listed,
            "extract/native-owners.txt differs from the runtime owner lists (file API, interrupts, "
            "video, audio, frame limiter, object range, motion store, sound banks, GX sync, card, IPL services, reset controls, disc-error module, directory scan, CRI file device, RtGCNFS, game memory, room reverb, "
            "sound tables)");
}

// extract/platform-sites.txt (what the generator lowers) must equal the site
// tables the native subsystems answer from (NativeVideo):
// address, instruction word and kind, each site in exactly one table. A site
// of kind `dead` (a store the generator drops: its register has no consumer,
// STAGE6_PI_MI.md D4) is answered by no subsystem.
void platform_sites_match(const char* sites_path) {
    std::ifstream in(sites_path);
    require(static_cast<bool>(in), "cannot read the platform sites list");
    std::set<std::string> listed;
    std::set<u32> dead;
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string site, word, kind;
        fields >> site >> word >> kind;
        if (kind == "dead") {
            require(dead.insert(static_cast<u32>(std::strtoul(site.c_str(), nullptr, 0))).second,
                    "a dead platform site is listed twice");
            continue;
        }
        char row[96];
        std::snprintf(row, sizeof(row), "%08X %08X %s",
                      static_cast<unsigned>(std::strtoul(site.c_str(), nullptr, 0)),
                      static_cast<unsigned>(std::strtoul(word.c_str(), nullptr, 0)), kind.c_str());
        require(listed.insert(row).second, "a platform site is listed twice");
    }
    std::set<std::string> runtime;
    std::set<u32> addresses;
    const auto add_sites = [&](const auto* sites, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            char row[96];
            std::snprintf(row, sizeof(row), "%08X %08X %s", sites[i].address, sites[i].instruction, sites[i].kind);
            require(runtime.insert(row).second, "the runtime lists a platform site twice");
            require(addresses.insert(sites[i].address).second, "two subsystems answer one platform site");
        }
    };
    std::size_t count = 0;
    const NativeVideo::Site* video_sites = NativeVideo::platform_sites(count);
    add_sites(video_sites, count);
    require(listed == runtime, "extract/platform-sites.txt differs from the native site tables");
    for (const u32 site : dead) {
        require(addresses.count(site) == 0u, "a subsystem answers a dead platform site");
    }
}
}  // namespace

int main(int argc, char** argv) {
    int failed = 0;
    const std::pair<const char*, void (*)()> tests[] = {
        {"effective-address copy", effective_address},
        {"token + offset selects the right bytes", offset_within_asset},
        {"the 32-byte tail past an asset zero-fills", tail_past_end_of_asset_zero_fills},
        {"an unmapped token is counted, not aliased", unmapped_token_is_counted_not_aliased},
        {"a request leaving its asset is refused", a_request_leaving_its_asset_is_refused},
        {"callback byte count and continuation", callback_continuation},
        {"identity arguments", identity_arguments},
        {"nested callback request", nested_request},
        {"read failure", failed_read},
        {"destination range", invalid_range},
        {"zero-length request", zero_length},
        {"DVDInit initialises nothing", init_touches_nothing},
        {"no disc device", no_disc_device},
        {"failed completion callback", failed_callback},
        {"invalid callback", invalid_callback},
        {"open: file, directory, miss, dot components", open_file_directory_and_miss},
        {"read through a file info", read_async_through_file_info},
        {"status, close and cancel", status_close_and_cancel},
        {"configuration flags", configuration_flags},
        {"the console reboot path stops", console_reboot_stops},
    };
    for (const auto& test : tests) {
        try {
            test.second();
            std::printf("PASS %s\n", test.first);
        } catch (const std::exception& e) {
            ++failed;
            std::printf("FAIL %s: %s\n", test.first, e.what());
        }
    }
    if (argc > 1) {
        try {
            owner_list_matches(argv[1]);
            std::printf("PASS the owner list matches the API\n");
        } catch (const std::exception& e) {
            ++failed;
            std::printf("FAIL the owner list: %s\n", e.what());
        }
    }
    if (argc > 2) {
        try {
            platform_sites_match(argv[2]);
            std::printf("PASS the platform sites list matches NativeVideo and NativeGxSync\n");
        } catch (const std::exception& e) {
            ++failed;
            std::printf("FAIL the platform sites list: %s\n", e.what());
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(gekko::test_support::scratch_directory("gekko-native-file-api-tests"), ec);
    return failed == 0 ? 0 : 1;
}
