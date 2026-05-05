/* =============================================================================
 * drivers/usb/usb.c
 * USB Host Controller and Mass Storage Driver Implementation
 * ============================================================================= */

#include "usb.h"
#include "../../lib/string.h"
#include "../../drivers/vga/vga_text.h"
#include "../../memory/heap.h"

/* Global USB state */
static usb_device_t usb_devices[USB_MAX_DEVICES];
static int usb_device_count = 0;
static usb_mass_storage_t usb_mass_storage;

/* USB initialization */
bool usb_init(void) {
    memset(usb_devices, 0, sizeof(usb_devices));
    memset(&usb_mass_storage, 0, sizeof(usb_mass_storage));
    usb_device_count = 0;

    vga_put_string("  [USB] Initializing USB host controller...\n");

    /* For QEMU development, simulate USB mass storage device */
    usb_mass_storage.present = true;
    usb_mass_storage.device = &usb_devices[0];
    usb_mass_storage.bulk_in.max_packet_size = 512;
    usb_mass_storage.bulk_out.max_packet_size = 512;
    usb_mass_storage.tag = 1;

    /* Create virtual device */
    usb_devices[0].addr = 1;
    usb_devices[0].class = USB_CLASS_MASS_STORAGE;
    usb_devices[0].subclass = USB_SUBCLASS_SCSI;
    usb_devices[0].protocol = USB_PROTOCOL_BULK_ONLY;
    usb_devices[0].vendor_id = 0x1234;
    usb_devices[0].product_id = 0x5678;
    usb_devices[0].max_packet_size = 64;
    usb_devices[0].configured = true;
    usb_device_count = 1;

    vga_put_string("  [USB] Found virtual USB mass storage device\n");
    return true;
}

/* Scan for USB devices */
bool usb_scan_devices(void) {
    /* In real implementation, this would scan the USB bus */
    /* For now, just return success */
    return usb_device_count > 0;
}

/* Find mass storage device */
usb_mass_storage_t* usb_find_mass_storage(void) {
    return usb_mass_storage.present ? &usb_mass_storage : NULL;
}