/* =============================================================================
 * net/net.h
 * Минимальный стек TCP/IP
 *
 * Поддерживаем:
 *   Ethernet II → IP v4 → ICMP (ping) → UDP → TCP → HTTP GET
 *
 * DHCP: через QEMU user-mode network автоматически получаем 10.0.2.15
 * DNS:  QEMU resolver на 10.0.2.3
 * GW:   10.0.2.2 (QEMU NAT gateway)
 * ============================================================================= */

#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../drivers/net/e1000.h"

/* ─── IP адрес ─── */
typedef uint32_t ip4_addr_t;  /* big-endian */

#define IP4(a,b,c,d) ((ip4_addr_t)( \
    ((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(d)))

#define IP4_MYOS    IP4(10,0,2,15)    /* QEMU user-mode наш адрес */
#define IP4_GW      IP4(10,0,2,2)     /* QEMU шлюз */
#define IP4_DNS     IP4(10,0,2,3)     /* QEMU DNS */
#define IP4_BCAST   IP4(255,255,255,255)

/* ─── Ethernet заголовок ─── */
typedef struct {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype;   /* big-endian: 0x0800=IPv4, 0x0806=ARP */
} __attribute__((packed)) eth_header_t;

#define ETH_IP4  0x0800
#define ETH_ARP  0x0806

/* ─── ARP пакет ─── */
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

/* ─── IPv4 заголовок ─── */
typedef struct {
    uint8_t  ihl_ver;     /* version(4) + IHL(4) */
    uint8_t  tos;
    uint16_t total_len;   /* big-endian */
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;    /* 1=ICMP, 6=TCP, 17=UDP */
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed)) ip4_header_t;

#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17

/* ─── ICMP ─── */
typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed)) icmp_header_t;

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY   0

/* ─── UDP ─── */
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_header_t;

/* ─── TCP ─── */
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;    /* header length >> 4 */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed)) tcp_header_t;

#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10

/* ─── ARP таблица ─── */
#define ARP_TABLE_SIZE 16
typedef struct {
    ip4_addr_t ip;
    uint8_t    mac[6];
    bool       valid;
} arp_entry_t;

/* ─── Простое TCP соединение ─── */
typedef struct {
    ip4_addr_t remote_ip;
    uint16_t   remote_port;
    uint16_t   local_port;
    uint32_t   seq;
    uint32_t   ack;
    bool       connected;
} tcp_conn_t;

/* ─── API ─── */

/* Инициализация стека */
void net_init(void);
bool net_ready(void);

/* Наш IP и MAC */
ip4_addr_t net_get_ip(void);
mac_addr_t net_get_mac(void);

/* ARP — получить MAC по IP */
bool net_arp_resolve(ip4_addr_t ip, uint8_t* mac_out);

/* ICMP ping */
bool net_ping(ip4_addr_t ip, uint32_t timeout_ms);

/* DNS resolver */
bool net_dns_resolve(const char* hostname, ip4_addr_t* ip_out);

/* TCP */
tcp_conn_t* net_tcp_connect(ip4_addr_t ip, uint16_t port);
int  net_tcp_send(tcp_conn_t* conn, const void* data, uint32_t len);
int  net_tcp_recv(tcp_conn_t* conn, void* buf, uint32_t maxlen,
                  uint32_t timeout_ms);
void net_tcp_close(tcp_conn_t* conn);

/* HTTP GET (простой, без TLS) */
int net_http_get(const char* host, uint16_t port,
                 const char* path, char* response_buf,
                 uint32_t buf_size);

/* HTTPS — HTTP через TLS */
int net_https_get(const char* host, uint16_t port,
                  const char* path, char* response_buf,
                  uint32_t buf_size);

/* TLS connection handle */
typedef struct {
    tcp_conn_t*     tcp;
    uint8_t         session_key[32];
    uint8_t         mac_key[32];
    uint8_t         iv[16];
    uint32_t        seq;
    bool            valid;
} tls_conn_t;

tls_conn_t* net_tls_connect(ip4_addr_t ip, uint16_t port, const char* hostname);
int  net_tls_send(tls_conn_t* tls, const void* data, uint32_t len);
int  net_tls_recv(tls_conn_t* tls, void* buf, uint32_t maxlen);
void net_tls_close(tls_conn_t* tls);

/* Обработка входящих пакетов (polling) */
void net_poll(void);

/* Утилиты */
void net_dump_ip(ip4_addr_t ip);
uint16_t net_checksum(const void* data, uint32_t len);

/* htons/ntohs для big-endian конверсии */
static inline uint16_t htons(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}
static inline uint32_t htonl(uint32_t x) {
    return ((x >> 24) & 0xFF) | (((x >> 16) & 0xFF) << 8) |
           (((x >> 8) & 0xFF) << 16) | ((x & 0xFF) << 24);
}
#define ntohs htons
#define ntohl htonl

#endif /* NET_H */
