/* =============================================================================
 * drivers/keyboard/keyboard.h
 * PS/2 Keyboard Driver
 * ============================================================================= */

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

/* Порты PS/2 контроллера */
#define KBD_DATA_PORT    0x60   /* данные (скан-коды) */
#define KBD_STATUS_PORT  0x64   /* статус контроллера */
#define KBD_CMD_PORT     0x64   /* команды контроллера */

/* Специальные скан-коды */
#define KBD_SCANCODE_LSHIFT  0x2A
#define KBD_SCANCODE_RSHIFT  0x36
#define KBD_SCANCODE_CTRL    0x1D
#define KBD_SCANCODE_ALT     0x38
#define KBD_SCANCODE_CAPS    0x3A
#define KBD_SCANCODE_RELEASE 0x80   /* бит 7 = клавиша отпущена */

/* Специальные клавиши (не ASCII) */
#define KEY_NONE      0x00
/* ASCII control codes */
#define KEY_BACKSPACE   0x08
#define KEY_TAB         0x09
#define KEY_ENTER       0x0A
#define KEY_ESCAPE      0x1B
#define KEY_DELETE      0x7F

/* Расширенные коды (> 127) — возвращаются как int из keyboard_getkey() */
#define KEY_ARROW_UP    0x180
#define KEY_ARROW_DOWN  0x181
#define KEY_ARROW_LEFT  0x182
#define KEY_ARROW_RIGHT 0x183
#define KEY_F1  0x191
#define KEY_F2  0x192
#define KEY_F3  0x193
#define KEY_F4  0x194
#define KEY_F5  0x195
#define KEY_F6  0x196
#define KEY_F7  0x197
#define KEY_F8  0x198
#define KEY_F9  0x199
#define KEY_F10 0x19A
#define KEY_F11 0x19B
#define KEY_F12 0x19C

/* NumPad коды */
#define KEY_KP_0        0x200
#define KEY_KP_1        0x201
#define KEY_KP_2        0x202
#define KEY_KP_3        0x203
#define KEY_KP_4        0x204
#define KEY_KP_5        0x205
#define KEY_KP_6        0x206
#define KEY_KP_7        0x207
#define KEY_KP_8        0x208
#define KEY_KP_9        0x209
#define KEY_KP_PLUS     0x20A
#define KEY_KP_MINUS    0x20B
#define KEY_KP_MULTIPLY 0x20C
#define KEY_KP_DIVIDE   0x20D
#define KEY_KP_ENTER    0x20E
#define KEY_KP_DOT      0x20F
#define KEY_KP_NUMLOCK  0x210

/* Раскладка */
#define LAYOUT_US       0
#define LAYOUT_UA       1   /* Украинская */

/* Размер буфера клавиатуры */
#define KBD_BUFFER_SIZE 256

/* --- API --- */
void    keyboard_init(void);
/* ASCII символы 0x00-0x7F. Расширенные коды возвращает как 0 — используй getkey() */
char    keyboard_getchar(void);
/* Возвращает int: ASCII 0-127 или KEY_ARROW_* (> 0x100), KEY_KP_* (> 0x200) */
int     keyboard_getkey(void);
bool    keyboard_has_input(void);
bool    keyboard_shift(void);
bool    keyboard_ctrl(void);

/* Раскладка */
void    keyboard_set_layout(int layout);  /* LAYOUT_US или LAYOUT_UA */
int     keyboard_get_layout(void);
bool    keyboard_numlock(void);

#endif /* KEYBOARD_H */
