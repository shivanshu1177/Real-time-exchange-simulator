#pragma once

#include "types.hpp"
#include "memory_pool.hpp"
#include "flat_map.hpp"
#include <cstddef>
#include <functional>
#include <map>
#include <string>

namespace rtes {

enum class CancelCause : uint8_t {
    NO_LIQUIDITY,   // unfilled remainder of a market order (IOC)
    SELF_TRADE,     // self-trade prevention
};

// Receives what the book does on its own. Called on the engine thread, before
// any order involved is returned to the pool.
class BookListener {
public:
    virtual ~BookListener() = default;
    // One call per fill; both orders already reflect the fill.
    virtual void on_trade(const Order& aggressor, const Order& passive, const Trade& trade) = 0;
    // The book cancelled this order (it was not asked to).
    virtual void on_book_cancel(const Order& order, CancelCause cause) = 0;
};

struct PriceLevel {
    Price    price{0};
    Quantity total_quantity{0};   // sum of remaining quantity of the orders below
    uint32_t order_count{0};
    Order*   head{nullptr};       // oldest (first to fill)
    Order*   tail{nullptr};
};

// Price-time priority limit order book for one symbol. Single-threaded: only
// the symbol's engine thread may call it (no locks).
//
// Price levels live in a std::map: a node is allocated only when a new price
// level appears, never per order. Orders at a level form an intrusive doubly
// linked list (the links are in Order), so cancel is O(1) after the id lookup,
// which is a fixed-capacity hash map allocated once.
class OrderBook {
public:
    OrderBook(std::string symbol, OrderPool& pool, size_t max_orders,
              BookListener* listener = nullptr, StpMode stp = StpMode::CANCEL_TAKER);

    // Takes ownership of `order`: it trades, rests, or is cancelled, and is
    // returned to the pool when done. Market orders are immediate-or-cancel.
    // Returns false, and does not take ownership, if the book cannot hold
    // another resting order or the id is already resting.
    bool add_order(Order* order);

    // Cancel a resting order and return it to the pool. False if unknown.
    bool cancel_order(OrderID order_id);

    // Live (resting) order with this id, or nullptr.
    const Order* find(OrderID order_id) const;

    // Change price and/or total quantity of a resting order. Precondition:
    // new_quantity > filled. Same price and a lower quantity keeps time
    // priority; anything else re-queues the order at the back (it may trade).
    bool modify_order(OrderID order_id, Price new_price, Quantity new_quantity);

    Price    best_bid() const { return bids_.empty() ? 0 : bids_.begin()->first; }
    Price    best_ask() const { return asks_.empty() ? 0 : asks_.begin()->first; }
    Quantity best_bid_quantity() const { return bids_.empty() ? 0 : bids_.begin()->second.total_quantity; }
    Quantity best_ask_quantity() const { return asks_.empty() ? 0 : asks_.begin()->second.total_quantity; }

    size_t   order_count() const { return lookup_.size(); }
    bool     full() const { return lookup_.size() >= lookup_.max_size(); }
    size_t   level_count(Side side) const { return side == Side::BUY ? bids_.size() : asks_.size(); }
    Quantity level_quantity(Side side, Price price) const;

    // f(price, total_quantity, order_count), best price first.
    template<typename F>
    void for_each_level(Side side, F&& f) const {
        if (side == Side::BUY) { for (const auto& [p, l] : bids_) f(p, l.total_quantity, l.order_count); }
        else                   { for (const auto& [p, l] : asks_) f(p, l.total_quantity, l.order_count); }
    }

    // Return every resting order to the pool without reporting anything.
    void clear();

private:
    using Bids = std::map<Price, PriceLevel, std::greater<Price>>;
    using Asks = std::map<Price, PriceLevel>;

    std::string   symbol_;
    OrderPool&    pool_;
    BookListener* listener_;
    StpMode       stp_;
    TradeID       next_trade_id_{1};

    Bids bids_;
    Asks asks_;
    FlatMap<Order*> lookup_;   // resting orders by id

    // Match `o` against the other side, then rest, cancel or finish it.
    // in_lookup: `o` is already in lookup_ (modify path).
    void process(Order* o, bool in_lookup);
    template<typename Levels, typename Crosses>
    bool match_against(Order* o, Levels& levels, Crosses crosses);   // true = cancel taker (STP)
    void rest(Order* o);
    void remove_from_level(Order* o);
    void finish(Order* o, bool in_lookup);
    static void unlink(PriceLevel& level, Order* o);
};

} // namespace rtes
