/* =============================================================================
 * lib/string.c
 * Реализация строковых функций для ядра
 * ============================================================================= */

#include "string.h"

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

char* strcpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++));
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

int strcmp(const char* a, const char* b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char* strcat(char* dst, const char* src) {
    char* d = dst + strlen(dst);
    while ((*d++ = *src++));
    return dst;
}

char* strncat(char* dst, const char* src, size_t n) {
    char* d = dst + strlen(dst);
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dst;
}

char* strchr(const char* s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return (c == '\0') ? (char*)s : (char*)0;
}

void* memset(void* ptr, int value, size_t size) {
    uint8_t* p = (uint8_t*)ptr;
    while (size--) *p++ = (uint8_t)value;
    return ptr;
}

void* memcpy(void* dst, const void* src, size_t size) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (size--) *d++ = *s++;
    return dst;
}

void* memmove(void* dst, const void* src, size_t size) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d < s) {
        while (size--) *d++ = *s++;
    } else {
        d += size; s += size;
        while (size--) *--d = *--s;
    }
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}

int itoa(int n, char* buf, int base) {
    char tmp[32]; int i = 0, neg = 0;
    if (n < 0 && base == 10) { neg = 1; n = -n; }
    if (n == 0) { buf[0]='0'; buf[1]='\0'; return 1; }
    while (n > 0) { tmp[i++] = "0123456789abcdef"[n % base]; n /= base; }
    int j = 0;
    if (neg) buf[j++] = '-';
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
    return j;
}

/* Simple snprintf implementation for basic format strings */
int snprintf(char* buf, size_t size, const char* fmt, ...) {
    if (!buf || size == 0) return 0;
    
    va_list args;
    va_start(args, fmt);
    
    size_t pos = 0;
    const char* p = fmt;
    
    while (*p && pos < size - 1) {
        if (*p == '%' && *(p+1)) {
            p++;
            char spec = *p;
            
            if (spec == 's') {
                /* String argument */
                const char* s = va_arg(args, const char*);
                if (!s) s = "(null)";
                while (*s && pos < size - 1) {
                    buf[pos++] = *s++;
                }
            } else if (spec == 'd' || spec == 'i') {
                /* Integer argument */
                int n = va_arg(args, int);
                char tmp[32];
                int len = itoa(n, tmp, 10);
                for (int i = 0; i < len && pos < size - 1; i++) {
                    buf[pos++] = tmp[i];
                }
            } else if (spec == 'x' || spec == 'X') {
                /* Hex argument */
                unsigned int n = va_arg(args, unsigned int);
                char tmp[32];
                int len = itoa((int)n, tmp, 16);
                for (int i = 0; i < len && pos < size - 1; i++) {
                    buf[pos++] = (spec == 'X') ? tmp[i] : tmp[i];
                }
            } else if (spec == '%') {
                /* Literal % */
                buf[pos++] = '%';
            } else {
                /* Unknown spec, just copy it */
                buf[pos++] = '%';
                if (pos < size - 1) buf[pos++] = spec;
            }
            p++;
        } else if (*p == '\\' && *(p+1)) {
            /* Escape sequence */
            p++;
            if (*p == 'n') buf[pos++] = '\n';
            else if (*p == 't') buf[pos++] = '\t';
            else if (*p == 'r') buf[pos++] = '\r';
            else buf[pos++] = *p;
            p++;
        } else {
            buf[pos++] = *p++;
        }
    }
    
    buf[pos] = '\0';
    va_end(args);
    return (int)pos;
}
