// The native asset namespace. Contract and rationale: include/gekko/asset_namespace.h.

#include "gekko/asset_namespace.h"

#include <algorithm>
#include <functional>

namespace gekko::assets {

namespace {

std::string folded(std::string_view name) {
    std::string out(name);
    for (char& ch : out) ch = static_cast<char>(AssetNamespace::fold(static_cast<u8>(ch)));
    return out;
}

// Children are ordered by folded name, then by the raw bytes: a total order
// that does not depend on the host filesystem's enumeration order.
bool child_before(const AssetNamespace::Node& a, const AssetNamespace::Node& b) {
    const std::string fa = folded(a.name), fb = folded(b.name);
    if (fa != fb) return fa < fb;
    return a.name < b.name;
}

}  // namespace

std::size_t AssetNamespace::KeyHash::operator()(const Key& k) const noexcept {
    return std::hash<std::string>{}(k.folded) ^ (static_cast<std::size_t>(k.directory) * 0x9E3779B97F4A7C15ull);
}

void AssetNamespace::clear() noexcept {
    entries_.clear();
    names_.clear();
    children_.clear();
    files_ = 0;
    directories_ = 0;
    total_bytes_ = 0;
}

bool AssetNamespace::from_tree(const Node& root, AssetNamespace& out, std::string& error) {
    out.clear();
    error.clear();
    if (!root.directory) {
        error = "the namespace root must be a directory";
        return false;
    }
    // Iterative pre-order: a stack of (node, parent index), children pushed in
    // reverse order so they are emitted in sorted order.
    struct Pending {
        const Node* node;
        u32 parent;
    };
    std::vector<u32> open;  // directories whose `end` is still being counted
    out.entries_.push_back(Entry{kRoot, 0u, 0u, 0u, 0u, true});
    ++out.directories_;

    // Recursive emission is simplest to state; the depth is bounded by the
    // scan (64) or by the caller's tree.
    std::function<bool(const Node&, u32)> emit = [&](const Node& node, u32 self) -> bool {
        std::vector<const Node*> order;
        order.reserve(node.children.size());
        for (const Node& child : node.children) order.push_back(&child);
        std::sort(order.begin(), order.end(),
                  [](const Node* a, const Node* b) { return child_before(*a, *b); });
        for (const Node* child : order) {
            if (child->name.empty() || child->name.find('/') != std::string::npos ||
                child->name.find('\0') != std::string::npos) {
                error = "invalid entry name in the asset tree";
                return false;
            }
            if (out.entries_.size() >= 0x7FFFFFFFu) {
                error = "asset tree has too many entries";
                return false;
            }
            const u32 index = static_cast<u32>(out.entries_.size());
            Entry entry;
            entry.parent = self;
            entry.name_offset = static_cast<u32>(out.names_.size());
            entry.name_length = static_cast<u32>(child->name.size());
            entry.directory = child->directory;
            entry.size = child->directory ? 0u : child->size;
            out.names_.append(child->name);
            out.names_.push_back('\0');
            out.entries_.push_back(entry);
            if (child->directory) {
                ++out.directories_;
                if (!emit(*child, index)) return false;
                out.entries_[index].end = static_cast<u32>(out.entries_.size());
            } else {
                ++out.files_;
                out.total_bytes_ += child->size;
                out.entries_[index].end = index + 1u;
            }
        }
        return true;
    };
    if (!emit(root, kRoot)) {
        out.clear();
        return false;
    }
    out.entries_[0].end = static_cast<u32>(out.entries_.size());
    if (!out.finish(error)) {
        out.clear();
        return false;
    }
    return true;
}

bool AssetNamespace::from_preorder(const std::vector<PreorderEntry>& list, AssetNamespace& out,
                                   std::string& error) {
    out.clear();
    error.clear();
    if (list.size() >= 0x7FFFFFFFu) {
        error = "namespace has too many entries";
        return false;
    }
    const u32 count = static_cast<u32>(list.size()) + 1u;
    out.entries_.resize(count);
    out.entries_[0] = Entry{kRoot, count, 0u, 0u, 0u, true};
    ++out.directories_;
    std::vector<u32> stack{0u};
    for (u32 i = 1u; i < count; ++i) {
        while (stack.size() > 1u && i >= out.entries_[stack.back()].end) stack.pop_back();
        const PreorderEntry& item = list[i - 1u];
        if (item.name.empty() || item.name.find('/') != std::string::npos ||
            item.name.find('\0') != std::string::npos) {
            error = "entry " + std::to_string(i) + " has an empty name or one containing '/' or NUL";
            out.clear();
            return false;
        }
        Entry& entry = out.entries_[i];
        entry.directory = item.directory;
        entry.parent = stack.back();
        entry.name_offset = static_cast<u32>(out.names_.size());
        entry.name_length = static_cast<u32>(item.name.size());
        out.names_.append(item.name);
        out.names_.push_back('\0');
        if (entry.directory) {
            if (item.end <= i || item.end > out.entries_[entry.parent].end) {
                error = "directory entry " + std::to_string(i) + " is not nested in its parent";
                out.clear();
                return false;
            }
            entry.end = item.end;
            ++out.directories_;
            stack.push_back(i);
        } else {
            entry.end = i + 1u;
            entry.size = item.size;
            ++out.files_;
            out.total_bytes_ += item.size;
        }
    }
    if (!out.finish(error)) {
        out.clear();
        return false;
    }
    return true;
}

bool AssetNamespace::finish(std::string& error) {
    children_.clear();
    children_.reserve(entries_.size());
    for (u32 i = 1u; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        const Entry& parent = entries_[e.parent];
        if (!parent.directory || e.parent >= i || i >= parent.end || e.end > parent.end ||
            e.end <= i) {
            error = "entry " + std::to_string(i) + " breaks the pre-order nesting";
            return false;
        }
        children_[Key{e.parent, folded(name(i))}].push_back(i);
    }
    return true;
}

std::string AssetNamespace::path(u32 index) const {
    std::vector<u32> chain;
    for (u32 at = index; at != kRoot && at < entries_.size(); at = entries_[at].parent) {
        chain.push_back(at);
        if (chain.size() > entries_.size()) break;  // cannot happen after finish()
    }
    std::string out;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (!out.empty()) out.push_back('/');
        out.append(name(*it));
    }
    return out;
}

