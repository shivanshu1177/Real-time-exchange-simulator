# RTES Performance

## Results
None published yet. Results will live in `bench/results/` with the hardware, kernel, compiler, flags and git
commit that produced them (added by the benchmark milestone, M4 in [ROADMAP.md](ROADMAP.md)).
Until then this repo makes no latency or throughput claims.

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

## Existing tools and their limits
- `bench_matching`: in-process order book loop. It prints a mean time per order only.
- `bench_exchange --symbols AAPL,MSFT --orders N`: drives the TCP gateway. `--orders` only sets the run duration
  (N/1000 seconds). Its "latency" figure is run time divided by orders sent (1/throughput), not measured per order. With the default risk config most orders hit the rate limit,
  so it mostly measures the reject path.
- `bench_memory_pool`: pool acquire/release loop.

## Host tuning (general Linux guidance, not yet applied by RTES)
```bash
# Larger socket buffers
sudo sysctl -w net.core.rmem_max=268435456 net.core.wmem_max=268435456
# Fixed CPU frequency
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
```
`performance.enable_cpu_pinning` exists in `configs/config.json` but is not implemented yet: no thread is pinned.
