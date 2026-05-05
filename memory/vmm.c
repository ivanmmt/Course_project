/* =============================================================================
 * memory/vmm.c
 * Virtual Memory Manager — включение paging, управление PD/PT
 * ============================================================================= */

#include "vmm.h"
#include "pmm.h"
#include "../kernel/kernel.h"
#include "../drivers/vga/vga_text.h"
#include "../lib/string.h"

/* =============================================================================
 * Глобальный Page Directory ядра
 * Выровнен на 4 КБ — обязательное требование x86
 * Располагается в .bss → инициализируется нулями автоматически
 * ============================================================================= */
static pde_t kernel_pd[VMM_PD_ENTRIES] __attribute__((aligned(VMM_PAGE_SIZE)));

/* Пул Page Tables для ядра
 * Создаём заранее PT для первых 256 МБ (64 PT × 4 МБ = 256 МБ)
 * В продакшн-ОС PT создаются динамически, но для учебных целей
 * такой пул проще и надёжнее. */
#define VMM_KERNEL_PT_COUNT  64
static pte_t kernel_pts[VMM_KERNEL_PT_COUNT][VMM_PT_ENTRIES]
    __attribute__((aligned(VMM_PAGE_SIZE)));

/* =============================================================================
 * vmm_get_or_create_pt — получить Page Table для виртуального адреса
 *
 * Если PDE для этого адреса не существует — создаём новую PT.
 * Возвращает указатель на PT (виртуальный адрес).
 * ============================================================================= */
static pte_t* vmm_get_or_create_pt(uint32_t virt, uint32_t flags) {
    uint32_t pd_idx = VMM_PD_INDEX(virt);
    pde_t    pde    = kernel_pd[pd_idx];

    if (pde & VMM_FLAG_PRESENT) {
        /* PDE уже существует — берём адрес PT из него */
        return (pte_t*)(VMM_PHYS_ADDR(pde));
    }

    /* PDE не существует — нужно создать новую PT */

    /* Используем PT из пула если ещё есть место */
    static uint32_t pt_pool_idx = 0;
    if (pt_pool_idx < VMM_KERNEL_PT_COUNT) {
        pte_t* pt = kernel_pts[pt_pool_idx++];
        memset(pt, 0, VMM_PAGE_SIZE);

        /* Запись в PDE: физ. адрес PT + флаги */
        kernel_pd[pd_idx] = ((uint32_t)pt) | flags | VMM_FLAG_PRESENT;
        return pt;
    }

    /* Пул исчерпан — выделяем физическую страницу динамически */
    uint32_t pt_phys = pmm_alloc_page();
    if (pt_phys == PMM_ALLOC_ERROR) {
        return (pte_t*)0; /* OOM */
    }

    memset((void*)pt_phys, 0, VMM_PAGE_SIZE);
    kernel_pd[pd_idx] = pt_phys | flags | VMM_FLAG_PRESENT;
    return (pte_t*)pt_phys;
}

/* =============================================================================
 * vmm_map_page — маппировать виртуальную страницу на физическую
 * ============================================================================= */
void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    /* Выравниваем оба адреса на границу страницы */
    virt &= VMM_PAGE_MASK;
    phys &= VMM_PAGE_MASK;

    pte_t* pt = vmm_get_or_create_pt(virt, flags);
    if (!pt) return; /* OOM — в реальной ОС здесь kernel panic */

    uint32_t pt_idx = VMM_PT_INDEX(virt);
    pt[pt_idx] = phys | flags | VMM_FLAG_PRESENT;

    /* Сбрасываем TLB для этого адреса */
    vmm_flush_tlb(virt);
}

/* =============================================================================
 * vmm_unmap_page — убрать маппинг
 * ============================================================================= */
void vmm_unmap_page(uint32_t virt) {
    uint32_t pd_idx = VMM_PD_INDEX(virt);
    pde_t    pde    = kernel_pd[pd_idx];

    if (!(pde & VMM_FLAG_PRESENT)) return;

    pte_t*   pt     = (pte_t*)(VMM_PHYS_ADDR(pde));
    uint32_t pt_idx = VMM_PT_INDEX(virt);
    pt[pt_idx] = 0;  /* очищаем PTE */

    vmm_flush_tlb(virt);
}

