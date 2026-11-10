// Shadow the Hedgehog Recompiled setup: a Win32 window that shows installer/web in WebView2.
// The page asks the host for native things through a small message bridge ("command|id|payload"):
// the Windows folder chooser, the PowerShell backend (installer/Backend.ps1, whitelisted commands
// only) and the install itself, which runs in a Job object so Cancel or closing the window stops
// the whole build. Derived from the approved installer demo (Native_Demo/src/main.cpp).
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <wrl.h>
#include <WebView2.h>
#include <mfplay.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include "resource.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

constexpr UINT PickFolderMessage = WM_APP + 1;
constexpr UINT CloseWindowMessage = WM_APP + 2;
constexpr UINT BackendDoneMessage = WM_APP + 3;
constexpr UINT InstallLineMessage = WM_APP + 4;
constexpr UINT InstallExitMessage = WM_APP + 5;
constexpr DWORD WindowStyle = WS_OVERLAPPEDWINDOW;
constexpr DWORD WindowExStyle = WS_EX_APPWINDOW;
constexpr int DesignWidth = 1265;
constexpr int DesignHeight = 648;
constexpr wchar_t PageHost[] = L"setup.nukkoro.local";
constexpr wchar_t ArtHost[] = L"art.nukkoro.local";
constexpr wchar_t PageUrl[] = L"https://setup.nukkoro.local/index.html";
constexpr wchar_t Title[] = L"Shadow the Hedgehog Recompiled Setup";
constexpr wchar_t GameExe[] = L"ShadowTheHedgehog.exe";

// ---- small helpers ----------------------------------------------------------------

std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::wstring Wide(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}
std::wstring JsonString(std::wstring_view text) {
    std::wstring result = L"\"";
    for (const wchar_t c : text) {
        if (c == L'"' || c == L'\\') { result += L'\\'; result += c; }
        else if (c < 0x20) { wchar_t escape[7]; swprintf_s(escape, L"\\u%04x", static_cast<unsigned>(c)); result += escape; }
        else result += c;
    }
    return result + L"\"";
}
// One argument quoted for CommandLineToArgvW (and PowerShell -File), backslashes included.
std::wstring QuoteArgument(std::wstring_view argument) {
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') result.append(backslashes * 2 + 1, L'\\');
        else result.append(backslashes, L'\\');
        backslashes = 0;
        result += c;
    }
    result.append(backslashes * 2, L'\\');
    return result + L"\"";
}
std::vector<std::wstring> Split(const std::wstring& text, wchar_t separator) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (true) {
        const size_t end = text.find(separator, start);
        parts.push_back(text.substr(start, end - start));
        if (end == std::wstring::npos) return parts;
        start = end + 1;
    }
}
bool LooksLikeJson(std::wstring_view text) {
    const auto first = text.find_first_not_of(L" \t\r\n");
    return first != std::wstring_view::npos && (text[first] == L'{' || text[first] == L'[');
}
fs::path ExeDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return fs::path(std::wstring(buffer.data(), count)).parent_path();
}
// The exe is built into <repo>/.build/setup/<config>/; the page and backend live in <repo>/installer.
fs::path FindInstallerDirectory() {
    for (fs::path dir = ExeDirectory(); !dir.empty(); dir = dir.parent_path()) {
        if (fs::is_regular_file(dir / "installer" / "web" / "index.html")) return dir / "installer";
        if (fs::is_regular_file(dir / "web" / "index.html") && fs::is_regular_file(dir / "Backend.ps1")) return dir;
        if (dir == dir.root_path()) break;
    }
    return {};
}
void WriteText(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    output << text;
}

