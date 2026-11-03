#include "rtes/strategies.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
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

// Wait up to timeout_ms for the socket to become readable.
static bool wait_readable(int fd, int timeout_ms) {
    struct pollfd pfd{fd, POLLIN, 0};
    return ::poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN);
}

// One complete inbound frame. The exchange only sends fixed-size messages
// well below sizeof(body).
struct StrategyFrame {
    MessageHeader hdr{};
    uint8_t       raw[256]{};   // whole message, header included
};

static bool read_frame(int fd, StrategyFrame& f) {
    if (!recv_all(fd, &f.hdr, sizeof(f.hdr))) return false;
    if (f.hdr.length < sizeof(f.hdr) || f.hdr.length > sizeof(f.raw)) return false;
    std::memcpy(f.raw, &f.hdr, sizeof(f.hdr));
    size_t body = f.hdr.length - sizeof(f.hdr);
    return body == 0 || recv_all(fd, f.raw + sizeof(f.hdr), body);
}

static bool as_exec_report(const StrategyFrame& f, ExecReportMessage& rep) {
    if (static_cast<MessageType>(f.hdr.type) != MessageType::EXEC_REPORT ||
        f.hdr.length != sizeof(rep)) return false;
    std::memcpy(&rep, f.raw, sizeof(rep));
    return true;
}

// Send LOGON and wait for LOGON_ACK. Returns false if the exchange refuses.
static bool logon(int fd, uint64_t client_id) {
    LogonMessage msg{};
    msg.protocol_version = PROTOCOL_VERSION;
    msg.client_id        = client_id;
    seal_message(msg, MessageType::LOGON, 0, 0);
    if (!send_all(fd, &msg, sizeof(msg))) return false;
    StrategyFrame f;
    if (!wait_readable(fd, 2000) || !read_frame(fd, f)) return false;
    return static_cast<MessageType>(f.hdr.type) == MessageType::LOGON_ACK;
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
    return sock_fd_ >= 0 && logon(sock_fd_, 1001);
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
    // msg is zero-initialised, so this null-pads the 8-byte wire field
    std::memcpy(msg.symbol, symbol_.data(), std::min(symbol_.size(), sizeof(msg.symbol)));
    msg.side       = side;
    msg.order_type = 1; // LIMIT
    msg.price      = price;
    msg.quantity   = quantity;

    seal_message(msg, MessageType::NEW_ORDER, next_sequence_++, now_ns());
    return send_all(sock_fd_, &msg, sizeof(msg));
}

