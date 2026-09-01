# TODO

Open items, roughly in the order they should be dealt with.

> This repo is **public**. Never put a secret, password or token in here — reference
> where it lives instead.

> **Repo split:** `apps/web`, `apps/signaling` and `packages/protocol/src` now live
> in a separate web/signaling/protocol repo (not yet created). Items below that
> reference `apps/web/...` / `apps/signaling/...` / `web/src/...` paths — mostly
> under *Security*, *Correctness*, *Client*, and the *Host — audio* SDP note —
> track work in **that** repo. This repo is host-only: `apps/host/` and the VM
> scripts. The *Simplification — now that the control plane is external* section
> is the split; keep the wire-format-sync item under *Robustness* — it now spans
> two repos and matters more.

---

## Security

- [ ] **The room secret is served publicly and unauthenticated.**
      `apps/web/src/app/api/session/route.ts` returns `{signalingUrl, roomId, secret}`
      to anyone who requests `https://play.resrve.xyz/api/session`. Whoever has it can
      join a live session as the client — and the host injects `SendInput`, so that is
      keyboard and mouse control of the desktop — or claim the host role in any room
      that currently has none.

      This was an acceptable trade while the client was a throwaway IP: the value was
      inlined into the JS bundle anyway, so serving it changed nothing. A permanent
      public domain changes it. The bundle is no longer the exposure — the URL is.

      **Fix:** per-session tokens minted by the control plane after the user
      authenticates, replacing the one shared secret. Short expiry, scoped to one room
      and one role, so a leaked token dies on its own and cannot claim `host`.
      The broker's single `GLSPLAY_ROOM_SECRET` check (`apps/signaling/src/server.ts:35`,
      `secretMatches`) becomes token verification.

      **Interim, if tokens are not close:** put the session endpoint behind basic auth,
      or restrict the Caddy site to known source IPs. Neither is a real answer.

      **Also:** rotate the current secret. It has been pasted into a chat transcript.

---

## Correctness

- [ ] **`/ws/health` returns 404.** Caddy forwards but does not strip the prefix, so the
      broker sees `/ws/health` and falls through to its 404 handler. WebSockets are
      unaffected — `WebSocketServer({ server })` accepts upgrades on any path, which is
      why the 101 works. Change `handle /ws*` to `handle_path /ws*` in the Caddyfile.
      Needed before anything can monitor the broker.

- [ ] **Verify `glsplay-host.exe` is not stale.** The committed binary is dated
      2026-08-28 20:58. Check whether `00b80c4` ("load NVML from nvml.dll at runtime")
      landed after that — if so the GPU and encoder percentages in the stats HUD read 0
      and the committed exe predates the fix. `git log -1 --format=%ci 00b80c4`.

- [ ] **Delete `vm-scripts/run-host.ps1`.** Stale duplicate of the root `run-host.ps1`,
      not updated in Phase 0. It has no `--output` flag, so it captures output 0 — the
      L4's 1280x800 phantom head instead of the 1920x1080 virtual display. Nothing
      references it. It fails by working, which is the worst way.

- [ ] **Delete `vdd/changeres-VDD.ps1`.** Dot-sources `set-dependencies.ps1`, which is
      not in the repo. Fails on contact. Superseded by `vdd/set-vdd-res.ps1`.

---

## Simplification — now that the control plane is external

- [x] **Strip the Node half out of the VM image.** The `build` stage became `check`,
      the Node install is gone, the `glsplay-signaling` and `glsplay-web` tasks are
      unregistered, and `boot.ps1` no longer writes `apps/web/.env`. Two stages that
      could fail a bake — `npm install` needing the network, `next build` running out
      of memory — no longer exist.

- [x] **Close the GPU VM's inbound TCP.** Docs now create only the UDP 50000-50100
      rule. Verify the old `glsplay-signaling` / `glsplay-web` VPC rules are gone
      from the project if nothing else uses them.

- [x] **Remove dead code.** `Test-Reboot` in `provision.ps1` and `Write-GlsplayWebEnv`
      in `session-config.ps1`, both unreferenced.

