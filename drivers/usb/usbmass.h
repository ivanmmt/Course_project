/* =============================================================================
 * drivers/usb/usbmass.h
 * USB Mass Storage Class Driver
 * ============================================================================= */

#ifndef USBMASS_H
#define USBMASS_H

#include "usb.h"
#include <stdint.h>
#include <stdbool.h>

/* SCSI commands */
#define SCSI_TEST_UNIT_READY    0x00
#define SCSI_READ_CAPACITY      0x25
#define SCSI_READ_10            0x28
#define SCSI_WRITE_10           0x2A
#define SCSI_INQUIRY            0x12

/* USB mass storage functions */
bool usbmass_init(usb_mass_storage_t* ms);
bool usbmass_read_sector(usb_mass_storage_t* ms, uint32_t sector, uint8_t* buffer);
bool usbmass_write_sector(usb_mass_storage_t* ms, uint32_t sector, const uint8_t* buffer);
uint32_t usbmass_get_sector_count(usb_mass_storage_t* ms);

#endif /* USBMASS_H */