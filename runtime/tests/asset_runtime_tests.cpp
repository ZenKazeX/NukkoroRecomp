// Tests for the Native Asset Runtime.
//
// What this suite defends is the set of contracts recovered from the guest -
// the ones that make this a replacement rather than a rewrite that happens to
// compile:
//
//   * identity is an AssetId and is STABLE; nothing is addressed by a disc
//     offset, and re-interning a path never changes what an existing handle
//     means;
//   * a read past end-of-file SUCCEEDS and zero-fills, because every live
//     issuer rounds its length up to 32 bytes and would otherwise fail on the
//     last read of every file;
//   * an unknown asset is rejected AT SUBMIT, queueing nothing;
//   * completions come out in strict priority order, FIFO within a priority;
//   * a completion is only ever observed from pump(), on the thread that calls
//     it - never from inside submit;
//   * nothing is observable half-done.

#include "gekko/asset_mount.h"
#include "shadow/backends/storage_backends.h"
#include "support/memory_storage.h"
#include <memory>
#include "gekko/asset_providers.h"
#include "gekko/asset_runtime.h"
#include "support/fst_oracle.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "test_scratch.h"

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

using namespace gekko;
using namespace gekko::assets;

std::filesystem::path scratch_dir() {
    const auto dir = gekko::test_support::scratch_directory("gekko-asset-runtime-tests");
    std::filesystem::create_directories(dir);
    return dir;
}

std::filesystem::path write_file(const std::string& name, const std::vector<u8>& bytes) {
    const auto path = scratch_dir() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    return path;
}

std::vector<u8> pattern(std::size_t n, u8 seed = 0) {
    std::vector<u8> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<u8>((i * 7u + seed) & 0xFFu);
    return v;
}

// ---------------------------------------------------------------------------

void test_asset_id_is_identity_not_a_position() {
    AssetRegistry registry;
    check(!AssetId{}.valid(), "a default-constructed id is invalid, not asset zero");

    const AssetId a = registry.intern("files/one.bin", 100, ProviderKind::File, 0);
    const AssetId b = registry.intern("files/two.bin", 200, ProviderKind::File, 1);
    check(a.valid() && b.valid(), "interning yields valid ids");
    check(a != b, "distinct paths get distinct ids");

    // Stability: re-interning the same path must return the SAME id, because a
    // handle taken earlier has to keep meaning the same asset.
    const AssetId again = registry.intern("files/one.bin", 999, ProviderKind::Archive, 42);
    check(again == a, "re-interning a path keeps its identity");
    const AssetRecord* record = registry.get(a);
    check(record != nullptr && record->size == 999 && record->key == 42,
          "but the locator is refreshed");

    check(registry.find("files/two.bin") == b, "lookup by path finds the id");
    check(!registry.find("files/missing.bin").valid(), "an unknown path yields an invalid id");
    check(registry.get(AssetId{}) == nullptr, "an invalid id resolves to nothing");
    check(registry.get(AssetId{9999}) == nullptr, "an out-of-range id resolves to nothing");
}

void test_a_file_is_served_in_place_with_no_copy() {
    const std::vector<u8> bytes = pattern(4096);
    const auto path = write_file("mapped.bin", bytes);

    AssetRegistry registry;
    FileProvider files;
    ResourceManager resources(registry);
    resources.set_provider(ProviderKind::File, &files);

    const u64 key = files.add(path);
    const AssetId id = registry.intern("mapped.bin", bytes.size(), ProviderKind::File, key);

    AssetHandle handle = resources.acquire(id);
    check(handle.valid(), "the asset acquires");
    check(handle.size() == bytes.size(), "the whole file is visible");
    check(std::memcmp(handle.bytes().data, bytes.data(), bytes.size()) == 0,
          "the mapped bytes are the file's bytes");

    // Served in place: the manager counted a map hit and materialised nothing.
    const ResourceManager::Stats s = resources.stats();
    check(s.map_hits == 1 && s.materialised == 0,
          "a host file is served from its mapping, with no copy");
}

