# LOB Project - Day 2: Individual Order Tracking with FIFO

## What I Built Today
✓ Order struct to represent individual orders
✓ Deque-based storage for FIFO priority at each price level
✓ Individual order display with aggregated totals

## Core Data Structure

### The Evolution
```cpp
// Day 1
std::map<double, int> bids;  // price -> total quantity

// Day 2
std::map<double, std::deque<Order>> bids;  // price -> queue of orders
```

**Why std::deque?**
- Maintains insertion order (FIFO guarantee)
- O(1) push_back (add new orders)
- O(1) front access (for matching later)
- Can iterate through all elements (for display)

**Alternatives I didn't use:**
- vector: could work, slightly less efficient for front operations
- queue: can't iterate through elements for display
- list: O(1) everywhere but worse cache performance

## Order Struct
```cpp
struct Order {
    std::string order_id;
    std::string side;
    double price;
    int quantity;
};
```
**Location:** Defined in order_book.h before function declarations (compiler needs to know about Order before seeing `deque<Order>`)

## Key Code Patterns

### Pattern 1: Brace Initialization
```cpp
bids[price].push_back({order_id, side, price, quantity});
```
**What happens:** Creates temporary Order with fields in struct order, adds to deque
**Why useful:** Cleaner than creating named Order variable first

### Pattern 2: Map Auto-Creation
```cpp
bids[100.0].push_back(order);  // even if 100.0 doesn't exist yet
```
**Behavior:**
- If key exists: returns reference to existing deque
- If key doesn't exist: creates empty deque, then returns reference
- No explicit check needed

### Pattern 3: Nested Iteration
```cpp
for (auto it = bids.rbegin(); it != bids.rend(); ++it) {
    std::cout << it->first << ": ";  // price level
    
    int total = 0;
    for (const auto& order : it->second) {  // orders at that price
        std::cout << "[" << order.order_id << ", " << order.quantity << "] ";
        total += order.quantity;
    }
    std::cout << "total: " << total << std::endl;
}
```
**Structure:** Outer loop = prices, Inner loop = orders at each price

## Implementation Details

### add_order()
```cpp
void add_order(const std::string& side, double price, int quantity,
               const std::string& order_id,
               std::map<double, std::deque<Order>>& bids,
               std::map<double, std::deque<Order>>& asks) {
    if (side == "buy") {
        bids[price].push_back({order_id, side, price, quantity});
    }
    else if (side == "sell") { 
        asks[price].push_back({order_id, side, price, quantity});
    }
}
```

### display_book()
- Bids: reverse iteration (rbegin → rend) for descending prices
- Asks: forward iteration (begin → end) for ascending prices
- Shows individual orders plus calculated total

## Actual Working Output
```
Bids:
100.5: [order1, 10] total: 10
100: [order2, 20] [order_A, 10] [order_B, 5] [order_C, 8] total: 43
Asks:
101: [order3, 5] total: 5
102: [order4, 15] total: 15
```

**FIFO verification:** At price 100, orders appear in insertion order

## C++ Syntax Notes

### const auto& in Range Loops
```cpp
for (const auto& order : it->second)
```
- `auto` - compiler deduces type is Order
- `&` - reference (avoid copying Order each iteration)
- `const` - promise not to modify

### Function Signature Changes
```cpp
// Old
void add_order(..., std::map<double, int>& bids, ...);

// New  
void add_order(..., std::map<double, std::deque<Order>>& bids, ...);
```
All functions using bids/asks needed signature updates

## What This Can't Do Yet (Day 3+)
- Cancel specific orders (need to find and remove from deque)
- Modify orders
- Match orders against each other

## Mental Models That Clicked
- Order book = two-level structure: map of price levels, each containing queue of orders
- FIFO comes free from deque + push_back (no extra logic needed)
- Each Order is independent but grouped by price

## Next Session Goals
- Implement cancel_order(order_id)
- Remove orders from middle of deque
- Handle edge cases (order not found, empty price level)