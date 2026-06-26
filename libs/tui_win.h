/* tui_win.h — General-purpose TUI window engine (C89)
 *
 * CP437 box-drawing, pane descriptors, border primitives, and text helpers.
 * Builds on conio.h screen-buffer mode (initscr/endwin/refresh).
 *
 * This is a LIBRARY — no application-specific layout or pane names.
 * Applications define their own layout structs and call the primitives.
 *
 * Usage:
 *   TuiPane box = { 0, 0, 40, 10 };   // x, y, w, h
 *   tui_init();
 *   tui_draw_box(&box, "Title", COLOR_PAIR(1));
 *   tui_clear_pane(&box);
 *   tui_pane_puts(&box, 0, 0, "Hello");
 *   tui_end();
 */

#ifndef TUI_WIN_H
#define TUI_WIN_H

#include "conio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════
 * CP437 box-drawing characters (single-line, Turbo Pascal style)
 * ═══════════════════════════════════════════════════════════════════════ */
#define TW_TL  ((char)0xDA)  /* ┌ */
#define TW_TR  ((char)0xBF)  /* ┐ */
#define TW_BL  ((char)0xC0)  /* └ */
#define TW_BR  ((char)0xD9)  /* ┘ */
#define TW_H   ((char)0xC4)  /* ─ */
#define TW_V   ((char)0xB3)  /* │ */
#define TW_LJ  ((char)0xC3)  /* ├ left T-junction */
#define TW_RJ  ((char)0xB4)  /* ┤ right T-junction */
#define TW_TJ  ((char)0xC2)  /* ┬ top T-junction */
#define TW_BJ  ((char)0xC1)  /* ┴ bottom T-junction */
#define TW_X   ((char)0xC5)  /* ┼ cross */

/* ═══════════════════════════════════════════════════════════════════════
 * Pane descriptor — content area inside borders
 *
 * x, y: top-left corner of content area (inside border)
 * w, h: content width and height (excluding border)
 * ═══════════════════════════════════════════════════════════════════════ */
typedef struct {
    int x, y, w, h;
} TuiPane;

/* ═══════════════════════════════════════════════════════════════════════
 * Color pair IDs (initialized by tui_init)
 * ═══════════════════════════════════════════════════════════════════════ */
#define TW_CP_BORDER   1
#define TW_CP_TITLE    2
#define TW_CP_NORMAL   3
#define TW_CP_HILITE   4
#define TW_CP_CHANGED  5
#define TW_CP_ANNOT    6
#define TW_CP_BP       7
#define TW_CP_ON       8   /* active/positive (green on black) */
#define TW_CP_OFF      9   /* inactive/negative (dim on black) */
#define TW_CP_INPUT    10
#define TW_CP_SLEEP    11
#define TW_CP_DIM      12

/* ═══════════════════════════════════════════════════════════════════════
 * Functions
 * ═══════════════════════════════════════════════════════════════════════ */

/* Initialize screen and color pairs. Calls initscr()/start_color(). */
int  tui_init(void);

/* Cleanup: calls endwin(). */
void tui_end(void);

/* Draw a bordered box with optional title.
 * box->x/y are the OUTER corner (border char position).
 * box->w/h include the border: content area = (x+1, y+1, w-2, h-2).
 * title: if non-NULL, drawn at top border after left corner.
 * attr: attribute for border chars (e.g., COLOR_PAIR(TW_CP_BORDER) | A_DIM). */
void tui_draw_box(const TuiPane *box, const char *title, unsigned int attr);

/* Draw a horizontal line from (y, x1) to (y, x2).
 * left_junc/right_junc: junction char at each end (TW_LJ, TW_RJ, TW_TJ, etc.),
 *                       or 0 for no junction (use TW_H corners instead). */
void tui_draw_hline(int y, int x1, int x2, char left_junc, char right_junc, unsigned int attr);

/* Draw a vertical line from (x, y1) to (x, y2).
 * top_junc/bot_junc: junction char at each end (TW_TJ, TW_BJ, TW_LJ, etc.),
 *                     or 0 for no junction (use TW_V). */
void tui_draw_vline(int x, int y1, int y2, char top_junc, char bot_junc, unsigned int attr);

/* Clear a pane's content area (fill with spaces using TW_CP_NORMAL). */
void tui_clear_pane(const TuiPane *p);

/* Write a string at pane coords (row, col), padded with spaces to pane width. */
void tui_pane_puts(const TuiPane *p, int row, int col, const char *s);

/* Write a string at pane coords, truncated to max_w chars, no padding. */
void tui_pane_puts_trunc(const TuiPane *p, int row, int col, const char *s, int max_w);

#ifdef __cplusplus
}
#endif

#endif /* TUI_WIN_H */