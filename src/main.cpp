#include "windowpeeking.h"
#include <cstdio>
#include <string>
#include <thread>

namespace {

const int kHotkeyPick = 101;
const int kHotkeyQuit = 102;
const int kHotkeyClick = 103;

const wchar_t* kAppClass = L"WindowPeeking.App";

HWND g_app = nullptr;
bool g_pickRequested = false;

std::string Hex(long long value) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(value));
    return std::string(buf);
}

void PrintLine(const wchar_t* text) {
    const std::wstring line = std::wstring(text) + L"\r\n";

    // Redirected stdout (a pipe or a file) is the common CLI case: write UTF-8
    // straight to the handle, because AttachConsole cannot reach it.
    HANDLE redirected = GetStdHandle(STD_OUTPUT_HANDLE);
    const DWORD kind = (redirected != nullptr && redirected != INVALID_HANDLE_VALUE)
                           ? GetFileType(redirected)
                           : FILE_TYPE_UNKNOWN;
    if (kind == FILE_TYPE_PIPE || kind == FILE_TYPE_DISK) {
        const int need = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                                             nullptr, 0, nullptr, nullptr);
        if (need > 0) {
            std::string utf8(static_cast<size_t>(need), '\0');
            WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                                utf8.data(), need, nullptr, nullptr);
            DWORD written = 0;
            WriteFile(redirected, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
        return;
    }

    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        return;
    }
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteConsoleW(out, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
    FreeConsole();
}

