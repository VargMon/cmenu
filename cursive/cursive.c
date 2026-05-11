// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright (C) 2013 Bryan Christ <bryan.christ@mediafire.com>
// 2007 Bryan Christ <bryan.christ@hp.com>
// 2014 Johannes Schauer <j.schauer@email.de>
// 2026 James Smith <vargmon98@gmail.com>
//
// list_for_each_safe:
// copyright (C) 2005 Kulesh Shanmugasundaram <kulesh@isis.poly.edu>
// copied Sept 23, 2017 by Bryan Christ <bryan.christ@gmail.com>

#define _POSIX_C_SOURCE 200809L /* for vsnprintf */

#include <assert.h>
#include <fcntl.h>
#include <gpm.h>
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <ncurses.h>             /* Required for KEY_MOUSE, BUTTON1_PRESSED, etc. */
#include <ncursesw/curses.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define NCURSES_OPAQUE 0

/* ============================================================================
 * Utility functions for dynamic string creation and vector manipulation.
 * ============================================================================
 */

/**
 * Allocate and return a new, null-terminated string formatted
 * with printf-style specifiers.
 *
 * @param fmt Format string.
 * @param ... Arguments for format specifiers.
 * @return Pointer to new string, NULL if allocation fails.
 *   Caller must free with free().
 */
char *strdup_printf(const char *fmt, ...) {
    va_list args;
    int needed;
    char *buf;

    va_start(args, fmt);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) {
        return NULL;
    }
    needed += 1; /* space for null terminator */

    buf = (char *)malloc((size_t)needed);
    if (!buf) {
        fprintf(stderr, "strdup_printf: allocation failed\n");
        return NULL;
    }

    va_start(args, fmt);
    vsnprintf(buf, (size_t)needed, fmt, args);
    va_end(args);
    return buf;
}

/**
 * Split a C-string by substring delimiter into a NULL-terminated array.
 * Caller must free the result with strfreev().
 *
 * @param input Source string.
 * @param delim Substring delimiter.
 * @return NULL-terminated array of strings, NULL if allocation fails.
 */
char **strsplitv(const char *input, const char *delim) {
    if (!input || !delim) return NULL;
    size_t delim_len = strlen(delim);
    if (delim_len == 0) return NULL;

    size_t count = 1;
    const char *p = input;
    while ((p = strstr(p, delim)) != NULL) {
        count++;
        p += delim_len;
    }

    char **tokens = calloc(count + 1, sizeof(char *));
    if (!tokens) return NULL;

    size_t idx = 0;
    const char *start = input;
    while ((p = strstr(start, delim)) != NULL) {
        size_t len = (size_t)(p - start);
        tokens[idx] = strndup(start, len);
        if (!tokens[idx]) {
            for (size_t j = 0; j < idx; j++)
                free(tokens[j]);
            free(tokens);
            return NULL;
        }
        idx++;
        start = p + delim_len;
    }
    tokens[idx] = strdup(start);
    if (!tokens[idx]) {
        for (size_t j = 0; j < idx; j++)
            free(tokens[j]);
        free(tokens);
        return NULL;
    }
    return tokens;
}

/**
 * Duplicate a NULL-terminated array of strings.
 * Caller must free with strfreev()
 *
 * @param array Source array.
 * @return NULL-terminated duplicate array, NULL if allocation fails.
 */
char **strdupv(char **array) {
    if (!array) return NULL;
    size_t count = 0;
    while (array[count]) count++;

    char **copy = calloc(count + 1, sizeof(char *));
    if (!copy) return NULL;

    for (size_t i = 0; i < count; i++) {
        copy[i] = strdup(array[i]);
        if (!copy[i]) {
            for (size_t j = 0; j < i; j++) free(copy[j]);
            free(copy);
            return NULL;
        }
    }
    return copy;
}

/**
 * Free a NULL-terminated array of strings.
 *
 * @param array Source array.
 */
void strfreev(char **array) {
    if (!array) return;
    for (char **p = array; *p; p++) free(*p);
    free(array);
}

/* ============================================================================
 * Doubly Linked List API for C
 * ============================================================================
 */

struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *head) {
    head->next = head;
    head->prev = head;
}

static inline void __list_add(struct list_head *node, struct list_head *prev, struct list_head *next) {
    next->prev = node;
    node->next = next;
    node->prev = prev;
    prev->next = node;
}

static inline void list_add(struct list_head *node, struct list_head *head) {
    __list_add(node, head, head->next);
}

