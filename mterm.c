#define _XOPEN_SOURCE 700

#include "mterm.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>

static void sleep_ms(int ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

/* =========================
 *  Hot Popup: 在 a 类节点上弹出，并可在红框区域内运行交互程序（例如 top/fzy）
 *  - 使用 pty 跑 /bin/sh -lc <cmd>
 *  - 在 curses 子窗口里做最小 ANSI/VT100 渲染：
 *      * 支持 CSI m (SGR)：反显/加粗/下划线，用于 fzy 的匹配高亮与选中行高亮
 *  - 对于 fzy：
 *      * Enter 选择后会退出，本窗口也随之关闭
 *      * 选中的那一行写入 owner->val，并在 TUI 中用 val 替换 [name] ____ 的 ____ 部分
 * ========================= */
#define TVA_REVERSE   0x01
#define TVA_BOLD      0x02
#define TVA_UNDERLINE 0x04
#define TVA_DIM       0x08
#define TVA_ACS       0x10 /* written while VT100 line-drawing charset active */

#define TVA_FG_SHIFT  8
#define TVA_BG_SHIFT  12
#define TVA_FG_MASK   (0xFu << TVA_FG_SHIFT)
#define TVA_BG_MASK   (0xFu << TVA_BG_SHIFT)
#define TVA_FG_GET(a) (((uint16_t)(a) >> TVA_FG_SHIFT) & 0xF)
#define TVA_BG_GET(a) (((uint16_t)(a) >> TVA_BG_SHIFT) & 0xF)
#define TVA_FG_SET(a,v) do {     (a) = (uint16_t)(((uint16_t)(a) & (uint16_t)~TVA_FG_MASK) | (((uint16_t)((v) & 0xF)) << TVA_FG_SHIFT)); } while (0)
#define TVA_BG_SET(a,v) do {     (a) = (uint16_t)(((uint16_t)(a) & (uint16_t)~TVA_BG_MASK) | (((uint16_t)((v) & 0xF)) << TVA_BG_SHIFT)); } while (0)


static void term_free(TermView *t) {
    if (!t) return;
    free(t->cells); t->cells = NULL;
    free(t->attrs); t->attrs = NULL;
    t->rows = t->cols = 0;
    t->cx = t->cy = 0;
    t->saved_cx = t->saved_cy = 0;
    t->cur_attr = 0;
    t->wrap_pending = false;
    t->scroll_top = 0;
    t->scroll_bottom = 0;
    t->esc_state = 0;
    t->esc_len = 0;
    t->osc_esc_seen = false;
}

static void term_clear_all(TermView *t) {
    if (!t || !t->cells || !t->attrs) return;
    memset(t->cells, ' ', (size_t)t->rows * (size_t)t->cols);
    memset(t->attrs, 0,   (size_t)t->rows * (size_t)t->cols * sizeof(uint16_t));
    t->cx = t->cy = 0;
    t->cur_attr = 0;
    t->g0_charset = 0;
    t->g1_charset = 0;
    t->use_g1 = false;
    t->app_cursor = false;
    t->app_keypad = false;
    t->scroll_top = 0;
    t->scroll_bottom = t->rows > 0 ? (t->rows - 1) : 0;
    t->wrap_pending = false;
}

/* Clear only the screen buffer (cells/attrs) but keep terminal modes.
 * This is important for ncurses apps when the viewport is resized:
 * we want a clean canvas without losing DECCKM / keypad modes. */
static void term_clear_screenbuf_keep_modes(TermView *t) {
    if (!t || !t->cells || !t->attrs) return;
    memset(t->cells, ' ', (size_t)t->rows * (size_t)t->cols);
    memset(t->attrs, 0,   (size_t)t->rows * (size_t)t->cols * sizeof(uint16_t));
    t->cx = t->cy = 0;
    t->saved_cx = t->saved_cy = 0;
    t->cur_attr = 0;
    t->wrap_pending = false;
    t->scroll_top = 0;
    t->scroll_bottom = t->rows > 0 ? (t->rows - 1) : 0;
}

static void term_init(TermView *t, int rows, int cols) {
    memset(t, 0, sizeof(*t));
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    t->rows = rows;
    t->cols = cols;
    t->cells = (char*)malloc((size_t)rows * (size_t)cols);
    t->attrs = (uint16_t*)malloc((size_t)rows * (size_t)cols * sizeof(uint16_t));
    if (!t->cells || !t->attrs) { perror("malloc"); exit(1); }
    term_clear_all(t);
}

static inline bool term_is_acs(const TermView *t) {
    if (!t) return false;
    uint8_t cs = t->use_g1 ? t->g1_charset : t->g0_charset;
    return cs == 1;
}

static void term_resize(TermView *t, int rows, int cols) {
    if (!t) return;
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    if (rows == t->rows && cols == t->cols && t->cells && t->attrs) return;

    char *oldc = t->cells;
    uint16_t *olda = t->attrs;
    int orows = t->rows, ocols = t->cols;

    t->rows = rows;
    t->cols = cols;
    t->cells = (char*)malloc((size_t)rows * (size_t)cols);
    t->attrs = (uint16_t*)malloc((size_t)rows * (size_t)cols * sizeof(uint16_t));
    if (!t->cells || !t->attrs) { perror("malloc"); exit(1); }
    memset(t->cells, ' ', (size_t)rows * (size_t)cols);
    memset(t->attrs, 0,   (size_t)rows * (size_t)cols * sizeof(uint16_t));

    if (oldc && olda) {
        int rmin = (orows < rows) ? orows : rows;
        int cmin = (ocols < cols) ? ocols : cols;
        for (int r = 0; r < rmin; r++) {
            memcpy(t->cells + (size_t)r * (size_t)cols,
                   oldc + (size_t)r * (size_t)ocols, (size_t)cmin);
            memcpy(t->attrs + (size_t)r * (size_t)cols,
                   olda + (size_t)r * (size_t)ocols, (size_t)cmin * sizeof(uint16_t));
        }
        free(oldc);
        free(olda);
    } else {
        free(oldc);
        free(olda);
    }

    if (t->cy >= rows) t->cy = rows - 1;
    if (t->cx >= cols) t->cx = cols - 1;
    t->wrap_pending = false;

    /* clamp scroll region */
    if (t->scroll_top < 0) t->scroll_top = 0;
    if (t->scroll_bottom < 0) t->scroll_bottom = rows - 1;
    if (t->scroll_top >= rows) t->scroll_top = 0;
    if (t->scroll_bottom >= rows) t->scroll_bottom = rows - 1;
    if (t->scroll_top > t->scroll_bottom) {
        t->scroll_top = 0;
        t->scroll_bottom = rows - 1;
    }
}

static void term_fill_blank_line(TermView *t, int row) {
    if (!t || !t->cells || !t->attrs) return;
    if (row < 0 || row >= t->rows) return;
    size_t off = (size_t)row * (size_t)t->cols;
    memset(t->cells + off, ' ', (size_t)t->cols);
    for (int c = 0; c < t->cols; c++) t->attrs[off + (size_t)c] = t->cur_attr;
}

static void term_scroll_up_region(TermView *t, int top, int bottom, int n) {
    if (!t || !t->cells || !t->attrs || n <= 0) return;
    if (top < 0) top = 0;
    if (bottom >= t->rows) bottom = t->rows - 1;
    if (top > bottom) return;
    int height = bottom - top + 1;
    if (n >= height) {
        for (int r = top; r <= bottom; r++) term_fill_blank_line(t, r);
        return;
    }
    size_t row_bytes = (size_t)t->cols;
    /* move up */
    memmove(t->cells + (size_t)top * row_bytes,
            t->cells + (size_t)(top + n) * row_bytes,
            (size_t)(height - n) * row_bytes);
    memmove(t->attrs + (size_t)top * row_bytes,
            t->attrs + (size_t)(top + n) * row_bytes,
            (size_t)(height - n) * row_bytes * sizeof(uint16_t));
    for (int r = bottom - n + 1; r <= bottom; r++) term_fill_blank_line(t, r);
}

static void term_scroll_down_region(TermView *t, int top, int bottom, int n) {
    if (!t || !t->cells || !t->attrs || n <= 0) return;
    if (top < 0) top = 0;
    if (bottom >= t->rows) bottom = t->rows - 1;
    if (top > bottom) return;
    int height = bottom - top + 1;
    if (n >= height) {
        for (int r = top; r <= bottom; r++) term_fill_blank_line(t, r);
        return;
    }
    size_t row_bytes = (size_t)t->cols;
    /* move down */
    memmove(t->cells + (size_t)(top + n) * row_bytes,
            t->cells + (size_t)top * row_bytes,
            (size_t)(height - n) * row_bytes);
    memmove(t->attrs + (size_t)(top + n) * row_bytes,
            t->attrs + (size_t)top * row_bytes,
            (size_t)(height - n) * row_bytes * sizeof(uint16_t));
    for (int r = top; r < top + n; r++) term_fill_blank_line(t, r);
}

static void term_scroll_up(TermView *t, int n) {
    /* full screen scroll */
    term_scroll_up_region(t, 0, t ? (t->rows - 1) : 0, n);
}

static void term_erase_all_keep_modes(TermView *t) {
    if (!t || !t->cells || !t->attrs) return;
    memset(t->cells, ' ', (size_t)t->rows * (size_t)t->cols);
    for (size_t i = 0, N = (size_t)t->rows * (size_t)t->cols; i < N; i++) t->attrs[i] = t->cur_attr;
}

static void term_get_region(TermView *t, int *top, int *bottom) {
    int tt = 0, bb = 0;
    if (t) {
        tt = t->scroll_top;
        bb = t->scroll_bottom;
    }
    if (!t || tt < 0 || bb < 0 || tt >= t->rows || bb >= t->rows || tt > bb) {
        tt = 0;
        bb = t ? (t->rows - 1) : 0;
    }
    if (top) *top = tt;
    if (bottom) *bottom = bb;
}

static void term_insert_lines(TermView *t, int n) {
    if (!t || !t->cells || !t->attrs) return;
    if (n <= 0) n = 1;
    int top, bottom;
    term_get_region(t, &top, &bottom);
    if (t->cy < top || t->cy > bottom) return;
    int maxn = bottom - t->cy + 1;
    if (n > maxn) n = maxn;

    /* shift down within region: [cy..bottom-n] -> [cy+n..bottom] */
    for (int r = bottom; r >= t->cy + n; r--) {
        size_t dst = (size_t)r * (size_t)t->cols;
        size_t src = (size_t)(r - n) * (size_t)t->cols;
        memcpy(t->cells + dst, t->cells + src, (size_t)t->cols);
        memcpy(t->attrs + dst, t->attrs + src, (size_t)t->cols * sizeof(uint16_t));
    }
    for (int r = t->cy; r < t->cy + n; r++) term_fill_blank_line(t, r);
}

static void term_delete_lines(TermView *t, int n) {
    if (!t || !t->cells || !t->attrs) return;
    if (n <= 0) n = 1;
    int top, bottom;
    term_get_region(t, &top, &bottom);
    if (t->cy < top || t->cy > bottom) return;
    int maxn = bottom - t->cy + 1;
    if (n > maxn) n = maxn;

    /* shift up within region: [cy+n..bottom] -> [cy..bottom-n] */
    for (int r = t->cy; r <= bottom - n; r++) {
        size_t dst = (size_t)r * (size_t)t->cols;
        size_t src = (size_t)(r + n) * (size_t)t->cols;
        memcpy(t->cells + dst, t->cells + src, (size_t)t->cols);
        memcpy(t->attrs + dst, t->attrs + src, (size_t)t->cols * sizeof(uint16_t));
    }
    for (int r = bottom - n + 1; r <= bottom; r++) term_fill_blank_line(t, r);
}

static void term_insert_chars(TermView *t, int n) {
    if (!t || !t->cells || !t->attrs) return;
    if (n <= 0) n = 1;
    if (t->cx < 0) t->cx = 0;
    if (t->cx >= t->cols) return;
    if (n > t->cols - t->cx) n = t->cols - t->cx;
    size_t row = (size_t)t->cy * (size_t)t->cols;

    for (int c = t->cols - 1; c >= t->cx + n; c--) {
        t->cells[row + (size_t)c] = t->cells[row + (size_t)(c - n)];
        t->attrs[row + (size_t)c] = t->attrs[row + (size_t)(c - n)];
    }
    for (int c = t->cx; c < t->cx + n; c++) {
        t->cells[row + (size_t)c] = ' ';
        t->attrs[row + (size_t)c] = t->cur_attr;
    }
}

static void term_delete_chars(TermView *t, int n) {
    if (!t || !t->cells || !t->attrs) return;
    if (n <= 0) n = 1;
    if (t->cx < 0) t->cx = 0;
    if (t->cx >= t->cols) return;
    if (n > t->cols - t->cx) n = t->cols - t->cx;
    size_t row = (size_t)t->cy * (size_t)t->cols;

    for (int c = t->cx; c < t->cols - n; c++) {
        t->cells[row + (size_t)c] = t->cells[row + (size_t)(c + n)];
        t->attrs[row + (size_t)c] = t->attrs[row + (size_t)(c + n)];
    }
    for (int c = t->cols - n; c < t->cols; c++) {
        t->cells[row + (size_t)c] = ' ';
        t->attrs[row + (size_t)c] = t->cur_attr;
    }
}

static void term_erase_chars(TermView *t, int n) {
    if (!t || !t->cells || !t->attrs) return;
    if (n <= 0) n = 1;
    if (t->cx < 0) t->cx = 0;
    if (t->cx >= t->cols) return;
    if (n > t->cols - t->cx) n = t->cols - t->cx;
    size_t row = (size_t)t->cy * (size_t)t->cols;
    for (int c = 0; c < n; c++) {
        t->cells[row + (size_t)(t->cx + c)] = ' ';
        t->attrs[row + (size_t)(t->cx + c)] = t->cur_attr;
    }
}

static void term_lf(TermView *t) {
    if (!t) return;
    t->wrap_pending = false;
    int top = t->scroll_top;
    int bottom = t->scroll_bottom;
    if (top < 0 || bottom < 0 || top >= t->rows || bottom >= t->rows || top > bottom) {
        top = 0;
        bottom = t->rows - 1;
    }

    if (t->cy == bottom) {
        term_scroll_up_region(t, top, bottom, 1);
        t->cy = bottom;
    } else {
        t->cy++;
        if (t->cy >= t->rows) t->cy = t->rows - 1;
    }
}

static void term_ri(TermView *t) {
    if (!t) return;
    t->wrap_pending = false;
    int top = t->scroll_top;
    int bottom = t->scroll_bottom;
    if (top < 0 || bottom < 0 || top >= t->rows || bottom >= t->rows || top > bottom) {
        top = 0;
        bottom = t->rows - 1;
    }

    if (t->cy == top) {
        term_scroll_down_region(t, top, bottom, 1);
        t->cy = top;
    } else {
        t->cy--;
        if (t->cy < 0) t->cy = 0;
    }
}

static void term_put_ch(TermView *t, char ch) {
    if (!t || !t->cells || !t->attrs) return;

    if (t->cx < 0) t->cx = 0;
    if (t->cy < 0) t->cy = 0;

    /* VT100 autowrap:
     * When a character is written in the last column, the cursor does not
     * immediately advance to the next line. Instead, a "wrap pending" flag is
     * set and the *next* printable character triggers the line wrap.
     *
     * Many ncurses apps (top/htop) write full-width lines and also emit explicit
     * cursor moves / linefeeds. If we wrap immediately, we end up skipping a
     * line and showing "blank lines" between rows. */
    if (t->wrap_pending) {
        t->wrap_pending = false;
        t->cx = 0;
        t->cy++;
        if (t->cy >= t->rows) {
            term_scroll_up(t, 1);
            t->cy = t->rows - 1;
        }
    }

    if (t->cx >= t->cols) t->cx = t->cols - 1;
    if (t->cy >= t->rows) {
        term_scroll_up(t, 1);
        t->cy = t->rows - 1;
    }

    size_t idx = (size_t)t->cy * (size_t)t->cols + (size_t)t->cx;
    t->cells[idx] = ch;
    t->attrs[idx] = (uint16_t)(t->cur_attr | (term_is_acs(t) ? TVA_ACS : 0));

    if (t->cx == t->cols - 1) {
        t->wrap_pending = true;
        /* keep cx at last column */
    } else {
        t->cx++;
    }
}

static int csi_get_int(const char *s, int *i, int end) {
    int v = 0, any = 0;
    while (*i < end && s[*i] && isdigit((unsigned char)s[*i])) { v = v * 10 + (s[*i] - '0'); (*i)++; any = 1; }
    return any ? v : -1;
}

static void term_clear_line_from(TermView *t, int from_x) {
    if (!t || !t->cells || !t->attrs) return;
    if (from_x < 0) from_x = 0;
    if (from_x >= t->cols) return;
    size_t off = (size_t)t->cy * (size_t)t->cols + (size_t)from_x;
    size_t n = (size_t)(t->cols - from_x);
    memset(t->cells + off, ' ', n);
    for (size_t k = 0; k < n; k++) t->attrs[off + k] = t->cur_attr;
}

static void term_clear_line_to(TermView *t, int to_x) {
    if (!t || !t->cells || !t->attrs) return;
    if (to_x < 0) return;
    if (to_x >= t->cols) to_x = t->cols - 1;
    size_t off = (size_t)t->cy * (size_t)t->cols;
    size_t n = (size_t)(to_x + 1);
    memset(t->cells + off, ' ', n);
    for (size_t k = 0; k < n; k++) t->attrs[off + k] = t->cur_attr;
}

static void term_clear_screen_from(TermView *t) {
    if (!t || !t->cells || !t->attrs) return;
    term_clear_line_from(t, t->cx);
    for (int r = t->cy + 1; r < t->rows; r++) {
        memset(t->cells + (size_t)r * (size_t)t->cols, ' ', (size_t)t->cols);
        for (int c = 0; c < t->cols; c++) t->attrs[(size_t)r * (size_t)t->cols + (size_t)c] = t->cur_attr;
    }
}

static void term_clear_screen_to(TermView *t) {
    if (!t || !t->cells || !t->attrs) return;
    for (int r = 0; r < t->cy; r++) {
        memset(t->cells + (size_t)r * (size_t)t->cols, ' ', (size_t)t->cols);
        for (int c = 0; c < t->cols; c++) t->attrs[(size_t)r * (size_t)t->cols + (size_t)c] = t->cur_attr;
    }
    term_clear_line_to(t, t->cx);
}

static int term_rgb_to_ansi8(int r, int g, int b) {
    /* 近似映射到 8 色：0 black,1 red,2 green,3 yellow,4 blue,5 magenta,6 cyan,7 white */
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;

    int maxc = r; if (g > maxc) maxc = g; if (b > maxc) maxc = b;
    int minc = r; if (g < minc) minc = g; if (b < minc) minc = b;
    int avg  = (r + g + b) / 3;

    if (maxc < 60) return 0;                 /* 很暗：黑 */
    if (minc > 210) return 7;                /* 很亮：白 */
    if ((maxc - minc) < 20) return (avg > 140) ? 7 : 0; /* 近灰 */

    bool rh = (r > 160), gh = (g > 160), bh = (b > 160);
    if (rh && gh && bh) return 7;
    if (rh && gh && !bh) return 3;
    if (rh && !gh && bh) return 5;
    if (!rh && gh && bh) return 6;
    if (rh && !gh && !bh) return 1;
    if (!rh && gh && !bh) return 2;
    if (!rh && !gh && bh) return 4;

    /* fallback: 选最大通道 */
    if (r >= g && r >= b) return 1;
    if (g >= r && g >= b) return 2;
    return 4;
}

static int term_xterm256_to_ansi8(int n) {
    if (n < 0) return 7;
    if (n < 8) return n;
    if (n < 16) return n - 8; /* bright -> 近似为基础色 */
    if (n >= 232 && n <= 255) {
        int level = 8 + (n - 232) * 10; /* 8..238 */
        return (level > 128) ? 7 : 0;
    }
    if (n >= 16 && n <= 231) {
        int x = n - 16;
        int rr = x / 36;
        int gg = (x % 36) / 6;
        int bb = x % 6;
        int r = rr * 51;
        int g = gg * 51;
        int b = bb * 51;
        return term_rgb_to_ansi8(r, g, b);
    }
    return 7;
}

static void term_apply_sgr(TermView *t, int code) {
    if (!t) return;

    /* reset */
    if (code == 0) { t->cur_attr = 0; return; }

    /* basic attrs */
    switch (code) {
        case 1:  t->cur_attr |= TVA_BOLD; break;
        case 2:  t->cur_attr |= TVA_DIM; break;
        case 4:  t->cur_attr |= TVA_UNDERLINE; break;
        case 7:  t->cur_attr |= TVA_REVERSE; break;
        case 22: t->cur_attr &= (uint16_t)~(TVA_BOLD | TVA_DIM); break;
        case 24: t->cur_attr &= (uint16_t)~TVA_UNDERLINE; break;
        case 27: t->cur_attr &= (uint16_t)~TVA_REVERSE; break;
        default: break;
    }

    /* 8-color fg/bg */
    if (code >= 30 && code <= 37) {
        int idx = code - 30;
        TVA_FG_SET(t->cur_attr, idx + 1);
        return;
    }
    if (code == 39) { TVA_FG_SET(t->cur_attr, 0); return; }

    if (code >= 40 && code <= 47) {
        int idx = code - 40;
        TVA_BG_SET(t->cur_attr, idx + 1);
        return;
    }
    if (code == 49) { TVA_BG_SET(t->cur_attr, 0); return; }

    /* bright fg/bg：近似映射到基础色，并加粗模拟“亮” */
    if (code >= 90 && code <= 97) {
        int idx = code - 90;
        TVA_FG_SET(t->cur_attr, idx + 1);
        t->cur_attr |= TVA_BOLD;
        return;
    }
    if (code >= 100 && code <= 107) {
        int idx = code - 100;
        TVA_BG_SET(t->cur_attr, idx + 1);
        return;
    }
}

static void term_handle_csi(TermView *t, const char *seq, int len) {
    if (!t || !seq || len <= 0) return;
    char final = seq[len - 1];
    int end = len - 1;

    bool priv = false;
    int i = 0;
    if (seq[0] == '?') { priv = true; i = 1; }

    if (final != 'm') t->wrap_pending = false;

    if (final == 'm') {
        /* SGR: 支持多参数 + 颜色（30/40/90/100 + 38;5;n / 48;5;n / 38;2;r;g;b） */
        if (i >= end) { term_apply_sgr(t, 0); return; } /* "m" */
        while (i <= end) {
            int v = csi_get_int(seq, &i, end);
            if (v < 0) v = 0; /* ";m" or empty -> 0 */

            if (v == 38 || v == 48) {
                bool is_fg = (v == 38);
                if (i < end && seq[i] == ';') i++;
                int mode = csi_get_int(seq, &i, end);
                if (mode < 0) mode = 0;

                if (mode == 5) {
                    if (i < end && seq[i] == ';') i++;
                    int ncol = csi_get_int(seq, &i, end);
                    if (ncol < 0) ncol = 0;
                    int idx = term_xterm256_to_ansi8(ncol);
                    if (is_fg) TVA_FG_SET(t->cur_attr, idx + 1);
                    else       TVA_BG_SET(t->cur_attr, idx + 1);
                } else if (mode == 2) {
                    if (i < end && seq[i] == ';') i++;
                    int r = csi_get_int(seq, &i, end);
                    if (r < 0) r = 0;
                    if (i < end && seq[i] == ';') i++;
                    int g = csi_get_int(seq, &i, end);
                    if (g < 0) g = 0;
                    if (i < end && seq[i] == ';') i++;
                    int b = csi_get_int(seq, &i, end);
                    if (b < 0) b = 0;
                    int idx = term_rgb_to_ansi8(r, g, b);
                    if (is_fg) TVA_FG_SET(t->cur_attr, idx + 1);
                    else       TVA_BG_SET(t->cur_attr, idx + 1);
                } else {
                    /* 未支持的扩展色模式：忽略 */
                }
            } else {
                term_apply_sgr(t, v);
            }

            if (i < end && seq[i] == ';') { i++; continue; }
            break;
        }
        return;
    }

    int p1 = csi_get_int(seq, &i, end);
    int p2 = -1;
    if (i < end && seq[i] == ';') { i++; p2 = csi_get_int(seq, &i, end); }

    if (p1 < 0) p1 = 0;
    if (p2 < 0) p2 = 0;

    switch (final) {
        case 'H':
        case 'f': {
            int row = (p1 == 0) ? 1 : p1;
            int col = (p2 == 0) ? 1 : p2;
            t->cy = row - 1;
            t->cx = col - 1;
            if (t->cy < 0) t->cy = 0;
            if (t->cx < 0) t->cx = 0;
            if (t->cy >= t->rows) t->cy = t->rows - 1;
            if (t->cx >= t->cols) t->cx = t->cols - 1;
            break;
        }
        case 'A': { int n = (p1 == 0) ? 1 : p1; t->cy -= n; if (t->cy < 0) t->cy = 0; break; }
        case 'B': { int n = (p1 == 0) ? 1 : p1; t->cy += n; if (t->cy >= t->rows) t->cy = t->rows - 1; break; }
        case 'C': { int n = (p1 == 0) ? 1 : p1; t->cx += n; if (t->cx >= t->cols) t->cx = t->cols - 1; break; }
        case 'D': { int n = (p1 == 0) ? 1 : p1; t->cx -= n; if (t->cx < 0) t->cx = 0; break; }
        case 'G': { int col = (p1 == 0) ? 1 : p1; t->cx = col - 1; if (t->cx < 0) t->cx = 0; if (t->cx >= t->cols) t->cx = t->cols - 1; break; }
        case 'd': { int row = (p1 == 0) ? 1 : p1; t->cy = row - 1; if (t->cy < 0) t->cy = 0; if (t->cy >= t->rows) t->cy = t->rows - 1; break; }
        case 'J': {
            int n = p1;
            /* ED: do NOT reset modes/state (ncurses relies on this). */
            if (n == 2) term_erase_all_keep_modes(t);
            else if (n == 0) term_clear_screen_from(t);
            else if (n == 1) term_clear_screen_to(t);
            break;
        }
        case 'K': {
            int n = p1;
            /* EL: do NOT move cursor. */
            if (n == 2) { term_clear_line_from(t, 0); }
            else if (n == 0) term_clear_line_from(t, t->cx);
            else if (n == 1) term_clear_line_to(t, t->cx);
            break;
        }
        case 'r': {
            /* DECSTBM: set scrolling region (top/bottom, inclusive). */
            int top = (p1 == 0) ? 1 : p1;
            int bot = (p2 == 0) ? t->rows : p2;
            if (top < 1) top = 1;
            if (bot < 1) bot = 1;
            if (top > t->rows) top = t->rows;
            if (bot > t->rows) bot = t->rows;
            if (top >= bot) {
                t->scroll_top = 0;
                t->scroll_bottom = t->rows - 1;
            } else {
                t->scroll_top = top - 1;
                t->scroll_bottom = bot - 1;
            }
            /* xterm/vt100 moves cursor to home after setting margins */
            t->cx = 0;
            t->cy = 0;
            break;
        }
        case 'L': {
            int n = (p1 == 0) ? 1 : p1;
            term_insert_lines(t, n);
            break;
        }
        case 'M': {
            int n = (p1 == 0) ? 1 : p1;
            term_delete_lines(t, n);
            break;
        }
        case '@': {
            int n = (p1 == 0) ? 1 : p1;
            term_insert_chars(t, n);
            break;
        }
        case 'P': {
            int n = (p1 == 0) ? 1 : p1;
            term_delete_chars(t, n);
            break;
        }
        case 'X': {
            int n = (p1 == 0) ? 1 : p1;
            term_erase_chars(t, n);
            break;
        }
        case 'S': {
            int n = (p1 == 0) ? 1 : p1;
            int top, bottom;
            term_get_region(t, &top, &bottom);
            term_scroll_up_region(t, top, bottom, n);
            break;
        }
        case 'T': {
            int n = (p1 == 0) ? 1 : p1;
            int top, bottom;
            term_get_region(t, &top, &bottom);
            term_scroll_down_region(t, top, bottom, n);
            break;
        }
        case 'E': { /* CNL */
            int n = (p1 == 0) ? 1 : p1;
            t->cy += n;
            if (t->cy >= t->rows) t->cy = t->rows - 1;
            t->cx = 0;
            break;
        }
        case 'F': { /* CPL */
            int n = (p1 == 0) ? 1 : p1;
            t->cy -= n;
            if (t->cy < 0) t->cy = 0;
            t->cx = 0;
            break;
        }
        case 's': t->saved_cx = t->cx; t->saved_cy = t->cy; break;
        case 'u': t->cx = t->saved_cx; t->cy = t->saved_cy; break;
        case 'h':
        case 'l':
            if (priv) {
                /* DECCKM: application cursor keys */
                if (p1 == 1) {
                    t->app_cursor = (final == 'h');
                }
                /* alt screen: 清空屏幕，但保持模式（DECCKM/keypad） */
                if (p1 == 1049 || p1 == 47) term_clear_screenbuf_keep_modes(t);
            }
            break;
        default: break;
    }
}

static void term_feed(TermView *t, const unsigned char *buf, int n) {
    if (!t || !buf || n <= 0) return;
    for (int k = 0; k < n; k++) {
        unsigned char ch = buf[k];

        /* OSC: ESC ] ... (BEL or ESC \) */
        if (t->esc_state == 3) {
            if (ch == 0x07) { t->esc_state = 0; t->osc_esc_seen = false; continue; }
            if (t->osc_esc_seen && ch == '\\') { t->esc_state = 0; t->osc_esc_seen = false; continue; }
            t->osc_esc_seen = (ch == 0x1b);
            continue;
        }

        if (t->esc_state == 0) {
            if (ch == 0x1b) { t->esc_state = 1; t->esc_len = 0; continue; }
            /* VT100: shift out/in (switch GL between G1/G0) */
            if (ch == 0x0e) { t->use_g1 = true; continue; }  /* SO */
            if (ch == 0x0f) { t->use_g1 = false; continue; } /* SI */
            if (ch == '\r') { t->cx = 0; t->wrap_pending = false; continue; }
            if (ch == '\n') { term_lf(t); continue; }
            if (ch == '\b') { t->wrap_pending = false; if (t->cx > 0) t->cx--; continue; }
            if (ch == '\t') {
                t->wrap_pending = false;
                int next = ((t->cx / 8) + 1) * 8;
                if (next >= t->cols) next = t->cols - 1;
                t->cx = next;
                continue;
            }
            if (ch == 0x07) { continue; }
            if (ch < 0x20) { continue; }
            term_put_ch(t, (char)ch);
            continue;
        }

        if (t->esc_state == 1) {
            if (ch == '[') { t->esc_state = 2; t->esc_len = 0; continue; }
            if (ch == ']') { t->esc_state = 3; t->osc_esc_seen = false; continue; }
            if (ch == '(' || ch == ')') { t->esc_state = 4; t->esc_buf[0] = (char)ch; continue; }
            if (ch == 'c') { term_clear_all(t); t->wrap_pending = false; t->esc_state = 0; continue; }
            if (ch == '7') { t->wrap_pending = false; t->saved_cx = t->cx; t->saved_cy = t->cy; t->esc_state = 0; continue; }
            if (ch == '8') { t->wrap_pending = false; t->cx = t->saved_cx; t->cy = t->saved_cy; t->esc_state = 0; continue; }
            /* common single-char escapes used by ncurses apps */
            if (ch == 'D') { /* IND */ term_lf(t); t->esc_state = 0; continue; }
            if (ch == 'M') { /* RI */  term_ri(t); t->esc_state = 0; continue; }
            if (ch == 'E') { /* NEL */ t->cx = 0; term_lf(t); t->esc_state = 0; continue; }
            if (ch == '=') { /* keypad application mode */ t->app_keypad = true; t->esc_state = 0; continue; }
            if (ch == '>') { /* keypad numeric mode */      t->app_keypad = false; t->esc_state = 0; continue; }
            t->esc_state = 0;
            continue;
        }

        if (t->esc_state == 4) {
            /* ESC ( X  /  ESC ) X : designate G0/G1 charset */
            uint8_t *dst = (t->esc_buf[0] == '(') ? &t->g0_charset : &t->g1_charset;
            if (ch == '0') *dst = 1;       /* line drawing */
            else if (ch == 'B') *dst = 0;  /* US ASCII */
            else if (ch == 'U') *dst = 0;  /* (often) UK -> treat as ASCII */
            else if (ch == 'K') *dst = 0;  /* (often) German -> ASCII */
            t->esc_state = 0;
            continue;
        }

        if (t->esc_state == 2) {
            if (t->esc_len < (int)sizeof(t->esc_buf) - 1) t->esc_buf[t->esc_len++] = (char)ch;
            if (ch >= '@' && ch <= '~') {
                t->esc_buf[t->esc_len] = 0;
                term_handle_csi(t, t->esc_buf, t->esc_len);
                t->esc_state = 0;
                t->esc_len = 0;
            }
            continue;
        }
    }
}

static short term_pair_ids[16][16];
static short term_next_pair_id = 10; /* 1..3 已被 UI 使用 */

static short term_get_pair_id(int fg, int bg) {
    /* fg/bg: 0=default, 1..8=ansi8+1 */
    if (!has_colors()) return 0;
    if (fg < 0) fg = 0;
    if (fg > 15) fg = 15;
    if (bg < 0) bg = 0;
    if (bg > 15) bg = 15;

    if (fg == 0 && bg == 0) return 0;
    short pid = term_pair_ids[fg][bg];
    if (pid != 0) return pid;

    if (term_next_pair_id >= COLOR_PAIRS) return 0;
    short c_fg = (fg == 0) ? -1 : (short)(fg - 1);
    short c_bg = (bg == 0) ? -1 : (short)(bg - 1);
    init_pair(term_next_pair_id, c_fg, c_bg);
    pid = term_next_pair_id++;
    term_pair_ids[fg][bg] = pid;
    return pid;
}

static attr_t term_attr_to_curses(uint16_t a) {
    attr_t r = 0;

    int fg = TVA_FG_GET(a);
    int bg = TVA_BG_GET(a);
    short pid = term_get_pair_id(fg, bg);
    if (pid) r |= COLOR_PAIR(pid);

    if (a & TVA_REVERSE)   r |= A_REVERSE;
    if (a & TVA_BOLD)      r |= A_BOLD;
    if (a & TVA_UNDERLINE) r |= A_UNDERLINE;
#ifdef A_DIM
    if (a & TVA_DIM)       r |= A_DIM;
#endif
    return r;
}

static chtype term_acs_map(unsigned char ch) {
    switch (ch) {
        case 'q': return ACS_HLINE;
        case 'x': return ACS_VLINE;
        case 'l': return ACS_ULCORNER;
        case 'k': return ACS_URCORNER;
        case 'm': return ACS_LLCORNER;
        case 'j': return ACS_LRCORNER;
        case 't': return ACS_LTEE;
        case 'u': return ACS_RTEE;
        case 'v': return ACS_BTEE;
        case 'w': return ACS_TTEE;
        case 'n': return ACS_PLUS;
        case 'a': return ACS_CKBOARD;
        case '`': return ACS_DIAMOND;
        case 'f': return ACS_DEGREE;
        case 'g': return ACS_PLMINUS;
        case '~': return ACS_BULLET;
        default:  return (chtype)ch;
    }
}

static void term_draw(WINDOW *win, TermView *t) {
    if (!win || !t || !t->cells || !t->attrs) return;
    int H, W;
    getmaxyx(win, H, W);
    int rows = (t->rows < H) ? t->rows : H;
    int cols = (t->cols < W) ? t->cols : W;

    for (int r = 0; r < rows; r++) {
        attr_t prev = (attr_t)~0;
        int c = 0;
        while (c < cols) {
            uint16_t a = t->attrs[(size_t)r * (size_t)t->cols + (size_t)c];
            int start = c;
            while (c < cols && t->attrs[(size_t)r * (size_t)t->cols + (size_t)c] == a) c++;
            attr_t ca = term_attr_to_curses(a);
            if (ca != prev) { wattrset(win, ca); prev = ca; }
            if (a & TVA_ACS) {
                for (int i = start; i < c; i++) {
                    unsigned char ch = (unsigned char)t->cells[(size_t)r * (size_t)t->cols + (size_t)i];
                    mvwaddch(win, r, i, term_acs_map(ch));
                }
            } else {
                mvwaddnstr(win, r, start, t->cells + (size_t)r * (size_t)t->cols + (size_t)start, c - start);
            }
        }
        wattrset(win, 0);
        if (cols < W) for (int cc = cols; cc < W; cc++) mvwaddch(win, r, cc, ' ');
    }
    for (int r = rows; r < H; r++) {
        for (int c = 0; c < W; c++) mvwaddch(win, r, c, ' ');
    }
}

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int open_pty_master(void) {
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0) return -1;
    if (grantpt(m) != 0) { close(m); return -1; }
    if (unlockpt(m) != 0) { close(m); return -1; }
    return m;
}

