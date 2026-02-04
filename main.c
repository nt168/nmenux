// main.c (config.txt -> Ndx tree -> dump -> TUI)
// Build:
//   gcc -O2 -Wall -Wextra -std=c11 main.c mterm.c -o perftui -lncursesw
// Run:
//   ./perftui config.txt

#define _XOPEN_SOURCE 700
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <unistd.h>
#include "ndx.h"
#include "mterm.h"

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


typedef struct {
    Node **v;
    int n;
    int cap;
} NodeVec;

/* =========================
 *  小型字符串 HashMap
 * ========================= */
typedef struct {
    char  *key;
    void  *val;
    bool   used;
} HSlot;

typedef struct {
    HSlot *s;
    int cap;
    int n;
} HMap;

static uint64_t fnv1a(const char *s) {
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ull;
    }
    return h;
}

static void hmap_init(HMap *m, int cap) {
    if (cap < 16) cap = 16;
    int p = 1;
    while (p < cap) p <<= 1; /* 2^k */
    cap = p;

    m->cap = cap;
    m->n = 0;
    m->s = (HSlot*)calloc((size_t)m->cap, sizeof(HSlot));
    if (!m->s) { perror("calloc"); exit(1); }
}

static void hmap_free(HMap *m, bool free_keys) {
    if (!m->s) return;
    if (free_keys) {
        for (int i = 0; i < m->cap; i++) if (m->s[i].used) free(m->s[i].key);
    }
    free(m->s);
    m->s = NULL;
    m->cap = 0;
    m->n = 0;
}

static void hmap_rehash(HMap *m) {
    HMap nm;
    hmap_init(&nm, m->cap * 2);
    for (int i = 0; i < m->cap; i++) {
        if (!m->s[i].used) continue;
        uint64_t h = fnv1a(m->s[i].key);
        int mask = nm.cap - 1;
        int p = (int)(h & (uint64_t)mask);
        while (nm.s[p].used) p = (p + 1) & mask;
        nm.s[p].used = true;
        nm.s[p].key = m->s[i].key;
        nm.s[p].val = m->s[i].val;
        nm.n++;
    }
    free(m->s);
    *m = nm;
}

static void hmap_put(HMap *m, const char *key, void *val) {
    if ((m->n + 1) * 10 >= m->cap * 7) hmap_rehash(m);
    uint64_t h = fnv1a(key);
    int mask = m->cap - 1;
    int p = (int)(h & (uint64_t)mask);
    while (m->s[p].used) {
        if (strcmp(m->s[p].key, key) == 0) { m->s[p].val = val; return; }
        p = (p + 1) & mask;
    }
    m->s[p].used = true;
    m->s[p].key = strdup(key);
    m->s[p].val = val;
    m->n++;
}

static void *hmap_get(HMap *m, const char *key) {
    if (!m->s) return NULL;
    uint64_t h = fnv1a(key);
    int mask = m->cap - 1;
    int p = (int)(h & (uint64_t)mask);
    while (m->s[p].used) {
        if (strcmp(m->s[p].key, key) == 0) return m->s[p].val;
        p = (p + 1) & mask;
    }
    return NULL;
}

/* =========================
 *  ndx
 * ========================= */
typedef struct {
    Node   *root;
    NodeVec all;
    HMap    by_base;       /* base_id -> Node*(非title) */
    HMap    title_by_base; /* base_id -> Node*(title) */
} Ndx;

static void vec_push(NodeVec *a, Node *x) {
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 128;
        a->v = (Node**)realloc(a->v, (size_t)a->cap * sizeof(Node*));
        if (!a->v) { perror("realloc"); exit(1); }
    }
    a->v[a->n++] = x;
}

static char *str_trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *e = 0;
    return s;
}

static int calc_level_from_base(const char *base) {
    if (!base || !*base) return 0;
    int lvl = 1;
    for (const char *p = base; *p; p++) if (*p == '.') lvl++;
    return lvl;
}

static bool ends_with_letter(const char *s) {
    size_t n = strlen(s);
    return (n > 0) && (isalpha((unsigned char)s[n - 1]) != 0);
}

static Node *node_new(const char *id_raw, const char *name) {
    Node *n = (Node*)calloc(1, sizeof(Node));
    if (!n) { perror("calloc"); exit(1); }
    n->id_raw = strdup(id_raw ? id_raw : "");
    n->name   = strdup(name ? name : "");
    n->suffix = 0;

    if (ends_with_letter(n->id_raw)) {
        n->suffix = n->id_raw[strlen(n->id_raw) - 1];
        n->id_base = strndup(n->id_raw, strlen(n->id_raw) - 1);
    } else {
        n->id_base = strdup(n->id_raw);
    }

    n->level = calc_level_from_base(n->id_base);
    n->hidden = (n->suffix == 't') || (n->name[0] == '\0');
    n->dim = 0;
    n->di_mode = DI_DEFAULT;
    n->di_explicit = false;
    return n;
}

static void node_add_child(Node *parent, Node *child) {
    child->parent = parent;
    if (!parent->first_child) {
        parent->first_child = parent->last_child = child;
    } else {
        child->prev = parent->last_child;
        parent->last_child->next = child;
        parent->last_child = child;
    }
    parent->child_count++;
}

static void split_titles(Node *title_node) {
    if (!title_node || title_node->suffix != 't') return;
    char *tmp = strdup(title_node->name);
    if (!tmp) { perror("strdup"); exit(1); }

    int cap = 8, n = 0;
    char **arr = (char**)calloc((size_t)cap, sizeof(char*));
    if (!arr) { perror("calloc"); exit(1); }

    char *save = NULL;
    for (char *tok = strtok_r(tmp, "|", &save); tok; tok = strtok_r(NULL, "|", &save)) {
        tok = str_trim(tok);
        if (*tok == 0) continue;
        if (n == cap) {
            cap *= 2;
            arr = (char**)realloc(arr, (size_t)cap * sizeof(char*));
            if (!arr) { perror("realloc"); exit(1); }
        }
        arr[n++] = strdup(tok);
    }
    free(tmp);

    title_node->col_titles = arr;
    title_node->col_title_count = n;
}

