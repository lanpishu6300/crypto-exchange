#include "core/orderbook.h"
#include <algorithm>
#include <cassert>
#include <mutex>

namespace perpetual {

// OrderBookSide implementation
OrderBookSide::OrderBookSide(bool is_buy) 
    : is_buy_(is_buy), root_(nullptr), sentinel_(nullptr) {
    sentinel_ = new Order();  // Sentinel node (nil)
    sentinel_->color = 0;  // Black
    sentinel_->left = sentinel_->right = sentinel_;
    sentinel_->parent = nullptr;
    root_ = sentinel_;
}

OrderBookSide::~OrderBookSide() {
    // Clean up all orders (they should be managed elsewhere)
    delete sentinel_;
}

bool OrderBookSide::price_better(Price a, Price b) const {
    if (is_buy_) {
        return a > b;  // For bids, higher is better
    } else {
        return a < b;  // For asks, lower is better
    }
}

bool OrderBookSide::price_equal_or_better(Price a, Price b) const {
    return a == b || price_better(a, b);
}

bool OrderBookSide::insert(Order* order) {
    if (!order || order->price <= 0 || order->quantity <= 0) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Initialize order tree pointers
    order->left = sentinel_;
    order->right = sentinel_;
    order->parent = nullptr;
    order->color = 1;  // Red
    
    // Insert into red-black tree
    Order* y = nullptr;
    Order* x = root_;
    
    while (x != sentinel_) {
        y = x;
        if (price_better(order->price, x->price) || 
            (order->price == x->price && order->timestamp < x->timestamp)) {
            x = x->left;
        } else {
            x = x->right;
        }
    }
    
    order->parent = y;
    if (y == nullptr) {
        root_ = order;
    } else if (price_better(order->price, y->price) ||
               (order->price == y->price && order->timestamp < y->timestamp)) {
        y->left = order;
    } else {
        y->right = order;
    }
    
    // Add to price level
    PriceLevel* level = get_or_create_price_level(order->price);
    add_order_to_price_level(level, order);
    
    // Fix red-black tree properties
    fix_insert(order);
    
    // Add to order map
    order_map_[order->order_id] = order;
    
    return true;
}

bool OrderBookSide::remove(Order* order) {
    if (!order || order_map_.find(order->order_id) == order_map_.end()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    // Remove from price level
    PriceLevel* level = get_or_create_price_level(order->price);
    remove_order_from_price_level(level, order);
    remove_price_level_if_empty(order->price);
    
    // Remove from red-black tree
    Order* z = order;
    Order* y = z;
    Order* x;
    
    uint8_t y_original_color = y->color;
    
    if (z->left == sentinel_) {
        x = z->right;
        if (z->parent == nullptr) {
            root_ = z->right;
        } else if (z == z->parent->left) {
            z->parent->left = z->right;
        } else {
            z->parent->right = z->right;
        }
        z->right->parent = z->parent;
    } else if (z->right == sentinel_) {
        x = z->left;
        if (z->parent == nullptr) {
            root_ = z->left;
        } else if (z == z->parent->left) {
            z->parent->left = z->left;
        } else {
            z->parent->right = z->left;
        }
        z->left->parent = z->parent;
    } else {
        y = find_min(z->right);
        y_original_color = y->color;
        x = y->right;
        if (y->parent == z) {
            x->parent = y;
        } else {
            if (y->parent->left == y) {
                y->parent->left = x;
            } else {
                y->parent->right = x;
            }
            x->parent = y->parent;
            y->right = z->right;
            if (y->right != sentinel_) {
                y->right->parent = y;
            }
        }
        if (z->parent == nullptr) {
            root_ = y;
        } else if (z == z->parent->left) {
            z->parent->left = y;
        } else {
            z->parent->right = y;
        }
        y->parent = z->parent;
        y->left = z->left;
        if (y->left != sentinel_) {
            y->left->parent = y;
        }
        y->color = z->color;
    }
    
    if (y_original_color == 0) {
        fix_delete(x);
    }
    
    // Remove from order map
    order_map_.erase(order->order_id);
    
    return true;
}

bool OrderBookSide::empty() const {
    return root_ == nullptr || root_ == sentinel_ || order_map_.empty();
}

Price OrderBookSide::best_price() const {
    // Note: This method assumes external synchronization
    // In production, consider using reader-writer lock or lock-free approach
    if (root_ == nullptr || root_ == sentinel_) {
        return 0;
    }
    
    Order* best = root_;
    // Best price is leftmost (smallest) in the tree
    // For bids: we want highest price (but tree is ordered by price-time)
    // For asks: we want lowest price
    // Both are achieved by finding leftmost node
    while (best->left != sentinel_) {
        best = best->left;
    }
    return best->price;
}

Quantity OrderBookSide::best_quantity() const {
    // Note: This method assumes external synchronization
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

Order* OrderBookSide::best_order() const {
    // Note: This method assumes external synchronization
    if (root_ == nullptr || root_ == sentinel_) {
        return nullptr;
    }
    
    // Optimized: traverse to leftmost node with prefetching
    // Add safety counter to prevent infinite loops
    Order* best = root_;
    const size_t max_depth = 1000;  // Safety limit for tree depth
    size_t depth = 0;
    
    while (best != nullptr && best->left != sentinel_ && depth < max_depth) {
        // Prefetch next node for better cache performance
        __builtin_prefetch(best->left, 0, 3);
        best = best->left;
        ++depth;
    }
    
    // Safety check: if we hit max depth, return nullptr
    if (depth >= max_depth) {
        return nullptr;
    }
    
    return best;
}

PriceLevel* OrderBookSide::best_level() const {
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

Order* OrderBookSide::find_order(OrderID order_id) const {
    auto it = order_map_.find(order_id);
    if (it != order_map_.end()) {
        return it->second;
    }
    return nullptr;
}

void OrderBookSide::get_depth(size_t n, std::vector<PriceLevel>& levels) const {
    levels.clear();
    levels.reserve(n);
    
    // Traverse price levels map (already sorted by price)
    // For bids: we want highest prices first (reverse iteration)
    // For asks: we want lowest prices first (forward iteration)
    if (is_buy_) {
        // Bids: iterate in reverse (highest first)
        auto it = price_levels_.rbegin();
        for (; it != price_levels_.rend() && levels.size() < n; ++it) {
            if (it->second.total_quantity > 0) {
                levels.push_back(it->second);
            }
        }
    } else {
        // Asks: iterate forward (lowest first)
        for (const auto& [price, level] : price_levels_) {
            if (levels.size() >= n) break;
            if (level.total_quantity > 0) {
                levels.push_back(level);
            }
        }
    }
}

void OrderBookSide::apply_trade_to_price_level(Order* order, Quantity traded_qty) {
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

Quantity OrderBookSide::crossable_quantity(Price limit_price, bool taker_is_buy) const {
    Quantity total = 0;
    if (is_buy_ == taker_is_buy) {
        return 0;
    }
    if (taker_is_buy) {
        for (const auto& [price, level] : price_levels_) {
            if (level.total_quantity == 0) {
                continue;
            }
            if (limit_price > 0 && price > limit_price) {
                break;
            }
            total += level.total_quantity;
        }
    } else {
        for (auto it = price_levels_.rbegin(); it != price_levels_.rend(); ++it) {
            if (it->second.total_quantity == 0) {
                continue;
            }
            if (limit_price > 0 && it->first < limit_price) {
                break;
            }
            total += it->second.total_quantity;
        }
    }
    return total;
}

void OrderBookSide::rotate_left(Order* x) {
    Order* y = x->right;
    x->right = y->left;
    if (y->left != sentinel_) {
        y->left->parent = x;
    }
    y->parent = x->parent;
    if (x->parent == nullptr) {
        root_ = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    y->left = x;
    x->parent = y;
}

void OrderBookSide::rotate_right(Order* x) {
    Order* y = x->left;
    x->left = y->right;
    if (y->right != sentinel_) {
        y->right->parent = x;
    }
    y->parent = x->parent;
    if (x->parent == nullptr) {
        root_ = y;
    } else if (x == x->parent->right) {
        x->parent->right = y;
    } else {
        x->parent->left = y;
    }
    y->right = x;
    x->parent = y;
}

void OrderBookSide::fix_insert(Order* z) {
    while (z->parent != nullptr && z->parent->color == 1) {
        if (z->parent == z->parent->parent->left) {
            Order* y = z->parent->parent->right;
            if (y != sentinel_ && y->color == 1) {
                z->parent->color = 0;
                y->color = 0;
                z->parent->parent->color = 1;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    rotate_left(z);
                }
                z->parent->color = 0;
                z->parent->parent->color = 1;
                rotate_right(z->parent->parent);
            }
        } else {
            Order* y = z->parent->parent->left;
            if (y != sentinel_ && y->color == 1) {
                z->parent->color = 0;
                y->color = 0;
                z->parent->parent->color = 1;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    rotate_right(z);
                }
                z->parent->color = 0;
                z->parent->parent->color = 1;
                rotate_left(z->parent->parent);
            }
        }
    }
    root_->color = 0;
}

void OrderBookSide::fix_delete(Order* x) {
    // Red-black tree delete fixup
    // Simplified implementation
    while (x != root_ && x->color == 0) {
        if (x == x->parent->left) {
            Order* w = x->parent->right;
            if (w->color == 1) {
                w->color = 0;
                x->parent->color = 1;
                rotate_left(x->parent);
                w = x->parent->right;
            }
            if (w->left->color == 0 && w->right->color == 0) {
                w->color = 1;
                x = x->parent;
            } else {
                if (w->right->color == 0) {
                    w->left->color = 0;
                    w->color = 1;
                    rotate_right(w);
                    w = x->parent->right;
                }
                w->color = x->parent->color;
                x->parent->color = 0;
                w->right->color = 0;
                rotate_left(x->parent);
                x = root_;
            }
        } else {
            Order* w = x->parent->left;
            if (w->color == 1) {
                w->color = 0;
                x->parent->color = 1;
                rotate_right(x->parent);
                w = x->parent->left;
            }
            if (w->right->color == 0 && w->left->color == 0) {
                w->color = 1;
                x = x->parent;
            } else {
                if (w->left->color == 0) {
                    w->right->color = 0;
                    w->color = 1;
                    rotate_left(w);
                    w = x->parent->left;
                }
                w->color = x->parent->color;
                x->parent->color = 0;
                w->left->color = 0;
                rotate_right(x->parent);
                x = root_;
            }
        }
    }
    x->color = 0;
}

Order* OrderBookSide::find_min(Order* node) const {
    while (node->left != sentinel_) {
        node = node->left;
    }
    return node;
}

Order* OrderBookSide::find_max(Order* node) const {
    while (node->right != sentinel_) {
        node = node->right;
    }
    return node;
}

PriceLevel* OrderBookSide::get_or_create_price_level(Price price) {
    auto& level = price_levels_[price];
    level.price = price;
    return &level;
}

void OrderBookSide::remove_price_level_if_empty(Price price) {
    auto it = price_levels_.find(price);
    if (it != price_levels_.end() && it->second.total_quantity == 0) {
        price_levels_.erase(it);
    }
}

void OrderBookSide::add_order_to_price_level(PriceLevel* level, Order* order) {
    if (!level || !order) return;
    
    if (level->first_order == nullptr) {
        level->first_order = order;
        level->last_order = order;
        order->prev_same_price = nullptr;
        order->next_same_price = nullptr;
    } else {
        order->prev_same_price = level->last_order;
        order->next_same_price = nullptr;
        level->last_order->next_same_price = order;
        level->last_order = order;
    }
    level->total_quantity += order->remaining_quantity;
}

void OrderBookSide::remove_order_from_price_level(PriceLevel* level, Order* order) {
    if (!level || !order) return;
    
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
}

bool OrderBookSide::update_quantity(Order* order, Quantity new_quantity) {
    if (!order || order_map_.find(order->order_id) == order_map_.end()) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    Quantity old_quantity = order->remaining_quantity;
    order->remaining_quantity = new_quantity;
    
    // Update price level
    auto it = price_levels_.find(order->price);
    if (it != price_levels_.end()) {
        PriceLevel* level = &it->second;
        level->total_quantity = level->total_quantity - old_quantity + new_quantity;
        if (level->total_quantity < 0) {
            level->total_quantity = 0;
        }
    }
    
    return true;
}

// OrderBook implementation
OrderBook::OrderBook(InstrumentID instrument_id)
    : instrument_id_(instrument_id), bids_(true), asks_(false) {
}

OrderBook::~OrderBook() {
}

bool OrderBook::insert_order(Order* order) {
    if (!order) return false;
    
    if (order->is_buy()) {
        return bids_.insert(order);
    } else {
        return asks_.insert(order);
    }
}

bool OrderBook::remove_order(Order* order) {
    if (!order) return false;
    
    if (order->is_buy()) {
        return bids_.remove(order);
    } else {
        return asks_.remove(order);
    }
}

Price OrderBook::spread() const {
    Price bid = best_bid();
    Price ask = best_ask();
    if (bid == 0 || ask == 0) {
        return 0;
    }
    return ask > bid ? ask - bid : 0;
}

Price OrderBook::mid_price() const {
    Price bid = best_bid();
    Price ask = best_ask();
    if (bid == 0 || ask == 0) {
        return 0;
    }
    return (bid + ask) / 2;
}

bool OrderBook::can_match(Order* order) const {
    if (!order) return false;
    
    if (order->is_buy()) {
        Price ask = best_ask();
        return ask > 0 && (order->order_type == OrderType::MARKET || order->price >= ask);
    } else {
        Price bid = best_bid();
        return bid > 0 && (order->order_type == OrderType::MARKET || order->price <= bid);
    }
}

void OrderBook::get_depth(size_t n, std::vector<PriceLevel>& bids, 
                          std::vector<PriceLevel>& asks) const {
    bids_.get_depth(n, bids);
    asks_.get_depth(n, asks);
}

void OrderBook::apply_trade_to_price_level(Order* order, Quantity traded_qty) {
    if (!order) {
        return;
    }
    if (order->is_buy()) {
        bids_.apply_trade_to_price_level(order, traded_qty);
    } else {
        asks_.apply_trade_to_price_level(order, traded_qty);
    }
}

Quantity OrderBook::crossable_quantity(Order* taker) const {
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
