// bench_book: per-operation latency of the order book, in process, one thread.
//
// What it measures: OrderBook::add_order / cancel_order / modify_order and a
// one-fill aggressive order, on a book that stays at a fixed depth. Each
// operation is timed on its own with steady_clock and recorded in a histogram,
// so it reports percentiles, not a mean. It also reports:
//   - the timer's own overhead and resolution (on Apple Silicon steady_clock
//     ticks about every 41.7 ns, which limits per-operation precision), and
//   - a batched figure per operation type (total time / operations over runs
//     of 1,000), which is not limited by timer resolution.
//
// What it does NOT measure: the network, the gateway, risk, queues between
// threads, or anything wire-to-wire. Those come with the end-to-end benchmark.
//
// Usage: bench_book [--ops N] [--depth D] [--seed S] [--json PATH]

#include "rtes/order_book.hpp"
#include "rtes/util/histogram.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace rtes;
using Clock = std::chrono::steady_clock;

namespace {

inline uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

struct NullListener : BookListener {
    uint64_t trades = 0;
    void on_trade(const Order&, const Order&, const Trade&) override { ++trades; }
    void on_book_cancel(const Order&, CancelCause) override {}
};

struct Result {
    const char*     name;
    util::Histogram hist;
    double          batched_ns{0};   // mean ns/op over batches of kBatch operations
};

constexpr size_t   kBatch     = 1000;
constexpr Quantity kBigQty    = 1'000'000'000;   // resting orders never fill completely
constexpr Price    kMid       = 100000;          // ticks; bids below, asks above
constexpr Price    kLevels    = 100;             // price levels per side

} // namespace

