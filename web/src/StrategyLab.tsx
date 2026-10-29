import { useEffect, useState } from 'react';
import { signed } from './format';
import {
  actions,
  defaults,
  fromSpec,
  lab,
  moneyDigits,
  outcomes,
  refusalText,
  runLabel,
  specRows,
  strategyLabel,
  toBody,
  useLab,
  windowLabels,
  type Form,
  type Run,
  type RunListing,
  type StrategyKind,
  type Window,
} from './lab';
import { Compare, Metrics, RunResults } from './LabResults';
import { store, useStore } from './store';
import { DayPicker, Switch, SymbolPicker } from './TopBar';

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
      <div className="title">New backtest</div>
      <div className="body">
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
                {k.label}
              </option>
            ))}
          </select>
        </label>
        {/* Not a label: a click on a match would reach the box through it and open the list again. */}
        <div className="kv">
          Symbol
          <SymbolPicker
            label="symbol"
            symbols={symbols}
            value={form.symbol}
            setValue={(symbol) => edit({ ...form, symbol })}
            pick={(symbol) => edit({ ...form, symbol })}
            invalid={refusal.field === 'symbol'}
          />
        </div>
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
        <h3>Parameters · {strategyLabel(strategies, form.strategy)}</h3>
        {kind.summary && <p className="muted">{kind.summary}</p>}
        {kind.params.map((p) =>
          field(`params.${p.name}`, p.name.replaceAll('_', ' '), form.params[p.name] ?? '', (v) =>
            edit({ ...form, params: { ...form.params, [p.name]: v } }),
          ),
        )}
        <h3>Execution</h3>
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
        <h3>Limits · empty follows the params</h3>
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
      </div>
    </form>
  );
}

// Saves a run's JSON, fetched with the token, from a Blob.
async function save(id: string) {
  const blob = await lab.download(id);
  if (!blob) return;
  const link = document.createElement('a');
  link.href = URL.createObjectURL(blob);
  link.download = `${id}.json`;
  link.click();
  setTimeout(() => URL.revokeObjectURL(link.href), 60_000); // a browser may still be reading it
}

// What the run shown allows: Watch in the terminal, which only the controlling tab may; Verify; Export; its settings
// as a new run; Cancel while it is queued or running; and Delete, confirmed here rather than by a dialog.
function RunActions({ run, edit }: { run: RunListing; edit(): void }) {
  const [confirming, setConfirming] = useState(false);
  const seat = useStore((s) =>
    s.status !== 'open' || !s.hello
      ? 'Not connected'
      : s.hello.role === 'controller'
        ? ''
        : 'Another tab holds control',
  );
  const can = actions(run);
  // A double click's second click is dropped: it would confirm a Delete just armed, or send a request twice.
  const once = (f: () => void) => (e: { detail: number }) => e.detail < 2 && f();
  return (
    <div className="actions">
      <button
        className="primary"
        disabled={!can.watch || seat !== ''}
        onClick={() => {
          if (store.control({ action: 'watch', run_id: run.id })) location.hash = '';
        }}
      >
        Watch in Terminal
      </button>
      <button disabled={!can.verify} onClick={once(() => void lab.verify(run.id))}>
        {run.verifying ? `Verifying: ${run.verifying}` : 'Verify'}
      </button>
      <button onClick={() => void save(run.id)}>Export JSON</button>
      <button onClick={edit}>Edit as new</button>
      {can.cancel && <button onClick={once(() => void lab.cancel(run.id))}>Cancel run</button>}
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
      {can.watch && seat && <span className="muted">{seat}</span>}
    </div>
  );
}

