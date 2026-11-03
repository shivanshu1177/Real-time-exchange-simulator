// Order lifecycle through GatewayCore + real MatchingEngines, in-process and
// single-threaded (engines are driven with poll_once, no sockets, no threads).
#include <gtest/gtest.h>
#include "rtes/gateway_core.hpp"
#include <algorithm>
#include <cstring>
#include <vector>

using namespace rtes;

namespace {

constexpr Price px(double dollars) { return static_cast<Price>(dollars * 10000 + 0.5); }

struct Sent {
    SessionId            session;
    std::vector<uint8_t> bytes;
    MessageType type() const {
        MessageHeader h; std::memcpy(&h, bytes.data(), sizeof(h));
        return static_cast<MessageType>(h.type);
    }
};

struct CaptureSink : OutputSink {
    std::vector<Sent>      sent;
    std::vector<SessionId> disconnected;
    void send(SessionId s, const void* data, size_t len) override {
        auto* p = static_cast<const uint8_t*>(data);
        sent.push_back({s, std::vector<uint8_t>(p, p + len)});
    }
    void disconnect(SessionId s) override { disconnected.push_back(s); }
};

class Lifecycle : public ::testing::Test {
protected:
    RiskConfig risk_cfg{/*max_order_size*/ 10000, /*max_notional*/ 1e12,
                        /*max_orders_per_second*/ 1000, /*collar*/ true};
    std::vector<SymbolConfig> symbols{{"AAPL", 0.01, 1, 10.0, StpMode::CANCEL_TAKER, 150.0},
                                      {"MSFT", 0.01, 1, 10.0, StpMode::CANCEL_TAKER, 150.0}};
    std::unique_ptr<RiskManager> risk;
    MatchingEngine aapl{"AAPL", 0, 4096, 1024};
    MatchingEngine msft{"MSFT", 1, 4096, 1024};
    CaptureSink sink;
    std::unique_ptr<GatewayCore> gw;
    SessionId A{}, B{};
    size_t cursor = 0;   // first message not yet consumed by take()

    void SetUp() override { make_gateway(); }

    void make_gateway(bool cancel_on_disconnect = true) {
        risk = std::make_unique<RiskManager>(risk_cfg, symbols);
        gw = std::make_unique<GatewayCore>(*risk, std::vector<MatchingEngine*>{&aapl, &msft},
                                           sink, 16384, cancel_on_disconnect);
        A = gw->open_session();
        B = gw->open_session();
        logon(A, A);   // client id = session id unless a test says otherwise
        logon(B, B);
        take();        // drop the LOGON_ACKs
    }

    void logon(SessionId s, ClientID client_id, uint32_t version = PROTOCOL_VERSION) {
        LogonMessage m{};
        m.protocol_version = version; m.client_id = client_id;
        seal_message(m, MessageType::LOGON, 1, 0);
        gw->on_message(s, reinterpret_cast<const uint8_t*>(&m), sizeof(m));
    }

    // Run engines and deliver their events, until quiet.
    void pump() {
        while (aapl.poll_once() + msft.poll_once() + gw->drain_events() > 0) {}
    }

    void new_order(SessionId s, uint64_t coid, uint8_t side, Price price, uint32_t qty,
                   const char* sym = "AAPL", uint8_t type = 1) {
        NewOrderMessage m{};
        m.client_order_id = coid; m.client_id = s;
        std::memcpy(m.symbol, sym, std::min(std::strlen(sym), sizeof(m.symbol)));
        m.side = side; m.order_type = type; m.price = price; m.quantity = qty;
        seal_message(m, MessageType::NEW_ORDER, 1, 0);
        gw->on_message(s, reinterpret_cast<const uint8_t*>(&m), sizeof(m));
    }
    void buy(SessionId s, uint64_t coid, Price p, uint32_t q)  { new_order(s, coid, 0, p, q); }
    void sell(SessionId s, uint64_t coid, Price p, uint32_t q) { new_order(s, coid, 1, p, q); }

