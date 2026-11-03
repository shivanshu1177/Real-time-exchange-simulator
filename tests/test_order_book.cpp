// OrderBook unit tests: priority, partial fills, level and BBO quantities,
// self-trade prevention, market orders, O(1) cancel and pool accounting.
#include <gtest/gtest.h>
#include "rtes/order_book.hpp"
#include "reference_book.hpp"

using namespace rtes;
using rtes::test::BookEvent;
using rtes::test::RecordingListener;

namespace {

class OrderBookTest : public ::testing::Test {
protected:
    static constexpr size_t kPool = 1000;
    OrderPool pool{kPool};
    RecordingListener events;
    std::unique_ptr<OrderBook> book;
    OrderID next_id = 1;

    void SetUp() override { make(StpMode::CANCEL_TAKER); }
    void make(StpMode stp) {
        if (book) book->clear();
        book = std::make_unique<OrderBook>("TEST", pool, kPool, &events, stp);
        events.events.clear();
    }

    OrderID add(ClientID client, Side side, Price price, Quantity qty,
                OrderType type = OrderType::LIMIT) {
        Order* o = pool.acquire();
        EXPECT_NE(o, nullptr);
        *o = Order{};
        o->order_id = next_id++; o->client_id = client; o->side = side;
        o->type = type; o->price = price; o->quantity = qty;
        EXPECT_TRUE(book->add_order(o));
        return o->order_id;
    }
    OrderID buy(ClientID c, Price p, Quantity q)  { return add(c, Side::BUY, p, q); }
    OrderID sell(ClientID c, Price p, Quantity q) { return add(c, Side::SELL, p, q); }

    std::vector<BookEvent> take() { auto out = events.events; events.events.clear(); return out; }
    static BookEvent trade(OrderID a, OrderID b, Quantity q, Price p) {
        return {BookEvent::TRADE, a, b, q, p, CancelCause::NO_LIQUIDITY};
    }
    static BookEvent cancelled(OrderID id, CancelCause c) {
        return {BookEvent::CANCEL, id, 0, 0, 0, c};
    }
};

} // namespace

TEST_F(OrderBookTest, RestingOrderSetsBboAndQuantity) {
    buy(1, 100, 10);
    buy(2, 100, 5);
    sell(3, 102, 7);
    EXPECT_EQ(book->order_count(), 3u);
    EXPECT_EQ(book->best_bid(), 100u);
    EXPECT_EQ(book->best_bid_quantity(), 15u);
    EXPECT_EQ(book->best_ask(), 102u);
    EXPECT_EQ(book->best_ask_quantity(), 7u);
    EXPECT_TRUE(take().empty());
}

TEST_F(OrderBookTest, PriceThenTimePriority) {
    OrderID b1 = buy(1, 100, 10);
    OrderID b2 = buy(2, 100, 10);
    OrderID b3 = buy(3, 101, 10);   // better price, arrived last
    OrderID s  = sell(4, 100, 25);
    EXPECT_EQ(take(), (std::vector<BookEvent>{trade(s, b3, 10, 101), trade(s, b1, 10, 100),
                                              trade(s, b2, 5, 100)}));
    EXPECT_EQ(book->level_quantity(Side::BUY, 100), 5u);
    EXPECT_EQ(book->find(b2)->remaining(), 5u);
}

TEST_F(OrderBookTest, PartialFillsAcrossLevelsAtPassivePrices) {
    OrderID a1 = sell(1, 101, 10);
    OrderID a2 = sell(1, 102, 10);
    OrderID a3 = sell(1, 103, 10);
    OrderID b  = buy(2, 103, 25);
    EXPECT_EQ(take(), (std::vector<BookEvent>{trade(b, a1, 10, 101), trade(b, a2, 10, 102),
                                              trade(b, a3, 5, 103)}));
    EXPECT_EQ(book->level_count(Side::SELL), 1u);
    EXPECT_EQ(book->level_quantity(Side::SELL, 103), 5u);
    EXPECT_EQ(book->find(b), nullptr);     // fully filled, not resting
}

TEST_F(OrderBookTest, LevelQuantityStaysCorrectThroughEveryOperation) {
    OrderID a = buy(1, 100, 10);
    OrderID b = buy(2, 100, 20);
    EXPECT_EQ(book->level_quantity(Side::BUY, 100), 30u);
    sell(3, 100, 4);                       // partial fill of a
    EXPECT_EQ(book->level_quantity(Side::BUY, 100), 26u);
    book->modify_order(b, 100, 15);        // quantity down
    EXPECT_EQ(book->level_quantity(Side::BUY, 100), 21u);
    book->cancel_order(a);                 // a had 6 left
    EXPECT_EQ(book->level_quantity(Side::BUY, 100), 15u);
    book->modify_order(b, 99, 15);         // move level
    EXPECT_EQ(book->level_quantity(Side::BUY, 100), 0u);
    EXPECT_EQ(book->level_quantity(Side::BUY, 99), 15u);
    EXPECT_EQ(book->level_count(Side::BUY), 1u);
}

