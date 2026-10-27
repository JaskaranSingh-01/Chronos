import type { Candle, Side } from './protocol';

// The chart's timeframes in seconds: 1s, 5s, 15s, 1m, 5m, 15m, 1h.
export const timeframes = [1, 5, 15, 60, 300, 900, 3_600];
export const emaPeriod = 20;

export interface Frame {
  seconds: number; // the bucket width
  scale: number; // ticks per dollar
  offset: (epochSeconds: number) => number; // New York's UTC offset then, so the chart's UTC axis reads New York time
}

export interface Point {
  time: number; // the bucket's start, in New York wall-clock seconds
  bar: { open: number; high: number; low: number; close: number; volume: number } | null; // null: halted, no trades
  vwap: number | null;
  ema: number | null;
  halted: boolean;
  pv: number; // the day's running sum of typical price x volume over 1 s bars, so VWAP is the same on every timeframe
  v: number;
}

// One point per bucket that has a 1 s bar or overlaps a halt, oldest first; VWAP restarts each New York day, EMA runs
// on. With `seed`, only the buckets after it, carrying its VWAP and EMA on.
export function points(bars: readonly Candle[], halts: readonly [number, number][], f: Frame, seed?: Point): Point[] {
  const bucket = (ns: number) => {
    const s = Math.floor(ns / 1e9);
    const t = s + f.offset(s);
    return t - (t % f.seconds);
  };
  const from = seed ? seed.time + f.seconds : -Infinity;
  const halted = new Set<number>();
  for (const [start, end] of halts) {
    for (let t = Math.max(bucket(start), from); t <= bucket(end); t += f.seconds) halted.add(t);
  }
  let i = bars.length;
  while (i > 0 && bucket(bars[i - 1][0]) >= from) i--;
  const times = [...new Set([...bars.slice(i).map((b) => bucket(b[0])), ...halted])].sort((a, b) => a - b);
  const alpha = 2 / (emaPeriod + 1);
  const out: Point[] = [];
  let prev = seed;
  for (const time of times) {
    let bar: Point['bar'] = null;
    const today = prev && Math.floor(prev.time / 86_400) === Math.floor(time / 86_400) ? prev : undefined;
    let pv = today?.pv ?? 0;
    let v = today?.v ?? 0;
    for (; i < bars.length && bucket(bars[i][0]) === time; i++) {
      const volume = bars[i][5];
      const [open, high, low, close] = bars[i].slice(1, 5).map((ticks) => ticks / f.scale);
      bar = bar
        ? {
            open: bar.open,
            high: Math.max(bar.high, high),
            low: Math.min(bar.low, low),
            close,
            volume: bar.volume + volume,
          }
        : { open, high, low, close, volume };
      pv += ((high + low + close) / 3) * volume;
      v += volume;
    }
    const ema = bar ? (prev?.ema == null ? bar.close : prev.ema + alpha * (bar.close - prev.ema)) : (prev?.ema ?? null);
    prev = { time, bar, vwap: v > 0 ? pv / v : null, ema, halted: halted.has(time), pv, v };
    out.push(prev);
  }
  return out;
}

export interface FillMark {
  time: number; // the bucket's start, as Point.time
  side: Side;
  shares: number;
}

// Our fills as one mark per bucket and side, oldest first, their shares summed: a run's thousands of fills draw as at
// most two arrows a candle.
export function fillMarks(
  fills: readonly { exchange_ts: number; side: Side; quantity: number }[],
  f: Frame,
): FillMark[] {
  const marks = new Map<string, FillMark>();
  for (const fill of fills) {
    const s = Math.floor(fill.exchange_ts / 1e9);
    const t = s + f.offset(s);
    const time = t - (t % f.seconds);
    const key = `${time} ${fill.side}`;
    const mark = marks.get(key);
    if (mark) mark.shares += fill.quantity;
    else marks.set(key, { time, side: fill.side, shares: fill.quantity });
  }
  return [...marks.values()].sort((a, b) => a.time - b.time || (a.side === 'buy' ? -1 : 1));
}
