import { useEffect, useRef, useState, type KeyboardEvent } from 'react';
import { lab, scriptName, useLab, type ScriptCheck } from './lab';
import { Switch } from './TopBar';

// A new script's text: a complete strategy to edit, as docs/strategies.md explains it.
export const template = `-- A strategy is a Lua script: it declares its params and limits, then defines callbacks.
summary = "Quotes both sides at the touch"

-- param(name, default, {min, max, help}) declares a param and returns its value in this run.
local size = param("size", 100, {min = 1, max = 10000, help = "shares a quote"})
local max_position = param("max_position", 500, {help = "shares held long or short at most"})

-- The run's risk limits unless the Lab's form sets its own.
limits{max_position = max_position, max_order_quantity = size, max_open_orders = 2}

local bid_id, ask_id, fills = nil, nil, 0

-- After every event the book applied: e.type, e.side, e.price, e.qty, e.time, e.trade_price, e.trade_qty.
function on_market(e)
  if not trading() then return end
  local bid = best_bid()
  local ask = best_ask()
  if bid and not bid_id and position() + size <= max_position then
    bid_id = buy(bid, size, "post_only")
  end
  if ask and not ask_id and position() - size >= -max_position then
    ask_id = sell(ask, size, "post_only")
  end
end

-- A report on one of your orders: r.type is accepted, rejected, fill, cancelled, cancel_rejected or expired.
function on_report(r)
  if r.type == "fill" then
    fills = fills + 1
    stat("Fills", fills)
    log("fill", r.side, r.qty, "at", string.format("%.2f", dollars(r.price)))
  end
  if r.type ~= "accepted" and r.leaves == 0 then
    if r.id == bid_id then bid_id = nil elseif r.id == ask_id then ask_id = nil end
  end
end
`;

// The line a script's error names, e.g. 12 in "on_market: script:12: attempt to index a nil value".
export const errorLine = (error: string) => Number(/script:(\d+):/.exec(error)?.[1] ?? 0);

// What loading a script found, in words: its params and limits, or the error and its line.
export function checkText(check: ScriptCheck): string {
  if (!check.ok) return check.error;
  const params = check.params.map((p) => `${p.name} ${p.default}`).join(', ');
  const l = check.limits;
  return `Loads. Params: ${params || 'none'}. Limits: ${l.max_position} position, ${l.max_order_quantity} an order, ${l.max_open_orders} open.`;
}

// Why a script cannot be saved as it stands, or ''. The server takes a script of at most 48 KiB, and a request of at most
// 64 KiB, which a script's quotes, backslashes and control characters fill twice as fast once escaped as JSON.
export function tooLarge(source: string): string {
  if (new TextEncoder().encode(source).length > 48 * 1024) return 'Not saved: over 48 KiB';
  if (new TextEncoder().encode(JSON.stringify({ source })).length > 63 * 1024)
    return 'Not saved: over 64 KiB as JSON, where quotes, backslashes and tabs count twice';
  return '';
}

// Tab indents by two spaces rather than leaving the box; Cmd/Ctrl+S saves.
function keys(e: KeyboardEvent<HTMLTextAreaElement>, save: () => void, set: (text: string) => void) {
  if ((e.metaKey || e.ctrlKey) && e.key === 's') {
    e.preventDefault();
    save();
  } else if (e.key === 'Tab' && !e.shiftKey) {
    e.preventDefault();
    const box = e.currentTarget;
    const { selectionStart: at, selectionEnd: end, value } = box;
    set(value.slice(0, at) + '  ' + value.slice(end));
    requestAnimationFrame(() => box.setSelectionRange(at + 2, at + 2));
  }
}

