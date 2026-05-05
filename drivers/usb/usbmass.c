/* =============================================================================
 * drivers/usb/usbmass.c
 * USB Mass Storage Class Driver Implementation
 * ============================================================================= */

#include "usbmass.h"
#include "usb.h"
#include "../../lib/string.h"
#include "../../drivers/vga/vga_text.h"
#include "../../memory/heap.h"

/* CBW (Command Block Wrapper) */
typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t  flags;
    uint8_t  lun;
    uint8_t  cmd_length;
    uint8_t  cmd_data[16];
} __attribute__((packed)) usb_cbw_t;

/* CSW (Command Status Wrapper) */
typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_residue;
    uint8_t  status;
} __attribute__((packed)) usb_csw_t;

/* Forward declaration */
static bool usbmass_execute_command(usb_mass_storage_t* ms, const uint8_t* cmd,
                                   uint8_t cmd_len, uint8_t* data, uint32_t data_len);

/* Initialize mass storage device */
bool usbmass_init(usb_mass_storage_t* ms) {
    if (!ms || !ms->present) return false;

    vga_put_string("  [USBMASS] Initializing USB mass storage...\n");

    /* Test unit ready */
    uint8_t cmd[6] = {SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0};
    if (!usbmass_execute_command(ms, cmd, sizeof(cmd), NULL, 0)) {
        vga_put_string("  [USBMASS] Device not ready\n");
        return false;
    }

    vga_put_string("  [USBMASS] USB mass storage ready\n");
    return true;
}

/* Execute SCSI command */
static bool usbmass_execute_command(usb_mass_storage_t* ms, const uint8_t* cmd,
                                   uint8_t cmd_len, uint8_t* data, uint32_t data_len) {
    /* In real implementation, this would send CBW, data, receive CSW */
    /* For QEMU stub, just simulate success */
    return true;
}

/* Read sector */
bool usbmass_read_sector(usb_mass_storage_t* ms, uint32_t sector, uint8_t* buffer) {
    if (!ms || !buffer) return false;

    /* Simulate reading sector data */
    /* In real implementation, this would send READ_10 command */
    memset(buffer, 0, 512);

    /* Create fake FAT32 boot sector for testing */
    if (sector == 0) {
        buffer[0] = 0xEB; buffer[1] = 0x58; buffer[2] = 0x90; // JMP
        memcpy(&buffer[3], "MSDOS5.0", 8); // OEM name
        *(uint16_t*)&buffer[11] = 512; // bytes per sector
        buffer[13] = 1; // sectors per cluster
        *(uint16_t*)&buffer[14] = 1; // reserved sectors
        buffer[16] = 2; // FAT copies
        *(uint16_t*)&buffer[17] = 512; // root entries (FAT16 style)
        *(uint16_t*)&buffer[19] = 0; // total sectors (small)
        buffer[21] = 0xF8; // media type
        *(uint16_t*)&buffer[22] = 256; // sectors per FAT
        *(uint16_t*)&buffer[24] = 63; // sectors per track
        *(uint16_t*)&buffer[26] = 255; // heads
        *(uint32_t*)&buffer[28] = 0; // hidden sectors
        *(uint32_t*)&buffer[32] = 1000000; // total sectors
        *(uint32_t*)&buffer[36] = 1000000; // sectors per FAT (FAT32)
        *(uint16_t*)&buffer[40] = 0; // flags
        *(uint16_t*)&buffer[42] = 0; // version
        *(uint32_t*)&buffer[44] = 2; // root cluster
        *(uint16_t*)&buffer[48] = 1; // FS info sector
        *(uint16_t*)&buffer[50] = 6; // backup boot sector
        memset(&buffer[52], 0, 12); // reserved
        buffer[64] = 0x80; // drive number
        buffer[65] = 0; // reserved
        buffer[66] = 0x29; // extended signature
        *(uint32_t*)&buffer[67] = 0x12345678; // volume ID
        memcpy(&buffer[71], "USB DRIVE  ", 11); // volume label
        memcpy(&buffer[82], "FAT32   ", 8); // filesystem type
    }

    return true;
}

/* Write sector */
bool usbmass_write_sector(usb_mass_storage_t* ms, uint32_t sector, const uint8_t* buffer) {
    /* Stub implementation */
    return true;
}

/* Get sector count */
uint32_t usbmass_get_sector_count(usb_mass_storage_t* ms) {
    return 1000000; /* 512MB fake drive */
}