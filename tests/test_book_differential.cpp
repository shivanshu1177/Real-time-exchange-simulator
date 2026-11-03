// Differential test: drive OrderBook and the naive ReferenceBook with the same
// random operations and require identical fills, cancels, BBO and depth after
// every operation. Default: 100 seeds x 100,000 operations. The sanitizer
// test presets lower this through RTES_DIFF_SEEDS / RTES_DIFF_OPS because the
// instrumented build is many times slower.
#include <gtest/gtest.h>
#include "rtes/order_book.hpp"
#include "reference_book.hpp"
#include <algorithm>
#include <cstdlib>
#include <random>

using namespace rtes;
using namespace rtes::test;

namespace {

size_t env_or(const char* name, size_t fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? static_cast<size_t>(std::strtoull(v, nullptr, 10)) : fallback;
}

std::string describe(const std::vector<BookEvent>& ev) {
    std::string s;
    for (auto& e : ev) {
        s += (e.kind == BookEvent::TRADE)
            ? "T(" + std::to_string(e.a) + "," + std::to_string(e.b) + "," + std::to_string(e.qty) + "@" + std::to_string(e.price) + ") "
            : "C(" + std::to_string(e.a) + "," + std::to_string(static_cast<int>(e.cause)) + ") ";
    }
    return s;
}

// What the random operations actually exercised, so a passing run can be
// shown to be meaningful (not an empty book that never trades).
struct Coverage {
    size_t trades = 0, self_trade_cancels = 0, no_liquidity_cancels = 0;
    size_t cancels = 0, modifies = 0, max_resting = 0;
};

// Returns the number of divergences (0 expected). Stops at the first one.
size_t run_seed(uint32_t seed, size_t ops, Coverage& cov) {
    const StpMode stp = static_cast<StpMode>(seed % 3);
    constexpr size_t kPool = 20000;
    OrderPool pool(kPool);
    RecordingListener rec;
    OrderBook book("DIFF", pool, kPool, &rec, stp);
    ReferenceBook ref(stp);

    std::mt19937_64 rng(seed);
    auto uni = [&](uint64_t lo, uint64_t hi) { return std::uniform_int_distribution<uint64_t>(lo, hi)(rng); };

    // Two profiles: a shallow, fast-churning book with few clients (many
    // self-trades), and a deep book with more clients and fewer cancels
    // (long queues, many levels). Bids and asks overlap only in 96..104, so
    // orders accumulate on both sides instead of always crossing.
    const bool     deep       = (seed % 2) == 0;
    const uint64_t clients    = deep ? 10 : 3;
    const uint64_t new_pct    = deep ? 70 : 50;
    const uint64_t cancel_pct = deep ? 15 : 30;

    OrderID next_id = 1;
    for (size_t step = 0; step < ops; ++step) {
        const auto live = ref.resting_ids();
        const uint64_t roll = uni(0, 99);

        // Keep the deep book deep but bounded (the reference is O(n) per op).
        const bool force_cancel = live.size() >= 400;

        if (!force_cancel && (roll < new_pct || live.empty())) {   // new order
            Order* o = pool.acquire();
            if (!o) { ADD_FAILURE() << "pool exhausted, seed " << seed; return 1; }
            *o = Order{};
            o->order_id  = next_id++;
            o->client_id = uni(1, clients);
            o->side      = uni(0, 1) ? Side::SELL : Side::BUY;
            o->type      = uni(0, 19) == 0 ? OrderType::MARKET : OrderType::LIMIT;
            o->price     = o->type == OrderType::MARKET ? 0
                         : o->side == Side::BUY ? uni(90, 104) : uni(96, 110);
            o->quantity  = static_cast<Quantity>(uni(1, 20));
            ReferenceBook::RefOrder r;
            r.id = o->order_id; r.client = o->client_id; r.side = o->side; r.type = o->type;
            r.price = o->price; r.qty = o->quantity;
            if (!book.add_order(o)) { ADD_FAILURE() << "add_order refused, seed " << seed; return 1; }
            ref.add(r);
        } else if (force_cancel || roll < new_pct + cancel_pct) {  // cancel
            const OrderID id = live[uni(0, live.size() - 1)];
            const bool a = book.cancel_order(id), b = ref.cancel(id);
            if (a != b) { ADD_FAILURE() << "cancel result differs, seed " << seed << " step " << step; return 1; }
        } else {                                               // modify
            const OrderID id = live[uni(0, live.size() - 1)];
            const auto* r = ref.find(id);
            const Price    price = uni(0, 2) == 0 ? r->price
                                 : r->side == Side::BUY ? uni(90, 104) : uni(96, 110);
            const Quantity qty   = r->filled + static_cast<Quantity>(uni(1, 20));
            const bool a = book.modify_order(id, price, qty), b = ref.modify(id, price, qty);
            if (a != b) { ADD_FAILURE() << "modify result differs, seed " << seed << " step " << step; return 1; }
        }

        for (auto& e : ref.events) {
            if (e.kind == BookEvent::TRADE) ++cov.trades;
            else if (e.cause == CancelCause::SELF_TRADE) ++cov.self_trade_cancels;
            else ++cov.no_liquidity_cancels;
        }
        if (force_cancel || (roll >= new_pct && !live.empty()))
            ++((force_cancel || roll < new_pct + cancel_pct) ? cov.cancels : cov.modifies);
        cov.max_resting = std::max(cov.max_resting, book.order_count());

        if (!(rec.events == ref.events)) {
            ADD_FAILURE() << "events differ, seed " << seed << " step " << step
                          << "\n  book: " << describe(rec.events) << "\n  ref:  " << describe(ref.events);
            return 1;
        }
        rec.events.clear();
        ref.events.clear();

        if (levels_of(book, Side::BUY) != ref.levels(Side::BUY) ||
            levels_of(book, Side::SELL) != ref.levels(Side::SELL)) {
            ADD_FAILURE() << "depth differs, seed " << seed << " step " << step;
            return 1;
        }
        if (book.order_count() != ref.resting_ids().size()) {
            ADD_FAILURE() << "order count differs, seed " << seed << " step " << step;
            return 1;
        }
    }

    // Everything comes back to the pool.
    book.clear();
    if (pool.available() != kPool) {
        ADD_FAILURE() << "pool leak, seed " << seed << ": " << pool.available() << " of " << kPool;
        return 1;
    }
    return 0;
}

} // namespace

TEST(BookDifferential, MatchesReferenceBook) {
    const size_t seeds = env_or("RTES_DIFF_SEEDS", 100);
    const size_t ops   = env_or("RTES_DIFF_OPS", 100000);
    size_t divergences = 0;
    Coverage cov;
    for (uint32_t seed = 1; seed <= seeds; ++seed) divergences += run_seed(seed, ops, cov);
    EXPECT_EQ(divergences, 0u);
    // The run must actually exercise every path it claims to check.
    EXPECT_GT(cov.trades, 0u);
    EXPECT_GT(cov.self_trade_cancels, 0u);
    EXPECT_GT(cov.no_liquidity_cancels, 0u);
    EXPECT_GT(cov.modifies, 0u);
    std::cout << "[ differential ] " << seeds << " seeds x " << ops << " ops, "
              << divergences << " divergences\n"
              << "[ differential ] trades " << cov.trades << ", self-trade cancels " << cov.self_trade_cancels
              << ", no-liquidity cancels " << cov.no_liquidity_cancels << ", cancels " << cov.cancels
              << ", modifies " << cov.modifies << ", max resting orders " << cov.max_resting << "\n";
}
