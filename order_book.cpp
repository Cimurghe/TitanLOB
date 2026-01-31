#include "order_book.h"
#include <algorithm>
#include <iostream>

void OptimizedOrderBook::add_order_internal(uint64_t order_id, bool is_buy, int64_t price, 
                                            int64_t quantity, uint32_t user_id) {
    
    size_t price_idx = price_to_index(price);
    if (price_idx >= MAX_PRICE_LEVELS) [[unlikely]] return;
    
    auto& levels = is_buy ? bid_levels_ : ask_levels_;
    PriceLevel& level = levels[price_idx];
    bool was_empty = level.empty();
    
    uint32_t idx = order_pool_.allocate();
    Order& order = order_pool_[idx];
    
    order.order_id = order_id;
    order.user_id_low = user_id;
    order.price = price;
    order.quantity = quantity;
    order.hidden_quantity = 0;
    order.peak_size = 0;
    order.flags = 0;
    order.set_buy(is_buy);
    order.set_aon(false);
    order.next = NULL_INDEX;
    order.prev = NULL_INDEX;
    
    list_push_back(level, idx);
    add_to_level_volume(level, order);
    
    if (was_empty) {
        if (is_buy) {
            bid_level_count_++;
            update_best_bid_after_add(price);
        } else {
            ask_level_count_++;
            update_best_ask_after_add(price);
        }
    }
    
    ensure_capacity(order_id);
    order_index_[order_id].price = price;
    order_index_[order_id].pool_idx = idx;
    order_index_[order_id].flags = 0;
    order_index_[order_id].set_buy(is_buy);
    order_index_[order_id].set_active(true);
    active_order_count_++;
    
    emit_order_accepted(order_id, bool_to_side(is_buy), price, quantity);

    if (best_bid_ >= 0 && best_ask_ != INT64_MAX && best_bid_ >= best_ask_) {
        std::cerr << "[CRITICAL] CROSS DETECTED! Bid: " << best_bid_ 
                  << " Ask: " << best_ask_ << std::endl;
    }
}

void OptimizedOrderBook::add_iceberg_internal(uint64_t order_id, bool is_buy, int64_t price,
                                              int64_t total_quantity, int64_t visible_quantity,
                                              uint32_t user_id) {
    size_t price_idx = price_to_index(price);
    if (price_idx >= MAX_PRICE_LEVELS) [[unlikely]] return;
    
    auto& levels = is_buy ? bid_levels_ : ask_levels_;
    PriceLevel& level = levels[price_idx];
    bool was_empty = level.empty();
    
    int64_t display_qty = std::min(visible_quantity, total_quantity);
    int64_t hidden_qty = total_quantity - display_qty;
    
    uint32_t idx = order_pool_.allocate();
    Order& order = order_pool_[idx];
    
    order.order_id = order_id;
    order.user_id_low = user_id;
    order.price = price;
    order.quantity = display_qty;
    order.hidden_quantity = hidden_qty;
    order.peak_size = visible_quantity;
    order.flags = 0;
    order.set_buy(is_buy);
    order.set_aon(false);
    order.next = NULL_INDEX;
    order.prev = NULL_INDEX;
    
    list_push_back(level, idx);
    add_to_level_volume(level, order);
    
    if (was_empty) {
        if (is_buy) {
            bid_level_count_++;
            update_best_bid_after_add(price);
        } else {
            ask_level_count_++;
            update_best_ask_after_add(price);
        }
    }
    
    ensure_capacity(order_id);
    order_index_[order_id].price = price;
    order_index_[order_id].pool_idx = idx;
    order_index_[order_id].flags = 0;
    order_index_[order_id].set_buy(is_buy);
    order_index_[order_id].set_active(true);
    active_order_count_++;
    
    emit_order_accepted(order_id, bool_to_side(is_buy), price, display_qty);
}

void OptimizedOrderBook::add_aon_internal(uint64_t order_id, bool is_buy, int64_t price,
                                          int64_t quantity, uint32_t user_id) {
    size_t price_idx = price_to_index(price);
    if (price_idx >= MAX_PRICE_LEVELS) [[unlikely]] return;
    
    auto& levels = is_buy ? bid_levels_ : ask_levels_;
    PriceLevel& level = levels[price_idx];
    bool was_empty = level.empty();
    
    uint32_t idx = order_pool_.allocate();
    Order& order = order_pool_[idx];
    
    order.order_id = order_id;
    order.user_id_low = user_id;
    order.price = price;
    order.quantity = quantity;
    order.hidden_quantity = 0;
    order.peak_size = 0;
    order.flags = 0;
    order.set_buy(is_buy);
    order.set_aon(true);
    order.next = NULL_INDEX;
    order.prev = NULL_INDEX;
    
    list_push_back(level, idx);
    add_to_level_volume(level, order);
    
    if (was_empty) {
        if (is_buy) {
            bid_level_count_++;
            update_best_bid_after_add(price);
        } else {
            ask_level_count_++;
            update_best_ask_after_add(price);
        }
    }
    
    ensure_capacity(order_id);
    order_index_[order_id].price = price;
    order_index_[order_id].pool_idx = idx;
    order_index_[order_id].flags = 0;
    order_index_[order_id].set_buy(is_buy);
    order_index_[order_id].set_active(true);
    active_order_count_++;
    
    emit_order_accepted(order_id, bool_to_side(is_buy), price, quantity);
}

