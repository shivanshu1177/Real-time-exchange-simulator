#pragma once

#include "rtes/order_book.hpp"
#include "rtes/spsc_queue.hpp"
#include "rtes/mpmc_queue.hpp"
#include "rtes/types.hpp"
#include "rtes/protocol.hpp"
#include <thread>
#include <atomic>
#include <unordered_map>
#include <cstring>
#include <memory>
#include <string>

namespace rtes {

// Gateway -> engine. Fixed size, passed by value through the engine's SPSC
// queue: orders cross threads as values, never as pool pointers, so each
// engine's pool has a single owner.
struct OrderRequest {
    enum Type : uint8_t { NEW_ORDER, CANCEL_ORDER, MODIFY_ORDER };
    Type      type{NEW_ORDER};
    bool      via_client_order_id{false}; // gateway resolved the id from its live-order map
    Side      side{Side::BUY};            // NEW
    OrderType order_type{OrderType::LIMIT}; // NEW
    SessionId session_id{0};              // requesting session (owner check)
    OrderID   order_id{0};                // exchange order id
    uint64_t  client_order_id{0};         // echoed in the response
    ClientID  client_id{0};               // NEW: identity from logon
    Price     price{0};                   // NEW (0 for market), or MODIFY: new price
    Quantity  quantity{0};                // NEW, or MODIFY: new total quantity
};

// Engine -> gateway. One per execution report or cancel reject.
struct ExecEvent {
    enum Kind : uint8_t { EXEC, CANCEL_REJECT };
    Kind         kind{EXEC};
    ExecType     exec_type{ExecType::NEW};
    RejectReason reason{RejectReason::NONE};
    CancelRejectResponseTo response_to{CancelRejectResponseTo::CANCEL};
    Side         side{Side::BUY};
    SessionId    session_id{0};
    OrderID      order_id{0};
    uint64_t     client_order_id{0};
    uint64_t     trade_id{0};
    Price        price{0};
    Price        last_px{0};
    Quantity     last_qty{0};
    Quantity     leaves_qty{0};
    Quantity     cum_qty{0};

    bool is_terminal() const noexcept {
        return kind == EXEC && (exec_type == ExecType::FILL ||
                                exec_type == ExecType::CANCELED ||
                                exec_type == ExecType::REJECTED);
    }
};

// POD trade data for use inside the union
struct TradeData {
    TradeID  trade_id{0};
    OrderID  buy_order_id{0};
    OrderID  sell_order_id{0};
    Price    price{0};
    Quantity quantity{0};
};

struct BboData {
    Price    bid_price{0};
    Quantity bid_quantity{0};
    Price    ask_price{0};
    Quantity ask_quantity{0};
};

struct MarketDataEvent {
    enum Type { TRADE, BBO_UPDATE } type{TRADE};
    char symbol[8]{};
    union {
        TradeData trade;
        BboData   bbo;
    };

    MarketDataEvent() : type(BBO_UPDATE), bbo{} {}

    MarketDataEvent(const Trade& t, const char (&sym)[8]) : type(TRADE), trade{} {
        trade.trade_id      = t.trade_id;
        trade.buy_order_id  = t.buy_order_id;
        trade.sell_order_id = t.sell_order_id;
        trade.price         = t.price;
        trade.quantity      = t.quantity;
        std::memcpy(symbol, sym, sizeof(symbol));
    }
};

class MatchingEngine : private BookListener {
public:
    // symbol_index is this engine's position in the exchange's symbol table;
    // it is encoded in the high bits of every order id the gateway assigns.
    // max_orders sizes this engine's own order pool and book: the most orders
    // the symbol can hold at once.
    MatchingEngine(const std::string& symbol, uint32_t symbol_index,
                   size_t queue_capacity = 65536, size_t max_orders = 65536,
                   StpMode stp = StpMode::CANCEL_TAKER);
    ~MatchingEngine() override;

    void start();
    void stop();

    // Gateway thread only (single producer). Returns false if the queue is full.
    bool submit(const OrderRequest& req);

    // Engine side: process every queued request. Called by the engine thread,
    // or directly by tests that do not start the thread. Returns the number processed.
    size_t poll_once();

    // Gateway thread only (single consumer of execution events).
    bool pop_event(ExecEvent& ev) { return event_queue_->pop(ev); }

    // Market data subscription
    void set_market_data_queue(MPMCQueue<MarketDataEvent>* queue);

    const std::string& symbol() const { return symbol_; }
    uint32_t symbol_index() const { return symbol_index_; }

    // Statistics
    uint64_t orders_processed() const { return orders_processed_.load(); }
    uint64_t trades_executed() const { return trades_executed_.load(); }
    uint64_t events_dropped() const { return events_dropped_.load(); }

    // Engine thread only (or after stop()): free order slots, for tests.
    size_t pool_available() const { return pool_.available(); }
    size_t pool_capacity() const { return pool_.capacity(); }

private:
    std::string symbol_;
    char        symbol8_[8]{};   // NUL-padded wire form
    uint32_t    symbol_index_;
    OrderPool   pool_;   // owned; only this engine's thread acquires and releases
    std::unique_ptr<OrderBook> book_;

    // Threading
    std::thread worker_thread_;
    std::atomic<bool> running_{false};

    // Gateway -> engine requests, engine -> gateway execution events
    std::unique_ptr<SPSCQueue<OrderRequest>> input_queue_;
    std::unique_ptr<SPSCQueue<ExecEvent>>    event_queue_;

    // Output queue (MPMC to market data publisher)
    MPMCQueue<MarketDataEvent>* market_data_queue_{nullptr};

    // Statistics
    std::atomic<uint64_t> orders_processed_{0};
    std::atomic<uint64_t> trades_executed_{0};
    std::atomic<uint64_t> events_dropped_{0};

    // Worker thread main loop
    void run();

    void process_new_order(const OrderRequest& req);
    void process_cancel(const OrderRequest& req);
    void process_modify(const OrderRequest& req);

    void emit(const ExecEvent& ev);
    void emit_cancel_reject(const OrderRequest& req, RejectReason reason,
                            CancelRejectResponseTo to);

    // Market data publishing
    void publish_bbo_update();

    // BookListener
    void on_trade(const Order& aggressor, const Order& passive, const Trade& trade) override;
    void on_book_cancel(const Order& order, CancelCause cause) override;
};

} // namespace rtes