#include <stdarg.h>
#include <string.h>

#include "../common/mtcp_shared.h"
#include "../common/linux_klist.h"
#include "mocktcp_dev.h"


#define CONCAT2(x, y)                    x##y
#define CONCAT(x, y)                     CONCAT2(x, y)

typedef enum {
    RS_HEADER,
    RS_DATA,
} rstate_t;

typedef enum {
    THS_HEADER_SEND_NEXT,
    THS_DATA_SEND_NEXT,
    THS_CALLBACK_NEXT,
    THS_FREE_NEXT
} tsh_state_t;

typedef enum {
    RIGID,
    FLEXIBLE
} rxflex_t;

typedef struct {
    uint32_t id;
    uint8_t *packet_ptr;
    uint32_t packet_sz;
    uint32_t remaining;
    rxflex_t flex_type; 
    union {
        struct {
            uint32_t sz;
        } rig;
        struct {
            uint32_t recvsz;
            uint32_t maxsz;
            uint32_t *retsz;
        } flex;
    } flexinfo;
    callback_t callback;
    void *cbarg;
    struct list_head link;
} rx_req_t;

typedef struct {
    uint32_t id;
    uint8_t *packet_ptr;
    uint32_t packet_sz;
    uint32_t remaining;
    callback_t callback;
    void *cbarg;
    struct list_head link;
} tx_req_t;

typedef struct {
    info_t header;
    tsh_state_t state;
    tx_req_t *req;
    struct list_head link;
} tx_mheader_t;
#define to_tx_mheader(x) container_of(x, tx_mheader_t, header)

typedef struct {
    info_t header;
    rx_req_t *req;
    struct list_head link;
} rx_mheader_t;
#define to_rx_mheader(x) container_of(x, rx_mheader_t, header)


static rx_mheader_t rx_headers[CONFIG_MTCP_NUM_RX_HEADERS];
static tx_mheader_t tx_headers[CONFIG_MTCP_NUM_TX_HEADERS];
static rx_req_t     rx_requests[CONFIG_MTCP_NUM_RX_REQUESTS];
static tx_req_t     tx_requests[CONFIG_MTCP_NUM_TX_REQUESTS];
#if defined(CONFIG_MTCP_STATS)
static uint32_t rx_heads_in_use = 0, max_rx_heads_used = 0;
static uint32_t tx_heads_in_use = 0, max_tx_heads_used = 0;
static uint32_t rx_reqs_in_use = 0, max_rx_reqs_used = 0;
static uint32_t tx_reqs_in_use = 0, max_tx_reqs_used = 0;
#define inc_usage(which) do { \
    CONCAT(which, s_in_use)++; \
    if(CONCAT(which, s_in_use) > CONCAT(CONCAT(max_, which), s_used)) \
        CONCAT(CONCAT(max_, which), s_used) = CONCAT(which, s_in_use); \
} while(0)
#define dec_usage(which) do { \
    CONCAT(which, s_in_use)--; \
} while(0)
#else
#define inc_usage(which)
#define dec_usage(which)
#endif

// US only
static LIST_HEAD(tx_header_free_list);
#define get_a_new_tx_header() ({ \
    if(list_empty(&tx_header_free_list)) \
        arch_BUG(); \
    tx_mheader_t *CONCAT(th_, __LINE__) = list_first_entry(&tx_header_free_list, tx_mheader_t, link); \
    list_del(&CONCAT(th_, __LINE__)->link); \
    inc_usage(tx_head); \
    CONCAT(th_, __LINE__); \
})
#define install_new_tx_header(__h, __state) ({ \
    tx_mheader_t *CONCAT(thi_, __LINE__) = __h ? __h : get_a_new_tx_header(); \
    /* Although this is shared, we dont need to critical scope this \
     * because it is only used under the guard of tx_free and backend_send \
     */ \
    if(current_tx_header) \
        arch_BUG(); \
    current_tx_header = &CONCAT(thi_, __LINE__)->header; \
    CONCAT(thi_, __LINE__)->state = __state; \
    current_tx_header; \
})
#define tx_mheader_free(x) do { \
    list_add_tail(&to_tx_mheader(x)->link, &tx_header_free_list); \
    dec_usage(tx_head); \
} while(0)