static char *read_line(FILE *fp) {
    size_t cap = 256, n = 0;
    char *buf = (char*)malloc(cap);
    if (!buf) return NULL;
    int c;
    while ((c = fgetc(fp)) != EOF) {
        if (n + 1 >= cap) {
            cap *= 2;
            char *nb = (char*)realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        buf[n++] = (char)c;
        if (c == '\n') break;
    }
    if (n == 0 && c == EOF) { free(buf); return NULL; }
    buf[n] = 0;
    return buf;
}

/* dim line support: "2" / "3" / "di2" / "di3" */
static int parse_dim_token(const char *s) {
    if (!s) return 0;
    while (*s && isspace((unsigned char)*s)) s++;
    if (strncmp(s, "di", 2) == 0) s += 2;
    while (*s && isspace((unsigned char)*s)) s++;
    return atoi(s);
}

/* 彻底不依赖“子集显示方式/模式”标题：只要满足形如 "1.1.1.4:2" 就认 */
static bool looks_like_dim_line(const char *s) {
    if (!s || !*s) return false;
    if (strchr(s, '>')) return false;          /* item line */
    const char *colon = strchr(s, ':');
    if (!colon) return false;
    /* 左侧必须以数字开头 */
    const char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!isdigit((unsigned char)*p)) return false;
    return true;
}


/* 节点类型行支持: "1.2.2:a" / "1.2.3:b" / "1.2.5:c" */
static bool looks_like_type_line(const char *s) {
    if (!s || !*s) return false;
    if (strchr(s, '>')) return false;          /* item line */
    const char *colon = strchr(s, ':');
    if (!colon) return false;
    /* 左侧必须以数字开头（与 dim 同样要求） */
    const char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!isdigit((unsigned char)*p)) return false;

    const char *rhs = colon + 1;
    while (*rhs && isspace((unsigned char)*rhs)) rhs++;
    if (!*rhs) return false;
    char t = (char)tolower((unsigned char)*rhs);
    return (t == 'a' || t == 'b' || t == 'c');
}

/* dim entries：收集后统一应用（避免 dim 段放在前面时找不到 node） */
typedef struct { char *idb; int dim; } DimEnt;
typedef struct { DimEnt *v; int n; int cap; } DimVec;

/* node type entries：收集后统一应用（a/b/c 显示模式） */
typedef struct { char *idb; char typ; } TypeEnt;
typedef struct { TypeEnt *v; int n; int cap; } TypeVec;

static void dimvec_push(DimVec *dv, const char *idb, int dim) {
    if (dv->n == dv->cap) {
        dv->cap = dv->cap ? dv->cap * 2 : 16;
        dv->v = (DimEnt*)realloc(dv->v, (size_t)dv->cap * sizeof(DimEnt));
        if (!dv->v) { perror("realloc"); exit(1); }
    }
    dv->v[dv->n].idb = strdup(idb);
    dv->v[dv->n].dim = dim;
    dv->n++;
}

static void dimvec_free(DimVec *dv) {
    for (int i = 0; i < dv->n; i++) free(dv->v[i].idb);
    free(dv->v);
    dv->v = NULL;
    dv->n = dv->cap = 0;
}

static void typevec_push(TypeVec *tv, const char *idb, char typ) {
    if (tv->n == tv->cap) {
        tv->cap = tv->cap ? tv->cap * 2 : 16;
        tv->v = (TypeEnt*)realloc(tv->v, (size_t)tv->cap * sizeof(TypeEnt));
        if (!tv->v) { perror("realloc"); exit(1); }
    }
    tv->v[tv->n].idb = strdup(idb);
    tv->v[tv->n].typ = typ;
    tv->n++;
}

static void typevec_free(TypeVec *tv) {
    for (int i = 0; i < tv->n; i++) free(tv->v[i].idb);
    free(tv->v);
    tv->v = NULL;
    tv->n = tv->cap = 0;
}

static char parse_type_token(const char *s) {
    if (!s) return 0;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == 'a' || *s == 'b' || *s == 'c') return *s;
    return 0;
}

static void apply_dim(Ndx *ndx, const char *id_base, int dim) {
    Node *n = (Node*)hmap_get(&ndx->by_base, id_base);
    if (!n) {
        fprintf(stderr, "warn: dim config refers to missing node base id: %s\n", id_base);
        return;
    }
    if (dim == 2 || dim == 3) {
        n->dim = dim;
        n->di_mode = (dim == 2) ? DI_HOR : DI_VER;
        n->di_explicit = true;
    }
}

static void apply_type(Ndx *ndx, const char *id_base, char typ) {
    Node *n = (Node*)hmap_get(&ndx->by_base, id_base);
    if (!n) {
        fprintf(stderr, "warn: type config refers to missing node base id: %s\n", id_base);
        return;
    }
    n->x = typ;
}


static void apply_x(Ndx *ndx, const char *id_base, char x) {
    Node *n = (Node*)hmap_get(&ndx->by_base, id_base);
    if (!n) {
        fprintf(stderr, "warn: type config refers to missing node base id: %s\n", id_base);
        return;
    }
    n->x = x;
}


static void ndx_init(Ndx *ndx) {
    memset(ndx, 0, sizeof(*ndx));
    hmap_init(&ndx->by_base, 256);
    hmap_init(&ndx->title_by_base, 128);
    ndx->root = node_new("", "<ROOT>");
    ndx->root->hidden = true;
    vec_push(&ndx->all, ndx->root);
}

