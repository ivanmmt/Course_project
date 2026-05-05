/* =============================================================================
 * lib/divdi3.c
 * Реализация 64-битного деления для 32-бит bare-metal без libgcc
 *
 * Компилятор i686 генерирует вызовы __udivdi3/__divdi3 при делении
 * 64-битных чисел. Без libgcc они не определены → ошибка линкера.
 * Мы реализуем их вручную через битовый сдвиг.
 * ============================================================================= */

#include <stdint.h>

/* Беззнаковое 64-бит деление */
uint64_t __udivdi3(uint64_t a, uint64_t b) {
    if (b == 0) return 0;  /* защита от деления на 0 */
    if (b > a)  return 0;
    if (b == 1) return a;

    uint64_t quotient = 0;
    uint64_t remainder = 0;

    for (int i = 63; i >= 0; i--) {
        remainder = (remainder << 1) | ((a >> i) & 1);
        if (remainder >= b) {
            remainder -= b;
            quotient |= (uint64_t)1 << i;
        }
    }
    return quotient;
}

/* Остаток от 64-бит деления */
uint64_t __umoddi3(uint64_t a, uint64_t b) {
    if (b == 0) return 0;
    if (b > a)  return a;
    return a - b * __udivdi3(a, b);
}

/* Знаковое 64-бит деление */
int64_t __divdi3(int64_t a, int64_t b) {
    if (b == 0) return 0;
    int neg = (a < 0) ^ (b < 0);
    uint64_t ua = (a < 0) ? (uint64_t)(-a) : (uint64_t)a;
    uint64_t ub = (b < 0) ? (uint64_t)(-b) : (uint64_t)b;
    uint64_t res = __udivdi3(ua, ub);
    return neg ? -(int64_t)res : (int64_t)res;
}

/* Знаковый остаток */
int64_t __moddi3(int64_t a, int64_t b) {
    if (b == 0) return 0;
    int neg = (a < 0);
    uint64_t ua = (a < 0) ? (uint64_t)(-a) : (uint64_t)a;
    uint64_t ub = (b < 0) ? (uint64_t)(-b) : (uint64_t)b;
    uint64_t res = __umoddi3(ua, ub);
    return neg ? -(int64_t)res : (int64_t)res;
}

/* Вспомогательная: умножение 64×64 (может понадобиться для double) */
uint64_t __muldi3(uint64_t a, uint64_t b) {
    uint32_t a_lo = (uint32_t)a, a_hi = (uint32_t)(a >> 32);
    uint32_t b_lo = (uint32_t)b, b_hi = (uint32_t)(b >> 32);
    return (uint64_t)a_lo * b_lo +
           (((uint64_t)a_lo * b_hi + (uint64_t)a_hi * b_lo) << 32);
}

/* Логический правый сдвиг 64-бит */
uint64_t __lshrdi3(uint64_t a, int b) {
    if (b >= 64) return 0;
    if (b >= 32) return (uint64_t)(a >> 32) >> (b - 32);
    return (a >> b);
}

/* Арифметический правый сдвиг 64-бит */
int64_t __ashrdi3(int64_t a, int b) {
    if (b >= 64) return (a < 0) ? -1 : 0;
    if (b >= 32) return (int64_t)((int32_t)(a >> 32) >> (b - 32));
    return (a >> b);
}

/* Левый сдвиг 64-бит */
uint64_t __ashldi3(uint64_t a, int b) {
    if (b >= 64) return 0;
    if (b >= 32) return (a & 0xFFFFFFFFULL) << (b - 32) << 32;
    return (a << b);
}
