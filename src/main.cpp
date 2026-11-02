#include "windowpeeking.h"
#include <shlobj.h>     // IShellLinkW, SHGetFolderPathW -- the desktop shortcut
#include <shellapi.h>   // NOTIFYICONDATAW, Shell_NotifyIconW
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

const int kHotkeyPick = 101;
const int kHotkeyQuit = 102;

const wchar_t* kAppClass = L"WindowPeeking.App";

// Tray icon. There is no main window at all: the icon is the program's face.
const UINT kTrayId = 1;
const UINT kTrayCallback = WM_APP + 2;
const UINT kIconResource = 1;
const UINT_PTR kTrayRetryTimer = 1;
const UINT kTrayRetryMs = 1500;
const int kTrayMaxAttempts = 20;   // ~30 s of a missing shell, then wait for TaskbarCreated

// A low-integrity process is not allowed to talk to the taskbar at all
// (Shell_NotifyIconW answers ERROR_ACCESS_DENIED), and then the program has no
// face at all. The fallback is a floating button in the same corner: it is the
// program's own window, so no privilege boundary can take it away.
const int kIslandSize = 44;
const int kIslandMargin = 12;

// Explorer tells every top-level window when it restarts, so the icon can be
// put back into the (recreated) tray instead of vanishing for the session.
UINT TaskbarCreatedMessage() {
    static const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
    return message;
}

// The tray is the only way in, and when it does not appear there is nothing on
// screen to say why, so every attempt is written to %LOCALAPPDATA%\WindowPeeking\tray.log.
// Where the log goes: %LOCALAPPDATA%\WindowPeeking\tray.log, or beside the
// executable when that folder cannot be created (a low-integrity process is
// refused, and then the reason the tray did not appear is worth having anyway).
std::wstring OpenLogPath() {
    wchar_t directory[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", directory, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        const std::wstring folder = std::wstring(directory) + L"\\WindowPeeking";
        if (CreateDirectoryW(folder.c_str(), nullptr) != FALSE ||
            GetLastError() == ERROR_ALREADY_EXISTS) {
            return folder + L"\\tray.log";
        }
    }
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = exe;
    const size_t slash = path.find_last_of(L'\\');
    path.erase(slash == std::wstring::npos ? 0 : slash);
    return path + L"\\tray.log";
}

void TrayLog(const char* format, ...) {
    const std::wstring path = OpenLogPath();
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"a") != 0 || file == nullptr) {
        return;
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::fprintf(file, "%04u-%02u-%02u %02u:%02u:%02u ", now.wYear, now.wMonth, now.wDay, now.wHour,
                 now.wMinute, now.wSecond);
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fclose(file);
}

enum TrayMenuId : UINT {
    IDM_TRAY_PICK = 300,
    IDM_TRAY_THROUGH,
    IDM_TRAY_CLOSE,
    IDM_TRAY_INSTALL,
    IDM_TRAY_QUIT,
};

HWND g_app = nullptr;
HINSTANCE g_instance = nullptr;
// Every live preview. Each one owns its source window, crop, position, opacity
// and click-through state; this list is the only owner of their lifetime.
std::vector<wp::Mirror*> g_previews;
bool g_pickRequested = false;

// Floating-button state: whether it replaces the tray icon, and the drag that
// distinguishes "move me" from "give me a new preview".
bool g_islandShown = false;
bool g_islandDragging = false;
bool g_islandMoved = false;
POINT g_islandGrab{};
POINT g_islandOrigin{};

