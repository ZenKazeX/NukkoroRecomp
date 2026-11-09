#include "gekko/host_window.h"
#include "gekko/audio_audit.h"

#include "gekko/display.h"
#include "gekko/plume_present.h"
#include "gekko/profiling.h"
#include "gekko/native_failure_publication.h"
#include "gekko/xinput_pad.h"
#include "peripheral/keyboard_layout.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace gekko {
namespace {

#ifdef _WIN32

constexpr wchar_t kWindowClass[] = L"ShadowTheHedgehogNative";
bool g_close_requested = false;

// ES-W3 (2026-10-20): the keyboard from this window's own key messages instead of ~35 GetAsyncKeyState syscalls per
// UI iteration (14.5 % of the UI thread's CPU in the W1 trace). Everything runs on the UI thread that pumps the
// window. Key messages only reach the focused window, so a key is only ever "held" while the game has the keyboard,
// which is what the old foreground test enforced; losing focus releases every key.
// g_key_held: the key's level. g_key_fresh: presses since the last pump() read, counted only for a WM_KEYDOWN whose
// previous-state bit (lparam bit 30) is clear - autorepeat and a key already held when focus returned never count,
// which is what the old rising-edge tracking of the physical level guaranteed.
bool g_key_held[256] = {};
unsigned g_key_fresh[256] = {};

void release_all_keys() noexcept {
    for (bool& held : g_key_held) held = false;
}

// Left clicks in the client area since the last pump() read, and the last one's client pixel: a replay's "the bug is
// here" point (HostWindow::set_click_marks).
unsigned g_clicks = 0;
int g_click_x = 0, g_click_y = 0;

bool take_fresh(int key) noexcept {
    const bool fresh = g_key_fresh[key & 0xFF] != 0u;
    g_key_fresh[key & 0xFF] = 0u;
    return fresh;
}

// The picture's aspect (width / height) when the game renders an exact size (docs/RESOLUTION_AUDIT.md): a windowed
// client keeps this shape - when created and while its edges are dragged - so the picture fills it without bars.
// 0: any shape (the render size follows the window).
double g_content_aspect = 0.0;

// The frame around a client area of this window's style.
SIZE frame_size(HWND window) noexcept {
    RECT frame{0, 0, 0, 0};
    AdjustWindowRect(&frame, static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE)) & ~WS_VISIBLE, FALSE);
    return SIZE{frame.right - frame.left, frame.bottom - frame.top};
}

// A windowed client of the asked size. An exact render size: the client takes the picture's shape - the largest of
// that aspect inside the asked window and inside the work area (less the frame) - so it shows the picture without
// bars.
SIZE windowed_client_size(int width, int height, DWORD style, const RECT& work) noexcept {
    SIZE client{width, height};
    if (g_content_aspect <= 0.0) return client;
    RECT frame{0, 0, 0, 0};
    AdjustWindowRect(&frame, style & ~WS_VISIBLE, FALSE);
    const double max_w = std::min<double>(width, (work.right - work.left) - (frame.right - frame.left));
    const double max_h = std::min<double>(height, (work.bottom - work.top) - (frame.bottom - frame.top));
    if (max_w >= 1.0 && max_h >= 1.0) {
        const double fit = std::min(max_w / g_content_aspect, max_h);  // the client height that fits both
        client.cy = std::max(1, static_cast<int>(std::floor(fit)));
        client.cx = std::max(1, static_cast<int>(std::lround(fit * g_content_aspect)));
    }
    return client;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_SIZING:
            if (g_content_aspect > 0.0) {
                RECT* r = reinterpret_cast<RECT*>(lparam);
                const SIZE frame = frame_size(window);
                const LONG client_w = (r->right - r->left) - frame.cx;
                const LONG client_h = (r->bottom - r->top) - frame.cy;
                if (wparam == WMSZ_TOP || wparam == WMSZ_BOTTOM) {
                    // Dragging the height: the width follows it.
                    r->right = r->left + frame.cx + std::max<LONG>(1, std::lround(client_h * g_content_aspect));
                } else {
                    // The width (a side or a corner): the height follows it, from the dragged edge's side.
                    const LONG h = frame.cy + std::max<LONG>(1, std::lround(client_w / g_content_aspect));
                    if (wparam == WMSZ_TOPLEFT || wparam == WMSZ_TOPRIGHT) r->top = r->bottom - h;
                    else r->bottom = r->top + h;
                }
                return TRUE;
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const unsigned key = static_cast<unsigned>(wparam) & 0xFFu;
            if ((lparam & (1 << 30)) == 0 && !g_key_held[key]) ++g_key_fresh[key];
            g_key_held[key] = true;
            break;  // WM_SYSKEYDOWN still reaches DefWindowProc (Alt+F4, the system menu)
        }
        case WM_KEYUP:
        case WM_SYSKEYUP:
            g_key_held[static_cast<unsigned>(wparam) & 0xFFu] = false;
            break;
        case WM_LBUTTONDOWN:
            ++g_clicks;
            g_click_x = static_cast<short>(LOWORD(lparam));
            g_click_y = static_cast<short>(HIWORD(lparam));
            break;
        case WM_KILLFOCUS:
            release_all_keys();
            break;
        case WM_ACTIVATEAPP:
            if (wparam == FALSE) release_all_keys();
            break;
        case WM_CLOSE:
            g_close_requested = true;
            return 0;
        case WM_DESTROY:
            g_close_requested = true;
            PostQuitMessage(0);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

#endif  // _WIN32

}  // namespace

