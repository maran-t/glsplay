/**
 * Binary input wire format, carried on the unreliable/unordered "input"
 * DataChannel (PRD section 4.4).
 *
 * Design constraints:
 *  - Fixed-size, little-endian, naturally aligned so the C++ host can memcpy
 *    straight into a packed struct. include/glsplay_input.h mirrors this file
 *    exactly, and its static_asserts fail the build if the two ever drift.
 *  - Every event carries a client timestamp so the host can measure one-way
 *    input delivery latency without a round trip.
 *  - Multiple events may be concatenated into one DataChannel message. Mouse
 *    movement at 1000Hz would otherwise swamp SCTP with tiny datagrams, so the
 *    client coalesces an animation frame worth of events into a single send.
 */

/** The header is 8 bytes; payloads start at this offset. */
export const HEADER_SIZE = 8;

export const InputType = {
  MouseMoveRelative: 0x01,
  MouseButton: 0x02,
  MouseWheel: 0x03,
  Key: 0x04,
  Gamepad: 0x05,
  Ping: 0x06,
  MouseMoveAbsolute: 0x07,
  GamepadConnection: 0x08,
} as const;

export type InputTypeValue = (typeof InputType)[keyof typeof InputType];

/** Total wire size, header included, per event type. */
export const INPUT_SIZES: Readonly<Record<number, number>> = {
  [InputType.MouseMoveRelative]: 12,
  [InputType.MouseButton]: 10,
  [InputType.MouseWheel]: 12,
  [InputType.Key]: 12,
  [InputType.Gamepad]: 24,
  [InputType.Ping]: 8,
  [InputType.MouseMoveAbsolute]: 12,
  [InputType.GamepadConnection]: 10,
};

/** Largest single event. Sizes the client coalescing buffer headroom. */
export const MAX_EVENT_SIZE = 24;

export const MouseButton = {
  Left: 0,
  Right: 1,
  Middle: 2,
  Back: 3,
  Forward: 4,
} as const;

/** Header flag bits. */
export const Flags = {
  /** Key needs the 0xE0 extended scan-code prefix on injection. */
  Extended: 1 << 0,
} as const;

/** One wheel notch, matching Win32 WHEEL_DELTA. */
export const WHEEL_DELTA = 120;

/**
 * Button bitmask matching XINPUT_GAMEPAD_* exactly, so the host forwards the
 * value to ViGEm with no translation table in between.
 */
export const GamepadButton = {
  DPadUp: 0x0001,
  DPadDown: 0x0002,
  DPadLeft: 0x0004,
  DPadRight: 0x0008,
  Start: 0x0010,
  Back: 0x0020,
  LeftThumb: 0x0040,
  RightThumb: 0x0080,
  LeftShoulder: 0x0100,
  RightShoulder: 0x0200,
  Guide: 0x0400,
  A: 0x1000,
  B: 0x2000,
  X: 0x4000,
  Y: 0x8000,
} as const;

/**
 * W3C Gamepad API standard-mapping button index to XInput bitmask.
 * Indices 6 and 7 are the analog triggers, sent as separate byte fields.
 */
export const STANDARD_GAMEPAD_MAP: ReadonlyArray<number> = [
  GamepadButton.A,             // 0
  GamepadButton.B,             // 1
  GamepadButton.X,             // 2
  GamepadButton.Y,             // 3
  GamepadButton.LeftShoulder,  // 4
  GamepadButton.RightShoulder, // 5
  0,                           // 6 left trigger (analog)
  0,                           // 7 right trigger (analog)
  GamepadButton.Back,          // 8
  GamepadButton.Start,         // 9
  GamepadButton.LeftThumb,     // 10
  GamepadButton.RightThumb,    // 11
  GamepadButton.DPadUp,        // 12
  GamepadButton.DPadDown,      // 13
  GamepadButton.DPadLeft,      // 14
  GamepadButton.DPadRight,     // 15
  GamepadButton.Guide,         // 16
];

/**
 * KeyboardEvent.code to PS/2 scan code set 1.
 *
 * Scan codes rather than virtual keys because DirectInput titles read the
 * hardware scan code directly and ignore the VK translation layer entirely
 * (PRD section 4.4B). Values above 0xFF are extended keys; the encoder strips
 * the high byte and sets Flags.Extended instead of sending two bytes.
 */
