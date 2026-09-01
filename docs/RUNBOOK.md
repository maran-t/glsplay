# Runbook — connecting your laptop to the VM

What runs where, and how to verify each link before adding the next one.

> **MOVED (repo split).** The control plane this runbook brings up — `apps/web`,
> `apps/signaling`, `packages/protocol/src` and the `.env` files they read — now
> lives in **a separate web/signaling/protocol repo (not yet created)**. All the
> `npm ... -w @glsplay/web` / `-w @glsplay/signaling` steps and `apps/signaling/test/smoke.mjs`
> below run inside that repo's checkout, not `C:\glsplay`. This `glsplay` repo is
> now host-only. The firewall/secret/verification logic is otherwise unchanged;
> `docs/DEPLOY-GCP-L4.md` is the current single-file runbook.

> **Status:** the host daemon is not finished yet. Everything below validates the
> control plane — signaling, firewall, secret, and the browser client. When the host
> lands, it slots into step 4 with no other changes.

```
   YOUR LAPTOP                          THE VM (public IP)
   ───────────                          ──────────────────
   Chrome                               glsplay-host.exe      [not built yet]
     ↓ localhost:3000                     ↕ ws://localhost:8080
   apps/web (dev server)  ──── ws ────▶ apps/signaling  :8080
     ↑                                    ↑
     └──────────── WebRTC / UDP 50000-50100 ────────────────┘
                   (direct, once the host exists)
```

---

## 0. GCP firewall — do this first

A manually created VM has no glsplay rules. Both layers must allow the traffic: the
VPC firewall **and** the Windows firewall inside the VM.

```bash
gcloud compute firewall-rules create glsplay-signaling \
  --allow tcp:8080 --source-ranges=0.0.0.0/0 \
  --description="glsplay WebSocket signaling"

gcloud compute firewall-rules create glsplay-media \
  --allow udp:50000-50100 --source-ranges=0.0.0.0/0 \
  --description="glsplay WebRTC media"
```

These apply network-wide because no `--target-tags` is given, which is what you want on
a hand-made VM. Narrow `--source-ranges` to your own IP once it works.

Get the VM's external IP — you need it in step 3:

```bash
gcloud compute instances list
```

---

## 1. On the VM — one-time setup

RDP in, then run **as Administrator**:

```powershell
# Windows firewall (the second layer)
.\vm-scripts\setup-firewall.ps1

# Driver, audio service, auto-logon, power settings
.\vm-scripts\setup-gcp-vm.ps1 -AutoLogonUser <user> -AutoLogonPassword '<password>'

# Virtual display - prints your options, then install with -DriverPath
.\vm-scripts\install-virtual-display.ps1
```

Install **Node.js 20 LTS**, then get the repo across and build:

```powershell
cd C:\glsplay
npm install
npm run build -w @glsplay/protocol
npm run build -w @glsplay/signaling
```

Create `C:\glsplay\.env`:

```ini
GLSPLAY_SIGNALING_PORT=8080
GLSPLAY_SIGNALING_HOST=0.0.0.0
GLSPLAY_ROOM_SECRET=<paste the secret from your laptop's .env>
```

`GLSPLAY_SIGNALING_HOST` must be `0.0.0.0`. Bound to `localhost` the broker works on the
VM and is invisible from outside, which looks exactly like a firewall problem.

Reboot, then verify from the **console session, not RDP**:

```powershell
.\vm-scripts\check-environment.ps1
```

---

## 2. On the VM — run the broker

```powershell
cd C:\glsplay
npm start -w @glsplay/signaling
```

Expect:

```
2026-08-29T... INFO  signaling broker listening host=0.0.0.0 port=8080
```

Leave it running.

---

## 3. On your laptop

Edit `.env` in the repo root — keep the **same secret** and point at the VM:

```ini
GLSPLAY_ROOM_SECRET=<the secret you already generated>
NEXT_PUBLIC_SIGNALING_URL=ws://<VM_EXTERNAL_IP>:8080
NEXT_PUBLIC_ROOM_ID=poc
NEXT_PUBLIC_ROOM_SECRET=<the same secret>
```

Then:

```bash
npm run dev -w @glsplay/web
```

Open <http://localhost:3000>.

---

## 4. Verify, in order

Work down this list. Each step assumes the one above passed — fixing them out of order
wastes time.

### 4.1 Is the broker reachable at all?

```bash
curl http://<VM_EXTERNAL_IP>:8080/health
```

Expect `{"status":"ok","rooms":0,"peers":0,...}`.

*Fails?* GCP firewall rule missing, Windows firewall rule missing, broker not running, or
it bound to `localhost` instead of `0.0.0.0`.

### 4.2 Does the full signaling protocol work over the internet?

```bash
node apps/signaling/test/smoke.mjs .env ws://<VM_EXTERNAL_IP>:8080
```

Expect `10/10 passed`. This exercises registration, pairing, offer/answer/ICE relay,
auth rejection, and peer-drop — **PRD Phase 1, over the real network path**.

*Fails on `bad secret`?* The two `.env` files disagree. They must match byte for byte.

### 4.3 Does the browser client connect?

At <http://localhost:3000> you should see the dark player and, bottom-left, no
`signaling:` warning — meaning it registered. The centre reads:

> **Waiting for host to join the room**

**That is the correct and expected state.** The client is connected to the broker and
waiting for a host that doesn't exist yet. Everything except the host is now proven.

The broker log on the VM should show:

```
INFO  peer registered peer=client-1 room=poc role=client paired=false
```

---

## 5. What's left

Once `glsplay-host.exe` is built, run it on the VM **in the console session**:

```powershell
.\glsplay-host.exe --room poc --signaling-url ws://localhost:8080
```

The browser then goes from "Waiting for host" to a live picture, and the telemetry HUD
starts reporting against the PRD §2 targets. No changes needed on your laptop.

---

## Troubleshooting

| Symptom | Cause |
|---|---|
| `curl /health` times out | GCP VPC rule missing, or broker bound to localhost |
| `curl` works, browser doesn't connect | Browser blocks `ws://` from an `https://` page — use `http://localhost:3000` |
| `bad-secret` | The two `.env` files differ |
| Broker sees no client | `NEXT_PUBLIC_*` vars need a dev-server restart; they're baked in at build |
| Host connects, no video | Almost always the RDP session trap — see `vm-scripts/README.md` |

`NEXT_PUBLIC_` variables are inlined at build time. Changing `.env` requires restarting
`npm run dev` — a hot reload will not pick them up.
