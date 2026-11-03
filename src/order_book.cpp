#include "rtes/order_book.hpp"
#include "rtes/logger.hpp"

#include <algorithm>

namespace rtes {

OrderBook::OrderBook(const std::string& symbol, OrderPool& pool, TradeCallback cb)
    : symbol_(symbol), pool_(pool), trade_callback_(cb) {
    order_lookup_.reserve(65536);
}

bool OrderBook::add_order(Order* order) {
    if (!order) return false;
    
    std::lock_guard<std::mutex> lock(mutex_);
    order_lookup_[order->order_id] = order;
    
    match_order(order);

    if (!order->is_filled()) {
        add_to_book(order);
    } else {
        // Aggressive order fully filled during matching — remove from lookup and return to pool
        order_lookup_.erase(order->order_id);
        pool_.release(order);
    }

    return true;
}

bool OrderBook::cancel_order(OrderID order_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = order_lookup_.find(order_id);
    if (it == order_lookup_.end()) {
        return false;
    }
    
    remove_order_from_book(it->second);
    pool_.release(it->second);
    order_lookup_.erase(it);
    
    return true;
}

const Order* OrderBook::find(OrderID order_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = order_lookup_.find(order_id);
    return it == order_lookup_.end() ? nullptr : it->second;
}

bool OrderBook::modify_order(OrderID order_id, Price new_price, Quantity new_quantity) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = order_lookup_.find(order_id);
    if (it == order_lookup_.end()) return false;
    Order* order = it->second;

    if (new_price == order->price && new_quantity <= order->quantity) {
        // Quantity decrease at the same price: keep time priority.
        Quantity reduction = order->quantity - new_quantity;
        auto adjust = [&](auto& side) {
            auto level_it = side.find(order->price);
            if (level_it != side.end()) level_it->second.total_quantity -= reduction;
        };
        if (order->side == Side::BUY) adjust(bids_); else adjust(asks_);
        order->quantity = new_quantity;
        return true;
    }

    // Price change or quantity increase: lose priority, re-enter as a new
    // arrival (it can match if the new price crosses).
    remove_order_from_book(order);
    order->price    = new_price;
    order->quantity = new_quantity;
    match_order(order);
    if (order->is_filled()) {
        order_lookup_.erase(order->order_id);
        pool_.release(order);
    } else {
        add_to_book(order);
    }
    return true;
}

Price OrderBook::best_bid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bids_.empty() ? 0 : bids_.begin()->first;
}

Price OrderBook::best_ask() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return asks_.empty() ? 0 : asks_.begin()->first;
}

size_t OrderBook::order_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return order_lookup_.size();
}

void OrderBook::match_order(Order* order) {
    if (order->side == Side::BUY) {
        // Buy order matches against asks
        while (!order->is_filled() && !asks_.empty()) {
            auto it = asks_.begin();
            // Prefetch the next price level while processing the current one.
            // __builtin_prefetch is portable: maps to prfm on arm64, prefetcht0 on x86.
            auto nxt = std::next(it);
            if (nxt != asks_.end())
                __builtin_prefetch(&nxt->second, 0, 1);

            auto& best_level = it->second;
            if (order->type == OrderType::MARKET || order->price >= best_level.price) {
                auto* passive_order = best_level.orders.front();
                Quantity trade_qty = std::min(order->remaining(), passive_order->remaining());

                execute_trade(order, passive_order, trade_qty, best_level.price);

                if (passive_order->is_filled()) {
                    best_level.orders.pop_front();
                    best_level.total_quantity -= passive_order->quantity;
                    order_lookup_.erase(passive_order->order_id);
                    pool_.release(passive_order);

                    if (best_level.orders.empty()) {
                        asks_.erase(it);
                    }
                }
            } else {
                break;
            }
        }
    } else {
        // Sell order matches against bids
        while (!order->is_filled() && !bids_.empty()) {
            auto it = bids_.begin();
            auto nxt = std::next(it);
            if (nxt != bids_.end())
                __builtin_prefetch(&nxt->second, 0, 1);

            auto& best_level = it->second;
            if (order->type == OrderType::MARKET || order->price <= best_level.price) {
                auto* passive_order = best_level.orders.front();
                Quantity trade_qty = std::min(order->remaining(), passive_order->remaining());

                execute_trade(order, passive_order, trade_qty, best_level.price);

                if (passive_order->is_filled()) {
                    best_level.orders.pop_front();
                    best_level.total_quantity -= passive_order->quantity;
                    order_lookup_.erase(passive_order->order_id);
                    pool_.release(passive_order);

                    if (best_level.orders.empty()) {
                        bids_.erase(it);
                    }
                }
            } else {
                break;
            }
        }
    }
}

void OrderBook::execute_trade(Order* aggressive_order, Order* passive_order, Quantity quantity, Price price) {
    aggressive_order->filled += quantity;
    passive_order->filled += quantity;
    
    if (trade_callback_) {
        Trade trade{
            .trade_id = next_trade_id_++,
            .buy_order_id = (aggressive_order->side == Side::BUY) ? aggressive_order->order_id : passive_order->order_id,
            .sell_order_id = (aggressive_order->side == Side::SELL) ? aggressive_order->order_id : passive_order->order_id,
            .symbol = symbol_,
            .price = price,
            .quantity = quantity
        };
        trade_callback_(trade, *aggressive_order, *passive_order);
    }
}

void OrderBook::remove_order_from_book(Order* order) {
    if (order->side == Side::BUY) {
        auto level_it = bids_.find(order->price);
        if (level_it != bids_.end()) {
            auto& orders = level_it->second.orders;
            orders.erase(std::remove(orders.begin(), orders.end(), order), orders.end());
            level_it->second.total_quantity -= order->remaining();
            
            if (orders.empty()) {
                bids_.erase(level_it);
            }
        }
    } else {
        auto level_it = asks_.find(order->price);
        if (level_it != asks_.end()) {
            auto& orders = level_it->second.orders;
            orders.erase(std::remove(orders.begin(), orders.end(), order), orders.end());
            level_it->second.total_quantity -= order->remaining();
            
            if (orders.empty()) {
                asks_.erase(level_it);
            }
        }
    }
}

bool OrderBook::add_to_book(Order* order) {
    if (order->type == OrderType::MARKET) {
        return false; // Market orders don't rest in book
    }
    
    if (order->side == Side::BUY) {
        auto [level_it, inserted] = bids_.try_emplace(order->price, order->price);
        level_it->second.orders.push_back(order);
        level_it->second.total_quantity += order->remaining();
    } else {
        auto [level_it, inserted] = asks_.try_emplace(order->price, order->price);
        level_it->second.orders.push_back(order);
        level_it->second.total_quantity += order->remaining();
    }
    
    return true;
}

} // namespace rtes