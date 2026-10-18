// chronos-serve's messages (server/src/protocol.cpp): prices in ticks, times in epoch ns (a double reads ~256 ns).

export type Side = 'buy' | 'sell';
export type OrderType = 'post_only' | 'ioc' | 'market';
export type SessionState = 'paused' | 'playing' | 'stepping' | 'seeking' | 'ended';
export type TradingState = 'trading' | 'halted' | 'paused' | 'quotation_only';
export type Level = [price: number, quantity: number, orders: number];
export type Trade = [ts: number, price: number, quantity: number, resting: Side];
export type Candle = [start: number, open: number, high: number, low: number, close: number, volume: number];
// A maker fill's rule, (a), (c) or (d), and the ITCH message behind it: E, C, an Add or a Replace.
export type FillRule = 'a_e' | 'a_c' | 'd_e' | 'd_c' | 'c_add' | 'c_replace';

interface Envelope {
  session_id: string;
  generation: number;
}

export interface Hello extends Envelope {
  type: 'hello';
  protocol: number;
  role: 'controller' | 'viewer';
  price_scale: number;
  quantity_scale: number;
  fees: { maker: number; taker: number }; // tick-shares per share; negative is a rebate
  limits: { max_position: number; max_order_quantity: number; max_open_orders: number };
}

export interface MyOrder {
  id: number;
  side: Side;
  price: number;
  remaining: number;
  ahead_quantity: number;
  ahead_orders: number;
}

export interface Account {
  position: number;
  cash: number;
  realised: number;
  unrealised: number;
  fees: number;
  pnl: number;
  max_drawdown: number;
}

export interface Snapshot extends Envelope {
  type: 'snapshot' | 'state';
  full_resync?: boolean; // state only: sent after a gap
  symbol: string;
  snapshot_seq: number;
  exchange_ts: number;
  event_seq: number;
  last_report_seq: number;
  last_command_seq: number;
  state: SessionState;
  speed: number; // 0 = as fast as possible
  progress: number; // the fraction of the file read
  failure: 'none' | 'source_error' | 'out_of_order_timestamp' | 'invariant_violation';
  source_error: string;
  trading_state: TradingState;
  bids: Level[];
  asks: Level[];
  trades: Trade[]; // since the last snapshot
  trades_truncated: boolean;
  candle: Candle | null; // the current 1 s bar
  closed_candles: Candle[];
  candles_truncated: boolean;
  account: Account;
  my_orders: MyOrder[];
  my_orders_truncated: boolean;
  metrics: { events_per_second: number; dropped_snapshots: number; command_queue_depth: number; log_failed: boolean };
}

interface Ring extends Envelope {
  seq: number;
}

export interface Execution extends Ring {
  type: 'report';
  kind: 'execution';
  order_id: number;
  report_type: 'accepted' | 'rejected' | 'fill' | 'cancelled' | 'cancel_rejected' | 'expired';
  side: Side;
  price: number;
  quantity: number;
  leaves: number;
  reject_reason: string;
  liquidity: 'none' | 'maker' | 'taker';
  exchange_ts: number;
  rule?: FillRule; // maker fills only
  ahead_at_entry?: number; // maker fills only: historical shares ahead of the order when it arrived
}

export interface Lifecycle extends Ring {
  type: 'report';
  kind: 'lifecycle';
  step: string;
  reason: string; // 'none' when OK
  command: 'order' | 'cancel' | 'attach' | 'detach';
  command_seq: number;
  client_id: number;
  order_id: number;
  exchange_ts: number;
  market_event_seq: number;
}

export interface GenerationStart extends Ring {
  type: 'report';
  kind: 'generation_start';
  symbol: string;
  strategy: string;
}

export interface GenerationEnd extends Ring {
  type: 'report';
  kind: 'generation_end';
  exchange_ts: number;
  market_event_seq: number;
}

export type GenerationMark = GenerationStart | GenerationEnd;

export interface ControlAck extends Ring {
  type: 'ack';
  control: string;
  control_seq: number;
  state: SessionState;
}

// The command is in the log; its verdict comes as lifecycle reports.
export interface CommandAck extends Envelope {
  type: 'ack';
  command_seq: number;
  persisted: 'flushed' | 'fsynced';
}

export type ServerMessage =
  | Hello
  | Snapshot
  | Execution
  | Lifecycle
  | GenerationMark
  | ControlAck
  | CommandAck
  | (Envelope & { type: 'gap'; from: number; to: number })
  | (Envelope & { type: 'progress'; seek: number })
  | (Envelope & { type: 'error'; code: string; message: string; command_seq?: number; client_id?: number });

export type Control =
  | { action: 'play' | 'pause' | 'restart' | 'cancel_seek' }
  | { action: 'step'; n: number }
  | { action: 'speed'; speed: number }
  | { action: 'seek'; ts: number }
  | { action: 'symbol'; symbol: string };

export interface Order {
  side: Side;
  order_type: OrderType;
  qty: number;
  price?: number; // absent for market: the server bands it 1% around the best opposite price
}

export interface History extends Envelope {
  candles: Candle[];
  trades: Trade[];
}
