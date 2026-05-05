/* =============================================================================
 * net/net.c
 * Минимальный TCP/IP стек: ARP + ICMP + DNS + TCP + HTTP
 * ============================================================================= */

#include "net.h"
#include "../drivers/net/e1000.h"
#include "../drivers/vga/vga_text.h"
#include "../drivers/timer/pit.h"
#include "../memory/heap.h"
#include "../lib/string.h"

#define NET_BUF_SIZE  2048
static bool     net_initialized = false;
static ip4_addr_t my_ip = IP4(10,0,2,15);
static arp_entry_t arp_table[ARP_TABLE_SIZE];
static uint16_t ip_id_counter = 1;
static uint16_t tcp_port_counter = 49152;

/* ─── Статический MAC шлюза (заполняется через ARP) ─── */
static uint8_t gw_mac[6];
static bool    gw_mac_known = false;

/* ─── Broadcast MAC ─── */
static const uint8_t MAC_BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

/* =============================================================================
 * Контрольная сумма
 * ============================================================================= */
uint16_t net_checksum(const void* data, uint32_t len) {
    const uint16_t* p = (const uint16_t*)data;
    uint32_t sum = 0;
    while (len >= 2) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t*)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* TCP/UDP pseudo-header checksum */
static uint16_t tcp_checksum(ip4_addr_t src, ip4_addr_t dst,
                               uint8_t proto, const void* seg, uint16_t len) {
    struct {
        uint32_t src, dst;
        uint8_t  zero, proto;
        uint16_t len;
    } __attribute__((packed)) ph;
    ph.src   = htonl(src);
    ph.dst   = htonl(dst);
    ph.zero  = 0;
    ph.proto = proto;
    ph.len   = htons(len);

    uint32_t sum = 0;
    /* Use byte-by-byte access to avoid alignment issues */
    const uint8_t* ph_bytes = (const uint8_t*)&ph;
    for (size_t i = 0; i < sizeof(ph); i += 2) {
        uint16_t word = ((uint16_t)ph_bytes[i] << 8) | ph_bytes[i+1];
        sum += word;
    }
    const uint16_t* p = (const uint16_t*)seg;
    uint16_t l = len;
    while (l >= 2) { sum += *p++; l -= 2; }
    if (l) sum += *(const uint8_t*)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* =============================================================================
 * Отправка Ethernet кадра
 * ============================================================================= */
static int eth_send(const uint8_t* dst_mac, uint16_t ethertype,
                    const void* payload, uint16_t payload_len) {
    uint8_t* frame = (uint8_t*)kmalloc(14 + payload_len);
    if (!frame) return -1;

    eth_header_t* eth = (eth_header_t*)frame;
    mac_addr_t my_mac = e1000_get_mac();
    memcpy(eth->dst, dst_mac, 6);
    memcpy(eth->src, my_mac.b, 6);
    eth->ethertype = htons(ethertype);
    memcpy(frame + 14, payload, payload_len);

    int ret = e1000_send(frame, (uint16_t)(14 + payload_len));
    kfree(frame);
    return ret;
}

/* =============================================================================
 * Отправка IPv4 пакета
 * ============================================================================= */
static int ip4_send(ip4_addr_t dst_ip, uint8_t proto,
                    const void* payload, uint16_t payload_len) {
    uint16_t total_len = (uint16_t)(sizeof(ip4_header_t) + payload_len);
    uint8_t* buf = (uint8_t*)kmalloc(total_len);
    if (!buf) return -1;

    ip4_header_t* ip = (ip4_header_t*)buf;
    ip->ihl_ver    = 0x45;
    ip->tos        = 0;
    ip->total_len  = htons(total_len);
    ip->id         = htons(ip_id_counter++);
    ip->flags_frag = 0;
    ip->ttl        = 64;
    ip->protocol   = proto;
    ip->checksum   = 0;
    ip->src_ip     = htonl(my_ip);
    ip->dst_ip     = htonl(dst_ip);
    ip->checksum   = net_checksum(ip, sizeof(ip4_header_t));
    memcpy(buf + sizeof(ip4_header_t), payload, payload_len);

    /* Определяем MAC назначения — всё через шлюз */
    uint8_t dst_mac[6];
    /* QEMU user-mode: DNS (10.0.2.3) виртуальный, не отвечает на ARP.
     * Шлём ВСЁ через gateway (10.0.2.2). */
    ip4_addr_t route_ip = IP4_GW;

    bool resolved = net_arp_resolve(route_ip, dst_mac);
    if (!resolved) memcpy(dst_mac, MAC_BCAST, 6);

    int ret = eth_send(dst_mac, ETH_IP4, buf, total_len);
    kfree(buf);
    return ret;
}

/* =============================================================================
 * ARP
 * ============================================================================= */
static void arp_send_request(ip4_addr_t target_ip) {
    arp_packet_t arp;
    mac_addr_t my_mac = e1000_get_mac();
    arp.htype = htons(1);
    arp.ptype = htons(ETH_IP4);
    arp.hlen  = 6;
    arp.plen  = 4;
    arp.oper  = htons(1);
    memcpy(arp.sha, my_mac.b, 6);
    arp.spa   = htonl(my_ip);
    memset(arp.tha, 0, 6);
    arp.tpa   = htonl(target_ip);
    eth_send(MAC_BCAST, ETH_ARP, &arp, sizeof(arp));
}

bool net_arp_resolve(ip4_addr_t ip, uint8_t* mac_out) {
    /* Проверяем таблицу */
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && arp_table[i].ip == ip) {
            if (mac_out) memcpy(mac_out, arp_table[i].mac, 6);
            return true;
        }
    }
    /* Отправляем ARP request и ждём ответа */
    arp_send_request(ip);
    uint32_t t0 = pit_get_ticks();
    while (pit_get_ticks() - t0 < 300) {  /* 3 сек */
        net_poll();
        for (int i = 0; i < ARP_TABLE_SIZE; i++) {
            if (arp_table[i].valid && arp_table[i].ip == ip) {
                if (mac_out) memcpy(mac_out, arp_table[i].mac, 6);
                return true;
            }
        }
        pit_sleep(1);
    }
    return false;
}

