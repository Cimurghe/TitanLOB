#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <csignal>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <arpa/inet.h>
#include <fstream>
#include <chrono>
#include <memory>

#include "protocol.h"
#include "order_book.h"
#include "titan_ws_server.h"

#define BRIDGE_PORT 9000
#define DASHBOARD_PORT 8080
#define BROADCAST_INTERVAL_MS 50

std::atomic<bool> running(true);
void signal_handler(int) { running = false; }

int setup_tcp_server() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("Socket creation failed"); exit(1); }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in servaddr;
    std::memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(BRIDGE_PORT);

    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("Bind failed (Is another Titan instance running?)");
        exit(1);
    }
    
    if (listen(sockfd, 1) < 0) {
        perror("Listen failed");
        exit(1);
    }
    
    std::cout << "[TITAN] Live Mode: TCP Server listening on port " << BRIDGE_PORT << "..." << std::endl;
    return sockfd;
}

std::string build_book_snapshot(OptimizedOrderBook& book) {
    JsonBuilder json;

    auto bids = book.get_bids_snapshot();
    auto asks = book.get_asks_snapshot();
    
    json.begin_object();

    json.key("type").value("book_snapshot");
    json.key("timestamp").value(static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    ));

    json.key("best_bid").value(book.get_best_bid());
    json.key("best_ask").value(book.get_best_ask());

    json.key("bid_levels").value(static_cast<int64_t>(book.bid_levels()));
    json.key("ask_levels").value(static_cast<int64_t>(book.ask_levels()));
    json.key("order_count").value(static_cast<int64_t>(book.order_count()));
    json.key("trades_executed").value(static_cast<int64_t>(book.trades_executed()));

    json.key("bids").begin_array();
    int count = 0;
    for (const auto& [price, volume] : bids) {
        if (count++ >= 10) break;
        json.array_item().begin_array();
        json.array_item().value(price);
        json.array_item().value(volume);
        json.end_array();
    }
    json.end_array();

    json.key("asks").begin_array();
    count = 0;
    for (const auto& [price, volume] : asks) {
        if (count++ >= 10) break;
        json.array_item().begin_array();
        json.array_item().value(price);
        json.array_item().value(volume);
        json.end_array();
    }
    json.end_array();
    
    json.end_object();
    
    return json.str();
}

void dispatch_message(OptimizedOrderBook& book, const uint8_t* buffer, size_t len) {
    if (len < sizeof(MsgHeader)) return;
    
    const MsgHeader* header = reinterpret_cast<const MsgHeader*>(buffer);
    
    switch (header->type) {
        case MsgType::ADD_ORDER: {
            if (len < sizeof(MsgAddOrder)) return;
            const MsgAddOrder* msg = msg_cast<MsgAddOrder>(buffer);
            book.add_order(
                msg->order_id,
                msg->side == Side::BUY,
                msg->price,
                msg->quantity,
                static_cast<uint32_t>(msg->user_id)
            );
            break;
        }
        case MsgType::CANCEL_ORDER: {
            if (len < sizeof(MsgCancel)) return;
            const MsgCancel* msg = msg_cast<MsgCancel>(buffer);
            book.cancel_order(msg->order_id);
            break;
        }
        case MsgType::MODIFY_ORDER: {
            if (len < sizeof(MsgModify)) return;
            const MsgModify* msg = msg_cast<MsgModify>(buffer);

            book.cancel_order(msg->order_id);
            break;
        }
        case MsgType::HEARTBEAT:

            break;
        case MsgType::RESET:

            break;
        default:

            break;
    }
}

void dispatch_message_no_lock(OptimizedOrderBook& book, const uint8_t* buffer, size_t len) {
    if (len < sizeof(MsgHeader)) return;
    
    const MsgHeader* header = reinterpret_cast<const MsgHeader*>(buffer);
    
    switch (header->type) {
        case MsgType::ADD_ORDER: {
            if (len < sizeof(MsgAddOrder)) return;
            const MsgAddOrder* msg = msg_cast<MsgAddOrder>(buffer);
            book.add_order_no_lock(
                msg->order_id,
                msg->side == Side::BUY,
                msg->price,
                msg->quantity,
                static_cast<uint32_t>(msg->user_id)
            );
            break;
        }
        case MsgType::CANCEL_ORDER: {
            if (len < sizeof(MsgCancel)) return;
            const MsgCancel* msg = msg_cast<MsgCancel>(buffer);
            book.cancel_order_no_lock(msg->order_id);
            break;
        }
        default:
            break;
    }
}