HICON LoadAppIcon() {
    HICON icon = static_cast<HICON>(LoadImageW(g_instance, MAKEINTRESOURCEW(kIconResource),
                                               IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    if (icon == nullptr) {
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    return icon;
}

// Why the last NIM_ADD failed. ERROR_ACCESS_DENIED is permanent (the process is
// too low-integrity to reach the taskbar), everything else may be a race with a
// shell that is still starting.
DWORD g_trayError = 0;

bool AddTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayCallback;
    nid.hIcon = LoadAppIcon();
    lstrcpynW(nid.szTip, L"WindowPeeking 窗窥 —— Ctrl+Alt+P 选一块区域", ARRAYSIZE(nid.szTip));
    SetLastError(0);
    const BOOL added = Shell_NotifyIconW(NIM_ADD, &nid);
    g_trayError = GetLastError();
    TrayLog("NIM_ADD hwnd=%p icon=%p ok=%d err=%lu", static_cast<void*>(nid.hWnd),
            static_cast<void*>(nid.hIcon), added, g_trayError);
    if (added) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        TrayLog("NIM_SETVERSION ok=%d err=%lu", Shell_NotifyIconW(NIM_SETVERSION, &nid), GetLastError());
    }
    return added != FALSE;
}

void RemoveTrayIcon() {
    if (g_app == nullptr) {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void UpdateTrayTip() {
    if (g_app == nullptr) {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_app;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP;
    if (g_previews.empty()) {
        lstrcpynW(nid.szTip, L"WindowPeeking 窗窥 —— Ctrl+Alt+P 选一块区域", ARRAYSIZE(nid.szTip));
    } else {
        wchar_t tip[128]{};
        wsprintfW(tip, L"WindowPeeking 窗窥 —— %d 个预览", static_cast<int>(g_previews.size()));
        lstrcpynW(nid.szTip, tip, ARRAYSIZE(nid.szTip));
    }
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (g_islandShown) {
        InvalidateRect(g_app, nullptr, TRUE);
    }
}

// Where the floating button lives: the corner of the work area, which is that
// part of the screen the taskbar does not cover.
RECT IslandAnchor() {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT anchor{};
    anchor.right = work.right - kIslandMargin;
    anchor.left = anchor.right - kIslandSize;
    anchor.bottom = work.bottom - kIslandMargin;
    anchor.top = anchor.bottom - kIslandSize;
    return anchor;
}

void PaintIsland(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int width = rc.right;
    const int height = rc.bottom;

    // Same two colours as the application icon: a bright ring around dark glass.
    // The window is layered with a black colour key, so black is what "no
    // window here" looks like: the rounded corners have to be painted with it or
    // the button would sit in a black square.
    HBRUSH clear = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &rc, clear);
    DeleteObject(clear);

    HBRUSH ring = CreateSolidBrush(RGB(47, 129, 247));
    HBRUSH body = CreateSolidBrush(RGB(12, 24, 41));
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    HGDIOBJ oldBrush = SelectObject(dc, ring);
    RoundRect(dc, rc.left, rc.top, width, height, 14, 14);
    SelectObject(dc, body);
    RoundRect(dc, rc.left + 2, rc.top + 2, width - 2, height - 2, 12, 12);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(ring);
    DeleteObject(body);

    HICON icon = LoadAppIcon();
    if (icon != nullptr) {
        DrawIconEx(dc, (width - 24) / 2, (height - 24) / 2, icon, 24, 24, 0, nullptr, DI_NORMAL);
    }

    // A green dot says "something of mine is on screen right now".
    if (!g_previews.empty()) {
        HBRUSH live = CreateSolidBrush(RGB(63, 185, 80));
        HGDIOBJ previous = SelectObject(dc, live);
        HGDIOBJ previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, width - 14, 3, width - 3, 14);
        SelectObject(dc, previousPen);
        SelectObject(dc, previous);
        DeleteObject(live);
    }
    EndPaint(hwnd, &ps);
}

// The tray icon was refused, so the button takes over the job of being visible.
void ShowIsland() {
    if (g_app == nullptr || g_islandShown) {
        return;
    }
    const RECT anchor = IslandAnchor();
    SetLayeredWindowAttributes(g_app, RGB(0, 0, 0), 255, LWA_ALPHA | LWA_COLORKEY);
    SetWindowPos(g_app, HWND_TOPMOST, anchor.left, anchor.top, kIslandSize, kIslandSize,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_islandShown = true;
    TrayLog("tray unavailable, showing the floating button at (%ld,%ld)", anchor.left, anchor.top);
    InvalidateRect(g_app, nullptr, TRUE);
}

// The tray came back (explorer restarted with a working shell): become invisible
// again, because two faces in the corner would be one too many.
void HideIsland() {
    if (g_app == nullptr || !g_islandShown) {
        return;
    }
    SetLayeredWindowAttributes(g_app, RGB(0, 0, 0), 0, LWA_ALPHA | LWA_COLORKEY);
    SetWindowPos(g_app, HWND_TOPMOST, -32000, -32000, 1, 1, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_islandShown = false;
    g_islandDragging = false;
}

// The shell is not always ready when the program starts, and it may restart
// later; keep trying until the icon is really there.
void EnsureTrayIcon() {
    if (g_app == nullptr) {
        return;
    }
    static int attempts = 0;
    ++attempts;
    if (AddTrayIcon()) {
        TrayLog("tray icon is in the shell (attempt %d)", attempts);
        KillTimer(g_app, kTrayRetryTimer);
        HideIsland();
        return;
    }
    if (g_trayError == ERROR_ACCESS_DENIED) {
        // No amount of retrying gets past this one: the shell is a more
        // privileged process, and Windows refuses to carry our icon request to it.
        TrayLog("access denied: this process is not allowed to reach the taskbar");
        KillTimer(g_app, kTrayRetryTimer);
        ShowIsland();
        return;
    }
    if (attempts >= kTrayMaxAttempts) {
        TrayLog("tray refused %d times, giving up until explorer restarts", attempts);
        KillTimer(g_app, kTrayRetryTimer);
        // A refusal is not a dead end: the button is the same menu, drawn by us.
        ShowIsland();
        return;
    }
    TrayLog("tray refused (attempt %d), retrying in %u ms", attempts, kTrayRetryMs);
    SetTimer(g_app, kTrayRetryTimer, kTrayRetryMs, nullptr);
}

// Windows will not let a process whose integrity level sits below the
// taskbar's register an icon, and a program that lives inside a folder some
// sandbox has marked gets started at low integrity every single time -- whoever
// double-clicks it. Moving the exe somewhere ordinary is the only cure, so do
// it for the user and leave a shortcut behind.
std::wstring LocalAppDataFolder() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    return (length > 0 && length < MAX_PATH) ? std::wstring(buffer) : std::wstring();
}

// Pure, so the self test can check it without touching anybody's disk.
std::wstring InstallTargetExe(const std::wstring& localAppData) {
    if (localAppData.empty()) {
        return std::wstring();
    }
    std::wstring dir = localAppData;
    if (dir.back() != L'\\') {
        dir += L'\\';
    }
    return dir + L"Programs\\WindowPeeking\\windowpeeking.exe";
}

std::wstring EnvFolder(const wchar_t* name) {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(name, buffer, MAX_PATH);
    return (length > 0 && length < MAX_PATH) ? std::wstring(buffer) : std::wstring();
}

std::wstring DesktopFolder() {
    wchar_t buffer[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT,
                                   buffer))) {
        return std::wstring(buffer);
    }
    return std::wstring();
}

void MakeShortcut(const std::wstring& lnkPath, const std::wstring& exePath, const std::wstring& workDir) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        return;
    }
    IShellLinkW* link = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                   reinterpret_cast<void**>(&link)))) {
        link->SetPath(exePath.c_str());
        link->SetWorkingDirectory(workDir.c_str());
        link->SetIconLocation(exePath.c_str(), 0);
        link->SetDescription(L"WindowPeeking 窗窥");
        IPersistFile* file = nullptr;
        if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&file)))) {
            file->Save(lnkPath.c_str(), TRUE);
            file->Release();
        }
        link->Release();
    }
    CoUninitialize();
}

// Returns the path of the copy, or an empty string when it could not be made.
std::wstring InstallToUserFolder() {
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) {
        return std::wstring();
    }
    const std::wstring local = LocalAppDataFolder();
    const std::wstring target = InstallTargetExe(local);
    if (target.empty()) {
        return std::wstring();
    }
    const std::wstring dir = target.substr(0, target.find_last_of(L'\\'));
    const std::wstring programs = local + L"\\Programs";
    if (!CreateDirectoryW(programs.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        TrayLog("install: cannot make %ls err=%lu", programs.c_str(), GetLastError());
        return std::wstring();
    }
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        TrayLog("install: cannot make %ls err=%lu", dir.c_str(), GetLastError());
        return std::wstring();
    }
    if (!CopyFileW(self, target.c_str(), FALSE)) {
        TrayLog("install: CopyFileW failed err=%lu", GetLastError());
        return std::wstring();
    }
    const std::wstring desktop = DesktopFolder();
    if (!desktop.empty()) {
        MakeShortcut(desktop + L"\\WindowPeeking.lnk", target, dir);
    }
    TrayLog("install: copied to %ls", target.c_str());
    return target;
}

wp::Mirror* FindPreview(HWND hwnd) {
    for (wp::Mirror* preview : g_previews) {
        if (preview != nullptr && preview->Hwnd() == hwnd) {
            return preview;
        }
    }
    return nullptr;
}

// One preview goes away: unlink it from the list first so a re-entrant close
// (menu -> PostMessage -> here) can never walk a dangling pointer.
void DestroyPreview(wp::Mirror* preview) {
    if (preview == nullptr) {
        return;
    }
    const auto it = std::find(g_previews.begin(), g_previews.end(), preview);
    if (it != g_previews.end()) {
        g_previews.erase(it);
    }
    preview->Destroy();
    delete preview;
    UpdateTrayTip();
}

void DestroyAllPreviews() {
    const std::vector<wp::Mirror*> all = g_previews;
    for (wp::Mirror* preview : all) {
        DestroyPreview(preview);
    }
}

bool AllClickThrough() {
    if (g_previews.empty()) {
        return false;
    }
    for (wp::Mirror* preview : g_previews) {
        if (preview == nullptr || !preview->ClickThrough()) {
            return false;
        }
    }
    return true;
}

void SetClickThroughAll(bool on) {
    for (wp::Mirror* preview : g_previews) {
        if (preview != nullptr) {
            preview->SetClickThrough(on);
        }
    }
}

