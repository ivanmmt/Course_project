/* =============================================================================
 * net/net_tls.c — TLS 1.2 client (RSA + AES-128-CBC-SHA256)
 * ============================================================================= */
#include "net.h"
#include "../lib/string.h"
#include "../lib/sha256.h"
#include "../lib/aes.h"
#include "../lib/rsa.h"
#include "../drivers/timer/pit.h"
#include "../memory/heap.h"

#define TLS_RX_SIZE     32768
#define TLS_HS_SIZE     16384
#define TLS_MAX_SOCKETS 2

#define TLS_CT_CHANGE_CIPHER 20
#define TLS_CT_ALERT         21
#define TLS_CT_HANDSHAKE     22
#define TLS_CT_APPLICATION   23

#define TLS_HS_CLIENT_HELLO       1
#define TLS_HS_SERVER_HELLO       2
#define TLS_HS_CERTIFICATE        11
#define TLS_HS_SERVER_HELLO_DONE  14
#define TLS_HS_CLIENT_KEY_EX      16
#define TLS_HS_FINISHED           20

#define TLS_CIPHER_RSA_AES128_SHA256 0x003Cu

typedef struct {
    int      tcp_sock;
    bool     valid;
    bool     secure;

    uint8_t  rx[TLS_RX_SIZE];
    uint32_t rx_len;

    uint8_t  hs[TLS_HS_SIZE];
    uint32_t hs_len;

    uint8_t  client_random[32];
    uint8_t  server_random[32];
    uint8_t  pre_master[48];
    uint8_t  master_secret[48];

    uint8_t  client_mac_key[32];
    uint8_t  server_mac_key[32];
    uint8_t  client_key[16];
    uint8_t  server_key[16];
    uint8_t  client_write_iv[16];
    uint8_t  server_write_iv[16];

    uint64_t client_seq;
    uint64_t server_seq;
    uint16_t cipher;
} tls_ctx_t;

static tls_ctx_t tls_pool[TLS_MAX_SOCKETS];

static void tls_fill_random(uint8_t* buf, uint32_t len) {
    uint32_t t = pit_get_ticks();
    for (uint32_t i = 0; i < len; i++) {
        t = t * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(t ^ (i * 73u));
    }
}

static void tls_p_hash(const uint8_t* secret, uint32_t slen,
                        const uint8_t* seed, uint32_t seed_len,
                        uint8_t* out, uint32_t out_len) {
    uint8_t a[SHA256_DIGEST_SIZE];
    uint8_t block[SHA256_DIGEST_SIZE];
    uint32_t pos = 0;

    hmac_sha256(secret, slen, seed, seed_len, a);
    while (pos < out_len) {
        uint8_t tmp[256];
        uint32_t n = seed_len + SHA256_DIGEST_SIZE;
        if (n > sizeof(tmp)) n = sizeof(tmp);
        memcpy(tmp, a, SHA256_DIGEST_SIZE);
        memcpy(tmp + SHA256_DIGEST_SIZE, seed, n - SHA256_DIGEST_SIZE);
        hmac_sha256(secret, slen, tmp, n, block);
        uint32_t cp = out_len - pos;
        if (cp > SHA256_DIGEST_SIZE) cp = SHA256_DIGEST_SIZE;
        memcpy(out + pos, block, cp);
        pos += cp;
        hmac_sha256(secret, slen, a, SHA256_DIGEST_SIZE, a);
    }
}

static void tls_prf(const uint8_t* secret, uint32_t slen, const char* label,
                    const uint8_t* seed, uint32_t seed_len,
                    uint8_t* out, uint32_t out_len) {
    uint8_t ls[128];
    uint32_t ll = (uint32_t)strlen(label);
    if (ll + seed_len > sizeof(ls)) return;
    memcpy(ls, label, ll);
    memcpy(ls + ll, seed, seed_len);
    tls_p_hash(secret, slen, ls, ll + seed_len, out, out_len);
}

