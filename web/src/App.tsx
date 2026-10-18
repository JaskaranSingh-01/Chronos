import { useState } from 'react';
import { store, useStore } from './store';

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

export function App() {
  const status = useStore((s) => s.status);
  return (
    <div className="terminal">
      <header className="bar">Chronos · {status}</header>
      <Gate />
    </div>
  );
}
