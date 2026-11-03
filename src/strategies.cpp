#include "rtes/strategies.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <chrono>
#include <thread>

namespace rtes {

// ---- helpers ---------------------------------------------------------------

static int tcp_connect(const std::string& host, uint16_t port) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    auto port_str = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
        return -1;
    }

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { ::freeaddrinfo(res); return -1; }

    if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        ::close(fd);
        ::freeaddrinfo(res);
        return -1;
    }
    ::freeaddrinfo(res);
    return fd;
}

static bool send_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, p + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

static bool recv_all(int fd, void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, p + got, len - got, 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

static uint64_t now_ns_impl() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ---- LiquidityTakerStrategy -----------------------------------------------

LiquidityTakerStrategy::LiquidityTakerStrategy(std::string host, uint16_t port,
                                                 std::string symbol)
    : host_(std::move(host)), port_(port), symbol_(std::move(symbol)) {}

LiquidityTakerStrategy::~LiquidityTakerStrategy() { disconnect(); }

bool LiquidityTakerStrategy::connect() {
    sock_fd_ = tcp_connect(host_, port_);
    return sock_fd_ >= 0;
}

void LiquidityTakerStrategy::disconnect() {
    if (sock_fd_ >= 0) { ::close(sock_fd_); sock_fd_ = -1; }
}

double LiquidityTakerStrategy::avg_latency_us() const {
    auto cnt = orders_acked_.load();
    if (cnt == 0) return 0.0;
    return static_cast<double>(total_latency_us_.load()) / cnt;
}

uint64_t LiquidityTakerStrategy::now_ns() { return now_ns_impl(); }

bool LiquidityTakerStrategy::send_order(uint8_t side, uint64_t price, uint32_t quantity) {
    NewOrderMessage msg{};
    msg.client_order_id = next_order_id_++;
    msg.client_id       = 1001;
    // msg is zero-initialised, so this null-pads the 8-byte wire field
    std::memcpy(msg.symbol, symbol_.data(), std::min(symbol_.size(), sizeof(msg.symbol)));
    msg.side       = side;
    msg.order_type = 1; // LIMIT
    msg.price      = price;
    msg.quantity   = quantity;

    seal_message(msg, MessageType::NEW_ORDER, next_sequence_++, now_ns());
    return send_all(sock_fd_, &msg, sizeof(msg));
}

bool LiquidityTakerStrategy::recv_ack() {
    MessageHeader hdr{};
    if (!recv_all(sock_fd_, &hdr, sizeof(hdr))) return false;
    // Read body
    uint32_t body = hdr.length > sizeof(hdr)
                    ? hdr.length - static_cast<uint32_t>(sizeof(hdr)) : 0;
    if (body > 0) {
        std::vector<uint8_t> buf(body);
        if (!recv_all(sock_fd_, buf.data(), body)) return false;
    }

    auto type = static_cast<MessageType>(hdr.type);
    if (type == MessageType::ORDER_ACK) {
        // Need to peek at status — it's in the full OrderAckMessage
        // We already read it into buf above if body > 0
        orders_acked_.fetch_add(1, std::memory_order_relaxed);
    } else if (type == MessageType::TRADE_REPORT) {
        trades_received_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void LiquidityTakerStrategy::run(std::chrono::seconds duration) {
    std::uniform_int_distribution<int>      side_dist(0, 1);
    std::uniform_int_distribution<uint64_t> price_dist(1490000, 1510000); // $149–$151
    std::uniform_int_distribution<uint32_t> qty_dist(1, 100);

    auto deadline = std::chrono::steady_clock::now() + duration;

    while (std::chrono::steady_clock::now() < deadline) {
        uint8_t  side  = static_cast<uint8_t>(side_dist(rng_));
        uint64_t price = price_dist(rng_);
        uint32_t qty   = qty_dist(rng_);

        auto t0 = now_ns();
        if (!send_order(side, price, qty)) break;
        orders_sent_.fetch_add(1, std::memory_order_relaxed);

        if (!recv_ack()) break;
        auto t1 = now_ns();
        total_latency_us_.fetch_add((t1 - t0) / 1000, std::memory_order_relaxed);
    }
}

// ---- MarketMakerStrategy --------------------------------------------------

MarketMakerStrategy::MarketMakerStrategy(std::string host, uint16_t port,
                                          std::string symbol,
                                          uint64_t spread_ticks, uint32_t quote_size)
    : host_(std::move(host)), port_(port), symbol_(std::move(symbol)),
      spread_ticks_(spread_ticks), quote_size_(quote_size) {}

MarketMakerStrategy::~MarketMakerStrategy() { disconnect(); }

bool MarketMakerStrategy::connect() {
    sock_fd_ = tcp_connect(host_, port_);
    return sock_fd_ >= 0;
}

void MarketMakerStrategy::disconnect() {
    if (sock_fd_ >= 0) { ::close(sock_fd_); sock_fd_ = -1; }
}

uint64_t MarketMakerStrategy::now_ns() { return now_ns_impl(); }

bool MarketMakerStrategy::send_order(uint8_t side, uint64_t price, uint32_t quantity) {
    NewOrderMessage msg{};
    msg.client_order_id = next_order_id_++;
    msg.client_id       = 1002;
    // msg is zero-initialised, so this null-pads the 8-byte wire field
    std::memcpy(msg.symbol, symbol_.data(), std::min(symbol_.size(), sizeof(msg.symbol)));
    msg.side       = side;
    msg.order_type = 1; // LIMIT
    msg.price      = price;
    msg.quantity   = quantity;

    seal_message(msg, MessageType::NEW_ORDER, next_sequence_++, now_ns());
    if (!send_all(sock_fd_, &msg, sizeof(msg))) return false;
    orders_sent_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool MarketMakerStrategy::recv_ack(uint64_t& assigned_order_id) {
    assigned_order_id = 0;
    MessageHeader hdr{};
    if (!recv_all(sock_fd_, &hdr, sizeof(hdr))) return false;

    uint32_t body_len = hdr.length > sizeof(hdr)
                        ? hdr.length - static_cast<uint32_t>(sizeof(hdr)) : 0;
    std::vector<uint8_t> body(body_len);
    if (body_len > 0 && !recv_all(sock_fd_, body.data(), body_len)) return false;

    auto type = static_cast<MessageType>(hdr.type);
    if (type == MessageType::ORDER_ACK) {
        // OrderAckMessage body starts with uint64_t order_id
        if (body_len >= sizeof(uint64_t)) {
            std::memcpy(&assigned_order_id, body.data(), sizeof(uint64_t));
        }
        orders_acked_.fetch_add(1, std::memory_order_relaxed);
    } else if (type == MessageType::TRADE_REPORT) {
        // Extract price from TradeReportMessage body
        // Layout after header: trade_id(8) buy_id(8) sell_id(8) symbol(8) price(8) qty(4) pad(4)
        if (body_len >= 8 + 8 + 8 + 8 + 8) {
            uint64_t price = 0;
            std::memcpy(&price, body.data() + 8 + 8 + 8 + 8, sizeof(uint64_t));
            on_trade(price);
        }
        trades_received_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool MarketMakerStrategy::recv_message(MessageHeader& hdr) {
    if (!recv_all(sock_fd_, &hdr, sizeof(hdr))) return false;
    uint32_t body_len = hdr.length > sizeof(hdr)
                        ? hdr.length - static_cast<uint32_t>(sizeof(hdr)) : 0;
    std::vector<uint8_t> discard(body_len);
    if (body_len > 0) recv_all(sock_fd_, discard.data(), body_len);
    return true;
}

void MarketMakerStrategy::cancel_order(uint64_t order_id) {
    if (order_id == 0) return;
    CancelOrderMessage msg{};
    msg.order_id  = order_id;
    msg.client_id = 1002;
    seal_message(msg, MessageType::CANCEL_ORDER, next_sequence_++, now_ns());
    send_all(sock_fd_, &msg, sizeof(msg));
    // Read the ack (best effort)
    MessageHeader hdr{};
    recv_message(hdr);
}

void MarketMakerStrategy::update_quotes() {
    // Cancel stale quotes
    cancel_order(bid_order_id_);
    cancel_order(ask_order_id_);

    uint64_t bid_price = base_price_ > spread_ticks_ ? base_price_ - spread_ticks_ : 1;
    uint64_t ask_price = base_price_ + spread_ticks_;

    // Send BUY quote
    send_order(0 /*BUY*/, bid_price, quote_size_);
    uint64_t id = 0;
    recv_ack(id);
    bid_order_id_ = id ? id : next_order_id_ - 1;

    // Send SELL quote
    send_order(1 /*SELL*/, ask_price, quote_size_);
    recv_ack(id);
    ask_order_id_ = id ? id : next_order_id_ - 1;
}

void MarketMakerStrategy::on_trade(uint64_t price) {
    if (price > 0) base_price_ = price;
    update_quotes();
}

void MarketMakerStrategy::run(std::chrono::seconds duration) {
    update_quotes();

    auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        // Poll for incoming messages (acks / trade reports)
        MessageHeader hdr{};
        // Non-blocking peek: set a short timeout via SO_RCVTIMEO
        struct timeval tv{0, 50000}; // 50ms
        ::setsockopt(sock_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (recv_all(sock_fd_, &hdr, sizeof(hdr))) {
            uint32_t body_len = hdr.length > sizeof(hdr)
                                ? hdr.length - static_cast<uint32_t>(sizeof(hdr)) : 0;
            std::vector<uint8_t> body(body_len);
            if (body_len > 0) recv_all(sock_fd_, body.data(), body_len);

            if (static_cast<MessageType>(hdr.type) == MessageType::TRADE_REPORT) {
                uint64_t price = 0;
                if (body_len >= 8 + 8 + 8 + 8 + 8) {
                    std::memcpy(&price, body.data() + 8 + 8 + 8 + 8, sizeof(uint64_t));
                }
                trades_received_.fetch_add(1, std::memory_order_relaxed);
                on_trade(price);
            }
        }
    }
}

} // namespace rtes
