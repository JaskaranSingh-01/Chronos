import { ColorType, createChart, LineSeries, LineStyle, TickMarkType, type Time } from 'lightweight-charts';
import { useEffect, useMemo, useRef, useState } from 'react';
import { px, ruleLabel, shares, signed, stamp, tone } from './format';
import {
  align,
  cards,
  orbCards,
  compareStats,
  drawdown,
  likeForLike,
  moneyDigits,
  specDiff,
  timeOfRun,
  useLab,
  type Results,
  type Run,
} from './lab';

const tabNames = ['Fills', 'Fill attribution', 'PnL by hour', 'Per-day PnL'] as const;
const phaseNames: Record<string, string> = {
  pre_open: 'pre-open',
  opening_cross: 'open cross',
  regular: 'regular',
  closing_cross: 'close cross',
  post_close: 'post-close',
};
const fillRows = 500; // ponytail: the first 500 fills of up to 50,000; a longer list needs windowing

// The equity curve, PnL at each event-second sample, over its drawdown in a pane below; a second run is dashed.
// ponytail: every sample is drawn, up to 57,600 a full day; thin them if a long run draws slowly.
export function Equity({ runs }: { runs: Run[] }) {
  const box = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const days = Math.max(...runs.map((r) => r.spec.days.length));
    const label = (t: Time) => timeOfRun(t as number, days);
    // A tick where the axis turns a day names the day; the others, the time.
    const tick = (t: Time, type: TickMarkType) =>
      type < TickMarkType.Time ? `day ${Math.floor((t as number) / 86_400) + 1}` : timeOfRun((t as number) % 86_400, 1);
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
      localization: { timeFormatter: label },
      timeScale: {
        timeVisible: true,
        secondsVisible: true,
        fixLeftEdge: true,
        fixRightEdge: true,
        tickMarkFormatter: tick,
      },
    });
    runs.forEach((run, i) => {
      const r = run.results;
      if (!r) return;
      const dollars = 10 ** moneyDigits(run);
      const t = align(r.equity.t, run.spec.days);
      const line = {
        lineWidth: 1,
        lineStyle: i === 0 ? LineStyle.Solid : LineStyle.Dashed,
        priceLineVisible: false,
        lastValueVisible: false,
        priceFormat: { type: 'price', precision: 2, minMove: 0.01 },
      } as const;
      const pnl = chart.addSeries(LineSeries, { ...line, color: '#5aa9ff' });
      pnl.setData(t.map((time, k) => ({ time: time as Time, value: r.equity.pnl[k] / dollars })));
      const below = chart.addSeries(LineSeries, { ...line, color: '#f0484f' }, 1);
      below.setData(drawdown(r.equity.pnl).map((d, k) => ({ time: t[k] as Time, value: d / dollars })));
    });
    chart.panes()[1]?.setHeight(90);
    chart.timeScale().fitContent();
    return () => chart.remove();
  }, [runs]);
  return (
    <div className="panel equity">
      <div className="title">
        Equity<span className="muted">drawdown below · event-second samples</span>
      </div>
      <div className="plot" ref={box} />
    </div>
  );
}

// A run's fills, its fill attribution as the headless table lays it out, and its PnL by hour and by day.
function ResultTabs({ run, r }: { run: Run; r: Results }) {
  const [tab, setTab] = useState<(typeof tabNames)[number]>('Fills');
  const days = run.spec.days.length;
  const digits = run.provenance.engine.price_scale;
  const cash = moneyDigits(run);
  return (
    <div className="panel lab-tabs">
      <div className="tabs-head">
        {tabNames
          .filter((name) => days > 1 || name !== 'Per-day PnL')
          .map((name) => (
            <button key={name} className={tab === name ? 'on' : ''} onClick={() => setTab(name)}>
              {name}
            </button>
          ))}
      </div>
      <table>
        {tab === 'Fills' && (
          <tbody>
            <tr className="muted">
              <td>Time</td>
              <td>Side</td>
              <td>Price</td>
              <td>Qty</td>
              <td>Liquidity</td>
              <td>Rule</td>
              <td>Queue ahead at entry</td>
              <td>Position</td>
            </tr>
            {r.fill_list.rows
              .slice(0, fillRows)
              .map(([ts, , side, price, qty, liquidity, rule, ahead, position], i) => (
                <tr key={i}>
                  <td>{stamp(ts, days > 1)}</td>
                  <td className={side}>{side}</td>
                  <td>{px(price, digits)}</td>
                  <td>{qty}</td>
                  <td>{liquidity}</td>
                  <td>{ruleLabel(rule === 'none' ? undefined : rule)}</td>
                  <td>{liquidity === 'maker' ? ahead : '—'}</td>
                  <td className={tone(position)}>{shares(position)}</td>
                </tr>
              ))}
            {r.fills > fillRows && (
              <tr className="muted">
                <td colSpan={8}>
                  The first {fillRows} of {r.fills.toLocaleString('en-US')} fills
                  {r.fill_list.truncated ? '; the run kept 50,000' : ''}
                </td>
              </tr>
            )}
          </tbody>
        )}
        {tab === 'Fill attribution' && (
          <tbody>
            <tr className="muted">
              <td>Maker fills / FIFO anomalies</td>
              {r.attribution.phases.map((phase) => (
                <td key={phase}>{phaseNames[phase] ?? phase}</td>
              ))}
            </tr>
            {r.attribution.rules.map((rule, i) => (
              <tr key={rule}>
                <td>{ruleLabel(rule)}</td>
                {r.attribution.fills[i].map((fills, j) => (
                  <td key={j}>
                    {fills}/{r.attribution.anomalies[i][j]}
                  </td>
                ))}
              </tr>
            ))}
          </tbody>
        )}
        {tab === 'PnL by hour' && (
          <tbody>
            <tr className="muted">
              <td>New York hour</td>
              <td>PnL</td>
            </tr>
            {r.by_hour.hour.map((hour, i) => (
              <tr key={hour}>
                <td>{String(hour).padStart(2, '0')}:00</td>
                <td className={tone(r.by_hour.pnl[i])}>{signed(r.by_hour.pnl[i], cash)}</td>
              </tr>
            ))}
          </tbody>
        )}
        {tab === 'Per-day PnL' && (
          <tbody>
            <tr className="muted">
              <td>Day</td>
              <td>PnL</td>
              <td>Fills</td>
            </tr>
            {r.by_day.map((day) => (
              <tr key={day.date}>
                <td>{day.date}</td>
                <td className={tone(day.pnl)}>{signed(day.pnl, cash)}</td>
                <td>{day.fills}</td>
              </tr>
            ))}
          </tbody>
        )}
      </table>
    </div>
  );
}