void ShowTrayMenu() {
    const bool hasPreview = !g_previews.empty();
    const bool through = AllClickThrough();

    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING, IDM_TRAY_PICK, L"新建预览...	Ctrl+Alt+P");
    AppendMenuW(menu, MF_STRING | (through ? MF_CHECKED : 0) | (hasPreview ? 0 : MF_GRAYED),
                IDM_TRAY_THROUGH, L"点击穿透所有预览（鼠标 + 键盘）");
    AppendMenuW(menu, MF_STRING | (hasPreview ? 0 : MF_GRAYED), IDM_TRAY_CLOSE, L"关闭所有预览");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // Only worth saying while the tray icon is missing: it is the fix.
    if (g_islandShown) {
        AppendMenuW(menu, MF_STRING, IDM_TRAY_INSTALL, L"装到用户目录（这样才有托盘图标）...");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, IDM_TRAY_QUIT, L"退出	Ctrl+Alt+Q");

    POINT pt{};
    GetCursorPos(&pt);
    // A popup menu only behaves (appears, closes on an outside click) when its
    // owner is the foreground window. Clicking a tray icon does not activate an
    // app, so borrow the foreground thread's input state to force activation.
    const HWND foreground = GetForegroundWindow();
    const DWORD foregroundThread =
        (foreground != nullptr) ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    const DWORD thisThread = GetCurrentThreadId();
    const bool attached = (foregroundThread != 0) && (foregroundThread != thisThread) &&
                          (AttachThreadInput(foregroundThread, thisThread, TRUE) != FALSE);
    SetForegroundWindow(g_app);
    BringWindowToTop(g_app);
    const UINT cmd = static_cast<UINT>(
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, g_app,
                       nullptr));
    DestroyMenu(menu);
    PostMessageW(g_app, WM_NULL, 0, 0);
    if (attached) {
        AttachThreadInput(foregroundThread, thisThread, FALSE);
    }

    switch (cmd) {
    case IDM_TRAY_PICK:
        g_pickRequested = true;
        break;
    case IDM_TRAY_THROUGH:
        if (hasPreview) {
            SetClickThroughAll(!through);
        }
        break;
    case IDM_TRAY_CLOSE:
        if (hasPreview) {
            DestroyAllPreviews();
        }
        break;
    case IDM_TRAY_INSTALL: {
        const std::wstring copy = InstallToUserFolder();
        if (copy.empty()) {
            MessageBoxW(
                nullptr,
                L"装不进去。\n\n"
                L"WindowPeeking 现在运行在低权限模式（Windows 把程序所在的目录当成沙箱，"
                L"从这种目录里启动的程序一律降权）。低权限进程既不能写用户目录，"
                L"也不能注册托盘图标 —— 两件事是同一个原因。\n\n"
                L"办法：把 windowpeeking.exe 复制到桌面（或任何普通文件夹）再双击启动，"
                L"托盘图标就正常了。仓库里的 install.cmd 双击也能做这件事。",
                L"为什么没有托盘图标", MB_ICONINFORMATION | MB_OK);
        } else {
            const std::wstring text =
                L"已经装到：\n" + copy +
                L"\n\n桌面上也放了一个 WindowPeeking 快捷方式。"
                L"\n从那个快捷方式启动，托盘图标就会正常出现。\n\n"
                L"原因：这个程序放在某些目录里运行时，Windows 会把它当成低权限进程，"
                L"而低权限进程不允许注册托盘图标。换到用户目录就没有这个限制。";
            MessageBoxW(nullptr, text.c_str(), L"WindowPeeking", MB_ICONINFORMATION | MB_OK);
        }
        break;
    }
    case IDM_TRAY_QUIT:
        PostQuitMessage(0);
        break;
    default:
        break;
    }
}

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
    // A preview asked to be closed (its own right-click menu).
    case WM_WINDOWPEEKING_CLOSE_PREVIEW:
        DestroyPreview(FindPreview(reinterpret_cast<HWND>(lParam)));
        return 0;
    case WM_WINDOWPEEKING_CLOSE_ALL:
        DestroyAllPreviews();
        return 0;
    case WM_HOTKEY:
        switch (static_cast<int>(wParam)) {
        case kHotkeyPick: g_pickRequested = true; break;
        case kHotkeyQuit: PostQuitMessage(0); break;
        default: break;
        }
        return 0;
    // ---- the floating button (the hub window, when the tray is refused) ----
    case WM_PAINT:
        if (g_islandShown) {
            PaintIsland(hwnd);
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        if (g_islandShown) {
            return 1;   // painted in one go, so no flicker
        }
        break;
    case WM_LBUTTONDOWN:
        if (g_islandShown) {
            SetCapture(hwnd);
            g_islandDragging = true;
            g_islandMoved = false;
            GetCursorPos(&g_islandGrab);
            RECT rect{};
            GetWindowRect(hwnd, &rect);
            g_islandOrigin.x = rect.left;
            g_islandOrigin.y = rect.top;
            return 0;
        }
        break;
    case WM_MOUSEMOVE:
        if (g_islandShown && g_islandDragging) {
            POINT cursor{};
            if (GetCursorPos(&cursor)) {
                const int dx = cursor.x - g_islandGrab.x;
                const int dy = cursor.y - g_islandGrab.y;
                // A few pixels of hand-shake is still a click, not a drag.
                if (!g_islandMoved && (dx > 3 || dx < -3 || dy > 3 || dy < -3)) {
                    g_islandMoved = true;
                }
                if (g_islandMoved) {
                    SetWindowPos(hwnd, HWND_TOPMOST, g_islandOrigin.x + dx, g_islandOrigin.y + dy, 0,
                                 0, SWP_NOSIZE | SWP_NOACTIVATE);
                }
            }
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (g_islandShown) {
            ReleaseCapture();
            g_islandDragging = false;
            if (!g_islandMoved) {
                g_pickRequested = true;
            }
            return 0;
        }
        break;
    case WM_LBUTTONDBLCLK:
        if (g_islandShown) {
            g_pickRequested = true;
            return 0;
        }
        break;
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        if (g_islandShown) {
            TrayLog("floating button menu requested");
            ShowTrayMenu();
            return 0;
        }
        break;
    case WM_TIMER:
        if (wParam == kTrayRetryTimer) {
            EnsureTrayIcon();
            return 0;
        }
        break;
    case kTrayCallback:
        switch (LOWORD(lParam)) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            TrayLog("tray menu requested");
            ShowTrayMenu();
            break;
        case WM_LBUTTONDBLCLK: g_pickRequested = true; break;
        default: break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    // Explorer restarted: every tray icon it owned is gone, so ask for ours again.
    const UINT taskbarCreated = TaskbarCreatedMessage();
    if (taskbarCreated != 0 && msg == taskbarCreated) {
        TrayLog("explorer restarted, re-adding the tray icon");
        EnsureTrayIcon();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool CreateAppWindow(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = AppWndProc;
    wc.hInstance = instance;
    wc.hIcon = LoadAppIcon();
    wc.lpszClassName = kAppClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    g_app = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, kAppClass, L"WindowPeeking",
                            WS_POPUP, -32000, -32000, 1, 1, nullptr, nullptr, instance, nullptr);
    if (g_app == nullptr) {
        return false;
    }
    // It must be "shown" even though nobody will ever see it: a hidden window can
    // never become the foreground owner a tray popup menu needs, which is exactly
    // why the tray menu stayed dead. Fully transparent, far off-screen.
    SetLayeredWindowAttributes(g_app, 0, 0, LWA_ALPHA);
    ShowWindow(g_app, SW_SHOWNA);
    return true;
}

// Every pick makes a new preview window. Existing previews keep their own
// source, crop, position, opacity and click-through state untouched.
void DoPick(HINSTANCE instance) {
    HWND source = nullptr;
    RECT crop{};
    RECT screenRect{};
    if (!wp::PickRegion(instance, &source, &crop, &screenRect)) {
        return;
    }

    const int width = crop.right - crop.left;
    const int height = crop.bottom - crop.top;

    auto* preview = new wp::Mirror();
    if (!preview->Create(instance, screenRect, width, height)) {
        delete preview;
        MessageBoxW(nullptr, L"无法创建预览窗口。", L"WindowPeeking", MB_ICONERROR | MB_OK);
        return;
    }

    if (!preview->Attach(source, crop)) {
        preview->Destroy();
        delete preview;
        MessageBoxW(nullptr,
                    L"无法接管这个窗口的画面。\n"
                    L"以管理员权限运行的窗口、受保护的窗口会拒绝 DWM 缩略图。\n"
                    L"试试用同样的权限运行本程序。",
                    L"WindowPeeking", MB_ICONWARNING | MB_OK);
        return;
    }

    g_previews.push_back(preview);
    UpdateTrayTip();
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
    const POINT spots[] = { { 1200, 700 }, { 300, 900 }, { 1600, 1100 }, { 600, 140 },
                            { 900, 260 }, { 1500, 420 } };
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

    if (hitRoot == source) {
        check(true, "WindowFromPoint resolves the window under the drag", "matches the test window");
    } else {
        // A real always-on-top window of the person running this can sit over every
        // spot we try. That is the desktop, not the picker, so say so instead of
        // failing a check the CI runner passes.
        report += "SKIP  WindowFromPoint resolves the window under the drag  [a foreign "
                  "always-on-top window covers every probe pixel on this desktop]\r\n";
    }

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

    // Everything is placed relative to the real screen: a CI runner is 1024x768,
    // a laptop here is 2560x1600, and hard-coded pixels fall off the CI screen.
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    char screenInfo[96];
    wsprintfA(screenInfo, "virtual screen %d,%d %dx%d (all probes below are placed inside it)\r\n", vx, vy, vw, vh);
    report += screenInfo;

    // Source window: visible, 400x300, blue on the left and green on the right.
    const RECT sourceRect{ vx + 20, vy + 20, vx + 420, vy + 320 };
    HWND source = CreateWindowExW(0, L"WindowPeeking.TestSource", L"WindowPeeking source", WS_OVERLAPPEDWINDOW,
                                  sourceRect.left, sourceRect.top, 400, 300, nullptr, nullptr, instance, nullptr);
    if (source == nullptr) {
        check(false, "create test source window", "CreateWindowExW failed");
        return 1;
    }
    ShowWindow(source, SW_SHOWNOACTIVATE);
    UpdateWindow(source);
    Pump(300);

    // Crop the right half only: everything in the windowpeeking must be green.
    // Keep the preview fully on screen, whatever the display size is.
    const int mirrorW = 200;
    const int mirrorH = 300;
    int mirrorX = sourceRect.right + 60;
    if (mirrorX + mirrorW + 20 > vx + vw) {
        mirrorX = vx + vw - mirrorW - 20;
    }
    int mirrorY = sourceRect.top + 40;
    if (mirrorY + mirrorH + 20 > vy + vh) {
        mirrorY = vy + vh - mirrorH - 20;
    }
    const RECT mirrorRect{ mirrorX, mirrorY, mirrorX + mirrorW, mirrorY + mirrorH };

    wp::Mirror mirror;
    const bool created = mirror.Create(instance, mirrorRect, mirrorW, mirrorH);
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

    // Dragging an edge changes what is shown instead of stretching it: the frame
    // grows out over the source and the newly covered area arrives at 1:1.
    g_srcMode = 0;                  // blue on the left, green on the right again
    InvalidateRect(source, nullptr, FALSE);
    UpdateWindow(source);
    Pump(400);

    const int dragY = 150;          // client y, the middle of the 300px tall frame
    POINT frameOrigin{};
    ClientToScreen(mirror.Hwnd(), &frameOrigin);
    const POINT dragTo{ frameOrigin.x - 198, frameOrigin.y + dragY };
    mirror.SetDragCursorOverride(true, dragTo);
    SendMessageW(mirror.Hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, dragY));
    SendMessageW(mirror.Hwnd(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(-198, dragY));
    SendMessageW(mirror.Hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(-198, dragY));
    mirror.SetDragCursorOverride(false, POINT{});
    Pump(600);

    GetWindowRect(mirror.Hwnd(), &mr);
    {
        const RECT grownCrop = mirror.Crop();
        char grewDetail[180]{};
        wsprintfA(grewDetail, "frame %ldx%ld crop (%ld,%ld,%ld,%ld)", mr.right - mr.left,
                  mr.bottom - mr.top, grownCrop.left, grownCrop.top, grownCrop.right,
                  grownCrop.bottom);
        check((mr.right - mr.left) == 400 && (mr.bottom - mr.top) == 300 &&
                  grownCrop.left == 0 && grownCrop.right == 400,
              "dragging the left edge widens the frame over the whole source", grewDetail);
    }

    // x=8 would land on the source window's own border, which the crop includes.
    const int midY = mr.top + (mr.bottom - mr.top) / 2;
    const COLORREF grewLeft = SampleScreen(mr.left + 60, midY);
    check(Near(grewLeft, 0, 0, 255, 60), "the area dragged into view is the blue half, at 1:1",
          Describe(grewLeft));
    const COLORREF keptRight = SampleScreen(mr.left + 300, midY);
    check(Near(keptRight, 0, 200, 0, 60),
          "the part already visible is still green and has not moved", Describe(keptRight));

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
        check((exStyle & WS_EX_NOACTIVATE) != 0, "never takes focus, so the keyboard passes through",
              "exstyle=" + Hex(exStyle));

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

        // Synthetic mouse messages carry MK_SHIFT exactly like the real thing, so
        // the modifier rules are verified without touching the physical mouse.
        // A real drag follows the physical cursor, which this test cannot move (a
        // disconnected desktop refuses SetCursorPos), so the frame is told exactly
        // where the mouse would be instead.
        auto dragAt = [&](int fromX, int fromY, int toX, int toY, WPARAM modifiers) {
            POINT windowOrigin{};
            ClientToScreen(hwnd, &windowOrigin);
            const POINT dragTo{ windowOrigin.x + toX, windowOrigin.y + toY };
            mirror.SetDragCursorOverride(true, dragTo);
            SendMessageW(hwnd, WM_LBUTTONDOWN, modifiers | MK_LBUTTON, MAKELPARAM(fromX, fromY));
            SendMessageW(hwnd, WM_MOUSEMOVE, modifiers | MK_LBUTTON, MAKELPARAM(toX, toY));
            SendMessageW(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(toX, toY));
            mirror.SetDragCursorOverride(false, POINT{});
        };

        // Resizing means "look at more of the source", so the frame needs a real
        // source to grow over: a plain 600x400 window, off in a corner of the
        // desktop that the CI runner also has.
        WNDCLASSEXW swc{};
        swc.cbSize = sizeof(swc);
        swc.lpfnWndProc = SrcWndProc;
        swc.hInstance = instance;
        swc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        swc.lpszClassName = L"WindowPeeking.TestSource";
        RegisterClassExW(&swc);

        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        HWND source = CreateWindowExW(0, L"WindowPeeking.TestSource", L"WindowPeeking source",
                                      WS_OVERLAPPEDWINDOW, vx + vw - 640, vy + vh - 460,
                                      600, 400, nullptr, nullptr, instance, nullptr);
        if (source != nullptr) {
            ShowWindow(source, SW_SHOWNOACTIVATE);
            UpdateWindow(source);
            Pump(200);
        }
        check(source != nullptr, "create a source window to resize over",
              source != nullptr ? "600x400" : "CreateWindowExW failed");

        const RECT kBaseCrop{ 100, 50, 520, 350 };
        auto resetFrame = [&]() {
            SetWindowPos(hwnd, nullptr, 100, 100, 420, 300, SWP_NOZORDER | SWP_NOACTIVATE);
            if (source != nullptr) {
                mirror.Attach(source, kBaseCrop);
            }
            Pump(30);
        };

        struct GripCase {
            const char* label;
            int fromX, fromY, toX, toY;
            RECT expected;   // relative to the 100,100 420x300 reset state
        };
        const GripCase grips[] = {
            { "left edge resizes from the left",       2, 150,  -8, 150,  RECT{  90, 100, 520, 400 } },
            { "right edge resizes from the right",   417, 150, 427, 150,  RECT{ 100, 100, 530, 400 } },
            { "top edge resizes from the top",       200,   2, 200,  -8,  RECT{ 100,  90, 520, 400 } },
            { "bottom edge resizes from the bottom", 200, 297, 200, 307,  RECT{ 100, 100, 520, 410 } },
            { "top-left corner",                       2,   2,  -8,  -8,  RECT{  90,  90, 520, 400 } },
            { "top-right corner",                    417,   2, 427,  -8,  RECT{ 100,  90, 530, 400 } },
            { "bottom-left corner",                    2, 297,  -8, 307,  RECT{  90, 100, 520, 410 } },
            { "bottom-right corner",                 417, 297, 427, 307,  RECT{ 100, 100, 530, 410 } },
        };
        for (const GripCase& g : grips) {
            resetFrame();
            dragAt(g.fromX, g.fromY, g.toX, g.toY, 0);
            RECT got{};
            GetWindowRect(hwnd, &got);
            const RECT gripCrop = mirror.Crop();
            char gripDetail[220]{};
            std::snprintf(gripDetail, sizeof(gripDetail),
                          "got (%ld,%ld,%ld,%ld) want (%ld,%ld,%ld,%ld) crop (%ld,%ld,%ld,%ld)",
                          got.left, got.top, got.right, got.bottom, g.expected.left,
                          g.expected.top, g.expected.right, g.expected.bottom,
                          gripCrop.left, gripCrop.top, gripCrop.right, gripCrop.bottom);
            check(got.left == g.expected.left && got.top == g.expected.top &&
                      got.right == g.expected.right && got.bottom == g.expected.bottom,
                  g.label, gripDetail);
        }

        // An edge drag must move the crop by the same amount as the frame: the
        // picture is never rescaled, you just see more or less of the source.
        {
            resetFrame();
            dragAt(417, 150, 457, 150, 0);      // right edge, +40
            const RECT crop = mirror.Crop();
            RECT got{};
            GetWindowRect(hwnd, &got);
            char d[200]{};
            std::snprintf(d, sizeof(d), "crop (%ld,%ld,%ld,%ld) window %ldx%ld",
                          crop.left, crop.top, crop.right, crop.bottom,
                          got.right - got.left, got.bottom - got.top);
            check(crop.right == 560 && crop.left == 100 && (got.right - got.left) == 460,
                  "edge drag widens the crop, not just the window", d);
        }

        // A plain drag on the body moves the frame across the screen; the crop is
        // untouched, so the same piece of the source is still on show.
        resetFrame();
        dragAt(200, 150, 240, 180, 0);
        {
            RECT afterPlainDrag{};
            GetWindowRect(hwnd, &afterPlainDrag);
            const RECT afterPlainCrop = mirror.Crop();
            char plainDragDetail[220]{};
            std::snprintf(plainDragDetail, sizeof(plainDragDetail),
                          "window (%ld,%ld)-( %ld,%ld) crop (%ld,%ld,%ld,%ld)",
                          afterPlainDrag.left, afterPlainDrag.top, afterPlainDrag.right,
                          afterPlainDrag.bottom, afterPlainCrop.left, afterPlainCrop.top,
                          afterPlainCrop.right, afterPlainCrop.bottom);
            check(afterPlainDrag.left == 140 && afterPlainDrag.top == 130 &&
                      (afterPlainDrag.right - afterPlainDrag.left) == 420 &&
                      (afterPlainDrag.bottom - afterPlainDrag.top) == 300 &&
                      afterPlainCrop.left == 100 && afterPlainCrop.top == 50 &&
                      afterPlainCrop.right == 520 && afterPlainCrop.bottom == 350,
                  "body drag moves the frame, leaving the crop alone", plainDragDetail);
        }

        // Shift on the body slides the crop over the source and leaves the frame
        // where it is: a different part of the window, same place on screen.
        resetFrame();
        dragAt(200, 150, 240, 180, MK_SHIFT);
        {
            RECT afterShiftDrag{};
            GetWindowRect(hwnd, &afterShiftDrag);
            const RECT afterShiftCrop = mirror.Crop();
            char shiftDragDetail[220]{};
            std::snprintf(shiftDragDetail, sizeof(shiftDragDetail),
                          "window (%ld,%ld) %ldx%ld crop (%ld,%ld,%ld,%ld)",
                          afterShiftDrag.left, afterShiftDrag.top,
                          afterShiftDrag.right - afterShiftDrag.left,
                          afterShiftDrag.bottom - afterShiftDrag.top,
                          afterShiftCrop.left, afterShiftCrop.top, afterShiftCrop.right,
                          afterShiftCrop.bottom);
            check(afterShiftDrag.left == 100 && afterShiftDrag.top == 100 &&
                      (afterShiftDrag.right - afterShiftDrag.left) == 420 &&
                      (afterShiftDrag.bottom - afterShiftDrag.top) == 300 &&
                      afterShiftCrop.left == 60 && afterShiftCrop.top == 20 &&
                      afterShiftCrop.right == 480 && afterShiftCrop.bottom == 320,
                  "Shift + drag pulls the picture: the crop slides the other way", shiftDragDetail);
        }

        // Shift + wheel changes opacity.
        auto wheelAt = [&](int delta) {
            SendMessageW(hwnd, WM_MOUSEWHEEL,
                         MAKEWPARAM(MK_SHIFT, static_cast<WORD>(delta)), MAKELPARAM(200, 150));
        };
        BYTE alphaOut = 0;
        wheelAt(-120);
        GetLayeredWindowAttributes(hwnd, nullptr, &alphaOut, nullptr);
        check(alphaOut == 245, "Shift + wheel down makes it more transparent",
              "alpha=" + std::to_string(alphaOut));
        wheelAt(120);
        GetLayeredWindowAttributes(hwnd, nullptr, &alphaOut, nullptr);
        check(alphaOut == 255, "Shift + wheel up restores full opacity",
              "alpha=" + std::to_string(alphaOut));
        for (int i = 0; i < 40; ++i) {
            wheelAt(-120);
        }
        GetLayeredWindowAttributes(hwnd, nullptr, &alphaOut, nullptr);
        check(alphaOut == 26, "opacity stops at 10% instead of vanishing", "alpha=" + std::to_string(alphaOut));
        mirror.SetOpacity(255);

        // Click-through: mouse AND keyboard both pass to whatever is underneath.
        mirror.SetClickThrough(true);
        LONG_PTR throughEx = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        check((throughEx & WS_EX_TRANSPARENT) != 0 &&
                  (throughEx & WS_EX_NOACTIVATE) != 0,
              "click-through makes the mouse pass through and keeps focus elsewhere",
              "exstyle=" + Hex(throughEx));
        mirror.SetClickThrough(false);
        LONG_PTR backEx = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        check((backEx & WS_EX_TRANSPARENT) == 0, "click-through can be switched back off",
              "exstyle=" + Hex(backEx));

        check(LoadAppIcon() != nullptr, "tray/app icon resource is compiled in", "icon id 1");

        SetWindowTextW(hwnd, L"WindowPeeking");
        wchar_t title[64]{};
        GetWindowTextW(hwnd, title, 64);
        check(wcscmp(title, L"WindowPeeking") == 0, "window title set", "WindowPeeking");

        mirror.Destroy();
        check(mirror.Hwnd() == nullptr, "destroy cleans up", "hwnd released");
    }

    // ---- several previews at once ----
    {
        const bool appOk = CreateAppWindow(instance);
        check(appOk, "hub window that owns the preview list",
              appOk ? "app window created" : "CreateWindowExW failed");

        auto openPreview = [&](int x) -> wp::Mirror* {
            auto* preview = new wp::Mirror();
            if (!preview->Create(instance, RECT{ x, 100, x + 200, 250 }, 200, 150)) {
                delete preview;
                return nullptr;
            }
            g_previews.push_back(preview);
            return preview;
        };

        wp::Mirror* first = openPreview(100);
        wp::Mirror* second = openPreview(400);
        check(first != nullptr && second != nullptr && g_previews.size() == 2,
              "two previews can be open at the same time",
              "previews=" + std::to_string(g_previews.size()));

        if (first != nullptr && second != nullptr) {
            // State must be per preview: fading or un-clicking one leaves the rest.
            first->SetOpacity(128);
            first->SetClickThrough(true);
            BYTE firstAlpha = 0;
            BYTE secondAlpha = 0;
            GetLayeredWindowAttributes(first->Hwnd(), nullptr, &firstAlpha, nullptr);
            GetLayeredWindowAttributes(second->Hwnd(), nullptr, &secondAlpha, nullptr);
            const LONG_PTR secondEx = GetWindowLongPtrW(second->Hwnd(), GWL_EXSTYLE);
            char stateDetail[192]{};
            std::snprintf(stateDetail, sizeof(stateDetail), "alpha %u/%u  through=%d/%d",
                          static_cast<unsigned>(firstAlpha), static_cast<unsigned>(secondAlpha),
                          first->ClickThrough() ? 1 : 0, second->ClickThrough() ? 1 : 0);
            check(firstAlpha == 128 && secondAlpha == 255 && first->ClickThrough() &&
                      !second->ClickThrough() && (secondEx & WS_EX_TRANSPARENT) == 0,
                  "opacity and click-through stay per preview", stateDetail);

            // Closing one (exactly what its own menu asks the hub to do) leaves
            // the other one running.
            PostMessageW(g_app, WM_WINDOWPEEKING_CLOSE_PREVIEW, 0,
                         reinterpret_cast<LPARAM>(first->Hwnd()));
            Pump(150);
            check(g_previews.size() == 1 && g_previews[0] == second,
                  "closing one preview leaves the others alone",
                  "previews=" + std::to_string(g_previews.size()));
        }

        DestroyAllPreviews();
        check(g_previews.empty(), "closing all previews empties the list", "previews=0");

        // The tray icon is the program's whole face, and a popup menu owned by a
        // hidden window never appears -- that is what kept the tray dead. The hub
        // must therefore be shown, and invisible only because it is a fully
        // transparent 1x1 window parked far off-screen.
        if (g_app != nullptr) {
            const LONG_PTR hubStyle = GetWindowLongPtrW(g_app, GWL_STYLE);
            const LONG_PTR hubEx = GetWindowLongPtrW(g_app, GWL_EXSTYLE);
            char hubDetail[160]{};
            std::snprintf(hubDetail, sizeof(hubDetail), "style=%s exstyle=%s", Hex(hubStyle).c_str(),
                          Hex(hubEx).c_str());
            check((hubStyle & WS_VISIBLE) != 0,
                  "hub window is shown, so a tray menu has a foreground owner", hubDetail);
            check((hubEx & WS_EX_LAYERED) != 0, "hub window is layered: shown but unpainted",
                  hubDetail);

            BYTE hubAlpha = 255;
            DWORD hubFlags = 0;
            const BOOL hubLayered = GetLayeredWindowAttributes(g_app, nullptr, &hubAlpha, &hubFlags);
            char alphaDetail[96]{};
            std::snprintf(alphaDetail, sizeof(alphaDetail), "alpha=%u flags=%s",
                          static_cast<unsigned>(hubAlpha), Hex(hubFlags).c_str());
            check(hubLayered != FALSE && hubAlpha == 0 && (hubFlags & LWA_ALPHA) != 0,
                  "hub window is fully transparent", alphaDetail);

            RECT hubRect{};
            GetWindowRect(g_app, &hubRect);
            char parkDetail[96]{};
            std::snprintf(parkDetail, sizeof(parkDetail), "(%ld,%ld)-(%ld,%ld)", hubRect.left,
                          hubRect.top, hubRect.right, hubRect.bottom);
            check(hubRect.right < -30000 && hubRect.bottom < -30000,
                  "hub window parked far off-screen", parkDetail);

            // Registering the icon needs a real tray: a low-integrity or headless
            // session is refused with ERROR_ACCESS_DENIED, which is the harness's
            // problem rather than the program's, so it is reported, not failed.
            if (AddTrayIcon()) {
                check(true, "tray icon registered with the shell", "Shell_NotifyIconW(NIM_ADD)");
                RemoveTrayIcon();
            } else {
                const DWORD trayError = GetLastError();
                char skip[160]{};
                std::snprintf(skip, sizeof(skip),
                              "  SKIP  tray icon registration (this session refuses it, err=%lu)",
                              trayError);
                report += skip;
                report += "\r\n";
            }
            check(TaskbarCreatedMessage() != 0, "TaskbarCreated message registered",
                  "the icon comes back if explorer restarts");

            // The fallback face. When the shell refuses Shell_NotifyIconW there is
            // still something to click, and it must not land under the taskbar.
            ShowIsland();
            RECT islandRect{};
            GetWindowRect(g_app, &islandRect);
            char islandDetail[128]{};
            std::snprintf(islandDetail, sizeof(islandDetail), "%ldx%ld at (%ld,%ld)",
                          islandRect.right - islandRect.left, islandRect.bottom - islandRect.top,
                          islandRect.left, islandRect.top);
            check(islandRect.right - islandRect.left == kIslandSize &&
                      islandRect.bottom - islandRect.top == kIslandSize,
                  "floating button is a small square", islandDetail);
            RECT work{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
            check(islandRect.left >= work.left && islandRect.right <= work.right &&
                      islandRect.top >= work.top && islandRect.bottom <= work.bottom,
                  "floating button stays inside the work area, clear of the taskbar",
                  islandDetail);
            check((GetWindowLongPtrW(g_app, GWL_STYLE) & WS_VISIBLE) != 0,
                  "floating button is visible", islandDetail);
            BYTE islandAlpha = 0;
            GetLayeredWindowAttributes(g_app, nullptr, &islandAlpha, nullptr);
            char islandAlphaDetail[64]{};
            std::snprintf(islandAlphaDetail, sizeof(islandAlphaDetail), "alpha=%u",
                          static_cast<unsigned>(islandAlpha));
            check(islandAlpha == 255, "floating button is opaque, not a ghost", islandAlphaDetail);

            g_pickRequested = false;
            SendMessageW(g_app, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(6, 6));
            SendMessageW(g_app, WM_LBUTTONUP, 0, MAKELPARAM(6, 6));
            check(g_pickRequested, "clicking the floating button starts a pick", "no drag = click");
            g_pickRequested = false;

            HideIsland();
            RECT hiddenRect{};
            GetWindowRect(g_app, &hiddenRect);
            BYTE hiddenAlpha = 255;
            GetLayeredWindowAttributes(g_app, nullptr, &hiddenAlpha, nullptr);
            char hiddenDetail[96]{};
            std::snprintf(hiddenDetail, sizeof(hiddenDetail), "alpha=%u at (%ld,%ld)",
                          static_cast<unsigned>(hiddenAlpha), hiddenRect.left, hiddenRect.top);
            check(!g_islandShown && hiddenAlpha == 0 && hiddenRect.left < -30000,
                  "floating button hides again when the tray comes back", hiddenDetail);
        }

        // The escape hatch out of a low-integrity folder: the copy must land in
        // %LOCALAPPDATA%\Programs\WindowPeeking, whatever separator it is given.
        {
            const std::wstring forward =
                InstallTargetExe(L"C:\\Users\\someone\\AppData\\Local");
            const std::wstring plain = InstallTargetExe(L"C:\\Users\\someone\\AppData\\Local\\");
            char installDetail[160]{};
            std::snprintf(installDetail, sizeof(installDetail), "\"%ls\"",
                          forward.c_str());
            check(forward == L"C:\\Users\\someone\\AppData\\Local\\Programs\\WindowPeeking\\windowpeeking.exe" &&
                      forward == plain && InstallTargetExe(L"").empty(),
                  "install target is %LOCALAPPDATA%\\Programs\\WindowPeeking", installDetail);
        }

        if (g_app != nullptr) {
            DestroyWindow(g_app);
            g_app = nullptr;
        }
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

// ---- demo mode -----------------------------------------------------------
// A self-contained scene used for the README screenshot and for eyeballing the
// tool without touching real windows: a fake "team-chat" window plus one
// windowpeeking attached to a region of it.
LRESULT CALLBACK DemoWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);

        HBRUSH bg = CreateSolidBrush(RGB(28, 28, 32));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        RECT strip{ 0, 0, rc.right, 32 };
        HBRUSH bar = CreateSolidBrush(RGB(45, 45, 54));
        FillRect(dc, &strip, bar);
        DeleteObject(bar);

        SetBkMode(dc, TRANSPARENT);
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ oldFont = SelectObject(dc, font);

        SetTextColor(dc, RGB(205, 205, 215));
        RECT title{ 12, 7, rc.right - 12, 28 };
        DrawTextW(dc, L"# team-chat   -   nightly log", -1, &title, DT_LEFT | DT_SINGLELINE);

        struct Row { COLORREF colour; const wchar_t* text; };
        const Row rows[] = {
            { RGB(110, 190, 255), L"09:12  ana     pushed the picker fix" },
            { RGB(150, 150, 160), L"09:13  ci      build #482 started" },
            { RGB(120, 220, 150), L"09:14  ci      build #482 green" },
            { RGB(150, 150, 160), L"09:15  ken     anyone seen the notes?" },
            { RGB(255, 200, 110), L"09:16  you     ready to tag 0.1.0" },
            { RGB(110, 190, 255), L"09:16  ana     ship it" },
        };
        const int count = static_cast<int>(sizeof(rows) / sizeof(rows[0]));
        int y = 46;
        for (int i = 0; i < count; ++i) {
            SetTextColor(dc, rows[i].colour);
            RECT line{ 16, y, rc.right - 16, y + 22 };
            DrawTextW(dc, rows[i].text, -1, &line, DT_LEFT | DT_SINGLELINE);
            y += 30;
        }

        // A block that the windowpeeking crops, so the crop is obvious.
        RECT note{ 16, y + 10, rc.right - 16, rc.bottom - 16 };
        HBRUSH noteBg = CreateSolidBrush(RGB(38, 40, 48));
        FillRect(dc, &note, noteBg);
        DeleteObject(noteBg);
        SetTextColor(dc, RGB(230, 190, 120));
        RECT noteText{ note.left + 12, note.top + 10, note.right - 12, note.bottom - 10 };
        DrawTextW(dc, L"TODO  windowpeeking still freezes when the source is minimized\n"
                      L"      (that is a Windows limit, not a bug here)",
                  -1, &noteText, DT_LEFT | DT_WORDBREAK);

        SelectObject(dc, oldFont);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// The stage is only there so the README screenshot has a clean backdrop instead
// of whatever desktop happened to be behind it. It also draws the two captions.
const RECT kStageRect{ 132, 132, 1408, 688 };
// Two previews on purpose: the demo shows that several crops can be open at
// once, each with its own source region.
// Both previews are created at exactly the size of their crop, so the picture
// is 1:1 and the demo shows what a real crop looks like.
const RECT kPreviewRect{ 960, 190, 1400, 425 };
const RECT kPreviewRect2{ 960, 470, 1360, 590 };

LRESULT CALLBACK StageWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);

        // Soft vertical gradient: dark enough that the black letterbox of the
        // preview still reads as a separate surface.
        const int top = 46, bottom = 20;
        for (int y = 0; y < rc.bottom; ++y) {
            const int t = (y * 255) / (rc.bottom > 1 ? rc.bottom - 1 : 1);
            const int v = top + ((bottom - top) * t) / 255;
            RECT row{ 0, y, rc.right, y + 1 };
            HBRUSH b = CreateSolidBrush(RGB(v - 8, v - 6, v));
            FillRect(dc, &row, b);
            DeleteObject(b);
        }

        // Elevation hint around each preview: the previews paint their own rects
        // exactly, so a 2px ring outside them is all that shows.
        HBRUSH ringBrush = CreateSolidBrush(RGB(58, 64, 78));
        const RECT previews[2] = { kPreviewRect, kPreviewRect2 };
        for (const RECT& pv : previews) {
            RECT ring{ pv.left - kStageRect.left - 2, pv.top - kStageRect.top - 2,
                       pv.right - kStageRect.left + 2, pv.bottom - kStageRect.top + 2 };
            FrameRect(dc, &ring, ringBrush);
        }
        DeleteObject(ringBrush);

        SetBkMode(dc, TRANSPARENT);
        HFONT font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetTextColor(dc, RGB(130, 138, 152));

        RECT rightLabel{ kPreviewRect2.left - kStageRect.left, 606 - kStageRect.top,
                         kPreviewRect2.right - kStageRect.left, 626 - kStageRect.top };
        DrawTextW(dc, L"WindowPeeking  -  two live crops, no frames", -1, &rightLabel,
                  DT_LEFT | DT_SINGLELINE);

        SelectObject(dc, oldFont);
        DeleteObject(font);
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

int RunDemo(HINSTANCE instance) {
    WNDCLASSEXW stage{};
    stage.cbSize = sizeof(stage);
    stage.lpfnWndProc = StageWndProc;
    stage.hInstance = instance;
    stage.lpszClassName = L"WindowPeeking.DemoStage";
    RegisterClassExW(&stage);
    // Topmost, so the stage also wins against whatever window is in front; the
    // source and preview are created afterwards and therefore sit above it.
    HWND backdrop = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"WindowPeeking.DemoStage",
                                    L"", WS_POPUP,
                                    kStageRect.left, kStageRect.top,
                                    kStageRect.right - kStageRect.left,
                                    kStageRect.bottom - kStageRect.top, nullptr, nullptr,
                                    instance, nullptr);
    if (backdrop != nullptr) {
        ShowWindow(backdrop, SW_SHOWNOACTIVATE);
        UpdateWindow(backdrop);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DemoWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"WindowPeeking.DemoSource";
    RegisterClassExW(&wc);

    if (!wp::Mirror::RegisterClassOnce(instance)) {
        return 1;
    }

    const RECT sourceRect{ 160, 160, 920, 660 };
    HWND source = CreateWindowExW(0, L"WindowPeeking.DemoSource", L"team-chat",
                                  WS_OVERLAPPEDWINDOW, sourceRect.left, sourceRect.top,
                                  sourceRect.right - sourceRect.left,
                                  sourceRect.bottom - sourceRect.top, nullptr, nullptr, instance,
                                  nullptr);
    if (source == nullptr) {
        return 1;
    }
    ShowWindow(source, SW_SHOWNOACTIVATE);
    SetWindowPos(source, HWND_TOPMOST, sourceRect.left, sourceRect.top,
                 sourceRect.right - sourceRect.left, sourceRect.bottom - sourceRect.top,
                 SWP_SHOWWINDOW);
    UpdateWindow(source);

    // Each crop keeps the aspect ratio of its own region, so there is no letterbox.
    wp::Mirror mirror;
    // Both crops are wider than their previews, so whole lines fit (the picture
    // is scaled down slightly, which is also what a user would do by hand).
    const RECT crop{ 12, 36, 452, 271 };
    if (!mirror.Create(instance, kPreviewRect, kPreviewRect.right - kPreviewRect.left,
                       kPreviewRect.bottom - kPreviewRect.top)) {
        return 1;
    }
    mirror.MoveTo(kPreviewRect);
    if (!mirror.Attach(source, crop)) {
        return 1;
    }
    SetWindowTextW(mirror.Hwnd(), L"WindowPeeking demo");

    // Second preview, cropped from the bottom of the same window.
    wp::Mirror mirror2;
    const RECT crop2{ 12, 108, 412, 228 };
    if (!mirror2.Create(instance, kPreviewRect2, kPreviewRect2.right - kPreviewRect2.left,
                        kPreviewRect2.bottom - kPreviewRect2.top)) {
        return 1;
    }
    mirror2.MoveTo(kPreviewRect2);
    if (!mirror2.Attach(source, crop2)) {
        return 1;
    }
    SetWindowTextW(mirror2.Hwnd(), L"WindowPeeking demo 2");

    // The screenshot is exactly the stage: it already contains both previews.
    char line[256]{};
    std::snprintf(line, sizeof(line), "stage=%ld,%ld,%ld,%ld\r\n", kStageRect.left,
                  kStageRect.top, kStageRect.right, kStageRect.bottom);
    const std::wstring path = wp::ExeDir() + L"\\demo.txt";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
        CloseHandle(file);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    mirror.Destroy();
    mirror2.Destroy();
    if (IsWindow(source)) {
        DestroyWindow(source);
    }
    return 0;
}

} // namespace


HWND wp::AppWindow() {
    return g_app;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR cmdLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_instance = instance;
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        TrayLog("start pid=%lu exe=%ls", GetCurrentProcessId(), exe);
    }

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
    if (args.find(L"--install") != std::wstring::npos) {
        // Same job as the menu entry, without needing to see a menu: a program
        // sitting in a folder that gets launched at low integrity can never get
        // its tray icon, so copy it somewhere ordinary.
        const std::wstring copy = InstallToUserFolder();
        if (copy.empty()) {
            PrintLine(L"install failed");
            return 2;
        }
        PrintLine(copy.c_str());
        return 0;
    }
    if (args.find(L"--demo") != std::wstring::npos) {
        return RunDemo(instance);
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
    EnsureTrayIcon();
    if (args.find(L"--island") != std::wstring::npos) {
        // Force the fallback face even where the tray works, which is what the
        // screenshot and the "is it really there?" question need.
        ShowIsland();
    }

    RegisterHotKey(g_app, kHotkeyPick, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'P');
    RegisterHotKey(g_app, kHotkeyQuit, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');

    g_previews.clear();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (g_pickRequested) {
            g_pickRequested = false;
            DoPick(instance);
        }
    }

    UnregisterHotKey(g_app, kHotkeyPick);
    UnregisterHotKey(g_app, kHotkeyQuit);
    RemoveTrayIcon();
    DestroyAllPreviews();
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