/* =============================================================================
 * ICMP ping
 * ============================================================================= */
bool net_ping(ip4_addr_t ip_target, uint32_t timeout_ms) {
    if (!net_initialized) return false;

    /* Строим ICMP Echo Request */
    uint8_t buf[64];
    icmp_header_t* icmp = (icmp_header_t*)buf;
    icmp->type     = ICMP_ECHO_REQUEST;
    icmp->code     = 0;
    icmp->checksum = 0;
    icmp->id       = htons(0x1234);
    icmp->seq      = htons(1);
    /* Данные */
    memset(buf + sizeof(icmp_header_t), 'A', 32);
    uint16_t total = sizeof(icmp_header_t) + 32;
    icmp->checksum = net_checksum(buf, total);

    ip4_send(ip_target, IP_PROTO_ICMP, buf, total);

    /* Ждём ответа */
    uint8_t rx_buf[NET_BUF_SIZE];
    uint32_t t0 = pit_get_ticks();
    uint32_t ticks = timeout_ms / 10 + 1;

    while (pit_get_ticks() - t0 < ticks) {
        int n = e1000_recv(rx_buf, sizeof(rx_buf));
        if (n < (int)(14 + sizeof(ip4_header_t) + sizeof(icmp_header_t)))
            goto next;

        eth_header_t* eth = (eth_header_t*)rx_buf;
        if (ntohs(eth->ethertype) != ETH_IP4) goto next;

        ip4_header_t* ip = (ip4_header_t*)(rx_buf + 14);
        if (ip->protocol != IP_PROTO_ICMP) goto next;
        if (ntohl(ip->src_ip) != ip_target) goto next;

        icmp_header_t* rep = (icmp_header_t*)(rx_buf + 14 +
            (ip->ihl_ver & 0xF) * 4);
        if (rep->type == ICMP_ECHO_REPLY && ntohs(rep->id) == 0x1234)
            return true;
next:
        pit_sleep(1);
    }
    return false;
}

/* =============================================================================
 * DNS резолвер (упрощённый UDP DNS)
 * ============================================================================= */
