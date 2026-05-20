#include "rsa.h"
#include "string.h"
#include "../drivers/timer/pit.h"

#define BN_MAX 128

typedef struct {
    uint32_t w[BN_MAX];
    int len;
} bn_t;

static int bn_cmp(const bn_t* a, const bn_t* b) {
    if (a->len != b->len) return (a->len > b->len) ? 1 : -1;
    for (int i = a->len - 1; i >= 0; i--) {
        if (a->w[i] > b->w[i]) return 1;
        if (a->w[i] < b->w[i]) return -1;
    }
    return 0;
}

static void bn_norm(bn_t* a) {
    while (a->len > 1 && a->w[a->len - 1] == 0) a->len--;
    if (a->len < 1) { a->len = 1; a->w[0] = 0; }
}

static void bn_from_be(const uint8_t* in, uint32_t in_len, bn_t* a) {
    memset(a->w, 0, sizeof(a->w));
    a->len = (int)((in_len + 3) / 4);
    for (uint32_t i = 0; i < in_len; i++)
        a->w[(in_len - 1 - i) / 4] |= ((uint32_t)in[i]) << (((in_len - 1 - i) % 4) * 8);
    bn_norm(a);
}

static void bn_to_be(const bn_t* a, uint8_t* out, uint32_t out_len) {
    memset(out, 0, out_len);
    for (uint32_t i = 0; i < out_len; i++) {
        uint32_t limb = a->w[(out_len - 1 - i) / 4];
        out[i] = (uint8_t)(limb >> (((out_len - 1 - i) % 4) * 8));
    }
}

static void bn_sub(const bn_t* a, const bn_t* b, bn_t* r) {
    uint64_t borrow = 0;
    r->len = a->len;
    for (int i = 0; i < a->len; i++) {
        uint64_t av = a->w[i];
        uint64_t bv = (i < b->len) ? b->w[i] : 0;
        uint64_t v = av - bv - borrow;
        r->w[i] = (uint32_t)v;
        borrow = (v >> 63) & 1;
    }
    bn_norm(r);
}

static void bn_shl1(bn_t* a) {
    uint32_t carry = 0;
    for (int i = 0; i < a->len; i++) {
        uint32_t nxt = a->w[i] >> 31;
        a->w[i] = (a->w[i] << 1) | carry;
        carry = nxt;
    }
    if (carry) a->w[a->len++] = carry;
}

static void bn_mod(const bn_t* a, const bn_t* n, bn_t* r) {
    *r = *a;
    if (bn_cmp(r, n) < 0) return;
    bn_t t = *n;
    while (bn_cmp(&t, r) <= 0) bn_shl1(&t);
    while (bn_cmp(&t, n) >= 0) {
        bn_t half = t;
        /* t >>= 1 */
        uint32_t carry = 0;
        for (int i = half.len - 1; i >= 0; i--) {
            uint32_t nxt = half.w[i] & 1;
            half.w[i] = (half.w[i] >> 1) | (carry << 31);
            carry = nxt;
        }
        bn_norm(&half);
        t = half;
    }
    while (bn_cmp(r, n) >= 0) {
        if (bn_cmp(r, &t) >= 0) bn_sub(r, &t, r);
        bn_t half = t;
        uint32_t carry = 0;
        for (int i = half.len - 1; i >= 0; i--) {
            uint32_t nxt = half.w[i] & 1;
            half.w[i] = (half.w[i] >> 1) | (carry << 31);
            carry = nxt;
        }
        bn_norm(&half);
        if (half.len == 1 && half.w[0] == 0) break;
        t = half;
    }
}

static void bn_mul(const bn_t* a, const bn_t* b, bn_t* out) {
    memset(out->w, 0, sizeof(out->w));
    out->len = a->len + b->len;
    for (int i = 0; i < a->len; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < b->len; j++) {
            uint64_t cur = (uint64_t)out->w[i + j] + (uint64_t)a->w[i] * b->w[j] + carry;
            out->w[i + j] = (uint32_t)cur;
            carry = cur >> 32;
        }
        out->w[i + b->len] = (uint32_t)carry;
    }
    bn_norm(out);
}

static void bn_mod_mul(const bn_t* a, const bn_t* b, const bn_t* n, bn_t* r) {
    bn_t t;
    bn_mul(a, b, &t);
    bn_mod(&t, n, r);
}