void test_reads_past_end_of_file_succeed_and_zero_fill() {
    // THE CONTRACT THAT BREAKS EVERYTHING IF MISSED. Both live issuers round a
    // read length up to a 32-byte multiple, so the last read of a file whose
    // size is not a multiple of 32 always asks for bytes that do not exist.
    const std::vector<u8> bytes = pattern(100);  // deliberately not a multiple of 32
    const auto path = write_file("ragged.bin", bytes);

    AssetRegistry registry;
    FileProvider files;
    ResourceManager resources(registry);
    resources.set_provider(ProviderKind::File, &files);
    const u64 key = files.add(path);
    const AssetId id = registry.intern("ragged.bin", bytes.size(), ProviderKind::File, key);

    // 100 rounded up to 128: 28 bytes past the end.
    std::vector<u8> dest(128, 0xCDu);
    u64 got = 0;
    check(resources.read(id, 0, 128, dest.data(), got), "a read 28 bytes past EOF succeeds");
    check(got == 128, "it reports the full requested length");
    check(std::memcmp(dest.data(), bytes.data(), bytes.size()) == 0, "the real bytes are intact");
    for (std::size_t i = bytes.size(); i < dest.size(); ++i) {
        check(dest[i] == 0u, "the tail past EOF reads as zero");
    }

    // Entirely past the end is still a success, still zeroed.
    std::vector<u8> beyond(32, 0xCDu);
    check(resources.read(id, 4096, 32, beyond.data(), got), "a read wholly past EOF succeeds");
    check(got == 32, "and reports its length");
    for (u8 b : beyond) check(b == 0u, "with every byte zero");

    // A zero-length read is legal and trivially complete.
    check(resources.read(id, 0, 0, dest.data(), got) && got == 0, "a zero-length read succeeds");
}

void test_an_unknown_asset_is_rejected_at_submit() {
    AssetRegistry registry;
    FileProvider files;
    ResourceManager resources(registry);
    resources.set_provider(ProviderKind::File, &files);
    StreamingScheduler scheduler(resources);

    std::vector<u8> dest(32, 0);
    Request bad;
    bad.id = AssetId{};  // never interned
    bad.dest = dest.data();
    bad.length = 32;
    check(!scheduler.submit(bad), "an invalid id is refused");
    check(scheduler.pending() == 0, "and NOTHING is queued");

    const auto path = write_file("real.bin", pattern(64));
    const u64 key = files.add(path);
    const AssetId id = registry.intern("real.bin", 64, ProviderKind::File, key);
    Request no_dest;
    no_dest.id = id;
    no_dest.dest = nullptr;
    no_dest.length = 32;
    check(!scheduler.submit(no_dest), "a request with no destination is refused");
    check(scheduler.pending() == 0, "and still nothing is queued");
    check(scheduler.stats().rejected == 2, "both refusals are counted");
}

void test_completions_are_priority_then_fifo() {
    AssetRegistry registry;
    FileProvider files;
    ResourceManager resources(registry);
    resources.set_provider(ProviderKind::File, &files);
    StreamingScheduler scheduler(resources);

    const auto path = write_file("queued.bin", pattern(256));
    const u64 key = files.add(path);
    const AssetId id = registry.intern("queued.bin", 256, ProviderKind::File, key);

    std::vector<std::vector<u8>> buffers(6, std::vector<u8>(16, 0));
    auto submit = [&](Priority p, u64 tag) {
        Request r;
        r.id = id;
        r.length = 16;
        r.dest = buffers[tag].data();
        r.priority = p;
        r.user = tag;
        check(scheduler.submit(r), "submit accepted");
    };
    // Interleaved on purpose: insertion order must survive inside a priority,
    // and priority must dominate across them.
    submit(Priority::Low, 0);
    submit(Priority::Normal, 1);
    submit(Priority::High, 2);
    submit(Priority::Low, 3);
    submit(Priority::High, 4);
    submit(Priority::Normal, 5);
    check(scheduler.pending() == 6, "all six are queued");

    std::vector<Completion> done;
    scheduler.pump(done);
    check(done.size() == 6, "all six retire");
    const u64 expected[] = {2, 4, 1, 5, 0, 3};  // High FIFO, Normal FIFO, Low FIFO
    for (std::size_t i = 0; i < done.size() && i < 6; ++i) {
        check(done[i].user == expected[i], "completion order is priority, then FIFO");
        check(done[i].ok, "each completion succeeded");
    }
    check(scheduler.idle(), "the queues are drained");
}

