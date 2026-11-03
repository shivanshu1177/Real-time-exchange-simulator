// TCP framing, malformed input, logon and cancel-on-disconnect.
// ClientConnection is tested on a socketpair; TcpGateway over real TCP
// (127.0.0.1, OS-assigned port) with matching engines on their own threads.
#include <gtest/gtest.h>
#include "rtes/tcp_gateway.hpp"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <optional>
#include <vector>

using namespace rtes;

namespace {

constexpr Price px(double dollars) { return static_cast<Price>(dollars * 10000 + 0.5); }

template<typename Msg>
std::vector<uint8_t> bytes_of(const Msg& m) {
    auto* p = reinterpret_cast<const uint8_t*>(&m);
    return std::vector<uint8_t>(p, p + sizeof(m));
}

std::vector<uint8_t> new_order_bytes(uint64_t coid, uint8_t side, Price price, uint32_t qty,
                                     uint64_t payload_client_id = 0) {
    NewOrderMessage m{};
    m.client_order_id = coid; m.client_id = payload_client_id;
    std::memcpy(m.symbol, "AAPL", 4);
    m.side = side; m.order_type = 1; m.price = price; m.quantity = qty;
    seal_message(m, MessageType::NEW_ORDER, coid, 0);
    return bytes_of(m);
}

std::vector<uint8_t> logon_bytes(ClientID id, uint32_t version = PROTOCOL_VERSION) {
    LogonMessage m{};
    m.protocol_version = version; m.client_id = id;
    seal_message(m, MessageType::LOGON, 1, 0);
    return bytes_of(m);
}

std::vector<uint8_t> cancel_bytes(uint64_t coid) {
    CancelOrderMessage m{};
    m.client_order_id = coid;
    seal_message(m, MessageType::CANCEL_ORDER, 1, 0);
    return bytes_of(m);
}

void write_all(int fd, const uint8_t* p, size_t n) {
    while (n > 0) {
        ssize_t w = ::send(fd, p, n, MSG_NOSIGNAL);
        ASSERT_GT(w, 0);
        p += w; n -= static_cast<size_t>(w);
    }
}
void write_all(int fd, const std::vector<uint8_t>& v) { write_all(fd, v.data(), v.size()); }

// ---- ClientConnection on a socketpair --------------------------------------

struct Pair {
    int client = -1;
    std::unique_ptr<ClientConnection> conn;   // owns the server end
    Pair() {
        int sv[2];
        EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        ::fcntl(sv[1], F_SETFL, ::fcntl(sv[1], F_GETFL, 0) | O_NONBLOCK);
        client = sv[0];
        conn = std::make_unique<ClientConnection>(sv[1], 1);
    }
    ~Pair() { ::close(client); }

    // Read what arrived and return every complete frame.
    std::vector<std::vector<uint8_t>> frames() {
        std::vector<std::vector<uint8_t>> out;
        EXPECT_EQ(conn->read_available(), ClientConnection::ReadResult::OK);
        const uint8_t* f = nullptr;
        while (true) {
            auto fc = conn->next_frame(f);
            if (fc.status != FrameCheck::OK) {
                EXPECT_EQ(fc.status, FrameCheck::NEED_MORE);
                break;
            }
            out.emplace_back(f, f + fc.length);
            conn->consume(fc.length);
        }
        return out;
    }
};

} // namespace

TEST(Framing, FrameSplitAtEveryByteBoundaryParsesExactlyOnce) {
    const auto msg = new_order_bytes(7, 0, px(150), 10);
    Pair p;
    for (size_t k = 1; k < msg.size(); ++k) {
        write_all(p.client, msg.data(), k);
        EXPECT_TRUE(p.frames().empty()) << "split at " << k;
        write_all(p.client, msg.data() + k, msg.size() - k);
        auto got = p.frames();
        ASSERT_EQ(got.size(), 1u) << "split at " << k;
        EXPECT_EQ(got[0], msg) << "split at " << k;
        EXPECT_EQ(p.conn->buffered(), 0u);
    }
}

TEST(Framing, ThreeMessagesInOneWriteGiveThreeFrames) {
    auto a = logon_bytes(5);
    auto b = new_order_bytes(1, 0, px(150), 10);
    auto c = cancel_bytes(1);
    std::vector<uint8_t> all;
    for (auto* v : {&a, &b, &c}) all.insert(all.end(), v->begin(), v->end());
    Pair p;
    write_all(p.client, all);
    auto got = p.frames();
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], a);
    EXPECT_EQ(got[1], b);
    EXPECT_EQ(got[2], c);
}

