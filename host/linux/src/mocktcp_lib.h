#ifndef __MOCKTCP_LIB_H__
#define __MOCKTCP_LIB_H__

#include <stdint.h>
#include <stdbool.h>
#include <semaphore.h>

typedef enum {
   REQ_TYPE_READ,
   REQ_TYPE_WRITE, 
} req_type_t;

struct __Req;
typedef struct __Req req_t;

/*
 * buffer : a character pointer (may or may not be NULL terminated)
 * len    : size of buffer
 *
 * buffer will not be available / valid when this call returns
 */
typedef void (*io_log_cb_t)(char *buffer, int len);

#ifdef __cplusplus
extern "C" {
#endif
/*
 * start mtcp I/O work in a separate thread
 * io_rfd       : must be an fd where read() can be called without blocking
 * io_wfd       : must be an fd where write() can be called without blocking
 * io_logger    : must be a function (or NULL) which can be called from a
 *                different thread
 */
void mtcp_work_start(int io_rfd, int io_wfd, io_log_cb_t io_logger);
/*
 * end mtcp I/O work and join the thread
 */
void mtcp_work_finish(void);

/* create an mtcp I/O request that will be processed asynchronously
 * id       : socket ID of peer node where this request must be sent
 * type     : READ or WRITE request
 * b        : buffer for I/O
 *            if READ,  this must be NULL, and it will be assigned a value after Txn
 *            if WRITE, this must be a valid pointer, and must be valid for the duration
 *                      of the Txn.
 *                      This must also be a buffer that can be used in `free()` inside
 *                      `mtcp_free_request()`.
 * sz       : sz of @b
 *            if READ,  this must be 0, and it will be assigned a value after Txn
 *            if WRITE, this must be the size of the requested Txn. If the peer does not
 *                      agree with this size, it will return an error, and no Txn will
 *                      take place.
 * blocking : True if the user will call `mtcp_wait_for_read_completion()` or
 *                                       `mtcp_wait_for_write_completion()` before
 *                      submitting more requests
 *            False if the user will continue to submit requests, and use
 *                                       `mtcp_wait_for_any_completion()`
 *
 * retuns opaque type req_t and you can use the `mtcp_req_get_XXX()` variants to
 *                      get the values
 */
req_t *mtcp_submit_request(uint32_t id, req_type_t type, uint8_t *b, uint32_t sz, bool blocking);
/* Destroys a previously returned req_t */
void   mtcp_free_request(req_t *r);


/* wait for a write completion
 * 
 * When this returns, you can
 * - check `mtcp_req_has_error()` to confirm that the write happened
 *   = if there is an error, you can check `mtcp_req_get_error_info()`
 *     to the size the peer is expecting
 * - call `mtcp_free_request()` to destroy the request
 *   = this will also `free()` the buffer you passed earlier to
 *     `mtcp_submit_request()`
 */
bool mtcp_wait_for_write_completion(req_t *r);

/* wait for a read completion
 * 
 * b        : the read buffer (returned)
 *            can be NULL
 *            you can get this also by calling `mtcp_req_get_buffer()`
 * sz       : the read buffer's size (returned)
 *            can be NULL
 *            you can get this also by calling `mtcp_req_get_size()`
 *
 * NOTE: you must call `free()` on the pointer returned in @b to avoid
 *       memory leaks
 */
void mtcp_wait_for_read_completion(req_t *r, uint8_t **b, uint32_t *sz);
/* wait for any submitted request to finish
 *
 * wids         : array of wait IDs (obtained by calling `mtcp_req_get_wait_id()`
 *                on requests submitted earlier
 *                If the requests were submitted with @blocking == true, then
 *                the result of `mtcp_req_get_wait_id()` is invalid
 * count        : number of entries in @wids
 * timeout      : see `sys/time.h`
 *
 * returns exactly one wait id (among the entries in @wids) which finished Txn
 * returns -1 if timeout expires / `select` is interrupted
 *
 * NOTE: there is no gurrantee that the returned wid is the most recently
 *       finished Txn, neither is it guranteed that the returned wid is the
 *       first Txn that finished. The selection logic is random
 */
int mtcp_wait_for_any_completion(int *wids, int count, struct timeval *timeout);

/* helper functions to get values from submitted mtcp requests */
uint32_t        mtcp_req_get_error_info(req_t *r);
req_type_t      mtcp_req_get_req_type(req_t *r);
uint8_t        *mtcp_req_get_buffer(req_t *r);
uint32_t        mtcp_req_get_size(req_t *r);
uint32_t        mtcp_req_get_req_size(req_t *r);
bool            mtcp_req_has_error(req_t *r);
int             mtcp_req_get_wait_id(req_t *r);
int             mtcp_req_get_id(req_t *r);
#ifdef __cplusplus
}
#endif

#endif
