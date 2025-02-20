#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <semaphore.h>
#include <time.h>
#include <sys/ioctl.h>
#include <ctype.h>
#include "linux_klist.h"
#include "mtcp_shared.h"
#include "mocktcp_lib.h"

#define enum_string(v, type, prefix) (*(v) >= (sizeof(type ## _strings) / sizeof(const char *))) ? #prefix "UNKNOWN" : type ## _strings[*(v)]

typedef enum {
    UBLOCK_T_SEM,
    UBLOCK_T_EFD,
} ublock_t;
const char *ublock_t_strings[] = {
    "UBLOCK_T_SEM",
    "UBLOCK_T_EFD",
};
#define ublock_t_fmtstr "%s"
#define ublock_t_values(v) enum_string(v, ublock_t, UBLOCK_T_)

const char *req_type_t_strings[] = {
    "REQ_TYPE_READ",
    "REQ_TYPE_WRITE",
};
#define req_type_t_fmtstr "%s"
#define req_type_t_values(v) enum_string(v, req_type_t, REQ_TYPE)

const char *info_type_t_strings[] = {
    "READ",
    "WRITE",
    "WRITE_REQ",
    "WRITE_RESP_ACK",
    "WRITE_RESP_NAK",
    "LOG",
};
#define info_type_t_fmtstr "%s"
#define info_type_t_values(v) enum_string(v, info_type_t, )

#define info_t_fmtstr "info_t@%p{id=%d, type=" info_type_t_fmtstr ", sz=%d, skey=0x%08x}"
#define info_t_values(v) (v), (v)->id, info_type_t_values(&(v)->type), (v)->sz, (v)->skey

struct __Req {
    req_type_t type;
    uint32_t id;
    uint8_t *buffer;
    uint32_t sz;
    bool error;
    uint32_t error_info;
    sem_t sem;
    int eid;
    ublock_t ublock_type;
    struct timespec ts;
    struct list_head link;
};
#define req_t_fmtstr "req_t@%p{type=" req_type_t_fmtstr ", id=%d, buffer=@%p, sz=%d, error=%s, error_info=%d, sem=@%p, eid=%d, bltype=" ublock_t_fmtstr "}"
#define req_t_values(v) (v), \
                        req_type_t_values(&(v)->type), \
                        (v)->id, \
                        (v)->buffer, \
                        (v)->sz, \
                        (v)->error ? "true" : "false", \
                        (v)->error_info, \
                       &(v)->sem, \
                        (v)->eid, \
                        ublock_t_values(&(v)->ublock_type)

typedef struct {
    info_t header;
    uint8_t *buffer;
    uint32_t sz;
    struct timespec ts;
    struct list_head link;
} read_ready_t;
#define read_ready_t_fmtstr "read_ready_t@%p{header=" info_t_fmtstr ", buffer=@%p, sz=%d}"
#define read_ready_t_values(v) (v), info_t_values(&(v)->header), (v)->buffer, (v)->sz

typedef struct {
    info_t header;
    req_t *req;
    struct list_head link;
    struct timespec ts;
} write_pending_t;
#define write_pending_t_fmtstr "write_pending_t@%p{header=" info_t_fmtstr ", req=@%p}"
#define write_pending_t_values(v) (v), info_t_values(&(v)->header), (v)->req

typedef enum {
    RS_CONTREAD,
    RS_HEADER,
    RS_DATA,
} rstate_t;
const char *rstate_t_strings[] = {
    "RS_CONTREAD",
    "RS_HEADER",
    "RS_DATA",
};
#define rstate_t_fmtstr "%s"
#define rstate_t_values(v) enum_string(v, rstate_t, RS_)

typedef enum {
    WS_CONTWRITE,
    WS_IDLE,
} wstate_t;
const char *wstate_t_strings[] = {
    "WS_CONTWRITE",
    "WS_IDLE",
};
#define wstate_t_fmtstr "%s"
#define wstate_t_values(v) enum_string(v, wstate_t, WS_)

typedef rstate_t (*contread_callback_t)(void *);
typedef struct {
    uint8_t *data;
    uint32_t remaining;
    uint32_t offset;
    contread_callback_t execute;
    void *arg;
} contread_t;
#define contread_t_fmtstr "contread_t@%p{data=@%p, remaining=%d, offset=%d, execute=func@%p, arg=@%p}"
#define contread_t_values(v) (v), (v)->data, (v)->remaining, (v)->offset, (v)->execute, (v)->arg


