#pragma once

#include "order.h"
#include <map>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <vector>

namespace perpetual {

// Price level represents all orders at a specific price
struct PriceLevel {
    Price price;
    Quantity total_quantity;
    Order* first_order;  // Linked list of orders at this price
    Order* last_order;
    
    PriceLevel() : price(0), total_quantity(0), first_order(nullptr), last_order(nullptr) {}
};

// Order book side (bid or ask)
// Uses red-black tree structure for O(log n) operations
class OrderBookSide {
public:
    OrderBookSide(bool is_buy);
    ~OrderBookSide();
    
    // Insert order into the book
    // Returns true if successful
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
    
    // Find order by order_id (O(log n))
    Order* find_order(OrderID order_id) const;
    
    // Get total number of orders
    size_t size() const { return order_map_.size(); }
    
    // Get total number of price levels
    size_t price_levels() const { return price_levels_.size(); }
    
    // Check if empty
    bool empty() const;
    
    // Get top N price levels for market data
    void get_depth(size_t n, std::vector<PriceLevel>& levels) const;

    // Adjust aggregated depth after a trade (maker/taker resting in book)
    void apply_trade_to_price_level(Order* order, Quantity traded_qty);

    // Sum crossable resting quantity for FOK pre-check
    Quantity crossable_quantity(Price limit_price, bool taker_is_buy) const;
    
private:
    // Red-black tree operations
    void rotate_left(Order* node);
    void rotate_right(Order* node);
    void fix_insert(Order* node);
    void fix_delete(Order* node);
    Order* find_min(Order* node) const;
    Order* find_max(Order* node) const;
    Order* successor(Order* node) const;
    Order* predecessor(Order* node) const;
    
    // Price level management
    PriceLevel* get_or_create_price_level(Price price);
    void remove_price_level_if_empty(Price price);
    void add_order_to_price_level(PriceLevel* level, Order* order);
    void remove_order_from_price_level(PriceLevel* level, Order* order);
    
    // Price comparison for buy vs sell
    bool price_better(Price a, Price b) const;
    bool price_equal_or_better(Price a, Price b) const;
    
private:
    bool is_buy_;  // True for bids, false for asks
    Order* root_;  // Root of red-black tree
    Order* sentinel_;  // Sentinel node (nil)
    
    // Fast lookup by order_id
    std::unordered_map<OrderID, Order*> order_map_;
    
    // Price level aggregation
    std::map<Price, PriceLevel> price_levels_;
    
    // For thread safety (can be removed if single-threaded)
    mutable std::mutex mutex_;
};

// Full order book (both sides)
class OrderBook {
public:
    OrderBook(InstrumentID instrument_id);
    ~OrderBook();
    
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
    
    // Access order book sides (for matching engine)
    OrderBookSide& bids() { return bids_; }
    OrderBookSide& asks() { return asks_; }
    const OrderBookSide& bids() const { return bids_; }
    const OrderBookSide& asks() const { return asks_; }
    
private:
    InstrumentID instrument_id_;
    OrderBookSide bids_;  // Buy orders
    OrderBookSide asks_;  // Sell orders
};

} // namespace perpetual