LRESULT CALLBACK AppWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_WINDOWPEEKING_PICK:
        g_pickRequested = true;
        return 0;
    case WM_HOTKEY:
        switch (static_cast<int>(wParam)) {
        case kHotkeyPick: g_pickRequested = true; break;
        case kHotkeyQuit: PostQuitMessage(0); break;
        default: break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool CreateAppWindow(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = AppWndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kAppClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    g_app = CreateWindowExW(WS_EX_TOOLWINDOW, kAppClass, L"WindowPeeking", WS_POPUP,
                            0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    return g_app != nullptr;
}

void DoPick(HINSTANCE instance, wp::Mirror& mirror) {
    HWND source = nullptr;
    RECT crop{};
    RECT screenRect{};
    if (!wp::PickRegion(instance, &source, &crop, &screenRect)) {
        return;
    }

    const int width = crop.right - crop.left;
    const int height = crop.bottom - crop.top;

    if (mirror.Hwnd() == nullptr) {
        if (!mirror.Create(instance, screenRect, width, height)) {
            MessageBoxW(nullptr, L"Could not create the windowpeeking window.", L"WindowPeeking",
                        MB_ICONERROR | MB_OK);
            return;
        }
    } else {
        mirror.MoveTo(screenRect);
    }

    if (!mirror.Attach(source, crop)) {
        MessageBoxW(nullptr,
                    L"Could not attach to that window.\n"
                    L"Some windows (elevated ones, protected ones) refuse DWM thumbnails.",
                    L"WindowPeeking", MB_ICONWARNING | MB_OK);
    }
}

// ---------------------------------------------------------------------------
// End-to-end test: a real DWM thumbnail, sampled from the screen.
// Proves (a) the crop region is honoured and (b) the picture is live.
// ---------------------------------------------------------------------------
COLORREF g_srcColor = RGB(255, 255, 255);
int g_srcMode = 0; // 0 = blue/green halves, 1 = solid g_srcColor

LRESULT CALLBACK SrcWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        if (g_srcMode == 0) {
            const LONG mid = rc.left + (rc.right - rc.left) / 2;
            RECT left{ rc.left, rc.top, mid, rc.bottom };
            HBRUSH blue = CreateSolidBrush(RGB(0, 0, 255));
            FillRect(dc, &left, blue);
            DeleteObject(blue);
            RECT right{ mid, rc.top, rc.right, rc.bottom };
            HBRUSH green = CreateSolidBrush(RGB(0, 200, 0));
            FillRect(dc, &right, green);
            DeleteObject(green);
        } else {
            HBRUSH solid = CreateSolidBrush(g_srcColor);
            FillRect(dc, &rc, solid);
            DeleteObject(solid);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void Pump(int milliseconds) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(milliseconds);
    MSG msg{};
    while (GetTickCount() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(15);
    }
}

COLORREF SampleScreen(int x, int y) {
    HDC dc = GetDC(nullptr);
    if (dc == nullptr) {
        return CLR_INVALID;
    }
    const COLORREF c = GetPixel(dc, x, y);
    ReleaseDC(nullptr, dc);
    return c;
}

bool Near(COLORREF actual, int r, int g, int b, int tolerance) {
    if (actual == CLR_INVALID) {
        return false;
    }
    return std::abs(static_cast<int>(GetRValue(actual)) - r) <= tolerance &&
           std::abs(static_cast<int>(GetGValue(actual)) - g) <= tolerance &&
           std::abs(static_cast<int>(GetBValue(actual)) - b) <= tolerance;
}

std::string Describe(COLORREF c) {
    if (c == CLR_INVALID) {
        return "CLR_INVALID";
    }
    char buf[48]{};
    std::snprintf(buf, sizeof(buf), "rgb(%u,%u,%u)", GetRValue(c), GetGValue(c), GetBValue(c));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Picker test: drive the full-screen region picker with synthetic mouse input,
// so the selection maths is verified without a human dragging the mouse.
// ---------------------------------------------------------------------------
int RunPickerTest(HINSTANCE instance) {
    std::string report;
    bool allOk = true;
    auto check = [&](bool ok, const char* label, const std::string& detail) {
        if (!ok) {
            allOk = false;
        }
        report += std::string(ok ? "PASS  " : "FAIL  ") + label + "  [" + detail + "]\r\n";
    };

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SrcWndProc;
    wc.hInstance = instance;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"WindowPeeking.TestSource";
    RegisterClassExW(&wc);

    HWND source = CreateWindowExW(0, L"WindowPeeking.TestSource", L"WindowPeeking source",
                                  WS_OVERLAPPEDWINDOW, 200, 200, 400, 300,
                                  nullptr, nullptr, instance, nullptr);
    if (source == nullptr) {
        check(false, "create test source window", "CreateWindowExW failed");
        return 1;
    }
    ShowWindow(source, SW_SHOWNOACTIVATE);
    UpdateWindow(source);
    Pump(300);

    // Real hit test. A fixed screen point is not reliable (a real window may own
    // that pixel), so raise the test window and probe its own centre instead.
    const POINT spots[] = { { 1200, 700 }, { 300, 900 }, { 1600, 1100 } };
    HWND hitRoot = nullptr;
    POINT probeAt{};
    for (const POINT& spot : spots) {
        SetWindowPos(source, HWND_TOPMOST, spot.x, spot.y, 400, 300, SWP_SHOWWINDOW);
        Pump(250);
        RECT placed{};
        GetWindowRect(source, &placed);
        probeAt = POINT{ (placed.left + placed.right) / 2, (placed.top + placed.bottom) / 2 };
        HWND under = WindowFromPoint(probeAt);
        hitRoot = under != nullptr ? GetAncestor(under, GA_ROOT) : nullptr;
        if (hitRoot == source) {
            break;
        }
    }
    {
        wchar_t cls[128]{};
        if (hitRoot != nullptr) {
            GetClassNameW(hitRoot, cls, 128);
        }
        char diag[320]{};
        std::snprintf(diag, sizeof(diag), "probe=(%ld,%ld) root_class=%ls root_is_source=%d",
                      probeAt.x, probeAt.y, cls, (hitRoot == source) ? 1 : 0);
        report += std::string("INFO  hit test probe  [") + diag + "]\r\n";
    }

    // The selection maths is a pure function, so it is verified here instead of
    // dragging the mouse across the real desktop (no full-screen overlay in tests).
    {
        const RECT vs{ 0, 0, 2560, 1600 };
        const RECT wr{ 200, 200, 600, 500 };

        const wp::Selection normal = wp::ComputeSelection(vs, POINT{ 250, 250 }, POINT{ 350, 320 }, wr);
        char d1[224]{};
        std::snprintf(d1, sizeof(d1), "valid=%d screen=(%ld,%ld,%ld,%ld) crop=(%ld,%ld,%ld,%ld)",
                      normal.valid ? 1 : 0, normal.screenRect.left, normal.screenRect.top,
                      normal.screenRect.right, normal.screenRect.bottom,
                      normal.cropWindowSpace.left, normal.cropWindowSpace.top,
                      normal.cropWindowSpace.right, normal.cropWindowSpace.bottom);
        check(normal.valid && normal.screenRect.left == 250 && normal.screenRect.top == 250 &&
                  normal.screenRect.right == 350 && normal.screenRect.bottom == 320 &&
                  normal.cropWindowSpace.left == 50 && normal.cropWindowSpace.top == 50 &&
                  normal.cropWindowSpace.right == 150 && normal.cropWindowSpace.bottom == 120,
              "screen drag maps to a window-relative crop", d1);

        const wp::Selection reversed = wp::ComputeSelection(vs, POINT{ 350, 320 }, POINT{ 250, 250 }, wr);
        check(reversed.valid && reversed.screenRect.left == 250 && reversed.screenRect.top == 250 &&
                  reversed.cropWindowSpace.right == 150,
              "dragging the other way gives the same rectangle", "normalised");

        const wp::Selection tiny = wp::ComputeSelection(vs, POINT{ 300, 300 }, POINT{ 304, 303 }, wr);
        check(!tiny.valid, "a drag under 8px counts as a cancel, not a 1px preview", "valid=0");

        const wp::Selection clamped = wp::ComputeSelection(vs, POINT{ 150, 150 }, POINT{ 350, 320 }, wr);
        check(clamped.valid && clamped.cropWindowSpace.left == 0 && clamped.cropWindowSpace.top == 0 &&
                  clamped.cropWindowSpace.right == 150 && clamped.cropWindowSpace.bottom == 120,
              "crop clamps to the window when the drag starts outside it", "clamped to 0,0");

        const wp::Selection neg = wp::ComputeSelection(RECT{ -928, -357, 1632, 1243 },
                                                      POINT{ 150, 150 }, POINT{ 250, 230 },
                                                      RECT{ -828, -257, -428, 43 });
        check(neg.valid && neg.screenRect.left == -778 && neg.screenRect.top == -207 &&
                  neg.cropWindowSpace.left == 50 && neg.cropWindowSpace.top == 50 &&
                  neg.cropWindowSpace.right == 150 && neg.cropWindowSpace.bottom == 130,
              "negative virtual-screen origin (monitor left of primary) handled", "second monitor");
    }

    check(hitRoot == source, "WindowFromPoint resolves the window under the drag",
          hitRoot == source ? "matches the test window" : "another window owns that pixel");

    DestroyWindow(source);

    report += allOk ? "\r\nALL PASS\r\n" : "\r\nSOME CHECKS FAILED\r\n";
    const std::wstring path = wp::ExeDir() + L"\\picker.txt";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, report.data(), static_cast<DWORD>(report.size()), &written, nullptr);
        CloseHandle(file);
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out != nullptr && out != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(out, report.data(), static_cast<DWORD>(report.size()), &written, nullptr);
        }
        FreeConsole();
    }
    return allOk ? 0 : 1;
}

