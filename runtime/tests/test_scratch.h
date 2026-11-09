#pragma once

// A scratch directory name that is unique to this process. The tests' fixed names under the system temp directory
// (`gekko-file-api-reference` ...) are shared by every worktree and every concurrent ctest of the same user: one run's
// remove_all deletes the files another run is using (reproduced: two concurrent native_file_api_reference_tests, the second
// ends with "DVDOpen(): FAILED: the namespace did not mount"). A per-process name makes concurrent runs independent.

#include <filesystem>
#include <string>

#ifdef _WIN32
#include <process.h>
#define GEKKO_TEST_PID() static_cast<unsigned long>(_getpid())
#else
#include <unistd.h>
#define GEKKO_TEST_PID() static_cast<unsigned long>(getpid())
#endif

namespace gekko::test_support {

inline std::filesystem::path scratch_directory(const std::string& name) {
    return std::filesystem::temp_directory_path() / (name + "-" + std::to_string(GEKKO_TEST_PID()));
}

}  // namespace gekko::test_support
