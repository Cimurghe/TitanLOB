#ifndef OUTPUT_MSG_H
#define OUTPUT_MSG_H

#include <cstdint>
#include "protocol.h"

struct OutputMsg {
    OutMsgType type;
    uint64_t timestamp;
    
    union {
        struct {
            uint64_t buy_order_id;
            uint64_t sell_order_id;
            int64_t price;
            int64_t quantity;
        } trade;
        
        struct {
            uint64_t order_id;
            Side side;
            int64_t price;
            int64_t quantity;
        } accepted;
        
        struct {
            uint64_t order_id;
            int64_t cancelled_qty;
        } cancelled;
        
        uint8_t _pad[48];
    };
    
    static OutputMsg make_trade(uint64_t ts, uint64_t buy_id, uint64_t sell_id, 
                                int64_t price, int64_t qty) {
        OutputMsg msg{};
        msg.type = OutMsgType::TRADE;
        msg.timestamp = ts;
        msg.trade.buy_order_id = buy_id;
        msg.trade.sell_order_id = sell_id;
        msg.trade.price = price;
        msg.trade.quantity = qty;
        return msg;
    }
    
    static OutputMsg make_accepted(uint64_t ts, uint64_t order_id, Side side, 
                                   int64_t price, int64_t qty) {
        OutputMsg msg{};
        msg.type = OutMsgType::ORDER_ACCEPTED;
        msg.timestamp = ts;
        msg.accepted.order_id = order_id;
        msg.accepted.side = side;
        msg.accepted.price = price;
        msg.accepted.quantity = qty;
        return msg;
    }
    
    static OutputMsg make_cancelled(uint64_t ts, uint64_t order_id, int64_t qty) {
        OutputMsg msg{};
        msg.type = OutMsgType::ORDER_CANCELLED;
        msg.timestamp = ts;
        msg.cancelled.order_id = order_id;
        msg.cancelled.cancelled_qty = qty;
        return msg;
    }
};

static_assert(sizeof(OutputMsg) <= 64, "OutputMsg should fit in a cache line");

#endif
