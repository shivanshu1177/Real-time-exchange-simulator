#include "rtes/protocol.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <algorithm>
#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <chrono>
#include <cstring>

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --host <host> --port <port>"
              << " --order \"SIDE,SYMBOL,QTY,PRICE,TYPE\"\n"
              << "  SIDE  : BUY or SELL\n"
              << "  TYPE  : LIMIT or MARKET\n"
              << "Example: " << prog
              << " --host localhost --port 8888"
              << " --order \"BUY,AAPL,100,150.00,LIMIT\"\n";
}

static std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, delim)) out.push_back(tok);
    return out;
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

static uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

int main(int argc, char* argv[]) {
    std::string host  = "localhost";
    uint16_t    port  = 8888;
    std::string order_str;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--host" || arg == "-h") && i + 1 < argc) host = argv[++i];
        else if ((arg == "--port" || arg == "-p") && i + 1 < argc) port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if ((arg == "--order" || arg == "-o") && i + 1 < argc) order_str = argv[++i];
    }

    if (order_str.empty()) { usage(argv[0]); return 1; }

    auto parts = split(order_str, ',');
    if (parts.size() < 4) {
        std::cerr << "Error: order must be SIDE,SYMBOL,QTY,PRICE[,TYPE]\n";
        return 1;
    }

    uint8_t  side      = (parts[0] == "BUY") ? 0 : 1;
    std::string symbol = parts[1];
    uint32_t qty       = static_cast<uint32_t>(std::stoul(parts[2]));
    // Price stored as fixed-point *10000; input is decimal dollars
    double   price_f   = std::stod(parts[3]);
    uint64_t price     = static_cast<uint64_t>(price_f * 10000);
    uint8_t  otype     = (parts.size() >= 5 && parts[4] == "MARKET") ? 0 : 1;

    // Connect (resolves hostnames via getaddrinfo)
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    auto port_str = std::to_string(port);
    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
        std::cerr << "Cannot resolve " << host << "\n";
        return 1;
    }
    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { perror("socket"); ::freeaddrinfo(res); return 1; }
    if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        perror("connect"); ::close(fd); ::freeaddrinfo(res); return 1;
    }
    ::freeaddrinfo(res);
    std::cout << "Connected to " << host << ":" << port << "\n";

    // Build NewOrderMessage
    rtes::NewOrderMessage msg{};
    msg.client_order_id = now_ns(); // unique per invocation
    msg.client_id       = 999;
    // msg is zero-initialised, so this null-pads the 8-byte wire field
    std::memcpy(msg.symbol, symbol.data(), std::min(symbol.size(), sizeof(msg.symbol)));
    msg.side       = side;
    msg.order_type = otype;
    msg.price      = price;
    msg.quantity   = qty;

    rtes::seal_message(msg, rtes::MessageType::NEW_ORDER, 1, now_ns());

    // Send and time
    uint64_t t0 = now_ns();
    if (!send_all(fd, &msg, sizeof(msg))) {
        std::cerr << "Send failed\n";
        ::close(fd);
        return 1;
    }

    // Receive ack
    rtes::OrderAckMessage ack{};
    if (!recv_all(fd, &ack, sizeof(ack))) {
        std::cerr << "Recv failed\n";
        ::close(fd);
        return 1;
    }
    uint64_t t1 = now_ns();
    double latency_us = static_cast<double>(t1 - t0) / 1000.0;

    const char* status_str = (ack.status == 0) ? "ACCEPTED" : "REJECTED";
    std::cout << "[ACK] order_id=" << ack.order_id
              << " status=" << status_str
              << " reason=" << ack.reason
              << " latency=" << latency_us << " μs\n";

    ::close(fd);
    return (ack.status == 0) ? 0 : 1;
}
