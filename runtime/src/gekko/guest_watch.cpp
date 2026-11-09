// Gate 2's dynamic half: exact attribution of accesses to watched guest bytes. Contract: include/gekko/guest_watch.h.

#include "gekko/guest_watch.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"

#if defined(_WIN32) && defined(_M_X64) && !defined(GEKKO_SHIPPING)
#define GEKKO_GUEST_WATCH 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace gekko::guest_watch {

#if defined(GEKKO_GUEST_WATCH)
namespace {

constexpr std::uintptr_t kPage = 0x1000u;

struct State {
    std::mutex mutex;
    std::vector<Range> ranges;                            // sorted by base
    std::unordered_map<std::uintptr_t, u32> host_pages;  // host page (either view) -> guest page
    std::map<std::uintptr_t, u32> rows;                   // host entry of a translated row -> its guest address
    std::map<std::tuple<std::size_t, bool, u32, std::uint64_t>, Hit> hits;
    Totals totals;
    PVOID handler = nullptr;
    bool active = false;
    bool symbols = false;  // DbgHelp found the module's symbols: exact function extents (leaf functions included)
};
State& state() {
    static State s;
    return s;
}
// Pages this thread opened for one instruction. Plain zero-initialised storage on purpose: a thread_local with a constructor
// (std::vector) runs its dynamic initializer from the TLS callback of every thread, before the host ISA gate's initializer
// (native_host_isa_gate lane: "thread_local initializer ... runs from a TLS callback, before the host ISA gate").
struct OpenPages {
    static constexpr unsigned kCapacity = 16u;  // one faulting instruction touches a handful of pages at most
    std::uintptr_t page[kCapacity];
    unsigned count;
};
constinit thread_local OpenPages t_open{};

// A host entry may be an incremental-linking jump thunk (E9 rel32): the row's code is its target.
std::uintptr_t body_of(std::uintptr_t fn) {
    const auto* p = reinterpret_cast<const unsigned char*>(fn);
    if (p[0] == 0xE9u) {
        std::int32_t rel = 0;
        std::memcpy(&rel, p + 1, 4);
        return fn + 5u + static_cast<std::intptr_t>(rel);
    }
    return fn;
}

// The primary function of a (possibly chained) unwind record.
PRUNTIME_FUNCTION primary(PRUNTIME_FUNCTION rf, DWORD64 image) {
    for (int guard = 0; rf != nullptr && guard < 32; ++guard) {
        const auto* info = reinterpret_cast<const unsigned char*>(image + rf->UnwindData);
        const unsigned flags = info[0] >> 3, codes = info[2];
        if ((flags & UNW_FLAG_CHAININFO) == 0u) return rf;
        const std::size_t slots = (codes + 1u) & ~1u;  // the code array is padded to an even count
        rf = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<unsigned char*>(info + 4u + slots * 2u));
    }
    return rf;
}

// Walk the faulting stack: the first frame that is a translated row decides; none -> native (innermost RVA).
void attribute(const CONTEXT& faulting, u32& row, std::uint64_t& native_rva, std::uint64_t& native_address, bool& ok) {
    CONTEXT c = faulting;
    State& s = state();
    row = 0;
    native_rva = 0;
    ok = false;
    for (int depth = 0; depth < 64; ++depth) {
        DWORD64 image = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
        if (depth == 0) {
            native_rva = c.Rip - image;
            native_address = c.Rip;
        }
        // The function containing this address: from the symbols when there are any (exact for leaf functions too,
        // which have no unwind record), else from the unwind record.
        std::uintptr_t begin = 0;
        if (s.symbols) {
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
            auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            if (SymFromAddr(GetCurrentProcess(), c.Rip, &displacement, symbol)) {
                begin = static_cast<std::uintptr_t>(symbol->Address);
                // The generator emits a row's code as gekko::recomp::fn_XXXXXXXX (the entry) and, for most rows,
                // fn_XXXXXXXX_body (its code, called by the entry): the name is the row's guest address.
                const std::string_view name(symbol->Name, symbol->NameLen);
                const std::size_t at = name.find("recomp::fn_");
                if (at != std::string_view::npos && name.size() >= at + 19u) {
                    const std::string_view hexdigits = name.substr(at + 11u, 8u);
                    const std::string_view rest = name.substr(at + 19u);
                    if (hexdigits.find_first_not_of("0123456789ABCDEFabcdef") == std::string_view::npos &&
                        (rest.empty() || rest == "_body")) {
                        row = static_cast<u32>(std::stoul(std::string(hexdigits), nullptr, 16));
                        ok = true;
                        return;
                    }
                }
            }
        }
        if (begin == 0 && rf != nullptr) begin = static_cast<std::uintptr_t>(image + primary(rf, image)->BeginAddress);
        const auto it = begin != 0 ? s.rows.find(begin) : s.rows.end();
        if (it != s.rows.end()) {
            row = it->second;
            ok = true;
            return;
        }
        if (rf == nullptr) {  // a leaf: the return address is at the stack pointer
            if (c.Rsp == 0) return;
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
            continue;
        }
        PVOID handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, rf, &c, &handler_data, &frame, nullptr);
        if (c.Rip == 0) break;
    }
    ok = native_rva != 0;  // no translated frame: native runtime code
}