static int tls_tcp_read(tls_ctx_t* t, uint32_t timeout_ms) {
    uint32_t start = (pit_get_ticks() * 1000) / 100;
    while (t->rx_len < TLS_RX_SIZE - 1) {
        int n = net_tcp_recv(t->tcp_sock, t->rx + t->rx_len,
                             TLS_RX_SIZE - t->rx_len, 200);
        if (n > 0) t->rx_len += (uint32_t)n;
        else if (n == 0) return (t->rx_len > 0) ? 0 : -1;
        if (t->rx_len > 0) return 0;
        uint32_t elapsed = (pit_get_ticks() * 1000) / 100 - start;
        if (elapsed >= timeout_ms) return -1;
        net_core_poll();
    }
    return 0;
}

static int tls_pull_tcp(tls_ctx_t* t, uint32_t need, uint32_t timeout_ms) {
    uint32_t start = (pit_get_ticks() * 1000) / 100;
    while (t->rx_len < need) {
        if (tls_tcp_read(t, 200) < 0 && t->rx_len < need) {
            uint32_t elapsed = (pit_get_ticks() * 1000) / 100 - start;
            if (elapsed >= timeout_ms) return -1;
        }
        if (t->rx_len >= need) return 0;
        net_core_poll();
    }
    return 0;
}

static int tls_read_record(tls_ctx_t* t, uint8_t* type_out,
                           uint8_t* payload, uint32_t* payload_len,
                           uint32_t timeout_ms) {
    if (tls_pull_tcp(t, 5, timeout_ms) < 0) return -1;
    uint8_t type = t->rx[0];
    uint16_t len = (uint16_t)((t->rx[3] << 8) | t->rx[4]);
    if (tls_pull_tcp(t, 5u + len, timeout_ms) < 0) return -1;

    memmove(t->rx, t->rx + 5, t->rx_len - 5);
    t->rx_len -= 5;

    if (len > *payload_len) return -1;
    memcpy(payload, t->rx, len);
    memmove(t->rx, t->rx + len, t->rx_len - len);
    t->rx_len -= len;
    *type_out = type;
    *payload_len = len;
    return 0;
}

static void tls_hs_append(tls_ctx_t* t, const uint8_t* data, uint32_t len) {
    if (t->hs_len + len <= TLS_HS_SIZE)
        memcpy(t->hs + t->hs_len, data, len);
    t->hs_len += len;
}

static int tls_send_record(tls_ctx_t* t, uint8_t type, const uint8_t* data, uint32_t len) {
    uint8_t hdr[5];
    hdr[0] = type;
    hdr[1] = 0x03;
    hdr[2] = 0x03;
    hdr[3] = (uint8_t)(len >> 8);
    hdr[4] = (uint8_t)(len);
    if (net_tcp_send(t->tcp_sock, hdr, 5) < 0) return -1;
    if (len && net_tcp_send(t->tcp_sock, data, len) < 0) return -1;
    return 0;
}

static int tls_mac(tls_ctx_t* t, bool from_server, uint8_t type,
                   const uint8_t* data, uint32_t len, uint8_t mac[32]) {
    uint8_t hdr[13];
    uint64_t seq = from_server ? t->server_seq : t->client_seq;
    for (int i = 7; i >= 0; i--) hdr[i] = (uint8_t)(seq >> ((7 - i) * 8));
    hdr[8] = type;
    hdr[9] = 0x03;
    hdr[10] = 0x03;
    hdr[11] = (uint8_t)(len >> 8);
    hdr[12] = (uint8_t)(len);
    uint8_t buf[4096];
    if (len + 13 > sizeof(buf)) return -1;
    memcpy(buf, hdr, 13);
    memcpy(buf + 13, data, len);
    const uint8_t* key = from_server ? t->server_mac_key : t->client_mac_key;
    hmac_sha256(key, 32, buf, len + 13, mac);
    return 0;
}

