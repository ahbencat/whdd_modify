#ifndef NCURSES_CONVENIENCE_H
#define NCURSES_CONVENIENCE_H

#include <menu.h>

void clear_body(void);

/* Renderer layouts need a minimum terminal size; when the terminal is
 * smaller, show an error dialog with actual vs. required size and return
 * -1. Returns 0 when the size is sufficient. */
int require_terminal_size(int min_lines, int min_cols);

#endif // NCURSES_CONVENIENCE_H
