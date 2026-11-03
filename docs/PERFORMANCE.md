# RTES Performance

## Results
Results live in [`bench/results/`](../bench/results/). Each file records the machine, OS, compiler, build flags
and git commit that produced it. Run `./scripts/bench.sh` to produce your own, and
`python3 scripts/plot_bench.py` to redraw the charts in `docs/img/` (shown in the README) from the latest runs.

First published results: Apple M2 (Mac14,2), macOS 26.3, Apple Clang 21, 3 runs at commit `e8e65d6`. Typical
cost per operation: add 39.7 ns, cancel 39.8 ns, match (one fill) 18.7 ns, modify 48.5 ns. p99.99 is at most
209 ns.

What is measured today: **order book operations only** (`bench_book`). It is single-threaded and in process:
add a passive order, cancel, an aggressive order that makes exactly one fill, and a modify that keeps priority.
It runs on a book that stays at 10,000 resting orders over 100 price levels.

What is **not** measured yet: anything wire-to-wire (TCP, the gateway, risk, the queues between threads). That
needs an open-loop load generator and percentiles from send to execution report, and it is planned in the
benchmark milestone (M4 in [ROADMAP.md](ROADMAP.md)). Until then this repo quotes no end-to-end latency or
throughput.

How to read `bench_book` output:
- Each operation is timed on its own and recorded in a histogram with three significant digits
  (`include/rtes/util/histogram.hpp`, tested against exact percentiles). The output gives p50/p90/p99/p99.9/
  p99.99/max, not a mean.
- Per-operation times include one clock read, and are quantised to the clock's step. The tool prints both: on
  Apple Silicon `steady_clock` advances about every 41.7 ns, so values for fast operations are coarse there.
- `batched/op` is total time divided by operations over batches of 1,000. It is not limited by the clock, so it
  is the better estimate of typical cost; the percentiles show the spread and the tail.

## Targets (not yet measured)
- Throughput: 100,000 orders/sec on a single host.
- Latency, wire to wire: p99 ≤ 100 μs, p99.9 ≤ 500 μs.

These are goals. Nothing in the repo shows they are met.

## Methodology (what the benchmarks must do)
- Report percentiles (p50/p90/p99/p99.9/max), never a mean alone.
- Use an open-loop load generator at a fixed offered rate, to avoid coordinated omission.
- Timestamp at send and at receipt of the execution report.
- Report fills and the reject rate, so a run that only measures the reject path is visible.
- Record CPU model, kernel, governor, compiler version, build flags, thread pinning and git sha with every result.
- Use `-DRTES_NATIVE=ON` only for local runs, and say so in the results.

## Tools
- `bench_book`: per-operation order book latency percentiles (above). `./scripts/bench.sh` builds it, runs it
  and saves the results with the machine details.
- `bench_exchange --symbols AAPL,MSFT --orders N`: drives the TCP gateway. `--orders` only sets the run duration
  (N/1000 seconds). Its "latency" figure is run time divided by orders sent (1/throughput), not measured per
  order, and with the default risk config most orders hit the rate limit. Not used for any published number;
  it will be replaced by the open-loop benchmark.
- `bench_memory_pool`: pool acquire/release loop.

## Host tuning (general Linux guidance, not yet applied by RTES)
```bash
# Larger socket buffers
sudo sysctl -w net.core.rmem_max=268435456 net.core.wmem_max=268435456
# Fixed CPU frequency
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```
`performance.enable_cpu_pinning` exists in `configs/config.json` but is not implemented yet: no thread is pinned.