typedef wstate_t (*contwrite_callback_t)(void *);
typedef struct {
    uint8_t *data;
    uint32_t remaining;
    uint32_t offset;
    contwrite_callback_t execute;
    void *arg;
} contwrite_t;
#define contwrite_t_fmtstr "contwrite_t@%p{data=@%p, remaining=%d, offset=%d, execute=func@%p, arg=@%p}"
#define contwrite_t_values(v) (v), (v)->data, (v)->remaining, (v)->offset, (v)->execute, (v)->arg


#define NOW() ({ \
    struct timespec __now; \
    clock_gettime(CLOCK_MONOTONIC, &__now); \
    __now; \
})
static long time_getdiff_nanos(struct timespec *tp)
{
    struct timespec now = NOW();
    return (now.tv_sec - tp->tv_sec) * (long)(1000000000) +
        now.tv_nsec - tp->tv_nsec;
}

static double time_getdiff_secs(struct timespec *tp)
{
    return time_getdiff_nanos(tp) / (double)(1000000000);
}

static LIST_HEAD(awaited_writes);
static LIST_HEAD(accepted_writes);
static LIST_HEAD(pending_reqs);
static LIST_HEAD(pending_reads);
static LIST_HEAD(pending_writes);
static LIST_HEAD(user_requests_list);
static pthread_t uthread;
static pthread_mutex_t ulock = PTHREAD_MUTEX_INITIALIZER;
static int efd;
static int tfd;
static int rfd; // we read from here
static int wfd; // we write to here
static rstate_t rstate = RS_HEADER;
static wstate_t wstate = WS_IDLE;
static contread_t contread;
static contwrite_t contwrite;
static info_t rheader;
static uint8_t *data = NULL;
static uint32_t datasz = 0;
static void *dataarg = NULL;
static io_log_cb_t io_logger_cb = NULL;
static bool expect_sync = true;

static rstate_t read_header_execute(void *arg);
static rstate_t read_data_execute(void *arg);
static rstate_t read_into_contread(void);
static wstate_t write_header_execute(void *arg);
static wstate_t write_data_execute(void *arg);
static wstate_t write_dataheader_execute(void *arg);
static wstate_t write_from_contwrite(void);

req_t *find_first_in_pending_reqs(uint32_t id) {
    req_t *r;
    list_for_each_entry(r, &pending_reqs, link)
        if(r->id == id)
            return r;
    return NULL;
}

read_ready_t *find_first_in_pending_reads(uint32_t id) {
    read_ready_t *r;
    list_for_each_entry(r, &pending_reads, link)
        if(r->header.id == id)
            return r;
    return NULL;
}

req_t *find_first_in_pending_writes(uint32_t id) {
    req_t *r;
    list_for_each_entry(r, &pending_writes, link)
        if(r->id == id)
            return r;
    return NULL;
}
write_pending_t *find_first_in_awaited_writes(uint32_t id) {
    write_pending_t *r;
    list_for_each_entry(r, &awaited_writes, link)
        if(r->req->id == id)
            return r;
    return NULL;
}

#if defined(MTCP_DEBUG)
FILE *debug_file = NULL;
#define debug_mtcp(fmt, ...) do { \
    fprintf(debug_file, "[%8d] " fmt, __LINE__, ##__VA_ARGS__); \
    fflush(debug_file); \
} while(0)
#define debug_mtcp_nonl(fmt, ...) do { \
    fprintf(debug_file, fmt, ##__VA_ARGS__); \
    fflush(debug_file); \
} while(0)
#else
#define debug_mtcp(...)
#define debug_mtcp_nonl(...)
#endif

