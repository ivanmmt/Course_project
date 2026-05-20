/* =============================================================================
 * net/net_dns.c — DNS resolver (UDP queries to configured DNS server)
 * ============================================================================= */
#include "net.h"
#include "../memory/heap.h"
#include "../lib/string.h"
#include "../drivers/timer/pit.h"

#define DNS_CACHE_SIZE  16
#define DNS_PORT        53
#define DNS_TIMEOUT_MS  5000
#define DNS_PKT_MAX     512

typedef struct {
    char hostname[64];
    ip4_addr_t ip;
    uint32_t timestamp;
    bool valid;
} dns_cache_entry_t;

static dns_cache_entry_t dns_cache[DNS_CACHE_SIZE];

static int dns_encode_qname(const char* host, uint8_t* out) {
    int pos = 0;
    const char* p = host;
    while (*p) {
        const char* dot = strchr(p, '.');
        int lab_len = dot ? (int)(dot - p) : (int)strlen(p);
        if (lab_len <= 0 || lab_len > 63) return -1;
        out[pos++] = (uint8_t)lab_len;
        memcpy(out + pos, p, (size_t)lab_len);
        pos += lab_len;
        p += lab_len;
        if (*p == '.') p++;
    }
    out[pos++] = 0;
    return pos;
}

static bool dns_name_matches(const uint8_t* pkt, uint32_t pkt_len, uint32_t off,
                             const char* host) {
    const char* p = host;
    while (*p && off < pkt_len) {
        uint8_t lab = pkt[off++];
        if (lab >= 0xC0) return false;
        if (lab == 0) return (*p == 0);
        if (lab > 63 || off + lab > pkt_len) return false;
        for (uint8_t i = 0; i < lab; i++) {
            char c = (char)pkt[off + i];
            if (*p >= 'A' && *p <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != *p) return false;
            p++;
        }
        off += lab;
        if (*p == '.') p++;
    }
    return false;
}

static uint32_t dns_skip_name(const uint8_t* pkt, uint32_t pkt_len, uint32_t off) {
    while (off < pkt_len) {
        uint8_t lab = pkt[off];
        if (lab == 0) return off + 1;
        if (lab >= 0xC0) return (off + 2 <= pkt_len) ? off + 2 : pkt_len;
        off += 1u + lab;
    }
    return off;
}

void net_dns_add_cache(const char* hostname, ip4_addr_t ip) {
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (!dns_cache[i].valid || strcmp(dns_cache[i].hostname, hostname) == 0) {
            strncpy(dns_cache[i].hostname, hostname, sizeof(dns_cache[i].hostname) - 1);
            dns_cache[i].hostname[sizeof(dns_cache[i].hostname) - 1] = '\0';
            dns_cache[i].ip = ip;
            dns_cache[i].timestamp = (pit_get_ticks() * 1000) / 100;
            dns_cache[i].valid = true;
            return;
        }
    }
}

bool net_dns_resolve(const char* hostname, ip4_addr_t* ip_out, uint32_t timeout_ms) {
    if (!hostname || !ip_out || !hostname[0])
        return false;

    if (strcmp(hostname, "localhost") == 0) {
        *ip_out = IP4(127, 0, 0, 1);
        return true;
    }

    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].valid && strcmp(dns_cache[i].hostname, hostname) == 0) {
            *ip_out = dns_cache[i].ip;
            return true;
        }
    }

    uint8_t query[DNS_PKT_MAX];
    memset(query, 0, sizeof(query));

    uint16_t txid = (uint16_t)(pit_get_ticks() ^ 0x5A5Au);
    query[0] = (uint8_t)(txid >> 8);
    query[1] = (uint8_t)(txid);
    query[2] = 0x01; /* recursion desired */
    query[5] = 0x01; /* one question */

    int qoff = 12;
    int nlen = dns_encode_qname(hostname, query + qoff);
    if (nlen < 0) return false;
    qoff += nlen;
    if (qoff + 4 > (int)sizeof(query)) return false;
    query[qoff++] = 0x00; query[qoff++] = 0x01; /* A */
    query[qoff++] = 0x00; query[qoff++] = 0x01; /* IN */

    int sock = net_udp_socket();
    if (sock < 0) return false;
    if (net_udp_connect(sock, IP4_DNS, DNS_PORT) < 0) {
        net_udp_close(sock);
        return false;
    }
    if (net_udp_send(sock, query, (uint32_t)qoff) < 0) {
        net_udp_close(sock);
        return false;
    }

    uint8_t resp[DNS_PKT_MAX];
    int rlen = net_udp_recv(sock, resp, sizeof(resp), timeout_ms ? timeout_ms : DNS_TIMEOUT_MS);
    net_udp_close(sock);
    if (rlen < 12) return false;

    if (((resp[0] << 8) | resp[1]) != txid) return false;
    if ((resp[3] & 0x0F) != 0) return false; /* RCODE */

    uint16_t qd = (uint16_t)((resp[4] << 8) | resp[5]);
    uint16_t an = (uint16_t)((resp[6] << 8) | resp[7]);
    uint32_t off = 12;

    for (uint16_t qi = 0; qi < qd && off < (uint32_t)rlen; qi++)
        off = dns_skip_name(resp, (uint32_t)rlen, off) + 4;

    for (uint16_t ai = 0; ai < an && off + 12 <= (uint32_t)rlen; ai++) {
        if (resp[off] >= 0xC0) off += 2;
        else off = dns_skip_name(resp, (uint32_t)rlen, off);
        if (off + 10 > (uint32_t)rlen) break;
        uint16_t type = (uint16_t)((resp[off] << 8) | resp[off + 1]);
        uint16_t cls  = (uint16_t)((resp[off + 2] << 8) | resp[off + 3]);
        uint32_t ttl  = ((uint32_t)resp[off + 4] << 24) | ((uint32_t)resp[off + 5] << 16) |
                        ((uint32_t)resp[off + 6] << 8) | resp[off + 7];
        (void)ttl;
        uint16_t rdlen = (uint16_t)((resp[off + 8] << 8) | resp[off + 9]);
        off += 10;
        if (off + rdlen > (uint32_t)rlen) break;
        if (type == 1 && cls == 1 && rdlen == 4) {
            ip4_addr_t ip = ((ip4_addr_t)resp[off] << 24) | ((ip4_addr_t)resp[off + 1] << 16) |
                            ((ip4_addr_t)resp[off + 2] << 8) | resp[off + 3];
            *ip_out = ip;
            net_dns_add_cache(hostname, ip);
            return true;
        }
        off += rdlen;
    }

    return false;
}
