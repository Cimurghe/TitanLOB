
"""
DeepFlow Binary Log Inspector
=============================
Reads and analyzes binary market data logs from TitanLOB.

Usage:
    python inspect_deepflow.py market_data.deepflow [--limit N] [--stats] [--trades-only]
"""

import struct
import sys
import argparse
from collections import defaultdict
from dataclasses import dataclass
from typing import Optional, Generator
import os






HEADER_SIZE = 56
HEADER_FORMAT = '<Q I I Q 4Q'  
HEADER_MAGIC = 0x574F4C46504545  
HEADER_VERSION = 1


MSG_SIZE = 64


MSG_TYPE_TRADE = ord('T')
MSG_TYPE_ACCEPTED = ord('A')
MSG_TYPE_REJECTED = ord('R')
MSG_TYPE_CANCELLED = ord('C')
MSG_TYPE_BOOK_UPDATE = ord('U')


SIDE_BUY = ord('B')
SIDE_SELL = ord('S')


@dataclass
class FileHeader:
    magic: int
    version: int
    msg_size: int
    timestamp_start: int
    
    def is_valid(self) -> bool:
        return (self.magic == HEADER_MAGIC and 
                self.version == HEADER_VERSION and 
                self.msg_size == MSG_SIZE)


@dataclass
class TradeMsg:
    timestamp: int
    buy_order_id: int
    sell_order_id: int
    price: int
    quantity: int


@dataclass
class AcceptedMsg:
    timestamp: int
    order_id: int
    side: str  
    price: int
    quantity: int


@dataclass
class CancelledMsg:
    timestamp: int
    order_id: int
    cancelled_qty: int


def read_header(f) -> Optional[FileHeader]:
    """Read and validate file header."""
    data = f.read(HEADER_SIZE)
    if len(data) < HEADER_SIZE:
        return None
    
    unpacked = struct.unpack(HEADER_FORMAT, data)
    header = FileHeader(
        magic=unpacked[0],
        version=unpacked[1],
        msg_size=unpacked[2],
        timestamp_start=unpacked[3]
    )
    return header


def parse_message(data: bytes):
    """Parse a single OutputMsg from raw bytes."""
    if len(data) < MSG_SIZE:
        return None
    
    
    msg_type = data[0]
    timestamp = struct.unpack('<Q', data[8:16])[0]
    
    if msg_type == MSG_TYPE_TRADE:
        
        buy_id, sell_id, price, qty = struct.unpack('<Q Q q q', data[16:48])
        return TradeMsg(timestamp, buy_id, sell_id, price, qty)
    
    elif msg_type == MSG_TYPE_ACCEPTED:
        
        order_id = struct.unpack('<Q', data[16:24])[0]
        side = 'BUY' if data[24] == SIDE_BUY else 'SELL'
        price, qty = struct.unpack('<q q', data[32:48])
        return AcceptedMsg(timestamp, order_id, side, price, qty)
    
    elif msg_type == MSG_TYPE_CANCELLED:
        
        order_id, cancelled_qty = struct.unpack('<Q q', data[16:32])
        return CancelledMsg(timestamp, order_id, cancelled_qty)
    
    return None


def read_messages(f) -> Generator:
    """Generator that yields parsed messages from file."""
    while True:
        data = f.read(MSG_SIZE)
        if len(data) < MSG_SIZE:
            break
        msg = parse_message(data)
        if msg:
            yield msg


def format_price(price_cents: int) -> str:
    """Format price from cents to dollars."""
    return f"${price_cents / 100:.2f}"


def format_timestamp(ns: int) -> str:
    """Format nanosecond timestamp to human-readable."""
    
    secs = ns / 1e9
    ms = (ns % 1_000_000_000) / 1e6
    return f"{secs:.3f}s ({ms:.3f}ms)"


def print_message(msg, index: int):
    """Pretty-print a single message."""
    if isinstance(msg, TradeMsg):
        print(f"[{index:>6}] TRADE     | ts={format_timestamp(msg.timestamp)} | "
              f"buy={msg.buy_order_id} sell={msg.sell_order_id} | "
              f"price={format_price(msg.price)} qty={msg.quantity}")
    elif isinstance(msg, AcceptedMsg):
        side_color = '\033[92m' if msg.side == 'BUY' else '\033[91m'
        reset = '\033[0m'
        print(f"[{index:>6}] ACCEPTED  | ts={format_timestamp(msg.timestamp)} | "
              f"id={msg.order_id} {side_color}{msg.side:4}{reset} | "
              f"price={format_price(msg.price)} qty={msg.quantity}")
    elif isinstance(msg, CancelledMsg):
        print(f"[{index:>6}] CANCELLED | ts={format_timestamp(msg.timestamp)} | "
              f"id={msg.order_id} | cancelled_qty={msg.cancelled_qty}")


