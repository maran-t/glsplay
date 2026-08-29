'use client';

import { useCallback, useEffect, useRef, useState } from 'react';
import {
  CHANNEL_CONTROL,
  CHANNEL_INPUT,
  parseControlMessage,
  type BrokerToClientMessage,
  type ClientToHostControl,
  type HostHelloMessage,
  type HostStatsMessage,
  type InputBytes,
} from '@glsplay/protocol';
import { SignalingClient, type SignalingState } from '@/lib/signaling';
import { describeNegotiatedVideo, preferH264, shapeAnswer } from '@/lib/sdp';

export interface WebRTCConfig {
  signalingUrl: string;
  roomId: string;
  secret: string;
  /** Ceiling advertised to the host via b=AS in our answer. */
  maxBitrateKbps: number;
  iceServers: RTCIceServer[];
}

/** The remote pointer, rendered client-side. `url` is null until the first shape arrives. */
export interface CursorInfo {
  url: string | null;
  /** Cursor top-left in host desktop pixels (hotspot already applied by Windows). */
  x: number;
  y: number;
  /** Shape image size in pixels. */
  width: number;
  height: number;
  hotspotX: number;
  hotspotY: number;
  visible: boolean;
}

export interface WebRTCState {
  signaling: SignalingState;
  signalingDetail?: string;
  connection: RTCPeerConnectionState | 'new';
  ice: RTCIceConnectionState | 'new';
  hostPresent: boolean;
  hello: HostHelloMessage | null;
  hostStats: HostStatsMessage | null;
  negotiatedVideo: string | null;
  cursor: CursorInfo | null;
  error: string | null;
}

const INITIAL: WebRTCState = {
  signaling: 'idle',
  connection: 'new',
  ice: 'new',
  hostPresent: false,
  hello: null,
  hostStats: null,
  negotiatedVideo: null,
  cursor: null,
  error: null,
};

/**
 * Owns the RTCPeerConnection and the signaling socket.
 *
 * The host is always the offerer. Making the browser answer-only removes an
 * entire class of glare bugs, and the host is the side that knows its encoder
 * configuration, so it has strictly more information when composing an offer.
 */
