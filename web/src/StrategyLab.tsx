import { signed } from './format';
import { lab, moneyDigits, strategyNames, useLab, type RunListing } from './lab';
import { Switch } from './TopBar';

// A listed run's state while it is queued or running or ended without results, else its PnL.
function result(r: RunListing): string {
  if (r.state === 'running') return `running ${Math.round(r.progress * 100)}%`;
  if (r.pnl === null) return r.state;
  return `${signed(r.pnl, moneyDigits(r))}${r.partial ? ` · ${r.state}` : ''}`;
}

// Every run, newest first, numbered in the order they were queued; a click shows one.
function Runs() {
  const runs = useLab((l) => l.runs);
  const selected = useLab((l) => l.selected);
  return (
    <div className="panel runs">
      <div className="title">Runs</div>
      <table>
        <tbody>
          <tr className="muted">
            <td>#</td>
            <td>Strategy</td>
            <td>Symbol</td>
            <td>Days</td>
            <td>Result</td>
          </tr>
          {runs
            .map((r, i) => (
              <tr key={r.id} className={r.id === selected ? 'on' : ''} onClick={() => void lab.select(r.id)}>
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
    </div>
  );
}

// The Strategy Lab: a run's settings on the left, its results in the middle, the runs on the right.
export function StrategyLab() {
  const offline = useLab((l) => l.offline);
  return (
    <>
      <header className="bar">
        <Switch lab />
        <span className="notice">{offline ? 'chronos-serve does not answer: retrying' : ''}</span>
      </header>
      <main className="lab">
        <section className="col" />
        <section className="col" />
        <section className="col">
          <Runs />
        </section>
      </main>
    </>
  );
}