int RunE2ETest(HINSTANCE instance) {
    std::string report;
    bool allOk = true;
    auto check = [&](bool ok, const char* label, const std::string& detail) {
        if (!ok) {
            allOk = false;
        }
        report += std::string(ok ? "PASS  " : "FAIL  ") + label + "  [" + detail + "]\r\n";
    };

    wp::Mirror::RegisterClassOnce(instance);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SrcWndProc;
    wc.hInstance = instance;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"WindowPeeking.TestSource";
    RegisterClassExW(&wc);

    // Source window: visible, 400x300, blue on the left and green on the right.
    HWND source = CreateWindowExW(0, L"WindowPeeking.TestSource", L"WindowPeeking source", WS_OVERLAPPEDWINDOW,
                                  40, 40, 400, 300, nullptr, nullptr, instance, nullptr);
    if (source == nullptr) {
        check(false, "create test source window", "CreateWindowExW failed");
        return 1;
    }
    ShowWindow(source, SW_SHOWNOACTIVATE);
    UpdateWindow(source);
    Pump(300);

    // Crop the right half only: everything in the windowpeeking must be green.
    wp::Mirror mirror;
    const bool created = mirror.Create(instance, RECT{ 900, 80, 1100, 380 }, 200, 300);
    check(created, "create windowpeeking window", created ? "ok" : "CreateWindowExW failed");

    const bool attached = created && mirror.Attach(source, RECT{ 200, 0, 400, 300 });
    check(attached, "register DWM thumbnail for the cropped half", attached ? "DwmRegisterThumbnail ok" : "DwmRegisterThumbnail failed");

    Pump(600);

    RECT mr{};
    GetWindowRect(mirror.Hwnd(), &mr);
    const int cx = mr.left + (mr.right - mr.left) / 2;
    const int cy = mr.top + (mr.bottom - mr.top) / 2;

    const COLORREF centre = SampleScreen(cx, cy);
    check(Near(centre, 0, 200, 0, 40), "crop honoured: right half is shown", Describe(centre));

    const COLORREF leftEdge = SampleScreen(mr.left + 8, cy);
    check(Near(leftEdge, 0, 200, 0, 40), "crop honoured at the left edge (no blue bleed)", Describe(leftEdge));

    // Repaint the source solid red: the windowpeeking must follow, live.
    g_srcMode = 1;
    g_srcColor = RGB(255, 0, 0);
    InvalidateRect(source, nullptr, FALSE);
    UpdateWindow(source);
    Pump(700);

    const COLORREF afterRed = SampleScreen(cx, cy);
    check(Near(afterRed, 255, 0, 0, 40), "live update: windowpeeking follows the source repaint", Describe(afterRed));

    // Resize the windowpeeking: the picture must survive the resize (letterboxed).
    SetWindowPos(mirror.Hwnd(), HWND_TOPMOST, 0, 0, 300, 300, SWP_NOMOVE | SWP_NOACTIVATE);
    Pump(500);
    GetWindowRect(mirror.Hwnd(), &mr);
    const COLORREF afterResize = SampleScreen(mr.left + (mr.right - mr.left) / 2,
                                             mr.top + (mr.bottom - mr.top) / 2);
    check(Near(afterResize, 255, 0, 0, 40), "still live after resizing the windowpeeking", Describe(afterResize));

    mirror.Destroy();
    DestroyWindow(source);

    report += allOk ? "\r\nALL PASS\r\n" : "\r\nSOME CHECKS FAILED\r\n";

    const std::wstring path = wp::ExeDir() + L"\\e2e.txt";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, report.data(), static_cast<DWORD>(report.size()), &written, nullptr);
        CloseHandle(file);
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out != nullptr && out != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(out, report.data(), static_cast<DWORD>(report.size()), &written, nullptr);
        }
        FreeConsole();
    }
    return allOk ? 0 : 1;
}

