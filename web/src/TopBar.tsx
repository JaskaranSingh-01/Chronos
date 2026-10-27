import { memo, useEffect, useRef, useState } from 'react';
import { dateOf, dayLabel, dayTs, stamp } from './format';
import type { Day } from './protocol';
import { selectFirstRun, store, useStore } from './store';

const speeds: [number, string][] = [
  [1, '1×'],
  [10, '10×'],
  [100, '100×'],
  [0, 'Max'],
];

// The day's symbols, memoised: a snapshot re-renders the bar, and these are thousands of options. Keyed by their
// count, so a list that arrives mounts whole: options added one by one to a mounted list cost React O(n²).
export const Symbols = memo(function Symbols({ id, symbols }: { id: string; symbols: string[] }) {
  return (
    <datalist id={id} key={symbols.length}>
      {symbols.map((s) => (
        <option key={s} value={s} />
      ))}
    </datalist>
  );
});

// The wordmark and the header switch between the Trading Terminal and the Strategy Lab, at #lab.
export function Switch({ lab }: { lab: boolean }) {
  return (
    <nav className="pages">
      <span className="brand">CHRONOS</span>
      <a href="#" className={lab ? '' : 'on'} aria-current={lab ? undefined : 'page'}>
        Terminal
      </a>
      <a href="#lab" className={lab ? 'on' : ''} aria-current={lab ? 'page' : undefined}>
        Strategy Lab
      </a>
    </nav>
  );
}

// The days a load or a run replays, found again each time the list opens; a day not ITCH 5.0 cannot be ticked.
export function DayPicker({
  days,
  load,
  picked,
  pick,
  disabled,
}: {
  days: Day[];
  load(): Promise<void>;
  picked: string[];
  pick(days: string[]): void;
  disabled: boolean;
}) {
  return (
    <details className="days" onToggle={(e) => e.currentTarget.open && void load()}>
      <summary>{picked.length > 1 ? `${picked.length} days` : (picked[0] ?? 'Days')}</summary>
      <div>
        {days.map((day) => (
          <label key={day.date}>
            <input
              type="checkbox"
              checked={picked.includes(day.date)}
              disabled={disabled || !day.itch50}
              onChange={(e) =>
                pick(e.target.checked ? [...picked, day.date].sort() : picked.filter((d) => d !== day.date))
              }
            />
            {dayLabel(day)}
          </label>
        ))}
      </div>
    </details>
  );
}

// Symbol and days, replay controls, the exchange clock and progress, seek, and the connection.
export function TopBar() {
  const snap = useStore((s) => s.snap);
  const symbol = useStore((s) => s.symbol);
  const status = useStore((s) => s.status);
  const hello = useStore((s) => s.hello);
  const generation = useStore((s) => s.generation);
  const state = useStore((s) => s.state);
  const seekAt = useStore((s) => s.seekAt);
  const notice = useStore((s) => s.notice);
  const genDays = useStore((s) => s.genDays);
  const symbols = useStore((s) => s.symbols);
  const days = useStore((s) => s.days);
  const fresh = useStore(selectFirstRun);
  const [typed, setTyped] = useState('');
  const [picked, setPicked] = useState<string[]>([]);
  const [time, setTime] = useState('');
  const [seekDay, setSeekDay] = useState('');
  useEffect(() => setPicked(genDays), [genDays]); // a new generation's days start the picker
  useEffect(() => {
    if (picked.length > 0) void store.loadSymbols(picked); // the symbols listed on every day picked
  }, [picked]);
  const symbolBox = useRef<HTMLInputElement>(null);
  useEffect(() => {
    if (fresh) symbolBox.current?.focus();
  }, [fresh]);
  const viewer = hello?.role !== 'controller';
  // Generation 1 is the empty ticker: until a symbol loads, only the switcher and the speed work.
  const idle = viewer || generation <= 1 || status !== 'open';
  const seeking = state === 'seeking';
  const cannotSeek = idle || seeking || !snap;
  const multiDay = genDays.length > 1; // the clock shows the date, and a seek picks the day
  const seekTo = (clockTime: string) => {
    const day = genDays.includes(seekDay) ? seekDay : snap && dateOf(snap.exchange_ts);
    if (day) store.control({ action: 'seek', ts: dayTs(day, clockTime) });
  };
  // The exchange time shown: a running seek's position, else the book's; 0 (shown as a dash) before a symbol loads.
  const at = seeking && seekAt > 0 ? seekAt : (snap?.exchange_ts ?? 0);
  return (
    <header className="bar">
      <Switch lab={false} />
      <span className="sep" />
      <form
        onSubmit={(e) => {
          e.preventDefault();
          // The server takes a directory entry exactly: send the entry itself, found whatever the case typed.
          const name = typed.trim();
          if (!name) return;
          const entry = symbols.find((s) => s === name) ?? symbols.find((s) => s.toUpperCase() === name.toUpperCase());
          if (entry === undefined)
            store.notify(picked.length > 1 ? `${name} is not listed on every day picked` : `unknown symbol ${name}`);
          // No days picked yet (no state so far): the server replays the last load's.
          else if (store.control({ action: 'symbol', symbol: entry, days: picked.length > 0 ? picked : undefined })) {
            setTyped('');
          }
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
        <Symbols id="symbols" symbols={symbols} />
        <DayPicker days={days} load={store.loadDays} picked={picked} pick={setPicked} disabled={viewer} />
        <button disabled={viewer || !typed.trim()}>Load</button>
      </form>
      <span className="sep" />
      <button disabled={idle} onClick={() => store.control({ action: 'restart' })}>
        ⟲ Restart
      </button>
      {state === 'playing' ? (
        <button className="primary" disabled={idle} onClick={() => store.control({ action: 'pause' })}>
          ⏸ Pause
        </button>
      ) : (
        <button
          className="primary"
          disabled={idle || state === 'ended' || seeking}
          onClick={() => store.control({ action: 'play' })}
        >
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
      <span className="num bright" data-testid="clock">
        {at > 0 ? stamp(at, multiDay) : '—'} ET
      </span>
      <progress value={snap?.progress ?? 0} aria-label="Day read" />
      <form
        onSubmit={(e) => {
          e.preventDefault();
          if (time) seekTo(time);
        }}
      >
        {multiDay && (
          <select
            aria-label="Seek day"
            value={seekDay}
            disabled={cannotSeek}
            onChange={(e) => setSeekDay(e.target.value)}
          >
            <option value="">this day</option>
            {genDays.map((day) => (
              <option key={day}>{day}</option>
            ))}
          </select>
        )}
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
      <span className="sep" />
      <span className="notice" title={notice}>
        {notice}
      </span>
      <span className={`status ${status}`}>
        {status === 'open' ? 'CONNECTED' : status.toUpperCase()}{' '}
        <span className="muted">{hello ? hello.role : ''}</span>
      </span>
    </header>
  );
}
