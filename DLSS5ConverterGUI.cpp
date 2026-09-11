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
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")

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
};
constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_DONE = WM_APP + 2;

HWND g_hwnd = nullptr;
HWND g_editLog = nullptr, g_editPath = nullptr, g_lblIntensity = nullptr, g_lblStatus = nullptr;
HWND g_btnChoose = nullptr, g_btnConvert = nullptr, g_slider = nullptr;
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
bool g_busy = false;
Gdiplus::Bitmap* g_preview = nullptr;
CRITICAL_SECTION g_previewLock;
RECT g_previewRect = { 20, 260, 620, 640 };

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

bool LoadBmpAsRgba(const char* path, std::vector<uint8_t>& outPixels, UINT& outW, UINT& outH) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("%s: could not open %s\n", kName, path); return false; }
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    if (fread(&fh, sizeof(fh), 1, f) != 1 || fread(&ih, sizeof(ih), 1, f) != 1 ||
        fh.bfType != 0x4D42 || ih.biBitCount != 24 || ih.biCompression != BI_RGB) {
        printf("%s: %s is not an uncompressed 24-bit BMP\n", kName, path);
        fclose(f);
        return false;
    }
    const UINT w = (UINT)ih.biWidth;
    const UINT h = (UINT)(ih.biHeight >= 0 ? ih.biHeight : -ih.biHeight);
    const bool bottomUp = ih.biHeight > 0;
    const UINT srcRowBytes = (w * 3 + 3) & ~3u;
    std::vector<uint8_t> row(srcRowBytes);
    outPixels.assign((size_t)w * h * 4, 255);
    fseek(f, fh.bfOffBits, SEEK_SET);
    for (UINT y = 0; y < h; y++) {
        if (fread(row.data(), 1, srcRowBytes, f) != srcRowBytes) { fclose(f); return false; }
        const UINT destY = bottomUp ? (h - 1 - y) : y;
        uint8_t* dst = &outPixels[(size_t)destY * w * 4];
        for (UINT x = 0; x < w; x++) {
            dst[x * 4 + 0] = row[x * 3 + 2];
            dst[x * 4 + 1] = row[x * 3 + 1];
            dst[x * 4 + 2] = row[x * 3 + 0];
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

bool ProcessVideo(const std::string& inputPath, const std::string& outputPath, float intensity) {
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
    encodeCmd += " -c:v libx264 -pix_fmt yuv420p \"" + outputPath + "\"";
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
    bool ok = isVideo ? ProcessVideo(inputPath, outputPath, g_intensity)
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
    g_busy = true;
    EnableWindow(g_btnConvert, FALSE);
    EnableWindow(g_btnChoose, FALSE);
    SetWindowTextA(g_editLog, "");
    SetStatus("Working...");
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
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        auto mk = [&](const char* cls, const char* text, int x, int y, int w, int h, DWORD style, int id) {
            HWND c = CreateWindowExA(0, cls, text, style | WS_CHILD | WS_VISIBLE, x, y, w, h,
                                      hwnd, (HMENU)(INT_PTR)id, GetModuleHandle(nullptr), nullptr);
            SendMessageA(c, WM_SETFONT, (WPARAM)font, TRUE);
            return c;
        };
        mk("STATIC", "Drag a photo or video onto this window, or:", 20, 15, 400, 20, SS_LEFT, 0);
        g_btnChoose = mk("BUTTON", "Choose File...", 20, 40, 140, 28, BS_PUSHBUTTON, IdChoose);
        g_editPath = mk("EDIT", "(no file selected)", 170, 44, 470, 20,
                        ES_READONLY | ES_AUTOHSCROLL | WS_BORDER, IdFilePath);

        mk("STATIC", "Intensity:", 20, 85, 70, 20, SS_LEFT, 0);
        g_slider = CreateWindowExA(0, TRACKBAR_CLASSA, "", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                    90, 80, 300, 30, hwnd, (HMENU)(INT_PTR)IdIntensitySlider,
                                    GetModuleHandle(nullptr), nullptr);
        SendMessageA(g_slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageA(g_slider, TBM_SETPOS, TRUE, (LPARAM)(int)(g_intensity * 100));
        SendMessageA(g_slider, TBM_SETTICFREQ, 10, 0);
        g_lblIntensity = mk("STATIC", "0.08  (0.05-0.15 looked cleanest tonight; 0.25+ gets harsh)",
                            400, 85, 260, 20, SS_LEFT, IdIntensityLabel);

        g_btnConvert = mk("BUTTON", "Convert", 20, 125, 140, 32, BS_PUSHBUTTON, IdConvert);
        EnableWindow(g_btnConvert, FALSE);
        g_lblStatus = mk("STATIC", "Ready.", 170, 130, 470, 20, SS_LEFT, IdStatus);

        mk("STATIC", "Log:", 20, 170, 100, 20, SS_LEFT, 0);
        g_editLog = mk("EDIT", "", 20, 190, 600, 60,
                       ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, IdLog);

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
            snprintf(buf, sizeof(buf), "%.2f  (0.05-0.15 looked cleanest tonight; 0.25+ gets harsh)", g_intensity);
            SetWindowTextA(g_lblIntensity, buf);
        }
        return 0;
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
        }
        return 0;
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
        if (result->ok) {
            char buf[MAX_PATH + 32];
            snprintf(buf, sizeof(buf), "Done -- wrote %s", result->outputPath);
            SetStatus(buf);
            if (!result->isVideo) SetPreview(result->outputPath);
        } else {
            SetStatus("Failed -- see the log above.");
        }
        delete result;
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Gdiplus::Graphics g(hdc);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        EnterCriticalSection(&g_previewLock);
        if (g_preview) {
            int pw = g_previewRect.right - g_previewRect.left, ph = g_previewRect.bottom - g_previewRect.top;
            double scale = std::min((double)pw / g_preview->GetWidth(), (double)ph / g_preview->GetHeight());
            int dw = (int)(g_preview->GetWidth() * scale), dh = (int)(g_preview->GetHeight() * scale);
            int dx = g_previewRect.left + (pw - dw) / 2, dy = g_previewRect.top + (ph - dh) / 2;
            g.DrawImage(g_preview, dx, dy, dw, dh);
        } else {
            HBRUSH b = CreateSolidBrush(RGB(240, 240, 240));
            FillRect(hdc, &g_previewRect, b);
            DeleteObject(b);
            DrawTextA(hdc, "Preview appears here after a photo conversion", -1, &g_previewRect,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        LeaveCriticalSection(&g_previewLock);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int) {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (char* slash = strrchr(exePath, '\\')) *(slash + 1) = '\0';
    g_exeDir = exePath;

    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetConsoleTitleA("DLSS5 Converter -- log");

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);
    ULONG_PTR gdiToken;
    Gdiplus::GdiplusStartupInput gdiInput;
    Gdiplus::GdiplusStartup(&gdiToken, &gdiInput, nullptr);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = GuiWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = "Dlss5ConverterGuiWindow";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);

    g_hwnd = CreateWindowExA(WS_EX_ACCEPTFILES, "Dlss5ConverterGuiWindow", kName,
                             WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 660, 480, nullptr, nullptr, hInstance, nullptr);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

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
