import type { WebSocket } from 'ws';
import type { PeerRole } from '@glsplay/protocol';
import { log } from './logger.js';

/**
 * A room pairs exactly one host with one client. The POC is deliberately 1:1 -
 * an SFU would insert a relay hop and blow the latency budget in PRD section 5
 * before a single frame was encoded.
 */
export interface Peer {
  readonly id: string;
  readonly role: PeerRole;
  readonly roomId: string;
  readonly socket: WebSocket;
  readonly agent: string;
  readonly connectedAt: number;
  registered: boolean;
  /** Liveness flag driven by the WebSocket ping/pong heartbeat. */
  alive: boolean;
  /** Sliding-window counters for the per-peer rate limiter. */
  windowStart: number;
  windowCount: number;
}

export interface Room {
  readonly id: string;
  host: Peer | null;
  client: Peer | null;
}

const rooms = new Map<string, Room>();
let peerCounter = 0;

export function nextPeerId(role: PeerRole): string {
  peerCounter += 1;
  return `${role}-${peerCounter.toString(36)}`;
}

export function getOrCreateRoom(id: string): Room {
  let room = rooms.get(id);
  if (!room) {
    room = { id, host: null, client: null };
    rooms.set(id, room);
    log.info('room created', { room: id });
  }
  return room;
}

/**
 * Seats a peer in its role slot, returning any peer it displaced.
 *
 * A second peer claiming an occupied role evicts the incumbent rather than
 * being refused. The common case in development is a host that died without
 * closing its socket cleanly; rejecting the reconnect would wedge the room
 * until TCP keepalive eventually noticed, which can take minutes.
 */
export function seatPeer(room: Room, peer: Peer): Peer | null {
  const incumbent = room[peer.role];
  room[peer.role] = peer;
  if (incumbent && incumbent.id !== peer.id) {
    log.warn('evicting stale peer', { room: room.id, role: peer.role, evicted: incumbent.id });
    return incumbent;
  }
  return null;
}

export function removePeer(peer: Peer): Room | null {
  const room = rooms.get(peer.roomId);
  if (!room) return null;
  // Only clear the slot if this peer still owns it. An evicted peer whose
  // close event arrives late must not knock out the peer that replaced it.
  if (room[peer.role]?.id === peer.id) room[peer.role] = null;
  if (!room.host && !room.client) {
    rooms.delete(room.id);
    log.info('room destroyed', { room: room.id });
  }
  return room;
}

export function opposite(room: Room, role: PeerRole): Peer | null {
  return role === 'host' ? room.client : room.host;
}

export function roomStats(): { rooms: number; peers: number } {
  let peers = 0;
  for (const room of rooms.values()) {
    if (room.host) peers += 1;
    if (room.client) peers += 1;
  }
  return { rooms: rooms.size, peers };
}

export function allPeers(): Peer[] {
  const out: Peer[] = [];
  for (const room of rooms.values()) {
    if (room.host) out.push(room.host);
    if (room.client) out.push(room.client);
  }
  return out;
}
