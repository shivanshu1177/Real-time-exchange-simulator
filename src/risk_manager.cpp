#include "rtes/risk_manager.hpp"
#include "rtes/logger.hpp"

namespace rtes {

RiskManager::RiskManager(const RiskConfig& risk,
                         const std::vector<SymbolConfig>& symbols)
    : risk_config_(risk)
{
    for (const auto& sc : symbols) {
        price_collar_pct_.push_back(sc.price_collar_pct);
        // Default reference price: $150.00 expressed in the fixed-point unit
        // The protocol uses price * 10000 (i.e. 150.00 -> 1500000)
        reference_prices_.push_back(1500000ULL);
    }
}

RejectReason RiskManager::reject(RejectReason r) {
    orders_rejected_.fetch_add(1, std::memory_order_relaxed);
    return r;
}

RejectReason RiskManager::check_size_and_collar(uint32_t symbol_index, OrderType type,
                                                Price price, Quantity quantity) {
    // Size limit
    if (quantity > risk_config_.max_order_size) {
        LOG_WARN("Risk: order size " + std::to_string(quantity) +
                 " exceeds limit " + std::to_string(risk_config_.max_order_size));
        return RejectReason::ORDER_SIZE_LIMIT;
    }

    // Price collar (only for LIMIT orders)
    if (risk_config_.price_collar_enabled && type == OrderType::LIMIT && price > 0) {
        auto ref = reference_prices_[symbol_index];
        auto pct = price_collar_pct_[symbol_index];
        uint64_t delta = static_cast<uint64_t>(static_cast<double>(ref) * pct / 100.0);
        uint64_t lo    = (ref > delta) ? (ref - delta) : 0;
        uint64_t hi    = ref + delta;
        if (price < lo || price > hi) {
            LOG_WARN("Risk: price " + std::to_string(price) +
                     " outside collar [" + std::to_string(lo) + "," +
                     std::to_string(hi) + "]");
            return RejectReason::PRICE_COLLAR;
        }
    }
    return RejectReason::NONE;
}

RejectReason RiskManager::check_new_order(ClientID client_id, uint32_t symbol_index,
                                          OrderType type, Price price, Quantity quantity) {
    if (auto r = check_size_and_collar(symbol_index, type, price, quantity);
        r != RejectReason::NONE) {
        return reject(r);
    }

    // Rate limit (per client)
    auto& state = client_states_[client_id];
    auto  now   = std::chrono::steady_clock::now();
    auto  elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - state.last_reset_time).count();
    if (elapsed_ms >= 1000) {
        state.order_count_last_second = 0;
        state.last_reset_time = now;
    }
    if (state.order_count_last_second >= risk_config_.max_orders_per_second) {
        LOG_WARN("Risk: rate limit exceeded for client " + std::to_string(client_id));
        return reject(RejectReason::RATE_LIMIT);
    }
    ++state.order_count_last_second;

    // Notional exposure
    // price is in fixed-point *10000, so actual price = price / 10000.0
    double notional = (static_cast<double>(price) / 10000.0) * quantity;
    if (state.notional_exposure + notional > risk_config_.max_notional_per_client) {
        LOG_WARN("Risk: notional limit exceeded for client " + std::to_string(client_id));
        return reject(RejectReason::NOTIONAL_LIMIT);
    }
    state.notional_exposure += notional;

    orders_accepted_.fetch_add(1, std::memory_order_relaxed);
    return RejectReason::NONE;
}

RejectReason RiskManager::check_modify(ClientID /*client_id*/, uint32_t symbol_index,
                                       Price new_price, Quantity new_quantity) {
    if (auto r = check_size_and_collar(symbol_index, OrderType::LIMIT, new_price, new_quantity);
        r != RejectReason::NONE) {
        return reject(r);
    }
    return RejectReason::NONE;
}

} // namespace rtes