void hot_init(HotPopup *p) {
    memset(p, 0, sizeof(*p));
    p->master_fd = -1;
}

static void hot_kill_child(HotPopup *p) {
    if (!p) return;
    if (p->running && p->pid > 0) {
        kill(p->pid, SIGTERM);
        int st = 0;
        for (int i = 0; i < 50; i++) {
            pid_t r = waitpid(p->pid, &st, WNOHANG);
            if (r == p->pid) break;
            sleep_ms(10);
        }
        waitpid(p->pid, &st, WNOHANG);
    }
    p->running = false;
    p->pid = -1;
    if (p->master_fd >= 0) { close(p->master_fd); p->master_fd = -1; }
    term_free(&p->term);
    p->raw_len = 0;
}

void hot_close(HotPopup *p) {
    if (!p) return;
    hot_kill_child(p);
    if (p->wi) { delwin(p->wi); p->wi = NULL; }
    if (p->wb) { delwin(p->wb); p->wb = NULL; }
    p->active = false;
    p->mode = HOT_INPUT;
    p->owner = NULL;
    p->y = p->x = p->h = p->w = 0;
    p->in_len = 0;
    p->input[0] = 0;
    curs_set(0);
}

bool hot_set_geom(HotPopup *p, int y, int x, int h, int w) {
    if (!p) return false;
    if (h < 3) h = 3;
    if (w < 10) w = 10;

    bool geom_changed = (h != p->h) || (w != p->w) || (y != p->y) || (x != p->x);
    bool need_recreate = (!p->wb || !p->wi) || (h != p->h) || (w != p->w);

    p->y = y; p->x = x; p->h = h; p->w = w;

    if (need_recreate) {
        if (p->wi) { delwin(p->wi); p->wi = NULL; }
        if (p->wb) { delwin(p->wb); p->wb = NULL; }
        p->wb = newwin(h, w, y, x);
        p->wi = derwin(p->wb, h - 2, w - 2, 1, 1);
    } else {
        mvwin(p->wb, y, x);
        wresize(p->wb, h, w);
    }

    if (p->mode == HOT_TERM) {
        int ih, iw;
        getmaxyx(p->wi, ih, iw);
        int oldr = p->term.rows;
        int oldc = p->term.cols;
        term_resize(&p->term, ih, iw);
        bool resized = (ih != oldr || iw != oldc);
        /* When the viewport changes, clear local screen buffer so the next
         * redraw from ncurses apps (e.g. htop) does not mix with stale cells.
         * Keep terminal modes (DECCKM/keypad) intact for correct key mapping. */
        if (resized) {
            term_clear_screenbuf_keep_modes(&p->term);
        }
        if (resized && p->master_fd >= 0 && p->pid > 0) {
            struct winsize wsz;
            memset(&wsz, 0, sizeof(wsz));
            wsz.ws_row = (unsigned short)ih;
            wsz.ws_col = (unsigned short)iw;
            ioctl(p->master_fd, TIOCSWINSZ, &wsz);
            kill(p->pid, SIGWINCH);
        }
    }
    return geom_changed;
}

