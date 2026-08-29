'use client';

import type { HostHelloMessage, HostStatsMessage } from '@glsplay/protocol';
import type { StreamStats } from '@/hooks/useStreamStats';

/**
 * Targets from PRD section 2. Each metric is scored against its POC target and
 * its stretch target so the HUD shows progress, not just a number.
 */
interface Threshold {
  /** Meets the stretch target. */
  stretch: number;
  /** Meets the POC target. */
  target: number;
  /** True when smaller is better. */
  lowerIsBetter: boolean;
}

const THRESHOLDS: Record<string, Threshold> = {
  fps: { stretch: 60, target: 58, lowerIsBetter: false },
  rttMs: { stretch: 18, target: 25, lowerIsBetter: true },
  frameDropPercent: { stretch: 0.1, target: 0.5, lowerIsBetter: true },
  bitrateMbps: { stretch: 15, target: 10, lowerIsBetter: false },
  jitterBufferMs: { stretch: 10, target: 25, lowerIsBetter: true },
  decodeMs: { stretch: 3, target: 5, lowerIsBetter: true },
};

type Grade = 'good' | 'warn' | 'bad' | 'none';

function grade(key: string, value: number | null): Grade {
  const t = THRESHOLDS[key];
  if (t === undefined || value === null) return 'none';
  if (t.lowerIsBetter) {
    if (value <= t.stretch) return 'good';
    if (value <= t.target) return 'warn';
    return 'bad';
  }
  if (value >= t.stretch) return 'good';
  if (value >= t.target) return 'warn';
  return 'bad';
}

const GRADE_CLASS: Record<Grade, string> = {
  good: 'text-good',
  warn: 'text-warn',
  bad: 'text-bad',
  none: 'text-muted',
};

function fmt(value: number | null, digits = 1, suffix = ''): string {
  if (value === null || Number.isNaN(value)) return '--';
  return value.toFixed(digits) + suffix;
}

function Row({
  label,
  value,
  gradeKey,
  raw,
}: {
  label: string;
  value: string;
  gradeKey?: string;
  raw?: number | null;
}) {
  const g = gradeKey ? grade(gradeKey, raw ?? null) : 'none';
  return (
    <div className="flex items-baseline justify-between gap-4">
      <span className="text-muted">{label}</span>
      <span className={`tnum tabular-nums ${GRADE_CLASS[g]}`}>{value}</span>
    </div>
  );
}

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div className="flex flex-col gap-1">
      <div className="text-[10px] uppercase tracking-[0.12em] text-muted/70">{title}</div>
      <div className="flex flex-col gap-0.5">{children}</div>
    </div>
  );
}

export interface StatsOverlayProps {
  stats: StreamStats;
  hostStats: HostStatsMessage | null;
  hello: HostHelloMessage | null;
  negotiatedVideo: string | null;
  pointerLocked: boolean;
  gamepads: number;
  droppedInput: number;
  visible: boolean;
}

/**
 * Telemetry HUD.
 *
 * Deliberately shows client and host numbers side by side: the interesting
 * failures live in the gap between them. A healthy 60fps decode alongside a
 * host reporting 45fps encode means the encoder is the bottleneck, and no
 * single-sided view would tell you that.
 */