static inline void list_add_tail(struct list_head *node, struct list_head *head) {
    __list_add(node, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next) {
    next->prev = prev;
    prev->next = next;
}

static inline void list_del(struct list_head *entry) {
    __list_del(entry->prev, entry->next);
    entry->next = NULL;
    entry->prev = NULL;
}

static inline void list_del_init(struct list_head *entry) {
    __list_del(entry->prev, entry->next);
    INIT_LIST_HEAD(entry);
}

static inline void list_move(struct list_head *entry, struct list_head *head) {
    __list_del(entry->prev, entry->next);
    list_add(entry, head);
}

static inline void list_move_tail(struct list_head *entry, struct list_head *head) {
    __list_del(entry->prev, entry->next);
    list_add_tail(entry, head);
}

static inline int list_empty(const struct list_head *head) {
    return head->next == head;
}

static inline int list_is_first(const struct list_head *e, const struct list_head *head) {
    return e->prev == head;
}

static inline int list_is_last(const struct list_head *e, const struct list_head *head) {
    return e->next == head;
}

static inline int list_is_singular(const struct list_head *head) {
    return !list_empty(head) && (head->next == head->prev);
}

static inline void list_rotate_left(struct list_head *head) {
    if (list_empty(head) || list_is_singular(head)) return;
    list_move_tail(head->next, head);
}

static inline void list_rotate_right(struct list_head *head) {
    if (list_empty(head) || list_is_singular(head)) return;
    list_move(head->prev, head);
}

static inline void __list_splice(struct list_head *list, struct list_head *head) {
    struct list_head *first = list->next;
    struct list_head *last = list->prev;
    struct list_head *at = head->next;
    first->prev = head;
    head->next = first;
    last->next = at;
    at->prev = last;
}

static inline void list_splice(struct list_head *list, struct list_head *head) {
    if (!list_empty(list)) __list_splice(list, head);
}

static inline void list_splice_init(struct list_head *list, struct list_head *head) {
    if (!list_empty(list)) {
        __list_splice(list, head);
        INIT_LIST_HEAD(list);
    }
}

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif

#define list_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define list_first_entry(head, type, member) \
    list_entry((head)->next, type, member)
#define list_last_entry(head, type, member) \
    list_entry((head)->prev, type, member)

#define list_next_entry(pos, member) \
    list_entry((pos)->member.next, typeof(*(pos)), member)
#define list_prev_entry(pos, member) \
    list_entry((pos)->member.prev, typeof(*(pos)), member)

#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)
#define list_for_each_prev(pos, head) \
    for (pos = (head)->prev; pos != (head); pos = pos->prev)
#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)
#define list_for_each_prev_safe(pos, n, head) \
    for (pos = (head)->prev, n = pos->prev; pos != (head); pos = n, n = pos->prev)

#define list_for_each_entry(pos, head, member) \
    for (pos = list_entry((head)->next, typeof(*pos), member); \
         &pos->member != (head); \
         pos = list_entry(pos->member.next, typeof(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member) \
    for (pos = list_entry((head)->next, typeof(*pos), member), \
         n = list_entry(pos->member.next, typeof(*pos), member); \
         &pos->member != (head); \
         pos = n, n = list_entry(n->member.next, typeof(*n), member))

#define list_for_each_entry_from(pos, head, member) \
    for (; &pos->member != (head); \
         pos = list_next_entry(pos, member))
#define list_for_each_entry_from_reverse(pos, head, member) \
    for (; &pos->member != (head); \
         pos = list_prev_entry(pos, member))

/* ============================================================================
 * Core definitions, types, macros, and function prototypes.
 * ============================================================================
 */

/* -- Mapping tables generated from cursive_gpm.def -- */
#define X_GPM(a,b,c,d) a,
static const uint16_t x_gpm_mode[] = { #include "cursive_gpm.def" };
#undef X_GPM
#define X_GPM(a,b,c,d) b,
static const mmask_t x_ncurses_state[] = { #include "cursive_gpm.def" };
#undef X_GPM
#define X_GPM(a,b,c,d) c,
static const short x_gpm_button[] = { #include "cursive_gpm.def" };
#undef X_GPM
#define X_GPM(a,b,c,d) d,
static const unsigned short x_gpm_event[] = { #include "cursive_gpm.def" };
#undef X_GPM

#define LIBCURSIVE_VERSION "0.0.1"

/* Utility macros */
#define ABSINT(x) ((x ^ (x >> ((sizeof(x) * 8) - 1))) - (x >> ((sizeof(x) * 8) - 1)))
#define ARRAY_SZ(x) (sizeof(x) / sizeof(x[0]))

#define MAX_SCREENS 4
#define CURSIVE_FASTCOLOR (1 << 1)
#define CURSIVE_GPM_SIGIO (1 << 2)
#define VECTOR_TOP_TO_BOTTOM 1
#define VECTOR_BOTTOM_TO_TOP -1
#define VECTOR_LEFT 1
#define VECTOR_RIGHT -1

#define WSIZE_MIN -1
#define WSIZE_DEFAULT (WSIZE_MIN)
#define WSIZE_UNCHANGED -2
#define WSIZE_MAX -3
#define WSIZE_FULLSCREEN (WSIZE_MAX)
#define WPOS_UNCHANGED -1
#define WPOS_STAGGERED -2
#define WPOS_DEFAULT (WPOS_STAGGERED)
#define WPOS_CENTERED -3

#define KMIO_HANDLED 0
#define KMIO_ERROR   -1
#define KMIO_NONE    -1

/* Cursor Position Masks */
#define CURS_RIGHT     0x1U
#define CURS_LEFT      0x2U
#define CURS_TOP       0x4U
#define CURS_BOTTOM    0x8U
#define CURS_EDGE      0xFU
#define CURS_LOWER_RIGHT (CURS_RIGHT | CURS_BOTTOM)
#define CURS_UPPER_RIGHT (CURS_RIGHT | CURS_TOP)
#define CURS_LOWER_LEFT (CURS_LEFT | CURS_BOTTOM)
#define CURS_UPPER_LEFT (CURS_LEFT | CURS_TOP)

/* Window States */
#define STATE_VISIBLE   (1UL << 1)
#define STATE_FOCUS     (1UL << 2)
#define STATE_FROZEN    (1UL << 3)
#define STATE_SHADOWED  (1UL << 5)
#define STATE_NORESIZE  (1UL << 7)

/* Msgbox Flags */
#define MSGBOX_ICON_INFO     (1UL << 1)
#define MSGBOX_ICON_WARN     (1UL << 2)
#define MSGBOX_ICON_ERROR    (1UL << 3)
#define MSGBOX_ICON_QUESTION (1UL << 4)
#define MSGBOX_TYPE_OK       (1UL << 10)
#define MSGBOX_TYPE_YESNO    (1UL << 11)

/* Redraw Mask Flags */
#define REDRAW_MOUSE     (1 << 1)
#define REDRAW_WINDOWS   (1 << 2)
#define REDRAW_WORKSPACE (1 << 4)
#define REDRAW_BACKGROUND (1 << 5)
#define REDRAW_ALL (REDRAW_MOUSE | REDRAW_WINDOWS | REDRAW_WORKSPACE)

/* Standard Keystrokes */
#ifndef KEY_TAB
# define KEY_TAB 9
#endif
#ifndef KEY_CRLF
# define KEY_CRLF 10
#endif

/* ============================================================================
 * Context, Screen, Window, and Event Structs
 * ============================================================================
 */

struct _cursive_ctx_s {
    int screen_id;
    bool managed;
};

struct _cursive_screen_s {
    WINDOW *screen;
    struct list_head managed_list;
    struct list_head unmanaged_list;
    WINDOW *wallpaper;
    CursiveBkgdFunc wallpaper_agent;
    uint32_t states;
};

struct _cursive_event_s {
    struct list_head list;
    char *event;
    CursiveFunc func;
    void *arg;
};

struct _cursive_window_s {
    WINDOW *user_window;
    WINDOW *window_frame;
    cursive_ctx_t *ctx;
    const char *title;
    struct list_head list;
    int min_width, min_height, max_width, max_height;
    uint32_t window_state;
    struct list_head event_list;
    CursiveWkeyFunc key_func;
    CursiveFunc border_agent[2];
    void *userptr;
    void *classid;
};

struct _cursive_s {
    int cur_scr_id;
    struct _cursive_screen_s cursive_screen[MAX_SCREENS];
    struct list_head zombie_list;
    WINDOW *console_mouse;
    void *wallpaper_arg;
    CursiveFunc border_agent[2];
    CursiveKmioHook kmio_dispatch_hook[2];
    int8_t xterm;
    uid_t user;
};

/* Typedefs for API */
typedef struct _cursive_s CURSIVE;
typedef struct _cursive_s cursive_t;
typedef struct _cursive_ctx_s CURSIVE_CTX;
typedef struct _cursive_ctx_s cursive_ctx_t;
typedef struct _cursive_screen_s CURSIVE_SCREEN;
typedef struct _cursive_screen_s cursive_screen_t;
typedef struct _cursive_window_s CURSIVE_WINDOW;
typedef struct _cursive_window_s cursive_window_t;
typedef struct _cursive_event_s CURSIVE_EVENT;
typedef struct _cursive_event_s cursive_event_t;

/* Mark a parameter as intentionally unused */
#define UNUSED(x) (void)(x)

/* Event breadcast */
#define CURSIVE_EVENT_BROADCAST	((cursive_window_t*)"ALL_CURSIVE_WINDOWS")

/* ============================================================================
 * Internal Helper Function Declarations
 * ============================================================================
 */
static void disable_echo(void);
static void enable_echo(void);
static void configure_curses(void);
static void configure_termios(void);
static void initialize_screens(CURSIVE *ctx, int height, int width);
static int32_t readEscapeSequence(void);

/* ============================================================================
 * Internal Helper Implementations
 * ============================================================================
 */

static void initialize_screens(CURSIVE *ctx, int height, int width) {
    for (int i = 0; i < MAX_SCREENS; ++i) {
        cursive_screen_t *cs = &ctx->cursive_screen[i];
        INIT_LIST_HEAD(&cs->managed_list);
        INIT_LIST_HEAD(&cs->unmanaged_list);
        cs->wallpaper = newwin(height, width, 0, 0);
        cs->wallpaper_agent = cursive_default_wallpaper_agent;
    }
}

static void configure_curses(void) {
    keypad(SCREEN_WINDOW, TRUE);
    nodelay(SCREEN_WINDOW, TRUE);
    scrollok(SCREEN_WINDOW, FALSE);
    noecho();
    raw();
    intrflush(NULL, TRUE);
    curs_set(0);
}

static void configure_termios(void) {
    disable_echo();
}

static void disable_echo(void) {
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) == 0) {
        t.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSADRAIN, &t);
    }
}

static void enable_echo(void) {
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) == 0) {
        t.c_lflag |= ECHO;
        tcsetattr(STDIN_FILENO, TCSADRAIN, &t);
    }
}

/* ============================================================================
 * Callback Function Typedefs
 * ============================================================================
 */
typedef int (*CursiveFunc)(CURSIVE_WINDOW *cwnd, void *arg);
typedef int (*CursiveKeyFunc)(int32_t keystroke, void *data);
typedef int (*CursiveWkeyFunc)(int32_t keystroke, CURSIVE_WINDOW *cursive_window);
typedef int32_t (*CursiveKmioHook)(int32_t keystroke);
typedef void (*CursiveBkgdFunc)(int screen_id);

/* Local static context pointer */
static CURSIVE *cursive = NULL;
static WINDOW *SCREEN_WINDOW = NULL;
static uint32_t cursive_global_flags = 0;

/* ============================================================================
 * Helper API for ncurses window geometry and general window helpers
 * ============================================================================
 */

static inline WINDOW* win_or_default(WINDOW *win) {
    return (win != NULL) ? win : SCREEN_WINDOW;
}

static inline void get_win_geom(WINDOW *win, int *rows, int *cols, int *beg_y, int *beg_x) {
    if (rows && cols) getmaxyx(win, *rows, *cols);
    if (beg_y && beg_x) getbegyx(win, *beg_y, *beg_x);
}

static inline void get_screen_size(int *rows, int *cols) {
    getmaxyx(stdscr, *rows, *cols);
}

/* ============================================================================
 * Cursor edge detection API
 * ============================================================================
 */

/* Return edge mask for cursor position within window */
static uint16_t get_window_cursor_edges(WINDOW *win, int *out_y, int *out_x) {
    int y, x;
    getyx(win, y, x);
    uint16_t mask = 0;
    if (x == 0)                   mask |= CURS_LEFT;
    if (x == getmaxx(win) - 1)    mask |= CURS_RIGHT;
    if (y == 0)                   mask |= CURS_TOP;
    if (y == getmaxy(win) - 1)    mask |= CURS_BOTTOM;
    if (out_y) *out_y = y;
    if (out_x) *out_x = x;
    return mask;
}

/**
 * Returns -1 if not on any edge; 0 if any edge (mask==CURS_EDGE) or
 * exact corner match; row (y) if left/right edge; column (x) if top/bottom edge.
 */
int is_cursor_at(WINDOW *window, uint16_t mask) {
    WINDOW *win = win_or_default(window);
    int y, x;
    uint16_t edges = get_window_cursor_edges(win, &y, &x);
    if (!edges) return -1;
    if (mask == CURS_EDGE) return 0;
    uint16_t horiz = CURS_LEFT | CURS_RIGHT;
    uint16_t vert = CURS_TOP | CURS_BOTTOM;
    if ((mask & horiz) && (edges & mask)) return y;
    if ((mask & vert) && (edges & mask)) return x;
    if (edges == mask) return 0;
    return -1;
}

#define is_curs_at_left(x) is_cursor_at(x, CURS_LEFT)
#define is_curs_at_right(x) is_cursor_at(x, CURS_RIGHT)
#define is_curs_at_top(x) is_cursor_at(x, CURS_TOP)
#define is_curs_at_bottom(x) is_cursor_at(x, CURS_BOTTOM)
#define is_curs_at_edge(x) is_cursor_at(x, CURS_EDGE)
#define is_curs_at_upper_left(x) is_cursor_at(x, CURS_UPPER_LEFT)
#define is_curs_at_lower_left(x) is_cursor_at(x, CURS_LOWER_LEFT)
#define is_curs_at_upper_right(x) is_cursor_at(x, CURS_UPPER_RIGHT)
#define is_curs_at_lower_right(x) is_cursor_at(x, CURS_LOWER_RIGHT)

/* ============================================================================
 * Window geometry helpers
 * ============================================================================
 */

/* Compute center relative to window origin */
void window_get_center(WINDOW *window, int *x, int *y) {
    WINDOW *win = win_or_default(window);
    int rows, cols, beg_y, beg_x;
    get_win_geom(win, &rows, &cols, &beg_y, &beg_x);
    if (x) *x = (cols - beg_x) / 2;
    if (y) *y = (rows - beg_y) / 2;
}

/* Overflow checks */
static int window_check_overflow(WINDOW *window, bool check_width) {
    if (!window) return -1;
    int scr_rows, scr_cols, dummy;
    get_win_geom(SCREEN_WINDOW, &scr_rows, &scr_cols, &dummy, &dummy);
    int rows, cols, beg_y, beg_x;
    get_win_geom(window, &rows, &cols, &beg_y, &beg_x);
    int overflow = check_width ? (beg_x + cols) - scr_cols : (beg_y + rows) - scr_rows;
    return (overflow > 0) ? overflow : 0;
}

int window_check_width(WINDOW *window) { return window_check_overflow(window, true); }
int window_check_height(WINDOW *window) { return window_check_overflow(window, false); }

/* Calculate scaled dimensions */
void window_get_size_scaled(WINDOW *reference, int *width, int *height, float hscale, float vscale) {
    WINDOW *win = win_or_default(reference);
    int rows, cols;
    get_win_geom(win, &rows, &cols, NULL, NULL);
    rows++; cols++;
    if (width) *width = (int)(cols * hscale);
    if (height) *height = (int)(rows * vscale);
}

/* ============================================================================
 * Stagger positioning constants and helpers
 * ============================================================================
 */

static const int kInitialStaggerX = 3;
static const int kInitialStaggerY = 3;
static const int kStaggerDeltaX = 2;
static const int kStaggerDeltaY = 2;

static void compute_stagger_position(int width, int height, int maxCols, int maxRows, int *outX, int *outY) {
    static int currentX = kInitialStaggerX;
    static int currentY = kInitialStaggerY;
    currentX += kStaggerDeltaX;
    currentY += kStaggerDeltaY;
    if (currentX + width > maxCols) currentX = 1;
    if (currentY + height > maxRows) currentY = 1;
    *outX = currentX;
    *outY = currentY;
}

/* ============================================================================
 * Window Creation, Borders, Shadows, Fill, Move, Resize
 * ============================================================================
 */

/* Window creation, returns new WINDOW* or NULL */
WINDOW *window_create(WINDOW *parent, int x, int y, int width, int height) {
    int scr_rows, scr_cols;
    get_screen_size(&scr_rows, &scr_cols);
    if (x == WPOS_STAGGERED || y == WPOS_STAGGERED)
        compute_stagger_position(width, height, scr_cols, scr_rows, &x, &y);
    WINDOW *win = parent ? derwin(parent, height, width, y, x) : newwin(height, width, y, x);
    return win;
}

/* Fill window with cchar_t, color, and attribute */
void window_fill(WINDOW *window, const cchar_t *ch, short color, attr_t attr) {
    if (!window || !ch) return;
    int rows, cols;
    get_win_geom(window, &rows, &cols, NULL, NULL);
    attr_t combined = attr | COLOR_PAIR(color);
    wattron(window, combined);
    wmove(window, 0, 0);
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) {
            wadd_wch(window, ch);
        }
    }
    wattroff(window, combined);
}

