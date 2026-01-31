
"""
Binary message replayer - feeds .dat file to TCP gateway
"""
import socket
import struct
import time
import sys

def replay_binary_file(filename, host='localhost', port=9000, speed_multiplier=1.0):
    """
    Read binary .dat file and send messages to TCP gateway.
    
    Args:
        filename: Path to .dat file
        host: Gateway host
        port: Gateway port
        speed_multiplier: 1.0 = real-time, 10.0 = 10x speed, 0 = unlimited
    """
    
    print(f"Connecting to {host}:{port}...")
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    
    try:
        sock.connect((host, port))
        print(f"Connected! Replaying {filename}...")
        
        with open(filename, 'rb') as f:
            
            print("Reading messages...")
            
            message_count = 0
            start_time = time.time()
            last_report = start_time
            
            while True:
                
                header_data = f.read(11)
                if len(header_data) < 11:
                    break  
                
                
                msg_type, msg_length, timestamp = struct.unpack('<BHQ', header_data)
                
                
                if msg_length < 11 or msg_length > 1024:
                    print(f"\nERROR: Invalid message length {msg_length} at message {message_count}")
                    print(f"Message type: {msg_type}, timestamp: {timestamp}")
                    break
                
                
                remaining = msg_length - 11
                if remaining > 0:
                    body_data = f.read(remaining)
                    
                    if len(body_data) < remaining:
                        print(f"\nUnexpected EOF - wanted {remaining} bytes, got {len(body_data)}")
                        break
                    
                    complete_msg = header_data + body_data
                else:
                    complete_msg = header_data
                
                
                try:
                    sock.sendall(complete_msg)
                except BrokenPipeError:
                    print("\nConnection closed by server")
                    break
                
                message_count += 1
                
                
                if message_count % 10000 == 0:
                    elapsed = time.time() - start_time
                    rate = message_count / elapsed
                    print(f"\rMessages: {message_count:,} | Rate: {rate:.0f} msg/sec | Elapsed: {elapsed:.1f}s", 
                          end='', flush=True)
                
                
                if speed_multiplier > 0 and message_count % 100 == 0:
                    time.sleep(0.01 / speed_multiplier)
            
            print(f"\n\nReplay complete!")
            print(f"Total messages: {message_count:,}")
            print(f"Total time: {time.time() - start_time:.1f}s")
            
    except ConnectionRefusedError:
        print(f"ERROR: Could not connect to {host}:{port}")
        print("Is the GUI running? (./titan_gui)")
    except KeyboardInterrupt:
        print("\n\nStopped by user")
    finally:
        sock.close()

if __name__ == '__main__':
    import argparse
    
    parser = argparse.ArgumentParser(description='Replay binary order data to TCP gateway')
    parser.add_argument('file', help='Binary .dat file to replay')
    parser.add_argument('--host', default='localhost', help='Gateway host')
    parser.add_argument('--port', type=int, default=9000, help='Gateway port')
    parser.add_argument('--speed', type=float, default=100.0, 
                       help='Speed multiplier (1.0=realtime, 100=fast, 0=unlimited)')
    
    args = parser.parse_args()
    
    replay_binary_file(args.file, args.host, args.port, args.speed)
