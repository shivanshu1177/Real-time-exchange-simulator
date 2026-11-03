# RTES Wire Protocol

Generated from `include/rtes/protocol.hpp` and `include/rtes/market_data.hpp`.
Sizes are enforced by `static_assert` in those headers, so this table and the code cannot drift apart silently.

Conventions:
- All structs are packed (`#pragma pack(1)`), no padding except the explicit `_pad` fields.
- Integers are little-endian (host order on x86-64/arm64; there is no byte swapping).
- Prices are fixed-point `uint64_t`: `actual_price * 10000` (150.25 → 1502500).
- `symbol` is 8 bytes, NUL-padded, not necessarily NUL-terminated.
- Known protocol defects are tracked in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

Protocol version: `PROTOCOL_VERSION = 3` in `protocol.hpp`.

## Message types (`MessageType`, `uint32_t`)

| Value | Name | Direction | Struct |
|---|---|---|---|
| 1 | `NEW_ORDER` | client → exchange (TCP) | `NewOrderMessage` |
| 2 | `CANCEL_ORDER` | client → exchange (TCP) | `CancelOrderMessage` |
| 3 | — | retired (`ORDER_ACK` in v1) | |
| 4 | `TRADE_UPDATE` | exchange → UDP multicast | `TradeUpdateMessage` |
| 5 | `BBO_UPDATE` | exchange → UDP multicast | `BboUpdateMessage` |
| 6 | `HEARTBEAT` | client → exchange, accepted and ignored; never sent | header only |
| 7 | `MODIFY_ORDER` | client → exchange (TCP) | `ModifyOrderMessage` |
| 8 | `EXEC_REPORT` | exchange → client (TCP) | `ExecReportMessage` |
| 9 | `CANCEL_REJECT` | exchange → client (TCP) | `CancelRejectMessage` |
| 10 | `LOGON` | client → exchange (TCP) | `LogonMessage` |
| 11 | `LOGON_ACK` | exchange → client (TCP) | `LogonAckMessage` |
| 12 | `SESSION_REJECT` | exchange → client (TCP), then the exchange closes the connection | `SessionRejectMessage` |

## Header: `MessageHeader` (28 bytes)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 4 | `type` | `uint32_t` | `MessageType` |
| 4 | 4 | `length` | `uint32_t` | total message length including header |
| 8 | 8 | `sequence` | `uint64_t` | sender's sequence number (per session for exchange → client) |
| 16 | 8 | `timestamp` | `uint64_t` | sender's `steady_clock` time in ns (not wall clock) |
| 24 | 4 | `checksum` | `uint32_t` | XOR-fold of the body bytes (after the header), see `compute_checksum` |

## Sessions and framing

- One TCP connection is one session. The first message must be `LOGON` with `protocol_version = 3`;
  the exchange answers `LOGON_ACK` with the session id.
- The `client_id` given at logon is the session's identity for risk limits. The `client_id` field of
  `NEW_ORDER` is ignored. Orders, cancels and modifies sent before logon are rejected with `NOT_LOGGED_IN`
  (the session stays open).
- Authentication is out of scope: `api_key` is not checked, so any client can log on as any `client_id`.
- Every inbound frame is checked as soon as its header arrives: the type must be one clients may send, and
  `length` must be exactly that type's size. The checksum is checked once the frame is complete. A violation,
  a second `LOGON`, or an unsupported version gets a `SESSION_REJECT` and the connection is closed.
- Frames may arrive split across any number of TCP reads, or several in one read.
- If a client stops reading and its 256 KiB send buffer fills up, the exchange drops the connection.
- When a session ends, its resting orders are cancelled (`exchange.cancel_on_disconnect`, default `true`).

## Order lifecycle

Every request gets exactly one answer, and the answer already reflects the risk check:

| Request | Answer | Then |
|---|---|---|
| `NEW_ORDER` | `EXEC_REPORT` `NEW`, or `REJECTED` with a `reject_reason` | `PARTIAL_FILL` / `FILL` reports as it trades |
| `CANCEL_ORDER` | `EXEC_REPORT` `CANCELED`, or `CANCEL_REJECT` | |
| `MODIFY_ORDER` | `EXEC_REPORT` `REPLACED`, or `CANCEL_REJECT` (`response_to` = MODIFY) | fills if the new price crosses |

- Both counterparties get an `EXEC_REPORT` for every fill, with the same `trade_id`.
  On `NEW`, fills and `REPLACED`, `leaves_qty + cum_qty` equals the order's total quantity;
  `CANCELED` reports `leaves_qty = 0` and the final `cum_qty`; `REJECTED` reports both as 0.
