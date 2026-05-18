/* =============================================================================
 * drivers/net/rtl8139.h
 * RTL8139 (Realtek 8139) Ethernet Driver
 * ============================================================================= */

#ifndef RTL8139_H
#define RTL8139_H

#include <stdint.h>
#include <stdbool.h>

/* RTL8139 initialization */
bool rtl8139_init(void);
bool rtl8139_present(void);

/* Packet transmission/reception */
int rtl8139_send(const void* data, int len);
int rtl8139_recv(void* buf, int maxlen);

/* Get MAC address */
typedef struct {
    uint8_t bytes[6];
} mac_addr_t;

mac_addr_t rtl8139_get_mac(void);

#endif
