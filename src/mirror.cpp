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
    IDM_OPACITY_100,
    IDM_OPACITY_80,
    IDM_OPACITY_60,
    IDM_OPACITY_40,
    IDM_CLICKTHROUGH,
    IDM_CLOSE,
    IDM_QUIT,
};

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
    wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
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
    const DWORD exStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    m_hwnd = CreateWindowExW(exStyle, kClassName, L"WindowPeeking", style,
                             screenRect.left, screenRect.top, width, height,
                             nullptr, nullptr, instance, this);
    if (m_hwnd == nullptr) {
        return false;
    }
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
    const auto ex = static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE));
    if (alpha < 255) {
        if ((ex & WS_EX_LAYERED) == 0) {
            SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(ex | WS_EX_LAYERED));
        }
        SetLayeredWindowAttributes(m_hwnd, 0, alpha, LWA_ALPHA);
    } else if ((ex & WS_EX_LAYERED) != 0) {
        SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);
    }
}

void Mirror::SetClickThrough(bool on) {
    if (m_hwnd == nullptr) {
        return;
    }
    m_clickThrough = on;
    const auto ex = static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE));
    if (on) {
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE,
                          static_cast<LONG_PTR>(ex | WS_EX_LAYERED | WS_EX_TRANSPARENT));
        SetLayeredWindowAttributes(m_hwnd, 0, m_opacity, LWA_ALPHA);
    } else {
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE,
                          static_cast<LONG_PTR>(ex & ~static_cast<DWORD>(WS_EX_TRANSPARENT)));
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
    LPWSTR name = IDC_SIZEALL;
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
        break;
    }
    SetCursor(LoadCursorW(nullptr, name));
}

void Mirror::ShowMenu(POINT screenPt) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING, IDM_PICK, L"Pick another window\tCtrl+Alt+P");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU opacity = CreatePopupMenu();
    AppendMenuW(opacity, MF_STRING | (m_opacity == 255 ? MF_CHECKED : 0), IDM_OPACITY_100, L"100%");
    AppendMenuW(opacity, MF_STRING | (m_opacity == 204 ? MF_CHECKED : 0), IDM_OPACITY_80, L"80%");
    AppendMenuW(opacity, MF_STRING | (m_opacity == 153 ? MF_CHECKED : 0), IDM_OPACITY_60, L"60%");
    AppendMenuW(opacity, MF_STRING | (m_opacity == 102 ? MF_CHECKED : 0), IDM_OPACITY_40, L"40%");
    AppendMenuW(menu, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(opacity), L"Opacity");

    AppendMenuW(menu, MF_STRING | (m_clickThrough ? MF_CHECKED : 0), IDM_CLICKTHROUGH,
                L"Click-through\tCtrl+Alt+O");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_CLOSE, L"Close this windowpeeking");
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit WindowPeeking\tCtrl+Alt+Q");

    SetForegroundWindow(m_hwnd);
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                                      screenPt.x, screenPt.y, 0, m_hwnd, nullptr));
    DestroyMenu(menu);

    switch (cmd) {
    case IDM_PICK:
        if (HWND app = AppWindow()) {
            PostMessageW(app, WM_WINDOWPEEKING_PICK, 0, 0);
        }
        break;
    case IDM_OPACITY_100: SetOpacity(255); break;
    case IDM_OPACITY_80: SetOpacity(204); break;
    case IDM_OPACITY_60: SetOpacity(153); break;
    case IDM_OPACITY_40: SetOpacity(102); break;
    case IDM_CLICKTHROUGH: SetClickThrough(!m_clickThrough); break;
    case IDM_CLOSE: Destroy(); break;
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
    case WM_LBUTTONDOWN: {
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        m_drag = HitZone(pt);
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
            DrawTextW(dc, L"source closed  -  Ctrl+Alt+P to pick another", -1, &rc,
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
