/* =============================================================================
 * drivers/net/rtl8139.c
 * RTL8139 (Realtek 8139) Ethernet Driver
 * ============================================================================= */

#include "rtl8139.h"
#include "pci.h"
#include "../../memory/heap.h"
#include "../../lib/string.h"
#include "../../drivers/vga/vga_text.h"

/* RTL8139 I/O Port Registers */
#define RTL_MAC0            0x00
#define RTL_RBSTART         0x30    /* RX Buffer Start Address */
#define RTL_CMD             0x37    /* Command Register */
#define RTL_ISR             0x3E    /* Interrupt Status Register */
#define RTL_IMR             0x3C    /* Interrupt Mask Register */
#define RTL_TCR             0x40    /* TX Control Register */
#define RTL_RCR             0x44    /* RX Control Register */
#define RTL_CAPR            0x38    /* Current Address of Packet Read */
#define RTL_CBCR            0x3A    /* Current Buffer Count Register */
#define RTL_TXADDR0         0x20    /* TX Address 0 */
#define RTL_TXADDR1         0x24    /* TX Address 1 */
#define RTL_TXADDR2         0x28    /* TX Address 2 */
#define RTL_TXADDR3         0x2C    /* TX Address 3 */
#define RTL_TXSTATUS0       0x10    /* TX Status 0 */
#define RTL_TXSTATUS1       0x14    /* TX Status 1 */
#define RTL_TXSTATUS2       0x18    /* TX Status 2 */
#define RTL_TXSTATUS3       0x1C    /* TX Status 3 */

/* Command Register Bits */
#define RTL_CMD_RESET       0x10
#define RTL_CMD_RXENABLE    0x08
#define RTL_CMD_TXENABLE    0x04

/* RX Control Register Bits */
#define RTL_RCR_RXFTH_NONE  0x0000  /* RX FIFO threshold = None */
#define RTL_RCR_AB          0x0008  /* Accept Broadcast packets */
#define RTL_RCR_AM          0x0004  /* Accept Multicast packets */
#define RTL_RCR_APM         0x0002  /* Accept Physical Match packets */
#define RTL_RCR_AAP         0x0001  /* Accept All Packets */

/* TX Control Register Bits */
#define RTL_TCR_CLRABT      0x00000001

/* Device state */
static struct {
    bool present;
    uint16_t io_base;
    uint16_t pci_vendor;
    uint16_t pci_device;
    mac_addr_t mac;
    
    /* RX buffer */
    uint8_t* rx_buffer;
    uint32_t rx_buffer_phys;
    uint32_t rx_read_pos;
} rtl_dev = {0};

