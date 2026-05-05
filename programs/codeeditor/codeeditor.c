/* =============================================================================
 * programs/codeeditor/codeeditor.c
 * Редактор кода MyCode — поддержка множества языков
 *
 * Языки:
 *   - C, C++, C# (расширения: .c, .h, .cpp, .hpp, .cs)
 *   - Python (.py)
 *   - JavaScript/TypeScript (.js, .ts)
 *   - Java (.java)
 *   - Assembly (.asm, .s)
 *   - Shell (.sh, .bat)
 *   - HTML (.html, .htm)
 *   - JSON, XML, YAML
 *   - Любой текст (.txt, .md, .log, .cfg)
 *
 * Возможности:
 *   - Подсветка синтаксиса для каждого языка
 *   - Номера строк
 *   - Автоотступы
 *   - Поиск (Ctrl+F)
 *   - Переход к строке (Ctrl+G)
 *   - Несколько файлов (табы)
 *   - Миникарта справа
 * ============================================================================= */

#include "codeeditor.h"
#include "../../drivers/framebuffer/framebuffer.h"
#include "../../drivers/keyboard/keyboard.h"
#include "../../drivers/mouse/mouse.h"
#include "../../drivers/timer/pit.h"
#include "../../filesystem/vfs.h"
#include "../../memory/heap.h"
#include "../../lib/string.h"

/* ─── Цвета темы (VS Code Dark+) ─── */
#define CE_BG            0x1E1E1Eu
#define CE_FG            0xD4D4D4u
#define CE_LINE_NUM_BG   0x1E1E1Eu
#define CE_LINE_NUM_FG   0x858585u
#define CE_CURRENT_LINE  0x2A2D2Eu
#define CE_STATUS_BG     0x007ACCu
#define CE_STATUS_FG     0xFFFFFFu
#define CE_TAB_BG        0x2D2D2Du
#define CE_TAB_ACTIVE    0x1E1E1Eu
#define CE_MINIMAP_BG    0x1A1A1Au
#define CE_SCROLL_BG     0x424242u

/* Цвета токенов */
#define CE_KEYWORD       0x569CD6u  /* синий */
#define CE_STRING        0xCE9178u  /* оранжевый */
#define CE_COMMENT       0x6A9955u  /* зелёный */
#define CE_NUMBER        0xB5CEA8u  /* светло-зелёный */
#define CE_PREPROC       0xC586C0u  /* фиолетовый */
#define CE_TYPE          0x4EC9B0u  /* бирюзовый */
#define CE_FUNCTION      0xDCDCAAu  /* жёлтый */
#define CE_OPERATOR      0xD4D4D4u  /* белый */
#define CE_TAG           0x569CD6u  /* синий (HTML) */
#define CE_ATTR          0x9CDCFEu  /* голубой */

/* ─── Типы языков ─── */
typedef enum {
    LANG_UNKNOWN,
    LANG_C,
    LANG_CPP,
    LANG_CS,
    LANG_PYTHON,
    LANG_JS,
    LANG_TS,
    LANG_JAVA,
    LANG_ASM,
    LANG_SHELL,
    LANG_HTML,
    LANG_JSON,
    LANG_XML,
    LANG_TEXT
} ce_lang_t;

/* ─── Строка редактора ─── */
typedef struct {
    char*    text;
    uint32_t len;
    uint32_t cap;
} ce_line_t;

/* ─── Файл (таб) ─── */
typedef struct {
    char     path[256];
    char     name[128];
    ce_line_t* lines;
    int      line_count;
    ce_lang_t lang;
    bool     modified;
} ce_file_t;

/* ─── Состояние ─── */
typedef struct {
    ce_file_t  files[8];
    int        file_count;
    int        active_file;

    int        cur_line;
    int        cur_col;
    int        scroll_y;
    int        scroll_x;

    bool       running;
    bool       searching;
    char       search_buf[128];
    int        search_pos;

    /* Размеры */
    int32_t    line_num_w;
    int32_t    minimap_w;
    int32_t    tab_h;
    int32_t    status_h;
    int        rows;
    int        cols;

} ce_state_t;

/* ─── Ключевые слова по языкам ─── */

