/* =============================================================================
 * net/net.h
 * Linux-style Network Stack: Ethernet → IP → TCP/UDP → Sockets → Apps
 * ============================================================================= */

#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../drivers/net/e1000.h"  /* mac_addr_t is defined here */

/* ═══════════════════════════════════════════════════════════════════════════
 * BASIC TYPES & CONSTANTS
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef uint32_t ip4_addr_t;      /* IPv4 address (big-endian) */
/* mac_addr_t is defined in e1000.h as: struct { uint8_t b[6]; } */

/* Quick IP constructors */
#define IP4(a,b,c,d) ((ip4_addr_t)( \
    ((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(d)))

#define IP4_MYOS    IP4(10,0,2,15)    /* QEMU user-mode our address */
#define IP4_GW      IP4(10,0,2,2)     /* QEMU gateway */
#define IP4_DNS     IP4(10,0,2,3)     /* QEMU DNS */
#define IP4_BCAST   IP4(255,255,255,255)
#define IP4_ANY     IP4(0,0,0,0)

/* Network byte order conversions */
static inline uint16_t htons(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}
static inline uint32_t htonl(uint32_t x) {
    return ((x >> 24) & 0xFF) | (((x >> 16) & 0xFF) << 8) |
           (((x >> 8) & 0xFF) << 16) | ((x & 0xFF) << 24);
}
#define ntohs htons
#define ntohl htonl

/* ═══════════════════════════════════════════════════════════════════════════
 * PACKET STRUCTURES (Layer 2-4)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* --- Ethernet Frame Header --- */
#define ETH_TYPE_IP4    0x0800
#define ETH_TYPE_ARP    0x0806
#define ETH_ADDR_LEN    6
#define ETH_TYPE_LEN    2
#define ETH_HDR_LEN     14

typedef struct {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;        /* big-endian */
} __attribute__((packed)) eth_header_t;

/* --- ARP Packet --- */
#define ARP_HTYPE_ETH   1
#define ARP_PTYPE_IP4   0x0800
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2
#define ARP_HDR_LEN     28

typedef struct {
    uint16_t htype;       /* 1 = Ethernet */
    uint16_t ptype;       /* 0x0800 = IPv4 */
    uint8_t  hlen;        /* 6 */
    uint8_t  plen;        /* 4 */
    uint16_t oper;        /* 1=request, 2=reply */
    uint8_t  sha[6];      /* sender MAC */
    uint32_t spa;         /* sender IP */
    uint8_t  tha[6];      /* target MAC */
    uint32_t tpa;         /* target IP */
} __attribute__((packed)) arp_packet_t;

/* --- IPv4 Header --- */
#define IP_PROTO_ICMP   1
#define IP_PROTO_IGMP   2
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17
#define IP_HDR_LEN      20

typedef struct {
    uint8_t  version_ihl;   /* version(4) + IHL(4) */
    uint8_t  tos;           /* type of service */
    uint16_t length;        /* total length (big-endian) */
    uint16_t id;            /* identification */
    uint16_t flags_offset;  /* flags(3) + fragment offset(13) */
    uint8_t  ttl;           /* time to live */
    uint8_t  protocol;      /* protocol */
    uint16_t checksum;      /* header checksum */
    uint32_t src_ip;        /* source IP */
    uint32_t dst_ip;        /* destination IP */
} __attribute__((packed)) ip4_header_t;

/* IP flags (in flags_offset) */
#define IP_FLAG_DF  0x4000  /* Don't Fragment */
#define IP_FLAG_MF  0x2000  /* More Fragments */
#define IP_OFFSET_MASK 0x1FFF

/* --- ICMP Header --- */
#define ICMP_TYPE_ECHO_REPLY    0
#define ICMP_TYPE_ECHO_REQUEST  8
#define ICMP_HDR_LEN            8

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed)) icmp_header_t;

/* --- UDP Header --- */
#define UDP_HDR_LEN 8

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_header_t;

/* --- TCP Header --- */
#define TCP_HDR_MIN_LEN 20

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_offset;  /* header length >> 4 */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed)) tcp_header_t;

