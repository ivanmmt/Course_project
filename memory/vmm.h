/* =============================================================================
 * memory/vmm.h
 * Virtual Memory Manager — управление виртуальной памятью (Paging)
 *
 * КАК РАБОТАЕТ PAGING В x86:
 *
 * Процессор переводит виртуальный адрес → физический через 2 уровня таблиц:
 *
 *   Виртуальный адрес (32 бита):
 *   [31..22] = индекс в Page Directory (10 бит, 1024 записи)
 *   [21..12] = индекс в Page Table    (10 бит, 1024 записи)
 *   [11..0]  = смещение внутри страницы (12 бит, 0-4095)
 *
 *   Page Directory (PD): массив из 1024 PDE, каждый 4 байта
 *     └─ Каждый PDE указывает на Page Table (PT)
 *        Page Table (PT): массив из 1024 PTE, каждый 4 байта
 *          └─ Каждый PTE указывает на физическую страницу (4 КБ)
 *
 * Итого: PD (4 КБ) + 1024 PT (каждая 4 КБ) = 4 МБ для маппинга 4 ГБ
 * На практике PT создаются лениво — только когда нужны.
 *
 * CR3: регистр процессора, содержит физический адрес Page Directory.
 * При переключении процесса меняем CR3.
 * ============================================================================= */

#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Размер страницы */
#define VMM_PAGE_SIZE   4096
#define VMM_PAGE_MASK   0xFFFFF000  /* маска для выравнивания на страницу */

/* Количество записей в PD и PT */
#define VMM_PD_ENTRIES  1024
#define VMM_PT_ENTRIES  1024

/* ─── Флаги PDE / PTE ─── */
#define VMM_FLAG_PRESENT    (1 << 0)  /* страница/таблица существует */
#define VMM_FLAG_WRITABLE   (1 << 1)  /* разрешена запись */
#define VMM_FLAG_USER       (1 << 2)  /* доступна из ring 3 */
#define VMM_FLAG_ACCESSED   (1 << 5)  /* процессор обращался */
#define VMM_FLAG_DIRTY      (1 << 6)  /* страница была изменена */
#define VMM_FLAG_HUGE       (1 << 7)  /* 4МБ страница (PSE) */

/* Удобные комбинации флагов */
#define VMM_KERNEL_FLAGS  (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE)
#define VMM_USER_FLAGS    (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER)

/* Извлечение индексов из виртуального адреса */
#define VMM_PD_INDEX(va)   (((uint32_t)(va)) >> 22)
#define VMM_PT_INDEX(va)   ((((uint32_t)(va)) >> 12) & 0x3FF)
#define VMM_OFFSET(va)     (((uint32_t)(va)) & 0xFFF)

/* Физический адрес из PDE/PTE (маскируем флаги) */
#define VMM_PHYS_ADDR(entry) ((entry) & VMM_PAGE_MASK)

/* Тип для Page Directory и Page Table записей */
typedef uint32_t pde_t;  /* Page Directory Entry */
typedef uint32_t pte_t;  /* Page Table Entry */

/* =============================================================================
 * Виртуальное адресное пространство ядра
 *
 * 0x00000000 - 0xBFFFFFFF: пользовательское пространство (3 ГБ)
 * 0xC0000000 - 0xFFFFFFFF: пространство ядра (1 ГБ)
 *
 * Мы используем identity mapping для простоты:
 * виртуальный адрес = физическому (1:1 mapping)
 * Это упрощает Step 3, в шагах с процессами добавим полноценный mapping.
 * ============================================================================= */
#define VMM_KERNEL_VIRT_BASE  0x00000000  /* identity map */
#define VMM_USER_SPACE_END    0xBFFFFFFF

/* --- API --- */

/* Инициализировать paging: создать PD ядра, включить CR0.PG */
void vmm_init(void);

/* Маппировать виртуальную страницу на физическую */
void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);

/* Размаппировать виртуальную страницу */
void vmm_unmap_page(uint32_t virt);

/* Получить физический адрес для виртуального (или 0 если не маппировано) */
uint32_t vmm_get_physical(uint32_t virt);

/* Проверить маппирован ли адрес */
bool vmm_is_mapped(uint32_t virt);

/* Сбросить TLB для адреса (после изменения PTE) */
static inline void vmm_flush_tlb(uint32_t virt) {
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}

/* Загрузить Page Directory в CR3 */
static inline void vmm_load_cr3(uint32_t pd_phys) {
    __asm__ volatile ("mov %0, %%cr3" :: "r"(pd_phys) : "memory");
}

/* Получить текущий CR3 */
static inline uint32_t vmm_get_cr3(void) {
    uint32_t val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

#endif /* VMM_H */
