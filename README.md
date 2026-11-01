# WindowPeeking

**English** | [中文](README_zh.md) | ![build](https://github.com/Yun-yyskk/windowpeeking/actions/workflows/build.yml/badge.svg)

A borderless, always-on-top live view of any window region on Windows.
Peek at one part of another window while that window stays in the background —
as many parts, of as many windows, as you like.

![The window on the left stays in the background; two regions of it are mirrored live on the right, with no title bars](docs/screenshot.png)

Built on the DWM thumbnail API, the same mechanism as PowerToys *Crop And Lock*
(Thumbnail mode), but with **no title bar and no frame** — just the pixels.

## Using it

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
| Close all / quit | Tray icon; the tooltip shows how many previews are open |

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
build\windowpeeking.exe --selftest         :: 36 checks: borderless geometry, edges and corners as a viewfinder, Shift-slide, wheel opacity, click-through, icon, several previews at once
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

## Credits

The DWM thumbnail parameters and the scale-to-fit maths are derived from
Microsoft PowerToys *Crop And Lock* (MIT). See `THIRD-PARTY-NOTICES.md`.

## License

MIT — see `LICENSE`.
