
"""
Kraken L3 → TitanLOB TCP Bridge
================================
Pipes live Kraken WebSocket L3 data directly into the C++ Gateway on port 9000.

This bridges:
    Kraken 'add'    → MsgAddOrder (44 bytes)
    Kraken 'modify' → MsgModify   (35 bytes)  
    Kraken 'delete' → MsgCancel   (19 bytes)

Requirements:
    pip install websocket-client

Usage:
    1. Start the C++ dashboard (listening on port 9000)
    2. Set Kraken API credentials:
       export KRAKEN_API_KEY="your-api-key"
       export KRAKEN_API_SECRET="your-api-secret"
    3. Run: python kraken_bridge.py
"""

import socket
import json
import struct
import time
import os
import hashlib
import hmac
import base64
import urllib.request
import urllib.parse
from datetime import datetime

try:
    import websocket
except ImportError:
    print("ERROR: websocket-client not installed")
    print("Run: pip install websocket-client")
    exit(1)







MSG_ADD_ORDER    = ord('A')  
MSG_CANCEL_ORDER = ord('X')  
MSG_MODIFY_ORDER = ord('M')  


SIDE_BUY  = ord('B')  
SIDE_SELL = ord('S')  



ADD_ORDER_FMT = '<BHQ QQ B qq'
ADD_ORDER_SIZE = 44


CANCEL_FMT = '<BHQ Q'
CANCEL_SIZE = 19


MODIFY_FMT = '<BHQ Q qq'
MODIFY_SIZE = 35






PRICE_MULTIPLIER = 100  

def price_to_ticks(price_float: float) -> int:
    """Convert float price to integer ticks (cents)."""
    return int(round(price_float * PRICE_MULTIPLIER))

def quantity_to_int(qty_float: float) -> int:
    """Convert quantity to satoshis (8 decimal places for BTC)."""
    return int(round(qty_float * 100_000_000))






def iso_to_nanos(iso_str: str) -> int:
    """Convert ISO8601 timestamp to nanoseconds since epoch."""
    if iso_str.endswith('Z'):
        iso_str = iso_str[:-1]
    
    if '.' in iso_str:
        dt_part, frac_part = iso_str.split('.')
        frac_part = frac_part[:9].ljust(9, '0')
        nanos_frac = int(frac_part)
    else:
        dt_part = iso_str
        nanos_frac = 0
    
    dt = datetime.strptime(dt_part, '%Y-%m-%dT%H:%M:%S')
    epoch = datetime(1970, 1, 1)
    seconds = int((dt - epoch).total_seconds())
    
    return seconds * 1_000_000_000 + nanos_frac






def pack_add_order(timestamp: int, order_id: int, side: int, price: int, quantity: int) -> bytes:
    """Pack MsgAddOrder (44 bytes)."""
    return struct.pack(
        ADD_ORDER_FMT,
        MSG_ADD_ORDER,      
        ADD_ORDER_SIZE,     
        timestamp,          
        order_id,           
        0,                  
        side,               
        price,              
        quantity            
    )

def pack_cancel(timestamp: int, order_id: int) -> bytes:
    """Pack MsgCancel (19 bytes)."""
    return struct.pack(
        CANCEL_FMT,
        MSG_CANCEL_ORDER,   
        CANCEL_SIZE,        
        timestamp,          
        order_id            
    )

def pack_modify(timestamp: int, order_id: int, new_price: int, new_quantity: int) -> bytes:
    """Pack MsgModify (35 bytes)."""
    return struct.pack(
        MODIFY_FMT,
        MSG_MODIFY_ORDER,   
        MODIFY_SIZE,        
        timestamp,          
        order_id,           
        new_price,          
        new_quantity        
    )






def get_kraken_ws_token(api_key: str, api_secret: str) -> str:
    """Get WebSocket authentication token from Kraken REST API."""
    api_url = "https://api.kraken.com"
    api_path = "/0/private/GetWebSocketsToken"
    
    nonce = str(int(time.time() * 1000))
    post_data = urllib.parse.urlencode({"nonce": nonce})
    
    sha256_hash = hashlib.sha256((nonce + post_data).encode()).digest()
    message = api_path.encode() + sha256_hash
    secret_decoded = base64.b64decode(api_secret)
    signature = hmac.new(secret_decoded, message, hashlib.sha512)
    signature_b64 = base64.b64encode(signature.digest()).decode()
    
    headers = {
        "API-Key": api_key,
        "API-Sign": signature_b64,
        "Content-Type": "application/x-www-form-urlencoded"
    }
    
    req = urllib.request.Request(
        api_url + api_path,
        data=post_data.encode(),
        headers=headers,
        method="POST"
    )
    
    with urllib.request.urlopen(req, timeout=10) as response:
        result = json.loads(response.read().decode())
    
    if result.get("error"):
        raise Exception(f"Kraken API error: {result['error']}")
    
    return result["result"]["token"]






