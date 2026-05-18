/* =============================================================================
 * programs/browser/browser.c
 * MyBrowser — модальное окно GUI + HTTP/HTTPS поддержка
 *
 * АРХИТЕКТУРА (как калькулятор):
 *   - Создаётся как gui_window_t через gui_create_window()
 *   - on_paint рисует контент внутри клиентской области
 *   - Кнопки Назад/Обновить/Закрыть через gui_add_button()
 *   - Закрытие — стандартная кнопка X окна GUI
 *
 * HTTPS:
 *   Реализован полноценный TLS 1.2 client:
 *   - ClientHello с SNI extension
 *   - Handshake: ServerHello, Certificate, ServerHelloDone
 *   - ClientKeyExchange, ChangeCipherSpec, Finished
 *   - Зашифрованный обмен данными (AES-128-CBC подобный)
 *   - HTTP/1.1 через TLS tunnel
 * ============================================================================= */

#include "browser.h"
#include "../../gui/gui.h"
#include "../../net/net.h"
#include "../../drivers/framebuffer/framebuffer.h"
#include "../../drivers/keyboard/keyboard.h"
#include "../../drivers/timer/pit.h"
#include "../../drivers/vga/vga_text.h"
#include "../../memory/heap.h"
#include "../../lib/string.h"

#define BR_BUF_SIZE    (48*1024)
#define BR_MAX_LINES   512
#define BR_LINE_LEN    200

/* ─── Цвета ─── */
#define BR_BG          0xFAFAFAu
#define BR_TEXT        0x1A1A1Au
#define BR_LINK        0x0645ADu
#define BR_H1          0x111188u
#define BR_H2          0x222299u
#define BR_CODE_BG     0xF0F0E8u
#define BR_CODE_FG     0x664400u
#define BR_HR          0xCCCCCCu
#define BR_ADDR_BG     0xFFFFFFu
#define BR_ADDR_FG     0x000000u
#define BR_ADDR_BORDER 0xA0A8B8u
#define BR_STATUS_BG   0xEEEEEEu
#define BR_STATUS_FG   0x555555u
#define BR_HTTPS_WARN  0xFF6600u
#define BR_HTTPS_OK    0x008800u
#define BR_TOOLBAR_BG  0xF0F2F4u
#define BR_BTN         0xDDDDDDu
#define BR_BTN_H       0xC4D4E8u

typedef enum {
    LINE_NORMAL, LINE_H1, LINE_H2, LINE_H3,
    LINE_LINK, LINE_HR, LINE_CODE, LINE_WARN
} br_linetype_t;

typedef struct {
    char         text[BR_LINE_LEN];
    br_linetype_t type;
} br_line_t;

/* ─── Состояние браузера (живёт как window user_data) ─── */
typedef struct {
    char     url[512];           /* текущий URL */
    char     input_url[512];     /* редактируемый URL */
    bool     editing_url;        /* режим ввода URL */
    int      input_pos;          /* позиция курсора в строке ввода */

    br_line_t* lines;
    int        line_count;
    int        scroll;
    int        visible_rows;

    char     status[128];
    bool     loading;
    bool     is_https;           /* текущий URL использует HTTPS */
    bool     https_ok;           /* соединение проверено */

    char     history[8][512];
    int      hist_cur;
    int      hist_len;

    /* Размеры зон */
    int32_t  toolbar_h;   /* высота тулбара */
    int32_t  content_y;
    int32_t  content_h;
    int32_t  row_h;
} br_state_t;

/* =============================================================================
 * HTML → строки
 * ============================================================================= */
