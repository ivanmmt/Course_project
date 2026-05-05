/* =============================================================================
 * drivers/framebuffer/framebuffer.c
 * Пиксельный фреймбуфер: примитивы, шрифт 8×16, двойная буферизация
 * ============================================================================= */

#include "framebuffer.h"
#include "../../memory/heap.h"
#include "../../memory/vmm.h"
#include "../../memory/pmm.h"
#include "../../lib/string.h"
#include "../../kernel/kernel.h"

/* ─── Состояние фреймбуфера ─── */
static fb_info_t  fb;
static uint32_t*  back_buffer  = (uint32_t*)0;  /* выделяется в heap */

/* =============================================================================
 * Встроенный шрифт 8×16 пикселей (кодировка ASCII 32-127)
 * Каждый символ = 16 байт (по 1 байту на строку, бит = пиксель)
 * Источник: стандартный VGA BIOS шрифт (public domain)
 * ============================================================================= */
static const uint8_t font8x16[96][16] = {
    /* 0x20 ' ' */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x21 '!' */ {0,0,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0,0x18,0x18,0,0,0,0},
    /* 0x22 '"' */ {0,0,0x66,0x66,0x66,0x24,0,0,0,0,0,0,0,0,0,0},
    /* 0x23 '#' */ {0,0,0x66,0x66,0xFF,0x66,0x66,0xFF,0x66,0x66,0,0,0,0,0,0},
    /* 0x24 '$' */ {0,0x18,0x7E,0xDB,0xD8,0xD8,0x7E,0x1B,0x1B,0xDB,0x7E,0x18,0,0,0,0},
    /* 0x25 '%' */ {0,0,0xC6,0xC6,0x0C,0x18,0x30,0x60,0xC6,0xC6,0,0,0,0,0,0},
    /* 0x26 '&' */ {0,0,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0x76,0,0,0,0,0},
    /* 0x27 '\'' */ {0,0,0x18,0x18,0x18,0x30,0,0,0,0,0,0,0,0,0,0},
    /* 0x28 '(' */ {0,0,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0,0,0,0,0},
    /* 0x29 ')' */ {0,0,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0,0,0,0,0},
    /* 0x2A '*' */ {0,0,0,0,0x66,0x3C,0xFF,0x3C,0x66,0,0,0,0,0,0,0},
    /* 0x2B '+' */ {0,0,0,0,0x18,0x18,0x7E,0x18,0x18,0,0,0,0,0,0,0},
    /* 0x2C ',' */ {0,0,0,0,0,0,0,0,0,0x38,0x38,0x70,0,0,0,0},
    /* 0x2D '-' */ {0,0,0,0,0,0,0x7E,0,0,0,0,0,0,0,0,0},
    /* 0x2E '.' */ {0,0,0,0,0,0,0,0,0,0x38,0x38,0,0,0,0,0},
    /* 0x2F '/' */ {0,0,0x03,0x06,0x0C,0x18,0x30,0x60,0xC0,0,0,0,0,0,0,0},
    /* 0x30 '0' */ {0,0,0x3C,0x66,0xC3,0xC3,0xDB,0xC3,0xC3,0x66,0x3C,0,0,0,0,0},
    /* 0x31 '1' */ {0,0,0x18,0x38,0x58,0x18,0x18,0x18,0x18,0x18,0x7E,0,0,0,0,0},
    /* 0x32 '2' */ {0,0,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC6,0xFE,0,0,0,0,0},
    /* 0x33 '3' */ {0,0,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0xC6,0x7C,0,0,0,0,0},
    /* 0x34 '4' */ {0,0,0x0E,0x1E,0x36,0x66,0xC6,0xFF,0x06,0x06,0x0F,0,0,0,0,0},
    /* 0x35 '5' */ {0,0,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0,0,0,0,0},
    /* 0x36 '6' */ {0,0,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0x7C,0,0,0,0,0},
    /* 0x37 '7' */ {0,0,0xFF,0xC6,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0,0,0,0,0},
    /* 0x38 '8' */ {0,0,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0x7C,0,0,0,0,0},
    /* 0x39 '9' */ {0,0,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x0C,0x78,0,0,0,0,0},
    /* 0x3A ':' */ {0,0,0,0,0x38,0x38,0,0,0x38,0x38,0,0,0,0,0,0},
    /* 0x3B ';' */ {0,0,0,0,0x38,0x38,0,0,0,0x38,0x38,0x70,0,0,0,0},
    /* 0x3C '<' */ {0,0,0,0x0E,0x1C,0x38,0x70,0x38,0x1C,0x0E,0,0,0,0,0,0},
    /* 0x3D '=' */ {0,0,0,0,0,0x7E,0,0x7E,0,0,0,0,0,0,0,0},
    /* 0x3E '>' */ {0,0,0,0x70,0x38,0x1C,0x0E,0x1C,0x38,0x70,0,0,0,0,0,0},
    /* 0x3F '?' */ {0,0,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0,0x18,0x18,0,0,0,0,0},
    /* 0x40 '@' */ {0,0,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x7C,0,0,0,0,0},
    /* 0x41 'A' */ {0,0,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0,0,0,0,0},
    /* 0x42 'B' */ {0,0,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0xFC,0,0,0,0,0},
    /* 0x43 'C' */ {0,0,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0,0,0,0,0},
    /* 0x44 'D' */ {0,0,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0,0,0,0,0},
    /* 0x45 'E' */ {0,0,0xFE,0x62,0x60,0x64,0x7C,0x64,0x60,0x62,0xFE,0,0,0,0,0},
    /* 0x46 'F' */ {0,0,0xFE,0x66,0x60,0x64,0x7C,0x64,0x60,0x60,0xF0,0,0,0,0,0},
    /* 0x47 'G' */ {0,0,0x3C,0x66,0xC2,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0,0,0,0,0},
    /* 0x48 'H' */ {0,0,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0,0,0,0,0},
    /* 0x49 'I' */ {0,0,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0},
    /* 0x4A 'J' */ {0,0,0x1E,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0,0,0,0,0},
    /* 0x4B 'K' */ {0,0,0xE6,0x66,0x6C,0x6C,0x78,0x6C,0x6C,0x66,0xE6,0,0,0,0,0},
    /* 0x4C 'L' */ {0,0,0xF0,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0,0,0,0,0},
    /* 0x4D 'M' */ {0,0,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0,0,0,0,0},
    /* 0x4E 'N' */ {0,0,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0,0,0,0,0},
    /* 0x4F 'O' */ {0,0,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0,0},
    /* 0x50 'P' */ {0,0,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0xF0,0,0,0,0,0},
    /* 0x51 'Q' */ {0,0,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xDC,0x7C,0x1E,0,0,0,0},
    /* 0x52 'R' */ {0,0,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0xE6,0,0,0,0,0},
    /* 0x53 'S' */ {0,0,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0xC6,0xC6,0x7C,0,0,0,0,0},
    /* 0x54 'T' */ {0,0,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0},
    /* 0x55 'U' */ {0,0,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0,0},
    /* 0x56 'V' */ {0,0,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0,0,0,0,0},
    /* 0x57 'W' */ {0,0,0xC6,0xC6,0xC6,0xD6,0xD6,0xFE,0x6C,0x6C,0x6C,0,0,0,0,0},
    /* 0x58 'X' */ {0,0,0xC6,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0xC6,0xC6,0,0,0,0,0},
    /* 0x59 'Y' */ {0,0,0xCC,0xCC,0xCC,0xCC,0x78,0x30,0x30,0x30,0x78,0,0,0,0,0},
    /* 0x5A 'Z' */ {0,0,0xFF,0xC6,0x8C,0x18,0x30,0x60,0xC2,0xC6,0xFF,0,0,0,0,0},
    /* 0x5B '[' */ {0,0,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0,0,0,0,0},
    /* 0x5C '\\' */ {0,0,0xC0,0x60,0x30,0x18,0x0C,0x06,0x03,0,0,0,0,0,0,0},
    /* 0x5D ']' */ {0,0,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0,0,0,0,0},
    /* 0x5E '^' */ {0,0x08,0x1C,0x36,0x63,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x5F '_' */ {0,0,0,0,0,0,0,0,0,0,0,0xFF,0,0,0,0},
    /* 0x60 '`' */ {0,0,0x18,0x18,0x0C,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x61 'a' */ {0,0,0,0,0,0x78,0x0C,0x7C,0xCC,0xCC,0x76,0,0,0,0,0},
    /* 0x62 'b' */ {0,0,0xE0,0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0xDC,0,0,0,0,0},
    /* 0x63 'c' */ {0,0,0,0,0,0x7C,0xC6,0xC0,0xC0,0xC6,0x7C,0,0,0,0,0},
    /* 0x64 'd' */ {0,0,0x1C,0x0C,0x0C,0x7C,0xCC,0xCC,0xCC,0xCC,0x76,0,0,0,0,0},
    /* 0x65 'e' */ {0,0,0,0,0,0x7C,0xC6,0xFE,0xC0,0xC6,0x7C,0,0,0,0,0},
    /* 0x66 'f' */ {0,0,0x1C,0x36,0x30,0x30,0xFC,0x30,0x30,0x30,0x78,0,0,0,0,0},
    /* 0x67 'g' */ {0,0,0,0,0,0x76,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0,0,0},
    /* 0x68 'h' */ {0,0,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0xE6,0,0,0,0,0},
    /* 0x69 'i' */ {0,0,0x18,0x18,0,0x38,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0},
    /* 0x6A 'j' */ {0,0,0x06,0x06,0,0x0E,0x06,0x06,0x06,0x66,0x66,0x3C,0,0,0,0},
    /* 0x6B 'k' */ {0,0,0xE0,0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0xE6,0,0,0,0,0},
    /* 0x6C 'l' */ {0,0,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0,0,0,0,0},
    /* 0x6D 'm' */ {0,0,0,0,0,0xEC,0xFE,0xD6,0xD6,0xD6,0xC6,0,0,0,0,0},
    /* 0x6E 'n' */ {0,0,0,0,0,0xDC,0x66,0x66,0x66,0x66,0x66,0,0,0,0,0},
    /* 0x6F 'o' */ {0,0,0,0,0,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0,0,0,0,0},
    /* 0x70 'p' */ {0,0,0,0,0,0xDC,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0,0,0},
    /* 0x71 'q' */ {0,0,0,0,0,0x76,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0,0,0},
    /* 0x72 'r' */ {0,0,0,0,0,0xDC,0x76,0x66,0x60,0x60,0xF0,0,0,0,0,0},
    /* 0x73 's' */ {0,0,0,0,0,0x7C,0xC6,0x70,0x1C,0xC6,0x7C,0,0,0,0,0},
    /* 0x74 't' */ {0,0,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x36,0x1C,0,0,0,0,0},
    /* 0x75 'u' */ {0,0,0,0,0,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0,0,0,0,0},
    /* 0x76 'v' */ {0,0,0,0,0,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0,0,0,0,0},
    /* 0x77 'w' */ {0,0,0,0,0,0xC6,0xC6,0xD6,0xD6,0xFE,0x6C,0,0,0,0,0},
    /* 0x78 'x' */ {0,0,0,0,0,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0,0,0,0,0},
    /* 0x79 'y' */ {0,0,0,0,0,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0,0,0},
    /* 0x7A 'z' */ {0,0,0,0,0,0xFF,0x86,0x0C,0x30,0x62,0xFF,0,0,0,0,0},
    /* 0x7B '{' */ {0,0,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x0E,0,0,0,0,0},
    /* 0x7C '|' */ {0,0,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0,0,0,0,0},
    /* 0x7D '}' */ {0,0,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x70,0,0,0,0,0},
    /* 0x7E '~' */ {0,0x76,0xDC,0,0,0,0,0,0,0,0,0,0,0,0,0},
    /* 0x7F DEL */ {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
};

/* =============================================================================
 * fb_init — инициализация из данных Multiboot2
 * ============================================================================= */
bool fb_init(uint32_t addr, uint32_t width, uint32_t height,
             uint32_t pitch, uint8_t bpp) {
    if (bpp < 24 || width == 0 || height == 0) return false;

    fb.addr   = (uint32_t*)(uint32_t)addr;
    fb.width  = width;
    fb.height = height;
    fb.pitch  = pitch;
    fb.bpp    = bpp;
    fb.ready  = true;

    /* Маппируем фреймбуфер в виртуальное адресное пространство */
    uint32_t fb_size = pitch * height;
    uint32_t phys    = (uint32_t)addr;
    for (uint32_t off = 0; off < fb_size; off += 4096) {
        vmm_map_page(phys + off, phys + off, VMM_KERNEL_FLAGS);
    }

    /* Выделяем back buffer в heap */
    size_t buf_size = (size_t)width * height * sizeof(uint32_t);
    back_buffer = (uint32_t*)kmalloc(buf_size);
    if (!back_buffer) {
        /* Fallback: используем фреймбуфер напрямую */
        back_buffer = fb.addr;
    }

    /* Очищаем экран */
    fb_clear(FB_BLACK);
    fb_swap();

    return true;
}

uint32_t fb_width(void)  { return fb.width; }
uint32_t fb_height(void) { return fb.height; }
bool     fb_ready(void)  { return fb.ready; }
uint32_t* fb_get_backbuf(void) { return back_buffer; }

/* =============================================================================
 * fb_swap — копируем back buffer → front buffer (реальный фреймбуфер)
 * Pitch может отличаться от width*4, поэтому копируем построчно
 * ============================================================================= */
void fb_swap(void) {
    if (!fb.ready || back_buffer == fb.addr) return;

    uint8_t* dst = (uint8_t*)fb.addr;
    uint32_t* src = back_buffer;
    uint32_t bytes_per_row = fb.width * 4;

    for (uint32_t y = 0; y < fb.height; y++) {
        memcpy(dst, src, bytes_per_row);
        dst += fb.pitch;
        src += fb.width;
    }
}

/* =============================================================================
 * fb_clear — заполнить back buffer одним цветом
 * ============================================================================= */
void fb_clear(fb_color_t c) {
    if (!fb.ready) return;
    uint32_t total = fb.width * fb.height;
    uint32_t* p = back_buffer;
    for (uint32_t i = 0; i < total; i++) p[i] = c;
}

/* =============================================================================
 * Базовые примитивы — записываем пиксели в back buffer
 * ============================================================================= */
static inline bool fb_clip(int32_t x, int32_t y) {
    return (x >= 0 && y >= 0 &&
            (uint32_t)x < fb.width && (uint32_t)y < fb.height);
}

void fb_put_pixel(int32_t x, int32_t y, fb_color_t c) {
    if (!fb.ready || !fb_clip(x, y)) return;
    back_buffer[(uint32_t)y * fb.width + (uint32_t)x] = c;
}

void fb_draw_hline(int32_t x, int32_t y, int32_t len, fb_color_t c) {
    if (!fb.ready || y < 0 || (uint32_t)y >= fb.height) return;
    int32_t x0 = (x < 0) ? 0 : x;
    int32_t x1 = x + len;
    if (x1 > (int32_t)fb.width) x1 = (int32_t)fb.width;
    uint32_t* row = back_buffer + (uint32_t)y * fb.width;
    for (int32_t i = x0; i < x1; i++) row[i] = c;
}

void fb_draw_vline(int32_t x, int32_t y, int32_t len, fb_color_t c) {
    if (!fb.ready || x < 0 || (uint32_t)x >= fb.width) return;
    for (int32_t i = 0; i < len; i++) fb_put_pixel(x, y + i, c);
}

/* Алгоритм Брезенхема */
void fb_draw_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, fb_color_t c) {
    int32_t dx = (x1>x0)?(x1-x0):(x0-x1), sx = x0<x1?1:-1;
    int32_t dy = (y1>y0)?(y1-y0):(y0-y1), sy = y0<y1?1:-1;
    dy = -dy;
    int32_t err = dx + dy;
    while (1) {
        fb_put_pixel(x0, y0, c);
        if (x0==x1 && y0==y1) break;
        int32_t e2 = 2*err;
        if (e2 >= dy) { err+=dy; x0+=sx; }
        if (e2 <= dx) { err+=dx; y0+=sy; }
    }
}

void fb_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, fb_color_t c) {
    for (int32_t row = 0; row < h; row++)
        fb_draw_hline(x, y + row, w, c);
}

void fb_draw_rect(int32_t x, int32_t y, int32_t w, int32_t h, fb_color_t c) {
    fb_draw_hline(x, y,         w, c);
    fb_draw_hline(x, y + h - 1, w, c);
    fb_draw_vline(x,         y, h, c);
    fb_draw_vline(x + w - 1, y, h, c);
}

/* Алгоритм Брезенхема для окружности */
static void circle_points(int32_t cx, int32_t cy, int32_t x, int32_t y,
                           fb_color_t c, bool fill) {
    if (fill) {
        fb_draw_hline(cx - x, cy + y, 2*x+1, c);
        fb_draw_hline(cx - x, cy - y, 2*x+1, c);
        fb_draw_hline(cx - y, cy + x, 2*y+1, c);
        fb_draw_hline(cx - y, cy - x, 2*y+1, c);
    } else {
        fb_put_pixel(cx+x, cy+y, c); fb_put_pixel(cx-x, cy+y, c);
        fb_put_pixel(cx+x, cy-y, c); fb_put_pixel(cx-x, cy-y, c);
        fb_put_pixel(cx+y, cy+x, c); fb_put_pixel(cx-y, cy+x, c);
        fb_put_pixel(cx+y, cy-x, c); fb_put_pixel(cx-y, cy-x, c);
    }
}

static void draw_circle_impl(int32_t cx, int32_t cy, int32_t r,
                              fb_color_t c, bool fill) {
    int32_t x = 0, y = r, d = 3 - 2*r;
    while (y >= x) {
        circle_points(cx, cy, x, y, c, fill);
        x++;
        if (d > 0) { y--; d += 4*(x-y)+10; }
        else       { d += 4*x+6; }
    }
}

void fb_fill_circle(int32_t cx, int32_t cy, int32_t r, fb_color_t c) {
    draw_circle_impl(cx, cy, r, c, true);
}
void fb_draw_circle(int32_t cx, int32_t cy, int32_t r, fb_color_t c) {
    draw_circle_impl(cx, cy, r, c, false);
}

/* Скруглённый прямоугольник */
void fb_fill_round_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                        int32_t r, fb_color_t c) {
    /* Центральная полоса */
    fb_fill_rect(x + r, y,     w - 2*r, h,     c);
    fb_fill_rect(x,     y + r, r,       h-2*r, c);
    fb_fill_rect(x+w-r, y + r, r,       h-2*r, c);
    /* Четыре угла */
    fb_fill_circle(x+r,     y+r,     r, c);
    fb_fill_circle(x+w-1-r, y+r,     r, c);
    fb_fill_circle(x+r,     y+h-1-r, r, c);
    fb_fill_circle(x+w-1-r, y+h-1-r, r, c);
}