export const SCANCODE_BY_CODE: Readonly<Record<string, number>> = {
  Escape: 0x01,
  Digit1: 0x02, Digit2: 0x03, Digit3: 0x04, Digit4: 0x05, Digit5: 0x06,
  Digit6: 0x07, Digit7: 0x08, Digit8: 0x09, Digit9: 0x0a, Digit0: 0x0b,
  Minus: 0x0c, Equal: 0x0d, Backspace: 0x0e, Tab: 0x0f,
  KeyQ: 0x10, KeyW: 0x11, KeyE: 0x12, KeyR: 0x13, KeyT: 0x14,
  KeyY: 0x15, KeyU: 0x16, KeyI: 0x17, KeyO: 0x18, KeyP: 0x19,
  BracketLeft: 0x1a, BracketRight: 0x1b, Enter: 0x1c, ControlLeft: 0x1d,
  KeyA: 0x1e, KeyS: 0x1f, KeyD: 0x20, KeyF: 0x21, KeyG: 0x22,
  KeyH: 0x23, KeyJ: 0x24, KeyK: 0x25, KeyL: 0x26,
  Semicolon: 0x27, Quote: 0x28, Backquote: 0x29,
  ShiftLeft: 0x2a, Backslash: 0x2b,
  KeyZ: 0x2c, KeyX: 0x2d, KeyC: 0x2e, KeyV: 0x2f, KeyB: 0x30,
  KeyN: 0x31, KeyM: 0x32,
  Comma: 0x33, Period: 0x34, Slash: 0x35, ShiftRight: 0x36,
  NumpadMultiply: 0x37, AltLeft: 0x38, Space: 0x39, CapsLock: 0x3a,
  F1: 0x3b, F2: 0x3c, F3: 0x3d, F4: 0x3e, F5: 0x3f,
  F6: 0x40, F7: 0x41, F8: 0x42, F9: 0x43, F10: 0x44,
  NumLock: 0x45, ScrollLock: 0x46,
  Numpad7: 0x47, Numpad8: 0x48, Numpad9: 0x49, NumpadSubtract: 0x4a,
  Numpad4: 0x4b, Numpad5: 0x4c, Numpad6: 0x4d, NumpadAdd: 0x4e,
  Numpad1: 0x4f, Numpad2: 0x50, Numpad3: 0x51, Numpad0: 0x52,
  NumpadDecimal: 0x53, IntlBackslash: 0x56, F11: 0x57, F12: 0x58,

  // Extended set - 0xE0 prefixed on real hardware.
  NumpadEnter: 0xe01c,
  ControlRight: 0xe01d,
  NumpadDivide: 0xe035,
  AltRight: 0xe038,
  Home: 0xe047,
  ArrowUp: 0xe048,
  PageUp: 0xe049,
  ArrowLeft: 0xe04b,
  ArrowRight: 0xe04d,
  End: 0xe04f,
  ArrowDown: 0xe050,
  PageDown: 0xe051,
  Insert: 0xe052,
  Delete: 0xe053,
  MetaLeft: 0xe05b,
  MetaRight: 0xe05c,
  ContextMenu: 0xe05d,
};

function clampI16(v: number): number {
  return Math.max(-32768, Math.min(32767, Math.trunc(v)));
}

function clampU8(v: number): number {
  return Math.max(0, Math.min(255, Math.round(v)));
}

/**
 * Incremental encoder that packs many events into one buffer.
 *
 * Per animation frame: reset(), push events, then send bytes() if byteLength
 * is non-zero. Reusing one instance keeps the hot path allocation-free. Every
 * writer returns false when the buffer is full, which the caller handles by
 * flushing and retrying rather than silently dropping the event.
 */
export class InputEncoder {
  private readonly buffer: ArrayBuffer;
  private readonly view: DataView;
  private offset = 0;
  private seq = 0;

  constructor(capacity = 4096) {
    this.buffer = new ArrayBuffer(capacity);
    this.view = new DataView(this.buffer);
  }

  get byteLength(): number {
    return this.offset;
  }

  reset(): void {
    this.offset = 0;
  }

  /**
   * Zero-copy view of the packed bytes. Valid until the next reset().
   *
   * The return type is intentionally inferred rather than annotated. Since
   * TypeScript 5.7 typed arrays are generic over their backing buffer, and
   * writing `Uint8Array` here would widen it to `ArrayBufferLike` - which
   * RTCDataChannel.send() then rejects, because it cannot accept a
   * SharedArrayBuffer view. Inference keeps it pinned to ArrayBuffer.
   */
  bytes() {
    return new Uint8Array(this.buffer, 0, this.offset);
  }