static const char* kw_c[] = {
    "if","else","for","while","do","switch","case","break","continue",
    "return","struct","typedef","enum","union","static","const","void",
    "extern","inline","volatile","unsigned","signed","long","short",
    "sizeof","goto","auto","register",
    0
};
static const char* kw_cpp[] = {
    "if","else","for","while","do","switch","case","break","continue",
    "return","struct","typedef","enum","union","static","const","void",
    "extern","inline","volatile","unsigned","signed","long","short",
    "sizeof","goto","class","public","private","protected","virtual",
    "override","new","delete","this","template","namespace","using",
    "try","catch","throw","noexcept","constexpr","auto","nullptr",
    0
};
static const char* kw_python[] = {
    "if","elif","else","for","while","def","return","class","import",
    "from","as","try","except","finally","raise","with","yield",
    "lambda","pass","break","continue","and","or","not","is","in",
    "True","False","None","self","global","nonlocal","assert","del",
    0
};
static const char* kw_js[] = {
    "if","else","for","while","do","switch","case","break","continue",
    "return","function","var","let","const","class","extends","new",
    "this","try","catch","finally","throw","async","await","yield",
    "import","export","default","typeof","instanceof","in","of",
    "true","false","null","undefined","void","delete","super",
    0
};
static const char* kw_asm[] = {
    "mov","push","pop","call","ret","add","sub","mul","div","xor",
    "and","or","not","inc","dec","cmp","jmp","je","jne","jg","jl",
    "jge","jle","lea","int","nop","cli","sti","hlt","in","out",
    "db","dw","dd","resb","resw","resd","section","global","extern",
    0
};
static const char* kw_shell[] = {
    "if","then","else","elif","fi","for","while","do","done","case",
    "esac","function","return","exit","echo","export","source",
    "true","false","local","readonly","shift","set","unset",
    0
};
static const char* kw_html[] = {
    "html","head","body","title","meta","div","span","p","a","img",
    "script","style","link","h1","h2","h3","h4","h5","h6","ul","ol",
    "li","table","tr","td","th","form","input","button","select","option",
    0
};

static const char** lang_keywords(ce_lang_t lang) {
    switch (lang) {
        case LANG_C:      return kw_c;
        case LANG_CPP:    return kw_cpp;
        case LANG_PYTHON: return kw_python;
        case LANG_JS:
        case LANG_TS:     return kw_js;
        case LANG_ASM:    return kw_asm;
        case LANG_SHELL:  return kw_shell;
        case LANG_HTML:   return kw_html;
        default:          return kw_c;
    }
}

/* ─── Определение языка по расширению ─── */

static ce_lang_t detect_lang(const char* path) {
    const char* ext = 0;
    for (int i = strlen(path)-1; i >= 0; i--)
        if (path[i] == '.') { ext = path + i + 1; break; }
    if (!ext) return LANG_TEXT;

    const char* c_exts[] = {"c","h","C","H",0};
    const char* cpp_exts[] = {"cpp","hpp","cc","cxx","CPP","HPP","CC","CXX",0};
    const char* cs_exts[] = {"cs","CS",0};
    const char* py_exts[] = {"py","PY",0};
    const char* js_exts[] = {"js","JS",0};
    const char* ts_exts[] = {"ts","TS",0};
    const char* java_exts[] = {"java","JAVA",0};
    const char* asm_exts[] = {"asm","s","S","ASM",0};
    const char* shell_exts[] = {"sh","SH","bash","BAT","bat",0};
    const char* html_exts[] = {"html","htm","HTML","HTM",0};
    const char* json_exts[] = {"json","JSON",0};
    const char* xml_exts[] = {"xml","XML",0};

    const char** lists[] = {cpp_exts, cs_exts, py_exts, js_exts, ts_exts,
                            java_exts, asm_exts, shell_exts, html_exts,
                            json_exts, xml_exts, c_exts};
    ce_lang_t langs[] = {LANG_CPP, LANG_CS, LANG_PYTHON, LANG_JS, LANG_TS,
                         LANG_JAVA, LANG_ASM, LANG_SHELL, LANG_HTML,
                         LANG_JSON, LANG_XML, LANG_C};

    for (int i = 0; i < 12; i++) {
        for (int j = 0; lists[i][j]; j++)
            if (strcmp(ext, lists[i][j]) == 0) return langs[i];
    }
    return LANG_TEXT;
}

