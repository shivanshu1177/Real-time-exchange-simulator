#pragma once

#include "rtes/types.hpp"
#include "rtes/config.hpp"
#include "rtes/protocol.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace rtes {

// Per-session order rate limit. Starts full; refills at `rate` tokens per
// second up to `burst`. Time is passed in, so it is deterministic in tests.
class TokenBucket {
public:
    TokenBucket(double rate_per_second, double burst)
        : rate_(rate_per_second), burst_(burst), tokens_(burst) {}

    bool try_take(uint64_t now_ns) noexcept {
        if (last_ns_ != 0 && now_ns > last_ns_) {
            tokens_ += static_cast<double>(now_ns - last_ns_) * 1e-9 * rate_;
            if (tokens_ > burst_) tokens_ = burst_;
        }
        last_ns_ = now_ns;
        if (tokens_ < 1.0) return false;
        tokens_ -= 1.0;
        return true;
    }

private:
    double   rate_;
    double   burst_;
    double   tokens_;
    uint64_t last_ns_{0};
};

// Pre-trade risk. Runs synchronously on the gateway thread, so the client's
// first response already reflects the risk decision. Not thread-safe: only
// the gateway thread may call it.
//
// Exposure is tracked per client as open notional (fixed-point price x
// quantity, exact integers) and open order count. It is reserved when an order
// is accepted and released by the engine's reports: fills, cancels, rejects.
class RiskManager {
public:
    RiskManager(const RiskConfig& risk, const std::vector<SymbolConfig>& symbols);

    // symbol_index is the position of the symbol in the config's symbol list.
    // On success (NONE) the order's exposure is reserved and `notional_basis`
    // is the price used for it (the limit price, or the reference price for a
    // market order); release it with release().
    RejectReason check_new_order(ClientID client, uint32_t symbol_index, OrderType type,
                                 Price price, Quantity quantity, Price& notional_basis);

    // Checks a modify. open_now / open_after: the order's open notional before
    // and after. Nothing is reserved; apply the change with adjust() when the
    // engine confirms it.
    RejectReason check_modify(ClientID client, uint32_t symbol_index, Price new_price,
                              Quantity new_quantity, uint64_t open_now, uint64_t open_after);

    // Exposure that is no longer open: a fill, or the rest of a cancelled or
    // rejected order. order_done: the order has reached a final state.
    void release(ClientID client, uint64_t notional, bool order_done);
    // An accepted modify changed the order's open notional.
    void adjust(ClientID client, uint64_t open_before, uint64_t open_after);

    // A trade happened: it becomes the symbol's collar reference.
    void on_trade(uint32_t symbol_index, Price price);
    Price reference_price(uint32_t symbol_index) const { return reference_[symbol_index]; }

    // Every reject the gateway sends is counted here instead of logged one by
    // one (the check_* methods only return the reason).
    void record_reject(RejectReason r);
    // Emit at most one summary line per second covering the rejects since the
    // last line. Returns the number of lines written (0 or 1).
    size_t flush_reject_log(uint64_t now_ns);

    const RiskConfig& config() const { return risk_config_; }
    uint64_t open_notional(ClientID client) const;
    uint32_t open_orders(ClientID client) const;
    uint64_t reject_count(RejectReason r) const { return reject_totals_[static_cast<uint8_t>(r)]; }
    size_t   tracked_clients() const { return clients_.size(); }

    uint64_t orders_accepted() const { return orders_accepted_.load(std::memory_order_relaxed); }
    uint64_t orders_rejected() const { return orders_rejected_.load(std::memory_order_relaxed); }

private:
    struct ClientState {
        uint64_t open_notional{0};   // fixed-point price units x quantity
        uint32_t open_orders{0};
    };

    RiskConfig            risk_config_;
    uint64_t              max_notional_;     // fixed-point
    std::vector<uint64_t> collar_bps_;       // by symbol index
    std::vector<Price>    reference_;        // by symbol index; 0 = none yet
    std::vector<Price>    tick_;             // by symbol index, fixed-point (0.01 -> 100)
    std::vector<Quantity> lot_;              // by symbol index

    std::unordered_map<ClientID, ClientState> clients_;   // erased when nothing is open

    static constexpr size_t kReasons = 32;
    std::array<uint64_t, kReasons> reject_totals_{};
    std::array<uint64_t, kReasons> reject_pending_{};     // since the last log line
    uint64_t last_log_ns_{0};

    std::atomic<uint64_t> orders_accepted_{0};
    std::atomic<uint64_t> orders_rejected_{0};

    // Checks that do not depend on client state: lot, tick, size, collar.
    RejectReason check_static(uint32_t symbol_index, OrderType type,
                              Price price, Quantity quantity) const;
};

} // namespace rtes
