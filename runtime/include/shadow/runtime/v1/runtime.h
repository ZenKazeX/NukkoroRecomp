// The Shadow runtime contract, version 1 - what the game core asks of the platform, and nothing about how a platform
// provides it. Specification: docs/RUNTIME_CONTRACT.md (this header is its normative summary).
//
//     Shadow game core            (translated game code, its compatibility binding to the guest ABI, the native
//            |                     subsystems: assets, audio mixing, rendering's frame packets ...)
//     this contract               (versioned C++ interfaces; plain values, spans and status codes)
//            |
//     a backend                   (chosen at start-up from the backend registry, runtime/backends)
//            |
//     the host, or the original GameCube environment (e.g. the console's disc image)
//
// Rules every service of the contract follows:
//  * Versioning: a backend reports the contract major / minor it implements (BackendInfo). The core requires the same
//    major and a minor at least its own; anything else is refused at selection, never at the first use.
//  * Capabilities: a backend states what it can do as bits per service; the core states what it requires; a missing
//    required bit refuses the backend at selection with the missing bits named (negotiate()). Optional bits change
//    only performance or reporting, never what the game observes.
//  * Status: every operation that can fail returns a Status; nothing throws across the contract; a failure the game
//    has no path for is reported to the core, which decides (the binding stops the machine with the message rather
//    than inventing a game-visible error).
//  * No GameCube types: no guest addresses, guest pointers, big-endian layouts, disc offsets, DVD / CARD / PAD
//    structures or handles cross this boundary. Those live in the core's compatibility binding (the guest-ABI side)
//    or inside a backend that implements the original environment.
//  * Ownership: the core owns the backend object it selected for its whole session; spans a backend returns stay
//    valid until the backend is destroyed unless a service says otherwise.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace shadow::runtime::v1 {

// Byte ranges across the contract (C++17-compatible; no ownership).
struct Bytes {
    std::byte* data = nullptr;
    std::size_t size = 0;
};
struct ConstBytes {
    const std::byte* data = nullptr;
    std::size_t size = 0;
    bool empty() const noexcept { return size == 0; }
};

inline constexpr std::uint32_t kContractMajor = 1;
inline constexpr std::uint32_t kContractMinor = 0;

enum class Status : std::uint8_t {
    Ok = 0,
    NotFound,         // the named thing does not exist
    InvalidArgument,  // a request outside the service's rules (e.g. a key the backend never gave out)
    Unsupported,      // a capability the backend did not state
    IoError,          // the backend's medium failed (the message says how)
};

constexpr const char* to_string(Status s) noexcept {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::NotFound: return "not found";
        case Status::InvalidArgument: return "invalid argument";
        case Status::Unsupported: return "unsupported";
        case Status::IoError: return "I/O error";
    }
    return "?";
}

struct BackendInfo {
    std::string_view name;         // the registry name the backend is selected by
    std::string_view description;  // one line for logs and reports
    std::uint32_t contract_major = 0;
    std::uint32_t contract_minor = 0;
};

// The outcome of matching a backend against the core's requirements.
struct Negotiation {
    bool accepted = false;
    std::string reason;  // empty when accepted; else what is missing or mismatched
};

inline Negotiation negotiate(const BackendInfo& info, std::uint32_t offered, std::uint32_t required,
                             std::string_view (*bit_name)(std::uint32_t)) {
    Negotiation n;
    if (info.contract_major != kContractMajor || info.contract_minor < kContractMinor) {
        n.reason = std::string(info.name) + " implements contract " + std::to_string(info.contract_major) + "." +
                   std::to_string(info.contract_minor) + ", the core needs " + std::to_string(kContractMajor) + "." +
                   std::to_string(kContractMinor) + " or a later minor";
        return n;
    }
    const std::uint32_t missing = required & ~offered;
    if (missing != 0u) {
        n.reason = std::string(info.name) + " lacks required capabilities:";
        for (std::uint32_t bit = 1u; bit != 0u; bit <<= 1u) {
            if ((missing & bit) != 0u) n.reason += std::string(" ") + std::string(bit_name(bit));
        }
        return n;
    }
    n.accepted = true;
    return n;
}

// ---------------------------------------------------------------------------------------------------------------------
// Storage: the game's read-only data tree (the disc's files)
// ---------------------------------------------------------------------------------------------------------------------
//
// The tree is enumerated once, at mount, on the mounting thread, before any read. Names are the medium's own
// spelling (printable ASCII without '/'); order is the backend's and carries no meaning - the core sorts. Each file
// carries a key, an opaque value the backend gives out and accepts back; it is not an offset the core may compute
// with. The tree does not change during a session.
//
// read(key, offset, dest, got) copies min(dest.size(), size - offset) bytes of the file starting at `offset` into
// dest and ZERO-FILLS the rest of dest; got = the bytes that came from the file. An offset at or past the end gives
// got = 0 and an all-zero dest. This is the asset runtime's recovered rule 5 (gekko/asset_runtime.h): every issuer in
// the game rounds its length up to 32 bytes. A read is complete when it returns: no partial completion, no
// callbacks, no latency the backend adds on purpose (rules 2 and 6). read() and view() may be called from several
// threads at once, for the same key or different ones; a backend must make that safe.
//
// view(key) returns the whole file in place when the backend has it mapped (capability StorageView), else an empty
// span; the span stays valid until the backend is destroyed. It is an optimisation only: a core that gets an empty
// span reads instead, and what the game sees is the same.
enum StorageCapability : std::uint32_t {
    StorageEnumerate = 1u << 0,  // enumerate() gives the whole tree with sizes and keys (required)
    StorageRead = 1u << 1,       // read() as specified (required)
    StorageView = 1u << 2,       // view() can return files in place (optional)
};

constexpr std::string_view storage_capability_name(std::uint32_t bit) noexcept {
    switch (bit) {
        case StorageEnumerate: return "enumerate";
        case StorageRead: return "read";
        case StorageView: return "view";
        default: return "unknown";
    }
}

struct StorageNode {
    std::string name;
    bool directory = false;
    std::uint64_t size = 0;  // files
    std::uint64_t key = 0;   // files: the backend's key
    std::vector<StorageNode> children;
};

class Storage {
public:
    virtual ~Storage() = default;
    virtual BackendInfo info() const noexcept = 0;
    virtual std::uint32_t capabilities() const noexcept = 0;
    // The whole tree under a root directory node (its name is empty).
    virtual Status enumerate(StorageNode& root, std::string& message) = 0;
    virtual Status read(std::uint64_t key, std::uint64_t offset, Bytes dest, std::uint64_t& got) = 0;
    virtual ConstBytes view(std::uint64_t key) = 0;
};

inline constexpr std::uint32_t kStorageRequired = StorageEnumerate | StorageRead;

}  // namespace shadow::runtime::v1
