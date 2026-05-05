/* =============================================================================
 * gui/gui.c
 * Реализация оконной системы
 * ============================================================================= */

#include "gui.h"
#include "../lib/string.h"
#include "../drivers/keyboard/keyboard.h"
#include "../drivers/timer/pit.h"
#include "../drivers/net/wifi.h"
#include "../drivers/vga/vga_text.h"
#include "../kernel/kernel.h"

/* extern из net.c */
extern void net_poll(void);
extern bool net_ready(void);

/* ─── Глобальное состояние рабочего стола ─── */
static gui_desktop_t desktop;

/* ─── Цвета темы ─── */
#define THEME_TITLE_ACT   0x2B5EA7u   /* заголовок активного окна */
#define THEME_TITLE_INACT 0x5A6A7Au   /* заголовок неактивного окна */
#define THEME_TITLE_TEXT  0xFFFFFFu
#define THEME_CLIENT_BG   0xF2F2F2u
#define THEME_BORDER      0x1A3A5Cu
#define THEME_BORDER_IN   0x9AAAC0u
#define THEME_BTN_NORM    0xDDDDDDu
#define THEME_BTN_HOVER   0xC5D5E8u
#define THEME_BTN_PRESS   0xA0B4C8u
#define THEME_BTN_TEXT    0x111111u
#define THEME_TASKBAR     0x1A2B3Cu
#define THEME_TASKBAR_BTN 0x2A4060u
#define THEME_TASKBAR_TXT 0xEEEEEEu
#define THEME_DESKTOP     0x1A5276u
#define THEME_SHADOW      0x20202060u

/* =============================================================================
 * Отрисовка курсора мыши (стрелка 12×20 пикселей)
 * ============================================================================= */
void gui_draw_cursor(int32_t x, int32_t y) {
    /* Простая стрелка: закрашенный треугольник + контур */
    for (int row = 0; row < 16; row++) {
        int width = row / 2 + 1;
        if (width > 8) width = 8;
        fb_draw_hline(x, y + row, width, FB_WHITE);
    }
    /* Контур */
    fb_draw_line(x, y, x, y+15, FB_BLACK);
    fb_draw_line(x, y, x+7, y+7, FB_BLACK);
    fb_draw_line(x+7, y+7, x+4, y+15, FB_BLACK);
    fb_draw_line(x+4, y+15, x, y+15, FB_BLACK);
}

/* =============================================================================
 * gui_client_rect — прямоугольник рабочей области окна
 * ============================================================================= */
fb_rect_t gui_client_rect(const gui_window_t* win) {
    fb_rect_t r;
    r.x = win->x + GUI_BORDER;
    r.y = win->y + GUI_TITLE_H + GUI_BORDER;
    r.w = win->w - 2 * GUI_BORDER;
    r.h = win->h - GUI_TITLE_H - 2 * GUI_BORDER;
    return r;
}

/* =============================================================================
 * Отрисовка одного окна
 * ============================================================================= */
