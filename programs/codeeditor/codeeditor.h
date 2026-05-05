/* =============================================================================
 * programs/codeeditor/codeeditor.h
 * Редактор кода MyCode — поддержка C, C++, Python, JS, ASM и др.
 * ============================================================================= */

#ifndef CODEEDITOR_H
#define CODEEDITOR_H

struct gui_window;
typedef struct gui_window gui_window_t;

gui_window_t* codeeditor_open(const char* filepath);
void          codeeditor_run(const char* filepath);

#endif