static int bn_bit(const bn_t* e, int bit) {
    int limb = bit / 32;
    int off  = bit % 32;
    if (limb >= e->len) return 0;
    return (e->w[limb] >> off) & 1;
}

static int bn_bits(const bn_t* e) {
    for (int i = e->len - 1; i >= 0; i--) {
        if (e->w[i] == 0) continue;
        for (int b = 31; b >= 0; b--)
            if (e->w[i] & (1u << b)) return i * 32 + b + 1;
    }
    return 1;
}

static void bn_mod_exp(const bn_t* base, const bn_t* exp, const bn_t* mod, bn_t* out) {
    bn_t result, b;
    result.w[0] = 1; result.len = 1;
    bn_mod(base, mod, &b);
    int bits = bn_bits(exp);
    for (int i = bits - 1; i >= 0; i--) {
        bn_mod_mul(&result, &result, mod, &result);
        if (bn_bit(exp, i)) bn_mod_mul(&result, &b, mod, &result);
    }
    *out = result;
}

static int der_len_at(const uint8_t* p, uint32_t max, uint32_t* len, uint32_t* hdr) {
    if (max < 2) return -1;
    if (p[1] < 0x80) { *len = p[1]; *hdr = 2; return 0; }
    uint8_t n = (uint8_t)(p[1] & 0x7f);
    if (n == 0 || n > 4 || max < 2u + n) return -1;
    *len = 0;
    for (uint8_t i = 0; i < n; i++) *len = (*len << 8) | p[2 + i];
    *hdr = 2u + n;
    return 0;
}

bool rsa_parse_pubkey_from_cert(const uint8_t* der, uint32_t der_len, rsa_pubkey_t* out) {
    uint32_t best_n = 0;
    uint8_t nbuf[RSA_MAX_BYTES];
    uint8_t ebuf[8];
    uint32_t nlen = 0, elen = 0;

    for (uint32_t i = 0; i + 4 < der_len; i++) {
        if (der[i] != 0x02) continue;
        uint32_t len, hdr;
        if (der_len_at(der + i, der_len - i, &len, &hdr) < 0) continue;
        if (hdr + len > der_len - i) continue;
        const uint8_t* v = der + i + hdr;
        uint32_t start = (len > 0 && v[0] == 0x00) ? 1 : 0;
        len -= start;
        if (len >= 128 && len <= RSA_MAX_BYTES && len > best_n) {
            memcpy(nbuf, v + start, len);
            nlen = len;
            best_n = len;
        } else if (len >= 1 && len <= 4) {
            uint32_t e = 0;
            for (uint32_t j = 0; j < len; j++) e = (e << 8) | v[start + j];
            if (e == 65537u || e == 3u) {
                memcpy(ebuf, v + start, len);
                elen = len;
            }
        }
    }

    if (!best_n || !elen) return false;
    memcpy(out->n, nbuf, nlen);
    out->n_len = nlen;
    memcpy(out->e, ebuf, elen);
    out->e_len = elen;
    return true;
}

bool rsa_pkcs1_v15_encrypt(const rsa_pubkey_t* pk, const uint8_t* msg, uint32_t msg_len,
                           uint8_t* out, uint32_t* out_len) {
    if (!pk || !msg || !out || !out_len || pk->n_len < 64 || msg_len > pk->n_len - 11)
        return false;

    uint8_t em[RSA_MAX_BYTES];
    uint32_t k = pk->n_len;
    uint32_t ps_len = k - msg_len - 3;
    if (ps_len < 8) return false;

    em[0] = 0x00;
    em[1] = 0x02;
    for (uint32_t i = 0; i < ps_len; i++) {
        uint32_t r = (uint32_t)pit_get_ticks() ^ (i * 0x9e3779b9u);
        em[2 + i] = (uint8_t)((r % 254u) + 1u);
    }
    em[2 + ps_len] = 0x00;
    memcpy(em + 3 + ps_len, msg, msg_len);

    bn_t n, e, m, c;
    bn_from_be(pk->n, pk->n_len, &n);
    bn_from_be(pk->e, pk->e_len, &e);
    bn_from_be(em, k, &m);
    bn_mod_exp(&m, &e, &n, &c);
    bn_to_be(&c, out, k);
    *out_len = k;
    return true;
}