s32 AssetNamespace::find_child(u32 directory, std::string_view child, bool directories_only) const {
    if (directory >= entries_.size() || !entries_[directory].directory) return kNotFound;
    const auto found = children_.find(Key{directory, folded(child)});
    if (found == children_.end()) return kNotFound;
    for (const u32 candidate : found->second) {
        if (!directories_only || entries_[candidate].directory) return static_cast<s32>(candidate);
    }
    return kNotFound;
}

s32 AssetNamespace::resolve(std::string_view text, u32 current) const {
    if (entries_.empty() || current >= entries_.size() || !entries_[current].directory) return kNotFound;
    const std::size_t nul = text.find('\0');
    if (nul != std::string_view::npos) text = text.substr(0, nul);
    const auto at = [&text](std::size_t k) -> u8 {
        return k < text.size() ? static_cast<u8>(text[k]) : u8{0};
    };
    u32 dir = current;
    std::size_t p = 0;
    for (;;) {
        const u8 first = at(p);
        if (first == 0u) return static_cast<s32>(dir);
        if (first == '/') {  // restart at the root
            dir = kRoot;
            p += 1u;
            continue;
        }
        if (first == '.') {
            const u8 second = at(p + 1u);
            if (second == '.') {
                const u8 third = at(p + 2u);
                if (third == '/') {
                    dir = entries_[dir].parent;
                    p += 3u;
                    continue;
                }
                if (third == 0u) return static_cast<s32>(entries_[dir].parent);
            } else if (second == '/') {
                p += 2u;
                continue;
            } else if (second == 0u) {
                return static_cast<s32>(dir);
            }
        }
        // One name, to '/' or the end.
        std::size_t end = p;
        while (at(end) != 0u && at(end) != '/') ++end;
        const bool want_directory = at(end) != 0u;
        const s32 found = find_child(dir, text.substr(p, end - p), want_directory);
        if (found < 0) return kNotFound;
        if (!want_directory) return found;
        dir = static_cast<u32>(found);
        p = end + 1u;
    }
}

}  // namespace gekko::assets
