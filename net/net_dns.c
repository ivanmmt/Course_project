/* =============================================================================
 * net/net_dns.c
 * DNS Resolver: Query DNS servers for IP addresses
 * ============================================================================= */

#include "net.h"
#include "../memory/heap.h"
#include "../lib/string.h"

#define DNS_CACHE_SIZE 16
#define DNS_PORT 53
#define DNS_TIMEOUT_MS 5000

typedef struct {
    char hostname[64];
    ip4_addr_t ip;
    uint32_t timestamp;
    bool valid;
} dns_cache_entry_t;

static dns_cache_entry_t dns_cache[DNS_CACHE_SIZE];

void net_dns_add_cache(const char* hostname, ip4_addr_t ip) {
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (!dns_cache[i].valid || strcmp(dns_cache[i].hostname, hostname) == 0) {
            strncpy(dns_cache[i].hostname, hostname, sizeof(dns_cache[i].hostname) - 1);
            dns_cache[i].ip = ip;
            dns_cache[i].timestamp = (pit_get_ticks() * 1000) / 100;
            dns_cache[i].valid = true;
            return;
        }
    }
}

bool net_dns_resolve(const char* hostname, ip4_addr_t* ip_out, uint32_t timeout_ms) {
    if (!hostname || !ip_out)
        return false;
    
    /* Check cache first */
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].valid && strcmp(dns_cache[i].hostname, hostname) == 0) {
            *ip_out = dns_cache[i].ip;
            return true;
        }
    }
    
    /* TODO: Send DNS query via UDP to QEMU DNS (10.0.2.3:53)
     * For now, hardcode some common hosts */
    
    if (strcmp(hostname, "example.com") == 0) {
        *ip_out = IP4(93, 184, 216, 34);
        net_dns_add_cache(hostname, *ip_out);
        return true;
    }
    if (strcmp(hostname, "google.com") == 0) {
        *ip_out = IP4(142, 250, 185, 46);
        net_dns_add_cache(hostname, *ip_out);
        return true;
    }
    if (strcmp(hostname, "localhost") == 0) {
        *ip_out = IP4(127, 0, 0, 1);
        return true;
    }
    
    return false;
}
