#ifndef ORDER_BOOK_H
#define ORDER_BOOK_H

#include <cstdint>
#include <vector>
#include <memory>
#include <cstring>
#include <map>
#include <shared_mutex>
#include <mutex>
#include <limits>
#include "protocol.h"
#include "object_pool.h"
#include "ring_buffer.h"
#include "output_msg.h"

constexpr size_t OUTPUT_BUFFER_SIZE = 1 << 20;
constexpr size_t BATCH_SIZE = 64;

constexpr int64_t PRICE_OFFSET = 0;
constexpr size_t MAX_PRICE_LEVELS = 33554432;

constexpr size_t BITMAP_WORDS = MAX_PRICE_LEVELS / 64;

using OutputBuffer = RingBuffer<OutputMsg, OUTPUT_BUFFER_SIZE>;

enum class TimeInForce {
    GTC, IOC, FOK, AON
};

inline TimeInForce tif_from_protocol(TIF t) {
    switch (t) {
        case TIF::GTC: return TimeInForce::GTC;
        case TIF::IOC: return TimeInForce::IOC;
        case TIF::FOK: return TimeInForce::FOK;
        case TIF::AON: return TimeInForce::AON;
        default: return TimeInForce::GTC;
    }
}

struct alignas(64) Order {
    uint64_t order_id;
    int64_t price;
    int64_t quantity;
    int64_t hidden_quantity;
    int64_t peak_size;
    uint32_t next;
    uint32_t prev;
    uint32_t user_id_low;
    uint8_t flags;
    uint8_t _pad[7];
    
    bool is_iceberg() const { return peak_size > 0; }
    int64_t total_quantity() const { return quantity + hidden_quantity; }
    bool is_buy() const { return flags & 0x01; }
    bool is_aon() const { return flags & 0x02; }
    void set_buy(bool v) { if (v) flags |= 0x01; else flags &= ~0x01; }
    void set_aon(bool v) { if (v) flags |= 0x02; else flags &= ~0x02; }
};
static_assert(sizeof(Order) == 64, "Order must be exactly 64 bytes");

struct PriceLevel {
    uint32_t head = NULL_INDEX;
    uint32_t tail = NULL_INDEX;
    uint32_t count = 0;
    int64_t total_volume = 0;
    int64_t total_visible_volume = 0;
    int64_t total_aon_volume = 0;
    int64_t total_non_aon_volume = 0;
    
    bool empty() const { return head == NULL_INDEX; }
    void reset() {
        head = tail = NULL_INDEX;
        count = 0;
        total_volume = total_visible_volume = total_aon_volume = total_non_aon_volume = 0;
    }
};

struct Trade {
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    int64_t price;
    int64_t quantity;
};

inline int64_t bitmap_find_highest(const uint64_t* bitmap, size_t num_words, int64_t start_word) {
    for (int64_t w = start_word; w >= 0; --w) {
        if (bitmap[w] != 0) {

            int bit_in_word = 63 - __builtin_clzll(bitmap[w]);
            return w * 64 + bit_in_word;
        }
    }
    return -1;
}

inline int64_t bitmap_find_lowest(const uint64_t* bitmap, size_t num_words, int64_t start_word) {
    for (size_t w = start_word; w < num_words; ++w) {
        if (bitmap[w] != 0) {

            int bit_in_word = __builtin_ctzll(bitmap[w]);
            return w * 64 + bit_in_word;
        }
    }
    return -1;
}

inline void bitmap_set(uint64_t* bitmap, size_t idx) {
    bitmap[idx / 64] |= (1ULL << (idx % 64));
}

inline void bitmap_clear(uint64_t* bitmap, size_t idx) {
    bitmap[idx / 64] &= ~(1ULL << (idx % 64));
}

inline bool bitmap_test(const uint64_t* bitmap, size_t idx) {
    return (bitmap[idx / 64] & (1ULL << (idx % 64))) != 0;
}

class OptimizedOrderBook {
private:

