// DLSS5 Converter -- GUI edition.
//
// Same engine-driving logic as DLSS5Converter.cpp (the CLI tool): runs a photo or video through
// the real, already-fixed dlss5-neural.addon64 via the real ReShade dxgi.dll proxy. This file
// wraps that in an actual window instead of a console: drag-and-drop or Choose File, an Intensity
// slider, a Convert button, a live log, and a preview of the result. The conversion itself runs
// on a worker thread (it creates its own separate D3D12/ReShade window to be hooked into, same as
// the CLI tool) so the GUI thread stays responsive; progress is marshalled back via PostMessage,
// since only the thread that created a window may touch its controls.
//
// Same requirements as the CLI tool: dxgi.dll, ReShade.ini, dlss5-neural.addon64,
// dlssnr_amd_pass1.dll, dlssnr_on_amd_weights.bin, dlssnr_on_amd.ini next to this exe; ffmpeg.exe
// on PATH; run from a real interactive desktop (Session 0 breaks swap chain creation).

#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <gdiplus.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")

// Pulls in the modern (themed) common-controls implementation at link time -- without this,
// buttons/edit boxes/the trackbar render in the ancient Windows 2000 raised-3D style regardless
// of what OS this actually runs on. No separate .manifest file or resource-compiler step needed.
#pragma comment(linker, \
    "\"/manifestdependency:type='Win32' name='Microsoft.Windows.Common-Controls' "\
    "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using Microsoft::WRL::ComPtr;

