// Wire protocol: sizes, field offsets, checksum and encode/decode round trips.
#include <gtest/gtest.h>
#include "rtes/protocol.hpp"
#include "rtes/market_data.hpp"
#include "rtes/flat_map.hpp"
#include <cstddef>
#include <random>
#include <unordered_map>

using namespace rtes;

namespace {

// Wire structs are packed. gtest takes arguments by const reference, and
// binding a reference to a misaligned field is undefined behaviour, so every
// field is copied out by value before it is compared.
template<typename T>
T val(T x) { return x; }

template<typename Msg>
Msg round_trip(const Msg& in) {
    uint8_t wire[sizeof(Msg)];
    std::memcpy(wire, &in, sizeof(Msg));
    Msg out;
    std::memcpy(&out, wire, sizeof(Msg));
    return out;
}

template<typename Msg>
bool checksum_ok(const Msg& m) {
    return m.header.checksum == compute_checksum(
        reinterpret_cast<const uint8_t*>(&m) + sizeof(MessageHeader),
        sizeof(Msg) - sizeof(MessageHeader));
}

} // namespace

TEST(Protocol, Sizes) {
    EXPECT_EQ(sizeof(MessageHeader), 28u);
    EXPECT_EQ(sizeof(NewOrderMessage), 68u);
    EXPECT_EQ(sizeof(CancelOrderMessage), 44u);
    EXPECT_EQ(sizeof(ModifyOrderMessage), 60u);
    EXPECT_EQ(sizeof(ExecReportMessage), 92u);
    EXPECT_EQ(sizeof(CancelRejectMessage), 48u);
    EXPECT_EQ(sizeof(LogonMessage), 60u);
    EXPECT_EQ(sizeof(LogonAckMessage), 36u);
    EXPECT_EQ(sizeof(SessionRejectMessage), 36u);
    EXPECT_EQ(sizeof(BboUpdateMessage), 60u);
    EXPECT_EQ(sizeof(TradeUpdateMessage), 76u);
}

TEST(Protocol, ExecReportOffsetsMatchApiDoc) {
    EXPECT_EQ(offsetof(ExecReportMessage, order_id), 28u);
    EXPECT_EQ(offsetof(ExecReportMessage, client_order_id), 36u);
    EXPECT_EQ(offsetof(ExecReportMessage, trade_id), 44u);
    EXPECT_EQ(offsetof(ExecReportMessage, symbol), 52u);
    EXPECT_EQ(offsetof(ExecReportMessage, price), 60u);
    EXPECT_EQ(offsetof(ExecReportMessage, last_px), 68u);
    EXPECT_EQ(offsetof(ExecReportMessage, last_qty), 76u);
    EXPECT_EQ(offsetof(ExecReportMessage, leaves_qty), 80u);
    EXPECT_EQ(offsetof(ExecReportMessage, cum_qty), 84u);
    EXPECT_EQ(offsetof(ExecReportMessage, exec_type), 88u);
    EXPECT_EQ(offsetof(ExecReportMessage, side), 89u);
    EXPECT_EQ(offsetof(ExecReportMessage, reject_reason), 90u);
    EXPECT_EQ(offsetof(ModifyOrderMessage, new_price), 44u);
    EXPECT_EQ(offsetof(ModifyOrderMessage, new_quantity), 52u);
    EXPECT_EQ(offsetof(CancelRejectMessage, reject_reason), 44u);
    EXPECT_EQ(offsetof(CancelRejectMessage, response_to), 45u);
}

