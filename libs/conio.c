/* conio.c — Minimal console I/O library
   Win32: Windows Console API
   POSIX: termios + VT100 escape sequences */

#include "conio.h"

#ifdef _WIN32

#include <windows.h>
#include <stdlib.h>
#include <string.h>

static HANDLE hStdOut = INVALID_HANDLE_VALUE;
static HANDLE hStdIn  = INVALID_HANDLE_VALUE;
static CONSOLE_SCREEN_BUFFER_INFO csbiOrig;
static DWORD dwOrigMode = 0;
static int scr_width = 80;
static int scr_height = 25;
static CHAR_INFO* screen_buf = NULL;
static int cur_x = 0;
static int cur_y = 0;
static unsigned int cur_attr = 0x07; /* white on black */
static int nodelay_mode = 0;

/* Lightweight mode state */
static int conio_is_console = 0; /* 1=input is a console, 0=pipe */
static int conio_pipe_mode = 0; /* 1=use ReadFile+VT parsing, 0=use ReadConsoleInput */
static DWORD conio_orig_in_mode = 0;
static DWORD conio_orig_out_mode = 0;
static int conio_initialized = 0;

/* Forward declarations for lightweight mode */
static int read_byte(void);
static int peek_byte(int wait_ms);
static int vt100_getch(void);

/* Color pair table (up to 64 pairs) */
static WORD color_pairs[64];
static int color_pairs_initialized = 0;

/* Map our color constants to Windows console colors */
static WORD color_map(int c) {
    switch (c) {
        case COLOR_BLACK:   return 0;
        case COLOR_RED:     return FOREGROUND_RED;
        case COLOR_GREEN:   return FOREGROUND_GREEN;
        case COLOR_YELLOW:  return FOREGROUND_RED | FOREGROUND_GREEN;
        case COLOR_BLUE:    return FOREGROUND_BLUE;
        case COLOR_MAGENTA: return FOREGROUND_RED | FOREGROUND_BLUE;
        case COLOR_CYAN:    return FOREGROUND_GREEN | FOREGROUND_BLUE;
        case COLOR_WHITE:   return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        case 8:             return FOREGROUND_INTENSITY;  /* dark gray = bright black */
        case 9:             return FOREGROUND_RED | FOREGROUND_INTENSITY;  /* bright red */
        case 10:            return FOREGROUND_GREEN | FOREGROUND_INTENSITY;  /* bright green */
        case 11:            return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;  /* bright yellow */
        case 12:            return FOREGROUND_BLUE | FOREGROUND_INTENSITY;  /* bright blue */
        case 13:            return FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;  /* bright magenta */
        case 14:            return FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;  /* bright cyan */
        case 15:            return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;  /* bright white */
        default:            return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }
}

static WORD make_attr(unsigned int attr) {
    WORD wAttr = 0;
    int pair;

    /* Extract color pair */
    pair = (attr >> 8) & 0x3F;
    if (pair > 0 && color_pairs_initialized) {
        wAttr = color_pairs[pair];
    } else {
        /* Default: light gray on black */
        wAttr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    }

    /* Bold = intensity */
    if (attr & A_BOLD)   wAttr |= FOREGROUND_INTENSITY;
    if (attr & A_DIM)    wAttr &= ~FOREGROUND_INTENSITY;
    /* Reverse = swap fg/bg */
    if (attr & A_REVERSE) {
        WORD fg = wAttr & 0x0F;
        WORD bg = wAttr & 0xF0;
        wAttr = (WORD)((fg << 4) | (bg >> 4));
    }

    return wAttr;
}

static int scr_buf_size = 0;  /* allocated size in CHAR_INFO elements */

static void ensure_buf(void) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    int needed;
    int i;
    if (!GetConsoleScreenBufferInfo(hStdOut, &csbi)) return;
    scr_width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    scr_height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    needed = scr_width * scr_height;
    if (needed > scr_buf_size) {
        free(screen_buf);
        screen_buf = (CHAR_INFO*)malloc((size_t)needed * sizeof(CHAR_INFO));
        scr_buf_size = needed;
    }
    for (i = 0; i < needed; i++) {
        screen_buf[i].Char.AsciiChar = ' ';
        screen_buf[i].Attributes = make_attr(cur_attr);
    }
}

