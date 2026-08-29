# vm-scripts

Provisioning and verification for the glsplay host VM.

## Order of operations

| # | Script | Where | Notes |
|---|--------|-------|-------|
| 0 | — | Cloud Console | **Request `NVIDIA_L4_GPUS` quota in `asia-south1`.** Longest lead time in the project; often zero on new projects |
| 1 | `provision-vm.ps1` | Your machine | Creates the VM and the VPC firewall rules |
| 2 | `setup-gcp-vm.ps1` | The VM (admin) | NVIDIA driver, audio service, auto-logon, power settings |
| 3 | `install-virtual-display.ps1` | The VM (admin) | IDD virtual display — the L4 has no display head |
| 4 | `setup-firewall.ps1` | The VM (admin) | Windows Firewall — a separate layer from the VPC rules |
| 5 | *reboot* | The VM | Required for the driver and auto-logon |
| 6 | `check-environment.ps1` | The VM (console) | Verifies everything. Run after every reboot |

## Getting started

```powershell
# 0. Check quota first - this blocks everything else
gcloud compute regions describe asia-south1 --format=json | Select-String -Pattern "L4" -Context 2,2

# 1. Provision (from your machine)
.\provision-vm.ps1 -ProjectId my-project

# 2. Get a Windows password
gcloud compute reset-windows-password glsplay-host --zone asia-south1-c

# 3. RDP in, copy this folder across, then as Administrator:
.\setup-gcp-vm.ps1 -AutoLogonUser myuser -AutoLogonPassword 'the-password'
.\install-virtual-display.ps1          # prints download options
.\install-virtual-display.ps1 -DriverPath C:\drivers\parsec-vdd
.\setup-firewall.ps1
Restart-Computer

# 4. After reboot, from the CONSOLE session (not RDP):
.\check-environment.ps1
```

## The three traps

Each of these produces a failure whose symptom points somewhere other than the cause.
`check-environment.ps1` tests all three.

**RDP hijacks the capture session.** Desktop Duplication captures whichever session
owns the display. Connect over RDP and you capture the RDP virtual display — which has
no NVENC. The host appears to start, then produces no frames or silently falls back to
software. After any RDP session, hand the desktop back:

```powershell
query session
tscon <session-id> /dest:console
```

Auto-logon (step 2) keeps a console session alive across reboots so there is always a
desktop to capture.

**No display head.** A data-center L4 has no monitor. `DuplicateOutput` fails with
`DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` until an IDD virtual display exists. Its mode list
must include 1920×1080 @ 60Hz, and it must be bound to the NVIDIA adapter — bound to the
Basic Display Adapter instead, capture still works but the zero-copy path to NVENC
quietly becomes a CPU roundtrip, and you lose most of the latency budget without any
error appearing.

**Two firewalls.** GCP VPC rules and Windows Firewall are independent; traffic needs
both. `provision-vm.ps1` does the VPC side, `setup-firewall.ps1` the guest side. The UDP
range must match the host's `--min-port`/`--max-port` or ICE gathers candidates on ports
nothing has opened.

## Audio

Windows Server VMs generally have no audio endpoint at all, and the Windows Audio
service is disabled by default. `setup-gcp-vm.ps1` starts the service, but WASAPI
loopback still needs something to capture — install a virtual audio device before
attempting PRD Phase 3, or run the host with `--no-audio`.

## Cost

The L4 bills hourly while the instance runs. Stop it when idle; you keep paying only for
the disk.

```powershell
gcloud compute instances stop glsplay-host --zone asia-south1-c
gcloud compute instances start glsplay-host --zone asia-south1-c
```

Note that the external IP changes on restart unless you reserve a static address. The
browser client's signaling URL will need updating each time, so reserving one is worth
it early.
