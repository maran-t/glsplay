# glsplay — deploy on a fresh GCP L4 Windows VM

Single-file runbook. Follow top to bottom on a clean `g2-standard-4` (NVIDIA L4)
Windows Server 2022 instance and you get a working browser cloud-gaming stream.
Every external download has its source link. Every fix we hit the hard way is in
**§9 Problems & solutions** with the exact cause.

> If you are Claude running this on a fresh VM: do §2 → §7 in order. §8 is the
> daily-use cheat sheet. §9 is reference. Do not skip the driver-model check in
> §4 or the RDP/console section in §6 — they are the two things that waste hours.

> **MOVED (repo split).** `apps/web`, `apps/signaling` and `packages/protocol/src`
> now live in **a separate web/signaling/protocol repo (not yet created)**. This
> `glsplay` repo is now host-only: `apps/host/` (C++) plus the VM scripts, and
> `packages/protocol/include/glsplay_input.h` (the one header the host `#include`s).
> Anything below about building/running the Node signaling broker or the Next.js
> web client — the `@glsplay/signaling` / `@glsplay/web` workspaces, `apps/web/.env`,
> the `glsplay-signaling` / `glsplay-web` scheduled tasks — belongs to that other
> repo now and is kept here only for reference until it is ported. The host build
> (§4–§6, §8 "Build the host") and the RDP/console task chain are unchanged.

---

## 1. What runs, and where

```
   LAPTOP (browser)                         THE VM  (GCP g2-standard-4, 1x L4)
   ───────────────                          ────────────────────────────────
   Chrome  ───ws://<VM_IP>:8080───▶  signaling broker    (Node, :8080)   [separate web/signaling repo]
     │                               web client          (Next.js, :3000)[separate web/signaling repo]
     │                               glsplay-host.exe    (C++)           [this repo — task: glsplay-host]
     └────── WebRTC / SRTP, UDP 50000-50100 ──────────▶  glsplay-host.exe (direct P2P once ICE completes)
```

- **signaling broker** (`apps/signaling`, *moved to the separate repo*) — WebSocket broker. Pairs one host + one client per room, relays opaque SDP/ICE. Never touches media.
- **web client** (`apps/web`, *moved to the separate repo*) — Next.js browser client. Answer-only WebRTC peer; renders the video track, sends packed input on a data channel, draws the cursor locally.
- **`apps/host` → `glsplay-host.exe`** *(this repo)* — DXGI Desktop Duplication → NVENC (H.264) → libwebrtc → UDP. Injects input with `SendInput`. Reports cursor shape+position on the control channel.
- **`packages/protocol`** — the wire contract. The TS message definitions (`src/`) moved to the separate repo with web+signaling; **this repo keeps only `include/glsplay_input.h`**, the C++ binary-input header the host compiles. The two must be kept byte-in-sync by hand (see §9, Robustness in `TODO.md`).

### Exact stack currently running (2026-08-29)

| Component | Version / detail | Source |
|---|---|---|
| OS | Windows Server 2022 Datacenter, 10.0.20348 | GCP image |
| GPU | NVIDIA L4, driver **582.53**, **WDDM** mode (GRID / vWS) | see §4 |
| Node.js | **v24.20.0** (repo min is 20 LTS) | <https://nodejs.org/en/download> |
| npm | 11.19.0 | ships with Node |
| CMake | 4.3.1 | <https://cmake.org/download/> |
| MSVC | Visual Studio 2022 Build Tools, "Desktop development with C++" (v143) | <https://visualstudio.microsoft.com/downloads/> → *Build Tools for Visual Studio 2022* |
| Windows SDK | 10.0.22621+ | VS workload |
| Virtual display | **MTT Virtual Display Driver 25.7.23** (`MttVDD`) | see §5 |
| libwebrtc | Shiguredo prebuilt, Windows x86_64, tag **`m152.7977.0.0`** (API drifts between releases - use this exact one) | <https://github.com/shiguredo-webrtc-build/webrtc-build/releases> |
| NVENC | NVIDIA Video Codec SDK headers only (`nvEncodeAPI.h`) | <https://developer.nvidia.com/nvidia-video-codec-sdk> (dev account) |
| ViGEmClient | optional (gamepad); not built in | <https://github.com/nefarius/ViGEmClient> |

### This instance's identifiers

| | |
|---|---|
| GCP project | `alpine-tracker-492412-i6` |
| Zone | `asia-south1-b` |
| Machine type | `g2-standard-4` (L4 is fixed to this type — cannot detach) |
| External IP | `34.180.13.189` — **EPHEMERAL**, changes on every stop/start (see §9.4). Reserve a static IP. |
| Internal IP | `10.160.0.4` |
| Windows user | `maranmani_t99` |
| Repo | `https://github.com/maran-t/glsplay.git`, branch `main`, checked out at `C:\glsplay` |