export function StatsOverlay({
  stats,
  hostStats,
  hello,
  negotiatedVideo,
  pointerLocked,
  gamepads,
  droppedInput,
  visible,
}: StatsOverlayProps) {
  if (!visible) return null;

  const resolution =
    stats.width !== null && stats.height !== null ? `${stats.width}x${stats.height}` : '--';

  // PRD section 5 budget, minus the parts only a camera can measure. Half the
  // RTT approximates the downstream network leg.
  const networkDown = stats.rttMs !== null ? stats.rttMs / 2 : null;
  const estimatedGlassToGlass =
    stats.rttMs !== null && stats.decodeMs !== null
      ? stats.rttMs +
        (hostStats?.captureMs ?? 0) +
        (hostStats?.encodeMs ?? 0) +
        stats.decodeMs +
        (stats.jitterBufferMs ?? 0)
      : null;

  const path =
    stats.nominatedLocalType && stats.nominatedRemoteType
      ? `${stats.nominatedLocalType} > ${stats.nominatedRemoteType}`
      : '--';
  const relayed = path.includes('relay');

  return (
    <div
      className="pointer-events-none absolute right-3 top-3 w-[248px] select-text rounded-md border border-edge bg-panel/92 p-3 font-mono text-hud shadow-2xl backdrop-blur-sm"
      role="status"
      aria-live="off"
      aria-label="Stream telemetry"
    >
      <div className="mb-2 flex items-baseline justify-between border-b border-edge pb-2">
        <span className="text-[11px] font-semibold tracking-wide text-ink">glsplay</span>
        <span className="text-[10px] text-muted">{negotiatedVideo ?? 'negotiating'}</span>
      </div>

      <div className="flex flex-col gap-2.5">
        <Section title="Video">
          <Row label="Resolution" value={resolution} />
          <Row label="FPS" value={fmt(stats.fps, 1)} gradeKey="fps" raw={stats.fps} />
          <Row
            label="Bitrate"
            value={fmt(stats.bitrateMbps, 2, ' Mbps')}
            gradeKey="bitrateMbps"
            raw={stats.bitrateMbps}
          />
          <Row
            label="Frame drops"
            value={fmt(stats.frameDropPercent, 2, '%')}
            gradeKey="frameDropPercent"
            raw={stats.frameDropPercent}
          />
          <Row
            label="Decoder"
            value={stats.hardwareDecode === null ? '--' : stats.hardwareDecode ? 'GPU' : 'SOFTWARE'}
          />
        </Section>

        <Section title="Network">
          <Row label="RTT" value={fmt(stats.rttMs, 1, ' ms')} gradeKey="rttMs" raw={stats.rttMs} />
          <Row label="Loss" value={fmt(stats.packetLossPercent, 2, '%')} />
          <Row
            label="Jitter buf"
            value={fmt(stats.jitterBufferMs, 1, ' ms')}
            gradeKey="jitterBufferMs"
            raw={stats.jitterBufferMs}
          />
          <div className="flex items-baseline justify-between gap-4">
            <span className="text-muted">Path</span>
            <span className={relayed ? 'text-bad' : 'text-good'}>{path}</span>
          </div>
        </Section>

        <Section title="Latency budget">
          <Row label="Capture" value={fmt(hostStats?.captureMs ?? null, 1, ' ms')} />
          <Row label="Encode" value={fmt(hostStats?.encodeMs ?? null, 1, ' ms')} />
          <Row label="Net down" value={fmt(networkDown, 1, ' ms')} />
          <Row
            label="Decode"
            value={fmt(stats.decodeMs, 1, ' ms')}
            gradeKey="decodeMs"
            raw={stats.decodeMs}
          />
          <div className="mt-1 flex items-baseline justify-between gap-4 border-t border-edge pt-1">
            <span className="text-muted">Est. total</span>
            <span
              className={
                estimatedGlassToGlass === null
                  ? 'text-muted'
                  : estimatedGlassToGlass <= 45
                    ? 'text-good'
                    : 'text-bad'
              }
            >
              {fmt(estimatedGlassToGlass, 1, ' ms')}
            </span>
          </div>
        </Section>

        {hostStats && (
          <Section title="Host">
            <Row label="Capture FPS" value={fmt(hostStats.capturedFps, 1)} />
            <Row label="Encode FPS" value={fmt(hostStats.encodedFps, 1)} />
            <Row label="GPU" value={fmt(hostStats.gpuUtilPercent, 0, '%')} />
            <Row label="NVENC" value={fmt(hostStats.encoderUtilPercent, 0, '%')} />
            <Row label="Dropped" value={String(hostStats.droppedFrames)} />
            <Row label="Input queue" value={fmt(hostStats.inputQueueMs, 1, ' ms')} />
          </Section>
        )}

        <Section title="Input">
          <div className="flex items-baseline justify-between gap-4">
            <span className="text-muted">Pointer</span>
            <span className={pointerLocked ? 'text-good' : 'text-warn'}>
              {pointerLocked ? 'LOCKED' : 'UNLOCKED'}
            </span>
          </div>
          <Row label="Gamepads" value={String(gamepads)} />
          <div className="flex items-baseline justify-between gap-4">
            <span className="text-muted">Dropped</span>
            <span className={droppedInput > 0 ? 'text-warn' : 'text-muted'}>{droppedInput}</span>
          </div>
        </Section>

        {hello && (
          <div className="border-t border-edge pt-2 text-[10px] leading-relaxed text-muted">
            <div className="truncate" title={hello.gpu}>
              {hello.gpu}
            </div>
            <div className="truncate" title={hello.encoder}>
              {hello.encoder}
            </div>
            <div>
              {hello.display.width}x{hello.display.height} @ {hello.display.refreshHz}Hz
              {hello.captureSource === 'test-pattern' && (
                <span className="ml-1 text-warn">TEST PATTERN</span>
              )}
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