TEST(Framing, ByteAtATimeStreamParsesIdentically) {
    std::vector<std::vector<uint8_t>> msgs = {logon_bytes(5), new_order_bytes(1, 0, px(150), 10),
                                              new_order_bytes(2, 1, px(151), 3), cancel_bytes(1)};
    std::vector<uint8_t> all;
    for (auto& m : msgs) all.insert(all.end(), m.begin(), m.end());
    Pair p;
    std::vector<std::vector<uint8_t>> got;
    for (uint8_t byte : all) {
        write_all(p.client, &byte, 1);
        for (auto& f : p.frames()) got.push_back(f);
    }
    EXPECT_EQ(got, msgs);
}

// ---- TcpGateway over real TCP ---------------------------------------------

namespace {

class GatewayTcp : public ::testing::Test {
protected:
    RiskConfig risk_cfg{10000, 1e12, 100000, true};
    std::vector<SymbolConfig> symbols{{"AAPL", 0.01, 1, 10.0, StpMode::CANCEL_TAKER, 150.0}};
    RiskManager risk{risk_cfg, symbols};
    MatchingEngine aapl{"AAPL", 0, 4096, 4096};
    std::unique_ptr<TcpGateway> gw;

    void start(bool cancel_on_disconnect = true) {
        gw = std::make_unique<TcpGateway>(0, risk, std::vector<MatchingEngine*>{&aapl},
                                          cancel_on_disconnect);
        ASSERT_TRUE(gw->start());
        aapl.start();
    }
    void TearDown() override {
        if (gw) gw->stop();
        aapl.stop();
    }

    int connect_client() {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(gw->bound_port());
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        return fd;
    }

    // Read one whole message, or nullopt on timeout/EOF.
    static std::optional<std::vector<uint8_t>> read_msg(int fd, int timeout_ms = 2000) {
        std::vector<uint8_t> buf(sizeof(MessageHeader));
        size_t got = 0, want = sizeof(MessageHeader);
        while (got < want) {
            pollfd pfd{fd, POLLIN, 0};
            if (::poll(&pfd, 1, timeout_ms) <= 0) return std::nullopt;
            ssize_t n = ::recv(fd, buf.data() + got, want - got, 0);
            if (n <= 0) return std::nullopt;
            got += static_cast<size_t>(n);
            if (got == sizeof(MessageHeader)) {
                MessageHeader h; std::memcpy(&h, buf.data(), sizeof(h));
                want = h.length;
                buf.resize(want);
            }
        }
        return buf;
    }
    static MessageType type_of(const std::vector<uint8_t>& m) {
        MessageHeader h; std::memcpy(&h, m.data(), sizeof(h));
        return static_cast<MessageType>(h.type);
    }
    // The peer closed: recv returns 0 (an RST also counts as closed).
    static bool closed_by_peer(int fd) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 2000) <= 0) return false;
        char c;
        return ::recv(fd, &c, 1, 0) <= 0;
    }
    static ExecType exec_type(const std::vector<uint8_t>& m) {
        ExecReportMessage r; std::memcpy(&r, m.data(), sizeof(r));
        return static_cast<ExecType>(r.exec_type);
    }
    static uint8_t exec_reject_reason(const std::vector<uint8_t>& m) {
        ExecReportMessage r; std::memcpy(&r, m.data(), sizeof(r));
        return r.reject_reason;
    }

    int logged_on_client(ClientID id) {
        int fd = connect_client();
        write_all(fd, logon_bytes(id));
        auto ack = read_msg(fd);
        EXPECT_TRUE(ack.has_value());
        if (ack) { EXPECT_EQ(type_of(*ack), MessageType::LOGON_ACK); }
        return fd;
    }

    void expect_session_reject(int fd, SessionRejectReason reason) {
        auto m = read_msg(fd);
        ASSERT_TRUE(m.has_value());
        ASSERT_EQ(type_of(*m), MessageType::SESSION_REJECT);
        SessionRejectMessage r; std::memcpy(&r, m->data(), sizeof(r));
        EXPECT_EQ(static_cast<SessionRejectReason>(r.reason), reason);
        EXPECT_TRUE(closed_by_peer(fd));
    }
};

} // namespace

TEST_F(GatewayTcp, OrderSplitAcrossTwoWritesIsAcknowledged) {
    start();
    int fd = logged_on_client(1);
    auto msg = new_order_bytes(1, 0, px(150), 10);
    write_all(fd, msg.data(), 20);
    ::usleep(50000);
    write_all(fd, msg.data() + 20, msg.size() - 20);
    auto r = read_msg(fd);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(exec_type(*r), ExecType::NEW);
    ::close(fd);
}

