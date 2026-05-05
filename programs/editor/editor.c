/* =============================================================================
 * programs/editor/editor.c
 * Текстовый редактор MyEdit
 *
 * Возможности:
 *   - Открытие и сохранение файлов (VFS)
 *   - Навигация стрелками, Home/End, PgUp/PgDn
 *   - Вставка/удаление символов и строк
 *   - Синтаксическая подсветка (C/ASM ключевые слова)
 *   - Номера строк
 *   - Строка статуса (имя файла, позиция, режим)
 *
 * Реализация: gap buffer или простой массив строк.
 * Мы используем массив строк (lines[]) — проще для учебных целей.
 * ============================================================================= */

#include "editor.h"
#include "../../drivers/framebuffer/framebuffer.h"
#include "../../drivers/keyboard/keyboard.h"
#include "../../drivers/vga/vga_text.h"
#include "../../filesystem/vfs.h"
#include "../../memory/heap.h"
#include "../../lib/string.h"

/* ─── Константы ─── */
#define ED_MAX_LINES    2048
#define ED_LINE_MAX     512
#define ED_TAB_SIZE     4

/* ─── Цвета темы редактора ─── */
#define ED_BG           0x1E1E2Eu  /* тёмный фон (как VS Code Dark) */
#define ED_FG           0xD4D4D4u  /* обычный текст */
#define ED_LINE_NUM     0x858585u  /* номера строк */
#define ED_LINE_NUM_BG  0x252537u  /* фон номеров */
#define ED_CURSOR_BG    0x264F78u  /* выделение строки курсора */
#define ED_STATUS_BG    0x007ACCu  /* синяя строка статуса */
#define ED_STATUS_FG    0xFFFFFFu
#define ED_KW_COLOR     0x569CD6u  /* ключевые слова (синий) */
#define ED_STR_COLOR    0xCE9178u  /* строки (оранжевый) */
#define ED_CMT_COLOR    0x6A9955u  /* комментарии (зелёный) */
#define ED_NUM_COLOR    0xB5CEA8u  /* числа (светло-зелёный) */
#define ED_PP_COLOR     0xC586C0u  /* препроцессор (фиолетовый) */

/* ─── Строка редактора ─── */
typedef struct {
    char*    text;      /* содержимое строки (heap) */
    uint32_t len;       /* длина */
    uint32_t cap;       /* выделено байт */
} ed_line_t;

/* ─── Состояние редактора ─── */
typedef struct {
    ed_line_t  lines[ED_MAX_LINES];
    int32_t    line_count;

    int32_t    cur_line;   /* строка курсора (0-based) */
    int32_t    cur_col;    /* колонка курсора */
    int32_t    scroll_y;   /* первая видимая строка */
    int32_t    scroll_x;   /* первая видимая колонка */

    char       filename[256];
    bool       modified;
    bool       running;

    /* Размеры области редактирования */
    int32_t    view_x;     /* начало X в пикселях */
    int32_t    view_y;     /* начало Y в пикселях */
    int32_t    view_w;
    int32_t    view_h;
    int32_t    line_num_w; /* ширина колонки номеров строк */

    int32_t    rows;       /* видимых строк */
    int32_t    cols;       /* видимых символов */
} ed_state_t;

static ed_state_t ed;

/* ─── Ключевые слова C ─── */
static const char* c_keywords[] = {
    "if","else","for","while","do","switch","case","break","continue",
    "return","struct","typedef","enum","union","static","const","void",
    "int","char","uint8_t","uint16_t","uint32_t","uint64_t","int32_t",
    "bool","true","false","NULL","sizeof","extern","inline","volatile",
    "include","define","ifndef","ifdef","endif","pragma",
    (const char*)0
};

/* =============================================================================
 * Работа со строками редактора
 * ============================================================================= */

static void line_ensure_cap(ed_line_t* l, uint32_t needed) {
    if (needed <= l->cap) return;
    uint32_t new_cap = needed * 2;
    if (new_cap < 64) new_cap = 64;
    char* new_text = (char*)krealloc(l->text, new_cap);
    if (!new_text) return;
    l->text = new_text;
    l->cap  = new_cap;
}

static void line_init(ed_line_t* l) {
    l->cap  = 64;
    l->text = (char*)kmalloc(l->cap);
    l->len  = 0;
    if (l->text) l->text[0] = '\0';
}