- [x] **Autologon password out of the plaintext registry.** `provision.ps1` now
      re-applies it with Sysinternals Autologon, which stores it as an LSA secret,
      and clears `DefaultPassword`. `bake-image.ps1` clears both forms.

- [x] **Restart-on-failure for the host task.** Three retries a minute apart. Not a
      substitute for a supervisor service — it cannot relaunch across a session
      change — but it covers the ordinary case of the host exiting.

---

## Robustness

- [ ] **Pinned installer URLs will 404.** `provision.ps1` hardcodes exact Node (:96) and
      Git (:104) download URLs. Both will break when those releases are pruned. Resolve
      the current LTS at runtime, or vendor the installers.

- [ ] **Nothing keeps the wire format in sync across languages.** The opcodes live twice:
      `packages/protocol/include/glsplay_input.h` for the host and
      `packages/protocol/src/input.ts` for web. The header's `static_assert`s catch C++
      drifting against itself but cannot see the TypeScript. Add a test asserting the TS
      `EVENT_SIZE` table against sizes parsed from the header, before the protocol
      starts changing.

- [ ] **Run `provision.ps1` and `bake-image.ps1` against a real instance.** Both are
      untested end to end. Most likely first failure is the `tools` stage (see pinned
      URLs); second is `nefcon` in `display`, which has never run headless here.

---

## Host — streaming runtime

The capture → NVENC → libwebrtc path is complete (zero-copy D3D11, P4+ULL, 0
B-frames, infinite GOP + intra-refresh, spatial AQ, GCC-driven `SetRates` →
`nvEncReconfigureEncoder`). These are the gaps that make a real session brittle.

- [ ] **`request-keyframe` is received and ignored.** `peer_session.cpp` (`OnMessage`,
      the `request-keyframe` branch) only `LOG_DEBUG`s. libwebrtc services RTCP PLI, but
      the client asks explicitly on tab refocus / decode corruption and nothing happens
      — the picture stays broken until the next natural refresh. Wire it to a forced
      refresh on the encoder (`NvencSession` — a one-shot `forceIntraRefresh` / IDR).

- [ ] **`set-bitrate` is received and ignored.** Same place. There is no way for the
      client to cap quality on a metered or shared connection — GCC only ever revises
      *up* to the link estimate. Apply it as a ceiling: `min(gcc_estimate, client_cap)`
      into `NvencSession::Reconfigure`.

- [ ] **A dropped peer is not recovered.** `PeerSession::OnConnectionChange` on
      `kDisconnected` / `kFailed` only calls `input_->ReleaseAll()`. It does not tear
      the peer down and re-arm for a fresh client, and does not re-offer. A browser
      refresh recovers only if the `renegotiate` path happens to fire. Needs an explicit
      "close peer, wait for a new client, re-offer" cycle.

- [ ] **Capture cadence is unstable, which inflates the client jitter buffer.**
      `host.log` shows captured FPS bouncing 46–59 (target 60) with per-frame capture
      time spiking 0.3 ms → ~20 ms. Irregular frame delivery is jitter the browser's
      receive buffer then has to absorb, so it sizes itself larger — this couples
      directly to the *Client — playout & transport* item below. Find why the loop
      misses 60 (AcquireNextFrame timeout handling? the composite/copy path? thread
      priority under encoder load?) and make delivery steady. Also worth it for
      smoothness on its own.

## Host — quality & features

- [ ] **No dynamic resolution.** `DesktopCaptureSource::AddOrUpdateSink` deliberately
      ignores `VideoSinkWants` resolution, so a poor link gets blocky 1080p instead of
      crisp 720p — only bitrate adapts. Needs a GPU scale before NVENC (gives up strict
      zero-copy for one blit; still no CPU roundtrip). Biggest single quality win on a
      real internet path.

- [ ] **No adaptive frame rate.** The capture loop is pinned to `target_fps`; a weak
      link cannot fall back to 30 to protect motion clarity.

