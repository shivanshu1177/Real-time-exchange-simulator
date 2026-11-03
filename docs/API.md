# RTES Wire Protocol

Generated from `include/rtes/protocol.hpp` and `include/rtes/market_data.hpp`.
Sizes are enforced by `static_assert` in those headers, so this table and the code cannot drift apart silently.

Conventions:
- All structs are packed (`#pragma pack(1)`), no padding except the explicit `_pad` fields.
- Integers are little-endian (host order on x86-64/arm64; there is no byte swapping).
- Prices are fixed-point `uint64_t`: `actual_price * 10000` (150.25 → 1502500).
- `symbol` is 8 bytes, NUL-padded, not necessarily NUL-terminated.
- Known protocol defects are tracked in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Message types (`MessageType`, `uint32_t`)

| Value | Name | Direction | Struct |
|---|---|---|---|
| 1 | `NEW_ORDER` | client → exchange (TCP) | `NewOrderMessage` |
| 2 | `CANCEL_ORDER` | client → exchange (TCP) | `CancelOrderMessage` |
| 3 | `ORDER_ACK` | exchange → client (TCP) | `OrderAckMessage` |
| 4 | `TRADE_REPORT` | exchange → client (TCP), and trades on UDP | `TradeReportMessage` (TCP), `TradeUpdateMessage` (UDP) |
| 5 | `BBO_UPDATE` | exchange → UDP multicast | `BboUpdateMessage` |
| 6 | `HEARTBEAT` | defined, not sent or handled | header only |

## Header: `MessageHeader` (28 bytes)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 4 | `type` | `uint32_t` | `MessageType` |
| 4 | 4 | `length` | `uint32_t` | total message length including header |
| 8 | 8 | `sequence` | `uint64_t` | sender's sequence number |
| 16 | 8 | `timestamp` | `uint64_t` | sender's `steady_clock` time in ns (not wall clock) |
| 24 | 4 | `checksum` | `uint32_t` | XOR-fold of the body bytes (after the header), see `compute_checksum` |

## TCP order entry (port 8888)

### `NewOrderMessage` (68 bytes, type 1)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 28 | `header` | `MessageHeader` | |
| 28 | 8 | `client_order_id` | `uint64_t` | client-assigned |
| 36 | 8 | `client_id` | `uint64_t` | taken from the payload, not authenticated |
| 44 | 8 | `symbol` | `char[8]` | |
| 52 | 1 | `side` | `uint8_t` | 0 = BUY, 1 = SELL |
| 53 | 1 | `order_type` | `uint8_t` | 0 = MARKET, 1 = LIMIT |
| 54 | 2 | `_pad` | `uint8_t[2]` | |
| 56 | 8 | `price` | `uint64_t` | fixed-point |
| 64 | 4 | `quantity` | `uint32_t` | |

### `CancelOrderMessage` (44 bytes, type 2)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 28 | `header` | `MessageHeader` | |
| 28 | 8 | `order_id` | `uint64_t` | |
| 36 | 8 | `client_id` | `uint64_t` | |

### `OrderAckMessage` (72 bytes, type 3)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 28 | `header` | `MessageHeader` | |
| 28 | 8 | `order_id` | `uint64_t` | echoes the client order id |
| 36 | 1 | `status` | `uint8_t` | 0 = accepted, 1 = rejected |
| 37 | 3 | `_pad` | `uint8_t[3]` | |
| 40 | 32 | `reason` | `char[32]` | free text, NUL-terminated |

### `TradeReportMessage` (76 bytes, type 4)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 28 | `header` | `MessageHeader` | |
| 28 | 8 | `trade_id` | `uint64_t` | |
| 36 | 8 | `buy_order_id` | `uint64_t` | |
| 44 | 8 | `sell_order_id` | `uint64_t` | |
| 52 | 8 | `symbol` | `char[8]` | |
| 60 | 8 | `price` | `uint64_t` | fixed-point |
| 68 | 4 | `quantity` | `uint32_t` | |
| 72 | 4 | `_pad` | `uint32_t` | |

The gateway defines this message but never sends it today (see KNOWN_ISSUES).

## UDP market data (multicast 239.0.0.1:9999)

### `BboUpdateMessage` (60 bytes, type 5)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 28 | `header` | `MessageHeader` | `sequence` is per publisher |
| 28 | 8 | `symbol` | `char[8]` | |
| 36 | 8 | `bid_price` | `uint64_t` | 0 if no bid |
| 44 | 4 | `bid_quantity` | `uint32_t` | currently always 0 (KNOWN_ISSUES) |
| 48 | 8 | `ask_price` | `uint64_t` | 0 if no ask |
| 56 | 4 | `ask_quantity` | `uint32_t` | currently always 0 (KNOWN_ISSUES) |

### `TradeUpdateMessage` (76 bytes, sent with type 4)

Same layout as `TradeReportMessage`.

| Offset | Size | Field | Type |
|---|---|---|---|
| 0 | 28 | `header` | `MessageHeader` |
| 28 | 8 | `trade_id` | `uint64_t` |
| 36 | 8 | `buy_order_id` | `uint64_t` |
| 44 | 8 | `sell_order_id` | `uint64_t` |
| 52 | 8 | `symbol` | `char[8]` |
| 60 | 8 | `price` | `uint64_t` |
| 68 | 4 | `quantity` | `uint32_t` |
| 72 | 4 | `_pad` | `uint32_t` |

## Planned (not implemented)

- Execution reports, modify and cancel-reject messages ([ROADMAP](ROADMAP.md) M1).
- Logon and session reject (M1).
- L2 depth snapshots and incremental updates (M2).
- HTTP `/metrics` and `/health` (M2). Nothing listens on the configured `metrics_port` today.
