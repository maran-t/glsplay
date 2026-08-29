/**
 * glsplay signaling broker (PRD section 4.5).
 *
 * Authenticates peers, pairs one host with one client per room, and relays
 * opaque SDP/ICE payloads between them. It never parses SDP and never touches
 * media - once ICE completes, the browser and the host talk directly and this
 * process is irrelevant to latency.
 *
 *   npm run dev -w @glsplay/signaling      (watch mode)
 *   npm start   -w @glsplay/signaling      (built)
 */

import { createServer, type IncomingMessage, type ServerResponse } from 'node:http';
import { WebSocketServer, type WebSocket } from 'ws';
import {
  isRelayedType,
  parseSignalingMessage,
  type BrokerToClientMessage,
  type PeerRole,
} from '@glsplay/protocol';
import { log } from './logger.js';
import {
  allPeers,
  getOrCreateRoom,
  nextPeerId,
  opposite,
  removePeer,
  roomStats,
  seatPeer,
  type Peer,
} from './rooms.js';

const PORT = Number(process.env['GLSPLAY_SIGNALING_PORT'] ?? 8080);
const HOST = process.env['GLSPLAY_SIGNALING_HOST'] ?? '0.0.0.0';
const SECRET = process.env['GLSPLAY_ROOM_SECRET'] ?? '';

/** Peers must register within this window or the socket is closed. */
const REGISTER_TIMEOUT_MS = 10_000;
/** Heartbeat period; one missed pong and the peer is terminated. */
const HEARTBEAT_MS = 10_000;
/** Per-peer rate limit. */
const RATE_WINDOW_MS = 1_000;
const RATE_MAX_MESSAGES = 200;
/** A 1080p60 SDP is a few KB. Anything far larger is not legitimate. */
const MAX_PAYLOAD_BYTES = 256 * 1024;

if (!SECRET || SECRET.startsWith('change-me')) {
  log.error('GLSPLAY_ROOM_SECRET is unset or still the placeholder - refusing to start');
  log.error('Generate one: node -e "console.log(require(\'crypto\').randomBytes(32).toString(\'hex\'))"');
  process.exit(1);
}

/**
 * Length-independent comparison. Not a hard security boundary - that is TLS
 * plus a firewall - but avoiding an early-exit costs nothing and stops the
 * secret leaking one character at a time through response timing.
 */
function secretMatches(candidate: string): boolean {
  if (candidate.length !== SECRET.length) return false;
  let diff = 0;
  for (let i = 0; i < SECRET.length; i += 1) {
    diff |= SECRET.charCodeAt(i) ^ candidate.charCodeAt(i);
  }
  return diff === 0;
}

function send(peer: Peer, msg: BrokerToClientMessage): void {
  if (peer.socket.readyState !== peer.socket.OPEN) return;
  peer.socket.send(JSON.stringify(msg));
}

function fail(socket: WebSocket, code: string, message: string): void {
  if (socket.readyState === socket.OPEN) {
    socket.send(JSON.stringify({ type: 'error', code, message }));
  }
  socket.close(1008, code);
}

function allowMessage(peer: Peer): boolean {
  const now = Date.now();
  if (now - peer.windowStart > RATE_WINDOW_MS) {
    peer.windowStart = now;
    peer.windowCount = 0;
  }
  peer.windowCount += 1;
  return peer.windowCount <= RATE_MAX_MESSAGES;
}

const httpServer = createServer((req: IncomingMessage, res: ServerResponse) => {
  // Health endpoint so the VM startup script can block until the broker is
  // reachable instead of racing it.
  if (req.url === '/health') {
    res.writeHead(200, { 'content-type': 'application/json' });
    res.end(JSON.stringify({ status: 'ok', ...roomStats(), uptimeSec: Math.round(process.uptime()) }));
    return;
  }
  res.writeHead(404, { 'content-type': 'text/plain' });
  res.end('glsplay signaling broker\n');
});

const wss = new WebSocketServer({
  server: httpServer,
  maxPayload: MAX_PAYLOAD_BYTES,
  // Compression costs latency and buys nothing on payloads this small.
  perMessageDeflate: false,
});

