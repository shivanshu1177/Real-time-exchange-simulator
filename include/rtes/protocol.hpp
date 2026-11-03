#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rtes {

// Bumped on every incompatible wire change.
// v2: execution reports, modify, cancel reject, exchange-assigned order ids;
//     ORDER_ACK and the TCP trade report removed.
// v3: LOGON / LOGON_ACK / SESSION_REJECT; identity bound at logon; frames validated.
inline constexpr uint32_t PROTOCOL_VERSION = 3;

enum class MessageType : uint32_t {
    NEW_ORDER     = 1,   // client -> exchange
    CANCEL_ORDER  = 2,   // client -> exchange
    // 3 was ORDER_ACK (v1), replaced by EXEC_REPORT NEW / REJECTED
    TRADE_UPDATE  = 4,   // exchange -> UDP market data
    BBO_UPDATE    = 5,   // exchange -> UDP market data
    HEARTBEAT     = 6,   // reserved
    MODIFY_ORDER  = 7,   // client -> exchange
    EXEC_REPORT   = 8,   // exchange -> client
    CANCEL_REJECT = 9,   // exchange -> client
    LOGON         = 10,  // client -> exchange, must be the first message
    LOGON_ACK     = 11,  // exchange -> client
    SESSION_REJECT = 12, // exchange -> client, sent just before the exchange closes the connection
};

enum class ExecType : uint8_t {
    NEW          = 0,
    PARTIAL_FILL = 1,
    FILL         = 2,
    CANCELED     = 3,
    REPLACED     = 4,
    REJECTED     = 5,
};

// Values are part of the protocol: append only, never renumber.
enum class RejectReason : uint8_t {
    NONE               = 0,
    UNKNOWN_SYMBOL     = 1,
    INVALID_QUANTITY   = 2,
    INVALID_PRICE      = 3,
    INVALID_SIDE       = 4,
    INVALID_ORDER_TYPE = 5,
    ORDER_SIZE_LIMIT   = 6,
    PRICE_COLLAR       = 7,
    RATE_LIMIT         = 8,
    NOTIONAL_LIMIT     = 9,
    DUPLICATE_ORDER_ID = 10,
    UNKNOWN_ORDER      = 11,  // no live order with that id for this session
    NOT_OWNER          = 12,  // order belongs to another session
    TOO_LATE           = 13,  // order filled or cancelled before the request arrived
    SYSTEM_BUSY        = 14,  // internal queue or pool full; nothing was changed
    NOT_LOGGED_IN      = 15,  // order request before a successful LOGON
    OFF_TICK           = 16,  // price is not a multiple of the symbol's tick size
    OFF_LOT            = 17,  // quantity is not a multiple of the symbol's lot size
    NO_LIQUIDITY       = 18,  // CANCELED: unfilled remainder of a market order
    SELF_TRADE         = 19,  // CANCELED: self-trade prevention
    OPEN_ORDER_LIMIT   = 20,  // client already has risk.max_open_orders_per_client live orders
};

// Why the exchange is closing a session. Values are part of the protocol.
enum class SessionRejectReason : uint8_t {
    NONE                = 0,
    BAD_CHECKSUM        = 1,
    BAD_LENGTH          = 2,  // length field does not match the message type
    UNKNOWN_TYPE        = 3,  // unknown type, or a type clients may not send
    DUPLICATE_LOGON     = 4,
    UNSUPPORTED_VERSION = 5,  // LOGON.protocol_version != PROTOCOL_VERSION
};

// Which request a CANCEL_REJECT answers.
enum class CancelRejectResponseTo : uint8_t {
    CANCEL = 0,
    MODIFY = 1,
};

// All wire structs are packed — no padding between fields
#pragma pack(push, 1)

struct MessageHeader {
    uint32_t type;       // MessageType
    uint32_t length;     // total message length including header
    uint64_t sequence;   // monotonic sequence per sender
    uint64_t timestamp;  // sender's steady_clock time in ns
    uint32_t checksum;   // XOR-fold of the body bytes (after the header)
};

struct NewOrderMessage {
    MessageHeader header;
    uint64_t client_order_id;  // client-assigned, unique among the session's live orders
    uint64_t client_id;
    char     symbol[8];        // null-padded, not necessarily null-terminated
    uint8_t  side;             // 0=BUY, 1=SELL
    uint8_t  order_type;       // 0=MARKET, 1=LIMIT
    uint8_t  _pad[2];
    uint64_t price;            // fixed-point: actual_price * 10000
    uint32_t quantity;
};

// Identifies the order by exchange order_id if non-zero, otherwise by the
// client_order_id of one of this session's live orders.
struct CancelOrderMessage {
    MessageHeader header;
    uint64_t order_id;
    uint64_t client_order_id;
};

// Same addressing as CancelOrderMessage. new_quantity is the new total order
// quantity (including what is already filled). A price change or a quantity
// increase loses time priority; a quantity decrease keeps it.
struct ModifyOrderMessage {
    MessageHeader header;
    uint64_t order_id;
    uint64_t client_order_id;
    uint64_t new_price;
    uint32_t new_quantity;
    uint32_t _pad;
};

struct ExecReportMessage {
    MessageHeader header;
    uint64_t order_id;         // exchange-assigned; 0 for REJECTED
    uint64_t client_order_id;
    uint64_t trade_id;         // non-zero for PARTIAL_FILL / FILL
    char     symbol[8];
    uint64_t price;            // order limit price (0 for market orders)
    uint64_t last_px;          // fill price (fills only)
    uint32_t last_qty;         // fill quantity (fills only)
    uint32_t leaves_qty;       // quantity still open
    uint32_t cum_qty;          // quantity filled so far
    uint8_t  exec_type;        // ExecType
    uint8_t  side;             // 0=BUY, 1=SELL
    uint8_t  reject_reason;    // RejectReason (REJECTED only)
    uint8_t  _pad;
};

