/* =============================================================================
 * drivers/vga/vga_text.h
 * Драйвер текстового режима VGA (80x25 символов)
 * ============================================================================= */

#ifndef VGA_TEXT_H
#define VGA_TEXT_H

#include <stdint.h>
#include <stddef.h>

#define VGA_TEXT_BUFFER  0xB8000
#define VGA_WIDTH        80
#define VGA_HEIGHT       25

typedef enum {
    VGA_COLOR_BLACK         = 0,
    VGA_COLOR_BLUE          = 1,
    VGA_COLOR_GREEN         = 2,
    VGA_COLOR_CYAN          = 3,
    VGA_COLOR_RED           = 4,
    VGA_COLOR_MAGENTA       = 5,
    VGA_COLOR_BROWN         = 6,
    VGA_COLOR_LIGHT_GREY    = 7,
    VGA_COLOR_DARK_GREY     = 8,
    VGA_COLOR_LIGHT_BLUE    = 9,
    VGA_COLOR_LIGHT_GREEN   = 10,
    VGA_COLOR_LIGHT_CYAN    = 11,
    VGA_COLOR_LIGHT_RED     = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_YELLOW        = 14,
    VGA_COLOR_WHITE         = 15,
} vga_color_t;

static inline uint8_t vga_make_color(vga_color_t fg, vga_color_t bg) {
    return (uint8_t)((bg << 4) | fg);
}
static inline uint16_t vga_make_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

void vga_init(void);
void vga_clear(void);
void vga_set_color(vga_color_t fg, vga_color_t bg);
void vga_put_char(char c);
void vga_put_string(const char* str);
void vga_put_at(char c, uint8_t color, size_t x, size_t y);
void vga_set_cursor(size_t x, size_t y);
void vga_printf(const char* fmt, ...);

#endif /* VGA_TEXT_H */