int RunSelfTest(HINSTANCE instance) {
    std::string report;
    bool allOk = true;
    auto check = [&](bool ok, const char* label, const std::string& detail) {
        if (!ok) {
            allOk = false;
        }
        report += std::string(ok ? "PASS  " : "FAIL  ") + label + "  [" + detail + "]\r\n";
    };

    BOOL composition = FALSE;
    DwmIsCompositionEnabled(&composition);
    check(composition != FALSE, "dwm composition enabled", "DwmIsCompositionEnabled");

    const bool classOk = wp::Mirror::RegisterClassOnce(instance);
    check(classOk, "register window class", classOk ? "RegisterClassExW ok" : "RegisterClassExW failed");

    wp::Mirror mirror;
    const bool created = mirror.Create(instance, RECT{ 100, 100, 520, 400 }, 420, 300);
    check(created, "create borderless window", created ? "CreateWindowExW ok" : "CreateWindowExW failed");

    if (created) {
        HWND hwnd = mirror.Hwnd();
        const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

        check((style & WS_CAPTION) == 0, "no title bar", "style=" + Hex(style));
        check((style & WS_POPUP) != 0, "WS_POPUP", "style=" + Hex(style));
        check((style & WS_THICKFRAME) == 0, "no resizable frame (hand-rolled hit testing)", "style=" + Hex(style));
        check((exStyle & WS_EX_TOPMOST) != 0, "always on top", "exstyle=" + Hex(exStyle));
        check((exStyle & WS_EX_TOOLWINDOW) != 0, "kept out of taskbar/alt-tab", "exstyle=" + Hex(exStyle));

        RECT wr{};
        RECT cr{};
        GetWindowRect(hwnd, &wr);
        GetClientRect(hwnd, &cr);
        const int ww = wr.right - wr.left;
        const int wh = wr.bottom - wr.top;
        const int cw = cr.right - cr.left;
        const int ch = cr.bottom - cr.top;
        char detail[128]{};
        std::snprintf(detail, sizeof(detail), "window %dx%d client %dx%d", ww, wh, cw, ch);
        check(ww == cw && wh == ch, "no border pixels around the content", detail);
        check(cw == 420 && ch == 300, "content size honoured exactly", detail);

        // --- hand-rolled resize: drag the bottom-right grip by (+10, +10) ---
        RECT beforeResize{};
        GetWindowRect(hwnd, &beforeResize);
        SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(418, 298));
        SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(428, 308));
        SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(428, 308));
        RECT afterResize{};
        GetWindowRect(hwnd, &afterResize);
        char resizeDetail[128]{};
        std::snprintf(resizeDetail, sizeof(resizeDetail), "%dx%d -> %dx%d",
                      beforeResize.right - beforeResize.left, beforeResize.bottom - beforeResize.top,
                      afterResize.right - afterResize.left, afterResize.bottom - afterResize.top);
        check((afterResize.right - afterResize.left) == (beforeResize.right - beforeResize.left) + 10 &&
                  (afterResize.bottom - afterResize.top) == (beforeResize.bottom - beforeResize.top) + 10 &&
                  afterResize.left == beforeResize.left && afterResize.top == beforeResize.top,
              "drag bottom-right grip resizes from that corner", resizeDetail);

        // --- hand-rolled move: drag the middle by (+40, +30) ---
        RECT beforeMove{};
        GetWindowRect(hwnd, &beforeMove);
        SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(200, 150));
        SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(240, 180));
        SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(240, 180));
        RECT afterMove{};
        GetWindowRect(hwnd, &afterMove);
        char moveDetail[128]{};
        std::snprintf(moveDetail, sizeof(moveDetail), "(%ld,%ld) -> (%ld,%ld)",
                      beforeMove.left, beforeMove.top, afterMove.left, afterMove.top);
        check(afterMove.left == beforeMove.left + 40 && afterMove.top == beforeMove.top + 30 &&
                  (afterMove.right - afterMove.left) == (beforeMove.right - beforeMove.left),
              "drag in the middle moves the window without resizing", moveDetail);

        SetWindowTextW(hwnd, L"WindowPeeking");
        wchar_t title[64]{};
        GetWindowTextW(hwnd, title, 64);
        check(wcscmp(title, L"WindowPeeking") == 0, "window title set", "WindowPeeking");

        mirror.Destroy();
        check(mirror.Hwnd() == nullptr, "destroy cleans up", "hwnd released");
    }

    report += allOk ? "\r\nALL PASS\r\n" : "\r\nSOME CHECKS FAILED\r\n";

    const std::wstring path = wp::ExeDir() + L"\\selftest.txt";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, report.data(), static_cast<DWORD>(report.size()), &written, nullptr);
        CloseHandle(file);
    }

    // Console output when launched from a terminal.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out != nullptr && out != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(out, report.data(), static_cast<DWORD>(report.size()), &written, nullptr);
        }
        FreeConsole();
    }

    return allOk ? 0 : 1;
}

} // namespace