namespace {

const char* kName = "DLSS5 Converter";

// ---------------------------------------------------------------------------------------------
// GUI plumbing: control IDs, the log/preview relay from the worker thread, GDI+.
// ---------------------------------------------------------------------------------------------

enum {
    IdChoose = 1001,
    IdConvert = 1002,
    IdIntensitySlider = 1003,
    IdIntensityLabel = 1004,
    IdFilePath = 1005,
    IdLog = 1006,
    IdStatus = 1007,
    IdProgress = 1008,
    IdEncoderCombo = 1009,
};
constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_DONE = WM_APP + 2;
constexpr UINT_PTR IdDoneAnimTimer = 1;

HWND g_hwnd = nullptr;
HWND g_editLog = nullptr, g_editPath = nullptr, g_lblIntensity = nullptr, g_lblStatus = nullptr;
HWND g_btnChoose = nullptr, g_btnConvert = nullptr, g_slider = nullptr, g_progress = nullptr;
HWND g_comboEncoder = nullptr;
std::string g_inputPath;
// The add-on itself always looks for dlss5-neural.ini/.log next to its own DLL (i.e. next to
// this exe), via GetModuleFileNameW on its own module handle -- not relative to whatever the
// current working directory happens to be. The file-open dialog silently changes the process's
// CWD to match wherever the user last browsed, which used to scatter our own temp files into
// that folder instead of next to the exe (and meant SeedAddonSettings() below was writing an ini
// the add-on would never actually find). Every one of our own temp/settings paths is anchored to
// this instead of a bare relative filename.
std::string g_exeDir;
float g_intensity = 0.08f;
// 0 = libx264 balanced (default), 1 = libx264 fast, 2 = h264_amf (AMD GPU hardware encode).
// Only used for video conversions -- ConvertPhoto never re-encodes anything.
int g_encoderIndex = 0;
// Probed once at startup (see CheckAmfEncoder). If the selected encoder is the AMD hardware one
// and this is false, ProcessVideo logs a warning and falls back to the fast CPU preset instead of
// just failing partway through the re-encode step.
bool g_amfAvailable = false;
bool g_busy = false;
Gdiplus::Bitmap* g_preview = nullptr;
CRITICAL_SECTION g_previewLock;

// ---------------------------------------------------------------------------------------------
// "Done" reveal animation -- a diagonal wipe (with a small diamond riding the seam) that sweeps
// the preview card left-to-right when a conversion finishes, plus a matching fill bar under the
// status line so video conversions (no preview image to reveal) still get a visible payoff.
// Driven by a plain WM_TIMER rather than anything fancier since one ~0.6s sweep at a time is all
// this ever needs to do.
// ---------------------------------------------------------------------------------------------
bool g_doneAnim = false;
DWORD g_doneAnimStart = 0;
constexpr DWORD kDoneAnimMs = 900;
constexpr float kWipeAngleDeg = 12.0f;
const RECT kDoneBarRect = { 20, 390, 680, 398 }; // same slot g_progress's marquee bar sits in

// ---------------------------------------------------------------------------------------------
// Theme: a light, card-based layout instead of a bare gray dialog. Cards are drawn as rounded
// rectangles directly onto the window background in WM_PAINT; the actual controls (still normal
// Win32 child windows -- nothing here is owner-drawn except the Convert button) sit visually
// inside them. Coordinates are all client-area pixels for a fixed 700x876 window.
// ---------------------------------------------------------------------------------------------

constexpr int kWinW = 700, kWinH = 876;
const RECT kCardInput    = { 20,  86, 680, 174 };
const RECT kCardSettings = { 20, 188, 680, 324 };
const RECT kCardLog      = { 20, 414, 680, 518 };
const RECT kCardPreview  = { 20, 532, 680, 852 };
RECT g_previewRect = { kCardPreview.left + 10, kCardPreview.top + 10,
                        kCardPreview.right - 10, kCardPreview.bottom - 10 };

constexpr COLORREF kColBg         = RGB(244, 244, 247);
constexpr COLORREF kColCard       = RGB(255, 255, 255);
constexpr COLORREF kColCardEdge   = RGB(226, 226, 232);
constexpr COLORREF kColText       = RGB(26, 26, 31);
constexpr COLORREF kColTextMuted  = RGB(114, 114, 124);
constexpr COLORREF kColAccent     = RGB(124, 92, 252);
constexpr COLORREF kColAccentDark = RGB(99, 70, 226);
constexpr COLORREF kColAccentDis  = RGB(206, 197, 245);

HFONT g_fontTitle = nullptr, g_fontSubtitle = nullptr, g_fontBody = nullptr, g_fontBold = nullptr,
      g_fontButton = nullptr;
HICON g_appIcon = nullptr;
HBRUSH g_brushBg = nullptr, g_brushCard = nullptr;

void CreateThemeFonts() {
    g_fontTitle = CreateFontA(-26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, "Segoe UI");
    g_fontSubtitle = CreateFontA(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH, "Segoe UI");
    g_fontBody = CreateFontA(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, "Segoe UI");
    g_fontBold = CreateFontA(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH, "Segoe UI");
    g_fontButton = CreateFontA(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, "Segoe UI");
}

// A small procedural icon (no image asset in this repo to draw from): a rounded accent-colour
// square with a simple white spark/star mark, rendered via GDI+ and converted to an HICON.
HICON MakeAppIcon() {
    const int size = 32;
    Gdiplus::Bitmap bmp(size, size, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush accent(Gdiplus::Color(255, GetRValue(kColAccent), GetGValue(kColAccent),
                                               GetBValue(kColAccent)));
    Gdiplus::GraphicsPath path;
    const float r = 8.0f, w = (float)size, h = (float)size;
    path.AddArc(0.0f, 0.0f, r * 2, r * 2, 180.0f, 90.0f);
    path.AddArc(w - r * 2, 0.0f, r * 2, r * 2, 270.0f, 90.0f);
    path.AddArc(w - r * 2, h - r * 2, r * 2, r * 2, 0.0f, 90.0f);
    path.AddArc(0.0f, h - r * 2, r * 2, r * 2, 90.0f, 90.0f);
    path.CloseFigure();
    g.FillPath(&accent, &path);
    Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::PointF star[10] = {
        {16, 5}, {18.5f, 12.5f}, {26, 13.5f}, {20, 18.5f}, {22, 26},
        {16, 21.5f}, {10, 26}, {12, 18.5f}, {6, 13.5f}, {13.5f, 12.5f},
    };
    g.FillPolygon(&white, star, 10);
    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

// Logs to both the console (useful when run from a terminal) and the GUI's log box. Every
// printf(...) call in the processing code below is routed through this via the macro further
// down, so status text shows up in both places without touching each call site.
void GuiLog(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    fputs(buf, stdout);
    if (g_hwnd) PostMessageA(g_hwnd, WM_APP_LOG, 0, (LPARAM)_strdup(buf));
}
#define printf(...) GuiLog(__VA_ARGS__)

void SetPreview(const std::string& imagePath) {
    std::wstring w(imagePath.begin(), imagePath.end());
    auto* bmp = Gdiplus::Bitmap::FromFile(w.c_str());
    EnterCriticalSection(&g_previewLock);
    delete g_preview;
    g_preview = (bmp && bmp->GetLastStatus() == Gdiplus::Ok) ? bmp : nullptr;
    if (!g_preview) delete bmp;
    LeaveCriticalSection(&g_previewLock);
    if (g_hwnd) InvalidateRect(g_hwnd, &g_previewRect, TRUE);
}

// ---------------------------------------------------------------------------------------------
// Path/ffmpeg/BMP utilities -- unchanged from the CLI tool.
// ---------------------------------------------------------------------------------------------

std::string ToLower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string ExtOf(const std::string& path) {
    auto pos = path.find_last_of('.');
    return pos == std::string::npos ? "" : ToLower(path.substr(pos));
}
std::string StemOf(const std::string& path) {
    auto slash = path.find_last_of("\\/");
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return path;
    return path.substr(0, dot);
}
bool IsVideoExt(const std::string& ext) {
    static const char* kVideo[] = { ".mp4", ".mov", ".mkv", ".avi", ".webm", ".wmv", ".flv", ".m4v" };
    for (auto v : kVideo) if (ext == v) return true;
    return false;
}
bool FileExists(const std::string& path) {
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

int RunCommand(const std::string& cmdline) {
    std::string full = "cmd.exe /c " + cmdline;
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::vector<char> buf(full.begin(), full.end());
    buf.push_back('\0');
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        printf("%s: could not launch: %s (error %lu)\n", kName, cmdline.c_str(), GetLastError());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

// ---------------------------------------------------------------------------------------------
// Dependency check, run once at startup. ffmpeg is the one dependency that is actually safe and
// practical to fetch automatically -- a normal, freely redistributable open-source build with a
// stable download URL -- and it is only ever fetched after asking, never silently. Everything
// else (ReShade's dxgi.dll, the add-on, and especially the closed-source engine runtime) either
// needs an interactive installer or is something this tool has no rights to fetch on your behalf
// (the engine runtime is Discord-gated, not something with a stable public URL); those just get a
// specific, actionable report in the log of what is missing and where to get it, instead of a
// confusing failure partway through a conversion.
// ---------------------------------------------------------------------------------------------

void PrependToPath(const std::string& dir) {
    char buf[32768] = {};
    DWORD len = GetEnvironmentVariableA("PATH", buf, sizeof(buf));
    std::string newPath = dir + ";" + (len > 0 ? std::string(buf, len) : "");
    SetEnvironmentVariableA("PATH", newPath.c_str());
}

bool CheckFfmpegOnPath() {
    return RunCommand("where ffmpeg >nul 2>nul") == 0 && RunCommand("where ffprobe >nul 2>nul") == 0;
}

// Whether this ffmpeg build has AMD's hardware H.264 encoder. Only meaningful once ffmpeg itself
// is confirmed on PATH -- callers check that first. A fresh or stripped-down driver install can
// be missing AMF even on a real AMD GPU, so this is a runtime probe rather than an assumption.
bool CheckAmfEncoder() {
    return RunCommand("ffmpeg -hide_banner -encoders 2>nul | findstr /I \"h264_amf\" >nul") == 0;
}

// Downloads gyan.dev's "essentials" static ffmpeg build into <exeDir>\ffmpeg-bin. Writes the
// download/extract logic to a temp .ps1 file and runs that, rather than trying to compose it as
// one inline command-line string -- passing a multi-step script through two more layers of shell
// quoting (this function's own cmd.exe wrapper, then powershell.exe's own -Command parsing) is
// exactly the kind of thing that breaks in some quoting edge case. A file sidesteps all of that.
bool DownloadFfmpeg() {
    const std::string binDir = g_exeDir + "ffmpeg-bin";
    printf("%s: downloading ffmpeg (about 80 MB, one time only)...\n", kName);

    const std::string scriptPath = g_exeDir + "dlss5convert_get_ffmpeg.ps1";
    FILE* f = fopen(scriptPath.c_str(), "w");
    if (!f) { printf("%s: could not write %s\n", kName, scriptPath.c_str()); return false; }
    fprintf(f,
        "$ErrorActionPreference = 'Stop'\n"
        "$zip = Join-Path $env:TEMP 'dlss5convert-ffmpeg.zip'\n"
        "$ex  = Join-Path $env:TEMP 'dlss5convert-ffmpeg-extract'\n"
        "Invoke-WebRequest -Uri 'https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip' -OutFile $zip\n"
        "Remove-Item -Recurse -Force $ex -ErrorAction SilentlyContinue\n"
        "Expand-Archive -Path $zip -DestinationPath $ex -Force\n"
        "New-Item -ItemType Directory -Force -Path '%s' | Out-Null\n"
        "$bin = Get-ChildItem $ex -Recurse -Filter ffmpeg.exe | Select-Object -First 1 -ExpandProperty DirectoryName\n"
        "Copy-Item (Join-Path $bin 'ffmpeg.exe') '%s' -Force\n"
        "Copy-Item (Join-Path $bin 'ffprobe.exe') '%s' -Force\n"
        "Remove-Item $zip, $ex -Recurse -Force -ErrorAction SilentlyContinue\n",
        binDir.c_str(), binDir.c_str(), binDir.c_str());
    fclose(f);

    RunCommand("powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath + "\"");
    remove(scriptPath.c_str());

    bool ok = FileExists(binDir + "\\ffmpeg.exe") && FileExists(binDir + "\\ffprobe.exe");
    if (ok) printf("%s: ffmpeg installed to %s\n", kName, binDir.c_str());
    else printf("%s: automatic ffmpeg install failed -- install it yourself and make sure it is "
                "on PATH (https://ffmpeg.org/download.html)\n", kName);
    return ok;
}

bool AskYesNo(const std::string& question) {
    std::string text = question + "\n\nThis downloads a file from ffmpeg.org's official build "
                        "host (gyan.dev) and saves it next to this exe.";
    return MessageBoxA(g_hwnd, text.c_str(), kName, MB_YESNO | MB_ICONQUESTION) == IDYES;
}

// Returns true only if everything needed to actually run a conversion is present. Unlike the CLI
// tool this does not stop the program -- the window still opens and shows what's missing in the
// log, since the user may be about to go fix it (drop a file in, install ReShade) with the window
// open in front of them.
bool CheckDependencies() {
    bool ffmpegOk = CheckFfmpegOnPath();
    const std::string localFfmpegBin = g_exeDir + "ffmpeg-bin";
    if (!ffmpegOk && FileExists(localFfmpegBin + "\\ffmpeg.exe") && FileExists(localFfmpegBin + "\\ffprobe.exe")) {
        PrependToPath(localFfmpegBin);
        ffmpegOk = true;
    }
    if (!ffmpegOk) {
        if (AskYesNo("ffmpeg was not found. Download a portable copy automatically?")) {
            if (DownloadFfmpeg()) {
                PrependToPath(localFfmpegBin);
                ffmpegOk = CheckFfmpegOnPath();
            }
        } else {
            printf("%s: ffmpeg is required and was not found. Install it yourself and put it on "
                   "PATH, or restart this app and say yes.\n", kName);
        }
    }

    g_amfAvailable = ffmpegOk && CheckAmfEncoder();
    if (ffmpegOk && !g_amfAvailable) {
        printf("%s: this ffmpeg build has no h264_amf (AMD hardware) encoder -- the GPU encoder "
               "option in Settings will fall back to CPU if selected.\n", kName);
    }

    std::vector<std::string> missing;
    auto need = [&](const char* file, const char* what) {
        if (!FileExists(g_exeDir + file)) missing.push_back(std::string(file) + " -- " + what);
    };
    need("dxgi.dll", "ReShade, add-on build, DirectX 12 target: https://reshade.me/");
    need("dlss5-neural.addon64", "build the 'neural' target from "
                                  "https://github.com/zmodelerlover/dlss5-neural-amd");
    need("dlssnr_amd_pass1.dll", "from that same project's Discord: https://discord.gg/wYhvS3JSHM");
    need("dlssnr_on_amd_weights.bin", "from that same project's Discord: https://discord.gg/wYhvS3JSHM");

    if (!missing.empty()) {
        printf("%s: missing dependencies --\n", kName);
        for (auto& m : missing) printf("%s:   %s\n", kName, m.c_str());
        printf("%s: see the README for full setup instructions. Converting will fail until "
               "these are in place.\n", kName);
    }
    return ffmpegOk && missing.empty();
}

bool ConvertToBmp(const std::string& input, const std::string& outputBmp) {
    // -pix_fmt bgr24 forces a plain 24-bit BMP regardless of the source's own format. Without
    // it, ffmpeg preserves an alpha channel when the source has one (any screenshot PNG, for
    // instance) and writes a 32-bit BMP instead, which LoadBmpAsRgba below rejects outright.
    std::string cmd = "ffmpeg -y -loglevel error -i \"" + input + "\" -pix_fmt bgr24 -frames:v 1 "
                       "-update 1 \"" + outputBmp + "\"";
    return RunCommand(cmd) == 0;
}
bool ConvertFromBmp(const std::string& inputBmp, const std::string& output) {
    std::string cmd = "ffmpeg -y -loglevel error -i \"" + inputBmp + "\" \"" + output + "\"";
    return RunCommand(cmd) == 0;
}

// Reads either a 24-bit (no alpha) or 32-bit (alpha ignored -- we always want opaque) uncompressed
// BMP, scanning the header for which one it actually is rather than assuming one. ffmpeg writes
// 24-bit for an opaque source and 32-bit when the source carries an alpha channel (any screenshot
// PNG, for instance) -- both are legitimate, and forcing ffmpeg's own output to 24-bit elsewhere
// in this file does not cover a BMP the user (or something else) handed us directly, unconverted.
bool LoadBmpAsRgba(const char* path, std::vector<uint8_t>& outPixels, UINT& outW, UINT& outH) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("%s: could not open %s\n", kName, path); return false; }
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    if (fread(&fh, sizeof(fh), 1, f) != 1 || fread(&ih, sizeof(ih), 1, f) != 1 || fh.bfType != 0x4D42) {
        printf("%s: %s is not a BMP\n", kName, path);
        fclose(f);
        return false;
    }
    const UINT bpp = ih.biBitCount;
    if ((bpp != 24 && bpp != 32) || ih.biCompression != BI_RGB) {
        printf("%s: %s is a %u-bit BMP (compression %u) -- only uncompressed 24-bit or 32-bit is "
               "supported\n", kName, path, bpp, ih.biCompression);
        fclose(f);
        return false;
    }
    const UINT bytesPerPixel = bpp / 8;
    const UINT w = (UINT)ih.biWidth;
    const UINT h = (UINT)(ih.biHeight >= 0 ? ih.biHeight : -ih.biHeight);
    const bool bottomUp = ih.biHeight > 0;
    const UINT srcRowBytes = (w * bytesPerPixel + 3) & ~3u;
    std::vector<uint8_t> row(srcRowBytes);
    outPixels.assign((size_t)w * h * 4, 255);
    fseek(f, fh.bfOffBits, SEEK_SET);
    for (UINT y = 0; y < h; y++) {
        if (fread(row.data(), 1, srcRowBytes, f) != srcRowBytes) {
            printf("%s: %s: short read at row %u\n", kName, path, y);
            fclose(f);
            return false;
        }
        const UINT destY = bottomUp ? (h - 1 - y) : y;
        uint8_t* dst = &outPixels[(size_t)destY * w * 4];
        for (UINT x = 0; x < w; x++) {
            const uint8_t* src = &row[x * bytesPerPixel];
            dst[x * 4 + 0] = src[2];
            dst[x * 4 + 1] = src[1];
            dst[x * 4 + 2] = src[0];
            dst[x * 4 + 3] = 255;
        }
    }
    fclose(f);
    outW = w; outH = h;
    return true;
}

bool WriteRgbaAsBmp(const std::vector<uint8_t>& pixels, UINT width, UINT height, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const UINT bmpRowBytes = (width * 3 + 3) & ~3u;
    const uint32_t pixelBytes = bmpRowBytes * height;
    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + pixelBytes;
    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof(BITMAPINFOHEADER);
    ih.biWidth = (LONG)width;
    ih.biHeight = (LONG)height;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = pixelBytes;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> row(bmpRowBytes, 0);
    for (UINT yy = 0; yy < height; yy++) {
        const uint8_t* src = &pixels[(size_t)(height - 1 - yy) * width * 4];
        for (UINT x = 0; x < width; x++) {
            row[x * 3 + 0] = src[x * 4 + 2];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 0];
        }
        fwrite(row.data(), 1, bmpRowBytes, f);
    }
    fclose(f);
    return true;
}

void SeedAddonSettings(float intensity) {
    std::string path = g_exeDir + "dlss5-neural.ini";
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { printf("%s: could not write %s\n", kName, path.c_str()); return; }
    fprintf(f, "[dlss5]\nIntensity=%.4g\nScale=1\nStructure=1\nSkin=1\nTone=1\nEncoding=0\n", intensity);
    fclose(f);
}

// ---------------------------------------------------------------------------------------------
// D3D12/DXGI plumbing -- unchanged from the CLI tool. This creates its OWN window (separate from
// the GUI window above) purely to be something ReShade can hook.
// ---------------------------------------------------------------------------------------------

LRESULT CALLBACK ProcWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hwnd, msg, wp, lp);
}

struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain1> swapchain1;
    ComPtr<IDXGISwapChain3> swapchain3;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12Resource> backBuffers[3];
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[3];
    D3D12_RESOURCE_STATES bbState[3] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
                                          D3D12_RESOURCE_STATE_COMMON };
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmdlist;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 1;
    HANDLE fenceEvent = nullptr;
    UINT w = 0, h = 0;

    void WaitGpu() {
        const UINT64 v = fenceValue++;
        queue->Signal(fence.Get(), v);
        if (fence->GetCompletedValue() < v) {
            fence->SetEventOnCompletion(v, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }
    }
};

bool SetupGpu(Gpu& gpu, UINT w, UINT h) {
    gpu.w = w; gpu.h = h;
    WNDCLASSA wc = {};
    wc.lpfnWndProc = ProcWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "Dlss5ConverterProcessingWindow";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("Dlss5ConverterProcessingWindow", "DLSS5 Converter -- processing",
                              WS_OVERLAPPEDWINDOW, 100, 100, (int)w, (int)h, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWMINIMIZED);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    SetActiveWindow(hwnd);
    BringWindowToTop(hwnd);

    ComPtr<IDXGIFactory4> preFactory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&preFactory));
    ComPtr<IDXGIAdapter1> chosenAdapter;
    for (UINT i = 0; ; i++) {
        ComPtr<IDXGIAdapter1> a;
        if (preFactory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 ad; a->GetDesc1(&ad);
        if (ad.VendorId == 0x1002 && !chosenAdapter) chosenAdapter = a;
    }

    if (FAILED(D3D12CreateDevice(chosenAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gpu.device))) ||
        !gpu.device) {
        printf("%s: D3D12CreateDevice failed\n", kName);
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC qdesc = {};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(gpu.device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&gpu.queue))) || !gpu.queue) {
        printf("%s: CreateCommandQueue failed\n", kName);
        return false;
    }

    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 scdesc = {};
    scdesc.Width = w; scdesc.Height = h;
    scdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scdesc.SampleDesc.Count = 1;
    scdesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scdesc.BufferCount = 3;
    scdesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT hrSc = factory->CreateSwapChainForHwnd(gpu.queue.Get(), hwnd, &scdesc, nullptr, nullptr, &gpu.swapchain1);
    if (FAILED(hrSc) || !gpu.swapchain1) {
        printf("%s: could not create a swap chain (0x%08lx). Run this from the desktop, not "
               "launched by an automated tool -- Session 0 breaks swap chain creation.\n", kName, hrSc);
        return false;
    }
    gpu.swapchain1.As(&gpu.swapchain3);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 3;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    gpu.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&gpu.rtvHeap));
    UINT rtvSize = gpu.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < 3; i++) {
        gpu.swapchain1->GetBuffer(i, IID_PPV_ARGS(&gpu.backBuffers[i]));
        gpu.rtvHandles[i] = gpu.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        gpu.rtvHandles[i].ptr += (SIZE_T)i * rtvSize;
        gpu.device->CreateRenderTargetView(gpu.backBuffers[i].Get(), nullptr, gpu.rtvHandles[i]);
    }

    gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.alloc));
    gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.alloc.Get(), nullptr,
                                   IID_PPV_ARGS(&gpu.cmdlist));
    gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence));
    gpu.fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    printf("%s: device/swap chain ready at %ux%u\n", kName, w, h);
    return true;
}