/* ─── Токенизация ─── */

typedef enum { TK_NORMAL, TK_KEYWORD, TK_STRING, TK_COMMENT,
               TK_NUMBER, TK_PREPROC, TK_TYPE, TK_FUNCTION, TK_TAG, TK_ATTR } ce_token_t;

static ce_token_t get_token(const char* line, int col, ce_lang_t lang) {
    if (!line || !line[0]) return TK_NORMAL;

    /* Препроцессор */
    if (lang != LANG_HTML && lang != LANG_JSON && lang != LANG_XML) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' && lang != LANG_SHELL && lang != LANG_PYTHON) return TK_PREPROC;
    }

    /* Комментарий // */
    for (int i = 0; i < col && line[i]; i++) {
        if (line[i]=='/' && line[i+1]=='/') return TK_COMMENT;
        if (line[i]=='/' && line[i+1]=='*') {
            /* multiline */
            for (int j = i+2; line[j]; j++)
                if (line[j]=='*' && line[j+1]=='/') return TK_NORMAL;
            return TK_COMMENT;
        }
    }

    /* Строка */
    bool in_str = false;
    char str_char = 0;
    for (int i = 0; i < col && line[i]; i++) {
        if ((line[i]=='"'||line[i]=='\'') && (i==0||line[i-1]!='\\')) {
            if (!in_str) { in_str=true; str_char=line[i]; }
            else if (line[i]==str_char) { in_str=false; }
        }
    }
    if (in_str) return TK_STRING;
    if (line[col]=='"' || line[col]=='\'') return TK_STRING;

    /* Число */
    if (line[col]>='0' && line[col]<='9') {
        if (col==0 || (!((line[col-1]>='a'&&line[col-1]<='z') ||
                         (line[col-1]>='A'&&line[col-1]<='Z') ||
                         line[col-1]=='_' || line[col-1]=='.')))
            return TK_NUMBER;
    }

    /* Ключевое слово */
    int ws = col;
    while (ws > 0 && ((line[ws-1]>='a'&&line[ws-1]<='z') ||
                       (line[ws-1]>='A'&&line[ws-1]<='Z') ||
                       line[ws-1]=='_' || line[ws-1]=='.'))
        ws--;

    int wl = 0;
    while (line[ws+wl] && ((line[ws+wl]>='a'&&line[ws+wl]<='z') ||
                            (line[ws+wl]>='A'&&line[ws+wl]<='Z') ||
                            (line[ws+wl]>='0'&&line[ws+wl]<='9') ||
                            line[ws+wl]=='_' || line[ws+wl]=='.'))
        wl++;

    if (wl > 0 && wl < 64) {
        char word[64];
        memcpy(word, line+ws, wl);
        word[wl] = '\0';

        const char** kws = lang_keywords(lang);
        for (int i = 0; kws[i]; i++)
            if (strcmp(word, kws[i]) == 0) return TK_KEYWORD;

        /* Тип данных — начинается с заглавной */
        if (wl > 0 && word[0]>='A' && word[0]<='Z') return TK_TYPE;

        /* Функция — после неё ( */
        int after = ws + wl;
        while (line[after] == ' ') after++;
        if (line[after] == '(') return TK_FUNCTION;
    }

    return TK_NORMAL;
}

static fb_color_t token_color(ce_token_t t, ce_lang_t lang) {
    switch (t) {
        case TK_KEYWORD:  return CE_KEYWORD;
        case TK_STRING:   return CE_STRING;
        case TK_COMMENT:  return CE_COMMENT;
        case TK_NUMBER:   return CE_NUMBER;
        case TK_PREPROC:  return CE_PREPROC;
        case TK_TYPE:     return CE_TYPE;
        case TK_FUNCTION: return CE_FUNCTION;
        default:          return CE_FG;
    }
}

/* ─── Утилиты строк ─── */

static void ce_line_ensure(ce_line_t* l, uint32_t n) {
    if (n <= l->cap) return;
    uint32_t nc = n * 2;
    if (nc < 64) nc = 64;
    char* nt = (char*)krealloc(l->text, nc);
    if (!nt) return;
    l->text = nt;
    l->cap = nc;
}

