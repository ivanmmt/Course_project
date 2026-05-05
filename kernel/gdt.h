/* =============================================================================
 * kernel/gdt.h
 * Global Descriptor Table (GDT) — таблица дескрипторов сегментов
 *
 * GDT — это таблица которая описывает сегменты памяти в x86 защищённом режиме.
 * Каждый дескриптор задаёт: базу сегмента, лимит, права доступа и флаги.
 *
 * Нам нужны минимум 3 дескриптора:
 *   0: нулевой (обязателен по спецификации x86)
 *   1: сегмент кода ядра (ring 0, исполняемый)
 *   2: сегмент данных ядра (ring 0, читаемый/записываемый)
 *
 * Без правильной GDT ничего другого работать не будет.
 * ============================================================================= */

#ifndef GDT_H
#define GDT_H

#include <stdint.h>

/* Количество дескрипторов в нашей GDT */
#define GDT_ENTRIES 5

/* --- Структура одного дескриптора GDT (8 байт) --- */
/* x86 использует хитрый формат: поля разбросаны для обратной совместимости */
typedef struct {
    uint16_t limit_low;     /* биты 0-15 лимита сегмента */
    uint16_t base_low;      /* биты 0-15 базы сегмента */
    uint8_t  base_mid;      /* биты 16-23 базы */
    uint8_t  access;        /* байт доступа: тип, уровень привилегий */
    uint8_t  granularity;   /* биты 16-19 лимита + флаги (32/64-бит, гранулярность) */
    uint8_t  base_high;     /* биты 24-31 базы */
} __attribute__((packed)) gdt_entry_t;

/* --- Указатель на GDT (загружается инструкцией LGDT) --- */
typedef struct {
    uint16_t limit;         /* размер GDT - 1 (в байтах) */
    uint32_t base;          /* физический адрес GDT */
} __attribute__((packed)) gdt_ptr_t;

/* --- Байт доступа (access byte) --- */
/* Бит 7: Present (1 = дескриптор активен) */
/* Биты 5-6: DPL — уровень привилегий (0=ядро, 3=пользователь) */
/* Бит 4: Descriptor type (1=сегментный, 0=системный) */
/* Биты 0-3: тип сегмента */
#define GDT_ACCESS_PRESENT    (1 << 7)
#define GDT_ACCESS_RING0      (0 << 5)
#define GDT_ACCESS_RING3      (3 << 5)
#define GDT_ACCESS_SEGMENT    (1 << 4)
#define GDT_ACCESS_EXECUTABLE (1 << 3)  /* сегмент кода */
#define GDT_ACCESS_DC         (1 << 2)  /* direction/conforming */
#define GDT_ACCESS_RW         (1 << 1)  /* readable(код)/writable(данные) */
#define GDT_ACCESS_ACCESSED   (1 << 0)  /* процессор ставит при обращении */

/* Готовые байты доступа для типичных сегментов */
#define GDT_CODE_KERNEL  (GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | \
                          GDT_ACCESS_SEGMENT | GDT_ACCESS_EXECUTABLE | \
                          GDT_ACCESS_RW)
#define GDT_DATA_KERNEL  (GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | \
                          GDT_ACCESS_SEGMENT | GDT_ACCESS_RW)
#define GDT_CODE_USER    (GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 | \
                          GDT_ACCESS_SEGMENT | GDT_ACCESS_EXECUTABLE | \
                          GDT_ACCESS_RW)
#define GDT_DATA_USER    (GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 | \
                          GDT_ACCESS_SEGMENT | GDT_ACCESS_RW)

/* --- Байт гранулярности (granularity byte) --- */
/* Бит 7: Granularity — 0=лимит в байтах, 1=лимит в страницах (4КБ) */
/* Бит 6: Size — 0=16-бит сегмент, 1=32-бит сегмент */
/* Бит 5: Long mode (для 64-бит) */
/* Биты 0-3: биты 16-19 лимита */
#define GDT_GRAN_4K    (1 << 7)    /* гранулярность 4KB страниц */
#define GDT_GRAN_32    (1 << 6)    /* 32-битный сегмент */
#define GDT_GRAN_16    0           /* 16-битный сегмент */

/* Селекторы сегментов — индекс*8 (биты 3+ = индекс, биты 0-1 = RPL) */
#define GDT_KERNEL_CODE_SEL  0x08   /* индекс 1, ring 0 */
#define GDT_KERNEL_DATA_SEL  0x10   /* индекс 2, ring 0 */
#define GDT_USER_CODE_SEL    0x18   /* индекс 3, ring 3 */
#define GDT_USER_DATA_SEL    0x20   /* индекс 4, ring 3 */

/* --- API --- */
void gdt_init(void);

/* Объявлена в gdt_flush.asm — загружает GDT и обновляет сегментные регистры */
extern void gdt_flush(uint32_t gdt_ptr);

#endif /* GDT_H */