TEST_F(GatewayTcp, BadChecksumIsRejectedAndClosed) {
    start();
    int fd = logged_on_client(1);
    auto msg = new_order_bytes(1, 0, px(150), 10);
    uint32_t bogus = 0xdeadbeef;
    std::memcpy(msg.data() + offsetof(MessageHeader, checksum), &bogus, 4);
    write_all(fd, msg);
    expect_session_reject(fd, SessionRejectReason::BAD_CHECKSUM);
    ::close(fd);
    EXPECT_GE(gw->stats().protocol_violations.load(), 1u);
}

TEST_F(GatewayTcp, WrongLengthIsRejectedAndClosed) {
    start();
    int fd = logged_on_client(1);
    auto msg = new_order_bytes(1, 0, px(150), 10);
    uint32_t len = 40;
    std::memcpy(msg.data() + offsetof(MessageHeader, length), &len, 4);
    write_all(fd, msg);
    expect_session_reject(fd, SessionRejectReason::BAD_LENGTH);
    ::close(fd);
}

TEST_F(GatewayTcp, HugeLengthIsRejectedFromTheHeaderAlone) {
    start();
    int fd = logged_on_client(1);
    MessageHeader h{};
    h.type = static_cast<uint32_t>(MessageType::NEW_ORDER);
    h.length = 1000000;
    write_all(fd, reinterpret_cast<const uint8_t*>(&h), sizeof(h));   // no body sent
    expect_session_reject(fd, SessionRejectReason::BAD_LENGTH);
    ::close(fd);
}

TEST_F(GatewayTcp, UnknownTypeIsRejectedAndClosed) {
    start();
    int fd = logged_on_client(1);
    MessageHeader h{};
    h.type = 99;
    h.length = sizeof(MessageHeader);
    write_all(fd, reinterpret_cast<const uint8_t*>(&h), sizeof(h));
    expect_session_reject(fd, SessionRejectReason::UNKNOWN_TYPE);
    ::close(fd);
}

TEST_F(GatewayTcp, OrderBeforeLogonIsRejectedButSessionStaysOpen) {
    start();
    int fd = connect_client();
    write_all(fd, new_order_bytes(1, 0, px(150), 10));
    auto r = read_msg(fd);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(exec_type(*r), ExecType::REJECTED);
    EXPECT_EQ(exec_reject_reason(*r), static_cast<uint8_t>(RejectReason::NOT_LOGGED_IN));

    write_all(fd, logon_bytes(1));
    auto ack = read_msg(fd);
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(type_of(*ack), MessageType::LOGON_ACK);
    write_all(fd, new_order_bytes(2, 0, px(150), 10));
    auto n = read_msg(fd);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(exec_type(*n), ExecType::NEW);
    ::close(fd);
}

TEST_F(GatewayTcp, SecondLogonClosesTheSession) {
    start();
    int fd = logged_on_client(1);
    write_all(fd, logon_bytes(2));
    expect_session_reject(fd, SessionRejectReason::DUPLICATE_LOGON);
    ::close(fd);
}

TEST_F(GatewayTcp, WrongProtocolVersionClosesTheSession) {
    start();
    int fd = connect_client();
    write_all(fd, logon_bytes(1, PROTOCOL_VERSION + 1));
    expect_session_reject(fd, SessionRejectReason::UNSUPPORTED_VERSION);
    ::close(fd);
}

TEST_F(GatewayTcp, DisconnectCancelsRestingOrders) {
    start(/*cancel_on_disconnect=*/true);
    int a = logged_on_client(1);
    write_all(a, new_order_bytes(1, 0, px(150), 10));
    auto n = read_msg(a);
    ASSERT_TRUE(n.has_value());
    ASSERT_EQ(exec_type(*n), ExecType::NEW);
    ::close(a);
    ::usleep(100000);   // let the gateway notice and cancel

    int b = logged_on_client(2);
    write_all(b, new_order_bytes(1, 1, px(150), 10));
    auto r = read_msg(b);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(exec_type(*r), ExecType::NEW);
    EXPECT_FALSE(read_msg(b, 300).has_value()) << "the disconnected client's order traded";
    ::close(b);
}

TEST_F(GatewayTcp, WithoutCancelOnDisconnectOrdersSurvive) {
    start(/*cancel_on_disconnect=*/false);
    int a = logged_on_client(1);
    write_all(a, new_order_bytes(1, 0, px(150), 10));
    ASSERT_TRUE(read_msg(a).has_value());
    ::close(a);
    ::usleep(100000);

    int b = logged_on_client(2);
    write_all(b, new_order_bytes(1, 1, px(150), 10));
    auto first = read_msg(b);
    auto second = read_msg(b);
    ASSERT_TRUE(first && second);
    EXPECT_EQ(exec_type(*first), ExecType::NEW);
    EXPECT_EQ(exec_type(*second), ExecType::FILL);
    ::close(b);
}
