#pragma once

#include "order.h"
#include "orderbook.h"  // For PriceLevel definition
#include "art_tree.h"
#include <map>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace perpetual {

// Order book side using ART (Adaptive Radix Tree) instead of Red-Black Tree
// Optimized for better cache locality and memory efficiency
class OrderBookSideART {
public:
    OrderBookSideART(bool is_buy);
    ~OrderBookSideART();
    
    // Insert order into the book
    bool insert(Order* order);
    
    // Remove order from the book
    bool remove(Order* order);
    
    // Update order quantity
    bool update_quantity(Order* order, Quantity new_quantity);
    
    // Get best price (highest bid or lowest ask)
    Price best_price() const;
    
    // Get total quantity at best price
    Quantity best_quantity() const;
    
    // Get best order
    Order* best_order() const;
    
    // Get price level at best price
    PriceLevel* best_level() const;
    
    // Find order by order_id
    Order* find_order(OrderID order_id) const;
    
    // Get total number of orders
    size_t size() const { return order_map_.size(); }
    
    // Get total number of price levels
    size_t price_levels() const { return price_levels_.size(); }
    
    // Check if empty
    bool empty() const { return art_tree_.empty(); }
    
    // Get top N price levels for market data
    void get_depth(size_t n, std::vector<PriceLevel>& levels) const;

    void apply_trade_to_price_level(Order* order, Quantity traded_qty);
    Quantity crossable_quantity(Price limit_price, bool taker_is_buy) const;
    
private:
    // Price level management
    PriceLevel* get_or_create_price_level(Price price);
    void remove_price_level_if_empty(Price price);
    void add_order_to_price_level(PriceLevel* level, Order* order);
    void remove_order_from_price_level(PriceLevel* level, Order* order);
    
    // Price comparison for buy vs sell
    bool price_better(Price a, Price b) const;
    
private:
    bool is_buy_;  // True for bids, false for asks
    
    // ART tree for price-based lookup (O(k) where k is key length, typically 8 bytes)
    ARTTree art_tree_;
    
    // Fast lookup by order_id
    std::unordered_map<OrderID, Order*> order_map_;
    
    // Price level aggregation (stored in ART tree values)
    std::unordered_map<Price, PriceLevel> price_levels_;
    
    // For thread safety
    mutable std::mutex mutex_;
};

// Full order book using ART (both sides)
class OrderBookART {
public:
    OrderBookART(InstrumentID instrument_id);
    ~OrderBookART();
    
    // Insert order into appropriate side
    bool insert_order(Order* order);
    
    // Remove order
    bool remove_order(Order* order);
    
    // Update order
    bool update_order(Order* order, Price new_price, Quantity new_quantity);
    
    // Get best bid and ask
    Price best_bid() const { return bids_.best_price(); }
    Price best_ask() const { return asks_.best_price(); }
    
    // Get spread
    Price spread() const;
    
    // Get mid price
    Price mid_price() const;
    
    // Check if order can match
    bool can_match(Order* order) const;
    
    // Get instrument ID
    InstrumentID instrument_id() const { return instrument_id_; }
    
    // Get order book depth
    void get_depth(size_t n, std::vector<PriceLevel>& bids, 
                   std::vector<PriceLevel>& asks) const;

    void apply_trade_to_price_level(Order* order, Quantity traded_qty);
    Quantity crossable_quantity(Order* taker) const;
    
    // Access order book sides
    OrderBookSideART& bids() { return bids_; }
    OrderBookSideART& asks() { return asks_; }
    const OrderBookSideART& bids() const { return bids_; }
    const OrderBookSideART& asks() const { return asks_; }
    
private:
    InstrumentID instrument_id_;
    OrderBookSideART bids_;  // Buy orders
    OrderBookSideART asks_;  // Sell orders
};

} // namespace perpetual