    void cancel(SessionId s, uint64_t coid, uint64_t order_id = 0) {
        CancelOrderMessage m{};
        m.order_id = order_id; m.client_order_id = coid;
        seal_message(m, MessageType::CANCEL_ORDER, 1, 0);
        gw->on_message(s, reinterpret_cast<const uint8_t*>(&m), sizeof(m));
    }
    void modify(SessionId s, uint64_t coid, Price p, uint32_t q) {
        ModifyOrderMessage m{};
        m.client_order_id = coid; m.new_price = p; m.new_quantity = q;
        seal_message(m, MessageType::MODIFY_ORDER, 1, 0);
        gw->on_message(s, reinterpret_cast<const uint8_t*>(&m), sizeof(m));
    }

    // Messages sent since the last take().
    std::vector<Sent> take() {
        std::vector<Sent> out(sink.sent.begin() + static_cast<long>(cursor), sink.sent.end());
        cursor = sink.sent.size();
        return out;
    }
    // Wire structs are packed; gtest binds arguments by reference, which is
    // undefined behaviour on misaligned fields. Tests decode into these
    // plain (aligned) copies instead.
    struct Report {
        uint64_t order_id, client_order_id, trade_id, price, last_px;
        uint32_t last_qty, leaves_qty, cum_qty;
        uint8_t  exec_type, side, reject_reason;
        char     symbol[8];
    };
    struct Reject {
        uint64_t order_id, client_order_id;
        uint8_t  reject_reason, response_to;
    };
    static Report er(const Sent& m) {
        EXPECT_EQ(m.type(), MessageType::EXEC_REPORT);
        ExecReportMessage w{}; std::memcpy(&w, m.bytes.data(), sizeof(w));
        Report r{w.order_id, w.client_order_id, w.trade_id, w.price, w.last_px,
                 w.last_qty, w.leaves_qty, w.cum_qty, w.exec_type, w.side, w.reject_reason, {}};
        std::memcpy(r.symbol, w.symbol, sizeof(r.symbol));
        return r;
    }
    static Reject cr(const Sent& m) {
        EXPECT_EQ(m.type(), MessageType::CANCEL_REJECT);
        CancelRejectMessage w{}; std::memcpy(&w, m.bytes.data(), sizeof(w));
        return Reject{w.order_id, w.client_order_id, w.reject_reason, w.response_to};
    }
    static ExecType type_of(const Report& r) { return static_cast<ExecType>(r.exec_type); }

    // Submit and expect exactly one REJECTED with the given reason, and no
    // request reaching an engine.
    void expect_rejected(RejectReason reason) {
        EXPECT_EQ(aapl.poll_once() + msft.poll_once(), 0u) << "rejected order reached an engine";
        auto msgs = take();
        ASSERT_EQ(msgs.size(), 1u);
        auto r = er(msgs[0]);
        EXPECT_EQ(type_of(r), ExecType::REJECTED);
        EXPECT_EQ(static_cast<RejectReason>(r.reject_reason), reason);
        EXPECT_EQ(r.order_id, 0u);
    }

    OrderID rest(SessionId s, uint64_t coid, uint8_t side, Price p, uint32_t q) {
        new_order(s, coid, side, p, q);
        pump();
        auto msgs = take();
        EXPECT_EQ(msgs.size(), 1u);
        auto r = er(msgs.at(0));
        EXPECT_EQ(type_of(r), ExecType::NEW);
        return r.order_id;
    }
};

} // namespace

TEST_F(Lifecycle, NewOrderGetsExactlyOneNewReport) {
    buy(A, 1, px(150), 100);
    pump();
    auto msgs = take();
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].session, A);
    auto r = er(msgs[0]);
    EXPECT_EQ(type_of(r), ExecType::NEW);
    EXPECT_NE(r.order_id, 0u);
    EXPECT_EQ(r.client_order_id, 1u);
    EXPECT_EQ(r.leaves_qty, 100u);
    EXPECT_EQ(r.cum_qty, 0u);
    EXPECT_EQ(std::memcmp(r.symbol, "AAPL", 4), 0);
}

