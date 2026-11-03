#pragma once

#include <cstdint>
#include <cstring>

namespace rtes {

enum class MessageType : uint32_t {
    NEW_ORDER    = 1,
    CANCEL_ORDER = 2,
    ORDER_ACK    = 3,
    TRADE_REPORT = 4,
    BBO_UPDATE   = 5,
    HEARTBEAT    = 6
};

// All wire structs are packed — no padding between fields
#pragma pack(push, 1)

struct MessageHeader {
    uint32_t type;       // MessageType
    uint32_t length;     // total message length including header
    uint64_t sequence;   // monotonic sequence per session
    uint64_t timestamp;  // nanoseconds since epoch
    uint32_t checksum;   // XOR checksum of bytes after this field
};
// 28 bytes

struct NewOrderMessage {
    MessageHeader header;
    uint64_t client_order_id;  // client-assigned order id
    uint64_t client_id;        // authenticated client identifier
    char     symbol[8];        // null-padded, not necessarily null-terminated
    uint8_t  side;             // 0=BUY, 1=SELL
    uint8_t  order_type;       // 0=MARKET, 1=LIMIT
    uint8_t  _pad[2];
    uint64_t price;            // fixed-point: actual_price * 10000
    uint32_t quantity;
};

struct CancelOrderMessage {
    MessageHeader header;
    uint64_t order_id;
    uint64_t client_id;
};

struct OrderAckMessage {
    MessageHeader header;
    uint64_t order_id;
    uint8_t  status;    // 0=accepted, 1=rejected
    uint8_t  _pad[3];
    char     reason[32];
};

struct TradeReportMessage {
    MessageHeader header;
    uint64_t trade_id;
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    char     symbol[8];
    uint64_t price;
    uint32_t quantity;
    uint32_t _pad;
};

#pragma pack(pop)

// Wire sizes are part of the protocol (see docs/API.md); a change here is a protocol change.
static_assert(sizeof(MessageHeader)      == 28, "MessageHeader wire size");
static_assert(sizeof(NewOrderMessage)    == 68, "NewOrderMessage wire size");
static_assert(sizeof(CancelOrderMessage) == 44, "CancelOrderMessage wire size");
static_assert(sizeof(OrderAckMessage)    == 72, "OrderAckMessage wire size");
static_assert(sizeof(TradeReportMessage) == 76, "TradeReportMessage wire size");

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

inline bool validate_header(const MessageHeader& h) noexcept {
    if (h.length < sizeof(MessageHeader)) return false;
    auto t = static_cast<MessageType>(h.type);
    return t == MessageType::NEW_ORDER    ||
           t == MessageType::CANCEL_ORDER ||
           t == MessageType::ORDER_ACK    ||
           t == MessageType::TRADE_REPORT ||
           t == MessageType::BBO_UPDATE   ||
           t == MessageType::HEARTBEAT;
}

} // namespace rtes
