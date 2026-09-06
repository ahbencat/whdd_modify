#ifndef NCURSES_CONVENIENCE_H
#define NCURSES_CONVENIENCE_H

#include <menu.h>

/* Fixed render geometry: sized for a 1024x768 screen with the classic 8x16
 * console font (128x48 cells). Renderer layouts are anchored to the top-left
 * corner and never adapt to LINES/COLS, so terminal resizes cannot garble
 * them; on larger terminals the area beyond RENDER_COLS x RENDER_ROWS stays
 * blank. While the terminal is smaller than this, renderers pause drawing. */
#define RENDER_COLS 128
#define RENDER_ROWS 48

void clear_body(void);

/* Renderer layouts need a minimum terminal size; when the terminal is
 * smaller, show an error dialog with actual vs. required size and return
 * -1. Returns 0 when the size is sufficient. */
int require_terminal_size(int min_lines, int min_cols);

#endif // NCURSES_CONVENIENCE_H