### Ports

| Port | Proto | Use | GCP VPC rule | Windows FW rule |
|---|---|---|---|---|
| 8080 | TCP | signaling WebSocket | `glsplay-signaling` | `glsplay-signaling-tcp` |
| 3000 | TCP | web app | `glsplay-web` | `glsplay-web-tcp` |
| 50000-50100 | UDP | WebRTC media | `glsplay-media` | `glsplay-media-udp` (+ `-out`) |
| 3389 | TCP | RDP | (default) | (default) |

---

## 2. GCP-side setup — run from your laptop (needs `gcloud`)

`gcloud` install: <https://cloud.google.com/sdk/docs/install>

```bash
PROJECT=alpine-tracker-492412-i6
ZONE=asia-south1-b
INSTANCE=<your-instance-name>

# --- VPC firewall (network-wide; narrow --source-ranges to your IP once it works) ---
gcloud compute firewall-rules create glsplay-signaling --project=$PROJECT \
  --allow tcp:8080 --source-ranges=0.0.0.0/0 --description="glsplay signaling"
gcloud compute firewall-rules create glsplay-web --project=$PROJECT \
  --allow tcp:3000 --source-ranges=0.0.0.0/0 --description="glsplay web"
gcloud compute firewall-rules create glsplay-media --project=$PROJECT \
  --allow udp:50000-50100 --source-ranges=0.0.0.0/0 --description="glsplay WebRTC media"

# --- virtual display device (headless VMs have no display head) + reserve the IP ---
gcloud compute instances stop  $INSTANCE --zone=$ZONE
gcloud compute instances update $INSTANCE --zone=$ZONE --enable-display-device
gcloud compute addresses create glsplay-ip --project=$PROJECT --region=asia-south1
gcloud compute instances delete-access-config $INSTANCE --zone=$ZONE --access-config-name="External NAT"
gcloud compute instances add-access-config    $INSTANCE --zone=$ZONE \
  --address=$(gcloud compute addresses describe glsplay-ip --region=asia-south1 --format='value(address)')
gcloud compute instances start $INSTANCE --zone=$ZONE

gcloud compute instances describe $INSTANCE --zone=$ZONE \
  --format="yaml(status,networkInterfaces[0].accessConfigs,guestAccelerators)"
```

> `--enable-display-device` alone does **not** give the L4 an output — it adds a
> separate virtio display. Capturing that yields a black screen (§9.7). You still
> need the MTT virtual display in §5. Enable it anyway; it guarantees the console
> session has *a* display.

If `start` fails with **"resource not available in the zone"** → L4 stockout (§9.6).
Retry later or recreate the instance in `asia-south1-a` / `-c`.

---

## 3. VM base tools

RDP in as `maranmani_t99`. Install, in this order:

1. **Node.js** — <https://nodejs.org/en/download> (LTS or newer; v24 tested). Installs to `C:\Program Files\nodejs\`.
2. **Visual Studio 2022 Build Tools** — <https://visualstudio.microsoft.com/downloads/> → *Tools for Visual Studio* → *Build Tools for Visual Studio 2022*. In the installer tick **"Desktop development with C++"** (pulls MSVC v143 + Windows SDK + CMake + Ninja). Only needed if you build the host on the VM (you can cross-build on a workstation and copy `glsplay-host.exe`).
3. **Git** — <https://git-scm.com/download/win> (if not present).

```powershell
git clone https://github.com/maran-t/glsplay.git C:\glsplay
cd C:\glsplay
# npm install  — MOVED: this repo is now host-only C++ (no package.json). Node/npm
# are needed only for the separate web/signaling repo; clone that and `npm install`
# inside it if you are also running the broker + web client on this VM.
```

---

## 4. GPU driver — get the L4 into WDDM mode

The GCP default is often the **compute-only (Tesla/CUDA) driver**, which is
**TCC mode**: DXGI cannot see the GPU, and `nvidia-smi -dm 0` returns
*"Not Supported"* because that driver has no WDDM mode at all. You must replace it
with the **GRID / RTX vWS** driver.

```powershell
# 1. Check what's installed
nvidia-smi --query-gpu=name,driver_version,driver_model.current --format=csv
#    -> want:  NVIDIA L4, 582.53, WDDM
#    -> if it says TCC (or the query fails / says 610.xx): continue

# 2. Uninstall the compute driver (elevated), then reboot
Start-Process -Verb RunAs -Wait -FilePath "C:\Windows\SysWOW64\RunDll32.EXE" `
  -ArgumentList '"C:\Program Files\NVIDIA Corporation\Installer2\InstallerCore\NVI2.DLL",UninstallPackage Display.Driver'