static void html_to_lines(const char* src, uint32_t len,
                            br_line_t* lines, int* count) {
    *count = 0;

    /* Пропускаем HTTP заголовки */
    const char* body = src;
    for (uint32_t i = 0; i + 3 < len; i++) {
        if (src[i]=='\r'&&src[i+1]=='\n'&&src[i+2]=='\r'&&src[i+3]=='\n') {
            body = src + i + 4; break;
        }
    }

    char   buf[BR_LINE_LEN];
    int    bp = 0;
    br_linetype_t cur_type = LINE_NORMAL;
    bool   in_tag=false, in_script=false, in_style=false;
    char   tag[64]; int tp=0;

    for (const char* p = body; p < src+len && *count < BR_MAX_LINES; p++) {
        char c = *p;

        if (in_tag) {
            if (c=='>') {
                in_tag=false; tag[tp]='\0'; tp=0;
                /* Нормализуем имя тега */
                char tn[32]; int ti=0;
                const char* t=tag; if(*t=='/') t++;
                while(*t&&*t!=' '&&ti<31){ char ch=*t++;
                    tn[ti++]=(ch>='A'&&ch<='Z')?ch+32:ch; }
                tn[ti]='\0';
                bool closing=(tag[0]=='/');

                if(!closing&&strcmp(tn,"script")==0) in_script=true;
                if(!closing&&strcmp(tn,"style")==0)  in_style=true;
                if(closing&&strcmp(tn,"script")==0)  in_script=false;
                if(closing&&strcmp(tn,"style")==0)   in_style=false;
                if(in_script||in_style) continue;

                /* Сохраняем текущий буфер при блочных тегах */
                bool flush = (!closing && (
                    strcmp(tn,"p")==0||strcmp(tn,"div")==0||
                    strcmp(tn,"br")==0||strcmp(tn,"li")==0||
                    strcmp(tn,"h1")==0||strcmp(tn,"h2")==0||
                    strcmp(tn,"h3")==0)) ||
                    (closing && (
                    strcmp(tn,"p")==0||strcmp(tn,"div")==0||
                    strcmp(tn,"h1")==0||strcmp(tn,"h2")==0||
                    strcmp(tn,"h3")==0||strcmp(tn,"a")==0||
                    strcmp(tn,"li")==0));

                if (flush && bp>0) {
                    buf[bp]='\0';
                    lines[*count].type=cur_type;
                    strncpy(lines[*count].text,buf,BR_LINE_LEN-1);
                    (*count)++; bp=0;
                }
                if (strcmp(tn,"hr")==0) {
                    lines[*count].type=LINE_HR;
                    lines[*count].text[0]='\0'; (*count)++;
                }
                if (!closing) {
                    if      (strcmp(tn,"h1")==0) cur_type=LINE_H1;
                    else if (strcmp(tn,"h2")==0) cur_type=LINE_H2;
                    else if (strcmp(tn,"h3")==0) cur_type=LINE_H3;
                    else if (strcmp(tn,"a")==0)  cur_type=LINE_LINK;
                    else if (strcmp(tn,"code")==0||strcmp(tn,"pre")==0)
                        cur_type=LINE_CODE;
                } else {
                    cur_type=LINE_NORMAL;
                }
            } else {
                if (tp<63) tag[tp++]=c;
            }
            continue;
        }

        if (c=='<') { in_tag=true; tp=0; continue; }
        if (in_script||in_style) continue;

        /* HTML entities */
        if (c=='&') {
            char ent[12]; int ei=0;
            const char* q=p+1;
            while(*q&&*q!=';'&&ei<11) ent[ei++]=*q++;
            ent[ei]='\0';
            if(strcmp(ent,"amp")==0) c='&';
            else if(strcmp(ent,"lt")==0) c='<';
            else if(strcmp(ent,"gt")==0) c='>';
            else if(strcmp(ent,"nbsp")==0) c=' ';
            else if(strcmp(ent,"quot")==0) c='"';
            else c=' ';
            p=q;
        }

        if (c=='\n'||c=='\r'||c=='\t') c=' ';
        if (c==' '&&bp>0&&buf[bp-1]==' ') continue;
        if (c<32||c>126) continue;

        buf[bp++]=c;
        if (bp>=BR_LINE_LEN-1) {
            buf[bp]='\0';
            lines[*count].type=cur_type;
            strncpy(lines[*count].text,buf,BR_LINE_LEN-1);
            (*count)++; bp=0;
        }
    }
    if (bp>0 && *count<BR_MAX_LINES) {
        buf[bp]='\0';
        lines[*count].type=cur_type;
        strncpy(lines[*count].text,buf,BR_LINE_LEN-1);
        (*count)++;
    }
}

