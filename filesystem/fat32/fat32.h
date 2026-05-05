/* =============================================================================
 * filesystem/fat32/fat32.h
 * FAT32 File System Driver
 *
 * СТРУКТУРА FAT32 РАЗДЕЛА:
 *
 *   ┌──────────────────────────────────────┐
 *   │  Сектор 0: Boot Record (BPB)         │ ← параметры ФС
 *   ├──────────────────────────────────────┤
 *   │  Зарезервированные секторы           │
 *   ├──────────────────────────────────────┤
 *   │  FAT #1 (File Allocation Table)      │ ← таблица цепочек кластеров
 *   ├──────────────────────────────────────┤
 *   │  FAT #2 (резервная копия)            │
 *   ├──────────────────────────────────────┤
 *   │  Область данных (кластеры 2, 3, ...) │ ← файлы и директории
 *   └──────────────────────────────────────┘
 *
 * КЛАСТЕР: минимальная единица хранения (обычно 4-64 КБ).
 * Файл занимает цепочку кластеров: 2 → 5 → 8 → EOC (End Of Chain).
 * FAT хранит эту цепочку: FAT[2]=5, FAT[5]=8, FAT[8]=0x0FFFFFFF (EOC).
 *
 * ДИРЕКТОРИЯ: файл содержащий массив 32-байтных записей (dirent).
 * Каждая запись — имя, атрибуты, начальный кластер, размер.
 * ============================================================================= */

#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../../drivers/ata/ata.h"
#include "../vfs.h"
#include "../../drivers/usb/usbmass.h"

/* ─── Специальные значения FAT32 ─── */
#define FAT32_EOC       0x0FFFFFF8u  /* End Of Chain (≥ этого = последний) */
#define FAT32_BAD       0x0FFFFFF7u  /* плохой кластер */
#define FAT32_FREE      0x00000000u  /* свободный кластер */
#define FAT32_CLUSTER_MASK 0x0FFFFFFFu

/* ─── Атрибуты файлов ─── */
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F   /* Long File Name entry */

/* ─── BPB (BIOS Parameter Block) — Boot Record FAT32 ─── */
typedef struct {
    uint8_t  jmp[3];             /* 0x00: прыжок на загрузчик */
    uint8_t  oem[8];             /* 0x03: OEM идентификатор */
    uint16_t bytes_per_sector;   /* 0x0B: байт в секторе (обычно 512) */
    uint8_t  sectors_per_cluster;/* 0x0D: секторов в кластере */
    uint16_t reserved_sectors;   /* 0x0E: зарезервированных секторов */
    uint8_t  num_fats;           /* 0x10: количество FAT таблиц (обычно 2) */
    uint16_t root_entry_count;   /* 0x11: 0 для FAT32 */
    uint16_t total_sectors_16;   /* 0x13: 0 для FAT32 */
    uint8_t  media_type;         /* 0x15: тип носителя */
    uint16_t fat_size_16;        /* 0x16: 0 для FAT32 */
    uint16_t sectors_per_track;  /* 0x18: для CHS */
    uint16_t num_heads;          /* 0x1A: для CHS */
    uint32_t hidden_sectors;     /* 0x1C: скрытых секторов */
    uint32_t total_sectors_32;   /* 0x20: всего секторов */
    /* FAT32 Extended BPB (начиная с 0x24) */
    uint32_t fat_size_32;        /* 0x24: секторов в одной FAT */
    uint16_t ext_flags;          /* 0x28: флаги */
    uint16_t fs_version;         /* 0x2A: версия (0.0) */
    uint32_t root_cluster;       /* 0x2C: кластер корневой директории */
    uint16_t fs_info;            /* 0x30: сектор FSInfo */
    uint16_t backup_boot;        /* 0x32: сектор резервного загрузчика */
    uint8_t  reserved[12];       /* 0x34 */
    uint8_t  drive_number;       /* 0x40 */
    uint8_t  reserved1;          /* 0x41 */
    uint8_t  boot_signature;     /* 0x42: 0x29 если serial+label+fstype присутствуют */
    uint32_t volume_id;          /* 0x43: серийный номер */
    uint8_t  volume_label[11];   /* 0x47: метка тома */
    uint8_t  fs_type[8];         /* 0x52: "FAT32   " */
} __attribute__((packed)) fat32_bpb_t;

/* ─── Запись директории (32 байта) ─── */
typedef struct {
    uint8_t  name[8];            /* 8.3 имя */
    uint8_t  ext[3];             /* расширение */
    uint8_t  attr;               /* атрибуты (FAT_ATTR_*) */
    uint8_t  reserved;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t last_access_date;
    uint16_t first_cluster_hi;   /* старшие 16 бит кластера */
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_lo;   /* младшие 16 бит кластера */
    uint32_t file_size;          /* размер в байтах */
} __attribute__((packed)) fat32_dirent_t;

/* ─── LFN запись (Long File Name, 32 байта) ─── */
typedef struct {
    uint8_t  order;              /* порядковый номер (0x40 = последняя) */
    uint16_t name1[5];           /* UCS-2 символы 1-5 */
    uint8_t  attr;               /* всегда FAT_ATTR_LFN = 0x0F */
    uint8_t  type;               /* тип (0) */
    uint8_t  checksum;           /* контрольная сумма short name */
    uint16_t name2[6];           /* символы 6-11 */
    uint16_t first_cluster;      /* всегда 0 */
    uint16_t name3[2];           /* символы 12-13 */
} __attribute__((packed)) fat32_lfn_t;

/* ─── Внутреннее состояние смонтированной FAT32 ─── */
typedef struct {
    ata_drive_t* drive;           /* физический диск (ATA) или USB mass storage */
    uint32_t     part_start;      /* начальный LBA раздела */
    fat32_bpb_t  bpb;             /* копия BPB */
    bool         is_usb;          /* true если USB устройство */

    /* Вычисленные поля */
    uint32_t     fat_start;       /* LBA первой FAT */
    uint32_t     data_start;      /* LBA области данных */
    uint32_t     root_cluster;    /* кластер корня */
    uint32_t     sectors_per_cluster;
    uint32_t     bytes_per_cluster;
    uint32_t     total_clusters;

    /* Кэш одного сектора FAT */
    uint8_t      fat_cache[512];
    uint32_t     fat_cache_sector; /* какой сектор в кэше */
    bool         fat_cache_valid;
    bool         fat_cache_dirty;
} fat32_fs_t;

/* ─── API ─── */

/* Монтировать FAT32 с диска (part_start = LBA начала раздела, 0 = без MBR) */
vfs_node_t* fat32_mount(ata_drive_t* drive, uint32_t part_start);

/* Монтировать FAT32 с USB устройства */
bool fat32_mount_usb(const char* mount_point, usb_mass_storage_t* ms);

/* Размонтировать (сбрасываем кэши на диск) */
void fat32_unmount(fat32_fs_t* fs);

/* Форматировать раздел как FAT32 */
bool fat32_format(ata_drive_t* drive, uint32_t part_start,
                  uint32_t total_sectors, const char* label);

/* Прочитать кластер данных в буфер */
int fat32_read_cluster(fat32_fs_t* fs, uint32_t cluster, uint8_t* buf);

/* Получить следующий кластер в цепочке (из FAT) */
uint32_t fat32_next_cluster(fat32_fs_t* fs, uint32_t cluster);

/* Распечатать дерево файлов (отладка) */
void fat32_dump_root(fat32_fs_t* fs);

#endif /* FAT32_H */