void decode_xfb(const u8* xfb, u32 width, u32 height, u32 stride, std::vector<u32>& out) {
    out.assign(static_cast<std::size_t>(width) * height, 0xff000000u);
    if (xfb == nullptr || width < 2) {
        return;
    }
    for (u32 y = 0; y < height; ++y) {
        const u8* row = xfb + static_cast<std::size_t>(y) * stride;
        for (u32 x = 0; x + 1 < width; x += 2) {
            const int y0 = row[x * 2 + 0];
            const int cb = row[x * 2 + 1] - 128;
            const int y1 = row[x * 2 + 2];
            const int cr = row[x * 2 + 3] - 128;
            const auto convert = [&](int luma) {
                const int r = luma + ((359 * cr) >> 8);
                const int g = luma - ((88 * cb + 183 * cr) >> 8);
                const int b = luma + ((454 * cb) >> 8);
                return 0xff000000u | (static_cast<u32>(std::clamp(r, 0, 255)) << 16) |
                       (static_cast<u32>(std::clamp(g, 0, 255)) << 8) |
                       static_cast<u32>(std::clamp(b, 0, 255));
            };
            out[static_cast<std::size_t>(y) * width + x] = convert(y0);
            out[static_cast<std::size_t>(y) * width + x + 1] = convert(y1);
        }
    }
}

HostWindow::HostWindow() = default;

bool HostWindow::plume_active() const noexcept {
    return presenter_ != nullptr && presenter_->available();
}

std::string HostWindow::plume_error() const {
    if (!plume_error_.empty()) return plume_error_;
    return presenter_ != nullptr ? presenter_->last_error() : std::string{};
}

bool HostWindow::plume_failed() const noexcept {
    return plume_failed_ || (presenter_ != nullptr && presenter_->fence_failed());
}

void HostWindow::remember_plume_failure() noexcept {
    if (presenter_ == nullptr || !presenter_->fence_failed()) return;
    native::publishFailureBeforeDiagnostics([&] { plume_failed_ = true; }, [&] {
        plume_error_ = presenter_->last_error();
    });
}

PlumePresenter::VerifyReport HostWindow::plume_verify_report() const {
    return presenter_ != nullptr ? presenter_->verify_report() : stopped_plume_verify_report_;
}

PlumePresenter::PresentStats HostWindow::plume_present_stats() const {
    return presenter_ != nullptr ? presenter_->present_stats() : stopped_plume_present_stats_;
}

void HostWindow::set_plume_narrow_queue_lock(bool enabled) noexcept {
    narrow_present_lock_ = enabled;
    if (presenter_ != nullptr) {
        presenter_->set_narrow_queue_lock(enabled);
    }
}

void HostWindow::set_plume_deferred_pin_release(bool enabled) noexcept {
    deferred_pin_release_ = enabled;
    if (presenter_ != nullptr) {
        presenter_->set_deferred_pin_release(enabled);
    }
}

void HostWindow::set_plume_native_release_hook(PlumePresenter::NativeReleaseHook hook,
                                               void* context) noexcept {
    native_release_hook_ = hook;
    native_release_context_ = context;
    if (presenter_ != nullptr) {
        presenter_->set_native_release_hook(hook, context);
    }
}