static int tls_send_ciphertext(tls_ctx_t* t, uint8_t type,
                               const uint8_t* key, uint8_t* iv,
                               const uint8_t* data, uint32_t len,
                               bool from_server_mac) {
    uint32_t pad = 16 - ((len + 32 + 1) % 16);
    if (pad == 16) pad = 0;
    uint32_t enc_len = len + 32 + 1 + pad;
    uint8_t* buf = (uint8_t*)kmalloc(enc_len * 2);
    if (!buf) return -1;
    uint8_t* plain = buf;
    uint8_t* enc   = buf + enc_len;
    memcpy(plain, data, len);
    uint8_t mac[32];
    tls_mac(t, from_server_mac, type, data, len, mac);
    memcpy(plain + len, mac, 32);
    plain[len + 32] = (uint8_t)pad;
    for (uint32_t i = 0; i < pad; i++) plain[len + 33 + i] = (uint8_t)pad;
    aes128_cbc_encrypt(key, iv, plain, enc_len, enc);
    int r = tls_send_record(t, type, enc, enc_len);
    if (r == 0 && enc_len >= 16)
        memcpy(iv, enc + enc_len - 16, 16);
    kfree(buf);
    if (r == 0 && !from_server_mac) t->client_seq++;
    return r;
}

static int tls_send_app(tls_ctx_t* t, const uint8_t* data, uint32_t len) {
    if (!t->secure) return net_tcp_send(t->tcp_sock, data, len);
    return tls_send_ciphertext(t, TLS_CT_APPLICATION, t->client_key, t->client_write_iv,
                             data, len, false);
}

static int tls_decrypt_record(tls_ctx_t* t, uint8_t type,
                              const uint8_t* payload, uint32_t len,
                              uint8_t* out, uint32_t* out_len) {
    if (!t->secure) {
        if (len > *out_len) return -1;
        memcpy(out, payload, len);
        *out_len = len;
        return 0;
    }
    uint8_t* dec = (uint8_t*)kmalloc(len);
    if (!dec) return -1;
    aes128_cbc_decrypt(t->server_key, t->server_write_iv, payload, len, dec);
    if (len >= 16)
        memcpy(t->server_write_iv, payload + len - 16, 16);
    if (len < 33) { kfree(dec); return -1; }
    uint8_t pad = dec[len - 1];
    if ((uint32_t)pad + 33 > len) { kfree(dec); return -1; }
    uint32_t data_len = len - 32 - 1 - pad;
    uint8_t mac[32];
    tls_mac(t, true, type, dec, data_len, mac);
    if (memcmp(mac, dec + data_len, 32) != 0) { kfree(dec); return -1; }
    if (data_len > *out_len) data_len = *out_len;
    memcpy(out, dec, data_len);
    *out_len = data_len;
    kfree(dec);
    t->server_seq++;
    return 0;
}

static int tls_recv_app(tls_ctx_t* t, uint8_t* buf, uint32_t maxlen) {
    uint8_t payload[8192];
    uint32_t out_len;

    while (1) {
        if (t->rx_len >= 5) {
            uint16_t len = (uint16_t)((t->rx[3] << 8) | t->rx[4]);
            if (t->rx_len >= 5u + len) {
                uint8_t type = t->rx[0];
                memcpy(payload, t->rx + 5, len);
                memmove(t->rx, t->rx + 5 + len, t->rx_len - 5 - len);
                t->rx_len -= 5 + len;
                if (type != TLS_CT_APPLICATION) continue;
                out_len = maxlen;
                if (tls_decrypt_record(t, type, payload, len, buf, &out_len) == 0)
                    return (int)out_len;
                return -1;
            }
        }
        int n = net_tcp_recv(t->tcp_sock, t->rx + t->rx_len,
                             TLS_RX_SIZE - t->rx_len, 500);
        if (n > 0) t->rx_len += (uint32_t)n;
        else if (n == 0) return 0;
        else return -1;
    }
}

