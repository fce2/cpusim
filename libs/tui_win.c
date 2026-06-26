/* tui_win.c — General-purpose TUI window engine implementation (C89)
 *
 * CP437 box-drawing, pane layout, border grid, text helpers.
 * Builds on conio.h screen-buffer mode.
 *
 * This is a LIBRARY — no application-specific layout or pane names.
 */

#include "tui_win.h"
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════
 * Initialize screen and color pairs
 * ═══════════════════════════════════════════════════════════════════════ */

int tui_init(void)
{
    if (initscr() < 0) return -1;
    start_color();
    curs_set(1);  /* line cursor (blinks) */

    init_pair(TW_CP_BORDER,  COLOR_WHITE,  COLOR_BLACK);
    init_pair(TW_CP_TITLE,   COLOR_CYAN,   COLOR_BLACK);
    init_pair(TW_CP_NORMAL,  COLOR_WHITE,  COLOR_BLACK);
    init_pair(TW_CP_HILITE,  COLOR_GREEN,  COLOR_BLACK);
    init_pair(TW_CP_CHANGED, COLOR_YELLOW, COLOR_BLACK);
    init_pair(TW_CP_ANNOT,   COLOR_CYAN,   COLOR_BLACK);
    init_pair(TW_CP_BP,      COLOR_RED,    COLOR_BLACK);
    init_pair(TW_CP_ON,      COLOR_GREEN,  COLOR_BLACK);
    init_pair(TW_CP_OFF,     COLOR_WHITE,  COLOR_BLACK);
    init_pair(TW_CP_INPUT,   COLOR_WHITE,  COLOR_BLACK);
    init_pair(TW_CP_SLEEP,   COLOR_MAGENTA,COLOR_BLACK);
    init_pair(TW_CP_DIM,     8,            COLOR_BLACK);  /* dark gray */

    return 0;
}

void tui_end(void)
{
    endwin();
}

/* ═══════════════════════════════════════════════════════════════════════
 * Draw a bordered box with optional title
 *
 * box->x/y: outer corner (where the ┌ goes)
 * box->w/h: total size including border (content = w-2, h-2)
 * ═══════════════════════════════════════════════════════════════════════ */

void tui_draw_box(const TuiPane *box, const char *title, unsigned int attr)
{
    int x = box->x;
    int y = box->y;
    int w = box->w;
    int h = box->h;

    /* Corners */
    attrset(attr);
    mvaddch(y, x, TW_TL);
    mvaddch(y, x + w - 1, TW_TR);
    mvaddch(y + h - 1, x, TW_BL);
    mvaddch(y + h - 1, x + w - 1, TW_BR);

    /* Top and bottom edges */
    for (int i = 1; i < w - 1; i++) {
        mvaddch(y, x + i, TW_H);
        mvaddch(y + h - 1, x + i, TW_H);
    }

    /* Left and right edges */
    for (int j = 1; j < h - 1; j++) {
        mvaddch(y + j, x, TW_V);
        mvaddch(y + j, x + w - 1, TW_V);
    }

    /* Title */
    if (title) {
        attrset(attr | A_BOLD);
        for (int i = 0; title[i]; i++) {
            if (x + 2 + i < x + w - 1)
                mvaddch(y, x + 2 + i, title[i]);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════════
 * Draw a horizontal line with optional junction chars at the ends
 * ═══════════════════════════════════════════════════════════════════════ */

void tui_draw_hline(int y, int x1, int x2, char left_junc, char right_junc, unsigned int attr)
{
    attrset(attr);
    mvaddch(y, x1, left_junc ? left_junc : TW_H);
    mvaddch(y, x2, right_junc ? right_junc : TW_H);
    for (int x = x1 + 1; x < x2; x++)
        mvaddch(y, x, TW_H);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Draw a vertical line with optional junction chars at the ends
 * ═══════════════════════════════════════════════════════════════════════ */

void tui_draw_vline(int x, int y1, int y2, char top_junc, char bot_junc, unsigned int attr)
{
    attrset(attr);
    mvaddch(y1, x, top_junc ? top_junc : TW_V);
    mvaddch(y2, x, bot_junc ? bot_junc : TW_V);
    for (int y = y1 + 1; y < y2; y++)
        mvaddch(y, x, TW_V);
}

/* ═══════════════════════════════════════════════════════════════════════
 * Pane text helpers
 * ═══════════════════════════════════════════════════════════════════════ */

void tui_clear_pane(const TuiPane *p)
{
    attrset(COLOR_PAIR(TW_CP_NORMAL));
    for (int y = 0; y < p->h; y++) {
        move(p->y + y, p->x);
        for (int x = 0; x < p->w; x++)
            addch(' ');
    }
}

void tui_pane_puts(const TuiPane *p, int row, int col, const char *s)
{
    if (row < 0 || row >= p->h) return;
    int max_len = p->w - col;
    if (max_len <= 0) return;
    int len = (int)strlen(s);
    if (len > max_len) len = max_len;
    mvaddstr(p->y + row, p->x + col, s);
    /* Pad with spaces to end of pane */
    if (len < max_len) {
        move(p->y + row, p->x + col + len);
        for (int i = len; i < max_len; i++) addch(' ');
    }
}

void tui_pane_puts_trunc(const TuiPane *p, int row, int col, const char *s, int max_w)
{
    if (row < 0 || row >= p->h) return;
    if (col >= p->w) return;
    int avail = p->w - col;
    if (avail <= 0) return;
    int limit = (max_w > 0 && max_w < avail) ? max_w : avail;
    int len = (int)strlen(s);
    if (len > limit) len = limit;
    char buf[256];
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    memcpy(buf, s, len);
    buf[len] = '\0';
    mvaddstr(p->y + row, p->x + col, buf);
}