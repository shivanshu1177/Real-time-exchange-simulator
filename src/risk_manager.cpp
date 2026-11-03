#include "rtes/risk_manager.hpp"
#include "rtes/logger.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace rtes {

namespace {
const char* reason_name(RejectReason r) {
    switch (r) {
        case RejectReason::UNKNOWN_SYMBOL:     return "UNKNOWN_SYMBOL";
        case RejectReason::INVALID_QUANTITY:   return "INVALID_QUANTITY";
        case RejectReason::INVALID_PRICE:      return "INVALID_PRICE";
        case RejectReason::INVALID_SIDE:       return "INVALID_SIDE";
        case RejectReason::INVALID_ORDER_TYPE: return "INVALID_ORDER_TYPE";
        case RejectReason::ORDER_SIZE_LIMIT:   return "ORDER_SIZE_LIMIT";
        case RejectReason::PRICE_COLLAR:       return "PRICE_COLLAR";
        case RejectReason::RATE_LIMIT:         return "RATE_LIMIT";
        case RejectReason::NOTIONAL_LIMIT:     return "NOTIONAL_LIMIT";
        case RejectReason::DUPLICATE_ORDER_ID: return "DUPLICATE_ORDER_ID";
        case RejectReason::UNKNOWN_ORDER:      return "UNKNOWN_ORDER";
        case RejectReason::NOT_OWNER:          return "NOT_OWNER";
        case RejectReason::TOO_LATE:           return "TOO_LATE";
        case RejectReason::SYSTEM_BUSY:        return "SYSTEM_BUSY";
        case RejectReason::NOT_LOGGED_IN:      return "NOT_LOGGED_IN";
        case RejectReason::OFF_TICK:           return "OFF_TICK";
        case RejectReason::OFF_LOT:            return "OFF_LOT";
        case RejectReason::OPEN_ORDER_LIMIT:   return "OPEN_ORDER_LIMIT";
        default:                               return "OTHER";
    }
}
} // namespace

RiskManager::RiskManager(const RiskConfig& risk, const std::vector<SymbolConfig>& symbols)
    : risk_config_(risk),
      max_notional_(static_cast<uint64_t>(std::llround(risk.max_notional_per_client * 10000.0)))
{
    for (const auto& sc : symbols) {
        collar_bps_.push_back(static_cast<uint64_t>(std::llround(sc.price_collar_pct * 100.0)));
        reference_.push_back(static_cast<Price>(std::llround(sc.ref_price * 10000.0)));
        const auto tick = static_cast<Price>(std::llround(sc.tick_size * 10000.0));
        tick_.push_back(tick > 0 ? tick : 1);
        lot_.push_back(sc.lot_size > 0 ? static_cast<Quantity>(sc.lot_size) : 1);
    }
}

RejectReason RiskManager::check_static(uint32_t symbol_index, OrderType type,
                                       Price price, Quantity quantity) const {
    if (quantity == 0) return RejectReason::INVALID_QUANTITY;
    if (quantity % lot_[symbol_index] != 0) return RejectReason::OFF_LOT;
    if (type == OrderType::LIMIT && price % tick_[symbol_index] != 0) return RejectReason::OFF_TICK;
    if (quantity > risk_config_.max_order_size) return RejectReason::ORDER_SIZE_LIMIT;

    // Collar around the last trade (or the configured reference before the
    // first trade). Integer basis points: no rounding drift.
    const Price ref = reference_[symbol_index];
    if (risk_config_.price_collar_enabled && type == OrderType::LIMIT && ref > 0) {
        const uint64_t delta = ref * collar_bps_[symbol_index] / 10000;
        const uint64_t lo    = ref > delta ? ref - delta : 0;
        const uint64_t hi    = ref + delta;
        if (price < lo || price > hi) return RejectReason::PRICE_COLLAR;
    }
    return RejectReason::NONE;
}