- [ ] **H.264 8-bit 4:2:0 only.** The L4's NVENC also does HEVC and AV1; neither is
      offered. No 10-bit / HDR. The 1440p60 stretch target has never been run.

- [ ] **No long-term reference (LTR) frames.** Loss recovery is a full intra-refresh
      sweep; LTR would let the decoder re-sync from an older good frame far faster.

- [ ] **Gamepad rumble is one-way.** ViGEm raises force-feedback notifications from the
      game; nothing forwards them to the client's `GamepadHapticActuator`. Needs a
      host→client control message and a `vigem_target_x360_register_notification` hook.

- [ ] **No clipboard sync.** Copy/paste between the local machine and the streamed
      desktop. Text-only over the control channel is a small, high-value start.

## Host — audio

The pipeline exists (WASAPI loopback → custom `AudioDeviceModule` → libwebrtc
Opus; the answer SDP sets `useinbandfec=1`, `minptime=10`, `usedtx=0`,
`maxaveragebitrate` in `web/src/lib/sdp.ts`). It has effectively never run on the
target.

- [ ] **Audio has never run on the real VM.** GCP Windows Server has no audio endpoint;
      `wasapi_loopback.cpp` logs "Install a virtual audio device, or run with
      `--no-audio`" and every documented run uses `--no-audio`. Nothing in
      `provision.ps1` / the docs installs one — the same gap the virtual *display*
      driver had. Pick a virtual audio device (VB-CABLE / Scream / a signed driver), add
      it to provisioning, bind loopback to it, and confirm the stream carries sound.

- [ ] **A/V sync never validated (PRD §7.3, < 10 ms drift).** The audio path runs on its
      own timeline with no alignment to the video capture timestamps. Measure drift with
      a real game once audio works on the VM.

- [ ] **10 ms framing may be pinned to 48 kHz.** `LoopbackAudioDevice::frames_per_10ms_`
      defaults to 480, but `WasapiLoopback` uses whatever rate the endpoint mix format
      reports (`GetMixFormat`, often 44100). If the framing isn't recomputed from the
      real rate, "10 ms" chunks are ~10.9 ms → slow drift. Verify `Deliver()` uses the
      passed `sample_rate`.

- [ ] **No audio bitrate adaptation.** Opus is fixed; on a constrained link video backs
      off via GCC but audio does not. Opus 96 kHz stretch target (PRD §2) also untried.

---

## Client — playout & transport

- [ ] **Playout / jitter buffer adds avoidable latency — likely the biggest single
      win.** The client never touches the receive buffer, so Chrome runs its default
      video jitter buffer (~60–120 ms of hold on a clean path; in the HUD it reads as
      the largest latency line, well above RTT). On the video `RTCRtpReceiver` set
      `jitterBufferTarget = 0` (Chrome 114+; it clamps to a safe minimum but runs far
      tighter — expect ~15–25 ms) and `playoutDelayHint = 0` as the older fallback.
      Wire it in `useWebRTC.ts` `ontrack` from `ev.receiver`. If that isn't enough, the
      host can additionally negotiate the `playout-delay` RTP header extension to cap it
      sender-side. Pairs with the *capture cadence* item above — a steadier 60 fps lets
      the buffer target shrink on its own.

      RTT itself (~20 ms) is not reducible — it is the Chennai↔Mumbai hop and
      `asia-south1` is already Mumbai. Only client-side checks apply: wired not WiFi,
      and the nominated candidate pair is `host`, not `relay`.

- [ ] **No TURN server.** `HostConfig` has `turn_url` / `turn_username` /
      `turn_credential` fields but nothing is deployed. The host has a public IP so its
      host candidate usually wins, but a client behind symmetric NAT or a strict
      corporate firewall cannot connect at all. Stand up a `coturn` and wire the creds.

- [ ] **Reconnect resumes nothing.** The "dropped peer" item under *Host — streaming
      runtime* re-arms for a *fresh* client. There is no path to rejoin the *same*
      running session (game still running, state intact) after a client network blip
      within a grace window — which is what every cloud gaming client does.

