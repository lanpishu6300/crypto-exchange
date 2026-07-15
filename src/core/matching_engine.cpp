#include "core/matching_engine.h"
#include <algorithm>
#include <unordered_map>

namespace perpetual {

MatchingEngine::MatchingEngine(InstrumentID instrument_id)
    : instrument_id_(instrument_id), orderbook_(instrument_id) {
}

MatchingEngine::~MatchingEngine() {
}

std::vector<Trade> MatchingEngine::process_order(Order* order) {
    if (!order || !validate_order(order)) {
        if (order) {
            order->status = OrderStatus::REJECTED;
        }
        return {};
    }
    
    // Set sequence
    order->sequence_id = ++trade_sequence_;
    order->timestamp = get_current_timestamp();
    
    // Match the order
    std::vector<Trade> trades = match_order(order);
    
    // If order is not fully filled and is a limit order, add to book
    if (order->is_active() && order->order_type == OrderType::LIMIT) {
        if (order->remaining_quantity > 0) {
            // Check if order already exists in orders_ map
            auto it = orders_.find(order->order_id);
            if (it == orders_.end()) {
                // New order - insert into book and take ownership
                if (orderbook_.insert_order(order)) {
                    orders_[order->order_id] = std::unique_ptr<Order>(order);
                    user_orders_[order->user_id].push_back(order->order_id);
                    
                    if (order_update_callback_) {
                        order_update_callback_(order);
                    }
                }
            } else {
                // Order already exists in orders_ map
                // Check if it's already in the orderbook by trying to find it
                OrderBookSide& side = (order->side == OrderSide::BUY) ? 
                    orderbook_.bids() : orderbook_.asks();
                if (!side.find_order(order->order_id)) {
                    // Order not in orderbook yet - insert it (e.g., after partial fill)
                    orderbook_.insert_order(order);
                }
                
                if (order_update_callback_) {
                    order_update_callback_(order);
                }
            }
        }
    }
    
    return trades;
}

std::vector<Trade> MatchingEngine::match_order(Order* order) {
    std::vector<Trade> trades;
    
    if (!order || order->remaining_quantity == 0) {
        return trades;
    }
    
    // Get opposite side of order book
    // For buy orders, match against asks (sell side)
    // For sell orders, match against bids (buy side)
    OrderBookSide* opposite_side = nullptr;
    if (order->is_buy()) {
        opposite_side = &orderbook_.asks();
    } else {
        opposite_side = &orderbook_.bids();
    }
    
    if (!opposite_side) {
        return trades;
    }

    if (order->order_type == OrderType::FOK &&
        !validate_fok_order(order, opposite_side)) {
        order->status = OrderStatus::CANCELLED;
        return trades;
    }
    
    // Match against resting orders (hot path optimization)
    // Add safety counter to prevent infinite loops
    const size_t max_iterations = 10000;  // Safety limit
    size_t iteration_count = 0;
    
    while (order->remaining_quantity > 0 && !opposite_side->empty() && iteration_count < max_iterations) {
        ++iteration_count;
        
        Order* resting_order = opposite_side->best_order();
        
        if (!resting_order || !can_orders_match(order, resting_order)) {
            break;
        }
        
        // Determine trade quantity (use branchless min for better performance)
        Quantity trade_qty = order->remaining_quantity;
        if (resting_order->remaining_quantity < trade_qty) {
            trade_qty = resting_order->remaining_quantity;
        }
        
        // Get match price (price-time priority)
        Price match_price = get_match_price(order, resting_order);
        
        // Execute trade
        execute_trade(order, resting_order, match_price, trade_qty);
        
        // Create trade record
        Trade trade = create_trade(order, resting_order, match_price, trade_qty);
        trades.push_back(trade);
        
        // Update statistics
        total_trades_++;
        total_volume_ += trade_qty;
        
        // Call trade callback
        if (trade_callback_) {
            trade_callback_(trade);
        }
        
        // Update orders
        update_order_after_trade(order, trade_qty);
        update_order_after_trade(resting_order, trade_qty);
        
        // Remove filled order from book
        if (resting_order->is_filled()) {
            remove_order_from_book(resting_order);
            if (order_update_callback_) {
                order_update_callback_(resting_order);
            }
            // After removing, check if we should break to prevent infinite loop
            if (opposite_side->empty()) {
                break;
            }
        }
    }
    
    // IOC/FOK: cancel any unfilled remainder (loop above sweeps all crossable makers)
    if ((order->order_type == OrderType::IOC || order->order_type == OrderType::FOK) &&
        order->remaining_quantity > 0) {
        order->status = OrderStatus::CANCELLED;
    }
    
    // Update order status
    if (order->remaining_quantity == 0) {
        order->status = OrderStatus::FILLED;
    } else if (order->filled_quantity > 0) {
        order->status = OrderStatus::PARTIAL_FILLED;
    }
    
    // Call order update callback
    if (order_update_callback_) {
        order_update_callback_(order);
    }
    
    return trades;
}

void MatchingEngine::execute_trade(Order* buy_order, Order* sell_order, 
                                    Price price, Quantity quantity) {
    // Determine which is buy and which is sell
    Order* actual_buy = buy_order->is_buy() ? buy_order : sell_order;
    Order* actual_sell = buy_order->is_buy() ? sell_order : buy_order;
    
    // Update filled quantities
    actual_buy->filled_quantity += quantity;
    actual_buy->remaining_quantity -= quantity;
    
    actual_sell->filled_quantity += quantity;
    actual_sell->remaining_quantity -= quantity;

    // Keep price-level aggregates in sync with partial/full fills
    if (orderbook_.bids().find_order(actual_buy->order_id)) {
        orderbook_.apply_trade_to_price_level(actual_buy, quantity);
    }
    if (orderbook_.asks().find_order(actual_sell->order_id)) {
        orderbook_.apply_trade_to_price_level(actual_sell, quantity);
    }
}

Trade MatchingEngine::create_trade(Order* buy_order, Order* sell_order, 
                                    Price price, Quantity quantity) {
    Order* actual_buy = buy_order->is_buy() ? buy_order : sell_order;
    Order* actual_sell = buy_order->is_buy() ? sell_order : buy_order;
    
    Trade trade;
    trade.buy_order_id = actual_buy->order_id;
    trade.sell_order_id = actual_sell->order_id;
    trade.buy_user_id = actual_buy->user_id;
    trade.sell_user_id = actual_sell->user_id;
    trade.instrument_id = instrument_id_;
    trade.price = price;
    trade.quantity = quantity;
    trade.timestamp = get_current_timestamp();
    trade.sequence_id = ++trade_sequence_;
    
    // Determine if buy order is taker (incoming order)
    trade.is_taker_buy = (actual_buy->sequence_id > actual_sell->sequence_id);
    
    return trade;
}

void MatchingEngine::update_order_after_trade(Order* order, Quantity traded_qty) {
    if (!order) return;
    
    // Order quantities are already updated in execute_trade
    // Update order status will be handled by caller
}

void MatchingEngine::remove_order_from_book(Order* order) {
    if (!order) return;
    
    orderbook_.remove_order(order);
    
    // Remove from order storage
    orders_.erase(order->order_id);
    
    // Remove from user's orders
    auto user_it = user_orders_.find(order->user_id);
    if (user_it != user_orders_.end()) {
        auto& user_orders = user_it->second;
        user_orders.erase(
            std::remove(user_orders.begin(), user_orders.end(), order->order_id),
            user_orders.end()
        );
        if (user_orders.empty()) {
            user_orders_.erase(user_it);
        }
    }
}

bool MatchingEngine::validate_order(const Order* order) const {
    if (!order) return false;
    if (order->instrument_id != instrument_id_) return false;
    if (order->quantity <= 0) return false;
    if (order->price <= 0 && order->order_type == OrderType::LIMIT) return false;
    return true;
}

bool MatchingEngine::can_orders_match(const Order* buy, const Order* sell) const {
    // Ensure one is buy and one is sell
    if (buy->side == sell->side) return false;
    
    // Determine which is actually buy and sell
    const Order* actual_buy = buy->is_buy() ? buy : sell;
    const Order* actual_sell = buy->is_buy() ? sell : buy;
    
    // Check price compatibility
    // Market orders always match
    if (actual_buy->order_type == OrderType::MARKET || 
        actual_sell->order_type == OrderType::MARKET) {
        return true;
    }
    
    // Limit orders: buy price must be >= sell price
    return actual_buy->price >= actual_sell->price;
}

Price MatchingEngine::get_match_price(const Order* incoming, const Order* resting) const {
    // Price-time priority: use the price of the order that was in the book first
    // (resting order)
    return resting->price;
}

bool MatchingEngine::validate_fok_order(Order* order, OrderBookSide* opposite_side) const {
    if (!order || !opposite_side) {
        return false;
    }
    return orderbook_.crossable_quantity(order) >= order->remaining_quantity;
}

bool MatchingEngine::cancel_order(OrderID order_id, UserID user_id) {
    Order* order = get_order(order_id);
    if (!order || order->user_id != user_id) {
        return false;
    }
    
    if (!order->is_active()) {
        return false;
    }
    
    order->status = OrderStatus::CANCELLED;
    remove_order_from_book(order);
    
    if (order_update_callback_) {
        order_update_callback_(order);
    }
    
    return true;
}

void MatchingEngine::cancel_all_orders(UserID user_id) {
    auto user_it = user_orders_.find(user_id);
    if (user_it == user_orders_.end()) {
        return;
    }
    
    // Make a copy since we'll modify the vector
    std::vector<OrderID> order_ids = user_it->second;
    
    for (OrderID order_id : order_ids) {
        cancel_order(order_id, user_id);
    }
}

Order* MatchingEngine::get_order(OrderID order_id) const {
    auto it = orders_.find(order_id);
    if (it != orders_.end()) {
        return it->second.get();
    }
    return nullptr;
}

} // namespace perpetual
