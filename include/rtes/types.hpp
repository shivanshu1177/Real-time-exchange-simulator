#pragma once

#include <cstdint>

namespace rtes {

using Price = uint64_t;
using Quantity = uint32_t;
using OrderID = uint64_t;
using TradeID = uint64_t;
using ClientID = uint64_t;
using SessionId = uint32_t;  // one per TCP connection, never reused

enum class Side : uint8_t {
    BUY = 0,
    SELL = 1
};

enum class OrderType : uint8_t {
    MARKET = 0,
    LIMIT = 1
};

// Self-trade prevention: what happens when an incoming order would trade
// with a resting order of the same client.
enum class StpMode : uint8_t {
    CANCEL_TAKER = 0,   // cancel the rest of the incoming order (default)
    CANCEL_MAKER = 1,   // cancel the resting order and keep matching
    CANCEL_BOTH  = 2,
};

struct Order {
    OrderID  order_id{0};          // exchange-assigned, unique across the exchange
    ClientID client_id{0};         // identity bound at logon (used for self-trade prevention)
    SessionId session_id{0};       // owning session
    uint64_t client_order_id{0};   // the owner's id for this order
    Side side{Side::BUY};
    OrderType type{OrderType::LIMIT};
    Price price{0};
    Quantity quantity{0};
    Quantity filled{0};

    // Intrusive FIFO links within the order's price level; owned by the book.
    Order* prev{nullptr};
    Order* next{nullptr};

    bool is_filled() const { return filled >= quantity; }
    Quantity remaining() const { return quantity - filled; }
};

struct Trade {
    TradeID trade_id{0};
    OrderID buy_order_id{0};
    OrderID sell_order_id{0};
    Price price{0};
    Quantity quantity{0};
};

} // namespace rtes