int initscr(void) {
    CONSOLE_CURSOR_INFO cci;
    hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    hStdIn  = GetStdHandle(STD_INPUT_HANDLE);

    if (hStdOut == INVALID_HANDLE_VALUE || hStdIn == INVALID_HANDLE_VALUE)
        return -1;

    /* Save original console mode */
    GetConsoleMode(hStdIn, &dwOrigMode);

    /* Set input mode: no line input, no echo, no process input */
    SetConsoleMode(hStdIn, ENABLE_WINDOW_INPUT);

    /* Save original screen buffer info */
    GetConsoleScreenBufferInfo(hStdOut, &csbiOrig);

    /* Hide cursor initially */
    GetConsoleCursorInfo(hStdOut, &cci);
    cci.bVisible = FALSE;
    SetConsoleCursorInfo(hStdOut, &cci);

    ensure_buf();

    /* Set up default color pairs */
    start_color();

    return 0;
}

void endwin(void) {
    COORD coord;
    DWORD written;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    CONSOLE_CURSOR_INFO cci;
    int cells;

    if (hStdOut != INVALID_HANDLE_VALUE) {
        /* Clear screen */
        coord.X = 0;
        coord.Y = 0;
        GetConsoleScreenBufferInfo(hStdOut, &csbi);
        cells = (csbi.srWindow.Right - csbi.srWindow.Left + 1) *
                (csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
        FillConsoleOutputCharacter(hStdOut, ' ', cells, coord, &written);
        FillConsoleOutputAttribute(hStdOut, csbiOrig.wAttributes, cells, coord, &written);

        /* Restore cursor */
        GetConsoleCursorInfo(hStdOut, &cci);
        cci.bVisible = TRUE;
        SetConsoleCursorInfo(hStdOut, &cci);

        /* Restore cursor position */
        SetConsoleCursorPosition(hStdOut, csbiOrig.dwCursorPosition);

        /* Restore input mode */
        SetConsoleMode(hStdIn, dwOrigMode);
    }

    free(screen_buf);
    screen_buf = NULL;
    scr_buf_size = 0;
}

void refresh(void) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    SMALL_RECT writeRegion;
    COORD bufSize;
    COORD bufCoord;
    COORD cursorPos;
    int w, h;

    if (!screen_buf || hStdOut == INVALID_HANDLE_VALUE) return;

    GetConsoleScreenBufferInfo(hStdOut, &csbi);
    w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;

    writeRegion.Left   = csbi.srWindow.Left;
    writeRegion.Top    = csbi.srWindow.Top;
    writeRegion.Right  = (SHORT)(csbi.srWindow.Left + w - 1);
    writeRegion.Bottom = (SHORT)(csbi.srWindow.Top + h - 1);
    bufSize.X = (SHORT)w;
    bufSize.Y = (SHORT)h;
    bufCoord.X = 0;
    bufCoord.Y = 0;

    WriteConsoleOutput(hStdOut, screen_buf, bufSize, bufCoord, &writeRegion);

    /* Position cursor */
    cursorPos.X = (SHORT)(csbi.srWindow.Left + cur_x);
    cursorPos.Y = (SHORT)(csbi.srWindow.Top + cur_y);
    SetConsoleCursorPosition(hStdOut, cursorPos);
}

void clear(void) {
    int cells;
    int i;
    WORD attr;
    if (!screen_buf) return;
    cells = scr_width * scr_height;
    attr = make_attr(cur_attr);
    for (i = 0; i < cells; i++) {
        screen_buf[i].Char.AsciiChar = ' ';
        screen_buf[i].Attributes = attr;
    }
    cur_x = 0;
    cur_y = 0;
}

void clrtoeol(void) {
    int x;
    int idx;
    WORD attr;
    if (!screen_buf) return;
    attr = make_attr(cur_attr);
    for (x = cur_x; x < scr_width; x++) {
        idx = cur_y * scr_width + x;
        screen_buf[idx].Char.AsciiChar = ' ';
        screen_buf[idx].Attributes = attr;
    }
}

