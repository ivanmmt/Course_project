#ifndef BROWSER_H
#define BROWSER_H

/* Прямое объявление без включения gui.h
 * (избегаем проблем с путями при включении из разных мест) */
struct gui_window;
typedef struct gui_window gui_window_t;

gui_window_t* browser_open(const char* start_url);
void          browser_run(const char* start_url);

#endif
