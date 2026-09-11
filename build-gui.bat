@echo off
setlocal
cd /d "%~dp0"

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
if "%VSPATH%"=="" (
    echo Could not find a Visual Studio install with the C++ workload. Install it, or edit this
    echo script to point VSPATH at your VS installation directory directly.
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x64

cl.exe /std:c++17 /EHsc /Zi /I. DLSS5ConverterGUI.cpp d3d12.lib dxgi.lib user32.lib gdi32.lib comdlg32.lib comctl32.lib gdiplus.lib shell32.lib /Fe:DLSS5ConverterGUI.exe /link /DEBUG /SUBSYSTEM:WINDOWS
