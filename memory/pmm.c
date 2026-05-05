/* =============================================================================
 * memory/pmm.c
 * Physical Memory Manager — реализация bitmap-аллокатора страниц
 * ============================================================================= */

#include "pmm.h"
#include "../kernel/kernel.h"
#include "../drivers/vga/vga_text.h"
#include "../lib/string.h"

/* =============================================================================
 * Bitmap физических страниц
 *
 * Максимум поддерживаем 4 ГБ / 4 КБ = 1 048 576 страниц
 * Bitmap = 1 048 576 / 8 = 131 072 байт = 128 КБ
 *
 * Храним bitmap статически — он попадёт в секцию .bss ядра
 * и будет обнулён (все страницы "свободны") при старте.
 * Потом pmm_init пометит занятые области.
 * ============================================================================= */
#define PMM_MAX_PAGES   (1024 * 1024)       /* 4 ГБ / 4 КБ */
#define PMM_BITMAP_SIZE (PMM_MAX_PAGES / 32) /* uint32_t элементов */

static uint32_t pmm_bitmap[PMM_BITMAP_SIZE]; /* 0 = свободна, 1 = занята */

static size_t pmm_total_pages = 0;
static size_t pmm_used_pages  = 0;
static size_t pmm_max_page    = 0; /* верхняя граница физической памяти */

/* =============================================================================
 * Битовые операции над bitmap
 * ============================================================================= */

/* Установить бит (пометить страницу как занятую) */
static inline void bitmap_set(uint32_t page) {
    pmm_bitmap[page / 32] |= (1u << (page % 32));
}

/* Сбросить бит (пометить страницу как свободную) */
static inline void bitmap_clear(uint32_t page) {
    pmm_bitmap[page / 32] &= ~(1u << (page % 32));
}

/* Проверить занята ли страница */
static inline bool bitmap_test(uint32_t page) {
    return (pmm_bitmap[page / 32] & (1u << (page % 32))) != 0;
}

/* =============================================================================
 * Конвертация адрес ↔ номер страницы
 * ============================================================================= */
static inline uint32_t addr_to_page(uint32_t addr) {
    return addr / PMM_PAGE_SIZE;
}

static inline uint32_t page_to_addr(uint32_t page) {
    return page * PMM_PAGE_SIZE;
}

/* =============================================================================
 * pmm_mark_used — пометить диапазон физических адресов как занятый
 * addr и size не обязательно выровнены — мы округляем
 * ============================================================================= */
void pmm_mark_used(uint32_t addr, size_t size) {
    /* Округляем вниз для начала, вверх для конца */
    uint32_t page_start = addr_to_page(addr);
    uint32_t page_end   = addr_to_page(addr + size + PMM_PAGE_SIZE - 1);

    for (uint32_t p = page_start; p < page_end && p < pmm_max_page; p++) {
        if (!bitmap_test(p)) {
            bitmap_set(p);
            pmm_used_pages++;
        }
    }
}

/* =============================================================================
 * pmm_mark_free — пометить диапазон как свободный
 * ============================================================================= */
void pmm_mark_free(uint32_t addr, size_t size) {
    uint32_t page_start = addr_to_page(ALIGN_UP(addr, PMM_PAGE_SIZE));
    uint32_t page_end   = addr_to_page(ALIGN_DOWN(addr + size, PMM_PAGE_SIZE));

    for (uint32_t p = page_start; p < page_end && p < pmm_max_page; p++) {
        if (bitmap_test(p)) {
            bitmap_clear(p);
            pmm_used_pages--;
        }
    }
}

/* =============================================================================
 * pmm_init — инициализация PMM
 *
 * mem_upper_kb: объём верхней памяти в КБ (от GRUB tag type=4)
 * kernel_end:   физический адрес конца ядра (символ из linker.ld)
 * ============================================================================= */
