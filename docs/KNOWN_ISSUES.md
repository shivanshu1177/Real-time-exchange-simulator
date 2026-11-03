# Known Issues

Defects that are known and not yet fixed. Each entry is removed by the change that fixes it.
Line numbers refer to the current tree. The milestone column refers to [ROADMAP.md](ROADMAP.md).

## Order entry (TCP gateway)

| # | Issue | Where | Milestone |
|---|---|---|---|
| W14 | No authentication: `LOGON.api_key` is not checked, so a client can log on as any `client_id` (and share that client's risk limits). Out of scope for a simulator; documented in API.md. | `src/gateway_core.cpp` (`on_logon`) | out of scope |

## Matching

| # | Issue | Where | Milestone |
|---|---|---|---|
| B1 | BBO quantities are always published as 0. | `src/matching_engine.cpp:181,183` | M1 |
| B2 | Level `total_quantity` is wrong: partial fills of a resting order do not reduce it, and an order that partially filled before resting adds its remaining quantity but subtracts its original quantity when it completes (unsigned underflow). | `src/order_book.cpp:122,151,209-222` | M1 |
| B3 | The unfilled part of a market order is never cancelled or reported (the client only sees `NEW` and any fills), and its pool slot leaks. | `src/order_book.cpp:21-22,209-211` | M1 |
| B4 | No self-trade prevention: a client can trade with itself. | `src/order_book.cpp:102` | M1 |
| B5 | Tick and lot size are loaded from config and never enforced. | `src/config.cpp:187-188` | M1 |
| B6 | The book has a mutex and a `std::function` callback despite the single-writer design; cancel is O(n). `Order`/`Trade` hold a `std::string` symbol: a string copy per order and per trade (no heap allocation for symbols of up to 15 characters, thanks to the small-string buffer, but not allocation-free by construction). | `include/rtes/order_book.hpp:25,56`, `src/order_book.cpp:188`, `include/rtes/types.hpp:30,45` | M1 |

## Risk

| # | Issue | Where | Milestone |
|---|---|---|---|
| R1 | The collar reference price is hard-coded to 150.00 for every symbol. | `src/risk_manager.cpp:14` | M1 |
| R2 | Notional exposure is added and never released on fill or cancel, so every client eventually hits the limit. | `src/risk_manager.cpp:78` | M1 |
| R4 | Every reject builds and logs a `std::string` line, now on the gateway thread (millions of lines under load). | `src/risk_manager.cpp:27-75` | M1 |

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
