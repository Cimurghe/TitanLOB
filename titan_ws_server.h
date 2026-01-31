#ifndef TITAN_WS_SERVER_H
#define TITAN_WS_SERVER_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_set>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <functional>
#include <algorithm>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef SOCKET socket_t;
    #define INVALID_SOCKET_VAL INVALID_SOCKET
    #define CLOSE_SOCKET closesocket
    #define SOCKET_ERROR_VAL SOCKET_ERROR
#else
    #include <sys/socket.h>
    #include <sys/select.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    typedef int socket_t;
    #define INVALID_SOCKET_VAL -1
    #define CLOSE_SOCKET close
    #define SOCKET_ERROR_VAL -1
#endif

namespace sha1 {
    inline uint32_t left_rotate(uint32_t value, size_t count) {
        return (value << count) | (value >> (32 - count));
    }
    
    inline void compute(const uint8_t* message, size_t len, uint8_t* digest) {
        uint32_t h0 = 0x67452301;
        uint32_t h1 = 0xEFCDAB89;
        uint32_t h2 = 0x98BADCFE;
        uint32_t h3 = 0x10325476;
        uint32_t h4 = 0xC3D2E1F0;
        
        uint64_t ml = len * 8;
        size_t padded_len = ((len + 8) / 64 + 1) * 64;
        std::vector<uint8_t> padded(padded_len, 0);
        memcpy(padded.data(), message, len);
        padded[len] = 0x80;
        
        for (int i = 0; i < 8; i++) {
            padded[padded_len - 1 - i] = (ml >> (i * 8)) & 0xFF;
        }
        
        for (size_t chunk = 0; chunk < padded_len; chunk += 64) {
            uint32_t w[80];
            for (int i = 0; i < 16; i++) {
                w[i] = (padded[chunk + i*4] << 24) |
                       (padded[chunk + i*4 + 1] << 16) |
                       (padded[chunk + i*4 + 2] << 8) |
                       (padded[chunk + i*4 + 3]);
            }
            for (int i = 16; i < 80; i++) {
                w[i] = left_rotate(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
            }
            
            uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
            
            for (int i = 0; i < 80; i++) {
                uint32_t f, k;
                if (i < 20) {
                    f = (b & c) | ((~b) & d);
                    k = 0x5A827999;
                } else if (i < 40) {
                    f = b ^ c ^ d;
                    k = 0x6ED9EBA1;
                } else if (i < 60) {
                    f = (b & c) | (b & d) | (c & d);
                    k = 0x8F1BBCDC;
                } else {
                    f = b ^ c ^ d;
                    k = 0xCA62C1D6;
                }
                
                uint32_t temp = left_rotate(a, 5) + f + e + k + w[i];
                e = d;
                d = c;
                c = left_rotate(b, 30);
                b = a;
                a = temp;
            }
            
            h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
        }
        
        digest[0] = (h0 >> 24) & 0xFF; digest[1] = (h0 >> 16) & 0xFF;
        digest[2] = (h0 >> 8) & 0xFF;  digest[3] = h0 & 0xFF;
        digest[4] = (h1 >> 24) & 0xFF; digest[5] = (h1 >> 16) & 0xFF;
        digest[6] = (h1 >> 8) & 0xFF;  digest[7] = h1 & 0xFF;
        digest[8] = (h2 >> 24) & 0xFF; digest[9] = (h2 >> 16) & 0xFF;
        digest[10] = (h2 >> 8) & 0xFF; digest[11] = h2 & 0xFF;
        digest[12] = (h3 >> 24) & 0xFF; digest[13] = (h3 >> 16) & 0xFF;
        digest[14] = (h3 >> 8) & 0xFF; digest[15] = h3 & 0xFF;
        digest[16] = (h4 >> 24) & 0xFF; digest[17] = (h4 >> 16) & 0xFF;
        digest[18] = (h4 >> 8) & 0xFF; digest[19] = h4 & 0xFF;
    }
}

namespace base64 {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    inline std::string encode(const uint8_t* data, size_t len) {
        std::string result;
        result.reserve(((len + 2) / 3) * 4);
        
        for (size_t i = 0; i < len; i += 3) {
            uint32_t n = (data[i] << 16);
            if (i + 1 < len) n |= (data[i + 1] << 8);
            if (i + 2 < len) n |= data[i + 2];
            
            result += chars[(n >> 18) & 0x3F];
            result += chars[(n >> 12) & 0x3F];
            result += (i + 1 < len) ? chars[(n >> 6) & 0x3F] : '=';
            result += (i + 2 < len) ? chars[n & 0x3F] : '=';
        }
        return result;
    }
}

class TitanWebSocketServer {
public:
    using MessageCallback = std::function<void(socket_t, const std::string&)>;
    
private:
    uint16_t port_;
    socket_t listen_socket_ = INVALID_SOCKET_VAL;
    std::atomic<bool> running_{false};
    std::thread server_thread_;
    
    std::mutex clients_mutex_;
    std::unordered_set<socket_t> clients_;
    