static int translate_key(INPUT_RECORD *rec) {
    WORD vk;
    DWORD ctrl;
    vk = rec->Event.KeyEvent.wVirtualKeyCode;
    ctrl = rec->Event.KeyEvent.dwControlKeyState;

    /* Function keys (Shift+F12 = turbo mode) */
    if (vk >= VK_F1 && vk <= VK_F12) {
        if (vk == VK_F12 && (GetAsyncKeyState(VK_SHIFT) & 0x8000))
            return KEY_SF12;
        return KEY_F1 + (vk - VK_F1);
    }

    /* Special keys (Ctrl-modified versions first) */
    if (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        switch (vk) {
            case VK_LEFT:  return KEY_CLEFT;
            case VK_RIGHT: return KEY_CRIGHT;
            case VK_HOME:  return KEY_CHOME;
            case VK_END:   return KEY_CEND;
        }
    }
    switch (vk) {
        case VK_DOWN:    return KEY_DOWN;
        case VK_UP:      return KEY_UP;
        case VK_LEFT:    return KEY_LEFT;
        case VK_RIGHT:   return KEY_RIGHT;
        case VK_HOME:    return KEY_HOME;
        case VK_END:     return KEY_END;
        case VK_PRIOR:   return KEY_PPAGE;  /* PgUp */
        case VK_NEXT:    return KEY_NPAGE;  /* PgDn */
        case VK_INSERT:  return KEY_IC;
        case VK_DELETE:  return KEY_DC;
        case VK_RETURN:  return KEY_ENTER;
        case VK_BACK:    return KEY_BACKSPACE;
        case VK_ESCAPE:  return 27;
        case VK_TAB:
            if (ctrl & SHIFT_PRESSED) return KEY_BTAB;
            return '\t';
    }

    /* Ctrl+key */
    if (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        if (vk == 'L') return 12; /* Ctrl+L */
        if (vk == 'K') return 11; /* Ctrl+K */
        if (vk == 'G') return 7;  /* Ctrl+G */
        if (vk == 'U') return 21; /* Ctrl+U */
        if (vk == 'Y') return 25; /* Ctrl+Y */
        /* Return ASCII control code */
        if (vk >= 'A' && vk <= 'Z') return vk - 'A' + 1;
        if (vk >= 'a' && vk <= 'z') return vk - 'a' + 1;
    }

    /* Regular character */
    if (rec->Event.KeyEvent.uChar.AsciiChar != 0)
        return rec->Event.KeyEvent.uChar.AsciiChar;

    return 0; /* Unknown */
}

/* getch() dispatches based on init mode:
   - initscr() mode: uses ReadConsoleInput + translate_key (for KFA/TUI)
   - conio_init() console mode: uses ReadConsoleInput + translate_key (for cccc)
   - conio_init() pipe mode: uses ReadFile + VT100 escape parsing (for mintty/pipes) */

int getch(void) {
    if (conio_initialized && !screen_buf) {
        if (conio_pipe_mode) {
            /* Pipe/mintty: ReadFile + VT100 parsing */
            return vt100_getch();
        } else {
            /* Console: ReadConsoleInput + translate_key (same as KFA) */
            INPUT_RECORD rec;
            DWORD count;
            int key;
            for (;;) {
                ReadConsoleInput(hStdIn, &rec, 1, &count);
                if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
                    ensure_buf();
                    return KEY_RESIZE;
                }
                if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
                    key = translate_key(&rec);
                    if (key != 0) return key;
                }
            }
        }
    }
    /* Curses mode (initscr): ReadConsoleInput + translate_key */
    INPUT_RECORD rec;
    DWORD count;
    int key;

    for (;;) {
        if (nodelay_mode) {
            if (!PeekConsoleInput(hStdIn, &rec, 1, &count) || count == 0)
                return -1;
            ReadConsoleInput(hStdIn, &rec, 1, &count);
        } else {
            ReadConsoleInput(hStdIn, &rec, 1, &count);
        }

        if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            ensure_buf();
            return KEY_RESIZE;
        }
        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
            key = translate_key(&rec);
            if (key != 0) return key;
        }
    }
}

/* Force screen buffer resize (call after terminal resize detected) */
void conio_resize(void) {
    ensure_buf();
}

int mvaddstr(int y, int x, const char* str) {
    int i;
    int idx;
    WORD attr;
    if (!screen_buf) return -1;
    cur_y = y;
    cur_x = x;
    attr = make_attr(cur_attr);
    for (i = 0; str[i]; i++) {
        if (cur_y >= 0 && cur_y < scr_height && cur_x >= 0 && cur_x < scr_width) {
            idx = cur_y * scr_width + cur_x;
            if (str[i] == '\n' || str[i] == '\r') {
                cur_x = 0;
                cur_y++;
            } else {
                screen_buf[idx].Char.AsciiChar = str[i];
                screen_buf[idx].Attributes = attr;
                cur_x++;
            }
        }
    }
    return 0;
}

int addstr(const char* str) {
    return mvaddstr(cur_y, cur_x, str);
}