struct CancelRejectMessage {
    MessageHeader header;
    uint64_t order_id;
    uint64_t client_order_id;
    uint8_t  reject_reason;    // RejectReason
    uint8_t  response_to;      // CancelRejectResponseTo
    uint8_t  _pad[2];
};

struct LogonMessage {
    MessageHeader header;
    uint32_t protocol_version;  // must equal PROTOCOL_VERSION
    uint32_t _pad;
    uint64_t client_id;         // identity for this session (used by risk)
    char     api_key[16];       // reserved: not verified, authentication is out of scope
};

struct LogonAckMessage {
    MessageHeader header;
    uint32_t protocol_version;
    uint32_t session_id;
};

struct SessionRejectMessage {
    MessageHeader header;
    uint8_t  reason;            // SessionRejectReason
    uint8_t  _pad[3];
    uint32_t detail;            // offending type or length, for diagnostics
};

#pragma pack(pop)

// Wire sizes are part of the protocol (see docs/API.md); a change here is a protocol change.
static_assert(sizeof(MessageHeader)       == 28, "MessageHeader wire size");
static_assert(sizeof(NewOrderMessage)     == 68, "NewOrderMessage wire size");
static_assert(sizeof(CancelOrderMessage)  == 44, "CancelOrderMessage wire size");
static_assert(sizeof(ModifyOrderMessage)  == 60, "ModifyOrderMessage wire size");
static_assert(sizeof(ExecReportMessage)   == 92, "ExecReportMessage wire size");
static_assert(sizeof(CancelRejectMessage) == 48, "CancelRejectMessage wire size");
static_assert(sizeof(LogonMessage)        == 60, "LogonMessage wire size");
static_assert(sizeof(LogonAckMessage)     == 36, "LogonAckMessage wire size");
static_assert(sizeof(SessionRejectMessage) == 36, "SessionRejectMessage wire size");

// Simple XOR-fold checksum over message body (bytes after the checksum field)
inline uint32_t compute_checksum(const void* data, size_t len) noexcept {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t csum = 0;
    for (size_t i = 0; i < len; ++i) {
        csum ^= static_cast<uint32_t>(bytes[i]) << ((i % 4) * 8);
    }
    return csum;
}

// Fill header and compute checksum in-place
template<typename Msg>
inline void seal_message(Msg& msg, MessageType type, uint64_t sequence, uint64_t timestamp_ns) noexcept {
    msg.header.type      = static_cast<uint32_t>(type);
    msg.header.length    = sizeof(Msg);
    msg.header.sequence  = sequence;
    msg.header.timestamp = timestamp_ns;
    msg.header.checksum  = 0;
    // Checksum over the message body (bytes after the fixed header)
    if (sizeof(Msg) > sizeof(MessageHeader)) {
        msg.header.checksum = compute_checksum(
            reinterpret_cast<const uint8_t*>(&msg) + sizeof(MessageHeader),
            sizeof(Msg) - sizeof(MessageHeader));
    }
}

// Exact length of each message type a client may send; 0 = not allowed.
inline constexpr uint32_t inbound_length(uint32_t type) noexcept {
    switch (static_cast<MessageType>(type)) {
        case MessageType::NEW_ORDER:    return sizeof(NewOrderMessage);
        case MessageType::CANCEL_ORDER: return sizeof(CancelOrderMessage);
        case MessageType::MODIFY_ORDER: return sizeof(ModifyOrderMessage);
        case MessageType::LOGON:        return sizeof(LogonMessage);
        case MessageType::HEARTBEAT:    return sizeof(MessageHeader);
        default:                        return 0;
    }
}

struct FrameCheck {
    enum Status : uint8_t { NEED_MORE, OK, INVALID };
    Status              status{NEED_MORE};
    uint32_t            length{0};    // frame length when OK
    SessionRejectReason reason{SessionRejectReason::NONE};
    uint32_t            detail{0};    // offending type or length when INVALID
};

// Check the frame at the start of a client's receive buffer. Type and length
// are judged as soon as the header is complete, so a bogus length is rejected
// without waiting for (or buffering) its body.
inline FrameCheck check_inbound_frame(const uint8_t* data, size_t avail) noexcept {
    FrameCheck fc;
    if (avail < sizeof(MessageHeader)) return fc;
    MessageHeader h;
    std::memcpy(&h, data, sizeof(h));
    const uint32_t expected = inbound_length(h.type);
    if (expected == 0) {
        fc.status = FrameCheck::INVALID; fc.reason = SessionRejectReason::UNKNOWN_TYPE; fc.detail = h.type;
        return fc;
    }
    if (h.length != expected) {
        fc.status = FrameCheck::INVALID; fc.reason = SessionRejectReason::BAD_LENGTH; fc.detail = h.length;
        return fc;
    }
    if (avail < expected) return fc;   // NEED_MORE
    const uint32_t csum = compute_checksum(data + sizeof(MessageHeader), expected - sizeof(MessageHeader));
    if (csum != h.checksum) {
        fc.status = FrameCheck::INVALID; fc.reason = SessionRejectReason::BAD_CHECKSUM; fc.detail = h.type;
        return fc;
    }
    fc.status = FrameCheck::OK;
    fc.length = expected;
    return fc;
}

} // namespace rtes
