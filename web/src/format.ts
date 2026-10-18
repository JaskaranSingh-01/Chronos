const newYork = { timeZone: 'America/New_York', hourCycle: 'h23' } as const;
const fields = new Intl.DateTimeFormat('en-US', {
  ...newYork,
  year: 'numeric',
  month: 'numeric',
  day: 'numeric',
  hour: 'numeric',
  minute: 'numeric',
  second: 'numeric',
});

// New York's UTC offset in seconds at `epochSeconds`; a session day never spans a DST change.
export function etOffset(epochSeconds: number): number {
  const at = Math.floor(epochSeconds);
  const f = Object.fromEntries(fields.formatToParts(at * 1_000).map((p) => [p.type, Number(p.value)]));
  return Date.UTC(f.year, f.month - 1, f.day, f.hour, f.minute, f.second) / 1_000 - at;
}
