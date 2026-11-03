# RTES Architecture

This describes the code as it is today. Known defects are listed in [KNOWN_ISSUES.md](KNOWN_ISSUES.md) and
planned changes in [ROADMAP.md](ROADMAP.md).

## Threads
1. **Gateway acceptor** (`TcpGateway::acceptor_loop`): accepts TCP connections on port 8888.
2. **Gateway worker** (`TcpGateway::worker_loop`): polls client sockets, decodes messages, runs a quick symbol and
   quantity check, takes an `Order` from the pool and pushes it to the risk queue.
3. **Risk** (`RiskManager::run`): one thread; full pre-trade checks (size, price collar, per-client rate,
   duplicate id, notional), then routes to the symbol's engine.
4. **Matching engine** (`MatchingEngine::run`): one thread per symbol; the only writer of that symbol's book.
5. **Market data** (`UdpPublisher::worker_loop`): drains market-data events and sends UDP multicast.

## Data flow
```
TCP client → gateway worker ─SPSC→ risk ─SPSC (per symbol)→ matching engine ─MPMC→ UDP publisher → multicast
                 │
                 └── ORDER_ACK back to the client (sent when the order is queued, before risk; see KNOWN_ISSUES W1)
```
No execution reports flow back to clients yet (KNOWN_ISSUES W5).

## Queues
- `SPSCQueue<T>` (`include/rtes/spsc_queue.hpp`): bounded ring, head and tail on separate cache lines.
- `MPMCQueue<T>` (`include/rtes/mpmc_queue.hpp`): bounded ring with per-cell sequence numbers; all engines push
  market-data events into one instance.

## Memory
- `OrderPool` (`include/rtes/memory_pool.hpp`): fixed-size pool of `Order` objects (`order_pool_size` in config,
  1M by default) with a CAS-based free list. Safe only with a single acquiring thread (KNOWN_ISSUES P1).
- Orders are returned to the pool on fill and cancel, and by risk on reject. Market orders that find no
  liquidity are not returned (KNOWN_ISSUES B3).
- `Order` and `Trade` still contain a `std::string` symbol, so the hot path allocates (KNOWN_ISSUES B6).

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
- **TCP order entry** (port 8888): `NEW_ORDER`, `CANCEL_ORDER` in; `ORDER_ACK` out.
- **UDP market data** (239.0.0.1:9999): BBO updates and trades, with a per-publisher sequence number.
  No depth messages yet.

## Configuration
`configs/config.json`, parsed by `src/config.cpp`. Loaded but not yet used:
- `tick_size`, `lot_size`
- `metrics_port`
- `enable_cpu_pinning`
- `persistence.*`
