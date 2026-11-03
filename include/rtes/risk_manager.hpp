#pragma once

#include "rtes/types.hpp"
#include "rtes/config.hpp"
#include "rtes/protocol.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace rtes {

// Pre-trade risk. Runs synchronously on the gateway thread, so the client's
// first response already reflects the risk decision. Not thread-safe: only
// the gateway thread may call it.
class RiskManager {
public:
    RiskManager(const RiskConfig& risk, const std::vector<SymbolConfig>& symbols);

    // symbol_index is the position of the symbol in the config's symbol list.
    // Returns RejectReason::NONE if the order may proceed.
    RejectReason check_new_order(ClientID client_id, uint32_t symbol_index,
                                 OrderType type, Price price, Quantity quantity);

    // Checks the new price and quantity of a modify request.
    RejectReason check_modify(ClientID client_id, uint32_t symbol_index,
                              Price new_price, Quantity new_quantity);

    uint64_t orders_accepted() const { return orders_accepted_.load(std::memory_order_relaxed); }
    uint64_t orders_rejected() const { return orders_rejected_.load(std::memory_order_relaxed); }

private:
    struct ClientRiskState {
        uint32_t order_count_last_second{0};
        std::chrono::steady_clock::time_point last_reset_time{std::chrono::steady_clock::now()};
        double notional_exposure{0.0};
    };

    RiskConfig risk_config_;
    std::vector<double>   price_collar_pct_;   // by symbol index
    std::vector<uint64_t> reference_prices_;   // by symbol index, fixed-point
    std::vector<Price>    tick_;               // by symbol index, fixed-point (0.01 -> 100)
    std::vector<Quantity> lot_;                // by symbol index

    std::unordered_map<ClientID, ClientRiskState> client_states_;

    std::atomic<uint64_t> orders_accepted_{0};
    std::atomic<uint64_t> orders_rejected_{0};

    // Checks that do not depend on client state: lot, tick, size, collar.
    RejectReason check_static(uint32_t symbol_index, OrderType type,
                              Price price, Quantity quantity);
    RejectReason reject(RejectReason r);
};

} // namespace rtes