static void ndx_free(Ndx *ndx) {
    if (!ndx) return;
    for (int i = 0; i < ndx->all.n; i++) {
        Node *n = ndx->all.v[i];
        free(n->id_raw);
        free(n->id_base);
        free(n->name);
        free(n->val);
        if (n->col_titles) {
            for (int k = 0; k < n->col_title_count; k++) free(n->col_titles[k]);
            free(n->col_titles);
        }
        free(n);
    }
    free(ndx->all.v);
    hmap_free(&ndx->by_base, true);
    hmap_free(&ndx->title_by_base, true);
}

static bool ndx_parse_file(Ndx *ndx, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return false;
    }

    bool in_items = false;
    Node *stack[64] = {0};
    stack[0] = ndx->root;

    DimVec dims = {0};
    TypeVec types = {0};

    char *line;
    while ((line = read_line(fp)) != NULL) {
        char *s = str_trim(line);
        if (*s == 0) { free(line); continue; }

        /* 先识别 <选项> */
        if (strstr(s, "<选项>")) { in_items = true; free(line); continue; }

        /* 注释：也作为“分段标记”使用（遇到子集显示段，结束 item 解析更稳） */
        if (s[0] == '#') {
            if (strstr(s, "子集") && (strstr(s, "显示") || strstr(s, "模式") || strstr(s, "方式"))) {
                in_items = false;
            }
            free(line);
            continue;
        }

        /* dim / type 行：不依赖标题，看到 "1.2.3:2"/"1.2.3:di2" 或 "1.2.3:a" 就认 */
        if (looks_like_dim_line(s)) {
            char *colon = strchr(s, ':');
            *colon = 0;
            char *idb = str_trim(s);
            char *rhs = str_trim(colon + 1);

            int dim = parse_dim_token(rhs);
            if (dim == 2 || dim == 3) {
                dimvec_push(&dims, idb, dim);
            } else {
                char typ = parse_type_token(rhs);
                if (typ) typevec_push(&types, idb, typ);
            }

            free(line);
            continue;
        }


        /* 节点类型行：1.2.2:a / 1.2.3:b / 1.2.5:c */
        if (looks_like_type_line(s)) {
            char *colon = strchr(s, ':');
            *colon = 0;
            char *idb = str_trim(s);
            char *rhs = str_trim(colon + 1);
            char t = (char)tolower((unsigned char)rhs[0]);
            if (t == 'a' || t == 'b' || t == 'c') {
                typevec_push(&types, idb, t);
            }
            free(line);
            continue;
        }

        if (in_items) {
            char *gt = strchr(s, '>');
            if (!gt) { free(line); continue; }
            *gt = 0;
            char *id = str_trim(s);
            char *name = str_trim(gt + 1);

            Node *n = node_new(id, name);
            vec_push(&ndx->all, n);

            int lvl = n->level;
            if (lvl < 1) lvl = 1;
            if (lvl >= (int)(sizeof(stack)/sizeof(stack[0]))) lvl = (int)(sizeof(stack)/sizeof(stack[0])) - 1;

            Node *parent = stack[lvl - 1] ? stack[lvl - 1] : ndx->root;
            node_add_child(parent, n);

            stack[lvl] = n;
            for (int k = lvl + 1; k < (int)(sizeof(stack)/sizeof(stack[0])); k++) stack[k] = NULL;

            if (n->suffix == 't') {
                split_titles(n);
                hmap_put(&ndx->title_by_base, n->id_base, n);
            } else {
                if (!hmap_get(&ndx->by_base, n->id_base)) hmap_put(&ndx->by_base, n->id_base, n);
            }
        }

        free(line);
    }

    fclose(fp);

    /* 绑定 title */
    for (int i = 0; i < ndx->all.n; i++) {
        Node *n = ndx->all.v[i];
        if (!n || n->suffix == 't') continue;
        Node *t = (Node*)hmap_get(&ndx->title_by_base, n->id_base);
        if (t) n->title = t;
    }

    /* 统一应用 dim/type 配置 */
    for (int i = 0; i < dims.n; i++) apply_dim(ndx, dims.v[i].idb, dims.v[i].dim);
    for (int i = 0; i < types.n; i++) apply_type(ndx, types.v[i].idb, types.v[i].typ);
    dimvec_free(&dims);
    typevec_free(&types);
    return true;
}

/* =========================
 *  dump
 * ========================= */
static const char *sid(Node *n) { return n ? n->id_raw : "NULL"; }

/* 更“树形”的遍历打印（满足需求②：像树一样遍历打印 ndx） */
static void dump_tree(FILE *out, Node *n, int depth) {
    if (!n) return;
    if (!n->parent) {
        fprintf(out, "- %s>%s  (base=%s, dim=%d, hidden=%d, child=%d)\n",
                n->id_raw, n->name, n->id_base, n->dim, (int)n->hidden, n->child_count);
    } else {
        for (int i = 0; i < depth; i++) fputs("  ", out);
        if (n->suffix == 't') {
            fprintf(out, "- %s[t]>%s  (base=%s, hidden=%d, cols=%d)\n",
                    n->id_raw, n->name, n->id_base, (int)n->hidden, n->col_title_count);
        } else {
            fprintf(out, "- %s>%s  (base=%s, dim=%d, hidden=%d, child=%d)\n",
                    n->id_raw, n->name, n->id_base, n->dim, (int)n->hidden, n->child_count);
        }
    }

    for (Node *c = n->first_child; c; c = c->next) dump_tree(out, c, depth + 1);
}

