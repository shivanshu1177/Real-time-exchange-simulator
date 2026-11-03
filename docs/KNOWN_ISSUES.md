# Known Issues

Defects that are known and not yet fixed. Each entry is removed by the change that fixes it.
Line numbers refer to the current tree. The milestone column refers to [ROADMAP.md](ROADMAP.md).

## Order entry (TCP gateway)

| # | Issue | Where | Milestone |
|---|---|---|---|
| W1 | **ACK before risk.** The gateway replies `ACCEPTED` as soon as the order is queued. Risk runs later in another thread and drops rejected orders silently: the client is never told. | `src/tcp_gateway.cpp:313-319`, `src/risk_manager.cpp:59-146` | M1 |
| W2 | Every pre-queue reject (unknown symbol, zero quantity) says `INVALID_SYMBOL`. | `src/tcp_gateway.cpp:293-294` | M1 |
| W3 | **Cancel is a no-op.** The gateway answers `CANCEL_ACCEPTED` without cancelling anything; the order can still trade. | `src/tcp_gateway.cpp:322-325` | M1 |
| W4 | The engine ignores the client id on cancel, so ownership is not checked. | `src/matching_engine.cpp:81-82` | M1 |
| W5 | **No fill reports.** `send_trade_report` exists but is never called; clients never learn about their fills. | `src/tcp_gateway.cpp:342` | M1 |
| W6 | The client's order id is used as the book's global order id: two clients using the same id overwrite each other in the book's lookup table. | `src/tcp_gateway.cpp:304`, `src/order_book.cpp:17` | M1 |
| W7 | No modify/amend message. | `include/rtes/protocol.hpp` | M1 |
| W8 | **Identity comes from the payload.** `client_id` is read from each message; any connection can act as any client. There is no logon. | `src/tcp_gateway.cpp:291,305` | M1 |
| W9 | **Partial TCP reads desync the stream.** A message split across two `recv` calls is discarded. Writes busy-spin on `EAGAIN`. | `src/tcp_gateway.cpp:21-43,61-85`, `:53` | M1 |
| W10 | The checksum is computed by senders but never verified. | `include/rtes/protocol.hpp:101` | M1 |
| W11 | Malformed frames (unknown type, wrong length) are silently ignored and the connection stays open. | `src/tcp_gateway.cpp:274-287` | M1 |
| W12 | Resting orders survive their client's disconnect (no cancel-on-disconnect). | `src/tcp_gateway.cpp:264-272` | M1 |
| W13 | The poll loop allocates two `std::vector`s per iteration. | `src/tcp_gateway.cpp:206,220` | M1 |

## Matching

| # | Issue | Where | Milestone |
|---|---|---|---|
| B1 | BBO quantities are always published as 0. | `src/matching_engine.cpp:106,108` | M1 |
| B2 | Level `total_quantity` is wrong: partial fills of a resting order do not reduce it, and an order that partially filled before resting adds its remaining quantity but subtracts its original quantity when it completes (unsigned underflow). | `src/order_book.cpp:82,111,177,181` | M1 |
| B3 | A market order on an empty book is accepted, never filled or cancelled, and its pool slot leaks. | `src/order_book.cpp:21-22,170-172` | M1 |
| B4 | No self-trade prevention: a client can trade with itself. | `src/order_book.cpp:62-124` | M1 |
| B5 | Tick and lot size are loaded from config and never enforced. | `src/config.cpp:187-188` | M1 |
| B6 | The book has a mutex and a `std::function` callback despite the single-writer design; cancel is O(n). `Order`/`Trade` hold a `std::string` symbol (heap allocation on the hot path). | `include/rtes/order_book.hpp:23,45`, `src/order_book.cpp:148,159`, `include/rtes/types.hpp:27,42` | M1 |

## Risk

| # | Issue | Where | Milestone |
|---|---|---|---|
| R1 | The collar reference price is hard-coded to 150.00 for every symbol. | `src/risk_manager.cpp:19` | M1 |
| R2 | Notional exposure is added and never released on fill or cancel, so every client eventually hits the limit. | `src/risk_manager.cpp:143` | M1 |
| R3 | The per-client active order id set grows forever. | `src/risk_manager.cpp:133` | M1 |
| R4 | Every reject builds and logs a `std::string` line (millions of lines under load). | `src/risk_manager.cpp:84-140` | M1 |

## Memory

| # | Issue | Where | Milestone |
|---|---|---|---|
| P1 | The pool's lock-free free list has no ABA protection. It is only safe with one acquiring thread, which is the case today, but the type does not say so. | `include/rtes/memory_pool.hpp:32-40` | M1 |

## Missing features that older docs claimed

| # | Issue | Milestone |
|---|---|---|
| F1 | No HTTP metrics or health endpoint: nothing listens on `metrics_port` (8080). | M2 |
| F2 | No L2 depth market data; only BBO and trades. | M2 |
| F3 | No published benchmark results. Latency/throughput figures in earlier docs were not produced by any tool in this repo and were removed. | M4 |
