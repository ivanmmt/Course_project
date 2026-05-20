/* =============================================================================
 * net/net_tcp.c
 * TCP Protocol: Reliable, ordered stream transport (RFC 793)
 * ============================================================================= */

#include "net.h"
#include "../memory/heap.h"
#include "../lib/string.h"
#include "../drivers/timer/pit.h"

static tcp_socket_t tcp_sockets[TCP_MAX_SOCKETS];
static uint16_t tcp_port_counter = 49152;

/* ─────────────────────────────────────────────────────────────────────────── */
static uint16_t tcp_checksum(ip4_addr_t src, ip4_addr_t dst,
                             const void* seg, uint16_t len) {
    struct {
        uint32_t src, dst;
        uint8_t zero, proto;
        uint16_t len;
    } __attribute__((packed)) ph;
    
    ph.src = src;
    ph.dst = dst;
    ph.zero = 0;
    ph.proto = IP_PROTO_TCP;
    ph.len = htons(len);
    
    uint8_t buf[4096];
    memcpy(buf, &ph, sizeof(ph));
    memcpy(buf + sizeof(ph), seg, len);
    
    return net_checksum(buf, sizeof(ph) + len);
}

/* ─────────────────────────────────────────────────────────────────────────── */
int net_tcp_socket(void) {
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!tcp_sockets[i].in_use) {
            memset(&tcp_sockets[i], 0, sizeof(tcp_socket_t));
            tcp_sockets[i].in_use = true;
            tcp_sockets[i].state = TCP_CLOSED;
            tcp_sockets[i].window = TCP_RX_BUF_SIZE;
            tcp_sockets[i].timeout_ms = TCP_TIMEOUT_MS;
            tcp_sockets[i].last_activity = (pit_get_ticks() * 1000) / 100;
            return i;
        }
    }
    return -1;
}

