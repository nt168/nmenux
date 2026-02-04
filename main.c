// main.c (config.txt -> Ndx tree -> dump -> TUI)
// Build:
//   gcc -O2 -Wall -Wextra -std=c11 main.c -o perftui -lncursesw
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
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>

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

typedef enum {
    DI_DEFAULT = 0,
    DI_HOR     = 2,
    DI_VER     = 3,
} DiMode;

typedef struct Node Node;
struct Node {
    char *id_raw;
    char *id_base;
    char  suffix;   /* 't' or 0 */
    char *name;
    int   level;

    Node *parent;
    Node *prev;
    Node *next;
    Node *first_child;
    Node *last_child;
    int   child_count;

    Node *title;
    bool  hidden;
    bool  placeholder;

    DiMode di_mode;
    bool   di_explicit;
    Node  *chosen_child;

    int    dim;     /* 2 横排，3 竖排，0 默认不展开 */
    char   x;  	   /* 节点类型 a,b,c*/
    /* title 节点使用 */
    char **col_titles;
    int    col_title_count;
    char link[20]; /*文件描述符，用于与外界交互，只有节点类型为 "a" 时才启用*/
    /*val表示：节点类型为"a"时 需要从tui交互节点中获取最终选择的数据，
          节点类型为"b"时，是判断该节点是否被选中，选中为"TRUE",未选择为"FALSE",
      节点类型为"c"时，val值为"static"表示这个是强制默认就选中的	
      */
    char* val;	   	
};

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
static const char *node_view_name(const Node *n, char *buf, size_t bufsz) {
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


typedef struct {
    int rows, cols;
    char    *cells;   /* rows*cols */
    uint16_t *attrs;  /* rows*cols */
    uint16_t  cur_attr;

    /* VT100 character set support (ncurses apps like htop use ESC ( 0 + SO/SI) */
    uint8_t g0_charset; /* 0=ASCII(B), 1=line-drawing(0) */
    uint8_t g1_charset; /* 0=ASCII(B), 1=line-drawing(0) */
    bool    use_g1;     /* false=GL uses G0 (SI), true=GL uses G1 (SO) */

    /* Terminal modes that affect key sequences */
    bool    app_cursor; /* DECCKM: ESC[?1h/l */
    bool    app_keypad; /* ESC= / ESC> (numeric keypad) */

    int cx, cy;
    int saved_cx, saved_cy;

    int esc_state;        /* 0=normal,1=ESC,2=CSI,3=OSC,4=CHARSET */
    char esc_buf[128];
    int  esc_len;
    bool osc_esc_seen;    /* for ESC \ terminator */
} TermView;

static void term_free(TermView *t) {
    if (!t) return;
    free(t->cells); t->cells = NULL;
    free(t->attrs); t->attrs = NULL;
    t->rows = t->cols = 0;
    t->cx = t->cy = 0;
    t->saved_cx = t->saved_cy = 0;
    t->cur_attr = 0;
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
}

static void term_scroll_up(TermView *t, int n) {
    if (!t || !t->cells || !t->attrs || n <= 0) return;
    if (n >= t->rows) { term_clear_all(t); return; }
    size_t row_bytes = (size_t)t->cols;
    memmove(t->cells, t->cells + (size_t)n * row_bytes, (size_t)(t->rows - n) * row_bytes);
    memmove(t->attrs, t->attrs + (size_t)n * row_bytes, (size_t)(t->rows - n) * row_bytes * sizeof(uint16_t));
    memset(t->cells + (size_t)(t->rows - n) * row_bytes, ' ', (size_t)n * row_bytes);
    memset(t->attrs + (size_t)(t->rows - n) * row_bytes, 0,   (size_t)n * row_bytes * sizeof(uint16_t));
    if (t->cy >= n) t->cy -= n;
    else t->cy = 0;
}

static void term_put_ch(TermView *t, char ch) {
    if (!t || !t->cells || !t->attrs) return;
    if (t->cx < 0) t->cx = 0;
    if (t->cy < 0) t->cy = 0;
    if (t->cx >= t->cols) { t->cx = 0; t->cy++; }
    if (t->cy >= t->rows) { term_scroll_up(t, 1); t->cy = t->rows - 1; }

    size_t idx = (size_t)t->cy * (size_t)t->cols + (size_t)t->cx;
    t->cells[idx] = ch;
    t->attrs[idx] = (uint16_t)(t->cur_attr | (term_is_acs(t) ? TVA_ACS : 0));

    t->cx++;
    if (t->cx >= t->cols) { t->cx = 0; t->cy++; }
    if (t->cy >= t->rows) { term_scroll_up(t, 1); t->cy = t->rows - 1; }
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
    memset(t->attrs + off, 0,   n * sizeof(uint16_t));
}

static void term_clear_line_to(TermView *t, int to_x) {
    if (!t || !t->cells || !t->attrs) return;
    if (to_x < 0) return;
    if (to_x >= t->cols) to_x = t->cols - 1;
    size_t off = (size_t)t->cy * (size_t)t->cols;
    size_t n = (size_t)(to_x + 1);
    memset(t->cells + off, ' ', n);
    memset(t->attrs + off, 0,   n * sizeof(uint16_t));
}

static void term_clear_screen_from(TermView *t) {
    if (!t || !t->cells || !t->attrs) return;
    term_clear_line_from(t, t->cx);
    for (int r = t->cy + 1; r < t->rows; r++) {
        memset(t->cells + (size_t)r * (size_t)t->cols, ' ', (size_t)t->cols);
        memset(t->attrs + (size_t)r * (size_t)t->cols, 0,   (size_t)t->cols * sizeof(uint16_t));
    }
}

static void term_clear_screen_to(TermView *t) {
    if (!t || !t->cells || !t->attrs) return;
    for (int r = 0; r < t->cy; r++) {
        memset(t->cells + (size_t)r * (size_t)t->cols, ' ', (size_t)t->cols);
        memset(t->attrs + (size_t)r * (size_t)t->cols, 0,   (size_t)t->cols * sizeof(uint16_t));
    }
    term_clear_line_to(t, t->cx);
}

static int term_rgb_to_ansi8(int r, int g, int b) {
    /* 近似映射到 8 色：0 black,1 red,2 green,3 yellow,4 blue,5 magenta,6 cyan,7 white */
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;

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
                    if (i < end && seq[i] == ';') i++; int r = csi_get_int(seq, &i, end); if (r < 0) r = 0;
                    if (i < end && seq[i] == ';') i++; int g = csi_get_int(seq, &i, end); if (g < 0) g = 0;
                    if (i < end && seq[i] == ';') i++; int b = csi_get_int(seq, &i, end); if (b < 0) b = 0;
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
            if (n == 2) term_clear_all(t);
            else if (n == 0) term_clear_screen_from(t);
            else if (n == 1) term_clear_screen_to(t);
            break;
        }
        case 'K': {
            int n = p1;
            if (n == 2) { t->cx = 0; term_clear_line_from(t, 0); }
            else if (n == 0) term_clear_line_from(t, t->cx);
            else if (n == 1) term_clear_line_to(t, t->cx);
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
                /* alt screen: 清空即可（fzy/top/htop 会用） */
                if (p1 == 1049 || p1 == 47) term_clear_all(t);
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
            if (ch == '\r') { t->cx = 0; continue; }
            if (ch == '\n') { t->cy++; if (t->cy >= t->rows) { term_scroll_up(t, 1); t->cy = t->rows - 1; } continue; }
            if (ch == '\b') { if (t->cx > 0) t->cx--; continue; }
            if (ch == '\t') {
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
            if (ch == 'c') { term_clear_all(t); t->esc_state = 0; continue; }
            if (ch == '7') { t->saved_cx = t->cx; t->saved_cy = t->cy; t->esc_state = 0; continue; }
            if (ch == '8') { t->cx = t->saved_cx; t->cy = t->saved_cy; t->esc_state = 0; continue; }
            /* common single-char escapes used by ncurses apps */
            if (ch == 'D') { /* IND */ t->cy++; if (t->cy >= t->rows) { term_scroll_up(t, 1); t->cy = t->rows - 1; } t->esc_state = 0; continue; }
            if (ch == 'M') { /* RI */  t->cy--; if (t->cy < 0) t->cy = 0; t->esc_state = 0; continue; }
            if (ch == 'E') { /* NEL */ t->cx = 0; t->cy++; if (t->cy >= t->rows) { term_scroll_up(t, 1); t->cy = t->rows - 1; } t->esc_state = 0; continue; }
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
    if (fg < 0) fg = 0; if (fg > 15) fg = 15;
    if (bg < 0) bg = 0; if (bg > 15) bg = 15;

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

typedef enum { HOT_INPUT = 0, HOT_TERM = 1 } HotMode;

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

    /* 记录 pty 原始输出尾部，用于从 fzy 抽取“最终选中行” */
    unsigned char raw_tail[8192];
    int           raw_len;

    /* 通知 run_tui：这是“回车选择(fzy)导致的关闭”，用于抑制立刻再次弹窗 */
    Node *last_owner;
    bool  closed_by_enter;
} HotPopup;

static void hot_close(HotPopup *p);

static void hot_init(HotPopup *p) {
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
            usleep(10 * 1000);
        }
        waitpid(p->pid, &st, WNOHANG);
    }
    p->running = false;
    p->pid = -1;
    if (p->master_fd >= 0) { close(p->master_fd); p->master_fd = -1; }
    term_free(&p->term);
    p->raw_len = 0;
}