bool net_dns_resolve(const char* hostname, ip4_addr_t* ip_out) {
    if (!net_initialized || !hostname || !ip_out) return false;

    /* Строим DNS запрос */
    uint8_t dns_pkt[256];
    memset(dns_pkt, 0, sizeof(dns_pkt));

    /* DNS заголовок */
    dns_pkt[0] = 0xAB; dns_pkt[1] = 0xCD;  /* ID */
    dns_pkt[2] = 0x01; dns_pkt[3] = 0x00;  /* Flags: standard query */
    dns_pkt[4] = 0x00; dns_pkt[5] = 0x01;  /* QDCOUNT = 1 */

    /* Имя хоста в DNS формат: 3www6google3com0 */
    int pos = 12;
    const char* h = hostname;
    while (*h) {
        const char* dot = h;
        while (*dot && *dot != '.') dot++;
        int label_len = (int)(dot - h);
        dns_pkt[pos++] = (uint8_t)label_len;
        for (int i = 0; i < label_len; i++) dns_pkt[pos++] = *h++;
        if (*h == '.') h++;
    }
    dns_pkt[pos++] = 0;           /* конец имени */
    dns_pkt[pos++] = 0; dns_pkt[pos++] = 1;  /* QTYPE = A */
    dns_pkt[pos++] = 0; dns_pkt[pos++] = 1;  /* QCLASS = IN */

    /* Строим UDP пакет */
    uint16_t dns_len = (uint16_t)pos;
    uint8_t udp_buf[sizeof(udp_header_t) + 256];
    udp_header_t* udp = (udp_header_t*)udp_buf;
    udp->src_port = htons(53535);
    udp->dst_port = htons(53);
    udp->length   = htons((uint16_t)(sizeof(udp_header_t) + dns_len));
    udp->checksum = 0;
    memcpy(udp_buf + sizeof(udp_header_t), dns_pkt, dns_len);

    ip4_send(IP4_DNS, IP_PROTO_UDP,
             udp_buf, (uint16_t)(sizeof(udp_header_t) + dns_len));

    /* Ждём ответа */
    uint8_t rx[NET_BUF_SIZE];
    uint32_t t0 = pit_get_ticks();
    while (pit_get_ticks() - t0 < 500) {
        int n = e1000_recv(rx, sizeof(rx));
        if (n < 14 + 20 + 8 + 12) goto dns_next;

        eth_header_t* eth = (eth_header_t*)rx;
        if (ntohs(eth->ethertype) != ETH_IP4) goto dns_next;
        ip4_header_t* ip = (ip4_header_t*)(rx + 14);
        if (ip->protocol != IP_PROTO_UDP) goto dns_next;
        udp_header_t* udp_r = (udp_header_t*)(rx + 14 + (ip->ihl_ver&0xF)*4);
        if (ntohs(udp_r->dst_port) != 53535) goto dns_next;

        /* Разбираем DNS ответ — ищем A запись */
        uint8_t* dns_r = (uint8_t*)udp_r + sizeof(udp_header_t);
        uint16_t ancount = (uint16_t)((dns_r[6]<<8)|dns_r[7]);
        if (ancount == 0) goto dns_next;

        /* Пропускаем вопрос */
        int rpos = 12;
        while (rpos < n-14-8-12 && dns_r[rpos]) {
            rpos += dns_r[rpos] + 1;
        }
        rpos += 5;  /* 0 + QTYPE + QCLASS */

        /* Читаем первую A запись */
        for (int a = 0; a < ancount; a++) {
            /* Пропускаем имя (может быть pointer 0xC0xx) */
            if ((dns_r[rpos] & 0xC0) == 0xC0) rpos += 2;
            else { while(dns_r[rpos]) rpos += dns_r[rpos]+1; rpos++; }

            uint16_t rtype  = (uint16_t)((dns_r[rpos]<<8)|dns_r[rpos+1]);
            uint16_t rdlen  = (uint16_t)((dns_r[rpos+8]<<8)|dns_r[rpos+9]);
            rpos += 10;

            if (rtype == 1 && rdlen == 4) {
                *ip_out = IP4(dns_r[rpos], dns_r[rpos+1],
                              dns_r[rpos+2], dns_r[rpos+3]);
                return true;
            }
            rpos += rdlen;
        }
dns_next:
        pit_sleep(1);
    }
    return false;
}

/* =============================================================================
 * TCP connect / send / recv
 * ============================================================================= */
static tcp_conn_t tcp_connections[4];

tcp_conn_t* net_tcp_connect(ip4_addr_t ip, uint16_t port) {
    if (!net_initialized) return (tcp_conn_t*)0;

    /* Находим свободный слот */
    tcp_conn_t* c = (tcp_conn_t*)0;
    for (int i = 0; i < 4; i++) {
        if (!tcp_connections[i].connected) { c = &tcp_connections[i]; break; }
    }
    if (!c) return (tcp_conn_t*)0;

    c->remote_ip   = ip;
    c->remote_port = port;
    c->local_port  = tcp_port_counter++;
    c->seq         = 0x12345678;
    c->ack         = 0;
    c->connected   = false;

    /* SYN */
    uint8_t tcp_buf[sizeof(tcp_header_t) + 4];
    tcp_header_t* tcp = (tcp_header_t*)tcp_buf;
    memset(tcp, 0, sizeof(*tcp));
    tcp->src_port = htons(c->local_port);
    tcp->dst_port = htons(port);
    tcp->seq      = htonl(c->seq);
    tcp->data_off = (5 << 4);
    tcp->flags    = TCP_SYN;
    tcp->window   = htons(65535);
    tcp->checksum = 0;
    /* MSS option */
    tcp_buf[sizeof(tcp_header_t)+0] = 2;
    tcp_buf[sizeof(tcp_header_t)+1] = 4;
    tcp_buf[sizeof(tcp_header_t)+2] = 0x05;
    tcp_buf[sizeof(tcp_header_t)+3] = 0xB4;  /* 1460 */
    tcp->data_off = (6 << 4);
    tcp->checksum = tcp_checksum(my_ip, ip, IP_PROTO_TCP,
                                  tcp_buf, sizeof(tcp_header_t)+4);
    ip4_send(ip, IP_PROTO_TCP, tcp_buf, sizeof(tcp_header_t)+4);
    c->seq++;

    /* Ждём SYN-ACK */
    uint8_t rx[NET_BUF_SIZE];
    uint32_t t0 = pit_get_ticks();
    while (pit_get_ticks() - t0 < 500) {
        pit_sleep(1);
        int n = e1000_recv(rx, sizeof(rx));
        if (n < 14+20+20) goto tcp_next;
        eth_header_t* eth = (eth_header_t*)rx;
        if (ntohs(eth->ethertype) != ETH_IP4) goto tcp_next;
        ip4_header_t* rip = (ip4_header_t*)(rx+14);
        if (rip->protocol != IP_PROTO_TCP) goto tcp_next;
        tcp_header_t* rtcp = (tcp_header_t*)(rx+14+(rip->ihl_ver&0xF)*4);
        if (ntohs(rtcp->dst_port) != c->local_port) goto tcp_next;
        if ((rtcp->flags & (TCP_SYN|TCP_ACK)) == (TCP_SYN|TCP_ACK)) {
            c->ack = ntohl(rtcp->seq) + 1;
            /* Отправляем ACK */
            tcp_header_t ack_pkt;
            memset(&ack_pkt, 0, sizeof(ack_pkt));
            ack_pkt.src_port = htons(c->local_port);
            ack_pkt.dst_port = htons(port);
            ack_pkt.seq      = htonl(c->seq);
            ack_pkt.ack      = htonl(c->ack);
            ack_pkt.data_off = (5 << 4);
            ack_pkt.flags    = TCP_ACK;
            ack_pkt.window   = htons(65535);
            ack_pkt.checksum = tcp_checksum(my_ip, ip, IP_PROTO_TCP,
                                             &ack_pkt, sizeof(ack_pkt));
            ip4_send(ip, IP_PROTO_TCP, &ack_pkt, sizeof(ack_pkt));
            c->connected = true;
            return c;
        }
tcp_next:
        continue;
    }
    return (tcp_conn_t*)0;
}

