#ifndef __MOCKTCP_DEV_H__
#define __MOCKTCP_DEV_H__

typedef enum {
    FULL_VALID,
    FULL_INVALID,
    PARTIAL_CONTINUE
} partial_recv_status_t;

typedef void (*callback_t)(void *arg);

void mtcp_queue_send(uint32_t id, uint8_t *data, uint32_t sz, callback_t cb, void *arg);
void mtcp_queue_recv(uint32_t id, uint8_t *data, uint32_t sz, callback_t cb, void *arg);

#if defined(CONFIG_MTCP_LOG)
void mtcp_log(const char *fmt, ...);
#endif

#endif
