import { describe, expect, it } from 'vitest';
import { pnlCurve } from './pnl';

const S = 1e9;

describe('pnlCurve', () => {
  it('starts at the last flat sample before the first change', () => {
    const c = pnlCurve([
      [0, 0],
      [1 * S, 0],
      [2 * S, 0],
      [3 * S, -50],
      [4 * S, 25],
    ]);
    expect([c.from, c.to, c.lo, c.hi]).toEqual([2 * S, 4 * S, -50, 25]);
    expect(c.d.split(' L')).toHaveLength(3);
  });

  it('keeps a one-second dip that thinning would drop, and takes the range from every sample', () => {
    const samples: [number, number][] = Array.from({ length: 7_200 }, (_, i) => [i * S, i === 0 ? 0 : -1]);
    samples[3_333][1] = -400;
    const c = pnlCurve(samples);
    expect(c.lo).toBe(-400);
    expect(c.d).toContain(' 96.0'); // the lowest value sits at the bottom of the box
    expect(c.d.split(' L').length).toBeLessThanOrEqual(482);
  });

  it('spaces the samples evenly, so the months between two days take no width', () => {
    const day = 86_400 * S;
    const c = pnlCurve([
      [0, 0],
      [1 * S, -10],
      [60 * day, -20],
      [60 * day + S, -30],
      [60 * day + 2 * S, -40],
    ]);
    expect(c.d).toBe('M0.0 4.0 L250.0 27.0 L500.0 50.0 L750.0 73.0 L1000.0 96.0');
  });
});