RECT MonitorWorkArea(HWND window) {
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info);
    return info.rcWork;
}
RECT OuterRect(int clientWidth, int clientHeight, UINT dpi) {
    RECT bounds{0, 0, clientWidth, clientHeight};
    AdjustWindowRectExForDpi(&bounds, WindowStyle, FALSE, WindowExStyle, dpi);
    return bounds;
}
void FitInitialWindow(HWND window) {
    const RECT work = MonitorWorkArea(window);
    const UINT dpi = GetDpiForWindow(window);
    const RECT frame = OuterRect(0, 0, dpi);
    const int margin = MulDiv(24, dpi, 96);
    const int workWidth = work.right - work.left;
    const int workHeight = work.bottom - work.top;
    const int availableWidth = std::max<LONG>(1, workWidth - 2 * margin - (frame.right - frame.left));
    const int availableHeight = std::max<LONG>(1, workHeight - 2 * margin - (frame.bottom - frame.top));
    const double fit = std::min({dpi / 96.0, availableWidth / static_cast<double>(DesignWidth), availableHeight / static_cast<double>(DesignHeight)});
    const RECT outer = OuterRect(static_cast<int>(DesignWidth * fit), static_cast<int>(DesignHeight * fit), dpi);
    const int width = outer.right - outer.left;
    const int height = outer.bottom - outer.top;
    SetWindowPos(window, nullptr, work.left + (workWidth - width) / 2, work.top + (workHeight - height) / 2,
                 width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}
void RefreshWindowIcons(HWND window) {
    const UINT dpi = GetDpiForWindow(window);
    for (const auto type : {ICON_BIG, ICON_SMALL}) {
        const bool large = type == ICON_BIG;
        HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
            GetSystemMetricsForDpi(large ? SM_CXICON : SM_CXSMICON, dpi),
            GetSystemMetricsForDpi(large ? SM_CYICON : SM_CYSMICON, dpi), LR_SHARED));
        if (icon) SendMessageW(window, WM_SETICON, type, reinterpret_cast<LPARAM>(icon));
    }
}

// ---- child processes ------------------------------------------------------------------

struct BackendResult { std::wstring id; std::wstring output; DWORD exitCode = 1; };

// Starts a process with stdout/stderr on a pipe. Returns false on failure. The caller owns the handles.
bool StartPiped(const std::wstring& commandLine, const fs::path& directory, HANDLE job, PROCESS_INFORMATION& process, HANDLE& readPipe) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &inherit, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nul;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    std::wstring mutableLine = commandLine;
    const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | (job ? CREATE_SUSPENDED : 0);
    const BOOL created = CreateProcessW(nullptr, mutableLine.data(), nullptr, nullptr, TRUE, flags, nullptr,
                                        directory.c_str(), &startup, &process);
    CloseHandle(writePipe);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!created) { CloseHandle(readPipe); readPipe = nullptr; return false; }
    if (job) {
        AssignProcessToJobObject(job, process.hProcess);
        ResumeThread(process.hThread);
    }
    return true;
}

std::wstring PowerShellLine(const fs::path& installer, const std::vector<std::wstring>& arguments) {
    std::wstring line = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
                        QuoteArgument((installer / L"Backend.ps1").wstring());
    for (const auto& argument : arguments) line += L" " + QuoteArgument(argument);
    return line;
}

// ---- music ----------------------------------------------------------------------------------

// Plays the art pack's music from this process, so Windows lists it under the setup's name in the Volume Mixer.
// Media Foundation is loaded at run time because some Windows editions ship without it; the setup then has no music.
class MusicPlayer final : public IMFPMediaPlayerCallback {
public:
    bool Start(const fs::path& file) {
        if (player) return true;
        HMODULE library = LoadLibraryExW(L"mfplay.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library) return false;
        const auto create = reinterpret_cast<decltype(&MFPCreateMediaPlayer)>(GetProcAddress(library, "MFPCreateMediaPlayer"));
        return create && SUCCEEDED(create(file.c_str(), FALSE, MFP_OPTION_NONE, this, nullptr, &player));
    }
    void Play() {
        wanted = true;
        if (loaded) player->Play();
    }
    void Pause() {
        wanted = false;
        if (loaded) player->Pause();
    }
    void SetVolume(float value) {
        volume = value;
        if (loaded) player->SetVolume(volume);
    }
    void Shutdown() {
        if (player) player->Shutdown();
        player.Reset();
        loaded = false;
    }
    bool Available() const { return player && !failed; }
    int Loops() const { return loops; }
    bool Playing() const {
        MFP_MEDIAPLAYER_STATE state = MFP_MEDIAPLAYER_STATE_EMPTY;
        return loaded && SUCCEEDED(player->GetState(&state)) && state == MFP_MEDIAPLAYER_STATE_PLAYING;
    }
    long long PositionMs() const {
        if (!loaded) return 0;
        PROPVARIANT position;
        PropVariantInit(&position);
        long long result = 0;
        if (SUCCEEDED(player->GetPosition(MFP_POSITIONTYPE_100NS, &position)) && position.vt == VT_I8) result = position.hVal.QuadPart / 10000;
        PropVariantClear(&position);
        return result;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IMFPMediaPlayerCallback)) { *object = nullptr; return E_NOINTERFACE; }
        *object = static_cast<IMFPMediaPlayerCallback*>(this);
        return S_OK;
    }
    // The host owns this object and outlives the player.
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* event) override {
        if (!player) return;
        if (FAILED(event->hrEvent) || event->eEventType == MFP_EVENT_TYPE_ERROR) { failed = true; return; }
        if (event->eEventType == MFP_EVENT_TYPE_MEDIAITEM_SET) {
            loaded = true;
            NameSession();
            player->SetVolume(volume);
            if (wanted) player->Play();
        } else if (event->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED) {
            ++loops;
            PROPVARIANT start;
            PropVariantInit(&start);
            start.vt = VT_I8;
            start.hVal.QuadPart = 0;
            player->SetPosition(MFP_POSITIONTYPE_100NS, &start);
            if (wanted) player->Play();
        }
    }