// Read reports until the order just sent is acknowledged (NEW) or rejected.
// Fills for any of our orders are counted as they arrive.
bool LiquidityTakerStrategy::recv_ack() {
    const uint64_t coid = next_order_id_ - 1;
    StrategyFrame f;
    while (wait_readable(sock_fd_, 2000)) {
        if (!read_frame(sock_fd_, f)) return false;
        ExecReportMessage rep;
        if (!as_exec_report(f, rep)) continue;
        auto type = static_cast<ExecType>(rep.exec_type);
        if (type == ExecType::PARTIAL_FILL || type == ExecType::FILL) {
            trades_received_.fetch_add(1, std::memory_order_relaxed);
        }
        if (rep.client_order_id != coid) continue;
        if (type == ExecType::NEW) {
            orders_acked_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        if (type == ExecType::REJECTED) {
            orders_rejected_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;   // no answer within 2 s
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
    return sock_fd_ >= 0 && logon(sock_fd_, 1002);
}

void MarketMakerStrategy::disconnect() {
    if (sock_fd_ >= 0) { ::close(sock_fd_); sock_fd_ = -1; }
}

uint64_t MarketMakerStrategy::now_ns() { return now_ns_impl(); }

bool MarketMakerStrategy::send_order(uint8_t side, uint64_t price, uint32_t quantity) {
    NewOrderMessage msg{};
    msg.client_order_id = next_order_id_++;
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

// Handle one execution report or cancel reject. Updates quote state and
// requests a requote after a fill.
void MarketMakerStrategy::handle(const StrategyFrame& f) {
    ExecReportMessage rep;
    if (!as_exec_report(f, rep)) return;
    auto type = static_cast<ExecType>(rep.exec_type);
    if (type == ExecType::PARTIAL_FILL || type == ExecType::FILL) {
        trades_received_.fetch_add(1, std::memory_order_relaxed);
        if (rep.last_px > 0) base_price_ = rep.last_px;
        requote_ = true;
    }
    if (type == ExecType::NEW)      orders_acked_.fetch_add(1, std::memory_order_relaxed);
    if (type == ExecType::REJECTED) orders_rejected_.fetch_add(1, std::memory_order_relaxed);
    const bool terminal = type == ExecType::FILL || type == ExecType::CANCELED ||
                          type == ExecType::REJECTED;
    if (terminal) {
        if (rep.client_order_id == bid_coid_) bid_coid_ = 0;
        if (rep.client_order_id == ask_coid_) ask_coid_ = 0;
    }
}

// Read and handle frames until done() is true or 2 s pass without data.
template<typename Done>
bool MarketMakerStrategy::wait_until(Done done) {
    StrategyFrame f;
    while (!done()) {
        if (!wait_readable(sock_fd_, 2000) || !read_frame(sock_fd_, f)) return false;
        handle(f);
        if (static_cast<MessageType>(f.hdr.type) == MessageType::CANCEL_REJECT) {
            CancelRejectMessage rej;
            std::memcpy(&rej, f.raw, sizeof(rej));
            // The quote is already gone (filled or unknown): stop tracking it.
            if (rej.client_order_id == bid_coid_) bid_coid_ = 0;
            if (rej.client_order_id == ask_coid_) ask_coid_ = 0;
        }
    }
    return true;
}

void MarketMakerStrategy::cancel_quote(uint64_t& coid) {
    if (coid == 0) return;
    CancelOrderMessage msg{};
    msg.client_order_id = coid;
    seal_message(msg, MessageType::CANCEL_ORDER, next_sequence_++, now_ns());
    if (!send_all(sock_fd_, &msg, sizeof(msg))) return;
    wait_until([&] { return coid == 0; });
}

void MarketMakerStrategy::place_quote(uint8_t side, uint64_t price, uint64_t& coid) {
    if (!send_order(side, price, quote_size_)) return;
    const uint64_t sent = next_order_id_ - 1;
    bool answered = false;
    // Watch for the NEW (or REJECTED) of exactly this order.
    StrategyFrame f;
    while (!answered && wait_readable(sock_fd_, 2000) && read_frame(sock_fd_, f)) {
        handle(f);
        ExecReportMessage rep;
        if (as_exec_report(f, rep) && rep.client_order_id == sent) {
            auto type = static_cast<ExecType>(rep.exec_type);
            if (type == ExecType::NEW) { coid = sent; answered = true; }
            if (type == ExecType::REJECTED) answered = true;
        }
    }
}

void MarketMakerStrategy::update_quotes() {
    requote_ = false;
    cancel_quote(bid_coid_);
    cancel_quote(ask_coid_);

    uint64_t bid_price = base_price_ > spread_ticks_ ? base_price_ - spread_ticks_ : 1;
    uint64_t ask_price = base_price_ + spread_ticks_;
    place_quote(0 /*BUY*/,  bid_price, bid_coid_);
    place_quote(1 /*SELL*/, ask_price, ask_coid_);
}

void MarketMakerStrategy::on_trade(uint64_t price) {
    if (price > 0) base_price_ = price;
    update_quotes();
}

void MarketMakerStrategy::run(std::chrono::seconds duration) {
    update_quotes();

    auto deadline = std::chrono::steady_clock::now() + duration;
    StrategyFrame f;
    while (std::chrono::steady_clock::now() < deadline) {
        if (wait_readable(sock_fd_, 50)) {
            if (!read_frame(sock_fd_, f)) break;
            handle(f);
        }
        if (requote_) update_quotes();
    }
}

} // namespace rtes
