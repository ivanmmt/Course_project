/* =============================================================================
 * net/net_ip.c
 * IPv4 Layer 3: Routing, fragmentation, packet handling
 * ============================================================================= */

#include "net.h"
#include "../memory/heap.h"
#include "../lib/string.h"

#define NET_FRAME_BUF_SIZE  2048

static routing_entry_t routing_table[IP_ROUTING_TABLE_SIZE];
static uint16_t ip_id_counter = 1;

/* ─────────────────────────────────────────────────────────────────────────── */
void net_ip_init(void) {
    memset(routing_table, 0, sizeof(routing_table));
    
    /* Default route: 0.0.0.0/0 via gateway */
    routing_table[0].dest = IP4_ANY;
    routing_table[0].mask = IP4_ANY;
    routing_table[0].gateway = IP4_GW;
    routing_table[0].valid = true;
}

/* ─────────────────────────────────────────────────────────────────────────── */
void net_ip_add_route(ip4_addr_t dest, ip4_addr_t mask, ip4_addr_t gw) {
    for (int i = 1; i < IP_ROUTING_TABLE_SIZE; i++) {
        if (!routing_table[i].valid) {
            routing_table[i].dest = dest;
            routing_table[i].mask = mask;
            routing_table[i].gateway = gw;
            routing_table[i].valid = true;
            return;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
static uint32_t count_bits(uint32_t mask) {
    /* Count set bits in mask - simple implementation */
    uint32_t count = 0;
    while (mask) {
        count += (mask & 1);
        mask >>= 1;
    }
    return count;
}

/* ─────────────────────────────────────────────────────────────────────────── */
ip4_addr_t net_ip_next_hop(ip4_addr_t dst_ip) {
    /* Longest prefix match routing */
    int best_match = -1;
    uint32_t longest_prefix = 0;
    
    for (int i = 0; i < IP_ROUTING_TABLE_SIZE; i++) {
        if (!routing_table[i].valid)
            continue;
        
        if ((dst_ip & routing_table[i].mask) == 
            (routing_table[i].dest & routing_table[i].mask)) {
            
            /* Count matching bits in mask */
            uint32_t mask_bits = count_bits(routing_table[i].mask);
            if (mask_bits > longest_prefix) {
                longest_prefix = mask_bits;
                best_match = i;
            }
        }
    }
    
    if (best_match >= 0)
        return routing_table[best_match].gateway;
    
    return IP4_GW;  /* Default gateway */
}

/* ─────────────────────────────────────────────────────────────────────────── */
int net_ip_send(ip4_addr_t dst_ip, uint8_t protocol, const void* payload, uint32_t len) {
    if (len > (NET_FRAME_BUF_SIZE - ETH_HDR_LEN - IP_HDR_LEN))
        return -1;
    
    /* Build IP header */
    uint8_t pkt[NET_FRAME_BUF_SIZE];
    ip4_header_t* iphdr = (ip4_header_t*)pkt;
    
    iphdr->version_ihl = 0x45;  /* IPv4, IHL=5 (20 bytes) */
    iphdr->tos = 0;
    iphdr->length = htons(IP_HDR_LEN + len);
    iphdr->id = htons(ip_id_counter++);
    iphdr->flags_offset = 0;  /* No flags, no fragmentation */
    iphdr->ttl = 64;
    iphdr->protocol = protocol;
    iphdr->checksum = 0;
    iphdr->src_ip = net_get_ip();
    iphdr->dst_ip = dst_ip;
    
    /* Calculate IP checksum */
    iphdr->checksum = net_checksum(iphdr, IP_HDR_LEN);
    
    /* Copy payload */
    if (payload && len > 0)
        memcpy(pkt + IP_HDR_LEN, payload, len);
    
    /* Find next hop */
    ip4_addr_t next_hop = net_ip_next_hop(dst_ip);
    
    /* Resolve next hop MAC via ARP */
    mac_addr_t dst_mac;
    if (!net_arp_resolve(next_hop, &dst_mac, 5000))
        return -1;  /* ARP failed */
    
    /* Send via Ethernet */
    return net_core_send_frame(&dst_mac, ETH_TYPE_IP4, pkt, IP_HDR_LEN + len);
}

/* ─────────────────────────────────────────────────────────────────────────── */
void net_ip_handle_packet(const void* pkt, int len) {
    if (len < IP_HDR_LEN)
        return;
    
    const ip4_header_t* iphdr = (const ip4_header_t*)pkt;
    
    /* Check version */
    if ((iphdr->version_ihl >> 4) != 4)
        return;
    
    /* Check IHL (header length) */
    uint8_t ihl = (iphdr->version_ihl & 0x0F) * 4;
    if (ihl < IP_HDR_LEN || ihl > len)
        return;
    
    /* Check checksum */
    uint16_t saved_checksum = iphdr->checksum;
    uint16_t calc_checksum = net_checksum(iphdr, ihl);
    if (calc_checksum != saved_checksum)
        return;  /* Checksum failed */
    
    /* Check if packet is for us */
    ip4_addr_t our_ip = net_get_ip();
    if (iphdr->dst_ip != our_ip && iphdr->dst_ip != IP4_BCAST)
        return;  /* Not for us */
    
    /* Extract payload */
    const void* payload = (const uint8_t*)pkt + ihl;
    int payload_len = ntohs(iphdr->length) - ihl;
    
    /* Dispatch to appropriate protocol handler */
    switch (iphdr->protocol) {
    case IP_PROTO_ICMP:
        net_icmp_handle_packet(iphdr, payload, payload_len);
        break;
    case IP_PROTO_UDP:
        if (payload_len >= (int)sizeof(udp_header_t)) {
            const udp_header_t* udphdr = (const udp_header_t*)payload;
            net_udp_handle_packet(iphdr, udphdr, 
                                  (const uint8_t*)payload + sizeof(udp_header_t),
                                  payload_len - sizeof(udp_header_t));
        }
        break;
    case IP_PROTO_TCP:
        if (payload_len >= (int)sizeof(tcp_header_t)) {
            const tcp_header_t* tcphdr = (const tcp_header_t*)payload;
            int tcp_hdr_len = (tcphdr->data_offset >> 4) * 4;
            if (tcp_hdr_len < TCP_HDR_MIN_LEN) tcp_hdr_len = TCP_HDR_MIN_LEN;
            if (tcp_hdr_len > payload_len) break;
            net_tcp_handle_packet(iphdr, tcphdr,
                                  (const uint8_t*)payload + tcp_hdr_len,
                                  payload_len - tcp_hdr_len);
        }
        break;
    default:
        /* Unknown protocol, ignore */
        break;
    }
}
