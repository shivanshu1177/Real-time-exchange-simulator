# RTES Architecture

This describes the code as it is today. Known defects are listed in [KNOWN_ISSUES.md](KNOWN_ISSUES.md) and
planned changes in [ROADMAP.md](ROADMAP.md).

## Threads
1. **Gateway** (`TcpGateway::worker_loop`): one thread. Accepts connections on port 8888, reads requests, and hands
   them to `GatewayCore`, which validates them, runs pre-trade risk synchronously, assigns the exchange order id
   and routes the request to the symbol's engine. The same thread drains every engine's event queue and writes
   execution reports back to clients.
2. **Matching engine** (`MatchingEngine::run`): one thread per symbol; the only writer of that symbol's book.
3. **Market data** (`UdpPublisher::worker_loop`): drains market-data events and sends UDP multicast.

Risk has no thread of its own: running it on the gateway thread means the first reply to a client already
carries the risk decision, and all risk state has a single owner.

## Data flow
```
TCP client ⇄ gateway thread ── OrderRequest (SPSC, per engine) ──→ matching engine ─MPMC→ UDP publisher → multicast
                  ↑                                                     │
                  └──────────── ExecEvent (SPSC, per engine) ←──────────┘
```
- Rejected requests are answered by the gateway directly (`EXEC_REPORT REJECTED` or `CANCEL_REJECT`) and never
  reach an engine.
- Accepted requests are answered by the engine's events: `NEW`, fills, `CANCELED`, `REPLACED`, or a cancel reject
  (unknown order, not the owner, too late). The gateway turns each event into a message for the owning session.
- `GatewayCore` (`include/rtes/gateway_core.hpp`) has no socket code, so the order lifecycle is tested in-process
  (`tests/test_order_lifecycle.cpp`).

## Queues
- `SPSCQueue<T>` (`include/rtes/spsc_queue.hpp`): bounded ring, head and tail on separate cache lines.
- `MPMCQueue<T>` (`include/rtes/mpmc_queue.hpp`): bounded ring with per-cell sequence numbers; all engines push
  market-data events into one instance.

## Memory
- `OrderPool` (`include/rtes/memory_pool.hpp`): fixed-size pool of `Order` objects (`order_pool_size` in config,
  1M by default) with a CAS-based free list. Safe only with a single acquiring thread (KNOWN_ISSUES P1).
- The gateway thread is the only thread that takes orders from the pool; engines return them on fill and cancel. Market orders that find no
  liquidity are not returned (KNOWN_ISSUES B3).
- `Order` and `Trade` still contain a `std::string` symbol (KNOWN_ISSUES B6).

## Order book (`include/rtes/order_book.hpp`)
```cpp
std::map<Price, PriceLevel, std::greater<Price>> bids_;   // best bid first
std::map<Price, PriceLevel>                      asks_;   // best ask first
std::unordered_map<OrderID, Order*>              order_lookup_;
// PriceLevel: price, std::deque<Order*> FIFO, total_quantity
```
Matching walks the best level first (price-time priority). Trades are reported through a `std::function`
callback to the engine, which publishes them. The book also holds a mutex, which the single-writer design
does not need.

## Network protocols
Exact message layouts are in [API.md](API.md).
- **TCP order entry** (port 8888): `NEW_ORDER`, `CANCEL_ORDER`, `MODIFY_ORDER` in; `EXEC_REPORT`, `CANCEL_REJECT` out.
- **UDP market data** (239.0.0.1:9999): BBO updates and trades, with a per-publisher sequence number.
  No depth messages yet.

## Configuration
`configs/config.json`, parsed by `src/config.cpp`. Loaded but not yet used:
- `tick_size`, `lot_size`
- `metrics_port`
- `enable_cpu_pinning`
- `persistence.*`
