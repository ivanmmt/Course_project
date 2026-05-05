/* =============================================================================
 * drivers/net/pci.c
 * PCI Bus Enumeration
 * ============================================================================= */

#include "pci.h"
#include "../../kernel/kernel.h"
#include "../../drivers/vga/vga_text.h"

/* =============================================================================
 * Формирование адреса для порта 0xCF8
 * Биты: 31=enable, 23:16=bus, 15:11=device, 10:8=function, 7:2=offset
 * ============================================================================= */
static uint32_t pci_addr(uint8_t bus, uint8_t dev,
                          uint8_t func, uint8_t offset) {
    return (1u << 31)
         | ((uint32_t)bus   << 16)
         | ((uint32_t)(dev & 0x1F) << 11)
         | ((uint32_t)(func & 0x07) << 8)
         | ((uint32_t)(offset & 0xFC));
}

uint32_t pci_read32(uint8_t bus, uint8_t dev,
                    uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev,
                    uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, offset));
    return (uint16_t)((inl(PCI_CONFIG_DATA) >> ((offset & 2) * 8)) & 0xFFFF);
}

uint8_t pci_read8(uint8_t bus, uint8_t dev,
                  uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, offset));
    return (uint8_t)((inl(PCI_CONFIG_DATA) >> ((offset & 3) * 8)) & 0xFF);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func,
                 uint8_t offset, uint32_t val) {
    outl(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, offset));
    outl(PCI_CONFIG_DATA, val);
}

void pci_write16(uint8_t bus, uint8_t dev, uint8_t func,
                 uint8_t offset, uint16_t val) {
    uint32_t cur = pci_read32(bus, dev, func, offset & ~3);
    int shift = (offset & 2) * 8;
    cur = (cur & ~(0xFFFF << shift)) | ((uint32_t)val << shift);
    pci_write32(bus, dev, func, offset & ~3, cur);
}

bool pci_find_device(uint16_t vendor, uint16_t device, pci_device_t* out) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t id = pci_read32((uint8_t)bus, dev, func, PCI_VENDOR_ID);
                uint16_t vid = (uint16_t)(id & 0xFFFF);
                uint16_t did = (uint16_t)(id >> 16);

                if (vid == 0xFFFF || vid == 0x0000) continue;
                if (vid == vendor && did == device) {
                    if (out) {
                        out->bus      = (uint8_t)bus;
                        out->dev      = dev;
                        out->func     = func;
                        out->vendor_id= vid;
                        out->device_id= did;
                        out->class_code = pci_read8((uint8_t)bus,dev,func,PCI_CLASS);
                        out->subclass   = pci_read8((uint8_t)bus,dev,func,PCI_SUBCLASS);
                        for (int b = 0; b < 6; b++)
                            out->bar[b] = pci_read32((uint8_t)bus,dev,func,
                                                     (uint8_t)(PCI_BAR0 + b*4));
                        out->irq = pci_read8((uint8_t)bus,dev,func,PCI_INTERRUPT_LINE);
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

void pci_enable_busmaster(uint8_t bus, uint8_t dev, uint8_t func) {
    uint16_t cmd = pci_read16(bus, dev, func, PCI_COMMAND);
    cmd |= PCI_CMD_BUSMASTER | PCI_CMD_MEMORY | PCI_CMD_IO;
    pci_write16(bus, dev, func, PCI_COMMAND, cmd);
}

void pci_enumerate(void) {
    vga_put_string("  PCI devices:\n");
    for (uint16_t bus = 0; bus < 4; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id = pci_read32((uint8_t)bus, dev, 0, PCI_VENDOR_ID);
            uint16_t vid = (uint16_t)(id & 0xFFFF);
            uint16_t did = (uint16_t)(id >> 16);
            if (vid == 0xFFFF || vid == 0) continue;
            uint8_t cls = pci_read8((uint8_t)bus, dev, 0, PCI_CLASS);
            uint8_t sub = pci_read8((uint8_t)bus, dev, 0, PCI_SUBCLASS);
            vga_printf("    %02x:%02x.0  %04x:%04x  class=%02x:%02x\n",
                       bus, dev, vid, did, cls, sub);
        }
    }
}

void pci_init(void) { /* Ничего не требует инициализации */ }
