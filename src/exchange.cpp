#include "rtes/exchange.hpp"
#include "rtes/logger.hpp"

namespace rtes {

Exchange::Exchange(std::unique_ptr<Config> config)
    : config_(std::move(config))
{
    initialize_risk_manager();
    initialize_matching_engines();
    initialize_market_data();
    wire_components();
}

Exchange::~Exchange() { stop(); }

void Exchange::initialize_risk_manager() {
    risk_manager_ = std::make_unique<RiskManager>(config_->risk, config_->symbols);
    LOG_INFO("Exchange: risk manager initialised");
}

void Exchange::initialize_matching_engines() {
    uint32_t index = 0;
    for (const auto& sym : config_->symbols) {
        // Each engine owns a pool of order_pool_size orders for its symbol.
        matching_engines_.push_back(std::make_unique<MatchingEngine>(
            sym.symbol, index++, config_->performance.queue_capacity,
            config_->performance.order_pool_size, sym.stp_mode));
        LOG_INFO("Exchange: matching engine initialised for " + sym.symbol);
    }
}

std::vector<MatchingEngine*> Exchange::get_engines() {
    std::vector<MatchingEngine*> out;
    for (auto& e : matching_engines_) out.push_back(e.get());
    return out;
}

void Exchange::initialize_market_data() {
    market_data_queue_ =
        std::make_unique<MPMCQueue<MarketDataEvent>>(config_->performance.queue_capacity);
    LOG_INFO("Exchange: market data queue initialised (capacity=" +
             std::to_string(config_->performance.queue_capacity) + ")");
}

void Exchange::wire_components() {
    for (auto& engine : matching_engines_) {
        engine->set_market_data_queue(market_data_queue_.get());
    }
    LOG_INFO("Exchange: components wired");
}

void Exchange::start() {
    LOG_INFO("Exchange: starting threads");
    for (auto& engine : matching_engines_) {
        engine->start();
        LOG_INFO("Exchange: matching engine started for " + engine->symbol());
    }
    LOG_INFO("Exchange: ready");
}

void Exchange::stop() {
    LOG_INFO("Exchange: stopping");
    for (auto& engine : matching_engines_) engine->stop();
    LOG_INFO("Exchange: stopped");
}

} // namespace rtes