static void draw_window(gui_window_t* win) {
    if (!win->active || !win->visible) return;

    int32_t x = win->x, y = win->y, w = win->w, h = win->h;
    bool    focused = win->focused;

    /* Тень окна */
    fb_fill_rect(x + 4, y + 4, w, h,
                 fb_blend(FB_BLACK, FB_BLACK, 80));

    /* Внешняя рамка */
    fb_draw_rect(x, y, w, h, focused ? THEME_BORDER : THEME_BORDER_IN);

    /* Заголовок */
    fb_color_t title_bg = focused ? THEME_TITLE_ACT : THEME_TITLE_INACT;
    fb_fill_rect(x + GUI_BORDER, y + GUI_BORDER,
                 w - 2*GUI_BORDER, GUI_TITLE_H - GUI_BORDER, title_bg);

    /* Текст заголовка */
    fb_draw_string(x + GUI_BORDER + 6,
                   y + GUI_BORDER + (GUI_TITLE_H - GUI_BORDER - FB_FONT_H) / 2,
                   win->title, THEME_TITLE_TEXT, title_bg, true);

    /* Кнопка закрыть [X] */
    int32_t bx = x + w - GUI_BORDER - 20;
    int32_t by = y + GUI_BORDER + 2;
    fb_fill_rect(bx, by, 18, GUI_TITLE_H - GUI_BORDER - 4, 0xCC3333u);
    fb_draw_string(bx + 5, by + 3, "X", FB_WHITE, 0xCC3333u, true);

    /* Клиентская область */
    fb_rect_t cr = gui_client_rect(win);
    fb_fill_rect(cr.x, cr.y, cr.w, cr.h, THEME_CLIENT_BG);

    /* Пользовательская отрисовка */
    if (win->on_paint) win->on_paint(win);

    /* Виджеты */
    for (uint32_t i = 0; i < win->widget_count; i++) {
        gui_widget_t* wgt = &win->widgets[i];
        if (!wgt->enabled) continue;

        int32_t wx = cr.x + wgt->x;
        int32_t wy = cr.y + wgt->y;

        switch (wgt->type) {
            case WIDGET_BUTTON: {
                fb_color_t bg = wgt->clicked  ? THEME_BTN_PRESS :
                                wgt->hovered  ? THEME_BTN_HOVER :
                                THEME_BTN_NORM;
                fb_fill_round_rect(wx, wy, wgt->w, wgt->h, 4, bg);
                fb_draw_rect(wx, wy, wgt->w, wgt->h, 0xAAAAAAAAu);
                int32_t tx = wx + (wgt->w - (int32_t)strlen(wgt->text)*FB_FONT_W) / 2;
                int32_t ty = wy + (wgt->h - FB_FONT_H) / 2;
                fb_draw_string(tx, ty, wgt->text, THEME_BTN_TEXT, bg, true);
                break;
            }
            case WIDGET_LABEL:
                fb_draw_string(wx, wy, wgt->text, wgt->fg, THEME_CLIENT_BG, true);
                break;
            case WIDGET_TEXTBOX: {
                fb_fill_rect(wx, wy, wgt->w, wgt->h, FB_WHITE);
                fb_draw_rect(wx, wy, wgt->w, wgt->h, 0x8888AAu);
                fb_draw_string(wx + 4, wy + (wgt->h - FB_FONT_H)/2,
                               wgt->text[0] ? wgt->text : wgt->user_data ?
                               (const char*)wgt->user_data : "",
                               wgt->text[0] ? THEME_BTN_TEXT : 0xAAAAAAAAu,
                               FB_WHITE, true);
                break;
            }
            default: break;
        }
    }
}

/* =============================================================================
 * Отрисовка таскбара
 * ============================================================================= */
