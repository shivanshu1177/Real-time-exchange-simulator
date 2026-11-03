#pragma once

#include "types.hpp"
#include "memory_pool.hpp"
#include <map>
#include <deque>
#include <unordered_map>
#include <functional>
#include <mutex>

namespace rtes {

struct PriceLevel {
    Price price;
    std::deque<Order*> orders;
    Quantity total_quantity{0};
    
    explicit PriceLevel(Price p) : price(p) {}
};

class OrderBook {
public:
    // Called once per fill, before a filled order is returned to the pool.
    // aggressor/passive reflect the state after the fill.
    using TradeCallback = std::function<void(const Trade&, const Order& aggressor, const Order& passive)>;
    
    explicit OrderBook(const std::string& symbol, OrderPool& pool, TradeCallback cb = nullptr);
    
    bool add_order(Order* order);
    bool cancel_order(OrderID order_id);

    // Live order with this id, or nullptr.
    const Order* find(OrderID order_id) const;

    // Change price and/or total quantity of a resting order. Precondition:
    // new_quantity > filled. Same price and a lower quantity keeps time
    // priority; anything else re-queues the order (and may match it).
    // Returns false if the order is unknown.
    bool modify_order(OrderID order_id, Price new_price, Quantity new_quantity);
    
    Price best_bid() const;
    Price best_ask() const;
    
    size_t order_count() const;

private:
    std::string symbol_;
    OrderPool& pool_;
    TradeCallback trade_callback_;
    TradeID next_trade_id_{1};
    
    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    std::map<Price, PriceLevel> asks_;
    std::unordered_map<OrderID, Order*> order_lookup_;
    
    mutable std::mutex mutex_;
    
    void match_order(Order* order);
    void execute_trade(Order* aggressive_order, Order* passive_order, Quantity quantity, Price price);
    void remove_order_from_book(Order* order);
    bool add_to_book(Order* order);
};

} // namespace rtes