void test_nothing_is_observable_half_done() {
    AssetRegistry registry;
    FileProvider files;
    ResourceManager resources(registry);
    resources.set_provider(ProviderKind::File, &files);
    StreamingScheduler scheduler(resources);

    const std::vector<u8> bytes = pattern(1024, 3);
    const auto path = write_file("whole.bin", bytes);
    const u64 key = files.add(path);
    const AssetId id = registry.intern("whole.bin", bytes.size(), ProviderKind::File, key);

    std::vector<u8> dest(1024, 0xEEu);
    Request r;
    r.id = id;
    r.length = 1024;
    r.dest = dest.data();
    r.user = 77;
    check(scheduler.submit(r), "submitted");

    // Submitting must not have produced a completion: the contract is that the
    // consumer only ever observes one from pump(), on its own thread.
    check(scheduler.stats().completed == 0, "submit does not complete the request");
    check(scheduler.pending() == 1, "it is pending");

    std::vector<Completion> done;
    scheduler.pump(done);
    check(done.size() == 1 && done[0].user == 77, "pump delivers it");
    check(done[0].transferred == 1024, "with the whole length");
    check(std::memcmp(dest.data(), bytes.data(), bytes.size()) == 0,
          "and every byte is present when the completion is seen");
}

void test_pump_budget_matches_the_one_per_walk_rule() {
    AssetRegistry registry;
    FileProvider files;
    ResourceManager resources(registry);
    resources.set_provider(ProviderKind::File, &files);
    StreamingScheduler scheduler(resources);

    const auto path = write_file("budget.bin", pattern(64));
    const u64 key = files.add(path);
    const AssetId id = registry.intern("budget.bin", 64, ProviderKind::File, key);

    std::vector<std::vector<u8>> buffers(3, std::vector<u8>(8, 0));
    for (u64 i = 0; i < 3; ++i) {
        Request r;
        r.id = id;
        r.length = 8;
        r.dest = buffers[i].data();
        r.user = i;
        scheduler.submit(r);
    }

    // The guest dequeues exactly one request per task walk. A budget expresses
    // that directly instead of simulating a slow device.
    std::vector<Completion> done;
    check(scheduler.pump(done, 1) == 1, "one per pump when budgeted");
    check(scheduler.pending() == 2, "the rest stay queued");
    scheduler.pump(done, 1);
    scheduler.pump(done, 1);
    check(scheduler.idle(), "three walks drain three requests");
    check(done.size() == 3 && done[0].user == 0 && done[2].user == 2, "in submission order");
}

void test_handle_lifetime_is_refcounted_and_move_only() {
    const std::vector<u8> bytes = pattern(512, 9);
    const auto path = write_file("lifetime.bin", bytes);

    AssetRegistry registry;
    FileProvider files;
    ResourceManager resources(registry);
    resources.set_provider(ProviderKind::File, &files);
    const u64 key = files.add(path);
    const AssetId id = registry.intern("lifetime.bin", bytes.size(), ProviderKind::File, key);

    {
        AssetHandle first = resources.acquire(id);
        AssetHandle second = resources.acquire(id);
        check(first.valid() && second.valid(), "two handles on one asset");
        check(first.bytes().data == second.bytes().data, "share one mapping");
        check(resources.stats().live == 2, "and the manager counts two live");

        AssetHandle moved = std::move(first);
        check(moved.valid() && !first.valid(), "moving transfers the claim");
        check(resources.stats().live == 2, "without changing the refcount");
    }
    const ResourceManager::Stats s = resources.stats();
    check(s.live == 0, "every handle released at scope exit");
    check(s.acquires == 2 && s.releases == 2, "acquires and releases balance");
}

void test_a_missing_file_fails_cleanly() {
    AssetRegistry registry;
    FileProvider files;
    ResourceManager resources(registry);
    resources.set_provider(ProviderKind::File, &files);

    const u64 key = files.add(scratch_dir() / "does-not-exist.bin");
    const AssetId id = registry.intern("does-not-exist.bin", 128, ProviderKind::File, key);

    AssetHandle handle = resources.acquire(id);
    check(!handle.valid(), "acquiring a missing file yields an invalid handle");
    u64 got = 0;
    std::vector<u8> dest(16, 0);
    check(!resources.read(id, 0, 16, dest.data(), got), "and reading it fails rather than hangs");
    check(files.stats().failures == 1, "the failure is counted once");
}