static void line_free(ed_line_t* l) {
    if (l->text) kfree(l->text);
    l->text = (char*)0; l->len = 0; l->cap = 0;
}

static void line_insert_char(ed_line_t* l, int32_t pos, char c) {
    line_ensure_cap(l, l->len + 2);
    if (!l->text) return;
    /* Сдвигаем вправо */
    for (int32_t i = (int32_t)l->len; i >= pos; i--)
        l->text[i+1] = l->text[i];
    l->text[pos] = c;
    l->len++;
    l->text[l->len] = '\0';
}

static void line_delete_char(ed_line_t* l, int32_t pos) {
    if (pos < 0 || pos >= (int32_t)l->len) return;
    for (uint32_t i = (uint32_t)pos; i < l->len; i++)
        l->text[i] = l->text[i+1];
    l->len--;
}

/* Разбить строку на pos: текущая становится [0..pos), новая [pos..] */
static void line_split(int32_t line_idx, int32_t pos) {
    if (ed.line_count >= ED_MAX_LINES) return;

    /* Сдвигаем строки вниз */
    for (int32_t i = ed.line_count; i > line_idx + 1; i--)
        ed.lines[i] = ed.lines[i-1];

    ed_line_t* cur = &ed.lines[line_idx];
    ed_line_t* nxt = &ed.lines[line_idx + 1];
    line_init(nxt);

    /* Копируем хвост в новую строку */
    uint32_t tail_len = cur->len - (uint32_t)pos;
    line_ensure_cap(nxt, tail_len + 1);
    if (nxt->text && cur->text) {
        memcpy(nxt->text, cur->text + pos, tail_len);
        nxt->len = tail_len;
        nxt->text[tail_len] = '\0';
        cur->len = (uint32_t)pos;
        cur->text[pos] = '\0';
    }
    ed.line_count++;
}

/* Объединить строку line_idx со следующей */
static void line_join(int32_t line_idx) {
    if (line_idx >= ed.line_count - 1) return;
    ed_line_t* cur = &ed.lines[line_idx];
    ed_line_t* nxt = &ed.lines[line_idx + 1];

    uint32_t new_len = cur->len + nxt->len;
    line_ensure_cap(cur, new_len + 1);
    if (cur->text && nxt->text)
        memcpy(cur->text + cur->len, nxt->text, nxt->len + 1);
    cur->len = new_len;

    line_free(nxt);
    /* Сдвигаем строки вверх */
    for (int32_t i = line_idx + 1; i < ed.line_count - 1; i++)
        ed.lines[i] = ed.lines[i+1];
    ed.lines[ed.line_count-1].text = (char*)0;
    ed.line_count--;
}

/* =============================================================================
 * Подсветка синтаксиса
 * ============================================================================= */

typedef enum {
    TK_NORMAL, TK_KEYWORD, TK_STRING, TK_COMMENT, TK_NUMBER, TK_PREPROC
} token_type_t;

static token_type_t get_token_type(const char* line, int32_t col) {
    if (!line) return TK_NORMAL;

    /* Проверяем комментарий от начала строки */
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#') return TK_PREPROC;

    /* Ищем // комментарий */
    for (int32_t i = 0; i < col; i++) {
        if (line[i] == '/' && line[i+1] == '/')
            return TK_COMMENT; /* всё правее — комментарий */
    }
    if (line[col] == '/' && line[col+1] == '/') return TK_COMMENT;

    /* Строка */
    bool in_str = false;
    for (int32_t i = 0; i < col; i++) {
        if (line[i] == '"' && (i == 0 || line[i-1] != '\\'))
            in_str = !in_str;
    }
    if (in_str || line[col] == '"') return TK_STRING;

    /* Число */
    if (col > 0 && line[col] >= '0' && line[col] <= '9') {
        /* Проверяем что предшествует не буква */
        if (col == 0 ||
            (!(line[col-1] >= 'a' && line[col-1] <= 'z') &&
             !(line[col-1] >= 'A' && line[col-1] <= 'Z') &&
             line[col-1] != '_'))
            return TK_NUMBER;
    }

    /* Ключевое слово */
    if (col >= 0) {
        /* Находим начало слова */
        int32_t word_start = col;
        while (word_start > 0 && (
            (line[word_start-1] >= 'a' && line[word_start-1] <= 'z') ||
            (line[word_start-1] >= 'A' && line[word_start-1] <= 'Z') ||
            line[word_start-1] == '_'))
            word_start--;

        /* Длина слова */
        int32_t word_len = 0;
        while (line[word_start + word_len] && (
            (line[word_start+word_len] >= 'a' && line[word_start+word_len] <= 'z') ||
            (line[word_start+word_len] >= 'A' && line[word_start+word_len] <= 'Z') ||
            (line[word_start+word_len] >= '0' && line[word_start+word_len] <= '9') ||
            line[word_start+word_len] == '_'))
            word_len++;

        char word[64];
        if (word_len > 0 && word_len < 63) {
            memcpy(word, line + word_start, (size_t)word_len);
            word[word_len] = '\0';
            for (int ki = 0; c_keywords[ki]; ki++) {
                if (strcmp(word, c_keywords[ki]) == 0)
                    return TK_KEYWORD;
            }
        }
    }

    return TK_NORMAL;
}

