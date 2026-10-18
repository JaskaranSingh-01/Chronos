import { memo, useEffect, useRef, useState } from 'react';
import { clock, seekTs, signed } from './format';
import { selectFirstRun, store, useStore } from './store';

const speeds: [number, string][] = [
  [1, '1×'],
  [10, '10×'],
  [100, '100×'],
  [0, 'Max'],
];

// The day's symbols, apart from TopBar: a snapshot re-renders the bar, and these are thousands of options.
const Symbols = memo(function Symbols() {
  const symbols = useStore((s) => s.symbols);
  return (
    <datalist id="symbols">
      {symbols.map((s) => (
        <option key={s} value={s} />
      ))}
    </datalist>
  );
});

// Symbol, replay controls, the exchange clock, progress, PnL and the session's status.
export function TopBar() {
  const snap = useStore((s) => s.snap);
  const symbol = useStore((s) => s.symbol);
  const status = useStore((s) => s.status);
  const hello = useStore((s) => s.hello);
  const generation = useStore((s) => s.generation);
  const state = useStore((s) => s.state);
  const seekAt = useStore((s) => s.seekAt);
  const notice = useStore((s) => s.notice);
  const fresh = useStore(selectFirstRun);
  const [typed, setTyped] = useState('');
  const [time, setTime] = useState('');
  const symbolBox = useRef<HTMLInputElement>(null);
  useEffect(() => {
    if (fresh) symbolBox.current?.focus();
  }, [fresh]);
  const viewer = hello?.role !== 'controller';
  // Generation 1 is the empty ticker: until a symbol loads, only the switcher and the speed work.
  const idle = viewer || generation <= 1 || status !== 'open';
  const seeking = state === 'seeking';
  const cannotSeek = idle || seeking || !snap;
  const seekTo = (clockTime: string) => {
    if (snap) store.control({ action: 'seek', ts: seekTs(snap.exchange_ts, clockTime) });
  };
  // The exchange time shown: a running seek's position, else the book's; 0 (shown as a dash) before a symbol loads.
  const at = seeking && seekAt > 0 ? seekAt : (snap?.exchange_ts ?? 0);
  return (
    <header className="bar">
      <form
        onSubmit={(e) => {
          e.preventDefault();
          // The server takes a directory entry exactly: send the entry itself, found whatever the case typed.
          const name = typed.trim();
          if (!name) return;
          const { symbols } = store.get();
          const entry = symbols.find((s) => s === name) ?? symbols.find((s) => s.toUpperCase() === name.toUpperCase());
          if (entry === undefined) store.notify(`unknown symbol ${name}`);
          else if (store.control({ action: 'symbol', symbol: entry })) setTyped('');
        }}
      >
        <input
          ref={symbolBox}
          aria-label="Symbol"
          list="symbols"
          size={10}
          placeholder={symbol || 'Symbol'}
          value={typed}
          disabled={viewer}
          onChange={(e) => setTyped(e.target.value)}
        />
        <Symbols />
        <button disabled={viewer || !typed.trim()}>Load</button>
      </form>
      <button disabled={idle} onClick={() => store.control({ action: 'restart' })}>
        ⟲ Restart
      </button>
      {state === 'playing' ? (
        <button disabled={idle} onClick={() => store.control({ action: 'pause' })}>
          ⏸ Pause
        </button>
      ) : (
        <button disabled={idle || state === 'ended' || seeking} onClick={() => store.control({ action: 'play' })}>
          ▶ Play
        </button>
      )}
      <button disabled={idle || state === 'ended' || seeking} onClick={() => store.control({ action: 'step', n: 1 })}>
        Step ⏭
      </button>
      <span className="seg">
        {speeds.map(([speed, name]) => (
          <button
            key={speed}
            className={snap?.speed === speed ? 'on' : ''}
            disabled={viewer}
            onClick={() => store.control({ action: 'speed', speed })}
          >
            {name}
          </button>
        ))}
      </span>
      <form
        onSubmit={(e) => {
          e.preventDefault();
          if (time) seekTo(time);
        }}
      >
        <input
          aria-label="Seek to"
          type="time"
          step="1"
          value={time}
          disabled={cannotSeek}
          onChange={(e) => setTime(e.target.value)}
        />
        <button disabled={cannotSeek || !time}>Seek</button>
        <button type="button" disabled={cannotSeek} onClick={() => seekTo('09:30')}>
          Open 09:30
        </button>
      </form>
      {seeking && (
        <button disabled={viewer} onClick={() => store.control({ action: 'cancel_seek' })}>
          Cancel seek
        </button>
      )}
      <span data-testid="clock">{at > 0 ? clock(at) : '—'} ET</span>
      <progress value={snap?.progress ?? 0} />
      <span>
        PnL <b>{snap && hello ? signed(snap.account.pnl, hello.price_scale + hello.quantity_scale) : '—'}</b>
      </span>
      <span className="muted" data-testid="session">
        {symbol || (generation > 1 ? '…' : 'no symbol')} · gen {generation} · {state}
        {status !== 'open' && ` · ${status}`}
        {viewer && hello && ' · viewer'}
        {snap && snap.trading_state !== 'trading' && <b className="sell"> · {snap.trading_state}</b>}
        {snap && ` · ${snap.metrics.events_per_second} ev/s · ${snap.metrics.dropped_snapshots} dropped`}
      </span>
      <span className="notice" title={notice}>
        {notice}
      </span>
    </header>
  );
}