ComPtr<ID3D12Resource> CreateSourceTexture(Gpu& gpu) {
    D3D12_HEAP_PROPERTIES defaultHeap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC srcDesc = {};
    srcDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    srcDesc.Width = gpu.w; srcDesc.Height = gpu.h; srcDesc.DepthOrArraySize = 1; srcDesc.MipLevels = 1;
    srcDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srcDesc.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> srcTex;
    gpu.device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &srcDesc,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&srcTex));
    return srcTex;
}

void UploadFrame(Gpu& gpu, ID3D12Resource* srcTex, const std::vector<uint8_t>& pixels, bool firstUpload) {
    gpu.alloc->Reset();
    gpu.cmdlist->Reset(gpu.alloc.Get(), nullptr);
    if (!firstUpload) {
        D3D12_RESOURCE_BARRIER toDest = {};
        toDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toDest.Transition = { srcTex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                               D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST };
        gpu.cmdlist->ResourceBarrier(1, &toDest);
    }
    UINT64 rowPitch = (UINT64)gpu.w * 4;
    UINT64 alignedRowPitch = (rowPitch + 255) & ~255ULL;
    UINT64 uploadSize = alignedRowPitch * gpu.h;
    D3D12_HEAP_PROPERTIES uploadHeap = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = uploadSize; bufDesc.Height = 1; bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1; bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    gpu.device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    void* mapped = nullptr;
    upload->Map(0, nullptr, &mapped);
    for (UINT y = 0; y < gpu.h; y++)
        memcpy((uint8_t*)mapped + y * alignedRowPitch, &pixels[(size_t)y * gpu.w * 4], rowPitch);
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = srcTex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, gpu.w, gpu.h, 1, (UINT)alignedRowPitch };
    gpu.cmdlist->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER toSrc = {};
    toSrc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrc.Transition = { srcTex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE };
    gpu.cmdlist->ResourceBarrier(1, &toSrc);
    gpu.cmdlist->Close();
    ID3D12CommandList* lists[] = { gpu.cmdlist.Get() };
    gpu.queue->ExecuteCommandLists(1, lists);
    gpu.WaitGpu();
}

void TouchAndPresent(Gpu& gpu, ID3D12Resource* srcTex, int frameCounter) {
    gpu.alloc->Reset();
    gpu.cmdlist->Reset(gpu.alloc.Get(), nullptr);
    UINT bbIndex = gpu.swapchain3 ? gpu.swapchain3->GetCurrentBackBufferIndex() : (frameCounter % 3);

    D3D12_RESOURCE_BARRIER toDest = {};
    toDest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toDest.Transition = { gpu.backBuffers[bbIndex].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                           gpu.bbState[bbIndex], D3D12_RESOURCE_STATE_COPY_DEST };
    gpu.cmdlist->ResourceBarrier(1, &toDest);
    gpu.cmdlist->CopyResource(gpu.backBuffers[bbIndex].Get(), srcTex);
    D3D12_RESOURCE_BARRIER toPresent = toDest;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    gpu.cmdlist->ResourceBarrier(1, &toPresent);
    gpu.bbState[bbIndex] = D3D12_RESOURCE_STATE_PRESENT;

    gpu.cmdlist->Close();
    ID3D12CommandList* lists[] = { gpu.cmdlist.Get() };
    gpu.queue->ExecuteCommandLists(1, lists);
    gpu.swapchain1->Present(1, 0);
    gpu.WaitGpu();
}

bool ReadCurrentBackBuffer(Gpu& gpu, std::vector<uint8_t>& outPixels) {
    UINT lastIndex = gpu.swapchain3 ? gpu.swapchain3->GetCurrentBackBufferIndex() : 0;
    UINT readIndex = (lastIndex + 2) % 3;
    ID3D12Resource* tex = gpu.backBuffers[readIndex].Get();

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                              IID_PPV_ARGS(&list))))
        return false;
    const UINT rowPitch = (gpu.w * 4 + 255) & ~255u;
    const UINT64 size = (UINT64)rowPitch * gpu.h;
    D3D12_HEAP_PROPERTIES rb = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> raw;
    if (FAILED(gpu.device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &rd,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&raw))))
        return false;
    D3D12_RESOURCE_BARRIER toSrc = {};
    toSrc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrc.Transition = { tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT,
                          D3D12_RESOURCE_STATE_COPY_SOURCE };
    list->ResourceBarrier(1, &toSrc);
    D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
    from.pResource = tex;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = raw.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, gpu.w, gpu.h, 1, rowPitch };
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    D3D12_RESOURCE_BARRIER toPresent = toSrc;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &toPresent);
    list->Close();
    ID3D12CommandList* lists[] = { list.Get() };
    gpu.queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    if (FAILED(gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    gpu.queue->Signal(fence.Get(), 1);
    if (HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, 5000);
        CloseHandle(ev);
    }
    void* mapped = nullptr;
    D3D12_RANGE range = { 0, (SIZE_T)size };
    if (FAILED(raw->Map(0, &range, &mapped))) return false;
    const auto* bytes = (const uint8_t*)mapped;
    outPixels.resize((size_t)gpu.w * gpu.h * 4);
    for (UINT y = 0; y < gpu.h; y++)
        memcpy(&outPixels[(size_t)y * gpu.w * 4], bytes + (size_t)y * rowPitch, (size_t)gpu.w * 4);
    D3D12_RANGE empty = { 0, 0 };
    raw->Unmap(0, &empty);
    return true;
}