static int tls_build_client_hello(tls_ctx_t* t, const char* host,
                                  uint8_t* out, uint32_t* out_len) {
    uint8_t* p = out;
    p[0] = TLS_HS_CLIENT_HELLO;
    p += 4;
    uint8_t* body = p;
    p[0] = 0x03; p[1] = 0x03;
    tls_fill_random(t->client_random, 32);
    memcpy(p + 2, t->client_random, 32);
    p += 34;
    p[0] = 0; p[1] = 0;
    p += 2;
    p[0] = 0; p[1] = 4;
    p[2] = 0x00; p[3] = TLS_CIPHER_RSA_AES128_SHA256 >> 8;
    p[4] = (uint8_t)TLS_CIPHER_RSA_AES128_SHA256;
    p[5] = 0x00; p[6] = 0x01; p[7] = 0x00;
    p += 8;
    uint8_t* ext_len_p = p; p += 2;
    p[0] = 0x00; p[1] = 0x00; /* server_name */
    p += 2;
    uint8_t* sni_len_p = p; p += 2;
    uint32_t hlen = (uint32_t)strlen(host);
    uint16_t list_len = (uint16_t)(hlen + 3);
    p[0] = (uint8_t)(list_len >> 8);
    p[1] = (uint8_t)list_len;
    p[2] = 0; /* host_name type */
    p[3] = (uint8_t)(hlen >> 8);
    p[4] = (uint8_t)hlen;
    memcpy(p + 5, host, hlen);
    p += 5 + hlen;
    uint32_t sni_len = (uint32_t)(p - sni_len_p - 2);
    sni_len_p[0] = (uint8_t)(sni_len >> 8);
    sni_len_p[1] = (uint8_t)sni_len;
    uint32_t ext_len = (uint32_t)(p - ext_len_p - 2);
    ext_len_p[0] = (uint8_t)(ext_len >> 8);
    ext_len_p[1] = (uint8_t)ext_len;
    uint32_t body_len = (uint32_t)(p - body);
    out[1] = (uint8_t)(body_len >> 16);
    out[2] = (uint8_t)(body_len >> 8);
    out[3] = (uint8_t)body_len;
    *out_len = (uint32_t)(p - out);
    tls_hs_append(t, out, *out_len);
    return 0;
}

static int tls_parse_server_hello(tls_ctx_t* t, const uint8_t* msg, uint32_t len) {
    if (len < 38 || msg[0] != TLS_HS_SERVER_HELLO) return -1;
    if (msg[4] != 0x03 || msg[5] != 0x03) return -1;
    memcpy(t->server_random, msg + 6, 32);
    uint32_t off = 38;
    if (off + 2 > len) return -1;
    t->cipher = (uint16_t)((msg[off] << 8) | msg[off + 1]);
    return (t->cipher == TLS_CIPHER_RSA_AES128_SHA256) ? 0 : -1;
}

static int tls_get_cert_der(tls_ctx_t* t, const uint8_t* msg, uint32_t len,
                            const uint8_t** cert, uint32_t* cert_len) {
    if (len < 7 || msg[0] != TLS_HS_CERTIFICATE) return -1;
    uint32_t list_len = ((uint32_t)msg[4] << 16) | ((uint32_t)msg[5] << 8) | msg[6];
    if (7 + list_len > len || list_len < 3) return -1;
    uint32_t clen = ((uint32_t)msg[7] << 16) | ((uint32_t)msg[8] << 8) | msg[9];
    if (10 + clen > len) return -1;
    *cert = msg + 10;
    *cert_len = clen;
    return 0;
}

