/* =============================================================================
 * lib/string.h
 * Базовые строковые функции для ядра (замена libc)
 * ============================================================================= */

#ifndef STRING_H
#define STRING_H

#include <stddef.h>
#include <stdint.h>

size_t  strlen(const char* s);
char*   strcpy(char* dst, const char* src);
char*   strncpy(char* dst, const char* src, size_t n);
int     strcmp(const char* a, const char* b);
int     strncmp(const char* a, const char* b, size_t n);
char*   strcat(char* dst, const char* src);
char*   strncat(char* dst, const char* src, size_t n);
char*   strchr(const char* s, int c);

void*   memset(void* ptr, int value, size_t size);
void*   memcpy(void* dst, const void* src, size_t size);
void*   memmove(void* dst, const void* src, size_t size);
int     memcmp(const void* a, const void* b, size_t n);

int     itoa(int n, char* buf, int base);

#endif /* STRING_H */