void OptimizedOrderBook::cancel_order_internal(uint64_t order_id) {
    if (order_id >= order_index_.size() || !order_index_[order_id].is_active()) [[unlikely]] {
        return;
    }
    
    OrderLocation& loc = order_index_[order_id];
    size_t price_idx = price_to_index(loc.price);
    if (price_idx >= MAX_PRICE_LEVELS) [[unlikely]] return;
    
    auto& levels = loc.is_buy() ? bid_levels_ : ask_levels_;
    PriceLevel& level = levels[price_idx];
    
    Order& order = order_pool_[loc.pool_idx];
    int64_t cancelled_qty = order.quantity + order.hidden_quantity;
    
    remove_from_level_volume(level, order);
    list_remove(level, loc.pool_idx);
    order_pool_.free(loc.pool_idx);
    
    if (level.empty()) {
        if (loc.is_buy()) {
            bid_level_count_--;
            update_best_bid_after_remove(loc.price);
        } else {
            ask_level_count_--;
            update_best_ask_after_remove(loc.price);
        }
    }
    
    loc.set_active(false);
    active_order_count_--;
    
    emit_order_cancelled(order_id, cancelled_qty);
}

void OptimizedOrderBook::modify_order_internal(uint64_t order_id, int64_t new_price, int64_t new_quantity) {
    if (order_id >= order_index_.size() || !order_index_[order_id].is_active()) [[unlikely]] {
        return;
    }
    
    OrderLocation& loc = order_index_[order_id];
    size_t price_idx = price_to_index(loc.price);
    if (price_idx >= MAX_PRICE_LEVELS) [[unlikely]] return;
    
    auto& levels = loc.is_buy() ? bid_levels_ : ask_levels_;
    PriceLevel& level = levels[price_idx];
    Order& order = order_pool_[loc.pool_idx];
    
    if (new_price == loc.price && new_quantity <= order.quantity) {
        int64_t delta = new_quantity - order.quantity;
        adjust_level_volume(level, delta, 0, order.is_aon());
        order.quantity = new_quantity;
    } else {
        bool is_buy = loc.is_buy();
        cancel_order_internal(order_id);
        add_order_internal(order_id, is_buy, new_price, new_quantity, 0);
    }
}

int64_t OptimizedOrderBook::calculate_available_quantity(bool is_buy, int64_t limit_price, 
                                                         int64_t incoming_qty) const {
    const auto& levels = is_buy ? ask_levels_ : bid_levels_;
    int64_t best = is_buy ? best_ask_ : best_bid_;

    if (is_buy && best == INT64_MAX) return 0;
    if (!is_buy && best < 0) return 0;
    
    int64_t available = 0;
    int64_t remaining = incoming_qty;
    
    if (is_buy) {
        for (int64_t p = best; p <= limit_price && remaining > 0; ++p) {
            size_t idx = price_to_index(p);
            if (idx >= MAX_PRICE_LEVELS) break;
            
            const PriceLevel& level = levels[idx];
            if (level.empty()) continue;
            
            if (level.total_aon_volume == 0) {
                int64_t fillable = std::min(remaining, level.total_volume);
                available += fillable;
                remaining -= fillable;
            } else {
                uint32_t curr = level.head;
                while (curr != NULL_INDEX && remaining > 0) {
                    const Order& order = order_pool_[curr];
                    int64_t order_total = order.quantity + order.hidden_quantity;
                    
                    if (order.is_aon()) {
                        if (remaining >= order_total) {
                            available += order_total;
                            remaining -= order_total;
                        }
                    } else {
                        int64_t fillable = std::min(remaining, order_total);
                        available += fillable;
                        remaining -= fillable;
                    }
                    curr = order.next;
                }
            }
        }
    } else {
        for (int64_t p = best; p >= limit_price && remaining > 0; --p) {
            size_t idx = price_to_index(p);
            if (idx >= MAX_PRICE_LEVELS) continue;
            
            const PriceLevel& level = levels[idx];
            if (level.empty()) continue;
            
            if (level.total_aon_volume == 0) {
                int64_t fillable = std::min(remaining, level.total_volume);
                available += fillable;
                remaining -= fillable;
            } else {
                uint32_t curr = level.head;
                while (curr != NULL_INDEX && remaining > 0) {
                    const Order& order = order_pool_[curr];
                    int64_t order_total = order.quantity + order.hidden_quantity;
                    
                    if (order.is_aon()) {
                        if (remaining >= order_total) {
                            available += order_total;
                            remaining -= order_total;
                        }
                    } else {
                        int64_t fillable = std::min(remaining, order_total);
                        available += fillable;
                        remaining -= fillable;
                    }
                    curr = order.next;
                }
            }
        }
    }
    
    return available;
}

