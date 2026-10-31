#include "windowpeeking.h"
#include <windowsx.h>

namespace wp {
namespace {

const wchar_t* kClassName = L"WindowPeeking.Mirror";
bool g_registered = false;

const int kGrip = 6;
const int kMinWidth = 64;
const int kMinHeight = 48;

enum MenuId : UINT {
    IDM_PICK = 1,
    IDM_OPACITY_FIRST,          // IDM_OPACITY_FIRST + i, i indexes kOpacitySteps
    IDM_CLICKTHROUGH = 100,
    IDM_CLOSE,
    IDM_CLOSEALL,
    IDM_QUIT,
};

// 100% .. 20% in 10% steps, top of the submenu first.
const BYTE kOpacitySteps[] = { 255, 230, 204, 179, 153, 128, 102, 77, 51 };
const int kOpacityStepCount = static_cast<int>(sizeof(kOpacitySteps) / sizeof(kOpacitySteps[0]));

} // namespace

bool Mirror::RegisterClassOnce(HINSTANCE instance) {
    if (g_registered) {
        return true;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProcStatic;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    g_registered = true;
    return true;
}

bool Mirror::Create(HINSTANCE instance, RECT const& screenRect, int width, int height) {
    m_instance = instance;
    const DWORD style = WS_POPUP | WS_CLIPCHILDREN;
    // LAYERED is always on so opacity can change at any time without flicker.
    // NOACTIVATE is what makes keyboard pass through: this window never takes
    // focus, so every keystroke keeps going to the app you are really using.
    const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE;
    m_hwnd = CreateWindowExW(exStyle, kClassName, L"WindowPeeking", style,
                             screenRect.left, screenRect.top, width, height,
                             nullptr, nullptr, instance, this);
    if (m_hwnd == nullptr) {
        return false;
    }
    SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);
    SetTimer(m_hwnd, 1, 500, nullptr);
    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    return true;
}

bool Mirror::Attach(HWND source, RECT const& cropInWindowSpace) {
    if (m_hwnd == nullptr || source == nullptr || !IsWindow(source)) {
        return false;
    }
    Disconnect();

    m_source = source;
    m_crop = cropInWindowSpace;
    m_sourceGone = false;

    const int width = m_crop.right - m_crop.left;
    const int height = m_crop.bottom - m_crop.top;
    if (width <= 0 || height <= 0) {
        return false;
    }

    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, width, height,
                 SWP_NOMOVE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    if (FAILED(DwmRegisterThumbnail(m_hwnd, source, &m_thumb))) {
        m_thumb = nullptr;
        return false;
    }
    UpdateThumb();
    return true;
}

void Mirror::MoveTo(RECT const& screenRect) {
    if (m_hwnd == nullptr) {
        return;
    }
    SetWindowPos(m_hwnd, HWND_TOPMOST, screenRect.left, screenRect.top, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void Mirror::UpdateThumb() {
    if (m_thumb == nullptr || m_hwnd == nullptr) {
        return;
    }
    RECT client{};
    GetClientRect(m_hwnd, &client);

    DWM_THUMBNAIL_PROPERTIES props{};
    props.dwFlags = DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_VISIBLE | DWM_TNP_OPACITY |
                    DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE;
    props.fSourceClientAreaOnly = FALSE;
    props.fVisible = TRUE;
    // Opacity is handled by the layered window, not by the thumbnail, so the
    // letterbox bars fade together with the picture.
    props.opacity = 255;
    props.rcDestination = FitRect(client, m_crop);
    props.rcSource = m_crop;
    DwmUpdateThumbnailProperties(m_thumb, &props);
}

void Mirror::Disconnect() {
    if (m_thumb != nullptr) {
        DwmUnregisterThumbnail(m_thumb);
        m_thumb = nullptr;
    }
    m_source = nullptr;
}

void Mirror::MarkSourceGone() {
    Disconnect();
    m_sourceGone = true;
    InvalidateRect(m_hwnd, nullptr, TRUE);
}

void Mirror::SetOpacity(BYTE alpha) {
    if (m_hwnd == nullptr) {
        return;
    }
    m_opacity = alpha;
    SetLayeredWindowAttributes(m_hwnd, 0, alpha, LWA_ALPHA);
}

void Mirror::SetClickThrough(bool on) {
    if (m_hwnd == nullptr) {
        return;
    }
    m_clickThrough = on;
    const LONG_PTR ex = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    if (on) {
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex | WS_EX_TRANSPARENT);
    } else {
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    }
}

Mirror::Drag Mirror::HitZone(POINT clientPt) const {
    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const bool left = clientPt.x < kGrip;
    const bool right = clientPt.x >= rc.right - kGrip;
    const bool top = clientPt.y < kGrip;
    const bool bottom = clientPt.y >= rc.bottom - kGrip;

    if (top && left) return Drag::TopLeft;
    if (top && right) return Drag::TopRight;
    if (bottom && left) return Drag::BottomLeft;
    if (bottom && right) return Drag::BottomRight;
    if (left) return Drag::Left;
    if (right) return Drag::Right;
    if (top) return Drag::Top;
    if (bottom) return Drag::Bottom;
    return Drag::Move;
}

void Mirror::ApplyDrag(POINT screenPt) {
    if (m_drag == Drag::None || m_hwnd == nullptr) {
        return;
    }
    const int dx = screenPt.x - m_dragStartScreen.x;
    const int dy = screenPt.y - m_dragStartScreen.y;

    RECT r = m_dragStartRect;
    switch (m_drag) {
    case Drag::Move:
        OffsetRect(&r, dx, dy);
        break;
    case Drag::Left:
        r.left += dx;
        if (r.right - r.left < kMinWidth) r.left = r.right - kMinWidth;
        break;
    case Drag::Right:
        r.right += dx;
        if (r.right - r.left < kMinWidth) r.right = r.left + kMinWidth;
        break;
    case Drag::Top:
        r.top += dy;
        if (r.bottom - r.top < kMinHeight) r.top = r.bottom - kMinHeight;
        break;
    case Drag::Bottom:
        r.bottom += dy;
        if (r.bottom - r.top < kMinHeight) r.bottom = r.top + kMinHeight;
        break;
    case Drag::TopLeft:
        r.left += dx;
        r.top += dy;
        if (r.right - r.left < kMinWidth) r.left = r.right - kMinWidth;
        if (r.bottom - r.top < kMinHeight) r.top = r.bottom - kMinHeight;
        break;
    case Drag::TopRight:
        r.right += dx;
        r.top += dy;
        if (r.right - r.left < kMinWidth) r.right = r.left + kMinWidth;
        if (r.bottom - r.top < kMinHeight) r.top = r.bottom - kMinHeight;
        break;
    case Drag::BottomLeft:
        r.left += dx;
        r.bottom += dy;
        if (r.right - r.left < kMinWidth) r.left = r.right - kMinWidth;
        if (r.bottom - r.top < kMinHeight) r.bottom = r.top + kMinHeight;
        break;
    case Drag::BottomRight:
        r.right += dx;
        r.bottom += dy;
        if (r.right - r.left < kMinWidth) r.right = r.left + kMinWidth;
        if (r.bottom - r.top < kMinHeight) r.bottom = r.top + kMinHeight;
        break;
    default:
        return;
    }

    SetWindowPos(m_hwnd, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOACTIVATE);
}

void Mirror::UpdateCursor(POINT clientPt) {
    const Drag zone = HitZone(clientPt);
    LPWSTR name = IDC_ARROW;
    switch (zone) {
    case Drag::Left:
    case Drag::Right:
        name = IDC_SIZEWE;
        break;
    case Drag::Top:
    case Drag::Bottom:
        name = IDC_SIZENS;
        break;
    case Drag::TopLeft:
    case Drag::BottomRight:
        name = IDC_SIZENWSE;
        break;
    case Drag::TopRight:
    case Drag::BottomLeft:
        name = IDC_SIZENESW;
        break;
    default:
        // The body only moves while Shift is held, so advertise that instead of
        // showing a move cursor that would do nothing.
        name = (GetKeyState(VK_SHIFT) < 0) ? IDC_SIZEALL : IDC_ARROW;
        break;
    }
    SetCursor(LoadCursorW(nullptr, name));
}

void Mirror::ShowMenu(POINT screenPt) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING, IDM_PICK, L"新建预览\tCtrl+Alt+P");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU opacity = CreatePopupMenu();
    for (int i = 0; i < kOpacityStepCount; ++i) {
        wchar_t label[16]{};
        wsprintfW(label, L"%d%%", static_cast<int>(kOpacitySteps[i]) * 100 / 255);
        AppendMenuW(opacity, MF_STRING | (m_opacity == kOpacitySteps[i] ? MF_CHECKED : 0),
                    static_cast<UINT_PTR>(IDM_OPACITY_FIRST + i), label);
    }
    AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(opacity), L"透明度");
    AppendMenuW(menu, MF_STRING | (m_clickThrough ? MF_CHECKED : 0), IDM_CLICKTHROUGH,
                L"点击穿透（鼠标 + 键盘）");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_CLOSE, L"关闭这个预览");
    AppendMenuW(menu, MF_STRING, IDM_CLOSEALL, L"关闭所有预览");
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"退出 WindowPeeking\tCtrl+Alt+Q");

    // The window is normally WS_EX_NOACTIVATE so it never steals the keyboard.
    // A popup menu needs the foreground briefly; lend it back right after.
    const LONG_PTR exStyle = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, exStyle & ~static_cast<LONG_PTR>(WS_EX_NOACTIVATE));
    HWND previous = GetForegroundWindow();
    SetForegroundWindow(m_hwnd);

    const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                                      screenPt.x, screenPt.y, 0, m_hwnd, nullptr));
    DestroyMenu(menu);

    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, exStyle);
    if (previous != nullptr && previous != m_hwnd && IsWindow(previous)) {
        SetForegroundWindow(previous);
    }

    if (cmd >= IDM_OPACITY_FIRST && cmd < static_cast<UINT>(IDM_OPACITY_FIRST + kOpacityStepCount)) {
        SetOpacity(kOpacitySteps[cmd - IDM_OPACITY_FIRST]);
        return;
    }

    switch (cmd) {
    case IDM_PICK:
        if (HWND app = AppWindow()) {
            PostMessageW(app, WM_WINDOWPEEKING_PICK, 0, 0);
        }
        break;
    case IDM_CLICKTHROUGH: SetClickThrough(!m_clickThrough); break;
    // The hub window owns every preview, so closing one is a request, not a
    // self-destruct: it removes us from its list and deletes us.
    case IDM_CLOSE:
        if (HWND app = AppWindow()) {
            PostMessageW(app, WM_WINDOWPEEKING_CLOSE_PREVIEW, 0, reinterpret_cast<LPARAM>(m_hwnd));
        } else {
            Destroy();
        }
        break;
    case IDM_CLOSEALL:
        if (HWND app = AppWindow()) {
            PostMessageW(app, WM_WINDOWPEEKING_CLOSE_ALL, 0, 0);
        } else {
            Destroy();
        }
        break;
    case IDM_QUIT: PostQuitMessage(0); break;
    default: break;
    }
}

