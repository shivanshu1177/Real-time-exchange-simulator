#pragma once

#include <string>
#include <vector>
#include <memory>

namespace rtes {

struct SymbolConfig {
    std::string symbol;
    double tick_size;
    uint64_t lot_size;
    double price_collar_pct;
};

struct ExchangeConfig {
    std::string name;
    uint16_t tcp_port;
    std::string udp_multicast_group;
    uint16_t udp_port;
    uint16_t metrics_port;
    bool cancel_on_disconnect{true};  // cancel a session's resting orders when it disconnects
};

struct RiskConfig {
    uint64_t max_order_size;
    double max_notional_per_client;
    uint32_t max_orders_per_second;
    bool price_collar_enabled;
};

struct PerformanceConfig {
    uint32_t order_pool_size;
    uint32_t queue_capacity;
    bool enable_cpu_pinning;
    bool tcp_nodelay;
    uint32_t udp_buffer_size;
};

struct LoggingConfig {
    std::string level;
    uint32_t rate_limit_ms;
    bool enable_structured;
};

struct PersistenceConfig {
    bool enable_event_log;
    uint32_t snapshot_interval_ms;
    std::string log_directory;
};

struct Config {
    ExchangeConfig exchange;
    std::vector<SymbolConfig> symbols;
    RiskConfig risk;
    PerformanceConfig performance;
    LoggingConfig logging;
    PersistenceConfig persistence;
    
    static std::unique_ptr<Config> load_from_file(const std::string& path);
};

} // namespace rtes