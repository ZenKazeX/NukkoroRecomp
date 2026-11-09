#include "gekko/asset_trace.h"

#include <cstring>

#include "gekko/cpu.h"

namespace gekko {

std::atomic<AssetTracer*> AssetTracer::active_{nullptr};

namespace {

// Minimal JSON string escaping. Guest paths are ASCII in this image, but a
// corrupt read must not be able to produce a malformed record - the whole value
// of the JSONL is that it can be checked mechanically.
std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (ch < 0x20 || ch >= 0x7F) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04X", ch);
                    out += esc;
                } else {
                    out += static_cast<char>(ch);
                }
        }
    }
    return out;
}

std::string hex32(u32 value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", value);
    return buffer;
}

// First `count` bytes as hex. This is what makes the byte-mutation acceptance
// test possible without a debugger: the record carries the actual bytes the
// guest was handed, so a one-byte edit to the asset on disk is visible in the
// trace as a one-nibble change in this field and nowhere else.
std::string hex_bytes(const u8* bytes, std::size_t count) {
    if (bytes == nullptr) {
        return {};
    }
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(count * 2);
    for (std::size_t index = 0; index < count; ++index) {
        out += digits[bytes[index] >> 4];
        out += digits[bytes[index] & 0x0F];
    }
    return out;
}

// FNV-1a over the served bytes. Cheap, and enough to prove two runs handed the
// guest the same or different data.
u64 fnv1a(const u8* bytes, std::size_t count) {
    u64 hash = 1469598103934665603ull;
    for (std::size_t index = 0; index < count; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

}  // namespace

AssetTracer::AssetTracer(std::FILE* out, std::string filter)
    : out_(out), filter_(std::move(filter)) {}

AssetTracer::~AssetTracer() {
    set_active(nullptr);
    if (out_ != nullptr) {
        std::fflush(out_);
        std::fclose(out_);
    }
}

std::unique_ptr<AssetTracer> AssetTracer::create(const std::string& path,
                                                 const std::string& filter) {
    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (out == nullptr) {
        return nullptr;
    }
    return std::unique_ptr<AssetTracer>(new AssetTracer(out, filter));
}

bool AssetTracer::wants(const std::string& file) const {
    if (filter_.empty()) {
        return true;
    }
    return file.find(filter_) != std::string::npos;
}

void AssetTracer::write_line(const std::string& line) {
    std::fwrite(line.data(), 1, line.size(), out_);
    std::fputc('\n', out_);
    ++events_;
}

std::string AssetTracer::call_chain(Cpu* cpu) {
    if (cpu == nullptr) {
        return "[]";
    }
    // PowerPC EABI: a frame's word 0 is the back chain to its caller's frame
    // and a function saves its return address at 4(its caller's frame). At a
    // hooked entry r1 is still the caller's frame and LR its return address.
    std::string out = "[\"" + hex32(cpu->lr & ~3u) + "\"";
    u32 frame = cpu->gpr[1];
    for (int depth = 1; depth < 8; ++depth) {
        const u32 back = cpu->read_guest_u32(frame);
        if (back <= frame || (back & 7u) != 0u) break;
        const u32 saved = cpu->read_guest_u32(back + 4u);
        if (saved == 0u) break;
        out += ",\"" + hex32(saved & ~3u) + "\"";
        frame = back;
    }
    return out + "]";
}

void AssetTracer::file_read(u32 issue_pc, u32 issue_lr, const std::string& issue_chain,
                            u64 token, u32 length, u32 destination,
                            const std::string& resolved_file, u64 file_offset, const u8* bytes,
                            bool ok) {
    if (!wants(resolved_file)) {
        return;
    }
    std::lock_guard<std::mutex> guard(mutex_);
    const std::size_t head = length < 32u ? length : 32u;
    char buffer[1024];
    std::snprintf(
        buffer, sizeof(buffer),
        "{\"seq\":%llu,\"ev\":\"file_read\",\"stage\":\"FILE->READ->GUEST_MEMORY\","
        "\"decided_by\":\"guest\","
        "\"issue_pc\":\"%s\",\"issue_lr\":\"%s\","
        "\"token\":%llu,\"length\":%u,\"dest_guest\":\"%s\","
        "\"file\":\"%s\",\"file_offset\":%llu,\"file_resolution\":\"host-reverse-lookup\","
        "\"ok\":%s,\"bytes_fnv1a\":\"0x%016llx\",\"head_hex\":\"%s\",\"call_chain\":%s}",
        static_cast<unsigned long long>(seq_++),
        hex32(issue_pc).c_str(), hex32(issue_lr).c_str(),
        static_cast<unsigned long long>(token), length, hex32(destination).c_str(),
        json_escape(resolved_file).c_str(), static_cast<unsigned long long>(file_offset),
        ok ? "true" : "false",
        static_cast<unsigned long long>(bytes != nullptr ? fnv1a(bytes, length) : 0ull),
        hex_bytes(bytes, head).c_str(), issue_chain.c_str());
    write_line(buffer);
}

void AssetTracer::guest_fn(const Cpu& cpu, const char* role, u32 address) {
    // Guest pointer arguments are dereferenced through the same translation the
    // guest uses, so what lands in the record is exactly what the guest function
    // is about to read. r3-r6 cover every argument on this asset path.
    Cpu& mutable_cpu = const_cast<Cpu&>(cpu);
    std::string strings;
    for (int reg = 3; reg <= 6; ++reg) {
        const u32 value = cpu.gpr[reg];
        if (!cpu.guest_memory().mapped(GcAddr(value), 1u)) {
            continue;
        }
        const std::string text = mutable_cpu.read_guest_string(value, 128);
        bool printable = !text.empty();
        for (const char ch : text) {
            if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) >= 0x7F) {
                printable = false;
                break;
            }
        }
        if (printable && text.size() >= 4) {
            if (!strings.empty()) {
                strings += ",";
            }
            strings += "\"r" + std::to_string(reg) + "=" + json_escape(text) + "\"";
        }
    }
    std::lock_guard<std::mutex> guard(mutex_);
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer),
                  "{\"seq\":%llu,\"ev\":\"guest_fn\",\"stage\":\"PARSER\","
                  "\"decided_by\":\"guest\",\"role\":\"%s\",\"guest_fn\":\"%s\","
                  "\"guest_lr\":\"%s\",\"r3\":\"%s\",\"r4\":\"%s\",\"r5\":\"%s\",\"r6\":\"%s\","
                  "\"guest_strings\":[%s],\"call_chain\":%s}",
                  static_cast<unsigned long long>(seq_++), role, hex32(address).c_str(),
                  hex32(cpu.lr).c_str(), hex32(cpu.gpr[3]).c_str(), hex32(cpu.gpr[4]).c_str(),
                  hex32(cpu.gpr[5]).c_str(), hex32(cpu.gpr[6]).c_str(), strings.c_str(),
                  call_chain(&mutable_cpu).c_str());
    write_line(buffer);
}

