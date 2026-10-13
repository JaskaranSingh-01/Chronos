#!/usr/bin/env python3
"""Summarise bench/run.sh results (Python 3 standard library only).

  bench/summary.py DIR              one protocol run: every replay, medians, benchmarks
  bench/summary.py DIR DIR ...      medians side by side, one column per DIR
  bench/summary.py --profile FILE   where a `sample` profile of a replay spent its time
"""
import glob
import json
import os
import re
import statistics
import sys


def runs(directory, kind):
    """The replay JSONs of `kind` ("full" or "slice"), in run order."""
    paths = sorted(glob.glob(os.path.join(directory, kind + "-*.json")),
                   key=lambda p: int(re.search(r"-(\d+)\.json$", p).group(1)))
    return [json.load(open(p)) for p in paths]


# (column, function of one replay JSON, format)
REPLAY_COLUMNS = [
    ("wall s", lambda r: r["time"]["wall_s"], "{:.2f}"),
    ("CPU s", lambda r: r["time"]["cpu_s"], "{:.2f}"),
    ("major faults", lambda r: r["memory"]["major_faults"], "{:,.0f}"),
    ("events/s", lambda r: r["time"]["events_per_s"], "{:,.0f}"),
    ("ns/event", lambda r: r["time"]["ns_per_event"], "{:.1f}"),
    ("p50", lambda r: r["latency"]["ns_per_event"]["p50"], "{:,.0f}"),
    ("p90", lambda r: r["latency"]["ns_per_event"]["p90"], "{:,.0f}"),
    ("p99", lambda r: r["latency"]["ns_per_event"]["p99"], "{:,.0f}"),
    ("p99.9", lambda r: r["latency"]["ns_per_event"]["p99_9"], "{:,.0f}"),
    ("allocations", lambda r: r["memory"]["allocations"], "{:,.0f}"),
    ("alloc/event", lambda r: r["memory"]["allocations_per_event"], "{:.3f}"),
    ("peak RSS MB", lambda r: r["memory"]["peak_rss_bytes"] / 1e6, "{:,.0f}"),
    ("peak footprint MB", lambda r: r["memory"]["peak_footprint_bytes"] / 1e6, "{:,.0f}"),
]


def median_of(replays, get):
    return statistics.median(get(r) for r in replays)


def table(header, rows):
    lines = ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)]
    lines += ["| " + " | ".join(str(c) for c in row) + " |" for row in rows]
    return "\n".join(lines)


def outcome(replays):
    """Digest, rejections and books left, whether every run agrees, and which runs failed."""
    digests = {r["result"]["digest"] for r in replays}
    first = replays[0]["result"]
    agree = "the same in every run" if len(digests) == 1 else "DIFFERENT: " + ", ".join(sorted(digests))
    failed = [str(i) for i, r in enumerate(replays, 1) if not r["result"]["succeeded"]]
    status = "every run succeeded" if not failed else "runs " + ", ".join(failed) + " exited non-zero (see the logs)"
    return (f"{first['events']:,} events, {first['rejected']:,} rejected, digest {first['digest']} ({agree}), "
            f"{first['books']:,} books, {first['books_not_empty']:,} not empty at the end, "
            f"peak live orders {first['peak_live_orders']:,}; {status}")


def replay_section(title, replays, cold_first):
    if not replays:
        return f"## {title}\n\nno runs\n"
    rows = []
    for i, r in enumerate(replays, 1):
        name = f"{i}" + (" (cold)" if cold_first and i == 1 else "")
        rows.append([name] + [fmt.format(get(r)) for _, get, fmt in REPLAY_COLUMNS])
    rows.append(["**median**"] + [fmt.format(median_of(replays, get)) for _, get, fmt in REPLAY_COLUMNS])
    rows.append(["min-max"] + ["{}-{}".format(fmt.format(min(get(r) for r in replays)),
                                              fmt.format(max(get(r) for r in replays)))
                              for _, get, fmt in REPLAY_COLUMNS])
    batch = replays[0]["latency"]
    note = (f"ns/event p50-p99.9: percentiles of the mean ns/event of each batch of {batch['batch_events']} "
            f"events (timer tick {batch['timer_tick_ns']} ns), not of single events.")
    return (f"## {title}\n\n{outcome(replays)}\n\n"
            + table(["run"] + [c for c, _, _ in REPLAY_COLUMNS], rows) + f"\n\n{note}\n")


def benchmarks(directory):
    """{name: (median ns per operation, coefficient of variation or None)} from simulator_bench's JSON."""
    path = os.path.join(directory, "bench.json")
    if not os.path.exists(path):
        return {}
    data = json.load(open(path))
    def aggregate(name):
        return {b["run_name"]: b["real_time"] for b in data["benchmarks"] if b.get("aggregate_name") == name}
    medians, cvs = aggregate("median"), aggregate("cv")
    if not medians:  # a single repetition has no aggregates
        medians = {b["run_name"]: b["real_time"] for b in data["benchmarks"]}
    return {name: (ns, cvs.get(name)) for name, ns in medians.items()}


def ns_cv(value):
    ns, cv = value
    return f"{ns:,.1f}" + (f" ({100 * cv:.0f} %)" if cv is not None else "")


