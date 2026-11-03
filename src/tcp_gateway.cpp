#include "rtes/tcp_gateway.hpp"
#include "rtes/logger.hpp"
#include "rtes/net_compat.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rtes {

// ---- ClientConnection ----------------------------------------------------

ClientConnection::ClientConnection(int fd, SessionId session)
    : fd_(fd), session_(session),
      rx_(std::make_unique<uint8_t[]>(kRecvCapacity)),
      tx_(std::make_unique<uint8_t[]>(kSendCapacity)) {}

ClientConnection::ReadResult ClientConnection::read_available() {
    // Make room: drop consumed bytes from the front.
    if (rx_begin_ == rx_end_) {
        rx_begin_ = rx_end_ = 0;
    } else if (rx_begin_ > 0 && rx_end_ == kRecvCapacity) {
        std::memmove(rx_.get(), rx_.get() + rx_begin_, rx_end_ - rx_begin_);
        rx_end_  -= rx_begin_;
        rx_begin_ = 0;
    }
    while (rx_end_ < kRecvCapacity) {
        ssize_t n = ::recv(fd_.get(), rx_.get() + rx_end_, kRecvCapacity - rx_end_, 0);
        if (n > 0) {
            rx_end_ += static_cast<size_t>(n);
        } else if (n == 0) {
            return ReadResult::CLOSED;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return ReadResult::OK;
        } else {
            return ReadResult::ERROR;
        }
    }
    return ReadResult::OK;   // buffer full; the rest is read after frames are consumed
}

FrameCheck ClientConnection::next_frame(const uint8_t*& frame) const {
    frame = rx_.get() + rx_begin_;
    return check_inbound_frame(frame, rx_end_ - rx_begin_);
}

void ClientConnection::consume(size_t n) {
    rx_begin_ += n;
    if (rx_begin_ == rx_end_) rx_begin_ = rx_end_ = 0;
}

bool ClientConnection::queue_send(const void* data, size_t len) {
    if (tx_end_ + len > kSendCapacity && tx_begin_ > 0) {
        std::memmove(tx_.get(), tx_.get() + tx_begin_, tx_end_ - tx_begin_);
        tx_end_  -= tx_begin_;
        tx_begin_ = 0;
    }
    if (tx_end_ + len > kSendCapacity) return false;
    std::memcpy(tx_.get() + tx_end_, data, len);
    tx_end_ += len;
    return true;
}

bool ClientConnection::flush() {
    while (tx_begin_ < tx_end_) {
        // A peer that already closed must not raise SIGPIPE (see net_compat.hpp).
        ssize_t n = ::send(fd_.get(), tx_.get() + tx_begin_, tx_end_ - tx_begin_, net::kSendNoSignal);
        if (n > 0) {
            tx_begin_ += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;   // wait for POLLOUT
        } else {
            return false;
        }
    }
    tx_begin_ = tx_end_ = 0;
    return true;
}

void ClientConnection::close() {
    fd_.reset();
}

// ---- TcpGateway ----------------------------------------------------------

TcpGateway::TcpGateway(uint16_t port, RiskManager& risk,
                       std::vector<MatchingEngine*> engines, bool cancel_on_disconnect,
                       size_t max_connections)
    : port_(port),
      core_(risk, std::move(engines), *this, 16384, cancel_on_disconnect),
      max_connections_(max_connections)
{
    pfds_.reserve(max_connections_ + 1);
    to_close_.reserve(max_connections_);
    connections_.reserve(max_connections_);
    by_session_.reserve(max_connections_);
}

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

    sockaddr_in bound{};
    socklen_t   len = sizeof(bound);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len);
    bound_port_ = ntohs(bound.sin_port);

    listen_fd_.reset(fd);
    LOG_INFO("TcpGateway: listening on port " + std::to_string(bound_port_));
    return true;
}

bool TcpGateway::start() {
    if (!setup_listen_socket()) {
        LOG_ERROR("TcpGateway: failed to set up listen socket");
        return false;
    }
    running_.store(true, std::memory_order_release);
    worker_thread_ = std::thread(&TcpGateway::worker_loop, this);
    return true;
}

