#include "rtes/udp_publisher.hpp"
#include "rtes/market_data.hpp"
#include "rtes/logger.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <chrono>

namespace rtes {

UdpPublisher::UdpPublisher(const std::string& multicast_group, uint16_t port,
                            MPMCQueue<MarketDataEvent>* input_queue)
    : multicast_group_(multicast_group), port_(port), input_queue_(input_queue)
{}

UdpPublisher::~UdpPublisher() { stop(); }

bool UdpPublisher::setup_multicast_socket() {
    socket_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        LOG_ERROR("UdpPublisher: socket() failed: " + std::string(std::strerror(errno)));
        return false;
    }

    // Set multicast TTL to 1 (local network only)
    int ttl = 1;
    if (::setsockopt(socket_fd_, IPPROTO_IP, IP_MULTICAST_TTL,
                     &ttl, sizeof(ttl)) < 0) {
        LOG_WARN("UdpPublisher: setsockopt IP_MULTICAST_TTL failed");
    }

    // Force outgoing multicast packets through the loopback interface so that
    // receivers on the same host see them (macOS routes 239.x.x.x via en0 by
    // default, which prevents loopback delivery unless we pin the interface).
    in_addr lo{};
    lo.s_addr = ::inet_addr("127.0.0.1");
    ::setsockopt(socket_fd_, IPPROTO_IP, IP_MULTICAST_IF, &lo, sizeof(lo));

    // Configure destination address
    std::memset(&multicast_addr_, 0, sizeof(multicast_addr_));
    multicast_addr_.sin_family      = AF_INET;
    multicast_addr_.sin_addr.s_addr = ::inet_addr(multicast_group_.c_str());
    multicast_addr_.sin_port        = htons(port_);

    LOG_INFO("UdpPublisher: ready on " + multicast_group_ + ":" + std::to_string(port_));
    return true;
}

void UdpPublisher::start() {
    if (!setup_multicast_socket()) return;
    running_.store(true, std::memory_order_release);
    worker_thread_ = std::thread(&UdpPublisher::worker_loop, this);
}

void UdpPublisher::stop() {
    running_.store(false, std::memory_order_release);
    if (worker_thread_.joinable()) worker_thread_.join();
    if (socket_fd_ >= 0) { ::close(socket_fd_); socket_fd_ = -1; }
}

void UdpPublisher::worker_loop() {
    while (running_.load(std::memory_order_acquire)) {
        MarketDataEvent ev;
        if (input_queue_->pop(ev)) {
            process_market_data_event(ev);
        }
    }
    // Drain remaining events
    MarketDataEvent ev;
    while (input_queue_->pop(ev)) process_market_data_event(ev);
}

void UdpPublisher::process_market_data_event(const MarketDataEvent& ev) {
    if (ev.type == MarketDataEvent::TRADE) {
        send_trade_update(ev);
    } else {
        send_bbo_update(ev);
    }
}

void UdpPublisher::send_bbo_update(const MarketDataEvent& ev) {
    BboUpdateMessage msg{};
    std::memcpy(msg.symbol, ev.symbol, sizeof(msg.symbol) - 1);  // last byte stays NUL
    msg.bid_price    = ev.bbo.bid_price;
    msg.bid_quantity = ev.bbo.bid_quantity;
    msg.ask_price    = ev.bbo.ask_price;
    msg.ask_quantity = ev.bbo.ask_quantity;

    seal_message(msg, MessageType::BBO_UPDATE,
                 next_sequence_.fetch_add(1, std::memory_order_relaxed),
                 get_timestamp_ns());
    send_message(&msg, sizeof(msg));
}

void UdpPublisher::send_trade_update(const MarketDataEvent& ev) {
    TradeUpdateMessage msg{};
    msg.trade_id      = ev.trade.trade_id;      // TradeData fields (POD)
    msg.buy_order_id  = ev.trade.buy_order_id;
    msg.sell_order_id = ev.trade.sell_order_id;
    std::memcpy(msg.symbol, ev.symbol, sizeof(msg.symbol) - 1);  // last byte stays NUL
    msg.symbol[sizeof(msg.symbol) - 1] = '\0';
    msg.price    = ev.trade.price;
    msg.quantity = ev.trade.quantity;

    seal_message(msg, MessageType::TRADE_REPORT,
                 next_sequence_.fetch_add(1, std::memory_order_relaxed),
                 get_timestamp_ns());
    send_message(&msg, sizeof(msg));
}

bool UdpPublisher::send_message(const void* message, size_t size) {
    ssize_t sent = ::sendto(socket_fd_, message, size, 0,
                            reinterpret_cast<const sockaddr*>(&multicast_addr_),
                            sizeof(multicast_addr_));
    if (sent < 0) {
        LOG_WARN("UdpPublisher: sendto failed: " + std::string(std::strerror(errno)));
        return false;
    }
    messages_sent_.fetch_add(1, std::memory_order_relaxed);
    bytes_sent_.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed);
    return true;
}

uint64_t UdpPublisher::get_timestamp_ns() const {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace rtes