#define Sheader_debug(x)  debug_mtcp("==> " info_t_fmtstr "\n", info_t_values(x))
#define Rheader_debug(x) do { if((x)->type != LOG) debug_mtcp("<== " info_t_fmtstr "\n", info_t_values(x)); } while(0)
#define cond_debug_mtcp(cond, ...) do { \
    if((cond)) \
        debug_mtcp(__VA_ARGS__); \
} while(0)
#define typed_debug_mtcp(__type, v, fmt, ...) debug_mtcp(fmt " = " __type ## _fmtstr "\n", ##__VA_ARGS__, __type ## _values(v))
#define timecheck_mtcp(__type, v, thresh, lst_name, ...) do { \
    double __alive = time_getdiff_secs(&(v)->ts); \
    if(__alive > (thresh)) { \
        fprintf(stderr, "waiting in " lst_name " list for %lf secs : " __type ## _fmtstr "\n", __alive, __type ## _values(v)); \
    } \
} while(0)
#define state_debug_mtcp(text) do { \
    debug_mtcp("STATE@" text ":\n"); \
    debug_mtcp("\t\trstate = " rstate_t_fmtstr ", wstate = " wstate_t_fmtstr "\n", rstate_t_values(&rstate), wstate_t_values(&wstate)); \
    debug_mtcp("\t\tcontread = " contread_t_fmtstr "\n", contread_t_values(&contread)); \
    debug_mtcp("\t\tcontwrite = " contwrite_t_fmtstr "\n", contwrite_t_values(&contwrite)); \
    debug_mtcp("\t\trheader = " info_t_fmtstr "\n", info_t_values(&rheader)); \
    debug_mtcp("\t\tdata=@%p, datasz=%d, dataarg=@%p\n", data, datasz, dataarg); \
    debug_mtcp("\t\tLISTS: awaited=%s, accepted=%s, pending_reQs=%s, pending_reADs=%s, pending_writes=%s, user_reqs=%s\n", \
            list_empty(&awaited_writes) ? "empty" : "full", \
            list_empty(&accepted_writes) ? "empty" : "full", \
            list_empty(&pending_reqs) ? "empty" : "full", \
            list_empty(&pending_reads) ? "empty" : "full", \
            list_empty(&pending_writes) ? "empty" : "full", \
            list_empty(&user_requests_list) ? "empty" : "full"); \
} while(0)

static void hexdump(uint8_t *ptr, uint8_t *mark, int lcount, int tot, const char *lead) {
    for(int i = 0; i < tot; i+=lcount) {
        fprintf(stderr, "%s[%8d] ", lead, i);
        for(int j = i; j < i + lcount; j++)
            if(j < tot)
                fprintf(stderr, "%02x%s ", ptr[j], (mark && (&ptr[j] == mark)) ? "*" : " ");
            else
                fprintf(stderr, "    ");
        fprintf(stderr, "\t");
        for(int j = i; j < (i + lcount); j++) {
            if(j >= tot)
                break;
            fprintf(stderr, "%c", isprint(ptr[j]) ? ptr[j] : '.');
        }
        fprintf(stderr, "\n");
    }
}

#if defined(CONFIG_MTCP_ENABLE_SYNC)
#define LIMIT (1024 * 1024)
uint8_t origin[LIMIT];

void flush_by_reading(int fd) {
    int bytes_available = 0;
    int ionread_ret = ioctl(fd, FIONREAD, &bytes_available);
    if(ionread_ret < 0 || bytes_available > sizeof(origin)) {
        fprintf(stderr, "flush_by_reading error: ionread_ret = %d, bytes_available = %d / %d\n", ionread_ret, bytes_available, sizeof(origin));
        exit(1);
    }
    int l = read(rfd, origin, bytes_available);
    debug_mtcp("flushed by reading %d bytes, reported available %d bytes\n", l, bytes_available);
}

void wait_for_sync() {
    uint8_t *next_byte = &origin[0];
    uint8_t *sendsearch = "DEVBCASTSEND", *acksearch = "DEVBCASTACKD";
    uint8_t *searchpos = &sendsearch[0], *searchinitpos = &sendsearch[0];
    int iter = 0;
    int tot_recvd = 0;
    bool send_recv = false;
    bool byte_searching = true;
    int woffset, remaining, wremaining;
    uint8_t *seq_pt;
    uint8_t send_recv_buffer[16] = "DEVBCASTRECV\0\0\0";
#define SEQPOSITION (12) 

    // We could have done a tcflush, but we are not necessarily a serial 
    flush_by_reading(rfd);

    debug_mtcp("waiting for synchronization, looking for \"%s\" (terminated by 4-byte sequence)\n", sendsearch);
    while(true) {
        fd_set rfds;
        fd_set wfds;
        int retval;
        int maxfd = 0;

#define RSET_AND_MAXFD(v) ({ \
    FD_SET((v), &rfds); \
    (v) > maxfd ? (v) : maxfd;  \
})
#define WSET_AND_MAXFD(v) ({ \
    FD_SET((v), &wfds); \
    (v) > maxfd ? (v) : maxfd;  \
})

        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        maxfd = RSET_AND_MAXFD(rfd);
        maxfd = RSET_AND_MAXFD(tfd);
        if(send_recv)
            maxfd = WSET_AND_MAXFD(wfd);
