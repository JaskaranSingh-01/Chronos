import { useEffect, useState } from 'react';
import { money, px, shares, signed, tone } from './format';
import { cost, parseTicks, terms, tickAt, validate } from './order';
import type { OrderType, Side } from './protocol';
import { selectOrders, selectTradeBlock, store, useStore } from './store';

const types: [OrderType, string][] = [
  ['post_only', 'Post-only'],
  ['ioc', 'IOC'],
  ['market', 'Market'],
];
const presets = [100, 200, 500, 1_000];

export function Ticket({ price, setPrice }: { price: string; setPrice: (price: string) => void }) {
  const hello = useStore((s) => s.hello);
  const spec = useStore((s) => s.spec);
  const symbol = useStore((s) => s.symbol);
  const blocked = useStore(selectTradeBlock);
  const position = useStore((s) => s.snap?.account.position ?? 0);
  const orders = useStore(selectOrders);
  const bid = useStore((s) => s.snap?.bids[0]?.[0] ?? 0);
  const ask = useStore((s) => s.snap?.asks[0]?.[0] ?? 0);
  const [side, setSide] = useState<Side>('buy');
  const [type, setType] = useState<OrderType>('post_only');
  const [qty, setQty] = useState('100');
  const [sent, setSent] = useState(false); // after a click, no second order until it has had time to show
  useEffect(() => {
    if (!sent) return;
    const timer = setTimeout(() => setSent(false), 300);
    return () => clearTimeout(timer);
  }, [sent]);
  if (!hello) return <section className="panel ticket" aria-label="Order ticket" />;
  const digits = hello.price_scale;
  const moneyDigits = hello.price_scale + hello.quantity_scale;
  const order = validate({ side, type, price, qty }, terms(hello, spec), position, orders);
  const valid = typeof order !== 'string';
  const at = valid ? (order.price ?? (side === 'buy' ? ask : bid)) : 0; // a market order's estimate: the best opposite
  const { notional, fee } = valid ? cost(order, terms(hello, spec), at) : { notional: 0, fee: 0 };
  const crosses = // a post-only order priced into the opposite side is rejected as would_cross
    valid &&
    order.order_type === 'post_only' &&
    order.price !== undefined &&
    (side === 'buy' ? ask > 0 && order.price >= ask : bid > 0 && order.price <= bid);
  // A grid step at a time, an off-grid price first snapped to the grid.
  const stepPrice = (by: number) => {
    const ticks = parseTicks(price, digits) || (side === 'buy' ? bid : ask);
    if (!(ticks > 0)) return;
    const tick = tickAt(ticks, digits);
    setPrice(px(Math.max(tick, Math.round(ticks / tick) * tick + by * tick), digits));
  };
  const stepQty = (by: number) => setQty(String(Math.max(1, (Number(qty) || 0) + by)));
  return (
    <section className="panel ticket" aria-label="Order ticket">
      <div className="title">
        Order ticket
        <span className="grow" />
        <b className="num">{symbol}</b>
      </div>
      <div className="body">
        <div className="sides">
          {(['sell', 'buy'] as const).map((s) => (
            <button
              key={s}
              aria-label={s === 'buy' ? 'Buy' : 'Sell'}
              className={`${s}${side === s ? ' on' : ''}`}
              aria-pressed={side === s}
              onClick={() => setSide(s)}
            >
              {s === 'buy' ? 'BUY' : 'SELL'}
              <span>{(s === 'buy' ? ask : bid) > 0 ? px(s === 'buy' ? ask : bid, digits) : '—'}</span>
            </button>
          ))}
        </div>
        <div className="seg types">
          {types.map(([t, name]) => (
            <button key={t} className={type === t ? 'on' : ''} aria-pressed={type === t} onClick={() => setType(t)}>
              {name}
            </button>
          ))}
        </div>
        <div className="stepper">
          <span className="muted">PX</span>
          <button aria-label="Tick down" disabled={type === 'market'} onClick={() => stepPrice(-1)}>
            −
          </button>
          <input
            aria-label="Price"
            value={type === 'market' ? '' : price}
            placeholder={type === 'market' ? 'market' : ''}
            disabled={type === 'market'}
            onChange={(e) => setPrice(e.target.value)}
          />
          <button aria-label="Tick up" disabled={type === 'market'} onClick={() => stepPrice(1)}>
            +
          </button>
          <span className="muted">QTY</span>
          <button aria-label="Quantity down 100" onClick={() => stepQty(-100)}>
            −
          </button>
          <input aria-label="Qty" value={qty} inputMode="numeric" onChange={(e) => setQty(e.target.value)} />
          <button aria-label="Quantity up 100" onClick={() => stepQty(100)}>
            +
          </button>
        </div>
        <div className="presets">
          {presets.map((n) => (
            <button key={n} className={qty === String(n) ? 'on' : ''} onClick={() => setQty(String(n))}>
              {n}
            </button>
          ))}
        </div>
        <div className="kv">
          <span className="muted">Notional</span>
          <span>{money(notional, moneyDigits)}</span>
        </div>
        <div className="kv">
          <span className="muted">{fee < 0 ? 'Est. rebate' : 'Est. fee'}</span>
          <span>{signed(-fee, moneyDigits)}</span>
        </div>
        <div className="kv">
          <span className="muted">Position after</span>
          <span className={valid ? tone(position + (side === 'buy' ? order.qty : -order.qty)) : ''}>
            {valid ? shares(position + (side === 'buy' ? order.qty : -order.qty)) : '—'}
          </span>
        </div>
        {crosses && <div className="hint">Would cross: use IOC</div>}
        <button
          className={`place ${side}`}
          disabled={blocked !== '' || !valid || sent}
          title={blocked || undefined}
          onClick={() => valid && store.order(order) && setSent(true)}
        >
          {valid
            ? `Place ${side} ${order.qty} ${order.price === undefined ? 'at market' : `@ ${px(order.price, digits)}`}`
            : order}
        </button>
        {blocked !== '' && <div className="lock">Locked: {blocked}</div>}
      </div>
    </section>
  );
}

export function Account() {
  const account = useStore((s) => s.snap?.account);
  const moneyDigits = useStore((s) => (s.hello ? s.hello.price_scale + s.hello.quantity_scale : 4));
  const logFailed = useStore((s) => s.snap?.metrics.log_failed ?? false);
  if (!account) return <section className="panel account" aria-label="Account" />;
  const rows: [string, string, string?][] = [
    ['Cash', money(account.cash, moneyDigits)],
    ['Position', shares(account.position), tone(account.position)],
    ['Realised', signed(account.realised, moneyDigits), tone(account.realised)],
    ['Unrealised', signed(account.unrealised, moneyDigits), tone(account.unrealised)],
    ['Fees', signed(-account.fees, moneyDigits)],
    ['PnL', signed(account.pnl, moneyDigits), tone(account.pnl)],
    ['Max drawdown', money(account.max_drawdown, moneyDigits)],
  ];
  return (
    <section className="panel account" aria-label="Account">
      <div className="title">Account</div>
      <div className="body">
        {rows.map(([name, value, color]) => (
          <div key={name} className="kv">
            <span className="muted">{name}</span>
            <span data-testid={name} className={color}>
              {value}
            </span>
          </div>
        ))}
        {logFailed && <div className="sell">The command log failed: new commands are refused.</div>}
      </div>
    </section>
  );
}