    MessageCallback on_message_;
    
    static constexpr const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    
    bool set_non_blocking(socket_t sock) {
#ifdef _WIN32
        u_long mode = 1;
        return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
        int flags = fcntl(sock, F_GETFL, 0);
        return fcntl(sock, F_SETFL, flags | O_NONBLOCK) != -1;
#endif
    }
    
    void set_tcp_nodelay(socket_t sock) {
        int flag = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
    }
    
    std::string extract_header(const std::string& request, const std::string& name) {
        std::string search = name + ": ";
        size_t pos = request.find(search);
        if (pos == std::string::npos) return "";
        
        size_t start = pos + search.length();
        size_t end = request.find("\r\n", start);
        if (end == std::string::npos) end = request.length();
        
        return request.substr(start, end - start);
    }
    
    bool perform_handshake(socket_t client) {
        char buffer[4096];
        int n = recv(client, buffer, sizeof(buffer) - 1, 0);
        if (n <= 0) return false;
        buffer[n] = '\0';
        
        std::string request(buffer);
        
        if (request.find("Upgrade: websocket") == std::string::npos &&
            request.find("Upgrade: WebSocket") == std::string::npos) {
            std::string http_response = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html\r\n"
                "Connection: close\r\n"
                "\r\n"
                "<!DOCTYPE html><html><body>"
                "<h1>TitanLOB WebSocket Server</h1>"
                "<p>Connect via WebSocket at ws://hostname:" + std::to_string(port_) + "</p>"
                "</body></html>";
            send(client, http_response.c_str(), http_response.length(), 0);
            return false;
        }
        
        std::string key = extract_header(request, "Sec-WebSocket-Key");
        if (key.empty()) return false;
        
        std::string combined = key + WS_GUID;
        uint8_t sha1_result[20];
        sha1::compute((const uint8_t*)combined.c_str(), combined.length(), sha1_result);
        std::string accept_key = base64::encode(sha1_result, 20);
        
        std::string response = 
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept_key + "\r\n"
            "\r\n";
        
        send(client, response.c_str(), response.length(), 0);
        return true;
    }
    
    std::vector<uint8_t> encode_frame(const std::string& message, uint8_t opcode = 0x01) {
        std::vector<uint8_t> frame;
        
        frame.push_back(0x80 | opcode);
        
        size_t len = message.length();
        if (len <= 125) {
            frame.push_back(static_cast<uint8_t>(len));
        } else if (len <= 65535) {
            frame.push_back(126);
            frame.push_back((len >> 8) & 0xFF);
            frame.push_back(len & 0xFF);
        } else {
            frame.push_back(127);
            for (int i = 7; i >= 0; i--) {
                frame.push_back((len >> (i * 8)) & 0xFF);
            }
        }
        
        frame.insert(frame.end(), message.begin(), message.end());
        
        return frame;
    }
    
    bool decode_frame(socket_t client, std::string& message) {
        uint8_t header[2];
        int n = recv(client, (char*)header, 2, 0);
        if (n != 2) return false;
        
        bool fin = (header[0] & 0x80) != 0;
        uint8_t opcode = header[0] & 0x0F;
        bool masked = (header[1] & 0x80) != 0;
        uint64_t payload_len = header[1] & 0x7F;
        
        if (opcode == 0x08) return false;
        
        if (payload_len == 126) {
            uint8_t ext[2];
            if (recv(client, (char*)ext, 2, 0) != 2) return false;
            payload_len = (ext[0] << 8) | ext[1];
        } else if (payload_len == 127) {
            uint8_t ext[8];
            if (recv(client, (char*)ext, 8, 0) != 8) return false;
            payload_len = 0;
            for (int i = 0; i < 8; i++) {
                payload_len = (payload_len << 8) | ext[i];
            }
        }
        
        uint8_t mask[4] = {0};
        if (masked) {
            if (recv(client, (char*)mask, 4, 0) != 4) return false;
        }
        
        if (payload_len > 0 && payload_len < 1024 * 1024) {
            std::vector<char> payload(payload_len);
            size_t received = 0;
            while (received < payload_len) {
                n = recv(client, payload.data() + received, payload_len - received, 0);
                if (n <= 0) return false;
                received += n;
            }
            
            if (masked) {
                for (size_t i = 0; i < payload_len; i++) {
                    payload[i] ^= mask[i % 4];
                }
            }
            
            message.assign(payload.begin(), payload.end());
        }
        
        return true;
    }
    
    void handle_client(socket_t client) {
        if (!perform_handshake(client)) {
            CLOSE_SOCKET(client);
            return;
        }
        
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients_.insert(client);
        }
        
        printf("[WS] Client connected (fd=%d)\n", (int)client);
        
        set_non_blocking(client);
        
        while (running_) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(client, &read_fds);
            
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000;
            
            int result = select(client + 1, &read_fds, nullptr, nullptr, &tv);
            
