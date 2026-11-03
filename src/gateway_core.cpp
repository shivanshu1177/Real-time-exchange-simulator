#include "rtes/gateway_core.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>

namespace rtes {

namespace {
uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

GatewayCore::GatewayCore(RiskManager& risk, OrderPool& pool,
                         std::vector<MatchingEngine*> engines, OutputSink& out,
                         size_t max_live_orders_per_session)
    : risk_(risk), pool_(pool), engines_(std::move(engines)),
      next_order_seq_(engines_.size(), 0), out_(out),
      max_live_orders_(max_live_orders_per_session)
{
    for (auto* e : engines_) {
        std::array<char, 8> name{};
        std::memcpy(name.data(), e->symbol().data(), std::min(e->symbol().size(), name.size()));
        symbols_.push_back(name);
    }
}

SessionId GatewayCore::open_session() {
    SessionId id = next_session_id_++;
    sessions_.emplace(id, std::make_unique<Session>(max_live_orders_));
    return id;
}

void GatewayCore::close_session(SessionId session) {
    sessions_.erase(session);
}

size_t GatewayCore::live_orders(SessionId session) const {
    auto it = sessions_.find(session);
    return it == sessions_.end() ? 0 : it->second->live.size();
}

int GatewayCore::find_symbol(const char (&symbol)[8]) const noexcept {
    for (size_t i = 0; i < symbols_.size(); ++i) {
        if (std::memcmp(symbols_[i].data(), symbol, 8) == 0) return static_cast<int>(i);
    }
    return -1;
}

template<typename Msg>
void GatewayCore::send(SessionId session, Session& s, Msg& msg, MessageType type) {
    seal_message(msg, type, s.next_out_seq++, now_ns());
    out_.send(session, &msg, sizeof(msg));
}

void GatewayCore::on_message(SessionId session, const uint8_t* data, size_t len) {
    if (len < sizeof(MessageHeader)) return;
    MessageHeader hdr;
    std::memcpy(&hdr, data, sizeof(hdr));
    switch (static_cast<MessageType>(hdr.type)) {
        case MessageType::NEW_ORDER:
            if (len >= sizeof(NewOrderMessage)) {
                NewOrderMessage m; std::memcpy(&m, data, sizeof(m)); on_new_order(session, m);
            }
            break;
        case MessageType::CANCEL_ORDER:
            if (len >= sizeof(CancelOrderMessage)) {
                CancelOrderMessage m; std::memcpy(&m, data, sizeof(m)); on_cancel(session, m);
            }
            break;
        case MessageType::MODIFY_ORDER:
            if (len >= sizeof(ModifyOrderMessage)) {
                ModifyOrderMessage m; std::memcpy(&m, data, sizeof(m)); on_modify(session, m);
            }
            break;
        default:
            break;
    }
}

void GatewayCore::send_reject(SessionId session, Session& s, const NewOrderMessage& msg,
                              RejectReason r) {
    ExecReportMessage rep{};
    rep.order_id        = 0;
    rep.client_order_id = msg.client_order_id;
    std::memcpy(rep.symbol, msg.symbol, sizeof(rep.symbol));
    rep.price           = msg.price;
    rep.exec_type       = static_cast<uint8_t>(ExecType::REJECTED);
    rep.side            = msg.side;
    rep.reject_reason   = static_cast<uint8_t>(r);
    send(session, s, rep, MessageType::EXEC_REPORT);
}

void GatewayCore::send_cancel_reject(SessionId session, Session& s, uint64_t order_id,
                                     uint64_t client_order_id, RejectReason r,
                                     CancelRejectResponseTo to) {
    CancelRejectMessage rej{};
    rej.order_id        = order_id;
    rej.client_order_id = client_order_id;
    rej.reject_reason   = static_cast<uint8_t>(r);
    rej.response_to     = static_cast<uint8_t>(to);
    send(session, s, rej, MessageType::CANCEL_REJECT);
}

void GatewayCore::on_new_order(SessionId session, const NewOrderMessage& msg) {
    auto it = sessions_.find(session);
    if (it == sessions_.end()) return;
    Session& s = *it->second;

    const int sym = find_symbol(msg.symbol);
    if (sym < 0)                                     return send_reject(session, s, msg, RejectReason::UNKNOWN_SYMBOL);
    if (msg.side > 1)                                return send_reject(session, s, msg, RejectReason::INVALID_SIDE);
    if (msg.order_type > 1)                          return send_reject(session, s, msg, RejectReason::INVALID_ORDER_TYPE);
    if (msg.quantity == 0)                           return send_reject(session, s, msg, RejectReason::INVALID_QUANTITY);
    const auto type = static_cast<OrderType>(msg.order_type);
    if (type == OrderType::LIMIT && msg.price == 0)  return send_reject(session, s, msg, RejectReason::INVALID_PRICE);
    if (s.live.find(msg.client_order_id))            return send_reject(session, s, msg, RejectReason::DUPLICATE_ORDER_ID);
    if (s.live.size() >= s.live.max_size())          return send_reject(session, s, msg, RejectReason::SYSTEM_BUSY);

    const auto idx = static_cast<uint32_t>(sym);
    const Price price = (type == OrderType::LIMIT) ? msg.price : 0;
    if (auto r = risk_.check_new_order(msg.client_id, idx, type, price, msg.quantity);
        r != RejectReason::NONE) {
        return send_reject(session, s, msg, r);
    }

    Order* order = pool_.acquire();
    if (!order) return send_reject(session, s, msg, RejectReason::SYSTEM_BUSY);

    const OrderID id = make_order_id(idx, ++next_order_seq_[idx]);
    order->order_id        = id;
    order->client_id       = msg.client_id;
    order->session_id      = session;
    order->client_order_id = msg.client_order_id;
    order->symbol          = engines_[idx]->symbol();
    order->side            = static_cast<Side>(msg.side);
    order->type            = type;
    order->price           = price;
    order->quantity        = msg.quantity;
    order->filled          = 0;

    s.live.insert(msg.client_order_id, LiveOrder{id, idx});

    OrderRequest req;
    req.type       = OrderRequest::NEW_ORDER;
    req.session_id = session;
    req.order      = order;
    if (!engines_[idx]->submit(req)) {
        s.live.erase(msg.client_order_id);
        pool_.release(order);
        return send_reject(session, s, msg, RejectReason::SYSTEM_BUSY);
    }
    // No reply here: the engine reports NEW (then any fills) in order.
}

bool GatewayCore::resolve(Session& s, uint64_t order_id, uint64_t client_order_id,
                          OrderRequest& req, uint32_t& symbol_index) const noexcept {
    if (order_id != 0) {
        const int64_t idx = symbol_index_of(order_id);
        if (idx < 0 || idx >= static_cast<int64_t>(engines_.size())) return false;
        symbol_index            = static_cast<uint32_t>(idx);
        req.order_id            = order_id;
        req.client_order_id     = client_order_id;
        req.via_client_order_id = false;   // the engine checks ownership
        return true;
    }
    const LiveOrder* live = s.live.find(client_order_id);
    if (!live) return false;
    symbol_index            = live->symbol_index;
    req.order_id            = live->order_id;
    req.client_order_id     = client_order_id;
    req.via_client_order_id = true;
    return true;
}

void GatewayCore::on_cancel(SessionId session, const CancelOrderMessage& msg) {
    auto it = sessions_.find(session);
    if (it == sessions_.end()) return;
    Session& s = *it->second;

    OrderRequest req;
    req.type       = OrderRequest::CANCEL_ORDER;
    req.session_id = session;
    uint32_t idx = 0;
    if (!resolve(s, msg.order_id, msg.client_order_id, req, idx)) {
        return send_cancel_reject(session, s, msg.order_id, msg.client_order_id,
                                  RejectReason::UNKNOWN_ORDER, CancelRejectResponseTo::CANCEL);
    }
    if (!engines_[idx]->submit(req)) {
        send_cancel_reject(session, s, msg.order_id, msg.client_order_id,
                           RejectReason::SYSTEM_BUSY, CancelRejectResponseTo::CANCEL);
    }
}

void GatewayCore::on_modify(SessionId session, const ModifyOrderMessage& msg) {
    auto it = sessions_.find(session);
    if (it == sessions_.end()) return;
    Session& s = *it->second;

    auto reject = [&](RejectReason r) {
        send_cancel_reject(session, s, msg.order_id, msg.client_order_id, r,
                           CancelRejectResponseTo::MODIFY);
    };

    OrderRequest req;
    req.type         = OrderRequest::MODIFY_ORDER;
    req.session_id   = session;
    req.new_price    = msg.new_price;
    req.new_quantity = msg.new_quantity;
    uint32_t idx = 0;
    if (!resolve(s, msg.order_id, msg.client_order_id, req, idx)) return reject(RejectReason::UNKNOWN_ORDER);
    if (msg.new_quantity == 0) return reject(RejectReason::INVALID_QUANTITY);
    if (msg.new_price == 0)    return reject(RejectReason::INVALID_PRICE);
    if (auto r = risk_.check_modify(0, idx, msg.new_price, msg.new_quantity);
        r != RejectReason::NONE) {
        return reject(r);
    }
    if (!engines_[idx]->submit(req)) reject(RejectReason::SYSTEM_BUSY);
}

size_t GatewayCore::drain_events() {
    size_t n = 0;
    ExecEvent ev;
    for (uint32_t i = 0; i < engines_.size(); ++i) {
        while (engines_[i]->pop_event(ev)) {
            handle_event(i, ev);
            ++n;
        }
    }
    return n;
}

void GatewayCore::handle_event(uint32_t symbol_index, const ExecEvent& ev) {
    auto it = sessions_.find(ev.session_id);
    if (it == sessions_.end()) return;   // session closed; nobody to tell
    Session& s = *it->second;

    if (ev.kind == ExecEvent::CANCEL_REJECT) {
        return send_cancel_reject(ev.session_id, s, ev.order_id, ev.client_order_id,
                                  ev.reason, ev.response_to);
    }

    if (ev.is_terminal()) {
        // The client order id becomes reusable once the order is done.
        const LiveOrder* live = s.live.find(ev.client_order_id);
        if (live && live->order_id == ev.order_id) s.live.erase(ev.client_order_id);
    }

    ExecReportMessage rep{};
    rep.order_id        = ev.order_id;
    rep.client_order_id = ev.client_order_id;
    rep.trade_id        = ev.trade_id;
    std::memcpy(rep.symbol, symbols_[symbol_index].data(), sizeof(rep.symbol));
    rep.price           = ev.price;
    rep.last_px         = ev.last_px;
    rep.last_qty        = ev.last_qty;
    rep.leaves_qty      = ev.leaves_qty;
    rep.cum_qty         = ev.cum_qty;
    rep.exec_type       = static_cast<uint8_t>(ev.exec_type);
    rep.side            = static_cast<uint8_t>(ev.side);
    rep.reject_reason   = static_cast<uint8_t>(ev.reason);
    send(ev.session_id, s, rep, MessageType::EXEC_REPORT);
}

} // namespace rtes
