/* =============================================================================
 * drivers/ata/ata.c
 * ATA PIO Mode драйвер — чтение/запись секторов по 512 байт
 *
 * PIO (Programmed I/O): CPU напрямую читает/пишет данные через порты.
 * Медленнее DMA, но проще для учебных целей — не нужны прерывания.
 *
 * ПРОТОКОЛ ЧТЕНИЯ (ATA PIO Read):
 *   1. Подождать BSY=0
 *   2. Выбрать устройство (master/slave) + LBA биты 24-27
 *   3. Записать количество секторов в SECCOUNT
 *   4. Записать LBA[0-7], LBA[8-15], LBA[16-23]
 *   5. Послать команду READ (0x20)
 *   6. Для каждого сектора:
 *      a. Ждать DRQ=1 (данные готовы)
 *      b. Читать 256 слов (512 байт) из порта DATA (0x1F0)
 * ============================================================================= */

#include "ata.h"
#include "../../kernel/kernel.h"
#include "../../drivers/vga/vga_text.h"
#include "../../lib/string.h"

/* Максимум 4 устройства: 2 канала × 2 устройства */
#define ATA_MAX_DRIVES  4
static ata_drive_t drives[ATA_MAX_DRIVES];
static int drive_count = 0;

/* =============================================================================
 * Низкоуровневые вспомогательные функции
 * ============================================================================= */

static inline uint8_t ata_status(ata_drive_t* d) {
    return inb((uint16_t)(d->base + ATA_REG_STATUS));
}

static inline void ata_select(ata_drive_t* d, uint8_t extra) {
    outb((uint16_t)(d->base + ATA_REG_HDDEVSEL),
         (uint8_t)(d->slave ? ATA_SEL_SLAVE : ATA_SEL_MASTER) | ATA_SEL_LBA | extra);
}

/* 400нс задержка — читаем Alternate Status 4 раза (каждое чтение ~100нс) */
static void ata_delay400ns(ata_drive_t* d) {
    inb(d->ctrl); inb(d->ctrl); inb(d->ctrl); inb(d->ctrl);
}

/* =============================================================================
 * ata_wait_ready — ждать BSY=0 и (DRQ=1 или DRDY=1)
 * Возвращает false если таймаут или ошибка
 * ============================================================================= */
bool ata_wait_ready(ata_drive_t* d) {
    uint32_t timeout = 100000;
    uint8_t status;

    /* Ждём пока BSY снимется */
    do {
        status = ata_status(d);
        if (--timeout == 0) return false;
    } while (status & ATA_SR_BSY);

    /* Проверяем ошибку */
    if (status & (ATA_SR_ERR | ATA_SR_DF)) return false;

    return true;
}

/* Ждём DRQ (данные готовы к передаче) */
static bool ata_wait_drq(ata_drive_t* d) {
    uint32_t timeout = 100000;
    uint8_t status;
    do {
        status = ata_status(d);
        if (status & ATA_SR_ERR) return false;
        if (--timeout == 0) return false;
    } while (!(status & ATA_SR_DRQ));
    return true;
}

/* =============================================================================
 * ata_identify — отправить IDENTIFY, прочитать 512 байт информации
 * Заполняет d->info
 * ============================================================================= */
