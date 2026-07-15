#include "core/matching_engine_art_simd.h"
#include "core/art_tree_simd_enhanced.h"
#include <algorithm>

namespace perpetual {

MatchingEngineARTSIMD::MatchingEngineARTSIMD(InstrumentID instrument_id)
    : MatchingEngineART(instrument_id),
      orderbook_art_simd_(instrument_id) {
}

MatchingEngineARTSIMD::~MatchingEngineARTSIMD() {
}

std::vector<Trade> MatchingEngineARTSIMD::process_order_art_simd(Order* order) {
    if (!order || order->remaining_quantity <= 0) {
        return {};
    }
    
    // Match order using SIMD-optimized ART
    std::vector<Trade> trades = match_order_art_simd(order);
    
    // If order not fully filled, add to order book
    if (order->remaining_quantity > 0 && order->order_type == OrderType::LIMIT) {
        orderbook_art_simd_.insert_order(order);
    }
    
    return trades;
}

std::vector<Trade> MatchingEngineARTSIMD::match_order_art_simd(Order* order) {
    std::vector<Trade> trades;
    
    if (order->order_type == OrderType::FOK &&
        orderbook_art_simd_.crossable_quantity(order) < order->remaining_quantity) {
        order->status = OrderStatus::CANCELLED;
        return trades;
    }
    
    if (order->side == OrderSide::BUY) {
        // Match against asks using SIMD-optimized lookup
        OrderBookSideARTSIMD& asks = orderbook_art_simd_.asks();
        
        // Add safety counter to prevent infinite loops
        const size_t max_iterations = 10000;
        size_t iteration_count = 0;
        
        while (order->remaining_quantity > 0 && !asks.empty() && iteration_count < max_iterations) {
            ++iteration_count;
            
            // Use SIMD-optimized best price lookup
            Price best_ask = asks.best_price();
            
            // Use SIMD-optimized price comparison
            if (best_ask == 0 || !ARTTreeSIMDEnhanced::can_match_price(order->price, best_ask, true)) {
                break;  // Cannot match
            }
            
            PriceLevel* level = asks.best_level();
            if (!level || level->first_order == nullptr) {
                break;
            }
            
            Order* maker = level->first_order;
            Price trade_price = maker->price;  // Price-time priority
            Quantity trade_qty = std::min(order->remaining_quantity, maker->remaining_quantity);
            
            // Execute trade
            order->remaining_quantity -= trade_qty;
            order->filled_quantity += trade_qty;
            maker->remaining_quantity -= trade_qty;
            maker->filled_quantity += trade_qty;
            
            if (order->remaining_quantity == 0) {
                order->status = OrderStatus::FILLED;
            } else {
                order->status = OrderStatus::PARTIAL_FILLED;
            }
            
            if (maker->remaining_quantity == 0) {
                maker->status = OrderStatus::FILLED;
            } else {
                maker->status = OrderStatus::PARTIAL_FILLED;
            }

            orderbook_art_simd_.apply_trade_to_price_level(maker, trade_qty);
            
            Trade trade;
            trade.buy_order_id = order->order_id;
            trade.sell_order_id = maker->order_id;
            trade.buy_user_id = order->user_id;
            trade.sell_user_id = maker->user_id;
            trade.instrument_id = order->instrument_id;
            trade.price = trade_price;
            trade.quantity = trade_qty;
            trade.timestamp = get_current_timestamp();
            trade.sequence_id = get_current_timestamp();  // Use timestamp as sequence
            trade.is_taker_buy = true;
            trades.push_back(trade);
            
            // Remove maker if fully filled
            if (maker->remaining_quantity == 0) {
                orderbook_art_simd_.remove_order(maker);
            }
        }
    } else {
        // Match against bids using SIMD-optimized lookup
        OrderBookSideARTSIMD& bids = orderbook_art_simd_.bids();
        
        // Add safety counter to prevent infinite loops
        const size_t max_iterations = 10000;
        size_t iteration_count = 0;
        
        while (order->remaining_quantity > 0 && !bids.empty() && iteration_count < max_iterations) {
            ++iteration_count;
            
            // Use SIMD-optimized best price lookup
            Price best_bid = bids.best_price();
            
            // Use SIMD-optimized price comparison
            if (best_bid == 0 || !ARTTreeSIMDEnhanced::can_match_price(order->price, best_bid, false)) {
                break;  // Cannot match
            }
            
            PriceLevel* level = bids.best_level();
            if (!level || level->first_order == nullptr) {
                break;
            }
            
            Order* maker = level->first_order;
            Price trade_price = maker->price;  // Price-time priority
            Quantity trade_qty = std::min(order->remaining_quantity, maker->remaining_quantity);
            
            // Execute trade
            order->remaining_quantity -= trade_qty;
            order->filled_quantity += trade_qty;
            maker->remaining_quantity -= trade_qty;
            maker->filled_quantity += trade_qty;
            
            if (order->remaining_quantity == 0) {
                order->status = OrderStatus::FILLED;
            } else {
                order->status = OrderStatus::PARTIAL_FILLED;
            }
            
            if (maker->remaining_quantity == 0) {
                maker->status = OrderStatus::FILLED;
            } else {
                maker->status = OrderStatus::PARTIAL_FILLED;
            }

            orderbook_art_simd_.apply_trade_to_price_level(maker, trade_qty);
            
            Trade trade;
            trade.buy_order_id = maker->order_id;
            trade.sell_order_id = order->order_id;
            trade.buy_user_id = maker->user_id;
            trade.sell_user_id = order->user_id;
            trade.instrument_id = order->instrument_id;
            trade.price = trade_price;
            trade.quantity = trade_qty;
            trade.timestamp = get_current_timestamp();
            trade.sequence_id = get_current_timestamp();  // Use timestamp as sequence
            trade.is_taker_buy = false;
            trades.push_back(trade);
            
            // Remove maker if fully filled
            if (maker->remaining_quantity == 0) {
                orderbook_art_simd_.remove_order(maker);
            }
        }
    }

    if ((order->order_type == OrderType::IOC || order->order_type == OrderType::FOK) &&
        order->remaining_quantity > 0) {
        order->status = OrderStatus::CANCELLED;
    }
    
    return trades;
}

} // namespace perpetual

