# Real-Time Trading Exchange Simulator (RTES)

[![CI](https://github.com/shivanshu1177/Real-time-exchange-simulator/actions/workflows/ci.yml/badge.svg)](https://github.com/shivanshu1177/Real-time-exchange-simulator/actions/workflows/ci.yml)

A C++20 exchange simulator: a TCP binary order gateway, pre-trade risk, one matching-engine thread per symbol
(price-time priority) and UDP multicast market data.

## Status
This is a work in progress. What exists today, with tests:
- **Builds and tests:** GCC 14 and Clang 18 with warnings as errors, plus ASan/UBSan and TSan builds, all run in CI.
- **Order entry:** TCP on port 8888, fixed-size binary messages ([API](docs/API.md)).
  - Logon binds the session's identity; malformed frames get a session reject and are closed.
  - Frames split across TCP reads are reassembled.
  - A session's resting orders are cancelled when it disconnects.
  - New, cancel and modify.
  - Pre-trade risk is decided before the first reply.
  - Rejects carry a specific reason.
  - Execution reports go to both sides of every fill.
  - Order ids are assigned by the exchange, and ownership is enforced per session.
- **Risk:** synchronous on the gateway thread.
  - Tick, lot and size checks.
  - A price collar that follows the last trade.
  - Per-client open notional and open-order limits, released by fills and cancels.
  - A per-session token-bucket rate limit.
  - Reject logging summarised once per second.
- **Matching:** one writer thread per symbol, price-time priority. Each engine owns its order pool, so no
  memory is shared across threads.
  - Limit orders, and market orders that behave as immediate-or-cancel.
  - Self-trade prevention, and tick/lot checks.
  - O(1) cancel.
  - Checked against a reference book by a differential test (100 seeds × 100,000 random operations).
- **Market data:** UDP multicast on 239.0.0.1:9999, BBO (with quantities) and trades.

Not working yet, and tracked in [KNOWN_ISSUES](docs/KNOWN_ISSUES.md):
- No authentication (the logon's API key is not checked).

Planned work is in the [ROADMAP](docs/ROADMAP.md): depth, metrics, determinism, perps, a market-making venue,
and measured benchmarks. No performance figures are claimed until benchmark results are committed to the repo.

## Architecture
- **TCP gateway** (port 8888): one thread; binary protocol, synchronous pre-trade risk, execution reports
  back to clients
- **Matching engine**: one single-writer thread per symbol, price-time priority
- **Market data**: UDP multicast (239.0.0.1:9999), BBO and trades

See [ARCHITECTURE](docs/ARCHITECTURE.md).

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