static void hot_raw_append(HotPopup *p, const unsigned char *buf, int n) {
    if (!p || !buf || n <= 0) return;
    const int cap = (int)sizeof(p->raw_tail);
    if (n >= cap) {
        memcpy(p->raw_tail, buf + (n - cap), (size_t)cap);
        p->raw_len = cap;
        return;
    }
    if (p->raw_len + n > cap) {
        int keep = cap / 2;
        if (keep < 1024) keep = 1024;
        if (p->raw_len > keep) {
            memmove(p->raw_tail, p->raw_tail + (p->raw_len - keep), (size_t)keep);
            p->raw_len = keep;
        }
    }
    int can = cap - p->raw_len;
    if (n > can) n = can;
    memcpy(p->raw_tail + p->raw_len, buf, (size_t)n);
    p->raw_len += n;
}

static bool hot_cmd_is_fzy(const char *cmd) {
    return (cmd && strstr(cmd, "fzy") != NULL);
}

static void strip_ansi_to_plain(const unsigned char *in, int n, char *out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!in || n <= 0) return;

    size_t w = 0;
    int st = 0; /* 0 normal, 1 ESC, 2 CSI, 3 OSC, 4 ESC_CHARSET(skip 1) */
    bool osc_esc = false;

    for (int i = 0; i < n; i++) {
        unsigned char ch = in[i];

        if (st == 3) { /* OSC */
            if (ch == 0x07) { st = 0; osc_esc = false; continue; }
            if (osc_esc && ch == '\\') { st = 0; osc_esc = false; continue; }
            osc_esc = (ch == 0x1b);
            continue;
        }

        if (st == 2) { /* CSI */
            if (ch >= '@' && ch <= '~') { st = 0; }
            continue;
        }

        if (st == 4) { st = 0; continue; }

        if (st == 1) { /* ESC */
            if (ch == '[') { st = 2; continue; }
            if (ch == ']') { st = 3; osc_esc = false; continue; }
            if (ch == '(' || ch == ')') { st = 4; continue; } /* ESC ( B / ESC ) 0 */
            st = 0;
            continue;
        }

        if (ch == 0x1b) { st = 1; continue; }
        if (ch == '\r') ch = '\n';

        if ((ch == '\n') || (ch >= 0x20 && ch != 0x7f)) {
            if (w + 1 < cap) out[w++] = (char)ch;
        }
    }
    out[w] = 0;
}

