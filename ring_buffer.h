#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <atomic>
#include <cstddef>
#include <cstring>
#include <new>
#include <type_traits>

#ifndef CACHE_LINE_SIZE
inline constexpr size_t CACHE_LINE_SIZE = 64;
#endif

template<typename T, size_t Size>
class RingBuffer {
    static_assert((Size & (Size - 1)) == 0, "Size must be a power of 2");
    static_assert(Size > 0, "Size must be positive");

public:
    using value_type = T;
    using size_type = size_t;
    static constexpr size_type capacity_value = Size;
    static constexpr size_type mask = Size - 1;

private:
    alignas(CACHE_LINE_SIZE) T buffer_[Size];
    alignas(CACHE_LINE_SIZE) std::atomic<size_type> head_{0};
    char pad_[CACHE_LINE_SIZE - sizeof(std::atomic<size_type>)];
    alignas(CACHE_LINE_SIZE) std::atomic<size_type> tail_{0};

public:
    RingBuffer() = default;
    
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    size_t push_batch(const T* batch_data, size_t count) noexcept {
        if (count == 0) return 0;

        const size_type current_head = head_.load(std::memory_order_relaxed);
        const size_type current_tail = tail_.load(std::memory_order_acquire);
        
        const size_type used = (current_head - current_tail) & mask;
        const size_type available = (capacity_value - 1) - used;
        
        const size_t to_write = (count < available) ? count : available;
        if (to_write == 0) return 0;

        const size_t write_idx = current_head & mask;
        const size_t first_chunk = capacity_value - write_idx;
        
        if (to_write <= first_chunk) {
            if constexpr (std::is_trivially_copyable_v<T>) {
                std::memcpy(&buffer_[write_idx], batch_data, to_write * sizeof(T));
            } else {
                for (size_t i = 0; i < to_write; ++i) {
                    buffer_[write_idx + i] = batch_data[i];
                }
            }
        } else {
            if constexpr (std::is_trivially_copyable_v<T>) {
                std::memcpy(&buffer_[write_idx], batch_data, first_chunk * sizeof(T));
                std::memcpy(&buffer_[0], batch_data + first_chunk, (to_write - first_chunk) * sizeof(T));
            } else {
                for (size_t i = 0; i < first_chunk; ++i) {
                    buffer_[write_idx + i] = batch_data[i];
                }
                for (size_t i = 0; i < to_write - first_chunk; ++i) {
                    buffer_[i] = batch_data[first_chunk + i];
                }
            }
        }

        head_.store(current_head + to_write, std::memory_order_release);
        
        return to_write;
    }

    [[nodiscard]] bool try_push(const T& item) noexcept {
        const size_type current_head = head_.load(std::memory_order_relaxed);
        const size_type current_tail = tail_.load(std::memory_order_acquire);
        
        if (((current_head + 1) & mask) == (current_tail & mask)) {
            return false;
        }
        
        buffer_[current_head & mask] = item;
        head_.store(current_head + 1, std::memory_order_release);
        
        return true;
    }

    [[nodiscard]] bool try_push_with_cached_tail(const T& item, size_type cached_tail) noexcept {
        const size_type current_head = head_.load(std::memory_order_relaxed);
        
        if (((current_head + 1) & mask) == (cached_tail & mask)) {
            return false;
        }
        
        buffer_[current_head & mask] = item;
        head_.store(current_head + 1, std::memory_order_release);
        
        return true;
    }

    [[nodiscard]] bool try_pop(T& item) noexcept {
        const size_type current_tail = tail_.load(std::memory_order_relaxed);
        const size_type current_head = head_.load(std::memory_order_acquire);
        
        if ((current_tail & mask) == (current_head & mask) && current_tail == current_head) {
            return false;
        }
        
        item = buffer_[current_tail & mask];
        tail_.store(current_tail + 1, std::memory_order_release);
        
        return true;
    }

    size_t pop_batch(T* out, size_t max_count) noexcept {
        const size_type current_tail = tail_.load(std::memory_order_relaxed);
        const size_type current_head = head_.load(std::memory_order_acquire);
        
        const size_type available = (current_head - current_tail);
        const size_t to_read = (max_count < available) ? max_count : available;
        
        if (to_read == 0) return 0;
        
        const size_t read_idx = current_tail & mask;
        const size_t first_chunk = capacity_value - read_idx;
        
        if (to_read <= first_chunk) {
            if constexpr (std::is_trivially_copyable_v<T>) {
                std::memcpy(out, &buffer_[read_idx], to_read * sizeof(T));
            } else {
                for (size_t i = 0; i < to_read; ++i) {
                    out[i] = buffer_[read_idx + i];
                }
            }
        } else {
            if constexpr (std::is_trivially_copyable_v<T>) {
                std::memcpy(out, &buffer_[read_idx], first_chunk * sizeof(T));
                std::memcpy(out + first_chunk, &buffer_[0], (to_read - first_chunk) * sizeof(T));
            } else {
                for (size_t i = 0; i < first_chunk; ++i) {
                    out[i] = buffer_[read_idx + i];
                }
                for (size_t i = 0; i < to_read - first_chunk; ++i) {
                    out[first_chunk + i] = buffer_[i];
                }
            }
        }
        
        tail_.store(current_tail + to_read, std::memory_order_release);
        return to_read;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_relaxed) == 
               tail_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool full() const noexcept {
        const size_type h = head_.load(std::memory_order_relaxed);
        const size_type t = tail_.load(std::memory_order_relaxed);
        return ((h + 1) & mask) == (t & mask);
    }

    [[nodiscard]] size_type size_approx() const noexcept {
        const size_type h = head_.load(std::memory_order_relaxed);
        const size_type t = tail_.load(std::memory_order_relaxed);
        return h - t;
    }

    [[nodiscard]] static constexpr size_type capacity() noexcept {
        return Size - 1;
    }

    [[nodiscard]] size_type get_tail_relaxed() const noexcept {
        return tail_.load(std::memory_order_relaxed);
    }
};

#endif
