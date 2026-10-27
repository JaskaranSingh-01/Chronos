import { describe, expect, it } from 'vitest';
import { cost, parseTicks, signedTicks, terms, validate, type Ticket } from './order';
import type { Hello, MyOrder, Spec } from './protocol';

const hello: Hello = {
  type: 'hello',
  session_id: 's1',
  generation: 2,
  protocol: 1,
  role: 'controller',
  price_scale: 4,
  quantity_scale: 0,
  fees: { maker: -20, taker: 30 },
  limits: { max_position: 1_000, max_order_quantity: 500, max_open_orders: 2 },
};
const ticket = (over: Partial<Ticket> = {}): Ticket => ({
  side: 'buy',
  type: 'post_only',
  price: '10.01',
  qty: '100',
  ...over,
});
const resting = (side: 'buy' | 'sell', remaining: number): MyOrder => ({
  id: 1,
  side,
  price: 100_000,
  remaining,
  ahead_quantity: 0,
  ahead_orders: 0,
});

describe('parseTicks', () => {
  it('reads a dollar price exactly, refusing anything finer than a tick or not positive', () => {
    expect(parseTicks('10.01', 4)).toBe(100_100);
    expect(parseTicks(' 10 ', 4)).toBe(100_000);
    expect(parseTicks('0.0001', 4)).toBe(1);
    expect(parseTicks('289.3699', 4)).toBe(2_893_699);
    for (const bad of ['', '0', '0.00', '-1', '1e3', '10.00001', 'abc', '10.0.1']) expect(parseTicks(bad, 4)).toBeNaN();
  });

  it('accepts a bare point either side of the digits, and refuses what is not a plain decimal', () => {
    expect(parseTicks('10.', 4)).toBe(100_000);
    expect(parseTicks('.5', 4)).toBe(5_000);
    for (const bad of ['.', '+5', '1,000', '1 000', '10.010000', '٣', '1_000']) expect(parseTicks(bad, 4)).toBeNaN();
  });

  it('refuses a price too large to be exact in ticks', () => {
    expect(parseTicks('900719925474.0991', 4)).toBe(Number.MAX_SAFE_INTEGER);
    expect(parseTicks('900719925474.0992', 4)).toBeNaN();
    expect(parseTicks('99999999999999999', 4)).toBeNaN();
    expect(parseTicks('9007199254740992', 0)).toBeNaN();
  });

  it('follows the price scale: whole numbers at 0 digits, cents at 2', () => {
    expect(parseTicks('10', 0)).toBe(10);
    expect(parseTicks('10.', 0)).toBe(10);
    for (const bad of ['10.5', '10.0', '0']) expect(parseTicks(bad, 0)).toBeNaN();
    expect(parseTicks('0.01', 2)).toBe(1);
    expect(parseTicks('10.011', 2)).toBeNaN();
  });
});

describe('signedTicks', () => {
  it('reads a dollar amount of either sign exactly, for fees, and refuses one finer than a tick', () => {
    expect([signedTicks('-0.002', 4), signedTicks('0.003', 4), signedTicks('0', 4), signedTicks('.5', 4)]).toEqual([
      -20, 30, 0, 5_000,
    ]);
    for (const bad of ['', '-', '.', '0.00001', '+1', '1e3', '--1']) expect(signedTicks(bad, 4)).toBeNaN();
  });
});