/* Move window relative */
int window_move_rel(WINDOW *window, int dx, int dy) {
    if (!window) return ERR;
    WINDOW *parent = wgetparent(window);
    int curr_y = 0, curr_x = 0;
    if (parent) {
        if (wgetparent(parent)) return ERR;
        getparyx(window, curr_y, curr_x);
        return mvderwin(window, curr_y + dy, curr_x + dx);
    } else {
        getbegyx(window, curr_y, curr_x);
        return mvwin(window, curr_y + dy, curr_x + dx);
    }
}

/* Dummy function for now */
void subwin_move_realign(WINDOW *subwin) {
    if (!subwin || !wgetparent(subwin)) return;
    int abs_x = 0, abs_y = 0;
    WINDOW *win = subwin, *parent;
    while ((parent = wgetparent(win)) != NULL) {
        abs_x += getparx(win);
        abs_y += getpary(win);
        win = parent;
    }
    abs_x += getbegx(win);
    abs_y += getbegy(win);
    /* abs_x, abs_y now absolute coordinate */
    (void)abs_x; (void)abs_y;
}

/* Window decoration: border and centered title */
void window_decorate(WINDOW *window, const char *title, bool border) {
    if (!window) return;
    int rows, cols;
    get_win_geom(window, &rows, &cols, NULL, NULL);
    if (border) box_set(window, WACS_VLINE, WACS_HLINE);
    if (title && *title) {
        int len = (int)strlen(title);
        int start_col = (cols - len) / 2;
        mvwprintw(window, 0, start_col, "%s", title);
    }
    touchwin(window);
}

/* Modify border attributes/colors */
void window_modify_border(WINDOW *window, int attrs, short color_pair) {
    if (!window) return;
    int rows, cols;
    get_win_geom(window, &rows, &cols, NULL, NULL);
    attr_t base_attr = (attr_t)attrs;
    for (int x = 0; x < cols; ++x) {
        chtype top = mvwinch(window, 0, x);
        chtype bottom = mvwinch(window, rows - 1, x);
        bool alt_top = (top & A_ALTCHARSET);
        bool alt_bottom = (bottom & A_ALTCHARSET);
        mvwchgat(window, 0, x, 1, base_attr | (alt_top ? A_ALTCHARSET : 0), color_pair, NULL);
        mvwchgat(window, rows - 1, x, 1, base_attr | (alt_bottom ? A_ALTCHARSET : 0), color_pair, NULL);
    }
    for (int y = 1; y < rows - 1; ++y) {
        chtype left = mvwinch(window, y, 0);
        chtype right = mvwinch(window, y, cols - 1);
        bool alt_left = (left & A_ALTCHARSET);
        bool alt_right = (right & A_ALTCHARSET);
        mvwchgat(window, y, 0, 1, base_attr | (alt_left ? A_ALTCHARSET : 0), color_pair, NULL);
        mvwchgat(window, y, cols - 1, 1, base_attr | (alt_right ? A_ALTCHARSET : 0), color_pair, NULL);
    }
}

/* Drop shadow implementation */
static int create_shadow_cell(WINDOW *src, int row, int col, cchar_t *shadow_cell) {
    cchar_t orig;
    wchar_t ch;
    attr_t attr_orig;
    short colp;
    if (mvwin_wch(src, row, col, &orig) == ERR) return ERR;
    getcchar(&orig, &ch, &attr_orig, &colp, NULL);
    setcchar(shadow_cell, ch, attr_orig, cursive_color_pair(COLOR_WHITE, COLOR_BLACK), NULL);
    return OK;
}

WINDOW *window_create_shadow(WINDOW *base_win, WINDOW *underlying_win) {
    if (!base_win) return NULL;
    int height, width, beg_y, beg_x;
    get_win_geom(base_win, &height, &width, &beg_y, &beg_x);
    if (!underlying_win) underlying_win = SCREEN_WINDOW;
    WINDOW *shadow = newwin(height, width, beg_y + 1, beg_x + 1);
    if (!shadow) return NULL;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            cchar_t cell;
            int src_y = beg_y + y + 1, src_x = beg_x + x + 1;
            if (create_shadow_cell(underlying_win, src_y, src_x, &cell) == OK)
                mvwadd_wch(shadow, y, x, &cell);
        }
    }
    return shadow;
}

/* ============================================================================
 * Color Management
 * ============================================================================
 */

short cursive_color_table[] = {
    COLOR_BLACK, COLOR_RED, COLOR_GREEN, COLOR_YELLOW, COLOR_BLUE,
    COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE
};
int cursive_color_count = 0;

static inline size_t total_pairs(void) {
    return (size_t)cursive_color_count * (size_t)cursive_color_count;
}

static inline void compute_matrix_indices(size_t index, short *raw_fg, short *raw_bg) {
    *raw_fg = (short)(index / cursive_color_count);
    *raw_bg = (short)(cursive_color_count - (index % cursive_color_count) - 1);
}

static inline short fast_color_pair(short fg, short bg) {
    return (short)(bg * cursive_color_count + (cursive_color_count - fg - 1));
}

struct color_mtx { int fg; int bg; };
static void swap_to_zero(struct color_mtx *matrix, size_t swap_index) {
    struct color_mtx tmp = matrix[0];
    matrix[0] = matrix[swap_index];
    matrix[swap_index] = tmp;
}

void cursive_color_init(void) {
    start_color();
    cursive_color_count = ARRAY_SZ(cursive_color_table);
    size_t pair_count = total_pairs();
    struct color_mtx *matrix = calloc(pair_count, sizeof *matrix);
    if (!matrix) return;
    size_t white_black_idx = (size_t)-1;
    for (size_t i = 0; i < pair_count; ++i) {
        short raw_fg, raw_bg;
        compute_matrix_indices(i, &raw_fg, &raw_bg);
        matrix[i].bg = raw_fg; matrix[i].fg = raw_bg;
        if (raw_bg == COLOR_WHITE && raw_fg == COLOR_BLACK)
            white_black_idx = i;
    }
    if (white_black_idx != (size_t)-1 && white_black_idx != 0)
        swap_to_zero(matrix, white_black_idx);
    for (size_t i = 1; i < pair_count; ++i)
        init_pair((int)i, matrix[i].fg, matrix[i].bg);
    free(matrix);
}

/* Return color pair number for (fg, bg) */
short cursive_color_pair(short fg, short bg) {
    if (fg == COLOR_WHITE && bg == COLOR_BLACK) return 0;
    if (cursive_global_flags & CURSIVE_FASTCOLOR) return fast_color_pair(fg, bg);
    for (int i = 1; i < COLOR_PAIRS; ++i) {
        short tf, tb;
        pair_content(i, &tf, &tb);
        if (tf == fg && tb == bg) return (short)i;
    }
    return 0;
}

/* Return fg/bg for given pair, using fast arithmetic or ncurses lookup */
int cursive_pair_content(short pair, short *fg, short *bg) {
    if (!(cursive_global_flags & CURSIVE_FASTCOLOR))
        return pair_content(pair, fg, bg);
    if (pair == 0) { *fg = COLOR_WHITE; *bg = COLOR_BLACK; }
    *bg = (short)(pair / cursive_color_count);
    *fg = (short)((cursive_color_count - pair) - (pair % cursive_color_count));
    return 0;
}

#define CURSIVE_COLORS(fg, bg) (COLOR_PAIR(cursive_color_pair(fg, bg)))

/* ============================================================================
 * Window property set/get API
 * ============================================================================
 */

void cursive_window_set_title(cursive_window_t *cursive_window, const char *title) {
    if (cursive_window == NULL) return;
    cursive_window->title = title;
}

const char *cursive_window_get_title(cursive_window_t *cursive_window) {
    if (cursive_window == NULL) return NULL;
    return cursive_window->title;
}

void cursive_window_set_class(cursive_window_t *cursive_window, void *classid) {
    if (cursive_window == NULL) return;
    cursive_window->classid = classid;
}

/* ============================================================================
 * Window resize API
 * ============================================================================
 */