/* =============================================================================
 * vmm_get_physical — получить физический адрес для виртуального
 * ============================================================================= */
uint32_t vmm_get_physical(uint32_t virt) {
    uint32_t pd_idx = VMM_PD_INDEX(virt);
    pde_t    pde    = kernel_pd[pd_idx];

    if (!(pde & VMM_FLAG_PRESENT)) return 0;

    pte_t*   pt     = (pte_t*)(VMM_PHYS_ADDR(pde));
    uint32_t pt_idx = VMM_PT_INDEX(virt);
    pte_t    pte    = pt[pt_idx];

    if (!(pte & VMM_FLAG_PRESENT)) return 0;

    return VMM_PHYS_ADDR(pte) | VMM_OFFSET(virt);
}

/* =============================================================================
 * vmm_is_mapped
 * ============================================================================= */
bool vmm_is_mapped(uint32_t virt) {
    uint32_t pd_idx = VMM_PD_INDEX(virt);
    if (!(kernel_pd[pd_idx] & VMM_FLAG_PRESENT)) return false;

    pte_t*   pt     = (pte_t*)(VMM_PHYS_ADDR(kernel_pd[pd_idx]));
    uint32_t pt_idx = VMM_PT_INDEX(virt);
    return (pt[pt_idx] & VMM_FLAG_PRESENT) != 0;
}

/* =============================================================================
 * vmm_init — инициализация виртуальной памяти
 *
 * Стратегия: Identity Mapping (виртуальный == физический)
 *
 * Маппируем:
 * 1. Первые 4 МБ (включает ядро, стек, VGA, BIOS)
 * 2. Всё ядро от 0x100000 до kernel_end
 * 3. VGA текстовый буфер 0xB8000
 *
 * Потом включаем бит PG в CR0.
 * ============================================================================= */

/* Символ из linker.ld — адрес конца ядра */
extern uint32_t kernel_end;

void vmm_init(void) {
    /* Шаг 1: Очищаем Page Directory */
    memset(kernel_pd, 0, sizeof(kernel_pd));

    /* Шаг 2: Identity map первых 4 МБ
     * Зачем: ядро грузится в 1 МБ, VGA в 0xB8000 — всё в первых 4 МБ.
     * Без этого маппинга после включения paging процессор сразу
     * получит Page Fault при попытке выполнить следующую инструкцию! */
    uint32_t map_end = 4 * 1024 * 1024; /* 4 МБ */

    /* Если ядро больше 4 МБ — маппируем до его конца */
    uint32_t kern_end_addr = (uint32_t)&kernel_end;
    if (kern_end_addr > map_end) {
        map_end = ALIGN_UP(kern_end_addr, VMM_PAGE_SIZE);
    }

    for (uint32_t addr = 0; addr < map_end; addr += VMM_PAGE_SIZE) {
        vmm_map_page(addr, addr, VMM_KERNEL_FLAGS);
    }

    /* Шаг 3: Маппируем VGA текстовый буфер */
    /* 0xB8000 уже попадает в первые 4 МБ, но добавим явно для ясности */
    vmm_map_page(0xB8000, 0xB8000, VMM_KERNEL_FLAGS);

    /* Шаг 4: Загружаем наш PD в CR3 */
    vmm_load_cr3((uint32_t)kernel_pd);

    /* Шаг 5: Включаем paging — бит 31 в CR0
     *
     * ВАЖНО: между записью CR3 и установкой CR0.PG не должно быть
     * ничего, что обращается к памяти через виртуальные адреса.
     * Инструкции в конвейере уже заряжены, поэтому identity mapping
     * гарантирует что следующая инструкция после mov cr0 найдётся
     * по тому же адресу что и ожидает EIP.
     */
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;  /* PG бит */
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0) : "memory");

    /* После этой строки процессор работает с виртуальными адресами. */
}
