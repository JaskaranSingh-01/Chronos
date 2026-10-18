import {
  CandlestickSeries,
  ColorType,
  createChart,
  createSeriesMarkers,
  HistogramSeries,
  LineSeries,
  LineStyle,
  type CreatePriceLineOptions,
  type IPriceLine,
  type Time,
} from 'lightweight-charts';
import { useEffect, useRef, useState } from 'react';
import { points, timeframes, type Frame, type Point } from './candles';
import { etOffset } from './format';
import type { Execution } from './protocol';
import { store, type State } from './store';

const color = {
  buy: '#26a69a',
  sell: '#ef5350',
  vwap: '#f0b90b',
  ema: '#b07cff',
  order: '#58a6ff',
  position: '#e6edf3',
};
const label = (seconds: number) =>
  seconds < 60 ? `${seconds}s` : seconds < 3_600 ? `${seconds / 60}m` : `${seconds / 3_600}h`;

// The background behind a bucket: the replay cursor on the last one, then halts, then outside 09:30-16:00 New York.
function shade(p: Point, last: boolean): string {
  if (last) return 'rgba(88, 166, 255, 0.14)';
  if (p.halted) return 'rgba(239, 83, 80, 0.18)';
  const second = p.time % 86_400;
  return second < 34_200 || second >= 57_600 ? 'rgba(125, 142, 163, 0.10)' : 'rgba(0, 0, 0, 0)';
}

