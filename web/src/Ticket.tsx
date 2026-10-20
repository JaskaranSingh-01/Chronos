import { useEffect, useState } from 'react';
import { money, px, shares, signed, tone } from './format';
import { cost, terms, validate } from './order';
import type { OrderType, Side } from './protocol';
import { selectOrders, selectTradeBlock, store, useStore } from './store';

const types: [OrderType, string][] = [
  ['post_only', 'Limit post-only'],
  ['ioc', 'IOC'],
  ['market', 'Market'],
];

export function Ticket({ price, setPrice }: { price: string; setPrice: (price: string) => void }) {
  const hello = useStore((s) => s.hello);
  const spec = useStore((s) => s.spec);
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
  if (!hello) return <div className="panel ticket" />;
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
  return (
    <div className="panel ticket">
      <div className="title">Order ticket</div>
      <div className="seg">
        {(['buy', 'sell'] as const).map((s) => (
          <button key={s} className={side === s ? `${s} on` : ''} aria-pressed={side === s} onClick={() => setSide(s)}>
            {s === 'buy' ? 'Buy' : 'Sell'}
          </button>
        ))}
      </div>
      <div className="seg">
        {types.map(([t, name]) => (
          <button key={t} className={type === t ? 'on' : ''} aria-pressed={type === t} onClick={() => setType(t)}>
            {name}
          </button>
        ))}
      </div>
      <label className="kv">
        Price
        <input
          aria-label="Price"
          value={type === 'market' ? '' : price}
          placeholder={type === 'market' ? 'market' : ''}
          disabled={type === 'market'}
          onChange={(e) => setPrice(e.target.value)}
        />
      </label>
      <label className="kv">
        Qty
        <input aria-label="Qty" value={qty} inputMode="numeric" onChange={(e) => setQty(e.target.value)} />
      </label>
      <div className="kv muted">
        <span>Notional</span>
        <span>{money(notional, moneyDigits)}</span>
      </div>
      <div className="kv muted">
        <span>{fee < 0 ? 'Est. rebate' : 'Est. fee'}</span>
        <span>{signed(-fee, moneyDigits)}</span>
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
    </div>
  );
}

export function Account() {
  const account = useStore((s) => s.snap?.account);
  const moneyDigits = useStore((s) => (s.hello ? s.hello.price_scale + s.hello.quantity_scale : 4));
  const logFailed = useStore((s) => s.snap?.metrics.log_failed ?? false);
  if (!account) return <div className="panel account" />;
  const rows: [string, string, string?][] = [
    ['Cash', money(account.cash, moneyDigits)],
    ['Position', shares(account.position), tone(account.position)],
    ['Realised', signed(account.realised, moneyDigits)],
    ['Unrealised', signed(account.unrealised, moneyDigits)],
    ['Fees', signed(-account.fees, moneyDigits)],
    ['PnL', signed(account.pnl, moneyDigits)],
    ['Max drawdown', money(account.max_drawdown, moneyDigits)],
  ];
  return (
    <div className="panel account">
      <div className="title">Account</div>
      {rows.map(([name, value, color]) => (
        <div key={name} className="kv">
          <span>{name}</span>
          <span data-testid={name} className={color}>
            {value}
          </span>
        </div>
      ))}
      {logFailed && <div className="sell">The command log failed: new commands are refused.</div>}
    </div>
  );
}
