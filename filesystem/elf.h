/* =============================================================================
 * filesystem/elf.h
 * ELF32 загрузчик — загрузка и запуск исполняемых файлов
 *
 * ELF (Executable and Linkable Format) — стандартный формат бинарных файлов
 * на Linux/Unix. Наша ОС будет поддерживать ELF32 (32-бит x86).
 *
 * СТРУКТУРА ELF файла:
 *
 *   ┌─────────────────────────┐
 *   │  ELF Header (52 байта)  │  ← магия, архитектура, точка входа
 *   ├─────────────────────────┤
 *   │  Program Headers        │  ← сегменты (LOAD, DYNAMIC, ...)
 *   ├─────────────────────────┤
 *   │  .text  (код)           │  ← исполняемый код
 *   ├─────────────────────────┤
 *   │  .data  (данные)        │  ← инициализированные переменные
 *   ├─────────────────────────┤
 *   │  .bss   (пусто)         │  ← неинициализированные (только размер)
 *   ├─────────────────────────┤
 *   │  Section Headers        │  ← для отладки/линкера
 *   └─────────────────────────┘
 *
 * Загрузчик читает Program Headers типа PT_LOAD и копирует сегменты
 * в память по нужным адресам (p_vaddr), затем прыгает на e_entry.
 * ============================================================================= */

#ifndef ELF_H
#define ELF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ─── ELF магическое число ─── */
#define ELF_MAGIC       0x464C457Fu  /* "\x7FELF" в little-endian */

/* ─── ELF классы ─── */
#define ELF_CLASS_32    1   /* 32-бит ELF */
#define ELF_CLASS_64    2   /* 64-бит */

/* ─── Порядок байт ─── */
#define ELF_DATA_LSB    1   /* Little-Endian (x86) */

/* ─── Типы ELF файлов ─── */
#define ET_NONE         0
#define ET_REL          1   /* перемещаемый (объектный файл) */
#define ET_EXEC         2   /* исполняемый */
#define ET_DYN          3   /* разделяемая библиотека */

/* ─── Архитектуры ─── */
#define EM_386          3   /* Intel 80386 */
#define EM_X86_64       62  /* AMD x86-64 */

/* ─── Типы сегментов (Program Header) ─── */
#define PT_NULL         0   /* игнорировать */
#define PT_LOAD         1   /* загружаемый сегмент */
#define PT_DYNAMIC      2   /* динамическая линковка */
#define PT_INTERP       3   /* путь к интерпретатору */
#define PT_NOTE         4

/* ─── Флаги сегментов ─── */
#define PF_X            (1 << 0)  /* Execute */
#define PF_W            (1 << 1)  /* Write */
#define PF_R            (1 << 2)  /* Read */

/* ─── ELF32 заголовок (52 байта) ─── */
typedef struct {
    uint8_t  e_ident[16];   /* магия + класс + порядок байт + версия */
    uint16_t e_type;         /* тип файла (ET_EXEC и т.д.) */
    uint16_t e_machine;      /* архитектура (EM_386 = 3) */
    uint32_t e_version;      /* версия ELF (всегда 1) */
    uint32_t e_entry;        /* точка входа (виртуальный адрес main) */
    uint32_t e_phoff;        /* смещение таблицы Program Headers */
    uint32_t e_shoff;        /* смещение таблицы Section Headers */
    uint32_t e_flags;        /* флаги архитектуры */
    uint16_t e_ehsize;       /* размер этого заголовка (52) */
    uint16_t e_phentsize;    /* размер одного Program Header (32) */
    uint16_t e_phnum;        /* количество Program Headers */
    uint16_t e_shentsize;    /* размер одного Section Header */
    uint16_t e_shnum;        /* количество Section Headers */
    uint16_t e_shstrndx;     /* индекс секции имён строк */
} __attribute__((packed)) elf32_header_t;

/* ─── ELF32 Program Header (32 байта) ─── */
typedef struct {
    uint32_t p_type;    /* тип сегмента (PT_LOAD и т.д.) */
    uint32_t p_offset;  /* смещение в файле */
    uint32_t p_vaddr;   /* виртуальный адрес загрузки */
    uint32_t p_paddr;   /* физический адрес (обычно = vaddr) */
    uint32_t p_filesz;  /* размер в файле */
    uint32_t p_memsz;   /* размер в памяти (>= filesz, остаток = BSS) */
    uint32_t p_flags;   /* флаги PF_R/W/X */
    uint32_t p_align;   /* выравнивание */
} __attribute__((packed)) elf32_phdr_t;

/* ─── ELF32 Section Header (40 байт) ─── */
typedef struct {
    uint32_t sh_name;       /* индекс имени в таблице строк */
    uint32_t sh_type;       /* тип секции */
    uint32_t sh_flags;      /* флаги */
    uint32_t sh_addr;       /* адрес в памяти */
    uint32_t sh_offset;     /* смещение в файле */
    uint32_t sh_size;       /* размер */
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} __attribute__((packed)) elf32_shdr_t;

/* ─── Информация о загруженной программе ─── */
typedef struct {
    uint32_t entry;         /* точка входа */
    uint32_t load_base;     /* базовый адрес загрузки */
    uint32_t load_end;      /* конец занятой памяти */
    bool     valid;         /* успешно загружен */
} elf_info_t;

/* ─── Прототип функции точки входа ─── */
typedef int (*elf_entry_t)(int argc, char** argv);

/* ─── API ─── */

/* Проверить является ли буфер валидным ELF32 */
bool elf_validate(const uint8_t* data, uint32_t size);

/* Загрузить ELF из буфера в память */
elf_info_t elf_load(const uint8_t* data, uint32_t size);

/* Загрузить ELF из VFS файла */
elf_info_t elf_load_file(const char* path);

/* Запустить загруженный ELF */
int elf_exec(elf_info_t* info, int argc, char** argv);

/* Распечатать заголовки ELF (для отладки) */
void elf_dump_info(const uint8_t* data, uint32_t size);

#endif /* ELF_H */
