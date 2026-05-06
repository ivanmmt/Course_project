/* =============================================================================
 * net/net_icmp.c
 * ICMP Protocol: Echo request/reply (ping)
 * ============================================================================= */

#include "net.h"
#include "../memory/heap.h"
#include "../lib/string.h"
#include "../drivers/timer/pit.h"

static struct {
    uint16_t id;
    uint16_t seq;
    bool received;
} icmp_ping_state = {0, 0, false};

/* ─────────────────────────────────────────────────────────────────────────── */
static uint16_t icmp_checksum(const icmp_header_t* hdr, uint32_t len) {
    /* Calculate pseudo-checksum (checksum field must be zero) */
    uint8_t buf[256];
    memcpy(buf, hdr, len);
    ((icmp_header_t*)buf)->checksum = 0;
    return net_checksum(buf, len);
}

/* ─────────────────────────────────────────────────────────────────────────── */
bool net_icmp_ping(ip4_addr_t ip, uint32_t timeout_ms) {
    /* Build ICMP echo request */
    icmp_header_t icmp;
    memset(&icmp, 0, sizeof(icmp));
    
    icmp.type = ICMP_TYPE_ECHO_REQUEST;
    icmp.code = 0;
    icmp.id = 0x1234;
    icmp.seq = icmp_ping_state.seq++;
    
    icmp.checksum = icmp_checksum(&icmp, sizeof(icmp));
    
    icmp_ping_state.received = false;
    icmp_ping_state.id = icmp.id;
    icmp_ping_state.seq = icmp.seq;
    
    /* Send via IP */
    if (net_ip_send(ip, IP_PROTO_ICMP, &icmp, sizeof(icmp)) < 0)
        return false;
    
    /* Wait for reply */
    uint32_t start = (pit_get_ticks() * 1000) / 100;
    while (1) {
        net_core_poll();
        
        if (icmp_ping_state.received)
            return true;
        
        uint32_t elapsed = (pit_get_ticks() * 1000) / 100 - start;
        if (elapsed >= timeout_ms)
            break;
        
        pit_sleep(10);
    }
    
    return false;
}

/* ─────────────────────────────────────────────────────────────────────────── */
void net_icmp_handle_packet(const ip4_header_t* iphdr, const void* payload, int len) {
    if (len < (int)sizeof(icmp_header_t))
        return;
    
    const icmp_header_t* icmp = (const icmp_header_t*)payload;
    
    switch (icmp->type) {
    case ICMP_TYPE_ECHO_REQUEST: {
        /* Send echo reply */
        icmp_header_t reply = *icmp;
        reply.type = ICMP_TYPE_ECHO_REPLY;
        reply.checksum = 0;
        reply.checksum = icmp_checksum(&reply, len);
        
        net_ip_send(iphdr->src_ip, IP_PROTO_ICMP, &reply, len);
        break;
    }
    
    case ICMP_TYPE_ECHO_REPLY: {
        /* Check if this is our ping */
        if (icmp->id == icmp_ping_state.id && 
            icmp->seq == icmp_ping_state.seq) {
            icmp_ping_state.received = true;
        }
        break;
    }
    
    default:
        /* Ignore other ICMP types */
        break;
    }
}