/* ─────────────────────────────────────────────────────────────────────────── */
int net_tcp_connect(int sock, ip4_addr_t ip, uint16_t port, uint32_t timeout_ms) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS || !tcp_sockets[sock].in_use)
        return -1;
    
    tcp_socket_t* s = &tcp_sockets[sock];
    
    if (s->state != TCP_CLOSED)
        return -1;
    
    s->remote_ip = ip;
    s->remote_port = port;
    s->local_port = tcp_port_counter++;
    s->seq = 0x12345678;  /* Random ISN (Initial Sequence Number) */
    s->ack = 0;
    s->peer_seq = 0;
    s->peer_ack = 0;
    s->peer_window = 65535;
    s->timeout_ms = timeout_ms;
    s->last_activity = (pit_get_ticks() * 1000) / 100;
    
    /* Send SYN packet */
    tcp_header_t syn;
    memset(&syn, 0, sizeof(syn));
    syn.src_port = htons(s->local_port);
    syn.dst_port = htons(s->remote_port);
    syn.seq = htonl(s->seq);
    syn.ack = 0;
    syn.data_offset = 5 << 4;  /* 5 words = 20 bytes */
    syn.flags = TCP_FLAG_SYN;
    syn.window = htons(s->window);
    syn.checksum = 0;
    syn.checksum = tcp_checksum(net_get_ip(), s->remote_ip, &syn, sizeof(syn));
    
    if (net_ip_send(ip, IP_PROTO_TCP, &syn, sizeof(syn)) < 0)
        return -1;

    s->seq++;  /* SYN consumes one sequence number */
    s->state = TCP_SYN_SENT;
    s->last_tx_time = (pit_get_ticks() * 1000) / 100;
    s->retry_count = 0;
    
    /* Wait for SYN-ACK */
    uint32_t start = (pit_get_ticks() * 1000) / 100;
    while (1) {
        net_core_poll();
        
        if (s->state == TCP_ESTABLISHED)
            return 0;
        
        uint32_t elapsed = (pit_get_ticks() * 1000) / 100 - start;
        if (elapsed >= timeout_ms) {
            s->state = TCP_CLOSED;
            return -1;
        }
        
        /* Retransmit SYN if needed */
        uint32_t now = (pit_get_ticks() * 1000) / 100;
        if ((now - s->last_tx_time) > TCP_RETRANSMIT_MS && s->retry_count < TCP_MAX_RETRIES) {
            syn.checksum = 0;
            syn.checksum = tcp_checksum(net_get_ip(), s->remote_ip, &syn, sizeof(syn));
            net_ip_send(ip, IP_PROTO_TCP, &syn, sizeof(syn));
            s->last_tx_time = now;
            s->retry_count++;
        }
        
        pit_sleep(10);
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
int net_tcp_send(int sock, const void* data, uint32_t len) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS || !tcp_sockets[sock].in_use)
        return -1;
    
    tcp_socket_t* s = &tcp_sockets[sock];
    
    if (s->state != TCP_ESTABLISHED)
        return -1;
    
    if (len > TCP_TX_BUF_SIZE)
        len = TCP_TX_BUF_SIZE;
    
    /* Build TCP segment */
    uint8_t pkt[4096];
    tcp_header_t* tcphdr = (tcp_header_t*)pkt;
    
    memset(tcphdr, 0, sizeof(tcp_header_t));
    tcphdr->src_port = htons(s->local_port);
    tcphdr->dst_port = htons(s->remote_port);
    tcphdr->seq = htonl(s->seq);
    tcphdr->ack = htonl(s->ack);
    tcphdr->data_offset = 5 << 4;
    tcphdr->flags = TCP_FLAG_PSH | TCP_FLAG_ACK;
    tcphdr->window = htons(s->window);
    tcphdr->checksum = 0;
    
    /* Copy payload */
    memcpy(pkt + sizeof(tcp_header_t), data, len);
    
    /* Calculate checksum */
    tcphdr->checksum = tcp_checksum(net_get_ip(), s->remote_ip, pkt, sizeof(tcp_header_t) + len);
    
    /* Send via IP */
    if (net_ip_send(s->remote_ip, IP_PROTO_TCP, pkt, sizeof(tcp_header_t) + len) < 0)
        return -1;
    
    s->seq += len;
    s->last_tx_time = (pit_get_ticks() * 1000) / 100;
    s->last_activity = s->last_tx_time;
    
    return len;
}

/* ─────────────────────────────────────────────────────────────────────────── */
int net_tcp_recv(int sock, void* buf, uint32_t maxlen, uint32_t timeout_ms) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS || !tcp_sockets[sock].in_use)
        return -1;
    
    tcp_socket_t* s = &tcp_sockets[sock];
    
    if (s->state != TCP_ESTABLISHED)
        return -1;
    
    uint32_t start = (pit_get_ticks() * 1000) / 100;
    while (1) {
        net_core_poll();
        
        /* Check if we have data */
        uint32_t available = (s->rx_tail >= s->rx_head) ? 
                            (s->rx_tail - s->rx_head) :
                            (TCP_RX_BUF_SIZE - s->rx_head + s->rx_tail);
        
        if (available > 0) {
            uint32_t copy_len = (available < maxlen) ? available : maxlen;
            
            if ((s->rx_head + copy_len) <= TCP_RX_BUF_SIZE) {
                memcpy(buf, s->rx_buf + s->rx_head, copy_len);
            } else {
                /* Wrap around */
                uint32_t first = TCP_RX_BUF_SIZE - s->rx_head;
                memcpy(buf, s->rx_buf + s->rx_head, first);
                memcpy((uint8_t*)buf + first, s->rx_buf, copy_len - first);
            }
            
            s->rx_head = (s->rx_head + copy_len) % TCP_RX_BUF_SIZE;
            s->last_activity = (pit_get_ticks() * 1000) / 100;
            return (int)copy_len;
        }

        if (s->state == TCP_CLOSE_WAIT || s->state == TCP_FIN_WAIT_2)
            return 0;
        
        uint32_t elapsed = (pit_get_ticks() * 1000) / 100 - start;
        if (elapsed >= timeout_ms)
            return -1;  /* Timeout */
        
        /* Check connection timeout */
        uint32_t now = (pit_get_ticks() * 1000) / 100;
        if ((now - s->last_activity) > s->timeout_ms) {
            s->state = TCP_CLOSED;
            return -1;
        }
        
        pit_sleep(10);
    }
}

