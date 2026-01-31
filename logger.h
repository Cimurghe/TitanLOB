#ifndef LOGGER_H
#define LOGGER_H

#include <cstdint>
#include <cstring>
#include <atomic>
#include <thread>
#include <memory>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "output_msg.h"

namespace deepflow {

#pragma pack(push, 1)
struct FileHeader {
    uint64_t magic;
    uint32_t version;
    uint32_t msg_size;
    uint64_t timestamp_start;
    uint64_t reserved[4];
    
    static constexpr uint64_t MAGIC = 0x574F4C46504545ULL;
    static constexpr uint32_t VERSION = 1;
    
    static FileHeader create() {
        FileHeader h{};
        h.magic = MAGIC;
        h.version = VERSION;
        h.msg_size = sizeof(OutputMsg);
        h.timestamp_start = 0;
        return h;
    }
    
    bool is_valid() const {
        return magic == MAGIC && version == VERSION && msg_size == sizeof(OutputMsg);
    }
};
#pragma pack(pop)

static_assert(sizeof(FileHeader) == 56, "FileHeader must be 56 bytes");

constexpr size_t BUFFER_CAPACITY = 65536;
constexpr size_t BUFFER_SIZE_BYTES = BUFFER_CAPACITY * sizeof(OutputMsg);

struct MessageBuffer {
    std::unique_ptr<OutputMsg[]> data;
    size_t count = 0;
    
    MessageBuffer() : data(std::make_unique<OutputMsg[]>(BUFFER_CAPACITY)), count(0) {}
    
    void reset() { count = 0; }
    bool is_full() const { return count >= BUFFER_CAPACITY; }
    size_t remaining() const { return BUFFER_CAPACITY - count; }
};

class BinaryLogger {
private:
    MessageBuffer buffer_a_;
    MessageBuffer buffer_b_;
    MessageBuffer* write_buffer_;
    MessageBuffer* flush_buffer_;
    
    std::atomic<bool> flush_pending_{false};
    std::atomic<bool> running_{false};
    std::thread flush_thread_;
    
    int fd_ = -1;
    
    std::atomic<uint64_t> messages_logged_{0};
    std::atomic<uint64_t> bytes_written_{0};
    std::atomic<uint64_t> flushes_completed_{0};
    
public:
    explicit BinaryLogger(const char* filename) {
        fd_ = ::open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_ < 0) {
            fd_ = ::open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        }
        
        FileHeader header = FileHeader::create();
        if (fd_ >= 0) {
            ::write(fd_, &header, sizeof(header));
        }
        
        write_buffer_ = &buffer_a_;
        flush_buffer_ = &buffer_b_;
        
        running_ = true;
        flush_thread_ = std::thread(&BinaryLogger::flush_thread_func, this);
    }
    
    ~BinaryLogger() {
        if (write_buffer_->count > 0) {
            flush_sync();
        }
        
        running_ = false;
        flush_pending_ = true;
        if (flush_thread_.joinable()) {
            flush_thread_.join();
        }
        
        if (fd_ >= 0) {
            ::fsync(fd_);
            ::close(fd_);
        }
    }
    
    BinaryLogger(const BinaryLogger&) = delete;
    BinaryLogger& operator=(const BinaryLogger&) = delete;
    
    void log(const OutputMsg& msg) noexcept {
        write_buffer_->data[write_buffer_->count++] = msg;
        messages_logged_.fetch_add(1, std::memory_order_relaxed);
        
        if (write_buffer_->is_full()) [[unlikely]] {
            trigger_flush();
        }
    }
    
    void log_batch(const OutputMsg* msgs, size_t count) noexcept {
        while (count > 0) {
            size_t space = write_buffer_->remaining();
            size_t to_copy = (count < space) ? count : space;
            
            std::memcpy(write_buffer_->data.get() + write_buffer_->count, msgs, to_copy * sizeof(OutputMsg));
            write_buffer_->count += to_copy;
            msgs += to_copy;
            count -= to_copy;
            messages_logged_.fetch_add(to_copy, std::memory_order_relaxed);
            
            if (write_buffer_->is_full()) [[unlikely]] {
                trigger_flush();
            }
        }
    }
    
