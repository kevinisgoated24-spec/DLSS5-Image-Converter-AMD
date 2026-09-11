# DLSS5 Image & Video Converter (AMD)

Runs a photo or video through AMD's neural denoiser -- the same engine the
[`dlss5-neural-amd`](https://github.com/zmodelerlover/dlss5-neural-amd) ReShade
add-on drives inside a live game -- on a **still photo or a video file**
instead of a running game. Point it at a picture or a clip; it hands the
image to the real, unmodified add-on and saves back whatever the add-on
actually produces.

This is not NVIDIA DLSS. Your GPU is AMD, and this repository does not
contain, does not need, and cannot use any NVIDIA binary. It drives a
community-built, reverse-engineered AMD port of a similar idea. If you were
hoping for [criso2hd-alt/DLSS5-Image-Converter](https://github.com/criso2hd-alt/DLSS5-Image-Converter)-level
results (that project wraps NVIDIA's actual proprietary `nvngx_dlssnr.dll` on
an RTX GPU with Tensor Cores) -- that tool inspired the idea of running this
kind of pass over a still image at all, but it needs real NVIDIA hardware and
files this project has neither the rights nor the ability to substitute.
What you get here is real, but modest: a working AMD-side neural pass, not a
clone of NVIDIA's.

## What you actually get

- Drop in a photo (png/jpg/webp/bmp/...) or a video (mp4/mov/mkv/avi/webm/...)
- The add-on's neural correction is blended back with the original at an
  adjustable strength, 0.08 by default -- a conservative starting point, not
  a hard ceiling. 1.00 (full strength, the add-on's own default) has since
  tested clean and good-looking too; see "Why intensity matters" below for
  why the default is more cautious than that.
- Video: frames are run through the add-on in order, with temporal history
  carried frame to frame (same as a live game), then re-encoded at the
  source frame rate with the original audio remuxed back in.

## Requirements

- Windows 10/11, an AMD GPU (tested on an RX 9060 XT)
- A real interactive desktop session. **This will not run from an automated
  tool, a CI runner, or anything launched non-interactively** --
  `CreateSwapChainForHwnd` fails with `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`
  outside Session 0's interactive desktop. Double-click it.
- [`ffmpeg`](https://ffmpeg.org/) (with `ffprobe`) on your `PATH` -- **or just
  say yes** when the tool offers to download a portable copy for you on
  first run
- Visual Studio 2022+ with the "Desktop development with C++" workload, to
  build

## Runtime files this repo does not and cannot include

None of the following are ours to redistribute. You have to bring your own,
same as `criso2hd-alt/DLSS5-Image-Converter` requires you bring your own
NVIDIA files:

| File | What it is | Where to get it |
|---|---|---|
| `dxgi.dll` | [ReShade](https://reshade.me/), add-on build, DirectX 12 target | Run ReShade's official installer against any DX12 game once, then copy the `dxgi.dll` it produces here (or install ReShade directly into this folder) |
| `ReShade.ini` | ReShade's own config | Comes from the same install |
| `dlss5-neural.addon64` | The actual neural rendering add-on | Build it from [`zmodelerlover/dlss5-neural-amd`](https://github.com/zmodelerlover/dlss5-neural-amd) (`neural` target) |
| `dlssnr_amd_pass1.dll`, `dlssnr_on_amd_weights.bin` | The AMD neural engine runtime + trained weights | Distributed via that project's Discord: **https://discord.gg/wYhvS3JSHM** -- read their README's setup section, it explains the SHA256 verification step too |
| `dlssnr_on_amd.ini` | The engine's own settings file | Auto-created on first run if missing |

Put all of these next to `DLSS5Converter.exe` / `DLSS5ConverterGUI.exe`.

**Both tools check for all of this automatically on startup** and tell you
specifically what's missing and where to get it, rather than letting you hit
a confusing failure partway through a conversion. `ffmpeg` is the one
exception it can actually fix for you -- it's a normal, freely
redistributable open-source build with a stable download URL, so the tool
offers to download a portable copy into `ffmpeg-bin\` next to itself (only
after you say yes; it never downloads anything on its own initiative). The
other files either need an interactive installer (ReShade) or aren't
something this tool has the rights to fetch on your behalf (the engine
runtime is Discord-gated) -- those just get reported clearly instead.

## Building

```
build-gui.bat   REM builds DLSS5ConverterGUI.exe
build-cli.bat   REM builds DLSS5Converter.exe
```

Both scripts locate Visual Studio and the Windows SDK automatically via
`vswhere`; no project file, no CMake.

## Using it

**GUI** (`DLSS5ConverterGUI.exe`): double-click it, drag a photo or video
onto the window (or use Choose File), adjust the Intensity slider if you
want, click Convert. A console window shows live progress; the main window
previews the result when it's a photo. Output is saved next to the input as
`<name>_dlss5.png` or `<name>_dlss5.mp4`.

**CLI** (`DLSS5Converter.exe`):

```
DLSS5Converter.exe photo.jpg
DLSS5Converter.exe video.mp4 --out result.mp4 --intensity 0.10
```

No input given opens the same file picker as the GUI.

## Why intensity matters

Early testing (before this tool switched to driving the real add-on through
ReShade -- see "How it works") used a cruder approach that talked to the
engine directly through the generic AMD FidelityFX API, with none of the
add-on's own correct compose/encoding math involved. On that path, Intensity
= 1.00 (the network's raw, full-strength output) read as harsh, oversaturated,
and edge-haloed on real photo/video content, and blending it back with the
source at ~0.08 was what made it usable. That finding shaped this tool's
default.

Once switched to driving the real add-on (what this tool has always done),
that problem mostly goes away: real-world testing since has found Intensity
= 1.00 looks clean and works well through this pipeline too -- the harshness
was specific to the abandoned raw-engine approach, not something inherent to
running at full strength. The default is still the conservative 0.08 for now,
but don't hesitate to push the slider (GUI) or `--intensity` (CLI) up toward
1.00 -- it's a legitimate, tested-good setting on this tool's actual
pipeline, not just a fallback.

## How it works

Rather than reimplementing the add-on's engine integration (raw memory
offsets into a loaded DLL, hand-written compose shaders, HIP setup) from
scratch, this tool drives the *real* add-on through the *real* ReShade
`dxgi.dll` proxy -- the exact mechanism that already works correctly in an
actual game. It creates a D3D12 device and a window/swap-chain sized to your
photo or video's resolution (purely so ReShade has something to hook and the
add-on has a back buffer to present into), copies each frame's pixels into
that back buffer, and lets the real, unmodified add-on do everything it
would do in a live game -- engine init, network dispatch, compose, the works.
It then reads back whatever the add-on left in the back buffer. Zero
reimplementation of the engine's own logic; 100% of the add-on's own
(already fixed, already tuned) code path.

The add-on requires being turned on via Ctrl+End each run (deliberately not
persisted across runs, by its own design) -- this tool synthesizes that
keypress itself; you don't need to do anything.

## Known issues / things to watch for

- **Run this from your real desktop.** Anything that launches it
  non-interactively (a script, a scheduled task, an SSH session) will fail
  at swap-chain creation.
- **Don't leave a second DLL proxy (e.g. an old standalone `version.dll`
  build of a different AMD DLSS-NR tool) in this folder.** Two competing
  hooks in the same process is exactly the kind of thing that produces
  confusing, contradictory on-screen messages. This tool only needs its own
  `dxgi.dll`.
- If you rename/replace a DLL proxy while this tool (or anything using it)
  is already running, the change won't take effect until you fully close and
  relaunch -- Windows keeps an already-loaded DLL mapped in memory
  regardless of what happens to the file on disk afterward.

## Changelog

### v1.1.0

- **GUI redesign.** A real light, card-based layout (Input / Settings / Log
  / Preview panels) instead of a bare gray dialog: modern-themed controls
  (a manifest now pulls in the current common-controls style instead of the
  ancient Windows 2000 raised-3D look), Segoe UI throughout with actual
  typographic hierarchy, a custom accent-purple rounded Convert button, a
  real progress bar while converting, and a small procedural app icon.
  Bigger window (700x820) with proper spacing. No behavior changes -- the
  CLI tool and the underlying engine-driving code are untouched.
- **Corrected the intensity guidance.** Real-world use since v1.0.0 has
  shown Intensity = 1.00 (full strength) looks clean through this tool's
  actual pipeline (the real add-on via ReShade) -- the harsh/oversaturated
  result at 1.00 documented below was specific to an early, abandoned
  raw-engine approach this tool no longer uses. The default stays at the
  conservative 0.08 for now, but 1.00 is a legitimate, tested-good setting,
  not just a fallback. See "Why intensity matters".

### v1.0.3

- **Startup dependency check.** Both tools now check for everything they
  need on launch and report exactly what's missing and where to get it,
  instead of failing confusingly mid-conversion. `ffmpeg` is offered as an
  automatic download (portable, into `ffmpeg-bin\`, only after you say yes)
  since it's the one dependency this tool actually has the rights and a
  stable source to fetch on your behalf; the CLI tool won't proceed to a
  conversion until everything required is in place, the GUI shows the same
  report in its log without blocking the window from opening.

### v1.0.2

- **The BMP loader now detects 24-bit vs. 32-bit itself** instead of relying
  solely on forcing `ffmpeg`'s output format. v1.0.1 fixed the common case
  (this tool's own `ffmpeg` conversion), but a BMP handed to it directly
  (`.bmp` input skips that conversion step entirely) could still be 32-bit
  and get rejected. The loader now reads the header's actual bit depth and
  handles either correctly, so this can't happen regardless of where the BMP
  came from.

### v1.0.1

- **Fixed: converting a screenshot (or any PNG/image with an alpha channel)
  failed outright** with "is not an uncompressed 24-bit BMP". The tool
  converts your input to BMP via `ffmpeg` before handing it to the add-on;
  without an explicit pixel format, `ffmpeg` preserves a source's alpha
  channel and writes a 32-bit BMP instead of a 24-bit one, which the loader
  rejected. Both the photo path and video frame extraction now force
  `-pix_fmt bgr24` explicitly, so this can't happen regardless of the
  source's own format.

### v1.0.0

- **First working end-to-end pipeline.** Earlier attempts drove the add-on's
  engine directly through the generic AMD FidelityFX API (`ffxDispatch`),
  which technically ran but produced consistently oversaturated, artifact-
  laden output with no clear path to matching what the add-on does correctly
  inside a real game. Switched to driving the real ReShade + real add-on
  instead (see "How it works") -- immediately cleaner, more accurate output,
  and zero risk of subtly reimplementing the add-on's own logic wrong.
- **Found the actual cause of the harsh/oversaturated look**: the add-on's
  raw network output (Intensity 1.00) is not meant to be used at full
  strength. Blending it back with the source at ~0.08-0.15 is what turns it
  from unusable into a real, working enhancement.
- **Video support**, added on top of the same ReShade-based pipeline:
  frame-accurate extraction and re-encoding via `ffmpeg`, correct source
  frame rate detection, original audio remuxed back in, temporal history
  carried frame to frame.
- **Fixed: settings silently not reaching the add-on.** Windows' file-open
  dialog changes the process's working directory to match wherever you
  browse, and this tool used to write its settings file (the one carrying
  your Intensity value) as a bare relative path -- it would land in whatever
  folder you last picked a file from instead of next to the add-on, which
  always looks for its config next to its own DLL. Every internal path is
  now anchored to the executable's own folder, and the file dialog is told
  not to change the working directory as a second line of defense.
- **Fixed: a stray leftover proxy DLL from an unrelated tool** could load
  alongside this one's `dxgi.dll` and produce a confusing on-screen message
  from a completely different program. Not a bug in this tool, but worth
  knowing about -- see "Known issues" above.
- GUI edition added: drag-and-drop, an Intensity slider, a live log, and a
  result preview, on top of the same engine-driving code as the CLI tool.

## Credits

- [`zmodelerlover/dlss5-neural-amd`](https://github.com/zmodelerlover/dlss5-neural-amd)
  (MIT) -- the actual neural rendering add-on this tool drives, and the
  source of the "blend at low intensity" finding this tool relies on for
  its default.
- [`criso2hd-alt/DLSS5-Image-Converter`](https://github.com/criso2hd-alt/DLSS5-Image-Converter) --
  inspired the idea of running this kind of neural pass over a still image
  or video file outside a live game. That project targets real NVIDIA DLSS
  on RTX hardware and shares no code with this one.
- [ReShade](https://reshade.me/) -- the injection framework this tool relies
  on to load the add-on correctly.
