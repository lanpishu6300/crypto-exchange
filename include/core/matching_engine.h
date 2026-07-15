#pragma once

#include "orderbook.h"
#include "order.h"
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <unordered_map>

namespace perpetual {

// Callback for trade events
using TradeCallback = std::function<void(const Trade& trade)>;
using OrderCallback = std::function<void(Order* order)>;

// Matching engine - the core of the exchange
// Designed for nanosecond-level latency
class MatchingEngine {
public:
    MatchingEngine(InstrumentID instrument_id);
    ~MatchingEngine();
    
    // Process new order
    // Returns list of trades generated
    std::vector<Trade> process_order(Order* order);
    
    // Cancel order
    bool cancel_order(OrderID order_id, UserID user_id);
    
    // Cancel all orders for a user
    void cancel_all_orders(UserID user_id);
    
    // Get order by ID
    Order* get_order(OrderID order_id) const;
    
    // Get order book
    const OrderBook& get_orderbook() const { return orderbook_; }
    OrderBook& get_orderbook() { return orderbook_; }
    
    // Register callbacks
    void set_trade_callback(TradeCallback cb) { trade_callback_ = cb; }
    void set_order_update_callback(OrderCallback cb) { order_update_callback_ = cb; }
    
    // Get statistics
    uint64_t total_trades() const { return total_trades_; }
    uint64_t total_volume() const { return total_volume_; }
    
protected:
    // Match order against the book
    std::vector<Trade> match_order(Order* order);
    
    // Execute a trade
    void execute_trade(Order* buy_order, Order* sell_order, Price price, Quantity quantity);
    
    // Create trade record
    Trade create_trade(Order* buy_order, Order* sell_order, Price price, Quantity quantity);
    
    // Update order after trade
    void update_order_after_trade(Order* order, Quantity traded_qty);
    
    // Remove filled/cancelled order from book
    void remove_order_from_book(Order* order);
    
    // Validate order
    bool validate_order(const Order* order) const;
    
    // Check if orders can match
    bool can_orders_match(const Order* buy, const Order* sell) const;
    
    // Get match price (price-time priority)
    Price get_match_price(const Order* incoming, const Order* resting) const;

    // FOK all-or-nothing pre-check
    bool validate_fok_order(Order* order, OrderBookSide* opposite_side) const;
    
    InstrumentID instrument_id_;
    OrderBook orderbook_;
    
    // Order storage (owned by matching engine)
    std::unordered_map<OrderID, std::unique_ptr<Order>> orders_;
    
    // User's orders index
    std::unordered_map<UserID, std::vector<OrderID>> user_orders_;
    
    // Callbacks
    TradeCallback trade_callback_;
    OrderCallback order_update_callback_;
    
    // Statistics
    std::atomic<uint64_t> total_trades_{0};
    std::atomic<uint64_t> total_volume_{0};
    
    // Sequence generator for trades
    std::atomic<SequenceID> trade_sequence_{0};
    
private:
};

} // namespace perpetual
