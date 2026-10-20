import { useEffect, useState } from 'react';
import { signed } from './format';
import {
  actions,
  defaults,
  fromSpec,
  lab,
  moneyDigits,
  refusalText,
  strategyNames,
  toBody,
  useLab,
  windowLabels,
  type Form,
  type RunListing,
  type StrategyKind,
  type Window,
} from './lab';
import { Compare, RunResults } from './LabResults';
import { useStore } from './store';
import { DayPicker, Switch, Symbols } from './TopBar';

const none = { field: '', text: '' };

// The run form: defaults and caps from /api/strategies, fees in dollars; a field past its cap, or one chronos-serve
// refused, is marked.
function RunForm({ form, setForm, digits }: { form: Form; setForm(f: Form): void; digits: number }) {
  const strategies = useLab((l) => l.strategies)!;
  const days = useLab((l) => l.days);
  const symbols = useLab((l) => l.symbols);
  const [refusal, setRefusal] = useState(none); // why the last Run was refused, until an edit
  const [busy, setBusy] = useState(false); // a Run is being sent; a disabled button also stops Enter
  const picked = form.days.join(',');
  useEffect(() => {
    if (picked) void lab.loadSymbols(picked.split(','));
  }, [picked]);
  const kind = strategies.strategies.find((k) => k.kind === form.strategy)!;
  const edit = (f: Form) => {
    setRefusal(none);
    setForm(f);
  };
  const field = (name: string, label: string, value: string, change: (value: string) => void, placeholder = '') => (
    <label className="kv" key={name}>
      {label}
      <input
        aria-label={name}
        aria-invalid={refusal.field === name}
        value={value}
        placeholder={placeholder}
        onChange={(e) => change(e.target.value)}
      />
    </label>
  );
  return (
    <form
      className="panel form"
      onSubmit={async (e) => {
        e.preventDefault();
        const body = toBody(form, strategies, digits);
        if (typeof body === 'string') return setRefusal({ field: body, text: `Check ${body}` });
        setBusy(true);
        const r = await lab.submit(body);
        setBusy(false);
        const named = r?.field ?? (r?.error === 'symbol_missing' ? 'symbol' : '');
        setRefusal(r ? { field: named, text: refusalText(r, body.symbol) } : none);
      }}
    >
      <div className="title">New run</div>
      <label className="kv">
        Strategy
        <select
          aria-label="strategy"
          value={form.strategy}
          onChange={(e) =>
            edit({ ...defaults(strategies, e.target.value as StrategyKind, digits, form.days), symbol: form.symbol })
          }
        >
          {strategies.strategies.map((k) => (
            <option key={k.kind} value={k.kind}>
              {strategyNames[k.kind]}
            </option>
          ))}
        </select>
      </label>
      <label className="kv">
        Symbol
        <input
          aria-label="symbol"
          aria-invalid={refusal.field === 'symbol'}
          list="lab-symbols"
          value={form.symbol}
          onChange={(e) => edit({ ...form, symbol: e.target.value })}
        />
      </label>
      <Symbols id="lab-symbols" symbols={symbols} />
      <div className={`kv${refusal.field.startsWith('days') ? ' bad' : ''}`}>
        Days
        <DayPicker
          days={days}
          load={lab.loadDays}
          picked={form.days}
          pick={(d) => edit({ ...form, days: d })}
          disabled={false}
        />
      </div>
      <label className="kv">
        Window
        <select
          aria-label="window"
          value={form.window}
          onChange={(e) => edit({ ...form, window: e.target.value as Window })}
        >
          {Object.entries(windowLabels).map(([id, label]) => (
            <option key={id} value={id}>
              {label}
            </option>
          ))}
        </select>
      </label>
      <div className="title">Parameters</div>
      {kind.params.map((p) =>
        field(`params.${p.name}`, p.name.replaceAll('_', ' '), form.params[p.name] ?? '', (v) =>
          edit({ ...form, params: { ...form.params, [p.name]: v } }),
        ),
      )}
      <div className="title">Execution</div>
      {field('latency.order_ns', 'order latency ns', form.latency.order_ns, (order_ns) =>
        edit({ ...form, latency: { ...form.latency, order_ns } }),
      )}
      {field('latency.report_ns', 'report latency ns', form.latency.report_ns, (report_ns) =>
        edit({ ...form, latency: { ...form.latency, report_ns } }),
      )}
      {field('fees.maker', 'maker fee $ a share', form.fees.maker, (maker) =>
        edit({ ...form, fees: { ...form.fees, maker } }),
      )}
      {field('fees.taker', 'taker fee $ a share', form.fees.taker, (taker) =>
        edit({ ...form, fees: { ...form.fees, taker } }),
      )}
      <div className="title">Limits · empty follows the params</div>
      {(['max_position', 'max_order_quantity', 'max_open_orders'] as const).map((name) =>
        field(
          `limits.${name}`,
          name.replaceAll('_', ' '),
          form.limits[name],
          (v) => edit({ ...form, limits: { ...form.limits, [name]: v } }),
          String(kind.limits[name]),
        ),
      )}
      <button className="place run" disabled={busy}>
        ▶ Run backtest
      </button>
      <div className="sell">{refusal.text}</div>
    </form>
  );
}

