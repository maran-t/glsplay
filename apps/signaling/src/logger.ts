/**
 * Minimal structured logger. Deliberately dependency-free: the broker is the
 * one component that must stay up while everything else is being debugged, so
 * it carries as little as possible.
 */

type Level = 'debug' | 'info' | 'warn' | 'error';

const ORDER: Record<Level, number> = { debug: 10, info: 20, warn: 30, error: 40 };
const threshold = ORDER[(process.env['GLSPLAY_LOG_LEVEL'] as Level) ?? 'info'] ?? 20;

function emit(level: Level, msg: string, fields?: Record<string, unknown>): void {
  if (ORDER[level] < threshold) return;
  const suffix = fields
    ? ' ' +
      Object.entries(fields)
        .filter(([, v]) => v !== undefined)
        .map(([k, v]) => `${k}=${typeof v === 'string' ? v : JSON.stringify(v)}`)
        .join(' ')
    : '';
  const line = `${new Date().toISOString()} ${level.toUpperCase().padEnd(5)} ${msg}${suffix}`;
  if (level === 'error' || level === 'warn') process.stderr.write(line + '\n');
  else process.stdout.write(line + '\n');
}

export const log = {
  debug: (m: string, f?: Record<string, unknown>) => emit('debug', m, f),
  info: (m: string, f?: Record<string, unknown>) => emit('info', m, f),
  warn: (m: string, f?: Record<string, unknown>) => emit('warn', m, f),
  error: (m: string, f?: Record<string, unknown>) => emit('error', m, f),
};
