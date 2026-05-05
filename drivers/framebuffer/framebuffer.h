/* =============================================================================
 * drivers/framebuffer/framebuffer.h
 * Framebuffer — пиксельный графический режим
 *
 * GRUB запрашивает у BIOS видеорежим через тег Multiboot2.
 * Мы получаем адрес линейного фреймбуфера — массив пикселей в памяти.
 * Каждый пиксель = 4 байта (BGRX или XRGB зависит от железа).
 *
 * Схема памяти фреймбуфера:
 *   pixel(x, y) = base_addr + y * pitch + x * (bpp/8)
 *   pitch — реальная ширина строки в байтах (может быть > width*4)
 *
 * Мы реализуем:
 *   - рисование пикселей, линий, прямоугольников
 *   - blit (копирование буфера)
 *   - встроенный растровый шрифт 8×16 пикселей
 *   - двойную буферизацию (back buffer → front buffer)
 * ============================================================================= */

#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ─── Цвет ─── */
/* Упакованный 32-битный цвет: 0x00RRGGBB */
typedef uint32_t fb_color_t;

/* Конструктор цвета */
static inline fb_color_t fb_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
static inline fb_color_t fb_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* Палитра стандартных цветов */
#define FB_BLACK        0x000000u
#define FB_WHITE        0xFFFFFFu
#define FB_RED          0xFF4444u
#define FB_GREEN        0x44FF44u
#define FB_BLUE         0x4488FFu
#define FB_YELLOW       0xFFFF44u
#define FB_CYAN         0x44FFFFu
#define FB_MAGENTA      0xFF44FFu
#define FB_ORANGE       0xFF8C00u
#define FB_DARK_GREY    0x333333u
#define FB_GREY         0x888888u
#define FB_LIGHT_GREY   0xCCCCCCu
#define FB_DARK_BLUE    0x003060u
#define FB_TASKBAR_BG   0x1E2A3Au
#define FB_WINDOW_BG    0xF0F0F0u
#define FB_WINDOW_TITLE 0x2B5EA7u
#define FB_BUTTON_BG    0xDDDDDDu
#define FB_BUTTON_HOVER 0xC0C8D8u
#define FB_SHADOW       0x00000060u
#define FB_DESKTOP_BG   0x1A5276u  /* тёмно-синий рабочий стол */

/* ─── Прямоугольник ─── */
typedef struct {
    int32_t x, y;
    int32_t w, h;
} fb_rect_t;

/* ─── Точка ─── */
typedef struct { int32_t x, y; } fb_point_t;

/* ─── Инфо о фреймбуфере ─── */
typedef struct {
    uint32_t* addr;     /* виртуальный адрес начала буфера */
    uint32_t  width;    /* ширина в пикселях */
    uint32_t  height;   /* высота в пикселях */
    uint32_t  pitch;    /* байт на строку */
    uint8_t   bpp;      /* бит на пиксель (обычно 32) */
    bool      ready;    /* инициализирован ли */
} fb_info_t;

/* ─── API ─── */

/* Инициализация из Multiboot2 тега (type=8) */
bool fb_init(uint32_t addr, uint32_t width, uint32_t height,
             uint32_t pitch, uint8_t bpp);

/* Размеры */
uint32_t fb_width(void);
uint32_t fb_height(void);
bool     fb_ready(void);

/* Двойная буферизация */
void fb_swap(void);          /* back → front (vsync-safe blit) */
void fb_clear(fb_color_t c); /* залить back buffer цветом */

/* Примитивы (рисуют в back buffer) */
void fb_put_pixel(int32_t x, int32_t y, fb_color_t c);
void fb_draw_hline(int32_t x, int32_t y, int32_t len, fb_color_t c);
void fb_draw_vline(int32_t x, int32_t y, int32_t len, fb_color_t c);
void fb_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, fb_color_t c);
void fb_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, fb_color_t c);
void fb_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, fb_color_t c);
void fb_fill_circle(int32_t cx, int32_t cy, int32_t r, fb_color_t c);
void fb_draw_circle(int32_t cx, int32_t cy, int32_t r, fb_color_t c);

/* Скруглённый прямоугольник */
void fb_fill_round_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                        int32_t r, fb_color_t c);

/* Текст (встроенный шрифт 8×16) */
void fb_draw_char(int32_t x, int32_t y, char c,
                  fb_color_t fg, fb_color_t bg, bool transparent_bg);
void fb_draw_string(int32_t x, int32_t y, const char* s,
                    fb_color_t fg, fb_color_t bg, bool transparent_bg);
void fb_draw_string_scaled(int32_t x, int32_t y, const char* s,
                           fb_color_t fg, uint32_t scale);

/* Размеры шрифта */
#define FB_FONT_W  8
#define FB_FONT_H  16

/* Копирование прямоугольной области */
void fb_blit(uint32_t* src, int32_t sx, int32_t sy, uint32_t src_w,
             int32_t dx, int32_t dy, int32_t w, int32_t h);

/* Прокрутка вверх на N строк пикселей */
void fb_scroll_up(int32_t lines, fb_color_t fill);

/* Смешивание цветов (alpha blend) */
fb_color_t fb_blend(fb_color_t dst, fb_color_t src, uint8_t alpha);

/* Получить указатель на back buffer (для прямой записи) */
uint32_t* fb_get_backbuf(void);

#endif /* FRAMEBUFFER_H */
