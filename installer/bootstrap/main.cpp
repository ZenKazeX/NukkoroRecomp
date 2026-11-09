// The downloadable installer: one .exe that unpacks the release package it carries and opens the setup window.
// Usage: NukkoroRecomp-<version>-Setup.exe [--extract-dir <dir>] [--extract-only] [any setup window arguments]

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr int kPackageResource = 101, kSetupResource = 102;
constexpr const wchar_t* kTitle = L"Nukkoro Recompiled Setup";

std::wstring env(const wchar_t* name) {
    std::wstring value(32768, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size())));
    return value;
}

std::wstring quoted(const std::wstring& argument) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') {
            ++slashes;
            out += c;
            continue;
        }
        if (c == L'"') out.append(slashes + 1, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes, L'\\');
    return out + L"\"";
}

bool write_resource(int id, const fs::path& target, std::uint64_t& size) {
    HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (found == nullptr) return false;
    size = SizeofResource(nullptr, found);
    const void* data = LockResource(LoadResource(nullptr, found));
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out.flush());
}

// Runs a program without a window, keeping this window responsive. False when it cannot start or exits non-zero.
bool run_hidden(std::wstring command, const fs::path& directory) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, directory.c_str(), &si, &pi)) return false;
    for (;;) {
        const DWORD wake = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, INFINITE, QS_ALLINPUT);
        if (wake == WAIT_OBJECT_0) break;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

int fail(const std::wstring& message) {
    MessageBoxW(nullptr, message.c_str(), kTitle, MB_ICONERROR | MB_OK);
    return 1;
}

HWND show_progress() {
    WNDCLASSW cls{};
    cls.lpfnWndProc = DefWindowProcW;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    cls.lpszClassName = L"NukkoroBootstrap";
    cls.hIcon = LoadIconW(cls.hInstance, MAKEINTRESOURCEW(1));
    RegisterClassW(&cls);
    const int dpi = GetDpiForSystem();
    const int w = MulDiv(380, dpi, 96), h = MulDiv(120, dpi, 96);
    HWND window = CreateWindowExW(0, cls.lpszClassName, kTitle, WS_OVERLAPPED | WS_CAPTION | WS_VISIBLE,
                                  (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, nullptr, nullptr, cls.hInstance, nullptr);
    HWND text = CreateWindowW(L"STATIC", L"Preparing the setup, please wait...", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE, 0, 0, w, MulDiv(70, dpi, 96), window, nullptr, cls.hInstance, nullptr);
    HFONT font = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SendMessageW(text, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    UpdateWindow(window);
    return window;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    // A folder with a plain path: the game's build tools run inside it, and a user name may hold any characters.
    fs::path target = fs::path(env(L"PUBLIC")) / L"NukkoroRecomp" / NUKKORO_VERSION;
    bool extract_only = false;
    std::wstring forwarded;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--extract-dir" && i + 1 < argc) target = argv[++i];
        else if (argument == L"--extract-only") extract_only = true;
        else forwarded += L" " + quoted(argument);
    }
    LocalFree(argv);

    std::error_code ec;
    const fs::path marker = target / L".unpacked";
    const fs::path setup = target / L"NukkoroSetup.exe";
    const bool ready = fs::exists(marker, ec) && fs::exists(setup, ec) && fs::exists(target / L"installer" / L"Backend.ps1", ec);
    if (!ready) {
        HWND progress = show_progress();
        fs::create_directories(target, ec);
        if (ec) return fail(L"Could not create the folder:\n" + target.wstring());
        const fs::path package = target / L"package.zip";
        std::uint64_t size = 0;
        if (!write_resource(kPackageResource, package, size) || !write_resource(kSetupResource, setup, size))
            return fail(L"Could not unpack the setup into:\n" + target.wstring() + L"\n\nCheck that the drive has free space.");
        const std::wstring tar = env(L"SystemRoot") + L"\\System32\\tar.exe";
        if (!run_hidden(quoted(tar) + L" -xf package.zip --strip-components=1", target))
            return fail(L"Could not unpack the setup. This Windows may be too old: Windows 10 version 1803 or newer is needed.");
        fs::remove(package, ec);
        std::ofstream(marker) << "unpacked\n";
        DestroyWindow(progress);
    }
    if (extract_only) return 0;

    std::wstring command = quoted(setup.wstring()) + L" --installer-dir " + quoted((target / L"installer").wstring()) + forwarded;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, target.c_str(), &si, &pi))
        return fail(L"Could not open the setup window.");
    DWORD code = 0;
    if (!forwarded.empty()) {  // a self-test: its result is ours
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}