TEST_F(Lifecycle, RejectReasonsReachTheClientBeforeAnything) {
    buy(A, 1, px(150), 20000);            expect_rejected(RejectReason::ORDER_SIZE_LIMIT);
    buy(A, 2, px(200), 10);               expect_rejected(RejectReason::PRICE_COLLAR);
    new_order(A, 3, 0, px(150), 10, "XXXX"); expect_rejected(RejectReason::UNKNOWN_SYMBOL);
    buy(A, 4, px(150), 0);                expect_rejected(RejectReason::INVALID_QUANTITY);
    buy(A, 5, 0, 10);                     expect_rejected(RejectReason::INVALID_PRICE);
    new_order(A, 6, 7, px(150), 10);      expect_rejected(RejectReason::INVALID_SIDE);
    new_order(A, 7, 0, px(150), 10, "AAPL", 9); expect_rejected(RejectReason::INVALID_ORDER_TYPE);
}

TEST_F(Lifecycle, DuplicateClientOrderIdRejectedWhileLive) {
    rest(A, 30, 0, px(150), 10);
    buy(A, 30, px(150), 10);
    expect_rejected(RejectReason::DUPLICATE_ORDER_ID);

    // Once the order is done the id may be reused.
    cancel(A, 30);
    pump();
    ASSERT_EQ(type_of(er(take().at(0))), ExecType::CANCELED);
    rest(A, 30, 0, px(150), 10);
}

TEST_F(Lifecycle, RateLimitRejectIsReported) {
    risk_cfg.max_orders_per_second = 3;
    make_gateway();
    for (uint64_t i = 1; i <= 3; ++i) rest(A, i, 0, px(150), 1);
    buy(A, 4, px(150), 1);
    expect_rejected(RejectReason::RATE_LIMIT);
}

TEST_F(Lifecycle, CancelRemovesOrderSoItCannotTrade) {
    rest(A, 1, 0, px(150), 10);
    cancel(A, 1);
    pump();
    auto msgs = take();
    ASSERT_EQ(msgs.size(), 1u);
    auto c = er(msgs[0]);
    EXPECT_EQ(type_of(c), ExecType::CANCELED);
    EXPECT_EQ(c.leaves_qty, 0u);
    EXPECT_EQ(gw->live_orders(A), 0u);

    // A crossing sell must not trade with the cancelled order.
    sell(B, 1, px(150), 10);
    pump();
    msgs = take();
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].session, B);
    EXPECT_EQ(type_of(er(msgs[0])), ExecType::NEW);
}

TEST_F(Lifecycle, CancelOfAnotherSessionsOrderIsRejected) {
    OrderID id = rest(A, 1, 0, px(150), 10);

    cancel(B, 0, id);            // by exchange order id: engine checks the owner
    pump();
    auto msgs = take();
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].session, B);
    EXPECT_EQ(static_cast<RejectReason>(cr(msgs[0]).reject_reason), RejectReason::NOT_OWNER);

    cancel(B, 1);                // A's client order id means nothing in B's session
    pump();
    msgs = take();
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(static_cast<RejectReason>(cr(msgs[0]).reject_reason), RejectReason::UNKNOWN_ORDER);

    cancel(A, 1);                // the owner still can
    pump();
    EXPECT_EQ(type_of(er(take().at(0))), ExecType::CANCELED);
}

TEST_F(Lifecycle, CancelAfterFillIsRejected) {
    rest(A, 1, 0, px(150), 10);
    sell(B, 1, px(150), 10);
    pump();
    take();
    cancel(A, 1);
    pump();
    auto msgs = take();
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(static_cast<RejectReason>(cr(msgs[0]).reject_reason), RejectReason::UNKNOWN_ORDER);
}