#undef RSET_AND_MAXFD
#undef WSET_AND_MAXFD
#define rounded_next_byte(b) (((b + 1) < &origin[LIMIT]) ? (b + 1) : &origin[0] + ((b + 1) - &origin[LIMIT]))
#define rounded_prev_byte(b) (((b - 1) >= &origin[0]) ? (b - 1) : &origin[LIMIT] - (&origin[0] - b))
#define rounded_uint32_ending_at(b) ({ \
    uint8_t mem[4]; \
    uint8_t *ptr; \
    int count; \
    for(ptr = b, count = 3; count >= 0; count--, ptr=rounded_prev_byte(ptr)) \
        mem[count] = *ptr; \
    *((uint32_t *)mem); \
})

        retval = select(maxfd + 1, &rfds, &wfds, NULL, NULL);
        if(FD_ISSET(tfd, &rfds)) {
            uint64_t eval;
            read(tfd, &eval, sizeof(eval));
            fprintf(stderr, "waiting in synchronization, debugging info:\n");
            fprintf(stderr, "\t&origin[0] = %p, sendsearch = %p, acksearch = %p\n", &origin[0], sendsearch, acksearch);
            fprintf(stderr, "\tnext_byte = %p, nextbyte @offset = %d, tot_recvd = %d, iter = %d\n", next_byte, next_byte - &origin[0], tot_recvd, iter);
            fprintf(stderr, "\tseq_pt = %p, seq_pt @offset = %d\n", seq_pt, seq_pt - &origin[0]);
            fprintf(stderr, "\tsearchpos = %p, searchinitpos = %p, searchinitpos is %s\n", searchpos, searchinitpos, (searchinitpos == sendsearch) ? "sendsearch" : ((searchinitpos == acksearch) ? "acksearch" : "UNKNOWN"));
            fprintf(stderr, "\tsend_recv = %s, byte_searching = %s\n", send_recv ? "true" : "false", byte_searching ? "true" : "false");
            fprintf(stderr, "\twoffset = %d, wremaining = %d, remaining = %d\n", woffset, wremaining, remaining);
            fprintf(stderr, "\torigin\n\t======\n");
            hexdump(origin, rounded_prev_byte(next_byte), 16, ((tot_recvd > LIMIT) ? LIMIT : tot_recvd), "\t");
            fprintf(stderr, "\tsend_recv_buffer\n\t================\n");
            hexdump(send_recv_buffer, NULL, 16, 16, "\t");
        } else if (send_recv && retval > 0 && FD_ISSET(wfd, &wfds)) {
            int l = write(wfd, &send_recv_buffer[woffset], wremaining);
            woffset += l;
            wremaining -= l;
            if(!wremaining) {
                send_recv = false;
            }
        } else if (retval > 0 && FD_ISSET(rfd, &rfds)) {
            int bytes_available = 0;
            int ionread_ret = ioctl(rfd, FIONREAD, &bytes_available);
            read(rfd, next_byte, 1);
            debug_mtcp("recieved %8d@%p : %02x (%3u) %c (IONREAD: ret = %d, available = %d bytes)\n", iter++, next_byte, *next_byte, *next_byte, isprint(*next_byte) ? *next_byte : '.', ionread_ret, bytes_available);
            if(byte_searching) {
                if(*next_byte == *searchpos) {
                    searchpos++;
                    if(!*searchpos) {
                        debug_mtcp("exit byte searching mode\n");
                        byte_searching = false;
                        remaining = 4;
                    }
                } else {
                    searchpos = searchinitpos;
                }
            } else if(remaining) {
                remaining--;
                if(!remaining) {
                    uint32_t seq = rounded_uint32_ending_at(next_byte);
                    debug_mtcp("sequence %08x\n", seq);
                    debug_mtcp("waiting for acknowledgement, looking for \"%s\" (terminated by 4-byte sequence)\n", acksearch);
                    if(searchinitpos == &sendsearch[0]) {
                        seq_pt = next_byte;
                        byte_searching = true;
                        searchinitpos = &acksearch[0];
                        searchpos = searchinitpos;
                        send_recv = true;
                        *((uint32_t *)&send_recv_buffer[SEQPOSITION]) = 0xdead0000 | seq;
                        woffset = 0;
                        wremaining = 16;
                    }
                    else {
                        debug_mtcp("synchronized, send sequence %08x, recv sequence %08x\n", seq, rounded_uint32_ending_at(next_byte));
                        break;
                    }
                }
            }
            next_byte = rounded_next_byte(next_byte);
            tot_recvd++;
        }
    }

    // lets see if this is actually needed 
    //sleep(1);
}
#else
#define wait_for_sync()
#endif

static void signal_waiter(req_t *r) {
    if(r->ublock_type == UBLOCK_T_SEM)
        sem_post(&r->sem);
    else {
        uint64_t notify = 1;
        write(r->eid, &notify, sizeof(notify));
    }
}