bool HostWindow::plume_pins_adopted() const noexcept {
    return presenter_ != nullptr && presenter_->deferred_pin_release_active();
}

void HostWindow::plume_retire_completed_presents() {
    if (presenter_ != nullptr) {
        presenter_->retire_completed_presents();
        remember_plume_failure();
    }
}

HostWindow::~HostWindow() { destroy(); }

#ifdef _WIN32

void HostWindow::set_content_aspect(double aspect) noexcept {
#ifdef _WIN32
    g_content_aspect = aspect > 0.0 ? aspect : 0.0;
#else
    (void)aspect;
#endif
}

bool HostWindow::create(int width, int height, const std::string& title, bool fullscreen) {
    if (plume_failed()) return false; // terminal owner, no implicit device restart
    // One event for the whole of host startup's window half: the class
    // registration, CreateWindowExW, and - the large part - PlumePresenter's
    // create, which builds or adopts the D3D12 device, the swap chain and the
    // blit pipeline with its embedded shaders. shadow_main reports startup as a
    // single number; this breaks out its biggest component.
    GEKKO_PROFILE_ZONE("host/window-create");
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    // The executable's icon (resource 1, linked from installer/art/app.ico when the art pack is present;
    // CMakeLists.txt). Without it both stay null and Windows draws its default icon.
    window_class.hIcon = static_cast<HICON>(LoadImageW(window_class.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    window_class.hIconSm = static_cast<HICON>(LoadImageW(window_class.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    window_class.lpszClassName = kWindowClass;
    RegisterClassExW(&window_class);

    fullscreen_ = fullscreen;
    DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    int pos_x = CW_USEDEFAULT;
    int pos_y = CW_USEDEFAULT;
    int create_width = width;
    int create_height = height;
    if (fullscreen) {
        style = WS_POPUP | WS_VISIBLE;
        const int monitor_w = GetSystemMetrics(SM_CXSCREEN);
        const int monitor_h = GetSystemMetrics(SM_CYSCREEN);
        create_width = monitor_w;
        create_height = monitor_h;
        pos_x = 0;
        pos_y = 0;
    } else if (g_content_aspect > 0.0) {
        RECT work{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const SIZE client = windowed_client_size(width, height, style, work);
        create_width = client.cx;
        create_height = client.cy;
    }
    RECT bounds{0, 0, create_width, create_height};
    AdjustWindowRect(&bounds, style & ~WS_VISIBLE, FALSE);

    const std::wstring wide_title(title.begin(), title.end());
    HWND window = CreateWindowExW(0, kWindowClass, wide_title.c_str(), style, pos_x, pos_y,
                                  bounds.right - bounds.left, bounds.bottom - bounds.top, nullptr,
                                  nullptr, window_class.hInstance, nullptr);
    if (window == nullptr) {
        return false;
    }
    // Boot validation has to be watched on screen. Restore and raise even when
    // this process was started from a background agent that does not hold the
    // Windows foreground lock.
    ShowWindow(window, SW_SHOWNORMAL);
    ShowWindow(window, SW_RESTORE);
    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(window);
    SetForegroundWindow(window);
    UpdateWindow(window);
    handle_ = window;
    if (want_plume_) {
        presenter_ = std::make_unique<PlumePresenter>();
        // Both of these have to be set BEFORE create(): the presenter decides
        // during creation whether to build the GPU blit pipeline, and verify
        // selects which present path that blit is allowed to replace. Setting
        // them afterwards left blit_requested false at the only point that
        // reads it, so --present-gpu-blit silently did nothing.
        presenter_->set_verify(verify_plume_);
        presenter_->set_gpu_blit(gpu_blit_plume_);
        presenter_->set_area_resolve(area_resolve_plume_);
        presenter_->set_vsync(vsync_);
        // These two do not have to precede create(), but keeping them with the
        // rest means a switch set on the window is always on the presenter.
        presenter_->set_narrow_queue_lock(narrow_present_lock_);
        presenter_->set_deferred_pin_release(deferred_pin_release_);
        if (native_release_hook_ != nullptr) {
            presenter_->set_native_release_hook(native_release_hook_, native_release_context_);
        }
        if (!present_dump_.empty()) {
            presenter_->set_present_dump(present_dump_.c_str(), present_dump_frame_);
        }
        const bool created = plume_device_ != nullptr && plume_queue_ != nullptr
                                 ? presenter_->create(handle_, plume_device_, plume_queue_,
                                                      plume_queue_mutex_)
                                 : presenter_->create(handle_);
        if (!created) {
            presenter_.reset();  // build has no plume, or no D3D12 device: use GDI
        }
    }
    g_close_requested = false;
    return true;
}

bool HostWindow::set_mode(bool fullscreen, int width, int height) {
    if (handle_ == nullptr) return false;
    HWND window = static_cast<HWND>(handle_);
    // The monitor the window is on now: a borderless window covers it, a windowed one is centred in its work area.
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
    const DWORD visible = static_cast<DWORD>(GetWindowLongW(window, GWL_STYLE)) & WS_VISIBLE;
    int x = 0, y = 0, w = 0, h = 0;
    DWORD style = 0;
    if (fullscreen) {
        style = WS_POPUP | visible;
        x = monitor.rcMonitor.left;
        y = monitor.rcMonitor.top;
        w = monitor.rcMonitor.right - monitor.rcMonitor.left;
        h = monitor.rcMonitor.bottom - monitor.rcMonitor.top;
    } else {
        style = WS_OVERLAPPEDWINDOW | visible;
        const SIZE client = windowed_client_size(width, height, style, monitor.rcWork);
        RECT bounds{0, 0, client.cx, client.cy};
        AdjustWindowRect(&bounds, style & ~WS_VISIBLE, FALSE);
        w = bounds.right - bounds.left;
        h = bounds.bottom - bounds.top;
        const RECT& work = monitor.rcWork;
        x = work.left + std::max(0, static_cast<int>((work.right - work.left - w) / 2));
        y = work.top + std::max(0, static_cast<int>((work.bottom - work.top - h) / 2));
    }
    SetWindowLongW(window, GWL_STYLE, static_cast<LONG>(style));
    // SWP_FRAMECHANGED: the new style's frame is applied with the new rectangle; the client-size change reaches the
    // swap chain through the caller's resize, as a dragged edge does.
    SetWindowPos(window, HWND_TOP, x, y, w, h, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    fullscreen_ = fullscreen;
    return true;
}

void HostWindow::destroy() {
    // Retire the presentation owner's native leases while its source renderer
    // and HWND still exist. Preserve terminal shutdown errors for the process.
    if (presenter_ != nullptr) {
        presenter_->destroy();
        remember_plume_failure();
        stopped_plume_verify_report_ = presenter_->verify_report();
        stopped_plume_present_stats_ = presenter_->present_stats();
        presenter_.reset();
    }
    if (handle_ != nullptr) {
        DestroyWindow(static_cast<HWND>(handle_));
        handle_ = nullptr;
    }
}

void HostWindow::client_size(int& width, int& height) const noexcept {
    width = 0;
    height = 0;
    if (handle_ == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(static_cast<HWND>(handle_), &client);
    width = client.right - client.left;
    height = client.bottom - client.top;
}

bool HostWindow::pump() {
    // The best stall detector in the host: a title-bar drag or a resize parks
    // this thread inside the Win32 modal move/size loop, and that shows here as
    // one multi-second bar rather than an unexplained gap. Also covers the
    // keep-foreground restore path below (D06-F8).
    //
    // Rate: once per UI-loop iteration, which is display-bound whenever a
    // present happens in the same iteration. The one fast-spin corner is
    // --render-rate 0 while the presenter is refusing (minimised, or an empty
    // swap chain), where nothing blocks on vsync; there it can reach the low
    // tens of thousands per second, still under the event ceiling.
    GEKKO_PROFILE_ZONE("host/window-pump");
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    // D06-F8 (2026-09-02): opt-in. This ran on every pump - ~120 times a
    // second while the window was iconic or hidden - so a minimised or
    // backgrounded session fought the user for the foreground for its whole
    // length. It exists for unattended boot validation, which now asks for it
    // explicitly. Host window state only; the guest has no way to observe the
    // window, and neither the pad sampling nor the presenter behaves
    // differently for a window that stays minimised (present_native returns
    // early on an empty swap chain and counts it).
    if (keep_foreground_ && handle_ != nullptr) {
        HWND window = static_cast<HWND>(handle_);
        if (IsIconic(window) || IsWindowVisible(window) == FALSE) {
            ShowWindow(window, SW_RESTORE);
            SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            BringWindowToTop(window);
            SetForegroundWindow(window);
            ++foreground_restores_;
        }
    }
    const bool keyboard_active =
        handle_ != nullptr && GetForegroundWindow() == static_cast<HWND>(handle_);
    escape_pressed_ = keyboard_active && g_key_held[VK_ESCAPE];
    // Savestate / mark / snapshot hotkeys: one request per fresh key press (ES-W3: the window's own WM_KEYDOWN
    // messages, see window_proc). Key messages reach only the focused window, so typing N or M in another
    // application can never overwrite or reload the slot, and a key already held when focus returns arrives as an
    // autorepeat (previous-state bit set) that never counts - the guarantees the polled rising edge gave.
    const bool save_fresh = take_fresh('N');
    const bool load_fresh = take_fresh('M');
    const bool mark_fresh = marker_key_ != 0 && take_fresh(marker_key_);
    const bool range_fresh = range_key_ != 0 && take_fresh(range_key_);
    const bool snapshot_fresh = snapshot_key_ != 0 && take_fresh(snapshot_key_);
    if (keyboard_active && save_fresh) save_state_requested_ = true;
    if (keyboard_active && load_fresh) load_state_requested_ = true;
    if (keyboard_active && mark_fresh) {
        ++user_marks_requested_;
        // Where the mouse points as the key goes down, when it is over the picture.
        POINT cursor{};
        RECT client{};
        mark_point_valid_ = false;
        if (GetCursorPos(&cursor) && ScreenToClient(static_cast<HWND>(handle_), &cursor) &&
            GetClientRect(static_cast<HWND>(handle_), &client) && cursor.x >= 0 && cursor.y >= 0 &&
            cursor.x < client.right && cursor.y < client.bottom) {
            mark_point_valid_ = true;
            mark_x_ = (cursor.x + 0.5) / client.right;
            mark_y_ = (cursor.y + 0.5) / client.bottom;
        }
    }
    const unsigned clicks = g_clicks;
    g_clicks = 0;
    if (click_marks_ && clicks != 0u) {
        RECT client{};
        if (GetClientRect(static_cast<HWND>(handle_), &client) && client.right > 0 && client.bottom > 0) {
            user_marks_requested_ += clicks;
            mark_point_valid_ = true;
            mark_x_ = (g_click_x + 0.5) / client.right;
            mark_y_ = (g_click_y + 0.5) / client.bottom;
        }
    }
    if (keyboard_active && range_fresh) ++range_marks_requested_;
    if (keyboard_active && snapshot_fresh) ++snapshot_requests_;
    {
        constexpr struct {
            int key;
            unsigned bit;
        } kEffectKeys[] = {
            // The numeric keypad (Num Lock on; with it off the keypad sends arrows the game reads) ...
            {VK_DIVIDE, kEffectKeyMenu}, {VK_NUMPAD8, kEffectKeyUp}, {VK_NUMPAD2, kEffectKeyDown},
            {VK_NUMPAD5, kEffectKeyToggle}, {VK_NUMPAD0, kEffectKeySolo}, {VK_DECIMAL, kEffectKeyIsolate},
            {VK_SUBTRACT, kEffectKeyAllOn}, {VK_MULTIPLY, kEffectKeyFps},
            // ... and the same functions on F6 and the navigation block.
            {VK_F6, kEffectKeyMenu}, {VK_PRIOR, kEffectKeyUp}, {VK_NEXT, kEffectKeyDown},
            {VK_INSERT, kEffectKeyToggle}, {VK_HOME, kEffectKeySolo}, {VK_END, kEffectKeyIsolate},
            {VK_DELETE, kEffectKeyAllOn}};
        for (const auto& k : kEffectKeys)
            if (take_fresh(k.key) && keyboard_active) effect_keys_ |= k.bit;
    }
    save_state_key_down_ = g_key_held['N'];
    load_state_key_down_ = g_key_held['M'];
    user_mark_key_down_ = marker_key_ != 0 && g_key_held[marker_key_ & 0xFF];
    snapshot_key_down_ = snapshot_key_ != 0 && g_key_held[snapshot_key_ & 0xFF];
    return !g_close_requested;
}

bool HostWindow::present_native(void* texture, void* previous_texture, float blend_alpha,
                                int width, int height, float content_aspect,
                                const PlumePresenter::NativePin* source_pin,
                                const PlumePresenter::NativePin* previous_pin) {
    audio_audit::Scope audit_present("present", "present", audio_audit::Lane::Presenter);
    // The short circuit is deliberate and matches plume_pins_adopted(): when
    // there is no available presenter it is false, so the caller still owns
    // the pins and releases them itself.
    const bool presented = presenter_ != nullptr && presenter_->available() &&
        presenter_->present_native(texture, previous_texture, blend_alpha, width, height,
                                   content_aspect, source_pin, previous_pin);
    if (presenter_ != nullptr) {
        remember_plume_failure();
    }
    return presented;
}

void HostWindow::present(const u32* pixels, int width, int height, float content_aspect) {
    if (handle_ == nullptr || pixels == nullptr || width <= 0 || height <= 0) {
        return;
    }
    // Same pixels either way; only the path to the screen differs. A plume
    // failure falls back to GDI permanently rather than dropping frames.
    if (presenter_ != nullptr) {
        if (presenter_->fence_failed()) { remember_plume_failure(); return; }
        if (presenter_->available()) {
            presenter_->present(pixels, width, height, content_aspect);
            remember_plume_failure();
            return;
        }
        presenter_.reset();
    }
    HWND window = static_cast<HWND>(handle_);
    RECT client{};
    GetClientRect(window, &client);
    const int target_width = client.right - client.left;
    const int target_height = client.bottom - client.top;
    if (target_width <= 0 || target_height <= 0) {
        return;
    }
    // Opened below the presenter_->available() return above, so it can never
    // double-count a plume present, and below the zero-area guard so a
    // minimised window does not emit a zero-length zone. The presenter is reset
    // PERMANENTLY on failure, so once this zone appears at all, every frame for
    // the rest of the run goes through FillRect + StretchDIBits on the UI
    // thread - which nothing else on the timeline would say.
    GEKKO_PROFILE_ZONE("host/present-gdi");
    const float aspect =
        content_aspect > 0.1f ? content_aspect
                              : static_cast<float>(width) / static_cast<float>(height);
    const ContentRect dest = fit_content_rect(target_width, target_height, aspect);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC device = GetDC(window);
    if (dest.x != 0 || dest.y != 0 || dest.width != target_width || dest.height != target_height) {
        RECT client_fill{0, 0, target_width, target_height};
        FillRect(device, &client_fill, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    }
    SetStretchBltMode(device, COLORONCOLOR);
    StretchDIBits(device, dest.x, dest.y, dest.width, dest.height, 0, 0, width, height, pixels,
                  &info, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(window, device);
}

shadow::peripheral::ControllerSample HostWindow::controller() const {
    // Same population as host/window-pump: once per UI-loop iteration,
    // unconditionally. The keyboard is a table lookup (ES-W3, window_proc); the XInput poll
    // below, and polling XInput for an absent controller is a known
    // multi-hundred-microsecond call - this is the site that decides whether
    // the refusal-spin corner described on pump() is cheap or expensive.
    GEKKO_PROFILE_ZONE("host/pad-sample");
    // Key messages reach only the focused window. Still ignore keyboard input while
    // another application owns focus so ordinary typing cannot steer the game
    // or contaminate an unattended benchmark. XInput remains available below.
    bool keyboard_active = handle_ != nullptr && GetForegroundWindow() == static_cast<HWND>(handle_);
    const shadow::peripheral::ControllerSample keyboard = shadow::peripheral::sample_from_keyboard(
        [](int key, void* context) {
            // ES-W3: the level from the window's key messages (pump() ran first in this UI iteration).
            return *static_cast<const bool*>(context) && g_key_held[key & 0xFF];
        },
        &keyboard_active);
    const shadow::peripheral::ControllerSample gamepad =
        xinput_enabled_ ? poll_xinput_controller(0) : shadow::peripheral::ControllerSample{};
    return shadow::peripheral::combine(keyboard, gamepad);
}

#else

bool HostWindow::create(int, int, const std::string&, bool) { return false; }
bool HostWindow::set_mode(bool, int, int) { return false; }
void HostWindow::destroy() {}
void HostWindow::client_size(int& width, int& height) const noexcept {
    width = 0;
    height = 0;
}
bool HostWindow::pump() { return false; }
void HostWindow::present(const u32*, int, int, float) {}
bool HostWindow::present_native(void*, void*, float, int, int, float,
                                const PlumePresenter::NativePin*,
                                const PlumePresenter::NativePin*) {
    return false;
}
shadow::peripheral::ControllerSample HostWindow::controller() const { return {}; }

#endif  // _WIN32

}  // namespace gekko