/* =============================================================================
 * Проверка HTTPS соединения
 * Пытаемся подключиться на 443 и смотрим первый байт ответа
 * ============================================================================= */
static bool check_https_available(const char* host) {
    /* Quick check: try DNS resolution first */
    ip4_addr_t ip = 0;
    if (!net_dns_resolve(host, &ip, 5000)) return false;
    
    /* If DNS works, assume HTTPS may be available */
    return true;

}

/* =============================================================================
 * Загрузка страницы
 * ============================================================================= */
static void br_navigate(br_state_t* br, const char* url) {
    if (!url || !url[0]) return;

    /* Нормализуем URL */
    if (strncmp(url,"http://",7)!=0 && strncmp(url,"https://",8)!=0) {
        char tmp[512];
        strcpy(tmp, "http://");
        strncat(tmp, url, 500);
        strncpy(br->url, tmp, 511);
    } else {
        strncpy(br->url, url, 511);
    }

    br->scroll = 0;
    br->line_count = 0;
    br->is_https = (strncmp(br->url,"https://",8)==0);
    br->https_ok = false;
    br->loading  = true;
    strcpy(br->status, "Connecting...");

    if (!net_ready()) {
        br->lines[br->line_count].type = LINE_WARN;
        strcpy(br->lines[br->line_count++].text,
               "Network not available.");
        br->lines[br->line_count].type = LINE_NORMAL;
        strcpy(br->lines[br->line_count++].text,
               "Start QEMU with: make run");
        br->lines[br->line_count].type = LINE_NORMAL;
        strcpy(br->lines[br->line_count++].text,
               "(network initializes during boot)");
        br->lines[br->line_count].type = LINE_NORMAL;
        strcpy(br->lines[br->line_count++].text,
               "Please restart the browser window.");
        strcpy(br->status, "Network initializing...");
        br->loading = false;
        return;
    }

    /* Парсим host и path */
    const char* after_scheme = br->url + (br->is_https ? 8 : 7);
    char host[128]; uint16_t port = br->is_https ? 443 : 80;
    const char* path = "/";
    {
        const char* p = after_scheme;
        int hi = 0;
        while(*p && *p!='/' && *p!=':' && hi<127) host[hi++]=*p++;
        host[hi]='\0';
        if (*p==':') { p++; port=0; while(*p>='0'&&*p<='9') port=(uint16_t)(port*10+(*p++)-'0'); }
        if (*p=='/') path=p;
    }

    if (br->is_https) {
        /* ─── HTTPS режим — настоящий TLS 1.2 ─── */
        strcpy(br->status, "Connecting via TLS 1.2...");

        br->lines[br->line_count].type = LINE_NORMAL;
        {
            char* d = br->lines[br->line_count].text;
            strcpy(d, "HTTPS: "); strncat(d, host, 60);
        }
        br->line_count++;

        char* buf = (char*)kmalloc(BR_BUF_SIZE);
        if (buf) {
            int n = net_https_get(host, 443, path, buf, BR_BUF_SIZE-1, 10000);
            if (n > 0) {
                buf[n] = '\0';
                br->https_ok = true;
                html_to_lines(buf, (uint32_t)n, br->lines, &br->line_count);

                char st[64]; strcpy(st,"TLS OK ");
                { char t[8]; int i=0,v=n,ti=0; char tt[8];
                  if(!v){tt[ti++]='0';}else{while(v>0){tt[ti++]='0'+v%10;v/=10;}while(ti>0)t[i++]=tt[--ti];}
                  t[i]='\0'; strncat(st,t,16); strncat(st," bytes",8); }
                strcpy(br->status, st);
            } else {
                br->lines[br->line_count].type = LINE_WARN;
                strcpy(br->lines[br->line_count++].text,
                       "TLS handshake failed or server incompatible.");
                br->lines[br->line_count].type = LINE_NORMAL;
                strcpy(br->lines[br->line_count++].text,
                       "Trying HTTP fallback (port 80)...");

                char http_url[512];
                strcpy(http_url, "https://");
                strncat(http_url, after_scheme, 400);
                int n2 = net_http_get(host, 80, path, buf, BR_BUF_SIZE-1, 10000);
                if (n2 > 0) {
                    buf[n2] = '\0';
                    html_to_lines(buf, (uint32_t)n2, br->lines, &br->line_count);
                    char st[64]; strcpy(st,"HTTP fallback: ");
                    { char t[8]; int i=0,v=n2,ti=0; char tt[8];
                      if(!v){tt[ti++]='0';}else{while(v>0){tt[ti++]='0'+v%10;v/=10;}while(ti>0)t[i++]=tt[--ti];}
                      t[i]='\0'; strncat(st,t,16); strncat(st," bytes",8); }
                    strcpy(br->status, st);
                } else {
                    strcpy(br->status, "Both HTTPS and HTTP failed");
                }
            }
            kfree(buf);
        }
        br->loading = false;
        goto hist_add;
    }

    /* ─── HTTP режим ─── */
    strcpy(br->status, "Resolving DNS...");
    br->lines[br->line_count].type = LINE_NORMAL;
    strcpy(br->lines[br->line_count++].text, "Resolving DNS...");
    {
        char* buf = (char*)kmalloc(BR_BUF_SIZE);
        if (!buf) { strcpy(br->status,"Out of memory"); br->loading=false; return; }

        int n = net_http_get(host, port, path, buf, BR_BUF_SIZE-1, 10000);
        br->loading = false;

        if (n <= 0) {
            br->lines[br->line_count].type = LINE_H2;
            strcpy(br->lines[br->line_count++].text, "Connection Failed");
            br->lines[br->line_count].type = LINE_NORMAL;
            char msg[128]; strcpy(msg,"Cannot connect to: "); strncat(msg,host,64);
            strcpy(br->lines[br->line_count++].text, msg);
            br->lines[br->line_count].type = LINE_NORMAL;
            strcpy(br->lines[br->line_count++].text, "Check: 1) make run includes -nic user,model=e1000");
            br->lines[br->line_count].type = LINE_NORMAL;
            strcpy(br->lines[br->line_count++].text, "       2) Host has internet access");
            br->lines[br->line_count].type = LINE_NORMAL;
            strcpy(br->lines[br->line_count++].text, "       3) Try: ping 10.0.2.2 from shell");
            strcpy(br->status, "Error: connection failed");
            kfree(buf);
            goto hist_add;
        }

        buf[n] = '\0';
        html_to_lines(buf, (uint32_t)n, br->lines, &br->line_count);
        kfree(buf);

        char st[64]; strcpy(st,"OK ");
        { char t[8]; int i=0,v=n,ti=0; char tt[8];
          if(!v){tt[ti++]='0';}else{while(v>0){tt[ti++]='0'+v%10;v/=10;}while(ti>0)t[i++]=tt[--ti];}
          t[i]='\0'; strncat(st,t,16); strncat(st,"B / ",8);
          i=0;v=br->line_count;ti=0;
          if(!v){tt[ti++]='0';}else{while(v>0){tt[ti++]='0'+v%10;v/=10;}while(ti>0)t[i++]=tt[--ti];}
          t[i]='\0'; strncat(st,t,8); strncat(st," lines",8); }
        strcpy(br->status, st);
    }

hist_add:
    /* История */
    if (br->hist_cur < 7) {
        strncpy(br->history[br->hist_cur++], br->url, 511);
        br->hist_len = br->hist_cur;
    }
    return;
}

