/* =============================================================================
 * drivers/net/virtio_net.c
 * VirtIO Network Device Driver
 * ============================================================================= */

#include "virtio_net.h"
#include "pci.h"
#include "../../memory/heap.h"
#include "../../lib/string.h"
#include "../../drivers/vga/vga_text.h"

/* VirtIO Device Vendor/Device IDs */
#define VIRTIO_VENDOR       0x1AF4
#define VIRTIO_NET_DEVICE   0x1000

/* VirtIO Configuration Space Registers */
#define VIRTIO_PCI_HOST_FEATURES        0x00
#define VIRTIO_PCI_GUEST_FEATURES       0x04
#define VIRTIO_PCI_QUEUE_PFN            0x08
#define VIRTIO_PCI_QUEUE_NUM            0x0C
#define VIRTIO_PCI_QUEUE_SEL            0x0E
#define VIRTIO_PCI_QUEUE_NOTIFY         0x10
#define VIRTIO_PCI_STATUS               0x12
#define VIRTIO_PCI_ISR_STATUS           0x13
#define VIRTIO_PCI_CONFIG_OFFSET        0x14

/* VirtIO Status Bits */
#define VIRTIO_CONFIG_S_ACKNOWLEDGE     0x01
#define VIRTIO_CONFIG_S_DRIVER          0x02
#define VIRTIO_CONFIG_S_DRIVER_OK       0x04
#define VIRTIO_CONFIG_S_FAILED          0x80

/* Device state */
static struct {
    bool present;
    uint16_t io_base;
    mac_addr_t mac;
} virtio_dev = {0};

/* PCI device detection */
static bool virtio_net_find_device(void) {
    uint16_t vendor, device;
    uint8_t bus, slot, func;
    
    for (bus = 0; bus < 255; bus++) {
        for (slot = 0; slot < 32; slot++) {
            for (func = 0; func < 8; func++) {
                uint32_t header = pci_read_config(bus, slot, func, 0);
                if (header == 0xFFFFFFFF) continue;
                
                vendor = header & 0xFFFF;
                device = (header >> 16) & 0xFFFF;
                
                if (vendor == VIRTIO_VENDOR && device == VIRTIO_NET_DEVICE) {
                    vga_printf("[VirtIO-Net] Found at %02X:%02X.%d\n", bus, slot, func);
                    
                    /* Get I/O base address (BAR0) */
                    uint32_t bar0 = pci_read_config(bus, slot, func, 0x10);
                    virtio_dev.io_base = bar0 & ~0x0F;
                    
                    /* Enable device */
                    uint32_t cmd = pci_read_config(bus, slot, func, 0x04);
                    cmd |= 0x01;  /* I/O Space Enable */
                    pci_write_config(bus, slot, func, 0x04, cmd);
                    
                    return true;
                }
            }
        }
    }
    
    return false;
}

/* I/O Port access */
static inline uint8_t virtio_inb(uint16_t offset) {
    uint8_t val;
    asm volatile("inb %1, %0" : "=a"(val) : "Nd"(virtio_dev.io_base + offset));
    return val;
}

static inline uint16_t virtio_inw(uint16_t offset) {
    uint16_t val;
    asm volatile("inw %1, %0" : "=a"(val) : "Nd"(virtio_dev.io_base + offset));
    return val;
}

static inline uint32_t virtio_indw(uint16_t offset) {
    uint32_t val;
    asm volatile("inl %1, %0" : "=a"(val) : "Nd"(virtio_dev.io_base + offset));
    return val;
}

static inline void virtio_outb(uint16_t offset, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(virtio_dev.io_base + offset));
}

static inline void virtio_outw(uint16_t offset, uint16_t val) {
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(virtio_dev.io_base + offset));
}

static inline void virtio_outdw(uint16_t offset, uint32_t val) {
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(virtio_dev.io_base + offset));
}

bool virtio_net_init(void) {
    if (virtio_dev.present) return true;
    
    if (!virtio_net_find_device()) {
        vga_put_string("[VirtIO-Net] Device not found\n");
        return false;
    }
    
    vga_printf("[VirtIO-Net] I/O Base: 0x%04X\n", virtio_dev.io_base);
    
    /* Reset device */
    virtio_outb(VIRTIO_PCI_STATUS, 0);
    
    /* Set ACKNOWLEDGE status */
    uint8_t status = virtio_inb(VIRTIO_PCI_STATUS);
    status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
    virtio_outb(VIRTIO_PCI_STATUS, status);
    
    /* Set DRIVER status */
    status |= VIRTIO_CONFIG_S_DRIVER;
    virtio_outb(VIRTIO_PCI_STATUS, status);
    
    /* Read MAC address from config space */
    for (int i = 0; i < 6; i++) {
        virtio_dev.mac.bytes[i] = virtio_inb(VIRTIO_PCI_CONFIG_OFFSET + i);
    }
    
    vga_printf("[VirtIO-Net] MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
               virtio_dev.mac.bytes[0], virtio_dev.mac.bytes[1],
               virtio_dev.mac.bytes[2], virtio_dev.mac.bytes[3],
               virtio_dev.mac.bytes[4], virtio_dev.mac.bytes[5]);
    
    /* Set DRIVER_OK status */
    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    virtio_outb(VIRTIO_PCI_STATUS, status);
    
    virtio_dev.present = true;
    vga_put_string("[VirtIO-Net] Initialized successfully\n");
    return true;
}

bool virtio_net_present(void) {
    return virtio_dev.present;
}

mac_addr_t virtio_net_get_mac(void) {
    return virtio_dev.mac;
}

int virtio_net_send(const void* data, int len) {
    if (!virtio_dev.present || len > 1500) return -1;
    
    /* Simple passthrough - in real implementation would use virtqueues */
    vga_printf("[VirtIO-Net] Sending %d bytes\n", len);
    return len;
}

int virtio_net_recv(void* buf, int maxlen) {
    if (!virtio_dev.present) return -1;
    
    /* Check for pending packets */
    uint8_t isr = virtio_inb(VIRTIO_PCI_ISR_STATUS);
    if ((isr & 0x01) == 0) {
        return 0;  /* No interrupts */
    }
    
    /* Simple passthrough - in real implementation would use virtqueues */
    return 0;  /* No data available in this simplified implementation */
}