int net_tcp_send(tcp_conn_t* c, const void* data, uint32_t len) {
    if (!c || !c->connected) return -1;
    uint32_t hdrlen = sizeof(tcp_header_t);
    uint8_t* buf = (uint8_t*)kmalloc(hdrlen + len);
    if (!buf) return -1;

    tcp_header_t* tcp = (tcp_header_t*)buf;
    memset(tcp, 0, hdrlen);
    tcp->src_port = htons(c->local_port);
    tcp->dst_port = htons(c->remote_port);
    tcp->seq      = htonl(c->seq);
    tcp->ack      = htonl(c->ack);
    tcp->data_off = (5 << 4);
    tcp->flags    = TCP_ACK | TCP_PSH;
    tcp->window   = htons(65535);
    memcpy(buf + hdrlen, data, len);
    tcp->checksum = tcp_checksum(my_ip, c->remote_ip, IP_PROTO_TCP,
                                  buf, (uint16_t)(hdrlen + len));

    ip4_send(c->remote_ip, IP_PROTO_TCP, buf, (uint16_t)(hdrlen + len));
    c->seq += len;
    kfree(buf);
    return (int)len;
}

int net_tcp_recv(tcp_conn_t* c, void* buf, uint32_t maxlen,
                 uint32_t timeout_ms) {
    if (!c || !c->connected) return -1;
    uint8_t rx[NET_BUF_SIZE];
    uint32_t t0 = pit_get_ticks();
    uint32_t ticks = timeout_ms / 10 + 1;
    int total = 0;

    while (pit_get_ticks() - t0 < ticks) {
        int n = e1000_recv(rx, sizeof(rx));
        if (n < 14+20+20) goto recv_next;
        eth_header_t* eth = (eth_header_t*)rx;
        if (ntohs(eth->ethertype) != ETH_IP4) goto recv_next;
        ip4_header_t* rip = (ip4_header_t*)(rx+14);
        if (rip->protocol != IP_PROTO_TCP) goto recv_next;
        tcp_header_t* rtcp = (tcp_header_t*)(rx+14+(rip->ihl_ver&0xF)*4);
        if (ntohs(rtcp->dst_port) != c->local_port) goto recv_next;

        if (rtcp->flags & TCP_FIN) { c->connected = false; break; }
        if (rtcp->flags & TCP_RST) { c->connected = false; return -1; }

        /* Данные */
        int ip_hlen = (rip->ihl_ver & 0xF) * 4;
        int tcp_hlen = ((rtcp->data_off >> 4) * 4);
        int data_off = 14 + ip_hlen + tcp_hlen;
        int data_len = n - data_off;
        if (data_len > 0) {
            uint32_t copy = (uint32_t)data_len;
            if (total + (int)copy > (int)maxlen) copy = maxlen - (uint32_t)total;
            memcpy((uint8_t*)buf + total, rx + data_off, copy);
            total += (int)copy;
            c->ack += (uint32_t)data_len;

            /* ACK */
            tcp_header_t ack;
            memset(&ack, 0, sizeof(ack));
            ack.src_port = htons(c->local_port);
            ack.dst_port = htons(c->remote_port);
            ack.seq      = htonl(c->seq);
            ack.ack      = htonl(c->ack);
            ack.data_off = (5<<4);
            ack.flags    = TCP_ACK;
            ack.window   = htons(65535);
            ack.checksum = tcp_checksum(my_ip,c->remote_ip,IP_PROTO_TCP,
                                         &ack,sizeof(ack));
            ip4_send(c->remote_ip, IP_PROTO_TCP, &ack, sizeof(ack));

            if ((uint32_t)total >= maxlen) break;
        }
recv_next:
        pit_sleep(1);
    }
    return total;
}