            if (result > 0 && FD_ISSET(client, &read_fds)) {
                std::string message;
                if (!decode_frame(client, message)) {
                    break;
                }
                
                if (on_message_ && !message.empty()) {
                    on_message_(client, message);
                }
            } else if (result < 0) {
                break;
            }
        }
        
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients_.erase(client);
        }
        
        auto close_frame = encode_frame("", 0x08);
        send(client, (const char*)close_frame.data(), close_frame.size(), 0);
        
        CLOSE_SOCKET(client);
        printf("[WS] Client disconnected (fd=%d)\n", (int)client);
    }
    
    void server_loop() {
#ifdef _WIN32
        WSADATA wsa_data;
        WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
        
        listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_socket_ == INVALID_SOCKET_VAL) {
            fprintf(stderr, "[WS] Failed to create socket\n");
            return;
        }
        
        int opt = 1;
        setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
        
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);
        
        if (bind(listen_socket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR_VAL) {
            fprintf(stderr, "[WS] Failed to bind to port %d\n", port_);
            CLOSE_SOCKET(listen_socket_);
            return;
        }
        
        if (listen(listen_socket_, SOMAXCONN) == SOCKET_ERROR_VAL) {
            fprintf(stderr, "[WS] Failed to listen\n");
            CLOSE_SOCKET(listen_socket_);
            return;
        }
        
        printf("[WS] WebSocket server listening on port %d\n", port_);
        
        while (running_) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(listen_socket_, &read_fds);
            
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            
            int result = select(listen_socket_ + 1, &read_fds, nullptr, nullptr, &tv);
            
            if (result > 0 && FD_ISSET(listen_socket_, &read_fds)) {
                sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                socket_t client = accept(listen_socket_, (sockaddr*)&client_addr, &client_len);
                
                if (client != INVALID_SOCKET_VAL) {
                    set_tcp_nodelay(client);
                    
                    std::thread([this, client]() {
                        this->handle_client(client);
                    }).detach();
                }
            }
        }
        
        CLOSE_SOCKET(listen_socket_);
        
#ifdef _WIN32
        WSACleanup();
#endif
    }
    
public:
    explicit TitanWebSocketServer(uint16_t port = 8080) : port_(port) {}
    
    ~TitanWebSocketServer() {
        stop();
    }
    
    void start() {
        if (running_) return;
        
        running_ = true;
        server_thread_ = std::thread(&TitanWebSocketServer::server_loop, this);
    }
    
    void stop() {
        if (!running_) return;
        
        running_ = false;
        
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (socket_t client : clients_) {
                CLOSE_SOCKET(client);
            }
            clients_.clear();
        }
        
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }
    
    void broadcast(const std::string& message) {
        auto frame = encode_frame(message);
        
        std::lock_guard<std::mutex> lock(clients_mutex_);
        
        std::vector<socket_t> to_remove;
        
        for (socket_t client : clients_) {
            int sent = send(client, (const char*)frame.data(), frame.size(), 0);
            if (sent <= 0) {
                to_remove.push_back(client);
            }
        }
        
        for (socket_t client : to_remove) {
            clients_.erase(client);
            CLOSE_SOCKET(client);
        }
    }
    
    void send_to(socket_t client, const std::string& message) {
        auto frame = encode_frame(message);
        send(client, (const char*)frame.data(), frame.size(), 0);
    }
    
    void set_message_callback(MessageCallback callback) {
        on_message_ = std::move(callback);
    }
    
    size_t client_count() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(clients_mutex_));
        return clients_.size();
    }
    
    bool is_running() const { return running_; }
    uint16_t get_port() const { return port_; }
};

class JsonBuilder {
    std::ostringstream ss_;
    bool first_ = true;
    
public:
    JsonBuilder& begin_object() { ss_ << "{"; first_ = true; return *this; }
    JsonBuilder& end_object() { ss_ << "}"; return *this; }
    JsonBuilder& begin_array() { ss_ << "["; first_ = true; return *this; }
    JsonBuilder& end_array() { ss_ << "]"; return *this; }
    
    JsonBuilder& key(const char* k) {
        if (!first_) ss_ << ",";
        first_ = false;
        ss_ << "\"" << k << "\":";
        return *this;
    }
    
    JsonBuilder& value(const char* v) { ss_ << "\"" << v << "\""; return *this; }
    JsonBuilder& value(const std::string& v) { ss_ << "\"" << v << "\""; return *this; }
    JsonBuilder& value(int64_t v) { ss_ << v; return *this; }
    JsonBuilder& value(uint64_t v) { ss_ << v; return *this; }
    JsonBuilder& value(double v) { ss_ << std::fixed << std::setprecision(2) << v; return *this; }
    JsonBuilder& value(bool v) { ss_ << (v ? "true" : "false"); return *this; }
    
    JsonBuilder& array_item() {
        if (!first_) ss_ << ",";
        first_ = false;
        return *this;
    }
    
    std::string str() const { return ss_.str(); }
    void clear() { ss_.str(""); ss_.clear(); first_ = true; }
};

#endif
