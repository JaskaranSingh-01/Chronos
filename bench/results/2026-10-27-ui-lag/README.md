# The page's time, before and after (2026-10-27)

`web/e2e/lag.spec.ts` on AAPL's 2019-12-30, the default market maker for the Lab windows, Chromium headless at
1440 × 860, on the Apple M4 of [`../../RESULTS.md`](../../RESULTS.md):

```bash
CHRONOS_LAG_FILE=../data/itch/12302019.NASDAQ_ITCH50.bin CHRONOS_LAG_OUT=<file> npx playwright test lag
```

| File | The page |
|---|---|
| `1-before.json` | the store told every listener of every WebSocket message |
| `2-notify-once-a-frame.json` | listeners told once an animation frame, the symbol list mounted whole |
| `3-dense-pro.json` | the Dense Pro layout, our fills drawn as one marker per candle and side |

Each window: the WebSocket messages and kilobytes a second, the time the socket's handler took (total, its longest and
its share of the window), the long tasks, the frames a second, the worst frame gap and the frames over 50 ms, and the
`/api` fetches with their size and time. [docs/ui.md](../../../docs/ui.md#measuring-the-page) has the summary.
