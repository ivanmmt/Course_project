/* =============================================================================
 * drivers/mouse/mouse.c
 * PS/2 Mouse Driver через IRQ12 (вектор 44)
 * ============================================================================= */

#include "mouse.h"
#include "../../kernel/idt.h"
#include "../../kernel/kernel.h"

#define PS2_DATA    0x60
#define PS2_CMD     0x64
#define PS2_STATUS  0x64

static mouse_state_t state;
static int32_t screen_w = 800;
static int32_t screen_h = 600;

/* Буфер пакета мыши (3 байта) */
static uint8_t  mouse_packet[3];
static uint8_t  mouse_cycle = 0;  /* 0, 1, 2 — номер байта в пакете */

/* =============================================================================
 * Вспомогательные функции PS/2 контроллера
 * ============================================================================= */
static void ps2_wait_write(void) {
    uint32_t timeout = 100000;
    while (timeout-- && (inb(PS2_STATUS) & 0x02));
}

static void ps2_wait_read(void) {
    uint32_t timeout = 100000;
    while (timeout-- && !(inb(PS2_STATUS) & 0x01));
}

static void ps2_mouse_write(uint8_t data) {
    ps2_wait_write();
    outb(PS2_CMD, 0xD4);   /* "следующий байт — для мыши" */
    ps2_wait_write();
    outb(PS2_DATA, data);
}

static uint8_t ps2_mouse_read(void) {
    ps2_wait_read();
    return inb(PS2_DATA);
}

/* =============================================================================
 * mouse_irq_handler — обработчик IRQ12 (вектор 44)
 * Собираем 3-байтный пакет, обновляем состояние
 * ============================================================================= */
static void mouse_irq_handler(registers_t* regs) {
    (void)regs;

    uint8_t data = inb(PS2_DATA);

    switch (mouse_cycle) {
        case 0:
            /* Первый байт: флаги. Бит 3 должен быть всегда 1 */
            if (!(data & 0x08)) return;  /* невалидный пакет — сбрасываем */
            mouse_packet[0] = data;
            mouse_cycle = 1;
            break;
        case 1:
            mouse_packet[1] = data;
            mouse_cycle = 2;
            break;
        case 2:
            mouse_packet[2] = data;
            mouse_cycle = 0;

            /* Обрабатываем полный пакет */
            uint8_t flags = mouse_packet[0];

            /* Переполнение = невалидные данные */
            if (flags & (MOUSE_X_OVERFLOW | MOUSE_Y_OVERFLOW)) break;

            /* Delta X (со знаком, 9 бит: бит знака в флагах) */
            int32_t dx = (int32_t)mouse_packet[1];
            if (flags & MOUSE_X_SIGN) dx |= ~0xFF;  /* знаковое расширение */

            /* Delta Y (инвертирован! мышь считает вверх = +, экран считает вниз = +) */
            int32_t dy = (int32_t)mouse_packet[2];
            if (flags & MOUSE_Y_SIGN) dy |= ~0xFF;
            dy = -dy;  /* инвертируем ось Y */

            state.dx = dx;
            state.dy = dy;

            /* Обновляем позицию с clipping */
            state.x += dx;
            state.y += dy;
            if (state.x < 0) state.x = 0;
            if (state.y < 0) state.y = 0;
            if (state.x >= screen_w) state.x = screen_w - 1;
            if (state.y >= screen_h) state.y = screen_h - 1;

            /* Кнопки */
            bool prev_left  = state.left;
            bool prev_right = state.right;
            state.left   = (flags & MOUSE_BTN_LEFT)   != 0;
            state.right  = (flags & MOUSE_BTN_RIGHT)  != 0;
            state.middle = (flags & MOUSE_BTN_MIDDLE) != 0;

            /* Клик = переход 0→1 */
            if (state.left  && !prev_left)  state.left_click  = true;
            if (state.right && !prev_right) state.right_click = true;

            break;
    }
}

/* =============================================================================
 * mouse_init — инициализация PS/2 мыши
 * ============================================================================= */
void mouse_init(int32_t sw, int32_t sh) {
    screen_w = sw;
    screen_h = sh;
    state.x  = sw / 2;
    state.y  = sh / 2;

    /* Включаем вспомогательное устройство (мышь) */
    ps2_wait_write();
    outb(PS2_CMD, 0xA8);   /* Enable Auxiliary Device */

    /* Включаем прерывания от мыши (бит 1 в байте конфигурации) */
    ps2_wait_write();
    outb(PS2_CMD, 0x20);   /* читаем байт конфигурации */
    ps2_wait_read();
    uint8_t cfg = inb(PS2_DATA);
    cfg |= 0x02;            /* IRQ12 enable */
    cfg &= ~0x20;           /* mouse clock enable */
    ps2_wait_write();
    outb(PS2_CMD, 0x60);   /* записываем конфигурацию */
    ps2_wait_write();
    outb(PS2_DATA, cfg);

    /* Сбрасываем мышь */
    ps2_mouse_write(0xFF);
    ps2_mouse_read();  /* ACK */
    ps2_mouse_read();  /* 0xAA (self-test passed) */
    ps2_mouse_read();  /* 0x00 (mouse ID) */

    /* Устанавливаем стандартные настройки */
    ps2_mouse_write(0xF6);  /* Set Defaults */
    ps2_mouse_read();       /* ACK */

    /* Включаем передачу данных */
    ps2_mouse_write(0xF4);  /* Enable Data Reporting */
    ps2_mouse_read();       /* ACK */

    /* Регистрируем обработчик IRQ12 (вектор 44) */
    idt_register_handler(44, mouse_irq_handler);
}

const mouse_state_t* mouse_get_state(void) { return &state; }

void mouse_set_pos(int32_t x, int32_t y) {
    state.x = x; state.y = y;
}

void mouse_consume_clicks(void) {
    state.left_click  = false;
    state.right_click = false;
}