TEST_F(Lifecycle, CancelRacingAFillIsTooLate) {
    rest(A, 1, 0, px(150), 10);
    sell(B, 1, px(150), 10);   // queued first
    cancel(A, 1);              // queued behind the sell
    pump();
    bool too_late = false, filled = false;
    for (auto& m : take()) {
        if (m.session != A) continue;
        if (m.type() == MessageType::CANCEL_REJECT)
            too_late = static_cast<RejectReason>(cr(m).reject_reason) == RejectReason::TOO_LATE;
        else if (type_of(er(m)) == ExecType::FILL)
            filled = true;
    }
    EXPECT_TRUE(filled);
    EXPECT_TRUE(too_late);
}

TEST_F(Lifecycle, PartialFillsReportToBothSidesWithConsistentQuantities) {
    rest(A, 1, 0, px(150), 10);
    sell(B, 1, px(150), 4);
    pump();
    auto msgs = take();
    // B: NEW then FILL; A: PARTIAL_FILL
    Report a{}, b_fill{};
    int a_count = 0, b_fill_count = 0;
    for (auto& m : msgs) {
        auto r = er(m);
        EXPECT_EQ(r.leaves_qty + r.cum_qty, m.session == A ? 10u : 4u);
        if (m.session == A) { a = r; ++a_count; }
        if (m.session == B && type_of(r) == ExecType::FILL) { b_fill = r; ++b_fill_count; }
    }
    ASSERT_EQ(a_count, 1);
    ASSERT_EQ(b_fill_count, 1);
    EXPECT_EQ(type_of(a), ExecType::PARTIAL_FILL);
    EXPECT_EQ(a.last_qty, 4u);  EXPECT_EQ(a.leaves_qty, 6u);  EXPECT_EQ(a.cum_qty, 4u);
    EXPECT_EQ(a.last_px, px(150));
    EXPECT_EQ(b_fill.last_qty, 4u); EXPECT_EQ(b_fill.leaves_qty, 0u); EXPECT_EQ(b_fill.cum_qty, 4u);
    EXPECT_EQ(a.trade_id, b_fill.trade_id);
    EXPECT_NE(a.trade_id, 0u);

    sell(B, 2, px(150), 6);
    pump();
    for (auto& m : take()) {
        auto r = er(m);
        if (m.session == A) {
            EXPECT_EQ(type_of(r), ExecType::FILL);
            EXPECT_EQ(r.leaves_qty, 0u);
            EXPECT_EQ(r.cum_qty, 10u);
        }
    }
    EXPECT_EQ(gw->live_orders(A), 0u);
}

TEST_F(Lifecycle, SameClientOrderIdInTwoSessionsIsIndependent) {
    OrderID a = rest(A, 30, 0, px(150), 10);
    OrderID b = rest(B, 30, 1, px(151), 10);
    EXPECT_NE(a, b);

    cancel(A, 30);
    pump();
    auto msgs = take();
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].session, A);
    EXPECT_EQ(er(msgs[0]).order_id, a);

    cancel(B, 30);
    pump();
    msgs = take();
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(er(msgs[0]).order_id, b);
}

TEST_F(Lifecycle, ModifyQuantityDownKeepsPriority) {
    rest(A, 1, 0, px(150), 10);    // X, first in queue
    rest(A, 2, 0, px(150), 10);    // Y
    modify(A, 1, px(150), 5);
    pump();
    auto r = er(take().at(0));
    EXPECT_EQ(type_of(r), ExecType::REPLACED);
    EXPECT_EQ(r.leaves_qty, 5u);

    sell(B, 1, px(150), 5);
    pump();
    std::vector<uint64_t> filled_coids;
    for (auto& m : take())
        if (m.session == A) filled_coids.push_back(er(m).client_order_id);
    ASSERT_EQ(filled_coids.size(), 1u);
    EXPECT_EQ(filled_coids[0], 1u) << "X should still be first";
}