// US only
static LIST_HEAD(tx_requests_free_list);
#define get_a_new_tx_request() ({ \
    if(list_empty(&tx_requests_free_list)) \
        arch_BUG(); \
    tx_req_t *CONCAT(tq_, __LINE__) = list_first_entry(&tx_requests_free_list, tx_req_t, link); \
    list_del(&CONCAT(tq_, __LINE__)->link); \
    inc_usage(tx_req); \
    CONCAT(tq_, __LINE__); \
})
#define tx_req_free(x) do { \
    list_add_tail(&x->link, &tx_requests_free_list); \
    dec_usage(tx_req); \
} while(0)

static LIST_HEAD(rx_requests_free_list);
#define get_a_new_rx_request() ({ \
    if(list_empty(&rx_requests_free_list)) \
        arch_BUG(); \
    rx_req_t *CONCAT(rq_, __LINE__) = list_first_entry(&rx_requests_free_list, rx_req_t, link); \
    list_del(&CONCAT(rq_, __LINE__)->link); \
    inc_usage(rx_req); \
    CONCAT(rq_, __LINE__); \
})
#define rx_req_free(x) do { \
    list_add_tail(&x->link, &rx_requests_free_list); \
    dec_usage(rx_req); \
} while(0)
static LIST_HEAD(rx_pending_requests_list);
#define push_to_pending_rx_requests_queue(x) do { \
    list_add_tail(&(x)->link, &rx_pending_requests_list); \
} while (0)
/* used in @find_first_in_rx_pending_requests_list@ for searching
 * and then @list_del is called conditionally inside
 * US
 */

static LIST_HEAD(tx_pending_list);
#define push_to_pending_tx_queue(x) do { \
    list_add_tail(&to_tx_mheader(x)->link, &tx_pending_list); \
} while (0)

static LIST_HEAD(rx_usawaiting_list);

// shared
static LIST_HEAD(rx_header_free_list);
#define get_a_new_rx_header() ({ \
    if(list_empty(&rx_header_free_list)) \
        arch_BUG(); \
    rx_mheader_t *CONCAT(rh_, __LINE__) = list_first_entry(&rx_header_free_list, rx_mheader_t, link); \
    list_del(&CONCAT(rh_, __LINE__)->link); \
    inc_usage(rx_head); \
    CONCAT(rh_, __LINE__); \
})
#define rx_mheader_free(x) do { \
    list_add_tail(&to_rx_mheader(x)->link, &rx_header_free_list); \
    dec_usage(rx_head); \
} while(0)
#define rx_mheader_free_with_lock(x) do { \
    uint32_t CONCAT(istate_, __LINE__) = arch_disable_interrupts(); \
    { \
        rx_mheader_free(x); \
    } \
    arch_restore_interrupts(CONCAT(istate_, __LINE__)); \
} while(0)

static LIST_HEAD(tx_done_list);
#define push_to_tx_done_queue(x) do { \
    list_add_tail(&to_tx_mheader(x)->link, &tx_done_list); \
} while (0)

static LIST_HEAD(rx_done_list);
#define push_to_rx_done_queue(x) do { \
    list_add_tail(&to_rx_mheader(x)->link, &rx_done_list); \
} while (0)
static LIST_HEAD(rx_awaiting_list);
#define push_to_awaiting_queue(x) do { \
    list_add_tail(&to_rx_mheader(x)->link, &rx_awaiting_list); \
} while (0)

// always set by ISR
static info_t *current_rx_header = NULL;
static arch_partial_header_recv_info_t current_rx_header_partial_recv_info;

// always unset by ISR, always checked and set by US 
static volatile bool tx_free = true;
#if defined(CONFIG_MTCP_LOG)
static volatile bool logging = false;
#endif
static info_t *current_tx_header = NULL;

// ISR only
static rstate_t rstate = RS_HEADER;

#define tx_user_handler_done(ptr, sz) do { \
    if(ptr) { \
        tx_free = false; \
        arch_backend_send(ptr, sz); \
    } \
    return; \
} while(0)

#define tx_user_force_kick_if_free() do { \
    if(tx_free) { \
        match_and_install_ack_nak_header_us(); \
        if(current_tx_header) \
            tx_user_handler_done(current_tx_header, sizeof(*current_tx_header)); \
    } \
} while(0)

