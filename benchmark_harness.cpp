

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <vector>

#include "order_book.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
inline uint64_t rdtscp() {
    unsigned int lo, hi, aux;
    __asm__ __volatile__ ("rdtscp" : "=a" (lo), "=d" (hi), "=c" (aux));
    return ((uint64_t)hi << 32) | lo;
}
inline void cpuid_serialize() {
    unsigned int eax, ebx, ecx, edx;
    __asm__ __volatile__ ("cpuid" : "=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx) : "a" (0));
}
double calibrate_tsc_frequency() {
    cpuid_serialize();
    auto start_time = std::chrono::high_resolution_clock::now();
    uint64_t start_tsc = rdtscp();
    auto target = start_time + std::chrono::milliseconds(100);
    while (std::chrono::high_resolution_clock::now() < target) {
        __asm__ __volatile__ ("pause");
    }
    uint64_t end_tsc = rdtscp();
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
    return static_cast<double>(end_tsc - start_tsc) / duration;
}
#define USE_RDTSC 1
#else
inline uint64_t rdtscp() { return 0; }
inline void cpuid_serialize() {}
double calibrate_tsc_frequency() { return 1.0; }
#define USE_RDTSC 0
#endif

struct LatencyStats {
    double min_ns, max_ns, mean_ns, median_ns;
    double p90_ns, p95_ns, p99_ns, p99_9_ns, p99_99_ns;
    double std_dev_ns, throughput_ops;
    size_t sample_count;
    
    void print(const std::string& test_name) const {
        std::cout << "\n╔══════════════════════════════════════════════════════════════════╗\n";
        std::cout << "║ " << std::left << std::setw(64) << test_name << " ║\n";
        std::cout << "╠══════════════════════════════════════════════════════════════════╣\n";
        std::cout << "║ Samples: " << std::setw(56) << sample_count << "║\n";
        std::cout << "╠══════════════════════════════════════════════════════════════════╣\n";
        std::cout << "║ LATENCY (nanoseconds)                                            ║\n";
        std::cout << std::fixed << std::setprecision(1);
        std::cout << "║   Min:     " << std::setw(12) << min_ns << std::setw(40) << " " << "║\n";
        std::cout << "║   Mean:    " << std::setw(12) << mean_ns << std::setw(40) << " " << "║\n";
        std::cout << "║   Median:  " << std::setw(12) << median_ns << " (P50)" << std::setw(33) << " " << "║\n";
        std::cout << "║   P90:     " << std::setw(12) << p90_ns << std::setw(40) << " " << "║\n";
        std::cout << "║   P95:     " << std::setw(12) << p95_ns << std::setw(40) << " " << "║\n";
        std::cout << "║   P99:     " << std::setw(12) << p99_ns << " ◀ CRITICAL" << std::setw(29) << " " << "║\n";
        std::cout << "║   P99.9:   " << std::setw(12) << p99_9_ns << " ◀ TAIL" << std::setw(32) << " " << "║\n";
        std::cout << "║   P99.99:  " << std::setw(12) << p99_99_ns << std::setw(40) << " " << "║\n";
        std::cout << "║   Max:     " << std::setw(12) << max_ns << std::setw(40) << " " << "║\n";
        std::cout << "║   Std Dev: " << std::setw(12) << std_dev_ns << std::setw(40) << " " << "║\n";
        std::cout << "╠══════════════════════════════════════════════════════════════════╣\n";
        std::cout << "║ THROUGHPUT: " << std::setw(12) << std::setprecision(0) << throughput_ops 
                  << " ops/sec (" << std::setprecision(2) << throughput_ops/1e6 << " M ops/s)" 
                  << std::setw(17) << " " << "║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════════╝\n";
    }
};

LatencyStats calculate_stats(std::vector<double>& latencies, double total_ns) {
    LatencyStats s{};
    s.sample_count = latencies.size();
    if (latencies.empty()) return s;
    
    std::sort(latencies.begin(), latencies.end());
    size_t n = latencies.size();
    
    s.min_ns = latencies.front();
    s.max_ns = latencies.back();
    s.mean_ns = std::accumulate(latencies.begin(), latencies.end(), 0.0) / n;
    
    auto pct = [&](double p) {
        double idx = (p / 100.0) * (n - 1);
        size_t lo = static_cast<size_t>(idx);
        size_t hi = std::min(lo + 1, n - 1);
        double f = idx - lo;
        return latencies[lo] * (1 - f) + latencies[hi] * f;
    };
    
    s.median_ns = pct(50);
    s.p90_ns = pct(90);
    s.p95_ns = pct(95);
    s.p99_ns = pct(99);
    s.p99_9_ns = pct(99.9);
    s.p99_99_ns = pct(99.99);
    
    double sq_sum = 0;
    for (double lat : latencies) sq_sum += (lat - s.mean_ns) * (lat - s.mean_ns);
    s.std_dev_ns = std::sqrt(sq_sum / n);
    s.throughput_ops = (n / total_ns) * 1e9;
    return s;
}

struct MessageBuffer {
    std::vector<uint8_t> data;
    MsgType type;
    uint64_t timestamp;
};

std::vector<MessageBuffer> load_binary_file(const std::string& filename) {
    std::vector<MessageBuffer> messages;
    
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "Error: Cannot open file: " << filename << "\n";
        return messages;
    }
    
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::cout << "Loading " << filename << " (" << file_size << " bytes)...\n";

    std::vector<uint8_t> file_data(file_size);
    file.read(reinterpret_cast<char*>(file_data.data()), file_size);
    file.close();
    
    size_t offset = 0;
    size_t msg_count = 0;
    
    while (offset + sizeof(MsgHeader) <= file_size) {
        const MsgHeader* header = reinterpret_cast<const MsgHeader*>(file_data.data() + offset);
        
        uint16_t msg_len = header->length;
        if (msg_len == 0 || offset + msg_len > file_size) {
            std::cerr << "Warning: Invalid message length at offset " << offset << "\n";
            break;
        }
        
        MessageBuffer msg;
        msg.data.resize(msg_len);
        std::memcpy(msg.data.data(), file_data.data() + offset, msg_len);
        msg.type = header->type;
        msg.timestamp = header->timestamp;
        
        messages.push_back(std::move(msg));
        offset += msg_len;
        msg_count++;
    }
    
    std::cout << "Loaded " << msg_count << " messages\n";
    
    size_t add_count = 0, cancel_count = 0, modify_count = 0, execute_count = 0, other_count = 0;
    for (const auto& msg : messages) {
        switch (msg.type) {
            case MsgType::ADD_ORDER: add_count++; break;
            case MsgType::CANCEL_ORDER: cancel_count++; break;
            case MsgType::MODIFY_ORDER: modify_count++; break;
            case MsgType::EXECUTE: execute_count++; break;
            default: other_count++; break;
        }
    }
    
    std::cout << "Message distribution:\n";
    std::cout << "  ADD_ORDER:    " << add_count << "\n";
    std::cout << "  CANCEL_ORDER: " << cancel_count << "\n";
    std::cout << "  MODIFY_ORDER: " << modify_count << "\n";
    std::cout << "  EXECUTE:      " << execute_count << "\n";
    std::cout << "  Other:        " << other_count << "\n";
    
    return messages;
}

