#ifndef GATEWAY_H
#define GATEWAY_H

#include <cstdint>
#include <cstring>
#include <thread>
#include <atomic>
#include <iostream>
#include "protocol.h"
#include "order_book.h"

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef SOCKET socket_t;
    #define INVALID_SOCKET_VALUE INVALID_SOCKET
    #define SOCKET_ERROR_VALUE SOCKET_ERROR
    #define close_socket closesocket
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    typedef int socket_t;
    #define INVALID_SOCKET_VALUE -1
    #define SOCKET_ERROR_VALUE -1
    #define close_socket close
#endif

class TcpGateway {
private:
    uint16_t port_;
    std::atomic<bool> running_;
    std::thread listener_thread_;
    OptimizedOrderBook& order_book_;
    
    static constexpr size_t MAX_MESSAGE_SIZE = 1024;
    
    bool recv_exact(socket_t client_socket, uint8_t* buffer, size_t num_bytes) {
        size_t total_received = 0;
        
        while (total_received < num_bytes) {
            int bytes_received = recv(client_socket, 
                                     reinterpret_cast<char*>(buffer + total_received),
                                     static_cast<int>(num_bytes - total_received), 
                                     0);
            
            if (bytes_received <= 0) {
                if (bytes_received == 0) {
                    std::cerr << "[Gateway] Client disconnected\n";
                } else {
                    std::cerr << "[Gateway] Recv error\n";
                }
                return false;
            }
            
            total_received += bytes_received;
        }
        
        return true;
    }
    
    void handle_client(socket_t client_socket) {
        std::cout << "[Gateway] Client connected\n";
        
        alignas(8) uint8_t message_buffer[MAX_MESSAGE_SIZE];
        
        while (running_.load(std::memory_order_relaxed)) {
            if (!recv_exact(client_socket, message_buffer, sizeof(MsgHeader))) {
                break;
            }
            
            const MsgHeader* header = reinterpret_cast<const MsgHeader*>(message_buffer);
            uint16_t total_length = header->length;
            
            if (total_length < sizeof(MsgHeader) || total_length > MAX_MESSAGE_SIZE) {
                std::cerr << "[Gateway] Invalid message length: " << total_length << "\n";
                break;
            }
            
            size_t remaining_bytes = total_length - sizeof(MsgHeader);
            if (remaining_bytes > 0) {
                if (!recv_exact(client_socket, message_buffer + sizeof(MsgHeader), remaining_bytes)) {
                    break;
                }
            }
            
            process_message(header);
        }
        
        close_socket(client_socket);
        std::cout << "[Gateway] Client disconnected\n";
    }
    
    void process_message(const MsgHeader* header) {
        switch (header->type) {
            case MsgType::ADD_ORDER: {
                const auto* msg = msg_cast<MsgAddOrder>(header);
                order_book_.add_order(msg->order_id, side_to_bool(msg->side), 
                                     msg->price, msg->quantity, 
                                     static_cast<uint32_t>(msg->user_id));
                break;
            }
            
            case MsgType::ADD_ICEBERG: {
                const auto* msg = msg_cast<MsgAddIceberg>(header);
                order_book_.add_order(msg->order_id, side_to_bool(msg->side),
                                     msg->price, msg->visible_quantity,
                                     static_cast<uint32_t>(msg->user_id));
                break;
            }
            
            case MsgType::ADD_AON: {
                const auto* msg = msg_cast<MsgAddAON>(header);
                order_book_.add_order(msg->order_id, side_to_bool(msg->side),
                                     msg->price, msg->quantity,
                                     static_cast<uint32_t>(msg->user_id));
                break;
            }
            
            case MsgType::CANCEL_ORDER: {
                const auto* msg = msg_cast<MsgCancel>(header);
                order_book_.cancel_order(msg->order_id);
                break;
            }
            
            case MsgType::MODIFY_ORDER: {
                const auto* msg = msg_cast<MsgModify>(header);
                order_book_.cancel_order(msg->order_id);
                break;
            }
            
            case MsgType::EXECUTE: {
                const auto* msg = msg_cast<MsgExecute>(header);
                order_book_.match_order(msg->order_id, side_to_bool(msg->side),
                                       msg->price, msg->quantity,
                                       tif_from_protocol(msg->time_in_force));
                break;
            }
            
            case MsgType::HEARTBEAT:
                break;
                
            case MsgType::RESET:
                break;
                
            default:
                std::cerr << "[Gateway] Unknown message type: " 
                         << static_cast<int>(header->type) << "\n";
                break;
        }
    }
    
    void listener_loop() {
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "[Gateway] WSAStartup failed\n";
            return;
        }
#endif
        
        socket_t listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_socket == INVALID_SOCKET_VALUE) {
            std::cerr << "[Gateway] Failed to create socket\n";
#ifdef _WIN32
            WSACleanup();
#endif
            return;
        }
        
        int opt = 1;
        setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, 
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
        
        sockaddr_in server_addr;
        std::memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port_);
        
        if (bind(listen_socket, reinterpret_cast<sockaddr*>(&server_addr), 
                 sizeof(server_addr)) == SOCKET_ERROR_VALUE) {
            std::cerr << "[Gateway] Bind failed on port " << port_ << "\n";
            close_socket(listen_socket);
#ifdef _WIN32
            WSACleanup();
#endif
            return;
        }
        
        if (listen(listen_socket, SOMAXCONN) == SOCKET_ERROR_VALUE) {
            std::cerr << "[Gateway] Listen failed\n";
            close_socket(listen_socket);
#ifdef _WIN32
            WSACleanup();
#endif
            return;
        }
        
        std::cout << "[Gateway] Listening on port " << port_ << "\n";
        
        while (running_.load(std::memory_order_relaxed)) {
            sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            socket_t client_socket = accept(listen_socket, 
                                           reinterpret_cast<sockaddr*>(&client_addr),
                                           &client_len);
            
            if (client_socket == INVALID_SOCKET_VALUE) {
                if (running_.load(std::memory_order_relaxed)) {
                    std::cerr << "[Gateway] Accept failed\n";
                }
                continue;
            }
            
            handle_client(client_socket);
        }
        
        close_socket(listen_socket);
        
#ifdef _WIN32
        WSACleanup();
#endif
        
        std::cout << "[Gateway] Listener stopped\n";
    }

public:
    explicit TcpGateway(OptimizedOrderBook& order_book, uint16_t port = 9000)
        : port_(port)
        , running_(false)
        , order_book_(order_book)
    {
    }
    
    ~TcpGateway() {
        stop();
    }
    
    TcpGateway(const TcpGateway&) = delete;
    TcpGateway& operator=(const TcpGateway&) = delete;
    
    void start() {
        if (running_.load(std::memory_order_relaxed)) {
            std::cerr << "[Gateway] Already running\n";
            return;
        }
        
        running_.store(true, std::memory_order_relaxed);
        listener_thread_ = std::thread(&TcpGateway::listener_loop, this);
        
        std::cout << "[Gateway] Started\n";
    }
    
    void stop() {
        if (!running_.load(std::memory_order_relaxed)) {
            return;
        }
        
        std::cout << "[Gateway] Stopping...\n";
        running_.store(false, std::memory_order_relaxed);
        
        if (listener_thread_.joinable()) {
            listener_thread_.detach(); 
        }
        
        std::cout << "[Gateway] Stopped\n";
    }
    
    bool is_running() const {
        return running_.load(std::memory_order_relaxed);
    }
    
    uint16_t get_port() const {
        return port_;
    }
};

#endif