static void hot_close(HotPopup *p) {
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

static void hot_set_geom(HotPopup *p, int y, int x, int h, int w) {
    if (!p) return;
    if (h < 3) h = 3;
    if (w < 10) w = 10;

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
        /* When the viewport changes, clear local screen buffer so the next
         * redraw from ncurses apps (e.g. htop) does not mix with stale cells.
         * Keep terminal modes (DECCKM/keypad) intact for correct key mapping. */
        if (ih != oldr || iw != oldc) {
            term_clear_screenbuf_keep_modes(&p->term);
        }
        if (p->master_fd >= 0 && p->pid > 0) {
            struct winsize wsz;
            memset(&wsz, 0, sizeof(wsz));
            wsz.ws_row = (unsigned short)ih;
            wsz.ws_col = (unsigned short)iw;
            ioctl(p->master_fd, TIOCSWINSZ, &wsz);
            kill(p->pid, SIGWINCH);
        }
    }
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

static void hot_draw(HotPopup *p) {
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

        wrefresh(p->wb);
        wrefresh(p->wi);
        return;
    }

    curs_set(0);
    term_draw(p->wi, &p->term);
    wrefresh(p->wb);
    wrefresh(p->wi);
}

static void hot_pump(HotPopup *p) {
    if (!p || !p->active || p->mode != HOT_TERM || p->master_fd < 0) return;

    unsigned char buf[4096];

    /* 先尽可能读出数据 */
    while (1) {
        ssize_t n = read(p->master_fd, buf, sizeof(buf));
        if (n > 0) {
            hot_raw_append(p, buf, (int)n);
            term_feed(&p->term, buf, (int)n);
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
            while (1) {
                ssize_t n = read(p->master_fd, buf, sizeof(buf));
                if (n > 0) {
                    hot_raw_append(p, buf, (int)n);
                    term_feed(&p->term, buf, (int)n);
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
                return;
            }

            /* 非 fzy：回到输入模式 */
            hot_kill_child(p);
            p->mode = HOT_INPUT;
            return;
        }
    }
}

static void hot_send_bytes(HotPopup *p, const char *s, size_t n) {
    if (!p || p->master_fd < 0 || !s || n == 0) return;
    (void)write(p->master_fd, s, n);
}

static bool hot_handle_key(HotPopup *p, int ch) {
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

