#pragma once

#include "rtes/config.hpp"
#include "rtes/memory_pool.hpp"
#include "rtes/risk_manager.hpp"
#include "rtes/matching_engine.hpp"
#include "rtes/mpmc_queue.hpp"
#include <memory>
#include <vector>
#include <string>

namespace rtes {

class Exchange {
public:
    explicit Exchange(std::unique_ptr<Config> config);
    ~Exchange();

    void start();
    void stop();

    // Accessors for wiring to TcpGateway / UdpPublisher
    RiskManager* get_risk_manager() { return risk_manager_.get(); }
    OrderPool*   get_order_pool()   { return order_pool_.get(); }
    // Engines in config symbol order (index = symbol index).
    std::vector<MatchingEngine*> get_engines();
    MPMCQueue<MarketDataEvent>* get_market_data_queue() { return market_data_queue_.get(); }
    const Config& get_config() const { return *config_; }

private:
    std::unique_ptr<Config>  config_;
    std::unique_ptr<OrderPool> order_pool_;
    std::unique_ptr<RiskManager> risk_manager_;
    std::vector<std::unique_ptr<MatchingEngine>> matching_engines_;   // config symbol order
    std::unique_ptr<MPMCQueue<MarketDataEvent>> market_data_queue_;

    void initialize_order_pool();
    void initialize_risk_manager();
    void initialize_matching_engines();
    void initialize_market_data();
    void wire_components();
};

} // namespace rtes
