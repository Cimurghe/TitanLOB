
"""
TitanLOB WebSocket Bridge Server
================================
High-performance bridge between C++ TCP Gateway and Web Browsers.

Architecture:
    C++ Engine (port 9000) → TCP → This Bridge → WebSocket → Browser

Features:
    - Async I/O for maximum throughput
    - Binary protocol parsing (matches protocol.h)
    - JSON broadcast to connected browsers
    - Automatic reconnection to engine
    - Rate-limited updates (configurable FPS)

Usage:
    1. Start your C++ engine (listening on TCP 9000)
    2. Run: python titan_ws_bridge.py
    3. Open dashboard.html in browser
"""

import asyncio
import json
import struct
import time
import logging
from dataclasses import dataclass, field
from typing import Set, Dict, List, Optional
from collections import defaultdict


try:
    import websockets
    from websockets.server import serve as ws_serve
except ImportError:
    print("ERROR: websockets library not installed")
    print("Run: pip install websockets")
    exit(1)





ENGINE_HOST = 'localhost'
ENGINE_PORT = 9000
WS_HOST = '0.0.0.0'
WS_PORT = 8080
HTTP_PORT = 8000
TARGET_FPS = 30  
MAX_BOOK_LEVELS = 20  





MSG_ADD_ORDER = ord('A')
MSG_CANCEL_ORDER = ord('X')
MSG_MODIFY_ORDER = ord('M')
MSG_EXECUTE = ord('E')
MSG_HEARTBEAT = ord('H')
MSG_RESET = ord('R')

OUT_TRADE = ord('T')
OUT_ACCEPTED = ord('A')
OUT_CANCELLED = ord('C')

SIDE_BUY = ord('B')
SIDE_SELL = ord('S')


HEADER_FMT = '<BHQ'  
HEADER_SIZE = 11

ADD_ORDER_FMT = '<BHQ QQ B qq'  
CANCEL_FMT = '<BHQ Q'  
MODIFY_FMT = '<BHQ Q qq'  


OUT_TRADE_FMT = '<BQQQqq'  
OUT_ACCEPTED_FMT = '<BQQBqq'  
OUT_CANCELLED_FMT = '<BQQq'  





logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    datefmt='%H:%M:%S'
)
log = logging.getLogger('TitanBridge')





@dataclass
class OrderBookState:
    """Aggregated order book state for broadcasting."""
    bids: Dict[int, int] = field(default_factory=dict)  
    asks: Dict[int, int] = field(default_factory=dict)  
    trades: List[dict] = field(default_factory=list)
    
    
    orders: Dict[int, dict] = field(default_factory=dict)  
    
    
    total_trades: int = 0
    total_orders: int = 0
    messages_processed: int = 0
    last_update_time: float = 0
    
    def add_order(self, order_id: int, side: int, price: int, quantity: int):
        """Add order to book."""
        book = self.bids if side == SIDE_BUY else self.asks
        
        
        self.orders[order_id] = {
            'price': price,
            'qty': quantity,
            'side': side
        }
        
        
        book[price] = book.get(price, 0) + quantity
        self.total_orders += 1
        self.messages_processed += 1
    
    def cancel_order(self, order_id: int):
        """Remove order from book."""
        if order_id not in self.orders:
            return
        
        order = self.orders[order_id]
        book = self.bids if order['side'] == SIDE_BUY else self.asks
        
        if order['price'] in book:
            book[order['price']] -= order['qty']
            if book[order['price']] <= 0:
                del book[order['price']]
        
        del self.orders[order_id]
        self.total_orders = max(0, self.total_orders - 1)
        self.messages_processed += 1
    
    def modify_order(self, order_id: int, new_price: int, new_qty: int):
        """Modify existing order (cancel + re-add)."""
        if order_id in self.orders:
            old_order = self.orders[order_id]
            self.cancel_order(order_id)
            self.add_order(order_id, old_order['side'], new_price, new_qty)
        self.messages_processed += 1
    
    def add_trade(self, buy_id: int, sell_id: int, price: int, quantity: int, timestamp: int):
        """Record a trade execution."""
        self.trades.append({
            'buy_order_id': buy_id,
            'sell_order_id': sell_id,
            'price': price,
            'quantity': quantity,
            'timestamp': timestamp,
            'time': time.time()
        })
        
        
        if len(self.trades) > 100:
            self.trades = self.trades[-100:]
        
        self.total_trades += 1
        self.messages_processed += 1
    
    def to_json(self) -> dict:
        """Convert to JSON for browser."""
        
        sorted_bids = sorted(self.bids.items(), key=lambda x: -x[0])[:MAX_BOOK_LEVELS]
        sorted_asks = sorted(self.asks.items(), key=lambda x: x[0])[:MAX_BOOK_LEVELS]
        
        return {
            'type': 'book',
            'bids': sorted_bids,
            'asks': sorted_asks,
            'stats': {
                'trades': self.total_trades,
                'orders': len(self.orders),
                'bid_levels': len(self.bids),
                'ask_levels': len(self.asks),
                'messages': self.messages_processed
            },
            'timestamp': time.time()
        }
    
    def reset(self):
        """Clear all state."""
        self.bids.clear()
        self.asks.clear()
        self.orders.clear()
        self.trades.clear()
        self.total_trades = 0
        self.total_orders = 0