static bool last_nonempty_line(const char *plain, char *out, size_t cap) {
    if (!out || cap == 0) return false;
    out[0] = 0;
    if (!plain) return false;

    size_t n = strlen(plain);
    if (n == 0) return false;

    /* 从尾部找最后一个非空行 */
    size_t i = n;
    while (i > 0 && (plain[i-1] == '\n' || plain[i-1] == ' ' || plain[i-1] == '\t')) i--;
    if (i == 0) return false;

    size_t end = i;
    while (i > 0 && plain[i-1] != '\n') i--;
    size_t start = i;

    /* trim */
    while (start < end && (plain[start] == ' ' || plain[start] == '\t')) start++;
    while (end > start && (plain[end-1] == ' ' || plain[end-1] == '\t')) end--;

    size_t len = end - start;
    if (len == 0) return false;
    if (len >= cap) len = cap - 1;
    memcpy(out, plain + start, len);
    out[len] = 0;

    /* 去掉可能的前缀 "> " */
    if (out[0] == '>' && out[1] == ' ') {
        memmove(out, out + 2, strlen(out + 2) + 1);
    }
    return out[0] != 0;
}

static bool hot_spawn(HotPopup *p, const char *cmd) {
    if (!p || !cmd || !*cmd) return false;

    int ih, iw;
    getmaxyx(p->wi, ih, iw);
    if (ih < 1) ih = 1;
    if (iw < 1) iw = 1;

    int master = open_pty_master();
    if (master < 0) return false;
    char *slave_name = ptsname(master);
    if (!slave_name) { close(master); return false; }

    int slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0) { close(master); return false; }

    struct winsize wsz;
    memset(&wsz, 0, sizeof(wsz));
    wsz.ws_row = (unsigned short)ih;
    wsz.ws_col = (unsigned short)iw;
    ioctl(slave, TIOCSWINSZ, &wsz);

    pid_t pid = fork();
    if (pid < 0) {
        close(slave); close(master);
        return false;
    }
    if (pid == 0) {
        setsid();
        ioctl(slave, TIOCSCTTY, 0);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        close(slave);
        close(master);

        setenv("TERM", "xterm-256color", 1);
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", iw);
        setenv("COLUMNS", buf, 1);
        snprintf(buf, sizeof(buf), "%d", ih);
        setenv("LINES", buf, 1);

        execl("/bin/sh", "sh", "-lc", cmd, (char*)NULL);
        _exit(127);
    }

    close(slave);
    set_nonblock(master);

    hot_kill_child(p);
    p->master_fd = master;
    p->pid = pid;
    p->running = true;
    p->mode = HOT_TERM;
    p->raw_len = 0;

    term_init(&p->term, ih, iw);
    term_clear_all(&p->term);

    ioctl(master, TIOCSWINSZ, &wsz);
    kill(pid, SIGWINCH);
    return true;
}