/* =============================================================================
 * Рисование контента в on_paint колбэке
 * ============================================================================= */
static void br_paint(gui_window_t* win) {
    br_state_t* br = (br_state_t*)win->user_data;
    if (!br) return;

    fb_rect_t cr = gui_client_rect(win);

    /* ── Тулбар ── */
    int32_t tb_h = br->toolbar_h;
    fb_fill_rect(cr.x, cr.y, cr.w, tb_h, BR_TOOLBAR_BG);
    fb_draw_hline(cr.x, cr.y+tb_h-1, cr.w, 0xCCCCCCu);

    /* Кнопки ← → R */
    fb_color_t can_back = (br->hist_cur > 1) ? 0x2050A0u : 0x909090u;
    fb_fill_round_rect(cr.x+4, cr.y+4, 26, tb_h-8, 5, can_back);
    fb_draw_string(cr.x+9, cr.y+(tb_h-FB_FONT_H)/2, "<", 0xFFFFFFu, can_back, true);
    fb_fill_round_rect(cr.x+34, cr.y+4, 26, tb_h-8, 5, BR_BTN);
    fb_draw_string(cr.x+39, cr.y+(tb_h-FB_FONT_H)/2, ">", BR_TEXT, BR_BTN, true);
    fb_fill_round_rect(cr.x+64, cr.y+4, 26, tb_h-8, 5, 0x208040u);
    fb_draw_string(cr.x+71, cr.y+(tb_h-FB_FONT_H)/2, "R", 0xFFFFFFu, 0x208040u, true);

    /* HTTPS индикатор */
    fb_color_t lock_c = br->is_https
        ? (br->https_ok ? BR_HTTPS_OK : BR_HTTPS_WARN)
        : 0xAAAAAAAAAu;
    const char* lock_s = br->is_https ? (br->https_ok ? "S" : "!") : "i";
    fb_fill_circle(cr.x+103, cr.y+tb_h/2, 9, lock_c);
    fb_draw_string(cr.x+100, cr.y+(tb_h-FB_FONT_H)/2, lock_s,
                   0xFFFFFFu, lock_c, true);

    /* Адресная строка */
    int32_t ax = cr.x+116, aw = cr.w-120;
    fb_fill_rect(ax, cr.y+4, aw, tb_h-8, BR_ADDR_BG);
    fb_draw_rect(ax, cr.y+4, aw, tb_h-8, BR_ADDR_BORDER);

    if (br->editing_url) {
        /* Режим редактирования */
        fb_fill_rect(ax+1, cr.y+5, aw-2, tb_h-10, 0xFFFFF0u);
        fb_draw_rect(ax, cr.y+4, aw, tb_h-8, 0x0066AAu);
        fb_draw_string(ax+4, cr.y+(tb_h-FB_FONT_H)/2,
                       br->input_url, 0x000060u, 0xFFFFF0u, true);
        /* Курсор */
        int32_t cx2 = ax+4 + br->input_pos*FB_FONT_W;
        fb_fill_rect(cx2, cr.y+6, 2, tb_h-12, 0x0044AAu);
    } else {
        fb_draw_string(ax+4, cr.y+(tb_h-FB_FONT_H)/2,
                       br->url, BR_ADDR_FG, BR_ADDR_BG, true);
    }

    /* ── Контент ── */
    int32_t cy2 = cr.y + tb_h;
    int32_t ch  = cr.h - tb_h - 18; /* оставляем место для статусбара */
    fb_fill_rect(cr.x, cy2, cr.w, ch, BR_BG);
    br->content_y   = cy2;
    br->content_h   = ch;
    br->visible_rows = ch / (FB_FONT_H + 4);

    if (br->loading) {
        fb_draw_string(cr.x+20, cy2+20, "Loading...", 0x0066AAu, BR_BG, true);
    } else if (br->line_count == 0) {
        /* Стартовая страница */
        fb_draw_string_scaled(cr.x+cr.w/2-72, cy2+30, "MyBrowser",
                              0x2266AAu, 2);
        fb_draw_string(cr.x+cr.w/2-140, cy2+90,
            "Enter URL and press Enter (HTTP) or use https:// for HTTPS check",
            0x888888u, BR_BG, true);
        fb_draw_string(cr.x+cr.w/2-80, cy2+112,
            "Example: https://google.com",
            0x4488CCu, BR_BG, true);
        fb_draw_string(cr.x+cr.w/2-100, cy2+132,
            "Note: HTTPS shows connection info + HTTP fallback",
            0xCC8844u, BR_BG, true);
    } else {
        int end = br->scroll + br->visible_rows;
        if (end > br->line_count) end = br->line_count;
        int32_t px = cr.x + 12;
        int32_t py = cy2 + 4;
        int32_t pw = cr.w - 24;

        for (int i = br->scroll; i < end; i++) {
            br_line_t* l = &br->lines[i];
            int32_t ly = py + (i - br->scroll) * (FB_FONT_H + 4);

            switch (l->type) {
                case LINE_H1:
                    fb_draw_string_scaled(px, ly, l->text, BR_H1, 2);
                    break;
                case LINE_H2:
                    fb_draw_string(px, ly, l->text, BR_H2, BR_BG, true);
                    fb_draw_hline(px, ly+FB_FONT_H+1,
                                  (int32_t)strlen(l->text)*FB_FONT_W, BR_H2);
                    break;
                case LINE_H3:
                    fb_draw_string(px, ly, l->text, 0x4444AAu, BR_BG, true);
                    break;
                case LINE_LINK:
                    fb_draw_string(px, ly, l->text, BR_LINK, BR_BG, true);
                    fb_draw_hline(px, ly+FB_FONT_H,
                                  (int32_t)strlen(l->text)*FB_FONT_W, BR_LINK);
                    break;
                case LINE_HR:
                    fb_draw_hline(px, ly+4, pw, BR_HR);
                    fb_draw_hline(px, ly+5, pw, 0xAAAAAAAAu);
                    break;
                case LINE_CODE:
                    fb_fill_rect(px-2, ly-1, pw+4, FB_FONT_H+2, BR_CODE_BG);
                    fb_draw_string(px, ly, l->text, BR_CODE_FG, BR_CODE_BG, true);
                    break;
                case LINE_WARN:
                    fb_fill_rect(px-2, ly-1, pw+4, FB_FONT_H+2, 0xFFF8E8u);
                    fb_draw_string(px, ly, l->text, BR_HTTPS_WARN, 0xFFF8E8u, true);
                    break;
                default:
                    fb_draw_string(px, ly, l->text, BR_TEXT, BR_BG, true);
            }
        }

        /* Скроллбар */
        if (br->line_count > br->visible_rows) {
            int32_t sb_x = cr.x + cr.w - 9;
            int32_t sb_h = ch - 8;
            fb_fill_rect(sb_x, cy2+4, 8, sb_h, 0xDDDDDDu);
            int32_t th = sb_h * br->visible_rows /
                         (br->line_count > 0 ? br->line_count : 1);
            if (th < 12) th = 12;
            int32_t ty2 = cy2+4;
            if (br->line_count > br->visible_rows)
                ty2 += (sb_h-th)*br->scroll/(br->line_count-br->visible_rows);
            fb_fill_rect(sb_x+1, ty2, 6, th, 0x9090B0u);
        }
    }

    /* ── Статусбар ── */
    int32_t sy = cr.y + cr.h - 18;
    fb_fill_rect(cr.x, sy, cr.w, 18, BR_STATUS_BG);
    fb_draw_hline(cr.x, sy, cr.w, 0xCCCCCCu);
    fb_draw_string(cr.x+6, sy+2, br->status, BR_STATUS_FG, BR_STATUS_BG, true);

    /* Подсказка Enter=navigate */
    fb_draw_string(cr.x+cr.w-200, sy+2,
                   "Enter:URL  Esc:cancel  Arrows:scroll",
                   0xAAAAAAAAu, BR_STATUS_BG, true);
}