// A run's verifications, each with its verdict, and its identity and record: the build, each day with its hash and
// session, the engine, the timings, the digests and the settings.
function Provenance({ run, label }: { run: Run; label: string }) {
  const p = run.provenance;
  const b = p.build;
  const e = p.engine;
  const digests = run.results?.digests;
  const rows: [string, string][] = [
    ['build', `${b.commit}${b.dirty ? ' dirty' : ''} · ${b.compiler} · ${b.build_type} · sanitize ${b.sanitize}`],
    ['platform', b.platform],
    ['source digest', b.source_digest],
    ...p.days.map((d): [string, string] => [
      d.date,
      `${d.hash} · ${d.session.source === 'fallback' ? '09:30-16:00 fallback' : 'feed session'} · ${d.size} bytes · ${
        d.segment ? `segment ${d.segment.hash}, ${d.segment.size} bytes` : 'before the per-symbol store'
      }`,
    ]),
    [
      'engine',
      `book ${e.book_capacity} · scales ${e.price_scale}/${e.quantity_scale} · invariants ${e.check_invariants_every}`,
    ],
    ['truncated tail', p.allow_truncated_tail ? 'allowed' : 'refused'],
    ['ran', `${p.started || 'not yet'} to ${p.finished || '…'} on ${p.host}: ${p.wall_ms} ms wall, ${p.cpu_ms} ms CPU`],
    [
      'digests',
      digests ? `book ${digests.book} · execution ${digests.execution} · portfolio ${digests.portfolio}` : 'none',
    ],
    ...specRows(run.spec, e.price_scale),
  ];
  return (
    <div className="prov">
      <div className="title">Run {label} provenance</div>
      {run.reason && <div className="sell">{run.reason}</div>}
      {run.warning && <div className="sell">{run.warning}</div>}
      <div className="title">
        Verifications ·{' '}
        {run.reproduced === null
          ? 'none on this code and days yet'
          : run.reproduced
            ? '✓ reproduced'
            : '✗ not reproduced'}
      </div>
      {run.verifications.map((v, i) => (
        <div key={i}>
          <b className={outcomes[v.outcome][1]}>{outcomes[v.outcome][0]}</b> {v.finished} · code{' '}
          {v.source_digest.slice(0, 16)}
          {v.changed.length > 0 && ` · changed ${v.changed.join(', ')}`}
        </div>
      ))}
      <table>
        <tbody>
          {rows.map(([name, value]) => (
            <tr key={name}>
              <td className="muted">{name}</td>
              <td>{value}</td>
            </tr>
          ))}
        </tbody>
      </table>
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
function Runs() {
  const runs = useLab((l) => l.runs);
  const strategies = useLab((l) => l.strategies);
  const selected = useLab((l) => l.selected);
  const compare = useLab((l) => l.compare);
  const ticked = runs.filter((r) => compare.includes(r.id));
  const n = (r: RunListing) => runs.indexOf(r) + 1;
  const comparing = useLab((l) => l.comparing && l.compare.length === 2);
  return (
    <section className="panel runs" aria-label="Runs">
      <div className="title">
        Runs<span className="muted">tick two to compare</span>
        <span className="grow" />
        <button
          className={comparing ? 'on' : ''}
          disabled={!comparing && (ticked.length !== 2 || ticked.some((r) => r.pnl === null))}
          onClick={() => lab.showCompare(!comparing)}
        >
          {comparing
            ? 'Close compare'
            : ticked.length === 2
              ? `Compare #${n(ticked[0])} vs #${n(ticked[1])}`
              : 'Compare'}
        </button>
      </div>
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
                <td>
                  <button aria-label={`show #${i + 1}`}>{i + 1}</button>
                </td>
                <td>{strategyLabel(strategies, r.spec.strategy)}</td>
                <td>{r.spec.symbol}</td>
                <td>{r.spec.days.length}</td>
                <td>{result(r)}</td>
              </tr>
            ))
            .reverse()}
        </tbody>
      </table>
    </section>
  );
}

// The run picked: what it allows, its verifications and its provenance.
function RunPanel({ edit }: { edit(r: RunListing): void }) {
  const runs = useLab((l) => l.runs);
  const strategies = useLab((l) => l.strategies);
  const selected = useLab((l) => l.selected);
  const full = useLab((l) => l.details[l.selected]); // its JSON, for the provenance
  const shown = runs.findIndex((r) => r.id === selected);
  const run = runs[shown];
  return (
    <section className="panel run" aria-label="Selected run">
      <div className="title">
        {run ? `Run #${shown + 1} · ${strategyLabel(strategies, run.spec.strategy)} · ${run.spec.symbol}` : 'Run'}
        <span className="grow" />
        {run && <b className="num">{result(run)}</b>}
      </div>
      {run && <RunActions key={selected} run={run} edit={() => edit(run)} />}
      <div className="body">{run && full && <Provenance run={full} label={runLabel(runs, selected)} />}</div>
    </section>
  );
}

// The Strategy Lab: the new run's settings on the left; the runs over the results or a comparison in the middle; the
// run picked's metrics over its actions and provenance on the right.
export function StrategyLab() {
  useEffect(() => void lab.start(), []); // runs queued or deleted elsewhere show on entering
  const offline = useLab((l) => l.offline);
  const notice = useLab((l) => l.notice);
  const comparing = useLab((l) => l.comparing && l.compare.length === 2);
  const strategies = useLab((l) => l.strategies);
  const start = useLab((l) => l.start);
  const digits = useStore((s) => s.hello?.price_scale ?? 4); // ITCH 5.0's, before hello
  const [typed, setForm] = useState<Form | null>(null);
  const [copies, setCopies] = useState(0); // each Edit as new starts the form afresh, without the last refusal
  // The defaults over the starting day until the form is edited, or once its strategy is gone (a script deleted): the
  // market maker if it is registered, else the first strategy.
  const first = strategies?.strategies.find((k) => k.kind === 'market_maker') ?? strategies?.strategies[0];
  const offered = typed !== null && strategies?.strategies.some((k) => k.kind === typed.strategy);
  const form = offered
    ? typed
    : strategies && first && start
      ? defaults(strategies, first.kind, digits, [start])
      : null;
  return (
    <>
      <header className="bar">
        <Switch page="lab" />
        <span className="sep" />
        <span className="notice">{offline ? 'chronos-serve does not answer: retrying' : notice}</span>
      </header>
      <main className="lab">
        {form ? <RunForm key={copies} form={form} setForm={setForm} digits={digits} /> : <div className="panel form" />}
        <Runs />
        <section className="panel results" aria-label="Results">
          {comparing ? <Compare /> : <RunResults />}
        </section>
        <Metrics />
        <RunPanel
          edit={(r) => {
            setForm(fromSpec(r.spec, digits));
            setCopies(copies + 1);
          }}
        />
      </main>
    </>
  );
}
