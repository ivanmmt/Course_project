/* =============================================================================
 * memory/heap.h
 * Kernel Heap — динамическое выделение памяти (kmalloc / kfree)
 *
 * АЛГОРИТМ: Free List с заголовками блоков
 *
 * Память ядра делится на блоки. Каждый блок имеет заголовок:
 *
 *   [  heap_block_t  ][      данные       ][  heap_block_t  ][  данные  ]...
 *    ^--- header           size байт        ^--- header
 *
 * Заголовок хранит:
 *   - размер блока данных
 *   - флаг is_free
 *   - указатели на следующий/предыдущий блок (двусвязный список)
 *
 * СТРАТЕГИЯ ПОИСКА: First Fit — берём первый подходящий свободный блок.
 * Быстро и достаточно хорошо для ядра.
 *
 * СЛИЯНИЕ: при kfree сливаем соседние свободные блоки (coalescing),
 * чтобы не было фрагментации.
 * ============================================================================= */

#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Начальный размер кучи ядра: 4 МБ */
#define HEAP_INITIAL_SIZE   (4 * 1024 * 1024)

/* Минимальный размер блока (меньше нет смысла делать) */
#define HEAP_MIN_BLOCK_SIZE 16

/* Выравнивание выделяемой памяти */
#define HEAP_ALIGNMENT      8

/* Магическое число для проверки целостности заголовка */
#define HEAP_MAGIC          0xDEADBEEF

/* Заголовок блока кучи */
typedef struct heap_block {
    uint32_t           magic;    /* 0xDEADBEEF — проверка на corruption */
    size_t             size;     /* размер блока данных (без заголовка) */
    bool               is_free;  /* свободен ли блок */
    struct heap_block* next;     /* следующий блок */
    struct heap_block* prev;     /* предыдущий блок */
} heap_block_t;

/* --- API --- */
void  heap_init(void);
void* kmalloc(size_t size);
void* kcalloc(size_t count, size_t size);    /* malloc + zeroing */
void* krealloc(void* ptr, size_t new_size);
void  kfree(void* ptr);

/* Статистика кучи */
void   heap_dump_stats(void);
size_t heap_get_used(void);
size_t heap_get_free(void);

#endif /* HEAP_H */
