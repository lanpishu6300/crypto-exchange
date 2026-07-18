#include "core/orderbook_art.h"
#include <algorithm>
#include <cassert>

namespace perpetual {

OrderBookSideART::OrderBookSideART(bool is_buy) : is_buy_(is_buy) {
}

OrderBookSideART::~OrderBookSideART() {
    // Cleanup handled by smart pointers and containers
}

bool OrderBookSideART::price_better(Price a, Price b) const {
    if (is_buy_) {
        return a > b;  // For bids, higher is better
    } else {
        return a < b;  // For asks, lower is better
    }
}

bool OrderBookSideART::insert(Order* order) {
    if (!order || order->price <= 0 || order->quantity <= 0) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Add to price level
    PriceLevel* level = get_or_create_price_level(order->price);
    add_order_to_price_level(level, order);
    
    // Insert into ART tree (if not already present)
    void* existing = art_tree_.find(order->price);
    if (existing == nullptr) {
        art_tree_.insert(order->price, level);
    }
    
    // Add to order map
    order_map_[order->order_id] = order;
    
    return true;
}

bool OrderBookSideART::remove(Order* order) {
    if (!order || order_map_.find(order->order_id) == order_map_.end()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Remove from price level
    PriceLevel* level = get_or_create_price_level(order->price);
    remove_order_from_price_level(level, order);
    
    // Remove price level if empty
    remove_price_level_if_empty(order->price);
    
    // Remove from order map
    order_map_.erase(order->order_id);
    
    return true;
}

bool OrderBookSideART::update_quantity(Order* order, Quantity new_quantity) {
    if (!order || order_map_.find(order->order_id) == order_map_.end()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    PriceLevel* level = get_or_create_price_level(order->price);
    level->total_quantity -= order->remaining_quantity;
    order->remaining_quantity = new_quantity;
    level->total_quantity += new_quantity;
    
    return true;
}

Price OrderBookSideART::best_price() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (art_tree_.empty()) {
        return 0;
    }
    
    if (is_buy_) {
        return art_tree_.max_key();  // Highest bid
    } else {
        return art_tree_.min_key();  // Lowest ask
    }
}

Quantity OrderBookSideART::best_quantity() const {
    Price price = best_price();
    if (price == 0) {
        return 0;
    }
    
    auto it = price_levels_.find(price);
    if (it != price_levels_.end()) {
        return it->second.total_quantity;
    }
    return 0;
}

Order* OrderBookSideART::best_order() const {
    Price price = best_price();
    if (price == 0) {
        return nullptr;
    }
    
    auto it = price_levels_.find(price);
    if (it != price_levels_.end() && it->second.first_order != nullptr) {
        return it->second.first_order;
    }
    return nullptr;
}

PriceLevel* OrderBookSideART::best_level() const {
    Price price = best_price();
    if (price == 0) {
        return nullptr;
    }
    
    auto it = price_levels_.find(price);
    if (it != price_levels_.end()) {
        return const_cast<PriceLevel*>(&it->second);
    }
    return nullptr;
}

Order* OrderBookSideART::find_order(OrderID order_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = order_map_.find(order_id);
    if (it != order_map_.end()) {
        return it->second;
    }
    return nullptr;
}

void OrderBookSideART::get_depth(size_t n, std::vector<PriceLevel>& levels) const {
    std::lock_guard<std::mutex> lock(mutex_);
    levels.clear();
    levels.reserve(n);
    
    // Collect prices from ART tree
    std::vector<Price> prices;
    prices.reserve(price_levels_.size());
    
    // Simplified: iterate through price_levels_ (full implementation would traverse ART)
    for (const auto& pair : price_levels_) {
        prices.push_back(pair.first);
    }
    
    // Sort based on buy/sell
    if (is_buy_) {
        std::sort(prices.rbegin(), prices.rend());  // Descending for bids
    } else {
        std::sort(prices.begin(), prices.end());  // Ascending for asks
    }
    
    // Get top N
    size_t count = std::min(n, prices.size());
    for (size_t i = 0; i < count; ++i) {
        auto it = price_levels_.find(prices[i]);
        if (it != price_levels_.end()) {
            levels.push_back(it->second);
        }
    }
}

PriceLevel* OrderBookSideART::get_or_create_price_level(Price price) {
    auto it = price_levels_.find(price);
    if (it != price_levels_.end()) {
        return &it->second;
    }
    
    PriceLevel level;
    level.price = price;
    level.total_quantity = 0;
    level.first_order = nullptr;
    level.last_order = nullptr;
    
    auto result = price_levels_.emplace(price, level);
    return &result.first->second;
}

void OrderBookSideART::remove_price_level_if_empty(Price price) {
    auto it = price_levels_.find(price);
    if (it != price_levels_.end() && it->second.first_order == nullptr) {
        price_levels_.erase(it);
        art_tree_.remove(price);
    }
}

void OrderBookSideART::add_order_to_price_level(PriceLevel* level, Order* order) {
    if (level->first_order == nullptr) {
        level->first_order = order;
        level->last_order = order;
        order->next_same_price = nullptr;
        order->prev_same_price = nullptr;
    } else {
        order->prev_same_price = level->last_order;
        order->next_same_price = nullptr;
        level->last_order->next_same_price = order;
        level->last_order = order;
    }
    level->total_quantity += order->remaining_quantity;
}

void OrderBookSideART::remove_order_from_price_level(PriceLevel* level, Order* order) {
    if (order->prev_same_price) {
        order->prev_same_price->next_same_price = order->next_same_price;
    } else {
        level->first_order = order->next_same_price;
    }
    
    if (order->next_same_price) {
        order->next_same_price->prev_same_price = order->prev_same_price;
    } else {
        level->last_order = order->prev_same_price;
    }
    
    level->total_quantity -= order->remaining_quantity;
    if (level->total_quantity < 0) {
        level->total_quantity = 0;
    }
    order->next_same_price = nullptr;
    order->prev_same_price = nullptr;
}

void OrderBookSideART::apply_trade_to_price_level(Order* order, Quantity traded_qty) {
    if (!order || traded_qty == 0) {
        return;
    }
    auto it = price_levels_.find(order->price);
    if (it != price_levels_.end()) {
        if (it->second.total_quantity >= traded_qty) {
            it->second.total_quantity -= traded_qty;
        } else {
            it->second.total_quantity = 0;
        }
    }
}

Quantity OrderBookSideART::crossable_quantity(Price limit_price, bool taker_is_buy) const {
    if (is_buy_ == taker_is_buy) {
        return 0;
    }
    Quantity total = 0;
    for (const auto& [price, level] : price_levels_) {
        if (level.total_quantity == 0) {
            continue;
        }
        if (taker_is_buy) {
            if (limit_price == 0 || price <= limit_price) {
                total += level.total_quantity;
            }
        } else if (limit_price == 0 || price >= limit_price) {
            total += level.total_quantity;
        }
    }
    return total;
}

// OrderBookART implementation
OrderBookART::OrderBookART(InstrumentID instrument_id) 
    : instrument_id_(instrument_id), bids_(true), asks_(false) {
}

OrderBookART::~OrderBookART() {
}

bool OrderBookART::insert_order(Order* order) {
    if (!order) {
        return false;
    }
    
    if (order->side == OrderSide::BUY) {
        return bids_.insert(order);
    } else {
        return asks_.insert(order);
    }
}

bool OrderBookART::remove_order(Order* order) {
    if (!order) {
        return false;
    }
    
    if (order->side == OrderSide::BUY) {
        return bids_.remove(order);
    } else {
        return asks_.remove(order);
    }
}

bool OrderBookART::update_order(Order* order, Price new_price, Quantity new_quantity) {
    if (!order) {
        return false;
    }
    
    // Remove and reinsert with new price/quantity
    remove_order(order);
    order->price = new_price;
    order->quantity = new_quantity;
    return insert_order(order);
}

Price OrderBookART::spread() const {
    Price best_bid_price = best_bid();
    Price best_ask_price = best_ask();
    
    if (best_bid_price == 0 || best_ask_price == 0) {
        return 0;
    }
    
    return best_ask_price - best_bid_price;
}

Price OrderBookART::mid_price() const {
    Price best_bid_price = best_bid();
    Price best_ask_price = best_ask();
    
    if (best_bid_price == 0 || best_ask_price == 0) {
        return 0;
    }
    
    return (best_bid_price + best_ask_price) / 2;
}

bool OrderBookART::can_match(Order* order) const {
    if (!order) {
        return false;
    }
    
    if (order->side == OrderSide::BUY) {
        Price best_ask_price = asks_.best_price();
        return best_ask_price > 0 && order->price >= best_ask_price;
    } else {
        Price best_bid_price = bids_.best_price();
        return best_bid_price > 0 && order->price <= best_bid_price;
    }
}

void OrderBookART::get_depth(size_t n, std::vector<PriceLevel>& bids, 
                            std::vector<PriceLevel>& asks) const {
    bids_.get_depth(n, bids);
    asks_.get_depth(n, asks);
}

void OrderBookART::apply_trade_to_price_level(Order* order, Quantity traded_qty) {
    if (!order) {
        return;
    }
    if (order->is_buy()) {
        bids_.apply_trade_to_price_level(order, traded_qty);
    } else {
        asks_.apply_trade_to_price_level(order, traded_qty);
    }
}

Quantity OrderBookART::crossable_quantity(Order* taker) const {
    if (!taker) {
        return 0;
    }
    Price limit = (taker->order_type == OrderType::MARKET) ? 0 : taker->price;
    if (taker->is_buy()) {
        return asks_.crossable_quantity(limit, true);
    }
    return bids_.crossable_quantity(limit, false);
}

} // namespace perpetual