static int tls_handshake(tls_ctx_t* t, const char* host, uint32_t timeout_ms) {
    uint8_t hello[512];
    uint32_t hello_len = 0;
    if (tls_build_client_hello(t, host, hello, &hello_len) < 0) return -1;
    if (tls_send_record(t, TLS_CT_HANDSHAKE, hello, hello_len) < 0) return -1;

    bool got_hello = false, got_cert = false, got_done = false;
    const uint8_t* cert_der = NULL;
    uint32_t cert_der_len = 0;

    uint8_t payload[8192];
    uint32_t plen;
    uint8_t type;
    uint32_t start = (pit_get_ticks() * 1000) / 100;

    while (!got_done) {
        plen = sizeof(payload);
        if (tls_read_record(t, &type, payload, &plen, timeout_ms) < 0) return -1;
        if (type != TLS_CT_HANDSHAKE) continue;
        uint32_t off = 0;
        while (off + 4 <= plen) {
            uint32_t mlen = ((uint32_t)payload[off + 1] << 16) |
                            ((uint32_t)payload[off + 2] << 8) | payload[off + 3];
            if (off + 4 + mlen > plen) break;
            const uint8_t* msg = payload + off;
            tls_hs_append(t, msg, 4 + mlen);
            if (msg[0] == TLS_HS_SERVER_HELLO) {
                if (tls_parse_server_hello(t, msg, 4 + mlen) < 0) return -1;
                got_hello = true;
            } else if (msg[0] == TLS_HS_CERTIFICATE) {
                if (tls_get_cert_der(t, msg, 4 + mlen, &cert_der, &cert_der_len) < 0) return -1;
                got_cert = true;
            } else if (msg[0] == TLS_HS_SERVER_HELLO_DONE) {
                got_done = true;
            }
            off += 4 + mlen;
        }
        uint32_t elapsed = (pit_get_ticks() * 1000) / 100 - start;
        if (elapsed >= timeout_ms) return -1;
    }

    if (!got_hello || !got_cert || !cert_der) return -1;

    rsa_pubkey_t pk;
    if (!rsa_parse_pubkey_from_cert(cert_der, cert_der_len, &pk)) return -1;

    t->pre_master[0] = 0x03;
    t->pre_master[1] = 0x03;
    tls_fill_random(t->pre_master + 2, 46);

    uint8_t enc[512];
    uint32_t enc_len = 0;
    if (!rsa_pkcs1_v15_encrypt(&pk, t->pre_master, 48, enc, &enc_len)) return -1;

    uint8_t ckx[600];
    ckx[0] = TLS_HS_CLIENT_KEY_EX;
    ckx[4] = (uint8_t)(enc_len >> 8);
    ckx[5] = (uint8_t)enc_len;
    memcpy(ckx + 6, enc, enc_len);
    uint32_t ckx_body = 2 + enc_len;
    ckx[1] = (uint8_t)(ckx_body >> 16);
    ckx[2] = (uint8_t)(ckx_body >> 8);
    ckx[3] = (uint8_t)ckx_body;
    uint32_t ckx_len = 6 + enc_len;
    tls_hs_append(t, ckx, ckx_len);
    if (tls_send_record(t, TLS_CT_HANDSHAKE, ckx, ckx_len) < 0) return -1;

    uint8_t seed[64];
    memcpy(seed, t->client_random, 32);
    memcpy(seed + 32, t->server_random, 32);
    tls_prf(t->pre_master, 48, "master secret", seed, 64, t->master_secret, 48);

    uint8_t key_block[160];
    memcpy(seed, t->server_random, 32);
    memcpy(seed + 32, t->client_random, 32);
    tls_prf(t->master_secret, 48, "key expansion", seed, 64, key_block, 160);
    memcpy(t->client_mac_key, key_block, 32);
    memcpy(t->server_mac_key, key_block + 32, 32);
    memcpy(t->client_key, key_block + 64, 16);
    memcpy(t->server_key, key_block + 80, 16);
    memcpy(t->client_write_iv, key_block + 96, 16);
    memcpy(t->server_write_iv, key_block + 112, 16);

    uint8_t ccs[1] = {1};
    if (tls_send_record(t, TLS_CT_CHANGE_CIPHER, ccs, 1) < 0) return -1;

    uint8_t hs_hash[32];
    sha256(t->hs, t->hs_len, hs_hash);
    uint8_t verify[12];
    tls_prf(t->master_secret, 48, "client finished", hs_hash, 32, verify, 12);

    uint8_t fin[16];
    fin[0] = TLS_HS_FINISHED;
    fin[1] = 0; fin[2] = 0; fin[3] = 12;
    memcpy(fin + 4, verify, 12);
    t->secure = true;
    t->client_seq = 0;
    t->server_seq = 0;
    if (tls_send_ciphertext(t, TLS_CT_HANDSHAKE, t->client_key, t->client_write_iv,
                            fin, 16, false) < 0)
        return -1;
    tls_hs_append(t, fin, 16);

    bool got_ccs = false, got_fin = false;
    while (!got_fin) {
        plen = sizeof(payload);
        if (tls_read_record(t, &type, payload, &plen, timeout_ms) < 0) return -1;
        if (type == TLS_CT_CHANGE_CIPHER) { got_ccs = true; continue; }
        if (type == TLS_CT_HANDSHAKE && plen >= 16) {
            uint8_t* dec = (uint8_t*)kmalloc(plen);
            if (!dec) return -1;
            aes128_cbc_decrypt(t->server_key, t->server_write_iv, payload, plen, dec);
            if (plen >= 16)
                memcpy(t->server_write_iv, payload + plen - 16, 16);
            if (dec[0] == TLS_HS_FINISHED) {
                tls_hs_append(t, dec, 16);
                got_fin = true;
            }
            kfree(dec);
            t->server_seq++;
        }
    }
    (void)got_ccs;
    return 0;
}

