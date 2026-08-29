'use client';

import { useEffect, useRef, useState } from 'react';

/**
 * Snapshot of the PRD section 2 KPIs, sampled from RTCPeerConnection.getStats().
 *
 * Everything here is measured, never estimated. Fields that Chrome has not
 * reported yet stay null so the HUD can show a dash rather than a misleading
 * zero - a real 0ms RTT and "no sample yet" must not look identical.
 */
export interface StreamStats {
  /** framesDecoded delta over the sample window. */
  fps: number | null;
  width: number | null;
  height: number | null;
  /** Receive bitrate in Mbps, from the bytesReceived delta. */
  bitrateMbps: number | null;
  /** Percentage of frames dropped since the connection opened. */
  frameDropPercent: number | null;
  /** Selected candidate pair RTT, milliseconds. */
  rttMs: number | null;
  /** Mean jitter buffer delay per frame, milliseconds. */
  jitterBufferMs: number | null;
  /** Mean decode time per frame, milliseconds. */
  decodeMs: number | null;
  packetsLost: number | null;
  /** Percentage of packets lost since the connection opened. */
  packetLossPercent: number | null;
  /** Chrome reports its decoder; "ExternalDecoder" means GPU. */
  decoderImplementation: string | null;
  /** True when Chrome confirms hardware-accelerated decoding. */
  hardwareDecode: boolean | null;
  audioJitterMs: number | null;
  /** ICE candidate pair actually carrying media, for the connectivity check. */
  candidatePair: string | null;
  nominatedLocalType: string | null;
  nominatedRemoteType: string | null;
}

const EMPTY: StreamStats = {
  fps: null,
  width: null,
  height: null,
  bitrateMbps: null,
  frameDropPercent: null,
  rttMs: null,
  jitterBufferMs: null,
  decodeMs: null,
  packetsLost: null,
  packetLossPercent: null,
  decoderImplementation: null,
  hardwareDecode: null,
  audioJitterMs: null,
  candidatePair: null,
  nominatedLocalType: null,
  nominatedRemoteType: null,
};

interface Sample {
  t: number;
  bytesReceived: number;
  framesDecoded: number;
  totalDecodeTime: number;
  jitterBufferDelay: number;
  jitterBufferEmittedCount: number;
}

/**
 * Polls getStats() on an interval and derives per-second rates from deltas.
 *
 * Chrome reports cumulative counters, so a single report cannot give bitrate
 * or FPS - both need two samples. The first tick therefore populates only the
 * absolute fields.
 */
