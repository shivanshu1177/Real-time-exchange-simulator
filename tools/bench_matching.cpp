#include "rtes/order_book.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <random>

using namespace rtes;

int main() {
    const size_t WARMUP = 1000;
    const size_t BENCH  = 99'000;
    const size_t TOTAL  = WARMUP + BENCH;

    OrderPool pool(TOTAL);

    size_t trade_count = 0;
    OrderBook book("AAPL", pool, [&trade_count](const Trade&, const Order&, const Order&) { ++trade_count; });

    std::mt19937 gen(42);
    std::uniform_int_distribution<Price>    price_dist(14900, 15100);
    std::uniform_int_distribution<Quantity> qty_dist(1, 100);
    std::uniform_int_distribution<int>      side_dist(0, 1);

    // Warm up: populate price levels and fill i-cache before timing
    for (size_t i = 0; i < WARMUP; ++i) {
        auto* o = pool.acquire();
        if (!o) break;
        *o = Order{.order_id  = i + 1,
                   .client_id = 0,
                   .symbol    = "AAPL",
                   .side      = static_cast<Side>(side_dist(gen)),
                   .type      = OrderType::LIMIT,
                   .price     = price_dist(gen),
                   .quantity  = qty_dist(gen)};
        book.add_order(o);
    }
    trade_count = 0;

    auto t0 = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < BENCH; ++i) {
        auto* o = pool.acquire();
        if (!o) break;
        *o = Order{.order_id  = WARMUP + i + 1,
                   .client_id = 0,
                   .symbol    = "AAPL",
                   .side      = static_cast<Side>(side_dist(gen)),
                   .type      = OrderType::LIMIT,
                   .price     = price_dist(gen),
                   .quantity  = qty_dist(gen)};
        book.add_order(o);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    auto total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

    double   ns_per_order  = static_cast<double>(total_ns) / BENCH;
    uint64_t orders_per_sec = static_cast<uint64_t>(1e9 / ns_per_order);

    std::cout << "\n=== RTES Matching Engine Benchmark ===\n"
              << "  Scope         : in-process hot path (no TCP / network overhead)\n"
              << "  Critical path : pool.acquire → book.add_order → match → callback → pool.release\n"
              << "\n"
              << "  Orders timed  : " << BENCH << "\n"
              << "  Trades fired  : " << trade_count << "\n"
              << "  Total time    : " << total_ns / 1000 << " μs\n"
              << "  Avg latency   : " << std::fixed << std::setprecision(1)
                                      << ns_per_order << " ns/order\n"
              << "  Throughput    : " << orders_per_sec << " orders/sec\n\n";

    bool pass_tput = orders_per_sec >= 100'000;
    bool pass_lat  = ns_per_order   <= 10'000.0;
    std::cout << "  [" << (pass_tput ? "PASS" : "FAIL")
              << "] 100K+ orders/sec  (got " << orders_per_sec << ")\n"
              << "  [" << (pass_lat  ? "PASS" : "FAIL")
              << "] sub-10μs latency  (got " << std::fixed << std::setprecision(1)
              << ns_per_order << " ns)\n\n";

    return (pass_tput && pass_lat) ? 0 : 1;
}
