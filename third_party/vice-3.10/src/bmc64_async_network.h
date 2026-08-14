#ifndef BMC64_ASYNC_NETWORK_H
#define BMC64_ASYNC_NETWORK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bmc64_async_net_handle_s bmc64_async_net_handle_t;

enum {
    BMC64_ASYNC_NET_CONNECTING = 0,
    BMC64_ASYNC_NET_CONNECTED = 1,
    BMC64_ASYNC_NET_CLOSED = 2,
    BMC64_ASYNC_NET_ERROR = -1
};

bmc64_async_net_handle_t *bmc64_async_net_start(const char *target);
// Enables the fixed rings before VICE starts. The NetworkService owns pumping.
int bmc64_async_net_initialize(void);
void bmc64_async_net_process(void);
void bmc64_async_net_stop(bmc64_async_net_handle_t *handle);
int bmc64_async_net_status(bmc64_async_net_handle_t *handle);
int bmc64_async_net_error(bmc64_async_net_handle_t *handle);
int bmc64_async_net_send(bmc64_async_net_handle_t *handle,
                         const uint8_t *data, size_t len);
int bmc64_async_net_receive(bmc64_async_net_handle_t *handle,
                            uint8_t *data, size_t len);
size_t bmc64_async_net_rx_pending(bmc64_async_net_handle_t *handle);
size_t bmc64_async_net_rx_free(bmc64_async_net_handle_t *handle);
size_t bmc64_async_net_tx_pending(bmc64_async_net_handle_t *handle);
size_t bmc64_async_net_tx_free(bmc64_async_net_handle_t *handle);
unsigned long bmc64_async_net_tx_dropped(bmc64_async_net_handle_t *handle);
unsigned long bmc64_async_net_rx_dropped(bmc64_async_net_handle_t *handle);

#ifdef __cplusplus
}
#endif

#endif