static void start_header_receive(void) {
    rx_mheader_t *h = get_a_new_rx_header();
    current_rx_header = &h->header;
    current_rx_header_partial_recv_info = arch_backend_start_header_recv(current_rx_header); 
    rstate = RS_HEADER;
}

// TODO : we can check if it is one of our pool, and is in accept state
//        this should not be too dificult!!!!
#define looks_suspicious(x) false


arch_define_isr_may_schedule_bh(mtcp_tx_handler) {
#if defined(CONFIG_MTCP_LOG)
    // logging is a special case, because we hold the cpu busy
    if(logging) {
        logging = false;
        tx_free = true;
        return false;
    }
#endif
    // everything will be taken care by BH
    // except data pending
    // tx_free will not be reset in that case
    tx_mheader_t *th = to_tx_mheader(current_tx_header);
    if(th->state == THS_DATA_SEND_NEXT) {
        arch_backend_send_isr(th->req->packet_ptr, th->req->packet_sz);
        if(!th->req->remaining)
            th->state = THS_CALLBACK_NEXT;
        else {
            th->req->packet_ptr += th->req->packet_sz;
            th->req->packet_sz = (th->req->remaining < arch_backend_tx_limit) ? th->req->remaining : arch_backend_tx_limit - 1;
            th->req->remaining -= th->req->packet_sz;
        }
        return arch_isr_schedule_bh_false;
    } else {
        push_to_tx_done_queue(current_tx_header);
        current_tx_header = NULL;
        tx_free = true;
        return arch_isr_schedule_bh_true;
    }
}

arch_define_isr_may_schedule_bh(mtcp_rx_handler) {
    if(rstate == RS_HEADER) {
        switch(arch_backend_finalize_partial_recv(current_rx_header, current_rx_header_partial_recv_info)) {
        case PARTIAL_CONTINUE:
            arch_backend_continue_header_recv(current_rx_header, current_rx_header_partial_recv_info);
            return arch_isr_schedule_bh_false;
        case FULL_INVALID:
            arch_BUG();
        case FULL_VALID:
            break;
        default:
            arch_BUG();
        }
        if(current_rx_header->type == WRITE_REQ) {
            // if its a write req, push it to BH
            // BH will schedule ACK / NAK
            push_to_awaiting_queue(current_rx_header);
            start_header_receive();
            return arch_isr_schedule_bh_true;
        } else if(current_rx_header->type == WRITE) {
            // if its a write, immediately start the recv 
            // we can free the received header,
            // we have all the info in the `skey` header 
            rx_mheader_t *h = (rx_mheader_t *)current_rx_header->skey;
            if(looks_suspicious(h))
                arch_BUG(); // without accept??
            else if(h->req->flexinfo.rig.sz != current_rx_header->sz)
                arch_BUG(); // sneaky? we agreed on something else
            rstate = RS_DATA;
            arch_backend_start_data_recv_isr(h->req->packet_ptr, h->req->packet_sz);
            rx_mheader_free(current_rx_header);
            current_rx_header = &h->header;
            return arch_isr_schedule_bh_false;
        } else {
            arch_BUG(); // we dont understand you 
        }
    } else if(rstate == RS_DATA) {
        rx_mheader_t *h = to_rx_mheader(current_rx_header);
        if(!h->req->remaining) {
            // data recv done, so start header recv
            // BH will take care of callback
            push_to_rx_done_queue(current_rx_header);
            start_header_receive();
            return arch_isr_schedule_bh_true;
        } else {
            h->req->packet_ptr += h->req->packet_sz;
            h->req->packet_sz = (h->req->remaining < arch_backend_rx_limit) ? h->req->remaining : arch_backend_rx_limit - 1;
            h->req->remaining -= h->req->packet_sz;
            arch_backend_start_data_recv_isr(h->req->packet_ptr, h->req->packet_sz);
            return arch_isr_schedule_bh_false;
        }
    } else {
        // not gonna happen
        arch_BUG();
    }
    // make gcc happy!!!
    return arch_isr_schedule_bh_false;
}


