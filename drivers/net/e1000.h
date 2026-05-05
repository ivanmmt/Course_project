
#ifndef E1000_H
#define E1000_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define E1000_VENDOR_ID   0x8086   /* Intel */
#define E1000_DEVICE_ID   0x100E   /* e1000 (QEMU default) */
#define E1000_DEVICE_I217 0x153A   /* i217 */
#define E1000_DEVICE_82545 0x100F  /* 82545EM */


#define E1000_REG_CTRL    0x0000   /* Device Control */
#define E1000_REG_STATUS  0x0008   /* Device Status */
#define E1000_REG_EECD    0x0010   /* EEPROM Control */
#define E1000_REG_EERD    0x0014   /* EEPROM Read */
#define E1000_REG_ICR     0x00C0   /* Interrupt Cause Read */
#define E1000_REG_IMS     0x00D0   /* Interrupt Mask Set */
#define E1000_REG_IMC     0x00D8   /* Interrupt Mask Clear */
#define E1000_REG_RCTL    0x0100   /* Receive Control */
#define E1000_REG_TCTL    0x0400   /* Transmit Control */
#define E1000_REG_RDBAL   0x2800   /* RX Descriptor Base Low */
#define E1000_REG_RDBAH   0x2804   /* RX Descriptor Base High */
#define E1000_REG_RDLEN   0x2808   /* RX Descriptor Ring Length */
#define E1000_REG_RDH     0x2810   /* RX Descriptor Head */
#define E1000_REG_RDT     0x2818   /* RX Descriptor Tail */
#define E1000_REG_TDBAL   0x3800   /* TX Descriptor Base Low */
#define E1000_REG_TDBAH   0x3804   /* TX Descriptor Base High */
#define E1000_REG_TDLEN   0x3808   /* TX Descriptor Ring Length */
#define E1000_REG_TDH     0x3810   /* TX Descriptor Head */
#define E1000_REG_TDT     0x3818   /* TX Descriptor Tail */
#define E1000_REG_MTA     0x5200   /* Multicast Table Array (128 слов) */
#define E1000_REG_RAL     0x5400   /* Receive Address Low */
#define E1000_REG_RAH     0x5404   /* Receive Address High */

#define E1000_CTRL_RST    (1 << 26) /* software reset */
#define E1000_CTRL_ASDE   (1 << 5)  /* auto-speed detection */
#define E1000_CTRL_SLU    (1 << 6)  /* set link up */

#define E1000_RCTL_EN     (1 << 1)   /* enable */
#define E1000_RCTL_SBP    (1 << 2)   /* store bad packets */
#define E1000_RCTL_UPE    (1 << 3)   /* unicast promiscuous */
#define E1000_RCTL_MPE    (1 << 4)   /* multicast promiscuous */
#define E1000_RCTL_BAM    (1 << 15)  /* broadcast accept */
#define E1000_RCTL_BSIZE_2048 0      /* buffer size 2048 */
#define E1000_RCTL_SECRC  (1 << 26)  /* strip CRC */

#define E1000_TCTL_EN     (1 << 1)   /* enable */
#define E1000_TCTL_PSP    (1 << 3)   /* pad short packets */
#define E1000_TCTL_CT_SHIFT 4
#define E1000_TCTL_COLD_SHIFT 12

typedef struct {
    uint64_t addr;       /* физический адрес буфера */
    uint16_t length;     /* длина принятых данных */
    uint16_t checksum;
    uint8_t  status;     /* DD = descriptor done */
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed)) e1000_rx_desc_t;

#define E1000_RX_DD   (1 << 0)  /* descriptor done */
#define E1000_RX_EOP  (1 << 1)  /* end of packet */

typedef struct {
    uint64_t addr;       /* физический адрес данных */
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;        /* EOP | IFCS | RS */
    uint8_t  status;     /* DD = done */
    uint8_t  css;
    uint16_t special;
} __attribute__((packed)) e1000_tx_desc_t;

#define E1000_TX_CMD_EOP  (1 << 0)  /* end of packet */
#define E1000_TX_CMD_IFCS (1 << 1)  /* insert FCS/CRC */
#define E1000_TX_CMD_RS   (1 << 3)  /* report status */
#define E1000_TX_DD       (1 << 0)  /* descriptor done */

#define E1000_NUM_RX_DESC   32
#define E1000_NUM_TX_DESC   32
#define E1000_RX_BUF_SIZE   2048
#define E1000_PKT_MAX       1518    /* Ethernet max frame */

typedef struct { uint8_t b[6]; } mac_addr_t;

typedef struct {
    bool     present;
    uint32_t mmio_base;   /* виртуальный адрес MMIO */
    mac_addr_t mac;

    e1000_rx_desc_t* rx_descs;
    e1000_tx_desc_t* tx_descs;
    uint8_t*         rx_bufs[E1000_NUM_RX_DESC];
    uint8_t*         tx_bufs[E1000_NUM_TX_DESC];  /* TX buffer pool */
    uint32_t         rx_cur;   /* текущий RX хвост */
    uint32_t         tx_cur;   /* текущий TX хвост */
    uint32_t         tx_clean; /* последний очищенный TX дескриптор */
} e1000_dev_t;

bool e1000_init(void);                    /* обнаружить через PCI и инициализировать */
bool e1000_present(void);                 /* адаптер найден? */
mac_addr_t e1000_get_mac(void);           /* наш MAC адрес */
int  e1000_send(const void* data, uint16_t len);   /* отправить пакет */
int  e1000_recv(void* buf, uint16_t maxlen);        /* получить пакет (polling) */
void e1000_interrupt(void);               /* interrupt handler */
void e1000_dump_info(void);               /* отладочная информация */

#endif /* E1000_H */
