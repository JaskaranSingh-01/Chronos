import type { Hello, MyOrder, Order, OrderType, Side, Spec } from './protocol';

export interface Ticket {
  side: Side;
  type: OrderType;
  price: string; // dollars, as typed
  qty: string;
}

// A dollar amount as ticks at `digits` decimals ('10.' and '.5' read as 10 and 0.5, '-0.002' as -20 at 4); NaN unless
// it is a safe integer of ticks with at most that many decimals, trailing zeros included.
export function signedTicks(text: string, digits: number): number {
  const m = /^(-?)(\d*)(?:\.(\d*))?$/.exec(text.trim());
  if (!m || !/\d/.test(text) || (m[3] ?? '').length > digits) return NaN;
  const ticks = Number(m[2]) * 10 ** digits + Number((m[3] ?? '').padEnd(digits, '0'));
  return Number.isSafeInteger(ticks) ? (m[1] ? -ticks : ticks) : NaN;
}

// A dollar price as ticks: signedTicks, and positive.
export function parseTicks(text: string, digits: number): number {
  const ticks = signedTicks(text, digits);
  return ticks > 0 ? ticks : NaN;
}

// The fees and limits a manual order faces: the generation's own once its state has come, a Watch's run's included.
export const terms = (hello: Hello, spec: Spec | null): Hello =>
  spec ? { ...hello, fees: spec.fees, limits: spec.limits } : hello;

// The order the ticket describes, or why it would be refused; the server checks again, in Portfolio::reserve's order.
export function validate(t: Ticket, hello: Hello, position: number, orders: readonly MyOrder[]): Order | string {
  const { max_order_quantity, max_open_orders, max_position } = hello.limits;
  const qty = /^\d+$/.test(t.qty.trim()) ? Number(t.qty) : NaN; // digits only: not '1e2', '0x10' or '+5'
  if (!Number.isSafeInteger(qty) || qty < 1) return 'Enter a whole number of shares';
  if (qty > max_order_quantity) return `At most ${max_order_quantity} shares an order`;
  if (orders.length >= max_open_orders) return `At most ${max_open_orders} open orders`;
  const open = orders.filter((o) => o.side === t.side).reduce((sum, o) => sum + o.remaining, 0);
  const worst = t.side === 'buy' ? position + open + qty : open + qty - position; // if every open order fills
  if (worst > max_position) return `Position limit ${max_position}`;
  if (t.type === 'market') return { side: t.side, order_type: 'market', qty };
  const price = parseTicks(t.price, hello.price_scale);
  if (Number.isNaN(price)) return 'Enter a price';
  return { side: t.side, order_type: t.type, qty, price };
}

// Notional and estimated fee, in money units, at `price` ticks: post-only pays the maker fee, IOC and market the taker.
export function cost(order: Order, hello: Hello, price: number): { notional: number; fee: number } {
  const perShare = order.order_type === 'post_only' ? hello.fees.maker : hello.fees.taker;
  return { notional: price * order.qty, fee: perShare * order.qty };
}