static void *io_handler_thread(void *arg) {
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    wait_for_sync();

    state_debug_mtcp("begin");
    /* getchar(); */

    while(true) {
        uint64_t eval;
        int maxfd = 0, retval;
        fd_set rfds;
        fd_set wfds;


        state_debug_mtcp("loop");
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
#define RSET_AND_MAXFD(v) ({ \
    FD_SET((v), &rfds); \
    (v) > maxfd ? (v) : maxfd;  \
})
#define WSET_AND_MAXFD(v) ({ \
    FD_SET((v), &wfds); \
    (v) > maxfd ? (v) : maxfd;  \
})
        maxfd = RSET_AND_MAXFD(efd);
        maxfd = RSET_AND_MAXFD(rfd);
        maxfd = RSET_AND_MAXFD(tfd);
        if(!list_empty(&pending_writes) || !list_empty(&accepted_writes) || wstate == WS_CONTWRITE)
            maxfd = WSET_AND_MAXFD(wfd);
        cond_debug_mtcp(FD_ISSET(wfd, &wfds), "wfd added to select pool\n");
#undef RSET_AND_MAXFD
#undef WSET_AND_MAXFD
        retval = select(maxfd + 1, &rfds, &wfds, NULL, NULL);
        if (retval > 0 && FD_ISSET(efd, &rfds)) {
            debug_mtcp("efd has data\n");
            read_ready_t *rdata = NULL;
            req_t *r = NULL;
            read(efd, &eval, sizeof(eval));
            pthread_mutex_lock(&ulock);
            if(!list_empty(&user_requests_list)) {
                r = list_first_entry(&user_requests_list, req_t, link);
                list_del(&r->link);
            }
            pthread_mutex_unlock(&ulock);
            cond_debug_mtcp(!r, "new event but @user_requests_list empty\n");
            if(!r)
                continue;
            typed_debug_mtcp(req_t, r, "new request");
            if(r->type == REQ_TYPE_READ) {
                if(rdata = find_first_in_pending_reads(r->id)) {
                    list_del(&rdata->link);
                    r->buffer = rdata->buffer;
                    r->sz = rdata->sz;
                    free(rdata);
                    signal_waiter(r);
                    typed_debug_mtcp(read_ready_t, rdata, "found response for read, signalled");
                } else {
                    r->ts = NOW();
                    list_add_tail(&r->link, &pending_reqs);
                }
                cond_debug_mtcp(!rdata, "read request added to pending list\n");
            } else {
                write_pending_t *pendr = malloc(sizeof(*pendr));
                pendr->req = r;
                pendr->header = (info_t){r->id, WRITE_REQ, r->sz, 0};
                list_add_tail(&pendr->link, &pending_writes);
                typed_debug_mtcp(write_pending_t, pendr, "write request added to pending list");
            }
        } else if(retval > 0 && FD_ISSET(rfd, &rfds)) {
            debug_mtcp("rfd has data\n");
            switch(rstate) {
            case RS_CONTREAD:
                typed_debug_mtcp(contread_t, &contread, "reading into last contread");
                rstate = read_into_contread();
                break;
            case RS_HEADER:
                contread = (contread_t){(uint8_t *)&rheader, sizeof(rheader), 0, read_header_execute, NULL};
                typed_debug_mtcp(contread_t, &contread, "reading into new header contread");
                rstate = read_into_contread();
                break;
            case RS_DATA:
                contread = (contread_t){data, datasz, 0, read_data_execute, dataarg};
                typed_debug_mtcp(contread_t, &contread, "reading into new data contread");
                rstate = read_into_contread();
                break;
            }
            typed_debug_mtcp(rstate_t, &rstate, "new rstate");
        } else if(retval > 0 && FD_ISSET(wfd, &wfds)) {
            debug_mtcp("wfd wants data\n");
            write_pending_t *pendr = NULL;
            switch(wstate) {
            case WS_CONTWRITE:
                typed_debug_mtcp(contwrite_t, &contwrite, "writing from last contwrite");                
                wstate = write_from_contwrite();
                break;
            case WS_IDLE:
                if(!list_empty(&accepted_writes))
                    pendr = list_first_entry(&accepted_writes, write_pending_t, link);
                if(pendr) {
                    typed_debug_mtcp(write_pending_t, pendr, "got pending write (accepted)");
                    list_del(&pendr->link);
                    contwrite = (contwrite_t){(uint8_t *)&pendr->header, sizeof(pendr->header), 0, write_dataheader_execute, pendr};
                    typed_debug_mtcp(info_t, &pendr->header, "new header");                
                    typed_debug_mtcp(contwrite_t, &contwrite, "writing Aheader from new contwrite");                
                    wstate = write_from_contwrite();
                } else {
                    if(!list_empty(&pending_writes))                
                        pendr = list_first_entry(&pending_writes, write_pending_t, link);
                    if(pendr) {
                        typed_debug_mtcp(write_pending_t, pendr, "got pending write (to be requested)");
                        list_del(&pendr->link);
                        contwrite = (contwrite_t){(uint8_t *)&pendr->header, sizeof(pendr->header), 0, write_header_execute, pendr};
                        typed_debug_mtcp(info_t, &pendr->header, "new header");                
                        typed_debug_mtcp(contwrite_t, &contwrite, "writing Wheader from new contwrite");                
                        wstate = write_from_contwrite();
                    }
                }
                cond_debug_mtcp(!pendr, "wfd in pool but no pending writes\n");
                typed_debug_mtcp(wstate_t, &wstate, "new wstate");
                break;
            }
        } else if (retval > 0 && FD_ISSET(tfd, &rfds)) {
            debug_mtcp("timer has data\n");
            read(tfd, &eval, sizeof(eval));

            // sent a write req, no response
            write_pending_t *__wp;  list_for_each_entry(__wp, &awaited_writes,     link)  timecheck_mtcp(write_pending_t, __wp, 1, "awaited_write");  typed_debug_mtcp(write_pending_t, __wp, "awaited_write");
            // sent a write req, got response, but no data, SHOULD HAVE A HIGHER TOLERANCE
                                    list_for_each_entry(__wp, &accepted_writes,    link)  timecheck_mtcp(write_pending_t, __wp, 1, "accepted_write"); typed_debug_mtcp(write_pending_t, __wp, "accepted_write");
            // waiting for data, but data did not arrive, SHOULD HAVE A HIGHER TOLERANCE
            req_t           *__r;   list_for_each_entry(__r,  &pending_reqs,       link)  timecheck_mtcp(req_t,           __r,  1, "pending_req");    typed_debug_mtcp(req_t,           __r,  "pending_reqs");
            // data arrived, but user not collecting it, APPLICATION ISSUE
            read_ready_t    *__rr;  list_for_each_entry(__rr, &pending_reads,      link)  timecheck_mtcp(read_ready_t,    __rr, 1, "pending_read");   typed_debug_mtcp(read_ready_t,    __rr, "pending_reads");
                                    list_for_each_entry(__r,  &pending_writes,     link)                                                              typed_debug_mtcp(req_t,           __r,  "pending_writes");
                                    list_for_each_entry(__r,  &user_requests_list, link)                                                              typed_debug_mtcp(req_t,           __r,  "user_requests");
            continue;
        }
    }
    return NULL;
}
static wstate_t write_from_contwrite(void) {
    int l = write(wfd, &contwrite.data[contwrite.offset], contwrite.remaining);
    debug_mtcp("contwrite finished : l = %d, remaining = %d%s, ==>data@%d = \"", l, contwrite.remaining - l, (contwrite.remaining - l) ? "" : "^^^", contwrite.offset);
    for(int x = contwrite.offset, nolast = l - 1; x < contwrite.offset + l; x++, nolast--)
        debug_mtcp_nonl("%02x%s",contwrite.data[x], nolast ? " " : "");
    debug_mtcp_nonl("\"\n");
    contwrite.remaining -= l;
    contwrite.offset += l;
    if(!contwrite.remaining) return contwrite.execute(contwrite.arg);
    else return WS_CONTWRITE;
}
static wstate_t write_header_execute(void *arg) {
    write_pending_t *r = arg;
    Sheader_debug(&r->header);
    if(!r->req->sz)
        return write_data_execute(r);
    r->header = (info_t){r->req->id, WRITE, r->req->sz};
    r->ts = NOW();
    list_add_tail(&r->link, &awaited_writes);
    return WS_IDLE;
}
static wstate_t write_data_execute(void *arg) {
    write_pending_t *r = arg;
    signal_waiter(r->req);
    free(r);
    return WS_IDLE;
}
static wstate_t write_dataheader_execute(void *arg) {
    write_pending_t *r = arg;
    contwrite = (contwrite_t){r->req->buffer, r->req->sz, 0, write_data_execute, r};
    return WS_CONTWRITE;
}