    std::unique_ptr<PriceLevel[]> bid_levels_;
    std::unique_ptr<PriceLevel[]> ask_levels_;

    std::unique_ptr<uint64_t[]> bid_bitmap_;
    std::unique_ptr<uint64_t[]> ask_bitmap_;

    int64_t best_bid_ = -1;
    int64_t best_ask_ = INT64_MAX;

    int64_t best_bid_word_ = -1;
    int64_t best_ask_word_ = 0;
    
    uint32_t bid_level_count_ = 0;
    uint32_t ask_level_count_ = 0;

    ObjectPool<Order> order_pool_;

    struct OrderLocation {
        int64_t price;
        uint32_t pool_idx;
        uint8_t flags;
        uint8_t _pad[3];
        
        bool is_buy() const { return flags & 0x01; }
        bool is_active() const { return flags & 0x02; }
        void set_buy(bool v) { if (v) flags |= 0x01; else flags &= ~0x01; }
        void set_active(bool v) { if (v) flags |= 0x02; else flags &= ~0x02; }
    };
    std::vector<OrderLocation> order_index_;
    size_t active_order_count_ = 0;
    size_t max_order_id_ = 0;

    static constexpr size_t INITIAL_ORDER_CAPACITY = 5'000'000;
    
    inline void ensure_capacity(uint64_t order_id) {
        if (order_id >= order_index_.size()) [[unlikely]] {
            order_index_.resize(std::max(order_id + 1, order_index_.size() * 2));
        }
        if (order_id > max_order_id_) max_order_id_ = order_id;
    }
    
    static inline size_t price_to_index(int64_t price) {
        return static_cast<size_t>(price - PRICE_OFFSET);
    }
    
    static inline int64_t index_to_price(size_t idx) {
        return static_cast<int64_t>(idx) + PRICE_OFFSET;
    }

    OutputBuffer output_buffer_;
    bool use_ring_buffer_ = true;
    bool emit_accepts_ = true;
    bool emit_cancels_ = true;
    
    alignas(64) OutputMsg batch_buffer_[BATCH_SIZE];
    uint8_t batch_count_ = 0;

    uint64_t current_timestamp_ = 0;
    uint64_t messages_processed_ = 0;
    uint64_t trades_executed_ = 0;
    uint64_t messages_dropped_ = 0;

    mutable std::shared_mutex book_mutex_;

    inline void list_push_back(PriceLevel& level, uint32_t idx) {
        Order& node = order_pool_[idx];
        node.next = NULL_INDEX;
        node.prev = level.tail;
        
        if (level.tail != NULL_INDEX) {
            order_pool_[level.tail].next = idx;
        } else {
            level.head = idx;
        }
        level.tail = idx;
        level.count++;
    }
    
    inline void list_remove(PriceLevel& level, uint32_t idx) {
        Order& node = order_pool_[idx];
        
        if (node.prev != NULL_INDEX) {
            order_pool_[node.prev].next = node.next;
        } else {
            level.head = node.next;
        }
        
        if (node.next != NULL_INDEX) {
            order_pool_[node.next].prev = node.prev;
        } else {
            level.tail = node.prev;
        }
        
        node.prev = node.next = NULL_INDEX;
        level.count--;
    }

    inline void add_to_level_volume(PriceLevel& level, const Order& order) {
        int64_t total = order.quantity + order.hidden_quantity;
        level.total_volume += total;
        level.total_visible_volume += order.quantity;
        if (order.is_aon()) {
            level.total_aon_volume += total;
        } else {
            level.total_non_aon_volume += total;
        }
    }
    
    inline void remove_from_level_volume(PriceLevel& level, const Order& order) {
        int64_t total = order.quantity + order.hidden_quantity;
        level.total_volume -= total;
        level.total_visible_volume -= order.quantity;
        if (order.is_aon()) {
            level.total_aon_volume -= total;
        } else {
            level.total_non_aon_volume -= total;
        }
    }
    
