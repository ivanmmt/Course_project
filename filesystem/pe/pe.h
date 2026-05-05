/* =============================================================================
 * filesystem/pe/pe.h
 * PE (Portable Executable) загрузчик — поддержка Windows .exe файлов
 *
 * ФОРМАТ PE (упрощённо):
 *
 *   ┌──────────────────────────────────┐
 *   │  DOS Header (64 байта)           │ ← "MZ" магия + смещение до PE
 *   │  DOS Stub (маленькая программа)  │ ← "This program cannot be run..."
 *   ├──────────────────────────────────┤
 *   │  PE Signature "PE\0\0" (4 байта) │
 *   ├──────────────────────────────────┤
 *   │  COFF File Header (20 байт)      │ ← архитектура, число секций
 *   ├──────────────────────────────────┤
 *   │  Optional Header (224 байта)     │ ← точка входа, ImageBase, размеры
 *   ├──────────────────────────────────┤
 *   │  Section Headers []              │ ← .text .data .rdata .bss
 *   ├──────────────────────────────────┤
 *   │  Секции данных                   │
 *   └──────────────────────────────────┘
 *
 * МЫ ПОДДЕРЖИВАЕМ:
 *   - PE32 (32-bit x86, машина 0x014C)
 *   - Статически скомпилированные .exe (без импортов DLL)
 *   - Перемещение (relocation) если ImageBase занята
 *
 * НЕ ПОДДЕРЖИВАЕМ (слишком сложно для учебной ОС):
 *   - DLL импорты (нет kernel32.dll, ntdll.dll)
 *   - .NET / managed code
 *   - PE+ (64-bit)
 * ============================================================================= */

#ifndef PE_H
#define PE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ─── DOS заголовок (MZ header) ─── */
typedef struct {
    uint16_t magic;         /* 0x5A4D = "MZ" */
    uint16_t last_page_bytes;
    uint16_t pages;
    uint16_t relocations;
    uint16_t header_paragraphs;
    uint16_t min_extra;
    uint16_t max_extra;
    uint16_t ss;
    uint16_t sp;
    uint16_t checksum;
    uint16_t ip;
    uint16_t cs;
    uint16_t reloc_offset;
    uint16_t overlay;
    uint8_t  reserved[32];
    uint32_t pe_offset;     /* смещение PE заголовка в файле */
} __attribute__((packed)) pe_dos_header_t;

/* ─── COFF File Header ─── */
typedef struct {
    uint16_t machine;           /* 0x014C = i386 */
    uint16_t num_sections;
    uint32_t timestamp;
    uint32_t symbol_table;
    uint32_t num_symbols;
    uint16_t optional_header_size;
    uint16_t characteristics;
} __attribute__((packed)) pe_coff_header_t;

/* ─── Машины ─── */
#define PE_MACHINE_I386   0x014C
#define PE_MACHINE_AMD64  0x8664

/* ─── Характеристики файла ─── */
#define PE_CHAR_EXECUTABLE   0x0002
#define PE_CHAR_32BIT        0x0100
#define PE_CHAR_DLL          0x2000

/* ─── Optional Header (PE32) ─── */
typedef struct {
    /* Standard fields */
    uint16_t magic;             /* 0x010B = PE32, 0x020B = PE32+ */
    uint8_t  major_linker;
    uint8_t  minor_linker;
    uint32_t code_size;
    uint32_t init_data_size;
    uint32_t uninit_data_size;
    uint32_t entry_point;       /* RVA точки входа */
    uint32_t base_of_code;
    uint32_t base_of_data;
    /* Windows-specific fields */
    uint32_t image_base;        /* предпочтительный базовый адрес (обычно 0x400000) */
    uint32_t section_alignment; /* выравнивание секций в памяти */
    uint32_t file_alignment;    /* выравнивание в файле */
    uint16_t major_os;
    uint16_t minor_os;
    uint16_t major_image;
    uint16_t minor_image;
    uint16_t major_subsystem;
    uint16_t minor_subsystem;
    uint32_t win32_version;
    uint32_t image_size;        /* полный размер образа в памяти */
    uint32_t headers_size;
    uint32_t checksum;
    uint16_t subsystem;         /* 1=native, 2=GUI, 3=console */
    uint16_t dll_characteristics;
    uint32_t stack_reserve;
    uint32_t stack_commit;
    uint32_t heap_reserve;
    uint32_t heap_commit;
    uint32_t loader_flags;
    uint32_t num_rva_sizes;
    /* Data directories (16 штук) */
    struct {
        uint32_t rva;
        uint32_t size;
    } data_dir[16];
} __attribute__((packed)) pe_optional_header_t;

/* ─── Section Header ─── */
typedef struct {
    char     name[8];           /* название секции (.text, .data, ...) */
    uint32_t virtual_size;      /* размер в памяти */
    uint32_t virtual_address;   /* RVA (относительно ImageBase) */
    uint32_t raw_size;          /* размер в файле */
    uint32_t raw_offset;        /* смещение в файле */
    uint32_t reloc_offset;
    uint32_t lineno_offset;
    uint16_t num_relocs;
    uint16_t num_linenos;
    uint32_t characteristics;   /* флаги секции */
} __attribute__((packed)) pe_section_header_t;

/* Флаги секций */
#define PE_SCN_CODE       0x00000020  /* исполняемый код */
#define PE_SCN_INIT_DATA  0x00000040  /* инициализированные данные */
#define PE_SCN_UNINIT     0x00000080  /* неинициализированные (BSS) */
#define PE_SCN_EXEC       0x20000000  /* исполняемая */
#define PE_SCN_READ       0x40000000  /* читаемая */
#define PE_SCN_WRITE      0x80000000  /* записываемая */

/* ─── Индексы таблицы данных ─── */
#define PE_DIR_EXPORT    0
#define PE_DIR_IMPORT    1
#define PE_DIR_BASERELOC 5   /* таблица релокаций */

/* ─── Запись релокации ─── */
typedef struct {
    uint32_t page_rva;  /* RVA страницы */
    uint32_t block_size;
    /* за ним: массив uint16_t (тип | смещение_на_странице) */
} __attribute__((packed)) pe_basereloc_block_t;

#define PE_RELOC_ABSOLUTE  0  /* пропустить */
#define PE_RELOC_HIGHLOW   3  /* прибавить delta к 32-битному значению */

/* ─── Полный PE заголовок ─── */
typedef struct {
    uint32_t             signature;   /* "PE\0\0" */
    pe_coff_header_t     coff;
    pe_optional_header_t opt;
} __attribute__((packed)) pe_header_t;

/* ─── Результат загрузки ─── */
typedef struct {
    bool     valid;
    uint32_t entry;        /* абсолютный адрес точки входа */
    uint32_t image_base;   /* реальный базовый адрес (после релокации) */
    uint32_t image_size;
} pe_info_t;

/* ─── API ─── */

/* Проверить является ли буфер валидным PE32 */
bool pe_validate(const uint8_t* data, uint32_t size);

/* Загрузить PE в память */
pe_info_t pe_load(const uint8_t* data, uint32_t size);

/* Загрузить PE из VFS файла */
pe_info_t pe_load_file(const char* path);

/* Запустить загруженный PE */
int pe_exec(pe_info_t* info, int argc, char** argv);

/* Отладочный вывод заголовков */
void pe_dump_info(const uint8_t* data, uint32_t size);

#endif /* PE_H */
