
"""
Kraken L3 → TitanLOB Binary Normalizer
======================================
Converts Kraken JSON (NDJSON) to packed binary structs matching protocol.h

Mapping:
    Kraken 'add'    → MsgAddOrder (44 bytes)
    Kraken 'modify' → MsgModify   (35 bytes)
    Kraken 'delete' → MsgCancel   (19 bytes)

Order ID normalization:
    Kraken uses strings like "OTGUHB-NZBQO-O2WSMU"
    We map to sequential uint64: 0, 1, 2, ...
    This enables Day 19's "God Array" (O(1) lookup by array index)
"""

import struct
import json
import sys
from datetime import datetime
from pathlib import Path







MSG_ADD_ORDER    = ord('A')  
MSG_CANCEL_ORDER = ord('X')  
MSG_MODIFY_ORDER = ord('M')  


SIDE_BUY  = ord('B')  
SIDE_SELL = ord('S')  



HEADER_FMT = '<BHQ'
HEADER_SIZE = 11


ADD_ORDER_FMT = '<BHQ QQ B qq'
ADD_ORDER_SIZE = 44


CANCEL_FMT = '<BHQ Q'
CANCEL_SIZE = 19


MODIFY_FMT = '<BHQ Q qq'
MODIFY_SIZE = 35








PRICE_MULTIPLIER = 100  

def price_to_ticks(price_float: float) -> int:
    """Convert float price to integer ticks."""
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






class OrderIdMapper:
    """Maps Kraken string order IDs to sequential uint64."""
    
    def __init__(self):
        self.id_map: dict[str, int] = {}
        self.next_id: int = 0
    
    def get_or_create(self, kraken_id: str) -> int:
        """Get existing mapping or create new one."""
        if kraken_id not in self.id_map:
            self.id_map[kraken_id] = self.next_id
            self.next_id += 1
        return self.id_map[kraken_id]
    
    def get(self, kraken_id: str) -> int | None:
        """Get existing mapping, return None if not found."""
        return self.id_map.get(kraken_id)
    
    def __len__(self):
        return len(self.id_map)






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






def normalize(input_path: str, output_path: str, verbose: bool = True):
    """
    Convert Kraken L3 NDJSON to binary protocol format.
    
    Args:
        input_path: Path to Kraken JSON file (NDJSON)
        output_path: Path to output binary file
        verbose: Print progress
    """
    
    id_mapper = OrderIdMapper()
    
    stats = {
        'lines': 0,
        'add': 0,
        'modify': 0,
        'delete': 0,
        'skipped': 0,
        'bytes_written': 0,
    }
    
    input_size = Path(input_path).stat().st_size
    
    with open(input_path, 'r') as infile, open(output_path, 'wb') as outfile:
        
        for line_num, line in enumerate(infile, 1):
            line = line.strip()
            if not line:
                continue
            
            stats['lines'] += 1
            
            try:
                msg = json.loads(line)
            except json.JSONDecodeError as e:
                if verbose:
                    print(f"[WARN] Line {line_num}: JSON parse error: {e}")
                stats['skipped'] += 1
                continue
            
            
            if msg.get('channel') != 'level3':
                stats['skipped'] += 1
                continue
            
            
            if msg.get('type') == 'snapshot':
                
                for book in msg.get('data', []):
                    for order in book.get('bids', []):
                        process_snapshot_order(order, SIDE_BUY, id_mapper, outfile, stats)
                    for order in book.get('asks', []):
                        process_snapshot_order(order, SIDE_SELL, id_mapper, outfile, stats)
                continue
            
            
            if msg.get('type') != 'update':
                stats['skipped'] += 1
                continue
            
            for book in msg.get('data', []):
                
                for event in book.get('bids', []):
                    process_event(event, SIDE_BUY, id_mapper, outfile, stats)
                
                
                for event in book.get('asks', []):
                    process_event(event, SIDE_SELL, id_mapper, outfile, stats)
            
            
            if verbose and stats['lines'] % 100_000 == 0:
                total_events = stats['add'] + stats['modify'] + stats['delete']
                print(f"\r{stats['lines']:,} lines | {total_events:,} events | {len(id_mapper):,} unique orders", end='', flush=True)
    
    if verbose:
        print()  
        print_stats(stats, id_mapper, output_path)
    
    return stats


