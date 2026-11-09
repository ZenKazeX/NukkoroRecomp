#pragma once

// ES-T1 (2026-10-20): name the calling thread for the OS (SetThreadDescription), so samplers, debuggers, ETW / WPR
// and the resource census attribute every runtime thread exactly instead of inferring it from stacks. Inert: no
// behaviour depends on a thread's description. Independent of Tracy (whose GEKKO_PROFILE_THREAD names threads only
// in Tracy builds). Resolved dynamically, so a system without SetThreadDescription simply leaves the thread unnamed.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gekko {

inline void name_this_thread(const char* name) noexcept {
#if defined(_WIN32)
    using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const SetDescription set_description = []() -> SetDescription {
        const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        return kernel != nullptr
                   ? reinterpret_cast<SetDescription>(reinterpret_cast<void*>(GetProcAddress(kernel, "SetThreadDescription")))
                   : nullptr;
    }();
    if (set_description == nullptr || name == nullptr) return;
    wchar_t wide[64];
    int i = 0;
    for (; i < 63 && name[i] != '\0'; ++i) wide[i] = static_cast<wchar_t>(static_cast<unsigned char>(name[i]));
    wide[i] = L'\0';
    set_description(GetCurrentThread(), wide);
#else
    (void)name;
#endif
}

}  // namespace gekko