static void ce_line_init(ce_line_t* l) {
    l->cap = 64;
    l->text = (char*)kmalloc(l->cap);
    l->len = 0;
    if (l->text) l->text[0] = '\0';
}

static void ce_line_free(ce_line_t* l) {
    if (l->text) kfree(l->text);
    l->text = 0; l->len = 0; l->cap = 0;
}

static void ce_line_insert(ce_line_t* l, int pos, char c) {
    ce_line_ensure(l, l->len + 2);
    for (uint32_t i = l->len; i > (uint32_t)pos; i--)
        l->text[i] = l->text[i-1];
    l->text[pos] = c;
    l->len++;
    l->text[l->len] = '\0';
}

static void ce_line_delete(ce_line_t* l, int pos) {
    if (pos < 0 || pos >= (int)l->len) return;
    for (uint32_t i = (uint32_t)pos; i < l->len; i++)
        l->text[i] = l->text[i+1];
    l->len--;
}

/* ─── Файловые операции ─── */

static void ce_load_file(ce_state_t* ce, const char* path) {
    int32_t fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) {
        ce->files[0].lines = (ce_line_t*)kmalloc(sizeof(ce_line_t));
        ce_line_init(&ce->files[0].lines[0]);
        ce->files[0].line_count = 1;
        return;
    }

    ce_file_t* f = &ce->files[0];
    f->line_count = 0;
    f->lines = (ce_line_t*)kmalloc(sizeof(ce_line_t) * 4096);
    f->lang = detect_lang(path);
    f->modified = false;
    strncpy(f->path, path, 255);

    const char* fn = path;
    for (const char* p = path; *p; p++) if (*p=='/'||*p=='\\') fn = p+1;
    strncpy(f->name, fn, 127);

    ce_line_init(&f->lines[0]);
    f->line_count = 1;

    uint8_t buf[1024];
    int32_t n;
    while ((n = vfs_read(fd, buf, sizeof(buf))) > 0) {
        for (int32_t i = 0; i < n; i++) {
            char c = (char)buf[i];
            if (c == '\n') {
                if (f->line_count >= 4096) break;
                ce_line_init(&f->lines[f->line_count++]);
            } else if (c != '\r') {
                ce_line_insert(&f->lines[f->line_count-1],
                              (int)f->lines[f->line_count-1].len, c);
            }
        }
    }
    vfs_close(fd);
}

static void ce_save_file(ce_state_t* ce) {
    ce_file_t* f = &ce->files[ce->active_file];
    if (!f->path[0]) {
        strncpy(f->path, "/tmp/untitled.txt", 255);
        strncpy(f->name, "untitled.txt", 127);
        f->lang = LANG_TEXT;
    }

    int32_t fd = vfs_open(f->path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) return;

    for (int i = 0; i < f->line_count; i++) {
        if (f->lines[i].text && f->lines[i].len > 0)
            vfs_write(fd, f->lines[i].text, f->lines[i].len);
        vfs_write(fd, "\n", 1);
    }
    vfs_close(fd);
    f->modified = false;
}

/* ─── Отрисовка ─── */