inline void process_message(OptimizedOrderBook& book, const MessageBuffer& msg) {
    switch (msg.type) {
        case MsgType::ADD_ORDER: {
            const MsgAddOrder* m = reinterpret_cast<const MsgAddOrder*>(msg.data.data());
            book.add_order_no_lock(m->order_id, m->side == Side::BUY, m->price, m->quantity, 
                                   static_cast<uint32_t>(m->user_id));
            break;
        }
        case MsgType::CANCEL_ORDER: {
            const MsgCancel* m = reinterpret_cast<const MsgCancel*>(msg.data.data());
            book.cancel_order_no_lock(m->order_id);
            break;
        }
        case MsgType::MODIFY_ORDER: {
            const MsgModify* m = reinterpret_cast<const MsgModify*>(msg.data.data());
            book.cancel_order_no_lock(m->order_id);
            break;
        }
        case MsgType::EXECUTE: {
            const MsgExecute* m = reinterpret_cast<const MsgExecute*>(msg.data.data());
            book.add_order_no_lock(m->order_id, m->side == Side::BUY, m->price, m->quantity,
                                   static_cast<uint32_t>(m->user_id));
            break;
        }
        case MsgType::ADD_ICEBERG: {
            const MsgAddIceberg* m = reinterpret_cast<const MsgAddIceberg*>(msg.data.data());
            book.add_order_no_lock(m->order_id, m->side == Side::BUY, m->price, 
                                   m->total_quantity, static_cast<uint32_t>(m->user_id));
            break;
        }
        default:

            break;
    }
}

