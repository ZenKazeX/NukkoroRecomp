#pragma once

// Opt-in observation only. This fragment is NOT a resumable System savestate:
// generated guest calls can still be on the native stack at these boundaries.
#include "native_bootstrap.h"
#include "native_pc_boot_original_contract.h"
#include "native_pc_boot_constructor_audit.h"
#include "gekko/system.h"
#include "gekko/savestate.h"
#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <share.h>
#else
#include <unistd.h>
#endif

namespace shadow::pc_bootstrap {

class ContractObserver {
    struct Boundary { gekko::u32 address; const char* name; bool attempted = false; };
    // 80003170 is a resume label, not an HLE entry in the production generator.
    // Do not install a silent, never-firing observer there.
    std::array<Boundary, 9> boundaries_{{
        {0x80003154u, "start"}, {0x80003340u, "crt-entry"},
        {0x80370BF0u, "dbinit-entry"},
        {0x803796CCu, "constructor-entry"},  // liveness-roots: value-only (the opt-in observer; the native boot serves it)
        {0x800510C0u, "main-entry"}, {0x801E258Cu, "game-init-entry"},
        {0x801E2210u, "game-runtime-entry"},
        // OSInit's entry and its return into __start (the native boot's continuation owner has the same address).
        {0x80370E68u, "osinit-entry"}, {0x80003268u, "osinit-return"}}};
    std::filesystem::path directory_;
    bool crt_route_eligible_ = false;
    ConstructorAudit constructor_audit_;