static void signal_execute(info_t *h) {
    write_pending_t *pendr = find_first_in_awaited_writes(h->id);
    if(!pendr) {
        // TODO : investigate ERROR CASE!!!!
        return;
    }
    list_del(&pendr->link);
    if(h->type == WRITE_RESP_ACK) {
        pendr->header.skey = h->skey;
        pendr->ts = NOW();
        list_add_tail(&pendr->link, &accepted_writes);
    } else {
        pendr->req->error_info = h->sz;
        pendr->req->error = true;
        signal_waiter(pendr->req);
        free(pendr);
    }
}
static rstate_t read_header_execute(void *arg) {
    Rheader_debug(&rheader);
#if !defined(CONFIG_MTCP_ENABLE_SYNC)
    /* A little bit tricky first header recieve */
#if 0
    /* This is not working because the other channel may
     * be spewing bytes constantly, an din protocols like
     * uart it can make the bytes come all garbled because
     * of the loss of sync
     */
    if(expect_sync) {
        info_t pack2[2];
        memcpy(&pack2[0], &rheader, sizeof(rheader));
        memcpy(&pack2[1], &rheader, sizeof(rheader));
        if(memmem((char *)&pack2[0], 2 * sizeof(rheader), "DEVBCASTSEND", strlen("DEVBCASTSEND"))) {
            fprintf(stderr, "Recieved synchronisation broadcast, and did not expect that! ==> \"");
            for(int x = 0, nolast = sizeof(rheader) - 1; x < sizeof(rheader); x++, nolast--)
                fprintf(stderr, "%02x%s", ((char *)&rheader)[x], nolast ? " " : "");
            fprintf(stderr, "\"\n");
            exit(1);
        }
        expect_sync = false;
    }
#endif
#endif
    if(rheader.type == WRITE_RESP_ACK || rheader.type == WRITE_RESP_NAK) {
        signal_execute(&rheader);
        return RS_HEADER;
    }
    else {
        read_ready_t *r = malloc(sizeof(read_ready_t));
        r->header = rheader;
        r->buffer = data = malloc(rheader.sz);
        r->sz = datasz = rheader.sz;
        dataarg = r;
        return RS_DATA;
    }
}

