// Storage backends (storage_backends.h).
#include <cctype>
#include "shadow/backends/storage_backends.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <system_error>

#include "gekko/asset_mount.h"
#include "gekko/asset_providers.h"

namespace shadow::backends {
namespace {

// A path as UTF-8 text (u8string is std::u8string from C++20 on).
std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

using runtime::v1::BackendInfo;
using runtime::v1::Status;
using runtime::v1::StorageNode;

// Copies `size - offset` bytes (at most dest.size) from `src` and zero-fills the rest of dest (the contract's read).
void copy_zero_fill(const std::byte* src, std::uint64_t size, std::uint64_t offset, runtime::v1::Bytes dest,
                    std::uint64_t& got) {
    got = offset < size ? std::min<std::uint64_t>(dest.size, size - offset) : 0u;
    if (got) std::memcpy(dest.data, src + offset, static_cast<std::size_t>(got));
    if (got < dest.size) std::memset(dest.data + got, 0, static_cast<std::size_t>(dest.size - got));
}

// The contract's name rule (shadow/runtime/v1/runtime.h): printable ASCII, no '/' or '\\', not "." or "..".
// A name outside it is refused, never transcoded.
bool name_ok(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    for (const char ch : name) {
        const auto b = static_cast<unsigned char>(ch);
        if (b < 0x20u || b > 0x7Eu || b == '/' || b == '\\') return false;
    }
    return true;
}

// ---- host-directory ----------------------------------------------------------------------------------------------

class HostDirectoryStorage final : public runtime::v1::Storage {
public:
    explicit HostDirectoryStorage(std::filesystem::path root, std::vector<std::filesystem::path> extra = {})
        : root_(std::move(root)), extra_(std::move(extra)) {}

    BackendInfo info() const noexcept override {
        return {"host-directory", "the extracted disc tree on the host file system (mapped files)",
                runtime::v1::kContractMajor, runtime::v1::kContractMinor};
    }
    std::uint32_t capabilities() const noexcept override {
        return runtime::v1::StorageEnumerate | runtime::v1::StorageRead | runtime::v1::StorageView;
    }

    // The host tree is scanned once; later calls return the same tree (and the same keys).
    Status enumerate(StorageNode& root, std::string& message) override {
        std::lock_guard<std::mutex> hold(enumerate_lock_);
        if (!scanned_) {
            std::error_code ec;
            if (!std::filesystem::is_directory(root_, ec)) {
                message = "not a directory: " + utf8(root_);
                return Status::NotFound;
            }
            tree_ = StorageNode{};
            tree_.directory = true;
            if (const Status s = scan(root_, tree_, message, 0u); s != Status::Ok) return s;
            if (const Status s = add_extra(message); s != Status::Ok) return s;
            scanned_ = true;
            // the host's file opens move off the game thread's reads, as the mount did before the contract
            // (FileProvider::preopen; GEKKO_ASSET_PREOPEN=0 is its control) - host timing only
            files_.preopen();
        }
        root = tree_;
        return Status::Ok;
    }

    Status read(std::uint64_t key, std::uint64_t offset, runtime::v1::Bytes dest, std::uint64_t& got) override {
        got = 0;
        if (key >= sizes_.size()) return Status::InvalidArgument;
        const std::uint64_t size = sizes_[key];
        const gekko::assets::ByteSpan whole = files_.map(key, size);
        if (whole.valid() || size == 0u) {
            copy_zero_fill(reinterpret_cast<const std::byte*>(whole.data), size, offset, dest, got);
            return Status::Ok;
        }
        gekko::u64 n = 0;
        if (!files_.read(key, offset, dest.size, reinterpret_cast<gekko::u8*>(dest.data), n)) return Status::IoError;
        got = n;
        return Status::Ok;
    }

    runtime::v1::ConstBytes view(std::uint64_t key) override {
        if (key >= sizes_.size()) return {};
        const gekko::assets::ByteSpan s = files_.map(key, sizes_[key]);
        return s.valid() ? runtime::v1::ConstBytes{reinterpret_cast<const std::byte*>(s.data), static_cast<std::size_t>(s.size)}
                         : runtime::v1::ConstBytes{};
    }

private:
    Status scan(const std::filesystem::path& dir, StorageNode& node, std::string& message, unsigned depth) {
        if (depth > 64u) {
            message = "directory nesting deeper than 64: " + utf8(dir);
            return Status::InvalidArgument;
        }
        std::error_code ec;
        for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
            StorageNode child;
            child.name = utf8(item.path().filename());
            if (!name_ok(child.name)) {
                message = "a host name outside the contract's name rule (printable ASCII): " + utf8(item.path());
                return Status::InvalidArgument;
            }
            if (item.is_directory(ec)) {
                child.directory = true;
                if (const Status s = scan(item.path(), child, message, depth + 1u); s != Status::Ok) return s;
            } else if (item.is_regular_file(ec)) {
                child.size = item.file_size(ec);
                if (ec) {
                    message = "cannot read the size of " + utf8(item.path());
                    return Status::IoError;
                }
                child.key = files_.add(item.path());
                if (child.key != sizes_.size()) {
                    message = "the file provider's keys are not dense";
                    return Status::IoError;
                }
                sizes_.push_back(child.size);
            } else {
                continue;
            }
            node.children.push_back(std::move(child));
        }
        if (ec) {
            message = "cannot list " + utf8(dir) + ": " + ec.message();
            return Status::IoError;
        }
        return Status::Ok;
    }