    inline void adjust_level_volume(PriceLevel& level, int64_t visible_delta, 
                                    int64_t hidden_delta, bool is_aon) {
        level.total_volume += visible_delta + hidden_delta;
        level.total_visible_volume += visible_delta;
        if (is_aon) {
            level.total_aon_volume += visible_delta + hidden_delta;
        } else {
            level.total_non_aon_volume += visible_delta + hidden_delta;
        }
    }

    inline void update_best_bid_after_add(int64_t price) {
        size_t idx = price_to_index(price);
        bitmap_set(bid_bitmap_.get(), idx);
        if (best_bid_ < 0 || price > best_bid_) {
            best_bid_ = price;
            best_bid_word_ = idx / 64;
        }
    }
    
    inline void update_best_ask_after_add(int64_t price) {
        size_t idx = price_to_index(price);
        bitmap_set(ask_bitmap_.get(), idx);
        if (best_ask_ == INT64_MAX || price < best_ask_) {
            best_ask_ = price;
            best_ask_word_ = idx / 64;
        }
    }
    
    inline void update_best_bid_after_remove(int64_t removed_price) {
        size_t idx = price_to_index(removed_price);
        if (bid_levels_[idx].empty()) {
            bitmap_clear(bid_bitmap_.get(), idx);
        }
        
        if (removed_price == best_bid_) {
            int64_t new_best = bitmap_find_highest(bid_bitmap_.get(), BITMAP_WORDS, best_bid_word_);
            if (new_best >= 0) {
                best_bid_ = index_to_price(new_best);
                best_bid_word_ = new_best / 64;
            } else {
                best_bid_ = -1;
                best_bid_word_ = -1;
            }
        }
    }
    
    inline void update_best_ask_after_remove(int64_t removed_price) {
        size_t idx = price_to_index(removed_price);
        if (ask_levels_[idx].empty()) {
            bitmap_clear(ask_bitmap_.get(), idx);
        }
        
        if (removed_price == best_ask_) {
            int64_t new_best = bitmap_find_lowest(ask_bitmap_.get(), BITMAP_WORDS, best_ask_word_);
            if (new_best >= 0) {
                best_ask_ = index_to_price(new_best);
                best_ask_word_ = new_best / 64;
            } else {
                best_ask_ = INT64_MAX;
                best_ask_word_ = 0;
            }
        }
    }

    inline void flush_batch() {
        if (batch_count_ > 0) {
            size_t pushed = output_buffer_.push_batch(batch_buffer_, batch_count_);
            if (pushed < batch_count_) [[unlikely]] {
                messages_dropped_ += (batch_count_ - pushed);
            }
            batch_count_ = 0;
        }
    }
    
    inline void emit_trade(uint64_t buy_id, uint64_t sell_id, int64_t price, int64_t qty) {
        ++trades_executed_;
        if (use_ring_buffer_) [[likely]] {
            batch_buffer_[batch_count_++] = 
                OutputMsg::make_trade(current_timestamp_, buy_id, sell_id, price, qty);
            if (batch_count_ >= BATCH_SIZE) [[unlikely]] flush_batch();
        }
    }
    
    inline void emit_order_accepted(uint64_t order_id, Side side, int64_t price, int64_t qty) {
        if (!emit_accepts_) return;
        if (use_ring_buffer_) [[likely]] {
            batch_buffer_[batch_count_++] = 
                OutputMsg::make_accepted(current_timestamp_, order_id, side, price, qty);
            if (batch_count_ >= BATCH_SIZE) [[unlikely]] flush_batch();
        }
    }
    
    inline void emit_order_cancelled(uint64_t order_id, int64_t cancelled_qty) {
        if (!emit_cancels_) return;
        if (use_ring_buffer_) [[likely]] {
            batch_buffer_[batch_count_++] = 
                OutputMsg::make_cancelled(current_timestamp_, order_id, cancelled_qty);
            if (batch_count_ >= BATCH_SIZE) [[unlikely]] flush_batch();
        }
    }

