#include "rtes/tcp_gateway.hpp"
#include "rtes/logger.hpp"
#include <poll.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <vector>

namespace rtes {

// ---- ClientConnection ----------------------------------------------------

ClientConnection::ClientConnection(int fd) : fd_(fd) {}

ssize_t ClientConnection::read_data_safe(void* buffer, size_t size) {
    size_t total = 0;
    uint8_t* buf = static_cast<uint8_t*>(buffer);
    while (total < size) {
        ssize_t n = ::recv(fd_.get(), buf + total, size - total, 0);
        if (n > 0) {
            total += static_cast<size_t>(n);
        } else if (n == 0) {
            connected_.store(false);
            return static_cast<ssize_t>(total);
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Non-blocking fd: no data right now
                if (total > 0) return static_cast<ssize_t>(total);
                return -1;
            }
            if (errno == EINTR) continue;
            connected_.store(false);
            return -1;
        }
    }
    return static_cast<ssize_t>(total);
}

ssize_t ClientConnection::write_data_safe(const void* buffer, size_t size) {
    size_t total = 0;
    const uint8_t* buf = static_cast<const uint8_t*>(buffer);
    while (total < size) {
        ssize_t n = ::send(fd_.get(), buf + total, size - total, 0);
        if (n > 0) {
            total += static_cast<size_t>(n);
        } else if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            connected_.store(false);
            return -1;
        }
    }
    return static_cast<ssize_t>(total);
}

bool ClientConnection::read_message_safe(FixedSizeBuffer<8192>& buf) {
    buf.clear();

    // Read the fixed header first
    MessageHeader hdr{};
    ssize_t n = read_data_safe(&hdr, sizeof(hdr));
    if (n != static_cast<ssize_t>(sizeof(hdr))) return false;
    if (!validate_header(hdr)) return false;

    uint32_t total_len = hdr.length;
    if (total_len > buf.capacity() || total_len < sizeof(hdr)) return false;

    // Copy header into buf
    buf.append(&hdr, sizeof(hdr));

    // Read remaining body bytes
    uint32_t body_len = total_len - static_cast<uint32_t>(sizeof(hdr));
    if (body_len > 0) {
        // Use remaining space in buf directly
        n = read_data_safe(buf.data.data() + buf.size, body_len);
        if (n != static_cast<ssize_t>(body_len)) return false;
        buf.size += static_cast<size_t>(body_len);
    }
    return true;
}

bool ClientConnection::write_message_safe(const void* message, size_t size) {
    return write_data_safe(message, size) == static_cast<ssize_t>(size);
}

void ClientConnection::disconnect() {
    connected_.store(false);
    fd_.reset();
}

// ---- TcpGateway ----------------------------------------------------------

TcpGateway::TcpGateway(uint16_t port, RiskManager* risk_manager, OrderPool* order_pool)
    : port_(port), risk_manager_(risk_manager), order_pool_(order_pool)
{}

TcpGateway::~TcpGateway() { stop(); }

bool TcpGateway::setup_listen_socket() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("TcpGateway: socket() failed: " + std::string(std::strerror(errno)));
        return false;
    }

    // Reuse address for quick restarts
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // Non-blocking
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port_);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("TcpGateway: bind() failed on port " + std::to_string(port_) +
                  ": " + std::strerror(errno));
        ::close(fd);
        return false;
    }

    if (::listen(fd, 128) < 0) {
        LOG_ERROR("TcpGateway: listen() failed: " + std::string(std::strerror(errno)));
        ::close(fd);
        return false;
    }

    listen_fd_.reset(fd);
    LOG_INFO("TcpGateway: listening on port " + std::to_string(port_));
    return true;
}

void TcpGateway::start() {
    if (!setup_listen_socket()) {
        LOG_ERROR("TcpGateway: failed to set up listen socket");
        return;
    }
    running_.store(true, std::memory_order_release);
    acceptor_thread_ = std::thread(&TcpGateway::acceptor_loop, this);
    worker_thread_   = std::thread(&TcpGateway::worker_loop,   this);
}

void TcpGateway::stop() {
    running_.store(false, std::memory_order_release);
    if (acceptor_thread_.joinable()) acceptor_thread_.join();
    if (worker_thread_.joinable())   worker_thread_.join();
    // Close all client connections
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (auto& [fd, conn] : connections_) conn->disconnect();
    connections_.clear();
}

void TcpGateway::acceptor_loop() {
    while (running_.load(std::memory_order_acquire)) {
        struct pollfd pfd{};
        pfd.fd     = listen_fd_.get();
        pfd.events = POLLIN;
        int ret = ::poll(&pfd, 1, 100 /*ms*/);
        if (ret <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        // Accept all pending connections
        while (true) {
            sockaddr_in client_addr{};
            socklen_t   client_len = sizeof(client_addr);
            int client_fd = ::accept(listen_fd_.get(),
                                     reinterpret_cast<sockaddr*>(&client_addr),
                                     &client_len);
            if (client_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                break;
            }

            // Set client socket non-blocking
            int flags = ::fcntl(client_fd, F_GETFL, 0);
            ::fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

            int opt = 1;
            ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                connections_[client_fd] = std::make_shared<ClientConnection>(client_fd);
            }
            connections_accepted_.fetch_add(1, std::memory_order_relaxed);
            LOG_INFO("TcpGateway: accepted connection fd=" + std::to_string(client_fd) +
                     " from " + inet_ntoa(client_addr.sin_addr));
        }
    }
}