/* 更“链式/结构体级别”的调试打印（满足需求③：打印复杂链式结构） */
static void dump_debug(FILE *out, Node *n, int depth) {
    if (!n) return;
    for (int i = 0; i < depth; i++) fprintf(out, "  ");
    fprintf(out, "- id_raw=%s base=%s suffix=%c level=%d dim=%d hidden=%d name=\"%s\"\n",
            n->id_raw, n->id_base, n->suffix ? n->suffix : '0', n->level, n->dim, (int)n->hidden, n->name);

    for (int i = 0; i < depth; i++) fprintf(out, "  ");
    fprintf(out, "  parent=%s prev=%s next=%s first_child=%s last_child=%s child_count=%d\n",
            sid(n->parent), sid(n->prev), sid(n->next),
            sid(n->first_child), sid(n->last_child), n->child_count);

    if (n->suffix == 't' && n->col_title_count > 0) {
        for (int i = 0; i < depth; i++) fprintf(out, "  ");
        fprintf(out, "  titles:");
        for (int k = 0; k < n->col_title_count; k++) fprintf(out, " [%d]\"%s\"", k, n->col_titles[k]);
        fprintf(out, "\n");
    }

    for (Node *c = n->first_child; c; c = c->next) dump_debug(out, c, depth + 1);
}

/* =========================
 *  UTF-8 width/clip
 * ========================= */
static int u8_width(const char *s) {
    if (!s || !*s) return 0;
    /* 避免每次 malloc：逐字符计算宽度 */
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    int w = 0;
    const char *p = s;
    while (*p) {
        wchar_t wc;
        size_t n = mbrtowc(&wc, p, MB_CUR_MAX, &st);
        if (n == (size_t)-1 || n == (size_t)-2) {
            /* 非法/不完整 UTF-8：退化为字节长度 */
            return (int)strlen(s);
        }
        if (n == 0) break;
        int cw = wcwidth(wc);
        if (cw < 0) cw = 1;
        w += cw;
        p += n;
    }
    return w;
}

/*
 * 仅用于 TUI 渲染的“显示名称”，不修改 Node->name（后续逻辑仍需原始 name）。
 *  - x='a' : [name] ____
 *  - x='b' : [ ] name
 *  - x='c' : [x] name
 */
static const char *node_disp_name(const Node *n, char *buf, size_t bufsz) {
    if (!n) { if (bufsz) buf[0] = 0; return buf; }
    if (n->x != 'a' && n->x != 'b' && n->x != 'c') return n->name;
    if (!buf || bufsz == 0) return n->name;

    if (n->x == 'a') {
        const char *v = (n->val && n->val[0]) ? n->val : "____";
        snprintf(buf, bufsz, "[%s] %s", n->name, v);
    } else if (n->x == 'b') {
        snprintf(buf, bufsz, "[ ] %s", n->name);
    } else { /* 'c' */
        snprintf(buf, bufsz, "[x] %s", n->name);
    }
    return buf;
}

/* 兼容旧调用点：仅用于 TUI 显示，不修改 Node->name */
const char *node_view_name(const Node *n, char *buf, size_t bufsz) {
    return node_disp_name(n, buf, bufsz);
}

static void u8_clip_to(char *out, size_t outcap, const char *s, int maxw) {
    if (outcap == 0) return;
    out[0] = 0;
    if (!s) return;

    mbstate_t st;
    memset(&st, 0, sizeof(st));
    const char *p = s;
    int used = 0;
    size_t outn = 0;
    while (*p && used < maxw && outn + 1 < outcap) {
        wchar_t wc;
        size_t n = mbrtowc(&wc, p, MB_CUR_MAX, &st);
        if (n == (size_t)-1 || n == (size_t)-2) {
            /* 非法 UTF-8：按字节截断 */
            size_t can = outcap - 1;
            strncpy(out, s, can);
            out[can] = 0;
            return;
        }
        if (n == 0) break;
        int cw = wcwidth(wc);
        if (cw < 0) cw = 1;
        if (used + cw > maxw) break;
        /* 拷贝对应的 UTF-8 字节 */
        if (outn + n >= outcap) break;
        memcpy(out + outn, p, n);
        outn += n;
        used += cw;
        p += n;
    }
    out[outn] = 0;
}

static void mvadd_u8_fit(int y, int x, const char *s, int maxw) {
    char buf[4096];
    u8_clip_to(buf, sizeof(buf), s ? s : "", maxw);
    mvaddnstr(y, x, buf, (int)strlen(buf));
}

/* =========================
 *  Row model (dim=2 => HGROUP single line)
 * ========================= */
typedef enum { ROW_NODE, ROW_HGROUP } RowType;

typedef struct {
    RowType type;
    int     indent;
    Node   *node; /* ROW_NODE: item; ROW_HGROUP: parent with dim=2 */
} Row;

typedef struct {
    Row *v;
    int n;
    int cap;
} RowVec;

static void rvec_push(RowVec *rv, Row r) {
    if (rv->n == rv->cap) {
        rv->cap = rv->cap ? rv->cap * 2 : 64;
        rv->v = (Row*)realloc(rv->v, (size_t)rv->cap * sizeof(Row));
        if (!rv->v) { perror("realloc"); exit(1); }
    }
    rv->v[rv->n++] = r;
}

static bool is_visible_item(Node *n) {
    if (!n) return false;
    if (n->hidden) return false;
    if (n->suffix == 't') return false;
    return true;
}

static int visible_child_count(Node *p) {
    int c = 0;
    for (Node *n = p ? p->first_child : NULL; n; n = n->next)
        if (is_visible_item(n)) c++;
    return c;
}

static Node *nth_visible_child(Node *p, int idx) {
    if (!p) return NULL;
    int k = 0;
    for (Node *n = p->first_child; n; n = n->next) {
        if (!is_visible_item(n)) continue;
        if (k == idx) return n;
        k++;
    }
    return NULL;
}

