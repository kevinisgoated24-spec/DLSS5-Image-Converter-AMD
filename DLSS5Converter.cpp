// DLSS5 Image & Video Converter
//
// Runs a photo or video through the real, already-fixed dlss5-neural.addon64 via the real
// ReShade dxgi.dll proxy -- the same mechanism validated live in Spider-Man 2 tonight -- rather
// than reimplementing the engine's raw offset-poking integration and compose shaders. This tool's
// only job is to be something ReShade can hook: create a D3D12 device/window/swapchain sized to
// the input, copy each frame into the back buffer, synthesize the Ctrl+End toggle the add-on
// needs (GetAsyncKeyState is global system state, so keybd_event reaches it fine even though the
// add-on is a DLL loaded into this same process), let the real pipeline run, then read back the
// back buffer the add-on has already composited its result into.
//
// Needs, next to this exe: dxgi.dll (ReShade), ReShade.ini, dlss5-neural.addon64,
// dlssnr_amd_pass1.dll, dlssnr_on_amd_weights.bin, dlssnr_on_amd.ini, and ffmpeg.exe on PATH for
// anything other than a raw 24-bit BMP in and out, or for video. Same Session-0 restriction as
// every harness tonight: run from a real interactive desktop, not launched by an automated tool.
//
// Usage: DLSS5Converter.exe [input] [--out path] [--intensity 0.08]
//   No input given -> a normal Windows "Open" file dialog.
//   No --out given -> alongside the input, named <input>_dlss5<ext>.
//   Video: extracts frames with ffmpeg, runs them through the add-on in order (temporal history
//   carried frame to frame, same as a live game), re-encodes at the source frame rate and remuxes
//   the original audio (best-effort).

#include <windows.h>
#include <commdlg.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

const char* kName = "DLSS5 Converter";
// The add-on itself always looks for dlss5-neural.ini/.log next to its own DLL (i.e. next to
// this exe), via GetModuleFileNameW on its own module handle -- not relative to whatever the
// current working directory happens to be. The file-open dialog can silently change the
// process's CWD to match wherever the user last browsed, which would otherwise scatter our own
// temp files into that folder instead of next to the exe. Every one of our own temp/settings
// paths is anchored to this instead of a bare relative filename.
std::string g_exeDir;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------------------------
// Small utilities: paths, ffmpeg, the file-open dialog.
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