bool hot_start_cmd(HotPopup *p, const char *cmd) {
    if (!p || !cmd) return false;

    /* 复制到 input，用于 fzy 检测与结果回填 */
    size_t n = strlen(cmd);
    if (n >= sizeof(p->input)) n = sizeof(p->input) - 1;
    memcpy(p->input, cmd, n);
    p->input[n] = 0;
    p->in_len = (int)n;

    return hot_spawn(p, p->input);
}


void hot_draw(HotPopup *p) {
    if (!p || !p->active || !p->wb || !p->wi) return;
    werase(p->wb);
    box(p->wb, 0, 0);

    if (p->mode == HOT_INPUT) {
        char nb[256];
        const char *nm = p->owner ? node_view_name(p->owner, nb, sizeof(nb)) : "";
        char title[300];
        snprintf(title, sizeof(title), " Hot: %s ", nm);
        mvwaddnstr(p->wb, 0, 2, title, p->w - 4);

        mvwaddnstr(p->wi, 0, 0, "Enter=run  Ctrl+X=close  (例如: find /home/nt -type f | .../fzy)", p->w - 2);
        mvwaddnstr(p->wi, 1, 0, "> ", p->w - 2);
        mvwaddnstr(p->wi, 1, 2, p->input, p->w - 4);
        curs_set(1);
        wmove(p->wi, 1, 2 + p->in_len);

        /* Batch screen updates with doupdate() in the caller. */
        wnoutrefresh(p->wb);
        wnoutrefresh(p->wi);
        return;
    }

    curs_set(0);
    term_draw(p->wi, &p->term);
    /* Batch screen updates with doupdate() in the caller. */
    wnoutrefresh(p->wb);
    wnoutrefresh(p->wi);
}

