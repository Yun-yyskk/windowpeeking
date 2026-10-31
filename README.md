# WindowPeeking

**English** | [中文](README_zh.md) | ![build](https://github.com/Yun-yyskk/windowpeeking/actions/workflows/build.yml/badge.svg)

A borderless, always-on-top live view of any window region on Windows.
Peek at one part of another window while that window stays in the background.

![The window on the left stays in the background; only the region on the right is mirrored, live, with no title bar](docs/screenshot.png)

Built on the DWM thumbnail API, the same mechanism as PowerToys *Crop And Lock*
(Thumbnail mode), but with **no title bar and no frame** — just the pixels.

```
Ctrl+Alt+P   pick a window + drag out the region you want
Ctrl+Alt+O   toggle click-through (clicks pass to the window underneath)
Ctrl+Alt+Q   quit
```

- Drag the body to move the preview; drag an edge or corner to resize (eight
  directions, hand-rolled hit testing)
- Size is free-form; the crop is scaled to fit with letterboxing, so the aspect
  ratio of the source region is preserved
- Stays on top of everything, never appears in the taskbar or Alt-Tab

## Why not PowerToys Crop And Lock

PowerToys' thumbnail mode works well but keeps a normal window frame including
the title bar. If you want a small strip of a chat window or a video pinned in a
corner with nothing but pixels, that frame costs space and attention.
WindowPeeking drops it: `WS_POPUP`, no caption, no border, black letterbox.

## Build

Requires Windows 10/11 and a C++ toolchain (MSVC 2019+ / Build Tools with the
Windows SDK). No dependencies, no NuGet, no CMake.

```cmd
build.cmd            :: 67 KB, needs the Visual C++ runtime (VCRUNTIME140 / MSVCP140)
build.cmd release    :: 203 KB, self-contained - system DLLs only, no redist needed
```

Output: `build\windowpeeking.exe`. The release build links the C runtime
statically (`/MT /GL /LTCG`) and imports nothing but GDI32, USER32, KERNEL32 and
dwmapi, so it runs on a clean Windows install; the normal build is a third of the
size but wants the Microsoft Visual C++ Redistributable.

Self-tests (they create their own windows; nothing is installed). 29 checks cover
both build variants:

```cmd
build\windowpeeking.exe --selftest         :: borderless geometry, drag, resize, topmost
build\windowpeeking.exe --selftest-picker  :: selection maths and hit testing
build\windowpeeking.exe --selftest-e2e     :: real DWM thumbnail, verified by sampling pixels
build\windowpeeking.exe --demo             :: the scene in the screenshot above, for eyeballing
```

## Known limits

- **A minimized window is frozen.** Windows stops composing a window once it is
  minimized, so there is nothing live to mirror. Keep the source window on the
  same desktop, behind other windows — being *covered* is fine, being
  *minimized* is not.
- Exclusive full-screen applications (bypassing DWM) cannot be mirrored.
- Windows on another virtual desktop are cloaked by DWM and appear frozen.
- v0.1 has no tray icon: it starts silently, and `Ctrl+Alt+Q` removes every
  preview and exits.

## Credits

The DWM thumbnail parameters and the scale-to-fit maths are derived from
Microsoft PowerToys *Crop And Lock* (MIT). See `THIRD-PARTY-NOTICES.md`.

## License

MIT — see `LICENSE`.
