/* =============================================================================
 * programs/wordproc/wordproc.c
 * Текстовый процессор MyWord — аналог Microsoft Word
 *
 * Возможности:
 *   - Форматированный текст (жирный, курсив, подчёркивание)
 *   - Размеры шрифтов (обычный, крупный, заголовок)
 *   - Выравнивание (левое, центр, правое)
 *   - Нумерация страниц
 *   - Панель инструментов в стиле Word
 *   - Автосохранение
 *   - Открытие/сохранение файлов .doc, .rtf, .txt
 * ============================================================================= */

#include "wordproc.h"
#include "../../gui/gui.h"
#include "../../drivers/framebuffer/framebuffer.h"
#include "../../drivers/keyboard/keyboard.h"
#include "../../drivers/mouse/mouse.h"
#include "../../drivers/timer/pit.h"
#include "../../filesystem/vfs.h"
#include "../../memory/heap.h"
#include "../../lib/string.h"

/* ─── Константы ─── */
#define WP_TOOLBAR_H    56
#define WP_RULER_H      24
#define WP_STATUSBAR_H  22
#define WP_MAX_PAGES    64
#define WP_MAX_BLOCKS   4096
#define WP_LINE_H       20
#define WP_MARGIN_LEFT  72
#define WP_MARGIN_RIGHT 72
#define WP_MARGIN_TOP   36
#define WP_MARGIN_BOT   36

/* ─── Цвета ─── */
#define WP_BG          0xFFFFFFu
#define WP_RULER_BG    0xF0F0F0u
#define WP_TOOLBAR_BG  0xF5F5F5u
#define WP_STATUS_BG   0xE8E8E8u
#define WP_SEL_BG    0x0078D4u
#define WP_SEL_FG    0xFFFFFFu
#define WP_BTN_BG    0xE0E0E0u
#define WP_BTN_H     0xC8D8F0u
#define WP_BTN_ACTIVE 0x0078D4u
#define WP_TEXT      0x000000u
#define WP_BORDER    0xC0C0C0u

/* ─── Стили ─── */
#define STYLE_NORMAL  0
#define STYLE_BOLD    1
#define STYLE_ITALIC  2
#define STYLE_UNDERL  4
#define STYLE_BIG     8
#define STYLE_HEADER  16

#define ALIGN_LEFT    0
#define ALIGN_CENTER  32
#define ALIGN_RIGHT   64

/* ─── Текстовый блок ─── */
typedef struct {
    char*    text;
    uint32_t len;
    uint32_t cap;
    uint8_t  style;       /* STYLE_* биты */
    uint8_t  align;       /* ALIGN_* */
    uint8_t  font_size;   /* 10-24 */
} wp_block_t;

/* ─── Страница ─── */
typedef struct {
    int block_start;
    int block_count;
    int line_count;
} wp_page_t;

/* ─── Состояние ─── */
typedef struct {
    /* Документ */
    wp_block_t blocks[WP_MAX_BLOCKS];
    int        block_count;
    int        cur_block;
    int        cur_pos;

    /* Страницы */
    wp_page_t  pages[WP_MAX_PAGES];
    int        page_count;
    int        cur_page;
    int        page_scroll;

    /* Форматирование */
    uint8_t    default_style;
    uint8_t    default_align;
    uint8_t    default_size;

    /* UI */
    bool       running;
    bool       modified;
    char       filename[256];
    char       doc_title[128];

    /* Размеры */
    int32_t    win_x, win_y, win_w, win_h;
    int32_t    content_x, content_y, content_w, content_h;
    int32_t    page_w, page_h;
    int        lines_per_page;

    /* Скролл */
    int        scroll_y;
    int        visible_lines;

    /* Выделение */
    int sel_start_block, sel_start_pos;
    int sel_end_block,   sel_end_pos;
    bool selecting;

    /* Тулбар кнопки */
    struct { int32_t x, y, w, h; bool active; } btns[16];
    int btn_count;

} wp_state_t;

/* =============================================================================
 * Вспомогательные функции
 * ============================================================================= */

