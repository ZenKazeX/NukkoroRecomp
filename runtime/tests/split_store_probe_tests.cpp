// liveness-roots: reference-interpreter-only
//
// FM (CTest, integrator 14:4x): the split-store / split-load fault behaviour FM relies on, checked on the CPU the
// tests run on instead of assumed. Originally the 13:5x probe; FM open question (integrator 13:2x), measured on this CPU: a store / load that crosses from a read-write page into
// a NOACCESS page. Does the faulting store commit the bytes that fall in the first page? Does the faulting load write
// its destination? Every width the FM accessors use (2 / 4 / 8 bytes), both instruction forms (MOVBE via
// _store_be_* / _load_be_*, plain MOV via volatile pointers), every split offset (1 .. width-1 bytes in the first
// page), 100 repetitions each.
#include <windows.h>
#include <immintrin.h>
#include <intrin.h>
#include <initializer_list>
#include <cstdint>
#include <cstdio>
#include <cstring>

static unsigned char* g_page;  // two pages: [0] RW, [1] NOACCESS
static SYSTEM_INFO g_si;

template <typename F>
static bool faults(F&& f) {
    __try {
        f();
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return true;
    }
    return false;
}

int main() {
    GetSystemInfo(&g_si);
    const size_t ps = g_si.dwPageSize;
    g_page = static_cast<unsigned char*>(VirtualAlloc(nullptr, 2 * ps, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    DWORD old;
    VirtualProtect(g_page + ps, ps, PAGE_NOACCESS, &old);
    int partial_commits = 0, no_fault = 0, cases = 0, load_dest_written = 0;
    for (int rep = 0; rep < 100; ++rep) {
        static const int kWidths[] = {2, 4, 8};
        for (int width : kWidths) {
            for (int inside = 1; inside < width; ++inside) {
                unsigned char* p = g_page + ps - inside;
                for (int form = 0; form < 2; ++form) {
                    std::memset(g_page + ps - 16, 0xAA, 16);
                    bool f = false;
                    if (form == 0) {  // MOVBE
                        if (width == 2) f = faults([&] { _store_be_u16(p, 0x1122u); });
                        if (width == 4) f = faults([&] { _store_be_u32(p, 0x11223344u); });
                        if (width == 8) f = faults([&] { _store_be_u64(p, 0x1122334455667788ull); });
                    } else {  // MOV
                        if (width == 2) f = faults([&] { *reinterpret_cast<volatile uint16_t*>(p) = 0x1122u; });
                        if (width == 4) f = faults([&] { *reinterpret_cast<volatile uint32_t*>(p) = 0x11223344u; });
                        if (width == 8)
                            f = faults([&] { *reinterpret_cast<volatile uint64_t*>(p) = 0x1122334455667788ull; });
                    }
                    ++cases;
                    if (!f) ++no_fault;
                    for (int i = 0; i < 16; ++i)
                        if (g_page[ps - 16 + i] != 0xAA) {
                            ++partial_commits;
                            if (partial_commits <= 10)
                                std::printf("PARTIAL store: width %d, %d bytes inside, form %s\n", width, inside,
                                            form ? "MOV" : "MOVBE");
                            break;
                        }
                    // load: the destination keeps its sentinel when the load faults
                    volatile uint64_t dest = 0x5A5A5A5A5A5A5A5Aull;
                    bool lf = false;
                    if (form == 0) {
                        if (width == 2) lf = faults([&] { dest = _load_be_u16(p); });
                        if (width == 4) lf = faults([&] { dest = _load_be_u32(p); });
                        if (width == 8) lf = faults([&] { dest = _load_be_u64(p); });
                    } else {
                        if (width == 2) lf = faults([&] { dest = *reinterpret_cast<volatile uint16_t*>(p); });
                        if (width == 4) lf = faults([&] { dest = *reinterpret_cast<volatile uint32_t*>(p); });
                        if (width == 8) lf = faults([&] { dest = *reinterpret_cast<volatile uint64_t*>(p); });
                    }
                    if (!lf) ++no_fault;
                    if (dest != 0x5A5A5A5A5A5A5A5Aull) ++load_dest_written;
                }
            }
        }
    }
    int cpu[4];
    __cpuid(cpu, 1);
    std::printf("page %zu B; MOVBE %s; cases %d (x2 for loads); split stores that did not fault %d; partial commits %d; "
                "loads that wrote their destination %d\n",
                ps, (cpu[2] & (1 << 22)) ? "yes" : "NO", cases, no_fault, partial_commits, load_dest_written);
    return (partial_commits || no_fault || load_dest_written) ? 1 : 0;
}