LatencyStats run_latency_benchmark(const std::vector<MessageBuffer>& messages, 
                                    double tsc_freq,
                                    size_t warmup_count = 100000) {
    
    auto book = std::make_unique<OptimizedOrderBook>(2'000'000);
    book->use_ring_buffer_output(false);
    book->set_benchmark_mode(true);
    
    size_t actual_warmup = std::min(warmup_count, messages.size());
    size_t bench_start = actual_warmup;
    size_t bench_count = messages.size() - bench_start;
    
    std::cout << "\nRunning latency benchmark:\n";
    std::cout << "  Warmup messages: " << actual_warmup << "\n";
    std::cout << "  Benchmark messages: " << bench_count << "\n";

    for (size_t i = 0; i < actual_warmup; ++i) {
        process_message(*book, messages[i]);
    }
    
    std::vector<double> latencies;
    latencies.reserve(bench_count);
    
#if USE_RDTSC
    cpuid_serialize();
    uint64_t total_start = rdtscp();
    
    for (size_t i = bench_start; i < messages.size(); ++i) {
        uint64_t start = rdtscp();
        process_message(*book, messages[i]);
        uint64_t end = rdtscp();
        latencies.push_back(static_cast<double>(end - start) / tsc_freq);
    }
    
    uint64_t total_end = rdtscp();
    double total_ns = static_cast<double>(total_end - total_start) / tsc_freq;
#else
    auto total_start = std::chrono::high_resolution_clock::now();
    
    for (size_t i = bench_start; i < messages.size(); ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        process_message(*book, messages[i]);
        auto end = std::chrono::high_resolution_clock::now();
        latencies.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
    }
    
    auto total_end = std::chrono::high_resolution_clock::now();
    double total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(total_end - total_start).count();
#endif
    
    std::cout << "  Final book state:\n";
    std::cout << "    Active orders: " << book->order_count() << "\n";
    std::cout << "    Bid levels: " << book->bid_levels() << "\n";
    std::cout << "    Ask levels: " << book->ask_levels() << "\n";
    std::cout << "    Trades: " << book->trades_executed() << "\n";
    
    return calculate_stats(latencies, total_ns);
}

double run_throughput_benchmark(const std::vector<MessageBuffer>& messages) {
    auto book = std::make_unique<OptimizedOrderBook>(2'000'000);
    book->use_ring_buffer_output(false);
    book->set_benchmark_mode(true);
    
    std::cout << "\nRunning pure throughput benchmark (" << messages.size() << " messages)...\n";
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (const auto& msg : messages) {
        process_message(*book, msg);
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    
    double duration_s = std::chrono::duration<double>(end - start).count();
    double throughput = messages.size() / duration_s;
    
    std::cout << "  Time: " << std::fixed << std::setprecision(3) << duration_s << " s\n";
    std::cout << "  Throughput: " << std::setprecision(0) << throughput << " msgs/sec\n";
    std::cout << "  Throughput: " << std::setprecision(2) << throughput / 1e6 << " M msgs/sec\n";
    
    return throughput;
}

int main(int argc, char* argv[]) {
    std::cout << R"(
╔══════════════════════════════════════════════════════════════════╗
║          TITANLOB BENCHMARK - Binary File Replay                 ║
║       High-Frequency Trading Limit Order Book Engine             ║
╚══════════════════════════════════════════════════════════════════╝
)" << std::endl;

    std::string filename = "btc_l3.dat";
    if (argc > 1) {
        filename = argv[1];
    }
    
    std::cout << "System Configuration:\n";
    std::cout << "  Order struct size:    " << sizeof(Order) << " bytes\n";
    std::cout << "  PriceLevel size:      " << sizeof(PriceLevel) << " bytes\n";
    std::cout << "  MAX_PRICE_LEVELS:     " << MAX_PRICE_LEVELS << " (~$" << MAX_PRICE_LEVELS/100 << " range in cents)\n";
    std::cout << "  Price array memory:   " << (sizeof(PriceLevel) * MAX_PRICE_LEVELS * 2 / 1024 / 1024) << " MB (heap allocated)\n";
    std::cout << "  Bitmap memory:        " << (BITMAP_WORDS * 8 * 2 / 1024 / 1024) << " MB\n";
    
#if USE_RDTSC
    std::cout << "  Timer:                RDTSCP (high precision)\n";
    std::cout << "  Calibrating TSC...\n";
    double tsc_freq = calibrate_tsc_frequency();
    std::cout << "  TSC frequency:        " << std::fixed << std::setprecision(3) << tsc_freq << " cycles/ns\n";
#else
    std::cout << "  Timer:                std::chrono\n";
    double tsc_freq = 1.0;
#endif
    
    std::cout << "\n";
    
    auto messages = load_binary_file(filename);
    if (messages.empty()) {
        std::cerr << "No messages loaded. Exiting.\n";
        return 1;
    }

    auto latency_stats = run_latency_benchmark(messages, tsc_freq);
    latency_stats.print("BTC L3 Message Replay - Per-Message Latency");
    
    double throughput = run_throughput_benchmark(messages);
    
    std::cout << "\n═══════════════════════════════════════════════════════════════════\n";
    std::cout << " SUMMARY\n";
    std::cout << "═══════════════════════════════════════════════════════════════════\n\n";
    
    std::cout << "TitanLOB - BTC L3 Data Replay Results\n\n";
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "• Messages processed:   " << messages.size() << "\n";
    std::cout << "• Median Latency (P50): " << latency_stats.median_ns << " ns\n";
    std::cout << "• P99 Latency:          " << latency_stats.p99_ns << " ns\n";
    std::cout << "• P99.9 Latency:        " << latency_stats.p99_9_ns << " ns\n";
    std::cout << std::setprecision(2);
    std::cout << "• Pure Throughput:      " << throughput / 1e6 << " M msgs/sec\n";
    
    return 0;
}
