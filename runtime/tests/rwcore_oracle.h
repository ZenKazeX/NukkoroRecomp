// Shared harness of the native RenderWare core oracles (rwcore/README.md): the game's ORIGINAL RenderWare functions
// run in the reference interpreter, with the RenderWare environment they need set up in guest memory - the small-data
// bases, RwEngineInstance pointing at a guest engine block, and the engine's memory functions as host stubs over a
// guest heap - so each native function can be compared with its original call by call.
#pragma once

#include "gekko/cpu.h"
#include "gekko/guest_memory.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <utility>
#include <stdexcept>
#include <string>
#include <vector>

namespace rworacle {
using namespace gekko;

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// PAL main.dol (rwcore/README.md): the small-data bases and the RenderWare globals the core modules use.
constexpr u32 kR2 = 0x805FA780u, kR13 = 0x805EC500u;
constexpr u32 kRwEngineInstance = 0x805F265Cu;  // RwEngineInstance (.sbss)
constexpr u32 kErrorModule = 0x805F2600u;       // errorModule {globalsOffset, numInstances}
constexpr u32 kVectorModule = 0x805F2640u;      // vectorModule
constexpr u32 kMatrixModule = 0x805F2608u;      // matrixModule

constexpr u32 kReturn = 0x817FF000u, kStack = 0x817E0000u;
constexpr u32 kEngine = 0x81100000u, kEngineSize = 0x1000u;     // the guest engine block
constexpr u32 kHeap = 0x81200000u, kHeapEnd = 0x81700000u;      // the guest heap behind rwmalloc
constexpr u32 kMallocStub = 0x817F0000u, kFreeStub = 0x817F0010u;
constexpr u32 kMemoryAllocStub = 0x817F0020u, kMemoryFreeStub = 0x817F0030u;   // RwGlobals memoryAlloc / memoryFree
constexpr u32 kReallocStub = 0x817F0050u;                                        // memoryFuncs.rwrealloc
constexpr u32 kBuffer = 0x81001000u;                             // scratch arguments

/// One original function, pinned by its first and last words.
struct Original {
    const char* name;
    u32 entry, size, first, last;
};

/// The reference interpreter with the game loaded and the RenderWare environment in place.
class Guest {
public:
    explicit Guest(const char* dol_path) : cpu_(memory_) {
        std::ifstream input(dol_path, std::ios::binary);
        const std::vector<u8> dol{std::istreambuf_iterator<char>(input), {}};
        require(dol.size() >= 256, "missing DOL");
        const auto word = [&](std::size_t offset) { return load_be32(dol.data() + offset); };
        for (unsigned i = 0; i < 18; ++i) {
            const u32 offset = word(i * 4), address = word(0x48 + i * 4), size = word(0x90 + i * 4);
            require(u64(offset) + size <= dol.size(), "invalid DOL section");
            if (size) memory_.write_block(address, dol.data() + offset, size);
        }
        reset_engine();
        stubs_[kMallocStub] = [this](Cpu& c) {
            const u32 size = c.gpr[3], hint = c.gpr[4];
            mallocs.push_back({size, hint});
            c.gpr[3] = static_cast<int>(mallocs.size()) - 1 != fail_call ? heap_alloc(size) : 0u;
        };
        stubs_[kFreeStub] = [this](Cpu& c) {
            frees.push_back(c.gpr[3]);
            heap_free(c.gpr[3]);
        };
        stubs_[kReallocStub] = [this](Cpu& c) {
            const u32 old = c.gpr[3], size = c.gpr[4], hint = c.gpr[5];
            reallocs.push_back({size, hint});
            if (static_cast<int>(reallocs.size()) - 1 == fail_realloc) {
                c.gpr[3] = 0;
                return;
            }
            const u32 fresh = heap_alloc(size);
            const auto it = used_.find(old);
            if (it != used_.end()) {
                std::vector<u8> bytes(std::min(it->second, size));
                memory_.peek_block(old, bytes.data(), bytes.size());
                memory_.write_block(fresh, bytes.data(), bytes.size());
                heap_free(old);
            }
            c.gpr[3] = fresh;
        };
    }

    GuestMemory& memory() { return memory_; }

    /// A host function standing in for the guest function at address (its return goes to LR).
    void stub(u32 address, std::function<void(Cpu&)> handler) { stubs_[address] = std::move(handler); }

    /// n bytes of the guest heap, 32-byte aligned (what the stubs hand out): first fit among freed ranges, else
    /// fresh space.
    u32 heap_alloc(u32 n) {
        const u32 need = (std::max(n, 1u) + 31u) & ~31u;
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            if (it->second >= need) {
                const u32 at = it->first, left = it->second - need;
                free_.erase(it);
                if (left) free_[at + need] = left;
                used_[at] = need;
                return at;
            }
        }
        require(heap_ + need <= kHeapEnd, "guest heap exhausted");
        const u32 at = heap_;
        heap_ += need;
        used_[at] = need;
        return at;
    }

