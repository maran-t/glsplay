// Phase 1 smoke test: register a host and a client, verify pairing, relay an
// offer/answer/candidate round trip, and confirm a bad secret is rejected.
import { WebSocket } from 'ws';
import { readFileSync } from 'node:fs';

const env = Object.fromEntries(
  readFileSync(process.argv[2], 'utf8')
    .split(/\r?\n/)
    .filter((l) => l && !l.startsWith('#') && l.includes('='))
    .map((l) => [l.slice(0, l.indexOf('=')).trim(), l.slice(l.indexOf('=') + 1).trim()]),
);
// Second argument overrides the target, so the same test doubles as a
// connectivity check against the VM:
//   node test/smoke.mjs ../../.env ws://34.100.x.x:8080
const URL = process.argv[3] ?? process.env.GLSPLAY_SMOKE_URL ?? 'ws://localhost:8080';
const SECRET = env.GLSPLAY_ROOM_SECRET;
const ROOM = 'smoke';

console.log(`target: ${URL}\n`);

const results = [];
const check = (name, ok, detail = '') => {
  results.push({ name, ok, detail });
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '  -> ' + detail : ''}`);
};

function open(role, secret = SECRET) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(URL);
    const inbox = [];
    const waiters = [];
    ws.on('message', (d) => {
      const msg = JSON.parse(d.toString());
      const w = waiters.find((x) => x.match(msg));
      if (w) {
        waiters.splice(waiters.indexOf(w), 1);
        w.resolve(msg);
      } else inbox.push(msg);
    });
    ws.on('error', reject);
    ws.on('open', () => {
      ws.send(JSON.stringify({ type: 'register', role, roomId: ROOM, secret, agent: `smoke-${role}` }));
      resolve({
        ws,
        send: (m) => ws.send(JSON.stringify(m)),
        next: (match, ms = 3000) =>
          new Promise((res, rej) => {
            const found = inbox.find(match);
            if (found) {
              inbox.splice(inbox.indexOf(found), 1);
              return res(found);
            }
            const w = { match, resolve: res };
            waiters.push(w);
            setTimeout(() => rej(new Error('timeout waiting for message')), ms);
          }),
      });
    });
  });
}

try {
  // 1. Host registers into an empty room.
  const host = await open('host');
  const hostReg = await host.next((m) => m.type === 'registered');
  check('host registers', hostReg.role === 'host' && hostReg.peerPresent === false,
        `peerId=${hostReg.peerId} peerPresent=${hostReg.peerPresent}`);

  // 2. Client registers and both sides learn about each other.
  const client = await open('client');
  const clientReg = await client.next((m) => m.type === 'registered');
  check('client registers and sees host', clientReg.peerPresent === true);

  const hostSawClient = await host.next((m) => m.type === 'peer-state');
  check('host notified of client', hostSawClient.role === 'client' && hostSawClient.present === true);

  // 3. Client asks the host to offer.
  client.send({ type: 'renegotiate', reason: 'initial' });
  const renegAtHost = await host.next((m) => m.type === 'renegotiate');
  check('renegotiate relayed to host', renegAtHost.reason === 'initial');

  // 4. Offer / answer / candidate round trip.
  const fakeSdp = 'v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n';
  host.send({ type: 'offer', sdp: fakeSdp });
  const offerAtClient = await client.next((m) => m.type === 'offer');
  check('offer relayed host->client', offerAtClient.sdp === fakeSdp);

  client.send({ type: 'answer', sdp: 'v=0\r\nanswer\r\n' });
  const answerAtHost = await host.next((m) => m.type === 'answer');
  check('answer relayed client->host', answerAtHost.sdp.includes('answer'));

  client.send({ type: 'candidate', candidate: { candidate: 'candidate:1 1 udp 2130706431 10.0.0.1 50001 typ host', sdpMid: '0', sdpMLineIndex: 0 } });
  const candAtHost = await host.next((m) => m.type === 'candidate');
  check('ICE candidate relayed', candAtHost.candidate.candidate.includes('typ host'));

  // 5. Ping is answered by the broker itself.
  client.send({ type: 'ping', t: 12345 });
  const pong = await client.next((m) => m.type === 'pong');
  check('broker answers ping', pong.t === 12345 && typeof pong.serverTime === 'number');

  // 6. A wrong secret is rejected before any relaying is possible.
  const badSecret = await open('client', 'definitely-not-the-secret');
  const err = await badSecret.next((m) => m.type === 'error');
  check('bad secret rejected', err.code === 'bad-secret', err.message);

  // 7. Host disconnect notifies the surviving client.
  host.ws.close();
  const hostGone = await client.next((m) => m.type === 'peer-state' && m.present === false);
  check('client notified when host drops', hostGone.role === 'host');

  client.ws.close();
  badSecret.ws.close();
} catch (err) {
  check('unexpected failure', false, err.message);
}

const failed = results.filter((r) => !r.ok).length;
console.log(`\n${results.length - failed}/${results.length} passed`);
process.exit(failed === 0 ? 0 : 1);
