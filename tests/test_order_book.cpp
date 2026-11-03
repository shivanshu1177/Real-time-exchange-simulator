#include <gtest/gtest.h>
#include "rtes/order_book.hpp"
#include "rtes/memory_pool.hpp"

using namespace rtes;

class OrderBookTest : public ::testing::Test {
protected:
    void SetUp() override {
        pool = std::make_unique<OrderPool>(100);
        book = std::make_unique<OrderBook>("TEST", *pool);
    }
    
    std::unique_ptr<OrderPool> pool;
    std::unique_ptr<OrderBook> book;
};

TEST_F(OrderBookTest, AddOrder) {
    auto* order = pool->acquire();
    *order = Order{
        .order_id = 1,
        .symbol = "TEST",
        .side = Side::BUY,
        .type = OrderType::LIMIT,
        .price = 100,
        .quantity = 10
    };
    
    EXPECT_TRUE(book->add_order(order));
    EXPECT_EQ(book->order_count(), 1);
    EXPECT_EQ(book->best_bid(), 100);
}

TEST_F(OrderBookTest, CancelOrder) {
    auto* order = pool->acquire();
    *order = Order{
        .order_id = 1,
        .symbol = "TEST",
        .side = Side::BUY,
        .type = OrderType::LIMIT,
        .price = 100,
        .quantity = 10
    };
    
    book->add_order(order);
    EXPECT_EQ(book->order_count(), 1);
    
    EXPECT_TRUE(book->cancel_order(1));
    EXPECT_EQ(book->order_count(), 0);
}

TEST_F(OrderBookTest, MatchOrders) {
    size_t trade_count = 0;
    auto trade_callback = [&trade_count](const Trade&, const Order&, const Order&) { trade_count++; };
    
    OrderBook matching_book("TEST", *pool, trade_callback);
    
    auto* buy_order = pool->acquire();
    *buy_order = Order{
        .order_id = 1,
        .symbol = "TEST",
        .side = Side::BUY,
        .type = OrderType::LIMIT,
        .price = 100,
        .quantity = 10
    };
    
    auto* sell_order = pool->acquire();
    *sell_order = Order{
        .order_id = 2,
        .symbol = "TEST",
        .side = Side::SELL,
        .type = OrderType::LIMIT,
        .price = 100,
        .quantity = 5
    };
    
    matching_book.add_order(buy_order);
    matching_book.add_order(sell_order);
    
    EXPECT_EQ(trade_count, 1);
    EXPECT_EQ(matching_book.order_count(), 1); // Partial fill leaves buy order
}