    // The extra files at the root, each under its own name when the tree has no entry of that name (case folded).
    Status add_extra(std::string& message) {
        const auto folded = [](std::string n) {
            for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return n;
        };
        for (const std::filesystem::path& file : extra_) {
            StorageNode child;
            child.name = utf8(file.filename());
            if (!name_ok(child.name)) continue;
            const bool present = std::any_of(tree_.children.begin(), tree_.children.end(),
                                             [&](const StorageNode& c) { return folded(c.name) == folded(child.name); });
            if (present) continue;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(file, ec)) continue;
            child.size = std::filesystem::file_size(file, ec);
            if (ec) {
                message = "cannot read the size of " + utf8(file);
                return Status::IoError;
            }
            child.key = files_.add(file);
            if (child.key != sizes_.size()) {
                message = "the file provider's keys are not dense";
                return Status::IoError;
            }
            sizes_.push_back(child.size);
            tree_.children.push_back(std::move(child));
        }
        return Status::Ok;
    }

    std::filesystem::path root_;
    std::vector<std::filesystem::path> extra_;
    gekko::assets::FileProvider files_;
    std::vector<std::uint64_t> sizes_;  // index = key
    std::mutex enumerate_lock_;
    StorageNode tree_;
    bool scanned_ = false;
};

// ---- gamecube-disc -----------------------------------------------------------------------------------------------

std::uint32_t be32(const std::byte* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

class GameCubeDiscStorage final : public runtime::v1::Storage {
public:
    explicit GameCubeDiscStorage(std::filesystem::path image) : image_(std::move(image)) {}

    BackendInfo info() const noexcept override {
        return {"gamecube-disc", "a GameCube disc image (GCM): the console's medium, its FST and extents",
                runtime::v1::kContractMajor, runtime::v1::kContractMinor};
    }
    std::uint32_t capabilities() const noexcept override {
        return runtime::v1::StorageEnumerate | runtime::v1::StorageRead | runtime::v1::StorageView;
    }

    bool open(std::string& message) {
        std::error_code ec;
        const std::uint64_t size = std::filesystem::file_size(image_, ec);
        if (ec || size < 0x440u) {
            message = "not a disc image: " + utf8(image_);
            return false;
        }
        key_ = provider_.add(image_);
        const gekko::assets::ByteSpan whole = provider_.map(key_, size);
        if (!whole.valid()) {
            message = "cannot map the disc image " + utf8(image_);
            return false;
        }
        base_ = reinterpret_cast<const std::byte*>(whole.data);
        size_ = size;
        return true;
    }

    Status enumerate(StorageNode& root, std::string& message) override {
        // boot.bin: the FST's disc offset and size at 0x424 / 0x428 (the console's apploader reads the same words)
        const std::uint64_t fst = be32(base_ + 0x424), fst_size = be32(base_ + 0x428);
        if (fst + fst_size > size_ || fst_size < 12u) {
            message = "the FST lies outside the image";
            return Status::IoError;
        }
        const std::byte* t = base_ + fst;
        const std::uint32_t count = be32(t + 8);
        if (static_cast<std::uint64_t>(count) * 12u > fst_size) {
            message = "the FST's entry count exceeds its size";
            return Status::IoError;
        }
        const std::byte* names = t + static_cast<std::size_t>(count) * 12u;
        const std::byte* names_end = t + fst_size;
        extents_.assign(count, Extent{});
        root = StorageNode{};
        root.directory = true;
        // entry 0 is the root, ending at `count`; a directory entry's third word is the entry after its last child
        struct Open {
            StorageNode* node;
            std::uint32_t end;
        };
        std::vector<Open> stack{{&root, count}};
        for (std::uint32_t i = 1; i < count; ++i) {
            while (stack.size() > 1u && i >= stack.back().end) stack.pop_back();
            const std::byte* e = t + static_cast<std::size_t>(i) * 12u;
            const std::uint32_t w0 = be32(e), w1 = be32(e + 4), w2 = be32(e + 8);
            const std::byte* name = names + (w0 & 0xFFFFFFu);
            const std::byte* end = name;
            while (end < names_end && *end != std::byte{0}) ++end;
            if (name >= names_end || end == names_end) {
                message = "an FST name lies outside the table (entry " + std::to_string(i) + ")";
                return Status::IoError;
            }
            StorageNode child;
            child.name.assign(reinterpret_cast<const char*>(name), static_cast<std::size_t>(end - name));
            if (!name_ok(child.name)) {
                message = "an FST name outside the contract's name rule (entry " + std::to_string(i) + ")";
                return Status::InvalidArgument;
            }
            StorageNode* parent = stack.back().node;
            if ((w0 >> 24) != 0u) {
                child.directory = true;
                if (w2 <= i || w2 > count) {
                    message = "an FST directory ends outside the table (entry " + std::to_string(i) + ")";
                    return Status::IoError;
                }
                parent->children.push_back(std::move(child));
                // the subtree's entries all come before the parent's next child, so this pointer stays valid while
                // the directory is open (only a closed sibling can move when the parent's vector grows)
                stack.push_back({&parent->children.back(), w2});
            } else {
                if (static_cast<std::uint64_t>(w1) + w2 > size_) {
                    message = "an FST file lies outside the image (entry " + std::to_string(i) + ")";
                    return Status::IoError;
                }
                child.size = w2;
                child.key = i;
                extents_[i] = Extent{w1, w2, true};
                parent->children.push_back(std::move(child));
            }
        }
        return Status::Ok;
    }

    Status read(std::uint64_t key, std::uint64_t offset, runtime::v1::Bytes dest, std::uint64_t& got) override {
        got = 0;
        if (key >= extents_.size() || !extents_[key].file) return Status::InvalidArgument;
        const Extent& x = extents_[key];
        copy_zero_fill(base_ + x.offset, x.length, offset, dest, got);
        return Status::Ok;
    }

    runtime::v1::ConstBytes view(std::uint64_t key) override {
        if (key >= extents_.size() || !extents_[key].file) return {};
        return runtime::v1::ConstBytes{base_ + extents_[key].offset, static_cast<std::size_t>(extents_[key].length)};
    }

private:
    struct Extent {
        std::uint64_t offset = 0;
        std::uint64_t length = 0;
        bool file = false;
    };
    std::filesystem::path image_;
    gekko::assets::FileProvider provider_;  // one mapped view of the whole image
    std::uint64_t key_ = 0;
    const std::byte* base_ = nullptr;
    std::uint64_t size_ = 0;
    std::vector<Extent> extents_;           // index = FST entry
};

}  // namespace