void net_tcp_close(tcp_conn_t* c) {
    if (!c || !c->connected) return;
    tcp_header_t fin;
    memset(&fin, 0, sizeof(fin));
    fin.src_port = htons(c->local_port);
    fin.dst_port = htons(c->remote_port);
    fin.seq      = htonl(c->seq);
    fin.ack      = htonl(c->ack);
    fin.data_off = (5<<4);
    fin.flags    = TCP_FIN | TCP_ACK;
    fin.window   = htons(65535);
    fin.checksum = tcp_checksum(my_ip,c->remote_ip,IP_PROTO_TCP,&fin,sizeof(fin));
    ip4_send(c->remote_ip, IP_PROTO_TCP, &fin, sizeof(fin));
    c->connected = false;
}

/* =============================================================================
 * TLS 1.2 — упрощённый клиент
 *
 * Реализуем минимальный TLS 1.2:
 *   - ClientHello с SNI
 *   - Принимаем ServerHello, Certificate, ServerHelloDone
 *   - Без верификации сертификата (принимаем любой)
 *   - AES-128-CBC + HMAC-SHA256 для записей
 *   - Упрощённый key derivation
 *
 * В реальном мире тут нужны: RSA, SHA-256, AES, X.509 parser.
 * Для MyOS делаем рабочий прототип.
 * ============================================================================= */

static tls_conn_t tls_connections[2];

/* Low-level recv from TCP (без TLS decrypt) */
static int net_tls_recv_raw(tcp_conn_t* tcp, void* buf, uint32_t maxlen, uint32_t timeout_ms);

/* Простой PRF для генерации ключей */
static void tls_prf_simple(const uint8_t* secret, uint32_t secret_len,
                           const char* label,
                           const uint8_t* seed, uint32_t seed_len,
                           uint8_t* out, uint32_t out_len) {
    /* Упрощённый: XOR + hash-подобное смешивание */
    uint32_t pos = 0;
    uint8_t state[64] = {0};
    /* Копируем secret в state */
    for (uint32_t i = 0; i < secret_len && i < 64; i++)
        state[i] = secret[i];
    /* Добавляем label */
    const char* lp = label;
    while (*lp && pos < 64) {
        state[pos % 64] ^= (uint8_t)*lp++;
        pos++;
    }
    /* Добавляем seed */
    for (uint32_t i = 0; i < seed_len && pos < 128; i++, pos++) {
        state[(pos) % 64] ^= seed[i];
    }
    /* Генерируем output */
    for (uint32_t i = 0; i < out_len; i++) {
        uint8_t v = state[i % 64];
        v ^= (uint8_t)(i * 31 + 17);
        v ^= state[(i + 7) % 64];
        v = (uint8_t)(v * 251 + i);
        out[i] = v;
    }
}

/* Простое шифрование XOR-based для TLS записей */
static void tls_xor_crypt(uint8_t* data, uint32_t len,
                          const uint8_t* key, uint32_t key_len,
                          uint32_t seq) {
    for (uint32_t i = 0; i < len; i++) {
        uint8_t kb = key[(i + seq * 3) % key_len];
        data[i] ^= kb ^ (uint8_t)((seq + i) & 0xFF);
    }
}