size_t OptimizedOrderBook::match_internal(uint64_t order_id, bool is_buy, int64_t price, 
                                          int64_t quantity, TimeInForce tif) {
    auto& levels = is_buy ? ask_levels_ : bid_levels_;
    int64_t& best_price = is_buy ? best_ask_ : best_bid_;

    bool opposite_side_empty = is_buy ? (best_price == INT64_MAX) : (best_price < 0);

    if (tif == TimeInForce::FOK) {
        int64_t available = calculate_available_quantity(is_buy, price, quantity);
        if (available < quantity) {
            return 0;
        }
    }

    if (tif == TimeInForce::AON) {
        int64_t available = calculate_available_quantity(is_buy, price, quantity);
        if (available < quantity) {
            add_aon_internal(order_id, is_buy, price, quantity, 0);
            return 0;
        }
    }
    
    int64_t remaining_qty = quantity;
    size_t trade_count = 0;

    while (remaining_qty > 0 && !opposite_side_empty) {

        if (is_buy && best_price > price) break;
        if (!is_buy && best_price < price) break;
        
        size_t level_idx = price_to_index(best_price);
        if (level_idx >= MAX_PRICE_LEVELS) break;
        
        PriceLevel& level = levels[level_idx];
        if (level.empty()) {

            if (is_buy) {
                update_best_ask_after_remove(best_price);
            } else {
                update_best_bid_after_remove(best_price);
            }
            continue;
        }
        
        int64_t current_best = best_price;
        uint32_t curr = level.head;
        
        while (curr != NULL_INDEX && remaining_qty > 0) {
            Order& book_order = order_pool_[curr];
            uint32_t next_idx = book_order.next;

            if (book_order.is_aon()) {
                int64_t aon_total = book_order.quantity + book_order.hidden_quantity;
                if (remaining_qty < aon_total) {
                    curr = next_idx;
                    continue;
                }
            }
            
            int64_t trade_qty = std::min(remaining_qty, book_order.quantity);

            uint64_t buy_id = is_buy ? order_id : book_order.order_id;
            uint64_t sell_id = is_buy ? book_order.order_id : order_id;
            emit_trade(buy_id, sell_id, current_best, trade_qty);
            trade_count++;
            
            remaining_qty -= trade_qty;
            adjust_level_volume(level, -trade_qty, 0, book_order.is_aon());
            book_order.quantity -= trade_qty;
            
            if (book_order.quantity == 0) {
                if (book_order.hidden_quantity > 0) {

                    int64_t replenish = std::min(
                        book_order.peak_size > 0 ? book_order.peak_size : book_order.hidden_quantity,
                        book_order.hidden_quantity
                    );
                    
                    remove_from_level_volume(level, book_order);
                    list_remove(level, curr);
                    
                    book_order.quantity = replenish;
                    book_order.hidden_quantity -= replenish;
                    
                    list_push_back(level, curr);
                    add_to_level_volume(level, book_order);
                    
                    if (book_order.order_id < order_index_.size()) {
                        order_index_[book_order.order_id].pool_idx = curr;
                    }
                } else {

                    list_remove(level, curr);
                    
                    if (book_order.order_id < order_index_.size()) {
                        order_index_[book_order.order_id].set_active(false);
                        active_order_count_--;
                    }
                    
                    order_pool_.free(curr);
                }
            }
            
            curr = next_idx;
        }

        if (level.empty()) {
            if (is_buy) {
                ask_level_count_--;
                update_best_ask_after_remove(current_best);
            } else {
                bid_level_count_--;
                update_best_bid_after_remove(current_best);
            }
        }

        opposite_side_empty = is_buy ? (best_price == INT64_MAX) : (best_price < 0);
    }

    if (remaining_qty > 0) {
        switch (tif) {
            case TimeInForce::GTC:
                add_order_internal(order_id, is_buy, price, remaining_qty, 0);
                break;
            case TimeInForce::AON:
                add_aon_internal(order_id, is_buy, price, remaining_qty, 0);
                break;
            case TimeInForce::IOC:
            case TimeInForce::FOK:
                break;
        }
    }
    
    return trade_count;
}

void OptimizedOrderBook::reset_internal() {
    for (auto& level : bid_levels_) level.reset();
    for (auto& level : ask_levels_) level.reset();
    std::memset(bid_bitmap_, 0, sizeof(bid_bitmap_));
    std::memset(ask_bitmap_, 0, sizeof(ask_bitmap_));
    best_bid_ = -1;
    best_ask_ = INT64_MAX;
    best_bid_word_ = -1;
    best_ask_word_ = 0;
    bid_level_count_ = 0;
    ask_level_count_ = 0;
    active_order_count_ = 0;
    order_pool_.reset();
    for (size_t i = 0; i <= max_order_id_ && i < order_index_.size(); ++i) {
        order_index_[i].set_active(false);
    }
}