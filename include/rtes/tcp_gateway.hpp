#pragma once

#include "rtes/protocol.hpp"
#include "rtes/risk_manager.hpp"
#include "rtes/memory_pool.hpp"
#include "rtes/security_utils.hpp"
#include "rtes/memory_safety.hpp"
#include <thread>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <sys/socket.h>

namespace rtes {

class ClientConnection {
public:
    explicit ClientConnection(int fd);
    ~ClientConnection() = default;

    // Non-copyable, non-movable (owned via shared_ptr)
    ClientConnection(const ClientConnection&) = delete;
    ClientConnection& operator=(const ClientConnection&) = delete;

    int  get_fd() const { return fd_.get(); }
    bool is_connected() const { return connected_.load(); }

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
    FileDescriptor fd_;
    std::atomic<bool> connected_{true};
};

class TcpGateway {
public:
    explicit TcpGateway(uint16_t port, RiskManager* risk_manager, OrderPool* order_pool);
    ~TcpGateway();

    void start();
    void stop();

    uint64_t connections_accepted() const { return connections_accepted_.load(); }
    uint64_t messages_received()    const { return messages_received_.load(); }
    uint64_t messages_sent()        const { return messages_sent_.load(); }

private:
    uint16_t      port_;
    RiskManager*  risk_manager_;
    OrderPool*    order_pool_;

    std::thread acceptor_thread_;
    std::thread worker_thread_;
    std::atomic<bool> running_{false};

    FileDescriptor listen_fd_;

    std::unordered_map<int, std::shared_ptr<ClientConnection>> connections_;
    mutable std::mutex connections_mutex_;

    std::atomic<uint64_t> connections_accepted_{0};
    std::atomic<uint64_t> messages_received_{0};
    std::atomic<uint64_t> messages_sent_{0};
    std::atomic<uint64_t> next_sequence_{1};

    bool setup_listen_socket();

    void acceptor_loop();
    void worker_loop();

    void handle_client_data(int client_fd);
    void remove_connection(int client_fd);

    void process_message_safe(ClientConnection* conn, const FixedSizeBuffer<8192>& buffer);
    void handle_new_order(ClientConnection* conn, const NewOrderMessage& msg);
    void handle_cancel_order(ClientConnection* conn, const CancelOrderMessage& msg);

    void send_order_ack(ClientConnection* conn, uint64_t order_id,
                        uint8_t status, const char* reason);
    void send_trade_report(ClientConnection* conn, const Trade& trade);

    uint64_t now_ns() const;
};

} // namespace rtes