TEST_F(Lifecycle, ModifyQuantityUpLosesPriority) {
    rest(A, 1, 0, px(150), 10);
    rest(A, 2, 0, px(150), 10);
    modify(A, 1, px(150), 15);
    pump();
    EXPECT_EQ(type_of(er(take().at(0))), ExecType::REPLACED);

    sell(B, 1, px(150), 5);
    pump();
    std::vector<uint64_t> filled_coids;
    for (auto& m : take())
        if (m.session == A) filled_coids.push_back(er(m).client_order_id);
    ASSERT_EQ(filled_coids.size(), 1u);
    EXPECT_EQ(filled_coids[0], 2u) << "Y should now be first";
}

TEST_F(Lifecycle, ModifyPriceLosesPriority) {
    rest(A, 1, 0, px(149.99), 10);   // X, worse price
    rest(A, 2, 0, px(150), 10);      // Y
    modify(A, 1, px(150), 10);       // X joins Y's level, behind Y
    pump();
    take();
    sell(B, 1, px(150), 5);
    pump();
    std::vector<uint64_t> filled_coids;
    for (auto& m : take())
        if (m.session == A) filled_coids.push_back(er(m).client_order_id);
    ASSERT_EQ(filled_coids.size(), 1u);
    EXPECT_EQ(filled_coids[0], 2u);
}

TEST_F(Lifecycle, ModifyToCrossingPriceTrades) {
    rest(B, 1, 1, px(151), 10);
    rest(A, 1, 0, px(150), 10);
    modify(A, 1, px(151), 10);
    pump();
    auto msgs = take();
    ASSERT_GE(msgs.size(), 3u);
    EXPECT_EQ(type_of(er(msgs[0])), ExecType::REPLACED);
    int fills = 0;
    for (auto& m : msgs) if (type_of(er(m)) == ExecType::FILL) ++fills;
    EXPECT_EQ(fills, 2);
}

TEST_F(Lifecycle, ModifyRejects) {
    rest(A, 1, 0, px(150), 10);
    modify(A, 99, px(150), 5);           // unknown
    pump();
    EXPECT_EQ(static_cast<RejectReason>(cr(take().at(0)).reject_reason), RejectReason::UNKNOWN_ORDER);
    modify(A, 1, px(200), 5);            // outside collar
    pump();
    EXPECT_EQ(static_cast<RejectReason>(cr(take().at(0)).reject_reason), RejectReason::PRICE_COLLAR);

    sell(B, 1, px(150), 4);              // A now has cum 4
    pump();
    take();
    modify(A, 1, px(150), 4);            // new total must exceed what is filled
    pump();
    auto r = cr(take().at(0));
    EXPECT_EQ(static_cast<RejectReason>(r.reject_reason), RejectReason::INVALID_QUANTITY);
    EXPECT_EQ(static_cast<CancelRejectResponseTo>(r.response_to), CancelRejectResponseTo::MODIFY);
}

TEST_F(Lifecycle, OrderIdsEncodeTheSymbol) {
    OrderID a = rest(A, 1, 0, px(150), 1);
    new_order(A, 2, 0, px(150), 1, "MSFT");
    pump();
    OrderID m = er(take().at(0)).order_id;
    EXPECT_EQ(GatewayCore::symbol_index_of(a), 0);
    EXPECT_EQ(GatewayCore::symbol_index_of(m), 1);
}

