/**
 * JSON messages on the reliable, ordered "control" DataChannel.
 *
 * Separate from the "input" channel deliberately: control traffic is rare and
 * must not be lost, whereas input is high-rate and must never head-of-line
 * block. Sharing one channel would let a dropped control message stall every
 * mouse event queued behind it.
 */

/** Host capability report, sent once when the channel opens. */
export interface HostHelloMessage {
  type: 'hello';
  hostVersion: string;
  /** Encoder identity, e.g. "NVENC H.264 High" plus driver version. */
  encoder: string;
  gpu: string;
  display: { width: number; height: number; refreshHz: number };
  /** False when running with NullGamepadSink because ViGEmBus is absent. */
  gamepadAvailable: boolean;
  /** Whether a real desktop was captured, or a synthetic test pattern. */
  captureSource: 'dxgi' | 'test-pattern';
}

/**
 * Periodic host telemetry. The browser cannot observe encode time or GPU load
 * through getStats(), so the host reports them here and the HUD merges both
 * sources into one latency breakdown against the PRD section 5 budget.
 */
export interface HostStatsMessage {
  type: 'host-stats';
  /** Host monotonic clock, milliseconds. */
  t: number;
  capturedFps: number;
  encodedFps: number;
  /** Mean DXGI AcquireNextFrame to encoder submit, milliseconds. */
  captureMs: number;
  /** Mean NVENC submit to bitstream ready, milliseconds. */
  encodeMs: number;
  encoderBitrateKbps: number;
  gpuUtilPercent: number;
  encoderUtilPercent: number;
  /** Frames the capture loop skipped because the encoder was still busy. */
  droppedFrames: number;
  /** Mean age of input events on arrival at the host, milliseconds. */
  inputQueueMs: number;
}

export interface ControlPingMessage {
  type: 'ping';
  tClient: number;
}

/** Reply to a ping, carrying the host clock for one-way delay estimation. */
export interface ControlPongMessage {
  type: 'pong';
  tClient: number;
  tHost: number;
}

/** Retargets the encoder without renegotiating. */
export interface SetBitrateMessage {
  type: 'set-bitrate';
  bitrateKbps: number;
}

/**
 * Deliberate resync, e.g. on tab refocus. libwebrtc already sends RTCP PLI on
 * decode failure, so this is not the normal recovery path.
 */
export interface RequestKeyframeMessage {
  type: 'request-keyframe';
}

/** Relative drives games through Pointer Lock; absolute is for desktop use. */
export interface SetPointerModeMessage {
  type: 'set-pointer-mode';
  mode: 'relative' | 'absolute';
}

/**
 * Host tells the client to drop every held key. Sent on pointer-lock exit so
 * a key held at that moment does not stay latched down on the remote desktop.
 */
export interface ReleaseAllInputMessage {
  type: 'release-all-input';
}

/**
 * The remote pointer. The video stream never contains the cursor - the host
 * hides it from capture and reports it here instead, so the client can draw it
 * locally with zero latency and no encoder ghosting (the standard approach in
 * RDP, Parsec, Moonlight). `rgbaBase64` is present only when the shape itself
 * changed; a bare visibility flip omits it.
 */
export interface CursorUpdateMessage {
  type: 'cursor';
  visible: boolean;
  /** Cursor top-left in host desktop pixels. Used to place the overlay while
   *  Pointer Lock is active (there is no local mouse position to follow then). */
  x: number;
  y: number;
  /** Hotspot offset within the shape image, in shape pixels. */
  hotspotX: number;
  hotspotY: number;
  /** Decoded shape dimensions in pixels. */
  width: number;
  height: number;
  /** Raw RGBA8888 pixels, row-major, base64. Omitted when the shape is unchanged. */
  rgbaBase64?: string;
}

export type HostToClientControl =
  | HostHelloMessage
  | HostStatsMessage
  | ControlPongMessage
  | ReleaseAllInputMessage
  | CursorUpdateMessage;

export type ClientToHostControl =
  | ControlPingMessage
  | SetBitrateMessage
  | RequestKeyframeMessage
  | SetPointerModeMessage;

export type ControlMessage = HostToClientControl | ClientToHostControl;

/** Channel labels. Both peers must agree; the host creates both. */
export const CHANNEL_INPUT = 'input';
export const CHANNEL_CONTROL = 'control';

/**
 * Input channel: unordered with no retransmits, so a lost mouse packet is
 * skipped rather than stalling everything behind it (PRD section 4.4A). A late
 * input is worse than a missing one - the position it describes is already
 * stale by the time it arrives.
 */
export const INPUT_CHANNEL_INIT = {
  ordered: false,
  maxRetransmits: 0,
  protocol: 'glsplay-input-v1',
} as const;

export const CONTROL_CHANNEL_INIT = {
  ordered: true,
  protocol: 'glsplay-control-v1',
} as const;

export function parseControlMessage(raw: string): ControlMessage | null {
  let value: unknown;
  try {
    value = JSON.parse(raw);
  } catch {
    return null;
  }
  if (typeof value !== 'object' || value === null) return null;
  if (typeof (value as Record<string, unknown>)['type'] !== 'string') return null;
  return value as ControlMessage;
}
