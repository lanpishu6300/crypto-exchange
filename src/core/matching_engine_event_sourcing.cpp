#include "core/matching_engine_event_sourcing.h"
#include "core/matching_engine.h"
#include "core/deterministic_calculator.h"
#include <algorithm>

namespace perpetual {

MatchingEngineEventSourcing::MatchingEngineEventSourcing(InstrumentID instrument_id,
                                                          EventStore* event_store)
    : MatchingEngine(instrument_id), owns_event_store_(false) {
    if (event_store) {
        event_store_ = std::unique_ptr<EventStore>(event_store);
        event_publisher_ = std::make_unique<EventPublisher>(event_store);
    }
}

MatchingEngineEventSourcing::~MatchingEngineEventSourcing() {
    if (event_publisher_) {
        event_publisher_->flush();
    }
    if (event_store_ && owns_event_store_) {
        event_store_->flush();
    }
}

bool MatchingEngineEventSourcing::initialize(const std::string& event_store_dir) {
    if (!event_store_) {
        event_store_ = std::make_unique<EventStore>();
        owns_event_store_ = true;
    }
    
    if (!event_store_->initialize(event_store_dir)) {
        return false;
    }
    
    event_publisher_ = std::make_unique<EventPublisher>(event_store_.get());
    return true;
}

std::vector<Trade> MatchingEngineEventSourcing::process_order_es(Order* order) {
    if (!order) {
        return {};
    }
    
    // Validate order (call base class process_order first to validate)
    // We'll use a workaround: call process_order but intercept events
    // For now, do basic validation
    if (!order || order->price <= 0 || order->quantity <= 0) {
        if (event_publisher_) {
            emit_order_rejected_event(order->order_id, order->user_id, "Validation failed");
        }
        order->status = OrderStatus::REJECTED;
        return {};
    }
    
    // Use deterministic timestamp if enabled
    if (deterministic_mode_ && event_store_) {
        SequenceID seq = event_store_->get_latest_sequence() + 1;
        order->sequence_id = seq;
        order->timestamp = DeterministicCalculator::sequence_to_timestamp(seq);
    } else {
        order->timestamp = get_current_timestamp();
    }
    
    // Emit order placed event
    if (event_publisher_) {
        emit_order_placed_event(*order);
    }
    
    // Match the order using deterministic calculations
    std::vector<Trade> trades = match_order_deterministic(order);
    
    // If order is not fully filled and is a limit order, add to book
    if (order->is_active() && order->order_type == OrderType::LIMIT) {
        if (order->remaining_quantity > 0) {
            // Use base class process_order to handle order book insertion
            // But we've already matched, so just insert if not filled
            if (orderbook_.insert_order(order)) {
                // Order ownership is managed by base class
                if (order_update_callback_) {
                    order_update_callback_(order);
                }
            }
        }
    }
    
    return trades;
}

bool MatchingEngineEventSourcing::cancel_order_es(OrderID order_id, UserID user_id) {
    Order* order = get_order(order_id);
    if (!order || order->user_id != user_id) {
        return false;
    }
    
    OrderStatus old_status = order->status;
    order->status = OrderStatus::CANCELLED;
    
    // Remove from order book
    orderbook_.remove_order(order);
    
    // Emit cancellation event
    if (event_publisher_) {
        emit_order_cancelled_event(order_id, user_id, old_status, OrderStatus::CANCELLED);
    }
    
    if (order_update_callback_) {
        order_update_callback_(order);
    }
    
    return true;
}

bool MatchingEngineEventSourcing::replay_events(SequenceID from, SequenceID to) {
    if (!event_store_) {
        return false;
    }
    
    // Clear current state by resetting orderbook
    // Note: This is a simplified replay - in production, we'd properly reset all state
    // OrderBook doesn't support copy assignment, so we need to clear orders manually
    // For now, we'll just replay events which will rebuild the state
    
    // Replay events
    return event_store_->replay_events(from, to, [this](const Event& event) -> bool {
        switch (event.type) {
            case EventType::ORDER_PLACED: {
                // Reconstruct order from event
                auto order = std::make_unique<Order>(
                    event.data.order_placed.order_id,
                    event.data.order_placed.user_id,
                    event.instrument_id,
                    event.data.order_placed.side,
                    event.data.order_placed.price,
                    event.data.order_placed.quantity,
                    event.data.order_placed.order_type
                );
                order->sequence_id = event.sequence_id;
                order->timestamp = event.event_timestamp;
                
                // Process order (this will match and emit events, but we're replaying)
                // For replay, we should skip event emission
                process_order_es(order.release());
                break;
            }
            case EventType::ORDER_CANCELLED: {
                cancel_order_es(event.data.order_cancelled.order_id,
                               event.data.order_cancelled.user_id);
                break;
            }
            default:
                // Other events are side effects of order processing
                break;
        }
        return true;
    });
}

std::vector<Trade> MatchingEngineEventSourcing::match_order_deterministic(Order* order) {
    std::vector<Trade> trades;
    
    if (!order || order->remaining_quantity == 0) {
        return trades;
    }
    
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
    
    const size_t max_iterations = 10000;
    size_t iteration_count = 0;
    
    while (order->remaining_quantity > 0 && !opposite_side->empty() && iteration_count < max_iterations) {
        ++iteration_count;
        
        Order* resting_order = opposite_side->best_order();
        
        if (!resting_order || !can_match_deterministic(order, resting_order)) {
            break;
        }
        
        // Use deterministic calculations
        Price match_price = get_match_price_deterministic(order, resting_order);
        Quantity trade_qty = get_trade_quantity_deterministic(order, resting_order);
        
        // Execute trade (update orders manually since we can't access private methods)
        Quantity taker_filled = (order->remaining_quantity < trade_qty) ? order->remaining_quantity : trade_qty;
        Quantity maker_filled = (resting_order->remaining_quantity < trade_qty) ? resting_order->remaining_quantity : trade_qty;
        Quantity actual_qty = (taker_filled < maker_filled) ? taker_filled : maker_filled;
        
        order->filled_quantity += actual_qty;
        order->remaining_quantity -= actual_qty;
        resting_order->filled_quantity += actual_qty;
        resting_order->remaining_quantity -= actual_qty;

        orderbook_.apply_trade_to_price_level(resting_order, actual_qty);
        
        // Create trade record
        Trade trade;
        if (order->is_buy()) {
            trade.buy_order_id = order->order_id;
            trade.sell_order_id = resting_order->order_id;
            trade.buy_user_id = order->user_id;
            trade.sell_user_id = resting_order->user_id;
            trade.is_taker_buy = true;
        } else {
            trade.buy_order_id = resting_order->order_id;
            trade.sell_order_id = order->order_id;
            trade.buy_user_id = resting_order->user_id;
            trade.sell_user_id = order->user_id;
            trade.is_taker_buy = false;
        }
        trade.instrument_id = order->instrument_id;
        trade.price = match_price;
        trade.quantity = actual_qty;
        trade.timestamp = deterministic_mode_ ? 
            DeterministicCalculator::sequence_to_timestamp(event_store_->get_latest_sequence() + 1) :
            get_current_timestamp();
        trade.sequence_id = (event_store_ && deterministic_mode_) ? 
            event_store_->get_latest_sequence() + 1 : 
            get_current_timestamp();
        trades.push_back(trade);
        
        // Emit events
        if (event_publisher_) {
            emit_order_matched_event(order->order_id, resting_order->order_id,
                                    match_price, trade_qty);
            emit_trade_executed_event(trade);
        }
        
        // Update statistics
        total_trades_++;
        total_volume_ += trade_qty;
        
        // Call trade callback
        if (trade_callback_) {
            trade_callback_(trade);
        }
        
        // Orders already updated above
        
        // Remove filled order from book
        if (resting_order->is_filled()) {
            orderbook_.remove_order(resting_order);
            if (order_update_callback_) {
                order_update_callback_(resting_order);
            }
            if (opposite_side->empty()) {
                break;
            }
        }
    }
    
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
    
    return trades;
}

bool MatchingEngineEventSourcing::can_match_deterministic(const Order* taker, const Order* maker) const {
    return DeterministicCalculator::can_match(taker->price, maker->price, taker->is_buy());
}

Price MatchingEngineEventSourcing::get_match_price_deterministic(const Order* taker, const Order* maker) const {
    return DeterministicCalculator::calculate_match_price(taker->price, maker->price);
}

Quantity MatchingEngineEventSourcing::get_trade_quantity_deterministic(const Order* taker, const Order* maker) const {
    return DeterministicCalculator::calculate_trade_quantity(taker->remaining_quantity, maker->remaining_quantity);
}

void MatchingEngineEventSourcing::emit_order_placed_event(const Order& order) {
    if (event_publisher_) {
        event_publisher_->publish_order_placed(order);
    }
}

void MatchingEngineEventSourcing::emit_order_matched_event(OrderID taker_id, OrderID maker_id,
                                                            Price price, Quantity quantity) {
    if (event_publisher_) {
        event_publisher_->publish_order_matched(taker_id, maker_id, price, quantity);
    }
}

void MatchingEngineEventSourcing::emit_order_cancelled_event(OrderID order_id, UserID user_id,
                                                             OrderStatus old_status, OrderStatus new_status) {
    if (event_publisher_) {
        event_publisher_->publish_order_cancelled(order_id, user_id, old_status, new_status);
    }
}

void MatchingEngineEventSourcing::emit_trade_executed_event(const Trade& trade) {
    if (event_publisher_) {
        event_publisher_->publish_trade_executed(trade);
    }
}

void MatchingEngineEventSourcing::emit_order_rejected_event(OrderID order_id, UserID user_id,
                                                             const std::string& reason) {
    if (event_publisher_) {
        event_publisher_->publish_order_rejected(order_id, user_id, reason);
    }
}

} // namespace perpetual