---

## Known constraints

Not tasks — things that must be understood before "run a real game" (Phase 5).

- [ ] **Anti-cheat will reject this setup.** `SendInput` events carry `LLMHF_INJECTED`;
      EAC / BattlEye / Vanguard also flag virtual displays, ViGEm virtual gamepads, and
      VM environments, and will refuse to launch or ban the account. A large share of
      multiplayer titles are therefore off the table without publisher whitelisting
      (which is how GeForce NOW works). Decide the target game set with this in mind, or
      invest in a kernel-level virtual HID + a cleaner display path — a project in
      itself.

- [ ] **Some DRM / launchers fail under Desktop Duplication or on a virtual display.**
      Protected-content paths can render black or refuse. Verify per launcher (Steam,
      Epic, Xbox) during Phase 5.

## Game layer

The piece that separates cloud *gaming* from remote desktop. None of it exists —
today you RDP in and launch by hand; the window herder only drags the window
onto the captured display.

- [ ] **Game launch & lifecycle.** A catalogue, "select game → it launches on the VM",
      per-title launch args / settings, and process watching so "game exited" ends the
      session cleanly. Drives what the session API allocates and tears down.

- [ ] **Persistent user state.** User identity beyond a room id, and save-game / profile
      persistence across sessions — the VM is ephemeral, so this needs external storage
      (a per-user disk or object store mounted at session start).

- [ ] **Fleet, not one VM.** A pool of instances, a queue when full, capacity /
      autoscale, and region routing. The session API item currently implies a single
      instance; a product needs allocation across many, plus handling GCP spot / host
      preemption mid-session.

---

## Validation

- [ ] **Phase 5 — real 3D game glass-to-glass benchmark (PRD §7.5).** Everything measured
      so far is desktop / synthetic. Launch an actual DirectX or Vulkan title on the VM
      and measure end-to-end latency (high-speed camera or input-loopback), frame
      stability, and encoder GPU utilisation against the PRD §2 KPIs. **The ≤45 ms
      glass-to-glass target has never been measured.** This is the point of the POC and
      should come before more feature work — it says where the remaining budget goes.

- [ ] **Client latency-vs-budget HUD.** The host already reports every piece
      (`captureMs`, `encodeMs`, `inputQueueMs`) and the browser has RTT / jitter-buffer
      / decode from `getStats()`. Nothing assembles them into the PRD §5 breakdown so a
      regression is visible at a glance.

---

## Image lifecycle

- [ ] **A baked image runs stale code and nothing says so.** The image carries a
      `git clone` pinned to the commit it was baked from, and the `tools` stage that
      would update it is skipped on image boot (`bake-image.ps1` rewinds to `account`).
      So pushing to the branch does not reach existing images, and the symptom is a
      fix that appears not to work.

      Options: a `git pull` in `boot.ps1`; or stamp the baked commit somewhere
      `boot.log` prints, so the running code is at least identifiable. Pulling at boot
      trades reproducibility for freshness — an instance would no longer be pinned to
      what was tested — so stamping is probably the safer first move.

## Next phase

- [ ] **Replace the host's scheduled task with a supervisor service.** A Windows service
      in session 0 that calls `WTSGetActiveConsoleSessionId` → `WTSQueryUserToken` →
      `CreateProcessAsUser` to launch the host into the console session, and restarts it
      on crash or session change. This is what Parsec and Sunshine do. The logon-trigger
      task works, but a control plane needs something it can start, stop and query.

- [ ] **Session API.** `POST /sessions` allocates an instance and returns connect
      details; idle timeout tears it down. This is what makes the token work above
      possible, and what turns a VM into a product.

---

## Housekeeping

- [ ] **Resolve the `detailed_prd.md` rename.** It shows deleted with an untracked
      `detailed_prd (old).md` alongside — identical content, CRLF line endings only.
      Either restore it (`git checkout -- detailed_prd.md && rm "detailed_prd (old).md"`)
      or decide it is superseded.
