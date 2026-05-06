/* =============================================================================
 * net/net_http.c
 * HTTP Client: Simple HTTP GET requests
 * ============================================================================= */

#include "net.h"
#include "../memory/heap.h"
#include "../lib/string.h"

int net_http_get(const char* host, uint16_t port, const char* path,
                 char* response_buf, uint32_t buf_size, uint32_t timeout_ms) {
    if (!host || !path || !response_buf || buf_size < 256)
        return -1;
    
    /* Resolve hostname */
    ip4_addr_t server_ip;
    if (!net_dns_resolve(host, &server_ip, timeout_ms))
        return -1;
    
    /* Create TCP connection */
    int sock = net_tcp_socket();
    if (sock < 0)
        return -1;
    
    if (net_tcp_connect(sock, server_ip, port, timeout_ms) < 0) {
        net_tcp_close(sock);
        return -1;
    }
    
    /* Build HTTP request */
    char request[512];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: MyOS/1.0\r\n\r\n",
             path, host);
    
    /* Send request */
    if (net_tcp_send(sock, request, strlen(request)) < 0) {
        net_tcp_close(sock);
        return -1;
    }
    
    /* Receive response */
    int total = 0;
    while (total < (int)(buf_size - 1)) {
        int n = net_tcp_recv(sock, response_buf + total, buf_size - total - 1, timeout_ms);
        if (n < 0)
            break;
        if (n == 0)
            break;
        total += n;
    }
    
    response_buf[total] = '\0';
    net_tcp_close(sock);
    
    return total;
}