Remove-Item C:\Windows\System32\nvidia-smi.exe -Force -ErrorAction SilentlyContinue
Restart-Computer

# 3. Install the GRID/vWS driver (elevated). This script auto-detects the GCP
#    multi-region and downloads:
#      https://storage.googleapis.com/compute-gpu-installation-<asia|us|eu>/windows/
#        582.53_grid_win10_win11_server2022_server_2025_dch_64bit_international.exe
#    (SHA256 pinned inside the script). It exits early if nvidia-smi already exists.
powershell -ExecutionPolicy Bypass -File C:\glsplay\install_gpu_driver.ps1
Restart-Computer

# 4. Verify
nvidia-smi --query-gpu=name,driver_model.current --format=csv   # NVIDIA L4, WDDM
```

If the L4 has **vanished from the PCI bus** after a stop/start rather than showing
as TCC, that is §9.6 — a GCP-side detach; stop/start again.

---

## 5. Virtual display driver (MTT) — give the L4 an output

WDDM alone isn't enough: a headless L4 has **no display head**, so
`IDXGIOutputDuplication::DuplicateOutput` fails with `0x887A0002`. Install an
Indirect Display Driver bound to the L4.

We use **`VirtualDrivers/Virtual-Display-Driver`** (a.k.a. MTT VDD). It is the
standard in the Sunshine/Moonlight self-hosting world, SignPath-signed (chains to
GlobalSign, already trusted — **no cert import needed**), user-mode (no Secure Boot
/ test-signing changes).

- Repo: <https://github.com/VirtualDrivers/Virtual-Display-Driver>
- Driver-only zip (25.7.23): <https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VirtualDisplayDriver-x86.Driver.Only.zip>
- Already extracted in this repo at `C:\glsplay\vdd\VirtualDisplayDriver\` (`MttVDD.inf/.dll/.cat`, `vdd_settings.xml`).

```powershell
# 1. Settings file MUST be at this hardcoded path. Bind rendering to the L4.
mkdir C:\VirtualDisplayDriver -Force
Copy-Item C:\glsplay\vdd\VirtualDisplayDriver\vdd_settings.xml C:\VirtualDisplayDriver\
#   ensure it contains:  <gpu><friendlyname>NVIDIA L4</friendlyname></gpu>
#   (repo copy already set; the default is "default", which renders on the wrong adapter — §9.8)

# 2. Verify the signature (optional but reassuring)
Get-AuthenticodeSignature C:\glsplay\vdd\VirtualDisplayDriver\mttvdd.cat |
  Select-Object Status, @{n='Signer';e={$_.SignerCertificate.Subject}}
#   -> Valid, CN=SignPath Foundation

# 3. Install the device. It is a root device, so pnputil alone won't spawn it —
#    use the Add-Legacy-Hardware wizard:
devmgmt.msc
#   Action -> Add legacy hardware -> Next
#   -> "Install the hardware that I manually select" -> "Display adapters"
#   -> "Have Disk..." -> C:\glsplay\vdd\VirtualDisplayDriver\MttVDD.inf
#   -> "Virtual Display Driver by MTT" -> Finish
#   (headless scripted alternative: C:\glsplay\vdd\silent-install.ps1 — uses NefCon
#    https://github.com/nefarius/nefcon/releases/download/v1.14.0/nefcon_v1.14.0.zip)

# 4. Verify a "Virtual Display Driver" device appears, status OK
Get-PnpDevice -Class Display | Select Status, FriendlyName, InstanceId
```

Resolution is set later by `run-host.ps1` via `C:\glsplay\vdd\set-vdd-res.ps1`
(a self-contained `ChangeDisplaySettingsEx` P/Invoke — the driver's own
`vdd_settings.xml` mode often doesn't apply and it comes up 800x600, §9.9).

---

## 6. The RDP / console trap — and the task chain that solves it

**The core headless problem:** Desktop Duplication captures the desktop of the
session it runs in. While you are connected over **RDP**, that session's display is
the RDP adapter — the MTT/L4 display is on the **console session**, which is
disconnected. So `glsplay-host.exe` run from RDP sees `outputs=0` on the L4.
Disconnecting RDP does **not** hand your session to the console; it just goes
"Disconnected" and renders to nothing. `tscon <id> /dest:console` moves it — but
that needs **SYSTEM** privileges (`Access is denied` otherwise).

**Solution:** autologon so the console session always has a logged-in desktop +
four scheduled tasks. Set up in one shot:

```powershell
# One-time base OS setup: autologon, disable lock screen / screensaver / sleep,
# fSingleSessionPerUser=1  (repo script)
powershell -ExecutionPolicy Bypass -File C:\glsplay\vm-scripts\setup-gcp-vm.ps1 `
  -AutoLogonUser maranmani_t99 -AutoLogonPassword '<windows-password>'

# Windows firewall for media + signaling (repo script)
powershell -ExecutionPolicy Bypass -File C:\glsplay\vm-scripts\setup-firewall.ps1
#   (this does NOT add the :3000 web rule — add it once:)
New-NetFirewallRule -DisplayName 'glsplay-web-tcp' -Direction Inbound -Action Allow `
  -Protocol TCP -LocalPort 3000 -Profile Any

