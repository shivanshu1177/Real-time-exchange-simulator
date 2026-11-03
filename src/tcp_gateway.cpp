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

ClientConnection::ClientConnection(int fd, SessionId session) : fd_(fd), session_(session) {}

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

TcpGateway::TcpGateway(uint16_t port, RiskManager& risk, OrderPool& pool,
                       std::vector<MatchingEngine*> engines)
    : port_(port), core_(risk, pool, std::move(engines), *this)
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
    worker_thread_ = std::thread(&TcpGateway::worker_loop, this);
}

void TcpGateway::stop() {
    running_.store(false, std::memory_order_release);
    if (worker_thread_.joinable()) worker_thread_.join();
    for (auto& [fd, conn] : connections_) {
        core_.close_session(conn->session());
        conn->disconnect();
    }
    connections_.clear();
    by_session_.clear();
}

void TcpGateway::accept_pending() {
    while (true) {
        sockaddr_in client_addr{};
        socklen_t   client_len = sizeof(client_addr);
        int client_fd = ::accept(listen_fd_.get(),
                                 reinterpret_cast<sockaddr*>(&client_addr),
                                 &client_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            break;   // EAGAIN: nothing more to accept
        }

        int flags = ::fcntl(client_fd, F_GETFL, 0);
        ::fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
        int opt = 1;
        ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        const SessionId session = core_.open_session();
        auto conn = std::make_unique<ClientConnection>(client_fd, session);
        by_session_[session] = conn.get();
        connections_[client_fd] = std::move(conn);
        connections_accepted_.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO("TcpGateway: accepted connection fd=" + std::to_string(client_fd) +
                 " session=" + std::to_string(session) +
                 " from " + inet_ntoa(client_addr.sin_addr));
    }
}

void TcpGateway::worker_loop() {
    // After any input or output, keep polling without sleeping for a short
    // window: a request just handed to an engine produces events microseconds
    // later. When idle, poll with a 1 ms timeout instead of spinning.
    constexpr auto kBusyWindow = std::chrono::microseconds(200);
    auto last_activity = std::chrono::steady_clock::now() - kBusyWindow;
    while (running_.load(std::memory_order_acquire)) {
        std::vector<struct pollfd> pfds;
        pfds.reserve(connections_.size() + 1);
        pfds.push_back(pollfd{listen_fd_.get(), POLLIN, 0});
        for (auto& [fd, conn] : connections_) pfds.push_back(pollfd{fd, POLLIN, 0});

        const bool busy = std::chrono::steady_clock::now() - last_activity < kBusyWindow;
        int ret = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), busy ? 0 : 1 /*ms*/);
        if (ret > 0) {
            if (pfds[0].revents & POLLIN) accept_pending();
            for (size_t i = 1; i < pfds.size(); ++i) {
                const auto& pfd = pfds[i];
                if (pfd.revents & POLLIN) {
                    handle_client_data(pfd.fd);
                } else if (pfd.revents & (POLLHUP | POLLERR)) {
                    remove_connection(pfd.fd);
                }
            }
        }
        if (core_.drain_events() > 0 || ret > 0) {
            last_activity = std::chrono::steady_clock::now();
        }
    }
}

void TcpGateway::handle_client_data(int client_fd) {
    auto it = connections_.find(client_fd);
    if (it == connections_.end()) return;
    ClientConnection* conn = it->second.get();

    FixedSizeBuffer<8192> buf;
    if (!conn->read_message_safe(buf)) {
        if (!conn->is_connected()) remove_connection(client_fd);
        return;
    }
    messages_received_.fetch_add(1, std::memory_order_relaxed);
    core_.on_message(conn->session(), buf.begin(), buf.size);
}

void TcpGateway::remove_connection(int client_fd) {
    auto it = connections_.find(client_fd);
    if (it == connections_.end()) return;
    const SessionId session = it->second->session();
    core_.close_session(session);
    by_session_.erase(session);
    it->second->disconnect();
    connections_.erase(it);
    LOG_INFO("TcpGateway: connection closed fd=" + std::to_string(client_fd) +
             " session=" + std::to_string(session));
}

void TcpGateway::send(SessionId session, const void* data, size_t len) {
    auto it = by_session_.find(session);
    if (it == by_session_.end()) return;
    if (it->second->write_message_safe(data, len)) {
        messages_sent_.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace rtes