export function Chart() {
  const box = useRef<HTMLDivElement>(null);
  const legend = useRef<HTMLDivElement>(null);
  const [seconds, setSeconds] = useState(60);

  useEffect(() => {
    const chart = createChart(box.current!, {
      autoSize: true,
      layout: {
        background: { type: ColorType.Solid, color: '#151c25' },
        textColor: '#c9d1d9',
        panes: { separatorColor: '#243041' },
      },
      grid: { vertLines: { color: '#1c2533' }, horzLines: { color: '#1c2533' } },
      timeScale: { timeVisible: true, secondsVisible: seconds < 60, rightOffset: 15 }, // room for the price-line titles
    });
    const quiet = { priceLineVisible: false, lastValueVisible: false };
    const background = chart.addSeries(HistogramSeries, { ...quiet, priceScaleId: 'shade' });
    chart.priceScale('shade').applyOptions({ visible: false, scaleMargins: { top: 0, bottom: 0 } });
    const candles = chart.addSeries(CandlestickSeries, {
      upColor: color.buy,
      downColor: color.sell,
      wickUpColor: color.buy,
      wickDownColor: color.sell,
      borderVisible: false,
    });
    const line = { ...quiet, lineWidth: 1, crosshairMarkerVisible: false } as const;
    const vwap = chart.addSeries(LineSeries, { ...line, color: color.vwap });
    const ema = chart.addSeries(LineSeries, { ...line, color: color.ema });
    const volume = chart.addSeries(HistogramSeries, { ...quiet, priceFormat: { type: 'volume' } }, 1);
    chart.panes()[1].setHeight(70);
    const markers = createSeriesMarkers(candles);
    const lines = new Map<string, IPriceLine>();
    let drawn: Point[] = [];
    let version = -1;
    // What the last draw saw: a notification that changed none of it redraws nothing.
    let seenBars: State['bars'] | undefined;
    let seenHalts: State['halts'] | undefined;
    let seenSnap: State['snap'] | undefined;
    let frame: Frame = { seconds, scale: 1, offset: 0 };
    let fills: Execution[] = [];

    // One bucket's row in each series; a bucket with no trades (a halt) is whitespace except for its shading.
    const rows = (p: Point, last: boolean) => {
      const time = p.time as Time;
      const bar = p.bar;
      return {
        candle: bar ? { time, open: bar.open, high: bar.high, low: bar.low, close: bar.close } : { time },
        volume: bar ? { time, value: bar.volume, color: bar.close >= bar.open ? '#26a69a66' : '#ef535066' } : { time },
        vwap: p.vwap === null ? { time } : { time, value: p.vwap },
        ema: p.ema === null ? { time } : { time, value: p.ema },
        shade: { time, value: 1, color: shade(p, last) },
      };
    };

    function draw(s: State) {
      const moved = s.bars !== seenBars || s.halts !== seenHalts;
      const rebuild = version !== s.barsVersion || (moved && drawn.length === 0);
      if (rebuild) {
        const first = s.bars[0]?.[0] ?? s.halts[0]?.[0];
        frame = {
          seconds,
          scale: 10 ** (s.hello?.price_scale ?? 4),
          offset: first === undefined ? 0 : etOffset(first / 1e9),
        };
        drawn = points(s.bars, s.halts, frame);
        version = s.barsVersion;
        const all = drawn.map((p, i) => rows(p, i === drawn.length - 1));
        candles.setData(all.map((r) => r.candle));
        volume.setData(all.map((r) => r.volume));
        vwap.setData(all.map((r) => r.vwap));
        ema.setData(all.map((r) => r.ema));
        background.setData(all.map((r) => r.shade));
      } else if (moved) {
        // Only the last bucket can change: it is redrawn, and the new ones after it appended.
        const tail = points(s.bars, s.halts, frame, drawn.at(-2));
        drawn.length = Math.max(drawn.length - 1, 0);
        drawn.push(...tail);
        tail.forEach((p, i) => {
          const r = rows(p, i === tail.length - 1);
          candles.update(r.candle);
          volume.update(r.volume);
          vwap.update(r.vwap);
          ema.update(r.ema);
          background.update(r.shade);
        });
      }
      seenBars = s.bars;
      seenHalts = s.halts;
      if (rebuild || s.fills !== fills) {
        fills = s.fills;
        markers.setMarkers(
          fills.map((f) => {
            const t = Math.floor(f.exchange_ts / 1e9) + frame.offset;
            const buy = f.side === 'buy';
            return {
              time: (t - (t % frame.seconds)) as Time,
              position: buy ? 'belowBar' : 'aboveBar',
              shape: buy ? 'arrowUp' : 'arrowDown',
              color: buy ? color.buy : color.sell,
              text: `${buy ? 'B' : 'S'} ${f.quantity}`,
            };
          }),
        );
      }
      const snapChanged = rebuild || s.snap !== seenSnap;
      seenSnap = s.snap;
      if (snapChanged) priceLines(s);
      if ((snapChanged || moved) && hovered === undefined) showLegend();
    }

    // Best bid and ask, our resting orders and our position's average price, reconciled by key.
    function priceLines(s: State) {
      const want = new Map<string, CreatePriceLineOptions>();
      const snap = s.snap;
      if (snap) {
        const at = (ticks: number) => ticks / frame.scale;
        const dotted = { lineWidth: 1, lineStyle: LineStyle.Dotted, axisLabelVisible: true } as const;
        if (snap.bids[0]) want.set('bid', { ...dotted, price: at(snap.bids[0][0]), color: color.buy, title: 'bid' });
        if (snap.asks[0]) want.set('ask', { ...dotted, price: at(snap.asks[0][0]), color: color.sell, title: 'ask' });
        for (const o of snap.my_orders) {
          const title = `${o.side === 'buy' ? 'B' : 'S'} ${o.remaining} · ${o.ahead_quantity} ahead`;
          want.set(`order ${o.id}`, {
            ...dotted,
            lineStyle: LineStyle.Dashed,
            price: at(o.price),
            color: color.order,
            title,
          });
        }
        const { position, realised, cash } = snap.account;
        if (position !== 0) {
          const average = at((realised - cash) / position); // realised = cash + cost basis
          want.set('position', {
            ...dotted,
            lineStyle: LineStyle.Solid,
            price: average,
            color: color.position,
            title: `pos ${position}`,
          });
        }
      }
      for (const [key, line] of lines) {
        if (!want.has(key)) {
          candles.removePriceLine(line);
          lines.delete(key);
        }
      }
      for (const [key, options] of want) {
        const line = lines.get(key);
        if (line) line.applyOptions(options);
        else lines.set(key, candles.createPriceLine(options));
      }
    }

    // The OHLCV legend: the bar under the crosshair, else the latest.
    let hovered: Time | undefined;
    const showLegend = () => {
      const bar = (hovered === undefined ? drawn.findLast((p) => p.bar) : drawn.find((p) => p.time === hovered))?.bar;
      legend.current!.textContent = bar
        ? `O ${bar.open.toFixed(2)} H ${bar.high.toFixed(2)} L ${bar.low.toFixed(2)} C ${bar.close.toFixed(2)} V ${bar.volume}`
        : '';
    };
    chart.subscribeCrosshairMove((param) => {
      hovered = param.time;
      showLegend();
    });
    draw(store.get());
    const unsubscribe = store.subscribe(() => draw(store.get()));
    return () => {
      unsubscribe();
      chart.remove();
    };
  }, [seconds]);

  return (
    <div className="panel chart">
      <div className="title">
        Chart ·{' '}
        {timeframes.map((tf) => (
          <button key={tf} className={tf === seconds ? 'on' : ''} onClick={() => setSeconds(tf)}>
            {label(tf)}
          </button>
        ))}{' '}
        · <span style={{ color: color.vwap }}>VWAP</span> · <span style={{ color: color.ema }}>EMA 20</span> · Volume
      </div>
      <div className="legend" ref={legend} />
      <div className="plot" ref={box} />
    </div>
  );
}
