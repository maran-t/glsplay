# Product Requirements Document (PRD)

## Project: Browser-Based Cloud Gaming Streaming POC
* **Version:** `0.1.0-alpha`
* **Status:** In Progress
* **Target Audience:** Core Engineering Team
* **Target Environment:** GCP `asia-south1` (Mumbai) $\rightarrow$ Local Chrome Browser (Chennai, India)

---

## 1. Executive Summary & Vision

### 1.1 Objective
Validate the feasibility and performance of **low-latency, zero-install browser-based game streaming** running on a **Google Cloud Platform (GCP) G2 VM (NVIDIA L4 GPU, Windows Server 2022)** directly to a local **Google Chrome browser** via standard **WebRTC**.

### 1.2 Core Hypothesis
By utilizing native OS & vendor graphics APIs (Microsoft DXGI, NVIDIA NVENC SDK, Windows WASAPI, Win32 `SendInput`, and W3C WebRTC), we can achieve **sub-45ms end-to-end glass-to-glass latency** at **1080p @ 60 FPS** without any third-party proprietary client software (e.g. Parsec, Moonlight, OBS).

---

## 2. Success Criteria & KPIs

| Metric | Target (POC v0.1) | Stretch Target | Measurement Method |
| :--- | :--- | :--- | :--- |
| **Glass-to-Glass Latency** | $\le 45\text{ ms}$ | $\le 30\text{ ms}$ | High-speed camera / Input timestamp loopback |
| **Video Resolution & FPS** | `1920 × 1080` @ 60 FPS | `2560 × 1440` @ 60 FPS | WebRTC `getStats()` `framesDecoded/sec` |
| **Video Bitrate** | 15 – 25 Mbps (CBR) | 35 Mbps (Adaptive) | WebRTC `bytesReceived` delta |
| **Frame Drop Rate** | $< 0.5\%$ | $< 0.1\%$ | WebRTC `framesDropped` / `totalFrames` |
| **Network Round Trip Time (RTT)** | $\le 25\text{ ms}$ (Chennai $\leftrightarrow$ Mumbai) | $\le 18\text{ ms}$ | WebRTC `currentRoundTripTime` |
| **Input Delivery Latency** | $\le 2\text{ ms}$ over DataChannel | $\le 1\text{ ms}$ | RTCDataChannel timestamp delta |
| **Audio Quality & Delay** | Opus 48kHz Stereo, $< 20\text{ ms}$ delay | Opus 96kHz | WebAudio latency metrics |

---

## 3. System Architecture & Component Breakdown