void AssetTracer::guest_memory(const Cpu& cpu, const char* role, u32 guest_address,
                               u32 length) {
    Cpu& mutable_cpu = const_cast<Cpu&>(cpu);
    const u32 count = length < 32u ? length : 32u;
    u8 bytes[32]{};
    for (u32 index = 0; index < count; ++index) {
        bytes[index] = mutable_cpu.read_guest_u8(guest_address + index);
    }
    std::lock_guard<std::mutex> guard(mutex_);
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer),
                  "{\"seq\":%llu,\"ev\":\"guest_memory\",\"stage\":\"GUEST_MEMORY\","
                  "\"decided_by\":\"guest\",\"role\":\"%s\",\"guest_address\":\"%s\","
                  "\"length\":%u,\"head_hex\":\"%s\"}",
                  static_cast<unsigned long long>(seq_++), role, hex32(guest_address).c_str(),
                  length, hex_bytes(bytes, count).c_str());
    write_line(buffer);
}

void AssetTracer::note(const char* role, const std::string& text) {
    std::lock_guard<std::mutex> guard(mutex_);
    char buffer[1024];
    std::snprintf(buffer, sizeof(buffer),
                  "{\"seq\":%llu,\"ev\":\"note\",\"decided_by\":\"host\",\"role\":\"%s\","
                  "\"text\":\"%s\"}",
                  static_cast<unsigned long long>(seq_++), role, json_escape(text).c_str());
    write_line(buffer);
}

}  // namespace gekko