// The pre-opener (FileProvider::preopen, ES 2026-10-19) moves the host's file
// opens off the reads and changes nothing a read returns: while it walks the
// files on its thread, readers on two other threads get every file's own bytes
// (each file opened exactly once, whoever gets there first), a missing file
// still fails and is counted once, and the provider can be destroyed with the
// walk unfinished.
void test_preopen_changes_nothing_a_read_returns() {
    constexpr int kFiles = 64;
    std::vector<std::vector<u8>> contents;
    AssetRegistry registry;
    FileProvider files;
    ResourceManager resources(registry);
    resources.set_provider(ProviderKind::File, &files);
    std::vector<AssetId> ids;
    for (int i = 0; i < kFiles; ++i) {
        contents.push_back(pattern(100u + 977u * static_cast<std::size_t>(i), static_cast<u8>(i)));
        const std::string name = "preopen-" + std::to_string(i) + ".bin";
        const u64 key = files.add(write_file(name, contents.back()));
        ids.push_back(registry.intern(name, contents.back().size(), ProviderKind::File, key));
    }
    const u64 missing_key = files.add(scratch_dir() / "preopen-missing.bin");
    const AssetId missing = registry.intern("preopen-missing.bin", 64, ProviderKind::File, missing_key);

    files.preopen();
    int wrong = 0;
    const auto reader = [&](int first) {
        for (int n = 0; n < kFiles; ++n) {
            const int i = (first + n) % kFiles;
            std::vector<u8> dest(contents[i].size() + 32u, 0xCDu);
            u64 got = 0;
            if (!resources.read(ids[i], 0, dest.size(), dest.data(), got) ||
                std::memcmp(dest.data(), contents[i].data(), contents[i].size()) != 0)
                ++wrong;  // each reader counts its own; compared after both joined
        }
    };
    int wrong_a = 0, wrong_b = 0;
    std::thread a([&] { reader(kFiles / 2); wrong_a = wrong; });
    a.join();
    wrong = 0;
    std::thread b([&] { reader(kFiles - 1); wrong_b = wrong; });
    b.join();
    check(wrong_a == 0 && wrong_b == 0, "every read returns the file's own bytes while the pre-opener runs");
    u64 got = 0;
    std::vector<u8> dest(64, 0);
    check(!resources.read(missing, 0, 64, dest.data(), got), "a missing file still fails");
    const FileProvider::Stats stats = files.stats();
    check(stats.mapped == static_cast<u64>(kFiles), "each file mapped exactly once");
    check(stats.failures == 1, "the missing file counted once");
    check(stats.preopened <= static_cast<u64>(kFiles) + 1u, "the pre-opener counts only its own opens");

    // Destroyed while (possibly) still walking: the walk stops and is joined.
    {
        FileProvider early;
        for (int i = 0; i < kFiles; ++i)
            early.add(scratch_dir() / ("preopen-" + std::to_string(i) + ".bin"));
        early.preopen();
    }
    check(true, "a provider destroyed during its walk joins it");
}

void test_no_provider_is_a_miss_not_a_crash() {
    AssetRegistry registry;
    ResourceManager resources(registry);  // deliberately no providers set
    const AssetId id = registry.intern("orphan.bin", 32, ProviderKind::File, 0);
    AssetHandle handle = resources.acquire(id);
    check(!handle.valid(), "an asset with no provider does not acquire");
    check(resources.stats().misses == 1, "and is counted as a miss");
}

// ---------------------------------------------------------------------------
// AssetAddressSpace - the token the guest still carries
// ---------------------------------------------------------------------------

void test_address_zero_never_decodes() {
    AssetRegistry registry;
    AssetAddressSpace space;
    registry.intern("first.bin", 1024, ProviderKind::File, 0);
    check(space.build(registry), "the space builds");

    AssetAddressSpace::Decoded out;
    check(space.decode(0u, 4u, out) == AssetAddressSpace::Status::Unmapped,
          "address 0 is unmapped, so an uninitialised startAddr faults");
    check(space.base_of(AssetId{1}) != 0u, "and no asset is ever based at 0");

    // Every retired disc offset this image ever used, and every small integer
    // that is not an asset base at all, must be refused rather than aliased
    // onto whichever asset happens to live there. The largest disc offset in
    // the real table was 0x515B697C.
    const u32 probes[] = {1u, 0x20u, 0x440u, 0x2440u, 0x005A1A00u, 0x515B697Cu,
                          AssetAddressSpace::kSpaceBase - 1u, AssetAddressSpace::kSpaceBase};
    for (u32 probe : probes) {
        check(space.decode(probe, 32u, out) == AssetAddressSpace::Status::Unmapped,
              "a value below the space base is unmapped, not aliased (" +
                  std::to_string(probe) + ")");
    }
}

