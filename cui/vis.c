#include <inttypes.h>
#include "vis.h"
#include "../libdevcheck/utils.h"

vis_t bs_vis[DC_VIS_THRESHOLD_COUNT];  // populated by vis_set_block_size()
vis_t error_vis[]= {
                    { 0,      L' ',      1, MY_COLOR_RED }, // unused
                    { 0,      L'*',      1, MY_COLOR_RED }, // eError
                    { 0,      L'?',      1, MY_COLOR_GRAY }, // eTimeout
                    { 0,      L'x',      1, MY_COLOR_RED }, // eUnc
                    { 0,      L'S',      1, MY_COLOR_GREEN }, // eIdnf
                    { 0,      L'!',      1, MY_COLOR_RED }, // eAbrt
                    { 0,      L'A',      0, MY_COLOR_ORANGE }, // eAmnf
};

/* MHDD 4.6 color/char style, in the canonical 3/10/50/150/500 ms tiers
 * (calibrated for 256-sector blocks; values are populated at runtime
 * from dc_get_vis_thresholds()). Colors from low to high:
 *   dark gray, gray, light gray, green, light red, red.
 * `attrs` encoding: 0 = normal, 1 = bold (A_BOLD), 2 = dim (A_DIM). */
static const vis_t bs_vis_template[DC_VIS_THRESHOLD_COUNT] = {
    { 0,    L'\u2591', 2, MY_COLOR_GRAY }, // <3ms   dark gray
    { 0,    L'\u2591', 0, MY_COLOR_GRAY }, // <10ms  gray
    { 0,    L'\u2591', 1, MY_COLOR_GRAY }, // <50ms  light gray (bold white)
    { 0,    L'\u2588', 0, MY_COLOR_GREEN }, // <150ms green
    { 0,    L'\u2588', 0, MY_COLOR_RED  }, // <500ms light red
};
vis_t exceed_vis =  { 0,    L'\u2588', 1, MY_COLOR_RED };  // >=500ms red (bold)

void init_my_colors(void) {
    init_pair(MY_COLOR_GRAY, COLOR_WHITE, COLOR_BLACK);
    init_pair(MY_COLOR_GREEN, COLOR_GREEN, COLOR_BLACK);
    init_pair(MY_COLOR_RED, COLOR_RED, COLOR_BLACK);
    init_pair(MY_COLOR_WHITE_ON_BLUE, COLOR_WHITE, COLOR_BLUE);
    init_pair(MY_COLOR_ORANGE, COLOR_YELLOW, COLOR_BLACK);
    init_pair(MY_COLOR_BLUE, COLOR_BLUE, COLOR_BLACK);
    init_pair(MY_COLOR_YELLOW, COLOR_YELLOW, COLOR_BLACK);
}

void vis_set_block_size(int sectors_at_once) {
    uint64_t thresholds[DC_VIS_THRESHOLD_COUNT];
    unsigned int i;
    dc_get_vis_thresholds(sectors_at_once, thresholds);
    for (i = 0; i < DC_VIS_THRESHOLD_COUNT; i++) {
        bs_vis[i] = bs_vis_template[i];
        bs_vis[i].access_time = thresholds[i];
    }
}

vis_t choose_vis(uint64_t access_time) {
    unsigned int i;
    for (i = 0; i < DC_VIS_THRESHOLD_COUNT; i++)
        if (access_time < bs_vis[i].access_time)
            return bs_vis[i];
    return exceed_vis;
}


void print_vis(WINDOW *win, vis_t vis) {
    /* vis.attrs: 0=normal, 1=A_BOLD, 2=A_DIM */
    wattrset(win, COLOR_PAIR(vis.color_pair)
            | (vis.attrs == 1 ? A_BOLD : (vis.attrs == 2 ? A_DIM : A_NORMAL)));
    wprintw(win, "%lc", vis.vis);
}

void show_legend(WINDOW *win) {
    unsigned int i;
    for (i = 0; i < DC_VIS_THRESHOLD_COUNT; i++) {
        print_vis(win, bs_vis[i]);
        wattrset(win, A_NORMAL);
        wprintw(win, " <%"PRIu64"ms\n", bs_vis[i].access_time / 1000);
    }
    print_vis(win,exceed_vis);
    wattrset(win, A_NORMAL);
    wprintw(win, " >=%"PRIu64"ms\n", bs_vis[DC_VIS_THRESHOLD_COUNT-1].access_time / 1000);

    print_vis(win, error_vis[1]);
    wattrset(win, A_NORMAL);
    wprintw(win, " ERR\n");

    print_vis(win, error_vis[2]);
    wattrset(win, A_NORMAL);
    wprintw(win, " TIME\n");

    print_vis(win, error_vis[3]);
    wattrset(win, A_NORMAL);
    wprintw(win, " UNC\n");

    print_vis(win, error_vis[4]);
    wattrset(win, A_NORMAL);
    wprintw(win, " IDNF\n");

    print_vis(win, error_vis[5]);
    wattrset(win, A_NORMAL);
    wprintw(win, " ABRT\n");

    print_vis(win, error_vis[6]);
    wattrset(win, A_NORMAL);
    wprintw(win, " AMNF\n");

    wrefresh(win);
}
