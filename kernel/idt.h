/* =============================================================================
 * kernel/idt.h
 * Interrupt Descriptor Table (IDT) — таблица дескрипторов прерываний
 *
 * IDT — аналог GDT, но для прерываний. Когда происходит прерывание (нажата
 * клавиша, сработал таймер, произошла ошибка), процессор смотрит в IDT и
 * прыгает на соответствующий обработчик.
 *
 * x86 имеет 256 векторов прерываний:
 *   0x00-0x1F: исключения процессора (деление на ноль, page fault и т.д.)
 *   0x20-0x2F: аппаратные прерывания (IRQ) от PIC (клавиатура, таймер...)
 *   0x30-0xFF: программные прерывания (системные вызовы и т.д.)
 * ============================================================================= */

#ifndef IDT_H
#define IDT_H

#include <stdint.h>

/* Количество дескрипторов (все 256 векторов x86) */
#define IDT_ENTRIES 256

/* --- Дескриптор прерывания IDT (8 байт) --- */
typedef struct {
    uint16_t offset_low;   /* биты 0-15 адреса обработчика */
    uint16_t selector;     /* сегмент кода (обычно GDT_KERNEL_CODE_SEL = 0x08) */
    uint8_t  zero;         /* всегда 0 */
    uint8_t  type_attr;    /* тип и атрибуты */
    uint16_t offset_high;  /* биты 16-31 адреса обработчика */
} __attribute__((packed)) idt_entry_t;

/* --- Указатель на IDT --- */
typedef struct {
    uint16_t limit;        /* размер IDT - 1 */
    uint32_t base;         /* адрес IDT */
} __attribute__((packed)) idt_ptr_t;

/* --- Байт типа/атрибутов --- */
/* Бит 7: Present */
/* Биты 5-6: DPL (уровень привилегий для вызова прерывания) */
/* Биты 0-3: Gate type */
#define IDT_PRESENT         (1 << 7)
#define IDT_RING0           (0 << 5)
#define IDT_RING3           (3 << 5)
#define IDT_INTERRUPT_GATE  0x0E    /* Interrupt Gate (отключает прерывания при входе) */
#define IDT_TRAP_GATE       0x0F    /* Trap Gate (не отключает прерывания) */

/* Готовые атрибуты для типичных обработчиков */
#define IDT_KERNEL_INT  (IDT_PRESENT | IDT_RING0 | IDT_INTERRUPT_GATE)
#define IDT_USER_INT    (IDT_PRESENT | IDT_RING3 | IDT_INTERRUPT_GATE)

/* --- Структура контекста регистров при прерывании ---
 * Наши ISR-обработчики в ASM сохраняют регистры и передают эту структуру в C */
typedef struct {
    /* Сохранённые вручную в isr.asm (pusha) */
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    /* Дополнительные регистры */
    uint32_t ds;
    /* Номер прерывания и код ошибки (0 если нет) */
    uint32_t int_no, err_code;
    /* Сохранённые процессором автоматически при прерывании */
    uint32_t eip, cs, eflags, useresp, ss;
} __attribute__((packed)) registers_t;

/* Тип функции-обработчика прерывания */
typedef void (*irq_handler_t)(registers_t* regs);

/* --- API --- */
void idt_init(void);
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);
void idt_register_handler(uint8_t num, irq_handler_t handler);

/* Объявлена в isr_flush.asm */
extern void idt_flush(uint32_t idt_ptr);

/* --- Внешние ISR обработчики (объявлены в isr.asm) --- */
/* Исключения процессора (0-31) */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

/* Аппаратные прерывания IRQ (32-47) */
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

#endif /* IDT_H */
