# WindowPeeking

**English** | [中文](README_zh.md) | ![build](https://github.com/Yun-yyskk/windowpeeking/actions/workflows/build.yml/badge.svg)

A borderless, always-on-top live view of any window region on Windows.
Peek at one part of another window while that window stays in the background —
as many parts, of as many windows, as you like.

![The window on the left stays in the background; two regions of it are mirrored live on the right, with no title bars](docs/screenshot.png)

Built on the DWM thumbnail API, the same mechanism as PowerToys *Crop And Lock*
(Thumbnail mode), but with **no title bar and no frame** — just the pixels.

## Using it

**First run: double-click `install.cmd`.** It installs the program to
`%LOCALAPPDATA%\Programs\WindowPeeking`, adds a desktop shortcut and starts it from
there — which also sidesteps a Windows trap: an exe started out of this repository
folder runs at low integrity, and a low-integrity process may not register a tray
icon (see "No tray icon?" below). Already installed elsewhere? Just run the exe.

```
Ctrl+Alt+P   pick a window and drag out the region you want
Ctrl+Alt+Q   close every preview and quit
```

Once a preview exists, everything else is done with the mouse on the preview
itself and with the tray icon:

| Action | How |
| --- | --- |
| See more or less | Drag any edge or corner: the frame grows out over the source at 1:1, nothing is rescaled |
| Move on screen | Drag the body |
| Slide the view | Hold **Shift** and drag the body: same size, same place, a different part of the source |
| Opacity | Hold **Shift** and turn the mouse wheel (10%–100%) |
| Menu | Right-click: new preview, opacity presets, click-through, close this preview, close all, quit |
| Click-through | The menu item, or the tray icon — the mouse **and** the keyboard then go to the window underneath |
| Another preview | Press Ctrl+Alt+P again, right-click a preview, or double-click the tray icon — previews are independent (own window, own region, own position, opacity and click-through state) |
| Close all / quit | The tray icon (or the fallback button when Windows refuses one); its tooltip shows how many previews are open |

The preview is a **viewfinder**, not a scaled picture: whatever the frame covers
is what you see, one source pixel per preview pixel. Drag an edge to cover more
of the window, drag the body to move the frame across the screen, and Shift-drag
to slide the frame over the window without moving it on screen. Edges and
corners stop at the source window's own borders. The interface is in Chinese.

Other behaviour:

- The frame's size *is* the crop: resize it and you look at a bigger or smaller
  piece of the source, never at a rescaled copy of the same piece
- Stays on top of everything, never appears in the taskbar or Alt-Tab, and never
  steals keyboard focus even when it is not click-through
- Closing the source window leaves a hint in the preview instead of a dead black
  rectangle
- Single instance: starting it again just asks the running copy for a new preview

## Why not PowerToys Crop And Lock

PowerToys' thumbnail mode works well but keeps a normal window frame including
the title bar. If you want a small strip of a chat window or a video pinned in a
corner with nothing but pixels, that frame costs space and attention.
WindowPeeking drops it: `WS_POPUP`, no caption, no border, nothing but pixels.

## Build

Requires Windows 10/11 and a C++ toolchain (MSVC 2019+ / Build Tools with the
Windows SDK). No dependencies, no NuGet, no CMake.

```cmd
build.cmd            :: needs the Visual C++ runtime (VCRUNTIME140 / MSVCP140)
build.cmd release    :: self-contained - system DLLs only, no redist needed
```

Output: `build\windowpeeking.exe`. The release build links the C runtime
statically (`/MT /GL /LTCG`) and imports nothing but GDI32, USER32, KERNEL32 and
dwmapi, so it runs on a clean Windows install; the normal build is smaller but
wants the Microsoft Visual C++ Redistributable.

The icon and version resource are compiled from `app.rc`, so the executable also
carries a tray icon. Sources are UTF-8 and are built with `/utf-8`.

Self-tests (they create their own windows; nothing is installed):

```cmd
build\windowpeeking.exe --selftest         :: 48 checks: borderless geometry, edges and corners as a viewfinder, Shift-slide, wheel opacity, click-through, icon, tray owner window, several previews at once, the fallback button
build\windowpeeking.exe --selftest-picker  :: selection maths and hit testing
build\windowpeeking.exe --selftest-e2e     :: real DWM thumbnail, verified by sampling pixels -- including that dragging an edge in brings new source pixels into view instead of stretching the old ones
build\windowpeeking.exe --demo             :: the scene in the screenshot above, for eyeballing
```

## Known limits

- **A minimized window is frozen.** Windows stops composing a window once it is
  minimized, so there is nothing live to mirror. Keep the source window on the
  same desktop, behind other windows — being *covered* is fine, being
  *minimized* is not.
- Exclusive full-screen applications (bypassing DWM) cannot be mirrored.
- Windows on another virtual desktop are cloaked by DWM and appear frozen.
- While click-through is on, the preview ignores the mouse completely, so use the
  tray icon to switch it back off.
- Opacity stops at 10%; below that the preview would be impossible to find again.
- On Windows 11 the tray icon starts out folded into the "show hidden icons"
  overflow; drag it onto the taskbar to keep it there.
- **Do not run the exe out of this repository folder.** Windows treats programs in it as
  low-integrity, and a low-integrity process may not register a tray icon — double-clicking
  `build\windowpeeking.exe` here gives you no icon. Run `install.cmd` once instead
  (or copy the exe to the Desktop), see the next section.

## No tray icon?

The tray is the permanent entry point, but registering an icon runs into a Windows rule:
explorer (the taskbar) is a medium-integrity process, and a process of lower integrity that
asks it to add an icon is simply refused (`NIM_ADD` returns error 5). Windows decides that
integrity level **from the folder the exe lives in**: anything launched out of this repository
folder is downgraded, no matter who double-clicks it.

The same `whoami.exe`, started by the same medium-integrity cmd:

| Started from | Integrity | `NIM_ADD` |
| --- | --- | --- |
| `C:\Windows\System32\whoami.exe` | medium (0x2000) | — |
| the same file copied into `build\` | low (0x1000) | `ok=0 err=5` refused |
| the same file copied into `%LOCALAPPDATA%\Programs\` | medium (0x2000) | `ok=1 err=0` accepted |

So there is exactly one cure: **move the exe to an ordinary folder and start it there.**

```
install.cmd            :: double-click: copies the exe to
                       :: %LOCALAPPDATA%\Programs\WindowPeeking, adds a desktop
                       :: shortcut, and starts it from there (tray icon works)
```

A low-integrity process cannot even create a directory in your profile
(`CreateDirectory` returns error 5), which is why the copy has to be done by Explorer —
that is what double-clicking `install.cmd` gives you. The program's own menu offers the same
thing ("install to your user folder"); it only succeeds from an instance that is already fine,
and it explains itself when it fails.

If registration is still refused (system policy, security software), the program does not
wait and paints a 44×44 **floating button** just above the bottom-right corner of the work area:

| Click | What happens |
| --- | --- |
| Left click / double click | New preview (same as Ctrl+Alt+P) |
| Right click | The *same* menu as the tray icon: new preview, click-through, close all, quit |
| Drag | Move it anywhere; it returns to the corner next launch |

A small green dot appears in its corner while previews are open. If the icon did register,
the button never appears — you get one or the other, never both.

To see it on purpose: `build\windowpeeking.exe --island`.

<img src="docs/fallback-button.png" width="260" alt="the floating fallback button">

The program also keeps a log, so you can tell which of the two happened:

```
%LOCALAPPDATA%\WindowPeeking\tray.log      or tray.log beside the exe if that folder is refused
```

Every line is timestamped: `start`, `NIM_ADD ... ok=0 err=5`, `access denied ...`,
`tray unavailable, showing the floating button at (x,y)`, `explorer restarted`. Windows 11
also folds new icons into "show hidden icons" (^) by default, so a registered icon is simply
not on the taskbar until you drag it there.

And the preview's own right-click menu does everything too — new preview, opacity,
click-through, close this preview, quit — so a missing entry point is inconvenient, not fatal.

## Credits

The DWM thumbnail parameters and the scale-to-fit maths are derived from
Microsoft PowerToys *Crop And Lock* (MIT). See `THIRD-PARTY-NOTICES.md`.

## License

MIT — see `LICENSE`.
