#include "windowpeeking.h"

namespace wp {

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf);
    const auto p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? s : s.substr(0, p);
}

RECT ClientAreaInScreenSpace(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    POINT tl{ rc.left, rc.top };
    POINT br{ rc.right, rc.bottom };
    ClientToScreen(hwnd, &tl);
    ClientToScreen(hwnd, &br);
    return RECT{ tl.x, tl.y, br.x, br.y };
}

Selection ComputeSelection(RECT const& virtualScreen, POINT startClient, POINT curClient,
                           RECT const& sourceWindowRect) {
    Selection out;

    RECT sel{};
    sel.left = startClient.x < curClient.x ? startClient.x : curClient.x;
    sel.top = startClient.y < curClient.y ? startClient.y : curClient.y;
    sel.right = startClient.x < curClient.x ? curClient.x : startClient.x;
    sel.bottom = startClient.y < curClient.y ? curClient.y : startClient.y;

    if (sel.right - sel.left < 8 || sel.bottom - sel.top < 8) {
        return out; // an accidental click, not a region
    }

    const RECT selScreen{ virtualScreen.left + sel.left, virtualScreen.top + sel.top,
                          virtualScreen.left + sel.right, virtualScreen.top + sel.bottom };

    RECT crop{ selScreen.left - sourceWindowRect.left, selScreen.top - sourceWindowRect.top,
               selScreen.right - sourceWindowRect.left, selScreen.bottom - sourceWindowRect.top };

    const LONG fullW = sourceWindowRect.right - sourceWindowRect.left;
    const LONG fullH = sourceWindowRect.bottom - sourceWindowRect.top;
    if (crop.left < 0) crop.left = 0;
    if (crop.top < 0) crop.top = 0;
    if (crop.right > fullW) crop.right = fullW;
    if (crop.bottom > fullH) crop.bottom = fullH;

    if (crop.right - crop.left < 8 || crop.bottom - crop.top < 8) {
        return out; // the drag landed outside the window
    }

    out.valid = true;
    out.screenRect = selScreen;
    out.cropWindowSpace = crop;
    return out;
}

bool IsUsableSource(HWND hwnd) {
    if (hwnd == nullptr || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return false;
    }
    wchar_t cls[256]{};
    if (GetClassNameW(hwnd, cls, 256) == 0) {
        return false;
    }
    // Never mirror our own windows or the shell's.
    static const wchar_t* kBlocked[] = {
        L"WindowPeeking.Mirror", L"WindowPeeking.Picker", L"Progman", L"WorkerW",
        L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
    };
    for (auto name : kBlocked) {
        if (wcscmp(cls, name) == 0) {
            return false;
        }
    }
    return true;
}

// Same math as PowerToys ThumbnailCropAndLockWindow: scale the crop rect to fit
// the destination and center the result, keeping the source aspect ratio.
RECT FitRect(RECT const& dest, RECT const& source) {
    const auto destW = static_cast<float>(dest.right - dest.left);
    const auto destH = static_cast<float>(dest.bottom - dest.top);
    const auto srcW = static_cast<float>(source.right - source.left);
    const auto srcH = static_cast<float>(source.bottom - source.top);

    if (srcW <= 0.0f || srcH <= 0.0f || destW <= 0.0f || destH <= 0.0f) {
        return dest;
    }

    auto scale = destW / srcW;
    if (destW / destH > srcW / srcH) {
        scale = destH / srcH;
    }

    const auto contentW = static_cast<LONG>(srcW * scale);
    const auto contentH = static_cast<LONG>(srcH * scale);
    const auto left = (dest.right - dest.left - contentW) / 2;
    const auto top = (dest.bottom - dest.top - contentH) / 2;

    return RECT{ left, top, left + contentW, top + contentH };
}

} // namespace wp