static wp_block_t* wp_block_new(wp_state_t* wp) {
    if (wp->block_count >= WP_MAX_BLOCKS) return (wp_block_t*)0;
    wp_block_t* b = &wp->blocks[wp->block_count++];
    memset(b, 0, sizeof(*b));
    b->cap = 256;
    b->text = (char*)kmalloc(b->cap);
    if (b->text) { b->text[0] = '\0'; b->len = 0; }
    b->style = wp->default_style;
    b->align = wp->default_align;
    b->font_size = wp->default_size;
    return b;
}

static void wp_block_ensure(wp_block_t* b, uint32_t needed) {
    if (needed <= b->cap) return;
    uint32_t nc = needed * 2;
    if (nc < 512) nc = 512;
    char* nt = (char*)krealloc(b->text, nc);
    if (!nt) return;
    b->text = nt;
    b->cap = nc;
}

static void wp_block_insert(wp_block_t* b, int pos, char c) {
    wp_block_ensure(b, b->len + 2);
    for (uint32_t i = b->len; i > (uint32_t)pos; i--)
        b->text[i] = b->text[i-1];
    b->text[pos] = c;
    b->len++;
    b->text[b->len] = '\0';
}

static void wp_block_delete(wp_block_t* b, int pos) {
    if (pos < 0 || pos >= (int)b->len) return;
    for (uint32_t i = (uint32_t)pos; i < b->len; i++)
        b->text[i] = b->text[i+1];
    b->len--;
    b->text[b->len] = '\0';
}

/* =============================================================================
 * Пересчёт страниц
 * ============================================================================= */

static void wp_recalc_pages(wp_state_t* wp) {
    wp->page_count = 0;
    int cur_line = 0;
    int max_lines = wp->lines_per_page;

    for (int pi = 0; pi < WP_MAX_PAGES && cur_line < wp->block_count; pi++) {
        wp_page_t* pg = &wp->pages[pi];
        pg->block_start = cur_line;
        pg->line_count = 0;
        pg->block_count = 0;

        int lines_used = 0;
        while (cur_line < wp->block_count && lines_used < max_lines) {
            wp_block_t* b = &wp->blocks[cur_line];
            int lines_for_block = 1;
            if (b->style & STYLE_BIG) lines_for_block = 2;
            if (b->style & STYLE_HEADER) lines_for_block = 3;

            if (lines_used + lines_for_block > max_lines) break;

            lines_used += lines_for_block;
            pg->block_count++;
            pg->line_count += lines_for_block;
            cur_line++;
        }
        wp->page_count = pi + 1;
        if (lines_used < 1) break;
    }
}

/* =============================================================================
 * Рисование тулбара
 * ============================================================================= */