static void draw_taskbar(void) {
    int32_t sw = (int32_t)fb_width();
    int32_t sh = (int32_t)fb_height();
    int32_t ty = sh - GUI_TASKBAR_H;

    fb_fill_rect(0, ty, sw, GUI_TASKBAR_H, THEME_TASKBAR);
    fb_draw_hline(0, ty, sw, 0x4A6A8Au);

    /* Кнопка "Пуск" */
    fb_fill_round_rect(4, ty + 4, 60, GUI_TASKBAR_H - 8, 4, 0x2A6EA8u);
    fb_draw_string(10, ty + (GUI_TASKBAR_H - FB_FONT_H)/2,
                   "MyOS", FB_WHITE, 0x2A6EA8u, true);

    /* Кнопки окон в таскбаре */
    int32_t bx = 72;
    for (uint32_t i = 0; i < desktop.window_count; i++) {
        gui_window_t* win = &desktop.windows[i];
        if (!win->active) continue;
        bool focused = ((int32_t)i == desktop.focused_idx);
        fb_color_t bbg = focused ? 0x4A7AB0u : THEME_TASKBAR_BTN;
        fb_fill_round_rect(bx, ty + 4, 120, GUI_TASKBAR_H - 8, 3, bbg);
        fb_draw_string(bx + 6, ty + (GUI_TASKBAR_H - FB_FONT_H)/2,
                       win->title, THEME_TASKBAR_TXT, bbg, true);
        bx += 128;
    }

    /* Время (тики) */
    char tbuf[32];
    uint32_t secs = pit_get_ticks() / 100;
    uint32_t mm   = secs / 60;
    uint32_t ss   = secs % 60;
    /* простой itoa */
    int ti = 0;
    if (mm < 10) tbuf[ti++] = '0';
    /* мин */
    if (mm == 0) { tbuf[ti++] = '0'; }
    else { uint32_t m2 = mm; char tmp[8]; int tj=0;
           while(m2>0){tmp[tj++]='0'+m2%10;m2/=10;}
           while(tj>0) tbuf[ti++]=tmp[--tj]; }
    tbuf[ti++] = ':';
    if (ss < 10) tbuf[ti++] = '0';
    if (ss == 0) { tbuf[ti++] = '0'; }
    else { uint32_t s2=ss; char tmp[8]; int tj=0;
           while(s2>0){tmp[tj++]='0'+s2%10;s2/=10;}
           while(tj>0) tbuf[ti++]=tmp[--tj]; }
    tbuf[ti] = '\0';
    
    /* WiFi статус рядом с таймером */
    wifi_status_t wifi_status = wifi_get_status();
    const char* wifi_label = "";
    fb_color_t wifi_color = 0x666666u;
    if (wifi_status == WIFI_CONNECTED) {
        wifi_label = "[WiFi: ON]";
        wifi_color = 0x22DD22u;
    } else if (wifi_status == WIFI_CONNECTING) {
        wifi_label = "[WiFi...]";
        wifi_color = 0xFFDD22u;
    } else if (wifi_status == WIFI_FAILED) {
        wifi_label = "[WiFi X]";
        wifi_color = 0xFF5555u;
    } else if (wifi_status == WIFI_SCANNING) {
        wifi_label = "[WiFi?]";
        wifi_color = 0xFFDD22u;
    } else {
        wifi_label = "[WiFi OFF]";
        wifi_color = 0x888888u;
    }
    
    fb_draw_string(sw - 150, ty + (GUI_TASKBAR_H - FB_FONT_H)/2,
                   wifi_label, wifi_color, THEME_TASKBAR, true);
    fb_draw_string(sw - 56, ty + (GUI_TASKBAR_H - FB_FONT_H)/2,
                   tbuf, THEME_TASKBAR_TXT, THEME_TASKBAR, true);
}

/* =============================================================================
 * Обработка ввода мыши
 * ============================================================================= */
