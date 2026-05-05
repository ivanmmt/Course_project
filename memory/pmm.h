/* =============================================================================
 * memory/pmm.h
 * Physical Memory Manager — менеджер физической памяти
 *
 * ИДЕЯ:
 * Вся физическая RAM делится на страницы по 4 КБ.
 * Мы ведём bitmap: 1 бит = 1 страница (1=занята, 0=свободна).
 * При 128 МБ RAM → 32768 страниц → bitmap = 4096 байт (4 КБ).
 *
 * ПОЧЕМУ BITMAP:
 * Простота и скорость. Поиск свободной страницы = поиск нулевого бита,
 * что делается за O(N/32) операций с uint32_t.
 * ============================================================================= */

#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Размер страницы физической памяти */
#define PMM_PAGE_SIZE       4096
#define PMM_PAGES_PER_DWORD 32

/* Специальный адрес "ошибка выделения" */
#define PMM_ALLOC_ERROR     ((uint32_t)0xFFFFFFFF)

/* --- API --- */

/* Инициализировать PMM: сканируем память от GRUB и строим bitmap */
void     pmm_init(uint32_t mem_upper_kb, uint32_t kernel_end);

/* Выделить одну физическую страницу (4 КБ), вернуть физ. адрес */
uint32_t pmm_alloc_page(void);

/* Выделить N последовательных страниц */
uint32_t pmm_alloc_pages(size_t count);

/* Освободить страницу по физическому адресу */
void     pmm_free_page(uint32_t addr);

/* Освободить N страниц начиная с addr */
void     pmm_free_pages(uint32_t addr, size_t count);

/* Пометить диапазон как занятый (ядро, MMIO и т.д.) */
void     pmm_mark_used(uint32_t addr, size_t size);

/* Пометить диапазон как свободный */
void     pmm_mark_free(uint32_t addr, size_t size);

/* Статистика */
size_t   pmm_get_free_pages(void);
size_t   pmm_get_used_pages(void);
size_t   pmm_get_total_pages(void);
uint32_t pmm_get_free_kb(void);

/* Отладочный дамп состояния */
void     pmm_dump_stats(void);

#endif /* PMM_H */