static fb_color_t token_color(token_type_t t) {
    switch (t) {
        case TK_KEYWORD:  return ED_KW_COLOR;
        case TK_STRING:   return ED_STR_COLOR;
        case TK_COMMENT:  return ED_CMT_COLOR;
        case TK_NUMBER:   return ED_NUM_COLOR;
        case TK_PREPROC:  return ED_PP_COLOR;
        default:          return ED_FG;
    }
}

/* =============================================================================
 * Отрисовка
 * ============================================================================= */

static void ed_render(void) {
    if (!fb_ready()) return;

    /* Фон */
    fb_fill_rect(0, 0, (int32_t)fb_width(), (int32_t)fb_height(), ED_BG);

    /* Номера строк — фон */
    fb_fill_rect(0, 0, ed.line_num_w, ed.view_h, ED_LINE_NUM_BG);

    /* Строки текста */
    for (int32_t row = 0; row < ed.rows; row++) {
        int32_t line_idx = ed.scroll_y + row;
        if (line_idx >= ed.line_count) break;

        int32_t py = ed.view_y + row * FB_FONT_H;

        /* Подсветка текущей строки */
        if (line_idx == ed.cur_line) {
            fb_fill_rect(ed.line_num_w, py,
                         ed.view_w - ed.line_num_w, FB_FONT_H, ED_CURSOR_BG);
        }

        /* Номер строки */
        char lnum[8];
        int32_t n = line_idx + 1, li = 0;
        if (n == 0) { lnum[li++]='0'; }
        else { char tmp[8]; int ti=0;
               while(n>0){tmp[ti++]='0'+n%10;n/=10;}
               while(ti>0) lnum[li++]=tmp[--ti]; }
        lnum[li] = '\0';
        int32_t lnum_x = ed.line_num_w - (int32_t)(strlen(lnum)+1)*FB_FONT_W;
        fb_draw_string(lnum_x, py, lnum, ED_LINE_NUM, ED_LINE_NUM_BG, false);

        /* Текст строки с подсветкой */
        ed_line_t* l = &ed.lines[line_idx];
        if (!l->text) continue;

        int32_t px = ed.view_x;
        for (int32_t col = ed.scroll_x;
             col < (int32_t)l->len && px < (int32_t)fb_width();
             col++) {

            char c = l->text[col];
            token_type_t tt = get_token_type(l->text, col);
            fb_color_t   fg = token_color(tt);
            fb_color_t   bg = (line_idx == ed.cur_line) ? ED_CURSOR_BG : ED_BG;

            /* Курсор */
            if (line_idx == ed.cur_line && col == ed.cur_col) {
                fb_fill_rect(px, py, 2, FB_FONT_H, ED_STATUS_FG);
            }

            if (c == '\t') {
                int32_t spaces = ED_TAB_SIZE - (col - ed.scroll_x) % ED_TAB_SIZE;
                fb_fill_rect(px, py, spaces * FB_FONT_W, FB_FONT_H, bg);
                px += spaces * FB_FONT_W;
            } else {
                fb_draw_char(px, py, c, fg, bg, false);
                px += FB_FONT_W;
            }
        }

        /* Курсор в конце строки */
        if (line_idx == ed.cur_line && ed.cur_col >= (int32_t)l->len) {
            int32_t curs_px = ed.view_x +
                (int32_t)(ed.cur_col - ed.scroll_x) * FB_FONT_W;
            fb_fill_rect(curs_px, py, 2, FB_FONT_H, ED_STATUS_FG);
        }
    }

    /* Вертикальный разделитель номеров строк */
    fb_draw_vline(ed.line_num_w, 0, ed.view_h, 0x444466u);

    /* Строка статуса */
    int32_t stat_y = (int32_t)fb_height() - FB_FONT_H - 4;
    fb_fill_rect(0, stat_y, (int32_t)fb_width(), FB_FONT_H + 4, ED_STATUS_BG);

    char status[128];
    {
        /* Собираем строку статуса */
        int si = 0;
        const char* fname = ed.filename[0] ? ed.filename : "[No Name]";
        /* имя файла */
        for (int i = 0; fname[i] && si < 40; i++) status[si++] = fname[i];
        if (ed.modified) { status[si++]=' '; status[si++]='['; 
                           status[si++]='+'; status[si++]=']'; }
        /* разделитель */
        while (si < 60) status[si++] = ' ';
        /* позиция */
        const char* pos_label = "Ln:";
        for (int i = 0; pos_label[i]; i++) status[si++] = pos_label[i];
        int ln = ed.cur_line+1;
        char tmp[8]; int ti=0;
        while(ln>0){tmp[ti++]='0'+ln%10;ln/=10;}
        while(ti>0) status[si++]=tmp[--ti];
        status[si++] = ' '; status[si++]='C'; status[si++]='o'; 
        status[si++]='l'; status[si++]=':';
        int cn = ed.cur_col+1; ti=0;
        while(cn>0){tmp[ti++]='0'+cn%10;cn/=10;}
        while(ti>0) status[si++]=tmp[--ti];
        status[si] = '\0';
    }
    fb_draw_string(4, stat_y + 2, status, ED_STATUS_FG, ED_STATUS_BG, true);

    /* Подсказка горячих клавиш */
    fb_draw_string((int32_t)fb_width() - 260, stat_y + 2,
        "Ctrl+S Save  Ctrl+Q Quit",
        0xCCDDEEu, ED_STATUS_BG, true);

    fb_swap();
}