static void build_rows_for_ctx(Node *ctx, Row **out_rows, int *out_n) {
    *out_rows = NULL;
    *out_n = 0;
    if (!ctx) return;

    RowVec rv = {0};
    for (Node *c = ctx->first_child; c; c = c->next) {
        if (!is_visible_item(c)) continue;

        rvec_push(&rv, (Row){ .type = ROW_NODE, .indent = 0, .node = c });

        int dim = c->dim;
        if (dim == 3 && c->first_child) {
            for (Node *g = c->first_child; g; g = g->next) {
                if (!is_visible_item(g)) continue;
                rvec_push(&rv, (Row){ .type = ROW_NODE, .indent = 4, .node = g });
            }
        } else if (dim == 2 && c->first_child) {
            if (visible_child_count(c) > 0) {
                /* dim=2：子集只占一行 */
                rvec_push(&rv, (Row){ .type = ROW_HGROUP, .indent = 4, .node = c });
            }
        }
    }

    *out_rows = rv.v;
    *out_n = rv.n;
}

static Node *row_selected_node(const Row *r, int subidx) {
    if (!r) return NULL;
    if (r->type == ROW_NODE) return r->node;
    Node *p = r->node;
    int cnt = visible_child_count(p);
    if (cnt <= 0) return NULL;
    if (subidx < 0) subidx = 0;
    if (subidx >= cnt) subidx = cnt - 1;
    return nth_visible_child(p, subidx);
}

static int row_hgroup_count(const Row *r) {
    if (!r || r->type != ROW_HGROUP) return 0;
    return visible_child_count(r->node);
}

/* =========================
 *  UI
 * ========================= */
#define MAX_COLS 8
typedef struct {
    Ndx  *ndx;

    int   col_count;
    int   focus_col;

    Node *ctx[MAX_COLS];
    Row  *rows[MAX_COLS];
    int   nrows[MAX_COLS];

    int   sel_row[MAX_COLS];
    int   sel_sub[MAX_COLS];
    int   scroll[MAX_COLS];

    Node *active_title;
} UI;

static void ui_free_rows(UI *u) {
    for (int i = 0; i < MAX_COLS; i++) {
        free(u->rows[i]);
        u->rows[i] = NULL;
        u->nrows[i] = 0;
    }
}

static void ui_reset_after(UI *u, int col) {
    for (int i = col + 1; i < MAX_COLS; i++) {
        u->sel_row[i] = 0;
        u->sel_sub[i] = 0;
        u->scroll[i]  = 0;
    }
}

static void ui_rebuild(UI *u) {
    ui_free_rows(u);

    u->ctx[0] = u->ndx->root;
    build_rows_for_ctx(u->ctx[0], &u->rows[0], &u->nrows[0]);
    if (u->sel_row[0] >= u->nrows[0]) u->sel_row[0] = (u->nrows[0] > 0) ? (u->nrows[0] - 1) : 0;
    if (u->sel_row[0] < 0) u->sel_row[0] = 0;
    if (u->nrows[0] == 0) { u->col_count = 1; return; }

    Node *sel0 = row_selected_node(&u->rows[0][u->sel_row[0]], u->sel_sub[0]);
    u->ctx[1] = sel0;

    build_rows_for_ctx(u->ctx[1], &u->rows[1], &u->nrows[1]);
    if (u->sel_row[1] >= u->nrows[1]) u->sel_row[1] = (u->nrows[1] > 0) ? (u->nrows[1] - 1) : 0;
    if (u->sel_row[1] < 0) u->sel_row[1] = 0;

    Node *func = NULL;
    if (u->nrows[1] > 0) func = row_selected_node(&u->rows[1][u->sel_row[1]], u->sel_sub[1]);

    u->active_title = NULL;
    int title_cols = 1;
    if (func) {
        Node *t = (Node*)hmap_get(&u->ndx->title_by_base, func->id_base);
        if (t && t->col_title_count > 0) {
            u->active_title = t;
            title_cols = t->col_title_count;
        }
    }

    u->col_count = 1 + title_cols;
    if (u->col_count > MAX_COLS) u->col_count = MAX_COLS;
    if (u->focus_col >= u->col_count) u->focus_col = u->col_count - 1;
    if (u->focus_col < 0) u->focus_col = 0;

    for (int col = 2; col < u->col_count; col++) {
        Node *prev_sel = NULL;
        if (u->nrows[col - 1] > 0) {
            int pr = u->sel_row[col - 1];
            if (pr < 0) pr = 0;
            if (pr >= u->nrows[col - 1]) pr = u->nrows[col - 1] - 1;
            prev_sel = row_selected_node(&u->rows[col - 1][pr], u->sel_sub[col - 1]);
        }
        u->ctx[col] = prev_sel;
        build_rows_for_ctx(u->ctx[col], &u->rows[col], &u->nrows[col]);
        if (u->sel_row[col] >= u->nrows[col]) u->sel_row[col] = (u->nrows[col] > 0) ? (u->nrows[col] - 1) : 0;
        if (u->sel_row[col] < 0) u->sel_row[col] = 0;
    }
}

static const char *col_header(UI *u, int col) {
    if (col == 0) return "选项";
    if (u->active_title && (col - 1) < u->active_title->col_title_count) return u->active_title->col_titles[col - 1];
    if (col == 1) return "功能";
    return "";
}

static void ensure_visible(UI *u, int col, int view_h) {
    if (col < 0 || col >= u->col_count) return;
    if (view_h <= 0) return;
    int n = u->nrows[col];
    if (n <= 0) { u->scroll[col] = 0; return; }

    int r = u->sel_row[col];
    if (r < 0) r = 0;
    if (r >= n) r = n - 1;

    int top = u->scroll[col];
    if (top < 0) top = 0;
    if (top > n - 1) top = n - 1;

    if (r < top) top = r;
    if (r >= top + view_h) top = r - view_h + 1;
    if (top < 0) top = 0;
    if (top > n - view_h) top = (n > view_h) ? (n - view_h) : 0;
    u->scroll[col] = top;
}

