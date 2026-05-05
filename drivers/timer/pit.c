/* =============================================================================
 * drivers/timer/pit.c
 * Драйвер системного таймера PIT (IRQ0)
 * ============================================================================= */

#include "pit.h"
#include "../../kernel/idt.h"
#include "../../kernel/kernel.h"

static volatile uint32_t tick_count  = 0;
static          uint32_t ticks_per_ms = 0;

/* =============================================================================
 * timer_irq_handler — обработчик прерывания таймера (IRQ0, вектор 32)
 * ============================================================================= */
static void timer_irq_handler(registers_t* regs) {
    (void)regs;
    tick_count++;
}

/* =============================================================================
 * pit_init — настройка PIT на заданную частоту прерываний
 * ============================================================================= */
void pit_init(uint32_t frequency_hz) {
    /* Вычисляем делитель */
    uint32_t divisor = PIT_BASE_FREQ / frequency_hz;

    /* Командный байт:
     * биты 7-6: канал 0
     * биты 5-4: режим доступа — оба байта (lo затем hi)
     * биты 3-1: режим 3 (square wave)
     * бит 0:   двоичный счёт */
    outb(PIT_CMD, 0x36);

    /* Посылаем делитель: сначала младший байт, затем старший */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    ticks_per_ms = frequency_hz / 1000;
    if (ticks_per_ms == 0) ticks_per_ms = 1;

    /* Регистрируем обработчик на IRQ0 (вектор 32) */
    idt_register_handler(32, timer_irq_handler);
}

uint32_t pit_get_ticks(void) {
    return tick_count;
}

void pit_sleep(uint32_t ms) {
    uint32_t target = tick_count + ms * ticks_per_ms;
    while (tick_count < target) {
        __asm__ volatile ("hlt");
    }
}
