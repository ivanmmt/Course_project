/* =============================================================================
 * kernel/idt.c
 * Реализация Interrupt Descriptor Table + настройка PIC
 *
 * PIC (Programmable Interrupt Controller, 8259A):
 * Это чип который управляет аппаратными прерываниями (IRQ).
 * По умолчанию IRQ0-7 отображены на векторы 0x08-0x0F — КОНФЛИКТ с
 * исключениями CPU! Нам нужно перемапировать их на 0x20-0x2F.
 * ============================================================================= */

#include "idt.h"
#include "kernel.h"
#include "../drivers/vga/vga_text.h"

/* Таблица IDT и указатель */
static idt_entry_t idt_table[IDT_ENTRIES];
static idt_ptr_t   idt_pointer;

/* Таблица зарегистрированных C-обработчиков */
static irq_handler_t interrupt_handlers[IDT_ENTRIES];

/* --- Порты PIC --- */
#define PIC1_CMD    0x20    /* Master PIC — командный порт */
#define PIC1_DATA   0x21    /* Master PIC — порт данных */
#define PIC2_CMD    0xA0    /* Slave PIC */
#define PIC2_DATA   0xA1

/* Команды PIC */
#define PIC_EOI     0x20    /* End Of Interrupt — сигнал "прерывание обработано" */
#define PIC_INIT    0x11    /* инициализация в режиме ICW4 */

/* Смещения: перемапируем IRQ0-15 на векторы 0x20-0x2F */
#define PIC1_OFFSET 0x20
#define PIC2_OFFSET 0x28

/* Имена исключений процессора (для отладки) */
static const char* exception_names[] = {
    "Division By Zero",           /* 0 */
    "Debug",                      /* 1 */
    "Non-Maskable Interrupt",     /* 2 */
    "Breakpoint",                 /* 3 */
    "Overflow",                   /* 4 */
    "Bound Range Exceeded",       /* 5 */
    "Invalid Opcode",             /* 6 */
    "Device Not Available",       /* 7 */
    "Double Fault",               /* 8 */
    "Coprocessor Segment Overrun",/* 9 */
    "Invalid TSS",                /* 10 */
    "Segment Not Present",        /* 11 */
    "Stack-Segment Fault",        /* 12 */
    "General Protection Fault",   /* 13 */
    "Page Fault",                 /* 14 */
    "Reserved",                   /* 15 */
    "x87 FPU Exception",          /* 16 */
    "Alignment Check",            /* 17 */
    "Machine Check",              /* 18 */
    "SIMD FP Exception",          /* 19 */
    "Virtualization Exception",   /* 20 */
};

/* =============================================================================
 * pic_remap — перенастройка PIC
 * Стандартная процедура инициализации 8259A через ICW1-ICW4
 * ============================================================================= */
static void pic_remap(void) {
    /* Сохраняем текущие маски прерываний */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* ICW1: начало инициализации, ожидаем ICW4 */
    outb(PIC1_CMD,  PIC_INIT);  io_wait();
    outb(PIC2_CMD,  PIC_INIT);  io_wait();

    /* ICW2: смещения векторов */
    outb(PIC1_DATA, PIC1_OFFSET); io_wait();    /* Master: IRQ0 → вектор 0x20 */
    outb(PIC2_DATA, PIC2_OFFSET); io_wait();    /* Slave:  IRQ8 → вектор 0x28 */

    /* ICW3: конфигурация master/slave */
    outb(PIC1_DATA, 0x04); io_wait();  /* Master: slave подключён к IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();  /* Slave: cascade identity = 2 */

    /* ICW4: режим 8086 */
    outb(PIC1_DATA, 0x01); io_wait();
    outb(PIC2_DATA, 0x01); io_wait();

    /* Восстанавливаем маски */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

/* =============================================================================
 * idt_set_gate — заполнить один дескриптор IDT
 * ============================================================================= */
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt_table[num].offset_low  = (base & 0xFFFF);
    idt_table[num].offset_high = (base >> 16) & 0xFFFF;
    idt_table[num].selector    = sel;
    idt_table[num].zero        = 0;
    idt_table[num].type_attr   = flags;
}