static void process_mouse(void) {
    const mouse_state_t* ms = mouse_get_state();
    int32_t mx = ms->x, my = ms->y;

    /* Если окно перетаскивается — продолжаем */
    for (int32_t i = (int32_t)desktop.window_count - 1; i >= 0; i--) {
        gui_window_t* win = &desktop.windows[i];
        if (!win->active || !win->dragging) continue;

        win->x = mx - win->drag_ox;
        win->y = my - win->drag_oy;

        /* Клэмпинг чтобы окно не улетело */
        if (win->x < 0) win->x = 0;
        if (win->y < 0) win->y = 0;
        if (win->x + win->w > (int32_t)fb_width())
            win->x = (int32_t)fb_width() - win->w;
        if (win->y + win->h > (int32_t)fb_height() - GUI_TASKBAR_H)
            win->y = (int32_t)fb_height() - GUI_TASKBAR_H - win->h;

        if (!ms->left) win->dragging = false;
        return;
    }

    if (!ms->left_click) {
        /* Обновляем hover состояние виджетов */
        for (int32_t i = (int32_t)desktop.window_count-1; i >= 0; i--) {
            gui_window_t* win = &desktop.windows[i];
            if (!win->active) continue;
            fb_rect_t cr = gui_client_rect(win);
            for (uint32_t j = 0; j < win->widget_count; j++) {
                gui_widget_t* wgt = &win->widgets[j];
                if (!wgt->enabled) continue;
                int32_t wx = cr.x + wgt->x, wy = cr.y + wgt->y;
                wgt->hovered = (mx >= wx && mx < wx + wgt->w &&
                                my >= wy && my < wy + wgt->h);
                wgt->clicked = false;
            }
        }
        return;
    }

    /* ── Клик ── */
    /* Сначала проверяем клик по WiFi кнопке в таскбаре */
    int32_t sh = (int32_t)fb_height();
    int32_t sw = (int32_t)fb_width();
    int32_t ty = sh - GUI_TASKBAR_H;
    if (my >= ty && my < sh) {
        /* WiFi кнопка примерно в районе sw - 150 */
        if (mx >= sw - 160 && mx < sw - 120) {
            gui_create_wifi_dialog();
            mouse_consume_clicks();
            return;
        }
    }
    
    /* Сначала проверяем попал ли клик в какое-то окно */
    bool clicked_window = false;
    for (int32_t i = (int32_t)desktop.window_count - 1; i >= 0; i--) {
        gui_window_t* w = &desktop.windows[i];
        if (!w->active || !w->visible) continue;
        if (mx >= w->x && mx < w->x + w->w &&
            my >= w->y && my < w->y + w->h) {
            clicked_window = true; break;
        }
    }

    /* Если клик не попал ни в одно окно — проверяем иконки рабочего стола */
    if (!clicked_window) {
        apps_handle_icon_click(mx, my);
        mouse_consume_clicks();
        return;
    }

    /* Обрабатываем клик по окнам с верхнего вниз */
    for (int32_t i = (int32_t)desktop.window_count - 1; i >= 0; i--) {
        gui_window_t* win = &desktop.windows[i];
        if (!win->active || !win->visible) continue;

        /* Клик за пределами этого окна — пропускаем */
        if (mx < win->x || mx >= win->x + win->w ||
            my < win->y || my >= win->y + win->h) continue;

        /* Фокусируем окно */
        if (desktop.focused_idx != i) {
            if (desktop.focused_idx >= 0)
                desktop.windows[desktop.focused_idx].focused = false;
            desktop.focused_idx = i;
            win->focused = true;
        }

        /* Клик на кнопку X (закрыть) */
        int32_t bx = win->x + win->w - GUI_BORDER - 20;
        int32_t by = win->y + GUI_BORDER + 2;
        if (mx >= bx && mx < bx+18 && my >= by && my < by + GUI_TITLE_H - GUI_BORDER - 4) {
            win->active = false;
            if (desktop.focused_idx == i) desktop.focused_idx = -1;
            break;
        }

        /* Клик на заголовок → начинаем перетаскивание */
        if (my >= win->y && my < win->y + GUI_TITLE_H) {
            win->dragging  = true;
            win->drag_ox   = mx - win->x;
            win->drag_oy   = my - win->y;
            break;
        }

        /* Клик в клиентской области */
        fb_rect_t cr = gui_client_rect(win);
        if (mx >= cr.x && mx < cr.x + cr.w &&
            my >= cr.y && my < cr.y + cr.h) {
            int32_t lx = mx - cr.x, ly = my - cr.y;

            /* Проверяем виджеты */
            bool handled = false;
            for (uint32_t j = 0; j < win->widget_count; j++) {
                gui_widget_t* wgt = &win->widgets[j];
                if (!wgt->enabled) continue;
                int32_t wx = wgt->x, wy = wgt->y;
                if (lx >= wx && lx < wx + wgt->w &&
                    ly >= wy && ly < wy + wgt->h) {
                    wgt->clicked = true;
                    if (wgt->on_click) wgt->on_click(wgt->user_data);
                    handled = true;
                    break;
                }
            }
            if (!handled && win->on_click) win->on_click(win, lx, ly);
        }
        break;
    }

    mouse_consume_clicks();
}

/* =============================================================================
 * Обработка клавиатуры
 * ============================================================================= */
static void process_keyboard(void) {
    if (!keyboard_has_input()) return;
    int key = keyboard_getkey();   /* возвращает int: ASCII или KEY_ARROW_* */
    if (desktop.focused_idx < 0) return;
    gui_window_t* win = &desktop.windows[desktop.focused_idx];
    if (win->active && win->on_key) win->on_key(win, key);
}

/* =============================================================================
 * gui_run — главный цикл GUI (не возвращается)
 * ============================================================================= */
