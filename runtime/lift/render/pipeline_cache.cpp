#include "render/pipeline_cache.h"

#include <fstream>
#include <iterator>
#include <system_error>

namespace shadow::render {

namespace {

constexpr std::size_t kHeaderBytes = 4 + 4 + 8 + 4;  // magic, version, build, count
constexpr std::size_t kRecordBytes = 16;
constexpr std::size_t kTrailerBytes = 8;  // the hash of everything before it

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int s = 0; s < 32; s += 8) out.push_back(static_cast<std::uint8_t>(v >> s));
}
void put64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int s = 0; s < 64; s += 8) out.push_back(static_cast<std::uint8_t>(v >> s));
}
std::uint32_t get32(const std::uint8_t* p) {
    std::uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
std::uint64_t get64(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

bool fail(std::string* why, const std::string& text) {
    if (why != nullptr) *why = text;
    return false;
}

}  // namespace

PipelineRecord canonical_mesh(PipelineRecord r, std::uint8_t blend_one, std::uint8_t blend_zero,
                              std::uint8_t immediate) noexcept {
    if (r.kind != PipelineRecord::Kind::Mesh) return r;
    if ((r.flags & PipelineRecord::kPlain) != 0u) {
        r.src = blend_one;
        r.dst = blend_zero;
    }
    if ((r.flags & PipelineRecord::kNoDepth) != 0u)
        r.flags = static_cast<std::uint8_t>(r.flags & ~(PipelineRecord::kDepthTest | PipelineRecord::kDepthWrite));
    if (r.program != immediate) r.flags = static_cast<std::uint8_t>(r.flags & ~PipelineRecord::kLines);
    return r;
}

std::uint64_t pipeline_cache_hash(const void* data, std::size_t size, std::uint64_t seed) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t h = seed;
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

std::vector<std::uint8_t> PipelineCache::encode(std::uint64_t build, const std::vector<PipelineRecord>& records) {
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderBytes + records.size() * kRecordBytes + kTrailerBytes);
    put32(out, kMagic);
    put32(out, kVersion);
    put64(out, build);
    put32(out, static_cast<std::uint32_t>(records.size()));
    for (const PipelineRecord& r : records) {
        out.push_back(static_cast<std::uint8_t>(r.kind));
        out.push_back(r.program);
        out.push_back(r.src);
        out.push_back(r.dst);
        out.push_back(r.cull);
        out.push_back(r.flags);
        out.push_back(0u);
        out.push_back(0u);
        put32(out, r.format);
        put32(out, r.samples);
    }
    put64(out, pipeline_cache_hash(out.data(), out.size()));
    return out;
}

bool PipelineCache::decode(const std::vector<std::uint8_t>& bytes, std::uint64_t build,
                           std::vector<PipelineRecord>* out, std::string* why) {
    if (bytes.size() < kHeaderBytes + kTrailerBytes) return fail(why, "too short");
    const std::uint8_t* p = bytes.data();
    if (get32(p) != kMagic) return fail(why, "not a pipeline cache");
    if (get32(p + 4) != kVersion) return fail(why, "another format version");
    if (get64(p + 8) != build) return fail(why, "written by another build");
    const std::uint32_t count = get32(p + 16);
    if (count > kMaxRecords) return fail(why, "too many records");
    if (bytes.size() != kHeaderBytes + std::size_t{count} * kRecordBytes + kTrailerBytes)
        return fail(why, "wrong length");
    const std::size_t body = bytes.size() - kTrailerBytes;
    if (get64(p + body) != pipeline_cache_hash(p, body)) return fail(why, "damaged");
    std::vector<PipelineRecord> records;
    records.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* r = p + kHeaderBytes + std::size_t{i} * kRecordBytes;
        if (r[0] < 1u || r[0] > 3u) return fail(why, "unknown record kind");
        PipelineRecord record;
        record.kind = static_cast<PipelineRecord::Kind>(r[0]);
        record.program = r[1];
        record.src = r[2];
        record.dst = r[3];
        record.cull = r[4];
        record.flags = r[5];
        record.format = get32(r + 8);
        record.samples = get32(r + 12);
        records.push_back(record);
    }
    *out = std::move(records);
    return true;
}

bool PipelineCache::load(const std::filesystem::path& path, std::string* why) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail(why, "no file");
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) return fail(why, "cannot be read");
    std::vector<PipelineRecord> records;
    if (!decode(bytes, build_, &records, why)) return false;
    for (const PipelineRecord& r : records) {
        if (known_.insert(r).second) order_.push_back(r);
    }
    return true;
}

bool PipelineCache::save(const std::filesystem::path& path, std::string* error) {
    const std::vector<std::uint8_t> bytes = encode(build_, order_);
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return fail(error, "cannot write " + temporary.string());
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return fail(error, "cannot write " + temporary.string());
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return fail(error, "cannot replace " + path.string());
    }
    dirty_ = false;
    return true;
}

bool PipelineCache::note(const PipelineRecord& record) {
    if (known_.size() >= kMaxRecords || !known_.insert(record).second) return false;
    order_.push_back(record);
    dirty_ = true;
    return true;
}

}  // namespace shadow::render