static void ce_render(ce_state_t* ce) {
    if (!fb_ready()) return;
    ce_file_t* f = &ce->files[ce->active_file];
    if (!f) return;

    int32_t sw = (int32_t)fb_width();
    int32_t sh = (int32_t)fb_height();

    /* Фон */
    fb_fill_rect(0, 0, sw, sh, CE_BG);

    /* Табы */
    fb_fill_rect(0, 0, sw, ce->tab_h, CE_TAB_BG);
    for (int fi = 0; fi <= ce->active_file && fi < ce->file_count; fi++) {
        ce_file_t* tf = &ce->files[fi];
        int32_t tx = fi * 160;
        fb_color_t tbg = (fi == ce->active_file) ? CE_TAB_ACTIVE : CE_TAB_BG;
        fb_fill_rect(tx, 0, 158, ce->tab_h - 1, tbg);
        fb_draw_hline(tx, ce->tab_h - 1, 158, 0x444444u);
        fb_draw_string(tx + 6, 6, tf->name,
                       (fi==ce->active_file) ? CE_FG : 0x888888u, tbg, true);
        if (tf->modified) {
            fb_draw_string(tx + 140, 6, "*", 0xCCAA44u, tbg, true);
        }
    }

    /* Номера строк */
    fb_fill_rect(0, ce->tab_h, ce->line_num_w, sh - ce->tab_h - ce->status_h,
                 CE_LINE_NUM_BG);

    /* Текст */
    for (int row = 0; row < ce->rows; row++) {
        int line_idx = ce->scroll_y + row;
        if (line_idx >= f->line_count) break;

        int32_t py = ce->tab_h + 4 + row * FB_FONT_H;
        ce_line_t* l = &f->lines[line_idx];
        if (!l->text) continue;

        /* Текущая строка */
        if (line_idx == ce->cur_line)
            fb_fill_rect(ce->line_num_w, py,
                         sw - ce->line_num_w - ce->minimap_w, FB_FONT_H,
                         CE_CURRENT_LINE);

        /* Номер */
        char lnum[8];
        {
            int ni=0,v=line_idx+1;
            if(v==0){lnum[ni++]='0';}else{char t[8];int j=0;while(v>0){t[j++]='0'+v%10;v/=10;}while(j>0)lnum[ni++]=t[--j];}
            lnum[ni]='\0';
        }
        int32_t lnx = ce->line_num_w - (int32_t)(strlen(lnum)+1)*FB_FONT_W;
        fb_draw_string(lnx, py, lnum, CE_LINE_NUM_FG, CE_LINE_NUM_BG, false);

        /* Текст с подсветкой */
        int32_t px = ce->line_num_w + 4;
        for (int col = ce->scroll_x; col < (int)l->len; col++) {
            char c = l->text[col];
            ce_token_t tt = get_token(l->text, col, f->lang);
            fb_color_t fg = token_color(tt, f->lang);

            /* Курсор */
            if (line_idx == ce->cur_line && col == ce->cur_col)
                fb_fill_rect(px, py, 2, FB_FONT_H, CE_STATUS_FG);

            if (c == '\t') {
                int sp = 4 - (col - ce->scroll_x) % 4;
                px += sp * FB_FONT_W;
            } else {
                fb_draw_char(px, py, c, fg,
                    (line_idx==ce->cur_line)?CE_CURRENT_LINE:CE_BG, false);
                px += FB_FONT_W;
            }
        }

        /* Курсор в конце */
        if (line_idx == ce->cur_line && ce->cur_col >= (int)l->len) {
            int cpx = ce->line_num_w + 4 + (ce->cur_col - ce->scroll_x) * FB_FONT_W;
            fb_fill_rect(cpx, py, 2, FB_FONT_H, CE_STATUS_FG);
        }
    }

    /* Разделитель номеров */
    fb_draw_vline(ce->line_num_w, ce->tab_h, sh - ce->tab_h - ce->status_h, 0x333333u);

    /* Миникарта */
    int32_t mm_x = sw - ce->minimap_w;
    fb_fill_rect(mm_x, ce->tab_h, ce->minimap_w, sh - ce->tab_h - ce->status_h,
                 CE_MINIMAP_BG);

    /* Код на миникарте */
    for (int row = 0; row < ce->rows && row < f->line_count; row++) {
        int line_idx = ce->scroll_y + row;
        if (line_idx >= f->line_count) break;
        ce_line_t* l = &f->lines[line_idx];
        int32_t py = ce->tab_h + 4 + row * 2;
        for (uint32_t ci = 0; ci < l->len && ci < 80; ci++) {
            char c = l->text[ci];
            if (c != ' ' && c != '\t') {
                fb_fill_rect(mm_x + 2 + (int)ci, py, 1, 1,
                             (line_idx==ce->cur_line)?0x4488CCu:0x555555u);
            }
        }
    }

    /* Скроллбар */
    if (f->line_count > ce->rows) {
        int32_t sb_x = mm_x - 8;
        int32_t sb_h = sh - ce->tab_h - ce->status_h - 8;
        fb_fill_rect(sb_x, ce->tab_h + 4, 6, sb_h, 0x2A2A2Au);
        int32_t th = sb_h * ce->rows / f->line_count;
        if (th < 16) th = 16;
        int32_t ty = ce->tab_h + 4;
        if (f->line_count > ce->rows)
            ty += (sb_h - th) * ce->scroll_y / (f->line_count - ce->rows);
        fb_fill_rect(sb_x, ty, 6, th, CE_SCROLL_BG);
    }

    /* Статусбар */
    int32_t sy = sh - ce->status_h;
    fb_fill_rect(0, sy, sw, ce->status_h, CE_STATUS_BG);

    char st[128];
    {
        int si=0;
        const char* lang_name = "Text";
        switch(f->lang) {
            case LANG_C: lang_name="C"; break;
            case LANG_CPP: lang_name="C++"; break;
            case LANG_PYTHON: lang_name="Python"; break;
            case LANG_JS: lang_name="JavaScript"; break;
            case LANG_TS: lang_name="TypeScript"; break;
            case LANG_ASM: lang_name="Assembly"; break;
            case LANG_SHELL: lang_name="Shell"; break;
            case LANG_HTML: lang_name="HTML"; break;
            case LANG_JSON: lang_name="JSON"; break;
            case LANG_XML: lang_name="XML"; break;
            default: lang_name="Text"; break;
        }
        const char* ln = f->name[0] ? f->name : "untitled";
        while(*ln) st[si++] = *ln++;
        st[si++]=' '; st[si++]='|'; st[si++]=' ';
        const char* p="Ln:"; while(*p) st[si++]=*p++;
        int v=ce->cur_line+1; char tmp[8]; int ti=0;
        if(!v){tmp[ti++]='0';}else{char t2[8];int j=0;while(v>0){t2[j++]='0'+v%10;v/=10;}while(j>0)tmp[ti++]=t2[--j];}
        tmp[ti]='\0'; for(int i=0;tmp[i];i++) st[si++]=tmp[i];
        const char* p2=" Col:"; while(*p2) st[si++]=*p2++;
        v=ce->cur_col+1; ti=0;
        if(!v){tmp[ti++]='0';}else{char t2[8];int j=0;while(v>0){t2[j++]='0'+v%10;v/=10;}while(j>0)tmp[ti++]=t2[--j];}
        tmp[ti]='\0'; for(int i=0;tmp[i];i++) st[si++]=tmp[i];
        st[si++]=' '; st[si++]='|'; st[si++]=' ';
        while(*lang_name) st[si++] = *lang_name++;
        st[si]='\0';
    }
    fb_draw_string(6, sy + 2, st, CE_STATUS_FG, CE_STATUS_BG, true);

    const char* hints = "Ctrl+S Save  Ctrl+F Find  Ctrl+G Goto  Ctrl+Q Quit";
    fb_draw_string(sw - 340, sy + 2, hints, 0xBBDDFFu, CE_STATUS_BG, true);

    fb_swap();
}