tls_socket_t* net_tls_connect(ip4_addr_t ip, uint16_t port, uint32_t timeout_ms) {
    int slot = -1;
    for (int i = 0; i < TLS_MAX_SOCKETS; i++) {
        if (!tls_pool[i].valid) { slot = i; break; }
    }
    if (slot < 0) return NULL;

    int tcp_sock = net_tcp_socket();
    if (tcp_sock < 0) return NULL;
    if (net_tcp_connect(tcp_sock, ip, port, timeout_ms) < 0) {
        net_tcp_close(tcp_sock);
        return NULL;
    }

    tls_ctx_t* ctx = &tls_pool[slot];
    memset(ctx, 0, sizeof(*ctx));
    ctx->tcp_sock = tcp_sock;
    ctx->valid = true;
    return (tls_socket_t*)ctx;
}

static tls_ctx_t* tls_cast(tls_socket_t* tls) {
    return (tls_ctx_t*)tls;
}

int net_tls_send(tls_socket_t* tls, const void* data, uint32_t len) {
    tls_ctx_t* t = tls_cast(tls);
    if (!t || !t->valid) return -1;
    return tls_send_app(t, (const uint8_t*)data, len);
}

int net_tls_recv(tls_socket_t* tls, void* buf, uint32_t maxlen) {
    tls_ctx_t* t = tls_cast(tls);
    if (!t || !t->valid) return -1;
    return tls_recv_app(t, (uint8_t*)buf, maxlen);
}

void net_tls_close(tls_socket_t* tls) {
    tls_ctx_t* t = tls_cast(tls);
    if (!t || !t->valid) return;
    net_tcp_close(t->tcp_sock);
    memset(t, 0, sizeof(*t));
}

int net_https_get(const char* host, uint16_t port, const char* path,
                  char* response_buf, uint32_t buf_size, uint32_t timeout_ms) {
    if (!host || !path || !response_buf || buf_size < 256)
        return -1;

    ip4_addr_t server_ip;
    if (!net_dns_resolve(host, &server_ip, timeout_ms))
        return -1;

    tls_socket_t* tls = net_tls_connect(server_ip, port, timeout_ms);
    if (!tls) return -1;

    tls_ctx_t* ctx = tls_cast(tls);
    if (tls_handshake(ctx, host, timeout_ms) < 0) {
        net_tls_close(tls);
        return -1;
    }

    char request[512];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
             "User-Agent: MyOS/1.0 (TLS)\r\nAccept: text/html\r\n\r\n",
             path, host);

    if (net_tls_send(tls, request, (uint32_t)strlen(request)) < 0) {
        net_tls_close(tls);
        return -1;
    }

    int total = 0;
    while (total < (int)(buf_size - 1)) {
        int n = net_tls_recv(tls, response_buf + total, buf_size - total - 1);
        if (n < 0) break;
        if (n == 0) break;
        total += n;
    }
    response_buf[total] = '\0';
    net_tls_close(tls);
    return total;
}
