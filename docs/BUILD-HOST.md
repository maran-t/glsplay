# Building glsplay-host

The host is native Windows C++ and needs three dependencies plus MSVC. It targets
Windows Server 2022 on the VM, but builds identically on Windows 10/11.

## Prerequisites

| | |
|---|---|
| Visual Studio 2022 | with "Desktop development with C++" (MSVC v143) |
| Windows SDK | 10.0.22621 or newer |
| CMake | 3.24+ |
| Ninja | required by libwebrtc's headers, ships with VS 2022 |

## Dependencies

```powershell
.\vm-scripts\fetch-deps.ps1
```

That downloads what it can and tells you where to put the rest. Expected layout:

```
apps/host/third_party/
├── webrtc/            Shiguredo prebuilt: include/ and lib/
├── nvenc/Interface/   nvEncodeAPI.h from the NVIDIA Video Codec SDK
└── ViGEmClient/       optional, PRD Phase 5 only
```

**libwebrtc** — Shiguredo's `webrtc-build` publishes prebuilt Windows binaries, which is
why this is a download rather than a 30GB `depot_tools` checkout. Take the
`windows_x86_64` archive from a release and extract so `include/` and `lib/` sit directly
under `third_party/webrtc/`.

**NVIDIA Video Codec SDK** — headers only. `nvEncodeAPI64.dll` ships with the driver and
is loaded at runtime, so nothing links against it. Requires an NVIDIA developer account.

**ViGEmClient** — optional. Without it the host builds with `NullGamepadSink` and
everything except gamepad works.

## Build

```powershell
cd apps\host
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output: `apps/host/build/bin/Release/glsplay-host.exe`

Without ViGEm:

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 -DGLSPLAY_ENABLE_VIGEM=OFF
```

Pointing at libwebrtc somewhere else:

```powershell
cmake -B build -DWEBRTC_ROOT=C:\sdks\webrtc -G "Visual Studio 17 2022" -A x64
```

## Run

From the **console session**, not RDP:

```powershell
.\glsplay-host.exe --room poc --signaling-url ws://localhost:8080 --secret <secret>
```

Useful flags while bringing it up:

| Flag | Why |
|---|---|
| `--log-level debug` | ICE candidates, DXGI state changes, channel transitions |
| `--no-audio` | Windows Server VMs usually have no audio endpoint |
| `--no-gamepad` | Skip ViGEm entirely |
| `--adapter N` | Force a DXGI adapter if the wrong one is picked |
| `--output N` | Pick a display when several are attached |

`--help` lists everything.

## Known build friction

**libwebrtc's API moves between releases.** This code targets current-ish `webrtc::`
namespacing. If a Shiguredo release predates a rename you may hit signature mismatches
in `peer_session.cpp` or `nvenc_video_encoder.cpp` — the surfaces most likely to shift
are `CreatePeerConnectionFactory`, `VideoEncoderFactory::Create`, and `EncodedImage`
timestamp setters.

**The CRT must match.** libwebrtc is built with the static CRT, and `CMakeLists.txt` sets
`MultiThreaded` to match. A mismatch shows up as a wall of unresolved externals rather
than anything mentioning the CRT.

**RTTI is off** (`/GR-`) because libwebrtc is compiled without it. `dynamic_cast` will not
work in host code; use `static_cast` with a checked `type()` the way
`nvenc_video_encoder.cpp` does with `D3D11FrameBuffer`.

## Architecture

```
DXGI Desktop Duplication          ID3D11Texture2D, stays in VRAM
        ↓  CopyResource (GPU→GPU)
   cursor compositor              blends the pointer in
        ↓
   D3D11FrameBuffer               a libwebrtc kNative VideoFrameBuffer
        ↓
   NvencVideoEncoder              NV_ENC_REGISTER_RESOURCE, no CPU copy
        ↓
   libwebrtc                      pacing, NACK, FEC, congestion control
        ↓  SRTP over UDP 50000-50100
   Chrome
```

Input runs the other way: browser → DataChannel → `InputDispatcher` → `SendInput` /
ViGEm.

The one honest caveat on "zero-copy": Desktop Duplication hands back a shared surface
that blocks the compositor while held, so we `CopyResource` into our own texture and
release immediately. That copy is GPU-to-GPU inside VRAM — no CPU roundtrip, so PRD
§4.2's actual guarantee holds — but it is not literally zero copies.