book_state = OrderBookState()
connected_clients: Set[websockets.WebSocketServerProtocol] = set()
engine_connected = False





def parse_message(data: bytes) -> Optional[dict]:
    """Parse binary message from engine."""
    if len(data) < HEADER_SIZE:
        return None
    
    msg_type, msg_length, timestamp = struct.unpack(HEADER_FMT, data[:HEADER_SIZE])
    
    if msg_type == MSG_ADD_ORDER:
        if len(data) >= 44:
            _, _, _, order_id, user_id, side, price, quantity = struct.unpack(ADD_ORDER_FMT, data[:44])
            return {
                'type': 'add',
                'order_id': order_id,
                'side': side,
                'price': price,
                'quantity': quantity,
                'timestamp': timestamp
            }
    
    elif msg_type == MSG_CANCEL_ORDER:
        if len(data) >= 19:
            _, _, _, order_id = struct.unpack(CANCEL_FMT, data[:19])
            return {
                'type': 'cancel',
                'order_id': order_id,
                'timestamp': timestamp
            }
    
    elif msg_type == MSG_MODIFY_ORDER:
        if len(data) >= 35:
            _, _, _, order_id, new_price, new_qty = struct.unpack(MODIFY_FMT, data[:35])
            return {
                'type': 'modify',
                'order_id': order_id,
                'new_price': new_price,
                'new_quantity': new_qty,
                'timestamp': timestamp
            }
    
    return None

def parse_output_message(data: bytes) -> Optional[dict]:
    """Parse output message (trade, accepted, cancelled)."""
    if len(data) < 1:
        return None
    
    msg_type = data[0]
    
    if msg_type == OUT_TRADE:
        if len(data) >= 41:
            _, ts, buy_id, sell_id, price, qty = struct.unpack('<BQQQ qq', data[:41])
            return {
                'type': 'trade',
                'timestamp': ts,
                'buy_order_id': buy_id,
                'sell_order_id': sell_id,
                'price': price,
                'quantity': qty
            }
    
    elif msg_type == OUT_ACCEPTED:
        if len(data) >= 36:
            _, ts, order_id, side, price, qty = struct.unpack('<BQQ B qq', data[:36])
            return {
                'type': 'accepted',
                'timestamp': ts,
                'order_id': order_id,
                'side': side,
                'price': price,
                'quantity': qty
            }
    
    elif msg_type == OUT_CANCELLED:
        if len(data) >= 25:
            _, ts, order_id, qty = struct.unpack('<BQQ q', data[:25])
            return {
                'type': 'cancelled',
                'timestamp': ts,
                'order_id': order_id,
                'quantity': qty
            }
    
    return None





async def engine_reader(reader: asyncio.StreamReader):
    """Read messages from C++ engine and update book state."""
    global engine_connected
    
    log.info("Engine reader started")
    
    buffer = b''
    
    while True:
        try:
            
            chunk = await reader.read(65536)
            if not chunk:
                log.warning("Engine disconnected (EOF)")
                engine_connected = False
                break
            
            buffer += chunk
            
            
            while len(buffer) >= HEADER_SIZE:
                
                msg_type, msg_length, _ = struct.unpack(HEADER_FMT, buffer[:HEADER_SIZE])
                
                if msg_length > 1024:
                    log.error(f"Invalid message length: {msg_length}")
                    buffer = buffer[1:]  
                    continue
                
                if len(buffer) < msg_length:
                    break  
                
                
                msg_data = buffer[:msg_length]
                buffer = buffer[msg_length:]
                
                
                msg = parse_message(msg_data)
                if msg:
                    if msg['type'] == 'add':
                        book_state.add_order(
                            msg['order_id'],
                            msg['side'],
                            msg['price'],
                            msg['quantity']
                        )
                    elif msg['type'] == 'cancel':
                        book_state.cancel_order(msg['order_id'])
                    elif msg['type'] == 'modify':
                        book_state.modify_order(
                            msg['order_id'],
                            msg['new_price'],
                            msg['new_quantity']
                        )
                
        except asyncio.CancelledError:
            break
        except Exception as e:
            log.error(f"Engine reader error: {e}")
            await asyncio.sleep(0.1)

async def connect_to_engine():
    """Maintain connection to C++ engine."""
    global engine_connected
    
    reconnect_delay = 1.0
    max_delay = 30.0
    
    while True:
        try:
            log.info(f"Connecting to engine at {ENGINE_HOST}:{ENGINE_PORT}...")
            
            reader, writer = await asyncio.open_connection(ENGINE_HOST, ENGINE_PORT)
            
            engine_connected = True
            reconnect_delay = 1.0
            log.info("Connected to engine!")
            
            
            reader_task = asyncio.create_task(engine_reader(reader))
            
            
            await reader_task
            
            writer.close()
            await writer.wait_closed()
            
        except ConnectionRefusedError:
            log.warning(f"Engine not available, retrying in {reconnect_delay:.1f}s...")
        except Exception as e:
            log.error(f"Engine connection error: {e}")
        
        engine_connected = False
        await asyncio.sleep(reconnect_delay)
        reconnect_delay = min(reconnect_delay * 1.5, max_delay)





