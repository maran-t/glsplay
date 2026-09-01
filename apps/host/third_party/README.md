# apps/host/third_party

Native build dependencies for `glsplay-host`. CMake resolves each from a fixed
subdirectory here (`WEBRTC_ROOT`, `NVENC_ROOT`, `VIGEM_ROOT` in
`apps/host/CMakeLists.txt`).

| Dep | In the repo? | Why |
|---|---|---|
| `nvenc/` | **yes, committed** | 3 headers, ~400 KB. Otherwise behind an NVIDIA developer login. |
| `webrtc/` | **no — download** | ~800 MB, 39k files; `lib/webrtc.lib` alone is 347 MB, over GitHub's 100 MB per-file limit. |
| `ViGEmClient/` | **no — optional** | Only needed for the gamepad sink (PRD Phase 5). Host builds without it. |

`vm-scripts/fetch-deps.ps1` automates the `webrtc/` download and prints instructions
for the other two. Do it by hand as below if you prefer.

---

## 1. libwebrtc (required) — download, do not commit

Shiguredo `webrtc-build` prebuilt, **Windows x86_64**, pinned tag **`m152.7977.0.0`**.
The libwebrtc API drifts between releases (see `docs/BUILD-HOST.md`), so use this
exact tag unless you are ready to fix signature mismatches.

- Release: <https://github.com/shiguredo-webrtc-build/webrtc-build/releases/tag/m152.7977.0.0>
- Direct asset:
  <https://github.com/shiguredo-webrtc-build/webrtc-build/releases/download/m152.7977.0.0/webrtc.windows_x86_64.zip>

Extract so that `include/` and `lib/` sit **directly** under `webrtc/` (the archive
may nest one level - take the folder that contains `include/`):

```powershell
$tag = 'm152.7977.0.0'
$url = "https://github.com/shiguredo-webrtc-build/webrtc-build/releases/download/$tag/webrtc.windows_x86_64.zip"
Invoke-WebRequest $url -OutFile "$env:TEMP\webrtc.zip"
Expand-Archive "$env:TEMP\webrtc.zip" "$env:TEMP\webrtc-x" -Force
# move the dir that contains include\ to apps\host\third_party\webrtc
Move-Item "$env:TEMP\webrtc-x\webrtc" "$PSScriptRoot\webrtc" -Force
```

CMake needs `webrtc/include/` and `webrtc/lib/webrtc.lib`. Override the location
with `-DWEBRTC_ROOT=<path>` if you keep it elsewhere.

## 2. NVENC headers (required) — already committed

`nvenc/Interface/` holds `nvEncodeAPI.h`, `nvcuvid.h`, `cuviddec.h` from the
**NVIDIA Video Codec SDK**. Headers only - `nvEncodeAPI64.dll` ships with the
driver and is loaded at runtime, so nothing links against it.

Source (only if you need to update them): <https://developer.nvidia.com/nvidia-video-codec-sdk/download>
- accept the licence, download, extract, copy its `Interface/` folder over `nvenc/Interface/`.

## 3. ViGEmClient (optional) — gamepad only, do not commit

- Source: <https://github.com/nefarius/ViGEmClient> - build it, then place `include/`
  and `lib/` under `ViGEmClient/`.
- Runtime driver (installs on the VM, separate): <https://github.com/nefarius/ViGEmBus/releases>
- Skip it entirely: `cmake -B build -DGLSPLAY_ENABLE_VIGEM=OFF ...` -> builds with `NullGamepadSink`.

---

## Expected layout after all deps are in place

```
apps/host/third_party/
├── README.md                     # this file (committed)
│
├── nvenc/                        # COMMITTED
│   └── Interface/
│       ├── nvEncodeAPI.h
│       ├── nvcuvid.h
│       └── cuviddec.h
│
├── webrtc/                       # gitignored - from the Shiguredo release zip
│   ├── include/                  #   libwebrtc + vendored deps (abseil, boringssl, libyuv, ...)
│   │   └── ...
│   ├── lib/
│   │   └── webrtc.lib            #   ~347 MB static lib (static CRT - see docs/BUILD-HOST.md)
│   ├── DEPS
│   ├── NOTICE
│   └── VERSIONS
│
└── ViGEmClient/                  # gitignored - optional, only with -DGLSPLAY_ENABLE_VIGEM=ON
    ├── include/
    │   └── ViGEm/
    │       └── Client.h
    └── lib/
        └── release/x64/
            └── ViGEmClient.lib
```

Only `README.md` and `nvenc/` are tracked. `webrtc/` and `ViGEmClient/` are in
`.gitignore` and must be fetched per the steps above.