```
┌─────────────────────────────────────────────────────────────────────────────────────────────┐
│                                   CLIENT SIDE (Google Chrome)                               │
│                                                                                             │
│   ┌───────────────────────────┐      ┌─────────────────────────┐      ┌──────────────────┐  │
│   │    HTML5 <video> Element  │      │  W3C DOM Input Capture  │      │ WebRTC Stats HUD │  │
│   │   - Zero-Buffer Rendering │      │  - Pointer Lock API     │      │ - FPS, RTT, Loss │  │
│   │   - GPU Hardware Decode   │      │  - Keyboard Scan Codes  │      │ - Bitrate (Mbps) │  │
│   │   - MediaStream Hook      │      │  - Gamepad API (XInput) │      │ - Frame drops    │  │
│   └─────────────▲─────────────┘      └────────────┬────────────┘      └────────▲─────────┘  │
│                 │                                 │                            │            │
│                 │ (WebRTC Media UDP)              │ (WebRTC DataChannel UDP)   │            │
└─────────────────┼─────────────────────────────────┼────────────────────────────┼────────────┘
                  │                                 │                            │
             Internet                          Internet                          │
                  │                                 │                            │
┌─────────────────┼─────────────────────────────────┼────────────────────────────┼────────────┐
│                 │                                 │                            │            │
│   ┌─────────────▼─────────────┐      ┌────────────▼────────────┐      ┌────────┴─────────┐  │
│   │  WebRTC Streaming Engine  │      │  DataChannel Dispatcher │      │ WebSocket Signal │  │
│   │  - H.264 RTP Packetizer   │      │  - Parse binary input   │      │ - SDP Exchange   │  │
│   │  - Opus Audio RTP Packet  │      │  - Low-jitter queue     │      │ - ICE Candidates │  │
│   └─────────────▲─────────────┘      └────────────┬────────────┘      └──────────────────┘  │
│                 │                                 │                                         │
│   ┌─────────────┴─────────────┐      ┌────────────▼────────────┐                            │
│   │ Zero-Copy Capture & Encode│      │   Input Injection Layer │                            │
│   │ - DXGI Desktop Duplication│      │   - Win32 SendInput()   │                            │
│   │ - NVIDIA NVENC (L4 GPU)   │      │   - ViGEmBus (Gamepad)  │                            │
│   │ - WASAPI Audio Loopback   │      │                         │                            │
│   └─────────────▲─────────────┘      └────────────┬────────────┘                            │
│                 │                                 │                                         │
│   ┌─────────────┴─────────────────────────────────▼───────────┐                             │
│   │               GCP Windows G2 VM (NVIDIA L4)               │                             │
│   │  - Microsoft IDD Virtual Display Driver (1080p @ 60Hz)     │                             │
│   │  - Running 3D DirectX/Vulkan Game                         │                             │
│   └───────────────────────────────────────────────────────────┘                             │
│                                      SERVER SIDE (GCP VM)                                   │
└─────────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 4. Subsystem Deep-Dive

### 4.1 Display Subsystem (Headless Cloud VM Resolution)
* **Problem:** GCP Windows VMs operate without a physical display monitor, causing DirectX Desktop Duplication API to fail (`DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`) or cap at 30 FPS basic render mode.
* **Standard Solution:** Deploy the **Microsoft Indirect Display Driver (IDD)**.
* **Specifications:**
  * Resolution: `1920 × 1080` (16:9).
  * Refresh Rate: `60.00 Hz` (expandable to `120.00 Hz`).
  * Color Depth: 8-bit RGB (24 bpp).
  * Adapter Binding: Explicitly bound to the NVIDIA L4 GPU adapter.

### 4.2 Video Capture & NVENC Encoding Pipeline
* **Capture Standard:** Microsoft DirectX Graphics Infrastructure (DXGI) 1.2+ Desktop Duplication API (`IDXGIOutputDuplication`).
* **Zero-Copy Optimization:**
  1. DirectX 11 device initializes on NVIDIA L4 adapter.
  2. `AcquireNextFrame()` returns `ID3D11Texture2D` in VRAM.
  3. The texture pointer is registered directly into `NvEncodeAPI` via `NV_ENC_REGISTER_RESOURCE` without copying back to CPU system RAM.
* **NVENC Low-Latency Parameter Configuration:**
  * **Codec:** H.264 (AVC) / High Profile (`profile-level-id=640028`, `packetization-mode=1`).
  * **Encoder Preset:** `NV_ENC_PRESET_P1` (Ultra-Low Latency, High Performance).
  * **Tuning Info:** `NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY`.
  * **Rate Control Mode:** `NV_ENC_PARAMS_RC_CBR` (Constant Bitrate) with strict single-frame VBV buffer.
  * **B-Frames:** **0** (`max_b_frames = 0`) — *Strictly eliminated to prevent 16-33ms frame reordering latency*.
  * **GOP (Group of Pictures):** Infinite GOP with periodic intra-refresh slices to avoid keyframe bitrate spikes.

### 4.3 Audio Capture Pipeline
* **Capture Standard:** Microsoft Windows Audio Session API (WASAPI) in **Loopback Mode** (`AUDCLNT_STREAMFLAGS_LOOPBACK`).
* **Format:** 48,000 Hz, 16-bit, Stereo (2 Channels).
* **Encoder:** IETF RFC 6716 (Opus), 64–128 kbps, 5ms–10ms frame size for minimum encoding delay.

### 4.4 Input Capture & Injection Subsystem

#### A. Mouse Input (Pointer Lock API)
* **Client Capture:** `document.body.requestPointerLock()` captures relative cursor deltas (`movementX`, `movementY`).
* **Transport:** WebRTC DataChannel (Unordered, `maxRetransmits: 0`) to prevent packet stalling.
* **Host Injection:** Win32 `SendInput()` with `MOUSEEVENTF_MOVE`.

#### B. Keyboard Input (Hardware Scan Codes)
* **Client Capture:** DOM `keydown` / `keyup` capturing `event.code` (e.g. `KeyW`, `Space`, `ShiftLeft`).
* **Host Injection:** Win32 `SendInput()` with `KEYEVENTF_SCANCODE` to support 3D game engines that poll DirectInput hardware scan codes rather than Windows virtual keys.

#### C. Gamepad Input (XInput Emulation)
* **Client Capture:** W3C Gamepad API polled at 120Hz via `requestAnimationFrame`.
* **Host Injection:** Virtual XInput Controller via standard `ViGEmBus` kernel driver.

### 4.5 WebRTC Transport & Signaling Subsystem
* **Signaling Protocol:** Pure RFC 6455 WebSocket.
* **Messages:**
  * `{"type": "register", "role": "host" | "client"}`
  * `{"type": "offer", "sdp": "..."}`
  * `{"type": "answer", "sdp": "..."}`
  * `{"type": "candidate", "candidate": {...}}`
  * `{"type": "ping"} / {"type": "pong"}`
* **Network Traversal:**
  * Direct host ICE candidate with GCP Public IP.
  * VPC Firewall UDP ports: `50000-50100`.

---

## 5. End-to-End Latency Budget

$$\text{Total Glass-to-Glass Latency} = T_{\text{input}} + T_{\text{net\_up}} + T_{\text{inject\_game}} + T_{\text{capture}} + T_{\text{encode}} + T_{\text{net\_down}} + T_{\text{decode\_render}}$$

$$\mathbf{T_{\text{Total}} = 1\text{ms} + 10\text{ms} + 6\text{ms} + 2\text{ms} + 4\text{ms} + 10\text{ms} + 5\text{ms} \approx 38\text{ ms}}$$

---

## 6. Project Directory Structure

```text
cloud-gaming-poc/
├── web/                               # Next.js 14 WebRTC Client & HUD
│   ├── src/
│   │   ├── app/
│   │   │   ├── page.tsx               # Stream player & HUD view
│   │   │   └── layout.tsx
│   │   ├── components/
│   │   │   ├── StreamPlayer.tsx       # Video element & pointer lock handler
│   │   │   └── StatsOverlay.tsx       # WebRTC telemetry metrics display
│   │   ├── hooks/
│   │   │   ├── useWebRTC.ts           # Standard WebRTC connection manager
│   │   │   ├── useInputCapture.ts     # Pointer lock, Keyboard & Gamepad poller
│   │   │   └── useStreamStats.ts      # WebRTC getStats() collector
│   │   └── types/
│   │       └── protocol.ts            # Wire protocol message interfaces
│   ├── package.json
│   └── tsconfig.json
│
├── signaling/                         # WebSocket Signaling Server
│   ├── src/
│   │   ├── server.ts                  # WebSocket SDP/ICE broker
│   │   └── types.ts                   # Signaling payload definitions
│   ├── package.json
│   └── tsconfig.json
│
├── server/                            # GCP Host Streaming Daemon
│   ├── src/
│   │   ├── index.ts                   # Host daemon entrypoint
│   │   ├── capture/                   # DXGI Desktop Duplication
│   │   ├── encode/                    # NVENC H.264 hardware pipeline
│   │   ├── audio/                     # WASAPI loopback capture
│   │   └── input/                     # Win32 SendInput & ViGEm controller
│   ├── package.json
│   └── tsconfig.json
│
└── vm-scripts/                        # GCP Windows L4 Setup Scripts
    ├── setup-gcp-vm.ps1               # Complete one-click VM config
    ├── install-virtual-display.ps1    # Microsoft IDD virtual display installer
    └── setup-firewall.ps1             # VPC / Local Windows firewall setup
```

---

## 7. Phased Testing & Validation Roadmap

1. **Phase 1: Signaling & Direct Peer Connectivity**
   * Establish WebSocket connection between Client and GCP Host.
   * Verify SDP Offer/Answer exchange and ICE connection state transition to `connected`.
2. **Phase 2: Video Stream & Zero-Copy Capture**
   * Initialize DXGI Desktop Duplication on NVIDIA L4.
   * Verify 1080p @ 60 FPS video playback in Chrome `<video>` tag with hardware H.264 decoding.
3. **Phase 3: Audio Stream Synchronization**
   * Tap WASAPI loopback audio $\rightarrow$ Opus encoder $\rightarrow$ WebRTC audio track.
   * Validate AV sync (<10ms audio/video drift).
4. **Phase 4: Low-Latency Input Control**
   * Enable Pointer Lock and verify mouse movement on the host desktop.
   * Send keyboard scan codes and test responsiveness in Windows Notepad / games.
5. **Phase 5: Real Game Benchmark**
   * Launch a 3D game (e.g. Vulkan / DirectX 11/12 title) on the VM.
   * Measure glass-to-glass latency, frame stability, and encoder GPU utilization.
