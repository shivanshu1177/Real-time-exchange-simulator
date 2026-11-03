# Known Issues

Defects that are known and not yet fixed. Each entry is removed by the change that fixes it.
Line numbers refer to the current tree. The milestone column refers to [ROADMAP.md](ROADMAP.md).

## Order entry (TCP gateway)

| # | Issue | Where | Milestone |
|---|---|---|---|
| W14 | No authentication: `LOGON.api_key` is not checked, so a client can log on as any `client_id` (and share that client's risk limits). Out of scope for a simulator; documented in API.md. | `src/gateway_core.cpp` (`on_logon`) | out of scope |

## Risk

| # | Issue | Where | Milestone |
|---|---|---|---|
| R1 | The collar reference price is hard-coded to 150.00 for every symbol. | `src/risk_manager.cpp:15` | M1 |
| R2 | Notional exposure is added and never released on fill or cancel, so every client eventually hits the limit. | `src/risk_manager.cpp:85` | M1 |
| R4 | Every reject builds and logs a `std::string` line, now on the gateway thread (millions of lines under load). | `src/risk_manager.cpp:34-82` | M1 |

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
