// RiskManager and TokenBucket unit tests.
#include <gtest/gtest.h>
#include "rtes/risk_manager.hpp"

using namespace rtes;

namespace {

constexpr Price px(double dollars) { return static_cast<Price>(dollars * 10000 + 0.5); }
constexpr uint64_t kSec = 1'000'000'000ULL;

struct RiskTest : ::testing::Test {
    RiskConfig cfg{/*max_order_size*/ 1000, /*max_notional*/ 10000.0, /*rate*/ 1000, /*collar*/ true};
    std::vector<SymbolConfig> symbols{{"AAPL", 0.01, 1, 10.0, StpMode::CANCEL_TAKER, 150.0},
                                      {"NOREF", 0.01, 1, 10.0, StpMode::CANCEL_TAKER, 0.0}};

    RejectReason add(RiskManager& r, ClientID c, Price p, Quantity q, uint32_t sym = 0,
                     OrderType t = OrderType::LIMIT) {
        Price basis = 0;
        return r.check_new_order(c, sym, t, p, q, basis);
    }
};

} // namespace

TEST_F(RiskTest, CollarStartsFromConfiguredReferenceAndFollowsLastTrade) {
    RiskManager r(cfg, symbols);
    EXPECT_EQ(add(r, 1, px(170), 1), RejectReason::PRICE_COLLAR);    // 135..165 around 150
    EXPECT_EQ(add(r, 1, px(160), 1), RejectReason::NONE);
    r.on_trade(0, px(170));                                          // now 153..187
    EXPECT_EQ(r.reference_price(0), px(170));
    EXPECT_EQ(add(r, 1, px(180), 1), RejectReason::NONE);
    EXPECT_EQ(add(r, 1, px(140), 1), RejectReason::PRICE_COLLAR);
}

TEST_F(RiskTest, NoReferencePriceMeansNoCollarUntilTheFirstTrade) {
    RiskManager r(cfg, symbols);
    EXPECT_EQ(add(r, 1, px(5), 1, 1), RejectReason::NONE);
    r.on_trade(1, px(100));
    EXPECT_EQ(add(r, 1, px(5), 1, 1), RejectReason::PRICE_COLLAR);
}

TEST_F(RiskTest, NotionalIsReservedAndReleasedOnCancelAndFill) {
    RiskManager r(cfg, symbols);                                  // max $10,000
    EXPECT_EQ(add(r, 1, px(150), 50), RejectReason::NONE);        // $7,500 open
    EXPECT_EQ(r.open_notional(1), px(150) * 50);
    EXPECT_EQ(add(r, 1, px(150), 20), RejectReason::NOTIONAL_LIMIT);  // would be $10,500
    EXPECT_EQ(add(r, 2, px(150), 20), RejectReason::NONE);        // other client unaffected

    r.release(1, px(150) * 30, /*order_done=*/false);             // partial fill of 30
    EXPECT_EQ(r.open_notional(1), px(150) * 20);
    EXPECT_EQ(add(r, 1, px(150), 20), RejectReason::NONE);        // fits again ($6,000 open)

    r.release(1, px(150) * 20, true);                             // first order done
    r.release(1, px(150) * 20, true);                             // second cancelled
    EXPECT_EQ(r.open_notional(1), 0u);
    EXPECT_EQ(r.open_orders(1), 0u);
}

TEST_F(RiskTest, OpenOrderCountLimit) {
    cfg.max_open_orders_per_client = 2;
    RiskManager r(cfg, symbols);
    EXPECT_EQ(add(r, 1, px(150), 1), RejectReason::NONE);
    EXPECT_EQ(add(r, 1, px(150), 1), RejectReason::NONE);
    EXPECT_EQ(add(r, 1, px(150), 1), RejectReason::OPEN_ORDER_LIMIT);
    r.release(1, px(150), true);
    EXPECT_EQ(add(r, 1, px(150), 1), RejectReason::NONE);
}