TEST(Protocol, NewOrderRoundTrip) {
    NewOrderMessage m{};
    m.client_order_id = 42; m.client_id = 7;
    std::memcpy(m.symbol, "AAPL", 4);
    m.side = 1; m.order_type = 1; m.price = 1502500; m.quantity = 300;
    seal_message(m, MessageType::NEW_ORDER, 9, 1234);
    auto r = round_trip(m);
    EXPECT_EQ(val(r.header.type), static_cast<uint32_t>(MessageType::NEW_ORDER));
    EXPECT_EQ(val(r.header.length), sizeof(NewOrderMessage));
    EXPECT_EQ(val(r.header.sequence), 9u);
    EXPECT_EQ(val(r.client_order_id), 42u);
    EXPECT_EQ(val(r.price), 1502500u);
    EXPECT_EQ(val(r.quantity), 300u);
    EXPECT_EQ(std::memcmp(r.symbol, "AAPL\0\0\0\0", 8), 0);
    EXPECT_TRUE(checksum_ok(r));
    EXPECT_EQ(check_inbound_frame(reinterpret_cast<const uint8_t*>(&r), sizeof(r)).status, FrameCheck::OK);
}

TEST(Protocol, CancelAndModifyRoundTrip) {
    CancelOrderMessage c{};
    c.order_id = 0x0001000000000005ULL; c.client_order_id = 11;
    seal_message(c, MessageType::CANCEL_ORDER, 1, 2);
    auto rc = round_trip(c);
    EXPECT_EQ(val(rc.order_id), val(c.order_id));
    EXPECT_EQ(val(rc.client_order_id), 11u);
    EXPECT_TRUE(checksum_ok(rc));

    ModifyOrderMessage m{};
    m.client_order_id = 12; m.new_price = 1490000; m.new_quantity = 50;
    seal_message(m, MessageType::MODIFY_ORDER, 3, 4);
    auto rm = round_trip(m);
    EXPECT_EQ(val(rm.header.length), sizeof(ModifyOrderMessage));
    EXPECT_EQ(val(rm.new_price), 1490000u);
    EXPECT_EQ(val(rm.new_quantity), 50u);
    EXPECT_TRUE(checksum_ok(rm));
    EXPECT_EQ(check_inbound_frame(reinterpret_cast<const uint8_t*>(&rm), sizeof(rm)).status, FrameCheck::OK);
}

TEST(Protocol, ExecReportAndCancelRejectRoundTrip) {
    ExecReportMessage e{};
    e.order_id = 77; e.client_order_id = 5; e.trade_id = 3;
    e.last_px = 1500000; e.last_qty = 4; e.leaves_qty = 6; e.cum_qty = 4;
    e.exec_type = static_cast<uint8_t>(ExecType::PARTIAL_FILL);
    e.reject_reason = static_cast<uint8_t>(RejectReason::NONE);
    seal_message(e, MessageType::EXEC_REPORT, 1, 1);
    auto re = round_trip(e);
    EXPECT_EQ(val(re.order_id), 77u);
    EXPECT_EQ(val(re.leaves_qty) + val(re.cum_qty), 10u);
    EXPECT_EQ(static_cast<ExecType>(re.exec_type), ExecType::PARTIAL_FILL);
    EXPECT_TRUE(checksum_ok(re));

    CancelRejectMessage r{};
    r.order_id = 1; r.client_order_id = 2;
    r.reject_reason = static_cast<uint8_t>(RejectReason::NOT_OWNER);
    r.response_to = static_cast<uint8_t>(CancelRejectResponseTo::MODIFY);
    seal_message(r, MessageType::CANCEL_REJECT, 1, 1);
    auto rr = round_trip(r);
    EXPECT_EQ(static_cast<RejectReason>(rr.reject_reason), RejectReason::NOT_OWNER);
    EXPECT_EQ(static_cast<CancelRejectResponseTo>(rr.response_to), CancelRejectResponseTo::MODIFY);
    EXPECT_TRUE(checksum_ok(rr));
}

TEST(Protocol, ChecksumDetectsBodyChange) {
    NewOrderMessage m{};
    m.quantity = 100;
    seal_message(m, MessageType::NEW_ORDER, 1, 1);
    EXPECT_TRUE(checksum_ok(m));
    m.quantity = 101;
    EXPECT_FALSE(checksum_ok(m));
}

