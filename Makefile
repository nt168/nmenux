CC ?= gcc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11

# 建议使用 ncursesw 以稳定显示 UTF-8(中文)
CPPFLAGS ?= -DWITH_NCURSES
LDLIBS ?= -lncursesw

all: perftui

perftui: main.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDLIBS)

# 仅用于验证解析/遍历逻辑(不依赖 ncurses 头文件)
perftui_nocurses: main.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f perftui perftui_nocurses