static void wp_draw_toolbar(wp_state_t* wp) {
    int32_t x = wp->win_x, y = wp->win_y, w = wp->win_w;

    fb_fill_rect(x, y, w, WP_TOOLBAR_H, WP_TOOLBAR_BG);
    fb_draw_hline(x, y + WP_TOOLBAR_H - 1, w, WP_BORDER);

    /* Заголовок документа */
    fb_draw_string(x + 8, y + 4, wp->doc_title, 0x003399u, WP_TOOLBAR_BG, true);

    wp->btn_count = 0;
    int bx = x + 8;
    int by = y + 26;

    struct { const char* label; int flag; fb_color_t color; } fmt_btns[] = {
        {"B",  STYLE_BOLD,    0x000000u},
        {"I",  STYLE_ITALIC,  0x000000u},
        {"U",  STYLE_UNDERL,  0x000000u},
        {"H",  STYLE_HEADER,  0x003399u},
        {"L",  STYLE_BIG,     0x006600u},
        {"<",  ALIGN_LEFT,    0x000000u},
        {"C",  ALIGN_CENTER,  0x000000u},
        {">",  ALIGN_RIGHT,   0x000000u},
    };

    for (int i = 0; i < 8 && wp->btn_count < 16; i++) {
        int bw = 24, bh = 22;
        bool active = false;
        if (wp->cur_block >= 0 && wp->cur_block < wp->block_count) {
            wp_block_t* cb = &wp->blocks[wp->cur_block];
            if (fmt_btns[i].flag & (STYLE_BOLD|STYLE_ITALIC|STYLE_UNDERL|STYLE_BIG|STYLE_HEADER))
                active = (cb->style & fmt_btns[i].flag) != 0;
            else
                active = (cb->align == fmt_btns[i].flag);
        }

        fb_color_t bg = active ? WP_BTN_ACTIVE : WP_BTN_BG;
        fb_fill_round_rect(bx, by, bw, bh, 3, bg);
        fb_draw_string(bx + 7, by + 5, fmt_btns[i].label,
                       fmt_btns[i].color, bg, true);

        wp->btns[wp->btn_count].x = bx - x;
        wp->btns[wp->btn_count].y = by - y;
        wp->btns[wp->btn_count].w = bw;
        wp->btns[wp->btn_count].h = bh;
        wp->btns[wp->btn_count].active = active;
        wp->btn_count++;
        bx += bw + 4;
    }

    /* Разделитель */
    fb_draw_vline(bx + 2, by, 22, WP_BORDER);
    bx += 8;

    /* Кнопки сохранения/открытия */
    struct { const char* label; int action; } file_btns[] = {
        {"Save", 1}, {"Open", 2}, {"New", 3},
    };
    for (int i = 0; i < 3 && wp->btn_count < 16; i++) {
        int bw = 40, bh = 22;
        fb_fill_round_rect(bx, by, bw, bh, 3, WP_BTN_BG);
        fb_draw_string(bx + 6, by + 5, file_btns[i].label, 0x004400u, WP_BTN_BG, true);
        wp->btns[wp->btn_count].x = bx - x;
        wp->btns[wp->btn_count].y = by - y;
        wp->btns[wp->btn_count].w = bw;
        wp->btns[wp->btn_count].h = bh;
        wp->btns[wp->btn_count].active = false;
        wp->btn_count++;
        bx += bw + 4;
    }

    /* Информация о раскладке */
    const char* layout_name = keyboard_get_layout() == LAYOUT_UA ? "UA" : "US";
    int lx = x + w - 50;
    fb_draw_string(lx, y + 6, layout_name, 0x666666u, WP_TOOLBAR_BG, true);
    fb_draw_string(lx, y + 34, keyboard_numlock() ? "NUM" : "   ", 0x666666u, WP_TOOLBAR_BG, true);
}

/* =============================================================================
 * Рисование линейки
 * ============================================================================= */

static void wp_draw_ruler(wp_state_t* wp) {
    int32_t x = wp->win_x, y = wp->win_y + WP_TOOLBAR_H;
    int32_t w = wp->win_w;

    fb_fill_rect(x, y, w, WP_RULER_H, WP_RULER_BG);
    fb_draw_hline(x, y + WP_RULER_H - 1, w, WP_BORDER);

    /* Маркеры отступов */
    int32_t left_m = x + WP_MARGIN_LEFT;
    int32_t right_m = x + w - WP_MARGIN_RIGHT;

    /* Линейка с делениями */
    for (int px = left_m; px <= right_m; px += 10) {
        int h = (px - left_m) % 50 == 0 ? 8 : 4;
        fb_fill_rect(px, y, 1, h, 0x808080u);
    }

    /* Маркеры */
    fb_fill_rect(left_m - 2, y + 8, 6, 10, 0x0066CCu);
    fb_fill_rect(right_m - 4, y + 8, 6, 10, 0x0066CCu);
}

/* =============================================================================
 * Рисование контента (страницы)
 * ============================================================================= */

