#pragma once

#include "order.h"
#include "orderbook.h"  // For PriceLevel
#include "art_tree_simd.h"
#include <map>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace perpetual {

// Order book side using ART with SIMD optimizations
class OrderBookSideARTSIMD {
public:
    OrderBookSideARTSIMD(bool is_buy);
    ~OrderBookSideARTSIMD();
    
    // Insert order
    bool insert(Order* order);
    
    // Remove order
    bool remove(Order* order);
    
    // Update order quantity
    bool update_quantity(Order* order, Quantity new_quantity);
    
    // Get best price (SIMD optimized)
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
    bool empty() const { return art_tree_simd_.empty(); }
    
    // Get top N price levels
    void get_depth(size_t n, std::vector<PriceLevel>& levels) const;

    void apply_trade_to_price_level(Order* order, Quantity traded_qty);
    Quantity crossable_quantity(Price limit_price, bool taker_is_buy) const;
    
private:
    // Price level management
    PriceLevel* get_or_create_price_level(Price price);
    void remove_price_level_if_empty(Price price);
    void add_order_to_price_level(PriceLevel* level, Order* order);
    void remove_order_from_price_level(PriceLevel* level, Order* order);
    
    // Price comparison
    bool price_better(Price a, Price b) const;
    
private:
    bool is_buy_;
    ARTTreeSIMD art_tree_simd_;
    std::unordered_map<OrderID, Order*> order_map_;
    std::unordered_map<Price, PriceLevel> price_levels_;
    mutable std::mutex mutex_;
};

// Full order book using ART with SIMD
class OrderBookARTSIMD {
public:
    OrderBookARTSIMD(InstrumentID instrument_id);
    ~OrderBookARTSIMD();
    
    // Insert order
    bool insert_order(Order* order);
    
    // Remove order
    bool remove_order(Order* order);
    
    // Update order
    bool update_order(Order* order, Price new_price, Quantity new_quantity);
    
    // Get best bid and ask (SIMD optimized)
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
    OrderBookSideARTSIMD& bids() { return bids_; }
    OrderBookSideARTSIMD& asks() { return asks_; }
    const OrderBookSideARTSIMD& bids() const { return bids_; }
    const OrderBookSideARTSIMD& asks() const { return asks_; }
    
private:
    InstrumentID instrument_id_;
    OrderBookSideARTSIMD bids_;
    OrderBookSideARTSIMD asks_;
};

} // namespace perpetual
