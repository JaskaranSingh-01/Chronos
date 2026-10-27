import { useState } from 'react';
import { clock, money, px, ruleLabel, shares, signed, tone } from './format';
import { pnlCurve } from './pnl';
import { selectOrders, store, useStore } from './store';

const names = ['Open orders', 'Fills', 'Queue'] as const;
const fillRows = 200; // ponytail: the last 200 fills, newest first; a longer list needs windowing
// The position, marked at the bid when long and the ask when short, over the session's PnL as this page saw it.
export function PnlPanel() {
  const symbol = useStore((s) => s.snap?.symbol);
  const a = useStore((s) => s.snap?.account);
  const mark = useStore((s) => (s.snap && s.snap.account.position > 0 ? s.snap.bids[0] : s.snap?.asks[0])?.[0]);
  const pnl = useStore((s) => s.pnl);
  const hello = useStore((s) => s.hello);
  const digits = hello?.price_scale ?? 4;
  const cash = digits + (hello?.quantity_scale ?? 0);
  const c = pnl.length > 1 ? pnlCurve(pnl) : null;
  return (
    <section className="panel pnl" aria-label="Positions and PnL">
      <div className="title">
        Positions &amp; PnL
        <span className="grow" />
        {c ? (
          <span className="muted">
            {clock(c.from)} → {clock(c.to)} · high <b>{signed(c.hi, cash)}</b> · low <b>{signed(c.lo, cash)}</b>
          </span>
        ) : (
          <span className="muted">net PnL from the first change, as this page saw it</span>
        )}
      </div>
      <table>
        <tbody>
          <tr className="muted">
            <td>Sym</td>
            <td>Qty</td>
            <td>Avg</td>
            <td>Mark</td>
            <td>uPnL</td>
            <td>rPnL</td>
            <td>Net</td>
            <td>Max DD</td>
          </tr>
          {a && symbol && (
            <tr>
              <td>{symbol}</td>
              <td className={tone(a.position)}>{shares(a.position)}</td>
              <td>{a.position === 0 ? '—' : px(Math.round((a.realised - a.cash) / a.position), digits)}</td>
              <td>{mark === undefined ? '—' : px(mark, digits)}</td>
              <td className={tone(a.unrealised)}>{signed(a.unrealised, cash)}</td>
              <td className={tone(a.realised)}>{signed(a.realised, cash)}</td>
              <td className={tone(a.pnl)}>{signed(a.pnl, cash)}</td>
              <td>{money(a.max_drawdown, cash)}</td>
            </tr>
          )}
        </tbody>
      </table>
      {c && (
        <svg
          viewBox="0 0 1000 100"
          preserveAspectRatio="none"
          role="img"
          aria-label={`Net PnL now ${signed(pnl.at(-1)![1], cash)}`}
        >
          <line x1="0" x2="1000" y1={c.zero} y2={c.zero} stroke="#3a4655" vectorEffect="non-scaling-stroke" />
          <path d={`${c.d} L1000 ${c.zero} L0 ${c.zero} Z`} fill="rgba(90,169,255,0.10)" />
          <path d={c.d} fill="none" stroke="#5aa9ff" strokeWidth="1.5" vectorEffect="non-scaling-stroke" />
        </svg>
      )}
    </section>
  );
}

// Open orders (with cancel), fills and each resting order's place in the queue.
export function Tabs() {
  const [tab, setTab] = useState<(typeof names)[number]>('Open orders');
  const orders = useStore(selectOrders);
  const fills = useStore((s) => s.fills);
  const hello = useStore((s) => s.hello);
  const incomplete = useStore((s) => s.historyIncomplete);
  const digits = hello?.price_scale ?? 4;
  const controller = hello?.role === 'controller';
  return (
    <section className="panel tabs" aria-label="Orders">
      <div className="tabs-head">
        {names.map((name) => (
          <button key={name} className={tab === name ? 'on' : ''} onClick={() => setTab(name)}>
            {name === 'Open orders'
              ? `Open orders (${orders.length})`
              : name === 'Fills'
                ? `Fills ${fills.length}`
                : name}
          </button>
        ))}
        {incomplete && <span className="sell"> · report history incomplete</span>}
      </div>
      <div className="body">
        <table>
          {tab === 'Open orders' && (
            <tbody>
              <tr className="muted">
                <td>Id</td>
                <td>Side</td>
                <td>Price</td>
                <td>Remaining</td>
                <td>Ahead</td>
                <td />
              </tr>
              {orders.map((o) => (
                <tr key={o.id}>
                  <td>{o.id}</td>
                  <td className={o.side}>{o.side}</td>
                  <td>{px(o.price, digits)}</td>
                  <td>{o.remaining}</td>
                  <td>{o.ahead_quantity}</td>
                  <td>
                    <button disabled={!controller} onClick={() => store.cancel(o.id)}>
                      Cancel
                    </button>
                  </td>
                </tr>
              ))}
            </tbody>
          )}
          {tab === 'Fills' && (
            <tbody>
              <tr className="muted">
                <td>Time</td>
                <td>Order</td>
                <td>Side</td>
                <td>Qty</td>
                <td>Price</td>
                <td>Liquidity</td>
                <td>Rule</td>
                <td>Ahead at entry</td>
              </tr>
              {fills
                .slice(-fillRows)
                .reverse()
                .map((f) => (
                  <tr key={f.seq}>
                    <td>{clock(f.exchange_ts)}</td>
                    <td>{f.order_id}</td>
                    <td className={f.side}>{f.side}</td>
                    <td>{f.quantity}</td>
                    <td>{px(f.price, digits)}</td>
                    <td>{f.liquidity}</td>
                    <td>{ruleLabel(f.rule)}</td>
                    <td>{f.ahead_at_entry ?? '—'}</td>
                  </tr>
                ))}
            </tbody>
          )}
          {tab === 'Queue' && (
            <tbody>
              <tr className="muted">
                <td>Order</td>
                <td>Side</td>
                <td>Price</td>
                <td>Remaining</td>
                <td>Shares ahead</td>
                <td>Orders ahead</td>
              </tr>
              {orders.map((o) => (
                <tr key={o.id}>
                  <td>{o.id}</td>
                  <td className={o.side}>{o.side}</td>
                  <td>{px(o.price, digits)}</td>
                  <td>{o.remaining}</td>
                  <td>{o.ahead_quantity}</td>
                  <td>{o.ahead_orders}</td>
                </tr>
              ))}
            </tbody>
          )}
        </table>
      </div>
    </section>
  );
}