void Mirror::Destroy() {
    if (m_hwnd != nullptr) {
        KillTimer(m_hwnd, 1);
        Disconnect();
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

LRESULT CALLBACK Mirror::WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Mirror* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Mirror*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->m_hwnd = hwnd;
        }
    } else {
        self = reinterpret_cast<Mirror*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self != nullptr) {
        return self->Handle(msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Mirror::Handle(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    // Never become the active window: that is what lets the keyboard keep going
    // to whatever the user is actually working in.
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_LBUTTONDOWN: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const bool shift = (wParam & MK_SHIFT) != 0;
        Drag zone = HitZone(pt);
        if (shift) {
            zone = Drag::Move;      // Shift anywhere = move
        } else if (zone == Drag::Move) {
            zone = Drag::None;      // plain body drag does nothing, so no accidents
        }
        m_drag = zone;
        if (m_drag == Drag::None) {
            return 0;
        }
        m_dragStartScreen = pt;
        ClientToScreen(m_hwnd, &m_dragStartScreen);
        GetWindowRect(m_hwnd, &m_dragStartRect);
        SetCapture(m_hwnd);
        return 0;
    }

    case WM_MOUSEMOVE: {
        const POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (m_drag != Drag::None) {
            POINT screen = pt;
            ClientToScreen(m_hwnd, &screen);
            ApplyDrag(screen);
        } else {
            UpdateCursor(pt);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (m_drag != Drag::None) {
            m_drag = Drag::None;
            ReleaseCapture();
        }
        return 0;

    case WM_CAPTURECHANGED:
        m_drag = Drag::None;
        return 0;

    case WM_SETCURSOR: {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(m_hwnd, &pt);
        UpdateCursor(pt);
        return TRUE;
    }

    // Shift + wheel = opacity. Without Shift the wheel is left alone.
    case WM_MOUSEWHEEL: {
        if ((GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0) {
            SetOpacity(WheelOpacityStep(m_opacity, GET_WHEEL_DELTA_WPARAM(wParam)));
            return 0;
        }
        break;
    }

    case WM_RBUTTONUP: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(m_hwnd, &pt);
        ShowMenu(pt);
        return 0;
    }

    case WM_SIZE:
        UpdateThumb();
        return 0;

    case WM_TIMER:
        if (m_source != nullptr && !IsWindow(m_source)) {
            MarkSourceGone();
        }
        return 0;

    case WM_ERASEBKGND:
        if (m_sourceGone) {
            break;
        }
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(m_hwnd, &ps);
        if (m_sourceGone && dc != nullptr) {
            RECT rc{};
            GetClientRect(m_hwnd, &rc);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(230, 230, 230));
            DrawTextW(dc, L"源窗口已关闭 —— 按 Ctrl+Alt+P 重新选择", -1, &rc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        EndPaint(m_hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        Disconnect();
        m_hwnd = nullptr;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(m_hwnd, msg, wParam, lParam);
}

} // namespace wp