// Regression: an order that partially filled before resting used to add its
// remaining quantity but subtract its original quantity when it completed.
TEST_F(OrderBookTest, PartiallyFilledAggressorRestsWithItsRemainder) {
    sell(1, 100, 4);
    OrderID b = buy(2, 100, 10);           // fills 4, rests 6
    EXPECT_EQ(book->level_quantity(Side::BUY, 100), 6u);
    EXPECT_EQ(book->best_bid_quantity(), 6u);
    sell(3, 100, 6);
    EXPECT_EQ(book->level_count(Side::BUY), 0u);
    EXPECT_EQ(book->find(b), nullptr);
    EXPECT_EQ(book->best_bid_quantity(), 0u);
}

TEST_F(OrderBookTest, StpCancelTakerKeepsTheRestingOrder) {
    OrderID s = sell(1, 100, 10);
    OrderID b = buy(1, 100, 5);
    EXPECT_EQ(take(), std::vector<BookEvent>{cancelled(b, CancelCause::SELF_TRADE)});
    EXPECT_EQ(book->find(s)->remaining(), 10u);
    EXPECT_EQ(book->level_count(Side::BUY), 0u);
}

TEST_F(OrderBookTest, StpCancelTakerAfterPartialFillFromOthers) {
    OrderID s2 = sell(2, 100, 3);          // another client first in queue
    OrderID s1 = sell(1, 100, 10);         // then our own
    OrderID b  = buy(1, 100, 8);
    EXPECT_EQ(take(), (std::vector<BookEvent>{trade(b, s2, 3, 100), cancelled(b, CancelCause::SELF_TRADE)}));
    EXPECT_EQ(book->find(s1)->remaining(), 10u);
}

TEST_F(OrderBookTest, StpCancelMakerRemovesOwnOrderAndKeepsMatching) {
    make(StpMode::CANCEL_MAKER);
    OrderID s1 = sell(1, 100, 10);         // own order, first in queue
    OrderID s2 = sell(2, 100, 10);
    OrderID b  = buy(1, 100, 15);
    EXPECT_EQ(take(), (std::vector<BookEvent>{cancelled(s1, CancelCause::SELF_TRADE), trade(b, s2, 10, 100)}));
    EXPECT_EQ(book->find(b)->remaining(), 5u);   // rest of b rests
    EXPECT_EQ(book->level_count(Side::SELL), 0u);
}

TEST_F(OrderBookTest, StpCancelBothCancelsBoth) {
    make(StpMode::CANCEL_BOTH);
    OrderID s = sell(1, 100, 10);
    OrderID b = buy(1, 100, 5);
    EXPECT_EQ(take(), (std::vector<BookEvent>{cancelled(s, CancelCause::SELF_TRADE),
                                              cancelled(b, CancelCause::SELF_TRADE)}));
    EXPECT_EQ(book->order_count(), 0u);
}

TEST_F(OrderBookTest, MarketOrderOnEmptyBookIsCancelledAndSlotReturned) {
    const size_t before = pool.available();
    OrderID m = add(1, Side::BUY, 0, 10, OrderType::MARKET);
    EXPECT_EQ(take(), std::vector<BookEvent>{cancelled(m, CancelCause::NO_LIQUIDITY)});
    EXPECT_EQ(book->order_count(), 0u);
    EXPECT_EQ(pool.available(), before);
}

TEST_F(OrderBookTest, MarketOrderRemainderIsCancelled) {
    OrderID s = sell(1, 100, 5);
    OrderID m = add(2, Side::BUY, 0, 8, OrderType::MARKET);
    EXPECT_EQ(take(), (std::vector<BookEvent>{trade(m, s, 5, 100), cancelled(m, CancelCause::NO_LIQUIDITY)}));
    EXPECT_EQ(book->order_count(), 0u);
}

TEST_F(OrderBookTest, CancelFromTheMiddleOfALevelKeepsQueueOrder) {
    OrderID a = buy(1, 100, 1);
    OrderID b = buy(2, 100, 2);
    OrderID c = buy(3, 100, 3);
    EXPECT_TRUE(book->cancel_order(b));
    EXPECT_FALSE(book->cancel_order(b));
    EXPECT_EQ(book->level_quantity(Side::BUY, 100), 4u);
    OrderID s = sell(4, 100, 4);
    EXPECT_EQ(take(), (std::vector<BookEvent>{trade(s, a, 1, 100), trade(s, c, 3, 100)}));
    // Cancel head and tail of a fresh level, then the only one left.
    OrderID d = sell(5, 105, 1), e = sell(6, 105, 1), f = sell(7, 105, 1);
    EXPECT_TRUE(book->cancel_order(d));
    EXPECT_TRUE(book->cancel_order(f));
    EXPECT_EQ(book->level_quantity(Side::SELL, 105), 1u);
    EXPECT_TRUE(book->cancel_order(e));
    EXPECT_EQ(book->level_count(Side::SELL), 0u);
}

TEST_F(OrderBookTest, PoolSlotsAllComeBack) {
    const size_t initial = pool.available();
    for (int i = 0; i < 50; ++i) buy(static_cast<ClientID>(i + 1), 100 - i % 5, 10);
    for (int i = 0; i < 20; ++i) sell(static_cast<ClientID>(100 + i), 98, 7);
    add(500, Side::SELL, 0, 1000, OrderType::MARKET);
    book->clear();
    EXPECT_EQ(pool.available(), initial);
    EXPECT_EQ(pool.capacity(), kPool);
}