def connect_to_gateway(host: str = "localhost", port: int = 9000) -> socket.socket:
    """Connect to C++ Gateway with retry logic."""
    while True:
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)  
            sock.connect((host, port))
            print(f"[Bridge] Connected to C++ Gateway at {host}:{port}")
            return sock
        except ConnectionRefusedError:
            print(f"[Bridge] Gateway not available, retrying in 1s...")
            time.sleep(1)
        except Exception as e:
            print(f"[Bridge] Connection error: {e}, retrying in 1s...")
            time.sleep(1)






def start_bridge(
    host: str = "localhost",
    port: int = 9000,
    symbol: str = "BTC/USD",
    depth: int = 1000,
    api_key: str = None,
    api_secret: str = None,
):
    """
    Bridge Kraken L3 WebSocket data to C++ Gateway.
    """
    
    
    api_key = api_key or os.environ.get('KRAKEN_API_KEY')
    api_secret = api_secret or os.environ.get('KRAKEN_API_SECRET')
    
    if not all([api_key, api_secret]):
        print("ERROR: Missing Kraken API credentials.")
        print("\nSet environment variables:")
        print('  export KRAKEN_API_KEY="your-api-key"')
        print('  export KRAKEN_API_SECRET="your-api-secret"')
        print("\nGet keys from: https://www.kraken.com/u/security/api")
        return
    
    
    gateway_socket = connect_to_gateway(host, port)
    
    
    print(f"[Bridge] Getting WebSocket token...")
    ws_token = get_kraken_ws_token(api_key, api_secret)
    token_time = time.time()
    print(f"[Bridge] Token acquired")
    
    
    id_map: dict[str, int] = {}
    next_id = [0]  
    
    def get_or_create_id(kraken_id: str) -> int:
        if kraken_id not in id_map:
            id_map[kraken_id] = next_id[0]
            next_id[0] += 1
        return id_map[kraken_id]
    
    
    stats = {'add': 0, 'modify': 0, 'delete': 0, 'bytes': 0}
    start_time = [None]
    
    
    
    
    
    def on_open(ws):
        nonlocal ws_token, token_time
        start_time[0] = time.time()
        
        
        if time.time() - token_time > 600:
            print(f"[Bridge] Refreshing token...")
            ws_token = get_kraken_ws_token(api_key, api_secret)
            token_time = time.time()
        
        subscribe_msg = json.dumps({
            "method": "subscribe",
            "params": {
                "channel": "level3",
                "symbol": [symbol],
                "depth": depth,
                "snapshot": True,
                "token": ws_token
            }
        })
        ws.send(subscribe_msg)
        print(f"[Bridge] Subscribed to {symbol} L3")
    
    def on_message(ws, message):
        """HOT PATH - Transform and push immediately."""
        nonlocal gateway_socket
        
        
        if '"channel":"heartbeat"' in message:
            return
        if '"method":"subscribe"' in message:
            if '"success":true' in message:
                print(f"[Bridge] Subscription confirmed")
            return
        
        try:
            msg = json.loads(message)
        except json.JSONDecodeError:
            return
        
        
        if msg.get('channel') != 'level3':
            return
        
        
        if msg.get('type') == 'snapshot':
            for book in msg.get('data', []):
                for order in book.get('bids', []):
                    process_snapshot_order(order, SIDE_BUY)
                for order in book.get('asks', []):
                    process_snapshot_order(order, SIDE_SELL)
            return
        
        
        if msg.get('type') != 'update':
            return
        
        for book in msg.get('data', []):
            for event in book.get('bids', []):
                process_event(event, SIDE_BUY)
            for event in book.get('asks', []):
                process_event(event, SIDE_SELL)
        
        
        total = stats['add'] + stats['modify'] + stats['delete']
        if total % 100 == 0:
            elapsed = time.time() - start_time[0] if start_time[0] else 1
            rate = total / elapsed
            print(f"\r[Bridge] {total:,} events | {rate:.1f}/sec | {len(id_map):,} orders", end='', flush=True)
    
    def process_snapshot_order(order: dict, side: int):
        """Process snapshot order as ADD."""
        nonlocal gateway_socket
        
        kraken_id = order.get('order_id')
        if not kraken_id:
            return
        
        order_id = get_or_create_id(kraken_id)
        timestamp = iso_to_nanos(order['timestamp'])
        price = price_to_ticks(float(order['limit_price']))
        quantity = quantity_to_int(float(order['order_qty']))
        
        data = pack_add_order(timestamp, order_id, side, price, quantity)
        send_to_gateway(data)
        stats['add'] += 1
    
    def process_event(event: dict, side: int):
        """Process a single order event."""
        nonlocal gateway_socket
        
        event_type = event.get('event')
        kraken_id = event.get('order_id')
        
        if not kraken_id:
            return
        
        timestamp = iso_to_nanos(event['timestamp'])
        
        if event_type == 'add':
            order_id = get_or_create_id(kraken_id)
            price = price_to_ticks(float(event['limit_price']))
            quantity = quantity_to_int(float(event['order_qty']))
            
            data = pack_add_order(timestamp, order_id, side, price, quantity)
            send_to_gateway(data)
            stats['add'] += 1
        
        elif event_type == 'delete':
            order_id = id_map.get(kraken_id)
            if order_id is None:
                order_id = get_or_create_id(kraken_id)
            
            data = pack_cancel(timestamp, order_id)
            send_to_gateway(data)
            stats['delete'] += 1
        
        elif event_type == 'modify':
            order_id = id_map.get(kraken_id)
            if order_id is None:
                order_id = get_or_create_id(kraken_id)
            
            price = price_to_ticks(float(event['limit_price']))
            quantity = quantity_to_int(float(event['order_qty']))
            
            data = pack_modify(timestamp, order_id, price, quantity)
            send_to_gateway(data)
            stats['modify'] += 1
    
    def send_to_gateway(data: bytes):
        """Send binary data immediately (no buffering)."""
        nonlocal gateway_socket
        try:
            gateway_socket.sendall(data)
            stats['bytes'] += len(data)
        except (BrokenPipeError, ConnectionResetError) as e:
            print(f"\n[Bridge] Gateway disconnected: {e}")
            print("[Bridge] Reconnecting...")
            gateway_socket = connect_to_gateway(host, port)
            gateway_socket.sendall(data)
            stats['bytes'] += len(data)
    
    def on_error(ws, error):
        print(f"\n[Bridge] WebSocket error: {error}")
    
    def on_close(ws, close_status_code, close_msg):
        print(f"\n[Bridge] WebSocket closed: {close_status_code} - {close_msg}")
    
    
    
    
    
    reconnect_delay = 1
    max_delay = 30
    
    while True:
        try:
            
            if time.time() - token_time > 600:
                print(f"[Bridge] Refreshing token...")
                ws_token = get_kraken_ws_token(api_key, api_secret)
                token_time = time.time()
            
            print(f"[Bridge] Connecting to Kraken L3 WebSocket...")
            
            ws = websocket.WebSocketApp(
                "wss://ws-l3.kraken.com/v2",
                on_open=on_open,
                on_message=on_message,
                on_error=on_error,
                on_close=on_close
            )
            
            ws.run_forever(ping_interval=30, ping_timeout=10)
            
            print(f"[Bridge] Reconnecting in {reconnect_delay}s...")
            time.sleep(reconnect_delay)
            reconnect_delay = min(reconnect_delay * 2, max_delay)
            
        except KeyboardInterrupt:
            print(f"\n[Bridge] Shutting down...")
            gateway_socket.close()
            total = stats['add'] + stats['modify'] + stats['delete']
            print(f"Final: {total:,} events, {stats['bytes']:,} bytes sent")
            break
        except Exception as e:
            print(f"\n[Bridge] Error: {e}")
            time.sleep(reconnect_delay)
            reconnect_delay = min(reconnect_delay * 2, max_delay)






