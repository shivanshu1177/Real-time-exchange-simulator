#include "rtes/exchange.hpp"
#include "rtes/tcp_gateway.hpp"
#include "rtes/udp_publisher.hpp"
#include "rtes/config.hpp"
#include "rtes/logger.hpp"
#include <atomic>
#include <csignal>
#include <thread>
#include <chrono>
#include <iostream>

static std::atomic<bool> g_shutdown{false};

static void signal_handler(int /*sig*/) {
    g_shutdown.store(true, std::memory_order_relaxed);
}

int main(int argc, char* argv[]) {
    const std::string config_path =
        (argc >= 2) ? argv[1] : "configs/config.json";

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    LOG_INFO("RTES: loading config from " + config_path);
    auto config = rtes::Config::load_from_file(config_path);

    const auto& exc_cfg = config->exchange;

    LOG_INFO("RTES: initialising exchange");
    rtes::Exchange exchange(std::move(config));
    exchange.start();

    LOG_INFO("RTES: starting TCP gateway on port " +
             std::to_string(exc_cfg.tcp_port));
    rtes::TcpGateway gateway(exc_cfg.tcp_port,
                              *exchange.get_risk_manager(),
                              *exchange.get_order_pool(),
                              exchange.get_engines());
    gateway.start();

    LOG_INFO("RTES: starting UDP publisher on " +
             exc_cfg.udp_multicast_group + ":" +
             std::to_string(exc_cfg.udp_port));
    rtes::UdpPublisher udp(exc_cfg.udp_multicast_group,
                            exc_cfg.udp_port,
                            exchange.get_market_data_queue());
    udp.start();

    LOG_INFO("RTES: exchange running — press Ctrl+C to stop");

    while (!g_shutdown.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOG_INFO("RTES: shutdown signal received — stopping");
    udp.stop();
    gateway.stop();
    exchange.stop();

    LOG_INFO("RTES: shutdown complete");
    return 0;
}
