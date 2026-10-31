# Third-party notices

## Microsoft PowerToys — Crop And Lock

WindowPeeking reimplements the thumbnail-cropping idea from PowerToys' Crop And Lock
module and reuses its DWM thumbnail parameters and scale-to-fit arithmetic.
The upstream sources used as reference are kept in `reference-upstream/` and are
not part of the build.

- Source: https://github.com/microsoft/PowerToys
- Module path: `src/modules/CropAndLock`
- License: MIT

```
MIT License

Copyright (c) Microsoft Corporation.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Windows APIs

Only APIs shipped with Windows are used: `dwmapi.dll` (DWM thumbnails) and the
user32/gdi32 primitives.