void TcpGateway::stop() {
    running_.store(false, std::memory_order_release);
    if (worker_thread_.joinable()) worker_thread_.join();
    while (!connections_.empty()) remove_connection(connections_.begin()->first);
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
        if (connections_.size() >= max_connections_) {
            LOG_WARN("TcpGateway: connection limit reached, refusing fd=" + std::to_string(client_fd));
            ::close(client_fd);
            continue;
        }

        int flags = ::fcntl(client_fd, F_GETFL, 0);
        ::fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
        int opt = 1;
        ::setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
        net::disable_sigpipe(client_fd);

        const SessionId session = core_.open_session();
        auto conn = std::make_unique<ClientConnection>(client_fd, session);
        by_session_[session] = conn.get();
        connections_[client_fd] = std::move(conn);
        stats_.connections_accepted.fetch_add(1, std::memory_order_relaxed);
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
        // Rebuilt in place: capacity was reserved for max_connections_ + 1.
        pfds_.clear();
        pfds_.push_back(pollfd{listen_fd_.get(), POLLIN, 0});
        for (auto& [fd, conn] : connections_) {
            short events = POLLIN;
            if (conn->has_pending_output()) events |= POLLOUT;
            pfds_.push_back(pollfd{fd, events, 0});
        }

        const bool busy = std::chrono::steady_clock::now() - last_activity < kBusyWindow;
        int ret = ::poll(pfds_.data(), static_cast<nfds_t>(pfds_.size()), busy ? 0 : 1 /*ms*/);
        if (ret > 0) {
            if (pfds_[0].revents & POLLIN) accept_pending();
            for (size_t i = 1; i < pfds_.size(); ++i) {
                const auto& pfd = pfds_[i];
                if (pfd.revents == 0) continue;
                auto it = connections_.find(pfd.fd);
                if (it == connections_.end() || it->second->closing) continue;
                ClientConnection& conn = *it->second;
                if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) handle_readable(conn);
                if ((pfd.revents & POLLOUT) && !conn.closing && !conn.flush()) {
                    disconnect(conn.session());
                }
            }
        }

        const size_t events = core_.drain_events();
        core_.on_tick(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()));

        // Send what this iteration produced; whatever the socket does not
        // take now waits for POLLOUT.
        for (auto& [fd, conn] : connections_) {
            if (conn->has_pending_output() && !conn->closing && !conn->flush()) {
                disconnect(conn->session());
            }
        }
        close_pending();

        if (events > 0 || ret > 0) last_activity = std::chrono::steady_clock::now();
    }
}

void TcpGateway::handle_readable(ClientConnection& conn) {
    const auto result = conn.read_available();

    // Process every complete frame received, even if the peer then closed.
    const uint8_t* frame = nullptr;
    while (!conn.closing) {
        const FrameCheck fc = conn.next_frame(frame);
        if (fc.status == FrameCheck::NEED_MORE) break;
        if (fc.status == FrameCheck::INVALID) {
            stats_.protocol_violations.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN("TcpGateway: protocol violation on session " + std::to_string(conn.session()) +
                     " reason=" + std::to_string(static_cast<int>(fc.reason)) +
                     " detail=" + std::to_string(fc.detail));
            core_.reject_session(conn.session(), fc.reason, fc.detail);   // sends, then disconnect()
            return;
        }
        stats_.messages_received.fetch_add(1, std::memory_order_relaxed);
        core_.on_message(conn.session(), frame, fc.length);
        conn.consume(fc.length);
    }

    if (result != ClientConnection::ReadResult::OK) disconnect(conn.session());
}

void TcpGateway::send(SessionId session, const void* data, size_t len) {
    auto it = by_session_.find(session);
    if (it == by_session_.end()) return;
    ClientConnection& conn = *it->second;
    if (!conn.queue_send(data, len)) {
        // The client is not reading its reports: drop it rather than block
        // the gateway thread or grow without bound.
        stats_.slow_consumer_disconnects.fetch_add(1, std::memory_order_relaxed);
        disconnect(session);
        return;
    }
    stats_.messages_sent.fetch_add(1, std::memory_order_relaxed);
}

void TcpGateway::disconnect(SessionId session) {
    auto it = by_session_.find(session);
    if (it == by_session_.end() || it->second->closing) return;
    it->second->closing = true;
    to_close_.push_back(it->second->fd());
}

void TcpGateway::close_pending() {
    for (int fd : to_close_) {
        auto it = connections_.find(fd);
        if (it == connections_.end()) continue;
        it->second->flush();   // best effort, e.g. the SESSION_REJECT
        remove_connection(fd);
    }
    to_close_.clear();
}

void TcpGateway::remove_connection(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;
    const SessionId session = it->second->session();
    core_.close_session(session);   // cancel-on-disconnect happens here
    by_session_.erase(session);
    it->second->close();
    connections_.erase(it);
    LOG_INFO("TcpGateway: connection closed fd=" + std::to_string(fd) +
             " session=" + std::to_string(session));
}

} // namespace rtes