static rx_req_t *find_first_in_rx_pending_requests_list(uint32_t id) {
    rx_req_t *r;
    list_for_each_entry(r, &rx_pending_requests_list, link)
        if(r->id == id)
            return r;
    return NULL;
}
static void match_and_install_ack_nak_header_us(void) {
    rx_mheader_t *rh, *tmprh;
    rx_req_t *req;

    if(current_tx_header)
        arch_BUG();
    if(list_empty(&rx_usawaiting_list))
        return;


    list_for_each_entry_safe(rh, tmprh, &rx_usawaiting_list, link) {
        if(req = find_first_in_rx_pending_requests_list(rh->header.id)) {
            list_del(&rh->link);
            if((req->flex_type == RIGID && rh->header.sz == req->flexinfo.rig.sz) || (req->flex_type == FLEXIBLE && rh->header.sz <= req->flexinfo.flex.maxsz)) {
                list_del(&req->link);
                // rh becomes dangling, revived by WRITE
                // TODO: put it in a list, and if the user does never
                // come back, kill it
                // THINK ABOUT IT
                rh->req = req;
                if(req->flex_type == FLEXIBLE) {
                    req->flexinfo.flex.recvsz = rh->header.sz;
                    req->packet_sz = (req->flexinfo.flex.recvsz < arch_backend_rx_limit) ? req->flexinfo.flex.recvsz : arch_backend_rx_limit - 1;
                    req->remaining = (req->flexinfo.flex.recvsz < arch_backend_rx_limit) ? 0 : req->flexinfo.flex.recvsz - req->packet_sz;
                }
                *install_new_tx_header(NULL, THS_FREE_NEXT) = (info_t){rh->header.id, WRITE_RESP_ACK, 0, (uint32_t)rh};
                break;
            } else {
                *install_new_tx_header(NULL, THS_FREE_NEXT) = (info_t){rh->header.id, WRITE_RESP_NAK, req->flex_type == RIGID ? req->flexinfo.rig.sz : req->flexinfo.flex.maxsz, 0};
                rx_mheader_free_with_lock(&rh->header);
                break;
            }
        }
    }
}

static void mtcp_process(void) {
    tx_mheader_t *th, *tmpth;
    rx_mheader_t *rh, *tmprh;
    uint32_t istate;

    // critical context loop switching
    // because tx_done_list
    istate = arch_disable_interrupts();
    list_for_each_entry_safe(th, tmpth, &tx_done_list, link) {
        if(th->state == THS_FREE_NEXT) {
            list_del(&th->link);
            arch_restore_interrupts(istate);
            tx_mheader_free(&th->header);
            istate = arch_disable_interrupts();
        }
    }

    // while we are at it, get the rx awaiting list into more relaxed rx_usawaiting_list
    if(!list_empty(&rx_awaiting_list)) {
        rx_mheader_t *e = list_first_entry(&rx_awaiting_list, rx_mheader_t, link);
        list_del(&e->link);
        list_add_tail(&e->link, &rx_usawaiting_list);
    }
    arch_restore_interrupts(istate);

    tx_user_force_kick_if_free();
    /* if(tx_free) { */
    /*     match_and_install_ack_nak_header_us(); */
    /*     if(current_tx_header) */
    /*         tx_user_handler_done(current_tx_header, sizeof(*current_tx_header)); */
    /* } */

    // critical context loop switching
    // because tx_done_list
    istate = arch_disable_interrupts();
    list_for_each_entry_safe(th, tmpth, &tx_done_list, link) {
        if(th->state == THS_CALLBACK_NEXT) {
            list_del(&th->link);
            arch_restore_interrupts(istate);
            th->req->callback(th->req->cbarg);
            tx_req_free(th->req);
            tx_mheader_free(&th->header);
            istate = arch_disable_interrupts();
        }
    }
    arch_restore_interrupts(istate);

    // critical context loop switching
    // because rx_done_list
    istate = arch_disable_interrupts();
    list_for_each_entry_safe(rh, tmprh, &rx_done_list, link) {
        list_del(&rh->link);
        arch_restore_interrupts(istate);
        if(rh->req->flex_type == FLEXIBLE && rh->req->flexinfo.flex.retsz)
            *rh->req->flexinfo.flex.retsz = rh->req->flexinfo.flex.recvsz;
        rh->req->callback(rh->req->cbarg);
        rx_req_free(rh->req);
        rx_mheader_free_with_lock(&rh->header);
        istate = arch_disable_interrupts();
    }
    arch_restore_interrupts(istate);


    if(tx_free) {
        list_for_each_entry_safe(th, tmpth, &tx_pending_list, link) {
            if(th->state != THS_HEADER_SEND_NEXT)
                arch_BUG();
            list_del(&th->link);
            install_new_tx_header(th, THS_DATA_SEND_NEXT);
            tx_user_handler_done(&th->header, sizeof(th->header));
        }
    }
}