/* =============================================================================
 * idt_register_handler — зарегистрировать C-обработчик для вектора
 * ============================================================================= */
void idt_register_handler(uint8_t num, irq_handler_t handler) {
    interrupt_handlers[num] = handler;
}

/* =============================================================================
 * isr_handler — главный C-обработчик исключений (вызывается из ASM)
 * ============================================================================= */
void isr_handler(registers_t* regs) {
    if (regs->int_no < 32) {
        /* Исключение процессора */
        const char* name = (regs->int_no < 21) ?
            exception_names[regs->int_no] : "Reserved";

        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_RED);
        vga_printf("\n\n *** CPU EXCEPTION #%u: %s ***\n", regs->int_no, name);
        vga_printf(" Error code: 0x%x\n", regs->err_code);
        vga_printf(" EIP=0x%x  CS=0x%x  EFLAGS=0x%x\n",
                   regs->eip, regs->cs, regs->eflags);
        vga_printf(" EAX=0x%x  EBX=0x%x  ECX=0x%x  EDX=0x%x\n",
                   regs->eax, regs->ebx, regs->ecx, regs->edx);
        vga_printf(" ESI=0x%x  EDI=0x%x  EBP=0x%x  ESP=0x%x\n",
                   regs->esi, regs->edi, regs->ebp, regs->useresp);

        /* Вызываем зарегистрированный обработчик, если есть */
        if (interrupt_handlers[regs->int_no]) {
            interrupt_handlers[regs->int_no](regs);
        } else {
            __asm__ volatile ("cli; hlt");
        }
    }
}

/* =============================================================================
 * irq_handler — главный C-обработчик аппаратных прерываний (вызывается из ASM)
 * ============================================================================= */
void irq_handler(registers_t* regs) {
    /* Посылаем PIC сигнал EOI (End Of Interrupt) — иначе новые прерывания
     * не придут. Если прерывание от Slave PIC (IRQ8-15), нужно послать
     * EOI обоим контроллерам. */
    if (regs->int_no >= 40) {
        outb(PIC2_CMD, PIC_EOI);   /* Slave PIC */
    }
    outb(PIC1_CMD, PIC_EOI);       /* Master PIC */

    /* Вызываем зарегистрированный обработчик */
    if (interrupt_handlers[regs->int_no]) {
        interrupt_handlers[regs->int_no](regs);
    }
}

/* =============================================================================
 * idt_init — инициализировать IDT
 * ============================================================================= */