private:
    // The name and icon the Volume Mixer shows for this process's audio.
    void NameSession() {
        ComPtr<IMMDeviceEnumerator> devices;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioSessionManager> manager;
        ComPtr<IAudioSessionControl> session;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))) ||
            FAILED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
            FAILED(device->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(manager.GetAddressOf()))) ||
            FAILED(manager->GetAudioSessionControl(nullptr, 0, &session))) return;
        session->SetDisplayName(L"Nukkoro Recompiled Setup", nullptr);
        wchar_t exe[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) session->SetIconPath((std::wstring(exe) + L",0").c_str(), nullptr);
    }

    ComPtr<IMFPMediaPlayer> player;
    bool wanted = true, loaded = false, failed = false;
    float volume = 0.25f;
    int loops = 0;
};

// ---- the host -------------------------------------------------------------------------------

class SetupHost {
public:
    HWND window = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    fs::path installer;
    bool artPresent = false;
    MusicPlayer music;
    bool folderPending = false;
    bool closing = false;
    int exitCode = 0;
    std::wstring folderId;
    std::wstring folderInitial;
    bool folderForDisc = false;

    // The running install, if any.
    HANDLE installJob = nullptr;
    bool installRunning = false;
    std::wstring installTarget;       // as requested; launch/open only after it succeeded
    std::wstring installedTarget;

    // Self-test: hidden window, scripted pages, snapshots into selftestDir.
    bool selftest = false;
    fs::path selftestDir;
    std::wstring selftestDisc, selftestTarget;
    bool selftestInstall = false;

    void Fail(HRESULT result, const wchar_t* operation) {
        exitCode = 2;
        wchar_t detail[512];
        swprintf_s(detail, L"%s failed (0x%08X).", operation, static_cast<unsigned>(result));
        if (selftest) WriteText(selftestDir / "host_error.txt", Utf8(detail));
        else MessageBoxW(window, detail, Title, MB_OK | MB_ICONERROR);
        PostMessageW(window, CloseWindowMessage, 0, 0);
    }
    void Resize() {
        if (!controller) return;
        RECT bounds{};
        GetClientRect(window, &bounds);
        controller->put_Bounds(bounds);
        controller->put_IsVisible(!IsIconic(window));
    }
    void Post(const std::wstring& json) { if (webview) webview->PostWebMessageAsJson(json.c_str()); }
    void Reply(const std::wstring& id, const std::wstring& fields) { Post(L"{\"id\":" + id + L"," + fields + L"}"); }
    static bool TrustedSource(const wchar_t* source) {
        if (!source) return false;
        const std::wstring url(source);
        return url == PageUrl || url.rfind(std::wstring(PageUrl) + L"?", 0) == 0;
    }

