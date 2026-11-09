// The runtime contract's storage service (shadow/runtime/v1/runtime.h) against both backends of
// shadow/backends/storage_backends.h: host-directory on <game data>/files and gamecube-disc on a GameCube disc image
// rebuilt by tools/platform/gcm.py from the same disc's sys/ and files/.
//
//  * selection: unknown backends and missing media are refused at selection, with the reason
//  * the trees: the same files with the same sizes; the namespace the core builds from each is identical entry for
//    entry (so every guest token is identical)
//  * the bytes: every file, whole, identical through both backends, through read() and through view()
//  * the read rule: a range past the end copies what exists and zero-fills the rest; an offset at or past the end
//    gives nothing and zeros; a key the backend never gave out is refused
//  * concurrency: four threads reading at once get the same bytes as one
//  * through the core: both mounted by AssetSystem give the same token for every entry and the same bytes for reads
//    at tokens
//
// Usage: runtime_storage_contract_tests <game data dir> <disc image>
#include "shadow/backends/storage_backends.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "gekko/asset_mount.h"
#include "gekko/asset_namespace.h"

namespace {

namespace v1 = shadow::runtime::v1;
unsigned g_checks = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) throw std::runtime_error(what);
}

struct File {
    std::uint64_t key, size;
};

void flatten(const v1::StorageNode& n, const std::string& prefix, std::map<std::string, File>& out) {
    for (const v1::StorageNode& c : n.children) {
        const std::string p = prefix.empty() ? c.name : prefix + "/" + c.name;
        if (c.directory) flatten(c, p, out);
        else out[p] = File{c.key, c.size};
    }
}

gekko::assets::AssetNamespace::Node to_namespace(const v1::StorageNode& n) {
    gekko::assets::AssetNamespace::Node out;
    out.name = n.name;
    out.directory = n.directory;
    out.size = n.size;
    for (const v1::StorageNode& c : n.children) out.children.push_back(to_namespace(c));
    return out;
}