// What the run shown allows: its settings as a new run, Cancel while it is queued or running, and Delete, confirmed
// here rather than by a dialog.
function RunActions({ run, n, edit }: { run: RunListing; n: number; edit(): void }) {
  const [confirming, setConfirming] = useState(false);
  // The second click of a double click, which lands on Confirm delete once Delete arms it, is not a click.
  const once = (f: () => void) => (e: { detail: number }) => e.detail < 2 && f();
  return (
    <div className="actions">
      <span className="muted">#{n}</span>
      <button onClick={edit}>Edit as new</button>
      {actions(run).cancel && <button onClick={once(() => void lab.cancel(run.id))}>Cancel run</button>}
      {confirming ? (
        <>
          <button className="sell" onClick={once(() => void lab.remove(run.id))}>
            Confirm delete
          </button>
          <button onClick={() => setConfirming(false)}>Keep</button>
        </>
      ) : (
        <button onClick={() => setConfirming(true)}>Delete</button>
      )}
    </div>
  );
}

// A listed run's state while it is queued or running or ended without results, else its PnL.
function result(r: RunListing): string {
  if (r.state === 'running') return `running ${Math.round(r.progress * 100)}%`;
  if (r.pnl === null) return r.state;
  return `${signed(r.pnl, moneyDigits(r))}${r.partial ? ` · ${r.state}` : ''}`;
}

// Every run, newest first, numbered in the order they were listed; a click shows one, and two ticked compare.
function Runs({ edit }: { edit(r: RunListing): void }) {
  const runs = useLab((l) => l.runs);
  const selected = useLab((l) => l.selected);
  const compare = useLab((l) => l.compare);
  const shown = runs.findIndex((r) => r.id === selected);
  const ticked = runs.filter((r) => compare.includes(r.id));
  const n = (r: RunListing) => runs.indexOf(r) + 1;
  return (
    <div className="panel runs">
      <div className="title">Runs · tick two to compare</div>
      <table>
        <tbody>
          <tr className="muted">
            <td />
            <td>#</td>
            <td>Strategy</td>
            <td>Symbol</td>
            <td>Days</td>
            <td>Result</td>
          </tr>
          {runs
            .map((r, i) => (
              <tr key={r.id} className={r.id === selected ? 'on' : ''} onClick={() => void lab.select(r.id)}>
                <td onClick={(e) => e.stopPropagation()}>
                  <input
                    type="checkbox"
                    aria-label={`compare #${i + 1}`}
                    checked={compare.includes(r.id)}
                    onChange={(e) => void lab.tick(r.id, e.target.checked)}
                  />
                </td>
                <td>{i + 1}</td>
                <td>{strategyNames[r.spec.strategy] ?? r.spec.strategy}</td>
                <td>{r.spec.symbol}</td>
                <td>{r.spec.days.length}</td>
                <td>{result(r)}</td>
              </tr>
            ))
            .reverse()}
        </tbody>
      </table>
      <div className="actions">
        <button
          disabled={ticked.length !== 2 || ticked.some((r) => r.pnl === null)}
          onClick={() => lab.showCompare(true)}
        >
          {ticked.length === 2 ? `Compare #${n(ticked[0])} vs #${n(ticked[1])}` : 'Compare'}
        </button>
      </div>
      {shown >= 0 && <RunActions key={selected} run={runs[shown]} n={shown + 1} edit={() => edit(runs[shown])} />}
    </div>
  );
}

// The Strategy Lab: a run's settings on the left, its results in the middle, the runs on the right.
export function StrategyLab() {
  const offline = useLab((l) => l.offline);
  const notice = useLab((l) => l.notice);
  const comparing = useLab((l) => l.comparing && l.compare.length === 2);
  const strategies = useLab((l) => l.strategies);
  const start = useLab((l) => l.start);
  const digits = useStore((s) => s.hello?.price_scale ?? 4); // ITCH 5.0's, before hello
  const [typed, setForm] = useState<Form | null>(null);
  const [copies, setCopies] = useState(0); // each Edit as new starts the form afresh, without the last refusal
  // The defaults over the starting day until the form is edited.
  const form = typed ?? (strategies && start ? defaults(strategies, 'market_maker', digits, [start]) : null);
  return (
    <>
      <header className="bar">
        <Switch lab />
        <span className="notice">{offline ? 'chronos-serve does not answer: retrying' : notice}</span>
      </header>
      <main className="lab">
        <section className="col">
          {form && <RunForm key={copies} form={form} setForm={setForm} digits={digits} />}
        </section>
        <section className="col">{comparing ? <Compare /> : <RunResults />}</section>
        <section className="col">
          <Runs
            edit={(r) => {
              setForm(fromSpec(r.spec, digits));
              setCopies(copies + 1);
            }}
          />
        </section>
      </main>
    </>
  );
}
