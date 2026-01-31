#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cstdint>
#include <cstring>

#pragma pack(push, 1)

enum class MsgType : uint8_t {
    ADD_ORDER       = 'A',
    ADD_ICEBERG     = 'I',
    ADD_AON         = 'N',
    CANCEL_ORDER    = 'X',
    MODIFY_ORDER    = 'M',
    EXECUTE         = 'E',
    ADD_STOP        = 'S',
    ADD_STOP_MARKET = 'T',
    HEARTBEAT       = 'H',
    RESET           = 'R',
    SNAPSHOT_REQ    = 'Q',
};

enum class Side : uint8_t {
    BUY  = 'B',
    SELL = 'S'
};

enum class TIF : uint8_t {
    GTC = 0,
    IOC = 1,
    FOK = 2,
    AON = 3
};

struct MsgHeader {
    MsgType type;
    uint16_t length;
    uint64_t timestamp;
};
static_assert(sizeof(MsgHeader) == 11, "MsgHeader must be 11 bytes");

struct MsgAddOrder {
    MsgHeader header;
    uint64_t order_id;
    uint64_t user_id;
    Side side;
    int64_t price;
    int64_t quantity;
    
    static MsgAddOrder create(uint64_t ts, uint64_t oid, uint64_t uid, 
                              Side s, int64_t p, int64_t q) {
        MsgAddOrder msg{};
        msg.header.type = MsgType::ADD_ORDER;
        msg.header.length = sizeof(MsgAddOrder);
        msg.header.timestamp = ts;
        msg.order_id = oid;
        msg.user_id = uid;
        msg.side = s;
        msg.price = p;
        msg.quantity = q;
        return msg;
    }
};
static_assert(sizeof(MsgAddOrder) == 44, "MsgAddOrder must be 44 bytes");

struct MsgAddIceberg {
    MsgHeader header;
    uint64_t order_id;
    uint64_t user_id;
    Side side;
    int64_t price;
    int64_t total_quantity;
    int64_t visible_quantity;
    
    static MsgAddIceberg create(uint64_t ts, uint64_t oid, uint64_t uid,
                                Side s, int64_t p, int64_t total, int64_t visible) {
        MsgAddIceberg msg{};
        msg.header.type = MsgType::ADD_ICEBERG;
        msg.header.length = sizeof(MsgAddIceberg);
        msg.header.timestamp = ts;
        msg.order_id = oid;
        msg.user_id = uid;
        msg.side = s;
        msg.price = p;
        msg.total_quantity = total;
        msg.visible_quantity = visible;
        return msg;
    }
};
static_assert(sizeof(MsgAddIceberg) == 52, "MsgAddIceberg must be 52 bytes");

struct MsgAddAON {
    MsgHeader header;
    uint64_t order_id;
    uint64_t user_id;
    Side side;
    int64_t price;
    int64_t quantity;
    
    static MsgAddAON create(uint64_t ts, uint64_t oid, uint64_t uid,
                            Side s, int64_t p, int64_t q) {
        MsgAddAON msg{};
        msg.header.type = MsgType::ADD_AON;
        msg.header.length = sizeof(MsgAddAON);
        msg.header.timestamp = ts;
        msg.order_id = oid;
        msg.user_id = uid;
        msg.side = s;
        msg.price = p;
        msg.quantity = q;
        return msg;
    }
};
static_assert(sizeof(MsgAddAON) == 44, "MsgAddAON must be 44 bytes");

struct MsgCancel {
    MsgHeader header;
    uint64_t order_id;
    
    static MsgCancel create(uint64_t ts, uint64_t oid) {
        MsgCancel msg{};
        msg.header.type = MsgType::CANCEL_ORDER;
        msg.header.length = sizeof(MsgCancel);
        msg.header.timestamp = ts;
        msg.order_id = oid;
        return msg;
    }
};
static_assert(sizeof(MsgCancel) == 19, "MsgCancel must be 19 bytes");

struct MsgModify {
    MsgHeader header;
    uint64_t order_id;
    int64_t new_price;
    int64_t new_quantity;
    
    static MsgModify create(uint64_t ts, uint64_t oid, int64_t p, int64_t q) {
        MsgModify msg{};
        msg.header.type = MsgType::MODIFY_ORDER;
        msg.header.length = sizeof(MsgModify);
        msg.header.timestamp = ts;
        msg.order_id = oid;
        msg.new_price = p;
        msg.new_quantity = q;
        return msg;
    }
};
static_assert(sizeof(MsgModify) == 35, "MsgModify must be 35 bytes");

struct MsgExecute {
    MsgHeader header;
    uint64_t order_id;
    uint64_t user_id;
    Side side;
    int64_t price;
    int64_t quantity;
    TIF time_in_force;
    
