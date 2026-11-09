// A storage backend of the runtime contract (shadow/runtime/v1/runtime.h) held in memory, for tests whose tree is
// synthetic (a namespace built from a table, files that exist nowhere on disk): every file of the namespace, its
// bytes a deterministic pattern of its entry number. The same contract rules as the real backends (zero-fill past
// the end, unknown keys refused).
#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "gekko/asset_namespace.h"
#include "shadow/runtime/v1/runtime.h"

namespace gekko::test_support {

class MemoryStorage final : public shadow::runtime::v1::Storage {
public:
    explicit MemoryStorage(const assets::AssetNamespace& names) {
        root_.directory = true;
        if (names.count() == 0u) return;   // an empty namespace: an empty tree
        std::vector<shadow::runtime::v1::StorageNode*> node_of(names.count(), nullptr);
        node_of[0] = &root_;
        // entries come in pre-order: a parent precedes its children; reserve so the pointers stay valid
        std::vector<std::size_t> children(names.count(), 0u);
        for (u32 e = 1; e < names.count(); ++e) ++children[names.entry(e).parent];
        root_.children.reserve(children[0]);
        for (u32 e = 1; e < names.count(); ++e) {
            const assets::AssetNamespace::Entry& entry = names.entry(e);
            shadow::runtime::v1::StorageNode node;
            node.name = std::string(names.name(e));
            node.directory = entry.directory;
            if (!entry.directory) {
                node.size = entry.size;
                node.key = bytes_.size();
                std::vector<std::byte> b(static_cast<std::size_t>(entry.size));
                for (std::size_t i = 0; i < b.size(); ++i) b[i] = static_cast<std::byte>((e * 131u + i) & 0xFFu);
                bytes_.push_back(std::move(b));
            }
            shadow::runtime::v1::StorageNode* parent = node_of[entry.parent];
            parent->children.push_back(std::move(node));
            node_of[e] = &parent->children.back();
            if (entry.directory) node_of[e]->children.reserve(children[e]);
        }
    }

    shadow::runtime::v1::BackendInfo info() const noexcept override {
        return {"memory", "an in-memory tree for tests", shadow::runtime::v1::kContractMajor,
                shadow::runtime::v1::kContractMinor};
    }
    std::uint32_t capabilities() const noexcept override {
        return shadow::runtime::v1::StorageEnumerate | shadow::runtime::v1::StorageRead | shadow::runtime::v1::StorageView;
    }
    shadow::runtime::v1::Status enumerate(shadow::runtime::v1::StorageNode& root, std::string&) override {
        root = root_;
        return shadow::runtime::v1::Status::Ok;
    }
    shadow::runtime::v1::Status read(std::uint64_t key, std::uint64_t offset, shadow::runtime::v1::Bytes dest,
                                     std::uint64_t& got) override {
        got = 0;
        if (key >= bytes_.size()) return shadow::runtime::v1::Status::InvalidArgument;
        const std::vector<std::byte>& b = bytes_[key];
        got = offset < b.size() ? std::min<std::uint64_t>(dest.size, b.size() - offset) : 0u;
        if (got) std::memcpy(dest.data, b.data() + offset, static_cast<std::size_t>(got));
        if (got < dest.size) std::memset(dest.data + got, 0, static_cast<std::size_t>(dest.size - got));
        return shadow::runtime::v1::Status::Ok;
    }
    shadow::runtime::v1::ConstBytes view(std::uint64_t key) override {
        if (key >= bytes_.size()) return {};
        return {bytes_[key].data(), bytes_[key].size()};
    }

private:
    shadow::runtime::v1::StorageNode root_;
    std::vector<std::vector<std::byte>> bytes_;
};

}  // namespace gekko::test_support
