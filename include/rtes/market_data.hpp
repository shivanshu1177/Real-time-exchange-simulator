#pragma once

#include "rtes/protocol.hpp"

namespace rtes {

#pragma pack(push, 1)

struct BboUpdateMessage {
    MessageHeader header;
    char     symbol[8];
    uint64_t bid_price;
    uint32_t bid_quantity;
    uint64_t ask_price;
    uint32_t ask_quantity;
};

struct TradeUpdateMessage {
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

static_assert(sizeof(BboUpdateMessage)   == 60, "BboUpdateMessage wire size");
static_assert(sizeof(TradeUpdateMessage) == 76, "TradeUpdateMessage wire size");

} // namespace rtes
