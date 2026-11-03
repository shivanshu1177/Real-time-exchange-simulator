#pragma once

#include "rtes/protocol.hpp"
#include "rtes/gateway_core.hpp"
#include "rtes/memory_safety.hpp"
#include <thread>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <memory>
#include <sys/socket.h>

namespace rtes {

class ClientConnection {
public:
    ClientConnection(int fd, SessionId session);
    ~ClientConnection() = default;

    // Non-copyable, non-movable
    ClientConnection(const ClientConnection&) = delete;
    ClientConnection& operator=(const ClientConnection&) = delete;

    int       get_fd() const { return fd_.get(); }
    SessionId session() const { return session_; }
    bool      is_connected() const { return connected_.load(); }

    // Blocking read of exactly `size` bytes into buffer
    ssize_t read_data_safe(void* buffer, size_t size);

    // Blocking write of `size` bytes
    ssize_t write_data_safe(const void* buffer, size_t size);

    // Read a complete framed message into buf; returns false on error/disconnect
    bool read_message_safe(FixedSizeBuffer<8192>& buf);

    // Write a framed message; returns false on error
    bool write_message_safe(const void* message, size_t size);

    void disconnect();

private:
    FileDescriptor    fd_;
    SessionId         session_;
    std::atomic<bool> connected_{true};
};

// TCP transport for GatewayCore. One thread does everything: accept, read,
// hand frames to the core, and write the execution reports the core produces
// from engine events. Risk runs inside the core on this thread.
class TcpGateway : public OutputSink {
public:
    TcpGateway(uint16_t port, RiskManager& risk, OrderPool& pool,
               std::vector<MatchingEngine*> engines);
    ~TcpGateway() override;

    void start();
    void stop();

    uint64_t connections_accepted() const { return connections_accepted_.load(); }
    uint64_t messages_received()    const { return messages_received_.load(); }
    uint64_t messages_sent()        const { return messages_sent_.load(); }

    // OutputSink
    void send(SessionId session, const void* data, size_t len) override;

private:
    uint16_t    port_;
    GatewayCore core_;

    std::thread       worker_thread_;
    std::atomic<bool> running_{false};

    FileDescriptor listen_fd_;

    // Only touched by the worker thread (and by stop() after it has joined).
    std::unordered_map<int, std::unique_ptr<ClientConnection>> connections_;   // by fd
    std::unordered_map<SessionId, ClientConnection*>           by_session_;

    std::atomic<uint64_t> connections_accepted_{0};
    std::atomic<uint64_t> messages_received_{0};
    std::atomic<uint64_t> messages_sent_{0};

    bool setup_listen_socket();
    void worker_loop();
    void accept_pending();
    void handle_client_data(int client_fd);
    void remove_connection(int client_fd);
};

} // namespace rtes