int cursive_wresize(cursive_window_t *cursive_window, int width, int height) {
    if (cursive_window == NULL) return ERR;
    if (width == 0 && height == 0) return ERR;
    if (width == 0) width = WSIZE_UNCHANGED;
    if (height == 0) height = WSIZE_UNCHANGED;
    if (cursive_window->window_state & STATE_NORESIZE) return ERR;
    if (cursive_window->ctx->screen_id != cursive->cur_scr_id) return ERR;

    int orig_max_y, orig_max_x;
    getmaxyx(cursive_window->window_frame, orig_max_y, orig_max_x);
    int orig_beg_y, orig_beg_x;
    getbegyx(cursive_window->window_frame, orig_beg_y, orig_beg_x);

    WINDOW *copy_pad = newwin(orig_max_y - 1, orig_max_x - 1, orig_beg_y, orig_beg_x);
    overwrite(cursive_window->window_frame, copy_pad);

    int screen_max_y, screen_max_x;
    getmaxyx(CURRENT_SCREEN, screen_max_y, screen_max_x);

    if (width == WSIZE_FULLSCREEN) {
        cursive_mvwin_abs(cursive_window, WPOS_UNCHANGED, 0);
        width = screen_max_x;
    }
    if (height == WSIZE_FULLSCREEN) {
        cursive_mvwin_abs(cursive_window, 0, WPOS_UNCHANGED);
        height = screen_max_y;
    }
    if (width == WSIZE_DEFAULT) width = cursive_window->min_width;
    if (height == WSIZE_DEFAULT) height = cursive_window->min_height;

    wresize(cursive_window->window_frame, height, width);
    werase(cursive_window->window_frame);

    if (cursive_window->ctx->managed) {
        wresize(cursive_window->user_window, height - 2, width - 2);
        werase(cursive_window->user_window);
    }

    getmaxyx(cursive_window->window_frame, orig_max_y, orig_max_x);
    overwrite(copy_pad, cursive_window->window_frame);
    delwin(copy_pad);

    if (cursive_window->ctx->managed) {
        if (cursive_window->window_state & STATE_FOCUS)
            cursive_event_run(cursive_window, "window-focus");
        else
            cursive_event_run(cursive_window, "window-unfocus");
    }

    cursive_event_run(cursive_window, "window-resized");
    cursive_window_redraw(cursive_window);
    return 0;
}

int cursive_wresize_rel(cursive_window_t *cursive_window, int vector_x, int vector_y) {
    if (cursive_window == NULL) return ERR;
    if (vector_x == 0 && vector_y == 0) return 0;
    if ((cursive_window->window_state & STATE_NORESIZE)
         || (cursive_window->ctx->screen_id != cursive->cur_scr_id))
        return ERR;
    int curr_max_y, curr_max_x;
    getmaxyx(cursive_window->window_frame, curr_max_y, curr_max_x);
    int new_width = curr_max_x + vector_x;
    int new_height = curr_max_y + vector_y;
    return cursive_wresize(cursive_window, new_width, new_height);
}

/* ============================================================================
 * Window move API
 * ============================================================================
 */

static inline void reposition_subwindow(cursive_window_t *cursive_window, int status) {
    if (cursive_window->ctx->managed == TRUE && status != ERR) {
        mvderwin(cursive_window->user_window,
                 getpary(cursive_window->user_window),
                 getparx(cursive_window->user_window));
    }
}

static inline void post_move_actions(cursive_window_t *cursive_window) {
    cursive_event_run(cursive_window, "window-move");
    cursive_screen_redraw(cursive_window->ctx->screen_id, REDRAW_ALL);
}

int cursive_mvwin_rel(cursive_window_t *cursive_window, int vector_x, int vector_y) {
    if (vector_x == 0 && vector_y == 0) return 1;
    if (cursive_window == NULL) return 0;
    int status = window_move_rel(cursive_window->window_frame, vector_x, vector_y);
    reposition_subwindow(cursive_window, status);
    post_move_actions(cursive_window);
    return status;
}

int cursive_mvwin_abs(cursive_window_t *cursive_window, int x, int y) {
    if (!cursive_window) return 0;
    int current_y, current_x;
    getbegyx(cursive_window->window_frame, current_y, current_x);
    int target_x = (x == WPOS_UNCHANGED) ? current_x : x;
    int target_y = (y == WPOS_UNCHANGED) ? current_y : y;
    int status = mvwin(cursive_window->window_frame, target_y, target_x);
    reposition_subwindow(cursive_window, status);
    post_move_actions(cursive_window);
    return status;
}

/* ============================================================================
 * Window lookup API (by class/title)
 * ============================================================================
 */

typedef bool (*cursive_window_match_fn)(cursive_window_t *wnd, const void *key);

static cursive_window_t *find_window_generic(int screen_id, bool managed,
                                             cursive_window_match_fn matcher, const void *key) {
    cursive_screen_t *screen;
    struct list_head *head, *pos;
    if (screen_id < 0) screen_id = cursive->cur_scr_id;
    screen = &cursive->cursive_screen[screen_id];
    head = managed ? &screen->managed_list : &screen->unmanaged_list;
    if (list_empty(head)) return NULL;
    list_for_each(pos, head) {
        cursive_window_t *wnd = list_entry(pos, cursive_window_t, list);
        if (matcher(wnd, key)) return wnd;
    }
    return NULL;
}

static bool match_by_classid(cursive_window_t *wnd, const void *key) {
    return (wnd->classid == key);
}

static bool match_by_title(cursive_window_t *wnd, const void *key) {
    return (strcmp(wnd->title, (const char *)key) == 0);
}

cursive_window_t *cursive_window_find_by_class(int screen_id, bool managed, void *classid) {
    return find_window_generic(screen_id, managed, match_by_classid, classid);
}

cursive_window_t *cursive_window_find_by_title(int screen_id, bool managed, char *title) {
    return find_window_generic(screen_id, managed, match_by_title, title);
}

/* ============================================================================
 * Window close and destruction API
 * ============================================================================
 */

static void focus_next_managed_window(int screen_id) {
    cursive_window_t *next = cursive_window_get_top(screen_id, true);
    if (next) cursive_window_set_focus(next);
}

void cursive_window_close(cursive_window_t *cursive_window) {
    if (!cursive_window) return;
    int screen_id = cursive_window->ctx->screen_id;
    bool was_managed = (cursive_window->ctx->managed == TRUE);
    cursive_event_run(cursive_window, "window-close");
    list_move(&cursive_window->list, &cursive->zombie_list);
    if (was_managed) focus_next_managed_window(screen_id);
    cursive_screen_redraw(screen_id, REDRAW_ALL);
}

/* ============================================================================
 * Window creation API
 * ============================================================================
 */

static int normalize_dimension(float spec, int screen_dim, int *min_limit, bool is_width) {
    if (spec == WSIZE_FULLSCREEN) {
        *min_limit = WSIZE_FULLSCREEN; return screen_dim;
    }
    if (spec > 0.0f && spec < 1.0f) {
        int scaled = 0;
        if (is_width) window_get_size_scaled(CURRENT_SCREEN, &scaled, NULL, spec, 0);
        else          window_get_size_scaled(CURRENT_SCREEN, NULL, &scaled, 0, spec);
        return scaled;
    }
    return (int)spec;
}

static int normalize_position(float spec, int screen_dim, int dim) {
    if (spec > 0.0f && spec < 1.0f)
        return (int)((screen_dim - dim - 2) * spec);
    return (int)spec;
}

cursive_window_t *cursive_window_create(int screen_id, bool managed, char *title,
                                        float x, float y, float width, float height) {
    if (screen_id == -1) screen_id = cursive->cur_scr_id;
    cursive_screen_t *cursive_screen = &cursive->cursive_screen[screen_id];
    cursive_window_t *cursive_window = calloc(1, sizeof(*cursive_window));
    cursive_ctx_t *cursive_ctx = calloc(1, sizeof(*cursive_ctx));
    if (!cursive_window || !cursive_ctx) {
        free(cursive_window);
        free(cursive_ctx);
        return NULL;
    }

    cursive_ctx->screen_id = screen_id;
    cursive_ctx->managed = managed;
    cursive_window->ctx = cursive_ctx;
    cursive_window->title = title;
    cursive_window->window_state |= STATE_VISIBLE;

    if (managed) list_add(&cursive_window->list, &cursive_screen->managed_list);
    else         list_add(&cursive_window->list, &cursive_screen->unmanaged_list);

    int screen_w = 0, screen_h = 0;
    getmaxyx(cursive_screen->screen, screen_h, screen_w);
    int win_w = normalize_dimension(width, screen_w, &cursive_window->min_width, true);
    int win_h = normalize_dimension(height, screen_h, &cursive_window->min_height, false);

    int win_x = normalize_position(x, screen_w, win_w);
    int win_y = normalize_position(y, screen_h, win_h);

    if (managed) {
        if (win_w + 1 > screen_w) win_w -= 2;
        if (win_h + 1 > screen_h) win_h -= 2;
    }

    WINDOW *frame = NULL, *user = NULL;
    if (managed) {
        frame = window_create(NULL, win_x, win_y, win_w + 2, win_h + 2);
        cursive_window->window_state |= STATE_SHADOWED;
        user = window_create(frame, 1, 1, win_w, win_h);
    } else {
        user = window_create(NULL, win_x, win_y, win_w, win_h);
        frame = user;
    }
    cursive_window->window_frame = frame;
    cursive_window->user_window = user;

    if (managed) {
        wbkgdset(frame, CURSIVE_COLORS(COLOR_BLACK, COLOR_WHITE));
        wbkgdset(user, CURSIVE_COLORS(COLOR_BLACK, COLOR_WHITE));
        werase(user);
        cursive_window->border_agent[0] = cursive->border_agent[0];
        cursive_window->border_agent[1] = cursive->border_agent[1];
    }

    if (cursive_window->min_width == 0) cursive_window->min_width = win_w;
    if (cursive_window->min_height == 0) cursive_window->min_height = win_h;

    INIT_LIST_HEAD(&cursive_window->event_list);
    cursive_event_set(cursive_window, "term-resized", cursive_event_default_TERM_RESIZE, NULL);
    cursive_window_set_top(cursive_window);

    return cursive_window;
}

