'use client';

import { useEffect, useMemo, useRef, useState } from 'react';
import { StreamPlayer } from '@/components/StreamPlayer';
import { StatsOverlay } from '@/components/StatsOverlay';
import { useInputCapture } from '@/hooks/useInputCapture';
import { useStreamStats } from '@/hooks/useStreamStats';
import { useWebRTC, type WebRTCConfig } from '@/hooks/useWebRTC';

/** PRD section 2 targets 15-25 Mbps CBR; the ceiling allows the stretch case. */
const DEFAULT_MAX_BITRATE_KBPS = 25_000;

export default function Page() {
  const videoRef = useRef<HTMLVideoElement>(null);
  const mainRef = useRef<HTMLElement>(null);
  const [hudVisible, setHudVisible] = useState(false);
  const [isFullscreen, setIsFullscreen] = useState(false);

  const toggleFullscreen = () => {
    if (document.fullscreenElement) {
      void document.exitFullscreen();
    } else {
      void mainRef.current?.requestFullscreen();
    }
  };

  useEffect(() => {
    const onChange = () => setIsFullscreen(document.fullscreenElement !== null);
    document.addEventListener('fullscreenchange', onChange);
    return () => document.removeEventListener('fullscreenchange', onChange);
  }, []);

  const config = useMemo<WebRTCConfig | null>(() => {
    const signalingUrl = process.env['NEXT_PUBLIC_SIGNALING_URL'];
    const roomId = process.env['NEXT_PUBLIC_ROOM_ID'];
    const secret = process.env['NEXT_PUBLIC_ROOM_SECRET'];
    if (!signalingUrl || !roomId || !secret) return null;
    return {
      signalingUrl,
      roomId,
      secret,
      maxBitrateKbps: DEFAULT_MAX_BITRATE_KBPS,
      // STUN only. The host has a public IP, so its host candidate should win
      // outright - a relay would add a hop the latency budget cannot absorb.
      iceServers: [{ urls: 'stun:stun.l.google.com:19302' }],
    };
  }, []);

  const { state, stream, peerConnection, sendInput, sendControl } = useWebRTC(config);
  const stats = useStreamStats(peerConnection);

  const input = useInputCapture({
    target: videoRef,
    sendInput,
    sendControl,
    enabled: state.connection === 'connected',
  });

  // F1 toggles the HUD. Handled here rather than in useInputCapture because it
  // is a client-side control that must never reach the host.
  useEffect(() => {
    const onKey = (ev: KeyboardEvent) => {
      if (ev.code !== 'F1') return;
      ev.preventDefault();
      setHudVisible((v) => !v);
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);

  if (!config) {
    return (
      <main className="flex h-full items-center justify-center p-8">
        <div className="max-w-md rounded-lg border border-edge bg-panel p-6">
          <h1 className="mb-3 font-mono text-sm text-bad">Configuration missing</h1>
          <p className="mb-4 text-sm leading-relaxed text-muted">
            Copy <code className="font-mono text-ink">.env.example</code> to{' '}
            <code className="font-mono text-ink">.env</code> at the repository root and set the
            values below, then restart the dev server.
          </p>
          <ul className="flex flex-col gap-1 font-mono text-xs text-muted">
            <li>NEXT_PUBLIC_SIGNALING_URL</li>
            <li>NEXT_PUBLIC_ROOM_ID</li>
            <li>NEXT_PUBLIC_ROOM_SECRET</li>
          </ul>
        </div>
      </main>
    );
  }

  return (
    <main ref={mainRef} className="relative h-full w-full bg-void">
      <StreamPlayer
        stream={stream}
        videoRef={videoRef}
        connection={state.connection}
        signaling={state.signaling}
        signalingDetail={state.signalingDetail}
        signalingUrl={config.signalingUrl}
        hostPresent={state.hostPresent}
        pointerLocked={input.pointerLocked}
        cursor={state.cursor}
        captureSize={
          state.hello ? { w: state.hello.display.width, h: state.hello.display.height } : null
        }
        error={state.error}
      />

      <StatsOverlay
        stats={stats}
        hostStats={state.hostStats}
        hello={state.hello}
        negotiatedVideo={state.negotiatedVideo}
        pointerLocked={input.pointerLocked}
        gamepads={input.gamepads}
        droppedInput={input.dropped}
        visible={hudVisible}
      />

      <div className="absolute bottom-3 right-3 z-10 flex gap-2 font-mono text-[11px]">
        <button
          type="button"
          onClick={() => setHudVisible((v) => !v)}
          className="rounded-md border border-edge bg-panel/90 px-3 py-1.5 text-muted transition-colors hover:text-ink"
        >
          {hudVisible ? 'Hide stats' : 'Stats'}
        </button>
        <button
          type="button"
          onClick={toggleFullscreen}
          className="rounded-md border border-edge bg-panel/90 px-3 py-1.5 text-muted transition-colors hover:text-ink"
        >
          {isFullscreen ? 'Exit full screen' : 'Full screen'}
        </button>
      </div>

      {state.signaling === 'error' && (
        <div className="pointer-events-none absolute bottom-3 left-3 font-mono text-[10px] text-bad/80">
          signaling: {state.signalingDetail ?? 'error'}
        </div>
      )}
    </main>
  );
}