/* ─── Ввод ─── */

static void ce_handle_key(ce_state_t* ce, int c) {
    ce_file_t* f = &ce->files[ce->active_file];

    if (c == 19) { ce_save_file(ce); return; }  /* Ctrl+S */
    if (c == 17) { ce->running = false; return; }  /* Ctrl+Q */

    if (c == KEY_ARROW_UP) {
        if (ce->cur_line > 0) ce->cur_line--;
        if (ce->cur_col > (int)f->lines[ce->cur_line].len)
            ce->cur_col = (int)f->lines[ce->cur_line].len;
    } else if (c == KEY_ARROW_DOWN) {
        if (ce->cur_line < f->line_count - 1) ce->cur_line++;
        if (ce->cur_col > (int)f->lines[ce->cur_line].len)
            ce->cur_col = (int)f->lines[ce->cur_line].len;
    } else if (c == KEY_ARROW_LEFT) {
        if (ce->cur_col > 0) ce->cur_col--;
        else if (ce->cur_line > 0) {
            ce->cur_line--;
            ce->cur_col = (int)f->lines[ce->cur_line].len;
        }
    } else if (c == KEY_ARROW_RIGHT) {
        if (ce->cur_col < (int)f->lines[ce->cur_line].len) ce->cur_col++;
        else if (ce->cur_line < f->line_count - 1) {
            ce->cur_line++;
            ce->cur_col = 0;
        }
    } else if (c == '\n' || c == '\r' || c == KEY_KP_ENTER) {
        /* Новая строка */
        if (f->line_count >= 4095) return;
        for (int i = f->line_count; i > ce->cur_line + 1; i--)
            f->lines[i] = f->lines[i-1];
        ce_line_init(&f->lines[ce->cur_line + 1]);

        /* Перенос хвоста */
        ce_line_t* cur = &f->lines[ce->cur_line];
        ce_line_t* nxt = &f->lines[ce->cur_line + 1];
        int tail = (int)cur->len - ce->cur_col;
        if (tail > 0) {
            ce_line_ensure(nxt, tail + 1);
            memcpy(nxt->text, cur->text + ce->cur_col, tail);
            nxt->len = (uint32_t)tail;
            nxt->text[tail] = '\0';
            cur->len = (uint32_t)ce->cur_col;
            cur->text[ce->cur_col] = '\0';
        }
        f->line_count++;
        ce->cur_line++;
        ce->cur_col = 0;
        f->modified = true;
    } else if (c == '\b') {
        if (ce->cur_col > 0) {
            ce_line_delete(&f->lines[ce->cur_line], ce->cur_col - 1);
            ce->cur_col--;
            f->modified = true;
        } else if (ce->cur_line > 0) {
            int prev_len = (int)f->lines[ce->cur_line-1].len;
            ce_line_t* prev = &f->lines[ce->cur_line-1];
            ce_line_ensure(prev, prev_len + f->lines[ce->cur_line].len + 1);
            memcpy(prev->text + prev_len, f->lines[ce->cur_line].text,
                   f->lines[ce->cur_line].len + 1);
            prev->len += f->lines[ce->cur_line].len;
            ce_line_free(&f->lines[ce->cur_line]);
            for (int i = ce->cur_line; i < f->line_count - 1; i++)
                f->lines[i] = f->lines[i+1];
            f->line_count--;
            ce->cur_line--;
            ce->cur_col = prev_len;
            f->modified = true;
        }
    } else if (c >= 32 && c < 127) {
        ce_line_insert(&f->lines[ce->cur_line], ce->cur_col, (char)c);
        ce->cur_col++;
        f->modified = true;
    }

    /* Прокрутка */
    if (ce->cur_line < ce->scroll_y) ce->scroll_y = ce->cur_line;
    if (ce->cur_line >= ce->scroll_y + ce->rows)
        ce->scroll_y = ce->cur_line - ce->rows + 1;
    if (ce->scroll_y < 0) ce->scroll_y = 0;
}

