# bcachefs btree 学习示例 Makefile
#
# persist.c              — 数据持久化原理演示
# val-types.c            — 值类型系统与 X-macro 教学示例
# bkey-three-types-demo.c — 三种 bkey 类型设计模式优劣演示
#
# 编译: make
# 运行: ./persist ./val-types ./bkey-three-types-demo
# 文档: make docs
# 清理: make clean

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=gnu11
# Clang 支持此标志，GCC 会警告不认识——仅在 Clang 下启用
ifneq ($(filter clang%,$(shell $(CC) --version 2>/dev/null | head -1)),)
CFLAGS += -Wno-bitfield-constant-conversion
endif
LDFLAGS =
RM      = rm -f

.PHONY: all clean docs

all: persist val-types bkey-three-types-demo

persist: persist.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

val-types: val-types.c types-defs.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

bkey-three-types-demo: bkey-three-types-demo.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# 文档生成：运行测试程序输出类型字段表格
docs: val-types
	./val-types --docs > types-doc.md

clean:
	$(RM) persist val-types bkey-three-types-demo types-doc.md
