import { useState, useSyncExternalStore } from 'react';
import { Book, Tape } from './Book';
import { Chart } from './Chart';
import { outcomes, page, runLabel, useLab } from './lab';
import { store, useStore } from './store';
import { StrategyLab } from './StrategyLab';
import { Tabs } from './Tabs';
import { Account, Ticket } from './Ticket';
import { TopBar } from './TopBar';

// Asks for the token a non-loopback server wants, or says why the terminal cannot connect.
function Gate() {
  const status = useStore((s) => s.status);
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

// Says which Lab run this generation replays, and, once it has ended, the verdict.
function WatchBanner() {
  const runId = useStore((s) => s.runId);
  const verdict = useStore((s) => s.verdict);
  const label = useLab((l) => runLabel(l.runs, runId));
  if (!runId) return null;
  return (
    <p className="banner" data-testid="watch">
      Watching run {label}
      {verdict ? (
        <b className={outcomes[verdict.outcome][1]}> · {outcomes[verdict.outcome][0]}</b>
      ) : (
        ': the verdict comes when its replay ends.'
      )}
    </p>
  );
}

const onHash = (f: () => void) => {
  addEventListener('hashchange', f);
  return () => removeEventListener('hashchange', f);
};

// The Trading Terminal, or the Strategy Lab at #lab. Each panel subscribes to its own slice, so a snapshot never
// re-renders this tree.
export function App() {
  const [price, setPrice] = useState('');
  const shown = useSyncExternalStore(onHash, () => page(location.hash));
  return (
    <div className="terminal">
      {shown === 'lab' ? (
        <StrategyLab />
      ) : (
        <>
          <TopBar />
          <ViewerBanner />
          <WatchBanner />
          <main>
            <section className="col">
              <Chart />
              <Tabs />
            </section>
            <section className="col">
              <Book onPick={setPrice} />
              <Tape />
            </section>
            <section className="col">
              <Ticket price={price} setPrice={setPrice} />
              <Account />
            </section>
          </main>
        </>
      )}
      <Gate />
    </div>
  );
}