  /**
   * Writes the 8-byte header and returns the payload offset, or -1 when the
   * buffer cannot fit another event of this type.
   */
  private header(type: number, flags: number, tClient: number): number {
    const size = INPUT_SIZES[type];
    if (size === undefined || this.offset + size > this.buffer.byteLength) return -1;
    const at = this.offset;
    this.view.setUint8(at, type);
    this.view.setUint8(at + 1, flags);
    this.view.setUint16(at + 2, this.seq & 0xffff, true);
    this.view.setUint32(at + 4, tClient >>> 0, true);
    this.seq = (this.seq + 1) & 0xffff;
    this.offset += size;
    return at + HEADER_SIZE;
  }

  mouseMoveRelative(dx: number, dy: number, t: number): boolean {
    const p = this.header(InputType.MouseMoveRelative, 0, t);
    if (p < 0) return false;
    this.view.setInt16(p, clampI16(dx), true);
    this.view.setInt16(p + 2, clampI16(dy), true);
    return true;
  }

  /** x and y are normalised to 0..32767 across the remote desktop. */
  mouseMoveAbsolute(x: number, y: number, t: number): boolean {
    const p = this.header(InputType.MouseMoveAbsolute, 0, t);
    if (p < 0) return false;
    this.view.setInt16(p, clampI16(x), true);
    this.view.setInt16(p + 2, clampI16(y), true);
    return true;
  }

  mouseButton(button: number, down: boolean, t: number): boolean {
    const p = this.header(InputType.MouseButton, 0, t);
    if (p < 0) return false;
    this.view.setUint8(p, button & 0xff);
    this.view.setUint8(p + 1, down ? 1 : 0);
    return true;
  }

  /** Deltas are in WHEEL_DELTA units (120 per notch). */
  mouseWheel(vertical: number, horizontal: number, t: number): boolean {
    const p = this.header(InputType.MouseWheel, 0, t);
    if (p < 0) return false;
    this.view.setInt16(p, clampI16(vertical), true);
    this.view.setInt16(p + 2, clampI16(horizontal), true);
    return true;
  }

  /** False when the buffer is full, or the code has no scan-code mapping. */
  key(code: string, down: boolean, t: number): boolean {
    const raw = SCANCODE_BY_CODE[code];
    if (raw === undefined) return false;
    const extended = raw > 0xff;
    const p = this.header(InputType.Key, extended ? Flags.Extended : 0, t);
    if (p < 0) return false;
    this.view.setUint16(p, raw & 0xff, true);
    this.view.setUint8(p + 2, down ? 1 : 0);
    this.view.setUint8(p + 3, 0);
    return true;
  }

  gamepad(
    index: number,
    buttons: number,
    leftTrigger: number,
    rightTrigger: number,
    lx: number,
    ly: number,
    rx: number,
    ry: number,
    t: number,
  ): boolean {
    const p = this.header(InputType.Gamepad, 0, t);
    if (p < 0) return false;
    this.view.setUint8(p, index & 0xff);
    this.view.setUint8(p + 1, 0);
    this.view.setUint16(p + 2, buttons & 0xffff, true);
    this.view.setUint8(p + 4, clampU8(leftTrigger));
    this.view.setUint8(p + 5, clampU8(rightTrigger));
    this.view.setInt16(p + 6, clampI16(lx), true);
    this.view.setInt16(p + 8, clampI16(ly), true);
    this.view.setInt16(p + 10, clampI16(rx), true);
    this.view.setInt16(p + 12, clampI16(ry), true);
    return true;
  }

  gamepadConnection(index: number, connected: boolean, t: number): boolean {
    const p = this.header(InputType.GamepadConnection, 0, t);
    if (p < 0) return false;
    this.view.setUint8(p, index & 0xff);
    this.view.setUint8(p + 1, connected ? 1 : 0);
    return true;
  }

  ping(t: number): boolean {
    return this.header(InputType.Ping, 0, t) >= 0;
  }
}

/**
 * Exact type of a packed input batch, as produced by InputEncoder.bytes().
 *
 * Consumers should use this rather than writing `Uint8Array`, which widens the
 * backing-buffer generic and breaks assignment to RTCDataChannel.send().
 */
export type InputBytes = ReturnType<InputEncoder['bytes']>;