/* PCI device detection */
static bool rtl8139_find_device(void) {
    /* Search for RTL8139 (Vendor: 0x10EC, Device: 0x8139) */
    uint16_t vendor, device;
    uint8_t bus, slot, func;
    
    for (bus = 0; bus < 255; bus++) {
        for (slot = 0; slot < 32; slot++) {
            for (func = 0; func < 8; func++) {
                uint32_t header = pci_read_config(bus, slot, func, 0);
                if (header == 0xFFFFFFFF) continue;
                
                vendor = header & 0xFFFF;
                device = (header >> 16) & 0xFFFF;
                
                if (vendor == 0x10EC && device == 0x8139) {
                    vga_printf("[RTL8139] Found at %02X:%02X.%d\n", bus, slot, func);
                    
                    /* Get I/O base address (BAR0) */
                    uint32_t bar0 = pci_read_config(bus, slot, func, 0x10);
                    rtl_dev.io_base = bar0 & ~0x0F;
                    rtl_dev.pci_vendor = vendor;
                    rtl_dev.pci_device = device;
                    
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
static inline uint8_t rtl_inb(uint16_t port) {
    uint8_t val;
    asm volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline uint16_t rtl_inw(uint16_t port) {
    uint16_t val;
    asm volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline uint32_t rtl_indw(uint16_t port) {
    uint32_t val;
    asm volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void rtl_outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void rtl_outw(uint16_t port, uint16_t val) {
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline void rtl_outdw(uint16_t port, uint32_t val) {
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

bool rtl8139_init(void) {
    if (rtl_dev.present) return true;
    
    if (!rtl8139_find_device()) {
        vga_put_string("[RTL8139] Device not found\n");
        return false;
    }
    
    vga_printf("[RTL8139] I/O Base: 0x%04X\n", rtl_dev.io_base);
    
    /* Reset the device */
    rtl_outb(rtl_dev.io_base + RTL_CMD, RTL_CMD_RESET);
    while (rtl_inb(rtl_dev.io_base + RTL_CMD) & RTL_CMD_RESET) {
        /* Wait for reset to complete */
    }
    
    /* Read MAC address */
    for (int i = 0; i < 6; i++) {
        rtl_dev.mac.bytes[i] = rtl_inb(rtl_dev.io_base + RTL_MAC0 + i);
    }
    
    vga_printf("[RTL8139] MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
               rtl_dev.mac.bytes[0], rtl_dev.mac.bytes[1],
               rtl_dev.mac.bytes[2], rtl_dev.mac.bytes[3],
               rtl_dev.mac.bytes[4], rtl_dev.mac.bytes[5]);
    
    /* Allocate RX buffer (8KB + 16 bytes for safety) */
    rtl_dev.rx_buffer = (uint8_t*)malloc(8192 + 16);
    if (!rtl_dev.rx_buffer) {
        vga_put_string("[RTL8139] Failed to allocate RX buffer\n");
        return false;
    }
    
    /* Set RX buffer address (assuming identity mapping) */
    uint32_t rx_addr = (uint32_t)rtl_dev.rx_buffer & 0xFFFFFFFF;
    rtl_outdw(rtl_dev.io_base + RTL_RBSTART, rx_addr);
    
    /* Setup RX Control Register */
    uint16_t rcr = RTL_RCR_RXFTH_NONE | RTL_RCR_AB | RTL_RCR_APM | RTL_RCR_AAP;
    rtl_outw(rtl_dev.io_base + RTL_RCR, rcr);
    
    /* Clear interrupt status */
    rtl_outw(rtl_dev.io_base + RTL_ISR, 0xFFFF);
    
    /* Enable RX and TX */
    rtl_outb(rtl_dev.io_base + RTL_CMD, RTL_CMD_RXENABLE | RTL_CMD_TXENABLE);
    
    rtl_dev.present = true;
    vga_put_string("[RTL8139] Initialized successfully\n");
    return true;
}

bool rtl8139_present(void) {
    return rtl_dev.present;
}

mac_addr_t rtl8139_get_mac(void) {
    return rtl_dev.mac;
}

int rtl8139_send(const void* data, int len) {
    if (!rtl_dev.present || len > 1600) return -1;
    
    /* Find available TX descriptor */
    for (int i = 0; i < 4; i++) {
        uint32_t status = rtl_indw(rtl_dev.io_base + RTL_TXSTATUS0 + i * 4);
        if (!(status & 0x80000000)) {  /* Buffer not owned by device */
            /* Write packet data to TX buffer */
            uint32_t tx_addr = rtl_dev.io_base + RTL_TXADDR0 + i * 4;
            
            /* For simplicity, use a shared TX buffer */
            static uint8_t tx_buf[1600];
            memcpy(tx_buf, data, len);
            
            /* Set packet length and transmit */
            rtl_outdw(rtl_dev.io_base + RTL_TXSTATUS0 + i * 4, len & 0xFFF);
            return len;
        }
    }
    
    return -1;  /* No available TX buffer */
}

int rtl8139_recv(void* buf, int maxlen) {
    if (!rtl_dev.present) return -1;
    
    uint16_t capr = rtl_inw(rtl_dev.io_base + RTL_CAPR);
    uint16_t cbcr = rtl_inw(rtl_dev.io_base + RTL_CBCR);
    
    if (cbcr == 0xFFFF) {
        return 0;  /* No data available */
    }
    
    uint32_t pkt_addr = (capr + 4) % (8192);
    uint16_t pkt_len = *(uint16_t*)(rtl_dev.rx_buffer + pkt_addr);
    
    if (pkt_len > (uint16_t)maxlen) {
        return -1;  /* Packet too large */
    }
    
    /* Copy packet data */
    uint32_t data_addr = (pkt_addr + 4);
    if (data_addr + pkt_len <= 8192) {
        memcpy(buf, rtl_dev.rx_buffer + data_addr, pkt_len);
    } else {
        uint32_t first = 8192 - data_addr;
        memcpy(buf, rtl_dev.rx_buffer + data_addr, first);
        memcpy((uint8_t*)buf + first, rtl_dev.rx_buffer, pkt_len - first);
    }
    
    /* Update read pointer */
    uint16_t new_capr = (capr + pkt_len + 4 + 3) & ~3;
    if (new_capr >= 8192) new_capr -= 8192;
    rtl_outw(rtl_dev.io_base + RTL_CAPR, new_capr);
    
    return pkt_len;
}
