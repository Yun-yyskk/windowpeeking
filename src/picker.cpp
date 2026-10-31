#include "windowpeeking.h"
#include <windowsx.h>

namespace wp {
namespace {

const wchar_t* kClassName = L"WindowPeeking.Picker";
bool g_registered = false;

void DebugLog(const wchar_t* text) {
    wchar_t path[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"WINDOWPEEKING_DEBUG", path, MAX_PATH) == 0) {
        return;
    }
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return;
    }
    char utf8[1024]{};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), nullptr, nullptr);
    DWORD written = 0;
    if (n > 1) {
        WriteFile(f, utf8, static_cast<DWORD>(n - 1), &written, nullptr);
    }
    CloseHandle(f);
}

struct PickerState {
    HWND hwnd = nullptr;
    RECT vs{};
    bool dragging = false;
    POINT start{};
    POINT cur{};
    bool done = false;
    bool cancelled = false;
    HWND source = nullptr;
    RECT cropWindowSpace{};
    RECT screenRect{};
    HBITMAP dib = nullptr;
    HDC mem = nullptr;
    HGDIOBJ oldBmp = nullptr;
    void* bits = nullptr;
    int w = 0;
    int h = 0;
};

RECT RectOf(POINT a, POINT b) {
    RECT r{ a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y,
            a.x < b.x ? b.x : a.x, a.y < b.y ? b.y : a.y };
    return r;
}

bool MakeSurface(PickerState& st) {
    HDC screen = GetDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = st.w;
    bi.bmiHeader.biHeight = -st.h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    st.dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &st.bits, nullptr, 0);
    st.mem = CreateCompatibleDC(screen);
    if (st.dib != nullptr && st.mem != nullptr) {
        st.oldBmp = SelectObject(st.mem, st.dib);
    }
    ReleaseDC(nullptr, screen);
    return st.dib != nullptr && st.mem != nullptr && st.bits != nullptr;
}

void PutPx(PickerState& st, int x, int y) {
    if (x < 0 || y < 0 || x >= st.w || y >= st.h) {
        return;
    }
    static_cast<DWORD*>(st.bits)[static_cast<size_t>(y) * st.w + x] = 0xFFFFFFFF;
}

