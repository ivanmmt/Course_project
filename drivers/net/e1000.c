#include "e1000.h"
#include "pci.h"
#include "../../memory/heap.h"
#include "../../memory/vmm.h"
#include "../../memory/pmm.h"
#include "../../lib/string.h"
#include "../../drivers/vga/vga_text.h"
#include "../../kernel/kernel.h"

static e1000_dev_t e1000;


static inline uint32_t e1000_read(uint32_t reg) {
    return *(volatile uint32_t*)(e1000.mmio_base + reg);
}
static inline void e1000_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)(e1000.mmio_base + reg) = val;
}

static uint16_t eeprom_read(uint8_t addr) {

    e1000_write(E1000_REG_EERD, 1 | ((uint32_t)addr << 8));
    uint32_t val;
    uint32_t timeout = 10000;
    do {
        val = e1000_read(E1000_REG_EERD);
        if (--timeout == 0) return 0;
    } while (!(val & (1 << 4)));
    return (uint16_t)(val >> 16);
}

static void e1000_read_mac(void) {
    uint32_t ral = e1000_read(E1000_REG_RAL);
    uint32_t rah = e1000_read(E1000_REG_RAH);

    if (ral != 0 && ral != 0xFFFFFFFF) {
        e1000.mac.b[0] = (uint8_t)(ral & 0xFF);
        e1000.mac.b[1] = (uint8_t)((ral >> 8) & 0xFF);
        e1000.mac.b[2] = (uint8_t)((ral >> 16) & 0xFF);
        e1000.mac.b[3] = (uint8_t)((ral >> 24) & 0xFF);
        e1000.mac.b[4] = (uint8_t)(rah & 0xFF);
        e1000.mac.b[5] = (uint8_t)((rah >> 8) & 0xFF);
        return;
    }

    uint16_t w0 = eeprom_read(0);
    uint16_t w1 = eeprom_read(1);
    uint16_t w2 = eeprom_read(2);
    e1000.mac.b[0] = (uint8_t)(w0 & 0xFF);
    e1000.mac.b[1] = (uint8_t)(w0 >> 8);
    e1000.mac.b[2] = (uint8_t)(w1 & 0xFF);
    e1000.mac.b[3] = (uint8_t)(w1 >> 8);
    e1000.mac.b[4] = (uint8_t)(w2 & 0xFF);
    e1000.mac.b[5] = (uint8_t)(w2 >> 8);
}

static void e1000_init_rx(void) {
    e1000.rx_descs = (e1000_rx_desc_t*)kmalloc(
        sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);
    memset(e1000.rx_descs, 0,
           sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);

    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        e1000.rx_bufs[i] = (uint8_t*)kmalloc(E1000_RX_BUF_SIZE);
        e1000.rx_descs[i].addr = (uint64_t)(uint32_t)e1000.rx_bufs[i];
        e1000.rx_descs[i].status = 0;
    }
    e1000.rx_cur = 0;

    uint32_t rx_base = (uint32_t)e1000.rx_descs;
    e1000_write(E1000_REG_RDBAL, rx_base);
    e1000_write(E1000_REG_RDBAH, 0);
    e1000_write(E1000_REG_RDLEN,
                E1000_NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    e1000_write(E1000_REG_RDH, 0);
    e1000_write(E1000_REG_RDT, E1000_NUM_RX_DESC - 1);

    e1000_write(E1000_REG_RCTL,
        E1000_RCTL_EN  |
        E1000_RCTL_BAM |
        E1000_RCTL_UPE |
        E1000_RCTL_MPE |
        E1000_RCTL_SECRC);
}


static void e1000_init_tx(void) {
    e1000.tx_descs = (e1000_tx_desc_t*)kmalloc(
        sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);
    memset(e1000.tx_descs, 0,
           sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);
    
    /* Allocate TX buffer pool */
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        e1000.tx_bufs[i] = (uint8_t*)kmalloc(E1000_PKT_MAX);
    }
    
    e1000.tx_cur = 0;
    e1000.tx_clean = 0;

    uint32_t tx_base = (uint32_t)e1000.tx_descs;
    e1000_write(E1000_REG_TDBAL, tx_base);
    e1000_write(E1000_REG_TDBAH, 0);
    e1000_write(E1000_REG_TDLEN,
                E1000_NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    e1000_write(E1000_REG_TDH, 0);
    e1000_write(E1000_REG_TDT, 0);

    e1000_write(E1000_REG_TCTL,
        E1000_TCTL_EN  |
        E1000_TCTL_PSP |
        (0x0F << E1000_TCTL_CT_SHIFT) |
        (0x40 << E1000_TCTL_COLD_SHIFT));
}


