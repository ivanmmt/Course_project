/* =============================================================================
 * net/net_core.c
 * Ethernet Layer 2: Frame send/recv, dispatching
 * ============================================================================= */

#include "net.h"
#include "../drivers/net/e1000.h"
#include "../memory/heap.h"
#include "../lib/string.h"
#include "../drivers/timer/pit.h"

#define NET_FRAME_BUF_SIZE  2048
#define NET_RX_FRAMES       16

static mac_addr_t our_mac = {{0x52, 0x54, 0x00, 0x12, 0x34, 0x56}};  /* QEMU default */
static ip4_addr_t our_ip = IP4_MYOS;

static bool net_initialized = false;

/* Receive buffer ring */
static struct {
    uint8_t data[NET_FRAME_BUF_SIZE];
    int len;
    bool valid;
} rx_ring[NET_RX_FRAMES];
static int rx_head = 0;

/* ─────────────────────────────────────────────────────────────────────────── */
void net_core_init(void) {
    if (net_initialized) return;
    
    e1000_init();  /* Initialize NIC driver */
    
    /* Get MAC from hardware */
    our_mac = e1000_get_mac();
    
    net_initialized = true;
}

/* ─────────────────────────────────────────────────────────────────────────── */
int net_core_send_frame(const mac_addr_t* dst_mac, uint16_t ethertype,
                        const void* payload, uint32_t len) {
    if (!net_initialized || len > (NET_FRAME_BUF_SIZE - ETH_HDR_LEN))
        return -1;
    
    /* Build Ethernet frame */
    uint8_t frame[NET_FRAME_BUF_SIZE];
    eth_header_t* eth = (eth_header_t*)frame;
    
    memcpy(eth->dst, dst_mac->b, 6);
    memcpy(eth->src, our_mac.b, 6);
    eth->type = htons(ethertype);
    
    /* Copy payload */
    if (payload && len > 0)
        memcpy(frame + ETH_HDR_LEN, payload, len);
    
    /* Send via NIC */
    int ret = e1000_send((const uint8_t*)frame, ETH_HDR_LEN + len);
    return ret;
}

/* ─────────────────────────────────────────────────────────────────────────── */
void net_core_handle_frame(const void* frame, int len) {
    if (len < ETH_HDR_LEN)
        return;
    
    const eth_header_t* eth = (const eth_header_t*)frame;
    uint16_t ethertype = ntohs(eth->type);
    
    const void* payload = (const uint8_t*)frame + ETH_HDR_LEN;
    int payload_len = len - ETH_HDR_LEN;
    
    switch (ethertype) {
    case ETH_TYPE_ARP:
        net_arp_handle_packet((const arp_packet_t*)payload, payload_len);
        break;
    case ETH_TYPE_IP4:
        net_ip_handle_packet(payload, payload_len);
        break;
    default:
        /* Unknown ethertype, ignore */
        break;
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
void net_core_poll(void) {
    if (!net_initialized)
        return;
    
    /* Poll NIC for incoming frames */
    uint8_t buf[NET_FRAME_BUF_SIZE];
    int len = e1000_recv(buf, NET_FRAME_BUF_SIZE);
    
    if (len > 0) {
        net_core_handle_frame(buf, len);
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
mac_addr_t net_get_mac(void) {
    return our_mac;
}

void net_set_mac(mac_addr_t mac) {
    memcpy(our_mac.b, mac.b, 6);
}

ip4_addr_t net_get_ip(void) {
    return our_ip;
}

void net_set_ip(ip4_addr_t ip) {
    our_ip = ip;
}

/* ─────────────────────────────────────────────────────────────────────────── */
void net_dump_mac(const mac_addr_t* mac) {
    if (!mac) return;
    /* Debug: print MAC address (requires kprintf) */
    (void)mac;  /* Suppress unused warning */
}

void net_dump_ip(ip4_addr_t ip) {
    /* Debug: print IP address (requires kprintf) */
    (void)ip;  /* Suppress unused warning */
}

/* ─────────────────────────────────────────────────────────────────────────── */
uint16_t net_checksum(const void* data, uint32_t len) {
    const uint16_t* p = (const uint16_t*)data;
    uint32_t sum = 0;
    
    while (len >= 2) {
        sum += *p++;
        len -= 2;
    }
    
    if (len)
        sum += *(const uint8_t*)p;
    
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    
    return (uint16_t)~sum;
}
