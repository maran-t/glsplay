/**
 * Shared wire contract between the browser client, the signaling broker and
 * the native Windows host.
 *
 * The binary input format has a C++ mirror at
 * packages/protocol/include/glsplay_input.h. Those two files change together.
 */

export * from './signaling.js';
export * from './input.js';
export * from './control.js';

/** Bumped whenever the wire format changes incompatibly. */
export const PROTOCOL_VERSION = 1;
