// windows.h defines min/max as macros unless told not to; std::min/std::max
// are needed below for the aspect-locked arithmetic.
#define NOMINMAX
#include "windowpeeking.h"
#include <windowsx.h>

#include <algorithm>
#include <cmath>

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

    const int cropW = m_crop.right - m_crop.left;
    const int cropH = m_crop.bottom - m_crop.top;
    if (cropW <= 0 || cropH <= 0) {
        return false;
    }

    // The frame is a window onto a piece of the source at 1:1, so its own size is
    // the crop size: one source pixel, one preview pixel.
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, cropW, cropH,
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

void Mirror::SetDragCursorOverride(bool armed, POINT screenPoint) {
    m_cursorOverride = armed;
    m_cursorOverridePoint = screenPoint;
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

void Mirror::ClampCropToSource() {
    if (m_source == nullptr || m_thumb == nullptr || !IsWindow(m_source)) {
        return;
    }
    RECT src{};
    if (!GetWindowRect(m_source, &src)) {
        return;
    }
    const int srcW = src.right - src.left;
    const int srcH = src.bottom - src.top;

    RECT c = m_crop;
    const bool outside = (c.left < 0 || c.top < 0 || c.right > srcW || c.bottom > srcH);
    if (!outside) {
        return;
    }
    if (c.left < 0) c.left = 0;
    if (c.top < 0) c.top = 0;
    const int cw = c.right - c.left;
    const int ch = c.bottom - c.top;
    if (cw < 8 || ch < 8) {
        return;     // the source shrank below anything usable; leave the frame be
    }
    const int roomW = srcW - c.left;
    const int roomH = srcH - c.top;
    if (roomW < 8 || roomH < 8) {
        return;
    }

    // The crop keeps its shape: shrink both axes by the same factor instead of
    // clipping one edge, which would leave the picture stretched.
    double f = 1.0;
    if (cw > roomW) f = std::min(f, static_cast<double>(roomW) / cw);
    if (ch > roomH) f = std::min(f, static_cast<double>(roomH) / ch);
    int nw = cw;
    int nh = ch;
    if (f < 1.0) {
        nw = static_cast<int>(std::lround(static_cast<double>(cw) * f));
        if (nw < 8) nw = 8;
        nh = static_cast<int>(std::lround(static_cast<double>(nw) * ch / cw));
        if (nh < 8) nh = 8;
    }
    if (nw > srcW - c.left) nw = srcW - c.left;
    if (nh > srcH - c.top) nh = srcH - c.top;
    if (nw == cw && nh == ch) {
        return;
    }

    c.right = c.left + nw;
    c.bottom = c.top + nh;
    m_crop = c;
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, nw, nh, SWP_NOMOVE | SWP_NOACTIVATE);
    UpdateThumb();
}

