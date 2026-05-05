#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include <stdbool.h>

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

#define PCI_VENDOR_ID        0x00
#define PCI_DEVICE_ID        0x02
#define PCI_COMMAND          0x04
#define PCI_STATUS           0x06
#define PCI_CLASS_REVISION   0x08
#define PCI_SUBCLASS         0x0A
#define PCI_CLASS            0x0B
#define PCI_HEADER_TYPE      0x0E
#define PCI_BAR0             0x10
#define PCI_BAR1             0x14
#define PCI_BAR2             0x18
#define PCI_BAR3             0x1C
#define PCI_BAR4             0x20
#define PCI_BAR5             0x24
#define PCI_INTERRUPT_LINE   0x3C

#define PCI_CMD_IO          (1 << 0)
#define PCI_CMD_MEMORY      (1 << 1)
#define PCI_CMD_BUSMASTER   (1 << 2)

/* Информация об устройстве */
typedef struct {
    uint8_t  bus, dev, func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint32_t bar[6];
    uint8_t  irq;
} pci_device_t;

/* ─── API ─── */
void pci_init(void);

/* Читать/писать конфигурационное пространство */
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
uint8_t  pci_read8 (uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t func,
                     uint8_t offset, uint32_t val);
void     pci_write16(uint8_t bus, uint8_t dev, uint8_t func,
                     uint8_t offset, uint16_t val);

/* Найти устройство по Vendor+Device ID */
bool pci_find_device(uint16_t vendor, uint16_t device, pci_device_t* out);

/* Включить Bus Mastering (нужно для DMA) */
void pci_enable_busmaster(uint8_t bus, uint8_t dev, uint8_t func);

/* Вывести все найденные устройства */
void pci_enumerate(void);

#endif /* PCI_H */
