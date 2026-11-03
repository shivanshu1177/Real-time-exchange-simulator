#include "rtes/protocol.hpp"
#include "rtes/market_data.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <csignal>
#include <atomic>
#include <iostream>
#include <string>
#include <cstring>

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true); }

int main(int argc, char* argv[]) {
    std::string multicast = "239.0.0.1";
    uint16_t    port      = 9999;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--multicast" && i + 1 < argc) multicast = argv[++i];
        else if (arg == "--port" && i + 1 < argc) port = static_cast<uint16_t>(std::stoi(argv[++i]));
    }

    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);

    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    sockaddr_in bind_addr{};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = INADDR_ANY;
    bind_addr.sin_port        = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) < 0) {
        perror("bind"); ::close(fd); return 1;
    }

    // Join multicast group on the loopback interface so packets sent via lo0
    // (from the local publisher) are received on this host.
    struct ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = ::inet_addr(multicast.c_str());
    mreq.imr_interface.s_addr = ::inet_addr("127.0.0.1");
    if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        // Non-fatal: may work on loopback without multicast routing
        perror("IP_ADD_MEMBERSHIP (non-fatal)");
    }

    // Set receive timeout so we can check g_stop
    struct timeval tv{0, 200000}; // 200ms
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::cout << "UDP receiver listening on " << multicast << ":" << port
              << " (Ctrl+C to stop)\n";

    uint64_t last_seq   = 0;
    uint64_t msg_count  = 0;
    uint64_t gap_count  = 0;

    uint8_t buf[4096];

    while (!g_stop.load()) {
        ssize_t n = ::recvfrom(fd, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n < 0) continue; // timeout or error
        if (static_cast<size_t>(n) < sizeof(rtes::MessageHeader)) continue;

        const auto* hdr = reinterpret_cast<const rtes::MessageHeader*>(buf);
        auto type = static_cast<rtes::MessageType>(hdr->type);

        // Sequence gap detection
        if (last_seq > 0 && hdr->sequence != last_seq + 1) {
            std::cout << "[GAP] expected seq " << last_seq + 1
                      << " got " << hdr->sequence << "\n";
            ++gap_count;
        }
        last_seq = hdr->sequence;
        ++msg_count;

        if (type == rtes::MessageType::TRADE_REPORT &&
            static_cast<size_t>(n) >= sizeof(rtes::TradeUpdateMessage)) {
            const auto* tr = reinterpret_cast<const rtes::TradeUpdateMessage*>(buf);
            char sym[9] = {};
            std::memcpy(sym, tr->symbol, 8);  // sym[8] stays NUL
            double price = static_cast<double>(tr->price) / 10000.0;
            std::cout << "[TRADE] seq=" << hdr->sequence
                      << " symbol=" << sym
                      << " buy_id=" << tr->buy_order_id
                      << " sell_id=" << tr->sell_order_id
                      << " price=$" << price
                      << " qty=" << tr->quantity << "\n";

        } else if (type == rtes::MessageType::BBO_UPDATE &&
                   static_cast<size_t>(n) >= sizeof(rtes::BboUpdateMessage)) {
            const auto* bbo = reinterpret_cast<const rtes::BboUpdateMessage*>(buf);
            char sym[9] = {};
            std::memcpy(sym, bbo->symbol, 8);  // sym[8] stays NUL
            double bid = static_cast<double>(bbo->bid_price) / 10000.0;
            double ask = static_cast<double>(bbo->ask_price) / 10000.0;
            std::cout << "[BBO]   seq=" << hdr->sequence
                      << " symbol=" << sym
                      << " bid=$" << bid << "(" << bbo->bid_quantity << ")"
                      << " ask=$" << ask << "(" << bbo->ask_quantity << ")\n";
        }
    }

    std::cout << "\n=== Summary ===\n"
              << "  messages : " << msg_count << "\n"
              << "  gaps     : " << gap_count << "\n";

    ::close(fd);
    return 0;
}