/* =============================================================================
 * Файловые операции
 * ============================================================================= */

static void ed_load_file(const char* path) {
    int32_t fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) {
        /* Новый файл */
        line_init(&ed.lines[0]);
        ed.line_count = 1;
        strncpy(ed.filename, path, 255);
        return;
    }

    /* Освобождаем старые строки */
    for (int32_t i = 0; i < ed.line_count; i++) line_free(&ed.lines[i]);
    ed.line_count = 0;

    strncpy(ed.filename, path, 255);

    uint8_t buf[1024];
    int32_t n;
    int32_t cur_line = 0;
    line_init(&ed.lines[cur_line]);

    while ((n = vfs_read(fd, buf, sizeof(buf))) > 0) {
        for (int32_t i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n') {
                cur_line++;
                if (cur_line >= ED_MAX_LINES) break;
                line_init(&ed.lines[cur_line]);
            } else if (c != '\r') {
                line_insert_char(&ed.lines[cur_line],
                                 (int32_t)ed.lines[cur_line].len, c);
            }
        }
    }
    ed.line_count = cur_line + 1;
    vfs_close(fd);
    ed.modified = false;
}

static void ed_save_file(void) {
    if (!ed.filename[0]) return;

    int32_t fd = vfs_open(ed.filename,
                          VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) return;

    for (int32_t i = 0; i < ed.line_count; i++) {
        if (ed.lines[i].text && ed.lines[i].len > 0)
            vfs_write(fd, ed.lines[i].text, ed.lines[i].len);
        vfs_write(fd, "\n", 1);
    }
    vfs_close(fd);
    ed.modified = false;
}

/* =============================================================================
 * Навигация
 * ============================================================================= */

static void ed_clamp_cursor(void) {
    if (ed.cur_line < 0) ed.cur_line = 0;
    if (ed.cur_line >= ed.line_count) ed.cur_line = ed.line_count - 1;
    if (ed.cur_col < 0) ed.cur_col = 0;
    int32_t line_len = ed.lines[ed.cur_line].len;
    if (ed.cur_col > line_len) ed.cur_col = line_len;

    /* Прокрутка */
    if (ed.cur_line < ed.scroll_y) ed.scroll_y = ed.cur_line;
    if (ed.cur_line >= ed.scroll_y + ed.rows) ed.scroll_y = ed.cur_line - ed.rows + 1;
    if (ed.cur_col < ed.scroll_x) ed.scroll_x = ed.cur_col;
    if (ed.cur_col >= ed.scroll_x + ed.cols) ed.scroll_x = ed.cur_col - ed.cols + 1;
    if (ed.scroll_y < 0) ed.scroll_y = 0;
    if (ed.scroll_x < 0) ed.scroll_x = 0;
}