    void PickFolder() {
        const auto id = folderId;
        ComPtr<IFileOpenDialog> dialog;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        std::wstring path;
        if (SUCCEEDED(hr)) {
            FILEOPENDIALOGOPTIONS options{};
            hr = dialog->GetOptions(&options);
            if (SUCCEEDED(hr)) hr = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
            dialog->SetTitle(folderForDisc ? L"Choose the folder Dolphin extracted the disc into (it holds sys and files)"
                                           : L"Choose where to install Shadow the Hedgehog Recompiled");
            // Start at the typed folder, or the nearest existing parent of it.
            fs::path start = folderInitial;
            std::error_code error;
            while (!start.empty() && !fs::is_directory(start, error) && start != start.root_path()) start = start.parent_path();
            ComPtr<IShellItem> initial;
            if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&initial)))) dialog->SetFolder(initial.Get());
            if (SUCCEEDED(hr)) hr = dialog->Show(window);
            if (SUCCEEDED(hr)) {
                ComPtr<IShellItem> selected;
                hr = dialog->GetResult(&selected);
                PWSTR selectedPath = nullptr;
                if (SUCCEEDED(hr)) hr = selected->GetDisplayName(SIGDN_FILESYSPATH, &selectedPath);
                if (SUCCEEDED(hr)) { path = selectedPath; CoTaskMemFree(selectedPath); }
            }
        }
        folderPending = false;
        if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) Reply(id, L"\"error\":\"The Windows folder chooser failed.\"");
        else Reply(id, L"\"path\":" + JsonString(path));
    }

    // prereqs / defaults / check-disc / check-target, on a worker thread.
    void RunBackend(const std::wstring& id, const std::vector<std::wstring>& arguments) {
        const HWND target = window;
        const std::wstring line = PowerShellLine(installer, arguments);
        const fs::path directory = installer;
        std::thread([target, id, line, directory] {
            auto* result = new BackendResult{id};
            PROCESS_INFORMATION process{};
            HANDLE readPipe = nullptr;
            if (StartPiped(line, directory, nullptr, process, readPipe)) {
                std::string bytes;
                char buffer[4096];
                DWORD read = 0;
                while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read) bytes.append(buffer, read);
                WaitForSingleObject(process.hProcess, INFINITE);
                GetExitCodeProcess(process.hProcess, &result->exitCode);
                CloseHandle(process.hProcess); CloseHandle(process.hThread); CloseHandle(readPipe);
                result->output = Wide(bytes);
            } else {
                result->output = L"Could not start PowerShell.";
            }
            if (!PostMessageW(target, BackendDoneMessage, 0, reinterpret_cast<LPARAM>(result))) delete result;
        }).detach();
    }
    void BackendDone(BackendResult* result) {
        std::wstring output = result->output;
        while (!output.empty() && (output.back() == L'\n' || output.back() == L'\r')) output.pop_back();
        // The JSON is the last line; anything before it is PowerShell noise.
        const auto lastLine = output.find_last_of(L'\n');
        const std::wstring json = lastLine == std::wstring::npos ? output : output.substr(lastLine + 1);
        if (LooksLikeJson(json)) Reply(result->id, L"\"result\":" + json);
        else Reply(result->id, L"\"error\":" + JsonString(output.empty() ? L"The backend returned nothing." : output));
        delete result;
    }

    void StartInstall(const std::wstring& id, const std::vector<std::wstring>& fields) {
        if (installRunning) { Reply(id, L"\"error\":\"An installation is already running.\""); return; }
        if (fields.size() != 4) { Reply(id, L"\"error\":\"Bad install request.\""); return; }
        if (installJob) { CloseHandle(installJob); installJob = nullptr; }
        installJob = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(installJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        const std::wstring line = PowerShellLine(installer, {L"install", fields[0], fields[1], fields[2] == L"1" ? L"1" : L"0", fields[3] == L"1" ? L"1" : L"0"});
        PROCESS_INFORMATION process{};
        HANDLE readPipe = nullptr;
        if (!StartPiped(line, installer, installJob, process, readPipe)) { Reply(id, L"\"error\":\"Could not start the installation.\""); return; }
        installRunning = true;
        installTarget = fields[1];
        installedTarget.clear();
        Reply(id, L"\"started\":true");
        const HWND target = window;
        std::thread([target, process, readPipe] {
            std::string pending;
            char buffer[8192];
            DWORD read = 0;
            const auto flush = [&](bool all) {
                size_t newline;
                while ((newline = pending.find('\n')) != std::string::npos || (all && !pending.empty())) {
                    const size_t end = newline == std::string::npos ? pending.size() : newline;
                    auto* text = new std::wstring(Wide(std::string_view(pending).substr(0, end)));
                    pending.erase(0, newline == std::string::npos ? pending.size() : newline + 1);
                    if (!PostMessageW(target, InstallLineMessage, 0, reinterpret_cast<LPARAM>(text))) delete text;
                }
            };
            while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read) { pending.append(buffer, read); flush(false); }
            flush(true);
            DWORD code = 1;
            WaitForSingleObject(process.hProcess, INFINITE);
            GetExitCodeProcess(process.hProcess, &code);
            CloseHandle(process.hProcess); CloseHandle(process.hThread); CloseHandle(readPipe);
            PostMessageW(target, InstallExitMessage, code, 0);
        }).detach();
    }
    void InstallLine(std::wstring* text) {
        std::wstring line = *text;
        delete text;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        if (LooksLikeJson(line)) Post(L"{\"type\":\"install\",\"event\":" + line + L"}");
        else if (!line.empty()) Post(L"{\"type\":\"install\",\"event\":{\"type\":\"log\",\"text\":" + JsonString(line + L"\n") + L"}}");
    }
    void InstallExit(DWORD code) {
        installRunning = false;
        if (code == 0) installedTarget = installTarget;
        Post(L"{\"type\":\"install-exit\",\"code\":" + std::to_wstring(code) + L"}");
    }
    void CancelInstall() {
        if (installJob) TerminateJobObject(installJob, 1);
    }
    // Launch and open act only on the folder that was just installed successfully.
    bool InstalledFolder(const std::wstring& requested, fs::path& folder) {
        if (installedTarget.empty() || requested != installedTarget) return false;
        folder = installedTarget;
        std::error_code error;
        return fs::is_regular_file(folder / GameExe, error);
    }
    static bool AllowedUrl(const std::wstring& url) {
        for (const wchar_t* prefix : {L"https://cmake.org/", L"https://www.python.org/", L"https://visualstudio.microsoft.com/",
                                      L"https://developer.microsoft.com/", L"https://dolphin-emu.org/"})
            if (url.rfind(prefix, 0) == 0) return true;
        return false;
    }

    void Snapshot(const std::wstring& id, const std::wstring& name) {
        ComPtr<IStream> stream;
        const auto file = selftestDir / (name + L".png");
        HRESULT hr = SHCreateStreamOnFileEx(file.c_str(), STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream);
        if (FAILED(hr)) { Reply(id, L"\"error\":\"Cannot create snapshot file\""); return; }
        hr = webview->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
            Callback<ICoreWebView2CapturePreviewCompletedHandler>([this, stream, id](HRESULT result) -> HRESULT {
                if (FAILED(result) || FAILED(stream->Commit(STGC_DEFAULT))) Reply(id, L"\"error\":\"Snapshot failed\"");
                else Reply(id, L"\"captured\":true");
                return S_OK;
            }).Get());
        if (FAILED(hr)) Reply(id, L"\"error\":\"Cannot start snapshot\"");
    }

    // start / play / pause / volume:<0-100> / state; every answer reports the player's state.
    void MusicCommand(const std::wstring& id, const std::wstring& action) {
        std::error_code error;
        const fs::path file = installer / "art" / "audio" / "music.mp3";
        if (action == L"start") { if (artPresent && fs::is_regular_file(file, error)) music.Start(file); }
        else if (action == L"play") music.Play();
        else if (action == L"pause") music.Pause();
        else if (action.rfind(L"volume:", 0) == 0) music.SetVolume(std::clamp(_wtoi(action.c_str() + 7), 0, 100) / 100.0f);
        Reply(id, std::wstring(L"\"available\":") + (music.Available() ? L"true" : L"false") + L",\"playing\":" + (music.Playing() ? L"true" : L"false") +
                  L",\"positionMs\":" + std::to_wstring(music.PositionMs()) + L",\"loops\":" + std::to_wstring(music.Loops()));
    }

    void Message(ICoreWebView2WebMessageReceivedEventArgs* args) {
        LPWSTR source = nullptr;
        args->get_Source(&source);
        const bool trusted = TrustedSource(source);
        CoTaskMemFree(source);
        if (!trusted) return;
        LPWSTR raw = nullptr;
        if (FAILED(args->TryGetWebMessageAsString(&raw))) return;
        const std::wstring message(raw ? raw : L"");
        CoTaskMemFree(raw);
        if (message.size() > 65536) return;
        if (message == L"close") { CancelInstall(); PostMessageW(window, CloseWindowMessage, 0, 0); return; }

        const auto first = message.find(L'|');
        const auto second = first == std::wstring::npos ? std::wstring::npos : message.find(L'|', first + 1);
        if (second == std::wstring::npos) return;
        const auto command = message.substr(0, first);
        const auto id = message.substr(first + 1, second - first - 1);
        const auto payload = message.substr(second + 1);
        if (id.empty() || id.size() > 9 || id.find_first_not_of(L"0123456789") != std::wstring::npos) return;

        if (command == L"ping") {
            Reply(id, std::wstring(L"\"native\":true,\"version\":2,\"art\":") + (artPresent ? L"true" : L"false") +
                      L",\"selftest\":" + (selftest ? L"true" : L"false"));
        } else if (command == L"music") {
            MusicCommand(id, payload);
        } else if (command == L"folder" || command == L"disc-folder") {
            if (folderPending) { Reply(id, L"\"error\":\"The folder chooser is already open.\""); return; }
            folderPending = true; folderId = id; folderInitial = payload; folderForDisc = command == L"disc-folder";
            PostMessageW(window, PickFolderMessage, 0, 0);
        } else if (command == L"backend") {
            const auto fields = Split(payload, L'\t');
            const auto& name = fields[0];
            const size_t expected = name == L"prereqs" || name == L"defaults" ? 1 : name == L"check-disc" ? 2 : name == L"check-target" ? 3 : 0;
            if (expected == 0 || fields.size() != expected) { Reply(id, L"\"error\":\"Unknown backend request.\""); return; }
            RunBackend(id, fields);
        } else if (command == L"install") {
            StartInstall(id, Split(payload, L'\t'));
        } else if (command == L"cancel-install") {
            CancelInstall();
            Reply(id, L"\"cancelling\":true");
        } else if (command == L"launch" || command == L"open-folder") {
            fs::path folder;
            if (!InstalledFolder(payload, folder)) { Reply(id, L"\"error\":\"The game is not installed there.\""); return; }
            const auto result = command == L"launch"
                ? ShellExecuteW(window, L"open", (folder / GameExe).c_str(), nullptr, folder.c_str(), SW_SHOWNORMAL)
                : ShellExecuteW(window, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            Reply(id, reinterpret_cast<INT_PTR>(result) > 32 ? L"\"ok\":true" : L"\"error\":\"Windows could not open it.\"");
        } else if (command == L"open-url") {
            if (!AllowedUrl(payload)) { Reply(id, L"\"error\":\"Link not allowed.\""); return; }
            ShellExecuteW(window, L"open", payload.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            Reply(id, L"\"ok\":true");
        } else if (selftest && command == L"selftest-config") {
            Reply(id, L"\"disc\":" + JsonString(selftestDisc) + L",\"target\":" + JsonString(selftestTarget) +
                      L",\"install\":" + (selftestInstall ? L"true" : L"false"));
        } else if (selftest && command == L"selftest-snapshot" && !payload.empty() && payload.size() <= 40 &&
                   payload.find_first_not_of(L"abcdefghijklmnopqrstuvwxyz0123456789-") == std::wstring::npos) {
            Snapshot(id, payload);
        } else if (selftest && command == L"selftest-done") {
            WriteText(selftestDir / "selftest.json", Utf8(payload));
            exitCode = payload.find(L"\"passed\":true") != std::wstring::npos ? 0 : 1;
            PostMessageW(window, CloseWindowMessage, 0, 0);
        }
    }

    void Start() {
        LPWSTR version = nullptr;
        HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
        if (version) CoTaskMemFree(version);
        if (FAILED(hr) || !version) {
            const int answer = MessageBoxW(window,
                L"This setup needs the Microsoft Edge WebView2 Runtime, which is missing.\n\n"
                L"Open Microsoft's download page now? You can also install from the console:\n"
                L"Install.cmd -DiscPath <disc folder> -InstallPath <install folder>", Title, MB_YESNO | MB_ICONWARNING);
            if (answer == IDYES) ShellExecuteW(window, L"open", L"https://developer.microsoft.com/microsoft-edge/webview2/", nullptr, nullptr, SW_SHOWNORMAL);
            exitCode = 3;
            PostMessageW(window, CloseWindowMessage, 0, 0);
            return;
        }
        PWSTR appData = nullptr;
        fs::path userData;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData))) { Fail(E_FAIL, L"Find the user data folder"); return; }
        userData = fs::path(appData) / "NukkoroSetup" / "WebView2";
        CoTaskMemFree(appData);
        hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, userData.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([this](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (FAILED(result)) { Fail(result, L"Create the WebView2 environment"); return S_OK; }
                const HRESULT created = environment->CreateCoreWebView2Controller(window,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([this](HRESULT result, ICoreWebView2Controller* made) -> HRESULT {
                        if (FAILED(result)) { Fail(result, L"Create the WebView2 controller"); return S_OK; }
                        controller = made;
                        controller->get_CoreWebView2(&webview);
                        ComPtr<ICoreWebView2Controller3> rasterization;
                        if (SUCCEEDED(controller.As(&rasterization))) {
                            rasterization->put_ShouldDetectMonitorScaleChanges(!selftest);
                            rasterization->put_RasterizationScale(selftest ? 1.0 : GetDpiForWindow(window) / 96.0);
                        }
                        ComPtr<ICoreWebView2_3> mapping;
                        if (FAILED(webview.As(&mapping)) ||
                            FAILED(mapping->SetVirtualHostNameToFolderMapping(PageHost, (installer / "web").c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY))) {
                            Fail(E_FAIL, L"Map the setup pages"); return S_OK;
                        }
                        // The optional art pack: images the page may embed, nothing else.
                        if (artPresent) mapping->SetVirtualHostNameToFolderMapping(ArtHost, (installer / "art").c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                        ComPtr<ICoreWebView2Settings> settings;
                        webview->get_Settings(&settings);
                        settings->put_IsWebMessageEnabled(TRUE);
                        settings->put_AreDevToolsEnabled(FALSE);
                        settings->put_AreDefaultContextMenusEnabled(FALSE);
                        settings->put_IsStatusBarEnabled(FALSE);
                        settings->put_IsZoomControlEnabled(FALSE);
                        EventRegistrationToken token{};
                        webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT { Message(args); return S_OK; }).Get(), &token);
                        webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                LPWSTR url = nullptr; args->get_Uri(&url);
                                if (!TrustedSource(url)) args->put_Cancel(TRUE);
                                CoTaskMemFree(url); return S_OK; }).Get(), &token);
                        webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT { args->put_Handled(TRUE); return S_OK; }).Get(), &token);
                        Resize();
                        webview->Navigate(selftest ? (std::wstring(PageUrl) + L"?selftest=1").c_str() : PageUrl);
                        return S_OK;
                    }).Get());
                if (FAILED(created)) Fail(created, L"Start the WebView2 controller");
                return S_OK;
            }).Get());
        if (FAILED(hr)) Fail(hr, L"Start the WebView2 environment");
    }
};

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* host = reinterpret_cast<SetupHost*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        host = static_cast<SetupHost*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        host->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }
    if (!host) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_SIZE: host->Resize(); return 0;
    case WM_DPICHANGED: {
        const RECT* bounds = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window, nullptr, bounds->left, bounds->top, bounds->right - bounds->left, bounds->bottom - bounds->top, SWP_NOZORDER | SWP_NOACTIVATE);
        RefreshWindowIcons(window);
        host->Resize();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = GetDpiForWindow(window);
        const RECT minimum = OuterRect(MulDiv(960, dpi, 96), MulDiv(492, dpi, 96), dpi);
        const RECT work = MonitorWorkArea(window);
        limits->ptMinTrackSize.x = std::min(minimum.right - minimum.left, work.right - work.left);
        limits->ptMinTrackSize.y = std::min(minimum.bottom - minimum.top, work.bottom - work.top);
        return 0;
    }
    case PickFolderMessage: host->PickFolder(); return 0;
    case BackendDoneMessage: host->BackendDone(reinterpret_cast<BackendResult*>(lParam)); return 0;
    case InstallLineMessage: host->InstallLine(reinterpret_cast<std::wstring*>(lParam)); return 0;
    case InstallExitMessage: host->InstallExit(static_cast<DWORD>(wParam)); return 0;
    case CloseWindowMessage: host->closing = true; DestroyWindow(window); return 0;
    // The page asks before closing (and stops a running install); without a page, just close.
    case WM_CLOSE:
        if (host->webview && !host->closing) host->webview->ExecuteScript(L"window.requestClose ? requestClose() : chrome.webview.postMessage('close')", nullptr);
        else DestroyWindow(window);
        return 0;
    case WM_TIMER:
        if (wParam == 1 && host->selftest) {
            WriteText(host->selftestDir / "host_error.txt", "Self-test timed out.");
            host->exitCode = 2;
            host->CancelInstall();
            DestroyWindow(window);
        }
        return 0;
    case WM_DESTROY:
        host->music.Shutdown();
        if (host->controller) host->controller->Close();
        host->webview.Reset();
        host->controller.Reset();
        PostQuitMessage(host->exitCode);
        return 0;
    default: return DefWindowProcW(window, message, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    SetCurrentProcessExplicitAppUserModelID(L"Nukkoro.ShadowRecompiled.Setup");
    SetupHost host;

    // --installer-dir <dir>; --selftest <out dir> --disc <dir> --target <dir> [--install]
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        const bool hasValue = i + 1 < argc;
        if (argument == L"--installer-dir" && hasValue) host.installer = argv[++i];
        else if (argument == L"--selftest" && hasValue) { host.selftest = true; host.selftestDir = argv[++i]; }
        else if (argument == L"--disc" && hasValue) host.selftestDisc = argv[++i];
        else if (argument == L"--target" && hasValue) host.selftestTarget = argv[++i];
        else if (argument == L"--install") host.selftestInstall = true;
    }
    LocalFree(argv);
    if (host.installer.empty()) host.installer = FindInstallerDirectory();
    if (host.installer.empty() || !fs::is_regular_file(host.installer / "web" / "index.html") || !fs::is_regular_file(host.installer / "Backend.ps1")) {
        MessageBoxW(nullptr, L"The setup files were not found. Start the installer with Install.cmd from the source folder.", Title, MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 2;
    }
    std::error_code error;
    host.artPresent = fs::is_directory(host.installer / "art", error);
    if (host.selftest) fs::create_directories(host.selftestDir, error);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.lpszClassName = L"NukkoroSetupWindow";
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    if (!windowClass.hIcon) windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = CreateSolidBrush(RGB(20, 18, 19));
    RegisterClassExW(&windowClass);

    // Place the window on the monitor the user launched from, then size it for that monitor's DPI.
    POINT launchPoint{};
    GetCursorPos(&launchPoint);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(launchPoint, MONITOR_DEFAULTTONEAREST), &monitor);
    HWND window = CreateWindowExW(WindowExStyle, windowClass.lpszClassName, Title, WindowStyle,
                                  monitor.rcWork.left, monitor.rcWork.top, 640, 480, nullptr, nullptr, instance, &host);
    if (!window) { DeleteObject(windowClass.hbrBackground); CoUninitialize(); return 2; }
    if (host.selftest) {
        const RECT outer = OuterRect(DesignWidth, DesignHeight, GetDpiForWindow(window));
        SetWindowPos(window, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetTimer(window, 1, host.selftestInstall ? 3600 * 1000 : 120 * 1000, nullptr);
    } else {
        FitInitialWindow(window);
    }
    RefreshWindowIcons(window);
    const BOOL darkCaption = TRUE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkCaption, sizeof(darkCaption));
    ShowWindow(window, host.selftest ? SW_HIDE : showCommand);
    host.Start();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    if (host.installJob) CloseHandle(host.installJob);  // kills anything still running (KILL_ON_JOB_CLOSE)
    DeleteObject(windowClass.hbrBackground);
    CoUninitialize();
    return host.exitCode;
}
