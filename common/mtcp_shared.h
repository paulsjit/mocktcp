#ifndef __MOCKTCP_SHARED_H__
#define __MOCKTCP_SHARED_H__
#include <stdint.h>

typedef enum {
    READ,
    WRITE,
    WRITE_REQ,
    WRITE_RESP_ACK,
    WRITE_RESP_NAK,
    LOG,
} info_type_t;

typedef struct {
    uint32_t id;
    uint32_t type;
    uint32_t sz;
    uint32_t skey;
} __attribute__((packed)) info_t; 


#endif

