#!/usr/bin/env python3
"""Write a day file holding only some symbols of a NASDAQ TotalView-ITCH 5.0 day (Python 3 standard library only).

  scripts/make-sample.py DAY OUT SYMBOL...

OUT keeps, in file order, every frame the store gives every symbol (each System Event, each frame no stock locate
routes) and each SYMBOL's Stock Directory entry and every frame on its locate from then on: the rule of
StoreBuilder::add in src/store/store.cpp. So each SYMBOL's store segment of OUT is byte-identical to the full day's,
and its replays and backtests are the same. The repository's sample/ was made with:

  scripts/make-sample.py data/itch/12302019.NASDAQ_ITCH50.bin sample/12302019.NASDAQ_ITCH50.bin AAPL AMZN NVDA TSLA
  gzip -9n sample/12302019.NASDAQ_ITCH50.bin
"""
import mmap
import sys

MARKET_EVENTS = b"AFECXDUP"
STOCK_DIRECTORY_PAYLOAD = 38


def main(day_path, out_path, symbols):
    wanted = {symbol.encode() for symbol in symbols}
    locates = set()
    found = set()
    with open(day_path, "rb") as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as day, \
            open(out_path, "wb") as out:
        at, end = 0, len(day)
        while at < end:
            size = (day[at] << 8 | day[at + 1]) + 2 if at + 1 < end else 1
            if size < 3 or at + size > end:  # not a whole frame: everyone's, and the rest of the file
                out.write(day[at:])
                break
            kind = day[at + 2]
            payload = size - 3
            if kind == ord("S"):
                keep = True
            elif kind == ord("R"):
                symbol = day[at + 13:at + 21].rstrip(b" ")
                keep = payload != STOCK_DIRECTORY_PAYLOAD or symbol in wanted
                if payload == STOCK_DIRECTORY_PAYLOAD and symbol in wanted:
                    locates.add(day[at + 3] << 8 | day[at + 4])
                    found.add(symbol)
            elif payload < 2:
                keep = kind in MARKET_EVENTS or kind == ord("H")
            else:
                keep = (day[at + 3] << 8 | day[at + 4]) in locates
            if keep:
                out.write(day[at:at + size])
            at += size
    missing = wanted - found
    if missing:
        sys.exit("no Stock Directory entry for " + ", ".join(sorted(s.decode() for s in missing)))


if __name__ == "__main__":
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2], sys.argv[3:])
