
"""
Kraken L3 (Level 3) Market Data Recorder
=========================================
TRUE order-by-order data with individual order IDs. FREE.

This is real L3 data that maps directly to your TitanLOB engine:
    Kraken 'add'    → Your ADD
    Kraken 'modify' → Your MODIFY  
    Kraken 'delete' → Your CANCEL/EXECUTE

Endpoint: wss://ws-l3.kraken.com/v2
Auth: Required (free Kraken API key)

Get API keys: https://www.kraken.com/u/security/api
    - Create key with "Access WebSockets API" permission
    - No trading permissions needed
"""

import websocket
import json
import time
import os
import hashlib
import hmac
import base64
import urllib.request
import urllib.parse
from datetime import datetime


def get_kraken_ws_token(api_key: str, api_secret: str) -> str:
    """
    Get WebSocket authentication token from Kraken REST API.
    
    The token is valid for 15 minutes.
    """
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


def start_recording(
    filename="kraken_l3.json",
    symbol="BTC/USD",
    depth=1000,
    api_key=None,
    api_secret=None,
):
    """
    Connect to Kraken L3 WebSocket and record order-by-order data.
    
    Args:
        filename: Output file for NDJSON data
        symbol: Trading pair (e.g., BTC/USD, ETH/USD)
        depth: Order book depth (10, 100, or 1000)
        api_key: Kraken API key (or set KRAKEN_API_KEY env var)
        api_secret: Kraken API secret (or set KRAKEN_API_SECRET env var)
    """
    
    
    api_key = api_key or os.environ.get('KRAKEN_API_KEY')
    api_secret = api_secret or os.environ.get('KRAKEN_API_SECRET')
    
    if not all([api_key, api_secret]):
        print("ERROR: Missing Kraken API credentials.")
        print("\nSet environment variables:")
        print('  export KRAKEN_API_KEY="your-api-key"')
        print('  export KRAKEN_API_SECRET="your-api-secret"')
        print("\nGet keys from: https://www.kraken.com/u/security/api")
        print("Required permission: 'Access WebSockets API' (no trading needed)")
        return
    
    
    print(f"[{datetime.now().isoformat()}] Getting WebSocket token...")
    try:
        ws_token = get_kraken_ws_token(api_key, api_secret)
        token_time = time.time()
        print(f"[{datetime.now().isoformat()}] Token acquired (valid 15 min)")
    except Exception as e:
        print(f"ERROR: Failed to get WebSocket token: {e}")
        return
    
    
    
    
    file_handle = open(filename, "a", buffering=8192)
    message_count = 0
    order_event_count = 0
    start_time = None
    
    
    
    
    
    def on_open(ws):
        """Send L3 subscription."""
        nonlocal start_time, ws_token, token_time
        start_time = time.time()
        
        
        if time.time() - token_time > 600:  
            print(f"[{datetime.now().isoformat()}] Refreshing token...")
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
        print(f"[{datetime.now().isoformat()}] Subscribed to {symbol} L3 (depth={depth})")
    
    def on_message(ws, message):
        """
        HOT PATH - Zero parsing, raw write.
        """
        nonlocal message_count, order_event_count
        
        
        if '"channel":"heartbeat"' in message:
            return
        if '"method":"subscribe"' in message:
            if '"success":true' in message:
                print(f"[{datetime.now().isoformat()}] Subscription confirmed")
            elif '"success":false' in message:
                print(f"[{datetime.now().isoformat()}] Subscription FAILED: {message}")
            return
        
        
        file_handle.write(message)
        file_handle.write("\n")
        message_count += 1
        
        
        order_event_count += message.count('"event"')
        
        
        if message_count % 50 == 0:
            file_handle.flush()
            elapsed = time.time() - start_time
            msg_rate = message_count / elapsed if elapsed > 0 else 0
            evt_rate = order_event_count / elapsed if elapsed > 0 else 0
            print(f"\r[Recording] {message_count:,} msgs | {order_event_count:,} order events | {evt_rate:.1f} evt/sec", end="", flush=True)
    
    def on_error(ws, error):
        print(f"\n[{datetime.now().isoformat()}] WebSocket error: {error}")
    
    def on_close(ws, close_status_code, close_msg):
        print(f"\n[{datetime.now().isoformat()}] Connection closed: {close_status_code} - {close_msg}")
        file_handle.flush()
    
    
    
    
    
    def run_with_reconnect():
        nonlocal ws_token, token_time
        reconnect_delay = 1
        max_delay = 30
        
        while True:
            try:
                
                if time.time() - token_time > 600:
                    print(f"[{datetime.now().isoformat()}] Refreshing token...")
                    ws_token = get_kraken_ws_token(api_key, api_secret)
                    token_time = time.time()
                
                print(f"[{datetime.now().isoformat()}] Connecting to Kraken L3 WebSocket...")
                
                ws = websocket.WebSocketApp(
                    "wss://ws-l3.kraken.com/v2",
                    on_open=on_open,
                    on_message=on_message,
                    on_error=on_error,
                    on_close=on_close
                )
                
                ws.run_forever(
                    ping_interval=30,
                    ping_timeout=10,
                )
                
                print(f"[{datetime.now().isoformat()}] Reconnecting in {reconnect_delay}s...")
                time.sleep(reconnect_delay)
                reconnect_delay = min(reconnect_delay * 2, max_delay)
                
            except KeyboardInterrupt:
                print(f"\n[{datetime.now().isoformat()}] Shutting down...")
                file_handle.flush()
                file_handle.close()
                print(f"Final: {message_count:,} messages, {order_event_count:,} order events → {filename}")
                break
            except Exception as e:
                print(f"\n[{datetime.now().isoformat()}] Error: {e}")
                time.sleep(reconnect_delay)
                reconnect_delay = min(reconnect_delay * 2, max_delay)
    
    run_with_reconnect()


if __name__ == "__main__":
    import argparse
    
    parser = argparse.ArgumentParser(
        description="Kraken L3 Market Data Recorder (FREE order-by-order data)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
This records TRUE L3 data with individual order IDs!

Event types you'll see:
  add     - New order placed (→ your ADD)
  modify  - Order partially filled (→ your MODIFY)  
  delete  - Order removed (→ your CANCEL or EXECUTE)

Setup:
  1. Create API key at https://www.kraken.com/u/security/api
  2. Enable "Access WebSockets API" permission
  3. Set environment variables:
     
     export KRAKEN_API_KEY="your-key"
     export KRAKEN_API_SECRET="your-secret"
     
  4. Run: python3 kraken_l3_recorder.py -o btc_l3.json -s BTC/USD

Popular symbols: BTC/USD, ETH/USD, SOL/USD, XRP/USD
        """
    )
    parser.add_argument("-o", "--output", default="kraken_l3.json", help="Output filename")
    parser.add_argument("-s", "--symbol", default="BTC/USD", help="Trading pair (e.g., BTC/USD)")
    parser.add_argument("-d", "--depth", type=int, default=1000, choices=[10, 100, 1000],
                        help="Order book depth (default: 1000)")
    args = parser.parse_args()
    
    print("=" * 60)
    print("Kraken L3 Market Data Recorder")
    print("=" * 60)
    print(f"Output: {args.output}")
    print(f"Symbol: {args.symbol}")
    print(f"Depth:  {args.depth} levels")
    print()
    print("This is TRUE L3 data with individual order IDs!")
    print("Perfect for feeding your TitanLOB engine.")
    print()
    print("Press Ctrl+C to stop")
    print("=" * 60)
    
    start_recording(
        filename=args.output,
        symbol=args.symbol,
        depth=args.depth
    )