function Guide() {
  return (
    <div className="body guide">
      <h3>Before you write or test a strategy</h3>
      <ol>
        <li>
          <b>It reads history, never changes it.</b> The book is the historical one. Your orders fill only as the queue
          ahead of them at their price trades or leaves; they never move the market.
        </li>
        <li>
          <b>Acting takes time.</b> An order arrives 50 µs after you send it; its report reaches you 50 µs after the
          exchange made it. Count a fill only once its report arrives.
        </li>
        <li>
          <b>Every run must repeat.</b> There is no clock, file or network. <code>math.random</code> starts from the
          same seed each run, and no address is ever shown. Iterate with <code>ipairs</code>, or <code>pairs</code> over
          string and number keys, never over tables used as keys.
        </li>
        <li>
          <b>Units are integers.</b> A price is in ticks: <code>price_scale</code> is 4, so $1 is 10,000 ticks, and at
          $1 or more a price must be a whole cent (a multiple of 100). Use <code>ticks(9.99)</code> and{' '}
          <code>dollars(p)</code>. Quantities are shares; times are ns since the epoch.
        </li>
        <li>
          <b>Limits are checked on every order.</b> <code>buy</code> and <code>sell</code> return the order id, or nil
          and why: <code>order_too_large</code>, <code>position_limit</code>, <code>too_many_open_orders</code>,{' '}
          <code>closed</code>.
        </li>
        <li>
          <b>Each call has a budget.</b> 10 million instructions, 1 GiB allocated, 30 s, 256 orders and cancels, and 64
          MiB held. Past any of them the run stops, naming the line (but for memory), and <code>pcall</code> cannot
          catch it. <code>set_timer</code> refuses a 65th pending timer. <code>string.find</code> matches plain text;
          there are no patterns, <code>xpcall</code> or <code>__gc</code>. The event and report tables are reused: copy
          a field to keep it.
        </li>
        <li>
          <b>Save, then test.</b> Save loads the script and shows its params or its error. Then run it in the Strategy
          Lab on one day, read its fills, stats and log, compare it with another run, verify it, and watch it in the
          Terminal. A run keeps the source it ran, so editing the script later never changes a saved run.
        </li>
      </ol>
      <h3>What a script can call</h3>
      <table>
        <tbody>
          <tr>
            <td>
              <code>param(name, default, opts)</code>, <code>limits{'{...}'}</code>
            </td>
            <td>at the top level only</td>
          </tr>
          <tr>
            <td>
              <code>on_market(e)</code>, <code>on_report(r)</code>, <code>on_timer(id)</code>
            </td>
            <td>the callbacks you define</td>
          </tr>
          <tr>
            <td>
              <code>best_bid()</code>, <code>best_ask()</code>, <code>level(side, n)</code>
            </td>
            <td>price, shares, orders; or nil</td>
          </tr>
          <tr>
            <td>
              <code>buy(price, qty, tif)</code>, <code>sell(...)</code>, <code>cancel(id)</code>
            </td>
            <td>tif: "ioc" (default) or "post_only"</td>
          </tr>
          <tr>
            <td>
              <code>position()</code>, <code>cash()</code>, <code>pnl()</code>, <code>open_orders()</code>
            </td>
            <td>money in ticks × shares</td>
          </tr>
          <tr>
            <td>
              <code>now()</code>, <code>trading()</code>, <code>session()</code>, <code>set_timer(at, id)</code>
            </td>
            <td>exchange time; the day&apos;s open and close</td>
          </tr>
          <tr>
            <td>
              <code>stat(name, value, is_price)</code>, <code>log(...)</code>
            </td>
            <td>shown with the run&apos;s results</td>
          </tr>
        </tbody>
      </table>
      <p className="muted">The full guide is docs/strategies.md in the repository.</p>
    </div>
  );
}