def short(name):
    return name.replace("/iterations:4096", "").replace("/iterations:8", "").replace("/manual_time", "")


def one(directory):
    full = runs(directory, "full")
    slice_ = runs(directory, "slice")
    any_run = (full or slice_ or [None])[0]
    out = [f"# {os.path.basename(os.path.normpath(directory))}\n"]
    if any_run:
        b, m = any_run["build"], any_run["machine"]
        out.append(f"Build {b['commit'][:12]}{' (dirty)' if b['dirty'] else ''}, {b['type']}, {b['compiler']}; "
                   f"{m['cpu']}, {m['cores']} cores, {m['memory_bytes'] / 2**30:.0f} GiB, {m['os']}; "
                   f"{any_run['date']}.\n")
    for title, replays, cold in (("Full day, --all", full, True), ("Slice, --all", slice_, False)):
        if replays:
            i = replays[0]["input"]
            title += f": {os.path.basename(i['path'])}, {i['bytes']:,} bytes, fingerprint {i['fingerprint']}"
        out.append(replay_section(title, replays, cold))
    bench = benchmarks(directory)
    if bench:
        rows = [[short(name), ns_cv(value)] for name, value in bench.items()]
        out.append("## simulator_bench: median ns per operation (per message for ItchSource)\n\n"
                   "Coefficient of variation over the repetitions in brackets.\n\n"
                   + table(["benchmark", "ns (cv)"], rows) + "\n")
    profile = os.path.join(directory, "profile-top.txt")
    if os.path.exists(profile):
        out.append("## Profile\n\n```\n" + open(profile).read().rstrip() + "\n```\n")
    return "\n".join(out)


def compare(directories):
    names = [os.path.basename(os.path.normpath(d)) for d in directories]
    rows = []
    for kind, label in (("full", "full day"), ("slice", "slice")):
        replays = [runs(d, kind) for d in directories]
        rows.append([f"**{label}** digest"] + [(r[0]["result"]["digest"] if r else "-") for r in replays])
        rows.append([f"{label} rejected"] + [(f"{r[0]['result']['rejected']:,}" if r else "-") for r in replays])
        rows.append([f"{label} runs"] + [str(len(r)) for r in replays])
        for column, get, fmt in REPLAY_COLUMNS:
            rows.append([f"{label} {column}"] + [(fmt.format(median_of(r, get)) if r else "-") for r in replays])
    bench = [benchmarks(d) for d in directories]
    for name in dict.fromkeys(n for b in bench for n in b):
        rows.append([short(name) + " ns (cv)"] + [(ns_cv(b[name]) if name in b else "-") for b in bench])
    return ("Medians per protocol run (replay rows: median over its runs; benchmarks: median of repetitions, "
            "coefficient of variation in brackets).\n\n" + table(["metric"] + names, rows))


def profile(path, top=15):
    """Self and total samples per function inside MarketReplay::run, from `sample` output.

    System libraries are merged into one line each, e.g. [libsystem_malloc.dylib]."""
    lines = open(path).read().split("\n")
    nodes = []  # (depth, samples, frame)
    for line in lines[lines.index("Call graph:") + 1:]:
        m = re.match(r"^([ +!:|]*)(\d+) (.*)$", line)
        if not m:
            break
        frame, _, lib = m.group(3).partition("  (in ")
        lib = lib.split(")")[0]
        if lib and lib != "simulator":
            frame = f"[{lib}]"
        nodes.append((len(m.group(1)) // 2, int(m.group(2)), frame))

    self_time, total_time = {}, {}
    replay_samples = 0
    i = 0
    while i < len(nodes):
        depth, samples, frame = nodes[i]
        if not frame.startswith("simulator::MarketReplay::run("):
            i += 1
            continue
        replay_samples += samples
        end = i + 1
        while end < len(nodes) and nodes[end][0] > depth:
            end += 1
        stack = []  # (depth, frame) of the ancestors of the node being read
        for j in range(i, end):
            d, n, f = nodes[j]
            children = 0
            k = j + 1
            while k < end and nodes[k][0] > d:
                if nodes[k][0] == d + 1:
                    children += nodes[k][1]
                k += 1
            self_time[f] = self_time.get(f, 0) + n - children
            while stack and stack[-1][0] >= d:
                stack.pop()
            if f not in (a for _, a in stack):  # count a recursive frame once
                total_time[f] = total_time.get(f, 0) + n
            stack.append((d, f))
        i = end

    def rows(counts):
        ranked = sorted(counts.items(), key=lambda kv: -kv[1])[:top]
        return "\n".join(f"  {100 * n / replay_samples:5.1f}%  {n:>8,}  {f}" for f, n in ranked)

    return (f"{replay_samples:,} samples (1 ms apart) inside MarketReplay::run, from {os.path.basename(path)}\n\n"
            f"Self (top of stack), share of replay time:\n{rows(self_time)}\n\n"
            f"Total (self + callees), share of replay time:\n{rows(total_time)}\n")


if __name__ == "__main__":
    args = sys.argv[1:]
    if len(args) == 2 and args[0] == "--profile":
        print(profile(args[1]))
    elif len(args) == 1:
        print(one(args[0]))
    elif len(args) > 1:
        print(compare(args))
    else:
        sys.exit(__doc__)