export function useStreamStats(
  pc: React.RefObject<RTCPeerConnection | null>,
  intervalMs = 1000,
): StreamStats {
  const [stats, setStats] = useState<StreamStats>(EMPTY);
  const previous = useRef<Sample | null>(null);

  useEffect(() => {
    let cancelled = false;

    const tick = async () => {
      const connection = pc.current;
      if (!connection || connection.connectionState !== 'connected') {
        previous.current = null;
        if (!cancelled) setStats(EMPTY);
        return;
      }

      let report: RTCStatsReport;
      try {
        report = await connection.getStats();
      } catch {
        return;
      }
      if (cancelled) return;

      const next: StreamStats = { ...EMPTY };
      let sample: Sample | null = null;
      let candidatePairId: string | null = null;
      const candidates = new Map<string, { candidateType?: string; protocol?: string }>();

      report.forEach((entry: Record<string, unknown>) => {
        const type = entry['type'] as string;

        if (type === 'inbound-rtp' && entry['kind'] === 'video') {
          const framesDecoded = (entry['framesDecoded'] as number) ?? 0;
          const framesReceived = (entry['framesReceived'] as number) ?? 0;
          const framesDropped = (entry['framesDropped'] as number) ?? 0;
          const packetsLost = (entry['packetsLost'] as number) ?? 0;
          const packetsReceived = (entry['packetsReceived'] as number) ?? 0;

          next.width = (entry['frameWidth'] as number) ?? null;
          next.height = (entry['frameHeight'] as number) ?? null;
          next.packetsLost = packetsLost;
          next.decoderImplementation = (entry['decoderImplementation'] as string) ?? null;
          // powerEfficientDecoder is the authoritative signal where Chrome
          // provides it; the implementation string is the older fallback.
          const powerEfficient = entry['powerEfficientDecoder'] as boolean | undefined;
          next.hardwareDecode =
            powerEfficient ??
            (next.decoderImplementation
              ? !/ffmpeg|libvpx|openh264/i.test(next.decoderImplementation)
              : null);

          if (framesReceived > 0) {
            next.frameDropPercent = (framesDropped / framesReceived) * 100;
          }
          const totalPackets = packetsReceived + packetsLost;
          if (totalPackets > 0) {
            next.packetLossPercent = (packetsLost / totalPackets) * 100;
          }

          sample = {
            t: performance.now(),
            bytesReceived: (entry['bytesReceived'] as number) ?? 0,
            framesDecoded,
            totalDecodeTime: (entry['totalDecodeTime'] as number) ?? 0,
            jitterBufferDelay: (entry['jitterBufferDelay'] as number) ?? 0,
            jitterBufferEmittedCount: (entry['jitterBufferEmittedCount'] as number) ?? 0,
          };
        }

        if (type === 'inbound-rtp' && entry['kind'] === 'audio') {
          const jitter = entry['jitter'] as number | undefined;
          next.audioJitterMs = jitter !== undefined ? jitter * 1000 : null;
        }

        if (type === 'transport') {
          candidatePairId = (entry['selectedCandidatePairId'] as string) ?? candidatePairId;
        }

        if (type === 'candidate-pair' && entry['nominated'] === true && entry['state'] === 'succeeded') {
          const rtt = entry['currentRoundTripTime'] as number | undefined;
          if (rtt !== undefined) next.rttMs = rtt * 1000;
          candidatePairId = (entry['id'] as string) ?? candidatePairId;
          next.candidatePair = `${entry['localCandidateId']}>${entry['remoteCandidateId']}`;
        }

        if (type === 'local-candidate' || type === 'remote-candidate') {
          candidates.set(entry['id'] as string, {
            candidateType: entry['candidateType'] as string | undefined,
            protocol: entry['protocol'] as string | undefined,
          });
        }
      });

      // Resolve the nominated pair into readable candidate types. "host>host"
      // is the direct path PRD section 4.5 expects; "relay" anywhere means
      // traffic is going through TURN and the latency budget is blown.
      if (next.candidatePair) {
        const [localId, remoteId] = next.candidatePair.split('>');
        next.nominatedLocalType = candidates.get(localId ?? '')?.candidateType ?? null;
        next.nominatedRemoteType = candidates.get(remoteId ?? '')?.candidateType ?? null;
      }

      const current = sample as Sample | null;
      const last = previous.current;
      if (current && last) {
        const dt = (current.t - last.t) / 1000;
        if (dt > 0) {
          const bytes = current.bytesReceived - last.bytesReceived;
          next.bitrateMbps = (bytes * 8) / dt / 1_000_000;

          const frames = current.framesDecoded - last.framesDecoded;
          next.fps = frames / dt;

          const decodeDelta = current.totalDecodeTime - last.totalDecodeTime;
          if (frames > 0) next.decodeMs = (decodeDelta / frames) * 1000;

          const emitted = current.jitterBufferEmittedCount - last.jitterBufferEmittedCount;
          const delay = current.jitterBufferDelay - last.jitterBufferDelay;
          if (emitted > 0) next.jitterBufferMs = (delay / emitted) * 1000;
        }
      }
      if (current) previous.current = current;

      setStats(next);
    };

    void tick();
    const timer = window.setInterval(() => void tick(), intervalMs);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, [pc, intervalMs]);

  return stats;
}