int cursive_window_set_limits(cursive_window_t *cursive_window, int min_width, int min_height,
                             int max_width, int max_height) {
    if (!cursive_window) return ERR;
    if (min_width != 0 && min_width != WSIZE_UNCHANGED) cursive_window->min_width = min_width;
    if (min_height != 0 && min_height != WSIZE_UNCHANGED) cursive_window->min_height = min_height;
    if (max_width != 0 && max_width != WSIZE_UNCHANGED) cursive_window->max_width = max_width;
    if (max_height != 0 && max_height != WSIZE_UNCHANGED) cursive_window->max_height = max_height;
    return 0;
}

void cursive_window_modify_border(cursive_window_t *cursive_window, int attrs, short colors) {
    if (!cursive_window) return;
    window_modify_border(cursive_window->window_frame, attrs, colors);
}

void cursive_window_set_border_agent(cursive_window_t *cursive_window, CursiveFunc agent, int id) {
    if (!cursive_window || id < 1) return;
    cursive_window->border_agent[id] = agent;
}

/* ============================================================================
 * Window destruction API
 * ============================================================================
 */

static void trigger_window_destroy_event(cursive_window_t *cursive_window) {
    CURSIVE_EVENT *evt = cursive_get_cursive_event(cursive_window, "window-destroy");
    if (evt != NULL && evt->func != NULL) evt->func(cursive_window, evt->arg);
}

static void destroy_ncurses_windows(cursive_window_t *cursive_window) {
    if (cursive_window->user_window != cursive_window->window_frame)
        delwin(cursive_window->user_window);
    delwin(cursive_window->window_frame);
}

static void free_window_resources(cursive_window_t *cursive_window) {
    free(cursive_window->ctx);
    free(cursive_window);
}

int cursive_window_destroy(cursive_window_t *cursive_window) {
    if (cursive_window == NULL) return 0;
    trigger_window_destroy_event(cursive_window);
    cursive_event_del(cursive_window, "*");
    destroy_ncurses_windows(cursive_window);
    free_window_resources(cursive_window);
    return 0;
}

/* ============================================================================
 * Window user pointer API
 * ============================================================================
 */

void cursive_window_set_userptr(cursive_window_t *cursive_window, void *userptr) {
    if (cursive_window == NULL) return;
    cursive_window->userptr = userptr;
}

void *cursive_window_get_userptr(cursive_window_t *cursive_window) {
    if (cursive_window == NULL) return NULL;
    return cursive_window->userptr;
}

/* ============================================================================
 * Deck, Window Stacking, Hit Testing, Cycling, Occlusion
 * ============================================================================
 */

static inline struct list_head *get_wnd_list(int screen_id, bool managed) {
    if (screen_id < 0) screen_id = cursive->cur_scr_id;
    cursive_screen_t *screen = &cursive->cursive_screen[screen_id];
    return managed ? &screen->managed_list : &screen->unmanaged_list;
}

cursive_window_t *cursive_deck_hit_test(int screen_id, bool managed, int x, int y) {
    struct list_head *wnd_list = get_wnd_list(screen_id, managed);
    if (list_empty(wnd_list)) return NULL;
    struct list_head *pos;
    cursive_window_t *cursive_window = NULL;
    list_for_each(pos, wnd_list) {
        cursive_window = list_entry(pos, CURSIVE_WINDOW, list);
        if (wenclose(cursive_window->window_frame, y, x)) break;
        cursive_window = NULL;
    }
    return cursive_window;
}

cursive_window_t *cursive_window_get_top(int screen_id, bool managed) {
    struct list_head *wnd_list = get_wnd_list(screen_id, managed);
    if (list_empty(wnd_list)) return NULL;
    struct list_head *pos;
    list_for_each(pos, wnd_list) {
        cursive_window_t *cursive_window = list_entry(pos, cursive_window_t, list);
        if (cursive_window->window_state & STATE_VISIBLE) return cursive_window;
    }
    return NULL;
}

bool cursive_window_set_top(cursive_window_t *cursive_window) {
    if (!cursive_window) return false;
    int screen_id = cursive->cur_scr_id;
    if (cursive_window->ctx->screen_id != screen_id) return false;
    struct list_head *wnd_list = get_wnd_list(screen_id, cursive_window->ctx->managed);
    if (!is_cursive_window_visible(cursive_window)) return false;
    list_move(&cursive_window->list, wnd_list);
    return cursive_window_set_focus(cursive_window);
}

void cursive_deck_cycle(int screen_id, bool managed, int vector) {
    if (screen_id != cursive->cur_scr_id) return;
    struct list_head *wnd_list = get_wnd_list(screen_id, managed);
    if (list_empty(wnd_list) || list_is_singular(wnd_list)) return;
    cursive_window_t *first_wnd = NULL, *cursive_window = NULL;
    do {
        if (vector >= VECTOR_TOP_TO_BOTTOM)
            list_rotate_left(wnd_list);
        if (vector <= VECTOR_BOTTOM_TO_TOP)
            list_rotate_right(wnd_list);
        cursive_window = list_first_entry(wnd_list, cursive_window_t, list);
        if (first_wnd == NULL) first_wnd = cursive_window;
    } while (cursive_window && !is_cursive_window_visible(cursive_window)
                && cursive_window != first_wnd);
    if (cursive_window && cursive_window_set_top(cursive_window)) {
        cursive_window_redraw(cursive_window);
        cursive_screen_redraw(screen_id, REDRAW_ALL);
    }
}

char **cursive_deck_get_wndlist(int screen_id, bool managed) {
    struct list_head *wnd_list = get_wnd_list(screen_id, managed);
    if (list_empty(wnd_list)) return NULL;
    const int MAX_TITLES = 256;
    char **titles = calloc(MAX_TITLES, sizeof(*titles));
    if (!titles) return NULL;
    int i = 0;
    struct list_head *pos;
    list_for_each(pos, wnd_list) {
        cursive_window_t *cursive_window = list_entry(pos, cursive_window_t, list);
        if (cursive_window->title) {
            titles[i++] = strdup(cursive_window->title);
            if (i >= MAX_TITLES - 1) break;
        }
    }
    titles[i] = NULL;
    return titles;
}

static inline bool cursive_deck_check_occlusion(cursive_window_t *top_wnd, cursive_window_t *bottom_wnd) {
    int bx, by, bw, bh;
    int tx, ty, tw, th;
    getbegyx(bottom_wnd->window_frame, by, bx);
    getmaxyx(bottom_wnd->window_frame, bh, bw);
    getbegyx(top_wnd->window_frame, ty, tx);
    getmaxyx(top_wnd->window_frame, th, tw);
    if (top_wnd->window_state & STATE_SHADOWED) {
        tw += 1; th += 1;
    }
    if (by > ty + th + 1) return false;
    if (bx > tx + tw + 1) return false;
    if (bx + bw + 1 < tx) return false;
    if (by + bh + 1 < ty) return false;
    return true;
}

/* ============================================================================
 * Msgbox API
 * ============================================================================
 */

static const char *MSGBOX_ICONS[4] = { " II ", " WW ", " EE ", " ?? " };
static const chtype MSGBOX_ICON_COLORS[4] = {
    CURSIVE_COLORS(COLOR_WHITE, COLOR_BLACK),
    CURSIVE_COLORS(COLOR_YELLOW, COLOR_BLACK),
    CURSIVE_COLORS(COLOR_RED, COLOR_BLACK),
    CURSIVE_COLORS(COLOR_BLUE, COLOR_BLACK)
};

static int determine_icon_index(uint32_t flags) {
    if (flags & MSGBOX_ICON_INFO) return 0;
    if (flags & MSGBOX_ICON_WARN) return 1;
    if (flags & MSGBOX_ICON_ERROR) return 2;
    if (flags & MSGBOX_ICON_QUESTION) return 3;
    return -1;
}

static const char *get_prompt_text(uint32_t flags) {
    if (flags & MSGBOX_TYPE_OK) return "[Enter] Okay";
    if (flags & MSGBOX_TYPE_YESNO) return "[Y]es | [N]o";
    return NULL;
}

cursive_window_t *cursive_msgbox_create(int screen_id, const char *title, float x, float y,
                                        int width, int height, const char *msg, uint32_t flags) {
    if (!msg) return NULL;
    char **msg_lines = strsplitv(msg, "\r\n");
    int min_w = 0, min_h = 0;
    calc_msgbox_metrics(msg_lines, &min_w, &min_h);
    if (height < 1) height = min_h;
    if (width < 1) width = min_w;
    int icon_idx = determine_icon_index(flags);
    const char *prompt = get_prompt_text(flags);
    if (prompt) height += 2;
    if (icon_idx >= 0) {
        int need = (int)strlen(MSGBOX_ICONS[icon_idx])
                 + (int)strlen(msg_lines[0]) + 1;
        if (need > width) width = need;
    }
    cursive_window_t *cursive_window = cursive_window_create(
        screen_id, true, (char *)title, x, y,
        width + 2, height + 2
    );
    int frame_h, frame_w;
    getmaxyx(cursive_window->window_frame, frame_h, frame_w);

    wresize(cursive_window->window_frame, frame_h - 2, frame_w - 2);
    mvderwin(cursive_window->user_window, 2, 2);

    wmove(cursive_window->window_frame, 0, 0);
    if (icon_idx >= 0) {
        chtype attrs = MSGBOX_ICON_COLORS[icon_idx] | A_REVERSE;
        wattron(cursive_window->window_frame, attrs);
        wprintw(cursive_window->window_frame, "%s", MSGBOX_ICONS[icon_idx]);
        wattroff(cursive_window->window_frame, attrs);
        wprintw(cursive_window->window_frame, " ");
    }
    for (char **line = msg_lines; *line; ++line) {
        int len = (int)strlen(*line);
        if (len == (frame_w - 2))
            wprintw(cursive_window->window_frame, "%s", *line);
        else
            wprintw(cursive_window->window_frame, "%s\n", *line);
    }
    strfreev(msg_lines);
    if (prompt) {
        int prompt_len = (int)strlen(prompt);
        int col = (frame_w - 2 - prompt_len) / 2;
        int row = frame_h - 3;
        mvwprintw(cursive_window->window_frame, row, col, "%s", prompt);
        if (flags & MSGBOX_TYPE_OK)
            cursive_window_set_key_func(cursive_window, cursive_kbd_default_MSGBOX_OK);
    }
    return cursive_window;
}

