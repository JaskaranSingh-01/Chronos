export interface PnlCurve {
  d: string; // the SVG path over a 1000 × 100 box
  zero: number; // the zero line's y in that box
  lo: number; // the lowest and highest sample, zero included, in money units
  hi: number;
  from: number; // the first and last sample's exchange time, ns
  to: number;
}

// The PnL samples from the last flat one before the first change, as an SVG path of at most about `vertices` points:
// each bucket of samples keeps its lowest and its highest, in time order, so a dip survives the thinning. The range
// is every sample's. Two samples at least.
export function pnlCurve(all: readonly [number, number][], vertices = 480): PnlCurve {
  const pnl = all.slice(Math.max(0, all.findIndex((p) => p[1] !== 0) - 1));
  let lo = 0;
  let hi = 0;
  for (const [, v] of pnl) {
    lo = Math.min(lo, v);
    hi = Math.max(hi, v);
  }
  const k = Math.max(1, Math.ceil((2 * pnl.length) / vertices));
  const kept: [number, number][] = [];
  for (let i = 0; i < pnl.length; i += k) {
    let min = pnl[i];
    let max = pnl[i];
    for (let j = i + 1; j < Math.min(i + k, pnl.length); j++) {
      if (pnl[j][1] < min[1]) min = pnl[j];
      if (pnl[j][1] > max[1]) max = pnl[j];
    }
    if (min === max) kept.push(min);
    else kept.push(...(min[0] < max[0] ? [min, max] : [max, min]));
  }
  if (kept.at(-1) !== pnl.at(-1)) kept.push(pnl.at(-1)!);
  const from = pnl[0][0];
  const to = pnl.at(-1)![0];
  const span = Math.max(1, to - from);
  const y = (v: number) => (hi === lo ? 50 : 4 + (92 * (hi - v)) / (hi - lo));
  const d = kept.map(([t, v], i) => `${i ? 'L' : 'M'}${((1000 * (t - from)) / span).toFixed(1)} ${y(v).toFixed(1)}`);
  return { d: d.join(' '), zero: y(0), lo, hi, from, to };
}
