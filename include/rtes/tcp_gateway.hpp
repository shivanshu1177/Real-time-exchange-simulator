#pragma once

#include "rtes/protocol.hpp"
#include "rtes/gateway_core.hpp"
#include "rtes/memory_safety.hpp"
#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>
#include <poll.h>

namespace rtes {

// One client connection: a non-blocking socket with a fixed-capacity receive
// buffer (bytes are appended as they arrive and complete frames are taken
// from the front, so a frame split across any number of recv calls is
// reassembled) and a fixed-capacity send buffer (flushed when the socket is
// writable instead of spinning on EAGAIN). No allocation after construction.
class ClientConnection {
public:
    static constexpr size_t kRecvCapacity = 64 * 1024;
    static constexpr size_t kSendCapacity = 256 * 1024;

    ClientConnection(int fd, SessionId session);

    ClientConnection(const ClientConnection&) = delete;
    ClientConnection& operator=(const ClientConnection&) = delete;

    int       fd() const { return fd_.get(); }
    SessionId session() const { return session_; }

    enum class ReadResult { OK, CLOSED, ERROR };
    // recv() until EAGAIN or the buffer is full.
    ReadResult read_available();

    // Validate the frame at the front of the receive buffer. On OK, `frame`
    // points at it until consume() is called.
    FrameCheck next_frame(const uint8_t*& frame) const;
    void       consume(size_t n);
    size_t     buffered() const { return rx_end_ - rx_begin_; }

    // Append to the send buffer. Returns false if it would overflow (the peer
    // is not reading); the caller should then drop the connection.
    bool queue_send(const void* data, size_t len);
    // Write as much as the socket accepts. Returns false on a fatal error.
    bool flush();
    bool has_pending_output() const { return tx_end_ > tx_begin_; }

    void close();

    bool closing{false};   // scheduled to close once pending output is flushed

private:
    FileDescriptor             fd_;
    SessionId                  session_;
    std::unique_ptr<uint8_t[]> rx_;
    size_t                     rx_begin_{0}, rx_end_{0};
    std::unique_ptr<uint8_t[]> tx_;
    size_t                     tx_begin_{0}, tx_end_{0};
};

struct GatewayStats {
    std::atomic<uint64_t> connections_accepted{0};
    std::atomic<uint64_t> messages_received{0};
    std::atomic<uint64_t> messages_sent{0};
    std::atomic<uint64_t> protocol_violations{0};   // sessions closed for bad frames
    std::atomic<uint64_t> slow_consumer_disconnects{0};
};

// TCP transport for GatewayCore. One thread does everything: accept, read,
// validate frames, hand them to the core, and write what the core produces.
// Risk runs inside the core on this thread.
class TcpGateway : public OutputSink {
public:
    TcpGateway(uint16_t port, RiskManager& risk,
               std::vector<MatchingEngine*> engines, bool cancel_on_disconnect = true,
               size_t max_connections = 1024);
    ~TcpGateway() override;

    // Binds and starts the thread. Returns false if the port cannot be bound.
    // With port 0 the OS picks a free port; see bound_port().
    bool start();
    void stop();
    uint16_t bound_port() const { return bound_port_; }

    const GatewayStats& stats() const { return stats_; }

    // OutputSink (gateway thread only)
    void send(SessionId session, const void* data, size_t len) override;
    void disconnect(SessionId session) override;

private:
    uint16_t     port_;
    uint16_t     bound_port_{0};
    GatewayCore  core_;
    size_t       max_connections_;
    GatewayStats stats_;

    std::thread       worker_thread_;
    std::atomic<bool> running_{false};

    FileDescriptor listen_fd_;

    // Only touched by the worker thread (and by stop() after it has joined).
    std::unordered_map<int, std::unique_ptr<ClientConnection>> connections_;   // by fd
    std::unordered_map<SessionId, ClientConnection*>           by_session_;
    std::vector<struct pollfd> pfds_;       // reserved once; rebuilt in place every loop
    std::vector<int>           to_close_;   // reserved once

    bool setup_listen_socket();
    void worker_loop();
    void accept_pending();
    void handle_readable(ClientConnection& conn);
    void close_pending();
    void remove_connection(int fd);
};

} // namespace rtes
