@echo off
rem WindowPeeking build script. Requires Visual Studio Build Tools (2022 or newer)
rem plus the Windows 10/11 SDK. Produces build\windowpeeking.exe.
setlocal
cd /d "%~dp0"

set "VCVARS="
for /d %%D in ("%ProgramFiles(x86)%\Microsoft Visual Studio\*" "%ProgramFiles%\Microsoft Visual Studio\*") do (
    for /d %%E in ("%%~D\*") do (
        if exist "%%~E\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%~E\VC\Auxiliary\Build\vcvars64.bat"
    )
)
if not defined VCVARS (
    echo [build] could not find vcvars64.bat - install Visual Studio Build Tools
    exit /b 1
)
echo [build] using %VCVARS%
call "%VCVARS%" >nul || exit /b 1

rem "build.cmd"          normal build, needs the MSVC redistributable runtime
rem "build.cmd release"  static C runtime, whole-program optimisation, smallest binary
set "RELEASE=0"
if /i "%~1"=="release" set "RELEASE=1"

if not exist build mkdir build

rem Icon + version resource. rc.exe ships with the Windows SDK that vcvars just set up.
rc /nologo /fo build\app.res app.rc
if errorlevel 1 exit /b 1
rem /utf-8 matters: the Chinese UI strings are UTF-8 source, and without it
rem MSVC reads them as the ANSI code page and silently mangles every literal.
set "CFLAGS=/nologo /std:c++17 /W4 /EHsc /utf-8 /DUNICODE /D_UNICODE /Fo:build\ /Fd:build\"
set "LFLAGS=/SUBSYSTEM:WINDOWS /OUT:build\windowpeeking.exe /MANIFEST:EMBED /MANIFESTINPUT:app.manifest build\app.res user32.lib gdi32.lib dwmapi.lib shell32.lib ole32.lib uuid.lib"

if "%RELEASE%"=="1" (
    rem /MT links the CRT in, /GL + /LTCG inline across translation units,
    rem /OPT:REF+ICF drop everything unreachable.
    set "CFLAGS=%CFLAGS% /O2 /MT /GL /Gy /DNDEBUG"
    set "LFLAGS=/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO %LFLAGS%"
) else (
    set "CFLAGS=%CFLAGS% /O2 /MD"
)

cl %CFLAGS% src\util.cpp src\mirror.cpp src\picker.cpp src\main.cpp /link %LFLAGS%
if errorlevel 1 exit /b 1

if "%RELEASE%"=="1" (echo [build] ok: build\windowpeeking.exe ^(release^)) else (echo [build] ok: build\windowpeeking.exe)
