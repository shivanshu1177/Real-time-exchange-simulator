# RTES: a small exchange, built to be correct first

[![CI](https://github.com/shivanshu1177/Real-time-exchange-simulator/actions/workflows/ci.yml/badge.svg)](https://github.com/shivanshu1177/Real-time-exchange-simulator/actions/workflows/ci.yml)

RTES is an exchange simulator written in C++20. Clients connect over TCP, log on, and send orders, cancels and
modifies. A matching engine per symbol fills orders in price-time priority. Every fill comes back to both sides as
an execution report, and market data goes out over UDP multicast.

I built it to learn how a real trading venue behaves on the wire, and to practise the kind of engineering trading
firms care about: correct order handling, simple threading, and tests that prove things rather than assert them.

## Why this project looks the way it does

The first version of this repo looked impressive and was wrong. It acknowledged orders before checking risk, then
dropped them silently. Cancel did nothing, and clients never heard about their fills. A message split across two
TCP reads broke the connection, and the README quoted latency numbers that no tool in the repo produced.

So I rebuilt it in small, reviewable steps. Each fix landed with a test that would fail without it. The docs now
say only what the code does, and the bugs that are still open are listed in
[KNOWN_ISSUES](docs/KNOWN_ISSUES.md) instead of being hidden.

## What it does

- **Order entry over TCP** (port 8888, fixed-size binary messages, [protocol reference](docs/API.md)):
  - logon that binds the client's identity to the connection;
  - new, cancel and modify orders;
  - execution reports for every state change (new, partial fill, fill, cancelled, replaced, rejected);
  - a specific reason on every reject.
- **Pre-trade risk**, decided before the client hears anything:
  - tick, lot and size checks;
  - a price collar that follows the last trade;
  - per-client limits on open notional and open orders, released again by fills and cancels;
  - a per-connection rate limit.
- **Matching**, one thread per symbol:
  - price-time priority, with limit orders and immediate-or-cancel market orders;
  - self-trade prevention (configurable per symbol);
  - modify keeps the order's place in the queue only when it reduces quantity at the same price.
- **Robust connections:**
  - messages split across any number of TCP reads are reassembled;
  - a malformed message closes the session with an explicit reason;
  - a client's resting orders are cancelled when it disconnects.
- **Market data:** best bid and offer (with quantities) and trades over UDP multicast (239.0.0.1:9999).

## How an order flows

```
             TCP                         by value (SPSC queue)
  client ───────────► gateway thread ─────────────────────────► matching engine (one per symbol)
    ▲                 · checks the frame                        · fills in price-time order
    │                 · validates + pre-trade risk              · owns its book and its memory
    │                 · assigns the exchange order id                    │
    │                                                                    │ fills, cancels
    └──── execution reports ◄──── gateway thread ◄──── events (SPSC queue)
                                                                         │
                                                  trades, BBO ─► UDP publisher ─► multicast
```

1. The gateway thread reads bytes into the connection's buffer and checks each complete message: known type,
   exact length, valid checksum.
2. It validates the order and runs the risk checks. A rejected order gets one `REJECTED` report and never reaches
   the engine.
3. An accepted order is copied into the engine's queue. No pointers cross threads.
4. The engine reports `NEW` first, then matches. Each fill produces a report for both the buyer and the seller.
5. The gateway turns those events into messages for the right client, and updates the client's risk exposure.

More detail in [ARCHITECTURE](docs/ARCHITECTURE.md).

## How I know it works

- **85 automated tests** (87 in debug builds), run in CI on GCC 14 and Clang 18 (debug and release) and on
  macOS with Apple Clang.
- **A differential test for the order book.** The real book and a deliberately naive reference book
  ([`tests/reference_book.hpp`](tests/reference_book.hpp)) get the same random orders, cancels and modifies:
  100 seeds × 100,000 operations. After every operation, every fill, cancel and price level must match. They do,
  with zero differences. When I planted bugs on purpose (a broken priority rule, broken self-trade prevention),
  this test caught each one within a few hundred operations. See
  [`tests/test_book_differential.cpp`](tests/test_book_differential.cpp).
- **Order lifecycle tests** that drive the gateway and the engines in-process
  ([`tests/test_order_lifecycle.cpp`](tests/test_order_lifecycle.cpp)): every reject reason, a cancel racing a
  fill, a cancel from the wrong client, modify priority rules, risk exposure being released.
- **Real TCP tests** ([`tests/test_framing_sessions.cpp`](tests/test_framing_sessions.cpp)): a message split at
  every possible byte, bad checksums, wrong lengths, unknown types, a second logon, cancel-on-disconnect.
- **Sanitizers in CI:** AddressSanitizer with UndefinedBehaviorSanitizer, and ThreadSanitizer. ThreadSanitizer
  also ran clean against the live exchange with real clients connected.

## Design decisions worth asking me about

- **Risk runs on the gateway thread, not its own thread.** The first reply to a client then already carries the
  risk decision, and all risk state has exactly one owner. The original design acknowledged first and checked
  later, which is how orders got silently dropped.
- **Each engine owns its memory; orders cross threads by value.** The old shared lock-free pool had an ABA race
  that only stayed hidden because one thread happened to allocate. Removing the need for cross-thread allocation
  was simpler and safer than writing a cleverer lock-free stack. See [design/memory.md](docs/design/memory.md).
- **Orders at a price level are an intrusive linked list,** so a cancel from the middle of the queue is O(1), and
  nothing is allocated per order.
- **Numbers only with their context.**
  - What I publish today: order-book latency percentiles from `bench_book`
    ([`bench/results/`](bench/results/)), measured on my own machine.
  - Each results file records the CPU, OS, compiler, flags and commit, and states that it is in-process and
    single-threaded.
  - Wire-to-wire latency needs an open-loop load generator to avoid coordinated omission. I'll measure it
    that way before quoting it. See [PERFORMANCE](docs/PERFORMANCE.md).

## Try it

You need CMake 3.20+, Ninja and a C++20 compiler:
- **Linux or WSL2:** GCC 14 or Clang 18.
- **macOS:** the Xcode command-line tools plus `brew install cmake ninja`.

CI builds and tests all three: GCC 14 and Clang 18 on Ubuntu, and Apple Clang on macOS.

```bash
cmake --preset release && cmake --build --preset release -j && ctest --preset release
```

Start the exchange:

```bash
./build/release/trading_exchange configs/config.json
```

In a second terminal, log on as client 1, rest a buy order and keep listening:

```bash
./build/release/tcp_client --client-id 1 --order "BUY,AAPL,10,150.00,LIMIT" --listen-ms 5000
```

In a third, log on as client 2 and sell into it:

```bash
./build/release/tcp_client --client-id 2 --order "SELL,AAPL,4,150.00,LIMIT"
```

The seller sees `NEW` then `FILL`. The buyer sees `NEW` then `PARTIAL_FILL` with 6 left and 4 filled.
If both use the same client id, self-trade prevention cancels the incoming sell instead (`CANCELED`, reason 19),
which is the point of it.
The [RUNBOOK](docs/RUNBOOK.md) shows the market-data receiver and the strategy clients.

Other build presets: `debug`, `asan` (address and undefined-behaviour sanitizers), `tsan` (thread sanitizer).

To measure the order book on your machine (results go to `bench/results/` with the hardware recorded):

```bash
./scripts/bench.sh
```

## What's not done yet

- **No authentication.** Logon binds an identity but does not check the API key.
- **No order book depth on the market-data feed,** and **no metrics endpoint** yet.
- **No wire-to-wire latency or throughput figures yet,** only in-process order book percentiles (see above).

The order I'm tackling these in is in the [ROADMAP](docs/ROADMAP.md): an end-to-end test in CI, depth, a
deterministic journal with replay, fuzzing, crypto-perpetual order types, a market-making research venue, and
then honest benchmarks.

## Repository layout

```
include/rtes/   headers: protocol, gateway, matching engine, order book, risk, memory pool, queues
src/            implementation
tests/          unit, lifecycle, TCP and differential tests
tools/          tcp_client, client_simulator, udp_receiver, bench_book
scripts/        bench.sh: run the benchmark and record the machine it ran on
bench/results/  benchmark results, one file per run
configs/        exchange configuration (symbols, risk limits)
docs/           protocol, architecture, runbook, roadmap, known issues
```

## License

MIT, see [LICENSE](LICENSE).