    static MsgExecute create(uint64_t ts, uint64_t oid, uint64_t uid,
                             Side s, int64_t p, int64_t q, TIF tif) {
        MsgExecute msg{};
        msg.header.type = MsgType::EXECUTE;
        msg.header.length = sizeof(MsgExecute);
        msg.header.timestamp = ts;
        msg.order_id = oid;
        msg.user_id = uid;
        msg.side = s;
        msg.price = p;
        msg.quantity = q;
        msg.time_in_force = tif;
        return msg;
    }
    
    static MsgExecute market_buy(uint64_t ts, uint64_t oid, uint64_t uid, int64_t q) {
        return create(ts, oid, uid, Side::BUY, INT64_MAX, q, TIF::IOC);
    }
    
    static MsgExecute market_sell(uint64_t ts, uint64_t oid, uint64_t uid, int64_t q) {
        return create(ts, oid, uid, Side::SELL, 0, q, TIF::IOC);
    }
};
static_assert(sizeof(MsgExecute) == 45, "MsgExecute must be 45 bytes");

struct MsgAddStop {
    MsgHeader header;
    uint64_t order_id;
    uint64_t user_id;
    Side side;
    int64_t trigger_price;
    int64_t limit_price;
    int64_t quantity;
    uint8_t is_market;
    
    static MsgAddStop create(uint64_t ts, uint64_t oid, uint64_t uid, Side s,
                             int64_t trigger, int64_t limit, int64_t q, bool is_mkt = false) {
        MsgAddStop msg{};
        msg.header.type = MsgType::ADD_STOP;
        msg.header.length = sizeof(MsgAddStop);
        msg.header.timestamp = ts;
        msg.order_id = oid;
        msg.user_id = uid;
        msg.side = s;
        msg.trigger_price = trigger;
        msg.limit_price = limit;
        msg.quantity = q;
        msg.is_market = is_mkt ? 1 : 0;
        return msg;
    }
};
static_assert(sizeof(MsgAddStop) == 53, "MsgAddStop must be 53 bytes");

struct MsgHeartbeat {
    MsgHeader header;
    
    static MsgHeartbeat create(uint64_t ts) {
        MsgHeartbeat msg{};
        msg.header.type = MsgType::HEARTBEAT;
        msg.header.length = sizeof(MsgHeartbeat);
        msg.header.timestamp = ts;
        return msg;
    }
};
static_assert(sizeof(MsgHeartbeat) == 11, "MsgHeartbeat must be 11 bytes");

struct MsgReset {
    MsgHeader header;
    
    static MsgReset create(uint64_t ts) {
        MsgReset msg{};
        msg.header.type = MsgType::RESET;
        msg.header.length = sizeof(MsgReset);
        msg.header.timestamp = ts;
        return msg;
    }
};
static_assert(sizeof(MsgReset) == 11, "MsgReset must be 11 bytes");

#pragma pack(pop)

#pragma pack(push, 1)

enum class OutMsgType : uint8_t {
    TRADE           = 'T',
    ORDER_ACCEPTED  = 'A',
    ORDER_REJECTED  = 'R',
    ORDER_CANCELLED = 'C',
    BOOK_UPDATE     = 'U',
};

struct OutMsgHeader {
    OutMsgType type;
    uint16_t length;
    uint64_t timestamp;
};
static_assert(sizeof(OutMsgHeader) == 11, "OutMsgHeader must be 11 bytes");

struct OutTrade {
    OutMsgHeader header;
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    int64_t price;
    int64_t quantity;
};
static_assert(sizeof(OutTrade) == 43, "OutTrade must be 43 bytes");

struct OutOrderAccepted {
    OutMsgHeader header;
    uint64_t order_id;
    Side side;
    int64_t price;
    int64_t quantity;
};
static_assert(sizeof(OutOrderAccepted) == 36, "OutOrderAccepted must be 36 bytes");

struct OutOrderCancelled {
    OutMsgHeader header;
    uint64_t order_id;
    int64_t cancelled_quantity;
};
static_assert(sizeof(OutOrderCancelled) == 27, "OutOrderCancelled must be 27 bytes");

#pragma pack(pop)

template<typename T>
inline const T* msg_cast(const MsgHeader* header) {
    return reinterpret_cast<const T*>(header);
}

template<typename T>
inline const T* msg_cast(const void* data) {
    return reinterpret_cast<const T*>(data);
}

inline bool side_to_bool(Side s) {
    return s == Side::BUY;
}

inline Side bool_to_side(bool is_buy) {
    return is_buy ? Side::BUY : Side::SELL;
}

#endif
