#include "core/matching_engine.h"
#include "core/order.h"
#include <cassert>
#include <iostream>
#include <random>
#include <vector>

using namespace perpetual;

static Price min_live_ask(const std::vector<Order*>& live) {
    Price min_price = 0;
    for (Order* o : live) {
        if (!o) continue;
        if (min_price == 0 || o->price < min_price) {
            min_price = o->price;
        }
    }
    return min_price;
}

static void test_rb_cancel_churn() {
    MatchingEngine engine(1);
    std::mt19937 rng(12345);
    std::vector<Order*> live;
    uint64_t id = 1;

    for (int step = 0; step < 100000; ++step) {
        if (live.empty() || (rng() & 1)) {
            Price p = 2000 + (rng() % 500);
            auto* o = new Order(id, 1, 1, OrderSide::SELL, p, 1, OrderType::LIMIT);
            engine.process_order(o);
            if (engine.get_order(id)) {
                live.push_back(engine.get_order(id));
            }
            id++;
        } else {
            size_t i = rng() % live.size();
            engine.cancel_order(live[i]->order_id, 1);
            live[i] = live.back();
            live.pop_back();
        }

        Price best = engine.get_orderbook().best_ask();
        Price expected = min_live_ask(live);
        assert(best == expected);
    }
}

static void test_phantom_depth() {
    MatchingEngine engine(1);
    engine.process_order(new Order(1, 1, 1, OrderSide::BUY, 100, 1, OrderType::LIMIT));
    engine.process_order(new Order(2, 1, 1, OrderSide::SELL, 100, 1, OrderType::LIMIT));

    assert(engine.get_orderbook().best_bid() == 0);

    std::vector<PriceLevel> bids;
    engine.get_orderbook().get_depth(1, bids, bids);
    assert(bids.empty());
}

static void test_partial_depth() {
    MatchingEngine engine(1);
    engine.process_order(new Order(1, 1, 1, OrderSide::BUY, 100, 10, OrderType::LIMIT));
    engine.process_order(new Order(2, 1, 1, OrderSide::SELL, 100, 4, OrderType::LIMIT));

    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    engine.get_orderbook().get_depth(1, bids, asks);
    assert(!bids.empty());
    assert(bids[0].total_quantity == 6);
}

static void test_ioc_sweep() {
    MatchingEngine engine(1);
    engine.process_order(new Order(1, 1, 1, OrderSide::SELL, 100, 1, OrderType::LIMIT));
    engine.process_order(new Order(2, 1, 1, OrderSide::SELL, 101, 1, OrderType::LIMIT));
    engine.process_order(new Order(3, 1, 1, OrderSide::SELL, 102, 1, OrderType::LIMIT));

    auto trades = engine.process_order(new Order(10, 2, 1, OrderSide::BUY, 105, 3, OrderType::IOC));
    assert(trades.size() == 3);
    assert(engine.get_orderbook().best_ask() == 0);
}

int main() {
    test_rb_cancel_churn();
    test_phantom_depth();
    test_partial_depth();
    test_ioc_sweep();
    std::cout << "issue #2 regression tests passed\n";
    return 0;
}