// The strategies written in the Lab: a list, an editor that saves and checks each one, and the guidelines beside it.
export function Strategies() {
  const scripts = useLab((l) => l.scripts);
  const [name, setName] = useState(''); // the script open; '' for none
  const [text, setText] = useState('');
  const [saved, setSaved] = useState(''); // the text as last saved or opened
  const [check, setCheck] = useState<ScriptCheck | null>(null);
  const [notice, setNotice] = useState({ text: '', bad: false }); // a refusal is shown red, news muted
  const say = (text: string, bad = false) => setNotice({ text, bad });
  const [fresh, setFresh] = useState(''); // a new script's name, as typed
  const [deleting, setDeleting] = useState(false); // Delete asks again before it deletes
  useEffect(() => void lab.loadScripts(), []);
  // The script open now, for an answer that arrives after another was opened: it must not land on that one.
  const current = useRef('');
  current.current = name;

  // Opens a saved script; edits not saved to the one open are dropped, and the notice says so.
  const open = async (n: string) => {
    const dropped = name && text !== saved ? name : '';
    current.current = n;
    const script = await lab.openScript(n);
    if (current.current !== n) return; // another was opened meanwhile
    if (!script) return say(`${n}: could not be opened`, true);
    setName(n);
    setText(script.source);
    setSaved(script.source);
    setCheck(script.check);
    setDeleting(false);
    say(dropped ? `${dropped}: unsaved edits dropped` : '');
  };
  const create = () => {
    if (!scriptName(fresh)) return say('A name is 1-40 of a-z, 0-9 and _', true);
    if (scripts.some((s) => s.name === fresh)) return void open(fresh);
    setName(fresh);
    setText(template);
    setSaved('');
    setCheck(null);
    setFresh('');
    say(`${fresh}: new, not saved yet`);
  };
  const save = async () => {
    if (!name) return;
    const why = tooLarge(text);
    if (why) return say(why, true);
    const saving = name;
    const answer = await lab.saveScript(saving, text);
    if (typeof answer === 'string') return say(answer, true);
    if (current.current !== saving) return say(`${saving}: saved`); // another is open now: its own check stays
    setSaved(answer.source);
    setCheck(answer.check);
    say(answer.check.ok ? `${name}: saved; pick “${name} (script)” in the Strategy Lab` : `${name}: saved`);
  };
  const remove = async () => {
    if (!deleting) return setDeleting(true);
    if (await lab.deleteScript(name)) {
      say(`${name}: deleted`);
      setName('');
      setText('');
      setSaved('');
      setCheck(null);
    } else say(`${name}: not deleted`, true);
    setDeleting(false);
  };
  const line = check && !check.ok ? errorLine(check.error) : 0;

  return (
    <>
      <header className="bar">
        <Switch page="strategies" />
        <span className="sep" />
        <span className={notice.bad ? 'notice' : 'notice ok'}>{notice.text}</span>
      </header>
      <main className="strategies">
        <section className="panel" aria-label="Scripts">
          <div className="title">Strategies</div>
          <div className="body">
            <ul className="scripts">
              {scripts.map((s) => (
                <li key={s.name}>
                  <button
                    className={s.name === name ? 'on' : ''}
                    aria-label={`open ${s.name}`}
                    title={s.ok ? 'loads' : s.error}
                    onClick={() => void open(s.name)}
                  >
                    {s.name}
                  </button>
                  <span className={s.ok ? 'buy' : 'sell'}>{s.ok ? 'ok' : 'error'}</span>
                </li>
              ))}
            </ul>
            {scripts.length === 0 && <p className="muted">None yet: name one below.</p>}
            <form
              className="new"
              onSubmit={(e) => {
                e.preventDefault();
                create();
              }}
            >
              <input
                aria-label="new strategy name"
                placeholder="my_strategy"
                value={fresh}
                onChange={(e) => setFresh(e.target.value.trim())}
              />
              <button>New</button>
            </form>
          </div>
        </section>
        <section className="panel editor" aria-label="Editor">
          <div className="title">
            {name ? (
              <>
                <b>{name}</b>
                {text !== saved && <span aria-label="unsaved">●</span>}
              </>
            ) : (
              'Editor'
            )}
            <span className="grow" />
            {name && (
              <>
                <button className="primary" onClick={() => void save()} title="Cmd/Ctrl+S">
                  Save
                </button>
                {saved !== '' && <button onClick={() => void remove()}>{deleting ? 'Delete: sure?' : 'Delete'}</button>}
              </>
            )}
          </div>
          {name ? (
            <>
              <textarea
                aria-label="source"
                spellCheck={false}
                value={text}
                onChange={(e) => setText(e.target.value)}
                onKeyDown={(e) => keys(e, () => void save(), setText)}
              />
              <p className={`check ${check === null ? 'muted' : check.ok ? 'buy' : 'sell'}`} role="status">
                {check === null
                  ? 'Save checks the script: it loads it, then lists its params or its error.'
                  : checkText(check)}
                {line > 0 && ` (line ${line})`}
              </p>
            </>
          ) : (
            <p className="body muted">Open a strategy, or name a new one: it starts from a template that runs.</p>
          )}
        </section>
        <section className="panel" aria-label="Guidelines">
          <div className="title">Guidelines</div>
          <Guide />
        </section>
      </main>
    </>
  );
}