// Engines on their own threads, this thread acting as the gateway thread:
// exercises both SPSC directions (requests in, execution events out) so the
// TSan preset sees the real cross-thread hand-off.
TEST_F(Lifecycle, ThreadedEnginesDeliverEveryReport) {
    risk_cfg.max_orders_per_second = 1000000;
    make_gateway();
    aapl.start();
    msft.start();

    constexpr uint64_t N = 400;   // N buys rest, N sells cross them one-for-one
    for (uint64_t i = 1; i <= N; ++i) {
        buy(A, i, px(150), 1);
        sell(B, i, px(150), 1);
        gw->drain_events();
    }
    // Expected: per pair, A NEW + A FILL + B NEW + B FILL.
    const size_t expected = cursor + 4 * N;   // cursor: messages before the test (LOGON_ACKs)
    for (int spins = 0; sink.sent.size() < expected && spins < 2000000; ++spins) {
        gw->drain_events();
    }
    aapl.stop();
    msft.stop();
    gw->drain_events();

    auto msgs = take();
    ASSERT_EQ(msgs.size(), 4 * N);
    size_t fills = 0;
    for (auto& m : msgs) {
        auto r = er(m);
        if (type_of(r) == ExecType::FILL) ++fills;
    }
    EXPECT_EQ(fills, 2 * N);
    EXPECT_EQ(gw->live_orders(A), 0u);
    EXPECT_EQ(gw->live_orders(B), 0u);
    EXPECT_EQ(aapl.events_dropped(), 0u);
}

TEST_F(Lifecycle, OrdersBeforeLogonAreRejected) {
    SessionId c = gw->open_session();
    new_order(c, 1, 0, px(150), 10);
    expect_rejected(RejectReason::NOT_LOGGED_IN);
    cancel(c, 1);
    auto msgs = take();
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(static_cast<RejectReason>(cr(msgs[0]).reject_reason), RejectReason::NOT_LOGGED_IN);
}

TEST_F(Lifecycle, PayloadClientIdIsIgnored) {
    // Two sessions logged on as client 7 share client 7's open-order limit,
    // whatever client_id their orders carry in the payload.
    risk_cfg.max_open_orders_per_client = 1;
    make_gateway();
    SessionId c = gw->open_session();
    SessionId d = gw->open_session();
    logon(c, 7);
    logon(d, 7);
    take();

    NewOrderMessage m{};
    std::memcpy(m.symbol, "AAPL", 4);
    m.side = 0; m.order_type = 1; m.price = px(150); m.quantity = 1;

    m.client_order_id = 1; m.client_id = 100;
    seal_message(m, MessageType::NEW_ORDER, 1, 0);
    gw->on_message(c, reinterpret_cast<const uint8_t*>(&m), sizeof(m));
    pump();
    EXPECT_EQ(type_of(er(take().at(0))), ExecType::NEW);

    m.client_order_id = 1; m.client_id = 200;   // pretends to be someone else
    seal_message(m, MessageType::NEW_ORDER, 2, 0);
    gw->on_message(d, reinterpret_cast<const uint8_t*>(&m), sizeof(m));
    expect_rejected(RejectReason::OPEN_ORDER_LIMIT);
    EXPECT_EQ(risk->open_orders(7), 1u);
    EXPECT_EQ(risk->open_orders(100), 0u);
    EXPECT_EQ(risk->open_orders(200), 0u);
}

TEST_F(Lifecycle, CloseSessionCancelsItsRestingOrders) {
    rest(A, 1, 0, px(150), 10);
    rest(A, 2, 0, px(149), 10);
    gw->close_session(A);
    pump();
    EXPECT_TRUE(take().empty()) << "reports for a closed session must be dropped";

    sell(B, 1, px(149), 20);   // would hit both if they were still there
    pump();
    auto msgs = take();
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(type_of(er(msgs[0])), ExecType::NEW);
}

TEST_F(Lifecycle, CloseSessionWithoutCancelOnDisconnectKeepsOrders) {
    make_gateway(/*cancel_on_disconnect=*/false);
    rest(A, 1, 0, px(150), 10);
    gw->close_session(A);
    sell(B, 1, px(150), 10);
    pump();
    int fills = 0;
    for (auto& m : take()) if (type_of(er(m)) == ExecType::FILL) ++fills;
    EXPECT_EQ(fills, 1);   // only B is left to be told
}

