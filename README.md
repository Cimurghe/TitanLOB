<p align="center">
  <img src="https://img.shields.io/badge/C%2B%2B-17-blue?style=flat-square&logo=c%2B%2B" alt="C++17">
  <img src="https://img.shields.io/badge/Python-3.10+-yellow?style=flat-square&logo=python" alt="Python 3.10+">
  <img src="https://img.shields.io/badge/Platform-Linux%20%7C%20macOS-lightgrey?style=flat-square" alt="Platform">
  <img src="https://img.shields.io/badge/License-MIT-green?style=flat-square" alt="License">
</p>

<h1 align="center">⚡ TitanLOB</h1>

<p align="center">
  <strong>High-Frequency Trading Limit Order Book Engine</strong><br>
  Sub-30ns median latency • 23M+ orders/sec throughput • Production-grade architecture
</p>

---

## Overview

TitanLOB is a high-performance limit order book (LOB) engine designed for quantitative finance research and HFT system development. It features a cache-optimized matching engine written in C++17 with real-time visualization via WebSocket and a professional trading dashboard.

### Key Features

- **Ultra-Low Latency**: Sub-30ns median latency per operation using cache-aligned data structures
- **High Throughput**: 23M+ messages/second on commodity hardware  
- **Real Market Data**: Native integration with Kraken L3 WebSocket feed (order-by-order data)
- **Live Visualization**: Professional 4-quadrant trading dashboard with depth charts and BBO history
- **Binary Protocol**: Compact wire format for minimal serialization overhead
- **Flexible Modes**: Live trading feed or historical replay from binary files

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           TitanLOB System                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  ┌──────────────┐     ┌──────────────┐     ┌──────────────────────────┐ │
│  │   Kraken     │     │    TCP       │     │     C++ Matching         │ │
│  │   L3 Feed    │────▶│   Gateway    │────▶│       Engine             │ │
│  │  (WebSocket) │     │  (Port 9000) │     │  (OptimizedOrderBook)    │ │
│  └──────────────┘     └──────────────┘     └───────────┬──────────────┘ │
│         │                                              │                 │
│         │              Binary Protocol                 │                 │
│         │              (protocol.h)                    ▼                 │
│         │                                   ┌──────────────────────────┐ │
│         │                                   │    WebSocket Server      │ │
│  ┌──────▼──────┐                            │     (Port 8080)          │ │
│  │   Bridge    │                            └───────────┬──────────────┘ │
│  │   Scripts   │                                        │                │
│  │  (Python)   │                                        ▼                │
│  └─────────────┘                            ┌──────────────────────────┐ │
│                                             │   Trading Dashboard      │ │
│                                             │   (Titan_Dash.html)      │ │
│                                             └──────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## Performance

Benchmarked with 6.18M real Kraken L3 messages (BTC/USD):

| Metric | Value |
|--------|-------|
| Median Latency (P50) | **26 ns** |
| P90 Latency | **59.5 ns** |
| P95 Latency | **120.5 ns** |
| P99 Latency | **286.8 ns** |
| P99.9 Latency | **6.9 μs** |
| Throughput (with latency measurement) | **8.85 M msgs/sec** |
| Pure Throughput | **23.80 M msgs/sec** |
| Memory (Order Book) | ~3 GB |

---

## Quick Start

### Prerequisites

```bash
# Ubuntu/Debian
sudo apt update
sudo apt install build-essential g++ python3 python3-pip

# Python dependencies
pip3 install websocket-client websockets
```

### Build

```bash
# Clone repository
git clone https://github.com/Cimurghe/TitanLOB.git
cd TitanLOB

# Build main application (Live Mode)
g++ -std=c++17 -O3 -march=native -o titan main.cpp order_book.cpp -lpthread

# Build benchmark harness
g++ -std=c++17 -O3 -march=native -o titan_bench benchmark_harness.cpp -lpthread
```

### Run

**Option 1: Live Mode (Real-Time Kraken Data)**

