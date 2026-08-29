'use client';

import { useCallback, useEffect, useRef, useState } from 'react';
import {
  InputEncoder,
  MouseButton,
  STANDARD_GAMEPAD_MAP,
  WHEEL_DELTA,
  type ClientToHostControl,
  type InputBytes,
} from '@glsplay/protocol';

export interface InputCaptureOptions {
  /** Element that receives Pointer Lock. Usually the video. */
  target: React.RefObject<HTMLElement>;
  sendInput: (bytes: InputBytes) => boolean;
  sendControl: (msg: ClientToHostControl) => boolean;
  enabled: boolean;
}

export interface InputCaptureState {
  pointerLocked: boolean;
  gamepads: number;
  /** Events the transport refused, usually from DataChannel backpressure. */
  dropped: number;
}

/** Chrome deltaMode values. Line and page scrolling need scaling to pixels. */
const DOM_DELTA_LINE = 1;
const DOM_DELTA_PAGE = 2;

/**
 * Largest single-event mouse delta forwarded to the host. A real mouse, even a
 * 1000Hz one moving fast, stays well under this per event; anything bigger is a
 * Pointer Lock glitch (the jump from the old OS cursor position on the first
 * event after lock, or a focus bounce) and would teleport the remote pointer.
 */
const MAX_MOUSE_DELTA = 160;

/**
 * Captures mouse, keyboard and gamepad, and packs them onto the input
 * DataChannel (PRD section 4.4).
 *
 * Events are encoded and flushed on a microtask rather than on
 * requestAnimationFrame. Batching to the frame would be kinder to SCTP, but it
 * adds up to a full frame of latency to every input - which is most of the 1ms
 * that PRD section 5 budgets for the entire input stage.
 */