RejectReason RiskManager::check_new_order(ClientID client, uint32_t symbol_index, OrderType type,
                                          Price price, Quantity quantity, Price& notional_basis) {
    auto reject = [](RejectReason r) { return r; };   // the gateway counts rejects (record_reject)

    if (auto r = check_static(symbol_index, type, price, quantity); r != RejectReason::NONE) {
        return reject(r);
    }

    // Market orders are valued at the reference price.
    notional_basis = (type == OrderType::LIMIT) ? price : reference_[symbol_index];
    if (notional_basis > std::numeric_limits<uint64_t>::max() / quantity) {
        return reject(RejectReason::NOTIONAL_LIMIT);   // would overflow: certainly above any limit
    }
    const uint64_t notional = notional_basis * quantity;

    auto it = clients_.find(client);
    const uint64_t open   = it == clients_.end() ? 0 : it->second.open_notional;
    const uint32_t orders = it == clients_.end() ? 0 : it->second.open_orders;
    if (orders >= risk_config_.max_open_orders_per_client) return reject(RejectReason::OPEN_ORDER_LIMIT);
    if (notional > max_notional_ || open > max_notional_ - notional) return reject(RejectReason::NOTIONAL_LIMIT);

    ClientState& st = (it == clients_.end()) ? clients_[client] : it->second;
    st.open_notional += notional;
    st.open_orders   += 1;
    orders_accepted_.fetch_add(1, std::memory_order_relaxed);
    return RejectReason::NONE;
}

RejectReason RiskManager::check_modify(ClientID client, uint32_t symbol_index, Price new_price,
                                       Quantity new_quantity, uint64_t open_now, uint64_t open_after) {
    if (auto r = check_static(symbol_index, OrderType::LIMIT, new_price, new_quantity);
        r != RejectReason::NONE) {
        return r;
    }
    if (open_after > open_now) {
        const uint64_t open = open_notional(client);
        const uint64_t extra = open_after - open_now;
        if (extra > max_notional_ || open > max_notional_ - extra) return RejectReason::NOTIONAL_LIMIT;
    }
    return RejectReason::NONE;
}

void RiskManager::release(ClientID client, uint64_t notional, bool order_done) {
    auto it = clients_.find(client);
    if (it == clients_.end()) return;
    ClientState& st = it->second;
    st.open_notional -= std::min(notional, st.open_notional);
    if (order_done && st.open_orders > 0) --st.open_orders;
    // Keep the table bounded: forget clients with nothing open.
    if (st.open_orders == 0) clients_.erase(it);
}

void RiskManager::adjust(ClientID client, uint64_t open_before, uint64_t open_after) {
    auto it = clients_.find(client);
    if (it == clients_.end()) return;
    ClientState& st = it->second;
    if (open_after >= open_before) st.open_notional += open_after - open_before;
    else st.open_notional -= std::min(open_before - open_after, st.open_notional);
}

void RiskManager::on_trade(uint32_t symbol_index, Price price) {
    if (symbol_index < reference_.size() && price > 0) reference_[symbol_index] = price;
}

uint64_t RiskManager::open_notional(ClientID client) const {
    auto it = clients_.find(client);
    return it == clients_.end() ? 0 : it->second.open_notional;
}

uint32_t RiskManager::open_orders(ClientID client) const {
    auto it = clients_.find(client);
    return it == clients_.end() ? 0 : it->second.open_orders;
}

void RiskManager::record_reject(RejectReason r) {
    const auto i = static_cast<uint8_t>(r);
    if (i >= kReasons) return;
    ++reject_totals_[i];
    ++reject_pending_[i];
    orders_rejected_.fetch_add(1, std::memory_order_relaxed);
}

size_t RiskManager::flush_reject_log(uint64_t now_ns) {
    if (last_log_ns_ != 0 && now_ns - last_log_ns_ < 1'000'000'000ULL) return 0;
    std::string line;
    for (size_t i = 0; i < kReasons; ++i) {
        if (reject_pending_[i] == 0) continue;
        line += ' ';
        line += reason_name(static_cast<RejectReason>(i));
        line += '=';
        line += std::to_string(reject_pending_[i]);
        reject_pending_[i] = 0;
    }
    if (line.empty()) return 0;   // nothing to report: do not start a new window
    last_log_ns_ = now_ns;
    LOG_WARN("Rejects since last report:" + line);
    return 1;
}

} // namespace rtes
