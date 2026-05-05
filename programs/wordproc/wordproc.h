/* =============================================================================
 * programs/wordproc/wordproc.h
 * Текстовый процессор MyWord — аналог Microsoft Word
 * ============================================================================= */

#ifndef WORDPROC_H
#define WORDPROC_H

struct gui_window;
typedef struct gui_window gui_window_t;

gui_window_t* wordproc_open(const char* filepath);
void          wordproc_run(const char* filepath);

#endif