/* ─────────────────────────────────────────────────────────────────────────── */
int net_tcp_close(int sock) {
    if (sock < 0 || sock >= TCP_MAX_SOCKETS || !tcp_sockets[sock].in_use)
        return -1;
    
    tcp_socket_t* s = &tcp_sockets[sock];
    
    if (s->state == TCP_ESTABLISHED) {
        /* Send FIN packet */
        tcp_header_t fin;
        memset(&fin, 0, sizeof(fin));
        fin.src_port = htons(s->local_port);
        fin.dst_port = htons(s->remote_port);
        fin.seq = htonl(s->seq);
        fin.ack = htonl(s->ack);
        fin.data_offset = 5 << 4;
        fin.flags = TCP_FLAG_FIN | TCP_FLAG_ACK;
        fin.window = htons(s->window);
        fin.checksum = 0;
        fin.checksum = tcp_checksum(net_get_ip(), s->remote_ip, &fin, sizeof(fin));
        
        net_ip_send(s->remote_ip, IP_PROTO_TCP, &fin, sizeof(fin));
        s->state = TCP_FIN_WAIT_1;
        s->seq++;
    }
    
    s->in_use = false;
    s->state = TCP_CLOSED;
    return 0;
}

/* ─────────────────────────────────────────────────────────────────────────── */
void net_tcp_handle_packet(const ip4_header_t* iphdr, const tcp_header_t* tcphdr,
                           const void* payload, int len) {
    uint16_t src_port = ntohs(tcphdr->src_port);
    uint16_t dst_port = ntohs(tcphdr->dst_port);
    uint32_t seq = ntohl(tcphdr->seq);
    uint32_t ack = ntohl(tcphdr->ack);
    uint8_t flags = tcphdr->flags;
    
    /* Find matching socket */
    tcp_socket_t* s = NULL;
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!tcp_sockets[i].in_use)
            continue;
        
        if (tcp_sockets[i].local_port == dst_port &&
            tcp_sockets[i].remote_ip == iphdr->src_ip &&
            tcp_sockets[i].remote_port == src_port) {
            s = &tcp_sockets[i];
            break;
        }
    }
    
    if (!s)
        return;  /* No matching socket */
    
    s->last_activity = (pit_get_ticks() * 1000) / 100;
    
    /* State machine */
    switch (s->state) {
    case TCP_SYN_SENT:
        if ((flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK)) {
            /* SYN-ACK received */
            s->peer_seq = seq + 1;
            s->peer_ack = ack;
            s->peer_window = ntohs(tcphdr->window);
            s->ack = seq + 1;
            
            /* Send ACK */
            tcp_header_t ack_pkt;
            memset(&ack_pkt, 0, sizeof(ack_pkt));
            ack_pkt.src_port = htons(s->local_port);
            ack_pkt.dst_port = htons(s->remote_port);
            ack_pkt.seq = htonl(s->seq);
            ack_pkt.ack = htonl(s->ack);
            ack_pkt.data_offset = 5 << 4;
            ack_pkt.flags = TCP_FLAG_ACK;
            ack_pkt.window = htons(s->window);
            ack_pkt.checksum = 0;
            ack_pkt.checksum = tcp_checksum(net_get_ip(), s->remote_ip, &ack_pkt, sizeof(ack_pkt));
            
            net_ip_send(s->remote_ip, IP_PROTO_TCP, &ack_pkt, sizeof(ack_pkt));
            s->state = TCP_ESTABLISHED;
        }
        break;
    
    case TCP_ESTABLISHED:
        if (flags & TCP_FLAG_ACK) {
            s->peer_ack = ack;
            s->peer_window = ntohs(tcphdr->window);
        }
        
        if (len > 0) {
            /* Data received, store in RX buffer */
            uint32_t used = (s->rx_tail >= s->rx_head) ?
                            (s->rx_tail - s->rx_head) :
                            (TCP_RX_BUF_SIZE - s->rx_head + s->rx_tail);
            uint32_t space = TCP_RX_BUF_SIZE - used - 1;
            
            if (space >= (uint32_t)len) {
                if ((s->rx_tail + len) <= TCP_RX_BUF_SIZE) {
                    memcpy(s->rx_buf + s->rx_tail, payload, len);
                } else {
                    uint32_t first = TCP_RX_BUF_SIZE - s->rx_tail;
                    memcpy(s->rx_buf + s->rx_tail, payload, first);
                    memcpy(s->rx_buf, (const uint8_t*)payload + first, len - first);
                }
                s->rx_tail = (s->rx_tail + len) % TCP_RX_BUF_SIZE;
                s->ack = seq + (uint32_t)len;
                s->peer_seq = s->ack;
                
                /* Send ACK */
                tcp_header_t ack_pkt;
                memset(&ack_pkt, 0, sizeof(ack_pkt));
                ack_pkt.src_port = htons(s->local_port);
                ack_pkt.dst_port = htons(s->remote_port);
                ack_pkt.seq = htonl(s->seq);
                ack_pkt.ack = htonl(s->ack);
                ack_pkt.data_offset = 5 << 4;
                ack_pkt.flags = TCP_FLAG_ACK;
                ack_pkt.window = htons(s->window);
                ack_pkt.checksum = 0;
                ack_pkt.checksum = tcp_checksum(net_get_ip(), s->remote_ip, &ack_pkt, sizeof(ack_pkt));
                
                net_ip_send(s->remote_ip, IP_PROTO_TCP, &ack_pkt, sizeof(ack_pkt));
            }
        }
        
        if (flags & TCP_FLAG_FIN) {
            s->state = TCP_CLOSE_WAIT;
            s->peer_seq++;
            s->ack = s->peer_seq;
            
            /* Send ACK for FIN */
            tcp_header_t fin_ack;
            memset(&fin_ack, 0, sizeof(fin_ack));
            fin_ack.src_port = htons(s->local_port);
            fin_ack.dst_port = htons(s->remote_port);
            fin_ack.seq = htonl(s->seq);
            fin_ack.ack = htonl(s->ack);
            fin_ack.data_offset = 5 << 4;
            fin_ack.flags = TCP_FLAG_ACK;
            fin_ack.window = htons(s->window);
            fin_ack.checksum = 0;
            fin_ack.checksum = tcp_checksum(net_get_ip(), s->remote_ip, &fin_ack, sizeof(fin_ack));
            
            net_ip_send(s->remote_ip, IP_PROTO_TCP, &fin_ack, sizeof(fin_ack));
        }
        break;
    
    case TCP_CLOSE_WAIT:
        /* Already sent ACK for FIN, just wait for app to close */
        break;
    
    case TCP_FIN_WAIT_1:
        if (flags & TCP_FLAG_ACK) {
            s->state = TCP_FIN_WAIT_2;
        }
        break;
    
    case TCP_FIN_WAIT_2:
        if (flags & TCP_FLAG_FIN) {
            s->state = TCP_TIME_WAIT;
            
            /* Send final ACK */
            tcp_header_t final_ack;
            memset(&final_ack, 0, sizeof(final_ack));
            final_ack.src_port = htons(s->local_port);
            final_ack.dst_port = htons(s->remote_port);
            final_ack.seq = htonl(s->seq);
            final_ack.ack = htonl(ack + 1);
            final_ack.data_offset = 5 << 4;
            final_ack.flags = TCP_FLAG_ACK;
            final_ack.window = htons(s->window);
            final_ack.checksum = 0;
            final_ack.checksum = tcp_checksum(net_get_ip(), s->remote_ip, &final_ack, sizeof(final_ack));
            
            net_ip_send(s->remote_ip, IP_PROTO_TCP, &final_ack, sizeof(final_ack));
        }
        break;
    
    default:
        break;
    }
}