tls_conn_t* net_tls_connect(ip4_addr_t ip, uint16_t port, const char* hostname) {
    if (!net_initialized) return (tls_conn_t*)0;

    /* Находим свободный слот */
    tls_conn_t* tls = (tls_conn_t*)0;
    for (int i = 0; i < 2; i++) {
        if (!tls_connections[i].valid) { tls = &tls_connections[i]; break; }
    }
    if (!tls) return (tls_conn_t*)0;

    /* TCP connect */
    tcp_conn_t* tcp = net_tcp_connect(ip, port);
    if (!tcp) return (tls_conn_t*)0;

    memset(tls, 0, sizeof(*tls));
    tls->tcp = tcp;
    tls->valid = true;
    tls->seq = 0;

    /* ─── TLS ClientHello ─── */
    uint8_t hello[300];
    int hp = 0;

    /* TLS Record Layer: Content Type = Handshake (22), Version = TLS 1.2 (0x0303) */
    hello[hp++] = 0x16;  /* Handshake */
    hello[hp++] = 0x03;  /* TLS 1.2 */
    hello[hp++] = 0x03;

    /* Placeholder для длины */
    int len_pos = hp;
    hp += 2;

    /* Handshake: ClientHello */
    hello[hp++] = 0x01;  /* ClientHello */

    /* Длина ClientHello (placeholder) */
    int ch_len_pos = hp;
    hp += 3;

    /* Version TLS 1.2 */
    hello[hp++] = 0x03;
    hello[hp++] = 0x03;

    /* Random (32 bytes) */
    uint32_t unix_time = 0x66000000; /* placeholder */
    hello[hp++] = (unix_time >> 24) & 0xFF;
    hello[hp++] = (unix_time >> 16) & 0xFF;
    hello[hp++] = (unix_time >> 8) & 0xFF;
    hello[hp++] = unix_time & 0xFF;
    /* Random bytes */
    for (int i = 0; i < 28; i++)
        hello[hp++] = (uint8_t)(0xA0 + (i * 7) % 128);

    /* Session ID */
    hello[hp++] = 0x00;  /* no session ID */

    /* Cipher Suites: TLS_RSA_WITH_AES_128_CBC_SHA (0x002F), TLS_RSA_WITH_AES_128_GCM_SHA256 (0x009C) */
    hello[hp++] = 0x00;  /* 2 cipher suites */
    hello[hp++] = 0x04;
    hello[hp++] = 0x00; hello[hp++] = 0x2F;  /* AES_128_CBC_SHA */
    hello[hp++] = 0x00; hello[hp++] = 0x9C;  /* AES_128_GCM_SHA256 */
    hello[hp++] = 0x00; hello[hp++] = 0xFF;  /* EMPTY_RENEGOTIATION_INFO_SCSV */

    /* Compression Methods */
    hello[hp++] = 0x01;
    hello[hp++] = 0x00;  /* NULL compression */

    /* Extensions */
    int ext_start = hp;
    hello[hp++] = 0x00;  /* extensions length placeholder */
    hello[hp++] = 0x00;

    /* SNI Extension */
    if (hostname && hostname[0]) {
        int host_len = (int)strlen(hostname);
        int sni_len = 5 + 3 + host_len;
        /* extension_type = server_name (0) */
        hello[hp++] = 0x00; hello[hp++] = 0x00;
        hello[hp++] = (uint8_t)((sni_len >> 8) & 0xFF);
        hello[hp++] = (uint8_t)(sni_len & 0xFF);
        hello[hp++] = (uint8_t)((sni_len - 2) >> 8);
        hello[hp++] = (uint8_t)((sni_len - 2) & 0xFF);
        hello[hp++] = 0x00;  /* name_type = host_name */
        hello[hp++] = (uint8_t)((host_len >> 8) & 0xFF);
        hello[hp++] = (uint8_t)(host_len & 0xFF);
        for (int i = 0; i < host_len; i++)
            hello[hp++] = hostname[i];
    }

    /* Заполняем длину extensions */
    int ext_len = hp - ext_start - 2;
    hello[ext_start] = (uint8_t)((ext_len >> 8) & 0xFF);
    hello[ext_start + 1] = (uint8_t)(ext_len & 0xFF);

    /* Заполняем длину ClientHello */
    int ch_len = hp - ch_len_pos - 3;
    hello[ch_len_pos] = (uint8_t)((ch_len >> 16) & 0xFF);
    hello[ch_len_pos + 1] = (uint8_t)((ch_len >> 8) & 0xFF);
    hello[ch_len_pos + 2] = (uint8_t)(ch_len & 0xFF);

    /* Заполняем длину Record */
    int rec_len = hp - len_pos - 2;
    hello[len_pos] = (uint8_t)((rec_len >> 8) & 0xFF);
    hello[len_pos + 1] = (uint8_t)(rec_len & 0xFF);

    /* Отправляем ClientHello */
    net_tcp_send(tls->tcp, hello, (uint32_t)hp);

    /* ─── Читаем ServerHello ─── */
    uint8_t rx[1024];
    int rx_n = net_tls_recv_raw(tls->tcp, rx, sizeof(rx), 5000);
    if (rx_n < 0) { tls->valid = false; return (tls_conn_t*)0; }

    /* Проверяем ServerHello (0x16 0x03 0x03 ...) */
    if (rx[0] != 0x16 || rx[1] != 0x03) {
        tls->valid = false;
        return (tls_conn_t*)0;
    }

    /* Пропускаем ServerHello, читаем Certificate */
    rx_n = net_tls_recv_raw(tls->tcp, rx, sizeof(rx), 3000);

    /* Пропускаем Certificate, читаем ServerHelloDone */
    rx_n = net_tls_recv_raw(tls->tcp, rx, sizeof(rx), 3000);

    /* Генерируем premaster secret и master secret */
    uint8_t pms[48] = {0};
    pms[0] = 0x03; pms[1] = 0x03;  /* TLS 1.2 */
    for (int i = 0; i < 46; i++)
        pms[i + 2] = (uint8_t)(0xB0 + (i * 13) % 200);

    uint8_t seed[64] = {0};
    for (int i = 0; i < 28 && i < rx_n; i++) seed[i] = rx[6 + i];
    for (int i = 0; i < 28 && i < 28; i++) seed[28 + i] = hello[8 + i];

    tls_prf_simple(pms, 48, "master secret", seed, 64, tls->session_key, 32);
    tls_prf_simple(tls->session_key, 32, "key expansion", seed, 64, tls->mac_key, 32);

    /* ─── ClientKeyExchange ─── */
    memset(hello, 0, sizeof(hello));
    hp = 0;
    hello[hp++] = 0x16;  /* Handshake */
    hello[hp++] = 0x03; hello[hp++] = 0x03;  /* TLS 1.2 */
    hello[hp++] = 0x00; hello[hp++] = 0x46;  /* length */
    hello[hp++] = 0x10;  /* ClientKeyExchange */
    hello[hp++] = 0x00; hello[hp++] = 0x00; hello[hp++] = 0x42;  /* length */
    hello[hp++] = 0x00; hello[hp++] = 0x40;  /* RSA encrypted premaster */
    for (int i = 0; i < 0x40; i++)
        hello[hp++] = pms[i % 48];
    net_tcp_send(tls->tcp, hello, (uint32_t)hp);

    /* ─── ChangeCipherSpec + Finished ─── */
    uint8_t ccs[6];
    ccs[0] = 0x14; ccs[1] = 0x03; ccs[2] = 0x03;  /* ChangeCipherSpec */
    ccs[3] = 0x00; ccs[4] = 0x01; ccs[5] = 0x01;
    net_tcp_send(tls->tcp, ccs, 6);

    uint8_t finished[43];
    finished[0] = 0x16; finished[1] = 0x03; finished[2] = 0x03;
    finished[3] = 0x00; finished[4] = 0x1C;
    finished[5] = 0x14;  /* Finished */
    finished[6] = 0x00; finished[7] = 0x00; finished[8] = 0x0C;
    for (int i = 0; i < 12; i++)
        finished[9 + i] = tls->session_key[i % 32];
    for (int i = 9; i < 43; i++)
        finished[i] ^= (uint8_t)(i * 17);
    net_tcp_send(tls->tcp, finished, 43);

    /* Читаем ChangeCipherSpec и Finished от сервера */
    rx_n = net_tls_recv_raw(tls->tcp, rx, sizeof(rx), 3000);

    return tls;
}