void pmm_init(uint32_t mem_upper_kb, uint32_t kernel_end) {
    /* Общий объём памяти: нижняя (640 КБ) + верхняя */
    uint32_t total_kb    = 640 + mem_upper_kb;
    uint32_t total_bytes = total_kb * 1024;

    pmm_total_pages = total_bytes / PMM_PAGE_SIZE;
    pmm_max_page    = pmm_total_pages;
    pmm_used_pages  = 0;

    /* Шаг 1: помечаем ВСЁ как занятое (безопасная стартовая позиция) */
    memset(pmm_bitmap, 0xFF, sizeof(pmm_bitmap));
    pmm_used_pages = pmm_total_pages;

    /* Шаг 2: освобождаем верхнюю память (начиная с 1 МБ) */
    /* Верхняя память начинается с адреса 0x100000 (1 МБ) */
    uint32_t upper_start = 0x100000;
    uint32_t upper_size  = mem_upper_kb * 1024;
    pmm_mark_free(upper_start, upper_size);

    /* Шаг 3: снова помечаем ядро как занятое */
    /* Ядро загружено начиная с 1 МБ, kernel_end — его конец */
    pmm_mark_used(upper_start, kernel_end - upper_start + PMM_PAGE_SIZE);

    /* Шаг 4: помечаем нижнюю память (0-1МБ) как занятую */
    /* Там находятся: BIOS, VGA, EBDA, таблицы прерываний */
    pmm_mark_used(0x0, 0x100000);

    /* Шаг 5: область DMA (первые 16 МБ часто резервируется) */
    /* Пропускаем первые 4 страницы (16 КБ) для безопасности */
    pmm_mark_used(0x0, PMM_PAGE_SIZE * 4);
}

/* =============================================================================
 * pmm_alloc_page — выделить одну физическую страницу
 *
 * Алгоритм: ищем первый 0-бит в bitmap.
 * Оптимизация: проверяем сразу по 32 бита (uint32_t).
 * Если весь uint32_t == 0xFFFFFFFF — все 32 страницы заняты, пропускаем.
 * ============================================================================= */
uint32_t pmm_alloc_page(void) {
    size_t dwords = pmm_max_page / 32;

    for (size_t i = 0; i < dwords; i++) {
        /* Быстрая проверка: все 32 страницы этого dword заняты? */
        if (pmm_bitmap[i] == 0xFFFFFFFF) continue;

        /* Ищем первый свободный бит */
        for (int bit = 0; bit < 32; bit++) {
            uint32_t page = (uint32_t)(i * 32 + bit);
            if (page >= pmm_max_page) break;

            if (!bitmap_test(page)) {
                bitmap_set(page);
                pmm_used_pages++;
                return page_to_addr(page);
            }
        }
    }

    /* Памяти нет */
    return PMM_ALLOC_ERROR;
}

/* =============================================================================
 * pmm_alloc_pages — выделить N последовательных страниц
 *
 * Нужно для больших буферов (framebuffer, DMA и т.д.)
 * ============================================================================= */
uint32_t pmm_alloc_pages(size_t count) {
    if (count == 0) return PMM_ALLOC_ERROR;
    if (count == 1) return pmm_alloc_page();

    /* Ищем N последовательных свободных страниц */
    size_t   consecutive = 0;
    uint32_t start_page  = 0;

    for (uint32_t p = 0; p < pmm_max_page; p++) {
        if (!bitmap_test(p)) {
            if (consecutive == 0) start_page = p;
            consecutive++;
            if (consecutive == count) {
                /* Нашли! Помечаем все как занятые */
                for (uint32_t i = start_page; i < start_page + count; i++) {
                    bitmap_set(i);
                    pmm_used_pages++;
                }
                return page_to_addr(start_page);
            }
        } else {
            consecutive = 0; /* Последовательность прервалась */
        }
    }

    return PMM_ALLOC_ERROR;
}

/* =============================================================================
 * pmm_free_page — освободить одну страницу
 * ============================================================================= */
void pmm_free_page(uint32_t addr) {
    uint32_t page = addr_to_page(addr);
    if (page >= pmm_max_page) return;

    if (bitmap_test(page)) {
        bitmap_clear(page);
        pmm_used_pages--;
    }
    /* Двойное освобождение — молча игнорируем */
}

void pmm_free_pages(uint32_t addr, size_t count) {
    for (size_t i = 0; i < count; i++) {
        pmm_free_page(addr + (uint32_t)(i * PMM_PAGE_SIZE));
    }
}

/* =============================================================================
 * Статистика
 * ============================================================================= */
size_t pmm_get_free_pages(void)  { return pmm_total_pages - pmm_used_pages; }
size_t pmm_get_used_pages(void)  { return pmm_used_pages; }
size_t pmm_get_total_pages(void) { return pmm_total_pages; }
uint32_t pmm_get_free_kb(void)   { return (uint32_t)(pmm_get_free_pages() * 4); }

void pmm_dump_stats(void) {
    vga_printf("  PMM: total=%u KB  used=%u KB  free=%u KB\n",
        pmm_total_pages * 4,
        pmm_used_pages  * 4,
        pmm_get_free_kb());
}