static void compute_layout(UI *u, int term_w, int *xs, int *ws) {
    const int sep = 1;
    const int minw = 12;
    const int maxw_nonlast = 28;

    int count = u->col_count;
    if (count < 1) count = 1;

    int fixed_sum = 0;
    for (int c = 0; c < count; c++) {
        int want = u8_width(col_header(u, c)) + 2;

        for (int i = 0; i < u->nrows[c]; i++) {
            Row *r = &u->rows[c][i];
            if (r->type == ROW_NODE) {
                char tmp[1024];
                const char *disp = node_disp_name(r->node, tmp, sizeof(tmp));
                int w = r->indent + u8_width(disp);
                if (w + 2 > want) want = w + 2;
            } else {
                int w = r->indent;
                int cnt = visible_child_count(r->node);
                for (int k = 0; k < cnt; k++) {
                    Node *ch = nth_visible_child(r->node, k);
                    if (!ch) continue;
                    if (k) w += 1;
                    char tmp[1024];
                    const char *disp = node_disp_name(ch, tmp, sizeof(tmp));
                    w += u8_width(disp);
                }
                if (w + 2 > want) want = w + 2;
            }
        }

        if (want < minw) want = minw;
        if (c != count - 1 && want > maxw_nonlast) want = maxw_nonlast;

        ws[c] = want;
        fixed_sum += want;
    }

    int total_sep = (count - 1) * sep;
    int remain = term_w - fixed_sum - total_sep;
    if (remain != 0 && count >= 1) {
        ws[count - 1] += remain;
        if (ws[count - 1] < minw) ws[count - 1] = minw;
    }

    int x = 0;
    for (int c = 0; c < count; c++) {
        xs[c] = x;
        x += ws[c] + sep;
    }
}

static Node *ui_get_active_node(UI *u) {
    int c = u->focus_col;
    for (; c >= 0; c--) {
        if (u->nrows[c] <= 0) continue;
        int r = u->sel_row[c];
        if (r < 0) r = 0;
        if (r >= u->nrows[c]) r = u->nrows[c] - 1;
        Node *n = row_selected_node(&u->rows[c][r], u->sel_sub[c]);
        if (n) return n;
    }
    return NULL;
}

static void draw_ui(UI *u) {
    int H, W;
    getmaxyx(stdscr, H, W);
    erase();

    int list_y0 = 1;
    int list_h = H - 2;
    if (list_h < 1) list_h = 1;

    int xs[MAX_COLS] = {0};
    int ws[MAX_COLS] = {0};
    compute_layout(u, W, xs, ws);

    for (int c = 0; c < u->col_count; c++) {
        int x = xs[c], w = ws[c];
        if (x >= W || w <= 0) continue;

        attron(COLOR_PAIR(1));
        for (int i = 0; i < w && (x + i) < W; i++) mvaddch(0, x + i, ' ');
        mvadd_u8_fit(0, x + 1, col_header(u, c), w - 2);
        attroff(COLOR_PAIR(1));

        if (c != u->col_count - 1) {
            int sx = x + w;
            if (sx < W) for (int y = 0; y < H - 1; y++) mvaddch(y, sx, ACS_VLINE);
        }
    }

    for (int c = 0; c < u->col_count; c++) {
        ensure_visible(u, c, list_h);
        int top = u->scroll[c];
        int x = xs[c], w = ws[c];
        if (w <= 0) continue;

        for (int i = 0; i < list_h; i++) {
            int ridx = top + i;
            int y = list_y0 + i;
            if (y >= H - 1) break;

            for (int k = 0; k < w && (x + k) < W; k++) mvaddch(y, x + k, ' ');
            if (ridx >= u->nrows[c]) continue;

            Row *r = &u->rows[c][ridx];
            bool focused_row = (c == u->focus_col && ridx == u->sel_row[c]);

            if (r->type == ROW_NODE) {
                if (focused_row) attron(COLOR_PAIR(2));
                int ox = x + 1, avail = w - 2;
                int ind = r->indent;
                if (ind > avail) ind = avail;
                for (int sp = 0; sp < ind && sp < avail; sp++) if (ox + sp < W) mvaddch(y, ox + sp, ' ');
                {
                    char tmp[1024];
                    const char *disp = node_disp_name(r->node, tmp, sizeof(tmp));
                    mvadd_u8_fit(y, ox + ind, disp, avail - ind);
                }
                if (focused_row) attroff(COLOR_PAIR(2));
            } else {
                int cnt = row_hgroup_count(r);
                if (focused_row) attron(COLOR_PAIR(2));

                int ox = x + 1, avail = w - 2;
                int ind = r->indent;
                if (ind > avail) ind = avail;
                for (int sp = 0; sp < ind && sp < avail; sp++) if (ox + sp < W) mvaddch(y, ox + sp, ' ');

                int pos = ind;
                for (int k = 0; k < cnt; k++) {
                    Node *ch = nth_visible_child(r->node, k);
                    if (!ch) continue;
                    if (k) {
                        if (pos < avail) mvaddch(y, ox + pos, ' ');
                        pos += 1;
                    }
                    if (pos >= avail) break;

                    if (focused_row && k == u->sel_sub[c]) attron(A_BOLD | A_UNDERLINE);
                    {
                        char tmp[1024];
                        const char *disp = node_disp_name(ch, tmp, sizeof(tmp));
                        mvadd_u8_fit(y, ox + pos, disp, avail - pos);
                        pos += u8_width(disp);
                    }
                    if (focused_row && k == u->sel_sub[c]) attroff(A_BOLD | A_UNDERLINE);
                }

                if (focused_row) attroff(COLOR_PAIR(2));
            }
        }
    }

    /* status: path + 当前节点 dim */
    Node *cur = ui_get_active_node(u);
    char status[4096]; status[0] = 0;

    if (cur) {
        Node *stk[128]; int sn = 0;
        for (Node *p = cur; p; p = p->parent) {
            if (p == u->ndx->root) break;
            if (p->suffix == 't') continue;
            if (p->hidden) continue;
            stk[sn++] = p;
            if (sn >= (int)(sizeof(stk)/sizeof(stk[0]))) break;
        }
        for (int i = sn - 1; i >= 0; i--) {
            char one[512];
            snprintf(one, sizeof(one), "%s>%s", stk[i]->id_raw, stk[i]->name);
            if (status[0]) strncat(status, " / ", sizeof(status) - strlen(status) - 1);
            strncat(status, one, sizeof(status) - strlen(status) - 1);
        }

        char tail[64];
        snprintf(tail, sizeof(tail), "   [dim=%d]", cur->dim);
        strncat(status, tail, sizeof(status) - strlen(status) - 1);
    }

    attron(COLOR_PAIR(3));
    for (int x = 0; x < W; x++) mvaddch(H - 1, x, ' ');
    mvadd_u8_fit(H - 1, 0, status, W);
    attroff(COLOR_PAIR(3));

    refresh();
}