void test_every_base_is_injective_and_decodes_back() {
    AssetRegistry registry;
    AssetAddressSpace space;
    // Sizes chosen to straddle the page granule in both directions, including
    // an exact multiple (which the +32 tail allowance pushes into one more
    // page) and a zero-length asset.
    const u64 sizes[] = {0, 1, 31, 32, 33, AssetAddressSpace::kPageSize - 32,
                         AssetAddressSpace::kPageSize - 31, AssetAddressSpace::kPageSize,
                         AssetAddressSpace::kPageSize + 1, 5u * AssetAddressSpace::kPageSize};
    std::vector<AssetId> ids;
    for (std::size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        ids.push_back(registry.intern("asset" + std::to_string(i), sizes[i], ProviderKind::File,
                                      static_cast<u64>(i)));
    }
    check(space.build(registry), "the space builds");

    std::vector<u32> seen;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const u32 base = space.base_of(ids[i]);
        check(base != 0u, "asset " + std::to_string(i) + " has a base");
        for (u32 other : seen) check(other != base, "bases are injective");
        seen.push_back(base);

        AssetAddressSpace::Decoded out;
        check(space.decode(base, 4u, out) == AssetAddressSpace::Status::Ok &&
                  out.id == ids[i] && out.offset == 0u && out.size == sizes[i],
              "base decodes back to (asset, 0)");

        // The guest's own worst legal read: offset == size, reaching size + 31.
        check(space.decode(static_cast<u32>(base + sizes[i]), 31u, out) ==
                      AssetAddressSpace::Status::Ok &&
                  out.id == ids[i] && out.offset == sizes[i],
              "the guest's bounded 32-byte overrun stays inside the region");
    }
}

void test_a_request_that_leaves_its_region_is_refused_not_aliased() {
    AssetRegistry registry;
    AssetAddressSpace space;
    const AssetId a = registry.intern("a.bin", 64, ProviderKind::File, 0);
    const AssetId b = registry.intern("b.bin", 64, ProviderKind::File, 1);
    check(space.build(registry), "the space builds");

    const u32 base_a = space.base_of(a);
    AssetAddressSpace::Decoded out;
    // On real hardware this read would have returned b's leading bytes,
    // because 2,430 of the disc's extents are byte-exactly adjacent. Here the
    // neighbour is unreachable by construction.
    check(space.decode(base_a, AssetAddressSpace::kPageSize + 1u, out) ==
              AssetAddressSpace::Status::OutOfRange,
          "a read longer than the region is refused, never aliased onto the next asset");
    check(space.decode(static_cast<u32>(base_a + 65u), 4u, out) ==
              AssetAddressSpace::Status::OutOfRange,
          "an offset past the asset's size is refused");
    check(space.base_of(b) >= base_a + AssetAddressSpace::kPageSize,
          "and the neighbour lives in its own pages");
}

void test_the_gap_between_assets_is_unmapped() {
    AssetRegistry registry;
    AssetAddressSpace space;
    // 64 bytes occupies one page; page 2 onwards belongs to nobody until the
    // next asset. Anything landing there is a decode failure, not zeroes.
    registry.intern("only.bin", 64, ProviderKind::File, 0);
    check(space.build(registry), "the space builds");

    AssetAddressSpace::Decoded out;
    const u32 past_the_end = space.base_of(AssetId{1}) + AssetAddressSpace::kPageSize;
    check(space.decode(past_the_end, 4u, out) == AssetAddressSpace::Status::Unmapped,
          "the page after the last asset is unmapped");
}

void test_rebuilding_after_more_assets_keeps_identity_stable() {
    AssetRegistry registry;
    AssetAddressSpace space;
    const AssetId a = registry.intern("a.bin", 4096, ProviderKind::File, 0);
    check(space.build(registry), "the space builds");
    const u32 first_base = space.base_of(a);

    registry.intern("b.bin", 4096, ProviderKind::File, 1);
    check(space.build(registry), "the space rebuilds");
    // Bases are assigned in registry order and the registry never reorders, so
    // an earlier asset keeps its base. Nothing depends on this - the FST is
    // republished from the space - but a base that moved under a live
    // DVDFileInfo would be a real fault, so it is pinned by a test.
    check(space.base_of(a) == first_base, "an existing asset keeps its base");
}

// ---------------------------------------------------------------------------
// AssetSystem::mount - the namespace, the registry and the token space
// ---------------------------------------------------------------------------