const Range* range_of(const State& s, u32 address, std::size_t& index) {
    for (std::size_t i = 0; i < s.ranges.size(); ++i) {
        const Range& r = s.ranges[i];
        if (address >= r.base && address - r.base < r.size) {
            index = i;
            return &r;
        }
    }
    return nullptr;
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
    State& s = state();
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && t_open.count != 0u) {
        for (unsigned i = 0; i < t_open.count; ++i) {
            DWORD old = 0;
            VirtualProtect(reinterpret_cast<void*>(t_open.page[i]), kPage, PAGE_NOACCESS, &old);
        }
        t_open.count = 0u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION || info->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const std::uintptr_t fault = static_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
    const std::uintptr_t page = fault & ~(kPage - 1u);
    u32 guest = 0;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.active) return EXCEPTION_CONTINUE_SEARCH;
        const auto it = s.host_pages.find(page);
        if (it == s.host_pages.end()) return EXCEPTION_CONTINUE_SEARCH;
        guest = it->second + static_cast<u32>(fault - page);
        ++s.totals.faults;
        std::size_t index = 0;
        if (range_of(s, guest, index) == nullptr) {
            ++s.totals.outside_ranges;
        } else {
            const bool write = info->ExceptionRecord->ExceptionInformation[0] == 1;
            u32 row = 0;
            std::uint64_t rva = 0, address = 0;
            bool ok = false;
            attribute(*info->ContextRecord, row, rva, address, ok);
            if (!ok) ++s.totals.unattributed;
            const auto key = std::make_tuple(index, write, row, row != 0 ? 0ull : rva);
            Hit& h = s.hits[key];
            if (h.count == 0) {
                h.label = s.ranges[index].label;
                h.write = write;
                h.row = row;
                h.native_rva = row != 0 ? 0 : rva;
                h.native_address = row != 0 ? 0 : address;
                h.first_guest_address = guest;
            }
            ++h.count;
        }
    }
    if (t_open.count == OpenPages::kCapacity) {  // never silently lose a page that must be re-protected
        std::fprintf(stderr, "guest watch: one instruction opened more than %u pages\n", OpenPages::kCapacity);
        std::abort();
    }
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(page), kPage, PAGE_READWRITE, &old);
    t_open.page[t_open.count++] = page;
    info->ContextRecord->EFlags |= 0x100u;  // trap after this one instruction
    return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

bool start(GuestMemory& memory, const RecompiledCode& code, std::vector<Range> ranges, std::string* error) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.active) {
        if (error) *error = "already watching";
        return false;
    }
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.base < b.base; });
    s.ranges = std::move(ranges);
    s.rows.clear();
    for (std::size_t i = 0; i < code.size(); ++i) {
        const RecompiledCode::Entry& e = code.entries()[i];
        if (e.fn != nullptr) s.rows.emplace(body_of(reinterpret_cast<std::uintptr_t>(e.fn)), e.address);
    }
    s.host_pages.clear();
    for (const Range& r : s.ranges) {
        for (u32 g = r.base & ~0xFFFu; g < r.base + r.size; g += 0x1000u) {
            u8* native = const_cast<u8*>(memory.peek_pointer(g, 0x1000u));
            if (native == nullptr) {
                if (error) *error = "guest page " + std::to_string(g) + " is not committed";
                return false;
            }
            s.host_pages[reinterpret_cast<std::uintptr_t>(native)] = g;
            s.host_pages[reinterpret_cast<std::uintptr_t>(memory.fm_window() + g)] = g;
        }
    }
    // Without symbols a leaf row (no unwind record) cannot be told from the code around it: the report says so.
    SymSetOptions(SYMOPT_UNDNAME);  // eager: a deferred module reports no symbol type yet
    s.symbols = SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    if (s.symbols) {
        IMAGEHLP_MODULE64 module{};
        module.SizeOfStruct = sizeof(module);
        DWORD64 image = 0;
        RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(&on_exception), &image, nullptr);
        s.symbols = SymGetModuleInfo64(GetCurrentProcess(), image, &module) != FALSE &&
                    (module.SymType == SymPdb || module.SymType == SymDia);
    }
    s.handler = AddVectoredExceptionHandler(1, &on_exception);
    for (const auto& [page, guest] : s.host_pages) {
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(page), kPage, PAGE_NOACCESS, &old)) {
            if (error) *error = "VirtualProtect failed for guest page " + std::to_string(guest);
            return false;
        }
    }
    s.active = true;
    return true;
}