static rstate_t read_data_execute(void *arg) {
    req_t *r;
    read_ready_t *rdata = dataarg;
    if(rdata->header.type == LOG) {
        if(io_logger_cb)
            io_logger_cb(rdata->buffer, rdata->sz);
        free(rdata);
        free(rdata->buffer);
    }
    else if(r = find_first_in_pending_reqs(rdata->header.id)) {
        list_del(&r->link);
        r->buffer = rdata->buffer;
        r->sz = rdata->sz;
        free(rdata);
        signal_waiter(r);
    } else {
        rdata->ts = NOW();
        list_add_tail(&rdata->link, &pending_reads);
    }
    data = NULL;
    datasz = 0;
    dataarg = NULL;
    return RS_HEADER;
}

static rstate_t read_into_contread(void) {
    int l = read(rfd, &contread.data[contread.offset], contread.remaining);
    debug_mtcp("contread finished : l = %d, remaining = %d%s, <==data@%d = \"", l, contread.remaining - l, (contread.remaining - l) ? "" : "^^^", contread.offset);
    for(int x = contread.offset, nolast = l - 1; x < contread.offset + l; x++, nolast--)
        debug_mtcp_nonl("%02x%s",contread.data[x], nolast ? " " : "");
    debug_mtcp_nonl("\"\n");
    contread.remaining -= l;
    contread.offset += l;
    if(!contread.remaining) return contread.execute(contread.arg);
    else return RS_CONTREAD;
}

#define NO_OTHER_MACRO_STARTS_WITH_THIS_NAME_1
#define NO_OTHER_MACRO_STARTS_WITH_THIS_NAME_
#define IS_EMPTY(name) defined(NO_OTHER_MACRO_STARTS_WITH_THIS_NAME_ ## name)
#define BECOMES_ONE(name) defined(NO_OTHER_MACRO_STARTS_WITH_THIS_NAME_ ## name)
#define EMPTY(name) IS_EMPTY(name)
#define STR(v) #v
#define FILENAME(v) STR(v)
#define  DEBUG_FILE_STDERR do { debug_file = stderr; } while(0)