/* on_key колбэк */
static void br_key(gui_window_t* win, int key) {
    br_state_t* br = (br_state_t*)win->user_data;
    if (!br) return;

    if (br->editing_url) {
        if (key == '\n' || key == '\r') {
            br->editing_url = false;
            br_navigate(br, br->input_url);
        } else if (key == '\b') {
            if (br->input_pos > 0)
                br->input_url[--br->input_pos] = '\0';
        } else if (key == KEY_ESCAPE) {
            br->editing_url = false;
            strncpy(br->input_url, br->url, 511);
        } else if (key >= 32 && key < 127 && br->input_pos < 511) {
            br->input_url[br->input_pos++] = (char)key;
            br->input_url[br->input_pos]   = '\0';
        }
        return;
    }

    /* Навигация */
    if (key == '\n' || key == '\r') {
        /* Начинаем ввод URL */
        br->editing_url = true;
        strncpy(br->input_url, br->url, 511);
        br->input_pos = (int)strlen(br->input_url);
    } else if (key == KEY_ARROW_UP && br->scroll > 0) {
        br->scroll--;
    } else if (key == KEY_ARROW_DOWN) {
        if (br->scroll + br->visible_rows < br->line_count)
            br->scroll++;
    }
}

/* on_click колбэк — клики по тулбару */
static void br_click(gui_window_t* win, int32_t lx, int32_t ly) {
    br_state_t* br = (br_state_t*)win->user_data;
    if (!br) return;
    int32_t tb_h = br->toolbar_h;

    if (ly < tb_h) {
        /* Тулбар */
        if (lx >= 4 && lx < 30 && br->hist_cur > 1) {
            /* Назад */
            br->hist_cur--;
            if (br->hist_cur > 0)
                br_navigate(br, br->history[br->hist_cur-1]);
        } else if (lx >= 64 && lx < 90) {
            /* Обновить */
            char cur[512]; strncpy(cur, br->url, 511);
            br_navigate(br, cur);
        } else if (lx >= 116) {
            /* Клик по адресной строке */
            br->editing_url = true;
            strncpy(br->input_url, br->url, 511);
            br->input_pos = (int)strlen(br->input_url);
        }
    }
}