int cursive_kbd_default_MSGBOX_OK(int32_t keystroke, cursive_window_t *cursive_window) {
    if (keystroke == KEY_CRLF) cursive_window_destroy(cursive_window);
    return 1;
}

int calc_msgbox_metrics(char **msg_array, int *width, int *height) {
    int max_w = 0, count = 0, longest_idx = 0;
    while (msg_array[count]) {
        int len = (int)strlen(msg_array[count]);
        if (len > max_w) { max_w = len; longest_idx = count; }
        ++count;
    }
    *width = max_w; *height = count;
    return longest_idx;
}

/* ============================================================================
 * Key Event Callback API
 * ============================================================================
 */

void cursive_window_set_key_func(cursive_window_t *cursive_window, CursiveWkeyFunc func) {
    if (cursive_window == NULL) return;
    cursive_window->key_func = func;
}

CursiveWkeyFunc cursive_window_get_key_func(cursive_window_t *cursive_window) {
    return (cursive_window != NULL) ? cursive_window->key_func : NULL;
}

/* ============================================================================
 * Screen Management API
 * ============================================================================
 */

static inline int resolve_screen_id(int screen_id) {
    return (screen_id == -1 ? cursive->cur_scr_id : screen_id);
}
static inline cursive_screen_t *get_screen(int screen_id) {
    int id = resolve_screen_id(screen_id);
    return &cursive->cursive_screen[id];
}

WINDOW *cursive_screen_get_wallpaper(int screen_id) {
    return get_screen(screen_id)->wallpaper;
}

void cursive_screen_set_wallpaper(int screen_id, WINDOW *wallpaper, CursiveBkgdFunc agent) {
    cursive_screen_t *scr = get_screen(screen_id);
    scr->wallpaper = wallpaper;
    scr->wallpaper_agent = agent;
}

WINDOW *cursive_get_screen_window(int screen_id) {
    return get_screen(screen_id)->screen;
}

int cursive_get_active_screen(void) {
    return cursive->cur_scr_id;
}

void cursive_screen_reset(int screen_id) {
    int id = resolve_screen_id(screen_id);
    if (id != cursive->cur_scr_id) return;
    cursive_screen_t *scr = &cursive->cursive_screen[id];
    if (scr->wallpaper) overwrite(scr->wallpaper, scr->screen);
    else werase(scr->screen);
}

void cursive_screen_redraw(int screen_id, uint32_t update_mask) {
    int id = resolve_screen_id(screen_id);
    if (id != cursive->cur_scr_id) return;
    cursive_screen_t *scr = &cursive->cursive_screen[id];
    uint32_t state = 0;
    CursiveBkgdFunc agent_cb = NULL;
    if ((update_mask & REDRAW_BACKGROUND) && scr->wallpaper) {
        agent_cb = scr->wallpaper_agent;
        if (agent_cb) agent_cb(id);
    }
    if (update_mask & REDRAW_WORKSPACE)
        cursive_screen_reset(id);
    if (update_mask & REDRAW_WINDOWS) {
        state |= STATE_VISIBLE;
        cursive_prune_zombie_list();
        cursive_window_for_each(id, true, VECTOR_BOTTOM_TO_TOP,
                               cursive_callback_blit_window, &state);
        cursive_window_for_each(id, false, VECTOR_BOTTOM_TO_TOP,
                               cursive_callback_blit_window, &state);
    }
    if ((update_mask & REDRAW_MOUSE) && cursive->console_mouse)
        overwrite(cursive->console_mouse, scr->screen);
    touchwin(scr->screen); wnoutrefresh(scr->screen); doupdate();
}

/* ============================================================================
 * Window State and Redraw API
 * ============================================================================
 */

typedef struct {
    cursive_window_t *catalyst;
    bool managed;
} redraw_marker_t;

static inline void set_window_flag(cursive_window_t *wnd, uint32_t flag, bool enable) {
    if (!wnd) return;
    if (enable) wnd->window_state |= flag;
    else        wnd->window_state &= ~flag;
}

uint32_t cursive_window_get_state(cursive_window_t *cursive_window) {
    return cursive_window ? cursive_window->window_state : 0;
}

void cursive_window_set_shadow(cursive_window_t *cursive_window, bool enable) {
    set_window_flag(cursive_window, STATE_SHADOWED, enable);
}

void cursive_window_set_visible(cursive_window_t *cursive_window, bool enable) {
    set_window_flag(cursive_window, STATE_VISIBLE, enable);
}

void cursive_window_hide(cursive_window_t *cursive_window) {
    set_window_flag(cursive_window, STATE_VISIBLE, FALSE);
}

void cursive_window_set_resizable(cursive_window_t *cursive_window, bool resizable) {
    if (!cursive_window) return;
    set_window_flag(cursive_window, STATE_NORESIZE, !resizable);
}

int cursive_window_get_screen_id(cursive_window_t *cursive_window) {
    return cursive_window ? cursive_window->ctx->screen_id : -1;
}

bool cursive_window_set_focus(cursive_window_t *cursive_window) {
    if (!cursive_window) return false;
    int screen_id = cursive_window->ctx->screen_id;
    cursive_screen_t *screen = &cursive->cursive_screen[screen_id];
    struct list_head *wnd_list = cursive_window->ctx->managed ? &screen->managed_list : &screen->unmanaged_list;
    cursive_window_t *sibling;
    list_for_each_entry(sibling, wnd_list, list)
        if (sibling->window_state & STATE_FOCUS) {
            sibling->window_state &= ~STATE_FOCUS;
            cursive_event_run(sibling, "window-unfocus");
        }
    cursive_window->window_state |= STATE_FOCUS;
    cursive_event_run(cursive_window, "window-focus");
    return true;
}

void cursive_window_redraw(cursive_window_t *cursive_window) {
    static redraw_marker_t *marker = NULL;
    if (!cursive_window) return;
    int screen_id = cursive_window->ctx->screen_id;
    if (screen_id != cursive->cur_scr_id) return; /* only redraw active screen */
    cursive_screen_t *screen = &cursive->cursive_screen[screen_id];
    if (!marker) {
        marker = calloc(1, sizeof(*marker));
        if (!marker) return;
        marker->catalyst = cursive_window;
        marker->managed = cursive_window->ctx->managed;
    }
    struct list_head *wnd_list = marker->managed ? &screen->managed_list : &screen->unmanaged_list;
    cursive_callback_blit_window(cursive_window, NULL);
    if (!list_is_first(&cursive_window->list, wnd_list)) {
        cursive_window_t *prev = list_prev_entry(cursive_window, list);
        list_for_each_entry_from_reverse(prev, wnd_list, list)
            if (cursive_deck_check_occlusion(prev, cursive_window))
                cursive_window_redraw(prev);
    }
    if (cursive_window == marker->catalyst) {
        if (marker->managed)
            cursive_window_for_each(screen_id, false, VECTOR_BOTTOM_TO_TOP,
                                   cursive_callback_blit_window, NULL);
        free(marker); marker = NULL;
        if (cursive->console_mouse) overwrite(cursive->console_mouse, SCREEN_WINDOW);
    }
}

bool is_cursive_window_visible(cursive_window_t *cursive_window) {
    return (cursive_window && (cursive_window->window_state & STATE_VISIBLE)) ? true : false;
}

/* ============================================================================
 * Initialization/Teardown
 * ============================================================================
 */

CURSIVE *cursive_init(uint32_t init_flags) {
    if (cursive) return cursive;
    setlocale(LC_ALL, "UTF-8");
    SCREEN_WINDOW = initscr();
    if (!SCREEN_WINDOW) {
        fprintf(stderr, "cursive_init: initscr() failed\n");
        return NULL;
    }
    cursive_global_flags |= init_flags;
    cursive = calloc(1, sizeof(*cursive));
    if (!cursive) {
        endwin();
        SCREEN_WINDOW = NULL;
        fprintf(stderr, "cursive_init: memory allocation failed\n");
        return NULL;
    }
    cursive->cur_scr_id = 0;
    cursive->cursive_screen[0].screen = SCREEN_WINDOW;
    cursive_color_init();
    const char *term_env = getenv("TERM");
    if (term_env && strstr(term_env, "xterm")) cursive->xterm = true;
    cursive->user = getuid();
    int height = 0, width = 0;
    getmaxyx(SCREEN_WINDOW, height, width);
    cursive->border_agent[0] = cursive_default_border_agent_unfocus;
    cursive->border_agent[1] = cursive_default_border_agent_focus;
    mmask_t mouse_mask = ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION;
    mousemask(mouse_mask, NULL);
    initialize_screens(cursive, height, width);
    INIT_LIST_HEAD(&cursive->zombie_list);
    configure_curses();
    configure_termios();
    return cursive;
}

void cursive_end(void) {
    cursive_kmio_gpm(NULL, CMD_GPM_CLOSE);
    free(cursive); cursive = NULL;
    curs_set(1);
    endwin(); SCREEN_WINDOW = NULL;
    enable_echo();
}

void cursive_set_border_agent(CursiveFunc agent, int id) {
    if (id >= 0 && id <= 1)
        cursive->border_agent[id] = agent;
}

WINDOW *cursive_window_get_frame(const cursive_window_t *cursive_window) {
    return cursive_window ? cursive_window->window_frame : NULL;
}

void cursive_window_for_each(int screen_id, bool managed, int vector,
                             CursiveFunc func, void *arg) {
    if (!func) return;
    if (screen_id == -1) screen_id = cursive->cur_scr_id;
    cursive_screen_t *vs = &cursive->cursive_screen[screen_id];
    struct list_head *wnd_list = managed ? &vs->managed_list : &vs->unmanaged_list;
    if (list_empty(wnd_list)) return;
    struct list_head *pos;
    if (vector == VECTOR_BOTTOM_TO_TOP) {
        list_for_each_prev(pos, wnd_list) {
            cursive_window_t *vw = list_entry(pos, cursive_window_t, list);
            func(vw, arg);
        }
    } else {
        list_for_each(pos, wnd_list) {
            cursive_window_t *vw = list_entry(pos, cursive_window_t, list);
            func(vw, arg);
        }
    }
}