static void wp_draw_content(wp_state_t* wp) {
    int32_t x = wp->win_x, y = wp->win_y + WP_TOOLBAR_H + WP_RULER_H;
    int32_t w = wp->win_w, h = wp->content_h;

    /* Фон — серый как в Word */
    fb_fill_rect(x, y, w, h, 0xC0C0C0u);

    /* Область страницы */
    int32_t px = x + (w - wp->page_w) / 2;
    int32_t py = y + 10 - wp->scroll_y * WP_LINE_H;

    /* Белая страница */
    fb_fill_rect(px, py, wp->page_w, wp->page_h, WP_BG);
    fb_draw_rect(px, py, wp->page_w, wp->page_h, 0x808080u);

    /* Рендер блоков */
    int32_t ty = py + 8;
    int content_w = wp->page_w - WP_MARGIN_LEFT - WP_MARGIN_RIGHT;

    for (int bi = 0; bi < wp->block_count; bi++) {
        wp_block_t* b = &wp->blocks[bi];
        if (!b->text || b->len == 0) {
            ty += (b->style & STYLE_HEADER) ? 24 :
                  (b->style & STYLE_BIG) ? 18 : WP_LINE_H;
            continue;
        }

        /* Определяем высоту строки */
        int line_h = WP_LINE_H;
        int font_off = 0;
        if (b->style & STYLE_BIG) { line_h = 18; font_off = 2; }
        if (b->style & STYLE_HEADER) { line_h = 24; font_off = 4; }

        /* Выравнивание */
        int32_t text_w = (int32_t)strlen(b->text) * FB_FONT_W;
        int32_t tx = px + WP_MARGIN_LEFT;
        if (b->align == ALIGN_CENTER) tx = px + WP_MARGIN_LEFT + (content_w - text_w) / 2;
        if (b->align == ALIGN_RIGHT)  tx = px + wp->page_w - WP_MARGIN_RIGHT - text_w;

        if (tx < px + WP_MARGIN_LEFT) tx = px + WP_MARGIN_LEFT;

        /* Цвет текста */
        fb_color_t fg = WP_TEXT;

        /* Рисуем текст по частям (для стиля) */
        int32_t cur_x = tx;
        for (uint32_t ci = 0; ci < b->len; ci++) {
            char c = b->text[ci];
            int32_t cw = FB_FONT_W + (b->style & STYLE_BIG ? 2 : 0) +
                         (b->style & STYLE_HEADER ? 4 : 0) + font_off;

            if (b->style & STYLE_BOLD) {
                fb_draw_char(cur_x, ty + font_off, c, fg, WP_BG, true);
                fb_draw_char(cur_x+1, ty + font_off, c, fg, WP_BG, true);
            } else {
                fb_draw_char(cur_x, ty + font_off, c, fg, WP_BG, false);
            }

            /* Подчёркивание */
            if (b->style & STYLE_UNDERL) {
                fb_draw_hline(cur_x, ty + line_h - 2 + font_off, cw, 0x0000CCu);
            }

            cur_x += cw;
        }

        ty += line_h;
    }

    /* Курсор */
    if (wp->cur_block >= 0 && wp->cur_block < wp->block_count) {
        wp_block_t* b = &wp->blocks[wp->cur_block];
        int line_h = WP_LINE_H;
        int font_off = 0;
        if (b->style & STYLE_BIG) { line_h = 18; font_off = 2; }
        if (b->style & STYLE_HEADER) { line_h = 24; font_off = 4; }

        /* Находим Y позицию курсора */
        int32_t cur_ty = py + 8;
        for (int bi2 = 0; bi2 < wp->cur_block; bi2++) {
            wp_block_t* b2 = &wp->blocks[bi2];
            if (b2->style & STYLE_HEADER) cur_ty += 24;
            else if (b2->style & STYLE_BIG) cur_ty += 18;
            else cur_ty += WP_LINE_H;
        }

        int32_t cursor_x = px + WP_MARGIN_LEFT + wp->cur_pos * FB_FONT_W;
        fb_fill_rect(cursor_x, cur_ty + font_off, 2, line_h - 2, 0x000000u);
    }

    /* Скроллбар справа */
    if (wp->block_count > wp->visible_lines) {
        int32_t sb_x = x + w - 12;
        int32_t sb_h = h - 16;
        fb_fill_rect(sb_x, y + 8, 10, sb_h, 0xD0D0D0u);
        fb_draw_rect(sb_x, y + 8, 10, sb_h, 0xA0A0A0u);

        int32_t thumb_h = sb_h * wp->visible_lines / wp->block_count;
        if (thumb_h < 20) thumb_h = 20;
        int32_t thumb_y = y + 8;
        if (wp->block_count > wp->visible_lines)
            thumb_y += (sb_h - thumb_h) * wp->scroll_y /
                       (wp->block_count - wp->visible_lines);
        fb_fill_rect(sb_x + 1, thumb_y, 8, thumb_h, 0x8080A0u);
    }
}