    class ExclusiveFile {
        std::FILE* file_ = nullptr;
    public:
        explicit ExclusiveFile(const std::filesystem::path& path) {
#ifdef _WIN32
            int fd = -1;
            (void)_wsopen_s(&fd, path.c_str(), _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY,
                           _SH_DENYRW, _S_IREAD | _S_IWRITE);
            if (fd >= 0) { file_ = _fdopen(fd, "wb"); if (!file_) _close(fd); }
#else
            const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
            if (fd >= 0) { file_ = ::fdopen(fd, "wb"); if (!file_) ::close(fd); }
#endif
            if (!file_) throw std::runtime_error("boot contract: refusing unavailable/existing output " + path.string());
        }
        ~ExclusiveFile() { if (file_) std::fclose(file_); }
        ExclusiveFile(const ExclusiveFile&) = delete;
        void bytes(const void* data, std::size_t size) {
            if (size && std::fwrite(data, 1u, size, file_) != size)
                throw std::runtime_error("boot contract: output write failed");
        }
        template<class T> void pod(const T& value) { bytes(&value, sizeof(value)); }
        void block(const void* data, std::size_t size) {
            pod(static_cast<gekko::u64>(size)); bytes(data, size);
        }
        void finish() {
            auto* file = file_; file_ = nullptr;
            if (std::fclose(file) != 0) throw std::runtime_error("boot contract: output close failed");
        }
    };
    static gekko::u32 word(const DolImage& dol, gekko::u32 address, bool executable = true) {
        for (const auto& section : dol.sections) {
            if ((executable && !section.executable) || address < section.address) continue;
            const auto delta = std::uint64_t(address) - section.address;
            if (delta + 4u > section.size) continue;
            const auto offset = std::uint64_t(section.file_offset) + delta;
            if (offset + 4u > dol.bytes.size()) break;
            return (gekko::u32(dol.bytes[offset]) << 24u) |
                   (gekko::u32(dol.bytes[offset + 1u]) << 16u) |
                   (gekko::u32(dol.bytes[offset + 2u]) << 8u) | dol.bytes[offset + 3u];
        }
        throw std::runtime_error("boot contract: missing original executable word");
    }
    static void verify_original(const DolImage& dol) {
        if (dol.entry_point != 0x80003154u)
            throw std::runtime_error("boot contract: requires the original DOL entry");
        // Every word of __start and __init_data, including all intervening
        // branches/stores between CRT return and DBInit; retail PAL main.dol
        // SHA256 fde4fa6f81a60313b710161c196dc51c2260be62251ee02775d5eee06f9d55af.
        constexpr std::array<gekko::u32, 87> start{
            0x4800015Du,0x480002A9u,0x3800FFFFu,0x9421FFF8u,0x90010004u,0x90010000u,
            0x480001D5u,0x38000000u,0x3CC08000u,0x38C60044u,0x90060000u,0x3CC08000u,
            0x38C600F4u,0x80C60000u,0x28060000u,0x4182000Cu,0x80E6000Cu,0x48000024u,
            0x3CA08000u,0x38A50034u,0x80A50000u,0x28050000u,0x4182004Cu,0x3CE08000u,
            0x38E730E8u,0x80E70000u,0x38A00000u,0x28070002u,0x41820024u,0x28070003u,
            0x38A00001u,0x41820018u,0x28070004u,0x40820020u,0x38A00002u,0x4BFFFF61u,
            0x48000014u,0x3CC0803Au,0x38C6F8E0u,0x7CC803A6u,0x4E800021u,0x3CC08000u,
            0x38C600F4u,0x80A60000u,0x28050000u,0x41A20050u,0x80C50008u,0x28060000u,
            0x41A20044u,0x7CC53214u,0x81C60000u,0x280E0000u,0x41820034u,0x39E60004u,
            0x7DC903A6u,0x38C60004u,0x80E60000u,0x7CE72A14u,0x90E60000u,0x4200FFF0u,
            0x3CA08000u,0x38A50034u,0x55E70034u,0x90E50000u,0x4800000Cu,0x39C00000u,
            0x39E00000u,0x4836D991u,0x4836DC05u,0x3C808000u,0x388430E6u,0xA0640000u,
            0x70658000u,0x41820010u,0x70637FFFu,0x28030001u,0x40820008u,0x4BFFFE79u,
            0x4BFFFEC1u,0x28030001u,0x40820008u,0x4839C6E1u,0x48376411u,0x7DC37378u,
            0x7DE47B78u,0x4804DE19u,0x483A0858u};
        constexpr auto& crt = original::kCrtWords;
        for (std::size_t i = 0; i < start.size(); ++i)
            if (word(dol, 0x80003154u + gekko::u32(i * 4u)) != start[i])
                throw std::runtime_error("boot contract: unknown original __start words");
        for (std::size_t i = 0; i < crt.size(); ++i)
            if (word(dol, 0x80003340u + gekko::u32(i * 4u)) != crt[i])
                throw std::runtime_error("boot contract: unknown original CRT words");
        const auto guard = [&](gekko::u32 address, const auto& words, bool executable = true) {
            for (std::size_t i = 0; i < words.size(); ++i)
                if (word(dol, address + gekko::u32(i * 4u), executable) != words[i])
                    throw std::runtime_error("boot contract: unknown constructor/main/runtime original words");
        };
        guard(0x803796ACu, original::kConstructorCode);  // liveness-roots: value-only (original words read as data)
        guard(0x800510C0u, original::kMainCode);
        guard(0x801E258Cu, original::kGameInitCode);
        guard(0x801E2210u, original::kRuntimeEntryCode);
        guard(ConstructorAudit::kTableAddress, original::kConstructorTable, false);
    }
    static gekko::u32 ram_word(gekko::System& system, gekko::u32 address) {
        const gekko::u8* const at = system.guest_memory().peek(gekko::GcAddr(address), 4u);
        if (at == nullptr) throw std::runtime_error("boot contract: route pointer outside the game's mapped memory");
        return (gekko::u32(at[0]) << 24u) | (gekko::u32(at[1]) << 16u) | (gekko::u32(at[2]) << 8u) | at[3];
    }
    bool ordinary_crt_route(gekko::System& system) {
        const auto bi2 = ram_word(system, 0x800000F4u);
        // Nonzero boot debugger/argument tables execute extra owners. Refuse
        // their memory-scope equivalence instead of silently filtering bytes.
        return system.guest_memory().mapped(gekko::GcAddr(bi2), 16u) &&
               ram_word(system, bi2 + 8u) == 0u && ram_word(system, bi2 + 12u) == 0u;
    }
    static bool constructor_table_matches(gekko::System& system) {
        for (std::size_t i = 0; i < original::kConstructorTable.size(); ++i)
            if (ram_word(system, ConstructorAudit::kTableAddress + gekko::u32(i * 4u)) !=
                original::kConstructorTable[i]) return false;
        return true;
    }
    void write_constructor_audit() {
        ExclusiveFile trace(directory_ / "constructor-order.gfctors");
        trace.bytes("GFCTOR01", 8u); trace.pod(gekko::u32{1u});
        trace.pod(static_cast<gekko::u32>(ConstructorAudit::kExpected));
        trace.pod(static_cast<gekko::u32>(constructor_audit_.count()));
        trace.pod(constructor_audit_.overflow());
        // Bits describe observed completeness only, never full handoff.
        const gekko::u32 flags = (constructor_audit_.started() ? 1u : 0u) |
            (constructor_audit_.sealed() ? 2u : 0u) | (constructor_audit_.table_matches() ? 4u : 0u);
        trace.pod(flags);
        for (std::size_t i = 0; i < constructor_audit_.count(); ++i) {
            const auto& record = constructor_audit_.records()[i];
            trace.pod(record.index); trace.pod(record.target); trace.pod(record.table_slot);
            trace.pod(record.return_address); trace.pod(record.game_ticks);
        }
        trace.finish();
    }
    void observe(gekko::System& system, std::size_t index) noexcept {
        auto& boundary = boundaries_[index];
        if (boundary.attempted) return;
        boundary.attempted = true; // Failure is also terminal; no retry growth.
        try {
            if (index == 1u) crt_route_eligible_ = ordinary_crt_route(system);
            if (index == 3u) constructor_audit_.begin(constructor_table_matches(system));
            if (index == 4u) {
                constructor_audit_.seal(constructor_table_matches(system));
                write_constructor_audit();
            }
            const bool crt_memory_post = index == 2u && boundaries_[1].attempted &&
                                         crt_route_eligible_ && ordinary_crt_route(system);
            const auto stem = directory_ / boundary.name;
            auto fragment_path = stem; fragment_path += ".gfboot";
            ExclusiveFile data(fragment_path);
            data.bytes("GFBCON01", 8u);
            // Schema 1; the CPU and device blocks below are the savestate's.
            data.pod(gekko::u32{1u}); data.pod(gekko::save_state_version());
            data.pod(boundary.address); data.pod(system.cpu().pc);
            data.pod(system.game_ticks());
            data.pod(gekko::u32{crt_memory_post ? 1u : 0u});
            std::vector<gekko::u8> fragment;
            system.cpu().save_state(fragment);
            data.block(fragment.data(), fragment.size());
            // The game's address window (every region, its bounds and its
            // committed bytes, the native sound stores among them), written while
            // guest execution is suspended in the hook.
            fragment.clear();
            system.guest_memory().save_state(fragment);
            data.bytes(reinterpret_cast<const char*>(fragment.data()), fragment.size());
            // Reuse each maintained serializer, never hand-transcribe device
            // state. A single temporary is reused, not retained per boundary.
            const auto device = [&](const auto& owner) {
                fragment.clear(); owner.save_state(fragment);
                data.block(fragment.data(), fragment.size());
            };
            device(system.interrupts()); device(system.video());
            // Controller input is the lifted peripheral layer (docs/lift-20261006/L1_INPUT.md).
            fragment.clear(); system.peripheral().save(fragment);
            data.block(fragment.data(), fragment.size());
            device(system.audio()); device(system.gx_sync());
#if !defined(SHADOW_RENDER_LIFTED)  // G5
            device(system.immediate());
#else
            fragment.clear(); gekko::System::save_idle_immediate_state(fragment);  // G5: the idle block
            data.block(fragment.data(), fragment.size());
#endif
            device(system.card()); device(system.ipl());
            data.finish();
            // Metadata is written only after the fragment closes successfully.
            // Names/fields are fixed; no mutable string is embedded in JSON.
            const std::string metadata = std::string("{\"schema\":\"GFBCON01\",\"capture_complete\":true,"
                "\"boundary\":\"") + boundary.name + "\",\"declared_address\":" +
                std::to_string(boundary.address) + ",\"cpu_pc\":" + std::to_string(system.cpu().pc) +
                ",\"game_ticks\":" + std::to_string(system.game_ticks()) +
                ",\"source\":\"original-translated-boot\",\"cpu_save_version\":" +
                std::to_string(gekko::save_state_version()) + ","
                "\"crt_return_address\":2147496304,\"crt_memory_post_candidate\":" +
                (crt_memory_post ? "true" : "false") +
                ",\"constructor_order_complete\":" + (constructor_audit_.valid() ? "true" : "false") +
                ",\"full_handoff_proven\":false,\"resumable\":false,"
                "\"gaps\":[\"active-native-continuation\",\"native-event-callback-ownership\","
                "\"gx-cp-pe-capture-and-host-gpu-owners\",\"asset-worker-audio-host-owners\"],"
                "\"system_save_not_attempted\":\"native guest call stack may be active\"}\n";
            auto metadata_path = stem; metadata_path += ".json";
            ExclusiveFile json(metadata_path);
            json.bytes(metadata.data(), metadata.size()); json.finish();
            std::fprintf(stderr, "[native-boot-contract] %s observed; incomplete non-resumable fragment\n", boundary.name);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "[native-boot-contract] %s capture refused: %s\n", boundary.name, error.what());
        }
    }
public:
    ContractObserver(const std::filesystem::path& directory, const DolImage& original)
        : directory_(directory) {
        verify_original(original); // No filesystem mutation or hooks before guard.
        std::error_code error;
        if (!std::filesystem::create_directory(directory_, error))
            throw std::runtime_error("boot contract: output directory must be new with an existing parent: " + directory_.string());
    }
    void install(gekko::System& system) {
        for (std::size_t i = 0; i < boundaries_.size(); ++i)
            system.add_hle_observer(boundaries_[i].address,
                [this, &system, i](gekko::Cpu&) { observe(system, i); },
                "native PC boot contract observation");
        // Install once per unique original entry, not once per table slot.
        // The original walker still executes every indirect call unchanged.
        for (std::size_t i = 0; i < ConstructorAudit::kExpected; ++i) {
            const auto target = original::kConstructorTable[i];
            bool duplicate = false;
            for (std::size_t j = 0; j < i; ++j) duplicate |= original::kConstructorTable[j] == target;
            if (duplicate) continue;
            system.add_hle_observer(target, [this, target](gekko::Cpu& cpu) {
                constructor_audit_.observe(target, cpu.gpr[31], cpu.lr, cpu.game_clock().now());
            }, "native PC original constructor order observation");
        }
    }
};

} // namespace shadow::pc_bootstrap
