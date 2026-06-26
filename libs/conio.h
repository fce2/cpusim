/* conio.h — Minimal console I/O library
 * Win32: Windows Console API (in conio.c, needs <windows.h> there)
 * POSIX: termios + VT100 escape sequences
 * Subset of curses API: initscr(), endwin(), mvaddstr(), etc.
 */

#ifndef CONIO_H
#define CONIO_H

#include <stddef.h>  /* wchar_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Color pair support (foreground + background) */
#define COLOR_BLACK   0
#define COLOR_RED     1
#define COLOR_GREEN   2
#define COLOR_YELLOW  3
#define COLOR_BLUE    4
#define COLOR_MAGENTA 5
#define COLOR_CYAN    6
#define COLOR_WHITE   7

#define COLOR_PAIR(n) ((n) << 8)   /* bits 8-15: pair index (8 bits, up to 255) */
#define A_BOLD        0x00010000   /* bit 16: bold/intensity */
#define A_REVERSE     0x00020000   /* bit 17: swap fg/bg */
#define A_UNDERLINE   0x00040000   /* bit 18: underline */
#define A_DIM         0x00080000   /* bit 19: dim/low intensity */
#define A_NORMAL      0

/* Key codes (beyond ASCII) */
#define KEY_DOWN      0x0100
#define KEY_UP        0x0101
#define KEY_LEFT      0x0102
#define KEY_RIGHT     0x0103
#define KEY_HOME      0x0104
#define KEY_END       0x0105
#define KEY_PPAGE     0x0106
#define KEY_NPAGE     0x0107
#define KEY_F1        0x0108
#define KEY_F2        0x0109
#define KEY_F3        0x010A
#define KEY_F4        0x010B
#define KEY_F5        0x010C
#define KEY_F6        0x010D
#define KEY_F7        0x010E
#define KEY_F8        0x010F
#define KEY_F9        0x0110
#define KEY_F10       0x0111
#define KEY_F11       0x0112
#define KEY_F12       0x0113
#define KEY_IC        0x0114  /* Insert */
#define KEY_DC        0x0115  /* Delete */
#define KEY_BTAB      0x0116  /* Back-tab (Shift+Tab) */
#define KEY_ENTER     0x0117
#define KEY_BACKSPACE 0x0118
#define KEY_SLEFT    0x0119  /* Shift+Left (select) */
#define KEY_SRIGHT   0x011A  /* Shift+Right (select) */
#define KEY_CLEFT    0x0120  /* Ctrl+Left (word left) */
#define KEY_CRIGHT   0x0121  /* Ctrl+Right (word right) */
#define KEY_CHOME    0x0122  /* Ctrl+Home (doc top) */
#define KEY_CEND     0x0123  /* Ctrl+End (doc end) */
#define KEY_SF12     0x0124  /* Shift+F12 */
#define KEY_RESIZE   0x0200  /* Terminal/window resized */

/* Screen functions (Win32 screen-buffer mode only) */
int  initscr(void);
void endwin(void);
void refresh(void);
void clear(void);
void clrtoeol(void);

/* Input — works in both modes */
int  getch(void);
void nodelay(int mode);
void keypad(void*, int);

/* Output (Win32 screen-buffer mode only) */
int  mvaddstr(int y, int x, const char* str);
int  addstr(const char* str);
int  mvaddch(int y, int x, char ch);
int  addch(char ch);
int  mvaddwch(int y, int x, wchar_t wch);  /* wide/Unicode char */
int  addwch(wchar_t wch);
int  mvaddwstr(int y, int x, const wchar_t *wstr); /* wide string */
void move(int y, int x);
void attrset(unsigned int attr);
void attron(unsigned int attr);
void attroff(unsigned int attr);

/* Color functions (Win32 screen-buffer mode only) */
int   has_colors(void);
void  start_color(void);
void  init_pair(int pair, int fg, int bg);

/* Screen info (Win32 screen-buffer mode only) */
int  getmaxy(void);
int  getmaxx(void);
int  getcury(void);
int  getcurx(void);

/* Cursor control (Win32 screen-buffer mode only) */
void curs_set(int visibility);

/* Lightweight init — enables VT100 output + raw input.
 * Use instead of initscr() for printf-based output with getch() input.
 * Returns 0 on success, -1 on error. */
int conio_init(void);

/* Cleanup — restores terminal. Call on exit after conio_init(). */
void conio_cleanup(void);

/* Force screen buffer resize after terminal size change */
void conio_resize(void);

/* Expose internal state for debugging */
#ifdef __cplusplus
extern "C" {
#endif
int conio_is_console_flag(void);
int conio_pipe_mode_flag(void);
#ifdef __cplusplus
}
#endif

/* ANSI escape codes (for VT100-based output) */
#define ANSI_RESET     "\033[0m"
#define ANSI_BOLD      "\033[1m"
#define ANSI_DIM       "\033[2m"
#define ANSI_RED       "\033[31m"
#define ANSI_GREEN     "\033[32m"
#define ANSI_YELLOW    "\033[33m"
#define ANSI_BLUE      "\033[34m"
#define ANSI_MAGENTA   "\033[35m"
#define ANSI_CYAN      "\033[36m"
#define ANSI_BRED      "\033[91m"
#define ANSI_BGREEN    "\033[92m"
#define ANSI_BYELLOW   "\033[93m"
#define ANSI_BBLUE     "\033[94m"
#define ANSI_BMAGENTA  "\033[95m"
#define ANSI_BCYAN     "\033[96m"

#ifdef __cplusplus
}
#endif

#endif /* CONIO_H */