```bash
# Terminal 1: Start the C++ engine
./titan

# Terminal 2: Start the Kraken bridge (requires API keys)
export KRAKEN_API_KEY="your-api-key"
export KRAKEN_API_SECRET="your-api-secret"
python3 kraken_bridge.py --symbol BTC/USD

# Terminal 3: Open dashboard
open Titan_Dash.html   # macOS
xdg-open Titan_Dash.html   # Linux
```

**Option 2: Replay Mode (Historical Data)**

```bash
# Build with replay mode
g++ -std=c++17 -O3 -march=native -DREPLAY_MODE=\"btc_l3.dat\" -o titan_replay main.cpp order_book.cpp -lpthread

# Run replay
./titan_replay
```

---

## Creating Your Own Data Files

Since `.dat` files are large (100MB+ for a few hours of data), you'll need to record your own:

### Step 1: Get Kraken API Keys (Free)

1. Create account at [kraken.com](https://www.kraken.com)
2. Go to **Security** → **API** → **Create Key**
3. Enable only **"Access WebSockets API"** (no trading permissions needed)
4. Save your API Key and Secret

### Step 2: Record L3 Data

```bash
# Set credentials
export KRAKEN_API_KEY="your-api-key"
export KRAKEN_API_SECRET="your-api-secret"

# Record to JSON (run for desired duration, Ctrl+C to stop)
python3 kraken_l3_recorder.py -o btc_l3.json -s BTC/USD -d 1000

# Available symbols: BTC/USD, ETH/USD, SOL/USD, XRP/USD, etc.
```

### Step 3: Convert to Binary Format

```bash
python3 kraken_normalizer.py btc_l3.json btc_l3.dat
```

### Step 4: Run Benchmark

```bash
./titan_bench btc_l3.dat
```

**Expected output:**
```
╔══════════════════════════════════════════════════════════════════╗
║ BTC L3 Message Replay - Per-Message Latency                      ║
╠══════════════════════════════════════════════════════════════════╣
║ Samples: 6080010                                                 ║
╠══════════════════════════════════════════════════════════════════╣
║ LATENCY (nanoseconds)                                            ║
║   Min:           13.0                                            ║
║   Mean:          93.9                                            ║
║   Median:        26.0 (P50)                                      ║
║   P90:           59.5                                            ║
║   P95:           120.5                                           ║
║   P99:           286.8 ◀ CRITICAL                                ║
║   P99.9:         6938.6 ◀ TAIL                                   ║
╠══════════════════════════════════════════════════════════════════╣
║ THROUGHPUT: 8,845,216 ops/sec (8.85 M ops/s)                     ║
╚══════════════════════════════════════════════════════════════════╝

Pure Throughput: 23.80 M msgs/sec
```

---

## Project Structure

```
TitanLOB/
├── Core Engine
│   ├── order_book.h          # Main order book header (OptimizedOrderBook class)
│   ├── order_book.cpp        # Order book implementation
│   ├── protocol.h            # Binary message protocol definitions
│   ├── output_msg.h          # Output message structs (trades, accepts, cancels)
│   ├── object_pool.h         # O(1) memory pool allocator
│   └── ring_buffer.h         # Lock-free SPSC ring buffer
│
├── Application
│   ├── main.cpp              # Main entry point (TCP server + WebSocket)
│   ├── gateway.h             # TCP gateway for binary protocol
│   ├── titan_ws_server.h     # WebSocket server (pure C++, no dependencies)
│   └── benchmark_harness.cpp # Latency/throughput benchmarking
│
├── Data Pipeline
│   ├── kraken_bridge.py      # Live Kraken L3 → TCP bridge
│   ├── kraken_l3_recorder.py # Record Kraken L3 to JSON
│   ├── kraken_normalizer.py  # Convert JSON → binary .dat
│   ├── replay_binary.py      # Replay .dat files to TCP gateway
│   └── Inspect_deepflow.py   # Analyze .deepflow binary logs
│
├── Visualization
│   ├── Titan_Dash.html       # Professional trading dashboard
│   └── tui.h                 # Terminal UI utilities
│
├── Logging
│   └── logger.h              # Double-buffered async binary logger
│
└── README.md
```

---

## Configuration

### Compile-Time Options

| Flag | Description | Default |
|------|-------------|---------|
| `-DREPLAY_MODE=\"file.dat\"` | Enable replay mode with specified file | Disabled (live mode) |
| `-DMAX_PRICE_LEVELS=N` | Maximum price levels (memory tradeoff) | 33,554,432 |
| `-O3 -march=native` | Recommended optimization flags | — |

### Runtime Ports

| Port | Service |
|------|---------|
| 9000 | TCP Gateway (binary protocol) |
| 8080 | WebSocket Server (JSON to dashboard) |

---

## Binary Protocol

TitanLOB uses a compact binary protocol for minimal latency:

### Message Types

| Type | Code | Size | Description |
|------|------|------|-------------|
| ADD_ORDER | `'A'` | 44 bytes | Add limit order |
| CANCEL_ORDER | `'X'` | 19 bytes | Cancel order |
| MODIFY_ORDER | `'M'` | 35 bytes | Modify order |
| EXECUTE | `'E'` | 45 bytes | Execute against book |
| HEARTBEAT | `'H'` | 11 bytes | Keep-alive |

### Message Header (11 bytes)

```c
struct MsgHeader {
    uint8_t  type;       // Message type code
    uint16_t length;     // Total message length
    uint64_t timestamp;  // Nanoseconds since epoch
};
```

See `protocol.h` for complete definitions.

---

## Order Types Supported

- **Limit Orders** (GTC, IOC, FOK)
- **Market Orders** (IOC with extreme price)
- **Iceberg Orders** (hidden quantity with peak refresh)
- **All-or-None (AON)** orders
- **Stop Orders** (trigger price activation)

---

## Dashboard Features

The `Titan_Dash.html` dashboard provides:

- **Order Book Table**: Top 15 bid/ask levels with volume bars
- **Depth Chart**: Cumulative bid/ask visualization
- **Price Histogram**: Volume distribution across price levels
- **BBO History**: Best bid/offer time series with spread tracking
- **Connection Status**: Live/simulated mode indicator

Connect via WebSocket at `ws://localhost:8080`.

---

## API Reference

### OptimizedOrderBook

```cpp
// Construction
OptimizedOrderBook book(1'000'000);  // Pool capacity

// Add order (thread-safe)
book.add_order(order_id, is_buy, price, quantity, user_id);

// Add order (no lock - for single-threaded replay)
book.add_order_no_lock(order_id, is_buy, price, quantity, user_id);

// Cancel order
book.cancel_order(order_id);

// Match against book
book.match_order(order_id, is_buy, price, quantity, TimeInForce::IOC);

// Query state
int64_t best_bid = book.get_best_bid();
int64_t best_ask = book.get_best_ask();
size_t orders = book.order_count();
```

---

## Troubleshooting

### "Bind failed (Is another Titan instance running?)"
Another process is using port 9000. Kill it or change `BRIDGE_PORT` in `main.cpp`.

### "Engine not available, retrying..."
Start the C++ engine (`./titan`) before running bridge scripts.

### Dashboard shows "Disconnected"
Ensure WebSocket server is running on port 8080. Check browser console for errors.

### High latency spikes
- Disable CPU frequency scaling: `sudo cpupower frequency-set -g performance`
- Pin to specific CPU core: `taskset -c 0 ./titan`
- Increase process priority: `sudo nice -n -20 ./titan`

---

## Roadmap

- [ ] FIX protocol gateway
- [ ] Multi-symbol support
- [ ] FPGA acceleration layer
- [ ] Historical tick database
- [ ] Strategy backtesting framework

---

## Contributing

Contributions welcome! Please open an issue to discuss changes before submitting PRs.

---

## License

MIT License - see [LICENSE](LICENSE) for details.

---

## Acknowledgments

- Market data provided by [Kraken](https://www.kraken.com) L3 WebSocket API
- Architecture inspired by [this HFT blog post](https://web.archive.org/web/20110219163448/http://howtohft.wordpress.com/2011/02/15/how-to-build-a-fast-limit-order-book/) and Optiver engineering discussions

---

<p align="center">
  <strong>Built for speed. Designed for traders.</strong>
</p>