bool e1000_init(void) {
    memset(&e1000, 0, sizeof(e1000));

    pci_device_t pci_dev;
    uint16_t ids[] = {E1000_DEVICE_ID, E1000_DEVICE_I217, E1000_DEVICE_82545};
    bool found = false;

    for (int i = 0; i < 3; i++) {
        if (pci_find_device(E1000_VENDOR_ID, ids[i], &pci_dev)) {
            found = true;
            vga_printf("  e1000: found %04x:%04x at %02x:%02x.0\n",
                       pci_dev.vendor_id, pci_dev.device_id,
                       pci_dev.bus, pci_dev.dev);
            break;
        }
    }

    if (!found) {
        vga_put_string("  e1000: not found (add -nic user,model=e1000 to QEMU)\n");
        return false;
    }

    pci_enable_busmaster(pci_dev.bus, pci_dev.dev, pci_dev.func);

    uint32_t bar0 = pci_dev.bar[0] & ~0xF;
    if (!bar0) {
        vga_put_string("  e1000: BAR0 is zero\n");
        return false;
    }

    for (uint32_t off = 0; off < 0x20000; off += 4096)
        vmm_map_page(bar0 + off, bar0 + off, VMM_KERNEL_FLAGS);

    e1000.mmio_base = bar0;

    e1000_write(E1000_REG_CTRL, e1000_read(E1000_REG_CTRL) | E1000_CTRL_RST);
    uint32_t timeout = 100000;
    while ((e1000_read(E1000_REG_CTRL) & E1000_CTRL_RST) && --timeout);

    e1000_write(E1000_REG_CTRL,
        e1000_read(E1000_REG_CTRL) | E1000_CTRL_SLU | E1000_CTRL_ASDE);

    /* Enable interrupts: RXO, RXT, TXDW */
    e1000_write(E1000_REG_IMS, (1 << 7) | (1 << 0) | (1 << 1));

    e1000_read_mac();

    for (int i = 0; i < 128; i++)
        e1000_write(E1000_REG_MTA + i*4, 0);

    e1000_init_rx();
    e1000_init_tx();

    e1000.present = true;

    vga_printf("  e1000: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
        e1000.mac.b[0], e1000.mac.b[1], e1000.mac.b[2],
        e1000.mac.b[3], e1000.mac.b[4], e1000.mac.b[5]);

    return true;
}

bool e1000_present(void) { return e1000.present; }
mac_addr_t e1000_get_mac(void) { return e1000.mac; }

/* Clean up completed TX descriptors */
static void e1000_tx_cleanup(void) {
    while (e1000.tx_clean != e1000.tx_cur) {
        uint32_t idx = e1000.tx_clean;
        e1000_tx_desc_t* desc = &e1000.tx_descs[idx];
        
        if (!(desc->status & E1000_TX_DD)) break;
        
        /* This descriptor is done, move to next */
        e1000.tx_clean = (idx + 1) % E1000_NUM_TX_DESC;
    }
}

int e1000_send(const void* data, uint16_t len) {
    if (!e1000.present || len == 0 || len > E1000_PKT_MAX) return -1;

    /* Clean up any completed transmissions first */
    e1000_tx_cleanup();
    
    uint32_t cur = e1000.tx_cur;
    uint32_t next = (cur + 1) % E1000_NUM_TX_DESC;
    
    /* If ring is full, wait for cleanup */
    if (next == e1000.tx_clean) {
        uint32_t timeout = 100000;
        while (next == e1000.tx_clean && --timeout) {
            __asm__ volatile ("pause");
            e1000_tx_cleanup();
        }
        if (next == e1000.tx_clean) return -1;  /* Ring still full */
    }

    /* Use buffer from pool */
    uint8_t* tx_buf = e1000.tx_bufs[cur];
    memcpy(tx_buf, data, len);

    e1000_tx_desc_t* desc = &e1000.tx_descs[cur];
    desc->addr   = (uint64_t)(uint32_t)tx_buf;
    desc->length = len;
    desc->cmd    = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    desc->status = 0;

    e1000.tx_cur = next;
    e1000_write(E1000_REG_TDT, e1000.tx_cur);

    return len;
}

/* =============================================================================
 * e1000_interrupt — interrupt handler (called from IRQ context)
 * ============================================================================= */
void e1000_interrupt(void) {
    if (!e1000.present) return;

    /* Read and clear interrupt status */
    uint32_t icr = e1000_read(E1000_REG_ICR);
    if (icr == 0) return;

    /* RXO = Receiver Overrun, RXT = Receive Timer Interrupt */
    if (icr & ((1 << 7) | (1 << 0))) {
        /* Process received packets */
        uint32_t cur = e1000.rx_cur;
        while (e1000.rx_descs[cur].status & E1000_RX_DD) {
            /* Mark as processed for next poll */
            e1000.rx_cur = (cur + 1) % E1000_NUM_RX_DESC;
            cur = e1000.rx_cur;
        }
    }

    /* TXDW = Transmit Descriptor Written Back */
    if (icr & (1 << 1)) {
        /* Clean up transmitted packets */
        e1000_tx_cleanup();
    }
}

int e1000_recv(void* buf, uint16_t maxlen) {
    if (!e1000.present) return -1;

    uint32_t cur = e1000.rx_cur;
    e1000_rx_desc_t* desc = &e1000.rx_descs[cur];

    if (!(desc->status & E1000_RX_DD)) return 0;

    uint16_t len = desc->length;
    if (len > maxlen) len = maxlen;

    memcpy(buf, e1000.rx_bufs[cur], len);

    desc->status = 0;
    e1000_write(E1000_REG_RDT, cur);
    e1000.rx_cur = (cur + 1) % E1000_NUM_RX_DESC;

    return len;
}

void e1000_dump_info(void) {
    if (!e1000.present) {
        vga_put_string("  e1000: not present\n");
        return;
    }
    vga_printf("  e1000: MMIO=0x%x  status=0x%x\n",
        e1000.mmio_base, e1000_read(E1000_REG_STATUS));
    vga_printf("  MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
        e1000.mac.b[0], e1000.mac.b[1], e1000.mac.b[2],
        e1000.mac.b[3], e1000.mac.b[4], e1000.mac.b[5]);
}
