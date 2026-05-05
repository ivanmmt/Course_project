/* =============================================================================
 * drivers/ata/ata.h
 * ATA/IDE Disk Driver — драйвер жёсткого диска
 *
 * КАК РАБОТАЕТ ATA:
 * ATA (Advanced Technology Attachment) — стандарт подключения HDD/SSD.
 * Контроллер доступен через порты ввода-вывода x86.
 *
 * В системе обычно два канала (Primary / Secondary),
 * на каждом канале до двух устройств (Master / Slave).
 *
 * Порты Primary канала:
 *   0x1F0-0x1F7 — данные и команды
 *   0x3F6       — статус/управление
 *
 * Мы реализуем:
 *   - PIO Mode (Programmed I/O) — простейший режим, без DMA
 *   - 28-бит LBA адресация (поддерживает до 128 ГБ)
 *   - Чтение и запись секторов (512 байт каждый)
 *   - Идентификация устройств (IDENTIFY command)
 * ============================================================================= */

#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ─── Порты ATA ─── */
#define ATA_PRIMARY_BASE    0x1F0   /* Data, Error/Features, Sector Count, LBA, Command */
#define ATA_PRIMARY_CTRL    0x3F6   /* Device Control / Alternate Status */
#define ATA_SECONDARY_BASE  0x170
#define ATA_SECONDARY_CTRL  0x376

/* Смещения от BASE */
#define ATA_REG_DATA        0   /* 16-бит: данные */
#define ATA_REG_ERROR       1   /* чтение: ошибка */
#define ATA_REG_FEATURES    1   /* запись: функции */
#define ATA_REG_SECCOUNT    2   /* количество секторов */
#define ATA_REG_LBA0        3   /* LBA биты 0-7 */
#define ATA_REG_LBA1        4   /* LBA биты 8-15 */
#define ATA_REG_LBA2        5   /* LBA биты 16-23 */
#define ATA_REG_HDDEVSEL    6   /* выбор устройства + LBA биты 24-27 */
#define ATA_REG_COMMAND     7   /* запись: команда */
#define ATA_REG_STATUS      7   /* чтение: статус */

/* Биты статусного регистра */
#define ATA_SR_BSY   0x80   /* контроллер занят */
#define ATA_SR_DRDY  0x40   /* устройство готово */
#define ATA_SR_DF    0x20   /* ошибка устройства */
#define ATA_SR_DSC   0x10   /* поиск завершён */
#define ATA_SR_DRQ   0x08   /* данные готовы к передаче */
#define ATA_SR_CORR  0x04   /* исправленная ошибка */
#define ATA_SR_IDX   0x02   /* индекс */
#define ATA_SR_ERR   0x01   /* ошибка */

/* Команды ATA */
#define ATA_CMD_READ_PIO      0x20   /* читать сектора (PIO) */
#define ATA_CMD_WRITE_PIO     0x30   /* писать сектора (PIO) */
#define ATA_CMD_IDENTIFY      0xEC   /* идентификация устройства */
#define ATA_CMD_FLUSH_CACHE   0xE7   /* сбросить кэш на диск */

/* Биты регистра выбора устройства (HDDEVSEL) */
#define ATA_SEL_MASTER  0xA0    /* Primary Master: 0b10100000 */
#define ATA_SEL_SLAVE   0xB0    /* Primary Slave:  0b10110000 */
#define ATA_SEL_LBA     0x40    /* режим LBA адресации */

/* Информация об устройстве из IDENTIFY */
typedef struct {
    bool     present;           /* устройство найдено */
    bool     lba48;             /* поддержка 48-бит LBA */
    uint16_t cylinders;
    uint16_t heads;
    uint16_t sectors_per_track;
    uint32_t total_sectors_lba28;  /* суммарно секторов в LBA28 режиме */
    uint64_t total_sectors_lba48;
    char     model[41];         /* строка модели */
    char     serial[21];        /* серийный номер */
} ata_device_t;

/* ─── Дескриптор диска ─── */
typedef struct {
    uint16_t     base;       /* базовый порт (0x1F0 или 0x170) */
    uint16_t     ctrl;       /* порт управления */
    uint8_t      slave;      /* 0 = master, 1 = slave */
    ata_device_t info;       /* информация из IDENTIFY */
} ata_drive_t;

/* ─── API ─── */

/* Инициализация: сканировать оба канала, заполнить таблицу дисков */
void ata_init(void);

/* Получить количество найденных дисков */
int  ata_get_count(void);

/* Получить указатель на дескриптор диска (0-3: pri.master, pri.slave, sec.master, sec.slave) */
ata_drive_t* ata_get_drive(int index);

/* Чтение секторов в буфер (PIO Mode)
 * drive:    дескриптор диска
 * lba:      логический номер блока (начиная с 0)
 * count:    количество секторов (1-255)
 * buffer:   буфер размером count*512 байт
 * Возвращает количество прочитанных секторов или -1 при ошибке */
int ata_read_sectors(ata_drive_t* drive, uint32_t lba,
                     uint8_t count, uint8_t* buffer);

/* Запись секторов из буфера на диск */
int ata_write_sectors(ata_drive_t* drive, uint32_t lba,
                      uint8_t count, const uint8_t* buffer);

/* Ожидать готовности (без BSY и с DRQ или DRDY) */
bool ata_wait_ready(ata_drive_t* drive);

/* Напечатать информацию о найденных дисках */
void ata_dump_info(void);

#endif /* ATA_H */
