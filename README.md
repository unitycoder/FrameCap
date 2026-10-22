<img src="https://img.shields.io/badge/VibeCoded-100%25-green" alt="AI Generated Content"/>

# FrameCap

<img width="758" height="532" alt="image" src="https://github.com/user-attachments/assets/be4ea9f5-fb81-476a-9c91-607b1668e251" />

A small, native C++ WebM screen recorder for Windows 10/11 x64.

## Run

Open `dist/FrameCap.exe`. No installation, FFmpeg, codec installation, or .NET runtime is needed. The other files in `dist` are license notices, not runtime dependencies.

1. Drag the title strip to position the frame.
2. Resize its edges or corners. The blue inner border marks the exact recorded area.
3. Set FPS from 1 to 60. The default is 20.
4. Leave Cursor checked to include the pointer.
5. Click Record, or press F8. The border turns red.
6. Click Stop, or press F8 again. Choose a destination in the save dialog.

The center is a real window cutout: it shows the desktop and passes mouse clicks through to the applications underneath. Controls and borders are outside the recorded rectangle. The frame stays above other applications. Position and size are locked during recording so the output dimensions stay constant.

Canceling Save keeps the completed recording in a temporary file for this session. Click Save to try again, or Discard to delete it. Closing with an unsaved recording offers Save, Discard, or Cancel. Closing while recording stops and offers the save dialog first. If another program owns F8, the status says that it is unavailable; the buttons still work.

## Output and scope

- `.webm` container with VP8 video and an index for seeking.
- Video only; no microphone or system audio.
- Software encoding with up to eight encoder threads.
- Disk-backed recording; raw frames are not accumulated in memory.
- Real capture timestamps preserve playback speed if the machine cannot meet the requested frame rate. FPS is a target, not a guarantee.
- Even pixel dimensions for I420: the visible capture hole is rounded down by at most one pixel in each dimension.
- Supports coordinates across the virtual Windows desktop, including negative monitor coordinates. Keep the whole capture rectangle on the desktop. Mixed scaling configurations need testing on Windows.
- GDI desktop capture: intended for normal desktop apps and short demonstrations. Protected video, secure desktops, HDR accuracy, and exclusive fullscreen capture are outside this version's scope. Use borderless/windowed games.
- A display configuration change stops the recording to avoid continuing with stale coordinates.
- Temporary files use the current user's Windows temporary directory. A crash or forced termination does not provide recording recovery.

## Build on Windows

Install Visual Studio 2026 or Build Tools with **Desktop development with C++**, including Windows SDK, CMake tools, and the vcpkg package manager component.

Double-click `build.bat`. It configures and builds a Release x64 executable using the vcpkg bundled with Visual Studio. The first build needs an Internet connection for dependencies and can take several minutes. The output is `dist/FrameCap.exe`.

The repository's `build.ps1` is used by the GitHub Actions workflow.

CMake uses the `x64-windows-static` triplet and a static MSVC runtime. GitHub Actions configuration is included for an optional Windows build.

## Implementation

- Win32 controls and GDI drawing; the center is removed using a window region.
- Per-monitor V2 DPI awareness, Unicode Windows paths, and a standard Save As dialog.
- BGRA desktop capture converted to limited-range BT.601 I420.
- libvpx VP8 encoding with a real-time deadline and constant-quality settings.
- A small video-only WebM muxer with duration, SeekHead, and keyframe Cues.
- Recording and encoding run on a worker thread; Stop interrupts its frame wait.

## Validation

The included EXE was cross-compiled for Windows x64 with MinGW GCC 13 and statically linked libvpx 1.15.2. Its import table contains only Windows system DLLs. The native application compiled with `-Wall -Wextra -Wpedantic -Werror`.

Portable encoder/muxer tests ran on Linux. FFmpeg/ffprobe verified decoding, dimensions, frame count, color metadata, duration, and seeking. A test with a simulated 500 ms capture stall preserved the expected 3.5 second timeline. These tools are used only for validation, not by the application.

**The Windows UI and live desktop capture have not been run in this environment.** This is a compiled first version for testing on your Windows machine, rather than a Windows-runtime-verified release.

The test sources are in `tests`. `encode_smoke.cpp` uses the same encoder and color conversion as the application; `mux_ivf.cpp` tests muxing an external VP8 IVF stream.

## License

FrameCap source is MIT licensed. libvpx has its own BSD-style license. Preserve the included third-party notices when redistributing the supplied binary. Source builds copy the installed libvpx notices into `dist`.