    void add_order_internal(uint64_t order_id, bool is_buy, int64_t price, 
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
    }

    void add_iceberg_internal(uint64_t order_id, bool is_buy, int64_t price,
                              int64_t total_quantity, int64_t visible_quantity, uint32_t user_id) {
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

    void add_aon_internal(uint64_t order_id, bool is_buy, int64_t price,
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

    void cancel_order_internal(uint64_t order_id) {
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

    void modify_order_internal(uint64_t order_id, int64_t new_price, int64_t new_quantity) {
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
    
    int64_t calculate_available_quantity(bool is_buy, int64_t price, int64_t incoming_qty) const {
        auto& levels = is_buy ? ask_levels_ : bid_levels_;
        int64_t best = is_buy ? best_ask_ : best_bid_;
        int64_t limit_price = price;
        
        if (is_buy && best == INT64_MAX) return 0;
        if (!is_buy && best < 0) return 0;
        
        int64_t available = 0;
        int64_t remaining = incoming_qty;
        
        if (is_buy) {
            for (int64_t p = best; p <= limit_price && remaining > 0; ++p) {
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

    size_t match_internal(uint64_t order_id, bool is_buy, int64_t price, 
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

    void reset_internal() {
        for (size_t i = 0; i < MAX_PRICE_LEVELS; ++i) {
            bid_levels_[i].reset();
            ask_levels_[i].reset();
        }
        std::memset(bid_bitmap_.get(), 0, BITMAP_WORDS * sizeof(uint64_t));
        std::memset(ask_bitmap_.get(), 0, BITMAP_WORDS * sizeof(uint64_t));
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

public:

    OptimizedOrderBook(size_t order_capacity = 1'000'000)
        : bid_levels_(new PriceLevel[MAX_PRICE_LEVELS])
        , ask_levels_(new PriceLevel[MAX_PRICE_LEVELS])
        , bid_bitmap_(new uint64_t[BITMAP_WORDS])
        , ask_bitmap_(new uint64_t[BITMAP_WORDS])
        , order_pool_(order_capacity)
    {

        order_index_.resize(INITIAL_ORDER_CAPACITY);

        std::memset(bid_bitmap_.get(), 0, BITMAP_WORDS * sizeof(uint64_t));
        std::memset(ask_bitmap_.get(), 0, BITMAP_WORDS * sizeof(uint64_t));

        for (size_t i = 0; i < MAX_PRICE_LEVELS; ++i) {
            bid_levels_[i].reset();
            ask_levels_[i].reset();
        }
    }

    OptimizedOrderBook(const OptimizedOrderBook&) = delete;
    OptimizedOrderBook& operator=(const OptimizedOrderBook&) = delete;

    OptimizedOrderBook(OptimizedOrderBook&&) = default;
    OptimizedOrderBook& operator=(OptimizedOrderBook&&) = default;

    void add_order(uint64_t order_id, bool is_buy, int64_t price, 
                   int64_t quantity, uint32_t user_id = 0) {
        std::unique_lock lock(book_mutex_);
        
        bool is_aggressive = is_buy 
            ? (best_ask_ != INT64_MAX && price >= best_ask_)
            : (best_bid_ >= 0 && price <= best_bid_);
        
        if (is_aggressive) {
            match_internal(order_id, is_buy, price, quantity, TimeInForce::GTC);
        } else {
            add_order_internal(order_id, is_buy, price, quantity, user_id);
        }
    }

    void add_order_no_lock(uint64_t order_id, bool is_buy, int64_t price, 
                           int64_t quantity, uint32_t user_id = 0) {
        bool is_aggressive = is_buy 
            ? (best_ask_ != INT64_MAX && price >= best_ask_)
            : (best_bid_ >= 0 && price <= best_bid_);
        
        if (is_aggressive) {
            match_internal(order_id, is_buy, price, quantity, TimeInForce::GTC);
        } else {
            add_order_internal(order_id, is_buy, price, quantity, user_id);
        }
    }
    
    void match_order(uint64_t order_id, bool is_buy, int64_t price,
                     int64_t quantity, TimeInForce tif = TimeInForce::GTC) {
        std::unique_lock lock(book_mutex_);
        match_internal(order_id, is_buy, price, quantity, tif);
    }
    
    void cancel_order(uint64_t order_id) {
        std::unique_lock lock(book_mutex_);
        cancel_order_internal(order_id);
    }
    
    void cancel_order_no_lock(uint64_t order_id) {
        cancel_order_internal(order_id);
    }

    void use_ring_buffer_output(bool enable = true) { use_ring_buffer_ = enable; }
    void set_benchmark_mode(bool trades_only = true) {
        emit_accepts_ = !trades_only;
        emit_cancels_ = !trades_only;
    }
    void set_emit_accepts(bool enable) { emit_accepts_ = enable; }
    void set_emit_cancels(bool enable) { emit_cancels_ = enable; }
    void flush_output_buffer() { flush_batch(); }

    OutputBuffer& get_output_buffer() { return output_buffer_; }
    const OutputBuffer& get_output_buffer() const { return output_buffer_; }

    int64_t get_best_bid() const { 
        std::shared_lock lock(book_mutex_);
        return best_bid_ >= 0 ? best_bid_ : 0; 
    }
    int64_t get_best_ask() const { 
        std::shared_lock lock(book_mutex_);
        return best_ask_;
    }
    
    int64_t get_best_bid_volume() const {
        std::shared_lock lock(book_mutex_);
        if (best_bid_ < 0) return 0;
        return bid_levels_[price_to_index(best_bid_)].total_visible_volume;
    }
    
    int64_t get_best_ask_volume() const {
        std::shared_lock lock(book_mutex_);
        if (best_ask_ == INT64_MAX) return 0;
        return ask_levels_[price_to_index(best_ask_)].total_visible_volume;
    }

    std::map<int64_t, int64_t, std::greater<int64_t>> get_bids_snapshot() const {
        std::shared_lock lock(book_mutex_);
        
        std::map<int64_t, int64_t, std::greater<int64_t>> result;
        
        for (size_t i = 0; i < MAX_PRICE_LEVELS; ++i) {
            if (bid_levels_[i].total_visible_volume > 0) {
                int64_t price = index_to_price(i);
                result[price] = bid_levels_[i].total_visible_volume;
            }
        }
        return result;
    }
    
    std::map<int64_t, int64_t> get_asks_snapshot() const {
        std::shared_lock lock(book_mutex_);
        
        std::map<int64_t, int64_t> result;
        
        for (size_t i = 0; i < MAX_PRICE_LEVELS; ++i) {
            if (ask_levels_[i].total_visible_volume > 0) {
                int64_t price = index_to_price(i);
                result[price] = ask_levels_[i].total_visible_volume;
            }
        }
        return result;
    }

    uint64_t messages_processed() const { 
        std::shared_lock lock(book_mutex_);
        return messages_processed_; 
    }
    uint64_t trades_executed() const { 
        std::shared_lock lock(book_mutex_);
        return trades_executed_; 
    }
    uint64_t messages_dropped() const { 
        std::shared_lock lock(book_mutex_);
        return messages_dropped_; 
    }
    size_t order_count() const { 
        std::shared_lock lock(book_mutex_);
        return active_order_count_; 
    }
    size_t bid_levels() const { 
        std::shared_lock lock(book_mutex_);
        return bid_level_count_; 
    }
    size_t ask_levels() const { 
        std::shared_lock lock(book_mutex_);
        return ask_level_count_; 
    }
    size_t pool_capacity() const { return order_pool_.capacity(); }
    size_t pool_used() const { 
        std::shared_lock lock(book_mutex_);
        return order_pool_.used_count(); 
    }
    size_t output_buffer_size() const { return output_buffer_.size_approx(); }
};

#endif