- `NEW` is always sent before any fill of that order; `REPLACED` before any fill caused by the modify.
- `order_id` is assigned by the exchange. The top 16 bits hold the symbol index + 1, the low 48 bits a per-symbol sequence.
- A session is one TCP connection. `client_order_id` must be unique among that session's live orders;
  it is forgotten (and may be reused) once the order is filled, cancelled or rejected.
  Two sessions may use the same `client_order_id` independently.
- Cancel and modify address an order by `order_id` if non-zero, otherwise by the session's `client_order_id`.
  An order owned by another session is rejected with `NOT_OWNER`.
- Market orders are immediate-or-cancel: whatever does not trade at once is reported as `CANCELED` with
  `reject_reason = NO_LIQUIDITY`.
- Self-trade prevention applies when an order would trade with a resting order of the same `client_id`
  (from `LOGON`). The mode is set per symbol (`stp` in the config): `cancel_taker` (default) cancels the rest of
  the incoming order; `cancel_maker` cancels the resting order and keeps matching; `cancel_both` cancels both.
  Each cancelled order gets `CANCELED` with `reject_reason = SELF_TRADE`.
- Prices must be multiples of the symbol's tick size and quantities multiples of its lot size.
- Modify: `new_quantity` is the new total quantity and must exceed what is already filled.
  Same price with a lower quantity keeps time priority; a price change or a higher quantity loses it.
  Market orders cannot be modified.

### Reject reasons (`RejectReason`, `uint8_t`)

| Value | Name | Meaning |
|---|---|---|
| 0 | `NONE` | |
| 1 | `UNKNOWN_SYMBOL` | symbol not configured |
| 2 | `INVALID_QUANTITY` | zero quantity; or a modify that does not exceed the filled quantity |
| 3 | `INVALID_PRICE` | zero price on a limit order or modify |
| 4 | `INVALID_SIDE` | side not 0 or 1 |
| 5 | `INVALID_ORDER_TYPE` | order type not 0 or 1; or modify of a market order |
| 6 | `ORDER_SIZE_LIMIT` | quantity above `risk.max_order_size` |
| 7 | `PRICE_COLLAR` | limit price outside the collar |
| 8 | `RATE_LIMIT` | client above `risk.max_orders_per_second` |
| 9 | `NOTIONAL_LIMIT` | client above `risk.max_notional_per_client` |
| 10 | `DUPLICATE_ORDER_ID` | `client_order_id` already used by a live order of this session |
| 11 | `UNKNOWN_ORDER` | no live order with that id for this session |
| 12 | `NOT_OWNER` | the order belongs to another session |
| 13 | `TOO_LATE` | the order filled or was cancelled while the request was in flight |
| 14 | `SYSTEM_BUSY` | an internal queue or pool was full; nothing changed |
| 15 | `NOT_LOGGED_IN` | request before a successful `LOGON` |
| 16 | `OFF_TICK` | limit or modify price is not a multiple of the symbol's `tick_size` |
| 17 | `OFF_LOT` | quantity is not a multiple of the symbol's `lot_size` |
| 18 | `NO_LIQUIDITY` | on `CANCELED`: the unfilled part of a market order |
| 19 | `SELF_TRADE` | on `CANCELED`: self-trade prevention |

### Session reject reasons (`SessionRejectReason`, `uint8_t`)

| Value | Name | `detail` |
|---|---|---|
| 1 | `BAD_CHECKSUM` | message type |
| 2 | `BAD_LENGTH` | the `length` received |
| 3 | `UNKNOWN_TYPE` | the `type` received (unknown, or one only the exchange sends) |
| 4 | `DUPLICATE_LOGON` | 0 |
| 5 | `UNSUPPORTED_VERSION` | the version received |

`ExecType`: 0 `NEW`, 1 `PARTIAL_FILL`, 2 `FILL`, 3 `CANCELED`, 4 `REPLACED`, 5 `REJECTED`.

## TCP order entry (port 8888)

### `LogonMessage` (60 bytes, type 10)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 28 | `header` | `MessageHeader` | |
| 28 | 4 | `protocol_version` | `uint32_t` | must be 3 |
| 32 | 4 | `_pad` | `uint32_t` | |
| 36 | 8 | `client_id` | `uint64_t` | session identity for risk |
| 44 | 16 | `api_key` | `char[16]` | reserved, not checked |

### `LogonAckMessage` (36 bytes, type 11)

| Offset | Size | Field | Type |
|---|---|---|---|
| 0 | 28 | `header` | `MessageHeader` |
| 28 | 4 | `protocol_version` | `uint32_t` |
| 32 | 4 | `session_id` | `uint32_t` |

