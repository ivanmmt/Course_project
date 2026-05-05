/* =============================================================================
 * memory/heap.c
 * Реализация кучи ядра: kmalloc / kfree с двусвязным списком блоков
 * ============================================================================= */

#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "../kernel/kernel.h"
#include "../drivers/vga/vga_text.h"
#include "../lib/string.h"

/* =============================================================================
 * Куча располагается сразу после ядра.
 * Размещаем её по фиксированному виртуальному адресу.
 * В реальной ОС куча росла бы через вызов типа sbrk().
 *
 * Мы выделим начальный пул через PMM и зарезервируем адресное пространство.
 * ============================================================================= */
#define HEAP_START_ADDR  0x01000000  /* 16 МБ — куча начинается тут */

static heap_block_t* heap_start = (heap_block_t*)HEAP_START_ADDR;
static heap_block_t* heap_end   = (heap_block_t*)HEAP_START_ADDR;
static size_t        heap_total = 0;

/* =============================================================================
 * heap_expand — расширить кучу, добавив страницы физической памяти
 * ============================================================================= */
static bool heap_expand(size_t bytes) {
    size_t pages = (bytes + VMM_PAGE_SIZE - 1) / VMM_PAGE_SIZE;

    for (size_t i = 0; i < pages; i++) {
        uint32_t phys = pmm_alloc_page();
        if (phys == PMM_ALLOC_ERROR) return false;

        uint32_t virt = HEAP_START_ADDR + heap_total + (uint32_t)(i * VMM_PAGE_SIZE);
        vmm_map_page(virt, phys, VMM_KERNEL_FLAGS);
    }

    heap_total += pages * VMM_PAGE_SIZE;
    return true;
}

/* =============================================================================
 * heap_init — инициализировать кучу
 * ============================================================================= */
void heap_init(void) {
    /* Выделяем начальный пул страниц */
    size_t pages = HEAP_INITIAL_SIZE / VMM_PAGE_SIZE;

    for (size_t i = 0; i < pages; i++) {
        uint32_t phys = pmm_alloc_page();
        if (phys == PMM_ALLOC_ERROR) break;

        uint32_t virt = HEAP_START_ADDR + (uint32_t)(i * VMM_PAGE_SIZE);
        vmm_map_page(virt, phys, VMM_KERNEL_FLAGS);
        heap_total += VMM_PAGE_SIZE;
    }

    /* Создаём один большой свободный блок на всю кучу */
    heap_start = (heap_block_t*)HEAP_START_ADDR;
    heap_start->magic   = HEAP_MAGIC;
    heap_start->size    = heap_total - sizeof(heap_block_t);
    heap_start->is_free = true;
    heap_start->next    = (heap_block_t*)0;
    heap_start->prev    = (heap_block_t*)0;
    heap_end = heap_start;
}

/* =============================================================================
 * Вспомогательные функции
 * ============================================================================= */

/* Проверить целостность заголовка */
static inline bool block_valid(heap_block_t* b) {
    return b && b->magic == HEAP_MAGIC;
}

/* Данные блока = адрес сразу после заголовка */
static inline void* block_data(heap_block_t* b) {
    return (void*)((uint8_t*)b + sizeof(heap_block_t));
}

/* Заголовок блока = адрес перед данными */
static inline heap_block_t* data_to_block(void* ptr) {
    return (heap_block_t*)((uint8_t*)ptr - sizeof(heap_block_t));
}

/* Следующий блок в памяти (по адресу, не по linked list) */
static inline heap_block_t* next_block_by_addr(heap_block_t* b) {
    return (heap_block_t*)((uint8_t*)b + sizeof(heap_block_t) + b->size);
}

/* =============================================================================
 * split_block — разбить блок на два если он слишком большой
 *
 * [  block (size=N)  ] →  [  block (size=need)  ][  new_block (size=N-need-header)  ]
 * ============================================================================= */
static void split_block(heap_block_t* block, size_t need) {
    /* Стоит ли вообще делить? Если остаток < min — нет */
    size_t remainder = block->size - need - sizeof(heap_block_t);
    if (remainder < HEAP_MIN_BLOCK_SIZE) return;

    /* Создаём новый блок для остатка */
    heap_block_t* new_block = (heap_block_t*)((uint8_t*)block_data(block) + need);
    new_block->magic   = HEAP_MAGIC;
    new_block->size    = remainder;
    new_block->is_free = true;
    new_block->next    = block->next;
    new_block->prev    = block;

    if (block->next) block->next->prev = new_block;
    block->next = new_block;
    block->size = need;

    /* Обновляем указатель на конец кучи */
    if (heap_end == block) heap_end = new_block;
}

/* =============================================================================
 * coalesce — слить блок с соседями если они свободны
 * Вызывается после kfree для борьбы с фрагментацией
 * ============================================================================= */
