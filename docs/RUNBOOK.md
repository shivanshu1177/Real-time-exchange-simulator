# RTES Runbook

How to build, run and check RTES as it exists today. There is no install target, no service unit and no HTTP
health endpoint yet (see [ROADMAP.md](ROADMAP.md)).

## Build
```bash
sudo apt-get install -y g++-14 cmake ninja-build      # Ubuntu 24.04; clang-18 also works
cmake --preset release && cmake --build --preset release -j
ctest --preset release
```
Binaries are in `build/release/`.

## Run
```bash
./build/release/trading_exchange configs/config.json
```
The process logs to stdout (INFO/WARN) and stderr (ERROR) and stops cleanly on Ctrl+C or SIGTERM.

| Port | Protocol | Purpose |
|---|---|---|
| 8888 | TCP | order entry ([API.md](API.md)) |
| 239.0.0.1:9999 | UDP multicast | market data |

`metrics_port` (8080) is in the config but nothing listens on it.

## Check it works
```bash
# Listen to market data
./build/release/udp_receiver --multicast 239.0.0.1 --port 9999

# Send one order (logs on as client 999, prints every report for 300 ms)
./build/release/tcp_client --host localhost --port 8888 --order "BUY,AAPL,100,150.00,LIMIT"

# Or run a simple strategy client for 10 seconds
./build/release/client_simulator --strategy market_maker --symbol AAPL --duration 10
```
Multicast needs a route for 239.0.0.0/8 on the receiving host (`ip route show | grep 239`). On a single
machine without one, add it on loopback: `sudo ip route add 239.0.0.0/8 dev lo`.

## Debugging
```bash
# Memory and undefined-behaviour errors
cmake --preset asan && cmake --build --preset asan -j && ctest --preset asan
# Data races
cmake --preset tsan && cmake --build --preset tsan -j && ctest --preset tsan
# Profiling
perf record -g ./build/release/trading_exchange configs/config.json && perf report
```
Sanitizer binaries are in `build/asan/` and `build/tsan/`, and can run the exchange the same way.

## Host tuning (optional, general Linux)
```bash
sudo sysctl -w net.core.rmem_max=268435456 net.core.wmem_max=268435456
```
CPU pinning (`performance.enable_cpu_pinning`) is in the config but not implemented.