TEST_F(Lifecycle, OffTickAndOffLotAreRejected) {
    symbols[0].lot_size = 10;                 // AAPL: tick 0.01, lot 10
    make_gateway();
    buy(A, 1, px(150) + 50, 10);              // 150.005: not a multiple of 0.01
    expect_rejected(RejectReason::OFF_TICK);
    buy(A, 2, px(150), 15);
    expect_rejected(RejectReason::OFF_LOT);
    rest(A, 3, 0, px(150.01), 20);
    modify(A, 3, px(150) + 1, 20);
    pump();
    EXPECT_EQ(static_cast<RejectReason>(cr(take().at(0)).reject_reason), RejectReason::OFF_TICK);
}

TEST_F(Lifecycle, MarketOrderRemainderIsCancelledWithNoLiquidity) {
    rest(B, 1, 1, px(150), 4);
    new_order(A, 1, 0, 0, 10, "AAPL", /*MARKET*/ 0);
    pump();
    std::vector<Report> mine;
    for (auto& m : take()) if (m.session == A) mine.push_back(er(m));
    ASSERT_EQ(mine.size(), 3u);
    EXPECT_EQ(type_of(mine[0]), ExecType::NEW);
    EXPECT_EQ(type_of(mine[1]), ExecType::PARTIAL_FILL);
    EXPECT_EQ(type_of(mine[2]), ExecType::CANCELED);
    EXPECT_EQ(static_cast<RejectReason>(mine[2].reject_reason), RejectReason::NO_LIQUIDITY);
    EXPECT_EQ(mine[2].cum_qty, 4u);
    EXPECT_EQ(mine[2].leaves_qty, 0u);
    EXPECT_EQ(gw->live_orders(A), 0u);
}

TEST_F(Lifecycle, SelfTradeIsPreventedAndReported) {
    OrderID sell_id = rest(A, 1, 1, px(150), 10);
    buy(A, 2, px(150), 5);                    // same client: default STP cancels the taker
    pump();
    auto msgs = take();
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(type_of(er(msgs[0])), ExecType::NEW);
    auto c = er(msgs[1]);
    EXPECT_EQ(type_of(c), ExecType::CANCELED);
    EXPECT_EQ(c.client_order_id, 2u);
    EXPECT_EQ(static_cast<RejectReason>(c.reject_reason), RejectReason::SELF_TRADE);
    cancel(A, 0, sell_id);                    // the resting sell is untouched
    pump();
    auto r = er(take().at(0));
    EXPECT_EQ(type_of(r), ExecType::CANCELED);
    EXPECT_EQ(r.cum_qty, 0u);
}

TEST_F(Lifecycle, BboUpdatesCarryRealQuantities) {
    MPMCQueue<MarketDataEvent> md(1024);
    aapl.set_market_data_queue(&md);
    rest(A, 1, 0, px(150), 10);
    rest(A, 2, 0, px(150), 5);
    rest(B, 1, 1, px(151), 7);
    sell(B, 2, px(150), 3);                   // takes 3 from the bid
    pump();
    MarketDataEvent ev, last_bbo;
    int trades = 0;
    while (md.pop(ev)) {
        if (ev.type == MarketDataEvent::BBO_UPDATE) last_bbo = ev;
        else ++trades;
    }
    EXPECT_EQ(trades, 1);
    EXPECT_EQ(last_bbo.bbo.bid_price, px(150));
    EXPECT_EQ(last_bbo.bbo.bid_quantity, 12u);
    EXPECT_EQ(last_bbo.bbo.ask_price, px(151));
    EXPECT_EQ(last_bbo.bbo.ask_quantity, 7u);
    aapl.set_market_data_queue(nullptr);
}