TEST(Protocol, InboundFrameChecks) {
    auto check = [](const void* p, size_t n) {
        return check_inbound_frame(static_cast<const uint8_t*>(p), n);
    };
    NewOrderMessage m{};
    m.quantity = 1;
    seal_message(m, MessageType::NEW_ORDER, 1, 1);

    // Too short to judge: wait for more.
    EXPECT_EQ(check(&m, sizeof(MessageHeader) - 1).status, FrameCheck::NEED_MORE);
    // Header complete, body not: still waiting, type and length already fine.
    EXPECT_EQ(check(&m, sizeof(MessageHeader)).status, FrameCheck::NEED_MORE);
    auto ok = check(&m, sizeof(m));
    EXPECT_EQ(ok.status, FrameCheck::OK);
    EXPECT_EQ(ok.length, sizeof(m));

    MessageHeader h{};
    h.length = sizeof(MessageHeader);
    for (uint32_t t : {3u /*retired ORDER_ACK*/, 99u, static_cast<uint32_t>(MessageType::EXEC_REPORT)}) {
        h.type = t;
        auto r = check(&h, sizeof(h));
        EXPECT_EQ(r.status, FrameCheck::INVALID) << t;
        EXPECT_EQ(r.reason, SessionRejectReason::UNKNOWN_TYPE) << t;
    }

    NewOrderMessage bad_len = m;
    bad_len.header.length = 40;   // rejected as soon as the header is in
    EXPECT_EQ(check(&bad_len, sizeof(MessageHeader)).reason, SessionRejectReason::BAD_LENGTH);
    bad_len.header.length = 1000000;
    EXPECT_EQ(check(&bad_len, sizeof(MessageHeader)).reason, SessionRejectReason::BAD_LENGTH);

    NewOrderMessage bad_sum = m;
    bad_sum.header.checksum = 0xdeadbeef;
    EXPECT_EQ(check(&bad_sum, sizeof(bad_sum)).reason, SessionRejectReason::BAD_CHECKSUM);
}

// FlatMap backs the gateway's per-session order table: compare it with
// std::unordered_map under random insert/erase/find.
TEST(FlatMap, MatchesUnorderedMapUnderRandomOps) {
    for (uint32_t seed = 1; seed <= 20; ++seed) {
        std::mt19937_64 rng(seed);
        FlatMap<uint64_t> fm(512);
        std::unordered_map<uint64_t, uint64_t> ref;
        std::uniform_int_distribution<uint64_t> key(0, 2000);   // dense keys -> many collisions
        for (int i = 0; i < 20000; ++i) {
            uint64_t k = key(rng);
            switch (rng() % 3) {
                case 0: {
                    bool expect = ref.size() < fm.max_size() && !ref.count(k);
                    ASSERT_EQ(fm.insert(k, k * 3), expect) << "seed " << seed;
                    if (expect) ref[k] = k * 3;
                    break;
                }
                case 1:
                    ASSERT_EQ(fm.erase(k), ref.erase(k) == 1) << "seed " << seed;
                    break;
                default: {
                    auto* v = fm.find(k);
                    auto it = ref.find(k);
                    ASSERT_EQ(v != nullptr, it != ref.end()) << "seed " << seed;
                    if (v) { ASSERT_EQ(*v, it->second); }
                }
            }
            ASSERT_EQ(fm.size(), ref.size());
        }
        // Every remaining key is still reachable after all the backward shifts.
        for (auto& [k, v] : ref) {
            auto* got = fm.find(k);
            ASSERT_NE(got, nullptr);
            EXPECT_EQ(*got, v);
        }
    }
}

TEST(FlatMap, RejectsWhenFull) {
    FlatMap<int> fm(4);
    size_t n = 0;
    while (fm.insert(n, 1)) ++n;
    EXPECT_EQ(n, fm.max_size());
    EXPECT_TRUE(fm.erase(0));
    EXPECT_TRUE(fm.insert(1000, 1));
}