#if defined(MTCP_DEBUG)
#if EMPTY(MTCP_DEBUG)
#define OPENDEBUGFILE() DEBUG_FILE_STDERR 
#elif BECOMES_ONE(MTCP_DEBUG)
#define OPENDEBUGFILE() DEBUG_FILE_STDERR
#else
#define OPENDEBUGFILE() do { \
    debug_file = fopen(FILENAME(MTCP_DEBUG), "w"); \
    if(!debug_file) { \
        fprintf(stderr, "debug file = " FILENAME(MTCP_DEBUG) " could not be opened, exiting\n"); \
        exit(1); \
    } \
} while(0)
#endif
#else
#define OPENDEBUGFILE()
#endif

void mtcp_work_start(int io_rfd, int io_wfd, io_log_cb_t io_logger) {
    OPENDEBUGFILE();
    struct itimerspec spec = {{1, 0}, {1, 0}};
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK); 
    timerfd_settime(tfd, 0, &spec, NULL);
    efd = eventfd(0, EFD_SEMAPHORE);
    rfd = io_rfd;
    wfd = io_wfd;
    io_logger_cb = io_logger;
    pthread_create(&uthread, NULL, io_handler_thread, NULL);
}
void mtcp_work_finish(void) {
    pthread_cancel(uthread);
    pthread_join(uthread, NULL);
}

req_t *mtcp_submit_request(uint32_t id, req_type_t type, uint8_t *b, uint32_t sz, bool blocking) {
    uint64_t notify = 1;

    req_t *r = malloc(sizeof(*r)); 
    if(blocking) {
        sem_init(&r->sem, 0, 0);
        r->ublock_type = UBLOCK_T_SEM;
    } else {
        r->eid = eventfd(0, EFD_SEMAPHORE);
        r->ublock_type = UBLOCK_T_EFD;
    }
    r->type = type;
    r->buffer = b;
    r->sz = sz;
    r->id = id;
    r->error = false;

    pthread_mutex_lock(&ulock);
    list_add(&r->link, &user_requests_list);
    pthread_mutex_unlock(&ulock);

    write(efd, &notify, sizeof(notify));
    return r;
}

void mtcp_free_request(req_t *r) {
    if(r->ublock_type == UBLOCK_T_EFD) {
        close(r->eid);
        if(r->type == REQ_TYPE_WRITE)
            free(r->buffer);
    }
    free(r);
}

bool mtcp_wait_for_write_completion(req_t *r) {
    if(r->type != REQ_TYPE_WRITE)
        return false;
    sem_wait(&r->sem);
    return r->error;
}
void mtcp_wait_for_read_completion(req_t *r, uint8_t **b, uint32_t *sz) {
    if(r->type != REQ_TYPE_READ) {
        fprintf(stderr, "%s : invalid request : r->type != REQ_TYPE_READ\n", __func__);
        return;
    }
    sem_wait(&r->sem);
    if(b)
        *b= r->buffer;
    if(sz)
        *sz = r->sz;
}

int mtcp_wait_for_any_completion(int *wids, int count, struct timeval *timeout) {
    fd_set rfds;
    int retval;
    int maxfd = 0;

    FD_ZERO(&rfds);
#define RSET_AND_MAXFD(v) ({ \
    FD_SET((v), &rfds); \
    (v) > maxfd ? (v) : maxfd;  \
})
    for(int i = 0; i < count; i++)
        maxfd = RSET_AND_MAXFD(wids[i]);
#undef RSET_AND_MAXFD
    retval = select(maxfd + 1, &rfds, NULL, NULL, timeout);
    if (retval > 0) {
        for(int i = 0; i < count; i++) {
            if(FD_ISSET(wids[i], &rfds)) {
                uint64_t eval;
                read(wids[i], &eval, sizeof(eval));
                return wids[i];
            }
        }
    }

    return -1;
}

uint32_t mtcp_req_get_error_info(req_t *r) {
    return (r->type == REQ_TYPE_WRITE && r->error) ? r->error_info : 0;
}

int mtcp_req_get_wait_id(req_t *r) {
    return (r->ublock_type == UBLOCK_T_EFD) ? r->eid : -1;
}
req_type_t mtcp_req_get_req_type(req_t *r) {
    return r->type;
}
uint8_t *mtcp_req_get_buffer(req_t *r) {
    return (r->type == REQ_TYPE_READ) ? r->buffer : NULL;
}
uint32_t mtcp_req_get_size(req_t *r) {
    return (r->type == REQ_TYPE_READ) ? r->sz : 0;
}
uint32_t mtcp_req_get_req_size(req_t *r) {
    return (r->type == REQ_TYPE_WRITE) ? r->sz : 0;
}
bool mtcp_req_has_error(req_t *r) {
    return (r->type == REQ_TYPE_WRITE) ? r->error : false;
}
int mtcp_req_get_id(req_t *r) {
    return r->id;
}




