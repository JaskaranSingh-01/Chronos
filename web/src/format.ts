const newYork = { timeZone: 'America/New_York', hourCycle: 'h23' } as const;
const wallClock = new Intl.DateTimeFormat('en-US', {
  ...newYork,
  hour: '2-digit',
  minute: '2-digit',
  second: '2-digit',
  fractionalSecondDigits: 3,
});
const fields = new Intl.DateTimeFormat('en-US', {
  ...newYork,
  year: 'numeric',
  month: 'numeric',
  day: 'numeric',
  hour: 'numeric',
  minute: 'numeric',
  second: 'numeric',
});
// signDisplay 'negative' keeps a tiny loss from showing as '-$0.00'.
const usd = new Intl.NumberFormat('en-US', { style: 'currency', currency: 'USD', signDisplay: 'negative' });
const usdSigned = new Intl.NumberFormat('en-US', { style: 'currency', currency: 'USD', signDisplay: 'exceptZero' });

// Ticks at `digits` decimals as dollars: cents when the price is whole cents, else every decimal.
export function px(ticks: number, digits: number): string {
  const cents = ticks % 10 ** Math.max(digits - 2, 0) === 0;
  return (ticks / 10 ** digits).toFixed(cents ? Math.min(digits, 2) : digits);
}

// Money at `digits` decimals as dollars; `signed` for profit and loss.
export const money = (value: number, digits: number) => usd.format(value / 10 ** digits);
export const signed = (value: number, digits: number) => usdSigned.format(value / 10 ** digits);

// A share count with its sign shown: '+100', '-100', '0'; and its class, buy for a long and sell for a short.
export const shares = (n: number) => (n > 0 ? `+${n}` : String(n));
export const tone = (n: number) => (n > 0 ? 'buy' : n < 0 ? 'sell' : '');

// An exchange time (epoch ns) as New York wall-clock time to the millisecond.
export const clock = (ns: number) => wallClock.format(ns / 1e6);

// New York's UTC offset in seconds at `epochSeconds`; a session day never spans a DST change.
export function etOffset(epochSeconds: number): number {
  const at = Math.floor(epochSeconds);
  const f = Object.fromEntries(fields.formatToParts(at * 1_000).map((p) => [p.type, Number(p.value)]));
  return Date.UTC(f.year, f.month - 1, f.day, f.hour, f.minute, f.second) / 1_000 - at;
}

// The epoch ns of New York wall-clock time `hhmmss` (HH:MM or HH:MM:SS) on the session day of `ns`.
export function seekTs(ns: number, hhmmss: string): number {
  const [hours, minutes, seconds = 0] = hhmmss.split(':').map(Number);
  const now = Math.floor(ns / 1e9);
  const midnight = now - ((now + etOffset(now)) % 86_400);
  return (midnight + hours * 3_600 + minutes * 60 + seconds) * 1e9; // whole seconds x 1e9 are exact in a double
}