static bool is_leaf_parent(Node *n) {
    /* 末级子项父节点：自身至少有 1 个“可见子项”，且这些子项都没有更深的可见子项 */
    int cnt = visible_child_count(n);
    if (cnt <= 0) return false;
    for (int i = 0; i < cnt; i++) {
        Node *ch = nth_visible_child(n, i);
        if (!ch) continue;
        if (visible_child_count(ch) > 0) return false;
    }
    return true;
}

static void validate_subset_dim_or_die(Ndx *ndx) {
    /*
     * 规则：只有“末级子项的父节点”才允许配置子集显示方式 (dim=2/3)。
     * - 若节点显式配置了 dim，但其子项里存在“非末级子项”（还有孩子），或该节点没有任何子项：报错退出。
     * - 未配置 dim 的节点不做限制。
     */
    for (int i = 0; i < ndx->all.n; i++) {
        Node *n = ndx->all.v[i];
        if (!n || n == ndx->root) continue;
        if (!n->di_explicit) continue; /* 只检查 config 里显式设置过的 */

        if (!is_leaf_parent(n)) {
            int cnt = visible_child_count(n);
            fprintf(stderr,
                    "config error: %s>%s 配置了子集显示方式 dim=%d，但它不是『末级子项的父节点』\n",
                    n->id_raw, n->name, n->dim);

            if (cnt <= 0) {
                fprintf(stderr, "  - 原因：该节点没有任何可见子项（无法展开子集）\n");
            } else {
                for (int k = 0; k < cnt; k++) {
                    Node *ch = nth_visible_child(n, k);
                    if (!ch) continue;
                    int cc = visible_child_count(ch);
                    if (cc > 0) {
                        fprintf(stderr,
                                "  - 原因：其子项 %s>%s 仍有 %d 个可见子项（不是末级子项）\n",
                                ch->id_raw, ch->name, cc);
                        break;
                    }
                }
            }
            fprintf(stderr,
                    "  - 约束：dim 只能写在『直接子项全部为末级子项』的节点上，例如 1.1.1.4 的子项 1.1.1.4.1/1.1.1.4.2/... 都没有更深子项\n");
            ndx_free(ndx);
            exit(1);
        }
    }
}


static Node *ui_get_cursor_node(UI *u) {
    if (!u) return NULL;
    int c = u->focus_col;
    if (c < 0 || c >= u->col_count) return NULL;
    if (u->nrows[c] <= 0) return NULL;
    int r = u->sel_row[c];
    if (r < 0) r = 0;
    if (r >= u->nrows[c]) r = u->nrows[c] - 1;
    return row_selected_node(&u->rows[c][r], u->sel_sub[c]);
}