int main(int argc, char** argv) {
    size_t   ops   = 1'000'000;
    size_t   depth = 10'000;
    uint64_t seed  = 42;
    std::string json_path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--ops") ops = std::strtoull(next(), nullptr, 10);
        else if (a == "--depth") depth = std::strtoull(next(), nullptr, 10);
        else if (a == "--seed") seed = std::strtoull(next(), nullptr, 10);
        else if (a == "--json") json_path = next();
        else { std::fprintf(stderr, "usage: %s [--ops N] [--depth D] [--seed S] [--json PATH]\n", argv[0]); return 2; }
    }
    if (depth < 10) depth = 10;

    const size_t capacity = depth + kBatch + 16;   // the batched add phase adds kBatch more
    OrderPool pool(capacity);
    NullListener listener;
    OrderBook book("BENCH", pool, capacity, &listener, StpMode::CANCEL_TAKER);
    std::mt19937_64 rng(seed);
    auto uni = [&](uint64_t lo, uint64_t hi) { return std::uniform_int_distribution<uint64_t>(lo, hi)(rng); };

    OrderID next_id = 1;
    std::vector<OrderID> live;       // resting order ids (all bids)
    live.reserve(capacity);

    auto make = [&](Side side, OrderType type, Price price, Quantity qty) {
        Order* o = pool.acquire();
        *o = Order{};
        o->order_id  = next_id;
        o->client_id = next_id;      // distinct clients: self-trade prevention never applies
        ++next_id;
        o->side = side; o->type = type; o->price = price; o->quantity = qty;
        return o;
    };
    auto random_bid_price = [&] { return kMid - uni(1, kLevels); };

    // Pre-populate: `depth` resting bids over kLevels levels.
    for (size_t i = 0; i < depth; ++i) {
        Order* o = make(Side::BUY, OrderType::LIMIT, random_bid_price(), kBigQty);
        live.push_back(o->order_id);
        book.add_order(o);
    }

    Result add{"add (passive)", {}}, cancel{"cancel", {}}, match{"match (1 fill)", {}}, modify{"modify (qty down)", {}};

    // One round = add, cancel, aggressive 1-lot sell, modify. Depth stays constant.
    auto round = [&](bool record) {
        // add
        Order* o = make(Side::BUY, OrderType::LIMIT, random_bid_price(), kBigQty);
        const OrderID added = o->order_id;
        uint64_t t0 = now_ns();
        book.add_order(o);
        uint64_t t1 = now_ns();
        if (record) add.hist.record(t1 - t0);
        live.push_back(added);

        // cancel a random resting order
        size_t k = uni(0, live.size() - 1);
        const OrderID victim = live[k];
        live[k] = live.back();
        live.pop_back();
        t0 = now_ns();
        book.cancel_order(victim);
        t1 = now_ns();
        if (record) cancel.hist.record(t1 - t0);

        // aggressive sell for 1 lot at the best bid: exactly one fill
        Order* s = make(Side::SELL, OrderType::LIMIT, book.best_bid(), 1);
        t0 = now_ns();
        book.add_order(s);
        t1 = now_ns();
        if (record) match.hist.record(t1 - t0);

        // modify a random resting order down by one lot (keeps priority)
        const OrderID m = live[uni(0, live.size() - 1)];
        const Order* mo = book.find(m);
        t0 = now_ns();
        book.modify_order(m, mo->price, mo->quantity - 1);
        t1 = now_ns();
        if (record) modify.hist.record(t1 - t0);
    };

    // Warm up caches and branch predictors.
    for (size_t i = 0; i < ops / 10 + 1; ++i) round(false);
    for (size_t i = 0; i < ops; ++i) round(true);

    // Batched figures: time kBatch rounds at once, per operation type, by
    // running the same operation kBatch times between two clock reads.
    auto batched = [&](Result& r, auto&& op) {
        double total = 0; size_t n = 0;
        for (size_t b = 0; b < std::max<size_t>(ops / kBatch / 10, 10); ++b) {
            uint64_t t0 = now_ns();
            for (size_t i = 0; i < kBatch; ++i) op();
            total += static_cast<double>(now_ns() - t0);
            n += kBatch;
        }
        r.batched_ns = total / static_cast<double>(n);
    };
    std::vector<OrderID> batch_ids(kBatch);
    // add then cancel the same kBatch orders, each timed as one batch
    for (size_t rep = 0; rep < 2; ++rep) {
        double add_total = 0, cancel_total = 0; size_t n = 0;
        for (size_t b = 0; b < std::max<size_t>(ops / kBatch / 10, 10); ++b) {
            std::vector<Order*> pending(kBatch);
            for (size_t i = 0; i < kBatch; ++i) pending[i] = make(Side::BUY, OrderType::LIMIT, random_bid_price(), kBigQty);
            uint64_t t0 = now_ns();
            for (size_t i = 0; i < kBatch; ++i) { batch_ids[i] = pending[i]->order_id; book.add_order(pending[i]); }
            uint64_t t1 = now_ns();
            for (size_t i = 0; i < kBatch; ++i) book.cancel_order(batch_ids[i]);
            uint64_t t2 = now_ns();
            add_total += static_cast<double>(t1 - t0);
            cancel_total += static_cast<double>(t2 - t1);
            n += kBatch;
        }
        add.batched_ns    = add_total / static_cast<double>(n);
        cancel.batched_ns = cancel_total / static_cast<double>(n);
    }
    batched(match, [&] {
        Order* s = make(Side::SELL, OrderType::LIMIT, book.best_bid(), 1);
        book.add_order(s);
    });
    batched(modify, [&] {
        const OrderID m = live[uni(0, live.size() - 1)];
        const Order* mo = book.find(m);
        book.modify_order(m, mo->price, mo->quantity - 1);
    });

    // Timer overhead and resolution: two back-to-back clock reads.
    util::Histogram timer;
    uint64_t resolution = UINT64_MAX;
    for (size_t i = 0; i < 1'000'000; ++i) {
        uint64_t a = now_ns(), b = now_ns();
        timer.record(b - a);
        if (b > a && b - a < resolution) resolution = b - a;
    }

    Result* results[] = {&add, &cancel, &match, &modify};
    std::printf("\nbench_book: order book only, single thread, in process (no network, no gateway, no queues)\n");
    std::printf("ops per type: %zu   resting depth: %zu orders over %llu levels   trades: %llu\n\n",
                ops, depth, static_cast<unsigned long long>(kLevels),
                static_cast<unsigned long long>(listener.trades));
    std::printf("%-19s %8s %8s %8s %8s %8s %10s %12s\n", "operation (ns)", "p50", "p90", "p99", "p99.9", "p99.99", "max", "batched/op");
    for (Result* r : results) {
        const auto& h = r->hist;
        std::printf("%-19s %8llu %8llu %8llu %8llu %8llu %10llu %12.1f\n", r->name,
                    static_cast<unsigned long long>(h.percentile(50)), static_cast<unsigned long long>(h.percentile(90)),
                    static_cast<unsigned long long>(h.percentile(99)), static_cast<unsigned long long>(h.percentile(99.9)),
                    static_cast<unsigned long long>(h.percentile(99.99)), static_cast<unsigned long long>(h.max()),
                    r->batched_ns);
    }
    std::printf("\ntimer: overhead p50 %llu ns, smallest non-zero step %llu ns.\n",
                static_cast<unsigned long long>(timer.percentile(50)),
                static_cast<unsigned long long>(resolution == UINT64_MAX ? 0 : resolution));
    std::printf("Per-operation percentiles include one timer read and are quantised to that step.\n"
                "'batched/op' = total time / operations over batches of %zu, not limited by the timer.\n\n", kBatch);

    if (!json_path.empty()) {
        FILE* f = std::fopen(json_path.c_str(), "w");
        if (!f) { std::perror("open json"); return 1; }
        std::fprintf(f, "{\n  \"benchmark\": \"bench_book\",\n  \"scope\": \"order book only, single thread, in process\",\n");
        std::fprintf(f, "  \"ops_per_type\": %zu,\n  \"resting_depth\": %zu,\n  \"price_levels\": %llu,\n  \"seed\": %llu,\n",
                     ops, depth, static_cast<unsigned long long>(kLevels), static_cast<unsigned long long>(seed));
        std::fprintf(f, "  \"timer\": {\"overhead_p50_ns\": %llu, \"resolution_ns\": %llu},\n",
                     static_cast<unsigned long long>(timer.percentile(50)),
                     static_cast<unsigned long long>(resolution == UINT64_MAX ? 0 : resolution));
        std::fprintf(f, "  \"results_ns\": {\n");
        for (size_t i = 0; i < 4; ++i) {
            const auto& h = results[i]->hist;
            std::fprintf(f, "    \"%s\": {\"p50\": %llu, \"p90\": %llu, \"p99\": %llu, \"p99_9\": %llu, \"p99_99\": %llu, \"max\": %llu, \"batched_per_op\": %.1f}%s\n",
                         results[i]->name,
                         static_cast<unsigned long long>(h.percentile(50)), static_cast<unsigned long long>(h.percentile(90)),
                         static_cast<unsigned long long>(h.percentile(99)), static_cast<unsigned long long>(h.percentile(99.9)),
                         static_cast<unsigned long long>(h.percentile(99.99)), static_cast<unsigned long long>(h.max()),
                         results[i]->batched_ns, i < 3 ? "," : "");
        }
        std::fprintf(f, "  }\n}\n");
        std::fclose(f);
    }
    return 0;
}