describe('validate', () => {
  it('builds the order the server expects: a price in ticks, none for market', () => {
    expect(validate(ticket(), hello, 0, [])).toEqual({
      side: 'buy',
      order_type: 'post_only',
      qty: 100,
      price: 100_100,
    });
    expect(validate(ticket({ type: 'ioc', side: 'sell' }), hello, 0, [])).toEqual({
      side: 'sell',
      order_type: 'ioc',
      qty: 100,
      price: 100_100,
    });
    expect(validate(ticket({ type: 'market', price: 'junk' }), hello, 0, [])).toEqual({
      side: 'buy',
      order_type: 'market',
      qty: 100,
    });
  });

  it('refuses what the risk check would, against the hello limits', () => {
    expect(validate(ticket({ price: '289.3699' }), hello, 0, [])).toBe('Whole cents at $1 or more');
    expect(validate(ticket({ price: '0.5001' }), hello, 0, [])).toMatchObject({ price: 5_001 }); // any tick under $1
    expect(validate(ticket({ qty: '0' }), hello, 0, [])).toBe('Enter a whole number of shares');
    expect(validate(ticket({ qty: '1.5' }), hello, 0, [])).toBe('Enter a whole number of shares');
    expect(validate(ticket({ qty: '501' }), hello, 0, [])).toBe('At most 500 shares an order');
    expect(validate(ticket(), hello, 0, [resting('buy', 1), resting('sell', 1)])).toBe('At most 2 open orders');
    expect(validate(ticket({ qty: '500' }), hello, 600, [])).toBe('Position limit 1000');
    expect(validate(ticket({ qty: '500' }), hello, 400, [resting('buy', 200)])).toBe('Position limit 1000'); // if it fills
    expect(validate(ticket({ side: 'sell', qty: '500' }), hello, -600, [])).toBe('Position limit 1000');
    expect(validate(ticket({ side: 'sell', qty: '500' }), hello, 600, [])).not.toBeTypeOf('string'); // selling a long
    expect(validate(ticket({ price: '10.00001' }), hello, 0, [])).toBe('Enter a price');
  });

  it('takes a quantity of plain digits only', () => {
    expect(validate(ticket({ qty: ' 100 ' }), hello, 0, [])).toMatchObject({ qty: 100 });
    for (const bad of ['', ' ', 'abc', '-5', '+5', '1e2', '0x10', '1,0', '1_0', '99999999999999999999'])
      expect(validate(ticket({ qty: bad }), hello, 0, [])).toBe('Enter a whole number of shares');
  });

  it('allows exactly the limits: max_position, max_order_quantity, and one below max_open_orders', () => {
    expect(validate(ticket({ qty: '500' }), hello, 500, [])).not.toBeTypeOf('string'); // 1000
    expect(validate(ticket({ qty: '500' }), hello, 501, [])).toBe('Position limit 1000'); // 1001
    expect(validate(ticket({ side: 'sell', qty: '500' }), hello, -500, [])).not.toBeTypeOf('string');
    expect(validate(ticket({ side: 'sell', qty: '500' }), hello, -501, [])).toBe('Position limit 1000');
    expect(validate(ticket({ qty: '500' }), hello, 0, [])).not.toBeTypeOf('string');
    expect(validate(ticket(), hello, 0, [resting('buy', 1)])).not.toBeTypeOf('string'); // 1 of 2
  });

  it('counts resting sells for a sell and ignores the other side', () => {
    const sell = ticket({ side: 'sell', qty: '500' });
    expect(validate(sell, hello, -200, [resting('sell', 400)])).toBe('Position limit 1000'); // 400 + 500 + 200
    expect(validate(sell, hello, -100, [resting('sell', 400)])).not.toBeTypeOf('string'); // 1000
    expect(validate(ticket({ qty: '500' }), hello, 400, [resting('sell', 900)])).not.toBeTypeOf('string');
    expect(validate(ticket({ side: 'sell', qty: '500' }), hello, 0, [resting('buy', 900)])).not.toBeTypeOf('string');
  });

  it('checks quantity, then the order size, the open orders, the position and last the price', () => {
    const two = [resting('buy', 1), resting('sell', 1)];
    expect(validate(ticket({ qty: '0', price: 'x' }), hello, 999, two)).toBe('Enter a whole number of shares');
    expect(validate(ticket({ qty: '501', price: 'x' }), hello, 999, two)).toBe('At most 500 shares an order');
    expect(validate(ticket({ qty: '500', price: 'x' }), hello, 999, two)).toBe('At most 2 open orders');
    expect(validate(ticket({ qty: '500', price: 'x' }), hello, 999, [])).toBe('Position limit 1000');
    expect(validate(ticket({ qty: '500', price: 'x' }), hello, 0, [])).toBe('Enter a price');
  });

  it('limit-checks a market order, and wants a price for the others', () => {
    expect(validate(ticket({ type: 'market', qty: '501' }), hello, 0, [])).toBe('At most 500 shares an order');
    expect(validate(ticket({ type: 'market', qty: '500' }), hello, 600, [])).toBe('Position limit 1000');
    expect(validate(ticket({ type: 'market' }), hello, 0, [resting('buy', 1), resting('buy', 1)])).toBe(
      'At most 2 open orders',
    );
    for (const type of ['ioc', 'post_only'] as const) {
      for (const price of ['', '0', '0.00', 'x']) {
        expect(validate(ticket({ type, price }), hello, 0, [])).toBe('Enter a price');
      }
    }
  });
});

describe('terms', () => {
  it("prices and limits a manual order by the generation's spec, a Watch's included, else by hello", () => {
    const spec: Spec = {
      symbol: 'ACME',
      days: ['2019-12-30'],
      strategy: 'market_maker',
      params: {},
      latency: { order_ns: 50_000, report_ns: 50_000 },
      fees: { maker: -25, taker: 35 },
      limits: { max_position: 500, max_order_quantity: 100, max_open_orders: 2 },
      window: 'regular',
    };
    expect(terms(hello, null)).toBe(hello);
    expect(terms(hello, spec)).toEqual({ ...hello, fees: spec.fees, limits: spec.limits });
    expect(validate(ticket({ qty: '200' }), terms(hello, spec), 0, [])).toBe('At most 100 shares an order');
  });
});

describe('cost', () => {
  it('estimates notional and the fee: maker for post-only, taker otherwise', () => {
    expect(cost({ side: 'buy', order_type: 'post_only', qty: 100, price: 100_100 }, hello, 100_100)).toEqual({
      notional: 10_010_000,
      fee: -2_000,
    });
    expect(cost({ side: 'buy', order_type: 'market', qty: 100 }, hello, 100_100)).toEqual({
      notional: 10_010_000,
      fee: 3_000,
    });
  });

  it('charges an IOC the taker fee, on either side', () => {
    expect(cost({ side: 'buy', order_type: 'ioc', qty: 100, price: 100_100 }, hello, 100_100)).toEqual({
      notional: 10_010_000,
      fee: 3_000,
    });
    expect(cost({ side: 'sell', order_type: 'ioc', qty: 50, price: 100_000 }, hello, 100_000)).toEqual({
      notional: 5_000_000,
      fee: 1_500,
    });
  });

  it('gives a post-only sell the maker rebate, priced at the price asked rather than the order', () => {
    expect(cost({ side: 'sell', order_type: 'post_only', qty: 50, price: 100_000 }, hello, 100_000)).toEqual({
      notional: 5_000_000,
      fee: -1_000,
    });
    expect(cost({ side: 'sell', order_type: 'market', qty: 50 }, hello, 99_900)).toEqual({
      notional: 4_995_000,
      fee: 1_500,
    });
  });
});
