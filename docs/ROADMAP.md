# Roadmap

Milestones in order. A milestone is done when its items are implemented **and** covered by tests or by benchmark
output committed in the repo. Open defects are listed in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## M0: Honest baseline
- Builds from a fresh clone on GCC 14 and Clang 18 with warnings as errors; CMake presets incl. ASan/UBSan and TSan.
- One CI workflow that is actually green; CodeQL.
- No build artefacts or non-engineering documents in the repo.
- README and docs state only what is true.

## M1: Correct matching core
- Order lifecycle on the wire: risk decided before ACK, exchange-assigned order ids, real cancel and modify,
  execution reports to both counterparties, specific reject reasons.
- TCP framing: partial reads reassembled, checksum and length validated, malformed input rejected; logon-bound
  identity; cancel-on-disconnect.
- Order book: correct level and BBO quantities, self-trade prevention, tick/lot validation, IOC market orders,
  O(1) cancel; differential test against a reference book.
- Stateful risk (collar from last trade, exposure released on fill/cancel, token-bucket rate limit) and a
  memory pool with explicit single-thread ownership.
- Unit tests per module, a wire-level end-to-end test in CI, coverage.

## M2: Determinism, market data and operations
- L2 depth (snapshots and incremental updates), correct BBO and trades, gap detection.
- Minimal `/metrics` and `/health`.
- Docker and docker-compose that build and run.
- Deterministic sequencer, journal and replay; fuzz targets.

## M3: Crypto-perp semantics and market-making research venue
- PostOnly, IOC/FOK, reduceOnly, positions, mark price, funding.
- Tape replay, latency and queue model, strategy SDK, reference Avellaneda–Stoikov maker, TCA report.

## M4: Performance credibility
- Percentile latency (not means), open-loop load generation, hardware disclosure, results committed under
  `bench/results/`; zero hot-path allocations proven by a test.

## M5: v1.0.0
- Design document, README with diagram, demo, CHANGELOG, release.