std::vector<std::byte> whole(v1::Storage& s, const File& f) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(f.size));
    std::uint64_t got = 0;
    const v1::Status st = s.read(f.key, 0, v1::Bytes{bytes.data(), bytes.size()}, got);
    check(st == v1::Status::Ok && got == f.size, std::string(s.info().name) + ": a whole-file read");
    return bytes;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 3, "usage: runtime_storage_contract_tests <game data dir> <disc image>");
        const std::filesystem::path game = argv[1], image = argv[2];
        std::string error;

        // ---- selection ------------------------------------------------------------------------------------------
        check(shadow::backends::create_storage("no-such-backend", {}, error) == nullptr && !error.empty(),
              "an unknown backend is refused with a reason");
        check(shadow::backends::create_storage("gamecube-disc", {{}, game / "no-such.gcm"}, error) == nullptr &&
                  !error.empty(),
              "a missing disc image is refused at selection");
        auto host = shadow::backends::create_storage("host-directory", {game / "files", {}}, error);
        check(host != nullptr, "host-directory: " + error);
        auto disc = shadow::backends::create_storage("gamecube-disc", {{}, image}, error);
        check(disc != nullptr, "gamecube-disc: " + error);
        for (v1::Storage* s : {host.get(), disc.get()}) {
            check(s->info().contract_major == v1::kContractMajor, "the contract major");
            check((s->capabilities() & v1::kStorageRequired) == v1::kStorageRequired, "the required capabilities");
        }

        // ---- the trees ------------------------------------------------------------------------------------------
        v1::StorageNode ht, dt;
        std::string message;
        check(host->enumerate(ht, message) == v1::Status::Ok, "host-directory enumerates: " + message);
        check(disc->enumerate(dt, message) == v1::Status::Ok, "gamecube-disc enumerates: " + message);
        std::map<std::string, File> hf, df;
        flatten(ht, "", hf);
        flatten(dt, "", df);
        check(!hf.empty() && hf.size() == df.size(), "the same number of files (" + std::to_string(hf.size()) +
                                                          " / " + std::to_string(df.size()) + ")");
        for (const auto& [path, f] : hf) {
            const auto it = df.find(path);
            check(it != df.end(), "gamecube-disc has " + path);
            check(it->second.size == f.size, path + ": the same size");
        }
        v1::StorageNode again;
        check(host->enumerate(again, message) == v1::Status::Ok, "a second enumeration");
        std::map<std::string, File> hf2;
        flatten(again, "", hf2);
        bool same_keys = hf2.size() == hf.size();
        for (const auto& [path, f] : hf) same_keys = same_keys && hf2[path].key == f.key;
        check(same_keys, "enumerating again gives the same tree and keys");
        gekko::assets::AssetNamespace hn, dn;
        auto hroot = to_namespace(ht), droot = to_namespace(dt);
        hroot.name.clear(), droot.name.clear();
        check(gekko::assets::AssetNamespace::from_tree(hroot, hn, error), "the host namespace: " + error);
        check(gekko::assets::AssetNamespace::from_tree(droot, dn, error), "the disc namespace: " + error);
        check(hn.count() == dn.count(), "namespaces of the same size");
        for (gekko::u32 e = 1; e < hn.count(); ++e) {
            check(hn.path(e) == dn.path(e) && hn.is_directory(e) == dn.is_directory(e) &&
                      hn.entry(e).size == dn.entry(e).size,
                  "namespace entry " + std::to_string(e) + " the same: " + hn.path(e) + " / " + dn.path(e));
        }

        // ---- every file's bytes, through read() and view() --------------------------------------------------------
        std::uint64_t bytes = 0, viewed = 0;
        for (const auto& [path, f] : hf) {
            const File& g = df[path];
            const std::vector<std::byte> a = whole(*host, f), b = whole(*disc, g);
            check(a == b, path + ": the same bytes through both backends");
            for (v1::Storage* s : {host.get(), disc.get()}) {
                const v1::ConstBytes view = s->view(s == host.get() ? f.key : g.key);
                if (!view.empty()) {
                    check(view.size == f.size && std::memcmp(view.data, a.data(), a.size()) == 0,
                          path + ": view() is the file");
                    ++viewed;
                }
            }
            bytes += f.size;
        }
        std::printf("  %zu files, %llu bytes identical through both backends (%llu views checked)\n", hf.size(),
                    static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(viewed));

        // ---- the read rule ----------------------------------------------------------------------------------------
        for (v1::Storage* s : {host.get(), disc.get()}) {
            const std::map<std::string, File>& files = s == host.get() ? hf : df;
            const std::string name(s->info().name);
            unsigned tested = 0;
            for (const auto& [path, f] : files) {
                if (f.size < 8) continue;
                const std::vector<std::byte> all = whole(*s, f);
                std::vector<std::byte> tail(64, std::byte{0x5A});
                std::uint64_t got = 0;
                check(s->read(f.key, f.size - 5, v1::Bytes{tail.data(), tail.size()}, got) == v1::Status::Ok && got == 5,
                      name + ": a read running past the end copies what exists");
                bool ok = std::memcmp(tail.data(), all.data() + f.size - 5, 5) == 0;
                for (std::size_t i = 5; i < tail.size(); ++i) ok = ok && tail[i] == std::byte{0};
                check(ok, name + ": ... and zero-fills the rest");
                std::fill(tail.begin(), tail.end(), std::byte{0x5A});
                check(s->read(f.key, f.size + 100, v1::Bytes{tail.data(), tail.size()}, got) == v1::Status::Ok && got == 0,
                      name + ": a read past the end gives nothing");
                bool zero = true;
                for (std::byte x : tail) zero = zero && x == std::byte{0};
                check(zero, name + ": ... and zeros");
                if (++tested == 50) break;
            }
            std::byte one[4];
            std::uint64_t got = 0;
            check(s->read(~0ull, 0, v1::Bytes{one, 4}, got) == v1::Status::InvalidArgument, name + ": an unknown key");
            check(s->view(~0ull).empty(), name + ": no view of an unknown key");
        }

        // ---- concurrency --------------------------------------------------------------------------------------------
        {
            std::vector<std::pair<std::string, File>> list(df.begin(), df.end());
            std::atomic<unsigned> bad{0};
            std::vector<std::thread> workers;
            for (unsigned t = 0; t < 4; ++t) {
                workers.emplace_back([&, t] {
                    std::mt19937 rng(t + 1u);
                    for (unsigned i = 0; i < 400; ++i) {
                        const auto& [path, g] = list[rng() % list.size()];
                        const File& f = hf.at(path);
                        const std::uint64_t off = g.size ? rng() % g.size : 0;
                        std::vector<std::byte> a(256), b(256);
                        std::uint64_t ga = 0, gb = 0;
                        if (host->read(f.key, off, v1::Bytes{a.data(), a.size()}, ga) != v1::Status::Ok ||
                            disc->read(g.key, off, v1::Bytes{b.data(), b.size()}, gb) != v1::Status::Ok || ga != gb ||
                            a != b) {
                            ++bad;
                        }
                    }
                });
            }
            for (auto& w : workers) w.join();
            check(bad == 0u, "four threads reading at once get the same bytes from both backends");
        }

        // ---- through the core -----------------------------------------------------------------------------------------
        {
            gekko::assets::AssetSystem a, b;
            std::vector<gekko::u8> identity(0x20, 0);
            check(a.mount(shadow::backends::create_storage("host-directory", {game / "files", {}}, error), identity, error),
                  "AssetSystem mounts host-directory: " + error);
            check(b.mount(shadow::backends::create_storage("gamecube-disc", {{}, image}, error), identity, error),
                  "AssetSystem mounts gamecube-disc: " + error);
            check(a.names().count() == b.names().count(), "the same entries");
            unsigned compared = 0;
            for (gekko::u32 e = 1; e < a.names().count(); ++e) {
                check(a.token_of(e) == b.token_of(e), "entry " + std::to_string(e) + ": the same guest token");
                if (a.names().is_directory(e) || e % 7u != 0u) continue;
                const gekko::u64 size = a.names().entry(e).size;
                const gekko::u64 len = ((std::min<gekko::u64>(size, 4096) + 31u) / 32u) * 32u;
                std::vector<gekko::u8> x(static_cast<std::size_t>(len)), y(static_cast<std::size_t>(len));
                gekko::u64 gx = 0, gy = 0;
                check(a.read(a.token_of(e), len, x.data(), gx) == gekko::assets::AssetSystem::ReadStatus::Ok &&
                          b.read(b.token_of(e), len, y.data(), gy) == gekko::assets::AssetSystem::ReadStatus::Ok &&
                          gx == gy && x == y,
                      a.names().path(e) + ": the same bytes at its token through both mounts");
                ++compared;
            }
            std::printf("  through AssetSystem: %u tokens identical, %u token reads identical\n", a.names().count() - 1u,
                        compared);
        }
        std::printf("runtime_storage_contract_tests: %u checks passed\n", g_checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "runtime_storage_contract_tests: FAILED: %s\n", e.what());
        return 1;
    }
}