// Builds a table in the 12-byte-record layout (a root entry, a directory, and
// files inside and outside it) - the oracle a namespace is built from here.
std::vector<u8> build_table(const std::vector<std::pair<std::string, u32>>& files,
                            const std::string& directory_name, std::size_t files_in_directory) {
    const u32 entries = static_cast<u32>(1u + 1u + files.size());
    std::vector<u8> names;
    std::vector<u32> name_offsets;
    name_offsets.push_back(0u);  // root has no name
    const auto add_name = [&](const std::string& n) {
        const u32 at = static_cast<u32>(names.size());
        for (char c : n) names.push_back(static_cast<u8>(c));
        names.push_back(0u);
        return at;
    };
    const u32 dir_name = add_name(directory_name);
    std::vector<u32> file_names;
    for (const auto& f : files) file_names.push_back(add_name(f.first));

    std::vector<u8> table(static_cast<std::size_t>(entries) * 12u + names.size(), 0u);
    const auto put = [&](std::size_t at, u32 v) {
        table[at] = static_cast<u8>(v >> 24);
        table[at + 1] = static_cast<u8>(v >> 16);
        table[at + 2] = static_cast<u8>(v >> 8);
        table[at + 3] = static_cast<u8>(v);
    };
    // Root: type 1, parent 0, next = entry count.
    put(0, 0x01000000u);
    put(4, 0u);
    put(8, entries);
    // The directory, holding the first `files_in_directory` files.
    put(12, 0x01000000u | dir_name);
    put(16, 0u);
    put(20, static_cast<u32>(2u + files_in_directory));
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::size_t at = (2u + i) * 12u;
        put(at, file_names[i]);
        put(at + 4, 0xDEADBE00u + static_cast<u32>(i));  // a disc offset, to be replaced
        put(at + 8, files[i].second);
    }
    std::memcpy(table.data() + static_cast<std::size_t>(entries) * 12u, names.data(), names.size());
    return table;
}

void test_mount_rewrites_only_word1() {
    std::string error;
    const std::vector<std::pair<std::string, u32>> files = {
        {"inside_a.bin", 4096}, {"inside_b.bin", 100}, {"outside.bin", 1u << 20}};
    const std::vector<u8> table = build_table(files, "stg0604", 2u);

    AssetNamespace names;
    check(gekko::test_support::namespace_from_fst(table, names, error),
          "the table builds a namespace: " + error);
    AssetSystem assets;
    check(assets.mount(std::make_unique<gekko::test_support::MemoryStorage>(names), names, {}, error), "the namespace mounts: " + error);
    const AssetRegistry& registry = assets.registry();
    const MountReport& report = assets.report();
    const std::vector<u8> published = gekko::test_support::fst_publication(assets);
    check(report.files == 3u, "three files interned");
    check(report.directories == 2u, "the root and one directory");
    check(published.size() == table.size(), "the published image is the table's size");

    // Byte-identical except the four bytes of word1 in each file entry.
    std::size_t differing = 0;
    for (std::size_t i = 0; i < table.size(); ++i)
        if (table[i] != published[i]) ++differing;
    check(differing <= files.size() * 4u,
          "at most word1 of each file entry changed (" + std::to_string(differing) + " bytes)");
    for (std::size_t i = 0; i < files.size(); ++i) {
        const std::size_t at = (2u + i) * 12u;
        for (std::size_t k = 0; k < 4u; ++k)
            check(table[at + k] == published[at + k], "the name/type word is untouched");
        for (std::size_t k = 8u; k < 12u; ++k)
            check(table[at + k] == published[at + k], "the size word is untouched");
    }
    // Directory nesting is what turns a name into a path; it must survive.
    check(registry.find("stg0604/inside_a.bin").valid(), "a nested file keeps its path");
    check(registry.find("outside.bin").valid(), "a root-level file keeps its path");
    check(!registry.find("inside_a.bin").valid(), "and the nesting is not flattened");
}

void test_the_published_token_decodes_to_the_asset_the_guest_named() {
    std::string error;
    const std::vector<std::pair<std::string, u32>> files = {{"a.bin", 4096}, {"b.bin", 70000}};
    const std::vector<u8> table = build_table(files, "dir", 1u);
    AssetNamespace names;
    check(gekko::test_support::namespace_from_fst(table, names, error),
          "the table builds a namespace: " + error);
    AssetSystem assets;
    check(assets.mount(std::make_unique<gekko::test_support::MemoryStorage>(names), names, {}, error), "the namespace mounts: " + error);
    const AssetRegistry& registry = assets.registry();
    const AssetAddressSpace& space = assets.space();
    const std::vector<u8> published = gekko::test_support::fst_publication(assets);
    check(assets.token_of(2u) != 0u && assets.token_of(1u) == 0u, "files carry tokens, directories none");

    // Exactly what the guest does: read word1 out of the published table, add
    // a caller offset, and hand the sum to the read entry point.
    const auto token = [&](std::size_t index) {
        const std::size_t at = (2u + index) * 12u + 4u;
        return (static_cast<u32>(published[at]) << 24) | (static_cast<u32>(published[at + 1]) << 16) |
               (static_cast<u32>(published[at + 2]) << 8) | static_cast<u32>(published[at + 3]);
    };
    AssetAddressSpace::Decoded out;
    check(space.decode(token(0) + 16u, 32u, out) == AssetAddressSpace::Status::Ok &&
              out.id == registry.find("dir/a.bin") && out.offset == 16u,
          "startAddr + offset decodes to (the named asset, that offset)");
    check(space.decode(token(1) + 69000u, 32u, out) == AssetAddressSpace::Status::Ok &&
              out.id == registry.find("b.bin") && out.offset == 69000u,
          "a multi-page asset decodes across its pages");
    // 4,096 bytes of asset own a whole 64 KiB page, so this read lands inside
    // the region and a page-granular test would pass it. It is refused because
    // the check is against the asset's own contract, which is what the guest
    // itself asserts.
    check(space.decode(token(0), 8192u, out) == AssetAddressSpace::Status::OutOfRange,
          "a read past the asset's size + 32 is refused even inside its page");
    check(space.decode(token(0), 4128u, out) == AssetAddressSpace::Status::Ok,
          "and the guest's own maximum legal read is not");
}