// The run shown's stats, as metric rows; a partial run says so.
export function Metrics() {
  const run = useLab((l) => l.details[l.selected]);
  const r = run?.results;
  return (
    <section className="panel metrics" aria-label="Metrics">
      <div className="title">
        Metrics
        <span className="grow" />
        {r && (
          <span className="muted">
            {run.spec.symbol} · {run.spec.days.length === 1 ? run.spec.days[0] : `${run.spec.days.length} days`}
          </span>
        )}
      </div>
      <div className="body">
        {r?.partial && (
          <div className="sell">
            Partial: {run.state}
            {run.reason && `; ${run.reason}`}. These are the aggregates so far.
          </div>
        )}
        {r && (
          <div className="cards">
            {cards(r, moneyDigits(run)).map(([name, value, color]) => (
              <div key={name} className="card">
                <span className="muted">{name}</span>
                <b className={color}>{value}</b>
              </div>
            ))}
            {r.orb &&
              orbCards(r.orb, run.provenance.engine.price_scale).map(([name, value]) => (
                <div key={name} className="card wide">
                  <span className="muted">ORB · {name}</span>
                  <b>{value}</b>
                </div>
              ))}
          </div>
        )}
      </div>
    </section>
  );
}

// The run shown: its equity and drawdown, and tabs; one without results says why.
export function RunResults() {
  const run = useLab((l) => l.details[l.selected]);
  const listed = useLab((l) => l.runs.find((r) => r.id === l.selected));
  const shown = useMemo(() => (run ? [run] : []), [run]);
  const r = run?.results;
  if (!listed || !run || !r) {
    const why = !listed
      ? 'Run a backtest, or pick one from the runs.'
      : listed.state === 'running'
        ? `Running: ${Math.round(listed.progress * 100)}% read`
        : listed.state === 'queued' || listed.state === 'completed'
          ? `${listed.state}…`
          : `${listed.state}: no results${listed.reason ? `; ${listed.reason}` : ''}`;
    return <div className="body muted">{why}</div>;
  }
  return (
    <>
      <Equity runs={shown} />
      <ResultTabs key={run.id} run={run} r={r} />
    </>
  );
}

// Two runs ticked, in list order: equity overlaid with the second dashed, the stats side by side with the second's
// difference, every setting that differs marked, and a flag when the symbol or the days differ.
export function Compare() {
  const runs = useLab((l) => l.runs);
  const compare = useLab((l) => l.compare);
  const details = useLab((l) => l.details);
  const ids = runs.filter((r) => compare.includes(r.id)).map((r) => r.id);
  const key = ids.join();
  const shown = useMemo(() => key.split(',').flatMap((id) => details[id] ?? []), [key, details]);
  const [a, b] = shown;
  if (!a?.results || !b?.results) return <div className="body muted">Fetching the runs to compare…</div>;
  const n = (run: Run) => runs.findIndex((r) => r.id === run.id) + 1;
  return (
    <>
      <div className="title">
        Compare #{n(a)} and #{n(b)}, #{n(b)} dashed
        {!likeForLike(a.spec, b.spec) && (
          <span className="sell"> · not like-for-like: another symbol or other days</span>
        )}
      </div>
      <Equity runs={shown} />
      <div className="panel lab-tabs">
        <table>
          <tbody>
            <tr className="muted">
              <td>Stat</td>
              <td>#{n(a)}</td>
              <td>#{n(b)}</td>
              <td>
                #{n(b)} − #{n(a)}
              </td>
            </tr>
            {compareStats(a.results, b.results, moneyDigits(a)).map(([name, x, y, delta]) => (
              <tr key={name}>
                <td>{name}</td>
                <td>{x}</td>
                <td>{y}</td>
                <td>{delta}</td>
              </tr>
            ))}
            <tr className="muted">
              <td>Setting</td>
              <td />
              <td />
              <td />
            </tr>
            {specDiff(a.spec, b.spec, a.provenance.engine.price_scale).map((row) => (
              <tr key={row.field} className={row.differs ? 'diff' : ''}>
                <td>{row.field}</td>
                <td>{row.a}</td>
                <td>{row.b}</td>
                <td>{row.differs ? 'differs' : ''}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </>
  );
}