void mtcp_queue_send(uint32_t id, uint8_t *data, uint32_t sz, callback_t cb, void *arg) {
    tx_mheader_t *th;
    tx_req_t *req = get_a_new_tx_request();
    {
        req->id = id;
        req->packet_ptr = data;
        req->packet_sz = (sz < arch_backend_tx_limit) ? sz : arch_backend_tx_limit - 1;
        req->remaining = (sz < arch_backend_tx_limit) ? 0 : sz - req->packet_sz;
        req->callback = cb;
        req->cbarg = arg;
    }

    th = get_a_new_tx_header();
    {
        th->header = (info_t){id, READ, sz, 0};
        th->state = THS_HEADER_SEND_NEXT;
        th->req = req;
        if(tx_free) {
            install_new_tx_header(th, THS_DATA_SEND_NEXT);
            tx_user_handler_done(&th->header, sizeof(th->header));
        } else
            push_to_pending_tx_queue(&th->header);
    }
}

static void mtcp_queue_recv_common_tail(rx_req_t *req) {
    push_to_pending_rx_requests_queue(req);

    uint32_t istate = arch_disable_interrupts();
    {
        if(!list_empty(&rx_awaiting_list)) {
            rx_mheader_t *e = list_first_entry(&rx_awaiting_list, rx_mheader_t, link);
            list_del(&e->link);
            list_add_tail(&e->link, &rx_usawaiting_list);
        }
    }
    arch_restore_interrupts(istate);

    tx_user_force_kick_if_free();
}

void mtcp_queue_recv(uint32_t id, uint8_t *data, uint32_t sz, callback_t cb, void *arg) {
    rx_req_t *req = get_a_new_rx_request();
    {
        req->id = id;
        req->packet_ptr = data;
        req->flex_type = RIGID;
        req->flexinfo.rig.sz = sz;
        req->packet_sz = (sz < arch_backend_rx_limit) ? sz : arch_backend_rx_limit - 1;
        req->remaining = (sz < arch_backend_rx_limit) ? 0 : sz - req->packet_sz;
        req->callback = cb;
        req->cbarg = arg;
    }
    mtcp_queue_recv_common_tail(req);
}

void mtcp_queue_recv_flex(uint32_t id, uint8_t *data, uint32_t maxsz, uint32_t *recvsz, callback_t cb, void *arg) {
    rx_req_t *req = get_a_new_rx_request();
    {
        req->id = id;
        req->packet_ptr = data;
        req->flex_type = FLEXIBLE;
        req->flexinfo.flex.maxsz = maxsz;
        req->flexinfo.flex.retsz = recvsz;
        req->callback = cb;
        req->cbarg = arg;
    }
    mtcp_queue_recv_common_tail(req);
}

#if defined(CONFIG_MTCP_LOG)
typedef struct {
    info_t header;
    char data[512];
} __attribute__((packed)) log_sendbuf_t;

void mtcp_log(const char *fmt, ...) {
    log_sendbuf_t sendbuf = {{0, 0, 0, 0}};

    va_list va;
    va_start(va, fmt);
    int l = arch_vsnprintf(sendbuf.data, sizeof(sendbuf.data), fmt, va);
    va_end(va);

    *((volatile uint32_t *)&sendbuf.header.type) = LOG;
    *((volatile uint32_t *)&sendbuf.header.sz) = l + 1;

    while(!tx_free);
    logging = true;
    tx_free = false;
    arch_backend_send(&sendbuf.header, sizeof(sendbuf.header) + l + 1);
    while(!tx_free);
}
#endif

arch_define_bh(mtcp_user_rx_handler) {
    mtcp_process();
}
arch_define_bh(mtcp_user_tx_handler) {
    mtcp_process();
}