int cursive_prune_zombie_list(void) {
    if (list_empty(&cursive->zombie_list)) return 0;
    struct list_head *pos, *tmp;
    list_for_each_safe(pos, tmp, &cursive->zombie_list) {
        cursive_window_t *vw = list_entry(pos, cursive_window_t, list);
        cursive_window_destroy(vw);
        list_del(pos);
    }
    return 0;
}

/* ============================================================================
 * Window manager callback implementations (touchwin, blit, border, wallpaper)
 * ============================================================================
 */

int cursive_callback_touchwin(cursive_window_t *cursive_window, void *arg) {
    if (cursive_window == NULL) return ERR;
    if (cursive_window->window_state & STATE_VISIBLE) {
        touchwin(cursive_window->user_window);
        touchwin(cursive_window->window_frame);
    }
    UNUSED(arg);
    return 0;
}

inline int cursive_callback_blit_window(cursive_window_t *cursive_window, void *arg) {
    if (cursive_window == NULL) return ERR;
    if (!(cursive_window->window_state & STATE_VISIBLE)) return 0;
    int index = 0, result = 0;
    if (cursive_window->ctx->managed == TRUE) {
        if (cursive_window->window_state & STATE_FOCUS) index = 1;
        if (cursive_window->border_agent[index] != NULL)
            cursive_window->border_agent[index](cursive_window, cursive_window);
    }
    if (cursive_window->window_state & STATE_SHADOWED) {
        WINDOW *shadow = window_create_shadow(
            cursive_window->window_frame, SCREEN_WINDOW
        );
        overwrite(shadow, SCREEN_WINDOW);
        delwin(shadow);
    }
    result = overwrite(cursive_window->window_frame, SCREEN_WINDOW);
    if (result == ERR) return -1;
    UNUSED(arg);
    return 0;
}

void cursive_callback_del_event(void *data, void *anything) {
    free(data); UNUSED(anything);
}

void cursive_default_wallpaper_agent(int screen_id) {
    if (screen_id > MAX_SCREENS) return;
    cursive_screen_t *vscr = &cursive->cursive_screen[screen_id];
    int height, width;
    getmaxyx(vscr->screen, height, width);
    wresize(vscr->wallpaper, height, width);
    static cchar_t bg_char;
    wchar_t space[] = { 0x0020, 0x0000 };
    setcchar(&bg_char, space, 0, 0, NULL);
    window_fill(
        vscr->wallpaper, &bg_char,
        cursive_color_pair(COLOR_WHITE, COLOR_BLUE), A_NORMAL
    );
}

static int border_agent_common(cursive_window_t *cursive_window, int attribute, int color_pair) {
    if (cursive_window == NULL || cursive_window->ctx->managed == FALSE)
        return -1;
    window_decorate(cursive_window->window_frame, (char *)cursive_window->title, TRUE);
    window_modify_border(cursive_window->window_frame, attribute, color_pair);
    return 0;
}

int cursive_default_border_agent_focus(cursive_window_t *cursive_window, void *arg) {
    UNUSED(arg);
    return border_agent_common(cursive_window, A_NORMAL,
        cursive_color_pair(COLOR_MAGENTA, COLOR_WHITE));
}

int cursive_default_border_agent_unfocus(cursive_window_t *cursive_window, void *arg) {
    UNUSED(arg);
    return border_agent_common(cursive_window, A_BOLD,
        cursive_color_pair(COLOR_BLACK, COLOR_WHITE));
}

/* ============================================================================
 * Event management API
 * ============================================================================
 */

static void invoke_callback(const CURSIVE_EVENT *evt, cursive_window_t *window, void *payload) {
    if (!evt || !evt->func) return;
    void *arg_to_pass = (payload != NULL) ? payload : evt->arg;
    evt->func(window, arg_to_pass);
}

static bool is_broadcast_target(const cursive_window_t *window) {
    return (memcmp(window, CURSIVE_EVENT_BROADCAST,
                   strlen((const char*)CURSIVE_EVENT_BROADCAST)) == 0);
}

static void process_broadcast_event(const char *event_name, void *payload) {
    int total_loops = MAX_SCREENS * 2;
    for (int i = 0; i < total_loops; ++i) {
        bool managed = (i < MAX_SCREENS);
        int screen_idx = managed ? i : (i - MAX_SCREENS);
        struct list_head *wnd_list = managed
            ? &cursive->cursive_screen[screen_idx].managed_list
            : &cursive->cursive_screen[screen_idx].unmanaged_list;

        struct list_head *pos;
        list_for_each_prev(pos, wnd_list) {
            cursive_window_t *wnd = list_entry(pos, cursive_window_t, list);
            CURSIVE_EVENT *evt = cursive_get_cursive_event(wnd, (char*)event_name);
            if (evt) invoke_callback(evt, wnd, payload);
        }
    }
}

CURSIVE_EVENT *cursive_get_cursive_event(cursive_window_t *window, char *event_name) {
    if (!window || !event_name) return NULL;
    if (list_empty(&window->event_list)) return NULL;
    struct list_head *pos;
    list_for_each(pos, &window->event_list) {
        CURSIVE_EVENT *evt = list_entry(pos, CURSIVE_EVENT, list);
        if (strcmp(evt->event, event_name) == 0)
            return evt;
    }
    return NULL;
}

int cursive_event_set(cursive_window_t *window, char *event_name, CursiveFunc func, void *arg) {
    if (!window || !event_name || !func) return -1;
    CURSIVE_EVENT *evt = cursive_get_cursive_event(window, event_name);
    if (!evt) {
        evt = (CURSIVE_EVENT*)calloc(1, sizeof(CURSIVE_EVENT));
        if (!evt) return -1;
        list_add(&evt->list, &window->event_list);
    }
    evt->event = event_name;
    evt->func = func;
    evt->arg = arg;
    return 1;
}

int cursive_event_exec(cursive_window_t *window, char *event_name, void *payload) {
    if (!window || !event_name) return ERR;
    if (is_broadcast_target(window)) process_broadcast_event(event_name, payload);
    else {
        CURSIVE_EVENT *evt = cursive_get_cursive_event(window, event_name);
        if (evt) invoke_callback(evt, window, payload);
    }
    return 1;
}

int cursive_event_del(cursive_window_t *window, char *event_name) {
    if (!window || !event_name) return -1;
    if (list_empty(&window->event_list)) return ERR;
    if (event_name[0] == '*') {
        struct list_head *pos, *tmp;
        list_for_each_safe(pos, tmp, &window->event_list) {
            CURSIVE_EVENT *evt = list_entry(pos, CURSIVE_EVENT, list);
            list_del(&evt->list); free(evt);
        }
        return 1;
    }
    CURSIVE_EVENT *evt = cursive_get_cursive_event(window, event_name);
    if (evt) {
        list_del(&evt->list); free(evt);
        return 1;
    }
    return ERR;
}

int cursive_event_default_TERM_RESIZE(cursive_window_t *window, void *arg) {
    int delta_rows = window_check_height(window->window_frame);
    int delta_cols = window_check_width(window->window_frame);
    if (delta_rows > 0 || delta_cols > 0)
        cursive_wresize(window, window->min_width, window->min_height);
    cursive_screen_redraw(window->ctx->screen_id, REDRAW_ALL);
    (void)arg; return 0;
}

int cursive_event_run(cursive_window_t *window, char *event_name) {
    return cursive_event_exec(window, event_name, NULL);
}

/* ============================================================================
 * Keyboard/Mouse I/O API
 * ============================================================================
 */

static cursive_window_t *s_eventWindow = NULL;
static MEVENT s_prevMouseEvent = {0};
static int s_eventMode = EVENTMODE_IDLE;
static CursiveWkeyFunc s_currentKeyFunc = NULL;

#define CURRENT_SCREEN         cursive_get_screen_window(-1)
#define CURRENT_SCREEN_ID      cursive_get_active_screen()
#define WINDOW_FRAME(wnd)      cursive_window_get_frame(wnd)
#define CWINDOW(wnd)           (*(WINDOW**)(wnd))
#define TOPMOST_MANAGED        cursive_window_get_top(-1, true)
#define TOPMOST_UNMANAGED      cursive_window_get_top(-1, false)

int32_t cursive_kmio_fetch(MEVENT *mouse_event) {
    int32_t key = ERR;
    cursive_kmio_gpm(mouse_event, 0);
    key = getch();
    if (key == ERR) return KMIO_NONE;
    if (key == KEY_MOUSE) { getmouse(mouse_event); return KEY_MOUSE; }
    if (key != 27) return key;
    return readEscapeSequence();
}

static int32_t readEscapeSequence(void) {
    int32_t escCode = 27, ch = 0;
    uint8_t shiftBits = 4;
    while (shiftBits < 24) {
        ch = getch();
        if (ch == ERR) break;
        escCode |= (ch << shiftBits);
        shiftBits <<= 1;
    }
    return escCode;
}

void cursive_kmio_dispatch(int32_t keystroke, MEVENT *mouse_event) {
    extern int gpm_fd;
    if (keystroke == KMIO_NONE) return;
    if (keystroke != KEY_RESIZE && keystroke != KEY_MOUSE) {
        keystroke = dispatchToUnmanaged(keystroke);
        if (keystroke == KMIO_NONE || keystroke == KMIO_HANDLED) return;
    }
    if (keystroke == KEY_RESIZE) handleResizeEvent();
    if (keystroke == KEY_MOUSE && mouse_event != NULL)
        dispatchMouseEvent(mouse_event, &keystroke);
    if (keystroke != KEY_RESIZE && keystroke != KMIO_NONE && keystroke != KMIO_HANDLED)
        keystroke = dispatchToManaged(keystroke);
    if (gpm_fd > 0) {
        cursive_kmio_show_mouse(mouse_event);
        cursive_screen_redraw(CURRENT_SCREEN_ID, REDRAW_ALL);
    }
}

