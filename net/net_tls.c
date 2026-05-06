/* =============================================================================
 * net/net_tls.c
 * TLS Protocol Stub: Minimal TLS 1.2 support
 * ============================================================================= */

#include "net.h"
#include "../memory/heap.h"
#include "../lib/string.h"

static tls_socket_t tls_sockets[2];

tls_socket_t* net_tls_connect(ip4_addr_t ip, uint16_t port, uint32_t timeout_ms) {
    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < 2; i++) {
        if (!tls_sockets[i].valid) {
            slot = i;
            break;
        }
    }
    
    if (slot < 0)
        return NULL;
    
    /* Create TCP connection first */
    int tcp_sock = net_tcp_socket();
    if (tcp_sock < 0)
        return NULL;
    
    if (net_tcp_connect(tcp_sock, ip, port, timeout_ms) < 0) {
        net_tcp_close(tcp_sock);
        return NULL;
    }
    
    tls_socket_t* tls = &tls_sockets[slot];
    memset(tls, 0, sizeof(tls_socket_t));
    tls->tcp_sock = tcp_sock;
    tls->valid = true;
    
    /* TODO: Implement TLS 1.2 handshake
     * For now, just return the socket without actual encryption */
    
    return tls;
}

int net_tls_send(tls_socket_t* tls, const void* data, uint32_t len) {
    if (!tls || !tls->valid)
        return -1;
    
    /* TODO: Encrypt data with TLS
     * For now, just send plaintext via TCP */
    return net_tcp_send(tls->tcp_sock, data, len);
}

int net_tls_recv(tls_socket_t* tls, void* buf, uint32_t maxlen) {
    if (!tls || !tls->valid)
        return -1;
    
    /* TODO: Decrypt TLS records
     * For now, just receive plaintext via TCP */
    return net_tcp_recv(tls->tcp_sock, buf, maxlen, 5000);
}

void net_tls_close(tls_socket_t* tls) {
    if (!tls || !tls->valid)
        return;
    
    net_tcp_close(tls->tcp_sock);
    memset(tls, 0, sizeof(tls_socket_t));
}

int net_https_get(const char* host, uint16_t port, const char* path,
                  char* response_buf, uint32_t buf_size, uint32_t timeout_ms) {
    if (!host || !path || !response_buf || buf_size < 256)
        return -1;
    
    /* Resolve hostname */
    ip4_addr_t server_ip;
    if (!net_dns_resolve(host, &server_ip, timeout_ms))
        return -1;
    
    /* Connect via TLS */
    tls_socket_t* tls = net_tls_connect(server_ip, port, timeout_ms);
    if (!tls)
        return -1;
    
    /* Build HTTP request */
    char request[512];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: MyOS/1.0\r\n\r\n",
             path, host);
    
    /* Send request */
    if (net_tls_send(tls, request, strlen(request)) < 0) {
        net_tls_close(tls);
        return -1;
    }
    
    /* Receive response */
    int total = 0;
    while (total < (int)(buf_size - 1)) {
        int n = net_tls_recv(tls, response_buf + total, buf_size - total - 1);
        if (n < 0)
            break;
        if (n == 0)
            break;
        total += n;
    }
    
    response_buf[total] = '\0';
    net_tls_close(tls);
    
    return total;
}
