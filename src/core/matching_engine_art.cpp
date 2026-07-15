#include "core/matching_engine_art.h"
#include <algorithm>

namespace perpetual {

MatchingEngineART::MatchingEngineART(InstrumentID instrument_id)
    : MatchingEngine(instrument_id),
      orderbook_art_(instrument_id),
      trade_sequence_(0),
      total_trades_(0),
      total_volume_(0.0) {
}

MatchingEngineART::~MatchingEngineART() {
}

std::vector<Trade> MatchingEngineART::process_order_art(Order* order) {
    if (!order || order->remaining_quantity <= 0) {
        return {};
    }
    
    // Store order
    orders_[order->order_id] = std::unique_ptr<Order>(order);
    user_orders_[order->user_id].push_back(order->order_id);
    
    // Match order
    std::vector<Trade> trades = match_order_art(order);
    
    // If order not fully filled, add to order book
    if (order->remaining_quantity > 0 && order->order_type == OrderType::LIMIT) {
        orderbook_art_.insert_order(order);
    }
    
    return trades;
}

std::vector<Trade> MatchingEngineART::match_order_art(Order* order) {
    std::vector<Trade> trades;
    
    if (order->order_type == OrderType::FOK &&
        orderbook_art_.crossable_quantity(order) < order->remaining_quantity) {
        order->status = OrderStatus::CANCELLED;
        return trades;
    }
    
    if (order->side == OrderSide::BUY) {
        // Match against asks
        OrderBookSideART& asks = orderbook_art_.asks();
        
        // Add safety counter to prevent infinite loops
        const size_t max_iterations = 10000;
        size_t iteration_count = 0;
        
        while (order->remaining_quantity > 0 && !asks.empty() && iteration_count < max_iterations) {
            ++iteration_count;
            
            Price best_ask = asks.best_price();
            if (best_ask == 0 || order->price < best_ask) {
                break;  // Cannot match
            }
            
            PriceLevel* level = asks.best_level();
            if (!level || level->first_order == nullptr) {
                break;
            }
            
            Order* maker = level->first_order;
            Price trade_price = maker->price;  // Price-time priority
            Quantity trade_qty = std::min(order->remaining_quantity, maker->remaining_quantity);
            
            execute_trade_art(order, maker, trade_price, trade_qty);
            
            Trade trade;
            trade.buy_order_id = order->order_id;
            trade.sell_order_id = maker->order_id;
            trade.buy_user_id = order->user_id;
            trade.sell_user_id = maker->user_id;
            trade.instrument_id = order->instrument_id;
            trade.price = trade_price;
            trade.quantity = trade_qty;
            trade.timestamp = get_current_timestamp();
            trade.sequence_id = ++trade_sequence_;
            trade.is_taker_buy = true;
            trades.push_back(trade);
            
            total_trades_++;
            total_volume_ += quantity_to_double(trade_qty);
            
            // Remove maker if fully filled
            if (maker->remaining_quantity == 0) {
                orderbook_art_.remove_order(maker);
            }
        }
    } else {
        // Match against bids
        OrderBookSideART& bids = orderbook_art_.bids();
        
        // Add safety counter to prevent infinite loops
        const size_t max_iterations = 10000;
        size_t iteration_count = 0;
        
        while (order->remaining_quantity > 0 && !bids.empty() && iteration_count < max_iterations) {
            ++iteration_count;
            
            Price best_bid = bids.best_price();
            if (best_bid == 0 || order->price > best_bid) {
                break;  // Cannot match
            }
            
            PriceLevel* level = bids.best_level();
            if (!level || level->first_order == nullptr) {
                break;
            }
            
            Order* maker = level->first_order;
            Price trade_price = maker->price;  // Price-time priority
            Quantity trade_qty = std::min(order->remaining_quantity, maker->remaining_quantity);
            
            execute_trade_art(order, maker, trade_price, trade_qty);
            
            Trade trade;
            trade.buy_order_id = order->order_id;
            trade.sell_order_id = maker->order_id;
            trade.buy_user_id = order->user_id;
            trade.sell_user_id = maker->user_id;
            trade.instrument_id = order->instrument_id;
            trade.price = trade_price;
            trade.quantity = trade_qty;
            trade.timestamp = get_current_timestamp();
            trade.sequence_id = ++trade_sequence_;
            trade.is_taker_buy = true;
            trades.push_back(trade);
            
            total_trades_++;
            total_volume_ += quantity_to_double(trade_qty);
            
            // Remove maker if fully filled
            if (maker->remaining_quantity == 0) {
                orderbook_art_.remove_order(maker);
            }
        }
    }

    if ((order->order_type == OrderType::IOC || order->order_type == OrderType::FOK) &&
        order->remaining_quantity > 0) {
        order->status = OrderStatus::CANCELLED;
    }
    
    return trades;
}

void MatchingEngineART::execute_trade_art(Order* taker, Order* maker, Price price, Quantity quantity) {
    taker->remaining_quantity -= quantity;
    taker->filled_quantity += quantity;
    maker->remaining_quantity -= quantity;
    maker->filled_quantity += quantity;

    orderbook_art_.apply_trade_to_price_level(maker, quantity);
    
    if (taker->remaining_quantity == 0) {
        taker->status = OrderStatus::FILLED;
    } else {
        taker->status = OrderStatus::PARTIAL_FILLED;
    }
    
    if (maker->remaining_quantity == 0) {
        maker->status = OrderStatus::FILLED;
    } else {
        maker->status = OrderStatus::PARTIAL_FILLED;
    }
}

} // namespace perpetual