/* =============================================================================
 * Обработка ввода
 * ============================================================================= */

static void ed_handle_key(int c) {
    /* Ctrl коды */
    if (c == 19) { ed_save_file(); return; } /* Ctrl+S */
    if (c == 17) { ed.running = false; return; } /* Ctrl+Q */

    switch (c) {
        case KEY_ARROW_UP:
            ed.cur_line--;
            ed_clamp_cursor();
            break;
        case KEY_ARROW_DOWN:
            ed.cur_line++;
            ed_clamp_cursor();
            break;
        case KEY_ARROW_LEFT:
            if (ed.cur_col > 0) ed.cur_col--;
            else if (ed.cur_line > 0) {
                ed.cur_line--;
                ed.cur_col = (int32_t)ed.lines[ed.cur_line].len;
            }
            ed_clamp_cursor();
            break;
        case KEY_ARROW_RIGHT:
            if (ed.cur_col < (int32_t)ed.lines[ed.cur_line].len)
                ed.cur_col++;
            else if (ed.cur_line < ed.line_count - 1) {
                ed.cur_line++;
                ed.cur_col = 0;
            }
            ed_clamp_cursor();
            break;
        case '\n': case '\r':
            line_split(ed.cur_line, ed.cur_col);
            ed.cur_line++;
            ed.cur_col = 0;
            ed.modified = true;
            ed_clamp_cursor();
            break;
        case '\b':
            if (ed.cur_col > 0) {
                line_delete_char(&ed.lines[ed.cur_line], ed.cur_col - 1);
                ed.cur_col--;
                ed.modified = true;
            } else if (ed.cur_line > 0) {
                int32_t prev_len = (int32_t)ed.lines[ed.cur_line-1].len;
                line_join(ed.cur_line - 1);
                ed.cur_line--;
                ed.cur_col = prev_len;
                ed.modified = true;
            }
            ed_clamp_cursor();
            break;
        case KEY_DELETE:
            if (ed.cur_col < (int32_t)ed.lines[ed.cur_line].len) {
                line_delete_char(&ed.lines[ed.cur_line], ed.cur_col);
                ed.modified = true;
            } else if (ed.cur_line < ed.line_count - 1) {
                line_join(ed.cur_line);
                ed.modified = true;
            }
            break;
        case '\t':
            for (int i = 0; i < ED_TAB_SIZE; i++) {
                line_insert_char(&ed.lines[ed.cur_line], ed.cur_col, ' ');
                ed.cur_col++;
            }
            ed.modified = true;
            ed_clamp_cursor();
            break;
        default:
            /* Печатаемые символы */
            if (c >= 32 && c < 127) {
                line_insert_char(&ed.lines[ed.cur_line], ed.cur_col, c);
                ed.cur_col++;
                ed.modified = true;
                ed_clamp_cursor();
            }
            break;
    }
}

/* =============================================================================
 * editor_run — точка входа
 * ============================================================================= */
void editor_run(const char* filepath) {
    /* Инициализация */
    memset(&ed, 0, sizeof(ed));
    ed.running = true;

    /* Разметка области */
    ed.line_num_w = 5 * FB_FONT_W;  /* "9999 " */
    ed.view_x = ed.line_num_w;
    ed.view_y = 0;
    ed.view_w = (int32_t)fb_width();
    ed.view_h = (int32_t)fb_height() - FB_FONT_H - 4;
    ed.rows   = ed.view_h / FB_FONT_H;
    ed.cols   = (ed.view_w - ed.line_num_w) / FB_FONT_W;

    /* Загружаем файл */
    if (filepath && filepath[0]) {
        ed_load_file(filepath);
    } else {
        line_init(&ed.lines[0]);
        ed.line_count = 1;
        strncpy(ed.filename, "/tmp/untitled.txt", 255);
    }

    /* Главный цикл */
    while (ed.running) {
        ed_render();

        /* Ждём ввода */
        if (keyboard_has_input()) {
            int c = keyboard_getkey();
            ed_handle_key(c);
        }
        __asm__ volatile ("hlt");
    }

    /* Освобождаем память */
    for (int32_t i = 0; i < ed.line_count; i++)
        line_free(&ed.lines[i]);
}
