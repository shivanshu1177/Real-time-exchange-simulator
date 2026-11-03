# Known Issues

Defects that are known and not yet fixed. Each entry is removed by the change that fixes it.
Line numbers refer to the current tree. The milestone column refers to [ROADMAP.md](ROADMAP.md).

## Order entry (TCP gateway)

| # | Issue | Where | Milestone |
|---|---|---|---|
| W14 | No authentication: `LOGON.api_key` is not checked, so a client can log on as any `client_id` (and share that client's risk limits). Out of scope for a simulator; documented in API.md. | `src/gateway_core.cpp` (`on_logon`) | out of scope |

## Missing features that older docs claimed

| # | Issue | Milestone |
|---|---|---|
| F1 | No HTTP metrics or health endpoint: nothing listens on `metrics_port` (8080). | M2 |
| F2 | No L2 depth market data; only BBO and trades. | M2 |
| F3 | No wire-to-wire benchmark yet: only in-process order book percentiles (`bench/results/`). Latency/throughput figures in earlier docs were not produced by any tool in this repo and were removed. | M4 |
