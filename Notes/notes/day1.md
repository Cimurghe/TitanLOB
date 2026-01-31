# LOB Project - Day 1: Basic Order Book State

## What I Built Today
✓ Two-sided order book with price aggregation
✓ Add orders to correct side (buy/sell)
✓ Display book with proper price ordering

## Core Data Structure

### The Foundation
```cpp
std::map<double, int> bids;  // price -> total quantity
std::map<double, int> asks;  // price -> total quantity
```

**Why std::map?**
- Keys auto-sorted (no manual sorting needed)
- O(log n) insert/lookup
- O(1) access to best prices via begin()/rbegin()
- Red-black tree internally

**Alternatives I didn't use:**
- vector: would need manual sorting, O(n) for best price
- unordered_map: no ordering, O(n) to find best price
- skip list: not in C++ stdlib, would need to implement

## Key Code Patterns

### Pattern 1: Route and Aggregate
```cpp
void add_order(const string& side, double price, int quantity, ...) {
    if (side == "buy") {
        bids[price] += quantity;  // creates entry if doesn't exist
    }
    else if (side == "sell") {
        asks[price] += quantity;
    }
}
```
**What happens:** `map[key] += value` either creates new entry (starts at 0) or adds to existing

### Pattern 2: Iterate Forward (Ascending)
```cpp
for (auto it = asks.begin(); it != asks.end(); ++it) {
    cout << it->first << ": " << it->second << endl;
}
```
**Use for:** Asks (want lowest price first)

### Pattern 3: Iterate Backward (Descending)
```cpp
for (auto it = bids.rbegin(); it != bids.rend(); ++it) {
    cout << it->first << ": " << it->second << endl;
}
```
**Use for:** Bids (want highest price first)

### Pattern 4: Iterator Access
```cpp
it->first   // the key (price)
it->second  // the value (quantity)
```

## C++ Syntax I Learned/Reviewed

### const and &
```cpp
void func(const map<double, int>& data)
         // ^                     ^
         // |                     |
         // can't modify          pass by reference (no copy)
```
**When to use:**
- `const Type&` - reading large data, don't want to copy
- `Type&` - modifying data in place
- `Type` - small data (int, double), copying is fine

### Function Signatures Must Match
```cpp
// order_book.h
void add_order(const string& side, ...);

// order_book.cpp  
void add_order(const string& side, ...) {  // EXACT SAME signature
    // implementation
}
```

## Project Structure
```
LOB/
├── order_book.h       // declarations only
├── order_book.cpp     // implementations
├── main.cpp           // test driver
└── Tests/
    └── test1.txt      // (not used yet)
```

## Compilation & Running
```bash
g++ -std=c++17 -Wall -Wextra -O2 main.cpp order_book.cpp -o lob
lob.exe  # Windows
./lob    # Unix/Mac
```

## Actual Working Output
```
Bids:
100.5: 10
100: 20
Asks:
101: 5
102: 15
```

## What This Can't Do Yet (Day 2+)
- Track individual orders (only sees aggregated quantities)
- Maintain FIFO priority within price level
- Cancel specific orders
- Match orders against each other

## Next Session Goals
- Change `map<double, int>` to `map<double, queue<Order>>`
- Create Order struct
- Track individual orders for FIFO

## Mental Models That Clicked
- LOB = two sorted price maps, nothing more
- std::map automatically maintains sort invariant
- Iterator direction determines display order
- This is just state management - matching comes later

## Questions for Later
- std::map vs skip list performance tradeoff?
- When is O(log n) vs O(1) actually noticeable?
- How do real exchanges handle millions of price levels?