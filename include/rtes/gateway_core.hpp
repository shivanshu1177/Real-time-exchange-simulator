#pragma once

#include "rtes/protocol.hpp"
#include "rtes/types.hpp"
#include "rtes/flat_map.hpp"
#include "rtes/memory_pool.hpp"
#include "rtes/risk_manager.hpp"
#include "rtes/matching_engine.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace rtes {

// Where the gateway core writes outbound messages (one complete frame per call).
class OutputSink {
public:
    virtual ~OutputSink() = default;
    virtual void send(SessionId session, const void* data, size_t len) = 0;
    // Close the session after what was already sent has been flushed.
    virtual void disconnect(SessionId session) = 0;
};

// Order-entry logic without sockets: validation, risk, per-session order ids,
// routing to engines, and turning engine events into execution reports.
// Single-threaded: every method must be called from the gateway thread.
class GatewayCore {
public:
    GatewayCore(RiskManager& risk,
                std::vector<MatchingEngine*> engines,   // index = symbol index
                OutputSink& out,
                size_t max_live_orders_per_session = 16384,
                bool cancel_on_disconnect = true);

    // Session lifecycle. Allocation happens here, not per order.
    // A new session must LOGON before it may send orders.
    SessionId open_session();
    // The connection is gone. With cancel-on-disconnect, its live orders are
    // cancelled. The session's state is kept, silently, until its last order
    // is done, so the engines' reports still release its risk exposure.
    void      close_session(SessionId session);
    // Send SESSION_REJECT and ask the transport to close the connection.
    void      reject_session(SessionId session, SessionRejectReason reason, uint32_t detail);
    bool      has_session(SessionId session) const {
        auto it = sessions_.find(session);
        return it != sessions_.end() && !it->second->closed;
    }
    size_t    tracked_sessions() const { return sessions_.size(); }   // includes closed ones with live orders
    size_t    live_orders(SessionId session) const;

    // Handle one complete inbound frame that already passed check_inbound_frame().
    void on_message(SessionId session, const uint8_t* data, size_t len);

    void on_logon(SessionId session, const LogonMessage& msg);
    void on_new_order(SessionId session, const NewOrderMessage& msg);
    void on_cancel(SessionId session, const CancelOrderMessage& msg);
    void on_modify(SessionId session, const ModifyOrderMessage& msg);

    // Turn pending engine events into outbound messages. Returns the number handled.
    size_t drain_events();

    // Periodic housekeeping from the transport loop (reject log summary).
    void on_tick(uint64_t now_ns);

    // Exchange order ids carry the symbol index in the top 16 bits so cancels
    // and modifies by order id can be routed without a lookup.
    static constexpr OrderID make_order_id(uint32_t symbol_index, uint64_t seq) noexcept {
        return (static_cast<uint64_t>(symbol_index + 1) << 48) | (seq & ((1ULL << 48) - 1));
    }
    static constexpr int64_t symbol_index_of(OrderID id) noexcept {
        return static_cast<int64_t>(id >> 48) - 1;
    }

private:
    struct LiveOrder {
        OrderID  order_id{0};
        uint32_t symbol_index{0};
        Price    notional_basis{0};   // price risk values this order at
        Quantity quantity{0};         // current total quantity
        Quantity leaves{0};           // still open
        uint64_t open_notional() const { return notional_basis * leaves; }
    };
    struct Session {
        Session(size_t max_live, double rate, double burst) : live(max_live), bucket(rate, burst) {}
        FlatMap<LiveOrder> live;      // client_order_id -> live order
        TokenBucket bucket;           // order rate limit
        uint64_t next_out_seq{1};
        bool     logged_in{false};
        bool     closed{false};       // connection gone; kept until `live` is empty
        ClientID client_id{0};        // bound at LOGON; payload client_id is ignored
    };

    RiskManager&                  risk_;
    std::vector<MatchingEngine*>  engines_;
    std::vector<std::array<char, 8>> symbols_;  // NUL-padded, by symbol index
    std::vector<uint64_t>         next_order_seq_;  // by symbol index
    OutputSink&                   out_;
    size_t                        max_live_orders_;
    bool                          cancel_on_disconnect_;

    std::unordered_map<SessionId, std::unique_ptr<Session>> sessions_;
    SessionId next_session_id_{1};

    int  find_symbol(const char (&symbol)[8]) const noexcept;
    bool resolve(Session& s, uint64_t order_id, uint64_t client_order_id,
                 OrderRequest& req, uint32_t& symbol_index) const noexcept;

    void send_reject(SessionId session, Session& s, const NewOrderMessage& msg, RejectReason r);
    void send_cancel_reject(SessionId session, Session& s, uint64_t order_id,
                            uint64_t client_order_id, RejectReason r,
                            CancelRejectResponseTo to);
    void handle_event(uint32_t symbol_index, const ExecEvent& ev);

    template<typename Msg>
    void send(SessionId session, Session& s, Msg& msg, MessageType type);
};

} // namespace rtes