static bool ata_identify(ata_drive_t* d) {
    /* Выбираем устройство */
    ata_select(d, 0);
    ata_delay400ns(d);

    /* Обнуляем LBA регистры */
    outb((uint16_t)(d->base + ATA_REG_SECCOUNT), 0);
    outb((uint16_t)(d->base + ATA_REG_LBA0), 0);
    outb((uint16_t)(d->base + ATA_REG_LBA1), 0);
    outb((uint16_t)(d->base + ATA_REG_LBA2), 0);

    /* IDENTIFY команда */
    outb((uint16_t)(d->base + ATA_REG_COMMAND), ATA_CMD_IDENTIFY);
    ata_delay400ns(d);

    /* Если статус == 0 — устройства нет */
    uint8_t status = ata_status(d);
    if (status == 0) return false;

    /* Ждём снятия BSY */
    if (!ata_wait_ready(d)) return false;

    /* Проверяем что это ATA (не ATAPI) */
    uint8_t lba1 = inb((uint16_t)(d->base + ATA_REG_LBA1));
    uint8_t lba2 = inb((uint16_t)(d->base + ATA_REG_LBA2));
    if (lba1 != 0 || lba2 != 0) return false;   /* ATAPI — не поддерживаем */

    if (!ata_wait_drq(d)) return false;

    /* Читаем 256 слов (512 байт) идентификационных данных */
    uint16_t identify_data[256];
    for (int i = 0; i < 256; i++)
        identify_data[i] = inw((uint16_t)(d->base + ATA_REG_DATA));

    /* Извлекаем информацию */
    d->info.present  = true;
    d->info.lba48    = (identify_data[83] & (1 << 10)) != 0;

    /* Общее количество LBA28 секторов (слова 60-61) */
    d->info.total_sectors_lba28 =
        ((uint32_t)identify_data[61] << 16) | identify_data[60];

    /* LBA48 (слова 100-103) */
    d->info.total_sectors_lba48 =
        ((uint64_t)identify_data[100]) |
        ((uint64_t)identify_data[101] << 16) |
        ((uint64_t)identify_data[102] << 32) |
        ((uint64_t)identify_data[103] << 48);

    /* Модель (слова 27-46, big-endian в каждом слове) */
    for (int i = 0; i < 20; i++) {
        uint16_t w = identify_data[27 + i];
        d->info.model[i*2]     = (char)(w >> 8);
        d->info.model[i*2 + 1] = (char)(w & 0xFF);
    }
    d->info.model[40] = '\0';
    /* Убираем хвостовые пробелы */
    for (int i = 39; i >= 0 && d->info.model[i] == ' '; i--)
        d->info.model[i] = '\0';

    /* Серийный номер (слова 10-19) */
    for (int i = 0; i < 10; i++) {
        uint16_t w = identify_data[10 + i];
        d->info.serial[i*2]     = (char)(w >> 8);
        d->info.serial[i*2 + 1] = (char)(w & 0xFF);
    }
    d->info.serial[20] = '\0';
    for (int i = 19; i >= 0 && d->info.serial[i] == ' '; i--)
        d->info.serial[i] = '\0';

    return true;
}

/* =============================================================================
 * ata_read_sectors — чтение секторов в PIO режиме
 * ============================================================================= */
int ata_read_sectors(ata_drive_t* d, uint32_t lba,
                     uint8_t count, uint8_t* buffer) {
    if (!d || !d->info.present || count == 0) return -1;
    if (lba + count > d->info.total_sectors_lba28) return -1;

    /* Выбираем устройство + биты LBA[24:27] */
    ata_select(d, (uint8_t)((lba >> 24) & 0x0F));
    ata_delay400ns(d);

    if (!ata_wait_ready(d)) return -1;

    /* Настраиваем параметры */
    outb((uint16_t)(d->base + ATA_REG_SECCOUNT), count);
    outb((uint16_t)(d->base + ATA_REG_LBA0), (uint8_t)(lba & 0xFF));
    outb((uint16_t)(d->base + ATA_REG_LBA1), (uint8_t)((lba >> 8) & 0xFF));
    outb((uint16_t)(d->base + ATA_REG_LBA2), (uint8_t)((lba >> 16) & 0xFF));

    /* Отправляем команду READ PIO */
    outb((uint16_t)(d->base + ATA_REG_COMMAND), ATA_CMD_READ_PIO);

    /* Читаем count секторов */
    uint16_t* buf16 = (uint16_t*)buffer;
    for (int s = 0; s < count; s++) {
        if (!ata_wait_drq(d)) return s;  /* вернём сколько успели прочитать */

        /* 256 слов = 512 байт */
        for (int i = 0; i < 256; i++)
            buf16[s * 256 + i] = inw((uint16_t)(d->base + ATA_REG_DATA));

        ata_delay400ns(d);
    }
    return count;
}

