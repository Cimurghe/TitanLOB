#ifndef TUI_H
#define TUI_H

#include <string>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
    #include <windows.h>
#else
    #ifdef __has_include
        #if __has_include(<sys/ioctl.h>)
            #include <sys/ioctl.h>
            #include <unistd.h>
            #define HAS_IOCTL 1
        #endif
    #endif
#endif

namespace tui {

constexpr int SCREEN_WIDTH = 120;
constexpr int SCREEN_HEIGHT = 30;
constexpr int COL1_WIDTH = 40;
constexpr int COL2_WIDTH = 40;
constexpr int COL3_WIDTH = 40;

constexpr const char* BLOCK_FULL   = "█";
constexpr const char* BLOCK_HIGH   = "▓";
constexpr const char* BLOCK_MED    = "▒";
constexpr const char* BLOCK_LOW    = "░";
constexpr const char* BLOCK_EMPTY  = " ";

constexpr const char* BOX_H  = "─";
constexpr const char* BOX_V  = "│";
constexpr const char* BOX_TL = "┌";
constexpr const char* BOX_TR = "┐";
constexpr const char* BOX_BL = "└";
constexpr const char* BOX_BR = "┘";
constexpr const char* BOX_T  = "┬";
constexpr const char* BOX_B  = "┴";
constexpr const char* BOX_L  = "├";
constexpr const char* BOX_R  = "┤";
constexpr const char* BOX_X  = "┼";

namespace ansi {
    constexpr const char* CLEAR_SCREEN = "\033[2J";
    constexpr const char* HOME = "\033[H";
    constexpr const char* HIDE_CURSOR = "\033[?25l";
    constexpr const char* SHOW_CURSOR = "\033[?25h";
    
    constexpr const char* FG_BLACK   = "\033[30m";
    constexpr const char* FG_RED     = "\033[31m";
    constexpr const char* FG_GREEN   = "\033[32m";
    constexpr const char* FG_YELLOW  = "\033[33m";
    constexpr const char* FG_BLUE    = "\033[34m";
    constexpr const char* FG_MAGENTA = "\033[35m";
    constexpr const char* FG_CYAN    = "\033[36m";
    constexpr const char* FG_WHITE   = "\033[37m";
    
    constexpr const char* FG_BRIGHT_RED    = "\033[91m";
    constexpr const char* FG_BRIGHT_GREEN  = "\033[92m";
    constexpr const char* FG_BRIGHT_YELLOW = "\033[93m";
    constexpr const char* FG_BRIGHT_CYAN   = "\033[96m";
    constexpr const char* FG_BRIGHT_WHITE  = "\033[97m";
    
    constexpr const char* BG_BLACK   = "\033[40m";
    constexpr const char* BG_RED     = "\033[41m";
    constexpr const char* BG_GREEN   = "\033[42m";
    constexpr const char* BG_BLUE    = "\033[44m";
    
    constexpr const char* BOLD      = "\033[1m";
    constexpr const char* DIM       = "\033[2m";
    constexpr const char* UNDERLINE = "\033[4m";
    constexpr const char* BLINK     = "\033[5m";
    constexpr const char* REVERSE   = "\033[7m";
    constexpr const char* RESET     = "\033[0m";
    
    inline void move_to(std::string& buf, int row, int col) {
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "\033[%d;%dH", row, col);
        buf += tmp;
    }
}

class FrameBuffer {
private:
    std::string buffer_;
    size_t estimated_size_;
    
public:
    FrameBuffer() : estimated_size_(SCREEN_WIDTH * SCREEN_HEIGHT * 4) {
        buffer_.reserve(estimated_size_);
    }
    
    void clear() {
        buffer_.clear();
        buffer_ += ansi::HOME;
    }
    
    void append(const char* str) {
        buffer_ += str;
    }
    
    void append(const std::string& str) {
        buffer_ += str;
    }
    
    void append_char(char c) {
        buffer_ += c;
    }
    
    void append_repeated(char c, int count) {
        buffer_.append(count, c);
    }
    
    void append_repeated(const char* str, int count) {
        for (int i = 0; i < count; ++i) {
            buffer_ += str;
        }
    }
    
    template<typename... Args>
    void appendf(const char* fmt, Args... args) {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), fmt, args...);
        buffer_ += tmp;
    }
    
    void at(int row, int col, const char* str) {
        ansi::move_to(buffer_, row, col);
        buffer_ += str;
    }
    
    void at(int row, int col, const std::string& str) {
        ansi::move_to(buffer_, row, col);
        buffer_ += str;
    }
    
    void newline() {
        buffer_ += '\n';
    }
    
    void flush() {
        fwrite(buffer_.data(), 1, buffer_.size(), stdout);
        fflush(stdout);
    }
    
    size_t size() const { return buffer_.size(); }
    const std::string& str() const { return buffer_; }
};

class DepthBar {
public:
    static std::string render(int64_t volume, int64_t max_volume, int width, bool is_bid) {
        if (max_volume <= 0 || volume <= 0) {
            return std::string(width, ' ');
        }
        
        double ratio = static_cast<double>(volume) / max_volume;
        int bar_len = static_cast<int>(ratio * width);
        if (bar_len > width) bar_len = width;
        if (bar_len < 0) bar_len = 0;
        
        std::string result;
        result.reserve(width * 4);
        
        if (is_bid) {
            result += ansi::FG_GREEN;
        } else {
            result += ansi::FG_RED;
        }
        
        for (int i = 0; i < bar_len; ++i) {
            double pos_ratio = static_cast<double>(i) / width;
            if (pos_ratio < 0.25) {
                result += BLOCK_FULL;
            } else if (pos_ratio < 0.5) {
                result += BLOCK_HIGH;
            } else if (pos_ratio < 0.75) {
                result += BLOCK_MED;
            } else {
                result += BLOCK_LOW;
            }
        }
        
        for (int i = bar_len; i < width; ++i) {
            result += ' ';
        }
        
        result += ansi::RESET;
        return result;
    }
    