void TcpGateway::worker_loop() {
    while (running_.load(std::memory_order_acquire)) {
        // Snapshot current fds under lock
        std::vector<std::pair<int, std::shared_ptr<ClientConnection>>> snapshot;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            snapshot.reserve(connections_.size());
            for (auto& [fd, conn] : connections_) {
                snapshot.emplace_back(fd, conn);
            }
        }

        if (snapshot.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        std::vector<struct pollfd> pfds;
        pfds.reserve(snapshot.size());
        for (auto& [fd, conn] : snapshot) {
            struct pollfd pfd{};
            pfd.fd     = fd;
            pfd.events = POLLIN;
            pfds.push_back(pfd);
        }

        int ret = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 100 /*ms*/);
        if (ret <= 0) continue;

        for (size_t i = 0; i < pfds.size(); ++i) {
            auto& pfd = pfds[i];
            if (pfd.revents & (POLLHUP | POLLERR)) {
                remove_connection(pfd.fd);
            } else if (pfd.revents & POLLIN) {
                handle_client_data(pfd.fd);
            }
        }
    }
}

void TcpGateway::handle_client_data(int client_fd) {
    std::shared_ptr<ClientConnection> conn;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(client_fd);
        if (it == connections_.end()) return;
        conn = it->second;
    }

    FixedSizeBuffer<8192> buf;
    if (!conn->read_message_safe(buf)) {
        if (!conn->is_connected()) {
            remove_connection(client_fd);
        }
        return;
    }

    messages_received_.fetch_add(1, std::memory_order_relaxed);
    process_message_safe(conn.get(), buf);
}

void TcpGateway::remove_connection(int client_fd) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(client_fd);
    if (it != connections_.end()) {
        it->second->disconnect();
        connections_.erase(it);
        LOG_INFO("TcpGateway: connection closed fd=" + std::to_string(client_fd));
    }
}

void TcpGateway::process_message_safe(ClientConnection* conn,
                                       const FixedSizeBuffer<8192>& buffer) {
    if (buffer.size < sizeof(MessageHeader)) return;
    const auto* hdr = reinterpret_cast<const MessageHeader*>(buffer.begin());
    auto type = static_cast<MessageType>(hdr->type);

    if (type == MessageType::NEW_ORDER && buffer.size >= sizeof(NewOrderMessage)) {
        const auto* msg = reinterpret_cast<const NewOrderMessage*>(buffer.begin());
        handle_new_order(conn, *msg);
    } else if (type == MessageType::CANCEL_ORDER && buffer.size >= sizeof(CancelOrderMessage)) {
        const auto* msg = reinterpret_cast<const CancelOrderMessage*>(buffer.begin());
        handle_cancel_order(conn, *msg);
    }
}

void TcpGateway::handle_new_order(ClientConnection* conn, const NewOrderMessage& msg) {
    AuthContext ctx;
    ctx.client_id = msg.client_id;

    if (!risk_manager_->validate_order(msg, ctx)) {
        send_order_ack(conn, msg.client_order_id, 1, "INVALID_SYMBOL");
        return;
    }

    Order* order = order_pool_->acquire();
    if (!order) {
        send_order_ack(conn, msg.client_order_id, 1, "POOL_EXHAUSTED");
        return;
    }

    order->order_id  = msg.client_order_id;
    order->client_id = msg.client_id;
    order->symbol    = std::string(msg.symbol, strnlen(msg.symbol, 8));
    order->side      = static_cast<Side>(msg.side);
    order->type      = static_cast<OrderType>(msg.order_type);
    order->price     = msg.price;
    order->quantity  = msg.quantity;
    order->filled    = 0;

    if (!risk_manager_->submit_order(order)) {
        // submit_order already released the order on failure
        send_order_ack(conn, msg.client_order_id, 1, "QUEUE_FULL");
        return;
    }

    send_order_ack(conn, msg.client_order_id, 0, "ACCEPTED");
}

void TcpGateway::handle_cancel_order(ClientConnection* conn, const CancelOrderMessage& msg) {
    // Cancel is best-effort; always ack
    send_order_ack(conn, msg.order_id, 0, "CANCEL_ACCEPTED");
}

void TcpGateway::send_order_ack(ClientConnection* conn, uint64_t order_id,
                                  uint8_t status, const char* reason) {
    OrderAckMessage ack{};
    ack.order_id = order_id;
    ack.status   = status;
    std::strncpy(ack.reason, reason, sizeof(ack.reason) - 1);

    seal_message(ack, MessageType::ORDER_ACK,
                 next_sequence_.fetch_add(1, std::memory_order_relaxed),
                 now_ns());

    conn->write_message_safe(&ack, sizeof(ack));
    messages_sent_.fetch_add(1, std::memory_order_relaxed);
}

void TcpGateway::send_trade_report(ClientConnection* conn, const Trade& trade) {
    TradeReportMessage rep{};
    rep.trade_id      = trade.trade_id;
    rep.buy_order_id  = trade.buy_order_id;
    rep.sell_order_id = trade.sell_order_id;
    std::strncpy(rep.symbol, trade.symbol.c_str(), sizeof(rep.symbol) - 1);
    rep.price    = trade.price;
    rep.quantity = trade.quantity;

    seal_message(rep, MessageType::TRADE_REPORT,
                 next_sequence_.fetch_add(1, std::memory_order_relaxed),
                 now_ns());

    conn->write_message_safe(&rep, sizeof(rep));
    messages_sent_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t TcpGateway::now_ns() const {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace rtes