export function useWebRTC(config: WebRTCConfig | null) {
  const [state, setState] = useState<WebRTCState>(INITIAL);
  const [stream, setStream] = useState<MediaStream | null>(null);

  const pcRef = useRef<RTCPeerConnection | null>(null);
  const signalRef = useRef<SignalingClient | null>(null);
  const inputChannelRef = useRef<RTCDataChannel | null>(null);
  const controlChannelRef = useRef<RTCDataChannel | null>(null);
  /**
   * Candidates that arrive before setRemoteDescription. Applying one early
   * throws, and Chrome delivers them aggressively during ICE trickle, so they
   * are parked here and flushed once the remote description lands.
   */
  const pendingCandidates = useRef<RTCIceCandidateInit[]>([]);
  /** Last decoded cursor shape as a data URL; reused when only visibility flips. */
  const lastCursorUrl = useRef<string | null>(null);

  const patch = useCallback((next: Partial<WebRTCState>) => {
    setState((prev) => ({ ...prev, ...next }));
  }, []);

  /** Sends a control message to the host. False if the channel is not open. */
  const sendControl = useCallback((msg: ClientToHostControl): boolean => {
    const channel = controlChannelRef.current;
    if (channel?.readyState !== 'open') return false;
    channel.send(JSON.stringify(msg));
    return true;
  }, []);

  /** Sends packed binary input. False if the channel is not open. */
  const sendInput = useCallback((bytes: InputBytes): boolean => {
    const channel = inputChannelRef.current;
    if (channel?.readyState !== 'open') return false;
    // Backpressure guard. If SCTP is congested, dropping input is correct -
    // queuing it would deliver stale positions late, which feels far worse
    // than a missed frame of movement.
    if (channel.bufferedAmount > 64 * 1024) return false;
    channel.send(bytes);
    return true;
  }, []);

  useEffect(() => {
    if (!config) return;

    let disposed = false;
    const pc = new RTCPeerConnection({
      iceServers: config.iceServers,
      // The host has a public IP, so a direct host candidate should win. All
      // is kept rather than forcing relay-free in case a corporate network
      // needs TURN later.
      iceTransportPolicy: 'all',
      bundlePolicy: 'max-bundle',
      rtcpMuxPolicy: 'require',
    });
    pcRef.current = pc;

    const wireChannel = (channel: RTCDataChannel) => {
      if (channel.label === CHANNEL_INPUT) {
        channel.binaryType = 'arraybuffer';
        inputChannelRef.current = channel;
      } else if (channel.label === CHANNEL_CONTROL) {
        controlChannelRef.current = channel;
        channel.onmessage = (ev: MessageEvent<string>) => {
          const msg = parseControlMessage(ev.data);
          if (!msg) return;
          if (msg.type === 'hello') patch({ hello: msg });
          else if (msg.type === 'host-stats') patch({ hostStats: msg });
          else if (msg.type === 'cursor') {
            if (msg.rgbaBase64 && msg.width > 0 && msg.height > 0) {
              try {
                const bin = atob(msg.rgbaBase64);
                const bytes = new Uint8ClampedArray(bin.length);
                for (let i = 0; i < bin.length; i += 1) bytes[i] = bin.charCodeAt(i);
                const canvas = document.createElement('canvas');
                canvas.width = msg.width;
                canvas.height = msg.height;
                const ctx = canvas.getContext('2d');
                if (ctx) {
                  ctx.putImageData(new ImageData(bytes, msg.width, msg.height), 0, 0);
                  lastCursorUrl.current = canvas.toDataURL('image/png');
                }
              } catch {
                // A malformed shape just means we keep drawing the previous one.
              }
            }
            patch({
              cursor: {
                url: lastCursorUrl.current,
                x: msg.x,
                y: msg.y,
                width: msg.width,
                height: msg.height,
                hotspotX: msg.hotspotX,
                hotspotY: msg.hotspotY,
                visible: msg.visible,
              },
            });
          }
        };
      }
    };

    // The host creates both channels, so we receive them rather than opening.
    pc.ondatachannel = (ev: RTCDataChannelEvent) => wireChannel(ev.channel);

    pc.ontrack = (ev: RTCTrackEvent) => {
      const [first] = ev.streams;
      if (first) setStream(first);
    };

    pc.onconnectionstatechange = () => {
      if (disposed) return;
      patch({ connection: pc.connectionState });
    };

    pc.oniceconnectionstatechange = () => {
      if (disposed) return;
      patch({ ice: pc.iceConnectionState });
      // A failed ICE state will not recover on its own; ask the host to
      // restart rather than leaving a dead connection on screen.
      if (pc.iceConnectionState === 'failed') {
        signalRef.current?.send({ type: 'renegotiate', reason: 'recovery' });
      }
    };

    pc.onicecandidate = (ev: RTCPeerConnectionIceEvent) => {
      if (!ev.candidate) return;
      signalRef.current?.send({
        type: 'candidate',
        candidate: {
          candidate: ev.candidate.candidate,
          sdpMid: ev.candidate.sdpMid,
          sdpMLineIndex: ev.candidate.sdpMLineIndex,
          usernameFragment: ev.candidate.usernameFragment,
        },
      });
    };

    const flushCandidates = async () => {
      const queued = pendingCandidates.current;
      pendingCandidates.current = [];
      for (const candidate of queued) {
        try {
          await pc.addIceCandidate(candidate);
        } catch (err) {
          // A single bad candidate is not fatal - others may still connect.
          console.warn('addIceCandidate failed', err);
        }
      }
    };

    // The host re-offers on every `renegotiate`, and one can land while the
    // previous answer is still being built - which used to fire
    // setLocalDescription in the `stable` state and abort negotiation (taking
    // the data channels, and thus input, down with it). Serialize: `pendingOffer`
    // always holds the newest offer, and the loop drains it.
    let negotiating = false;
    let pendingOffer: string | null = null;

    const handleOffer = async (sdp: string) => {
      pendingOffer = sdp;
      if (negotiating) return;
      negotiating = true;
      try {
        while (pendingOffer !== null && !disposed) {
          const offer = pendingOffer;
          pendingOffer = null;

          // Valid from both `stable` and `have-remote-offer` (a re-offer just
          // replaces the pending one) - so no rollback dance is needed for an
          // answer-only peer.
          await pc.setRemoteDescription({ type: 'offer', sdp: offer });
          if (pc.signalingState !== 'have-remote-offer') continue;

          await flushCandidates();

          // Codec preferences must be set after the transceivers exist, which
          // setRemoteDescription is what creates for a recvonly client.
          for (const transceiver of pc.getTransceivers()) {
            if (transceiver.receiver.track?.kind === 'video') preferH264(transceiver);
          }

          const answer = await pc.createAnswer();
          const shaped = shapeAnswer(answer.sdp ?? '', {
            maxBitrateKbps: config.maxBitrateKbps,
          });

          // A newer offer arrived, or the state moved under us while building
          // the answer - drop this one and negotiate against what's current.
          if (pendingOffer !== null || pc.signalingState !== 'have-remote-offer') {
            continue;
          }

          await pc.setLocalDescription({ type: 'answer', sdp: shaped });
          signalRef.current?.send({ type: 'answer', sdp: shaped });
          patch({ negotiatedVideo: describeNegotiatedVideo(shaped), error: null });
        }
      } catch (err) {
        patch({ error: err instanceof Error ? err.message : String(err) });
      } finally {
        negotiating = false;
      }
    };

    const onMessage = (msg: BrokerToClientMessage) => {
      if (disposed) return;
      switch (msg.type) {
        case 'registered':
          patch({ hostPresent: msg.peerPresent });
          // If the host is already waiting, prompt it to offer immediately
          // instead of waiting for its own peer-state notification.
          if (msg.peerPresent) {
            signalRef.current?.send({ type: 'renegotiate', reason: 'initial' });
          }
          break;
        case 'peer-state':
          if (msg.role === 'host') {
            patch({ hostPresent: msg.present });
            if (msg.present) {
              signalRef.current?.send({ type: 'renegotiate', reason: 'initial' });
            }
          }
          break;
        case 'offer':
          void handleOffer(msg.sdp);
          break;
        case 'candidate':
          if (pc.remoteDescription) {
            void pc.addIceCandidate(msg.candidate).catch((err: unknown) => {
              console.warn('addIceCandidate failed', err);
            });
          } else {
            pendingCandidates.current.push(msg.candidate);
          }
          break;
        case 'error':
          patch({ error: `${msg.code}: ${msg.message}` });
          break;
        default:
          break;
      }
    };

    const client = new SignalingClient({
      url: config.signalingUrl,
      roomId: config.roomId,
      secret: config.secret,
      handlers: {
        onState: (signaling, signalingDetail) => {
          if (disposed) return;
          setState((prev) => ({ ...prev, signaling, signalingDetail }));
        },
        onMessage,
      },
    });
    signalRef.current = client;
    client.connect();

    return () => {
      disposed = true;
      client.close();
      inputChannelRef.current = null;
      controlChannelRef.current = null;
      pendingCandidates.current = [];
      pc.ontrack = null;
      pc.ondatachannel = null;
      pc.onicecandidate = null;
      pc.close();
      pcRef.current = null;
      signalRef.current = null;
      setStream(null);
      setState(INITIAL);
    };
  }, [config, patch]);

  return {
    state,
    stream,
    peerConnection: pcRef,
    sendInput,
    sendControl,
  };
}
