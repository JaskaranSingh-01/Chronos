import { useState } from 'react';
import { Book, Tape } from './Book';
import { Chart } from './Chart';
import { store, useStore } from './store';
import { Tabs } from './Tabs';
import { Account, Ticket } from './Ticket';

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

// The Trading Terminal. Each panel subscribes to its own slice, so a snapshot never re-renders this tree.
export function App() {
  const status = useStore((s) => s.status);
  const [price, setPrice] = useState('');
  return (
    <div className="terminal">
      <header className="bar">Chronos · {status}</header>
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
      <Gate />
    </div>
  );
}
