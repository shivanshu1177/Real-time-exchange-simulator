# Real-Time Trading Exchange Simulator (RTES)

[![CI](https://github.com/shivanshu1177/Real-time-exchange-simulator/actions/workflows/ci.yml/badge.svg)](https://github.com/shivanshu1177/Real-time-exchange-simulator/actions/workflows/ci.yml)

A C++20 exchange simulator: a TCP binary order gateway, pre-trade risk, one matching-engine thread per symbol
(price-time priority) and UDP multicast market data.

## Status
This is a work in progress. What exists today:
- **Builds and tests:** GCC 14 and Clang 18 with warnings as errors, plus ASan/UBSan and TSan builds, all run in CI.
  Test coverage is currently 3 order-book unit tests.
- **Order entry:** TCP on port 8888, fixed-size binary messages ([API](docs/API.md)).
- **Matching:** limit and market orders, price-time priority, one writer thread per symbol.
- **Market data:** UDP multicast on 239.0.0.1:9999, BBO (prices only) and trades.

Not working yet, and tracked in [KNOWN_ISSUES](docs/KNOWN_ISSUES.md):
- Orders are acknowledged before the risk check.
- Cancel does nothing.
- Clients receive no fill reports.
- Partial TCP reads are mishandled.
- There is no logon.

Planned work is in the [ROADMAP](docs/ROADMAP.md): depth, metrics, determinism, perps, a market-making venue,
and measured benchmarks. No performance figures are claimed until benchmark results are committed to the repo.

## Architecture
- **TCP Order Gateway** (port 8888): binary protocol
- **Risk Manager**: pre-trade checks (size, price collar, rate, notional) on its own thread
- **Matching Engine**: one single-writer thread per symbol, price-time priority
- **Market Data**: UDP multicast (239.0.0.1:9999), BBO and trades

## Quick Start

```bash
cmake --preset release && cmake --build --preset release -j
ctest --preset release

# Start the exchange (from the repo root)
./build/release/trading_exchange configs/config.json

# In another terminal: a simple market-making client
./build/release/client_simulator --strategy market_maker --symbol AAPL --duration 10
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