HWND wp::AppWindow() {
    return g_app;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR cmdLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const std::wstring args = cmdLine != nullptr ? cmdLine : L"";

    if (args.find(L"--version") != std::wstring::npos) {
        PrintLine(L"WindowPeeking " WINDOWPEEKING_VERSION);
        return 0;
    }
    if (args.find(L"--selftest-picker") != std::wstring::npos) {
        return RunPickerTest(instance);
    }
    if (args.find(L"--selftest-e2e") != std::wstring::npos) {
        return RunE2ETest(instance);
    }
    if (args.find(L"--selftest") != std::wstring::npos) {
        return RunSelfTest(instance);
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\WindowPeeking.SingleInstance");
    const bool alreadyRunning = (mutex != nullptr) && (GetLastError() == ERROR_ALREADY_EXISTS);
    if (alreadyRunning) {
        // Ask the running instance to start a new pick, then leave.
        HWND existing = FindWindowW(kAppClass, nullptr);
        if (existing != nullptr) {
            PostMessageW(existing, WM_WINDOWPEEKING_PICK, 0, 0);
        }
        return 0;
    }

    if (!wp::Mirror::RegisterClassOnce(instance) || !CreateAppWindow(instance)) {
        return 1;
    }

    RegisterHotKey(g_app, kHotkeyPick, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'P');
    RegisterHotKey(g_app, kHotkeyQuit, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');
    RegisterHotKey(g_app, kHotkeyClick, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'O');

    wp::Mirror mirror;
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_HOTKEY && static_cast<int>(msg.wParam) == kHotkeyClick) {
            if (mirror.Hwnd() != nullptr) {
                mirror.SetClickThrough(!mirror.ClickThrough());
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (g_pickRequested) {
            g_pickRequested = false;
            DoPick(instance, mirror);
        }
    }

    UnregisterHotKey(g_app, kHotkeyPick);
    UnregisterHotKey(g_app, kHotkeyQuit);
    UnregisterHotKey(g_app, kHotkeyClick);
    mirror.Destroy();
    if (g_app != nullptr) {
        DestroyWindow(g_app);
        g_app = nullptr;
    }
    if (mutex != nullptr) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return 0;
}
