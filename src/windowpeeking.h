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

#define WINDOWPEEKING_VERSION L"0.1.0"

// Posted to the existing instance to ask it to start a new pick.
#define WM_WINDOWPEEKING_PICK (WM_APP + 1)

namespace wp {

// ---- util.cpp ----
std::wstring ExeDir();
RECT ClientAreaInScreenSpace(HWND hwnd);
bool IsUsableSource(HWND hwnd);
// Fit "source" aspect inside "dest", centered (letterbox). Upstream algorithm.
RECT FitRect(RECT const& dest, RECT const& source);

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

    enum class Drag { None, Move, Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };
    Drag HitZone(POINT clientPt) const;
    void ApplyDrag(POINT screenPt);
    void UpdateCursor(POINT clientPt);

    Drag m_drag = Drag::None;
    POINT m_dragStartScreen{};
    RECT m_dragStartRect{};
};

} // namespace wp