void gui_run(void) {
    uint32_t last_tick = 0;
    uint32_t last_wifi_scan = 0;
    #define WIFI_SCAN_INTERVAL 3000  /* сканировать каждые 30 секунд (100Hz * 30) */

    while (1) {
        uint32_t now = pit_get_ticks();

        /* Ограничиваем FPS до ~60 (100Hz таймер / ~1-2 тика) */
        if (now - last_tick < 1) {
            __asm__ volatile ("hlt");
            continue;
        }
        last_tick = now;
        desktop.tick++;

        /* Сетевой опрос — принимаем входящие пакеты */
        if (net_ready()) net_poll();

        /* Периодическое сканирование WiFi сетей (каждые 30 сек) */
        if (now - last_wifi_scan >= WIFI_SCAN_INTERVAL) {
            wifi_scan();
            last_wifi_scan = now;
        }

        /* Обрабатываем ввод */
        process_mouse();
        process_keyboard();

        /* ── Рендер ── */
        /* Рабочий стол — градиентный фон */
        for (int32_t row = 0; row < (int32_t)fb_height() - GUI_TASKBAR_H; row++) {
            uint8_t b = (uint8_t)(row * 28 / (int32_t)fb_height());
            fb_color_t c = fb_rgb(0x1A + b, 0x52 + b/2, 0x76 + b/4);
            fb_draw_hline(0, row, (int32_t)fb_width(), c);
        }

        /* Водяной знак */
        fb_draw_string_scaled(
            (int32_t)fb_width()/2 - 96,
            (int32_t)fb_height()/2 - 20,
            "MyOS", fb_blend(0x1A5276u, FB_WHITE, 18), 4);

        /* Иконки приложений */
        apps_draw_desktop_icons();

        /* Окна */
        for (uint32_t i = 0; i < desktop.window_count; i++) {
            draw_window(&desktop.windows[i]);
        }

        /* Таскбар */
        draw_taskbar();

        /* Курсор */
        const mouse_state_t* ms = mouse_get_state();
        gui_draw_cursor(ms->x, ms->y);

        /* Показываем кадр */
        fb_swap();
    }
}

/* =============================================================================
 * Создание/управление окнами и виджетами
 * ============================================================================= */
void gui_init(void) {
    memset(&desktop, 0, sizeof(desktop));
    desktop.focused_idx = -1;
}

gui_window_t* gui_create_window(const char* title,
                                int32_t x, int32_t y,
                                int32_t w, int32_t h) {
    if (desktop.window_count >= GUI_MAX_WINDOWS) return (gui_window_t*)0;

    gui_window_t* win = &desktop.windows[desktop.window_count++];
    memset(win, 0, sizeof(*win));

    strncpy(win->title, title, GUI_TITLE_LEN - 1);
    win->x       = x;  win->y = y;
    win->w       = w;  win->h = h;
    win->active  = true;
    win->visible = true;
    win->focused = (desktop.focused_idx < 0);
    if (win->focused)
        desktop.focused_idx = (int32_t)(desktop.window_count - 1);

    return win;
}

void gui_close_window(gui_window_t* win) {
    if (win) win->active = false;
}

/* ─── Terminal Window ─── */
typedef struct {
    char    cmd_buf[256];
    int     cmd_pos;
    char    output[2048];
    int     output_pos;
} term_state_t;

static term_state_t term_state;

extern void shell_execute_command(const char* cmd);

static void terminal_paint(gui_window_t* w) {
    fb_rect_t cr = gui_client_rect(w);
    fb_fill_rect(cr.x, cr.y, cr.w, cr.h, 0x1A1A1Au);
    
    fb_draw_string(cr.x + 4, cr.y + 4, "MyOS Terminal", 0x00FF00u, 0, true);
    fb_draw_hline(cr.x, cr.y + 20, cr.w, 0x333333u);
    
    /* Output area */
    int y_pos = cr.y + 28;
    int lines_shown = 0;
    const char* p = term_state.output;
    while (*p && lines_shown < 12) {
        const char* line_end = p;
        while (*line_end && *line_end != '\n') line_end++;
        int line_len = (int)(line_end - p);
        if (line_len > 0) {
            char line_buf[128];
            if (line_len > 127) line_len = 127;
            memcpy(line_buf, p, line_len);
            line_buf[line_len] = '\0';
            fb_draw_string(cr.x + 4, y_pos, line_buf, 0x00FF00u, 0, true);
        }
        p = line_end;
        if (*p == '\n') p++;
        y_pos += 14;
        lines_shown++;
    }
    
    /* Command input area */
    fb_draw_hline(cr.x, y_pos, cr.w, 0x333333u);
    y_pos += 4;
    fb_draw_string(cr.x + 4, y_pos, "> ", 0x00FF00u, 0, true);
    fb_draw_string(cr.x + 20, y_pos, term_state.cmd_buf, 0x00FFFFu, 0, true);
}

