#include "rtes/order_book.hpp"

#include <algorithm>

namespace rtes {

OrderBook::OrderBook(std::string symbol, OrderPool& pool, size_t max_orders,
                     BookListener* listener, StpMode stp)
    : symbol_(std::move(symbol)), pool_(pool), listener_(listener), stp_(stp),
      lookup_(max_orders) {}

bool OrderBook::add_order(Order* order) {
    if (!order) return false;
    if (lookup_.find(order->order_id)) return false;
    if (order->type == OrderType::LIMIT && full()) return false;
    order->prev = order->next = nullptr;
    process(order, /*in_lookup=*/false);
    return true;
}

bool OrderBook::cancel_order(OrderID order_id) {
    Order** found = lookup_.find(order_id);
    if (!found) return false;
    Order* o = *found;
    remove_from_level(o);
    lookup_.erase(order_id);
    pool_.release(o);
    return true;
}

const Order* OrderBook::find(OrderID order_id) const {
    Order* const* found = lookup_.find(order_id);
    return found ? *found : nullptr;
}

bool OrderBook::modify_order(OrderID order_id, Price new_price, Quantity new_quantity) {
    Order** found = lookup_.find(order_id);
    if (!found) return false;
    Order* o = *found;

    if (new_price == o->price && new_quantity <= o->quantity) {
        // Quantity decrease at the same price keeps time priority.
        const Quantity reduction = o->quantity - new_quantity;
        PriceLevel& level = (o->side == Side::BUY) ? bids_.find(o->price)->second
                                                   : asks_.find(o->price)->second;
        level.total_quantity -= reduction;
        o->quantity = new_quantity;
        return true;
    }

    // Price change or quantity increase: back of the queue, possibly trading.
    remove_from_level(o);
    o->price    = new_price;
    o->quantity = new_quantity;
    process(o, /*in_lookup=*/true);
    return true;
}

Quantity OrderBook::level_quantity(Side side, Price price) const {
    if (side == Side::BUY) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total_quantity;
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.total_quantity;
}

void OrderBook::clear() {
    lookup_.for_each([&](uint64_t, Order*& o) { pool_.release(o); });
    lookup_.clear();
    bids_.clear();
    asks_.clear();
}

void OrderBook::process(Order* o, bool in_lookup) {
    const bool cancel_taker = (o->side == Side::BUY)
        ? match_against(o, asks_, [](Price limit, Price level) { return limit >= level; })
        : match_against(o, bids_, [](Price limit, Price level) { return limit <= level; });

    if (o->is_filled()) {
        finish(o, in_lookup);
    } else if (cancel_taker) {
        if (listener_) listener_->on_book_cancel(*o, CancelCause::SELF_TRADE);
        finish(o, in_lookup);
    } else if (o->type == OrderType::MARKET) {
        // Immediate-or-cancel: whatever did not trade now is cancelled.
        if (listener_) listener_->on_book_cancel(*o, CancelCause::NO_LIQUIDITY);
        finish(o, in_lookup);
    } else {
        rest(o);
        if (!in_lookup) lookup_.insert(o->order_id, o);
    }
}

template<typename Levels, typename Crosses>
bool OrderBook::match_against(Order* o, Levels& levels, Crosses crosses) {
    while (o->remaining() > 0 && !levels.empty()) {
        auto it = levels.begin();
        PriceLevel& level = it->second;
        if (o->type == OrderType::LIMIT && !crosses(o->price, level.price)) break;

        Order* passive = level.head;

        if (passive->client_id == o->client_id) {
            if (stp_ == StpMode::CANCEL_TAKER) return true;
            // CANCEL_MAKER / CANCEL_BOTH: remove the resting order.
            level.total_quantity -= passive->remaining();
            unlink(level, passive);
            lookup_.erase(passive->order_id);
            if (listener_) listener_->on_book_cancel(*passive, CancelCause::SELF_TRADE);
            pool_.release(passive);
            if (!level.head) levels.erase(it);
            if (stp_ == StpMode::CANCEL_BOTH) return true;
            continue;
        }

        const Quantity qty = std::min(o->remaining(), passive->remaining());
        o->filled             += qty;
        passive->filled       += qty;
        level.total_quantity  -= qty;

        Trade trade;
        trade.trade_id      = next_trade_id_++;
        trade.buy_order_id  = (o->side == Side::BUY) ? o->order_id : passive->order_id;
        trade.sell_order_id = (o->side == Side::SELL) ? o->order_id : passive->order_id;
        trade.price         = level.price;
        trade.quantity      = qty;
        if (listener_) listener_->on_trade(*o, *passive, trade);

        if (passive->is_filled()) {
            unlink(level, passive);
            lookup_.erase(passive->order_id);
            pool_.release(passive);
            if (!level.head) levels.erase(it);
        }
    }
    return false;
}

void OrderBook::rest(Order* o) {
    auto append = [o](auto& levels) {
        auto [it, inserted] = levels.try_emplace(o->price);
        PriceLevel& level = it->second;
        if (inserted) level.price = o->price;
        o->prev = level.tail;
        o->next = nullptr;
        if (level.tail) level.tail->next = o; else level.head = o;
        level.tail = o;
        level.total_quantity += o->remaining();
        ++level.order_count;
    };
    if (o->side == Side::BUY) append(bids_); else append(asks_);
}

void OrderBook::remove_from_level(Order* o) {
    auto remove = [o](auto& levels) {
        auto it = levels.find(o->price);
        if (it == levels.end()) return;
        PriceLevel& level = it->second;
        level.total_quantity -= o->remaining();
        unlink(level, o);
        if (!level.head) levels.erase(it);
    };
    if (o->side == Side::BUY) remove(bids_); else remove(asks_);
}

void OrderBook::finish(Order* o, bool in_lookup) {
    if (in_lookup) lookup_.erase(o->order_id);
    pool_.release(o);
}

void OrderBook::unlink(PriceLevel& level, Order* o) {
    if (o->prev) o->prev->next = o->next; else level.head = o->next;
    if (o->next) o->next->prev = o->prev; else level.tail = o->prev;
    o->prev = o->next = nullptr;
    --level.order_count;
}

} // namespace rtes