std::string PickInputFile() {
    char path[MAX_PATH] = {};
    OPENFILENAMEA ofn = { sizeof(ofn) };
    ofn.lpstrFilter =
        "Images and videos\0*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.mp4;*.mov;*.mkv;*.avi;*.webm\0"
        "Image files\0*.png;*.jpg;*.jpeg;*.bmp;*.webp\0"
        "Video files\0*.mp4;*.mov;*.mkv;*.avi;*.webm;*.wmv\0"
        "All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = "Pick a photo or video to run through DLSS5";
    if (GetOpenFileNameA(&ofn)) return path;
    return "";
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

// Writes dlss5-neural.ini before the add-on ever loads, so it starts at the settings this run
// wants instead of its compiled-in defaults (Intensity 1.00 among them -- already shown tonight,
// live in Spider-Man 2 and offline here, to be well past where the correction looks clean).
void SeedAddonSettings(float intensity) {
    std::string path = g_exeDir + "dlss5-neural.ini";
    FILE* f = fopen(path.c_str(), "w");
    if (!f) { printf("%s: could not write %s\n", kName, path.c_str()); return; }
    fprintf(f, "[dlss5]\nIntensity=%.4g\nScale=1\nStructure=1\nSkin=1\nTone=1\nEncoding=0\n", intensity);
    fclose(f);
}

// ---------------------------------------------------------------------------------------------
// D3D12/DXGI plumbing. The window and swapchain exist only to be something ReShade can hook and
// the add-on can present into -- same idea as a real game's own back buffer.
// ---------------------------------------------------------------------------------------------

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
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "Dlss5ConverterWindow";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("Dlss5ConverterWindow", kName, WS_OVERLAPPEDWINDOW, 100, 100,
                              (int)w, (int)h, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    // Needed for the synthesized Ctrl+End toggle to actually register: keybd_event delivers into
    // whichever window currently has input focus.
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
        printf("%s: could not create a swap chain (0x%08lx). If this was launched by an "
               "automated tool rather than double-clicked from the desktop, that is almost "
               "certainly why -- Session 0 breaks swap chain creation. Run it from the desktop.\n",
               kName, hrSc);
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

// Uploads `pixels` into `srcTex` (already sized gpu.w x gpu.h) and leaves it in COPY_SOURCE,
// ready for TouchAndPresent below to blit straight into the back buffer.
void UploadFrame(Gpu& gpu, ID3D12Resource* srcTex, const std::vector<uint8_t>& pixels,
                  bool firstUpload) {
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

// One present: blit srcTex into the current back buffer, execute, present, wait. In Inline mode
// (the add-on's compiled-in default -- confirmed by the "0 skipped" log tonight), Present()
// blocks until the add-on's own network pass for this exact frame is done, so nothing more is
// needed to know the back buffer holds this frame's real result afterward.
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
    // GetCurrentBackBufferIndex() right after Present() names the NEXT buffer to be written, not
    // the one just shown -- step back one, wrapping at 0, to read what is actually on screen.
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

// Runs the warm-up sequence on one held frame: presents for a real few seconds, with the
// Ctrl+End toggle pressed partway through so the add-on turns itself on and its engine has time
// to come up. The elapsed-time floor (not just an iteration count) also outlasts ReShade's own
// startup banner ("Press Home to open..."), which is drawn onto the back buffer for its first
// several real seconds regardless of frame rate -- a too-short warm-up captures that banner
// baked into the result.
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
// Image path
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

// ---------------------------------------------------------------------------------------------
// Video path: extract frames with ffmpeg, run them through the add-on in order (Inline mode
// means one present per frame is enough once warmed up), re-encode and remux.
// ---------------------------------------------------------------------------------------------

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
    printf("%s: wrote %s (frames kept in %s / %s)\n", kName, outputPath.c_str(), frameDir.c_str(), outFrameDir.c_str());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (char* slash = strrchr(exePath, '\\')) *(slash + 1) = '\0';
    g_exeDir = exePath;

    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("================================\n");
    printf(" %s\n", kName);
    printf("================================\n");

    std::string inputPath, outputPath;
    float intensity = 0.08f;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--intensity" && i + 1 < argc) intensity = (float)atof(argv[++i]);
        else if (a == "--out" && i + 1 < argc) outputPath = argv[++i];
        else if (inputPath.empty()) inputPath = a;
    }

    if (inputPath.empty()) {
        printf("%s: no input given, opening the file picker\n", kName);
        inputPath = PickInputFile();
        if (inputPath.empty()) { printf("%s: no file chosen, exiting\n", kName); return 1; }
    }
    if (!FileExists(inputPath)) { printf("%s: input not found: %s\n", kName, inputPath.c_str()); return 1; }

    std::string ext = ExtOf(inputPath);
    bool isVideo = IsVideoExt(ext);
    if (outputPath.empty()) outputPath = StemOf(inputPath) + "_dlss5" + (isVideo ? ".mp4" : ".png");

    printf("%s: input    %s (%s)\n", kName, inputPath.c_str(), isVideo ? "video" : "photo");
    printf("%s: output   %s\n", kName, outputPath.c_str());
    printf("%s: strength %.2f\n", kName, intensity);

    remove((g_exeDir + "dlss5-neural.log").c_str());
    SeedAddonSettings(intensity);

    bool ok = isVideo ? ProcessVideo(inputPath, outputPath, intensity)
                       : ProcessImage(inputPath, outputPath, intensity);

    printf("--------------------------------\n");
    printf("%s: %s\n", kName, ok ? "done" : "failed -- see the messages above, and check "
                                              "dlss5-neural.log next to this exe");
    return ok ? 0 : 1;
}