# The glsplay orchestration: machine secret + all 4 scheduled tasks + lock/power
# hardening (PromptOnSecureDesktop=0 etc). SELF-ELEVATES.
powershell -ExecutionPolicy Bypass -File C:\glsplay\vdd\setup-headless.ps1
```

`setup-headless.ps1` registers:

| Task | Trigger | Runs as | Action |
|---|---|---|---|
| ~~`glsplay-signaling`~~ | ~~At startup~~ | ~~SYSTEM~~ | **MOVED** — the registration loop is commented out in `setup-headless.ps1` (§3 of that script). Register from the separate web/signaling repo, pointed at its own checkout. |
| ~~`glsplay-web`~~ | ~~At startup~~ | ~~SYSTEM~~ | **MOVED** — same as above. |
| `glsplay-host` | *(none — on demand)* | `maranmani_t99` | `powershell -File C:\glsplay\run-host.ps1` |
| `glsplay-reclaim` | **Session RemoteDisconnect** | SYSTEM | `powershell -File C:\glsplay\vdd\reclaim-console.ps1` |

`setup-headless.ps1` now registers only `glsplay-host` and `glsplay-reclaim`.

**Flow when you disconnect RDP:**

```
RDP disconnect
  → glsplay-reclaim (SYSTEM)
      → reclaim-console.ps1: find user session via explorer.exe SessionId
      → tscon <sid> /dest:console   (hands that session to the physical console;
                                     the L4 now exposes 2 DXGI outputs —
                                       output 0 = L4 phantom head, ~1280x800
                                       output 1 = Virtual Display Driver, 1920x1080)
      → wait 4s → schtasks /run /tn glsplay-host
  → glsplay-host (maranmani_t99, now in the console session)
      → run-host.ps1:
          → set-vdd-res.ps1  — best-effort pin of the VDD to 1920x1080@60.
            May log "No MTT / Virtual Display device found" when it runs from a
            non-interactive window station; harmless — vdd_settings.xml already
            pins the VDD to 1920x1080@60.
          → glsplay-host.exe --output 1   (capture the Virtual Display Driver;
            run-host.ps1 -Output '' falls back to output 0 = the phantom)
      → C:\glsplay\host.log  (+ .prev, .res)
