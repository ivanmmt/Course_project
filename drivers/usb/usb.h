/* =============================================================================
 * drivers/usb/usb.h
 * USB Host Controller and Mass Storage Driver
 * ============================================================================= */

#ifndef USB_H
#define USB_H

#include <stdint.h>
#include <stdbool.h>

/* USB constants */
#define USB_MAX_DEVICES     16
#define USB_MAX_ENDPOINTS   16
#define USB_MAX_TRANSFERS   32

/* USB device classes */
#define USB_CLASS_MASS_STORAGE  0x08
#define USB_SUBCLASS_SCSI       0x06
#define USB_PROTOCOL_BULK_ONLY  0x50

/* USB request types */
#define USB_REQ_GET_STATUS      0x00
#define USB_REQ_CLEAR_FEATURE   0x01
#define USB_REQ_SET_FEATURE     0x03
#define USB_REQ_SET_ADDRESS     0x05
#define USB_REQ_GET_DESCRIPTOR  0x06
#define USB_REQ_SET_DESCRIPTOR  0x07
#define USB_REQ_GET_CONFIG      0x08
#define USB_REQ_SET_CONFIG      0x09

/* USB descriptor types */
#define USB_DESC_DEVICE         0x01
#define USB_DESC_CONFIG         0x02
#define USB_DESC_STRING         0x03
#define USB_DESC_INTERFACE      0x04
#define USB_DESC_ENDPOINT       0x05

/* USB transfer types */
#define USB_TRANSFER_CONTROL    0
#define USB_TRANSFER_BULK       2

/* USB device structure */
typedef struct {
    uint8_t  addr;
    uint8_t  class;
    uint8_t  subclass;
    uint8_t  protocol;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  max_packet_size;
    uint8_t  config_count;
    uint8_t  interface_count;
    bool     configured;
} usb_device_t;

/* USB endpoint structure */
typedef struct {
    uint8_t  addr;
    uint8_t  type;
    uint16_t max_packet_size;
    uint8_t  interval;
} usb_endpoint_t;

/* USB mass storage structure */
typedef struct {
    usb_device_t* device;
    usb_endpoint_t bulk_in;
    usb_endpoint_t bulk_out;
    uint32_t tag;
    bool present;
} usb_mass_storage_t;

/* API functions */
bool usb_init(void);
bool usb_scan_devices(void);
usb_mass_storage_t* usb_find_mass_storage(void);

#endif /* USB_H */