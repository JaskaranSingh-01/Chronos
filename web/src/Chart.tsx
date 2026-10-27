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
import { fillMarks, points, timeframes, type Frame, type Point } from './candles';
import { chartDigits, nyOffset } from './format';
import type { Execution } from './protocol';
import { selectFirstRun, store, useStore, type State } from './store';

const color = {
  buy: '#4fe0a8',
  sell: '#f0484f',
  vwap: '#e8b339',
  ema: '#a78bfa',
  order: '#5aa9ff',
  position: '#f0f3f6',
};
const markTexts = 40; // fill marks are labelled with their shares while there are at most this many
const label = (seconds: number) =>
  seconds < 60 ? `${seconds}s` : seconds < 3_600 ? `${seconds / 60}m` : `${seconds / 3_600}h`;

// The background behind a bucket: the replay cursor on the last one, then halts, then outside 09:30-16:00 New York.
function shade(p: Point, last: boolean): string {
  if (last) return 'rgba(90, 169, 255, 0.14)';
  if (p.halted) return 'rgba(240, 72, 79, 0.18)';
  const second = p.time % 86_400;
  return second < 34_200 || second >= 57_600 ? 'rgba(135, 146, 160, 0.07)' : 'rgba(0, 0, 0, 0)';
}

// The symbol the chart draws, or a dash.
function ChartSymbol() {
  const symbol = useStore((s) => s.symbol);
  return <b>{symbol || '—'}</b>;
}

// Tells a controller with no symbol loaded what to do.
function FirstRun() {
  const show = useStore(selectFirstRun);
  return show ? <p className="prompt">Type a ticker (e.g. AAPL) in the Symbol box and press Enter or Load.</p> : null;
}

export function Chart() {
  const box = useRef<HTMLDivElement>(null);
  const legend = useRef<HTMLDivElement>(null);
  const [seconds, setSeconds] = useState(60);

  useEffect(() => {
    const chart = createChart(box.current!, {
      autoSize: true,
      layout: {
        background: { type: ColorType.Solid, color: '#0e1217' },
        textColor: '#8792a0',
        fontFamily: "'JetBrains Mono', 'SF Mono', Menlo, Consolas, monospace",
        fontSize: 10,
        panes: { separatorColor: '#1f2630' },
      },
      grid: { vertLines: { color: '#171d26' }, horzLines: { color: '#171d26' } },
      rightPriceScale: { borderColor: '#1f2630' },
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
    let digits = 2; // the price decimals on the axis and legend; the series start at lightweight-charts' default
    // What the last draw saw: a notification that changed none of it redraws nothing.
    let seenBars: State['bars'] | undefined;
    let seenHalts: State['halts'] | undefined;
    let seenSnap: State['snap'] | undefined;
    let frame: Frame = { seconds, scale: 1, offset: nyOffset };
    let fills: Execution[] = [];

    // One bucket's row in each series; a bucket with no trades (a halt) is whitespace except for its shading.
    const rows = (p: Point, last: boolean) => {
      const time = p.time as Time;
      const bar = p.bar;
      return {
        candle: bar ? { time, open: bar.open, high: bar.high, low: bar.low, close: bar.close } : { time },
        volume: bar ? { time, value: bar.volume, color: bar.close >= bar.open ? '#4fe0a859' : '#f0484f66' } : { time },
        vwap: p.vwap === null ? { time } : { time, value: p.vwap },
        ema: p.ema === null ? { time } : { time, value: p.ema },
        shade: { time, value: 1, color: shade(p, last) },
      };
    };

    function draw(s: State) {
      const moved = s.bars !== seenBars || s.halts !== seenHalts;
      const rebuild = version !== s.barsVersion || (moved && drawn.length === 0);
      if (rebuild) {
        frame = { seconds, scale: 10 ** (s.hello?.price_scale ?? 4), offset: nyOffset };
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
      const bid = s.snap?.bids[0]?.[0];
      const price = drawn.findLast((p) => p.bar)?.bar?.close ?? (bid === undefined ? undefined : bid / frame.scale);
      if (price !== undefined && chartDigits(price) !== digits) {
        digits = chartDigits(price);
        const priceFormat = { type: 'price', precision: digits, minMove: 10 ** -digits } as const;
        for (const series of [candles, vwap, ema]) series.applyOptions({ priceFormat });
      }
      seenBars = s.bars;
      seenHalts = s.halts;
      if (rebuild || s.fills !== fills) {
        fills = s.fills;
        const marks = fillMarks(fills, frame);
        markers.setMarkers(
          marks.map((m) => {
            const buy = m.side === 'buy';
            return {
              time: m.time as Time,
              position: buy ? 'belowBar' : 'aboveBar',
              shape: buy ? 'arrowUp' : 'arrowDown',
              color: buy ? color.buy : color.sell,
              text: marks.length <= markTexts ? `${buy ? 'B' : 'S'} ${m.shares}` : '',
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
      const p = (price: number) => price.toFixed(digits);
      legend.current!.textContent = bar
        ? `O ${p(bar.open)} H ${p(bar.high)} L ${p(bar.low)} C ${p(bar.close)} V ${bar.volume}`
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
    <section className="panel chart" aria-label="Chart">
      <div className="title">
        <ChartSymbol />
        <span className="seg">
          {timeframes.map((tf) => (
            <button key={tf} className={tf === seconds ? 'on' : ''} onClick={() => setSeconds(tf)}>
              {label(tf)}
            </button>
          ))}
        </span>
        <span className="legend" ref={legend} />
        <span className="grow" />
        <span style={{ color: color.vwap }}>VWAP</span>
        <span style={{ color: color.ema }}>EMA 20</span>
      </div>
      <div className="plot" ref={box} />
      <FirstRun />
    </section>
  );
}