static void terminal_close(void* ud) {
    gui_close_window((gui_window_t*)ud);
}

gui_window_t* gui_create_terminal_window(void) {
    gui_window_t* w = gui_create_window("Terminal", 100, 100, 600, 400);
    if (!w) return (gui_window_t*)0;
    
    memset(&term_state, 0, sizeof(term_state));
    strcpy(term_state.output, "MyOS Terminal v0.1\n");
    term_state.output_pos = strlen(term_state.output);
    
    w->on_paint = terminal_paint;
    gui_add_button(w, 520, 365, 70, "Close", terminal_close, w);
    
    return w;
}

/* ─── WiFi Selection Window ─── */
typedef struct {
    wifi_network_t networks[16];
    int network_count;
    int selected_idx;
    char password[64];
    int password_pos;
    bool connecting;
} wifi_select_state_t;

static wifi_select_state_t wifi_select_state;

static void wifi_select_paint(gui_window_t* w) {
    fb_rect_t cr = gui_client_rect(w);
    fb_fill_rect(cr.x, cr.y, cr.w, cr.h, 0x1A1A1Au);
    
    fb_draw_string(cr.x + 4, cr.y + 4, "WiFi Networks", 0x00FF00u, 0, true);
    fb_draw_hline(cr.x, cr.y + 20, cr.w, 0x333333u);
    
    /* Получаем свежий список сетей каждый кадр */
    wifi_network_t temp_networks[16];
    int temp_count = 16;  /* Размер буфера - ВАЖНО! */
    wifi_get_networks(temp_networks, &temp_count);
    if (temp_count < 0) temp_count = 0;
    if (temp_count > 16) temp_count = 16;
    wifi_select_state.network_count = temp_count;
    for (int i = 0; i < temp_count; i++) {
        wifi_select_state.networks[i] = temp_networks[i];
    }
    if (wifi_select_state.selected_idx >= temp_count) wifi_select_state.selected_idx = 0;

    int y_pos = cr.y + 28;
    for (int i = 0; i < temp_count && i < 8; i++) {
        fb_color_t color = (i == wifi_select_state.selected_idx) ? 0xFFFF00u : 0x00FF00u;
        fb_color_t bg = (i == wifi_select_state.selected_idx) ? 0x333333u : 0x1A1A1Au;
        if (i == wifi_select_state.selected_idx) {
            fb_fill_rect(cr.x, y_pos - 2, cr.w, 16, bg);
        }
        
        /* Рисуем SSID и уровень сигнала */
        fb_draw_string(cr.x + 4, y_pos, temp_networks[i].ssid, color, 0, true);
        const char* rssi_str = "[???]";
        int rssi = temp_networks[i].rssi;
        if (rssi >= -50) rssi_str = "[Excellent]";
        else if (rssi >= -70) rssi_str = "[Good]";
        else if (rssi >= -80) rssi_str = "[Fair]";
        else rssi_str = "[Weak]";
        fb_draw_string(cr.x + 200, y_pos, rssi_str, color, 0, true);
        y_pos += 18;
    }
    
    if (temp_count == 0) {
        fb_draw_string(cr.x + 4, cr.y + 50, "No networks found.", 0xFF6666u, 0, true);
        fb_draw_string(cr.x + 4, cr.y + 68, "Scanning in background...", 0xFFDD22u, 0, true);
    } else {
        fb_draw_string(cr.x + 4, cr.y + cr.h - 30, "Click to connect", 0x00FF00u, 0, true);
    }
}

static void wifi_select_close(void* ud) {
    gui_close_window((gui_window_t*)ud);
}

static void wifi_connect_selected(void* ud) {
    gui_window_t* w = (gui_window_t*)ud;
    if (wifi_select_state.selected_idx >= wifi_select_state.network_count) return;
    
    wifi_network_t* net = &wifi_select_state.networks[wifi_select_state.selected_idx];
    
    if (net->security == WIFI_SECURITY_OPEN) {
        /* Open network - connect directly */
        wifi_connect(net->ssid, "", WIFI_SECURITY_OPEN);
        gui_close_window(w);
    } else {
        /* WPA2 network - would need password dialog, for now just show message */
        vga_printf("  [GUI] WPA2 network selected: %s - password input not implemented yet\n", net->ssid);
    }
}