// The drag model, with the frame understood as a window onto the source:
//   edges and corners  resize the crop. By default the resize keeps the crop's
//                      current aspect ratio (the picture never distorts); hold
//                      Shift for a free resize where each axis moves alone.
//   body drag          moves the frame on screen, crop untouched
//   Shift + body drag  slides the crop over the source, frame untouched
void Mirror::ApplyDrag(POINT screenPt) {
    if (m_drag == Drag::None || m_hwnd == nullptr) {
        return;
    }
    const int dx = screenPt.x - m_dragStartScreen.x;
    const int dy = screenPt.y - m_dragStartScreen.y;

    if (m_drag == Drag::Move) {
        RECT r = m_dragStartRect;
        OffsetRect(&r, dx, dy);
        SetWindowPos(m_hwnd, HWND_TOPMOST, r.left, r.top, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }

    if (m_source == nullptr || m_thumb == nullptr || !IsWindow(m_source)) {
        return;
    }
    RECT src{};
    if (!GetWindowRect(m_source, &src)) {
        return;
    }
    const int srcW = src.right - src.left;
    const int srcH = src.bottom - src.top;
    if (srcW <= 0 || srcH <= 0) {
        return;
    }

    const RECT c0 = m_dragStartCrop;
    const int w0 = c0.right - c0.left;
    const int h0 = c0.bottom - c0.top;
    if (w0 <= 0 || h0 <= 0) {
        return;
    }

    if (m_drag == Drag::Reframe) {
        // Dragging right pulls the picture right, which brings the part of the
        // source to the LEFT of the crop into view: the crop moves the other way.
        int x = c0.left - dx;
        int y = c0.top - dy;
        int maxX = srcW - w0;
        int maxY = srcH - h0;
        if (maxX < 0) maxX = 0;
        if (maxY < 0) maxY = 0;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x > maxX) x = maxX;
        if (y > maxY) y = maxY;
        const RECT moved{ x, y, x + w0, y + h0 };
        if (moved.left == m_crop.left && moved.top == m_crop.top) {
            return;
        }
        m_crop = moved;
        UpdateThumb();
        return;
    }

    const bool corner = (m_drag == Drag::TopLeft || m_drag == Drag::TopRight ||
                         m_drag == Drag::BottomLeft || m_drag == Drag::BottomRight);
    const bool horizontal = (m_drag == Drag::Left || m_drag == Drag::Right);
    if (!corner && !horizontal && m_drag != Drag::Top && m_drag != Drag::Bottom) {
        return;
    }
    // Which side of the crop the mouse is holding; the opposite side stays put.
    const int dirX = (m_drag == Drag::Right || m_drag == Drag::TopRight ||
                      m_drag == Drag::BottomRight) ? 1 : -1;
    const int dirY = (m_drag == Drag::Bottom || m_drag == Drag::BottomLeft ||
                      m_drag == Drag::BottomRight) ? 1 : -1;

    int w = w0;
    int h = h0;
    if (corner) {
        w = w0 + dirX * dx;
        h = h0 + dirY * dy;
    } else if (horizontal) {
        w = w0 + dirX * dx;
    } else {
        h = h0 + dirY * dy;
    }

    if (m_dragFree) {
        // Shift: the two axes are independent, so the crop can take any shape.
        if (w < kMinWidth) w = kMinWidth;
        if (h < kMinHeight) h = kMinHeight;
    } else {
        // Aspect-locked. The axis the mouse moved furthest (relative to the
        // starting size) drives the scale; the other dimension follows it.
        double scale = 1.0;
        if (corner) {
            const double sx = static_cast<double>(w) / w0;
            const double sy = static_cast<double>(h) / h0;
            scale = (std::fabs(sx - 1.0) >= std::fabs(sy - 1.0)) ? sx : sy;
        } else if (horizontal) {
            scale = static_cast<double>(w) / w0;
        } else {
            scale = static_cast<double>(h) / h0;
        }

        // How far it may grow before leaving the source, and how far it may
        // shrink before hitting the minimum frame size.
        double maxScale = 0.0;
        if (corner) {
            const int availW = (dirX > 0) ? (srcW - c0.left) : c0.right;
            const int availH = (dirY > 0) ? (srcH - c0.top) : c0.bottom;
            maxScale = std::min(static_cast<double>(availW) / w0,
                                static_cast<double>(availH) / h0);
        } else if (horizontal) {
            const int availW = (dirX > 0) ? (srcW - c0.left) : c0.right;
            maxScale = std::min(static_cast<double>(availW) / w0,
                                static_cast<double>(srcH) / h0);
        } else {
            const int availH = (dirY > 0) ? (srcH - c0.top) : c0.bottom;
            maxScale = std::min(static_cast<double>(availH) / h0,
                                static_cast<double>(srcW) / w0);
        }
        const double minScale = std::max(static_cast<double>(kMinWidth) / w0,
                                         static_cast<double>(kMinHeight) / h0);
        if (scale < minScale) scale = minScale;
        if (scale > maxScale) scale = maxScale;

        w = static_cast<int>(std::lround(static_cast<double>(w0) * scale));
        if (w < 1) w = 1;
        h = static_cast<int>(std::lround(static_cast<double>(w) * h0 / w0));
        if (h < 1) h = 1;
        // Rounding must not sneak past the limits.
        if (w < kMinWidth) {
            w = kMinWidth;
            h = static_cast<int>(std::lround(static_cast<double>(w) * h0 / w0));
        }
        if (h < kMinHeight) {
            h = kMinHeight;
            w = static_cast<int>(std::lround(static_cast<double>(h) * w0 / h0));
        }
        if (w > srcW) w = srcW;
        if (h > srcH) h = srcH;
    }

    RECT c = c0;
    if (corner) {
        if (dirX > 0) {
            c.left = c0.left;
            c.right = c0.left + w;
        } else {
            c.right = c0.right;
            c.left = c0.right - w;
        }
        if (dirY > 0) {
            c.top = c0.top;
            c.bottom = c0.top + h;
        } else {
            c.bottom = c0.bottom;
            c.top = c0.bottom - h;
        }
    } else if (horizontal) {
        if (dirX > 0) {
            c.left = c0.left;
            c.right = c0.left + w;
        } else {
            c.right = c0.right;
            c.left = c0.right - w;
        }
        // The other axis has no handle under the mouse, so it grows around the
        // crop's own centre: the picture stays where it was and widens both ways.
        const int grow = h - h0;
        c.top = c0.top - grow / 2;
        c.bottom = c.top + h;
    } else {
        if (dirY > 0) {
            c.top = c0.top;
            c.bottom = c0.top + h;
        } else {
            c.bottom = c0.bottom;
            c.top = c0.bottom - h;
        }
        const int grow = w - w0;
        c.left = c0.left - grow / 2;
        c.right = c.left + w;
    }

    // There is only the source to look at: slide the crop back inside it. The
    // size is already bounded above, so this only ever shifts, never clips.
    if (c.left < 0) {
        c.right -= c.left;
        c.left = 0;
    }
    if (c.top < 0) {
        c.bottom -= c.top;
        c.top = 0;
    }
    if (c.right > srcW) {
        c.left -= (c.right - srcW);
        c.right = srcW;
    }
    if (c.bottom > srcH) {
        c.top -= (c.bottom - srcH);
        c.bottom = srcH;
    }
    if (c.left < 0) c.left = 0;
    if (c.top < 0) c.top = 0;

    if (c.left == m_crop.left && c.top == m_crop.top && c.right == m_crop.right &&
        c.bottom == m_crop.bottom) {
        return;
    }

    // The frame is the crop at 1:1, so it moves with the crop's top-left corner
    // and takes the crop's size. Growing the crop rightwards therefore keeps the
    // frame's left edge still, exactly like growing it leftwards keeps the right.
    RECT r = m_dragStartRect;
    r.left += (c.left - c0.left);
    r.top += (c.top - c0.top);
    r.right = r.left + (c.right - c.left);
    r.bottom = r.top + (c.bottom - c.top);

    m_crop = c;
    SetWindowPos(m_hwnd, HWND_TOPMOST, r.left, r.top, r.right - r.left,
                 r.bottom - r.top, SWP_NOACTIVATE);
    UpdateThumb();
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
        // Body: drag moves the frame, Shift + drag slides the crop underneath it.
        name = (GetKeyState(VK_SHIFT) < 0) ? IDC_HAND : IDC_SIZEALL;
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
        if (zone == Drag::Move && shift) {
            zone = Drag::Reframe;   // Shift on the body slides the crop
        }
        m_drag = zone;
        // Shift on an edge or corner means "free resize" instead of the default
        // aspect-locked one. Refreshed on every move below, so it can be pressed
        // or released mid-drag.
        m_dragFree = shift && zone != Drag::Reframe && zone != Drag::Move;
        if (m_drag == Drag::None) {
            return 0;
        }
        m_dragStartScreen = pt;
        ClientToScreen(m_hwnd, &m_dragStartScreen);
        GetWindowRect(m_hwnd, &m_dragStartRect);
        m_dragStartCrop = m_crop;
        SetCapture(m_hwnd);
        return 0;
    }

    case WM_MOUSEMOVE: {
        const POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (m_drag != Drag::None) {
            if (m_drag != Drag::Move && m_drag != Drag::Reframe) {
                m_dragFree = (wParam & MK_SHIFT) != 0;
            }
            // Deliberately the cursor's own screen position rather than lParam:
            // client coordinates are relative to a window that this very drag is
            // moving, so they lag a step behind and a fast edge drag stutters.
            POINT cursor = m_cursorOverridePoint;
            if (m_cursorOverride || GetCursorPos(&cursor)) {
                ApplyDrag(cursor);
            }
        } else {
            UpdateCursor(pt);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (m_drag != Drag::None) {
            m_drag = Drag::None;
            m_dragFree = false;
            ReleaseCapture();
        }
        return 0;

    case WM_CAPTURECHANGED:
        m_drag = Drag::None;
        m_dragFree = false;
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
        } else if (m_source != nullptr) {
            ClampCropToSource();    // the source window may have been resized
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
