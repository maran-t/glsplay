'use client';

import { useEffect, useRef, useState } from 'react';
import type { SignalingState } from '@/lib/signaling';

export interface StreamPlayerProps {
  stream: MediaStream | null;
  /** Forwarded so input capture can request Pointer Lock on this element. */
  videoRef: React.RefObject<HTMLVideoElement>;
  connection: RTCPeerConnectionState | 'new';
  signaling: SignalingState;
  signalingDetail: string | undefined;
  signalingUrl: string;
  hostPresent: boolean;
  pointerLocked: boolean;
  error: string | null;
}

/** One row of the connection checklist. */
function Stage({
  label,
  state,
  detail,
}: {
  label: string;
  state: 'pending' | 'active' | 'done' | 'failed';
  detail?: string;
}) {
  const mark = { pending: '·', active: '…', done: '✓', failed: '✗' }[state];
  const tone = {
    pending: 'text-muted/50',
    active: 'text-warn',
    done: 'text-good',
    failed: 'text-bad',
  }[state];
  return (
    <div className="flex items-baseline gap-2.5">
      <span className={`w-3 shrink-0 text-center ${tone}`}>{mark}</span>
      <span className={state === 'pending' ? 'text-muted/50' : 'text-ink'}>{label}</span>
      {detail && <span className="truncate text-muted/70">{detail}</span>}
    </div>
  );
}

/**
 * The video surface.
 *
 * Two browser policies shape this component. Autoplay with audio requires a
 * user gesture, so the first frame sits behind an explicit start control.
 * Pointer Lock also requires a gesture, and the same click satisfies both.
 */