static void coalesce(heap_block_t* block) {
    /* Слияние с СЛЕДУЮЩИМ блоком */
    if (block->next && block_valid(block->next) && block->next->is_free) {
        block->size += sizeof(heap_block_t) + block->next->size;
        if (heap_end == block->next) heap_end = block;
        block->next = block->next->next;
        if (block->next) block->next->prev = block;
    }

    /* Слияние с ПРЕДЫДУЩИМ блоком */
    if (block->prev && block_valid(block->prev) && block->prev->is_free) {
        block->prev->size += sizeof(heap_block_t) + block->size;
        if (heap_end == block) heap_end = block->prev;
        block->prev->next = block->next;
        if (block->next) block->next->prev = block->prev;
    }
}

/* =============================================================================
 * kmalloc — выделить size байт, вернуть указатель на данные
 *
 * Алгоритм First Fit:
 *   1. Проходим список блоков
 *   2. Берём первый свободный блок с size >= запрошенного
 *   3. Делим его если он намного больше
 *   4. Если не нашли — расширяем кучу
 * ============================================================================= */
void* kmalloc(size_t size) {
    if (size == 0) return (void*)0;

    /* Выравниваем размер на HEAP_ALIGNMENT */
    size = ALIGN_UP(size, HEAP_ALIGNMENT);

    /* Ищем подходящий свободный блок (First Fit) */
    heap_block_t* current = heap_start;
    while (current) {
        if (!block_valid(current)) {
            /* Повреждение кучи! */
            kernel_panic("Heap corruption detected in kmalloc!");
        }

        if (current->is_free && current->size >= size) {
            /* Нашли! Делим если нужно */
            split_block(current, size);
            current->is_free = false;
            return block_data(current);
        }

        current = current->next;
    }

    /* Свободного блока нет — расширяем кучу */
    size_t expand = size + sizeof(heap_block_t);
    if (expand < VMM_PAGE_SIZE) expand = VMM_PAGE_SIZE;

    if (!heap_expand(expand)) {
        return (void*)0; /* OOM */
    }

    /* Создаём новый блок в конце кучи */
    heap_block_t* new_block = next_block_by_addr(heap_end);
    new_block->magic   = HEAP_MAGIC;
    new_block->size    = expand - sizeof(heap_block_t);
    new_block->is_free = true;
    new_block->next    = (heap_block_t*)0;
    new_block->prev    = heap_end;
    heap_end->next = new_block;
    heap_end = new_block;

    /* Рекурсивно пробуем выделить снова */
    return kmalloc(size);
}

/* =============================================================================
 * kcalloc — выделить и обнулить
 * ============================================================================= */
void* kcalloc(size_t count, size_t size) {
    size_t total = count * size;
    void* ptr = kmalloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

/* =============================================================================
 * krealloc — изменить размер выделенного блока
 * ============================================================================= */
void* krealloc(void* ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return (void*)0; }

    heap_block_t* block = data_to_block(ptr);
    if (!block_valid(block)) {
        kernel_panic("krealloc: invalid pointer!");
    }

    /* Если блок уже достаточно большой */
    if (block->size >= new_size) {
        split_block(block, new_size);
        return ptr;
    }

    /* Выделяем новый, копируем, освобождаем старый */
    void* new_ptr = kmalloc(new_size);
    if (!new_ptr) return (void*)0;

    memcpy(new_ptr, ptr, block->size);
    kfree(ptr);
    return new_ptr;
}

/* =============================================================================
 * kfree — освободить блок
 * ============================================================================= */
void kfree(void* ptr) {
    if (!ptr) return;

    heap_block_t* block = data_to_block(ptr);

    if (!block_valid(block)) {
        kernel_panic("kfree: invalid or corrupted pointer!");
    }

    if (block->is_free) {
        kernel_panic("kfree: double free detected!");
    }

    block->is_free = true;

    /* Сливаем с соседними свободными блоками */
    coalesce(block);
}

/* =============================================================================
 * Статистика
 * ============================================================================= */
size_t heap_get_used(void) {
    size_t used = 0;
    heap_block_t* b = heap_start;
    while (b) {
        if (!b->is_free) used += b->size;
        b = b->next;
    }
    return used;
}

size_t heap_get_free(void) {
    size_t free_sz = 0;
    heap_block_t* b = heap_start;
    while (b) {
        if (b->is_free) free_sz += b->size;
        b = b->next;
    }
    return free_sz;
}

void heap_dump_stats(void) {
    size_t total_blocks = 0, free_blocks = 0;
    heap_block_t* b = heap_start;
    while (b) {
        total_blocks++;
        if (b->is_free) free_blocks++;
        b = b->next;
    }

    vga_printf("  Heap: total=%u KB  used=%u KB  free=%u KB  blocks=%u (free=%u)\n",
        heap_total / 1024,
        heap_get_used() / 1024,
        heap_get_free() / 1024,
        (uint32_t)total_blocks,
        (uint32_t)free_blocks);
}