void idt_init(void) {
    /* Обнуляем таблицу и таблицу обработчиков */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_table[i].offset_low  = 0;
        idt_table[i].offset_high = 0;
        idt_table[i].selector    = 0;
        idt_table[i].zero        = 0;
        idt_table[i].type_attr   = 0;
        interrupt_handlers[i]    = 0;
    }

    /* Перемапируем PIC — ВАЖНО сделать до включения прерываний */
    pic_remap();

    /* Регистрируем ISR для исключений CPU (0-31) */
    idt_set_gate(0,  (uint32_t)isr0,  0x08, IDT_KERNEL_INT);
    idt_set_gate(1,  (uint32_t)isr1,  0x08, IDT_KERNEL_INT);
    idt_set_gate(2,  (uint32_t)isr2,  0x08, IDT_KERNEL_INT);
    idt_set_gate(3,  (uint32_t)isr3,  0x08, IDT_KERNEL_INT);
    idt_set_gate(4,  (uint32_t)isr4,  0x08, IDT_KERNEL_INT);
    idt_set_gate(5,  (uint32_t)isr5,  0x08, IDT_KERNEL_INT);
    idt_set_gate(6,  (uint32_t)isr6,  0x08, IDT_KERNEL_INT);
    idt_set_gate(7,  (uint32_t)isr7,  0x08, IDT_KERNEL_INT);
    idt_set_gate(8,  (uint32_t)isr8,  0x08, IDT_KERNEL_INT);
    idt_set_gate(9,  (uint32_t)isr9,  0x08, IDT_KERNEL_INT);
    idt_set_gate(10, (uint32_t)isr10, 0x08, IDT_KERNEL_INT);
    idt_set_gate(11, (uint32_t)isr11, 0x08, IDT_KERNEL_INT);
    idt_set_gate(12, (uint32_t)isr12, 0x08, IDT_KERNEL_INT);
    idt_set_gate(13, (uint32_t)isr13, 0x08, IDT_KERNEL_INT);
    idt_set_gate(14, (uint32_t)isr14, 0x08, IDT_KERNEL_INT);
    idt_set_gate(15, (uint32_t)isr15, 0x08, IDT_KERNEL_INT);
    idt_set_gate(16, (uint32_t)isr16, 0x08, IDT_KERNEL_INT);
    idt_set_gate(17, (uint32_t)isr17, 0x08, IDT_KERNEL_INT);
    idt_set_gate(18, (uint32_t)isr18, 0x08, IDT_KERNEL_INT);
    idt_set_gate(19, (uint32_t)isr19, 0x08, IDT_KERNEL_INT);
    idt_set_gate(20, (uint32_t)isr20, 0x08, IDT_KERNEL_INT);
    idt_set_gate(21, (uint32_t)isr21, 0x08, IDT_KERNEL_INT);
    idt_set_gate(22, (uint32_t)isr22, 0x08, IDT_KERNEL_INT);
    idt_set_gate(23, (uint32_t)isr23, 0x08, IDT_KERNEL_INT);
    idt_set_gate(24, (uint32_t)isr24, 0x08, IDT_KERNEL_INT);
    idt_set_gate(25, (uint32_t)isr25, 0x08, IDT_KERNEL_INT);
    idt_set_gate(26, (uint32_t)isr26, 0x08, IDT_KERNEL_INT);
    idt_set_gate(27, (uint32_t)isr27, 0x08, IDT_KERNEL_INT);
    idt_set_gate(28, (uint32_t)isr28, 0x08, IDT_KERNEL_INT);
    idt_set_gate(29, (uint32_t)isr29, 0x08, IDT_KERNEL_INT);
    idt_set_gate(30, (uint32_t)isr30, 0x08, IDT_KERNEL_INT);
    idt_set_gate(31, (uint32_t)isr31, 0x08, IDT_KERNEL_INT);

    /* Регистрируем обработчики IRQ (аппаратные прерывания, 32-47) */
    idt_set_gate(32, (uint32_t)irq0,  0x08, IDT_KERNEL_INT);
    idt_set_gate(33, (uint32_t)irq1,  0x08, IDT_KERNEL_INT);
    idt_set_gate(34, (uint32_t)irq2,  0x08, IDT_KERNEL_INT);
    idt_set_gate(35, (uint32_t)irq3,  0x08, IDT_KERNEL_INT);
    idt_set_gate(36, (uint32_t)irq4,  0x08, IDT_KERNEL_INT);
    idt_set_gate(37, (uint32_t)irq5,  0x08, IDT_KERNEL_INT);
    idt_set_gate(38, (uint32_t)irq6,  0x08, IDT_KERNEL_INT);
    idt_set_gate(39, (uint32_t)irq7,  0x08, IDT_KERNEL_INT);
    idt_set_gate(40, (uint32_t)irq8,  0x08, IDT_KERNEL_INT);
    idt_set_gate(41, (uint32_t)irq9,  0x08, IDT_KERNEL_INT);
    idt_set_gate(42, (uint32_t)irq10, 0x08, IDT_KERNEL_INT);
    idt_set_gate(43, (uint32_t)irq11, 0x08, IDT_KERNEL_INT);
    idt_set_gate(44, (uint32_t)irq12, 0x08, IDT_KERNEL_INT);
    idt_set_gate(45, (uint32_t)irq13, 0x08, IDT_KERNEL_INT);
    idt_set_gate(46, (uint32_t)irq14, 0x08, IDT_KERNEL_INT);
    idt_set_gate(47, (uint32_t)irq15, 0x08, IDT_KERNEL_INT);

    /* Заполняем указатель и загружаем IDT */
    idt_pointer.limit = (sizeof(idt_entry_t) * IDT_ENTRIES) - 1;
    idt_pointer.base  = (uint32_t)&idt_table;
    idt_flush((uint32_t)&idt_pointer);
}
