import { describe, expect, it } from 'vitest';
import { emaPeriod, fillMarks, points, timeframes, type Frame } from './candles';
import { nyOffset } from './format';
import type { Candle } from './protocol';

const open = 1_577_716_200; // 2019-12-30 09:30:00 ET, epoch seconds
const est = -5 * 3_600;
const frame = (seconds: number): Frame => ({ seconds, scale: 10_000, offset: () => est });
// A 1 s bar `second`s after the open, its prices in dollars.
const bar = (second: number, o: number, h: number, l: number, c: number, v: number): Candle => [
  (open + second) * 1e9,
  ...([o, h, l, c].map((dollars) => Math.round(dollars * 10_000)) as [number, number, number, number]),
  v,
];

const day = [
  bar(0, 10, 10.5, 9.5, 10.2, 100),
  bar(1, 10.2, 10.3, 10.1, 10.1, 50),
  bar(59, 10.1, 11, 9, 10.9, 10), // its low is below the first bar's, so a bucket's low is a minimum, not the first low
  bar(60, 10.9, 10.9, 10.8, 10.8, 20),
  bar(3_599, 10.8, 10.8, 10, 10, 5),
  bar(3_600, 10, 10.1, 10, 10.1, 7),
];

describe('points', () => {
  it('aggregates 1 s bars into every timeframe, on New York wall-clock time', () => {
    const bySeconds = Object.fromEntries(
      timeframes.map((tf) => [tf, points(day, [], frame(tf)).map((p) => [p.time - (open + est), p.bar])]),
    );
    expect(bySeconds[1].map(([t]) => t)).toEqual([0, 1, 59, 60, 3_599, 3_600]);
    expect(bySeconds[5].map(([t]) => t)).toEqual([0, 55, 60, 3_595, 3_600]);
    expect(bySeconds[15].map(([t]) => t)).toEqual([0, 45, 60, 3_585, 3_600]);
    expect(bySeconds[60]).toEqual([
      [0, { open: 10, high: 11, low: 9, close: 10.9, volume: 160 }],
      [60, { open: 10.9, high: 10.9, low: 10.8, close: 10.8, volume: 20 }],
      [3_540, { open: 10.8, high: 10.8, low: 10, close: 10, volume: 5 }],
      [3_600, { open: 10, high: 10.1, low: 10, close: 10.1, volume: 7 }],
    ]);
    expect(bySeconds[300].map(([t]) => t)).toEqual([0, 3_300, 3_600]);
    expect(bySeconds[900].map(([t]) => t)).toEqual([0, 2_700, 3_600]);
    expect(bySeconds[3_600]).toEqual([
      [-1_800, { open: 10, high: 11, low: 9, close: 10.8, volume: 180 }], // hours start on the hour: 09:00
      [1_800, { open: 10.8, high: 10.8, low: 10, close: 10.1, volume: 12 }],
    ]);
    expect(points(day, [], frame(60))[0].time % 86_400).toBe(9.5 * 3_600); // the axis reads 09:30
  });

  it('computes VWAP from 1 s bars on every timeframe, and an EMA of each timeframe close', () => {
    const typical = (b: Candle) => (b[2] + b[3] + b[4]) / 3 / 10_000;
    const vwap = day.reduce((sum, b) => sum + typical(b) * b[5], 0) / day.reduce((sum, b) => sum + b[5], 0);
    for (const tf of timeframes) expect(points(day, [], frame(tf)).at(-1)!.vwap).toBeCloseTo(vwap, 12);
    const alpha = 2 / (emaPeriod + 1);
    const closes = points(day, [], frame(60)).map((p) => p.bar!.close);
    const ema = closes.slice(1).reduce((e, c) => e + alpha * (c - e), closes[0]);
    expect(points(day, [], frame(60)).at(-1)!.ema).toBeCloseTo(ema, 12);
    const [first, second] = points([bar(0, 10, 10, 10, 10, 1), bar(1, 10, 11, 10, 11, 1)], [], frame(1));
    expect([first.ema, second.ema]).toEqual([10, 10 + (2 / 21) * 1]); // the first close seeds it, then alpha = 2 / 21
  });

  it('adds a bar-less point for every bucket of a halt, carrying VWAP and EMA across it', () => {
    const halt: [number, number] = [(open + 61) * 1e9, (open + 63.5) * 1e9];
    const p = points(day, [halt], frame(1));
    const halted = p.filter((x) => x.halted);
    expect(halted.map((x) => [x.time - (open + est), x.bar === null])).toEqual([
      [61, true],
      [62, true],
      [63, true],
    ]);
    expect(halted[2].ema).toBe(p.find((x) => x.time - (open + est) === 60)!.ema);
    // Before the first bar there is no VWAP and no EMA to carry.
    const early: [number, number] = [(open - 3) * 1e9, (open - 1.5) * 1e9];
    const [a, b, c] = points(day, [early], frame(1));
    expect([a, b].map((x) => [x.time - (open + est), x.halted, x.bar, x.vwap, x.ema])).toEqual([
      [-3, true, null, null, null],
      [-2, true, null, null, null],
    ]);
    expect([c.time - (open + est), c.halted, c.ema]).toEqual([0, false, 10.2]);
    expect(c.vwap).toBeCloseTo((10.5 + 9.5 + 10.2) / 3, 12);
  });

  it('continues from a seed exactly as a full rebuild would', () => {
    const halt: [number, number] = [(open + 61) * 1e9, (open + 62) * 1e9];
    for (const tf of timeframes) {
      const all = points(day, [halt], frame(tf));
      for (let k = 1; k < all.length; k++) {
        expect([...all.slice(0, k), ...points(day, [halt], frame(tf), all[k - 1])]).toEqual(all);
      }
    }
  });
});

describe('fillMarks', () => {
  const fill = (second: number, side: 'buy' | 'sell', quantity: number) => ({
    exchange_ts: (open + second) * 1e9,
    side,
    quantity,
  });

  it('sums our fills into one mark per bucket and side, oldest first', () => {
    const fills = [fill(70, 'sell', 100), fill(5, 'buy', 100), fill(30, 'buy', 50), fill(65, 'sell', 25)];
    expect(fillMarks(fills, frame(60))).toEqual([
      { time: open + est, side: 'buy', shares: 150 },
      { time: open + est + 60, side: 'sell', shares: 125 },
    ]);
    expect(fillMarks(fills, frame(1))).toHaveLength(4);
  });
});

describe('New York time across days', () => {
  it('puts each day on its own offset, so a day after a daylight-saving change still opens at 09:30', () => {
    const october = Date.UTC(2019, 9, 30, 13, 30) / 1_000; // 09:30 EDT
    const december = Date.UTC(2019, 11, 30, 14, 30) / 1_000; // 09:30 EST
    const f: Frame = { seconds: 60, scale: 10_000, offset: nyOffset };
    const day = (s: number): Candle => [s * 1e9, 100_000, 100_000, 100_000, 100_000, 100];
    expect(points([day(october), day(december)], [], f).map((p) => p.time % 86_400)).toEqual([34_200, 34_200]);
  });

  it('restarts VWAP each New York day', () => {
    const today = bar(0, 10, 10, 10, 10, 100);
    const tomorrow: Candle = [(open + 86_400) * 1e9, 200_000, 200_000, 200_000, 200_000, 100];
    expect(points([today, tomorrow], [], frame(60)).map((p) => p.vwap)).toEqual([10, 20]);
  });
});