/* =============================================================================
 * ata_write_sectors — запись секторов в PIO режиме
 * ============================================================================= */
int ata_write_sectors(ata_drive_t* d, uint32_t lba,
                      uint8_t count, const uint8_t* buffer) {
    if (!d || !d->info.present || count == 0) return -1;
    if (lba + count > d->info.total_sectors_lba28) return -1;

    ata_select(d, (uint8_t)((lba >> 24) & 0x0F));
    ata_delay400ns(d);

    if (!ata_wait_ready(d)) return -1;

    outb((uint16_t)(d->base + ATA_REG_SECCOUNT), count);
    outb((uint16_t)(d->base + ATA_REG_LBA0), (uint8_t)(lba & 0xFF));
    outb((uint16_t)(d->base + ATA_REG_LBA1), (uint8_t)((lba >> 8) & 0xFF));
    outb((uint16_t)(d->base + ATA_REG_LBA2), (uint8_t)((lba >> 16) & 0xFF));

    outb((uint16_t)(d->base + ATA_REG_COMMAND), ATA_CMD_WRITE_PIO);

    const uint16_t* buf16 = (const uint16_t*)buffer;
    for (int s = 0; s < count; s++) {
        if (!ata_wait_drq(d)) return s;

        for (int i = 0; i < 256; i++)
            outw((uint16_t)(d->base + ATA_REG_DATA), buf16[s * 256 + i]);

        ata_delay400ns(d);
    }

    /* Flush cache */
    outb((uint16_t)(d->base + ATA_REG_COMMAND), ATA_CMD_FLUSH_CACHE);
    ata_wait_ready(d);

    return count;
}

/* =============================================================================
 * ata_probe_channel — сканировать один канал (master + slave)
 * ============================================================================= */
static void ata_probe_channel(uint16_t base, uint16_t ctrl) {
    for (int slave = 0; slave <= 1 && drive_count < ATA_MAX_DRIVES; slave++) {
        ata_drive_t* d = &drives[drive_count];
        d->base  = base;
        d->ctrl  = ctrl;
        d->slave = (uint8_t)slave;
        memset(&d->info, 0, sizeof(d->info));

        if (ata_identify(d)) {
            drive_count++;
        }
    }
}

/* =============================================================================
 * ata_init — инициализация: сканируем оба канала
 * ============================================================================= */
void ata_init(void) {
    drive_count = 0;
    memset(drives, 0, sizeof(drives));

    /* Отключаем прерывания ATA (используем polling) */
    outb(ATA_PRIMARY_CTRL,   0x02);   /* nIEN = 1 */
    outb(ATA_SECONDARY_CTRL, 0x02);

    ata_probe_channel(ATA_PRIMARY_BASE,   ATA_PRIMARY_CTRL);
    ata_probe_channel(ATA_SECONDARY_BASE, ATA_SECONDARY_CTRL);
}

int ata_get_count(void) { return drive_count; }

ata_drive_t* ata_get_drive(int index) {
    if (index < 0 || index >= drive_count) return (ata_drive_t*)0;
    return &drives[index];
}

void ata_dump_info(void) {
    if (drive_count == 0) {
        vga_put_string("  ATA: no drives found\n");
        return;
    }
    for (int i = 0; i < drive_count; i++) {
        ata_drive_t* d = &drives[i];
        uint32_t mb = d->info.total_sectors_lba28 / 2048;
        vga_printf("  ATA%d: %s [%s] %u MB LBA28%s\n",
            i,
            d->slave ? "Slave" : "Master",
            d->info.model[0] ? d->info.model : "Unknown",
            mb,
            d->info.lba48 ? " +LBA48" : "");
    }
}