int mvaddch(int y, int x, char ch) {
    int idx;
    if (!screen_buf) return -1;
    cur_y = y;
    cur_x = x;
    if (cur_y >= 0 && cur_y < scr_height && cur_x >= 0 && cur_x < scr_width) {
        idx = cur_y * scr_width + cur_x;
        screen_buf[idx].Char.AsciiChar = ch;
        screen_buf[idx].Attributes = make_attr(cur_attr);
    }
    return 0;
}

int addch(char ch) {
    return mvaddch(cur_y, cur_x, ch);
}

int mvaddwch(int y, int x, wchar_t wch) {
    int idx;
    if (!screen_buf) return -1;
    cur_y = y;
    cur_x = x;
    if (cur_y >= 0 && cur_y < scr_height && cur_x >= 0 && cur_x < scr_width) {
        idx = cur_y * scr_width + cur_x;
        screen_buf[idx].Char.UnicodeChar = wch;
        screen_buf[idx].Attributes = make_attr(cur_attr);
    }
    return 0;
}

int addwch(wchar_t wch) {
    return mvaddwch(cur_y, cur_x, wch);
}

int mvaddwstr(int y, int x, const wchar_t* wstr) {
    int idx, i;
    if (!screen_buf) return -1;
    unsigned short attr = make_attr(cur_attr);
    for (i = 0; wstr[i] != L'\0'; i++) {
        if (y >= 0 && y < scr_height && x + i >= 0 && x + i < scr_width) {
            idx = y * scr_width + (x + i);
            screen_buf[idx].Char.UnicodeChar = wstr[i];
            screen_buf[idx].Attributes = attr;
        }
    }
    cur_y = y;
    cur_x = x + i;
    return 0;
}

void move(int y, int x) {
    cur_y = y;
    cur_x = x;
}

void attrset(unsigned int attr) {
    cur_attr = attr;
}

void attron(unsigned int attr) {
    cur_attr |= attr;
}

void attroff(unsigned int attr) {
    cur_attr &= ~attr;
}

int has_colors(void) {
    return 1;
}

void start_color(void) {
    int i;
    color_pairs_initialized = 1;
    /* Set up default pairs */
    for (i = 0; i < 64; i++)
        color_pairs[i] = 0x07; /* white on black */
    /* Pair 0 is always white on black */
    color_pairs[0] = 0x07;
}

void init_pair(int pair, int fg, int bg) {
    if (pair < 0 || pair >= 64) return;
    color_pairs[pair] = color_map(fg) | (color_map(bg) << 4);
}

int getmaxy(void) { return scr_height; }
int getmaxx(void) { return scr_width; }
int getcury(void) { return cur_y; }
int getcurx(void) { return cur_x; }

void curs_set(int visibility) {
    CONSOLE_CURSOR_INFO cci;
    if (hStdOut == INVALID_HANDLE_VALUE) return;
    GetConsoleCursorInfo(hStdOut, &cci);
    cci.bVisible = (visibility > 0) ? TRUE : FALSE;
    if (visibility == 2) cci.dwSize = 100; /* block cursor */
    else cci.dwSize = 25; /* line cursor */
    SetConsoleCursorInfo(hStdOut, &cci);
}

/* Lightweight init for printf-based apps (cccc-style) */
/* Enables VT100 output + VT100 input (escape sequences for all keys) */
/* Always uses VT100 escape sequence parsing — works on consoles, mintty, pipes */

/* Read one byte from stdin. Returns -1 on error. */
static int read_byte(void) {
    unsigned char ch;
    DWORD n;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (!ReadFile(h, &ch, 1, &n, NULL) || n == 0) return -1;
    return ch;
}

/* Check if more bytes are available on stdin (with brief wait) */
static int peek_byte(int wait_ms) {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD avail = 0;
    if (conio_is_console) {
        /* Console: use WaitForSingleObject to check for available input */
        DWORD result = WaitForSingleObject(h, wait_ms > 0 ? wait_ms : 0);
        return (result == WAIT_OBJECT_0) ? 1 : 0;
    }
    /* Pipe: poll with PeekNamedPipe, retry every 5ms within timeout */
    int elapsed = 0;
    while (elapsed <= wait_ms) {
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) return 0;
        if (avail > 0) return 1;
        if (wait_ms <= 0) return 0;
        Sleep(5);
        elapsed += 5;
    }
    return 0;
}