/* =============================================================================
 * Текст — растровый шрифт 8×16
 * ============================================================================= */
void fb_draw_char(int32_t x, int32_t y, char ch,
                  fb_color_t fg, fb_color_t bg, bool transparent_bg) {
    if (!fb.ready) return;
    uint8_t idx = (uint8_t)ch;
    if (idx < 32 || idx > 127) idx = '?';
    const uint8_t* glyph = font8x16[idx - 32];

    for (int row = 0; row < FB_FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FB_FONT_W; col++) {
            bool set = (bits >> (7 - col)) & 1;
            if (set)
                fb_put_pixel(x + col, y + row, fg);
            else if (!transparent_bg)
                fb_put_pixel(x + col, y + row, bg);
        }
    }
}

void fb_draw_string(int32_t x, int32_t y, const char* s,
                    fb_color_t fg, fb_color_t bg, bool transparent_bg) {
    int32_t cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += FB_FONT_H; }
        else {
            fb_draw_char(cx, y, *s, fg, bg, transparent_bg);
            cx += FB_FONT_W;
        }
        s++;
    }
}

void fb_draw_string_scaled(int32_t x, int32_t y, const char* s,
                           fb_color_t fg, uint32_t scale) {
    if (scale == 0) scale = 1;
    int32_t cx = x;
    while (*s) {
        if (*s == '\n') { cx = x; y += (int32_t)(FB_FONT_H * scale); }
        else {
            uint8_t idx = (uint8_t)*s;
            if (idx < 32 || idx > 127) idx = '?';
            const uint8_t* glyph = font8x16[idx - 32];
            for (int row = 0; row < FB_FONT_H; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FB_FONT_W; col++) {
                    if ((bits >> (7 - col)) & 1) {
                        fb_fill_rect(cx + (int32_t)(col*scale),
                                     y  + (int32_t)(row*scale),
                                     (int32_t)scale, (int32_t)scale, fg);
                    }
                }
            }
            cx += (int32_t)(FB_FONT_W * scale);
        }
        s++;
    }
}