def process_snapshot_order(order: dict, side: int, id_mapper: OrderIdMapper, outfile, stats: dict):
    """Process an order from a snapshot (treat as ADD)."""
    kraken_id = order.get('order_id')
    if not kraken_id:
        return
    
    order_id = id_mapper.get_or_create(kraken_id)
    timestamp = iso_to_nanos(order['timestamp'])
    price = price_to_ticks(order['limit_price'])
    quantity = quantity_to_int(order['order_qty'])
    
    data = pack_add_order(timestamp, order_id, side, price, quantity)
    outfile.write(data)
    stats['add'] += 1
    stats['bytes_written'] += len(data)


def process_event(event: dict, side: int, id_mapper: OrderIdMapper, outfile, stats: dict):
    """Process a single order event."""
    
    event_type = event.get('event')
    kraken_id = event.get('order_id')
    
    if not kraken_id:
        stats['skipped'] += 1
        return
    
    timestamp = iso_to_nanos(event['timestamp'])
    
    if event_type == 'add':
        order_id = id_mapper.get_or_create(kraken_id)
        price = price_to_ticks(event['limit_price'])
        quantity = quantity_to_int(event['order_qty'])
        
        data = pack_add_order(timestamp, order_id, side, price, quantity)
        outfile.write(data)
        stats['add'] += 1
        stats['bytes_written'] += len(data)
    
    elif event_type == 'delete':
        
        order_id = id_mapper.get(kraken_id)
        if order_id is None:
            
            order_id = id_mapper.get_or_create(kraken_id)
        
        data = pack_cancel(timestamp, order_id)
        outfile.write(data)
        stats['delete'] += 1
        stats['bytes_written'] += len(data)
    
    elif event_type == 'modify':
        order_id = id_mapper.get(kraken_id)
        if order_id is None:
            order_id = id_mapper.get_or_create(kraken_id)
        
        price = price_to_ticks(event['limit_price'])
        quantity = quantity_to_int(event['order_qty'])
        
        data = pack_modify(timestamp, order_id, price, quantity)
        outfile.write(data)
        stats['modify'] += 1
        stats['bytes_written'] += len(data)
    
    else:
        stats['skipped'] += 1


def print_stats(stats: dict, id_mapper: OrderIdMapper, output_path: str):
    """Print conversion statistics."""
    
    total_events = stats['add'] + stats['modify'] + stats['delete']
    output_size = Path(output_path).stat().st_size
    
    print("=" * 60)
    print("NORMALIZATION COMPLETE")
    print("=" * 60)
    print(f"Input lines:      {stats['lines']:,}")
    print(f"Total events:     {total_events:,}")
    print(f"  ADD:            {stats['add']:,}")
    print(f"  MODIFY:         {stats['modify']:,}")
    print(f"  DELETE/CANCEL:  {stats['delete']:,}")
    print(f"  Skipped:        {stats['skipped']:,}")
    print(f"Unique orders:    {len(id_mapper):,}")
    print(f"Output size:      {output_size:,} bytes ({output_size/1024/1024:.2f} MB)")
    print(f"Output file:      {output_path}")
    print("=" * 60)
    
    
    expected_size = (stats['add'] * ADD_ORDER_SIZE + 
                    stats['modify'] * MODIFY_SIZE + 
                    stats['delete'] * CANCEL_SIZE)
    if expected_size != output_size:
        print(f"[WARN] Size mismatch: expected {expected_size}, got {output_size}")






if __name__ == '__main__':
    import argparse
    
    parser = argparse.ArgumentParser(
        description='Convert Kraken L3 JSON to TitanLOB binary format',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Example:
  python kraken_normalizer.py btc_l3.json btc_l3.dat

Output format (protocol.h):
  MsgAddOrder:  44 bytes (type='A')
  MsgModify:    35 bytes (type='M')  
  MsgCancel:    19 bytes (type='X')

The .dat file can be memory-mapped and fed directly to your C++ engine.
        """
    )
    parser.add_argument('input', help='Input Kraken JSON file (NDJSON)')
    parser.add_argument('output', help='Output binary file (.dat)')
    parser.add_argument('-q', '--quiet', action='store_true', help='Suppress progress output')
    
    args = parser.parse_args()
    
    print(f"Converting: {args.input} → {args.output}")
    print()
    
    normalize(args.input, args.output, verbose=not args.quiet)