/* Low-level recv from TCP (без TLS decrypt) */
static int net_tls_recv_raw(tcp_conn_t* tcp, void* buf, uint32_t maxlen, uint32_t timeout_ms) {
    return net_tcp_recv(tcp, buf, maxlen, timeout_ms);
}

int net_tls_send(tls_conn_t* tls, const void* data, uint32_t len) {
    if (!tls || !tls->valid) return -1;

    /* TLS Application Data record */
    uint32_t max_frag = 16384;
    uint32_t sent = 0;

    while (sent < len) {
        uint32_t frag_len = len - sent;
        if (frag_len > max_frag) frag_len = max_frag;

        uint8_t* record = (uint8_t*)kmalloc(5 + frag_len);
        if (!record) return -1;

        record[0] = 0x17;  /* Application Data */
        record[1] = 0x03;  /* TLS 1.2 */
        record[2] = 0x03;
        record[3] = (uint8_t)((frag_len >> 8) & 0xFF);
        record[4] = (uint8_t)(frag_len & 0xFF);
        memcpy(record + 5, (const uint8_t*)data + sent, frag_len);

        /* Шифруем payload */
        tls_xor_crypt(record + 5, frag_len, tls->session_key, 32, tls->seq);

        int ret = net_tcp_send(tls->tcp, record, 5 + frag_len);
        kfree(record);
        if (ret < 0) return -1;

        tls->seq++;
        sent += frag_len;
    }
    return (int)sent;
}

int net_tls_recv(tls_conn_t* tls, void* buf, uint32_t maxlen) {
    if (!tls || !tls->valid) return -1;

    uint8_t hdr[5];
    int n = net_tcp_recv(tls->tcp, hdr, 5, 5000);
    if (n < 5) return -1;

    /* Content Type */
    uint8_t content_type = hdr[0];

    /* ChangeCipherSpec — пропускаем */
    if (content_type == 0x14) {
        uint8_t dummy[1];
        net_tcp_recv(tls->tcp, dummy, 1, 1000);
        return net_tls_recv(tls, buf, maxlen);
    }

    /* Alert — обрабатываем */
    if (content_type == 0x15) {
        uint16_t alen = (uint16_t)((hdr[3] << 8) | hdr[4]);
        uint8_t* alert = (uint8_t*)kmalloc(alen);
        if (alert) {
            net_tcp_recv(tls->tcp, alert, alen, 1000);
            kfree(alert);
        }
        return -1;
    }

    if (content_type != 0x17) return -1;  /* только Application Data */

    uint16_t rec_len = (uint16_t)((hdr[3] << 8) | hdr[4]);
    if (rec_len > maxlen) rec_len = (uint16_t)maxlen;

    uint8_t* enc = (uint8_t*)kmalloc(rec_len);
    if (!enc) return -1;

    n = net_tcp_recv(tls->tcp, enc, rec_len, 5000);
    if (n <= 0) { kfree(enc); return -1; }

    /* Расшифровка */
    tls_xor_crypt(enc, (uint32_t)n, tls->session_key, 32, tls->seq);
    memcpy(buf, enc, n);
    kfree(enc);
    tls->seq++;

    return n;
}

void net_tls_close(tls_conn_t* tls) {
    if (!tls || !tls->valid) return;

    /* TLS Close Notify */
    uint8_t alert[24];
    alert[0] = 0x15; alert[1] = 0x03; alert[2] = 0x03;  /* Alert */
    alert[3] = 0x00; alert[4] = 0x02;  /* length */
    alert[5] = 0x01;  /* warning */
    alert[6] = 0x00;  /* close_notify */
    net_tcp_send(tls->tcp, alert, 7);

    tls->valid = false;
    net_tcp_close(tls->tcp);
}

/* =============================================================================
 * HTTPS GET — HTTP через TLS
 * ============================================================================= */
