#pragma once
// A deliberately naive order book used as the reference in differential tests.
// One flat vector of resting orders, linear scans for everything: slow, but
// short enough to check by reading. It implements the same rules as OrderBook:
// price-time priority, market orders are IOC, self-trade prevention, and
// modify keeps priority only for a quantity decrease at the same price.

#include "rtes/order_book.hpp"
#include <algorithm>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

namespace rtes::test {

struct BookEvent {
    enum Kind : uint8_t { TRADE, CANCEL } kind{TRADE};
    OrderID     a{0};        // TRADE: aggressor; CANCEL: cancelled order
    OrderID     b{0};        // TRADE: passive
    Quantity    qty{0};
    Price       price{0};
    CancelCause cause{CancelCause::NO_LIQUIDITY};

    bool operator==(const BookEvent& o) const {
        if (kind != o.kind) return false;
        if (kind == TRADE) return a == o.a && b == o.b && qty == o.qty && price == o.price;
        return a == o.a && cause == o.cause;
    }
};

// (price, total quantity, order count), best price first.
using Levels = std::vector<std::tuple<Price, Quantity, uint32_t>>;

class ReferenceBook {
public:
    explicit ReferenceBook(StpMode stp) : stp_(stp) {}

    struct RefOrder {
        OrderID   id{0};
        ClientID  client{0};
        Side      side{Side::BUY};
        OrderType type{OrderType::LIMIT};
        Price     price{0};
        Quantity  qty{0};
        Quantity  filled{0};
        uint64_t  seq{0};   // arrival order; lower = earlier
        Quantity  remaining() const { return qty - filled; }
    };

    std::vector<BookEvent> events;

    void add(RefOrder o) {
        o.seq = ++seq_;
        process(o);
    }

    bool cancel(OrderID id) {
        auto it = find_it(id);
        if (it == resting_.end()) return false;
        resting_.erase(it);
        return true;
    }

    bool modify(OrderID id, Price price, Quantity qty) {
        auto it = find_it(id);
        if (it == resting_.end()) return false;
        if (price == it->price && qty <= it->qty) {
            it->qty = qty;            // keeps its seq, so keeps priority
            return true;
        }
        RefOrder o = *it;
        resting_.erase(it);
        o.price = price;
        o.qty   = qty;
        o.seq   = ++seq_;             // back of the queue
        process(o);
        return true;
    }

    const RefOrder* find(OrderID id) const {
        for (auto& o : resting_) if (o.id == id) return &o;
        return nullptr;
    }

    std::vector<OrderID> resting_ids() const {
        std::vector<OrderID> ids;
        for (auto& o : resting_) ids.push_back(o.id);
        return ids;
    }

    Levels levels(Side side) const {
        std::map<Price, std::pair<Quantity, uint32_t>> m;
        for (auto& o : resting_) {
            if (o.side != side) continue;
            m[o.price].first  += o.remaining();
            m[o.price].second += 1;
        }
        Levels out;
        for (auto& [p, v] : m) out.emplace_back(p, v.first, v.second);
        if (side == Side::BUY) std::reverse(out.begin(), out.end());   // best (highest) bid first
        return out;
    }

private:
    StpMode               stp_;
    uint64_t              seq_{0};
    std::vector<RefOrder> resting_;

    std::vector<RefOrder>::iterator find_it(OrderID id) {
        return std::find_if(resting_.begin(), resting_.end(), [&](auto& o) { return o.id == id; });
    }

    // Best resting order on the other side: best price, then earliest.
    std::vector<RefOrder>::iterator best_opposite(Side side) {
        auto best = resting_.end();
        for (auto it = resting_.begin(); it != resting_.end(); ++it) {
            if (it->side == side) continue;
            if (best == resting_.end()) { best = it; continue; }
            const bool better_price = (side == Side::BUY) ? it->price < best->price
                                                          : it->price > best->price;
            if (better_price || (it->price == best->price && it->seq < best->seq)) best = it;
        }
        return best;
    }

    void process(RefOrder o) {
        bool cancel_taker = false;
        while (o.remaining() > 0) {
            auto p = best_opposite(o.side);
            if (p == resting_.end()) break;
            if (o.type == OrderType::LIMIT) {
                const bool crosses = (o.side == Side::BUY) ? o.price >= p->price : o.price <= p->price;
                if (!crosses) break;
            }
            if (p->client == o.client) {
                if (stp_ == StpMode::CANCEL_TAKER) { cancel_taker = true; break; }
                events.push_back({BookEvent::CANCEL, p->id, 0, 0, 0, CancelCause::SELF_TRADE});
                resting_.erase(p);
                if (stp_ == StpMode::CANCEL_BOTH) { cancel_taker = true; break; }
                continue;
            }
            const Quantity q = std::min(o.remaining(), p->remaining());
            o.filled  += q;
            p->filled += q;
            events.push_back({BookEvent::TRADE, o.id, p->id, q, p->price, CancelCause::NO_LIQUIDITY});
            if (p->remaining() == 0) resting_.erase(p);
        }
        if (o.remaining() == 0) return;
        if (cancel_taker) {
            events.push_back({BookEvent::CANCEL, o.id, 0, 0, 0, CancelCause::SELF_TRADE});
        } else if (o.type == OrderType::MARKET) {
            events.push_back({BookEvent::CANCEL, o.id, 0, 0, 0, CancelCause::NO_LIQUIDITY});
        } else {
            resting_.push_back(o);
        }
    }
};

// Records what the real OrderBook reports, in the same form.
struct RecordingListener : BookListener {
    std::vector<BookEvent> events;
    void on_trade(const Order& aggressor, const Order& passive, const Trade& t) override {
        events.push_back({BookEvent::TRADE, aggressor.order_id, passive.order_id, t.quantity, t.price,
                          CancelCause::NO_LIQUIDITY});
    }
    void on_book_cancel(const Order& o, CancelCause cause) override {
        events.push_back({BookEvent::CANCEL, o.order_id, 0, 0, 0, cause});
    }
};

inline Levels levels_of(const OrderBook& book, Side side) {
    Levels out;
    book.for_each_level(side, [&](Price p, Quantity q, uint32_t n) { out.emplace_back(p, q, n); });
    return out;
}

} // namespace rtes::test