/* =============================================================================
 * Статусбар
 * ============================================================================= */

static void wp_draw_statusbar(wp_state_t* wp) {
    int32_t x = wp->win_x, y = wp->win_y + wp->win_h - WP_STATUSBAR_H;
    int32_t w = wp->win_w;

    fb_fill_rect(x, y, w, WP_STATUSBAR_H, WP_STATUS_BG);
    fb_draw_hline(x, y, w, WP_BORDER);

    /* Страница */
    char st[64];
    int si = 0;
    const char* p1 = "Page "; while(*p1) st[si++] = *p1++;
    int pn = wp->cur_page + 1;
    char tmp[8]; int ti = 0;
    if (!pn) { tmp[ti++] = '0'; }
    else { int n = pn; char t2[8]; int j = 0; while(n>0){t2[j++]='0'+n%10;n/=10;}while(j>0)tmp[ti++]=t2[--j];}
    tmp[ti]='\0';
    for(int i=0;tmp[i];i++) st[si++]=tmp[i];
    st[si++]=' '; st[si++]='/'; st[si++]=' ';
    int tp = wp->page_count;
    ti=0; if(!tp){tmp[ti++]='0';}else{int n=tp;char t2[8];int j=0;while(n>0){t2[j++]='0'+n%10;n/=10;}while(j>0)tmp[ti++]=t2[--j];}
    tmp[ti]='\0';
    for(int i=0;tmp[i];i++) st[si++]=tmp[i];

    /* Words count */
    int words = 0;
    for (int i = 0; i < wp->block_count; i++) {
        if (wp->blocks[i].len > 0) {
            words++;
            for (uint32_t j = 0; j < wp->blocks[i].len; j++)
                if (wp->blocks[i].text[j] == ' ') words++;
        }
    }
    const char* p2 = "   Words: "; while(*p2) st[si++] = *p2++;
    ti=0; int nw=words; if(!nw){tmp[ti++]='0';}else{char t2[8];int j=0;while(nw>0){t2[j++]='0'+nw%10;nw/=10;}while(j>0)tmp[ti++]=t2[--j];}
    tmp[ti]='\0';
    for(int i=0;tmp[i];i++) st[si++]=tmp[i];

    /* Modified */
    if (wp->modified) {
        st[si++]=' '; st[si++]='*'; st[si++]=' ';
        const char* mod = "Modified"; while(*mod) st[si++] = *mod++;
    }
    st[si] = '\0';

    fb_draw_string(x + 6, y + 4, st, 0x444444u, WP_STATUS_BG, true);

    /* Подсказки */
    const char* hints = "Ctrl+S Save  Ctrl+B Bold  Ctrl+I Italic  Ctrl+U Underline";
    int32_t hw = (int32_t)strlen(hints) * FB_FONT_W;
    fb_draw_string(x + w - hw - 6, y + 4, hints, 0x999999u, WP_STATUS_BG, true);
}

/* =============================================================================
 * Файловые операции
 * ============================================================================= */

static void wp_save_file(wp_state_t* wp) {
    if (!wp->filename[0]) return;

    int32_t fd = vfs_open(wp->filename, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) return;

    for (int i = 0; i < wp->block_count; i++) {
        wp_block_t* b = &wp->blocks[i];
        if (b->text && b->len > 0)
            vfs_write(fd, b->text, b->len);
        vfs_write(fd, "\n", 1);
    }
    vfs_close(fd);
    wp->modified = false;
}

static void wp_load_file(wp_state_t* wp, const char* path) {
    int32_t fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) {
        wp_block_new(wp);
        return;
    }

    wp->block_count = 0;

    uint8_t buf[1024];
    int32_t n;

    wp_block_t* cur = wp_block_new(wp);
    if (!cur) return;

    while ((n = vfs_read(fd, buf, sizeof(buf))) > 0) {
        for (int32_t i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n') {
                cur = wp_block_new(wp);
                if (!cur) break;
            } else if (c != '\r') {
                wp_block_insert(cur, (int)cur->len, c);
            }
        }
    }
    vfs_close(fd);
    wp->modified = false;
}