// Dim everything, punch a fully transparent hole where the selection is, draw a
// hairline border and a hint line. One 32-bit DIB handed to UpdateLayeredWindow.
void Repaint(PickerState& st) {
    auto* px = static_cast<DWORD*>(st.bits);
    const DWORD dim = 0x66000000; // A=0x66, black
    const size_t total = static_cast<size_t>(st.w) * st.h;
    for (size_t i = 0; i < total; ++i) {
        px[i] = dim;
    }

    if (st.dragging) {
        const RECT sel = RectOf(st.start, st.cur);
        for (int y = sel.top; y < sel.bottom; ++y) {
            if (y < 0 || y >= st.h) {
                continue;
            }
            DWORD* row = px + static_cast<size_t>(y) * st.w;
            for (int x = sel.left; x < sel.right; ++x) {
                if (x >= 0 && x < st.w) {
                    row[x] = 0;
                }
            }
        }
        for (int x = sel.left; x < sel.right; ++x) {
            PutPx(st, x, sel.top);
            PutPx(st, x, sel.bottom - 1);
        }
        for (int y = sel.top; y < sel.bottom; ++y) {
            PutPx(st, sel.left, y);
            PutPx(st, sel.right - 1, y);
        }
    }

    // GDI writes no alpha channel, so text is given one afterwards.
    SetBkMode(st.mem, TRANSPARENT);
    SetTextColor(st.mem, RGB(255, 255, 255));
    HGDIOBJ oldFont = SelectObject(st.mem, GetStockObject(DEFAULT_GUI_FONT));
    RECT hint{ 0, st.h - 64, st.w, st.h - 34 };
    DrawTextW(st.mem,
              L"拖动框选要一直盯住的区域        Esc 取消",
              -1, &hint, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(st.mem, oldFont);

    for (size_t i = 0; i < total; ++i) {
        if ((px[i] & 0x00FFFFFFu) != 0) {
            px[i] |= 0xFF000000u;
        }
    }

    HDC screen = GetDC(nullptr);
    POINT dst{ st.vs.left, st.vs.top };
    SIZE size{ st.w, st.h };
    POINT src{ 0, 0 };
    BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(st.hwnd, screen, &dst, &size, st.mem, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

void Finalize(PickerState& st) {
    {
        wchar_t line[192]{};
        swprintf_s(line, L"finalize start=(%ld,%ld) cur=(%ld,%ld)\r\n", st.start.x, st.start.y,
                   st.cur.x, st.cur.y);
        DebugLog(line);
    }
    st.dragging = false;
    ReleaseCapture();

    const RECT sel = RectOf(st.start, st.cur);
    if (sel.right - sel.left < 8 || sel.bottom - sel.top < 8) {
        st.cancelled = true;
        st.done = true;
        DestroyWindow(st.hwnd);
        return;
    }

    // Hide first: WindowFromPoint must not find the overlay itself.
    ShowWindow(st.hwnd, SW_HIDE);

    const RECT selScreen{ st.vs.left + sel.left, st.vs.top + sel.top,
                          st.vs.left + sel.right, st.vs.top + sel.bottom };

    POINT probe{ selScreen.left + 2, selScreen.top + 2 };
    HWND under = WindowFromPoint(probe);
    HWND root = under != nullptr ? GetAncestor(under, GA_ROOT) : nullptr;
    if (!IsUsableSource(root)) {
        POINT centre{ (selScreen.left + selScreen.right) / 2, (selScreen.top + selScreen.bottom) / 2 };
        under = WindowFromPoint(centre);
        root = under != nullptr ? GetAncestor(under, GA_ROOT) : nullptr;
    }

    {
        wchar_t cls[128]{};
        if (root != nullptr) {
            GetClassNameW(root, cls, 128);
        }
        wchar_t line[256]{};
        swprintf_s(line, L"probe under=%p root=%p class=%ls usable=%d\r\n",
                   reinterpret_cast<void*>(under), reinterpret_cast<void*>(root), cls,
                   IsUsableSource(root) ? 1 : 0);
        DebugLog(line);
    }
    if (!IsUsableSource(root)) {
        DebugLog(L"root unusable -> cancel\r\n");
        st.cancelled = true;
        st.done = true;
        DestroyWindow(st.hwnd);
        return;
    }

    RECT wal{};
    GetWindowRect(root, &wal);
    const Selection selection = ComputeSelection(st.vs, st.start, st.cur, wal);
    if (!selection.valid) {
        st.cancelled = true;
    } else {
        st.source = root;
        st.cropWindowSpace = selection.cropWindowSpace;
        st.screenRect = selection.screenRect;
    }
    st.done = true;
    DestroyWindow(st.hwnd);
}

LRESULT CALLBACK PickerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<PickerState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        st = static_cast<PickerState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return TRUE;
    }
    if (st == nullptr) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
    case WM_LBUTTONDOWN: {
        wchar_t line[128]{};
        swprintf_s(line, L"down=(%ld,%ld)\r\n", GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        DebugLog(line);
        st->dragging = true;
        st->start = POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        st->cur = st->start;
        SetCapture(hwnd);
        Repaint(*st);
        return 0;
    }

    case WM_MOUSEMOVE:
        if (st->dragging) {
            st->cur = POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            Repaint(*st);
        }
        return 0;

    case WM_LBUTTONUP:
        if (st->dragging) {
            st->cur = POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            Finalize(*st);
        }
        return 0;

    case WM_RBUTTONDOWN:
        st->cancelled = true;
        st->done = true;
        DestroyWindow(hwnd);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            st->cancelled = true;
            st->done = true;
        DestroyWindow(hwnd);
        }
        return 0;

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterPickerClass(HINSTANCE instance) {
    if (g_registered) {
        return true;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PickerWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    g_registered = true;
    return true;
}

} // namespace

bool PickRegion(HINSTANCE instance, HWND* outSource, RECT* outCrop, RECT* outScreenRect) {
    if (!RegisterPickerClass(instance)) {
        return false;
    }

    PickerState st{};
    st.vs = RECT{ GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0 };
    st.vs.right = st.vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    st.vs.bottom = st.vs.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    st.w = st.vs.right - st.vs.left;
    st.h = st.vs.bottom - st.vs.top;
    if (st.w <= 0 || st.h <= 0) {
        return false;
    }

    st.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
                              kClassName, L"WindowPeeking picker",
                              WS_POPUP | WS_VISIBLE,
                              st.vs.left, st.vs.top, st.w, st.h,
                              nullptr, nullptr, instance, &st);
    {
        wchar_t line[256]{};
        swprintf_s(line, L"create hwnd=%p vs=(%ld,%ld,%ld,%ld) size=%dx%d err=%lu\r\n",
                   reinterpret_cast<void*>(st.hwnd), st.vs.left, st.vs.top, st.vs.right,
                   st.vs.bottom, st.w, st.h, GetLastError());
        DebugLog(line);
    }
    if (st.hwnd == nullptr) {
        DebugLog(L"create failed\r\n");
        return false;
    }
    if (!MakeSurface(st)) {
        DebugLog(L"MakeSurface failed\r\n");
        st.done = true;
        DestroyWindow(st.hwnd);
        return false;
    }

    SetWindowPos(st.hwnd, HWND_TOPMOST, st.vs.left, st.vs.top, st.w, st.h, SWP_SHOWWINDOW);
    Repaint(st);
    SetForegroundWindow(st.hwnd);
    SetFocus(st.hwnd);

    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (st.oldBmp != nullptr) {
        SelectObject(st.mem, st.oldBmp);
    }
    if (st.mem != nullptr) {
        DeleteDC(st.mem);
    }
    if (st.dib != nullptr) {
        DeleteObject(st.dib);
    }

    if (st.cancelled || st.source == nullptr) {
        return false;
    }
    if (outSource != nullptr) *outSource = st.source;
    if (outCrop != nullptr) *outCrop = st.cropWindowSpace;
    if (outScreenRect != nullptr) *outScreenRect = st.screenRect;
    return true;
}

} // namespace wp