wss.on('connection', (socket: WebSocket, req: IncomingMessage) => {
  const remote = req.socket.remoteAddress ?? 'unknown';
  let peer: Peer | null = null;

  const registerTimer = setTimeout(() => {
    if (!peer?.registered) {
      log.warn('register timeout', { remote });
      fail(socket, 'not-registered', 'register within 10s of connecting');
    }
  }, REGISTER_TIMEOUT_MS);

  socket.on('pong', () => {
    if (peer) peer.alive = true;
  });

  socket.on('message', (data: Buffer, isBinary: boolean) => {
    if (isBinary) {
      fail(socket, 'bad-json', 'signaling is JSON text only');
      return;
    }

    const raw = data.toString('utf8');
    const msg = parseSignalingMessage(raw);
    if (!msg) {
      log.warn('malformed message', { remote, peer: peer?.id });
      fail(socket, 'bad-json', 'malformed or unrecognised signaling message');
      return;
    }

    // ---- registration ----------------------------------------------------
    if (msg.type === 'register') {
      if (peer?.registered) {
        fail(socket, 'bad-role', 'already registered on this connection');
        return;
      }
      if (!secretMatches(msg.secret)) {
        log.warn('bad secret', { remote, room: msg.roomId, role: msg.role });
        fail(socket, 'bad-secret', 'room secret rejected');
        return;
      }

      const role: PeerRole = msg.role;
      const room = getOrCreateRoom(msg.roomId);

      // A role is taken over only when its current holder is demonstrably gone -
      // socket no longer OPEN, or a missed heartbeat. As long as the incumbent's
      // socket is open and answering pings it keeps the seat, and every other
      // claimant is refused. This is what stops two peers that each reconnect
      // after being evicted from ping-ponging the seat forever: whoever holds it
      // keeps it, and the challenger's retries are cheap rejections instead of
      // evictions. A peer that crashed or dropped fails one of these checks
      // (immediately on a clean close, within HEARTBEAT_MS on a hard kill), so a
      // genuine reconnect still gets back in.
      const incumbent = room[role];
      if (
        incumbent &&
        incumbent.socket.readyState === incumbent.socket.OPEN &&
        incumbent.alive
      ) {
        log.warn('role already held', {
          remote,
          room: room.id,
          role,
          incumbent: incumbent.id,
        });
        fail(socket, 'role-taken', `this room already has a connected ${role}`);
        return;
      }

      peer = {
        id: nextPeerId(role),
        role,
        roomId: msg.roomId,
        socket,
        agent: msg.agent ?? 'unknown',
        connectedAt: Date.now(),
        registered: true,
        alive: true,
        windowStart: Date.now(),
        windowCount: 0,
      };
      clearTimeout(registerTimer);

      const evicted = seatPeer(room, peer);
      if (evicted) evicted.socket.close(4000, 'replaced by a newer connection');

      const other = opposite(room, role);
      send(peer, {
        type: 'registered',
        role,
        roomId: room.id,
        peerId: peer.id,
        peerPresent: other !== null,
      });

      // Tell both sides about each other so the host knows when to offer.
      if (other) {
        send(other, { type: 'peer-state', role, present: true });
        send(peer, { type: 'peer-state', role: other.role, present: true });
      }

      log.info('peer registered', {
        peer: peer.id,
        room: room.id,
        role,
        agent: peer.agent,
        remote,
        paired: other !== null,
      });
      return;
    }

    // ---- everything below requires a registered peer ---------------------
    if (!peer?.registered) {
      fail(socket, 'not-registered', 'send register before anything else');
      return;
    }

    if (!allowMessage(peer)) {
      log.warn('rate limited', { peer: peer.id, room: peer.roomId });
      fail(socket, 'rate-limited', `exceeded ${RATE_MAX_MESSAGES} messages/sec`);
      return;
    }

    if (msg.type === 'ping') {
      send(peer, { type: 'pong', t: msg.t, serverTime: Date.now() });
      return;
    }

    if (isRelayedType(msg.type)) {
      const room = getOrCreateRoom(peer.roomId);
      const target = opposite(room, peer.role);
      if (!target) {
        // Not fatal. The host commonly produces an offer before the browser
        // has loaded, so it retries on peer-state rather than tearing down.
        send(peer, { type: 'error', code: 'no-peer', message: 'no peer in room yet' });
        return;
      }
      target.socket.send(raw);
      log.debug('relayed', { from: peer.id, to: target.id, type: msg.type });
      return;
    }

    fail(socket, 'bad-json', `unexpected message type: ${msg.type}`);
  });

  socket.on('close', (code: number, reason: Buffer) => {
    clearTimeout(registerTimer);
    if (!peer) return;
    const room = removePeer(peer);
    log.info('peer disconnected', {
      peer: peer.id,
      room: peer.roomId,
      role: peer.role,
      code,
      reason: reason.toString('utf8') || undefined,
      sessionSec: Math.round((Date.now() - peer.connectedAt) / 1000),
    });
    // Tell the survivor promptly so it can tear down its PeerConnection
    // instead of waiting out an ICE disconnect timeout.
    const survivor = room ? opposite(room, peer.role) : null;
    if (survivor) send(survivor, { type: 'peer-state', role: peer.role, present: false });
  });

  socket.on('error', (err: Error) => {
    log.warn('socket error', { peer: peer?.id, remote, error: err.message });
  });
});

/**
 * Heartbeat. A half-open TCP connection looks alive to the OS indefinitely,
 * which would leave a room occupied by a peer that is already gone.
 */
const heartbeat = setInterval(() => {
  for (const peer of allPeers()) {
    if (!peer.alive) {
      log.warn('heartbeat timeout', { peer: peer.id, room: peer.roomId });
      peer.socket.terminate();
      continue;
    }
    peer.alive = false;
    if (peer.socket.readyState === peer.socket.OPEN) peer.socket.ping();
  }
}, HEARTBEAT_MS);

httpServer.listen(PORT, HOST, () => {
  log.info('signaling broker listening', { host: HOST, port: PORT });
});

function shutdown(signal: string): void {
  log.info('shutting down', { signal });
  clearInterval(heartbeat);
  for (const peer of allPeers()) peer.socket.close(1001, 'server shutting down');
  wss.close(() => httpServer.close(() => process.exit(0)));
  // Never let one stuck socket hold the process open.
  setTimeout(() => process.exit(0), 3_000).unref();
}

process.on('SIGINT', () => shutdown('SIGINT'));
process.on('SIGTERM', () => shutdown('SIGTERM'));