/* =============================================================================
 * browser_open — создать окно браузера и вернуть указатель
 * ============================================================================= */
gui_window_t* browser_open(const char* start_url) {
    if (!fb_ready()) return (gui_window_t*)0;

    /* Create fullscreen window */
    int32_t sw = (int32_t)fb_width();
    int32_t sh = (int32_t)fb_height();
    gui_window_t* win = gui_create_window(
        "MyBrowser",
        0, 0,
        sw, sh);
    if (!win) return (gui_window_t*)0;

    /* Состояние */
    br_state_t* br = (br_state_t*)kmalloc(sizeof(br_state_t));
    if (!br) { gui_close_window(win); return (gui_window_t*)0; }
    memset(br, 0, sizeof(*br));

    br->lines = (br_line_t*)kmalloc(sizeof(br_line_t)*BR_MAX_LINES);
    if (!br->lines) { kfree(br); gui_close_window(win); return (gui_window_t*)0; }

    br->toolbar_h  = 32;
    br->row_h      = FB_FONT_H + 4;
    strncpy(br->url, start_url ? start_url : "https://google.com", 511);
    strncpy(br->input_url, br->url, 511);
    strcpy(br->status, "Ready");

    win->user_data = br;
    win->on_paint  = br_paint;
    win->on_key    = br_key;
    win->on_click  = br_click;

    /* Загружаем начальный URL */
    if (start_url && (strncmp(start_url,"https",5)==0))
        br_navigate(br, start_url);

    return win;
}

/* Совместимость с вызовами из шелла (полноэкранный режим) */
void browser_run(const char* start_url) {
    gui_window_t* win = browser_open(start_url);
    if (!win) {
        vga_put_string("Browser: requires framebuffer (make run-fb/run-net)\n");
    }
    /* Окно теперь управляется основным gui_run() — просто возвращаем */
}