int main() {
    signal(SIGINT, signal_handler);

    std::cout << "[TITAN] Allocating order book on heap..." << std::endl;
    auto book = std::make_unique<OptimizedOrderBook>(33554432);
    std::cout << "[TITAN] Order book allocated successfully." << std::endl;

    TitanWebSocketServer ws_server(DASHBOARD_PORT);
    ws_server.start();
    std::cout << "[TITAN] Dashboard WebSocket server started on port " << DASHBOARD_PORT << std::endl;

#ifdef REPLAY_MODE

    std::cout << "[TITAN] Starting Replay Mode: " << REPLAY_MODE << std::endl;
    std::ifstream file(REPLAY_MODE, std::ios::binary | std::ios::ate);
    if (!file.is_open()) { 
        std::cerr << "File not found: " << REPLAY_MODE << std::endl; 
        return 1; 
    }
    
    std::streamsize file_size = file.tellg();
    file.seekg(0, std::ios::beg);
    
    std::vector<uint8_t> data(file_size);
    file.read(reinterpret_cast<char*>(data.data()), file_size);
    file.close();
    
    std::cout << "[TITAN] Loaded " << file_size << " bytes. Parsing messages..." << std::endl;

    auto start = std::chrono::high_resolution_clock::now();
    auto last_broadcast = start;
    
    size_t offset = 0;
    size_t msg_count = 0;
    
    while (offset < data.size() && running) {

        if (offset + sizeof(MsgHeader) > data.size()) break;
        
        const MsgHeader* header = reinterpret_cast<const MsgHeader*>(data.data() + offset);
        uint16_t msg_len = header->length;
        
        if (msg_len == 0 || offset + msg_len > data.size()) break;
        
        dispatch_message_no_lock(*book, data.data() + offset, msg_len);
        
        offset += msg_len;
        msg_count++;

        auto now = std::chrono::high_resolution_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_broadcast).count() >= BROADCAST_INTERVAL_MS) {
            std::string json = build_book_snapshot(*book);
            ws_server.broadcast(json);
            last_broadcast = now;
        }
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(end - start).count();
    
    std::cout << "Processed " << msg_count << " messages in " << ms << " ms (" 
              << (size_t)(msg_count / (ms/1000.0)) << " msg/sec)" << std::endl;

#else

    int server_fd = setup_tcp_server();

    int flags = fcntl(server_fd, F_GETFL, 0);
    fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
    
    while (running) {
        std::cout << "[TITAN] Waiting for bridge connection..." << std::endl;
        
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = -1;

        auto last_broadcast = std::chrono::steady_clock::now();
        while (running && client_fd < 0) {
            client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
            
            if (client_fd < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("Accept failed");
                    break;
                }

                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_broadcast).count() >= BROADCAST_INTERVAL_MS) {
                    std::string json = build_book_snapshot(*book);
                    ws_server.broadcast(json);
                    last_broadcast = now;
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        
        if (client_fd < 0) continue;

        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        flags = fcntl(client_fd, F_GETFL, 0);
        fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
        
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "[TITAN] Bridge connected from " << client_ip << std::endl;

        uint8_t buffer[4096];
        size_t buffer_used = 0;
        uint64_t msg_count = 0;
        
        last_broadcast = std::chrono::steady_clock::now();
        
        while (running) {

            ssize_t bytes_read = recv(client_fd, buffer + buffer_used, 
                                      sizeof(buffer) - buffer_used, 0);
            
            if (bytes_read > 0) {
                buffer_used += bytes_read;

                size_t offset = 0;
                while (offset + sizeof(MsgHeader) <= buffer_used) {
                    const MsgHeader* header = reinterpret_cast<const MsgHeader*>(buffer + offset);
                    uint16_t msg_len = header->length;

                    if (msg_len < sizeof(MsgHeader) || msg_len > 256) {
                        std::cerr << "[TITAN] Invalid message length: " << msg_len << std::endl;
                        offset++;
                        continue;
                    }

                    if (offset + msg_len > buffer_used) break;

                    dispatch_message(*book, buffer + offset, msg_len);
                    
                    offset += msg_len;
                    msg_count++;
                }

                if (offset > 0) {
                    buffer_used -= offset;
                    if (buffer_used > 0) {
                        std::memmove(buffer, buffer + offset, buffer_used);
                    }
                }
            } else if (bytes_read == 0) {

                std::cout << "\n[TITAN] Bridge disconnected." << std::endl;
                break;
            } else {

                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("\n[TITAN] Recv error");
                    break;
                }
            }

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_broadcast).count() >= BROADCAST_INTERVAL_MS) {
                std::string json = build_book_snapshot(*book);
                ws_server.broadcast(json);
                last_broadcast = now;

                if (msg_count % 5000 < 100) {
                    std::cout << "\r[TITAN] Orders: " << msg_count 
                              << " | Bid: " << book->get_best_bid() 
                              << " | Ask: " << book->get_best_ask()
                              << " | WS Clients: " << ws_server.client_count() << std::flush;
                }
            }

            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        
        close(client_fd);
        std::cout << "[TITAN] Processed " << msg_count << " messages from bridge." << std::endl;
    }
    
    close(server_fd);
#endif

    std::cout << "\n[TITAN] Stopping WebSocket server..." << std::endl;
    ws_server.stop();
    std::cout << "[TITAN] Shutting down." << std::endl;
    return 0;
}