/* TCP flags */
#define TCP_FLAG_FIN  0x01
#define TCP_FLAG_SYN  0x02
#define TCP_FLAG_RST  0x04
#define TCP_FLAG_PSH  0x08
#define TCP_FLAG_ACK  0x10
#define TCP_FLAG_URG  0x20

/* TCP states (RFC 793) */
typedef enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RCVD,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
} tcp_state_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * ARP (Layer 3 - Resolution)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define ARP_TABLE_SIZE  16
#define ARP_TIMEOUT_MS  300000  /* 5 minutes */

typedef struct {
    ip4_addr_t ip;
    mac_addr_t mac;
    uint32_t   timestamp;
    bool       valid;
} arp_entry_t;

/* ARP functions (net_arp.c) */
void net_arp_init(void);
bool net_arp_resolve(ip4_addr_t ip, mac_addr_t* mac_out, uint32_t timeout_ms);
void net_arp_table_add(ip4_addr_t ip, mac_addr_t mac);
void net_arp_table_clear(void);
void net_arp_handle_packet(const arp_packet_t* pkt, int len);

/* ═══════════════════════════════════════════════════════════════════════════
 * ICMP (Layer 4 - Control Messages)
 * ═══════════════════════════════════════════════════════════════════════════ */

bool net_icmp_ping(ip4_addr_t ip, uint32_t timeout_ms);
void net_icmp_handle_packet(const ip4_header_t* iphdr, const void* payload, int len);

/* ═══════════════════════════════════════════════════════════════════════════
 * UDP (Layer 4 - Transport)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define UDP_MAX_SOCKETS 8

typedef struct {
    uint16_t local_port;
    ip4_addr_t remote_ip;
    uint16_t remote_port;
    bool bound;
    bool connected;
} udp_socket_t;

int net_udp_socket(void);
int net_udp_bind(int sock, uint16_t port);
int net_udp_connect(int sock, ip4_addr_t ip, uint16_t port);
int net_udp_send(int sock, const void* data, uint32_t len);
int net_udp_recv(int sock, void* buf, uint32_t maxlen, uint32_t timeout_ms);
void net_udp_close(int sock);
void net_udp_handle_packet(const ip4_header_t* iphdr, const udp_header_t* udphdr, 
                           const void* payload, int len);

/* ═══════════════════════════════════════════════════════════════════════════
 * TCP (Layer 4 - Reliable Transport)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define TCP_MAX_SOCKETS   8
#define TCP_RX_BUF_SIZE   4096
#define TCP_TX_BUF_SIZE   4096
#define TCP_TIMEOUT_MS    30000    /* 30 seconds */
#define TCP_RETRANSMIT_MS 1000     /* 1 second */
#define TCP_MAX_RETRIES   3

typedef struct {
    /* Connection state */
    tcp_state_t state;
    ip4_addr_t  remote_ip;
    uint16_t    remote_port;
    uint16_t    local_port;
    
    /* Sequence numbers */
    uint32_t    seq;          /* Our sequence number */
    uint32_t    ack;          /* Last ACK we sent */
    uint32_t    peer_seq;     /* Peer's sequence number */
    uint32_t    peer_ack;     /* Peer's ACK */
    
    /* TCP window & flow control */
    uint16_t    window;       /* Our receive window */
    uint16_t    peer_window;  /* Peer's receive window */
    
    /* Buffers */
    uint8_t     rx_buf[TCP_RX_BUF_SIZE];
    uint32_t    rx_head;
    uint32_t    rx_tail;
    
    uint8_t     tx_buf[TCP_TX_BUF_SIZE];
    uint32_t    tx_head;
    uint32_t    tx_tail;
    
    /* Retransmission */
    uint32_t    last_tx_time;
    uint32_t    retry_count;
    
    /* Timeouts */
    uint32_t    last_activity;
    uint32_t    timeout_ms;
    
    bool        valid;
    bool        in_use;
} tcp_socket_t;