async def websocket_handler(websocket: websockets.WebSocketServerProtocol, path: str = ""):
    """Handle WebSocket connections from browsers."""
    connected_clients.add(websocket)
    client_ip = websocket.remote_address[0] if websocket.remote_address else 'unknown'
    log.info(f"Browser connected from {client_ip} (total: {len(connected_clients)})")
    
    try:
        
        await websocket.send(json.dumps(book_state.to_json()))
        
        
        async for message in websocket:
            
            pass
            
    except websockets.exceptions.ConnectionClosed:
        pass
    except Exception as e:
        log.error(f"WebSocket error: {e}")
    finally:
        connected_clients.discard(websocket)
        log.info(f"Browser disconnected (total: {len(connected_clients)})")

async def broadcast_loop():
    """Broadcast book state to all connected browsers at target FPS."""
    frame_interval = 1.0 / TARGET_FPS
    last_broadcast = time.time()
    
    while True:
        try:
            now = time.time()
            
            if now - last_broadcast >= frame_interval and connected_clients:
                
                message = json.dumps(book_state.to_json())
                
                
                if connected_clients:
                    await asyncio.gather(
                        *[client.send(message) for client in connected_clients],
                        return_exceptions=True
                    )
                
                last_broadcast = now
            
            
            await asyncio.sleep(0.001)
            
        except Exception as e:
            log.error(f"Broadcast error: {e}")
            await asyncio.sleep(0.1)





async def http_handler(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
    """Simple HTTP server for dashboard."""
    try:
        request = await reader.read(4096)
        request_line = request.decode().split('\r\n')[0]
        
        
        parts = request_line.split()
        if len(parts) >= 2:
            method, path = parts[0], parts[1]
        else:
            method, path = 'GET', '/'
        
        if path in ('/', '/dashboard.html', '/index.html'):
            
            try:
                with open('dashboard.html', 'rb') as f:
                    content = f.read()
                
                response = (
                    b'HTTP/1.1 200 OK\r\n'
                    b'Content-Type: text/html; charset=utf-8\r\n'
                    b'Connection: close\r\n'
                    b'\r\n'
                ) + content
            except FileNotFoundError:
                response = (
                    b'HTTP/1.1 404 Not Found\r\n'
                    b'Content-Type: text/plain\r\n'
                    b'Connection: close\r\n'
                    b'\r\n'
                    b'dashboard.html not found'
                )
        else:
            response = (
                b'HTTP/1.1 404 Not Found\r\n'
                b'Content-Type: text/plain\r\n'
                b'Connection: close\r\n'
                b'\r\n'
                b'Not Found'
            )
        
        writer.write(response)
        await writer.drain()
        
    except Exception as e:
        log.error(f"HTTP error: {e}")
    finally:
        writer.close()
        await writer.wait_closed()





async def stats_reporter():
    """Periodically log stats."""
    last_msg_count = 0
    
    while True:
        await asyncio.sleep(10)
        
        msg_count = book_state.messages_processed
        rate = (msg_count - last_msg_count) / 10
        last_msg_count = msg_count
        
        status = "CONNECTED" if engine_connected else "DISCONNECTED"
        
        log.info(
            f"[{status}] Messages: {msg_count:,} ({rate:.0f}/s) | "
            f"Trades: {book_state.total_trades:,} | "
            f"Orders: {len(book_state.orders):,} | "
            f"Browsers: {len(connected_clients)}"
        )





async def main():
    """Main entry point."""
    print("=" * 60)
    print("TitanLOB WebSocket Bridge")
    print("=" * 60)
    print(f"Engine:    tcp://{ENGINE_HOST}:{ENGINE_PORT}")
    print(f"WebSocket: ws://0.0.0.0:{WS_PORT}")
    print(f"HTTP:      http://0.0.0.0:{HTTP_PORT}")
    print(f"Target:    {TARGET_FPS} FPS")
    print("=" * 60)
    print()
    
    
    http_server = await asyncio.start_server(http_handler, '0.0.0.0', HTTP_PORT)
    log.info(f"HTTP server started on port {HTTP_PORT}")
    
    
    ws_server = await ws_serve(websocket_handler, WS_HOST, WS_PORT)
    log.info(f"WebSocket server started on port {WS_PORT}")
    
    
    engine_task = asyncio.create_task(connect_to_engine())
    broadcast_task = asyncio.create_task(broadcast_loop())
    stats_task = asyncio.create_task(stats_reporter())
    
    print()
    log.info("Bridge ready! Open http://localhost:8000 in your browser")
    print()
    
    try:
        
        await asyncio.gather(
            engine_task,
            broadcast_task,
            stats_task,
            return_exceptions=True
        )
    except KeyboardInterrupt:
        log.info("Shutting down...")
    finally:
        ws_server.close()
        await ws_server.wait_closed()
        http_server.close()
        await http_server.wait_closed()

if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nShutdown complete")
