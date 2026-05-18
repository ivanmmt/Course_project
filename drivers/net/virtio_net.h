/* =============================================================================
 * drivers/net/virtio_net.h
 * VirtIO Network Device Driver
 * ============================================================================= */

#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>
#include <stdbool.h>

/* VirtIO initialization */
bool virtio_net_init(void);
bool virtio_net_present(void);

/* Packet transmission/reception */
int virtio_net_send(const void* data, int len);
int virtio_net_recv(void* buf, int maxlen);

/* Get MAC address */
typedef struct {
    uint8_t bytes[6];
} mac_addr_t;

mac_addr_t virtio_net_get_mac(void);

#endif