bool hot_pump(HotPopup *p) {
    if (!p || !p->active || p->mode != HOT_TERM || p->master_fd < 0) return false;

    unsigned char buf[4096];
    bool changed = false;

    /* Avoid spending unbounded CPU time in one pump when the child (e.g. htop)
     * redraws aggressively or when resize triggers a burst of output. */
    int total = 0;
    const int max_bytes = 64 * 1024;

    /* 先尽可能读出数据 */
    while (total < max_bytes) {
        ssize_t n = read(p->master_fd, buf, sizeof(buf));
        if (n > 0) {
            hot_raw_append(p, buf, (int)n);
            term_feed(&p->term, buf, (int)n);
            changed = true;
            total += (int)n;
            continue;
        }
        if (n == 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        break;
    }

    /* 子进程是否退出 */
    if (p->running && p->pid > 0) {
        int st = 0;
        pid_t r = waitpid(p->pid, &st, WNOHANG);
        if (r == p->pid) {
            /* 退出后再 drain 一次，确保拿到最终输出（fzy 会在退出前打印选中行） */
            while (total < max_bytes) {
                ssize_t n = read(p->master_fd, buf, sizeof(buf));
                if (n > 0) {
                    hot_raw_append(p, buf, (int)n);
                    term_feed(&p->term, buf, (int)n);
                    total += (int)n;
                    continue;
                }
                if (n == 0) break;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                break;
            }

            if (hot_cmd_is_fzy(p->input) && p->owner) {
                char plain[8192];
                char sel[2048];
                strip_ansi_to_plain(p->raw_tail, p->raw_len, plain, sizeof(plain));
                if (last_nonempty_line(plain, sel, sizeof(sel))) {
                    free(p->owner->val);
                    p->owner->val = strdup(sel);
                }
                p->closed_by_enter = true;
                p->last_owner = p->owner;
                hot_close(p);
                return true;
            }

            /* 非 fzy：回到输入模式 */
            hot_kill_child(p);
            p->mode = HOT_INPUT;
            return true;
        }
    }
    return changed;
}

static void hot_send_bytes(HotPopup *p, const char *s, size_t n) {
    if (!p || p->master_fd < 0 || !s || n == 0) return;
    if (write(p->master_fd, s, n) < 0) { /* ignore */ }

}

bool hot_handle_key(HotPopup *p, int ch) {
    if (!p || !p->active) return false;

    if (p->mode == HOT_INPUT) {
        if (ch == 24) { p->closed_by_enter = false; p->last_owner = NULL; hot_close(p); return true; } /* Ctrl+X */
        if (ch == 27) { p->closed_by_enter = false; p->last_owner = NULL; hot_close(p); return true; }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            p->input[p->in_len] = 0;
            if (p->in_len > 0) {
                const char *cmd = p->input;
                while (*cmd && isspace((unsigned char)*cmd)) cmd++;
                if (*cmd) hot_spawn(p, cmd);
            }
            return true;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (p->in_len > 0) { p->in_len--; p->input[p->in_len] = 0; }
            return true;
        }
        if (ch >= 32 && ch <= 126) {
            if (p->in_len + 1 < (int)sizeof(p->input)) {
                p->input[p->in_len++] = (char)ch;
                p->input[p->in_len] = 0;
            }
            return true;
        }
        return false;
    }

    /* HOT_TERM：把按键转发给子进程（fzy/top 都需要） */
    if (ch == KEY_RESIZE) {
        /* Let the main loop handle ncurses resize bookkeeping. */
        return false;
    }
    if (ch == 24) { p->closed_by_enter = false; p->last_owner = NULL; hot_close(p); return true; } /* Ctrl+X 强制关闭 */
    if (ch == 27) {
        /* ESC 透传给子进程（fzy 需要 ESC 退出/取消；也更像正常终端） */
        char c = 0x1b; hot_send_bytes(p, &c, 1); return true;
    }

    const bool app = p->term.app_cursor;

    switch (ch) {
        case KEY_UP:    hot_send_bytes(p, app ? "\x1bOA" : "\x1b[A", 3); return true;
        case KEY_DOWN:  hot_send_bytes(p, app ? "\x1bOB" : "\x1b[B", 3); return true;
        case KEY_RIGHT: hot_send_bytes(p, app ? "\x1bOC" : "\x1b[C", 3); return true;
        case KEY_LEFT:  hot_send_bytes(p, app ? "\x1bOD" : "\x1b[D", 3); return true;
        case KEY_HOME:  hot_send_bytes(p, app ? "\x1bOH" : "\x1b[H", 3); return true;
        case KEY_END:   hot_send_bytes(p, app ? "\x1bOF" : "\x1b[F", 3); return true;
        case KEY_PPAGE: hot_send_bytes(p, "\x1b[5~", 4); return true;
        case KEY_NPAGE: hot_send_bytes(p, "\x1b[6~", 4); return true;
        case KEY_IC:    hot_send_bytes(p, "\x1b[2~", 4); return true;
        case KEY_DC:    hot_send_bytes(p, "\x1b[3~", 4); return true;
        case KEY_BTAB:  hot_send_bytes(p, "\x1b[Z", 3); return true;
        case KEY_BACKSPACE:
        case 127:
        case 8: {
            char c = 0x7f;
            hot_send_bytes(p, &c, 1);
            return true;
        }
        case KEY_ENTER:
            hot_send_bytes(p, "\r", 1); return true;
        case '\n':
        case '\r':
            hot_send_bytes(p, "\r", 1); return true;
        default: break;
    }

    /* Function keys (xterm-256color) */
    if (ch >= KEY_F(1) && ch <= KEY_F(12)) {
        static const char *seqs[12] = {
            "\x1bOP",  "\x1bOQ",  "\x1bOR",  "\x1bOS",
            "\x1b[15~","\x1b[17~","\x1b[18~","\x1b[19~",
            "\x1b[20~","\x1b[21~","\x1b[23~","\x1b[24~"
        };
        const char *s = seqs[ch - KEY_F(1)];
        hot_send_bytes(p, s, strlen(s));
        return true;
    }

    if (ch >= 0 && ch <= 255) {
        char c = (char)ch;
        hot_send_bytes(p, &c, 1);
        return true;
    }
    return true;
}