/* VT100 escape sequence parser — works for both console VT input and pipes */
static int vt100_getch(void) {
    int ch = read_byte();
    if (ch < 0) return -1;
    if (ch != 27) return ch; /* not ESC */

    /* ESC received — try to read escape sequence */
    unsigned char seq[8] = {0};
    int seq_len = 0;
    while (seq_len < 7) {
        if (!peek_byte(seq_len == 0 ? 50 : 50)) break;
        int c = read_byte();
        if (c < 0) break;
        seq[seq_len++] = (unsigned char)c;
    }

    /* CSI sequence: ESC [ <final> or ESC [ <params> <final> */
    if (seq_len >= 2 && seq[0] == '[') {
        /* Handle ESC [ <digits> ~ format (Home/End/PgUp/PgDn/Ins/Del) */
        if (seq_len >= 3 && seq[2] == '~') {
            switch (seq[1]) {
                case '1': return KEY_HOME;
                case '2': return KEY_IC;   /* Insert */
                case '3': return KEY_DC;   /* Delete */
                case '4': return KEY_END;
                case '5': return KEY_PPAGE; /* PgUp */
                case '6': return KEY_NPAGE; /* PgDn */
            }
        }
        /* Handle ESC [ <digit> ; <digit> <final> (modified keys) */
        /* Handle ESC [ <final> (simple arrows, home, end) */
        int final_idx = seq_len - 1;
        /* Skip past any digits and semicolons to find the final letter */
        while (final_idx > 0 && seq[final_idx] >= '0' && seq[final_idx] <= '9') final_idx--;
        switch (seq[final_idx]) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
        }
    }
    /* SS3 sequence: ESC O P/Q/R/S = F1-F4 */
    if (seq_len >= 2 && seq[0] == 'O') {
        switch (seq[1]) {
            case 'P': return KEY_F1;
            case 'Q': return KEY_F2;
            case 'R': return KEY_F3;
            case 'S': return KEY_F4;
        }
    }

    if (seq_len == 0) return 27; /* lone ESC — no sequence bytes followed */
    return 0; /* unrecognized escape — swallow (bytes consumed, don't return ESC) */
}

