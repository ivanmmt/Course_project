/* =============================================================================
 * drivers/vga/vga_text.c
 * Реализация текстового VGA драйвера
 * ============================================================================= */

#include "vga_text.h"
#include "../../kernel/kernel.h"
#include <stdarg.h>

static size_t    vga_row   = 0;
static size_t    vga_col   = 0;
static uint8_t   vga_color = 0;
static uint16_t* vga_buf   = (uint16_t*)VGA_TEXT_BUFFER;

#define VGA_CTRL_PORT  0x3D4
#define VGA_DATA_PORT  0x3D5

void vga_init(void) {
    vga_color = vga_make_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_row   = 0;
    vga_col   = 0;
    vga_clear();
}

void vga_clear(void) {
    uint16_t blank = vga_make_entry(' ', vga_color);
    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            vga_buf[y * VGA_WIDTH + x] = blank;
    vga_row = 0;
    vga_col = 0;
    vga_set_cursor(0, 0);
}

void vga_set_color(vga_color_t fg, vga_color_t bg) {
    vga_color = vga_make_color(fg, bg);
}

void vga_set_cursor(size_t x, size_t y) {
    uint16_t pos = (uint16_t)(y * VGA_WIDTH + x);
    outb(VGA_CTRL_PORT, 0x0F);
    outb(VGA_DATA_PORT, (uint8_t)(pos & 0xFF));
    outb(VGA_CTRL_PORT, 0x0E);
    outb(VGA_DATA_PORT, (uint8_t)((pos >> 8) & 0xFF));
}

static void vga_scroll(void) {
    for (size_t y = 0; y < VGA_HEIGHT - 1; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            vga_buf[y * VGA_WIDTH + x] = vga_buf[(y+1) * VGA_WIDTH + x];
    uint16_t blank = vga_make_entry(' ', vga_color);
    for (size_t x = 0; x < VGA_WIDTH; x++)
        vga_buf[(VGA_HEIGHT-1) * VGA_WIDTH + x] = blank;
    vga_row = VGA_HEIGHT - 1;
}

void vga_put_char(char c) {
    if (c == '\n') {
        vga_col = 0;
        if (++vga_row >= VGA_HEIGHT) vga_scroll();
    } else if (c == '\r') {
        vga_col = 0;
    } else if (c == '\t') {
        vga_col = (vga_col + 8) & ~(size_t)7;
        if (vga_col >= VGA_WIDTH) {
            vga_col = 0;
            if (++vga_row >= VGA_HEIGHT) vga_scroll();
        }
    } else if (c == '\b') {
        if (vga_col > 0)
            vga_buf[vga_row * VGA_WIDTH + --vga_col] = vga_make_entry(' ', vga_color);
    } else {
        vga_buf[vga_row * VGA_WIDTH + vga_col] = vga_make_entry(c, vga_color);
        if (++vga_col >= VGA_WIDTH) {
            vga_col = 0;
            if (++vga_row >= VGA_HEIGHT) vga_scroll();
        }
    }
    vga_set_cursor(vga_col, vga_row);
}

void vga_put_string(const char* str) {
    while (*str) vga_put_char(*str++);
}

void vga_put_at(char c, uint8_t color, size_t x, size_t y) {
    vga_buf[y * VGA_WIDTH + x] = vga_make_entry(c, color);
}

static void uint_to_str(uint32_t n, char* buf, int base, int uppercase) {
    const char* lo = "0123456789abcdef";
    const char* up = "0123456789ABCDEF";
    const char* d  = uppercase ? up : lo;
    char tmp[32]; int i = 0;
    if (n == 0) { buf[0]='0'; buf[1]='\0'; return; }
    while (n > 0) { tmp[i++] = d[n % base]; n /= base; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = '\0';
}

void vga_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[32];
    while (*fmt) {
        if (*fmt != '%') { vga_put_char(*fmt++); continue; }
        fmt++;
        switch (*fmt) {
            case 's': { const char* s = va_arg(args, const char*); vga_put_string(s ? s : "(null)"); break; }
            case 'c': vga_put_char((char)va_arg(args, int)); break;
            case 'd': { int32_t n = va_arg(args, int32_t); if(n<0){vga_put_char('-');n=-n;} uint_to_str((uint32_t)n,buf,10,0); vga_put_string(buf); break; }
            case 'u': uint_to_str(va_arg(args, uint32_t), buf, 10, 0); vga_put_string(buf); break;
            case 'x': uint_to_str(va_arg(args, uint32_t), buf, 16, 0); vga_put_string(buf); break;
            case 'X': uint_to_str(va_arg(args, uint32_t), buf, 16, 1); vga_put_string(buf); break;
            case '%': vga_put_char('%'); break;
            default:  vga_put_char('%'); vga_put_char(*fmt); break;
        }
        fmt++;
    }
    va_end(args);
}
