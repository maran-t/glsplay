import {
  parseSignalingMessage,
  type BrokerToClientMessage,
  type ClientToBrokerMessage,
} from '@glsplay/protocol';

export type SignalingState = 'idle' | 'connecting' | 'registered' | 'closed' | 'error';

export interface SignalingHandlers {
  onState: (state: SignalingState, detail?: string) => void;
  onMessage: (msg: BrokerToClientMessage) => void;
}

export interface SignalingOptions {
  url: string;
  roomId: string;
  secret: string;
  handlers: SignalingHandlers;
}

/**
 * WebSocket client for the signaling broker, with backoff reconnect.
 *
 * Reconnect matters more than it looks: the broker restarting mid-session must
 * not kill a working stream. Media flows peer-to-peer once ICE completes, so
 * the socket dropping is survivable - we only need it back for the next
 * renegotiation or ICE restart.
 */
export class SignalingClient {
  private socket: WebSocket | null = null;
  private closedByUs = false;
  private attempt = 0;
  private reconnectTimer: number | null = null;
  /** Fires once a connection has lasted long enough to count as healthy. */
  private stableTimer: number | null = null;

  constructor(private readonly opts: SignalingOptions) {}

  connect(): void {
    this.closedByUs = false;
    this.open();
  }

  private open(): void {
    this.opts.handlers.onState('connecting');

    let socket: WebSocket;
    try {
      socket = new WebSocket(this.opts.url);
    } catch (err) {
      this.opts.handlers.onState('error', err instanceof Error ? err.message : String(err));
      this.scheduleReconnect();
      return;
    }
    this.socket = socket;

    socket.onopen = () => {
      // Clear backoff only after the connection has proven stable. Resetting it
      // the instant the socket opens turns "upgrade, then immediate close" (a
      // rejected secret, or a second tab evicting this one) into a tight
      // reconnect loop instead of the intended exponential backoff.
      this.stableTimer = window.setTimeout(() => {
        this.stableTimer = null;
        this.attempt = 0;
      }, 3_000);
      this.send({
        type: 'register',
        role: 'client',
        roomId: this.opts.roomId,
        secret: this.opts.secret,
        agent: navigator.userAgent.slice(0, 120),
      });
    };

    socket.onmessage = (ev: MessageEvent<string>) => {
      const msg = parseSignalingMessage(ev.data);
      if (!msg) return;
      if (msg.type === 'registered') this.opts.handlers.onState('registered');
      if (msg.type === 'error' && (msg.code === 'bad-secret' || msg.code === 'role-taken')) {
        // Reconnecting after either of these just loops: a rejected secret
        // stays rejected, and a role a live peer holds stays held.
        this.closedByUs = true;
        this.opts.handlers.onState(
          'error',
          msg.code === 'bad-secret'
            ? 'room secret rejected by broker'
            : 'this room already has a client connected in another tab or device',
        );
      }
      this.opts.handlers.onMessage(msg as BrokerToClientMessage);
    };

    socket.onerror = () => {
      // The error event carries no useful detail in browsers; onclose follows
      // and is where the reconnect decision is made.
      this.opts.handlers.onState('error', 'websocket error');
    };

    socket.onclose = (ev: CloseEvent) => {
      this.socket = null;
      if (this.stableTimer !== null) {
        window.clearTimeout(this.stableTimer);
        this.stableTimer = null;
      }
      if (this.closedByUs) {
        this.opts.handlers.onState('closed');
        return;
      }
      // 4000 = the broker handed our room seat to a newer connection, almost
      // always a second tab or device open on the same room. Reconnecting just
      // evicts that one back, and the two loop forever - so stop and surface it.
      if (ev.code === 4000) {
        this.closedByUs = true;
        this.opts.handlers.onState(
          'error',
          ev.reason || 'this room is already open in another tab or device',
        );
        return;
      }
      this.opts.handlers.onState('closed', ev.reason || `code ${ev.code}`);
      this.scheduleReconnect();
    };
  }

  private scheduleReconnect(): void {
    if (this.closedByUs || this.reconnectTimer !== null) return;
    // Exponential backoff to 10s, with jitter so a broker restart does not get
    // hammered by every client reconnecting on the same tick.
    const base = Math.min(10_000, 500 * 2 ** this.attempt);
    const delay = base * (0.7 + Math.random() * 0.6);
    this.attempt += 1;
    this.reconnectTimer = window.setTimeout(() => {
      this.reconnectTimer = null;
      this.open();
    }, delay);
  }

  send(msg: ClientToBrokerMessage): boolean {
    if (this.socket?.readyState !== WebSocket.OPEN) return false;
    this.socket.send(JSON.stringify(msg));
    return true;
  }

  close(): void {
    this.closedByUs = true;
    if (this.reconnectTimer !== null) {
      window.clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    if (this.stableTimer !== null) {
      window.clearTimeout(this.stableTimer);
      this.stableTimer = null;
    }
    this.socket?.close(1000, 'client closing');
    this.socket = null;
  }
}