/* =============================================================================
 * fb_blend — альфа-смешивание двух цветов
 * alpha = 0 → полностью dst, 255 → полностью src
 * ============================================================================= */
fb_color_t fb_blend(fb_color_t dst, fb_color_t src, uint8_t alpha) {
    uint32_t a = alpha;
    uint32_t ia = 255 - a;
    uint8_t r = (uint8_t)(((dst >> 16 & 0xFF)*ia + (src >> 16 & 0xFF)*a) >> 8);
    uint8_t g = (uint8_t)(((dst >>  8 & 0xFF)*ia + (src >>  8 & 0xFF)*a) >> 8);
    uint8_t b = (uint8_t)(((dst       & 0xFF)*ia + (src       & 0xFF)*a) >> 8);
    return fb_rgb(r, g, b);
}

/* =============================================================================
 * fb_blit — копировать прямоугольную область из src в back buffer
 * ============================================================================= */
void fb_blit(uint32_t* src, int32_t sx, int32_t sy, uint32_t src_w,
             int32_t dx, int32_t dy, int32_t w, int32_t h) {
    for (int32_t row = 0; row < h; row++) {
        int32_t srow = sy + row;
        int32_t drow = dy + row;
        if (drow < 0 || (uint32_t)drow >= fb.height) continue;
        for (int32_t col = 0; col < w; col++) {
            int32_t dcol = dx + col;
            if (dcol < 0 || (uint32_t)dcol >= fb.width) continue;
            back_buffer[(uint32_t)drow * fb.width + (uint32_t)dcol] =
                src[(uint32_t)srow * src_w + (uint32_t)(sx + col)];
        }
    }
}

/* =============================================================================
 * fb_scroll_up — прокрутить экран вверх на N строк пикселей
 * ============================================================================= */
void fb_scroll_up(int32_t lines, fb_color_t fill) {
    if (lines <= 0 || !fb.ready) return;
    uint32_t move_rows = (lines < (int32_t)fb.height) ?
        fb.height - (uint32_t)lines : 0;

    if (move_rows > 0) {
        memmove(back_buffer,
                back_buffer + (uint32_t)lines * fb.width,
                move_rows * fb.width * sizeof(uint32_t));
    }
    /* Очищаем нижние строки */
    uint32_t* p = back_buffer + move_rows * fb.width;
    uint32_t  n = (uint32_t)lines * fb.width;
    for (uint32_t i = 0; i < n; i++) p[i] = fill;
}
