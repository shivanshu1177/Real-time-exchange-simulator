#pragma once

#include <cstdint>
#include <string>

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

struct Order {
    OrderID  order_id{0};          // exchange-assigned, unique across the exchange
    ClientID client_id{0};
    SessionId session_id{0};       // owning session
    uint64_t client_order_id{0};   // the owner's id for this order
    std::string symbol;
    Side side{Side::BUY};
    OrderType type{OrderType::LIMIT};
    Price price{0};
    Quantity quantity{0};
    Quantity filled{0};
    
    bool is_filled() const { return filled >= quantity; }
    Quantity remaining() const { return quantity - filled; }
};

struct Trade {
    TradeID trade_id{0};
    OrderID buy_order_id{0};
    OrderID sell_order_id{0};
    std::string symbol;
    Price price{0};
    Quantity quantity{0};
};

} // namespace rtes