    static std::string render_reversed(int64_t volume, int64_t max_volume, int width, bool is_bid) {
        if (max_volume <= 0 || volume <= 0) {
            return std::string(width, ' ');
        }
        
        double ratio = static_cast<double>(volume) / max_volume;
        int bar_len = static_cast<int>(ratio * width);
        if (bar_len > width) bar_len = width;
        if (bar_len < 0) bar_len = 0;
        
        std::string result;
        result.reserve(width * 4);
        
        if (is_bid) {
            result += ansi::FG_GREEN;
        } else {
            result += ansi::FG_RED;
        }
        
        for (int i = 0; i < width - bar_len; ++i) {
            result += ' ';
        }
        
        for (int i = 0; i < bar_len; ++i) {
            double pos_ratio = static_cast<double>(bar_len - 1 - i) / width;
            if (pos_ratio < 0.25) {
                result += BLOCK_FULL;
            } else if (pos_ratio < 0.5) {
                result += BLOCK_HIGH;
            } else if (pos_ratio < 0.75) {
                result += BLOCK_MED;
            } else {
                result += BLOCK_LOW;
            }
        }
        
        result += ansi::RESET;
        return result;
    }
};

class Box {
public:
    static void h_line(FrameBuffer& fb, int width) {
        for (int i = 0; i < width; ++i) {
            fb.append(BOX_H);
        }
    }
    
    static void header(FrameBuffer& fb, const char* title, int width) {
        fb.append(BOX_TL);
        
        int title_len = strlen(title);
        int padding = (width - 2 - title_len) / 2;
        
        for (int i = 0; i < padding; ++i) fb.append(BOX_H);
        fb.append(ansi::BOLD);
        fb.append(title);
        fb.append(ansi::RESET);
        for (int i = 0; i < width - 2 - padding - title_len; ++i) fb.append(BOX_H);
        
        fb.append(BOX_TR);
    }
    
    static void footer(FrameBuffer& fb, int width) {
        fb.append(BOX_BL);
        for (int i = 0; i < width - 2; ++i) fb.append(BOX_H);
        fb.append(BOX_BR);
    }
};

class Format {
public:
    static std::string price(int64_t price_cents, int width = 10) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%*.*f", width, 2, price_cents / 100.0);
        return std::string(buf);
    }
    
    static std::string volume(int64_t vol, int width = 8) {
        char buf[32];
        if (vol >= 1000000) {
            snprintf(buf, sizeof(buf), "%*.1fM", width - 1, vol / 1000000.0);
        } else if (vol >= 1000) {
            snprintf(buf, sizeof(buf), "%*.1fK", width - 1, vol / 1000.0);
        } else {
            snprintf(buf, sizeof(buf), "%*ld", width, static_cast<long>(vol));
        }
        return std::string(buf);
    }
    
    static std::string integer(int64_t val, int width = 8) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%*ld", width, static_cast<long>(val));
        return std::string(buf);
    }
    
    static std::string pad_left(const std::string& s, int width) {
        if (static_cast<int>(s.length()) >= width) return s.substr(0, width);
        return s + std::string(width - s.length(), ' ');
    }
    
    static std::string pad_right(const std::string& s, int width) {
        if (static_cast<int>(s.length()) >= width) return s.substr(0, width);
        return std::string(width - s.length(), ' ') + s;
    }
    
    static std::string center(const std::string& s, int width) {
        if (static_cast<int>(s.length()) >= width) return s.substr(0, width);
        int padding = (width - s.length()) / 2;
        return std::string(padding, ' ') + s + std::string(width - padding - s.length(), ' ');
    }
};

class Terminal {
public:
    static void init() {
        printf("%s%s", ansi::HIDE_CURSOR, ansi::CLEAR_SCREEN);
        fflush(stdout);
    }
    
    static void cleanup() {
        printf("%s%s", ansi::SHOW_CURSOR, ansi::RESET);
        fflush(stdout);
    }
    
    static void get_size(int& width, int& height) {
#ifdef _WIN32
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
            width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        } else {
            width = 120;
            height = 30;
        }
#elif defined(HAS_IOCTL)
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
            width = w.ws_col;
            height = w.ws_row;
        } else {
            width = 120;
            height = 30;
        }
#else
        width = 120;
        height = 30;
#endif
    }
};

class Sparkline {
public:
    static const char* blocks[];
    
    static std::string render(const int64_t* values, int count, int64_t min_val, int64_t max_val) {
        std::string result;
        if (max_val <= min_val) {
            return std::string(count, '_');
        }
        
        for (int i = 0; i < count; ++i) {
            double ratio = static_cast<double>(values[i] - min_val) / (max_val - min_val);
            int level = static_cast<int>(ratio * 7);
            if (level < 0) level = 0;
            if (level > 7) level = 7;
            
            switch (level) {
                case 0: result += "▁"; break;
                case 1: result += "▂"; break;
                case 2: result += "▃"; break;
                case 3: result += "▄"; break;
                case 4: result += "▅"; break;
                case 5: result += "▆"; break;
                case 6: result += "▇"; break;
                case 7: result += "█"; break;
            }
        }
        return result;
    }
};

}

#endif