std::vector<std::string_view> storage_backend_names() { return {"host-directory", "gamecube-disc"}; }

std::unique_ptr<runtime::v1::Storage> create_storage(std::string_view name, const StorageOptions& options,
                                                     std::string& error) {
    error.clear();
    std::unique_ptr<runtime::v1::Storage> made;
    if (name == "host-directory") {
        auto host = std::make_unique<HostDirectoryStorage>(options.files_root, options.extra_files);
        made = std::move(host);
    } else if (name == "gamecube-disc") {
        auto disc = std::make_unique<GameCubeDiscStorage>(options.disc_image);
        if (!disc->open(error)) return nullptr;
        made = std::move(disc);
    } else {
        error = "unknown storage backend '" + std::string(name) + "' (known: host-directory, gamecube-disc)";
        return nullptr;
    }
    const runtime::v1::Negotiation n =
        runtime::v1::negotiate(made->info(), made->capabilities(), runtime::v1::kStorageRequired,
                               runtime::v1::storage_capability_name);
    if (!n.accepted) {
        error = n.reason;
        return nullptr;
    }
    return made;
}

namespace {

gekko::assets::AssetNamespace::Node namespace_node(const StorageNode& n) {
    gekko::assets::AssetNamespace::Node out;
    out.name = n.name;
    out.directory = n.directory;
    out.size = n.size;
    out.children.reserve(n.children.size());
    for (const StorageNode& c : n.children) out.children.push_back(namespace_node(c));
    return out;
}

}  // namespace

bool namespace_from_host_directory(const std::filesystem::path& root, gekko::assets::AssetNamespace& out,
                                   std::string& error) {
    out = gekko::assets::AssetNamespace{};
    auto storage = create_storage("host-directory", StorageOptions{root, {}}, error);
    if (storage == nullptr) return false;
    StorageNode tree;
    std::string message;
    if (const Status s = storage->enumerate(tree, message); s != Status::Ok) {
        error = message.empty() ? runtime::v1::to_string(s) : message;
        return false;
    }
    gekko::assets::AssetNamespace::Node node = namespace_node(tree);
    node.name.clear();
    return gekko::assets::AssetNamespace::from_tree(node, out, error);
}

bool mount_host_directory(gekko::assets::AssetSystem& assets, const std::filesystem::path& files_root,
                          std::vector<gekko::u8> identity, std::string& error) {
    auto storage = create_storage("host-directory", StorageOptions{files_root, {}}, error);
    return storage != nullptr && assets.mount(std::move(storage), std::move(identity), error);
}

bool mount_host_directory(gekko::assets::AssetSystem& assets, const std::filesystem::path& files_root,
                          gekko::assets::AssetNamespace names, std::vector<gekko::u8> identity, std::string& error) {
    auto storage = create_storage("host-directory", StorageOptions{files_root, {}}, error);
    return storage != nullptr && assets.mount(std::move(storage), std::move(names), std::move(identity), error);
}

}  // namespace shadow::backends
