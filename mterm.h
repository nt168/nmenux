#ifndef MTERM_H
#define MTERM_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "ndx.h"

#ifdef __has_include
#  if __has_include(<ncursesw/ncurses.h>)
#    include <ncursesw/ncurses.h>
#  elif __has_include(<ncurses.h>)
#    include <ncurses.h>
#  else
#    error "ncurses headers not found (install libncursesw5-dev / ncurses-devel)"
#  endif
#else
#  include <ncurses.h>
#endif

typedef enum { HOT_INPUT = 0, HOT_TERM = 1 } HotMode;

typedef struct {
    int rows, cols;
    char    *cells;
    uint16_t *attrs;
    uint16_t  cur_attr;

    uint8_t g0_charset;
    uint8_t g1_charset;
    bool    use_g1;

    bool    app_cursor;
    bool    app_keypad;

    /* DECSTBM scroll region (inclusive). Ncurses apps (e.g. htop) rely on it,
     * especially after resize where insert/delete-line is used in a region.
     */
    int     scroll_top;
    int     scroll_bottom;

    int cx, cy;
    int saved_cx, saved_cy;
    bool    wrap_pending; /* VT100 autowrap pending at last column */


    int esc_state;
    char esc_buf[128];
    int  esc_len;
    bool osc_esc_seen;
} TermView;

typedef struct {
    bool    active;
    HotMode mode;
    Node   *owner;
    int     y, x, h, w;

    WINDOW *wb;
    WINDOW *wi;

    char    input[256];
    int     in_len;

    int     master_fd;
    pid_t   pid;
    bool    running;
    TermView term;

    unsigned char raw_tail[8192];
    int           raw_len;

    Node *last_owner;
    bool  closed_by_enter;
} HotPopup;

const char *node_view_name(const Node *n, char *buf, size_t bufsz);

void hot_init(HotPopup *p);
void hot_close(HotPopup *p);
bool hot_set_geom(HotPopup *p, int y, int x, int h, int w);
bool hot_pump(HotPopup *p);
void hot_draw(HotPopup *p);
bool hot_handle_key(HotPopup *p, int ch);

#endif
