import { useEffect, useState, useSyncExternalStore } from 'react';
import { DepthLadder, Tape } from './Book';
import { Chart } from './Chart';
import { outcomes, page, runLabel, useLab } from './lab';
import { store, useStore } from './store';
import { Strategies } from './Strategies';
import { StrategyLab } from './StrategyLab';
import { PnlPanel, Tabs } from './Tabs';
import { Account, Ticket } from './Ticket';
import { TopBar } from './TopBar';

// Asks for the token a non-loopback server wants, or says why the terminal cannot connect.
function Gate() {
  const status = useStore((s) => s.status);
  const notice = useStore((s) => s.notice); // why the last token was refused
  const [token, setToken] = useState('');
  if (status === 'host')
    return <p className="gate">Open the terminal at 127.0.0.1 or localhost, as chronos-serve printed it.</p>;
  if (status !== 'token') return null;
  return (
    <form
      className="gate"
      onSubmit={(e) => {
        e.preventDefault();
        store.setToken(token);
      }}
    >
      <label>
        Token <input type="password" value={token} onChange={(e) => setToken(e.target.value)} autoFocus />
      </label>
      <button>Connect</button>
      {notice && <span className="sell">{notice}</span>}
    </form>
  );
}

// Says why every control is disabled: another window holds the controller seat.
function ViewerBanner() {
  const viewer = useStore((s) => s.hello?.role === 'viewer');
  return viewer ? (
    <p className="banner">
      Read-only: another window or tab controls this session. Close it and reload this page to take control.
    </p>
  ) : null;
}

// Says which Lab run this generation replays, and, once it has ended, the verdict. Stop watching loads the plain
// replay of the generation's symbol and days, stamped with this generation, so a click after it has moved on is dropped.
function WatchBanner() {
  const runId = useStore((s) => s.runId);
  const verdict = useStore((s) => s.verdict);
  const state = useStore((s) => s.state);
  const disabled = useStore((s) => s.hello?.role !== 'controller' || s.status !== 'open');
  const label = useLab((l) => runLabel(l.runs, runId));
  if (!runId) return null;
  const until =
    state === 'ended'
      ? 'its verdict was sent before this tab connected'
      : `${state === 'paused' ? 'press ▶ Play; ' : ''}the verdict comes when its replay ends`;
  return (
    <p className="banner">
      <span data-testid="watch">
        Watching run {label}
        {verdict ? <b className={outcomes[verdict.outcome][1]}> · {outcomes[verdict.outcome][0]}</b> : `: ${until}.`}
      </span>
      <span className="grow" />
      <button
        disabled={disabled}
        onClick={() => {
          const { symbol, genDays } = store.get();
          store.control({ action: 'symbol', symbol, days: genDays });
        }}
      >
        Stop watching
      </button>
    </p>
  );
}

const onHash = (f: () => void) => {
  addEventListener('hashchange', f);
  return () => removeEventListener('hashchange', f);
};

// The replay's own figures, on both pages: its rate, the snapshots this page skipped, the commands it drained, the
// simulated latencies, and the session's symbol, generation and state.
function EngineStrip() {
  const snap = useStore((s) => s.snap);
  const spec = useStore((s) => s.spec);
  const symbol = useStore((s) => s.symbol);
  const generation = useStore((s) => s.generation);
  const state = useStore((s) => s.state);
  const status = useStore((s) => s.status);
  const viewer = useStore((s) => s.hello !== null && s.hello.role !== 'controller');
  const us = (ns: number) => `${(ns / 1_000).toLocaleString('en-US')} µs`;
  return (
    <footer className="engine">
      <span className="label">ENGINE</span>
      {snap && (
        <>
          <span>
            replay <b>{snap.metrics.events_per_second.toLocaleString('en-US')}</b> ev/s
          </span>
          <span>
            dropped <b>{snap.metrics.dropped_snapshots}</b>
          </span>
          <span>
            queue <b>{snap.metrics.command_queue_depth}</b>
          </span>
        </>
      )}
      {spec && (
        <span>
          latency order <b>{us(spec.latency.order_ns)}</b> · report <b>{us(spec.latency.report_ns)}</b>
        </span>
      )}
      <span className="grow" />
      <span className="session" data-testid="session">
        {symbol || (generation > 1 ? '…' : 'no symbol')} · gen {generation} · {state}
        {status !== 'open' && ` · ${status}`}
        {viewer && ' · viewer'}
        {snap && snap.trading_state !== 'trading' && <b className="sell"> · {snap.trading_state}</b>}
        {snap && ` · ${snap.metrics.events_per_second} ev/s · ${snap.metrics.dropped_snapshots} dropped`}
      </span>
    </footer>
  );
}

// The Trading Terminal, the Strategy Lab at #lab, or the strategy editor at #strategies: the panels on a 1 px grid. Each panel subscribes to its own slice,
// so a snapshot never re-renders this tree.
export function App() {
  const [price, setPrice] = useState('');
  const symbol = useStore((s) => s.symbol);
  useEffect(() => setPrice(''), [symbol]); // a new symbol's ticket starts unpriced
  const shown = useSyncExternalStore(onHash, () => page(location.hash));
  return (
    <div className="terminal">
      {shown === 'lab' ? (
        <StrategyLab />
      ) : shown === 'strategies' ? (
        <Strategies />
      ) : (
        <>
          <TopBar />
          <ViewerBanner />
          <WatchBanner />
          <main className="term">
            <DepthLadder onPick={setPrice} />
            <Chart />
            <PnlPanel />
            <Tape />
            <div className="side">
              <Ticket price={price} setPrice={setPrice} />
              <Account />
            </div>
            <Tabs />
          </main>
        </>
      )}
      <EngineStrip />
      <Gate />
    </div>
  );
}
