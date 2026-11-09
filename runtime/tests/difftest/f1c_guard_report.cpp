// F1c test-only: at process exit, verify every canary of the guard allocator (f1c_guard_alloc.cpp, linked into
// the same test executable) and write its summary to the file named by the F1C_GUARD_REPORT environment
// variable. Lets an unmodified driver (difftest_driver.cpp) run its vector programs with every heap block
// guard-paged. Never linked into a shipped target.
#define _CRT_SECURE_NO_WARNINGS
#include "f1c_guard_alloc.h"

#include <cstdio>
#include <cstdlib>

namespace {

void write_report() {
    const std::size_t found = f1c::guard::check_all("exit");
    const char* path = std::getenv("F1C_GUARD_REPORT");
    if (path == nullptr || *path == '\0') return;
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) return;
    const std::string summary = f1c::guard::summary_json();
    std::fprintf(file, "{\"exit_check_new_violations\":%zu,\"guard\":%s}\n", found, summary.c_str());
    std::fclose(file);
}

struct Registrar {
    Registrar() { std::atexit(&write_report); }
} registrar;

}  // namespace
