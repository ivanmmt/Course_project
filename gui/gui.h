/* =============================================================================
 * gui/gui.h
 * Минимальная оконная система
 *
 * Архитектура:
 *   Desktop → [Window, Window, ...]
 *   Window  → title bar + client area + [Widget, Widget, ...]
 *   Widget  → button | label | textbox | ...
 *
 * Рендер: каждый кадр рисуем всё заново (immediate mode упрощённый).
 * Это медленнее retained mode, но намного проще для учебной ОС.
 * ============================================================================= */

#ifndef GUI_H
#define GUI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../drivers/framebuffer/framebuffer.h"
#include "../drivers/mouse/mouse.h"

/* ─── Размеры элементов ─── */
#define GUI_TITLE_H      24   /* высота заголовка окна */
#define GUI_BORDER       2    /* толщина рамки */
#define GUI_TASKBAR_H    32   /* высота таскбара */
#define GUI_BTN_H        22   /* высота кнопки */
#define GUI_BTN_PAD      8    /* внутренний отступ кнопки */
#define GUI_MAX_WINDOWS  8
#define GUI_MAX_WIDGETS  32
#define GUI_TITLE_LEN    48

/* ─── Тип виджета ─── */
typedef enum {
    WIDGET_NONE   = 0,
    WIDGET_BUTTON,
    WIDGET_LABEL,
    WIDGET_TEXTBOX,
    WIDGET_IMAGE,
} widget_type_t;

/* ─── Виджет ─── */
typedef struct {
    widget_type_t type;
    int32_t  x, y, w, h;      /* позиция и размер внутри клиентской области */
    char     text[64];
    fb_color_t fg, bg;
    bool     enabled;
    bool     hovered;
    bool     clicked;          /* одиночный клик (сбрасывается после чтения) */
    void   (*on_click)(void*); /* callback */
    void*    user_data;
} gui_widget_t;

/* ─── Окно ─── */
typedef struct gui_window {
    bool     active;
    bool     visible;
    bool     focused;
    bool     dragging;         /* окно перетаскивается мышью */
    int32_t  x, y, w, h;      /* позиция и размер (включая рамку и заголовок) */
    int32_t  drag_ox, drag_oy; /* смещение при начале перетаскивания */
    char     title[GUI_TITLE_LEN];

    /* Виджеты */
    gui_widget_t widgets[GUI_MAX_WIDGETS];
    uint32_t     widget_count;

    /* Пользовательская отрисовка клиентской области */
    void (*on_paint)(struct gui_window* win);
    /* Клик внутри клиентской области */
    void (*on_click)(struct gui_window* win, int32_t lx, int32_t ly);
    /* Нажатие клавиши */
    void (*on_key)(struct gui_window* win, int key);

    void* user_data;
} gui_window_t;

/* ─── Рабочий стол ─── */
typedef struct {
    gui_window_t windows[GUI_MAX_WINDOWS];
    uint32_t     window_count;
    int32_t      focused_idx;
    uint32_t     tick;           /* счётчик кадров */
} gui_desktop_t;

/* ─── API ─── */

/* Инициализация GUI системы */
void gui_init(void);

/* Главный цикл (не возвращается) */
void gui_run(void);

/* Создать окно */
gui_window_t* gui_create_window(const char* title,
                                int32_t x, int32_t y,
                                int32_t w, int32_t h);

/* Закрыть окно */
void gui_close_window(gui_window_t* win);

/* Добавить виджет */
gui_widget_t* gui_add_button(gui_window_t* win,
                             int32_t x, int32_t y, int32_t w,
                             const char* label,
                             void (*on_click)(void*), void* ud);

gui_widget_t* gui_add_label(gui_window_t* win,
                            int32_t x, int32_t y,
                            const char* text,
                            fb_color_t fg);

gui_widget_t* gui_add_textbox(gui_window_t* win,
                              int32_t x, int32_t y, int32_t w,
                              const char* placeholder);

/* Обновить текст виджета */
void gui_widget_set_text(gui_widget_t* w, const char* text);

/* Прямоугольник клиентской области окна */
fb_rect_t gui_client_rect(const gui_window_t* win);

/* Рисовать курсор мыши */
void gui_draw_cursor(int32_t x, int32_t y);

/* Создать терминальное окно */
gui_window_t* gui_create_terminal_window(void);

/* Создать WiFi выбора сетей */
gui_window_t* gui_create_wifi_dialog(void);

#endif /* GUI_H */

/* ─── Desktop app functions (из apps.c) ─── */
void apps_draw_desktop_icons(void);
void apps_handle_icon_click(int32_t mx, int32_t my);
void apps_init(void);
