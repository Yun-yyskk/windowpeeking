// WindowPeeking - a borderless live crop of any window.
// Derived in part from Microsoft PowerToys Crop And Lock (MIT).
// See THIRD-PARTY-NOTICES.md.
#pragma once

#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <dwmapi.h>
#include <string>

#define WINDOWPEEKING_VERSION L"0.7.0"

// Posted to the existing instance to ask it to start a new pick.
#define WM_WINDOWPEEKING_PICK (WM_APP + 1)
// Posted by a preview window when the user closes it from its own menu.
// lParam is the preview HWND; the hub window owns the lifetime of every preview.
#define WM_WINDOWPEEKING_CLOSE_PREVIEW (WM_APP + 3)
// Close every preview, keep the tray icon.
#define WM_WINDOWPEEKING_CLOSE_ALL (WM_APP + 4)

namespace wp {

// ---- util.cpp ----
std::wstring ExeDir();
RECT ClientAreaInScreenSpace(HWND hwnd);
bool IsUsableSource(HWND hwnd);
// Fit "source" aspect inside "dest", centered (letterbox). Upstream algorithm.
RECT FitRect(RECT const& dest, RECT const& source);
// Opacity for one wheel notch: +/-10, clamped to 10%..100%.
BYTE WheelOpacityStep(BYTE current, int wheelDelta);

// Result of one drag across the picker overlay.
struct Selection {
    bool valid = false;
    RECT screenRect{};      // where the windowpeeking should be placed, screen space
    RECT cropWindowSpace{}; // what to show, relative to the source window origin
};

// Pure maths behind the picker: normalise a drag, reject accidental clicks,
// map it to screen space and clamp it to the source window. startClient and
// curClient are overlay-client coordinates; virtualScreen is the overlay rect.
Selection ComputeSelection(RECT const& virtualScreen, POINT startClient, POINT curClient,
                           RECT const& sourceWindowRect);

// ---- main.cpp ----
// Always-present message-only hub; other instances post WM_WINDOWPEEKING_PICK here.
HWND AppWindow();

// ---- picker.cpp ----
// Runs a modal region-picker. Returns false when cancelled.
// outCrop is in source-window coordinates (window space, not client space).
bool PickRegion(HINSTANCE instance, HWND* outSource, RECT* outCrop, RECT* outScreenRect);

// ---- mirror.cpp ----
class Mirror {
public:
    static bool RegisterClassOnce(HINSTANCE instance);

    bool Create(HINSTANCE instance, RECT const& screenRect, int width, int height);
    bool Attach(HWND source, RECT const& cropInWindowSpace);
    void MoveTo(RECT const& screenRect);
    HWND Hwnd() const { return m_hwnd; }
    void Destroy();
    void SetOpacity(BYTE alpha);
    void SetClickThrough(bool on);
    bool ClickThrough() const { return m_clickThrough; }
    BYTE Opacity() const { return m_opacity; }
    // The region of the source window being shown, in source-window coordinates.
    RECT Crop() const { return m_crop; }
    // Test seam. A drag tracks the physical mouse, but --selftest runs where the
    // cursor cannot be moved (a disconnected or locked desktop), so it pins the
    // point the drag handlers should believe instead.
    void SetDragCursorOverride(bool armed, POINT screenPoint);

private:
    static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wParam, LPARAM lParam);
    void UpdateThumb();
    void Disconnect();
    void ShowMenu(POINT screenPt);
    void MarkSourceGone();

    HINSTANCE m_instance = nullptr;
    HWND m_hwnd = nullptr;
    HWND m_source = nullptr;
    RECT m_crop{};
    HTHUMBNAIL m_thumb = nullptr;
    BYTE m_opacity = 255;
    bool m_clickThrough = false;
    bool m_sourceGone = false;

    enum class Drag { None, Move, Reframe, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };
    Drag HitZone(POINT clientPt) const;
    void ApplyDrag(POINT screenPt);
    void UpdateCursor(POINT clientPt);
    void ClampCropToSource();

    Drag m_drag = Drag::None;
    // Edges and corners keep the crop's aspect ratio unless Shift is held, in
    // which case each axis moves on its own. Latched from the mouse messages so
    // the rule is testable with synthetic input instead of a physical keyboard.
    bool m_dragFree = false;
    bool m_cursorOverride = false;
    POINT m_cursorOverridePoint{};
    POINT m_dragStartScreen{};
    RECT m_dragStartRect{};
    RECT m_dragStartCrop{};
};

} // namespace wp
