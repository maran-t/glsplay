/**
 * JSON control-plane messages exchanged over the WebSocket signaling broker.
 *
 * The broker is deliberately dumb: it authenticates, pairs exactly one host
 * with one client inside a room, and relays opaque SDP/ICE payloads between
 * them. It never parses SDP, and once ICE completes it is out of the media
 * path entirely. See PRD section 4.5.
 */

export type PeerRole = 'host' | 'client';

/** Sent first by both peers. The broker replies with registered, or an error. */
export interface RegisterMessage {
  type: 'register';
  role: PeerRole;
  roomId: string;
  /** Shared secret. Checked before any relaying is possible. */
  secret: string;
  /** Free-form; surfaced in broker logs to make multi-machine debugging sane. */
  agent?: string;
}

export interface RegisteredMessage {
  type: 'registered';
  role: PeerRole;
  roomId: string;
  /** Stable id the broker assigned this peer, echoed in its logs. */
  peerId: string;
  /** True when the opposite role is already present in the room. */
  peerPresent: boolean;
}

/** Broker to peer. Fires when the opposite role joins or drops. */
export interface PeerStateMessage {
  type: 'peer-state';
  role: PeerRole;
  present: boolean;
}

export interface OfferMessage {
  type: 'offer';
  sdp: string;
}

export interface AnswerMessage {
  type: 'answer';
  sdp: string;
}

export interface CandidateMessage {
  type: 'candidate';
  candidate: {
    candidate: string;
    sdpMid: string | null;
    sdpMLineIndex: number | null;
    usernameFragment?: string | null;
  };
}

/**
 * Client asks the host to produce a fresh offer without tearing down the
 * WebSocket. Used on resolution or bitrate changes and on ICE recovery.
 */
export interface RenegotiateMessage {
  type: 'renegotiate';
  reason: 'initial' | 'bitrate' | 'resolution' | 'recovery';
  video?: { width?: number; height?: number; fps?: number; bitrateKbps?: number };
}

/** Application-level keepalive. The broker answers these itself. */
export interface PingMessage {
  type: 'ping';
  t: number;
}

export interface PongMessage {
  type: 'pong';
  t: number;
  serverTime: number;
}

export interface ErrorMessage {
  type: 'error';
  code:
    | 'bad-json'
    | 'bad-secret'
    | 'bad-role'
    | 'not-registered'
    | 'no-peer'
    | 'rate-limited'
    | 'role-taken';
  message: string;
}

export type ClientToBrokerMessage =
  | RegisterMessage
  | OfferMessage
  | AnswerMessage
  | CandidateMessage
  | RenegotiateMessage
  | PingMessage;

export type BrokerToClientMessage =
  | RegisteredMessage
  | PeerStateMessage
  | OfferMessage
  | AnswerMessage
  | CandidateMessage
  | RenegotiateMessage
  | PongMessage
  | ErrorMessage;

export type SignalingMessage = ClientToBrokerMessage | BrokerToClientMessage;

/** Messages the broker forwards verbatim to the opposite peer. */
const RELAYED = new Set(['offer', 'answer', 'candidate', 'renegotiate']);

export function isRelayedType(type: string): boolean {
  return RELAYED.has(type);
}

/**
 * Structural validation. The broker is internet-facing, so every field that
 * gets read is checked before use rather than trusted from a type alias, which
 * erases at runtime and guarantees nothing.
 */
export function parseSignalingMessage(raw: string): SignalingMessage | null {
  let value: unknown;
  try {
    value = JSON.parse(raw);
  } catch {
    return null;
  }
  if (typeof value !== 'object' || value === null) return null;
  const msg = value as Record<string, unknown>;
  if (typeof msg['type'] !== 'string') return null;

  switch (msg['type']) {
    case 'register':
      if (msg['role'] !== 'host' && msg['role'] !== 'client') return null;
      if (typeof msg['roomId'] !== 'string' || msg['roomId'].length === 0) return null;
      if (msg['roomId'].length > 64) return null;
      if (typeof msg['secret'] !== 'string') return null;
      break;
    case 'offer':
    case 'answer':
      if (typeof msg['sdp'] !== 'string' || msg['sdp'].length === 0) return null;
      break;
    case 'candidate': {
      const c = msg['candidate'];
      if (typeof c !== 'object' || c === null) return null;
      if (typeof (c as Record<string, unknown>)['candidate'] !== 'string') return null;
      break;
    }
    case 'renegotiate':
      if (typeof msg['reason'] !== 'string') return null;
      break;
    case 'ping':
      if (typeof msg['t'] !== 'number') return null;
      break;
    default:
      return null;
  }
  return msg as unknown as SignalingMessage;
}