TEST_F(RiskTest, ClientStateIsForgottenWhenNothingIsOpen) {
    RiskManager r(cfg, symbols);
    for (ClientID c = 1; c <= 100; ++c) ASSERT_EQ(add(r, c, px(150), 1), RejectReason::NONE);
    EXPECT_EQ(r.tracked_clients(), 100u);
    for (ClientID c = 1; c <= 100; ++c) r.release(c, px(150), true);
    EXPECT_EQ(r.tracked_clients(), 0u);   // bounded over long runs
}

TEST_F(RiskTest, ModifyChecksTheNotionalIncreaseOnly) {
    RiskManager r(cfg, symbols);
    ASSERT_EQ(add(r, 1, px(150), 60), RejectReason::NONE);        // $9,000 open
    const uint64_t now = px(150) * 60;
    EXPECT_EQ(r.check_modify(1, 0, px(150), 50, now, px(150) * 50), RejectReason::NONE);       // decrease
    EXPECT_EQ(r.check_modify(1, 0, px(150), 70, now, px(150) * 70), RejectReason::NOTIONAL_LIMIT);
    r.adjust(1, now, px(150) * 50);
    EXPECT_EQ(r.open_notional(1), px(150) * 50);
}

TEST_F(RiskTest, TickLotAndSize) {
    symbols[0].lot_size = 10;
    RiskManager r(cfg, symbols);
    EXPECT_EQ(add(r, 1, px(150) + 1, 10), RejectReason::OFF_TICK);
    EXPECT_EQ(add(r, 1, px(150), 15), RejectReason::OFF_LOT);
    EXPECT_EQ(add(r, 1, px(150), 2000), RejectReason::ORDER_SIZE_LIMIT);
    EXPECT_EQ(add(r, 1, px(150), 0), RejectReason::INVALID_QUANTITY);
}

TEST_F(RiskTest, MarketOrdersAreValuedAtTheReferencePrice) {
    RiskManager r(cfg, symbols);
    Price basis = 0;
    EXPECT_EQ(r.check_new_order(1, 0, OrderType::MARKET, 0, 60, basis), RejectReason::NONE);
    EXPECT_EQ(basis, px(150));
    EXPECT_EQ(r.open_notional(1), px(150) * 60);
}

TEST_F(RiskTest, RejectLogIsOneLinePerSecond) {
    RiskManager r(cfg, symbols);
    for (int i = 0; i < 1500; ++i) r.record_reject(RejectReason::RATE_LIMIT);
    for (int i = 0; i < 10; ++i) r.record_reject(RejectReason::PRICE_COLLAR);
    size_t lines = 0;
    // A burst inside one second, flushed on every loop iteration.
    for (uint64_t t = 1; t <= 1000; ++t) {
        r.record_reject(RejectReason::RATE_LIMIT);
        lines += r.flush_reject_log(5 * kSec + t * 1'000'000ULL);   // 1 ms apart
    }
    EXPECT_EQ(lines, 1u);
    lines += r.flush_reject_log(7 * kSec);                           // next window
    EXPECT_EQ(lines, 2u);
    EXPECT_EQ(r.flush_reject_log(9 * kSec), 0u);                     // nothing new
    EXPECT_EQ(r.reject_count(RejectReason::RATE_LIMIT), 2500u);
    EXPECT_EQ(r.reject_count(RejectReason::PRICE_COLLAR), 10u);
}

TEST(TokenBucket, BurstThenRefillAtRate) {
    TokenBucket b(/*rate*/ 3, /*burst*/ 3);
    uint64_t t = 10 * kSec;
    EXPECT_TRUE(b.try_take(t));
    EXPECT_TRUE(b.try_take(t));
    EXPECT_TRUE(b.try_take(t));
    EXPECT_FALSE(b.try_take(t));
    EXPECT_FALSE(b.try_take(t + kSec / 4));        // 0.75 tokens
    EXPECT_TRUE(b.try_take(t + kSec / 3 + 1000));  // 1 token after 1/3 s
    EXPECT_FALSE(b.try_take(t + kSec / 3 + 2000));
    EXPECT_TRUE(b.try_take(t + 100 * kSec));       // long idle: capped at burst
    EXPECT_TRUE(b.try_take(t + 100 * kSec));
    EXPECT_TRUE(b.try_take(t + 100 * kSec));
    EXPECT_FALSE(b.try_take(t + 100 * kSec));
}