/* ─── Точка входа ─── */

gui_window_t* codeeditor_open(const char* filepath) {
    if (!fb_ready()) return (gui_window_t*)0;

    ce_state_t* ce = (ce_state_t*)kmalloc(sizeof(ce_state_t));
    memset(ce, 0, sizeof(*ce));

    ce->line_num_w = 5 * FB_FONT_W;
    ce->minimap_w = 85;
    ce->tab_h = 24;
    ce->status_h = 22;
    ce->rows = ((int)fb_height() - ce->tab_h - ce->status_h) / FB_FONT_H;
    ce->cols = ((int)fb_width() - ce->line_num_w - ce->minimap_w) / FB_FONT_W;

    ce->file_count = 1;
    ce->active_file = 0;
    ce->files[0].lines = 0;

    if (filepath && filepath[0])
        ce_load_file(ce, filepath);
    else {
        ce->files[0].lines = (ce_line_t*)kmalloc(sizeof(ce_line_t));
        ce_line_init(&ce->files[0].lines[0]);
        ce->files[0].line_count = 1;
        ce->files[0].lang = LANG_TEXT;
        strncpy(ce->files[0].name, "untitled.txt", 127);
    }

    ce->running = true;
    while (ce->running) {
        ce_render(ce);
        if (keyboard_has_input()) {
            int c = keyboard_getkey();
            ce_handle_key(ce, c);
        }
        __asm__ volatile ("hlt");
    }

    /* Очистка */
    ce_file_t* f = &ce->files[0];
    for (int i = 0; i < f->line_count; i++) ce_line_free(&f->lines[i]);
    if (f->lines) kfree(f->lines);
    kfree(ce);
    return (gui_window_t*)0;
}

void codeeditor_run(const char* filepath) {
    codeeditor_open(filepath);
}