export function useInputCapture(opts: InputCaptureOptions): InputCaptureState {
  const { target, sendInput, sendControl, enabled } = opts;

  const [pointerLocked, setPointerLocked] = useState(false);
  const [gamepads, setGamepads] = useState(0);
  const [dropped, setDropped] = useState(0);

  const encoderRef = useRef(new InputEncoder(4096));
  const flushScheduled = useRef(false);
  const droppedRef = useRef(0);
  /** Keys currently held, so they can be released on focus loss. */
  const heldKeys = useRef(new Set<string>());
  /** Last gamepad payload per index, to suppress duplicate frames. */
  const lastPad = useRef(new Map<number, string>());
  /** Set when Pointer Lock engages; the first mousemove after it carries a
   *  bogus jump from the previous OS cursor position and is dropped. */
  const swallowNextMove = useRef(false);
  const startRef = useRef(0);

  if (startRef.current === 0 && typeof performance !== 'undefined') {
    startRef.current = performance.now();
  }

  /** Client clock in ms since capture began; fits the u32 wire field. */
  const now = useCallback((): number => {
    return Math.round(performance.now() - startRef.current) >>> 0;
  }, []);

  const flush = useCallback(() => {
    flushScheduled.current = false;
    const encoder = encoderRef.current;
    if (encoder.byteLength === 0) return;
    if (!sendInput(encoder.bytes())) {
      droppedRef.current += 1;
      setDropped(droppedRef.current);
    }
    encoder.reset();
  }, [sendInput]);

  const scheduleFlush = useCallback(() => {
    if (flushScheduled.current) return;
    flushScheduled.current = true;
    queueMicrotask(flush);
  }, [flush]);

  /** Releases every held key. Called when focus or pointer lock is lost. */
  const releaseHeldKeys = useCallback(() => {
    if (heldKeys.current.size === 0) return;
    const encoder = encoderRef.current;
    const t = now();
    for (const code of heldKeys.current) {
      if (!encoder.key(code, false, t)) {
        flush();
        encoder.key(code, false, t);
      }
    }
    heldKeys.current.clear();
    scheduleFlush();
  }, [flush, now, scheduleFlush]);

  // ---- pointer lock --------------------------------------------------------
  useEffect(() => {
    if (!enabled) return;

    const onPointerLockChange = () => {
      const locked = document.pointerLockElement === target.current;
      setPointerLocked(locked);
      if (locked) swallowNextMove.current = true;
      sendControl({ type: 'set-pointer-mode', mode: locked ? 'relative' : 'absolute' });
      // Leaving lock mid-keypress would otherwise latch that key down on the
      // remote desktop with no matching keyup ever arriving.
      if (!locked) releaseHeldKeys();
    };

    document.addEventListener('pointerlockchange', onPointerLockChange);
    return () => document.removeEventListener('pointerlockchange', onPointerLockChange);
  }, [enabled, releaseHeldKeys, sendControl, target]);

  // ---- mouse ---------------------------------------------------------------
  useEffect(() => {
    if (!enabled) return;
    const element = target.current;
    if (!element) return;

    const onMouseMove = (ev: MouseEvent) => {
      if (document.pointerLockElement !== element) return;
      // The first event after Pointer Lock engages reports the delta from the
      // pre-lock cursor position - often hundreds of pixels. Drop it whole.
      if (swallowNextMove.current) {
        swallowNextMove.current = false;
        return;
      }
      const encoder = encoderRef.current;
      const t = now();
      const emit = (dx: number, dy: number) => {
        if (dx === 0 && dy === 0) return;
        // Guard against Pointer Lock delta spikes that would teleport the
        // remote pointer. A genuine fast flick stays under MAX_MOUSE_DELTA per
        // event; clamp rather than drop so normal motion is untouched.
        const cx = Math.max(-MAX_MOUSE_DELTA, Math.min(MAX_MOUSE_DELTA, dx));
        const cy = Math.max(-MAX_MOUSE_DELTA, Math.min(MAX_MOUSE_DELTA, dy));
        if (!encoder.mouseMoveRelative(cx, cy, t)) {
          flush();
          encoder.mouseMoveRelative(cx, cy, t);
        }
      };
      // getCoalescedEvents exposes sub-frame samples a high-polling-rate mouse
      // produced between paints. Sending them all preserves the true motion
      // curve instead of the single decimated sample Chrome surfaces.
      const pointer = ev as MouseEvent & { getCoalescedEvents?: () => PointerEvent[] };
      const samples = pointer.getCoalescedEvents?.() ?? [];
      if (samples.length > 1) {
        for (const sample of samples) emit(sample.movementX, sample.movementY);
      } else {
        emit(ev.movementX, ev.movementY);
      }
      scheduleFlush();
    };

    const buttonFor = (raw: number): number | null => {
      switch (raw) {
        case 0: return MouseButton.Left;
        case 1: return MouseButton.Middle;
        case 2: return MouseButton.Right;
        case 3: return MouseButton.Back;
        case 4: return MouseButton.Forward;
        default: return null;
      }
    };

    const onMouseDown = (ev: MouseEvent) => {
      // First click inside the viewport arms Pointer Lock rather than being
      // forwarded, matching what every browser-based game does.
      if (document.pointerLockElement !== element) {
        void element.requestPointerLock();
        return;
      }
      const button = buttonFor(ev.button);
      if (button === null) return;
      ev.preventDefault();
      encoderRef.current.mouseButton(button, true, now());
      scheduleFlush();
    };

    const onMouseUp = (ev: MouseEvent) => {
      if (document.pointerLockElement !== element) return;
      const button = buttonFor(ev.button);
      if (button === null) return;
      ev.preventDefault();
      encoderRef.current.mouseButton(button, false, now());
      scheduleFlush();
    };

    const onWheel = (ev: WheelEvent) => {
      if (document.pointerLockElement !== element) return;
      ev.preventDefault();
      // Normalise to pixels, then to Win32 wheel notches. Chrome reports lines
      // on some platforms and pages when a scroll region is paged.
      const scale =
        ev.deltaMode === DOM_DELTA_LINE ? 16 : ev.deltaMode === DOM_DELTA_PAGE ? 100 : 1;
      const vertical = Math.round((-ev.deltaY * scale * WHEEL_DELTA) / 100);
      const horizontal = Math.round((ev.deltaX * scale * WHEEL_DELTA) / 100);
      if (vertical === 0 && horizontal === 0) return;
      encoderRef.current.mouseWheel(vertical, horizontal, now());
      scheduleFlush();
    };

    // The browser context menu would steal a right-click that belongs to the
    // game, so it is suppressed for the viewport only.
    const onContextMenu = (ev: Event) => ev.preventDefault();

    element.addEventListener('mousemove', onMouseMove);
    element.addEventListener('mousedown', onMouseDown);
    window.addEventListener('mouseup', onMouseUp);
    element.addEventListener('wheel', onWheel, { passive: false });
    element.addEventListener('contextmenu', onContextMenu);

    return () => {
      element.removeEventListener('mousemove', onMouseMove);
      element.removeEventListener('mousedown', onMouseDown);
      window.removeEventListener('mouseup', onMouseUp);
      element.removeEventListener('wheel', onWheel);
      element.removeEventListener('contextmenu', onContextMenu);
    };
  }, [enabled, flush, now, scheduleFlush, target]);

  // ---- keyboard ------------------------------------------------------------
  useEffect(() => {
    if (!enabled) return;

    const onKeyDown = (ev: KeyboardEvent) => {
      if (document.pointerLockElement === null) return;
      // Escape is how the user leaves Pointer Lock; the browser handles it and
      // forwarding it too would also pause the game.
      if (ev.code === 'Escape') return;
      ev.preventDefault();
      // Ignore auto-repeat. Games derive their own repeat from held state, and
      // the OS repeat rate on the host is what should govern text fields.
      if (ev.repeat) return;
      if (encoderRef.current.key(ev.code, true, now())) {
        heldKeys.current.add(ev.code);
        scheduleFlush();
      }
    };

    const onKeyUp = (ev: KeyboardEvent) => {
      if (document.pointerLockElement === null) return;
      if (ev.code === 'Escape') return;
      ev.preventDefault();
      if (encoderRef.current.key(ev.code, false, now())) {
        heldKeys.current.delete(ev.code);
        scheduleFlush();
      }
    };

    // Alt-tabbing away while holding W would leave the character walking
    // forever on the host.
    const onBlur = () => releaseHeldKeys();

    window.addEventListener('keydown', onKeyDown, { capture: true });
    window.addEventListener('keyup', onKeyUp, { capture: true });
    window.addEventListener('blur', onBlur);

    return () => {
      window.removeEventListener('keydown', onKeyDown, { capture: true });
      window.removeEventListener('keyup', onKeyUp, { capture: true });
      window.removeEventListener('blur', onBlur);
    };
  }, [enabled, now, releaseHeldKeys, scheduleFlush]);

  // ---- gamepad -------------------------------------------------------------
  useEffect(() => {
    if (!enabled) return;
    if (typeof navigator.getGamepads !== 'function') return;

    let frame = 0;

    /** Maps the W3C -1..1 axis range onto the XInput signed 16-bit range. */
    const axis = (v: number): number => Math.round(Math.max(-1, Math.min(1, v)) * 32767);

    const poll = () => {
      frame = requestAnimationFrame(poll);

      const pads = navigator.getGamepads();
      let connected = 0;
      const encoder = encoderRef.current;
      const t = now();

      for (let i = 0; i < pads.length && i < 4; i += 1) {
        const pad = pads[i];
        if (!pad?.connected) {
          if (lastPad.current.delete(i)) {
            encoder.gamepadConnection(i, false, t);
            scheduleFlush();
          }
          continue;
        }
        connected += 1;

        let buttons = 0;
        for (let b = 0; b < pad.buttons.length && b < STANDARD_GAMEPAD_MAP.length; b += 1) {
          if (pad.buttons[b]?.pressed) buttons |= STANDARD_GAMEPAD_MAP[b] ?? 0;
        }
        const lt = Math.round((pad.buttons[6]?.value ?? 0) * 255);
        const rt = Math.round((pad.buttons[7]?.value ?? 0) * 255);
        // Y axes are inverted relative to XInput, which treats up as positive.
        const lx = axis(pad.axes[0] ?? 0);
        const ly = -axis(pad.axes[1] ?? 0);
        const rx = axis(pad.axes[2] ?? 0);
        const ry = -axis(pad.axes[3] ?? 0);

        const signature = `${buttons}|${lt}|${rt}|${lx}|${ly}|${rx}|${ry}`;
        const previous = lastPad.current.get(i);
        if (previous === undefined) encoder.gamepadConnection(i, true, t);
        // Resending an unchanged pad state every frame would be 60 wasted
        // packets a second per controller.
        if (previous === signature) continue;

        lastPad.current.set(i, signature);
        if (!encoder.gamepad(i, buttons, lt, rt, lx, ly, rx, ry, t)) {
          flush();
          encoder.gamepad(i, buttons, lt, rt, lx, ly, rx, ry, t);
        }
        scheduleFlush();
      }

      setGamepads((prev) => (prev === connected ? prev : connected));
    };

    frame = requestAnimationFrame(poll);
    return () => cancelAnimationFrame(frame);
  }, [enabled, flush, now, scheduleFlush]);

  return { pointerLocked, gamepads, dropped };
}