export function StreamPlayer({
  stream,
  videoRef,
  connection,
  signaling,
  signalingDetail,
  signalingUrl,
  hostPresent,
  pointerLocked,
  error,
}: StreamPlayerProps) {
  const containerRef = useRef<HTMLDivElement>(null);
  const [started, setStarted] = useState(false);
  const [playbackError, setPlaybackError] = useState<string | null>(null);

  useEffect(() => {
    const video = videoRef.current;
    if (!video || !stream) return;
    video.srcObject = stream;
    return () => {
      video.srcObject = null;
    };
  }, [stream, videoRef]);

  // The host now blends the pointer straight into the video, so there is no
  // client-side cursor to draw. Just hide the local one while Pointer-Locked
  // (the baked one is authoritative); show a normal arrow otherwise so the
  // "click to start" affordance is reachable.
  useEffect(() => {
    const video = videoRef.current;
    if (!video) return;
    video.style.cursor = pointerLocked ? 'none' : '';
  }, [pointerLocked, videoRef]);

  const start = async () => {
    const video = videoRef.current;
    if (!video) return;
    try {
      // Unmute here rather than in markup: Chrome blocks an unmuted autoplay
      // before any gesture, and a muted-then-unmuted element is the supported
      // way to get audio on the first play.
      video.muted = false;
      await video.play();
      setStarted(true);
      setPlaybackError(null);
      await video.requestPointerLock();
    } catch (err) {
      setPlaybackError(err instanceof Error ? err.message : String(err));
    }
  };

  // Each connection stage is reported separately. Collapsing them into one
  // message makes "never reached the broker" and "broker fine, no host yet"
  // look identical, which is exactly the state you most need to tell apart.
  const brokerState =
    signaling === 'registered' ? 'done'
    : signaling === 'error' ? 'failed'
    : signaling === 'connecting' ? 'active'
    : 'pending';

  const hostState =
    signaling !== 'registered' ? 'pending' : hostPresent ? 'done' : 'active';

  const mediaState =
    connection === 'connected' && stream ? 'done'
    : connection === 'failed' ? 'failed'
    : hostPresent ? 'active'
    : 'pending';

  const headline = (() => {
    if (error) return { text: error, tone: 'text-bad' as const };
    if (signaling === 'error') {
      return { text: 'Cannot reach the signaling broker', tone: 'text-bad' as const };
    }
    if (signaling !== 'registered') {
      return { text: 'Connecting to signaling broker', tone: 'text-warn' as const };
    }
    if (!hostPresent) {
      return { text: 'Waiting for a host to join the room', tone: 'text-warn' as const };
    }
    if (connection === 'failed') {
      return { text: 'Peer connection failed', tone: 'text-bad' as const };
    }
    if (connection === 'disconnected') {
      return { text: 'Reconnecting to host', tone: 'text-warn' as const };
    }
    if (!stream) return { text: 'Host connected, waiting for media', tone: 'text-muted' as const };
    return null;
  })();

  const hint = (() => {
    if (signaling === 'error' || signaling === 'closed') {
      return `Check the broker is running and reachable at ${signalingUrl}`;
    }
    if (signaling !== 'registered') return signalingUrl;
    if (!hostPresent) return 'Start glsplay-host on the VM to begin streaming.';
    if (connection === 'failed') return 'No direct path found - check UDP 50000-50100 on both firewalls.';
    return undefined;
  })();

  const showStartGate = stream !== null && !started;

  // Once the video is actually playing, don't slap a modal over it for a
  // transient signaling reconnect or a brief ICE blip - only for a hard
  // failure or an explicit error.
  const playing = started && stream !== null;
  const showOverlay =
    headline !== null && (!playing || connection === 'failed' || error !== null);

  return (
    <div ref={containerRef} className="relative h-full w-full overflow-hidden bg-void">
      <video
        ref={videoRef}
        className="h-full w-full object-contain"
        autoPlay
        playsInline
        muted
        // The stream is live and unseekable; native controls would only offer
        // a scrubber that does nothing and a fullscreen button that fights
        // Pointer Lock.
        controls={false}
        disablePictureInPicture
        tabIndex={-1}
      />

      {showOverlay && headline && (
        <div className="absolute inset-0 flex items-center justify-center">
          <div className="flex w-[340px] flex-col gap-4 rounded-lg border border-edge bg-panel/92 px-6 py-5">
            <div className="flex flex-col gap-1.5">
              <div className={`font-mono text-sm ${headline.tone}`}>{headline.text}</div>
              {hint && <div className="break-all text-xs leading-relaxed text-muted">{hint}</div>}
              {signalingDetail && signaling !== 'registered' && (
                <div className="break-all font-mono text-[11px] text-bad/80">{signalingDetail}</div>
              )}
            </div>

            <div className="flex flex-col gap-1.5 border-t border-edge pt-3 font-mono text-[11px]">
              <Stage label="Signaling broker" state={brokerState} />
              <Stage label="Host present" state={hostState} />
              <Stage
                label="Media stream"
                state={mediaState}
                detail={connection !== 'new' ? connection : undefined}
              />
            </div>
          </div>
        </div>
      )}

      {showStartGate && (
        <button
          type="button"
          onClick={() => void start()}
          className="absolute inset-0 flex cursor-pointer flex-col items-center justify-center gap-4 bg-void/70 backdrop-blur-sm transition-colors hover:bg-void/60"
        >
          <div className="rounded-full border border-signal/40 bg-signal/10 px-8 py-4 font-mono text-sm text-signal">
            Click to start
          </div>
          <div className="max-w-xs text-center text-xs leading-relaxed text-muted">
            Starts audio and captures your mouse and keyboard.
            <br />
            Press <kbd className="rounded border border-edge px-1 font-mono">Esc</kbd> to release
            them.
          </div>
          {playbackError && <div className="font-mono text-xs text-bad">{playbackError}</div>}
        </button>
      )}

      {started && !pointerLocked && !showOverlay && (
        <button
          type="button"
          onClick={() => void videoRef.current?.requestPointerLock()}
          className="absolute bottom-4 left-1/2 -translate-x-1/2 rounded-md border border-edge bg-panel/90 px-4 py-2 font-mono text-xs text-muted transition-colors hover:text-ink"
        >
          Click to recapture input
        </button>
      )}
    </div>
  );
}