static int32_t dispatchToUnmanaged(int32_t keystroke) {
    cursive_window_t *wnd = TOPMOST_UNMANAGED;
    s_currentKeyFunc = cursive_window_get_key_func(wnd);
    if (s_currentKeyFunc)
        return s_currentKeyFunc(keystroke, (void*)wnd);
    return keystroke;
}

static int32_t dispatchToManaged(int32_t keystroke) {
    cursive_window_t *wnd = TOPMOST_MANAGED;
    s_currentKeyFunc = cursive_window_get_key_func(wnd);
    if (s_currentKeyFunc)
        return s_currentKeyFunc(keystroke, (void*)wnd);
    return keystroke;
}

static void handleResizeEvent(void) {
    cursive_event_run(CURSIVE_EVENT_BROADCAST, "term-resized");
    cursive_screen_redraw(CURRENT_SCREEN_ID, REDRAW_ALL | REDRAW_BACKGROUND);
}

static void dispatchMouseEvent(MEVENT *mouse_event, int32_t *keystroke) {
    if ((mouse_event->bstate & REPORT_MOUSE_POSITION)
         && (s_eventMode == EVENTMODE_MOVE || s_eventMode == EVENTMODE_RESIZE))
        handleMouseMovement(mouse_event);
    if (mouse_event->bstate & BUTTON1_PRESSED)
        handleMousePress(mouse_event);
    if (mouse_event->bstate & BUTTON1_RELEASED)
        handleMouseRelease(mouse_event);
    if (mouse_event->bstate & BUTTON1_CLICKED)
        handleMouseClick(mouse_event, keystroke);
    if (mouse_event->bstate & BUTTON1_DOUBLE_CLICKED)
        handleMouseDoubleClick(mouse_event);
}

static void handleMouseMovement(const MEVENT *newMouse) {
    int dx = newMouse->x - s_prevMouseEvent.x;
    int dy = newMouse->y - s_prevMouseEvent.y;
    if (s_eventMode == EVENTMODE_MOVE)
        cursive_mvwin_rel(s_eventWindow, dx, dy);
    else if (s_eventMode == EVENTMODE_RESIZE)
        cursive_wresize_rel(s_eventWindow, dx, dy);
    memcpy(&s_prevMouseEvent, newMouse, sizeof(MEVENT));
}

static void handleMousePress(const MEVENT *newMouse) {
    s_eventWindow = hitTestWindow(newMouse->x, newMouse->y);
    if (s_eventWindow) {
        short beg_y, beg_x, max_y, max_x;
        cursive_window_set_top(s_eventWindow);
        cursive_window_redraw(s_eventWindow);
        cursive_screen_redraw(CURRENT_SCREEN_ID, REDRAW_ALL);
        memcpy(&s_prevMouseEvent, newMouse, sizeof(MEVENT));
        getbegyx(WINDOW_FRAME(s_eventWindow), beg_y, beg_x);
        getmaxyx(WINDOW_FRAME(s_eventWindow), max_y, max_x);
        if (newMouse->x == (beg_x + max_x - 1) && newMouse->y == (beg_y + max_y - 1))
            s_eventMode = EVENTMODE_RESIZE;
        else
            s_eventMode = EVENTMODE_MOVE;
    } else s_eventMode = EVENTMODE_IDLE;
}

static void handleMouseRelease(const MEVENT *newMouse) {
    if (!(newMouse->bstate & REPORT_MOUSE_POSITION)) {
        int dx = newMouse->x - s_prevMouseEvent.x;
        int dy = newMouse->y - s_prevMouseEvent.y;
        if (s_eventMode == EVENTMODE_MOVE)
            cursive_mvwin_rel(s_eventWindow, dx, dy);
        else if (s_eventMode == EVENTMODE_RESIZE)
            cursive_wresize_rel(s_eventWindow, dx, dy);
        cursive_screen_redraw(CURRENT_SCREEN_ID, REDRAW_ALL);
    }
    s_eventWindow = NULL; s_eventMode = EVENTMODE_IDLE;
}

static void handleMouseClick(const MEVENT *newMouse, int32_t *keystroke) {
    cursive_window_t *wnd = hitTestWindow(newMouse->x, newMouse->y);
    if (wnd) {
        short beg_y, beg_x, max_y, max_x;
        cursive_window_set_top(wnd);
        cursive_window_redraw(wnd);
        getbegyx(WINDOW_FRAME(wnd), beg_y, beg_x);
        getmaxyx(WINDOW_FRAME(wnd), max_y, max_x);
        if (newMouse->x == (beg_x + max_x - 2) && newMouse->y == beg_y) {
            cursive_window_close(wnd); *keystroke = KMIO_NONE;
        }
        else if (newMouse->x == (beg_x + max_x - 4) && newMouse->y == beg_y) {
            cursive_window_hide(wnd);
            cursive_deck_cycle(-1, TRUE, VECTOR_BOTTOM_TO_TOP);
            *keystroke = KMIO_NONE;
        }
    }
    s_eventWindow = NULL; s_eventMode = EVENTMODE_IDLE;
}

static void handleMouseDoubleClick(const MEVENT *newMouse) {
    cursive_window_t *wnd = hitTestWindow(newMouse->x, newMouse->y);
    if (wnd) {
        cursive_window_set_top(wnd);
        cursive_window_redraw(wnd);
    }
    s_eventWindow = NULL; s_eventMode = EVENTMODE_IDLE;
}

static cursive_window_t *hitTestWindow(int x, int y) {
    return cursive_deck_hit_test(-1, TRUE, x, y);
}

static void cursive_kmio_show_mouse(MEVENT *mouse_event) {
    WINDOW *screen_win = CURRENT_SCREEN;
    static WINDOW *cursor_win = NULL;
    static chtype cursor_color = 0;
    short fg, bg;
    if (!cursor_win) {
        cursor_win = newwin(1, 1, 0, 0);
        cursor_color = mvwinch(screen_win, 0, 0);
        cursive_pair_content(PAIR_NUMBER(cursor_color & A_COLOR), &fg, &bg);
        if (bg == COLOR_RED || bg == COLOR_YELLOW || bg == COLOR_MAGENTA)
            cursor_color = CURSIVE_COLORS(COLOR_CYAN, COLOR_CYAN);
        else cursor_color = CURSIVE_COLORS(COLOR_YELLOW, COLOR_YELLOW);
    }
    if (mouse_event) {
        cursor_color = mvwinch(screen_win, mouse_event->y, mouse_event->x);
        cursive_pair_content(PAIR_NUMBER(cursor_color & A_COLOR), &fg, &bg);
        if (bg == COLOR_RED || bg == COLOR_YELLOW || bg == COLOR_MAGENTA)
            cursor_color = CURSIVE_COLORS(COLOR_CYAN, COLOR_CYAN);
        else cursor_color = CURSIVE_COLORS(COLOR_YELLOW, COLOR_YELLOW);
        mvwin(cursor_win, mouse_event->y, mouse_event->x);
    }
    mvwaddch(cursor_win, 0, 0, ' ' | cursor_color);
}

int cursive_kmio_gpm(MEVENT *mouse_event, uint16_t cmd) {
    extern uint32_t cursive_global_flags;
    extern int gpm_tried;
    extern int gpm_fd;
    static int gpm_fd_internal = -1;
    struct pollfd pfd = {0};
    struct timespec wait_time = { .tv_sec = 0, .tv_nsec = 5000 };
    Gpm_Connect conn = {0};
    Gpm_Event gevent = {0};
    int fflags, i, array_len;

    if (cmd == CMD_GPM_CLOSE) {
        if (gpm_fd_internal > 0)
            Gpm_Close();
        gpm_fd_internal = -1;
        return 0;
    }
    if (!mouse_event) return -1;
    if (gpm_fd == -2 || (gpm_fd == -1 && gpm_tried)) return -1;
    if (gpm_fd_internal < 0) {
        conn.defaultMask = 0;
        conn.eventMask = GPM_MOVE | GPM_UP | GPM_DOWN | GPM_DRAG;
        conn.maxMod = ~0;
        gpm_fd_internal = Gpm_Open(&conn, 0);
        if (gpm_fd_internal > 0 && (cursive_global_flags & CURSIVE_GPM_SIGIO)) {
            fcntl(gpm_fd_internal, F_SETOWN, getpid());
            fflags = fcntl(gpm_fd_internal, F_GETFL);
            fcntl(gpm_fd_internal, F_SETFL, fflags | FASYNC);
        }
    }
    if (gpm_fd_internal < 0) return -1;
    pfd.fd = gpm_fd_internal; pfd.events = POLLIN;
    if (poll(&pfd, 1, 1) < 1) return -1;
    if (Gpm_GetEvent(&gevent) < 1) return -1;
    memset(mouse_event, 0, sizeof(*mouse_event));
    mouse_event->bstate = gevent.modifiers;
    mouse_event->x = gevent.x - 1;
    mouse_event->y = gevent.y - 1;
    array_len = sizeof(x_ncurses_state) / sizeof(x_ncurses_state[0]);
    if (!GPM_CLICK_STRICT(gevent.type)) {
        for (i = 0; i < array_len; ++i) {
            if (x_gpm_mode[i] == X_GPM_COOKED) continue;
            if (!(gevent.type & x_gpm_event[i]) || gevent.buttons != x_gpm_button[i]) continue;
            mouse_event->bstate |= x_ncurses_state[i];
            break;
        }
    }
    else {
        for (i = 0; i < array_len; ++i) {
            if (x_gpm_mode[i] == X_GPM_RAW) continue;
            if (!(gevent.type & x_gpm_event[i]) || gevent.buttons != x_gpm_button[i]) continue;
            mouse_event->bstate = x_ncurses_state[i];
            break;
        }
    }
    if ((gevent.type & GPM_DRAG) || (gevent.type & GPM_MOVE))
        mouse_event->bstate = REPORT_MOUSE_POSITION;
    while (ungetmouse(mouse_event) == ERR) nanosleep(&wait_time, NULL);
    return 0;
}

/* ============================================================================
 * End of cursive.c
 * ============================================================================
 */