```

Logs: `C:\glsplay\reclaim-console.log`, `C:\glsplay\host.log` (+ `.prev`, `.res`).

---

## 7. Config files & secret

The room secret is presented by **all three** sides (host, broker, browser) and is
inlined into the browser bundle. Generate once:

```powershell
node -e "console.log(require('crypto').randomBytes(32).toString('hex'))"
```

**There are TWO `.env` files — keep them in sync** (§9.4):

`C:\glsplay\.env` — read by the signaling broker (`node --env-file`):

```ini
GLSPLAY_SIGNALING_PORT=8080
GLSPLAY_SIGNALING_HOST=0.0.0.0
GLSPLAY_ROOM_SECRET=<secret>
NEXT_PUBLIC_SIGNALING_URL=ws://<STATIC_VM_IP>:8080
NEXT_PUBLIC_ROOM_ID=poc
NEXT_PUBLIC_ROOM_SECRET=<secret>
```

`apps/web/.env` — **read by `next build`** (Next.js loads `.env` from the project
dir, not the monorepo root). **MOVED:** this file now lives in the separate
web/signaling repo's checkout, not under `C:\glsplay`:

```ini
GLSPLAY_ROOM_SECRET=<secret>
NEXT_PUBLIC_SIGNALING_URL=ws://<STATIC_VM_IP>:8080
NEXT_PUBLIC_ROOM_ID=poc
NEXT_PUBLIC_ROOM_SECRET=<secret>
```

Machine env var (so `run-host.ps1` / `glsplay-host.exe` get the secret without a
`--secret` flag — `setup-headless.ps1` sets this):

```powershell
[Environment]::SetEnvironmentVariable('GLSPLAY_ROOM_SECRET','<secret>','Machine')
```

`C:\VirtualDisplayDriver\vdd_settings.xml` — MTT config, **hardcoded path**;
`<friendlyname>NVIDIA L4</friendlyname>` binds rendering to the L4.

---

## 8. Build & run

### Build the JS side (always)

> **MOVED to the separate web/signaling/protocol repo.** The commands below no
> longer work in `C:\glsplay` (no `package.json`, no `@glsplay/*` workspaces).
> Run them inside that repo's checkout instead. Kept here as the reference for
> what that repo's deploy has to do on this VM.

```powershell
cd <web-signaling-repo>
npm install
npm run build -w @glsplay/protocol
npm run build -w @glsplay/signaling
npm run build -w @glsplay/web        # reads apps/web/.env — rebuild after any IP/secret change
```

> **Never run `npm run dev -w @glsplay/web`** on the VM — `next dev` overwrites
> `.next` with a dev build that has no `BUILD_ID`, and `next start` then refuses it
> (§9.3). If it happens: `rm -r apps\web\.next; npm run build -w @glsplay/web`.

### Build the host (only if not copying a prebuilt exe)

```powershell
powershell -ExecutionPolicy Bypass -File C:\glsplay\vm-scripts\fetch-deps.ps1
#   then place, under apps/host/third_party/ :
#     webrtc/            <- Shiguredo windows_x86_64 archive (include/ + lib/)
#                           https://github.com/shiguredo-webrtc-build/webrtc-build/releases
#     nvenc/Interface/   <- nvEncodeAPI.h from the NVIDIA Video Codec SDK
#                           https://developer.nvidia.com/nvidia-video-codec-sdk
#     ViGEmClient/       <- optional; https://github.com/nefarius/ViGEmClient
cd C:\glsplay\apps\host
cmake -B build -G "Visual Studio 17 2022" -A x64          # add -DGLSPLAY_ENABLE_VIGEM=OFF if no ViGEm
cmake --build build --config Release
#   -> apps/host/build/bin/Release/glsplay-host.exe
```

Build friction is documented in `docs/BUILD-HOST.md` (CRT must be `MultiThreaded`,
RTTI off, libwebrtc API drift between Shiguredo releases).

### Daily use

The signaling broker + web client (`glsplay-signaling`, `glsplay-web` tasks) are
now owned by the separate web/signaling repo — start/verify them per that repo's
docs. Once they're up:

```powershell
Invoke-RestMethod http://localhost:8080/health          # {"peers":0,...}
```

Then:

1. **Disconnect RDP** (window ✕ — *not* Sign out). `glsplay-reclaim` fires → host starts.
2. Laptop browser → `http://<STATIC_VM_IP>:3000` → **Ctrl+Shift+R** (fresh bundle) → **click the video** (starts playback + Pointer Lock; input only works after this gesture; **Esc** releases).
3. To inspect: reconnect RDP (this *pauses* the stream — RDP re-steals the display), then:
   ```powershell
   Get-Content C:\glsplay\reclaim-console.log -Tail 6
   Get-Content C:\glsplay\host.log.res
   Get-Content C:\glsplay\host.log -Tail 40
   ```
   Want in `host.log`: `running in the console session`,
   `DXGI adapters: [0] NVIDIA L4 ... outputs=2`, `selected NVIDIA adapter 0: NVIDIA L4`,
   `DXGI duplication ready: NVIDIA L4 1920x1080`, `capture started: 1920x1080`, and the
   stats line's `input N ev` climbing while you move the mouse. `outputs=1` and
   `no output 1 on adapter NVIDIA L4` means the host ran while the session was still
   on RDP, not the console — the VDD output is only visible from the console session.

### Manual host run (debugging, from the console session only)

```powershell
$env:GLSPLAY_ROOM_SECRET = '<secret>'
C:\glsplay\apps\host\build\bin\Release\glsplay-host.exe `
  --room poc --signaling-url ws://localhost:8080 --log-level debug --no-audio --output 1
#   flags: --secret <s> | --adapter N | --output N | --no-gamepad
#   --output 1 = Virtual Display Driver (1920x1080); --output 0 = L4 phantom (~1280x800)
```

---

## 9. Problems & solutions (everything we hit)

> Several entries below cite `apps/web/...` / `apps/signaling/...` source paths in
> their "Fixed in code" notes. Those files moved to the separate web/signaling
> repo — the paths are kept as historical record of where each fix landed.

### 9.1 Signaling "loops continuously" / broker log spam
**Cause:** two browser tabs in the same room. The broker's `seatPeer` always
evicted the incumbent; the evicted client reconnected on WS close `4000`; the
reconnect backoff reset on every socket `open` → sub-second ping-pong forever.
**Fixed in code** (`apps/signaling/src/server.ts`, `apps/web/src/lib/signaling.ts`,
`apps/host/src/net/signaling_client.cpp`):
- Broker refuses a 2nd claimant while a *live* incumbent holds the role
  (`error: role-taken`), instead of evicting.
- Client treats `role-taken` **and** close `4000` as terminal (no reconnect).
- Reconnect backoff clears only after 3 s of stable connection (both web + host).
- Broker heartbeat 15 s → 10 s.
- Host backs off after a *post-connect* drop, not just a failed connect.
**Ops fix if it happens live:** close every stray browser tab (all devices);
or change `GLSPLAY_ROOM_SECRET` + rebuild web (old tabs then get `bad-secret`,
which is terminal); or firewall-block 8080 temporarily.

### 9.2 `/health` shows `peers:1` with nothing started
**Cause:** a leftover browser tab on the laptop. Identify the client IP in the
broker log (`remote=…`). Note `8.234.97.90` was **this VM's own external IP**
(self-connection, ignore); `49.206.99.36` was the **laptop** (same as the RDP
client IP).
**Fix — on the laptop:**
```
netstat -ano | findstr ":8080"     # ESTABLISHED -> note the PID
tasklist /FI "PID eq <pid>"        # chrome.exe / msedge.exe
taskkill /PID <pid> /F             # or: taskkill /IM chrome.exe /F
```
Verify on the VM: `curl http://localhost:8080/health` → `peers:0`.

### 9.3 Web: "Could not find a production build in the '.next' directory"
**Cause:** `next dev` was run at some point; it overwrote `.next` with dev
artifacts (`.next/static/development/` present, no `BUILD_ID`).
**Fix:** `Remove-Item -Recurse C:\glsplay\apps\web\.next ; npm run build -w @glsplay/web`.
Never run `npm run dev -w @glsplay/web` on the VM again.

### 9.4 Browser connects to the wrong signaling IP
**Cause A:** the web build reads `apps/web/.env`, **not** the repo-root `.env`.
They had drifted. **Cause B:** the VM's external IP is **ephemeral** and changed
(`8.234.97.90` → `34.180.13.189`) on a stop/start.
**Fix:** update `NEXT_PUBLIC_SIGNALING_URL` in **`apps/web/.env`**, rebuild web,
restart the web process. Permanently: reserve a **static IP** (§2).

### 9.5 Host: `no NVIDIA adapter found` / `EnumAdapters ... 0x887A0002`
**Cause:** L4 in **TCC mode** — the GCP compute/CUDA driver (`610.xx`) is TCC-only;
DXGI can't enumerate it; `nvidia-smi -dm 0` → *Not Supported*.
**Fix:** §4 — uninstall the compute driver, install the GRID/vWS `582.53` driver
via `install_gpu_driver.ps1`, reboot, confirm `driver_model.current = WDDM`.

### 9.6 L4 gone from the PCI bus after a stop/start
**Cause:** GCP detached the accelerator during the stop/start (host placement /
capacity). `Get-PnpDevice` shows no `VEN_10DE` device at all.
**Fix:** from the laptop —
`gcloud compute instances describe $INSTANCE --zone $ZONE --format="yaml(guestAccelerators)"`;
if listed, `stop` then `start` again. **"resource not available in the zone"** =
L4 stockout — retry later or recreate in `asia-south1-a` / `-c`. For `g2` the L4
is fixed to the machine type; you cannot add/remove it with `--accelerator`.

### 9.7 Host: `A headless L4 has no display head` / `DuplicateOutput ... 0x887A0002`
**Cause:** WDDM L4 with no attached monitor. GCP's `--enable-display-device`
provides a *separate* virtio "Basic Display Adapter", not an L4 output — capturing
it gives a black screen (the host code comments say so).
**Fix:** §5 — install the MTT virtual display driver bound to the L4.

### 9.8 MTT virtual display renders on the wrong GPU
**Cause:** `C:\VirtualDisplayDriver\vdd_settings.xml` had
`<friendlyname>default</friendlyname>` → Windows rendered the virtual monitor on
the MTT software adapter, so `EnumOutputs` on the L4 was still empty.
**Fix:** set `<friendlyname>NVIDIA L4</friendlyname>`, then reload:
`Disable-PnpDevice -InstanceId 'ROOT\DISPLAY\0001' -Confirm:$false ; Enable-PnpDevice -InstanceId 'ROOT\DISPLAY\0001' -Confirm:$false` (or reboot).

### 9.9 MTT display stuck at 800x600
**Cause:** the driver's `vdd_settings.xml` mode list doesn't reliably apply the
resolution at load.
**Fix:** `C:\glsplay\vdd\set-vdd-res.ps1 -Width 1920 -Height 1080 -Hz 60`
(self-contained `ChangeDisplaySettingsEx` P/Invoke, no modules). Must run in the
session that owns the display — it's already wired into `run-host.ps1`, which the
`glsplay-reclaim`→`glsplay-host` chain runs after `tscon`.

### 9.9b Stream runs but is 1280x800, not 1920x1080
**Cause:** in the console session the L4 adapter exposes **two** DXGI outputs —
output 0 is the L4's own phantom head (~1280x800), output 1 is the Virtual Display
Driver (1920x1080). `glsplay-host` defaults to `--output 0` and captures the
phantom.
**Fix:** `run-host.ps1` passes `--output 1` (via its `-Output` param, default
`'1'`). Confirm `host.log`: `DXGI adapters: [0] NVIDIA L4 ... outputs=2` then
`DXGI duplication ready: NVIDIA L4 1920x1080`. If instead you see `outputs=1` /
`no output 1 on adapter NVIDIA L4`, the host ran while the session was on RDP —
the VDD output only exists on the console session (§6). Set `-Output ''` to force
output 0 as a fallback.

### 9.10 `tscon 2 /dest:console` → "Access is denied" (Error 5)
**Cause:** moving another session to the console needs `SeTcbPrivilege` — an
elevated admin prompt is not enough, it must be **SYSTEM**.
**Fix:** `C:\glsplay\vdd\reclaim-console.ps1` runs as SYSTEM from the
`glsplay-reclaim` scheduled task (trigger: RemoteDisconnect). It finds the
interactive session via `explorer.exe`'s `SessionId`.

### 9.11 Host runs but captures nothing — "Running in session N, but the console is M"
**Cause:** the host started in an RDP (or disconnected-RDP) session, not the
console. Disconnecting RDP alone does not fix it.
**Fix:** the task chain in §6 (`tscon` to console *then* start the host). Autologon
(`setup-gcp-vm.ps1`) must have populated the console session first.

### 9.12 `DuplicateOutput access denied` — "a secure desktop is in the foreground"
**Cause:** the session is on the lock screen / secure desktop, or a UAC prompt is
up on the secure desktop. Desktop Duplication is forbidden from capturing it.
**Fix (in `setup-headless.ps1`):** registry —
`PromptOnSecureDesktop=0`, `InactivityTimeoutSecs=0`, `DisableLockWorkstation=1`,
`Personalization\NoLockScreen=1`, plus `powercfg` monitor/standby timeouts to 0.
Then recycle the session (`logoff <id>` or reboot) so autologon brings it up
unlocked. Check what's holding it: `Get-Process LogonUI, consent`.

### 9.13 Browser: `setLocalDescription ... Called in wrong state: stable` — and mouse dead
**Cause:** WebRTC negotiation race. The host re-offers on every `renegotiate`
(client sends it on `registered`, `peer-state`, and ICE `failed`), so a 2nd offer
landed while the first answer was still being built; `setLocalDescription(answer)`
then fired in `stable` and aborted negotiation → **data channels never opened →
no input reached the host** (keyboard/clicks worked only in a run where negotiation
happened to complete).
**Fixed** in `apps/web/src/hooks/useWebRTC.ts` `handleOffer`: serialized with a
`negotiating` flag + `pendingOffer` slot (always negotiates the newest offer),
`signalingState` re-checked before `createAnswer` and before
`setLocalDescription`, no rollback (invalid for an answer-only peer —
`setRemoteDescription({type:'offer'})` is valid from both `stable` and
`have-remote-offer`).

### 9.14 Mouse pointer has a "shadow" / flicker, then isn't visible at all
**Cause (shadow):** the host composited the cursor into the video frame
(`CursorCompositor::Draw`) → encoder ghosting on the small fast sprite + a second
(browser-local) cursor drawn on top. Inverting cursor types (`MASKED_COLOR` /
monochrome XOR — the I-beam) were rendered as an opaque box.
**Cause (invisible after first fix):** the app is Pointer-Lock/relative-mouse only,
and Pointer Lock hides the OS cursor — with compositing removed there was nothing
to show.
**Fixed — the standard way** (host stops compositing; cursor is metadata):
- `apps/host/src/capture/dxgi_duplicator.cpp` — new `DecodeCursorRgba()` converts
  any DXGI shape type to straight-alpha RGBA (XOR/invert → black/white, like RDP).
- `apps/host/src/capture/desktop_capture_source.cpp` — removed the
  `CursorCompositor::Draw` call; added a mutex-guarded `cursor_snapshot()`.
- `apps/host/src/net/host_stats_reporter.cpp` — polls the pointer at 20 Hz and
  pushes a `{"type":"cursor", visible, x, y, hotspotX/Y, width, height,
  rgbaBase64?}` message on the control channel when shape/visibility/position
  changes. (Includes a tiny base64 encoder.)
- `packages/protocol/src/control.ts` — new `CursorUpdateMessage`.
- `apps/web/src/hooks/useWebRTC.ts` — decodes the shape (base64 → `ImageData` →
  canvas → data URL), exposes `state.cursor`.
- `apps/web/src/components/StreamPlayer.tsx` — **not Pointer-Locked:** sets
  `video.style.cursor = url(<shape>) hotspotX hotspotY, auto` (browser draws it at
  the true mouse position, zero latency). **Pointer-Locked:** draws an `<img>`
  overlay at the host-reported position, mapped into the video's letterboxed box
  (needs `captureSize` from `hello.display`).
**Trade-off:** under Pointer Lock the overlay trails input by ~50–100 ms (it's
driven by the 20 Hz host position report). A zero-latency locked cursor needs
absolute-mouse mode (a change to `useInputCapture` + the host injector) — not done.

### 9.15 Web UI: status card covers a working stream; stats HUD always on
**Fixed:** `StreamPlayer` hides the status card once the video is playing (only a
hard `connection === 'failed'` / explicit error re-shows it). `page.tsx` — stats
HUD hidden by default, toggled by a **Stats** button (or F1); added a **Full
screen** button.

### 9.16 Host link error `LNK1104: cannot open file 'glsplay-host.exe'`
**Cause:** the running host has the exe locked.
**Fix before rebuilding:** `Get-Process glsplay-host | Stop-Process -Force`, and
`Disable-ScheduledTask glsplay-host ; Disable-ScheduledTask glsplay-reclaim` so
they don't relaunch mid-build; re-enable after.

### 9.17 `git commit` → "Author identity unknown"
**Fix:** `git config user.email "<you>@example.com" ; git config user.name "<you>"`.

---

## 10. File & script reference

### Our scripts — `C:\glsplay\vdd\`
| File | Purpose |
|---|---|
| `setup-headless.ps1` | **One-shot orchestration.** Machine secret + registers all 4 scheduled tasks + lock/power hardening + status. Self-elevates. |
| `reclaim-console.ps1` | Runs as SYSTEM from `glsplay-reclaim`. `tscon` the user session → console, then `schtasks /run glsplay-host`. Log: `C:\glsplay\reclaim-console.log`. |
| `set-vdd-res.ps1` | Sets the MTT display mode via `ChangeDisplaySettingsEx` P/Invoke. `-Width -Height -Hz`. Called by `run-host.ps1`. |
| `silent-install.ps1` | From the VirtualDrivers repo — headless MTT install via NefCon. Kept for reference; we used the Device Manager wizard instead. |
| `changeres-VDD.ps1` | From the VirtualDrivers repo — needs an extra PS module; unused. |
| `VirtualDisplayDriver/` | Extracted MTT driver: `MttVDD.inf/.dll/.cat`, `vdd_settings.xml`. |

### Repo scripts
| File | Purpose |
|---|---|
| `run-host.ps1` (root) | `set-vdd-res.ps1` → 1080p, then launch `glsplay-host.exe`. Log: `C:\glsplay\host.log` (+ `.prev`, `.res`). |
| `install_gpu_driver.ps1` (root) | Downloads + installs the NVIDIA GRID/vWS driver from the GCP bucket. |
| `vm-scripts/setup-gcp-vm.ps1` | Autologon, disable lock/screensaver/sleep, `fSingleSessionPerUser=1`. |
| `vm-scripts/setup-firewall.ps1` | Windows FW rules: media UDP 50000-50100, signaling 8080. (Does *not* add :3000.) |
| `vm-scripts/provision-vm.ps1` | Creates the GCP VPC firewall rules (run from the laptop). |
| `vm-scripts/install-virtual-display.ps1` | Generic IDD helper. Its printed Parsec URL is dead — use MTT (§5). Accepts `-DriverPath`. |
| `vm-scripts/check-environment.ps1` | Post-setup validation. Run from the console session. |
| `vm-scripts/fetch-deps.ps1` | Downloads / points at the host build dependencies. |
| `docs/BUILD-HOST.md` | Host build prerequisites and known friction. |
| `docs/RUNBOOK.md` | Original laptop↔VM control-plane bring-up (pre-headless-automation). Describes the broker + web client, which now live in the separate web/signaling repo. |

### Scheduled tasks (created by `setup-headless.ps1`)
`glsplay-host` — on-demand, `maranmani_t99`.
`glsplay-reclaim` — Session RemoteDisconnect, SYSTEM.
`glsplay-signaling`, `glsplay-web` — **MOVED**: AtStartup/SYSTEM tasks, now
registered from the separate web/signaling repo (the loop that created them in
`setup-headless.ps1` §3 is commented out).

### Logs
`C:\glsplay\host.log` (+ `.prev`, `.res`) · `C:\glsplay\reclaim-console.log` ·
broker: stdout of the `glsplay-signaling` task (web/signaling repo) ·
`curl http://localhost:8080/health`.
