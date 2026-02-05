#ifndef NDX_H
#define NDX_H

#include <stdbool.h>

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
    char   x;       /* 节点类型 a,b,c*/
    /* title 节点使用 */
    char **col_titles;
    int    col_title_count;
    char link[20]; /*文件描述符，用于与外界交互，只有节点类型为 "a" 时才启用*/
    /*val表示：节点类型为"a"时 需要从tui交互节点中获取最终选择的数据，
          节点类型为"b"时，是判断该节点是否被选中，选中为"TRUE",未选择为"FALSE",
      节点类型为"c"时，val值为"static"表示这个是强制默认就选中的	
      */
    char* val;
    char* cmd;   /* 节点类型为"a"时，对应的热区运行的指令*/
};

#endif
