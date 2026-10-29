import { useEffect, useId, useMemo, useRef, useState, type Ref } from 'react';
import { dateOf, dayLabel, dayTs, stamp } from './format';
import type { Page } from './lab';
import type { Day } from './protocol';
import { selectFirstRun, store, useStore } from './store';
import { matches } from './symbols';

const speeds: [number, string][] = [
  [1, '1×'],
  [10, '10×'],
  [100, '100×'],
  [0, 'Max'],
];

// A symbol box over the days' thousands of symbols. A click or typing lists the matches, the symbol typed first, then
// those it starts, then those it is inside; the arrows move through them, Enter or a click picks one, and Escape closes
// the list. Enter with no match picked leaves the box to its form.
export function SymbolPicker({
  label,
  symbols,
  value,
  setValue,
  pick,
  placeholder = '',
  disabled = false,
  invalid = false,
  inputRef,
}: {
  label: string;
  symbols: string[];
  value: string;
  setValue(value: string): void;
  pick(symbol: string): void;
  placeholder?: string;
  disabled?: boolean;
  invalid?: boolean;
  inputRef?: Ref<HTMLInputElement>;
}) {
  const id = useId();
  const [open, setOpen] = useState(false);
  const [active, setActive] = useState(-1);
  const { shown, total } = useMemo(() => matches(symbols, value), [symbols, value]);
  const list = useRef<HTMLUListElement>(null);
  useEffect(() => {
    list.current?.children[active]?.scrollIntoView({ block: 'nearest' }); // it may return a promise: not a cleanup
  }, [active]);
  const close = () => {
    setOpen(false);
    setActive(-1);
  };
  const choose = (symbol: string) => {
    close();
    pick(symbol);
  };
  const q = value.trim().toUpperCase();
  const marked = (s: string) => {
    const at = q ? s.toUpperCase().indexOf(q) : -1;
    if (at < 0) return s;
    return (
      <>
        {s.slice(0, at)}
        <b>{s.slice(at, at + q.length)}</b>
        {s.slice(at + q.length)}
      </>
    );
  };
  return (
    <span className="picker">
      <input
        ref={inputRef}
        role="combobox"
        aria-label={label}
        aria-expanded={open}
        aria-controls={id}
        aria-autocomplete="list"
        aria-activedescendant={shown[active] ? `${id}-${active}` : undefined}
        aria-invalid={invalid}
        autoComplete="off"
        spellCheck={false}
        placeholder={placeholder}
        value={value}
        disabled={disabled}
        onChange={(e) => {
          setValue(e.target.value);
          setOpen(true);
          setActive(-1);
        }}
        onClick={() => setOpen(true)}
        onBlur={close}
        onKeyDown={(e) => {
          if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
            e.preventDefault();
            setOpen(true);
            const n = shown.length;
            if (n > 0) setActive((a) => (e.key === 'ArrowDown' ? (a + 1) % n : a <= 0 ? n - 1 : a - 1));
          } else if (e.key === 'Enter' && open && shown[active]) {
            e.preventDefault();
            choose(shown[active]);
          } else if (e.key === 'Escape') {
            close();
          }
        }}
      />
      {open && !disabled && (
        // A press anywhere on the list, its scrollbar too, leaves the focus in the box.
        <span className="picker-list" onMouseDown={(e) => e.preventDefault()}>
          <ul id={id} role="listbox" aria-label="Matches" ref={list}>
            {shown.map((s, i) => (
              <li
                key={s}
                id={`${id}-${i}`}
                role="option"
                aria-selected={i === active}
                onClick={() => choose(s)}
                onMouseEnter={() => setActive(i)}
              >
                {marked(s)}
              </li>
            ))}
          </ul>
          <span className="count">
            {total === 0
              ? `No symbol matches ${value.trim()}`
              : q
                ? `${total.toLocaleString('en-US')} ${total === 1 ? 'match' : 'matches'}${total > shown.length ? `, ${shown.length} shown` : ''}`
                : `${total.toLocaleString('en-US')} symbols: type to filter`}
          </span>
        </span>
      )}
    </span>
  );
}

// The wordmark and the header switch between the Trading Terminal and the Strategy Lab, at #lab.
export function Switch({ page }: { page: Page }) {
  const link = (to: Page, href: string, label: string) => (
    <a href={href} className={page === to ? 'on' : ''} aria-current={page === to ? 'page' : undefined}>
      {label}
    </a>
  );
  return (
    <nav className="pages">
      <span className="brand">CHRONOS</span>
      {link('terminal', '#', 'Terminal')}
      {link('lab', '#lab', 'Strategy Lab')}
      {link('strategies', '#strategies', 'Strategies')}
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
  const box = useRef<HTMLDetailsElement>(null);
  useEffect(() => {
    // A click anywhere else closes the list, as a menu would; a <details> alone stays open over the panels.
    const away = (e: MouseEvent) => {
      if (box.current?.open && !box.current.contains(e.target as Node)) box.current.open = false;
    };
    addEventListener('mousedown', away);
    return () => removeEventListener('mousedown', away);
  }, []);
  return (
    <details
      ref={box}
      className="days"
      onToggle={(e) => e.currentTarget.open && void load()}
      onKeyDown={(e) => e.key === 'Escape' && (e.currentTarget.open = false)}
    >
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
  // The server takes a directory entry exactly: send the entry itself, found whatever the case typed.
  const load = (typedName: string) => {
    const name = typedName.trim();
    if (!name) return;
    const entry = symbols.find((s) => s === name) ?? symbols.find((s) => s.toUpperCase() === name.toUpperCase());
    if (entry === undefined)
      store.notify(picked.length > 1 ? `${name} is not listed on every day picked` : `unknown symbol ${name}`);
    // No days picked yet (no state so far): the server replays the last load's.
    else if (store.control({ action: 'symbol', symbol: entry, days: picked.length > 0 ? picked : undefined })) {
      setTyped('');
    }
  };
  // The exchange time shown: a running seek's position, else the book's; 0 (shown as a dash) before a symbol loads.
  const at = seeking && seekAt > 0 ? seekAt : (snap?.exchange_ts ?? 0);
  return (
    <header className="bar">
      <Switch page="terminal" />
      <span className="sep" />
      <form
        onSubmit={(e) => {
          e.preventDefault();
          load(typed);
        }}
      >
        <SymbolPicker
          label="Symbol"
          inputRef={symbolBox}
          symbols={symbols}
          value={typed}
          setValue={setTyped}
          pick={load}
          placeholder={symbol || 'Symbol'}
          disabled={viewer}
        />
        <DayPicker days={days} load={store.loadDays} picked={picked} pick={setPicked} disabled={viewer} />
        {/* With the box empty, Load reloads the symbol on the days now picked; Enter in an empty box does nothing. */}
        <button
          type="button"
          disabled={viewer || !(typed.trim() || symbol)}
          title={typed.trim() || !symbol ? undefined : `Load ${symbol} on the days picked`}
          onClick={() => load(typed.trim() || symbol)}
        >
          Load
        </button>
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
      <span className="notice" title={notice}>
        {notice}
      </span>
      <span className={`status ${status}`} title={status === 'open' ? `Connected as the ${hello?.role}` : undefined}>
        {status === 'open' ? hello?.role.toUpperCase() : status.toUpperCase()}
      </span>
    </header>
  );
}
