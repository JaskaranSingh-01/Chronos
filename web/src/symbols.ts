// The symbols `typed` matches, ignoring case: the symbol itself first, then those it starts, then those it is inside,
// each group with plain tickers before those with a suffix (MSFT before the preferred MS-A), alphabetical within; at
// most `limit` shown, and how many matched. Nothing typed matches all, in the server's order.
export function matches(symbols: readonly string[], typed: string, limit = 50): { shown: string[]; total: number } {
  const q = typed.trim().toUpperCase();
  if (q === '') return { shown: symbols.slice(0, limit), total: symbols.length };
  const exact: string[] = [];
  const starts: string[] = [];
  const inside: string[] = [];
  for (const s of symbols) {
    const u = s.toUpperCase();
    if (u === q) exact.push(s);
    else if (u.startsWith(q)) starts.push(s);
    else if (u.includes(q)) inside.push(s);
  }
  const suffixed = (s: string) => Number(!/^[A-Z]+$/i.test(s));
  const rank = (a: string, b: string) => suffixed(a) - suffixed(b) || (a < b ? -1 : 1);
  const all = [...exact, ...starts.sort(rank), ...inside.sort(rank)];
  return { shown: all.slice(0, limit), total: all.length };
}