### `SessionRejectMessage` (36 bytes, type 12)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 28 | `header` | `MessageHeader` | |
| 28 | 1 | `reason` | `uint8_t` | `SessionRejectReason` |
| 29 | 3 | `_pad` | `uint8_t[3]` | |
| 32 | 4 | `detail` | `uint32_t` | see the table above |

### `NewOrderMessage` (68 bytes, type 1)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 28 | `header` | `MessageHeader` | |
| 28 | 8 | `client_order_id` | `uint64_t` | client-assigned |
| 36 | 8 | `client_id` | `uint64_t` | ignored (identity comes from `LOGON`) |
| 44 | 8 | `symbol` | `char[8]` | |
| 52 | 1 | `side` | `uint8_t` | 0 = BUY, 1 = SELL |
| 53 | 1 | `order_type` | `uint8_t` | 0 = MARKET, 1 = LIMIT |
| 54 | 2 | `_pad` | `uint8_t[2]` | |
| 56 | 8 | `price` | `uint64_t` | fixed-point; ignored for market orders |
| 64 | 4 | `quantity` | `uint32_t` | |

### `CancelOrderMessage` (44 bytes, type 2)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 28 | `header` | `MessageHeader` | |
| 28 | 8 | `order_id` | `uint64_t` | exchange id, or 0 to use `client_order_id` |
| 36 | 8 | `client_order_id` | `uint64_t` | |

### `ModifyOrderMessage` (60 bytes, type 7)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 28 | `header` | `MessageHeader` | |
| 28 | 8 | `order_id` | `uint64_t` | exchange id, or 0 to use `client_order_id` |
| 36 | 8 | `client_order_id` | `uint64_t` | |
| 44 | 8 | `new_price` | `uint64_t` | fixed-point |
| 52 | 4 | `new_quantity` | `uint32_t` | new total quantity |
| 56 | 4 | `_pad` | `uint32_t` | |

### `ExecReportMessage` (92 bytes, type 8)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 28 | `header` | `MessageHeader` | |
| 28 | 8 | `order_id` | `uint64_t` | 0 for `REJECTED` |
| 36 | 8 | `client_order_id` | `uint64_t` | |
| 44 | 8 | `trade_id` | `uint64_t` | non-zero for fills |
| 52 | 8 | `symbol` | `char[8]` | |
| 60 | 8 | `price` | `uint64_t` | order limit price (new price for `REPLACED`; 0 for market) |
| 68 | 8 | `last_px` | `uint64_t` | fill price (fills only) |
| 76 | 4 | `last_qty` | `uint32_t` | fill quantity (fills only) |
| 80 | 4 | `leaves_qty` | `uint32_t` | quantity still open |
| 84 | 4 | `cum_qty` | `uint32_t` | quantity filled so far |
| 88 | 1 | `exec_type` | `uint8_t` | `ExecType` |
| 89 | 1 | `side` | `uint8_t` | |
| 90 | 1 | `reject_reason` | `uint8_t` | `RejectReason`: why it was `REJECTED`, or why the exchange `CANCELED` it (0 for a requested cancel) |
| 91 | 1 | `_pad` | `uint8_t` | |

### `CancelRejectMessage` (48 bytes, type 9)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 28 | `header` | `MessageHeader` | |
| 28 | 8 | `order_id` | `uint64_t` | as sent in the request |
| 36 | 8 | `client_order_id` | `uint64_t` | as sent in the request |
| 44 | 1 | `reject_reason` | `uint8_t` | `RejectReason` |
| 45 | 1 | `response_to` | `uint8_t` | 0 = CANCEL, 1 = MODIFY |
| 46 | 2 | `_pad` | `uint8_t[2]` | |

## UDP market data (multicast 239.0.0.1:9999)

### `BboUpdateMessage` (60 bytes, type 5)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 28 | `header` | `MessageHeader` | `sequence` is per publisher |
| 28 | 8 | `symbol` | `char[8]` | |
| 36 | 8 | `bid_price` | `uint64_t` | 0 if no bid |
| 44 | 4 | `bid_quantity` | `uint32_t` | total quantity at the best bid |
| 48 | 8 | `ask_price` | `uint64_t` | 0 if no ask |
| 56 | 4 | `ask_quantity` | `uint32_t` | total quantity at the best ask |

### `TradeUpdateMessage` (76 bytes, type 4)

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

- L2 depth snapshots and incremental updates (M2).
- HTTP `/metrics` and `/health` (M2). Nothing listens on the configured `metrics_port` today.