    uint64_t messages_logged() const { 
        return messages_logged_.load(std::memory_order_relaxed); 
    }
    
    uint64_t bytes_written() const { 
        return bytes_written_.load(std::memory_order_relaxed); 
    }
    
    uint64_t flushes_completed() const { 
        return flushes_completed_.load(std::memory_order_relaxed); 
    }
    
    size_t buffer_usage() const {
        return write_buffer_->count;
    }
    
private:
    void trigger_flush() {
        while (flush_pending_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        
        std::swap(write_buffer_, flush_buffer_);
        
        flush_pending_.store(true, std::memory_order_release);
    }
    
    void flush_sync() {
        if (write_buffer_->count == 0) return;
        
        while (flush_pending_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        
        size_t bytes = write_buffer_->count * sizeof(OutputMsg);
        if (fd_ >= 0) {
            ::write(fd_, write_buffer_->data.get(), bytes);
            bytes_written_.fetch_add(bytes, std::memory_order_relaxed);
        }
        write_buffer_->reset();
    }
    
    void flush_thread_func() {
        while (running_.load(std::memory_order_relaxed)) {
            if (!flush_pending_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }
            
            if (flush_buffer_->count > 0 && fd_ >= 0) {
                size_t bytes = flush_buffer_->count * sizeof(OutputMsg);
                ssize_t written = ::write(fd_, flush_buffer_->data.get(), bytes);
                if (written > 0) {
                    bytes_written_.fetch_add(static_cast<size_t>(written), std::memory_order_relaxed);
                }
                flush_buffer_->reset();
                flushes_completed_.fetch_add(1, std::memory_order_relaxed);
            }
            
            flush_pending_.store(false, std::memory_order_release);
        }
        
        if (flush_buffer_->count > 0 && fd_ >= 0) {
            size_t bytes = flush_buffer_->count * sizeof(OutputMsg);
            ::write(fd_, flush_buffer_->data.get(), bytes);
            bytes_written_.fetch_add(bytes, std::memory_order_relaxed);
        }
    }
};

class BinaryLogReader {
private:
    int fd_ = -1;
    FileHeader header_;
    uint64_t messages_read_ = 0;
    
public:
    explicit BinaryLogReader(const char* filename) {
        fd_ = ::open(filename, O_RDONLY);
        if (fd_ >= 0) {
            ssize_t n = ::read(fd_, &header_, sizeof(header_));
            if (n != sizeof(header_) || !header_.is_valid()) {
                ::close(fd_);
                fd_ = -1;
            }
        }
    }
    
    ~BinaryLogReader() {
        if (fd_ >= 0) ::close(fd_);
    }
    
    bool is_open() const { return fd_ >= 0; }
    const FileHeader& header() const { return header_; }
    
    bool read(OutputMsg& msg) {
        if (fd_ < 0) return false;
        ssize_t n = ::read(fd_, &msg, sizeof(msg));
        if (n == sizeof(msg)) {
            messages_read_++;
            return true;
        }
        return false;
    }
    
    size_t read_batch(OutputMsg* msgs, size_t max_count) {
        if (fd_ < 0) return 0;
        ssize_t n = ::read(fd_, msgs, max_count * sizeof(OutputMsg));
        if (n > 0) {
            size_t count = static_cast<size_t>(n) / sizeof(OutputMsg);
            messages_read_ += count;
            return count;
        }
        return 0;
    }
    
    uint64_t messages_read() const { return messages_read_; }
    
    void rewind() {
        if (fd_ >= 0) {
            ::lseek(fd_, sizeof(FileHeader), SEEK_SET);
            messages_read_ = 0;
        }
    }
};

}

#endif
