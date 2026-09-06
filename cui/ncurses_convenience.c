#include <assert.h>
#include <stdio.h>
#include <dialog.h>
#include "ncurses_convenience.h"
#include "vis.h"
#include "libdevcheck.h"

int require_terminal_size(int min_lines, int min_cols) {
    if (LINES >= min_lines && COLS >= min_cols)
        return 0;
    char msg[160];
    snprintf(msg, sizeof(msg),
            "Terminal is %d rows x %d cols.\n"
            "This procedure needs at least %d rows x %d cols to render.",
            LINES, COLS, min_lines, min_cols);
    dialog_msgbox("Error", msg, 0, 0, 1);
    return -1;
}

void clear_body(void) {
    WINDOW *body = derwin(stdscr, LINES, COLS, 0, 0);
    wclear(body);
    wrefresh(body);
    delwin(body);

    WINDOW *footer = subwin(stdscr, 1, COLS, LINES-1, 0);
    wbkgd(footer, COLOR_PAIR(MY_COLOR_WHITE_ON_BLUE));
    wprintw(footer, " WHDD rev. " WHDD_VERSION);
    wrefresh(footer);
    delwin(footer);
}

