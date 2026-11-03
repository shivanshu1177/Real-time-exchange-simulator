#include "rtes/matching_engine.hpp"
#include "rtes/logger.hpp"
#include <cstring>

namespace rtes {

MatchingEngine::MatchingEngine(const std::string& symbol, uint32_t symbol_index,
                               OrderPool& pool, size_t queue_capacity)
    : symbol_(symbol), symbol_index_(symbol_index), pool_(pool),
      input_queue_(std::make_unique<SPSCQueue<OrderRequest>>(queue_capacity)),
      event_queue_(std::make_unique<SPSCQueue<ExecEvent>>(queue_capacity))
{
    book_ = std::make_unique<OrderBook>(symbol, pool,
        [this](const Trade& trade, const Order& aggressor, const Order& passive) {
            on_trade(trade, aggressor, passive);
        });
}

MatchingEngine::~MatchingEngine() {
    stop();
}

void MatchingEngine::start() {
    running_.store(true, std::memory_order_release);
    worker_thread_ = std::thread(&MatchingEngine::run, this);
}

void MatchingEngine::stop() {
    running_.store(false, std::memory_order_release);
    if (worker_thread_.joinable()) worker_thread_.join();
}

bool MatchingEngine::submit(const OrderRequest& req) {
    return input_queue_->push(req);
}

void MatchingEngine::set_market_data_queue(MPMCQueue<MarketDataEvent>* queue) {
    market_data_queue_ = queue;
}

void MatchingEngine::run() {
    while (running_.load(std::memory_order_acquire)) {
        poll_once();
        // tight spin — no sleep to minimise latency
    }
}

size_t MatchingEngine::poll_once() {
    size_t n = 0;
    OrderRequest req;
    while (input_queue_->pop(req)) {
        switch (req.type) {
            case OrderRequest::NEW_ORDER:    process_new_order(req.order); break;
            case OrderRequest::CANCEL_ORDER: process_cancel(req);          break;
            case OrderRequest::MODIFY_ORDER: process_modify(req);          break;
        }
        publish_bbo_update();
        ++n;
    }
    return n;
}

void MatchingEngine::emit(const ExecEvent& ev) {
    // Execution events must not be lost: wait for the gateway to drain the
    // queue. Only give up when the engine is shutting down (or was never
    // started, as in single-threaded tests).
    while (!event_queue_->push(ev)) {
        if (!running_.load(std::memory_order_acquire)) {
            events_dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::this_thread::yield();
    }
}

void MatchingEngine::emit_cancel_reject(const OrderRequest& req, RejectReason reason,
                                        CancelRejectResponseTo to) {
    ExecEvent ev;
    ev.kind            = ExecEvent::CANCEL_REJECT;
    ev.reason          = reason;
    ev.response_to     = to;
    ev.session_id      = req.session_id;
    ev.order_id        = req.order_id;
    ev.client_order_id = req.client_order_id;
    emit(ev);
}

static ExecEvent order_event(const Order& o, ExecType type) {
    ExecEvent ev;
    ev.kind            = ExecEvent::EXEC;
    ev.exec_type       = type;
    ev.side            = o.side;
    ev.session_id      = o.session_id;
    ev.order_id        = o.order_id;
    ev.client_order_id = o.client_order_id;
    ev.price           = o.price;
    ev.leaves_qty      = o.remaining();
    ev.cum_qty         = o.filled;
    return ev;
}

void MatchingEngine::process_new_order(Order* order) {
    orders_processed_.fetch_add(1, std::memory_order_relaxed);
    // NEW is reported before any fill so the client always sees it first.
    emit(order_event(*order, ExecType::NEW));
    book_->add_order(order);
}

void MatchingEngine::process_cancel(const OrderRequest& req) {
    const Order* o = book_->find(req.order_id);
    if (!o) {
        emit_cancel_reject(req, req.via_client_order_id ? RejectReason::TOO_LATE
                                                        : RejectReason::UNKNOWN_ORDER,
                           CancelRejectResponseTo::CANCEL);
        return;
    }
    if (o->session_id != req.session_id) {
        emit_cancel_reject(req, RejectReason::NOT_OWNER, CancelRejectResponseTo::CANCEL);
        return;
    }
    ExecEvent ev = order_event(*o, ExecType::CANCELED);
    ev.leaves_qty = 0;
    book_->cancel_order(req.order_id);   // returns the order to the pool
    emit(ev);
}

void MatchingEngine::process_modify(const OrderRequest& req) {
    const Order* o = book_->find(req.order_id);
    if (!o) {
        emit_cancel_reject(req, req.via_client_order_id ? RejectReason::TOO_LATE
                                                        : RejectReason::UNKNOWN_ORDER,
                           CancelRejectResponseTo::MODIFY);
        return;
    }
    if (o->session_id != req.session_id) {
        emit_cancel_reject(req, RejectReason::NOT_OWNER, CancelRejectResponseTo::MODIFY);
        return;
    }
    if (o->type != OrderType::LIMIT) {
        emit_cancel_reject(req, RejectReason::INVALID_ORDER_TYPE, CancelRejectResponseTo::MODIFY);
        return;
    }
    if (req.new_quantity <= o->filled) {
        emit_cancel_reject(req, RejectReason::INVALID_QUANTITY, CancelRejectResponseTo::MODIFY);
        return;
    }
    // REPLACED goes out before any fill the new price may cause.
    ExecEvent ev = order_event(*o, ExecType::REPLACED);
    ev.price      = req.new_price;
    ev.leaves_qty = req.new_quantity - o->filled;
    emit(ev);
    book_->modify_order(req.order_id, req.new_price, req.new_quantity);
}

void MatchingEngine::on_trade(const Trade& trade, const Order& aggressor, const Order& passive) {
    trades_executed_.fetch_add(1, std::memory_order_relaxed);
    for (const Order* o : {&aggressor, &passive}) {
        ExecEvent ev = order_event(*o, o->is_filled() ? ExecType::FILL : ExecType::PARTIAL_FILL);
        ev.trade_id = trade.trade_id;
        ev.last_px  = trade.price;
        ev.last_qty = trade.quantity;
        emit(ev);
    }
    publish_trade(trade);
}

void MatchingEngine::publish_trade(const Trade& trade) {
    if (!market_data_queue_) return;
    MarketDataEvent ev(trade);
    market_data_queue_->push(ev);
}

void MatchingEngine::publish_bbo_update() {
    if (!market_data_queue_) return;

    MarketDataEvent ev;
    ev.type = MarketDataEvent::BBO_UPDATE;
    std::strncpy(ev.symbol, symbol_.c_str(), sizeof(ev.symbol) - 1);
    ev.symbol[sizeof(ev.symbol) - 1] = '\0';
    ev.bbo.bid_price    = book_->best_bid();
    ev.bbo.bid_quantity = 0;
    ev.bbo.ask_price    = book_->best_ask();
    ev.bbo.ask_quantity = 0;
    market_data_queue_->push(ev);
}

} // namespace rtes
