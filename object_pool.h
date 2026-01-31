#ifndef OBJECT_POOL_H
#define OBJECT_POOL_H

#include <vector>
#include <cstdint>
#include <stdexcept>

constexpr uint32_t NULL_INDEX = UINT32_MAX;

template<typename T>
class ObjectPool {
private:
    std::vector<T> pool_;
    std::vector<uint32_t> free_indices_;
    size_t initial_capacity_;
    
public:
    explicit ObjectPool(size_t capacity = 1'000'000) 
        : initial_capacity_(capacity) 
    {
        pool_.resize(capacity);
        free_indices_.reserve(capacity);
        
        for (size_t i = capacity; i > 0; --i) {
            free_indices_.push_back(static_cast<uint32_t>(i - 1));
        }
    }
    
    uint32_t allocate() {
        if (free_indices_.empty()) {
            grow();
        }
        
        uint32_t idx = free_indices_.back();
        free_indices_.pop_back();
        return idx;
    }
    
    void free(uint32_t idx) {
        pool_[idx] = T{};
        free_indices_.push_back(idx);
    }
    
    T& operator[](uint32_t idx) {
        return pool_[idx];
    }
    
    const T& operator[](uint32_t idx) const {
        return pool_[idx];
    }
    
    size_t capacity() const { return pool_.size(); }
    size_t free_count() const { return free_indices_.size(); }
    size_t used_count() const { return pool_.size() - free_indices_.size(); }
    
    void reset() {
        free_indices_.clear();
        for (size_t i = pool_.size(); i > 0; --i) {
            free_indices_.push_back(static_cast<uint32_t>(i - 1));
        }
    }
    
private:
    void grow() {
        size_t old_size = pool_.size();
        size_t new_size = old_size * 2;
        
        pool_.resize(new_size);
        
        for (size_t i = new_size; i > old_size; --i) {
            free_indices_.push_back(static_cast<uint32_t>(i - 1));
        }
    }
};

template<typename T, typename LevelT>
void intrusive_list_push_back(ObjectPool<T>& pool, LevelT& level, uint32_t idx) {
    T& node = pool[idx];
    node.next = NULL_INDEX;
    node.prev = level.tail;
    
    if (level.tail != NULL_INDEX) {
        pool[level.tail].next = idx;
    } else {
        level.head = idx;
    }
    
    level.tail = idx;
}

template<typename T, typename LevelT>
void intrusive_list_remove(ObjectPool<T>& pool, LevelT& level, uint32_t idx) {
    T& node = pool[idx];
    
    if (node.prev != NULL_INDEX) {
        pool[node.prev].next = node.next;
    } else {
        level.head = node.next;
    }
    
    if (node.next != NULL_INDEX) {
        pool[node.next].prev = node.prev;
    } else {
        level.tail = node.prev;
    }
    
    node.prev = NULL_INDEX;
    node.next = NULL_INDEX;
}

template<typename LevelT>
bool intrusive_list_empty(const LevelT& level) {
    return level.head == NULL_INDEX;
}

#endif