def compute_stats(filepath: str) -> dict:
    """Compute aggregate statistics from the log file."""
    stats = {
        'total_messages': 0,
        'trades': 0,
        'accepted': 0,
        'cancelled': 0,
        'total_volume': 0,
        'total_value': 0,
        'buy_orders': 0,
        'sell_orders': 0,
        'min_price': float('inf'),
        'max_price': float('-inf'),
        'first_timestamp': None,
        'last_timestamp': None,
        'prices': [],
    }
    
    with open(filepath, 'rb') as f:
        header = read_header(f)
        if not header or not header.is_valid():
            return stats
        
        for msg in read_messages(f):
            stats['total_messages'] += 1
            
            if stats['first_timestamp'] is None:
                stats['first_timestamp'] = msg.timestamp
            stats['last_timestamp'] = msg.timestamp
            
            if isinstance(msg, TradeMsg):
                stats['trades'] += 1
                stats['total_volume'] += msg.quantity
                stats['total_value'] += msg.price * msg.quantity
                stats['min_price'] = min(stats['min_price'], msg.price)
                stats['max_price'] = max(stats['max_price'], msg.price)
                stats['prices'].append(msg.price)
            elif isinstance(msg, AcceptedMsg):
                stats['accepted'] += 1
                if msg.side == 'BUY':
                    stats['buy_orders'] += 1
                else:
                    stats['sell_orders'] += 1
            elif isinstance(msg, CancelledMsg):
                stats['cancelled'] += 1
    
    return stats


def print_stats(stats: dict):
    """Pretty-print statistics."""
    print("\n" + "=" * 60)
    print("  DEEPFLOW BINARY LOG STATISTICS")
    print("=" * 60)
    
    print(f"\n  Total Messages:    {stats['total_messages']:,}")
    print(f"  ├─ Trades:         {stats['trades']:,}")
    print(f"  ├─ Accepted:       {stats['accepted']:,}")
    print(f"  └─ Cancelled:      {stats['cancelled']:,}")
    
    if stats['accepted'] > 0:
        print(f"\n  Order Breakdown:")
        print(f"  ├─ Buy Orders:     {stats['buy_orders']:,}")
        print(f"  └─ Sell Orders:    {stats['sell_orders']:,}")
    
    if stats['trades'] > 0:
        vwap = stats['total_value'] / stats['total_volume'] if stats['total_volume'] > 0 else 0
        print(f"\n  Trade Statistics:")
        print(f"  ├─ Total Volume:   {stats['total_volume']:,}")
        print(f"  ├─ VWAP:           {format_price(int(vwap))}")
        print(f"  ├─ Min Price:      {format_price(stats['min_price'])}")
        print(f"  └─ Max Price:      {format_price(stats['max_price'])}")
        
        if len(stats['prices']) > 1:
            avg_price = sum(stats['prices']) / len(stats['prices'])
            print(f"  └─ Avg Price:      {format_price(int(avg_price))}")
    
    if stats['first_timestamp'] and stats['last_timestamp']:
        duration_ns = stats['last_timestamp'] - stats['first_timestamp']
        duration_s = duration_ns / 1e9
        print(f"\n  Time Span:")
        print(f"  └─ Duration:       {duration_s:.3f} seconds")
        
        if duration_s > 0:
            msg_rate = stats['total_messages'] / duration_s
            print(f"  └─ Message Rate:   {msg_rate:,.0f} msg/s")
    
    print("\n" + "=" * 60)


def main():
    parser = argparse.ArgumentParser(
        description='Inspect DeepFlow binary market data logs',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s market_data.deepflow                 # Show first 50 messages
  %(prog)s market_data.deepflow --limit 100     # Show first 100 messages
  %(prog)s market_data.deepflow --stats         # Show statistics only
  %(prog)s market_data.deepflow --trades-only   # Show only trade messages
        """
    )
    parser.add_argument('file', help='Path to .deepflow binary log file')
    parser.add_argument('--limit', '-n', type=int, default=50,
                        help='Maximum number of messages to display (default: 50)')
    parser.add_argument('--stats', '-s', action='store_true',
                        help='Show statistics only')
    parser.add_argument('--trades-only', '-t', action='store_true',
                        help='Show only trade messages')
    
    args = parser.parse_args()
    
    if not os.path.exists(args.file):
        print(f"Error: File not found: {args.file}", file=sys.stderr)
        sys.exit(1)
    
    file_size = os.path.getsize(args.file)
    estimated_msgs = (file_size - HEADER_SIZE) // MSG_SIZE
    
    print(f"File: {args.file}")
    print(f"Size: {file_size:,} bytes ({file_size / (1024*1024):.2f} MB)")
    print(f"Estimated messages: {estimated_msgs:,}")
    
    
    with open(args.file, 'rb') as f:
        header = read_header(f)
        if not header:
            print("Error: Could not read file header", file=sys.stderr)
            sys.exit(1)
        
        if not header.is_valid():
            print(f"Error: Invalid file header", file=sys.stderr)
            print(f"  Magic: 0x{header.magic:X} (expected 0x{HEADER_MAGIC:X})")
            print(f"  Version: {header.version} (expected {HEADER_VERSION})")
            print(f"  Msg Size: {header.msg_size} (expected {MSG_SIZE})")
            sys.exit(1)
        
        print(f"Header: Valid (version={header.version}, msg_size={header.msg_size})")
    
    if args.stats:
        stats = compute_stats(args.file)
        print_stats(stats)
        return
    
    
    print("\n" + "-" * 80)
    
    with open(args.file, 'rb') as f:
        read_header(f)  
        
        count = 0
        for msg in read_messages(f):
            if args.trades_only and not isinstance(msg, TradeMsg):
                continue
            
            print_message(msg, count)
            count += 1
            
            if count >= args.limit:
                break
        
        print("-" * 80)
        print(f"Displayed {count} messages (use --limit to show more, --stats for summary)")


if __name__ == '__main__':
    main()