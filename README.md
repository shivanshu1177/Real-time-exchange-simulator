# Real-Time Trading Exchange Simulator (RTES)

A high-performance, low-latency trading exchange simulator built with modern C++20/23.

## Performance Targets
- **Throughput**: ≥100,000 orders/sec (single host)
- **Latency**: avg ≤10μs, p99 ≤100μs, p999 ≤500μs
- **Deterministic**: Zero data races, allocation-free hot path

## Architecture
- **TCP Order Gateway** (port 8888): Binary protocol with robust framing
- **Risk Manager**: Pre-trade validation (size, price, credit limits)
- **Matching Engine**: Per-symbol single-writer, price-time priority
- **Market Data**: UDP multicast (239.0.0.1:9999) BBO/trades/depth
- **Metrics**: Prometheus endpoint (port 8080)

## Quick Start

```bash
# Build
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Run tests
ctest --output-on-failure

# Start exchange
./trading_exchange ../configs/config.json

# Run client simulator
./client_simulator --strategy market_maker --symbol AAPL

# Monitor metrics
curl -s localhost:8080/metrics | head
```

## Repository Structure
```
├── include/           # Public headers
├── src/              # Implementation
├── tests/            # Unit & integration tests
├── tools/            # Load generators, utilities
├── docs/             # Architecture & performance guides
├── configs/          # Configuration files
├── docker/           # Container definitions
└── .github/workflows/ # CI/CD
```

## Requirements
- C++20 compiler. Verified: GCC 14 and Clang 18 on Ubuntu 24.04
- CMake 3.20+ and Ninja
- Linux (recommended for performance)

Build presets: `debug`, `release` (portable `-O3`), `asan` (address + undefined behaviour sanitizers), `tsan`.
```bash
cmake --preset release && cmake --build --preset release -j && ctest --preset release
```
Options: `-DRTES_NATIVE=ON` adds `-march=native` (binaries then only run on the build CPU); `-DRTES_WERROR=ON` turns warnings into errors.

## License
MIT License - see LICENSE file