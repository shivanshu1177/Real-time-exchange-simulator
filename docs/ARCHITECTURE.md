# RTES Architecture

This describes the code as it is today. Known defects are listed in [KNOWN_ISSUES.md](KNOWN_ISSUES.md) and
planned changes in [ROADMAP.md](ROADMAP.md).

## Threads
1. **Gateway** (`TcpGateway::worker_loop`): one thread. Accepts connections on port 8888, reads bytes into each
   connection's receive buffer, validates complete frames and hands them to `GatewayCore`. The core handles
   logon, validates requests, runs pre-trade risk synchronously, assigns the exchange order id and routes the
   request (by value) to the symbol's engine. The same thread drains every engine's event queue, queues execution reports
   in each connection's send buffer and flushes them (waiting for `POLLOUT` when the socket is full).
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

## Transport (`include/rtes/tcp_gateway.hpp`)
- Each `ClientConnection` owns a 64 KiB receive buffer and a 256 KiB send buffer, allocated once at accept.
  `recv` appends; complete frames are taken from the front, so any split of the byte stream parses the same.
- `check_inbound_frame` (`include/rtes/protocol.hpp`) checks type and length from the header alone, then the
  checksum. A bad frame gets `SESSION_REJECT` and the connection is closed after the reject is flushed.
- The `pollfd` array and the close list are reserved once; the poll loop does not allocate.
- On disconnect the core cancels the session's resting orders (configurable) before forgetting the session.

## Queues
- `SPSCQueue<T>` (`include/rtes/spsc_queue.hpp`): bounded ring, head and tail on separate cache lines.
- `MPMCQueue<T>` (`include/rtes/mpmc_queue.hpp`): bounded ring with per-cell sequence numbers; all engines push
  market-data events into one instance.

## Memory
- Each matching engine owns its order pool (`order_pool_size` orders per symbol). Only the engine thread
  acquires and releases, so the pool needs no atomics. Orders cross from the gateway by value
  (`OrderRequest`), never as pointers. See [design/memory.md](design/memory.md).
- Memory for orders, queues and id tables is allocated at start-up. Per order there is no allocation, except
  a `std::map` node when an order opens a new price level.

## Risk (`include/rtes/risk_manager.hpp`)
- Runs on the gateway thread. It is consulted before any reply, and fed by the engines' reports.
- Price collar: ± `price_collar_pct` (integer basis points) around the symbol's last trade price, or around
  `ref_price` from the config until the first trade.
- Per client:
  - open notional (exact fixed-point integer) and open order count, reserved when an order is accepted;
  - released by fills, cancels and rejects, and adjusted by modifies.
  - Clients with nothing open are forgotten, so the table stays bounded.
- Per session: a token bucket rate limit (`max_orders_per_second`, burst = one second's worth).
- Rejects are counted by reason. At most one summary log line per second, never one line per reject.
- A closed session's state is kept until its last order finishes, so cancel-on-disconnect still releases risk.

## Order book (`include/rtes/order_book.hpp`)
```cpp
std::map<Price, PriceLevel, std::greater<Price>> bids_;   // best bid first
std::map<Price, PriceLevel>                      asks_;   // best ask first
FlatMap<Order*>                                  lookup_; // resting orders by id, fixed capacity
// PriceLevel: price, total_quantity, order_count, head/tail of an intrusive FIFO of Order
```
- Single writer, no locks. Matching walks the best level from its head (price-time priority) and fills at
  the resting order's price.
- Each `Order` carries `prev`/`next` links, so a cancel is one hash lookup plus an O(1) unlink.
- `std::map` allocates a node only when a new price level appears, never per order. The id lookup is
  allocated once, sized by `order_pool_size`.
- Market orders are immediate-or-cancel. Self-trade prevention is per symbol (`cancel_taker`, `cancel_maker`,
  `cancel_both`).
- The book reports fills and its own cancels (self-trade, no liquidity) through a `BookListener`, implemented
  by the engine. Each callback runs before the order is returned to the pool.
- Correctness is checked by a differential test against a naive reference book
  (`tests/reference_book.hpp`, `tests/test_book_differential.cpp`): 100 seeds × 100,000 random operations,
  comparing every fill, cancel and depth level after every operation.

## Network protocols
Exact message layouts are in [API.md](API.md).
- **TCP order entry** (port 8888): `LOGON`, `NEW_ORDER`, `CANCEL_ORDER`, `MODIFY_ORDER`, `HEARTBEAT` in;
  `LOGON_ACK`, `EXEC_REPORT`, `CANCEL_REJECT`, `SESSION_REJECT` out.
- **UDP market data** (239.0.0.1:9999): BBO updates and trades, with a per-publisher sequence number.
  No depth messages yet.

## Configuration
`configs/config.json`, parsed by `src/config.cpp`. Loaded but not yet used:
- `metrics_port`
- `enable_cpu_pinning`
- `persistence.*`