/* =============================================================================
 * Обработка ввода
 * ============================================================================= */

static void wp_handle_key(wp_state_t* wp, int c) {
    /* Ctrl+S Save */
    if (c == 19) { wp_save_file(wp); return; }
    /* Ctrl+B Bold */
    if (c == 2 && wp->cur_block >= 0) {
        wp->blocks[wp->cur_block].style ^= STYLE_BOLD;
        wp->modified = true;
        return;
    }
    /* Ctrl+I Italic */
    if (c == 9 && wp->cur_block >= 0) {
        wp->blocks[wp->cur_block].style ^= STYLE_ITALIC;
        wp->modified = true;
        return;
    }
    /* Ctrl+U Underline */
    if (c == 21 && wp->cur_block >= 0) {
        wp->blocks[wp->cur_block].style ^= STYLE_UNDERL;
        wp->modified = true;
        return;
    }

    if (wp->cur_block < 0 || wp->cur_block >= wp->block_count) return;
    wp_block_t* b = &wp->blocks[wp->cur_block];

    switch (c) {
        case KEY_ARROW_UP:
            if (wp->cur_block > 0) wp->cur_block--;
            if (wp->cur_pos > (int)wp->blocks[wp->cur_block].len)
                wp->cur_pos = (int)wp->blocks[wp->cur_block].len;
            if (wp->cur_block < wp->scroll_y) wp->scroll_y = wp->cur_block;
            break;

        case KEY_ARROW_DOWN:
            if (wp->cur_block < wp->block_count - 1) wp->cur_block++;
            if (wp->cur_pos > (int)wp->blocks[wp->cur_block].len)
                wp->cur_pos = (int)wp->blocks[wp->cur_block].len;
            if (wp->cur_block >= wp->scroll_y + wp->visible_lines)
                wp->scroll_y = wp->cur_block - wp->visible_lines + 1;
            break;

        case KEY_ARROW_LEFT:
            if (wp->cur_pos > 0) wp->cur_pos--;
            else if (wp->cur_block > 0) {
                wp->cur_block--;
                wp->cur_pos = (int)wp->blocks[wp->cur_block].len;
            }
            break;

        case KEY_ARROW_RIGHT:
            if (wp->cur_pos < (int)b->len) wp->cur_pos++;
            else if (wp->cur_block < wp->block_count - 1) {
                wp->cur_block++;
                wp->cur_pos = 0;
            }
            break;

        case '\n': case '\r': case KEY_KP_ENTER:
            /* Новый блок */
            if (wp->block_count < WP_MAX_BLOCKS) {
                for (int i = wp->block_count; i > wp->cur_block + 1; i--)
                    wp->blocks[i] = wp->blocks[i-1];
                wp_block_t* nb = &wp->blocks[wp->cur_block + 1];
                memset(nb, 0, sizeof(*nb));
                nb->cap = 256;
                nb->text = (char*)kmalloc(nb->cap);
                if (nb->text) { nb->text[0]='\0'; nb->len=0; }
                nb->style = b->style;
                nb->align = b->align;
                nb->font_size = b->font_size;
                wp->block_count++;
                /* Переносим хвост */
                int tail = b->len - wp->cur_pos;
                if (tail > 0) {
                    wp_block_ensure(nb, tail + 1);
                    memcpy(nb->text, b->text + wp->cur_pos, tail);
                    nb->len = (uint32_t)tail;
                    nb->text[tail] = '\0';
                    b->len = (uint32_t)wp->cur_pos;
                    b->text[wp->cur_pos] = '\0';
                }
                wp->cur_block++;
                wp->cur_pos = 0;
                wp->modified = true;
                wp_recalc_pages(wp);
            }
            break;

        case '\b':
            if (wp->cur_pos > 0) {
                wp_block_delete(b, wp->cur_pos - 1);
                wp->cur_pos--;
                wp->modified = true;
            } else if (wp->cur_block > 0) {
                int prev_len = (int)wp->blocks[wp->cur_block - 1].len;
                wp_block_t* prev = &wp->blocks[wp->cur_block - 1];
                wp_block_ensure(prev, prev_len + b->len + 1);
                memcpy(prev->text + prev_len, b->text, b->len + 1);
                prev->len += b->len;
                if (b->text) kfree(b->text);
                for (int i = wp->cur_block; i < wp->block_count - 1; i++)
                    wp->blocks[i] = wp->blocks[i+1];
                wp->block_count--;
                wp->cur_block--;
                wp->cur_pos = prev_len;
                wp->modified = true;
                wp_recalc_pages(wp);
            }
            break;

        case KEY_DELETE:
            if (wp->cur_pos < (int)b->len) {
                wp_block_delete(b, wp->cur_pos);
                wp->modified = true;
            } else if (wp->cur_block < wp->block_count - 1) {
                wp_block_t* next = &wp->blocks[wp->cur_block + 1];
                wp_block_ensure(b, b->len + next->len + 1);
                memcpy(b->text + b->len, next->text, next->len + 1);
                b->len += next->len;
                if (next->text) kfree(next->text);
                for (int i = wp->cur_block + 1; i < wp->block_count - 1; i++)
                    wp->blocks[i] = wp->blocks[i+1];
                wp->block_count--;
                wp->modified = true;
                wp_recalc_pages(wp);
            }
            break;

        case '\t':
            for (int i = 0; i < 4; i++)
                wp_block_insert(b, wp->cur_pos++, ' ');
            wp->modified = true;
            break;

        case KEY_KP_NUMLOCK:
            break;

        default:
            if (c >= 32 && c < 127) {
                wp_block_insert(b, wp->cur_pos, (char)c);
                wp->cur_pos++;
                wp->modified = true;
            }
            break;
    }

    /* Скролл */
    if (wp->scroll_y < 0) wp->scroll_y = 0;
    if (wp->cur_block < wp->scroll_y) wp->scroll_y = wp->cur_block;
    if (wp->cur_block >= wp->scroll_y + wp->visible_lines)
        wp->scroll_y = wp->cur_block - wp->visible_lines + 1;
}