/* TCP socket API (like BSD sockets) */
int net_tcp_socket(void);
int net_tcp_connect(int sock, ip4_addr_t ip, uint16_t port, uint32_t timeout_ms);
int net_tcp_send(int sock, const void* data, uint32_t len);
int net_tcp_recv(int sock, void* buf, uint32_t maxlen, uint32_t timeout_ms);
int net_tcp_close(int sock);
void net_tcp_handle_packet(const ip4_header_t* iphdr, const tcp_header_t* tcphdr,
                           const void* payload, int len);

/* ═══════════════════════════════════════════════════════════════════════════
 * IPv4 (Layer 3 - Network)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define IP_ROUTING_TABLE_SIZE 8

typedef struct {
    ip4_addr_t dest;
    ip4_addr_t mask;
    ip4_addr_t gateway;
    bool       valid;
} routing_entry_t;

void net_ip_init(void);
int net_ip_send(ip4_addr_t dst_ip, uint8_t protocol, const void* payload, uint32_t len);
void net_ip_handle_packet(const void* pkt, int len);
void net_ip_add_route(ip4_addr_t dest, ip4_addr_t mask, ip4_addr_t gw);
ip4_addr_t net_ip_next_hop(ip4_addr_t dst_ip);

/* ═══════════════════════════════════════════════════════════════════════════
 * Ethernet (Layer 2 - Data Link)
 * ═══════════════════════════════════════════════════════════════════════════ */

void net_core_init(void);
int net_core_send_frame(const mac_addr_t* dst_mac, uint16_t ethertype,
                        const void* payload, uint32_t len);
void net_core_handle_frame(const void* frame, int len);
void net_core_poll(void);  /* Check for incoming frames */

/* ═══════════════════════════════════════════════════════════════════════════
 * DNS (Layer 7 - Application)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define DNS_CACHE_SIZE 16

bool net_dns_resolve(const char* hostname, ip4_addr_t* ip_out, uint32_t timeout_ms);
void net_dns_add_cache(const char* hostname, ip4_addr_t ip);

/* ═══════════════════════════════════════════════════════════════════════════
 * HTTP (Layer 7 - Application)
 * ═══════════════════════════════════════════════════════════════════════════ */

int net_http_get(const char* host, uint16_t port, const char* path,
                 char* response_buf, uint32_t buf_size, uint32_t timeout_ms);

/* ═══════════════════════════════════════════════════════════════════════════
 * TLS (Layer 6 - Encryption)
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    int tcp_sock;
    uint8_t session_key[32];
    uint8_t mac_key[32];
    uint8_t iv[16];
    uint32_t seq;
    bool valid;
} tls_socket_t;

tls_socket_t* net_tls_connect(ip4_addr_t ip, uint16_t port, uint32_t timeout_ms);
int net_tls_send(tls_socket_t* tls, const void* data, uint32_t len);
int net_tls_recv(tls_socket_t* tls, void* buf, uint32_t maxlen);
void net_tls_close(tls_socket_t* tls);

int net_https_get(const char* host, uint16_t port, const char* path,
                  char* response_buf, uint32_t buf_size, uint32_t timeout_ms);

/* ═══════════════════════════════════════════════════════════════════════════
 * CORE NETWORK FUNCTIONS
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Initialize entire network stack */
void net_init(void);

/* Check if network is ready */
bool net_ready(void);

/* Poll for incoming network events */
void net_poll(void);

/* ICMP ping - wrapper for backward compatibility */
bool net_ping(ip4_addr_t ip, uint32_t timeout_ms);

/* Get our IP and MAC */
ip4_addr_t net_get_ip(void);
mac_addr_t net_get_mac(void);

/* Set our IP and MAC */
void net_set_ip(ip4_addr_t ip);
void net_set_mac(mac_addr_t mac);

/* Process network events (call regularly from main loop) */
void net_poll(void);

/* Utilities */
uint16_t net_checksum(const void* data, uint32_t len);
void net_dump_ip(ip4_addr_t ip);
void net_dump_mac(const mac_addr_t* mac);

#endif /* NET_H */
