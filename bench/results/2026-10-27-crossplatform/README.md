# One file, two platforms, the same books (2026-10-27)

The first 256 MiB of NASDAQ's 2019-12-30 day (04:00 to 09:30:10 ET, its last message cut), replayed by:

| Log | Build |
|---|---|
| `linux-gcc14-*.log` | the Docker image: Ubuntu 24.04, g++ 14.2, Release, aarch64 |
| `macos-clang17-*.log` | macOS 27 on an Apple M4: Apple clang 17, RelWithDebInfo |

```bash
simulator replay 12302019.NASDAQ_ITCH50.bin --symbol AAPL --date 2019-12-30 --allow-truncated-tail   # *-aapl.log
simulator replay 12302019.NASDAQ_ITCH50.bin --all --date 2019-12-30 --allow-truncated-tail           # *-all.log
```

| | Linux, g++ | macOS, clang |
|---|---|---|
| AAPL: events applied, rejected | 8,418, 0 | 8,418, 0 |
| AAPL: book digest | `7f1e922654415cf3` | `7f1e922654415cf3` |
| every symbol: events applied, rejected | 6,529,870, 0 | 6,529,870, 0 |
| every symbol: combined digest | `b00db289f7c388b2` | `b00db289f7c388b2` |

The slice ends at the open, with 8,888 of its 8,906 books still holding orders, so these digests hash live books:
every level, order and queue position, not only the count of adds that a full day's empty books leave.