static void wp_handle_click(wp_state_t* wp, int32_t mx, int32_t my) {
    /* Тулбар */
    if (my < WP_TOOLBAR_H) {
        for (int i = 0; i < wp->btn_count; i++) {
            if (mx >= wp->btns[i].x && mx < wp->btns[i].x + wp->btns[i].w &&
                my >= wp->btns[i].y && my < wp->btns[i].y + wp->btns[i].h) {
                if (i < 8) {
                    /* Форматирование */
                    if (wp->cur_block >= 0 && wp->cur_block < wp->block_count) {
                        wp_block_t* b = &wp->blocks[wp->cur_block];
                        int flag = 0;
                        if (i == 0) flag = STYLE_BOLD;
                        else if (i == 1) flag = STYLE_ITALIC;
                        else if (i == 2) flag = STYLE_UNDERL;
                        else if (i == 3) flag = STYLE_HEADER;
                        else if (i == 4) flag = STYLE_BIG;
                        else if (i == 5) { b->align = ALIGN_LEFT; wp->modified=true; return; }
                        else if (i == 6) { b->align = ALIGN_CENTER; wp->modified=true; return; }
                        else if (i == 7) { b->align = ALIGN_RIGHT; wp->modified=true; return; }
                        if (flag) { b->style ^= flag; wp->modified = true; }
                    }
                } else if (i == 8) {
                    wp_save_file(wp);
                } else if (i == 9) {
                    /* Open — пока заглушка */
                } else if (i == 10) {
                    /* New */
                    for (int j = 0; j < wp->block_count; j++)
                        if (wp->blocks[j].text) kfree(wp->blocks[j].text);
                    wp->block_count = 0;
                    wp_block_new(wp);
                    wp->cur_block = 0;
                    wp->cur_pos = 0;
                    wp->modified = false;
                    wp->scroll_y = 0;
                }
                return;
            }
        }
        return;
    }

    /* Контент — находим блок по позиции */
    int32_t content_y_start = WP_TOOLBAR_H + WP_RULER_H;
    if (my >= content_y_start) {
        int rel_y = my - content_y_start + wp->scroll_y * WP_LINE_H;
        int cur_y = 8;
        for (int bi = 0; bi < wp->block_count; bi++) {
            int lh = WP_LINE_H;
            if (wp->blocks[bi].style & STYLE_BIG) lh = 18;
            if (wp->blocks[bi].style & STYLE_HEADER) lh = 24;
            if (rel_y >= cur_y && rel_y < cur_y + lh) {
                /* Определяем позицию по X */
                int32_t page_w_center = wp->win_w / 2;
                int rel_x = mx - (page_w_center - wp->page_w / 2 + WP_MARGIN_LEFT);
                int char_idx = rel_x / FB_FONT_W;
                if (char_idx < 0) char_idx = 0;
                if (char_idx > (int)wp->blocks[bi].len) char_idx = (int)wp->blocks[bi].len;
                wp->cur_block = bi;
                wp->cur_pos = char_idx;
                return;
            }
            cur_y += lh;
        }
    }
}