void WarmUp(Gpu& gpu, ID3D12Resource* srcTex) {
    const DWORD kWarmUpMs = 6000;
    DWORD start = GetTickCount();
    for (int frame = 0; GetTickCount() - start < kWarmUpMs; frame++) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        TouchAndPresent(gpu, srcTex, frame);
        if (frame == 10) {
            printf("%s: pressing Ctrl+End to enable the add-on\n", kName);
            keybd_event(VK_CONTROL, 0, 0, 0);
            keybd_event(VK_END, 0, 0, 0);
        } else if (frame == 15) {
            keybd_event(VK_END, 0, KEYEVENTF_KEYUP, 0);
            keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Image / video processing -- unchanged from the CLI tool.
// ---------------------------------------------------------------------------------------------

bool ProcessImage(const std::string& inputPath, const std::string& outputPath, float intensity) {
    std::string tempIn = g_exeDir + "dlss5convert_in.bmp";
    if (ExtOf(inputPath) != ".bmp") {
        if (!ConvertToBmp(inputPath, tempIn)) { printf("%s: could not read %s\n", kName, inputPath.c_str()); return false; }
    } else {
        tempIn = inputPath;
    }
    UINT w = 0, h = 0;
    std::vector<uint8_t> pixels;
    if (!LoadBmpAsRgba(tempIn.c_str(), pixels, w, h)) return false;
    printf("%s: %ux%u photo loaded\n", kName, w, h);

    Gpu gpu;
    if (!SetupGpu(gpu, w, h)) return false;
    auto srcTex = CreateSourceTexture(gpu);
    UploadFrame(gpu, srcTex.Get(), pixels, true);

    printf("%s: running the neural pass\n", kName);
    WarmUp(gpu, srcTex.Get());

    std::vector<uint8_t> result;
    if (!ReadCurrentBackBuffer(gpu, result)) { printf("%s: readback failed\n", kName); return false; }

    if (ExtOf(outputPath) == ".bmp") {
        WriteRgbaAsBmp(result, w, h, outputPath.c_str());
    } else {
        std::string tempOut = g_exeDir + "dlss5convert_out.bmp";
        WriteRgbaAsBmp(result, w, h, tempOut.c_str());
        ConvertFromBmp(tempOut, outputPath);
    }
    printf("%s: wrote %s\n", kName, outputPath.c_str());
    return true;
}

// Builds the "-c:v ..." tail of the re-encode command for one of the three Settings-card choices.
// Falls back to the fast CPU preset (rather than the slower "balanced" one, since a user picking
// the GPU option is presumably prioritizing speed) if AMD hardware was requested but this ffmpeg
// build doesn't actually have h264_amf -- see CheckAmfEncoder.
std::string EncoderFlags(int encoderIndex) {
    if (encoderIndex == 2 && !g_amfAvailable) {
        printf("%s: GPU (AMD hardware) encoder was selected but this ffmpeg build doesn't have "
               "h264_amf -- falling back to CPU fast preset.\n", kName);
        encoderIndex = 1;
    }
    switch (encoderIndex) {
    case 1:  return "-c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p";
    case 2:  return "-c:v h264_amf -quality quality -rc cqp -qp_i 18 -qp_p 20 -pix_fmt yuv420p";
    default: return "-c:v libx264 -preset medium -crf 18 -pix_fmt yuv420p";
    }
}

bool ProcessVideo(const std::string& inputPath, const std::string& outputPath, float intensity,
                   int encoderIndex) {
    std::string stem = StemOf(outputPath);
    std::string frameDir = stem + "_frames";
    std::string outFrameDir = stem + "_frames_out";
    CreateDirectoryA(frameDir.c_str(), nullptr);
    CreateDirectoryA(outFrameDir.c_str(), nullptr);

    printf("%s: extracting frames\n", kName);
    // -pix_fmt bgr24: see the comment on ConvertToBmp -- without it a source with an alpha
    // channel gets written as a 32-bit BMP, which LoadBmpAsRgba rejects.
    std::string extractCmd = "ffmpeg -y -loglevel error -i \"" + inputPath + "\" -pix_fmt bgr24 "
                              "-vsync 0 \"" + frameDir + "\\frame_%06d.bmp\"";
    if (RunCommand(extractCmd) != 0) { printf("%s: ffmpeg failed to extract frames\n", kName); return false; }

    std::string audioPath = stem + "_audio.m4a";
    std::string audioCmd = "ffmpeg -y -loglevel error -i \"" + inputPath + "\" -vn -acodec copy \"" + audioPath + "\"";
    bool haveAudio = RunCommand(audioCmd) == 0 && FileExists(audioPath);

    std::string fpsFile = stem + "_fps.txt";
    std::string fpsCmd = "ffprobe -v error -select_streams v:0 -of csv=p=0 -show_entries "
                          "stream=r_frame_rate \"" + inputPath + "\" > \"" + fpsFile + "\"";
    RunCommand(fpsCmd);
    std::string fps = "30";
    if (FILE* f = fopen(fpsFile.c_str(), "r")) {
        char buf[64] = {};
        if (fgets(buf, sizeof(buf), f)) {
            size_t len = strlen(buf);
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
            if (len > 0) fps = buf;
        }
        fclose(f);
    }
    printf("%s: source frame rate %s\n", kName, fps.c_str());

    int frameCount = 0;
    for (;; frameCount++) {
        char name[MAX_PATH];
        snprintf(name, sizeof(name), "%s\\frame_%06d.bmp", frameDir.c_str(), frameCount + 1);
        if (!FileExists(name)) break;
    }
    if (frameCount == 0) { printf("%s: no frames extracted, aborting\n", kName); return false; }
    printf("%s: %d frames extracted\n", kName, frameCount);

    char firstFrame[MAX_PATH];
    snprintf(firstFrame, sizeof(firstFrame), "%s\\frame_%06d.bmp", frameDir.c_str(), 1);
    UINT w = 0, h = 0;
    std::vector<uint8_t> firstPixels;
    if (!LoadBmpAsRgba(firstFrame, firstPixels, w, h)) return false;
    printf("%s: %ux%u frame resolution\n", kName, w, h);

    Gpu gpu;
    if (!SetupGpu(gpu, w, h)) return false;
    auto srcTex = CreateSourceTexture(gpu);
    UploadFrame(gpu, srcTex.Get(), firstPixels, true);

    printf("%s: warming up the neural pass on frame 1\n", kName);
    WarmUp(gpu, srcTex.Get());

    DWORD lastPrint = GetTickCount();
    for (int f = 1; f <= frameCount; f++) {
        std::vector<uint8_t> framePixels;
        UINT fw = 0, fh = 0;
        if (f == 1) {
            framePixels = firstPixels;
        } else {
            char inName[MAX_PATH];
            snprintf(inName, sizeof(inName), "%s\\frame_%06d.bmp", frameDir.c_str(), f);
            if (!LoadBmpAsRgba(inName, framePixels, fw, fh) || fw != w || fh != h) {
                printf("%s: frame %d: load failed or resolution mismatch, skipping\n", kName, f);
                continue;
            }
            UploadFrame(gpu, srcTex.Get(), framePixels, false);
            MSG msg;
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
            TouchAndPresent(gpu, srcTex.Get(), 1000 + f);
        }

        std::vector<uint8_t> result;
        if (!ReadCurrentBackBuffer(gpu, result)) {
            printf("%s: frame %d: readback failed, skipping\n", kName, f);
            continue;
        }
        char outName[MAX_PATH];
        snprintf(outName, sizeof(outName), "%s\\frame_%06d.bmp", outFrameDir.c_str(), f);
        WriteRgbaAsBmp(result, w, h, outName);

        if (GetTickCount() - lastPrint > 1000 || f == frameCount) {
            printf("%s: frame %d/%d\n", kName, f, frameCount);
            lastPrint = GetTickCount();
        }
    }

    printf("%s: re-encoding to %s\n", kName, outputPath.c_str());
    std::string encodeCmd = "ffmpeg -y -loglevel error -framerate " + fps + " -i \"" + outFrameDir +
                             "\\frame_%06d.bmp\"";
    if (haveAudio) encodeCmd += " -i \"" + audioPath + "\" -map 0:v -map 1:a -c:a aac -shortest";
    encodeCmd += " " + EncoderFlags(encoderIndex) + " \"" + outputPath + "\"";
    if (RunCommand(encodeCmd) != 0) { printf("%s: ffmpeg failed to re-encode the output video\n", kName); return false; }
    printf("%s: wrote %s\n", kName, outputPath.c_str());
    return true;
}

// ---------------------------------------------------------------------------------------------
// Worker thread: runs one conversion, then posts WM_APP_DONE back to the GUI thread.
// ---------------------------------------------------------------------------------------------

struct JobResult { bool ok; char outputPath[MAX_PATH]; bool isVideo; };

DWORD WINAPI ConvertThread(LPVOID param) {
    auto* result = new JobResult{};
    std::string inputPath = *(std::string*)param;
    delete (std::string*)param;

    std::string ext = ExtOf(inputPath);
    bool isVideo = IsVideoExt(ext);
    std::string outputPath = StemOf(inputPath) + "_dlss5" + (isVideo ? ".mp4" : ".png");

    printf("%s: input  %s (%s)\n", kName, inputPath.c_str(), isVideo ? "video" : "photo");
    printf("%s: output %s\n", kName, outputPath.c_str());

    remove((g_exeDir + "dlss5-neural.log").c_str());
    SeedAddonSettings(g_intensity);
    bool ok = isVideo ? ProcessVideo(inputPath, outputPath, g_intensity, g_encoderIndex)
                       : ProcessImage(inputPath, outputPath, g_intensity);

    result->ok = ok;
    result->isVideo = isVideo;
    strncpy_s(result->outputPath, outputPath.c_str(), MAX_PATH - 1);
    PostMessageA(g_hwnd, WM_APP_DONE, 0, (LPARAM)result);
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Main GUI window
// ---------------------------------------------------------------------------------------------

void SetStatus(const char* text) { SetWindowTextA(g_lblStatus, text); }

void StartConversion() {
    if (g_busy || g_inputPath.empty()) return;
    if (g_doneAnim) { g_doneAnim = false; KillTimer(g_hwnd, IdDoneAnimTimer); }
    g_busy = true;
    EnableWindow(g_btnConvert, FALSE);
    EnableWindow(g_btnChoose, FALSE);
    InvalidateRect(g_btnConvert, nullptr, TRUE);
    SetWindowTextA(g_editLog, "");
    SetStatus("Working...");
    ShowWindow(g_progress, SW_SHOW);
    SendMessageA(g_progress, PBM_SETMARQUEE, TRUE, 30);
    auto* param = new std::string(g_inputPath);
    CloseHandle(CreateThread(nullptr, 0, ConvertThread, param, 0, nullptr));
}

void SetInputPath(const std::string& path) {
    g_inputPath = path;
    SetWindowTextA(g_editPath, path.c_str());
    EnableWindow(g_btnConvert, TRUE);
    EnterCriticalSection(&g_previewLock);
    delete g_preview;
    g_preview = nullptr;
    LeaveCriticalSection(&g_previewLock);
    InvalidateRect(g_hwnd, &g_previewRect, TRUE);
}

LRESULT CALLBACK GuiWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        CreateThemeFonts();
        g_brushBg = CreateSolidBrush(kColBg);
        g_brushCard = CreateSolidBrush(kColCard);
        auto mk = [&](const char* cls, const char* text, int x, int y, int w, int h, DWORD style,
                      int id, HFONT fnt) {
            HWND c = CreateWindowExA(0, cls, text, style | WS_CHILD | WS_VISIBLE, x, y, w, h,
                                      hwnd, (HMENU)(INT_PTR)id, GetModuleHandle(nullptr), nullptr);
            SendMessageA(c, WM_SETFONT, (WPARAM)fnt, TRUE);
            return c;
        };

        // Card: input
        mk("STATIC", "Drag a photo or video onto this window, or click Choose File",
           kCardInput.left + 16, kCardInput.top + 12, 600, 20, SS_LEFT, 0, g_fontBody);
        g_btnChoose = mk("BUTTON", "Choose File...", kCardInput.left + 16, kCardInput.top + 42,
                          140, 32, BS_PUSHBUTTON, IdChoose, g_fontBody);
        g_editPath = mk("EDIT", "No file selected", kCardInput.left + 168, kCardInput.top + 46,
                         kCardInput.right - kCardInput.left - 168 - 16, 24,
                         ES_READONLY | ES_AUTOHSCROLL | WS_BORDER, IdFilePath, g_fontBody);

        // Card: settings
        mk("STATIC", "Intensity", kCardSettings.left + 16, kCardSettings.top + 12, 200, 20,
           SS_LEFT, 0, g_fontBold);
        g_slider = CreateWindowExA(0, TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                    kCardSettings.left + 16, kCardSettings.top + 36, 340, 30, hwnd,
                                    (HMENU)(INT_PTR)IdIntensitySlider, GetModuleHandle(nullptr), nullptr);
        SendMessageA(g_slider, WM_SETFONT, (WPARAM)g_fontBody, TRUE);
        SendMessageA(g_slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageA(g_slider, TBM_SETPOS, TRUE, (LPARAM)(int)(g_intensity * 100));
        SendMessageA(g_slider, TBM_SETTICFREQ, 10, 0);
        g_lblIntensity = mk("STATIC", "0.08  --  0.00 = original, 1.00 = full strength",
                             kCardSettings.left + 372, kCardSettings.top + 42,
                             kCardSettings.right - kCardSettings.left - 372 - 16, 20, SS_LEFT,
                             IdIntensityLabel, g_fontBody);

        // Video encoder -- only touches video conversions (ConvertPhoto never re-encodes), but
        // lives in the shared Settings card since it is a conversion-wide preference, not
        // something that only makes sense once a video is already loaded.
        mk("STATIC", "Video encoder", kCardSettings.left + 16, kCardSettings.top + 70, 200, 20,
           SS_LEFT, 0, g_fontBold);
        g_comboEncoder = CreateWindowExA(0, "COMBOBOX", "",
                                          WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                          kCardSettings.left + 16, kCardSettings.top + 94, 400, 200,
                                          hwnd, (HMENU)(INT_PTR)IdEncoderCombo,
                                          GetModuleHandle(nullptr), nullptr);
        SendMessageA(g_comboEncoder, WM_SETFONT, (WPARAM)g_fontBody, TRUE);
        SendMessageA(g_comboEncoder, CB_ADDSTRING, 0, (LPARAM)"CPU - balanced (x264, best quality)");
        SendMessageA(g_comboEncoder, CB_ADDSTRING, 0, (LPARAM)"CPU - fast (x264, larger file)");
        SendMessageA(g_comboEncoder, CB_ADDSTRING, 0,
                     (LPARAM)"GPU - AMD hardware (h264_amf, lightest on CPU/multitasking)");
        SendMessageA(g_comboEncoder, CB_SETCURSEL, g_encoderIndex, 0);

        // Convert + status + progress (sit directly on the window background, between cards)
        g_btnConvert = mk("BUTTON", "Convert", 20, 340, 160, 42,
                           BS_OWNERDRAW | BS_NOTIFY, IdConvert, g_fontButton);
        EnableWindow(g_btnConvert, FALSE);
        g_lblStatus = mk("STATIC", "Ready.", 196, 352, 464, 22, SS_LEFT, IdStatus, g_fontBody);
        g_progress = CreateWindowExA(0, PROGRESS_CLASSA, "", WS_CHILD | PBS_MARQUEE, 20, 390, 660,
                                      8, hwnd, (HMENU)(INT_PTR)IdProgress, GetModuleHandle(nullptr),
                                      nullptr);

        // Card: log
        mk("STATIC", "Log", kCardLog.left + 16, kCardLog.top + 10, 100, 20, SS_LEFT, 0, g_fontBold);
        g_editLog = mk("EDIT", "", kCardLog.left + 16, kCardLog.top + 34,
                        kCardLog.right - kCardLog.left - 32, kCardLog.bottom - kCardLog.top - 34 - 12,
                        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, IdLog,
                        g_fontBody);

        g_appIcon = MakeAppIcon();
        if (g_appIcon) {
            SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_appIcon);
            SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_appIcon);
        }

        DragAcceptFiles(hwnd, TRUE);
        InitializeCriticalSection(&g_previewLock);
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        char path[MAX_PATH];
        if (DragQueryFileA(drop, 0, path, MAX_PATH)) SetInputPath(path);
        DragFinish(drop);
        return 0;
    }
    case WM_HSCROLL: {
        if ((HWND)lp == g_slider) {
            int pos = (int)SendMessageA(g_slider, TBM_GETPOS, 0, 0);
            g_intensity = pos / 100.0f;
            char buf[96];
            snprintf(buf, sizeof(buf), "%.2f  --  0.00 = original, 1.00 = full strength", g_intensity);
            SetWindowTextA(g_lblIntensity, buf);
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        HWND ctl = (HWND)lp;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, GetDlgCtrlID(ctl) == IdStatus ? kColText : kColText);
        // Everything except the Convert-row status label sits on a white card; that one sits
        // directly on the window background.
        return (LRESULT)(GetDlgCtrlID(ctl) == IdStatus ? g_brushBg : g_brushCard);
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, kColCard);
        SetTextColor(hdc, kColText);
        return (LRESULT)g_brushCard;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IdChoose && HIWORD(wp) == BN_CLICKED) {
            char path[MAX_PATH] = {};
            OPENFILENAMEA ofn = { sizeof(ofn) };
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter =
                "Images and videos\0*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.mp4;*.mov;*.mkv;*.avi;*.webm\0"
                "All files\0*.*\0";
            ofn.lpstrFile = path;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            ofn.lpstrTitle = "Pick a photo or video to run through DLSS5";
            if (GetOpenFileNameA(&ofn)) SetInputPath(path);
        } else if (LOWORD(wp) == IdConvert && HIWORD(wp) == BN_CLICKED) {
            StartConversion();
        } else if (LOWORD(wp) == IdEncoderCombo && HIWORD(wp) == CBN_SELCHANGE) {
            g_encoderIndex = (int)SendMessageA(g_comboEncoder, CB_GETCURSEL, 0, 0);
        }
        return 0;
    case WM_DRAWITEM: {
        auto* dis = (DRAWITEMSTRUCT*)lp;
        if (dis->CtlID != IdConvert) break;
        Gdiplus::Graphics g(dis->hDC);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
        const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
        const COLORREF fill = disabled ? kColAccentDis : (pressed ? kColAccentDark : kColAccent);
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(fill), GetGValue(fill), GetBValue(fill)));
        const float w = (float)(dis->rcItem.right - dis->rcItem.left);
        const float h = (float)(dis->rcItem.bottom - dis->rcItem.top);
        const float r = 8.0f;
        Gdiplus::GraphicsPath path;
        path.AddArc(0.0f, 0.0f, r * 2, r * 2, 180.0f, 90.0f);
        path.AddArc(w - r * 2, 0.0f, r * 2, r * 2, 270.0f, 90.0f);
        path.AddArc(w - r * 2, h - r * 2, r * 2, r * 2, 0.0f, 90.0f);
        path.AddArc(0.0f, h - r * 2, r * 2, r * 2, 90.0f, 90.0f);
        path.CloseFigure();
        g.FillPath(&brush, &path);

        char text[64];
        GetWindowTextA(dis->hwndItem, text, sizeof(text));
        std::wstring wtext(text, text + strlen(text));
        Gdiplus::Font font(L"Segoe UI", 11, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
        Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 255, 255, 255));
        Gdiplus::StringFormat fmt;
        fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::RectF rect(0, 0, w, h);
        g.DrawString(wtext.c_str(), -1, &font, rect, &fmt, &textBrush);
        return TRUE;
    }
    case WM_APP_LOG: {
        char* text = (char*)lp;
        int len = GetWindowTextLengthA(g_editLog);
        SendMessageA(g_editLog, EM_SETSEL, len, len);
        SendMessageA(g_editLog, EM_REPLACESEL, FALSE, (LPARAM)text);
        free(text);
        return 0;
    }
    case WM_APP_DONE: {
        auto* result = (JobResult*)lp;
        g_busy = false;
        EnableWindow(g_btnConvert, TRUE);
        EnableWindow(g_btnChoose, TRUE);
        InvalidateRect(g_btnConvert, nullptr, TRUE);
        SendMessageA(g_progress, PBM_SETMARQUEE, FALSE, 0);
        ShowWindow(g_progress, SW_HIDE);
        if (result->ok) {
            char buf[MAX_PATH + 32];
            snprintf(buf, sizeof(buf), "Done -- wrote %s", result->outputPath);
            SetStatus(buf);
            // SetPreview() runs first so the image is already loaded and ready to be revealed the
            // instant the wipe animation starts painting, rather than popping in mid-sweep.
            if (!result->isVideo) SetPreview(result->outputPath);
            g_doneAnim = true;
            g_doneAnimStart = GetTickCount();
            SetTimer(hwnd, IdDoneAnimTimer, 20, nullptr);
        } else {
            SetStatus("Failed -- see the log above.");
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        delete result;
        return 0;
    }
    case WM_TIMER: {
        if (wp == IdDoneAnimTimer) {
            if (GetTickCount() - g_doneAnimStart >= kDoneAnimMs) {
                g_doneAnim = false;
                KillTimer(hwnd, IdDoneAnimTimer);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // WM_PAINT below paints the whole client area every time; avoid the flash-fill
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);

        // Double-buffered: everything below is drawn to an off-screen bitmap first and blitted
        // once, so the background/cards/preview never visibly flash or tear on repaint.
        Gdiplus::Bitmap buffer(client.right, client.bottom, PixelFormat32bppPARGB);
        Gdiplus::Graphics g(&buffer);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.Clear(Gdiplus::Color(255, GetRValue(kColBg), GetGValue(kColBg), GetBValue(kColBg)));

        auto DrawCard = [&](const RECT& r) {
            Gdiplus::SolidBrush fill(Gdiplus::Color(255, GetRValue(kColCard), GetGValue(kColCard),
                                                     GetBValue(kColCard)));
            Gdiplus::Pen edge(Gdiplus::Color(255, GetRValue(kColCardEdge), GetGValue(kColCardEdge),
                                              GetBValue(kColCardEdge)), 1.0f);
            const float x = (float)r.left, y = (float)r.top;
            const float w = (float)(r.right - r.left), h = (float)(r.bottom - r.top), rad = 10.0f;
            Gdiplus::GraphicsPath path;
            path.AddArc(x, y, rad * 2, rad * 2, 180.0f, 90.0f);
            path.AddArc(x + w - rad * 2, y, rad * 2, rad * 2, 270.0f, 90.0f);
            path.AddArc(x + w - rad * 2, y + h - rad * 2, rad * 2, rad * 2, 0.0f, 90.0f);
            path.AddArc(x, y + h - rad * 2, rad * 2, rad * 2, 90.0f, 90.0f);
            path.CloseFigure();
            g.FillPath(&fill, &path);
            g.DrawPath(&edge, &path);
        };
        DrawCard(kCardInput);
        DrawCard(kCardSettings);
        DrawCard(kCardLog);
        DrawCard(kCardPreview);

        // Header: drawn directly (not a STATIC control) for crisper text and one less thing that
        // needs WM_CTLCOLORSTATIC handling to blend into the background correctly.
        Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, GetRValue(kColText), GetGValue(kColText),
                                                      GetBValue(kColText)));
        Gdiplus::SolidBrush mutedBrush(Gdiplus::Color(255, GetRValue(kColTextMuted),
                                                       GetGValue(kColTextMuted), GetBValue(kColTextMuted)));
        Gdiplus::Font titleFont(L"Segoe UI", 19, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font subFont(L"Segoe UI", 12, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        g.DrawString(L"DLSS5 Converter", -1, &titleFont, Gdiplus::PointF(20, 16), &textBrush);
        g.DrawString(L"Neural rendering for AMD GPUs, offline", -1, &subFont,
                     Gdiplus::PointF(20, 52), &mutedBrush);

        // Done animation, part 1: a fill bar in the slot the "working..." marquee bar just
        // vacated (that control is hidden by the time this fires), so a video conversion -- which
        // has no preview image to reveal -- still gets a visible payoff moment.
        float doneP = 0.0f;
        if (g_doneAnim) {
            float t = std::min(1.0f, (GetTickCount() - g_doneAnimStart) / (float)kDoneAnimMs);
            doneP = 1.0f - powf(1.0f - t, 3.0f); // ease-out cubic: quick start, soft landing
            float bl = (float)kDoneBarRect.left, bt = (float)kDoneBarRect.top;
            float bw = (float)(kDoneBarRect.right - kDoneBarRect.left);
            float bh = (float)(kDoneBarRect.bottom - kDoneBarRect.top);
            Gdiplus::SolidBrush track(Gdiplus::Color(255, GetRValue(kColCardEdge),
                                                      GetGValue(kColCardEdge), GetBValue(kColCardEdge)));
            g.FillRectangle(&track, bl, bt, bw, bh);
            Gdiplus::SolidBrush fill(Gdiplus::Color(255, GetRValue(kColAccent), GetGValue(kColAccent),
                                                     GetBValue(kColAccent)));
            g.FillRectangle(&fill, bl, bt, bw * doneP, bh);
        }

        // Preview card's interior
        EnterCriticalSection(&g_previewLock);
        if (g_preview) {
            int pw = g_previewRect.right - g_previewRect.left, ph = g_previewRect.bottom - g_previewRect.top;
            double scale = std::min((double)pw / g_preview->GetWidth(), (double)ph / g_preview->GetHeight());
            int dw = (int)(g_preview->GetWidth() * scale), dh = (int)(g_preview->GetHeight() * scale);
            int dx = g_previewRect.left + (pw - dw) / 2, dy = g_previewRect.top + (ph - dh) / 2;

            if (g_doneAnim) {
                // Done animation, part 2: a diagonal wipe sweeps the freshly-finished image on
                // left-to-right, with a small diamond riding the seam -- the reveal this app's
                // preview card actually has a natural use for (unlike the video path above).
                const float rL = (float)g_previewRect.left, rT = (float)g_previewRect.top;
                const float rR = (float)g_previewRect.right, rB = (float)g_previewRect.bottom;
                const float rH = rB - rT;
                const float slant = tanf(kWipeAngleDeg * 3.1415926f / 180.0f) * (rH / 2.0f);
                const float travel = (rR - rL) + 2.0f * slant;
                const float centerX = rL - slant + doneP * travel;
                const float topX = centerX - slant, bottomX = centerX + slant;

                Gdiplus::PointF revealed[4] = { {rL, rT}, {topX, rT}, {bottomX, rB}, {rL, rB} };
                Gdiplus::GraphicsPath clip;
                clip.AddPolygon(revealed, 4);
                g.SetClip(&clip);
                g.DrawImage(g_preview, dx, dy, dw, dh);
                g.ResetClip();

                g.SetClip(Gdiplus::RectF(rL, rT, rR - rL, rH));
                Gdiplus::Pen glowWide(Gdiplus::Color(55, 255, 255, 255), 14.0f);
                Gdiplus::Pen glowNarrow(Gdiplus::Color(120, 255, 255, 255), 7.0f);
                Gdiplus::Pen seam(Gdiplus::Color(235, 255, 255, 255), 2.5f);
                g.DrawLine(&glowWide, topX, rT, bottomX, rB);
                g.DrawLine(&glowNarrow, topX, rT, bottomX, rB);
                g.DrawLine(&seam, topX, rT, bottomX, rB);

                const float midX = (topX + bottomX) / 2.0f, midY = (rT + rB) / 2.0f, dsz = 9.0f;
                Gdiplus::PointF diamond[4] = { {midX, midY - dsz}, {midX + dsz, midY},
                                                {midX, midY + dsz}, {midX - dsz, midY} };
                Gdiplus::SolidBrush diamondFill(Gdiplus::Color(255, GetRValue(kColAccent),
                                                                GetGValue(kColAccent), GetBValue(kColAccent)));
                Gdiplus::Pen diamondEdge(Gdiplus::Color(255, 255, 255, 255), 2.0f);
                g.FillPolygon(&diamondFill, diamond, 4);
                g.DrawPolygon(&diamondEdge, diamond, 4);
                g.ResetClip();
            } else {
                g.DrawImage(g_preview, dx, dy, dw, dh);
            }
        } else {
            Gdiplus::StringFormat fmt;
            fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
            fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            Gdiplus::RectF rect((float)g_previewRect.left, (float)g_previewRect.top,
                                (float)(g_previewRect.right - g_previewRect.left),
                                (float)(g_previewRect.bottom - g_previewRect.top));
            g.DrawString(L"Preview appears here after a photo conversion", -1, &subFont, rect, &fmt,
                        &mutedBrush);
        }
        LeaveCriticalSection(&g_previewLock);

        Gdiplus::Graphics screen(hdc);
        screen.DrawImage(&buffer, 0, 0);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        if (g_fontTitle) DeleteObject(g_fontTitle);
        if (g_fontSubtitle) DeleteObject(g_fontSubtitle);
        if (g_fontBody) DeleteObject(g_fontBody);
        if (g_fontBold) DeleteObject(g_fontBold);
        if (g_fontButton) DeleteObject(g_fontButton);
        if (g_brushBg) DeleteObject(g_brushBg);
        if (g_brushCard) DeleteObject(g_brushCard);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int) {
    SetProcessDPIAware(); // crisp text/GDI+ drawing on scaled displays instead of blurry OS upscale

    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (char* slash = strrchr(exePath, '\\')) *(slash + 1) = '\0';
    g_exeDir = exePath;

    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleTitleA("DLSS5 Converter -- log");

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
    ULONG_PTR gdiToken;
    Gdiplus::GdiplusStartupInput gdiInput;
    Gdiplus::GdiplusStartup(&gdiToken, &gdiInput, nullptr);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = GuiWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "Dlss5ConverterGuiWindow";
    wc.hbrBackground = nullptr; // WM_ERASEBKGND / WM_PAINT own the whole client area
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);

    // WS_CLIPCHILDREN matters here specifically because of the done-animation: without it,
    // the parent's GDI+ repaint (fired at ~50 Hz for the duration of the wipe) and the child
    // controls' own repaints aren't excluded from each other, which is what was causing the
    // brief flicker during that animation. Nothing else in this window repaints often enough
    // for the lack of it to have been visible before.
    const DWORD winStyle = (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX) | WS_CLIPCHILDREN;
    RECT winRect = { 0, 0, kWinW, kWinH };
    AdjustWindowRectEx(&winRect, winStyle, FALSE, WS_EX_ACCEPTFILES);
    g_hwnd = CreateWindowExA(WS_EX_ACCEPTFILES, "Dlss5ConverterGuiWindow", kName, winStyle,
                             CW_USEDEFAULT, CW_USEDEFAULT, winRect.right - winRect.left,
                             winRect.bottom - winRect.top, nullptr, nullptr, hInstance, nullptr);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    CheckDependencies();

    // A file dropped on the exe / passed on the command line pre-fills the input.
    if (lpCmdLine && lpCmdLine[0]) {
        std::string arg = lpCmdLine;
        if (arg.size() >= 2 && arg.front() == '"' && arg.back() == '"') arg = arg.substr(1, arg.size() - 2);
        if (FileExists(arg)) SetInputPath(arg);
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
