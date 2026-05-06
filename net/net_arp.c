/* =============================================================================
 * net/net_arp.c
 * ARP Protocol (Address Resolution Protocol)
 * ============================================================================= */

#include "net.h"
#include "../memory/heap.h"
#include "../lib/string.h"
#include "../drivers/timer/pit.h"
#include "../drivers/vga/vga_text.h"

static arp_entry_t arp_table[ARP_TABLE_SIZE];
static uint32_t arp_time_ms = 0;

/* ─────────────────────────────────────────────────────────────────────────── */
void net_arp_init(void) {
    memset(arp_table, 0, sizeof(arp_table));
    arp_time_ms = (pit_get_ticks() * 1000) / 100;  /* assuming 100Hz timer */
}

/* ─────────────────────────────────────────────────────────────────────────── */
void net_arp_table_add(ip4_addr_t ip, mac_addr_t mac) {
    /* Find existing or empty slot */
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid || arp_table[i].ip == ip) {
            arp_table[i].ip = ip;
            memcpy(arp_table[i].mac.b, mac.b, 6);
            arp_table[i].timestamp = arp_time_ms;
            arp_table[i].valid = true;
            return;
        }
    }
    
    /* Table full, overwrite oldest */
    int oldest = 0;
    uint32_t oldest_time = arp_table[0].timestamp;
    for (int i = 1; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].timestamp < oldest_time) {
            oldest_time = arp_table[i].timestamp;
            oldest = i;
        }
    }
    
    arp_table[oldest].ip = ip;
    memcpy(arp_table[oldest].mac.b, mac.b, 6);
    arp_table[oldest].timestamp = arp_time_ms;
    arp_table[oldest].valid = true;
}

/* ─────────────────────────────────────────────────────────────────────────── */
static arp_entry_t* arp_table_find(ip4_addr_t ip) {
    uint32_t now = (pit_get_ticks() * 1000) / 100;
    
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid)
            continue;
        
        if (arp_table[i].ip != ip)
            continue;
        
        /* Check if expired */
        if ((now - arp_table[i].timestamp) > ARP_TIMEOUT_MS) {
            arp_table[i].valid = false;
            continue;
        }
        
        return &arp_table[i];
    }
    
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────── */
static void arp_send_request(ip4_addr_t target_ip) {
    arp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    
    pkt.htype = htons(ARP_HTYPE_ETH);
    pkt.ptype = htons(ARP_PTYPE_IP4);
    pkt.hlen = 6;
    pkt.plen = 4;
    pkt.oper = htons(ARP_OP_REQUEST);
    
    /* Our MAC and IP */
    mac_addr_t our_mac = net_get_mac();
    memcpy(pkt.sha, our_mac.b, 6);
    pkt.spa = net_get_ip();
    
    /* Target IP (MAC unknown) */
    memset(pkt.tha, 0, 6);
    pkt.tpa = target_ip;
    
    /* Broadcast to FF:FF:FF:FF:FF:FF */
    mac_addr_t bcast = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    net_core_send_frame(&bcast, ETH_TYPE_ARP, &pkt, sizeof(pkt));
}

/* ─────────────────────────────────────────────────────────────────────────── */
bool net_arp_resolve(ip4_addr_t ip, mac_addr_t* mac_out, uint32_t timeout_ms) {
    if (!mac_out)
        return false;
    
    /* Check cache first */
    arp_entry_t* entry = arp_table_find(ip);
    if (entry) {
        memcpy(mac_out->b, entry->mac.b, 6);
        return true;
    }
    
    /* Not in cache, send ARP request */
    arp_send_request(ip);
    
    /* Wait for reply (polling) */
    uint32_t start_time = (pit_get_ticks() * 1000) / 100;
    while (1) {
        net_core_poll();
        
        entry = arp_table_find(ip);
        if (entry) {
            memcpy(mac_out->b, entry->mac.b, 6);
            return true;
        }
        
        uint32_t elapsed = (pit_get_ticks() * 1000) / 100 - start_time;
        if (elapsed >= timeout_ms)
            break;
        
        pit_sleep(10);  /* Sleep 10ms to avoid busy-wait */
    }
    
    return false;
}

/* ─────────────────────────────────────────────────────────────────────────── */
void net_arp_table_clear(void) {
    memset(arp_table, 0, sizeof(arp_table));
}

/* ─────────────────────────────────────────────────────────────────────────── */
void net_arp_handle_packet(const arp_packet_t* pkt, int len) {
    if (len < sizeof(arp_packet_t))
        return;
    
    uint16_t oper = ntohs(pkt->oper);
    
    /* Only handle requests for our IP */
    if (pkt->tpa != net_get_ip())
        return;
    
    if (oper == ARP_OP_REQUEST) {
        /* Add sender to ARP cache */
        mac_addr_t sender_mac;
        memcpy(sender_mac.b, pkt->sha, 6);
        net_arp_table_add(pkt->spa, sender_mac);
        
        /* Send ARP reply */
        arp_packet_t reply;
        memset(&reply, 0, sizeof(reply));
        
        reply.htype = htons(ARP_HTYPE_ETH);
        reply.ptype = htons(ARP_PTYPE_IP4);
        reply.hlen = 6;
        reply.plen = 4;
        reply.oper = htons(ARP_OP_REPLY);
        
        /* Our MAC and IP */
        mac_addr_t our_mac = net_get_mac();
        memcpy(reply.sha, our_mac.b, 6);
        reply.spa = net_get_ip();
        
        /* Sender becomes target */
        memcpy(reply.tha, pkt->sha, 6);
        reply.tpa = pkt->spa;
        
        /* Send reply to sender */
        net_core_send_frame(&sender_mac, ETH_TYPE_ARP, &reply, sizeof(reply));
        
    } else if (oper == ARP_OP_REPLY) {
        /* Add to ARP cache */
        mac_addr_t sender_mac;
        memcpy(sender_mac.b, pkt->sha, 6);
        net_arp_table_add(pkt->spa, sender_mac);
    }
}