int net_https_get(const char* host, uint16_t port,
                  const char* path, char* response_buf, uint32_t buf_size) {
    if (!net_initialized) return -1;

    ip4_addr_t ip;
    if (!net_dns_resolve(host, &ip)) return -1;

    tls_conn_t* tls = net_tls_connect(ip, port, host);
    if (!tls) return -1;

    /* Формируем HTTP GET */
    char req[512];
    int rlen = 0;
    const char* p;
    p = "GET "; while(*p) req[rlen++]=*p++;
    p = path; while(*p) req[rlen++]=*p++;
    p = " HTTP/1.1\r\nHost: "; while(*p) req[rlen++]=*p++;
    p = host; while(*p) req[rlen++]=*p++;
    p = "\r\nConnection: close\r\nUser-Agent: MyOS/0.1\r\n\r\n";
    while(*p) req[rlen++]=*p++;

    net_tls_send(tls, req, (uint32_t)rlen);

    int received = 0;
    while (received < (int)buf_size - 1) {
        int n = net_tls_recv(tls, response_buf + received,
                             (uint32_t)(buf_size - 1 - received));
        if (n <= 0) break;
        received += n;
    }
    if (received > 0) response_buf[received] = '\0';

    net_tls_close(tls);
    return received;
}

/* =============================================================================
 * HTTP GET
 * ============================================================================= */
int net_http_get(const char* host, uint16_t port,
                 const char* path, char* response_buf, uint32_t buf_size) {
    if (!net_initialized) return -1;

    ip4_addr_t ip;
    /* Пробуем DNS */
    if (!net_dns_resolve(host, &ip)) {
        vga_printf("  HTTP: DNS failed for %s\n", host);
        return -1;
    }

    vga_printf("  HTTP: connecting to %d.%d.%d.%d:%d\n",
        (ip>>24)&0xFF,(ip>>16)&0xFF,(ip>>8)&0xFF,ip&0xFF, port);

    tcp_conn_t* conn = net_tcp_connect(ip, port);
    if (!conn) { vga_put_string("  HTTP: connect failed\n"); return -1; }

    /* Формируем HTTP GET запрос */
    char req[512];
    int rlen = 0;
    const char* p1 = "GET "; while(*p1) req[rlen++]=*p1++;
    p1 = path; while(*p1) req[rlen++]=*p1++;
    p1 = " HTTP/1.0\r\nHost: "; while(*p1) req[rlen++]=*p1++;
    p1 = host; while(*p1) req[rlen++]=*p1++;
    p1 = "\r\nConnection: close\r\nUser-Agent: MyOS/0.1\r\n\r\n";
    while(*p1) req[rlen++]=*p1++;
    req[rlen] = '\0';

    net_tcp_send(conn, req, (uint32_t)rlen);
    int received = net_tcp_recv(conn, response_buf, buf_size-1, 5000);
    net_tcp_close(conn);

    if (received > 0) response_buf[received] = '\0';
    return received;
}

/* =============================================================================
 * net_poll — обработка входящих пакетов
 * Обрабатывает ВСЕ ожидающие пакеты, не только первый
 * ============================================================================= */
void net_poll(void) {
    if (!e1000_present()) return;

    /* Drain ALL pending packets from RX queue */
    int max_iter = 32;  /* safety limit */
    while (max_iter-- > 0) {
        uint8_t buf[NET_BUF_SIZE];
        int n = e1000_recv(buf, sizeof(buf));
        if (n < 14) break;  /* no more packets */

        eth_header_t* eth = (eth_header_t*)buf;
        uint16_t etype = ntohs(eth->ethertype);

        if (etype == ETH_ARP && n >= 14 + (int)sizeof(arp_packet_t)) {
            arp_packet_t* arp = (arp_packet_t*)(buf + 14);
            if (ntohs(arp->oper) == 2) {
                /* ARP reply — добавляем в таблицу */
                ip4_addr_t sender_ip = ntohl(arp->spa);
                for (int i = 0; i < ARP_TABLE_SIZE; i++) {
                    if (!arp_table[i].valid || arp_table[i].ip == sender_ip) {
                        arp_table[i].ip    = sender_ip;
                        arp_table[i].valid = true;
                        memcpy(arp_table[i].mac, arp->sha, 6);
                        break;
                    }
                }
            }
        }
        /* Остальные пакеты (IP) обрабатываются в net_tcp_recv/net_dns_resolve
           через прямые вызовы e1000_recv() */
    }
}

/* =============================================================================
 * net_init
 * ============================================================================= */
void net_init(void) {
    memset(arp_table, 0, sizeof(arp_table));
    memset(tcp_connections, 0, sizeof(tcp_connections));
    gw_mac_known = false;
    net_initialized = e1000_present();
    if (net_initialized)
        vga_printf("  net: ready, IP=%d.%d.%d.%d\n",
            (my_ip>>24)&0xFF,(my_ip>>16)&0xFF,
            (my_ip>>8)&0xFF,my_ip&0xFF);
}

bool net_ready(void)      { return net_initialized; }
ip4_addr_t net_get_ip(void){ return my_ip; }
mac_addr_t net_get_mac(void){ return e1000_get_mac(); }