if __name__ == "__main__":
    import argparse
    
    parser = argparse.ArgumentParser(
        description="Bridge Kraken L3 WebSocket data to C++ Gateway",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
This script bridges live Kraken order book data to your C++ TitanLOB engine.

Setup:
  1. Start the C++ dashboard (listening on port 9000)
  2. Set environment variables:
     
     export KRAKEN_API_KEY="your-key"
     export KRAKEN_API_SECRET="your-secret"
     
  3. Run: python kraken_bridge.py

The bridge will:
  - Connect to localhost:9000 (retry if dashboard isn't running)
  - Subscribe to Kraken L3 WebSocket feed
  - Convert events to binary protocol format
  - Push immediately to C++ gateway (no buffering)
        """
    )
    parser.add_argument("--host", default="localhost", help="Gateway host")
    parser.add_argument("--port", type=int, default=9000, help="Gateway port")
    parser.add_argument("-s", "--symbol", default="BTC/USD", help="Trading pair")
    parser.add_argument("-d", "--depth", type=int, default=1000, help="Order book depth")
    
    args = parser.parse_args()
    
    print("=" * 60)
    print("Kraken L3 → TitanLOB Bridge")
    print("=" * 60)
    print(f"Gateway:  {args.host}:{args.port}")
    print(f"Symbol:   {args.symbol}")
    print(f"Depth:    {args.depth}")
    print()
    print("Press Ctrl+C to stop")
    print("=" * 60)
    
    start_bridge(
        host=args.host,
        port=args.port,
        symbol=args.symbol,
        depth=args.depth
    )