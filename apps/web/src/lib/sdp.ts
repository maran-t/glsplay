/**
 * SDP shaping for the receive side.
 *
 * Preference order: use the typed WebRTC APIs (setCodecPreferences) wherever
 * they exist, and hand-edit SDP only for the few things those APIs cannot
 * express. Munged SDP is fragile across Chrome releases, so every edit here is
 * narrow and defensive - if a pattern is not found the SDP comes back
 * untouched rather than corrupted.
 */

/** PRD section 4.2: H.264 High profile, level 4.0, non-interleaved NAL mode. */
export const TARGET_H264_PROFILE = '640028';
export const TARGET_PACKETIZATION_MODE = '1';

/**
 * Restricts the transceiver to H.264, preferring the payload type whose fmtp
 * matches the host encoder exactly.
 *
 * Chrome offers several H.264 payload types differing in profile-level-id and
 * packetization-mode. Left alone it may negotiate constrained baseline, and
 * the host would then have to encode down to it - discarding the High profile
 * quality the L4 produces for free.
 */
export function preferH264(transceiver: RTCRtpTransceiver): boolean {
  const capabilities = RTCRtpReceiver.getCapabilities('video');
  if (!capabilities) return false;

  // Derived rather than named: the DOM lib has renamed this type across TS
  // releases, and setCodecPreferences must receive exactly what
  // getCapabilities returned anyway.
  type CodecCapability = (typeof capabilities.codecs)[number];

  const h264 = capabilities.codecs.filter((c) => /h264/i.test(c.mimeType));
  if (h264.length === 0) return false;

  const score = (codec: CodecCapability): number => {
    const fmtp = codec.sdpFmtpLine ?? '';
    let s = 0;
    if (fmtp.includes(`profile-level-id=${TARGET_H264_PROFILE}`)) s += 4;
    if (fmtp.includes(`packetization-mode=${TARGET_PACKETIZATION_MODE}`)) s += 2;
    // High profile ids begin 64; prefer them over baseline (42) as a fallback.
    if (/profile-level-id=64/i.test(fmtp)) s += 1;
    return s;
  };

  const ordered = [...h264].sort((a, b) => score(b) - score(a));

  // Keep RTX and FEC. Without RTX there is no retransmission, so one lost
  // packet becomes a visible artefact that persists until the next keyframe.
  const support = capabilities.codecs.filter((c) => /(rtx|red|ulpfec)$/i.test(c.mimeType));

  try {
    transceiver.setCodecPreferences([...ordered, ...support]);
    return true;
  } catch {
    // Older Chrome, or a list the browser rejects. Negotiation still succeeds,
    // just without our ordering.
    return false;
  }
}

/**
 * Caps what the remote peer may send us, expressed in our answer.
 *
 * b=AS is the only receiver-side bandwidth signal that reaches the sender, so
 * it is how the browser says "do not exceed this". The host still runs its own
 * rate control; this is a ceiling, not a target.
 */
export function setReceiveBitrate(sdp: string, kbps: number): string {
  const lines = sdp.split(/\r\n|\n/);
  const out: string[] = [];
  let inVideo = false;

  for (const line of lines) {
    if (line.startsWith('m=')) inVideo = line.startsWith('m=video');

    // Drop any existing bandwidth line in the video section so we never stack
    // two contradictory limits.
    if (inVideo && (line.startsWith('b=AS:') || line.startsWith('b=TIAS:'))) continue;

    out.push(line);

    // RFC 4566 ordering puts b= immediately after c=.
    if (inVideo && line.startsWith('c=')) {
      out.push(`b=AS:${Math.round(kbps)}`);
      out.push(`b=TIAS:${Math.round(kbps * 1000)}`);
    }
  }
  return out.join('\r\n');
}

/**
 * Asks the host for stereo Opus at a fixed rate (PRD section 4.3).
 *
 * Chrome negotiates mono at a conservative bitrate unless the answer says
 * otherwise, which is plainly audible on game audio with stereo positioning.
 */
export function configureOpus(sdp: string, bitrateBps = 128_000): string {
  const payloadMatch = sdp.match(/^a=rtpmap:(\d+)\s+opus\/48000\/2/im);
  if (!payloadMatch?.[1]) return sdp;
  const pt = payloadMatch[1];

  const wanted: ReadonlyArray<[string, string]> = [
    ['stereo', '1'],
    ['sprop-stereo', '1'],
    ['maxaveragebitrate', String(bitrateBps)],
    ['useinbandfec', '1'],
    // PRD section 4.3 asks for 5-10ms frames; 10 is the smallest Chrome
    // reliably negotiates.
    ['minptime', '10'],
    // Discontinuous transmission adds a resume delay after silence. Game audio
    // is near-continuous, so DTX saves nothing and costs a stutter.
    ['usedtx', '0'],
  ];

  const fmtpRe = new RegExp(`^a=fmtp:${pt} (.*)$`, 'im');
  if (fmtpRe.test(sdp)) {
    return sdp.replace(fmtpRe, (_full: string, existing: string) => {
      const params = new Map<string, string>();
      for (const kv of existing.split(';')) {
        const idx = kv.indexOf('=');
        if (idx > 0) params.set(kv.slice(0, idx).trim(), kv.slice(idx + 1).trim());
      }
      for (const [k, v] of wanted) params.set(k, v);
      const merged = [...params].map(([k, v]) => `${k}=${v}`).join(';');
      return `a=fmtp:${pt} ${merged}`;
    });
  }

  const params = wanted.map(([k, v]) => `${k}=${v}`).join(';');
  return sdp.replace(
    new RegExp(`^(a=rtpmap:${pt} opus/48000/2.*)$`, 'im'),
    `$1\r\na=fmtp:${pt} ${params}`,
  );
}

/** Everything the client applies to its answer, in one call. */
export function shapeAnswer(sdp: string, opts: { maxBitrateKbps: number }): string {
  return configureOpus(setReceiveBitrate(sdp, opts.maxBitrateKbps));
}

/** Reads the negotiated video codec back out, for display in the HUD. */
export function describeNegotiatedVideo(sdp: string): string {
  const m = sdp.match(/^m=video \S+ \S+ (\d+)/im);
  if (!m?.[1]) return 'unknown';
  const pt = m[1];
  const rtpmap = sdp.match(new RegExp(`^a=rtpmap:${pt} ([^/]+)/`, 'im'));
  const fmtp = sdp.match(new RegExp(`^a=fmtp:${pt} (.*)$`, 'im'));
  const name = rtpmap?.[1] ?? `pt:${pt}`;
  const profile = fmtp?.[1]?.match(/profile-level-id=([0-9a-f]+)/i)?.[1];
  return profile ? `${name} ${profile}` : name;
}