/* =============================================================================
 * Точка входа
 * ============================================================================= */

gui_window_t* wordproc_open(const char* filepath) {
    if (!fb_ready()) return (gui_window_t*)0;

    int32_t sw = (int32_t)fb_width();
    int32_t sh = (int32_t)fb_height();

    gui_window_t* win = gui_create_window("MyWord", 10, 10, sw - 20, sh - 40);
    if (!win) return (gui_window_t*)0;

    wp_state_t* wp = (wp_state_t*)kmalloc(sizeof(wp_state_t));
    if (!wp) { gui_close_window(win); return (gui_window_t*)0; }
    memset(wp, 0, sizeof(*wp));

    wp->win_x = 0; wp->win_y = 0;
    wp->win_w = sw - 20; wp->win_h = sh - 40;
    wp->content_h = wp->win_h - WP_TOOLBAR_H - WP_RULER_H - WP_STATUSBAR_H;
    wp->page_w = 595;  /* A4 в пикселях (~210mm) */
    wp->page_h = 842;  /* A4 высота */
    wp->lines_per_page = (wp->page_h - 16) / WP_LINE_H;
    wp->visible_lines = wp->content_h / WP_LINE_H;
    wp->cur_block = 0;
    wp->cur_pos = 0;
    wp->cur_page = 0;
    wp->default_style = STYLE_NORMAL;
    wp->default_align = ALIGN_LEFT;
    wp->default_size = 12;
    wp->modified = false;
    strcpy(wp->doc_title, "Document1");

    if (filepath && filepath[0]) {
        strncpy(wp->filename, filepath, 255);
        /* Извлекаем имя файла для заголовка */
        const char* fn = filepath;
        for (const char* p = filepath; *p; p++) if (*p == '/' || *p == '\\') fn = p + 1;
        strncpy(wp->doc_title, fn, 127);
        wp_load_file(wp, filepath);
    } else {
        wp_block_new(wp);
        strncpy(wp->filename, "/tmp/document.txt", 255);
    }

    wp_recalc_pages(wp);

    /* Колбэки */
    /* Используем user_data для хранения состояния */
    win->user_data = wp;

    /* Нам нужен свой главный цикл, т.к. GUI on_key работает через колбэки */
    wp->running = true;
    while (wp->running) {
        wp_draw_toolbar(wp);
        wp_draw_ruler(wp);
        wp_draw_content(wp);
        wp_draw_statusbar(wp);
        fb_swap();

        if (keyboard_has_input()) {
            int c = keyboard_getkey();
            wp_handle_key(wp, c);
        }

        const mouse_state_t* ms = mouse_get_state();
        if (ms->left_click) {
            int32_t lx = ms->x - win->x;
            int32_t ly = ms->y - win->y;
            wp_handle_click(wp, lx, ly);
            mouse_consume_clicks();
        }

        __asm__ volatile ("hlt");
    }

    /* Очистка */
    for (int i = 0; i < wp->block_count; i++)
        if (wp->blocks[i].text) kfree(wp->blocks[i].text);
    kfree(wp);
    gui_close_window(win);
    return (gui_window_t*)0;
}

void wordproc_run(const char* filepath) {
    wordproc_open(filepath);
}
