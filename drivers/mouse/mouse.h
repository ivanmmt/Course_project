/* =============================================================================
 * drivers/mouse/mouse.h
 * PS/2 Mouse Driver — мышь через IRQ12
 *
 * PS/2 мышь общается через тот же контроллер что и клавиатура (0x60/0x64).
 * Каждый пакет = 3 байта:
 *   Байт 0: флаги (кнопки, направление, переполнение)
 *   Байт 1: delta X (со знаком)
 *   Байт 2: delta Y (со знаком, ось Y инвертирована!)
 * ============================================================================= */

#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include <stdbool.h>

/* Флаги байта статуса мыши */
#define MOUSE_BTN_LEFT    (1 << 0)
#define MOUSE_BTN_RIGHT   (1 << 1)
#define MOUSE_BTN_MIDDLE  (1 << 2)
#define MOUSE_X_SIGN      (1 << 4)
#define MOUSE_Y_SIGN      (1 << 5)
#define MOUSE_X_OVERFLOW  (1 << 6)
#define MOUSE_Y_OVERFLOW  (1 << 7)

/* Состояние мыши */
typedef struct {
    int32_t  x, y;           /* текущая позиция (clipped к экрану) */
    int32_t  dx, dy;         /* последнее перемещение */
    bool     left, right, middle;  /* состояние кнопок */
    bool     left_click;     /* одиночный клик (нажатие) */
    bool     right_click;
} mouse_state_t;

/* --- API --- */
void              mouse_init(int32_t screen_w, int32_t screen_h);
const mouse_state_t* mouse_get_state(void);
void              mouse_set_pos(int32_t x, int32_t y);
void              mouse_consume_clicks(void);  /* сбросить флаги click */

#endif /* MOUSE_H */