int conio_init(void) {
    hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    hStdIn  = GetStdHandle(STD_INPUT_HANDLE);

    if (hStdOut == INVALID_HANDLE_VALUE || hStdIn == INVALID_HANDLE_VALUE)
        return -1;

    /* Check if input is a console or pipe */
    DWORD mode;
    conio_is_console = GetConsoleMode(hStdIn, &mode) ? 1 : 0;
    conio_orig_in_mode = mode;

    if (conio_is_console) {
        /* Console: use ReadConsoleInput + translate_key for key reading.
         * This is reliable on all Windows versions — no VT input mode needed. */
        SetConsoleMode(hStdIn, ENABLE_WINDOW_INPUT);
        conio_pipe_mode = 0;
    } else {
        /* Pipe/mintty: use ReadFile + VT100 escape parsing */
        conio_pipe_mode = 1;
    }

    /* Enable VT100 output processing */
    if (GetConsoleMode(hStdOut, &conio_orig_out_mode)) {
        SetConsoleMode(hStdOut, conio_orig_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    conio_initialized = 1;
    return 0;
}

void conio_cleanup(void) {
    if (!conio_initialized) return;
    if (conio_is_console) {
        SetConsoleMode(hStdIn, conio_orig_in_mode);
    }
    /* Restore VT output mode on console */
    DWORD out_mode;
    if (GetConsoleMode(hStdOut, &out_mode)) {
        SetConsoleMode(hStdOut, conio_orig_out_mode);
    }
    conio_initialized = 0;
}

int conio_is_console_flag(void) { return conio_is_console; }
int conio_pipe_mode_flag(void) { return conio_pipe_mode; }

void nodelay(int mode) {
    nodelay_mode = mode;
}

void keypad(void* d, int n) { (void)d; (void)n; }

/* Lightweight getch — VT100 parsing is in the shared getch() above */

/* This is the POSIX section replacement for conio.c */

#else /* !_WIN32 — POSIX implementation */

#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>

static struct termios orig_termios;
static int orig_flags = -1; /* saved stdin flags for O_NONBLOCK */
static int conio_initialized = 0;
static int unget_ch = -1; /* pushed-back character for getch */

/* Sleep for N milliseconds using select() (portable, no usleep needed) */
static void msleep(int ms) {
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    select(0, NULL, NULL, NULL, &tv);
}

int conio_init(void) {
    struct termios raw;
    unget_ch = -1;
    if (!isatty(STDIN_FILENO)) return 0; /* pipe mode — skip */
    tcgetattr(STDIN_FILENO, &orig_termios);
    raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    orig_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    conio_initialized = 1;
    return 0;
}

void conio_cleanup(void) {
    if (conio_initialized) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        if (orig_flags >= 0) fcntl(STDIN_FILENO, F_SETFL, orig_flags);
        conio_initialized = 0;
    }
}

/* Read one byte from stdin with optional non-blocking attempt.
 * If nonblock is true, returns -1 immediately if no data (EAGAIN). */
static int read_byte(int nonblock) {
    unsigned char ch;
    int n;
    if (nonblock) {
        /* Set stdin non-blocking, try to read, restore */
        fcntl(STDIN_FILENO, F_SETFL, orig_flags | O_NONBLOCK);
        n = read(STDIN_FILENO, &ch, 1);
        fcntl(STDIN_FILENO, F_SETFL, orig_flags);
        if (n < 1) return -1; /* EAGAIN or error */
    } else {
        n = read(STDIN_FILENO, &ch, 1);
        if (n <= 0) return -1;
    }
    return ch;
}

int getch(void) {
    int ch;

    /* Return pushed-back character first */
    if (unget_ch >= 0) {
        ch = unget_ch;
        unget_ch = -1;
        return ch;
    }

    ch = read_byte(0); /* blocking read */
    if (ch < 0) return -1;
    if (ch != 27) return ch; /* not escape */

    /* ESC received — check if more bytes follow (escape sequence).
     * Arrow/function keys send multi-byte sequences instantly,
     * so the next byte should be available immediately.
     * Use non-blocking read to check without waiting. */
    int first = read_byte(1); /* non-blocking */
    if (first < 0) {
        /* No byte immediately after ESC.
         * Could be a lone ESC key — wait briefly and check again
         * in case bytes arrive slowly (e.g. network terminal). */
        msleep(5);
        first = read_byte(1); /* non-blocking */
        if (first < 0) return 27; /* lone ESC */
    }

    /* Not a known sequence starter — return ESC, push back byte */
    if (first != '[' && first != 'O') {
        unget_ch = first;
        return 27;
    }

    /* Read remaining sequence bytes (non-blocking, they arrive together) */
    unsigned char seq[8];
    seq[0] = (unsigned char)first;
    int seq_len = 1;
    while (seq_len < 7) {
        int b = read_byte(1); /* non-blocking */
        if (b < 0) {
            /* Give a tiny delay for slow terminals, try once more */
            if (seq_len < 2) msleep(2);
            b = read_byte(1);
            if (b < 0) break;
        }
        seq[seq_len++] = (unsigned char)b;
    }

    /* CSI sequence: ESC [ A/B/C/D/H/F etc */
    if (seq_len >= 2 && seq[0] == '[') {
        int final_idx = seq_len - 1;
        while (final_idx > 0 && ((seq[final_idx] >= '0' && seq[final_idx] <= '9') || seq[final_idx] == ';'))
            final_idx--;
        switch (seq[final_idx]) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
        }
        if (seq_len >= 3 && seq[2] == '~') {
            switch (seq[1]) {
                case '1': return KEY_HOME;
                case '2': return KEY_IC;
                case '3': return KEY_DC;
                case '4': return KEY_END;
                case '5': return KEY_PPAGE;
                case '6': return KEY_NPAGE;
            }
        }
    }
    /* SS3 sequence: ESC O A/B/C/D/P/Q/R/S/H/F */
    if (seq_len >= 2 && seq[0] == 'O') {
        switch (seq[1]) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            case 'P': return KEY_F1;
            case 'Q': return KEY_F2;
            case 'R': return KEY_F3;
            case 'S': return KEY_F4;
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
        }
    }

    /* Unrecognized sequence — return ESC, push back last byte */
    unget_ch = seq[seq_len - 1];
    return 27;
}

void nodelay(int mode) {
    /* For now, no-op on POSIX. cccc uses blocking getch only. */
    (void)mode;
}

void keypad(void* d, int n) { (void)d; (void)n; }

int conio_is_console_flag(void) { return 0; /* POSIX: never a Windows console */ }
int conio_pipe_mode_flag(void) { return 0; }

/* Wide character stubs for POSIX — output UTF-8 directly */
int mvaddwch(int y, int x, wchar_t wch) {
    (void)y; (void)x; (void)wch;
    return -1; /* not supported in POSIX lightweight mode */
}
int addwch(wchar_t wch) { (void)wch; return -1; }
int mvaddwstr(int y, int x, const wchar_t *wstr) {
    (void)y; (void)x; (void)wstr;
    return -1; /* not supported in POSIX lightweight mode */
}

#endif /* _WIN32 */