void stop() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.active) return;
    for (const auto& [page, guest] : s.host_pages) {
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(page), kPage, PAGE_READWRITE, &old);
    }
    if (s.handler) RemoveVectoredExceptionHandler(s.handler);
    s.handler = nullptr;
    s.active = false;
}

bool active() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.active;
}

std::vector<Hit> hits() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    std::vector<Hit> out;
    for (const auto& [key, h] : s.hits) out.push_back(h);
    return out;
}

Totals totals() {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    Totals t = s.totals;
    t.symbols = s.symbols;
    return t;
}

// "function+0xoffset" for a native address, when symbols are loaded (no commas: it is a CSV field).
std::string symbol_name(std::uint64_t address) {
    State& s = state();
    if (!s.symbols || address == 0) return std::string();
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    if (!SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) return std::string();
    std::string name(symbol->Name);
    for (char& ch : name)
        if (ch == ',') ch = ';';
    char offset[32];
    std::snprintf(offset, sizeof(offset), "+0x%llX", static_cast<unsigned long long>(displacement));
    return name + offset;
}

#else  // not Windows x64, or a shipping build

bool start(GuestMemory&, const RecompiledCode&, std::vector<Range>, std::string* error) {
    if (error) *error = "the guest watch exists only in non-shipping Windows x64 builds";
    return false;
}
void stop() {}
bool active() { return false; }
std::vector<Hit> hits() { return {}; }
Totals totals() { return {}; }
std::string symbol_name(std::uint64_t) { return std::string(); }

#endif

bool write_report(const std::string& path, std::string* error) {
    std::ofstream out(path);
    if (!out) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    const Totals t = totals();
    out << "# guest watch (docs/GC_REMOVAL_PLAN.md gate 2, dynamic half)\n";
    out << "# symbols " << (t.symbols ? "yes (exact function extents)" : "NO (a leaf row is attributed as native code)")
        << "\n";
    out << "# faults " << t.faults << ", outside the ranges (same page) " << t.outside_ranges << ", unattributed "
        << t.unattributed << "\n";
    out << "# limits: page granularity; another thread's access while a page is open for one instruction is not "
           "seen\n";
    out << "label,kind,row,native_rva,native_symbol,count,first_guest_address\n";
    char buf[128];
    for (const Hit& h : hits()) {
        std::snprintf(buf, sizeof(buf), ",%s,0x%08X,0x%llX,", h.write ? "write" : "read", h.row,
                      static_cast<unsigned long long>(h.native_rva));
        out << h.label << buf << (h.row == 0 ? symbol_name(h.native_address) : std::string());
        std::snprintf(buf, sizeof(buf), ",%llu,0x%08X\n", static_cast<unsigned long long>(h.count),
                      h.first_guest_address);
        out << buf;
    }
    return static_cast<bool>(out);
}

bool parse_ranges(const std::string& path, std::vector<Range>& out, std::string* error) {
    std::ifstream in(path);
    if (!in) {
        if (error) *error = "cannot read " + path;
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string base, size, label;
        fields >> base >> size;
        std::getline(fields, label);
        label.erase(0, label.find_first_not_of(" \t"));
        Range r;
        r.base = static_cast<u32>(std::stoul(base, nullptr, 0));
        r.size = static_cast<u32>(std::stoul(size, nullptr, 0));
        r.label = label.empty() ? base : label;
        if (r.size == 0) {
            if (error) *error = "an empty range in " + path;
            return false;
        }
        out.push_back(std::move(r));
    }
    return true;
}

}  // namespace gekko::guest_watch