TEST_F(Lifecycle, NotionalIsReleasedByCancelAndByFill) {
    risk_cfg.max_notional_per_client = 2000.0;    // room for one 10 @ 150 order
    make_gateway();
    rest(A, 1, 0, px(150), 10);
    buy(A, 2, px(150), 10);
    expect_rejected(RejectReason::NOTIONAL_LIMIT);

    cancel(A, 1);
    pump();
    take();
    EXPECT_EQ(risk->open_notional(A), 0u);
    rest(A, 2, 0, px(150), 10);                   // fits again after the cancel

    sell(B, 1, px(150), 10);                      // fill releases A's exposure
    pump();
    take();
    EXPECT_EQ(risk->open_notional(A), 0u);
    EXPECT_EQ(risk->open_orders(A), 0u);
    rest(A, 3, 0, px(150), 10);
}

TEST_F(Lifecycle, PartialFillReleasesOnlyTheFilledPart) {
    rest(A, 1, 0, px(150), 10);
    sell(B, 1, px(150), 4);
    pump();
    take();
    EXPECT_EQ(risk->open_notional(A), px(150) * 6);
    EXPECT_EQ(risk->open_orders(A), 1u);
    modify(A, 1, px(149), 8);                     // cum 4: leaves 4 at 149
    pump();
    take();
    EXPECT_EQ(risk->open_notional(A), px(149) * 4);
}

TEST_F(Lifecycle, DisconnectReleasesExposureThroughTheCancels) {
    rest(A, 1, 0, px(150), 10);
    rest(A, 2, 0, px(149), 10);
    EXPECT_EQ(risk->open_orders(A), 2u);
    gw->close_session(A);
    EXPECT_FALSE(gw->has_session(A));
    EXPECT_EQ(gw->tracked_sessions(), 2u);        // A kept until its cancels come back
    pump();
    EXPECT_TRUE(take().empty());                  // nothing is sent to a gone session
    EXPECT_EQ(risk->open_orders(A), 0u);
    EXPECT_EQ(risk->open_notional(A), 0u);
    EXPECT_EQ(gw->tracked_sessions(), 1u);        // then forgotten
}

TEST_F(Lifecycle, GoneSessionOrdersStillReleaseRiskWhenTheyFill) {
    make_gateway(/*cancel_on_disconnect=*/false);
    rest(A, 1, 0, px(150), 10);
    gw->close_session(A);
    EXPECT_EQ(risk->open_orders(A), 1u);
    sell(B, 1, px(150), 10);
    pump();
    EXPECT_EQ(risk->open_orders(A), 0u);
    EXPECT_EQ(gw->tracked_sessions(), 1u);
}

TEST_F(Lifecycle, CollarFollowsTheLastTrade) {
    buy(A, 1, px(170), 1);                        // collar 135..165 around ref 150
    expect_rejected(RejectReason::PRICE_COLLAR);
    rest(A, 2, 0, px(160), 5);
    sell(B, 1, px(160), 5);                       // trade at 160: collar 144..176
    pump();
    take();
    EXPECT_EQ(risk->reference_price(0), px(160));
    rest(A, 3, 0, px(170), 1);
}

TEST_F(Lifecycle, EngineRejectsWhenItsPoolIsFull) {
    // Each engine has 1024 slots; fill AAPL with resting orders from many clients.
    risk_cfg.max_orders_per_second = 1000000;
    make_gateway();
    for (uint64_t i = 1; i <= 1024; ++i) {
        buy(A, i, px(150), 1);
        if (i % 256 == 0) pump();
    }
    pump();
    take();
    buy(A, 2000, px(150), 1);
    pump();
    auto r = er(take().at(0));
    EXPECT_EQ(type_of(r), ExecType::REJECTED);
    EXPECT_EQ(static_cast<RejectReason>(r.reject_reason), RejectReason::SYSTEM_BUSY);
    EXPECT_EQ(risk->open_orders(A), 1024u);       // the rejected order released its reservation
    EXPECT_EQ(aapl.pool_available(), 0u);
}
