/* =============================================================================
 * net/net_udp.c
 * UDP Protocol: Connectionless datagram transport
 * ============================================================================= */

#include "net.h"
#include "../memory/heap.h"
#include "../lib/string.h"
#include "../drivers/timer/pit.h"

static udp_socket_t udp_sockets[UDP_MAX_SOCKETS];
static uint16_t udp_port_counter = 49152;

typedef struct {
    uint8_t data[4096];
    int len;
    ip4_addr_t src_ip;
    uint16_t src_port;
    bool valid;
} udp_rx_buffer_t;

static udp_rx_buffer_t udp_rx[UDP_MAX_SOCKETS];

/* ─────────────────────────────────────────────────────────────────────────── */
static uint16_t udp_checksum(ip4_addr_t src, ip4_addr_t dst,
                             const void* seg, uint16_t len) {
    struct {
        uint32_t src, dst;
        uint8_t zero, proto;
        uint16_t len;
    } __attribute__((packed)) ph;
    
    ph.src = src;
    ph.dst = dst;
    ph.zero = 0;
    ph.proto = IP_PROTO_UDP;
    ph.len = htons(len);
    
    /* Calculate pseudo-header checksum */
    uint8_t buf[512];
    memcpy(buf, &ph, sizeof(ph));
    memcpy(buf + sizeof(ph), seg, len);
    
    return net_checksum(buf, sizeof(ph) + len);
}

/* ─────────────────────────────────────────────────────────────────────────── */
int net_udp_socket(void) {
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (!udp_sockets[i].bound && !udp_sockets[i].connected) {
            memset(&udp_sockets[i], 0, sizeof(udp_socket_t));
            return i;
        }
    }
    return -1;
}

/* ─────────────────────────────────────────────────────────────────────────── */
int net_udp_bind(int sock, uint16_t port) {
    if (sock < 0 || sock >= UDP_MAX_SOCKETS)
        return -1;
    
    udp_sockets[sock].local_port = port;
    udp_sockets[sock].bound = true;
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
int net_udp_connect(int sock, ip4_addr_t ip, uint16_t port) {
    if (sock < 0 || sock >= UDP_MAX_SOCKETS)
        return -1;
    
    udp_sockets[sock].remote_ip = ip;
    udp_sockets[sock].remote_port = port;
    udp_sockets[sock].connected = true;
    
    if (!udp_sockets[sock].bound) {
        udp_sockets[sock].local_port = udp_port_counter++;
        udp_sockets[sock].bound = true;
    }
    
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
int net_udp_send(int sock, const void* data, uint32_t len) {
    if (sock < 0 || sock >= UDP_MAX_SOCKETS)
        return -1;
    
    if (!udp_sockets[sock].bound || !udp_sockets[sock].connected)
        return -1;
    
    /* Build UDP header */
    uint8_t pkt[4096];
    udp_header_t* udphdr = (udp_header_t*)pkt;
    
    udphdr->src_port = htons(udp_sockets[sock].local_port);
    udphdr->dst_port = htons(udp_sockets[sock].remote_port);
    udphdr->length = htons(sizeof(udp_header_t) + len);
    
    /* Copy payload */
    memcpy(pkt + sizeof(udp_header_t), data, len);
    
    /* Calculate checksum */
    udphdr->checksum = udp_checksum(net_get_ip(), udp_sockets[sock].remote_ip,
                                    pkt, sizeof(udp_header_t) + len);
    if (udphdr->checksum == 0)
        udphdr->checksum = 0xFFFF;  /* RFC 768: 0 means no checksum, use 0xFFFF */
    
    /* Send via IP */
    return net_ip_send(udp_sockets[sock].remote_ip, IP_PROTO_UDP,
                       pkt, sizeof(udp_header_t) + len);
}

/* ─────────────────────────────────────────────────────────────────────────── */
int net_udp_recv(int sock, void* buf, uint32_t maxlen, uint32_t timeout_ms) {
    if (sock < 0 || sock >= UDP_MAX_SOCKETS || !udp_sockets[sock].bound)
        return -1;
    
    uint32_t start = (pit_get_ticks() * 1000) / 100;
    while (1) {
        net_core_poll();
        
        if (udp_rx[sock].valid) {
            int copy_len = (udp_rx[sock].len < (int)maxlen) ? 
                          udp_rx[sock].len : (int)maxlen;
            memcpy(buf, udp_rx[sock].data, copy_len);
            udp_rx[sock].valid = false;
            return copy_len;
        }
        
        uint32_t elapsed = (pit_get_ticks() * 1000) / 100 - start;
        if (elapsed >= timeout_ms)
            break;
        
        pit_sleep(10);
    }
    
    return -1;  /* Timeout */
}

/* ─────────────────────────────────────────────────────────────────────────── */
void net_udp_close(int sock) {
    if (sock < 0 || sock >= UDP_MAX_SOCKETS)
        return;
    
    memset(&udp_sockets[sock], 0, sizeof(udp_socket_t));
    memset(&udp_rx[sock], 0, sizeof(udp_rx_buffer_t));
}

/* ─────────────────────────────────────────────────────────────────────────── */
void net_udp_handle_packet(const ip4_header_t* iphdr, const udp_header_t* udphdr,
                           const void* payload, int len) {
    uint16_t dst_port = ntohs(udphdr->dst_port);
    
    /* Find matching socket */
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (!udp_sockets[i].bound)
            continue;
        
        if (udp_sockets[i].local_port != dst_port)
            continue;
        
        /* Found matching socket, store packet */
        if (!udp_rx[i].valid) {
            int copy_len = (len < (int)sizeof(udp_rx[i].data)) ? len : (int)sizeof(udp_rx[i].data);
            memcpy(udp_rx[i].data, payload, copy_len);
            udp_rx[i].len = copy_len;
            udp_rx[i].src_ip = iphdr->src_ip;
            udp_rx[i].src_port = ntohs(udphdr->src_port);
            udp_rx[i].valid = true;
        }
        return;
    }
}