static void run_tui(Ndx *ndx) {

    validate_subset_dim_or_die(ndx);
    UI u;
    memset(&u, 0, sizeof(u));
    u.ndx = ndx;
    u.col_count = 1;
    u.focus_col = 0;

    /*
     * “选到哪就显示到哪”：
     * - 始终显示从第0列到当前 focus_col 的所有列（路径/先前选中项）
     * - 再额外显示 focus_col 的下一列（子集）
     * - 更右侧的列全部不画（哪怕内部已构建出来）
     */
    int full_cols = 1; /* ui_rebuild() 计算出的完整列数（由标题列数决定） */

    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_BLACK, COLOR_GREEN);
        init_pair(2, COLOR_BLACK, COLOR_BLUE);
        init_pair(3, COLOR_WHITE, -1);
    }

    bool dirty = true;

    HotPopup pop;
    hot_init(&pop);
    Node *hot_suppress = NULL;

    while (1) {
        if (dirty) {
            ui_rebuild(&u);
            /* ui_rebuild() 会把 u.col_count 设为“完整标题列数” */
            full_cols = u.col_count;
            dirty = false;
        }

        /*
         * 只显示到“当前所在列 + 下一列(子集)”为止：
         * 例：focus=0 -> 显示 0,1；focus=1 -> 显示 0,1,2；focus=2 -> 显示 0..3
         */
        int visible_cols = u.focus_col + 2;
        if (visible_cols < 1) visible_cols = 1;
        if (visible_cols > full_cols) visible_cols = full_cols;
        if (visible_cols < u.focus_col + 1) visible_cols = u.focus_col + 1;
        u.col_count = visible_cols;

        draw_ui(&u);

        /* a 类热点：光标停留在 x=='a' 的节点上时弹出，并允许在红框内运行 top */
        Node *cursor = ui_get_cursor_node(&u);
        if (hot_suppress && cursor != hot_suppress) hot_suppress = NULL;
        bool want_hot = (cursor && cursor->x == 'a' && cursor != hot_suppress);

        int H, W;
        getmaxyx(stdscr, H, W);

        if (!want_hot) {
            if (pop.active) hot_close(&pop);
        } else {
            if (!pop.active || pop.owner != cursor) {
                hot_close(&pop);
                pop.active = true;
                pop.mode = HOT_INPUT;
                pop.owner = cursor;
                pop.closed_by_enter = false;
                pop.last_owner = NULL;
            }

            int xs[MAX_COLS] = {0}, ws[MAX_COLS] = {0};
            compute_layout(&u, W, xs, ws);
            int col = u.focus_col;
            if (col < 0) col = 0;
            if (col >= u.col_count) col = u.col_count - 1;

            int x = xs[col];
            int w = ws[col];
            if (x < 0) x = 0;
            if (x >= W) x = W - 1;
            if (w > W - x) w = W - x;

            int ph = (H - 2) / 2;
            if (ph < 8) ph = 8;
            if (ph > H - 3) ph = H - 3;
            int y = (H - 1) - ph;

            hot_set_geom(&pop, y, x, ph, w);

            hot_pump(&pop);
            hot_draw(&pop);
        }

        if (pop.closed_by_enter) {
            hot_suppress = pop.last_owner;
            pop.closed_by_enter = false;
            pop.last_owner = NULL;
        }

        if (pop.active && pop.mode == HOT_TERM) timeout(50);
        else timeout(-1);

        int ch = getch();
        if (ch == ERR) continue;

        /* Always handle resize at the top-level, even when hot popup is active.
         * Otherwise ncurses may keep stale internal geometry and the hot area
         * (especially ncurses apps like htop) will render garbled during drag-resize. */
        if (ch == KEY_RESIZE) {
            int nh, nw;
            getmaxyx(stdscr, nh, nw);
            resizeterm(nh, nw);
            dirty = true;
            continue;
        }

        if (pop.active) {
            if (hot_handle_key(&pop, ch)) continue;
        }

        if (ch == 'q' || ch == 'Q' || ch == 27) break;

        int c = u.focus_col;
        if (c < 0) c = 0;
        if (c >= u.col_count) c = u.col_count - 1;

        bool changed_sel = false;
        bool changed_focus = false;

        if (ch == KEY_UP || ch == 'k') {
            if (u.nrows[c] > 0) {
                u.sel_row[c]--;
                if (u.sel_row[c] < 0) u.sel_row[c] = u.nrows[c] - 1;
                u.sel_sub[c] = 0;
                changed_sel = true;
            }
        } else if (ch == KEY_DOWN || ch == 'j') {
            if (u.nrows[c] > 0) {
                u.sel_row[c]++;
                if (u.sel_row[c] >= u.nrows[c]) u.sel_row[c] = 0;
                u.sel_sub[c] = 0;
                changed_sel = true;
            }
        } else if (ch == KEY_LEFT || ch == KEY_RIGHT || ch == 'h' || ch == 'l') {
            if (ch == 'h') ch = KEY_LEFT;
            if (ch == 'l') ch = KEY_RIGHT;
            int dir = (ch == KEY_RIGHT) ? +1 : -1;

            if (u.nrows[c] > 0) {
                Row *r = &u.rows[c][u.sel_row[c]];
                if (r->type == ROW_HGROUP) {
                    int cnt = row_hgroup_count(r);
                    if (cnt > 0) {
                        int ns = u.sel_sub[c] + dir;
                        if (ns >= 0 && ns < cnt) {
                            u.sel_sub[c] = ns;
                            changed_sel = true;
                        } else {
                            int nc = c + dir;
                            if (nc >= 0 && nc < u.col_count) { u.focus_col = nc; changed_focus = true; }
                        }
                    } else {
                        int nc = c + dir;
                        if (nc >= 0 && nc < u.col_count) { u.focus_col = nc; changed_focus = true; }
                    }
                } else {
                    int nc = c + dir;
                    if (nc >= 0 && nc < u.col_count) { u.focus_col = nc; changed_focus = true; }
                }
            } else {
                int nc = c + dir;
                if (nc >= 0 && nc < u.col_count) { u.focus_col = nc; changed_focus = true; }
            }
        }

        if (changed_sel) {
            ui_reset_after(&u, c);
            dirty = true;
        } else if (changed_focus) {
            /* focus 变化不一定需要 rebuild，但需要重绘 */
            dirty = false;
        }
    }

    endwin();
    ui_free_rows(&u);
}

int main(int argc, char **argv) 
{
    const char *path = (argc >= 2) ? argv[1] : "config.txt";

    Ndx ndx;
    ndx_init(&ndx);

    if (!ndx_parse_file(&ndx, path)) {
        ndx_free(&ndx);
        return 1;
    }

    /* ②  遍历打印：先给 stdout 一个“树形结构”的清晰输出 */
    fprintf(stdout, "\n==== NDX TREE DUMP (preorder) ====\n");
    dump_tree(stdout, ndx.root, 0);

    /* ③  复杂链式结构（指针关系等）输出到文件，便于你 grep/比对 */
    FILE *fo = fopen("ndx_dump.txt", "w");
    if (fo) {
        fprintf(fo, "==== NDX DEBUG DUMP (pointers & links) ====\n");
        dump_debug(fo, ndx.root, 0);
        fclose(fo);
        fprintf(stdout, "\n==== NDX DEBUG DUMP (pointers & links) ====\n");
        dump_debug(stdout, ndx.root, 0);
        fprintf(stdout, "(also saved to ndx_dump.txt)\n\n");
    } else {
        fprintf(stderr, "warn: cannot write ndx_dump.txt: %s\n", strerror(errno));
        /* 仍然打印到 stdout，保证需求③ 可见 */
        fprintf(stdout, "\n==== NDX DEBUG DUMP (pointers & links) ====\n");
        dump_debug(stdout, ndx.root, 0);
    }

    /* ④  使用 ndx 画 TUI,TUI 退出后释放 ndx（见 main 末尾 ndx_free） */
    run_tui(&ndx);
    ndx_free(&ndx);
    return 0;
}