#if defined(CONFIG_MTCP_ENABLE_SYNC)
uint32_t sequence = 0;
#define next_send_seq() ({ \
    sequence++; \
})
uint32_t recv_send_seq;
bool need_ack_send = false;
bool ack_send_done = false;
volatile bool synchronised = false;
volatile uint32_t rpos, spos;
uint8_t dev_bcast_send[16] __attribute__((aligned(4))) = "DEVBCASTSEND\0\0\0";
uint8_t dev_bcast_ack[16] __attribute__((aligned(4))) =  "DEVBCASTACKD\0\0\0";
uint8_t dev_bcast_recv[16] __attribute__((aligned(4))) = "DEVBCASTRECV\0\0\0";
uint8_t rdata[16];
#define SEQPOSITION (12)

arch_define_isr_may_schedule_bh(sync_tx_isr) {
    if(ack_send_done) {
        synchronised = true;
        return false;
    } else if(need_ack_send) {
        *((uint32_t *)&dev_bcast_ack[SEQPOSITION]) = recv_send_seq;
        arch_backend_send((uint8_t *)dev_bcast_ack, sizeof(dev_bcast_ack));
        ack_send_done = true;
        return false;
    }
    *((uint32_t *)&dev_bcast_send[SEQPOSITION]) = next_send_seq();
    arch_backend_send((uint8_t *)dev_bcast_send, sizeof(dev_bcast_send));
    return false;
}

arch_define_isr_may_schedule_bh(sync_rx_isr) {
    if(spos >= SEQPOSITION) {
        spos++;
        rpos++;
        if(spos == sizeof(dev_bcast_recv)) {
            recv_send_seq = *((uint32_t *)&rdata[SEQPOSITION]);
            need_ack_send = true;
        } else
            arch_backend_start_data_recv(&rdata[rpos], 1);
        return false;
    } else {
        if(rdata[rpos] == dev_bcast_recv[spos]) {
            rpos++;
            spos++;
        } else {
            rpos = 0;
            spos = 0;
        }
        arch_backend_start_data_recv(&rdata[rpos], 1);
        return false;
    }
}

static void start_broadcast_send(void) {
    *((uint32_t *)&dev_bcast_send[SEQPOSITION]) = next_send_seq();
    arch_backend_send((uint8_t *)dev_bcast_send, sizeof(dev_bcast_send));
}

static void start_devbcastrecv_search(void) {
    rpos = 0; spos = 0;
    arch_backend_start_data_recv(&rdata[rpos], 1);
}

static void synchronize_with_lib(void) {
    arch_register_isr(arch_backend_tx_intr, sync_tx_isr);
    arch_register_isr(arch_backend_rx_intr, sync_rx_isr);
    start_devbcastrecv_search();
    start_broadcast_send();
    while(synchronised == false);
    arch_deregister_isr(arch_backend_tx_intr);
    arch_deregister_isr(arch_backend_rx_intr);
}
#else
#define synchronize_with_lib()
#endif

static void mtcp_start(void) {
    synchronize_with_lib();

    arch_register_bh(arch_backend_tx_intr, mtcp_user_tx_handler);
    arch_register_bh(arch_backend_rx_intr, mtcp_user_rx_handler);
    arch_register_isr(arch_backend_tx_intr, mtcp_tx_handler);
    arch_register_isr(arch_backend_rx_intr, mtcp_rx_handler);

    // We dont need locking here!!! this is in startup, nothing started yet
    start_header_receive();
}

arch_initcall(mtcp) {
    rx_mheader_t *rh;
    tx_mheader_t *th;
    rx_req_t *rq;
    tx_req_t *tq;

    for(rh = &rx_headers[0]; rh < &rx_headers[CONFIG_MTCP_NUM_RX_HEADERS]; rh++) {
        list_add_tail(&rh->link, &rx_header_free_list);
    }
    for(th = &tx_headers[0]; th < &tx_headers[CONFIG_MTCP_NUM_TX_HEADERS]; th++) {
        list_add_tail(&th->link, &tx_header_free_list);
    }
    for(rq = &rx_requests[0]; rq < &rx_requests[CONFIG_MTCP_NUM_RX_REQUESTS]; rq++) {
        list_add_tail(&rq->link, &rx_requests_free_list);
    }
    for(tq = &tx_requests[0]; tq < &tx_requests[CONFIG_MTCP_NUM_TX_REQUESTS]; tq++) {
        list_add_tail(&tq->link, &tx_requests_free_list);
    }

    arch_backend_init();

    mtcp_start();
}