    /// Returns a range heap_alloc handed out (others are ignored, as the stubs see foreign frees too).
    void heap_free(u32 address) {
        const auto it = used_.find(address);
        if (it == used_.end()) return;
        free_[it->first] = it->second;
        used_.erase(it);
    }
    Cpu& cpu() { return cpu_; }

    /// A fresh guest engine block: zeroed, rwmalloc / rwfree pointing at the stubs, RwEngineInstance set.
    void reset_engine() {
        std::vector<u8> zero(kEngineSize, 0);
        memory_.write_block(kEngine, zero.data(), zero.size());
        memory_.write32(kEngine + 0x108u, kMallocStub);  // memoryFuncs.rwmalloc (GameCube RwGlobals layout)
        memory_.write32(kEngine + 0x10Cu, kFreeStub);    // memoryFuncs.rwfree
        memory_.write32(kEngine + 0x110u, kReallocStub); // memoryFuncs.rwrealloc
        memory_.write32(kEngine + 0x118u, kMemoryAllocStub);  // memoryAlloc (free-list allocation)
        memory_.write32(kEngine + 0x11Cu, kMemoryFreeStub);   // memoryFree
        memory_.write32(kRwEngineInstance, kEngine);
        heap_ = kHeap;
        used_.clear();
        free_.clear();
        mallocs.clear();
        frees.clear();
        reallocs.clear();
        fail_call = -1;
        fail_realloc = -1;
    }

    void check_pinned(const Original& f) const {
        require(memory_.peek32(f.entry) == f.first && memory_.peek32(f.entry + f.size - 4) == f.last,
                "the DOL's original function is not the pinned one");
    }

    /// Calls the original at entry with r3.. = gprs and f1.. = fprs (doubles) until it returns (at most maxSteps
    /// instructions); the host stubs answer calls to the engine's memory functions. Returns the instructions run.
    u64 call(u32 entry, std::initializer_list<u32> gprs, std::initializer_list<double> fprs = {},
             u64 maxSteps = 50'000'000u) {
        cpu_.reset();
        cpu_.set_msr(cpu_.msr() | kMsrFP);
        cpu_.pc = entry;
        cpu_.npc = entry + 4;
        cpu_.lr = kReturn;
        cpu_.gpr[1] = kStack;
        cpu_.gpr[2] = kR2;
        cpu_.gpr[13] = kR13;
        unsigned r = 3;
        for (u32 v : gprs) cpu_.gpr[r++] = v;
        r = 1;
        for (double v : fprs) {
            cpu_.fpr[r].bits[0] = std::bit_cast<u64>(v);
            cpu_.fpr[r].bits[1] = std::bit_cast<u64>(v);
            ++r;
        }
        for (unsigned i = 0; i < 8; ++i) cpu_.spr[SPR_GQR0 + i] = 0;  // qr0 = plain floats, as RenderWare sets it
        u64 steps = 0;
        while (cpu_.pc != kReturn && !cpu_.stopped() && steps < maxSteps) {
            const auto stub = stubs_.find(cpu_.pc);
            if (stub != stubs_.end()) {
                stub->second(cpu_);
                cpu_.pc = cpu_.lr;
                cpu_.npc = cpu_.lr + 4;
                continue;
            }
            cpu_.run_steps(1);
            ++steps;
        }
        require(!cpu_.stopped() && cpu_.pc == kReturn && cpu_.gpr[1] == kStack, "original did not return cleanly");
        return steps;
    }

    u32 r3() const { return cpu_.gpr[3]; }
    float f1() const { return static_cast<float>(std::bit_cast<double>(cpu_.fpr[1].bits[0])); }

    void write_float(u32 address, float v) { memory_.write32(address, std::bit_cast<u32>(v)); }
    float read_float(u32 address) const { return std::bit_cast<float>(memory_.peek32(address)); }

    struct Malloc {
        u32 size, hint;
    };
    std::vector<Malloc> mallocs;  // every rwmalloc the original made, in order
    std::vector<u32> frees;       // every rwfree
    int fail_call = -1;           // the rwmalloc call with this index (0 = the first) returns NULL
    std::vector<Malloc> reallocs; // every rwrealloc: new size, hint
    int fail_realloc = -1;        // the rwrealloc call with this index returns NULL

private:
    GuestMemory memory_;
    Cpu cpu_;
    std::map<u32, std::function<void(Cpu&)>> stubs_;
    u32 heap_ = kHeap;
    std::map<u32, u32> used_, free_;   // guest heap ranges: start -> size
};

/// A deterministic generator (xorshift) for oracle inputs.
struct Rng {
    u64 s = 0x2026110120261101ull;
    u32 next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<u32>(s >> 16);
    }
    /// A finite float from 2^lo to 2^hi in magnitude, both signs, with +0 / -0 now and then.
    float real(int lo = -20, int hi = 20) {
        const u32 r = next();
        if ((r & 63u) == 0) return (r & 64u) ? -0.0f : 0.0f;
        const u32 exponent = static_cast<u32>(127 + lo) + next() % static_cast<u32>(hi - lo + 1);
        return std::bit_cast<float>(((r & 128u) ? 0x80000000u : 0u) | (exponent << 23) | (next() & 0x7FFFFFu));
    }
};

}  // namespace rworacle