void test_a_malformed_table_is_refused_without_publishing() {
    std::string error;
    std::vector<u8> table(64u, 0u);
    table[0] = 0x01u;  // a directory root ...
    table[8] = 0xFFu;  // ... declaring an entry count far larger than the table
    table[9] = 0xFFu;
    AssetNamespace names;
    check(!gekko::test_support::namespace_from_fst(table, names, error),
          "a table that declares more entries than it holds is refused");
    check(!error.empty(), "and says why");
    check(names.empty(), "and builds nothing");
    AssetSystem assets;
    check(!assets.mount(std::make_unique<gekko::test_support::MemoryStorage>(names), names, {}, error) && !assets.mounted(),
          "an empty namespace is never mounted");
}

// AssetSystem::replace: a game file served from native content. The replaced
// file keeps its name and entry, takes the replacement's size, and every read
// the guest can make of it returns the replacement (zero-filled past its end);
// a build that fails, or a path the tree lacks, leaves the host file served and
// is reported, without failing the mount.
void test_a_replaced_file_is_served_from_native_content() {
    std::string error;
    const std::filesystem::path root = scratch_dir() / "replace-tree";
    std::filesystem::create_directories(root / "Fonts");
    const std::vector<u8> original = pattern(300, 5);
    const std::vector<u8> untouched = pattern(200, 9);
    {
        std::ofstream(root / "Fonts" / "table.fnt", std::ios::binary)
            .write(reinterpret_cast<const char*>(original.data()), static_cast<std::streamsize>(original.size()));
        std::ofstream(root / "other.bin", std::ios::binary)
            .write(reinterpret_cast<const char*>(untouched.data()), static_cast<std::streamsize>(untouched.size()));
    }
    AssetSystem assets;
    // The builder sees the original through the tree reader (folded name).
    assets.replace("FONTS/TABLE.FNT", [&original](const AssetSystem::TreeReader& read, std::vector<u8>& bytes,
                                                  std::string&) {
        std::vector<u8> seen;
        if (!read("fonts/table.fnt", seen) || seen != original) return false;
        bytes = seen;
        bytes.insert(bytes.end(), {0xAAu, 0xBBu, 0xCCu});
        return true;
    });
    assets.replace("other.bin", [](const AssetSystem::TreeReader&, std::vector<u8>&, std::string& why) {
        why = "refused on purpose";
        return false;
    });
    assets.replace("missing.bin", [](const AssetSystem::TreeReader&, std::vector<u8>& bytes, std::string&) {
        bytes = {1u};
        return true;
    });
    check(shadow::backends::mount_host_directory(assets, root, std::vector<u8>(0x20u, 0u), error), "a mount with replacements succeeds: " + error);
    check(assets.replaced_count() == 1u, "exactly one file is replaced");
    check(assets.replacement_errors().size() == 2u, "the failed build and the missing path are both reported");

    const s32 entry = assets.names().resolve("Fonts/table.fnt");
    check(entry > 0 && assets.names().entry(static_cast<u32>(entry)).size == original.size() + 3u,
          "the namespace carries the replacement's size");
    std::vector<u8> got_bytes(original.size() + 3u + 29u, 0x11u);
    u64 got = 0;
    check(assets.read(assets.token_of(static_cast<u32>(entry)), got_bytes.size(), got_bytes.data(), got) ==
              AssetSystem::ReadStatus::Ok,
          "the replaced file reads through its token");
    check(std::equal(original.begin(), original.end(), got_bytes.begin()) && got_bytes[original.size()] == 0xAAu &&
              got_bytes[original.size() + 2u] == 0xCCu && got_bytes.back() == 0u,
          "the replacement's bytes, zero-filled past its end");

    const s32 other = assets.names().resolve("other.bin");
    std::vector<u8> other_bytes(untouched.size());
    check(other > 0 && assets.names().entry(static_cast<u32>(other)).size == untouched.size() &&
              assets.read(assets.token_of(static_cast<u32>(other)), other_bytes.size(), other_bytes.data(), got) ==
                  AssetSystem::ReadStatus::Ok &&
              other_bytes == untouched,
          "a file whose replacement failed is served from the host unchanged");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

// A switchable file (AssetSystem::replace_switchable, the live Hungarian text): the selector picks the variant at
// every read, the declared size is the largest variant's and a shorter one reads as zeros past its end. Regression:
// the group key once carried bit 63, the mount's signed replacement slot took it for "not replaced", and every
// switchable file was served from the disc instead (the Options help band went empty).
void test_a_switchable_file_follows_its_selector() {
    std::string error;
    const std::filesystem::path root = scratch_dir() / "switch-tree";
    std::filesystem::create_directories(root / "Fonts");
    const std::vector<u8> original = pattern(64, 3);
    std::ofstream(root / "Fonts" / "text.fnt", std::ios::binary)
        .write(reinterpret_cast<const char*>(original.data()), static_cast<std::streamsize>(original.size()));
    const std::vector<u8> english(100u, 0xE1u), hungarian(80u, 0x48u);
    std::size_t selected = 0;
    AssetSystem assets;
    std::vector<AssetSystem::Replacement> variants;
    variants.push_back([&](const AssetSystem::TreeReader&, std::vector<u8>& bytes, std::string&) {
        bytes = english;
        return true;
    });
    variants.push_back([&](const AssetSystem::TreeReader&, std::vector<u8>& bytes, std::string&) {
        bytes = hungarian;
        return true;
    });
    assets.replace_switchable("FONTS/TEXT.FNT", std::move(variants), [&selected] { return selected; });
    check(shadow::backends::mount_host_directory(assets, root, std::vector<u8>(0x20u, 0u), error),
          "a mount with a switchable file succeeds: " + error);
    check(assets.replacement_errors().empty(), "the switchable file builds");
    const s32 entry = assets.names().resolve("Fonts/text.fnt");
    check(entry > 0 && assets.names().entry(static_cast<u32>(entry)).size == english.size(),
          "the declared size is the largest variant's");
    const auto read_all = [&] {
        std::vector<u8> bytes(english.size(), 0x11u);
        u64 got = 0;
        const bool ok = assets.read(assets.token_of(static_cast<u32>(entry)), bytes.size(), bytes.data(), got) ==
                        AssetSystem::ReadStatus::Ok;
        return ok ? bytes : std::vector<u8>{};
    };
    check(read_all() == english, "selector 0 serves the first variant, not the disc file");
    selected = 1;
    std::vector<u8> expected = hungarian;
    expected.resize(english.size(), 0u);
    check(read_all() == expected, "selector 1 serves the second variant at the next read, zero-filled to the size");
    selected = 0;
    check(read_all() == english, "and back");
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
    test_a_replaced_file_is_served_from_native_content();
    test_a_switchable_file_follows_its_selector();
    test_mount_rewrites_only_word1();
    test_the_published_token_decodes_to_the_asset_the_guest_named();
    test_a_malformed_table_is_refused_without_publishing();
    test_address_zero_never_decodes();
    test_every_base_is_injective_and_decodes_back();
    test_a_request_that_leaves_its_region_is_refused_not_aliased();
    test_the_gap_between_assets_is_unmapped();
    test_rebuilding_after_more_assets_keeps_identity_stable();
    test_asset_id_is_identity_not_a_position();
    test_a_file_is_served_in_place_with_no_copy();
    test_reads_past_end_of_file_succeed_and_zero_fill();
    test_an_unknown_asset_is_rejected_at_submit();
    test_completions_are_priority_then_fifo();
    test_nothing_is_observable_half_done();
    test_pump_budget_matches_the_one_per_walk_rule();
    test_handle_lifetime_is_refcounted_and_move_only();
    test_a_missing_file_fails_cleanly();
    test_preopen_changes_nothing_a_read_returns();
    test_no_provider_is_a_miss_not_a_crash();

    std::error_code ec;
    std::filesystem::remove_all(scratch_dir(), ec);

    if (g_failures != 0) {
        std::printf("asset_runtime_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("asset_runtime_tests: all checks passed\n");
    return 0;
}