static void wifi_scan_button(void* ud) {
    /* Кнопка "Scan" — запускает сканирование */
    vga_printf("  [GUI] Manual WiFi scan triggered\n");
    wifi_scan();
    
    /* Обновляем список сетей после сканирования */
    wifi_select_state.network_count = 16;
    wifi_get_networks(wifi_select_state.networks, &wifi_select_state.network_count);
    wifi_select_state.selected_idx = 0;
}

static void wifi_select_on_click(gui_window_t* w, int32_t x, int32_t y) {
    fb_rect_t cr = gui_client_rect(w);

    /* Check if click is on a network item */
    int y_pos = 28;
    for (int i = 0; i < wifi_select_state.network_count && i < 8; i++) {
        if (x >= 0 && x < cr.w &&
            y >= y_pos && y < y_pos + 18) {
            wifi_select_state.selected_idx = i;
            wifi_connect_selected(w);
            return;
        }
        y_pos += 18;
    }
}

gui_window_t* gui_create_wifi_dialog(void) {
    gui_window_t* w = gui_create_window("WiFi Connect", 150, 150, 400, 300);
    if (!w) return (gui_window_t*)0;
    
    memset(&wifi_select_state, 0, sizeof(wifi_select_state));
    
    /* Сразу сканируем при открытии диалога */
    vga_printf("  [GUI] WiFi dialog opened - starting scan\n");
    wifi_scan();
    
    /* Получаем список сетей - нужно передать размер буфера! */
    wifi_select_state.network_count = 16;  /* Размер буфера */
    wifi_get_networks(wifi_select_state.networks, &wifi_select_state.network_count);
    wifi_select_state.selected_idx = 0;
    
    w->on_paint = wifi_select_paint;
    w->on_click = wifi_select_on_click;
    gui_add_button(w, 200, 265, 80, "Scan", wifi_scan_button, (void*)0);
    gui_add_button(w, 290, 265, 70, "Cancel", wifi_select_close, w);
    
    return w;
}

static gui_widget_t* alloc_widget(gui_window_t* win) {
    if (win->widget_count >= GUI_MAX_WIDGETS) return (gui_widget_t*)0;
    gui_widget_t* w = &win->widgets[win->widget_count++];
    memset(w, 0, sizeof(*w));
    w->enabled = true;
    return w;
}

gui_widget_t* gui_add_button(gui_window_t* win,
                             int32_t x, int32_t y, int32_t w,
                             const char* label,
                             void (*on_click)(void*), void* ud) {
    gui_widget_t* wgt = alloc_widget(win);
    if (!wgt) return (gui_widget_t*)0;
    wgt->type     = WIDGET_BUTTON;
    wgt->x        = x; wgt->y = y;
    wgt->w        = w; wgt->h = GUI_BTN_H + 4;
    wgt->on_click = on_click;
    wgt->user_data = ud;
    strncpy(wgt->text, label, 63);
    return wgt;
}

gui_widget_t* gui_add_label(gui_window_t* win,
                            int32_t x, int32_t y,
                            const char* text, fb_color_t fg) {
    gui_widget_t* wgt = alloc_widget(win);
    if (!wgt) return (gui_widget_t*)0;
    wgt->type = WIDGET_LABEL;
    wgt->x = x; wgt->y = y;
    wgt->w = (int32_t)strlen(text) * FB_FONT_W;
    wgt->h = FB_FONT_H;
    wgt->fg = fg;
    strncpy(wgt->text, text, 63);
    return wgt;
}

gui_widget_t* gui_add_textbox(gui_window_t* win,
                              int32_t x, int32_t y, int32_t w,
                              const char* placeholder) {
    gui_widget_t* wgt = alloc_widget(win);
    if (!wgt) return (gui_widget_t*)0;
    wgt->type = WIDGET_TEXTBOX;
    wgt->x = x; wgt->y = y;
    wgt->w = w; wgt->h = FB_FONT_H + 8;
    wgt->user_data = (void*)placeholder;
    return wgt;
}

void gui_widget_set_text(gui_widget_t* w, const char* text) {
    if (w) strncpy(w->text, text, 63);
}
