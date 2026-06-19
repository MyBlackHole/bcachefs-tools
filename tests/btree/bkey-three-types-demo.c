/*
 * bkey-three-types-demo.c — bcachefs bkey 三种类型设计模式的优劣演示
 *
 * 本文件是独立教学案例，以简化的「几何索引系统」模仿 bcachefs bkey 的
 * 三种类型设计（bkey_i / bkey_s_c / bkey_s），通过具体场景量化每项设计
 * 的收益与代价。
 *
 * 场景: 内存中的几何对象索引 — point(x,y), rect(x,y,w,h), tag(变长字符串)
 * 三种操作: 构造新键 / 只读遍历 / 原地修改
 *
 * 三种类型原型（对齐 bcachefs fs/btree/bkey_types.h）:
 *   bkey_i_##name   — 内联值, key header+value 同一连续内存 → 构造/插入
 *   bkey_s_c_##name — 拆分 const 指针对 → 只读遍历/查找
 *   bkey_s_##name   — 拆分可变指针对 → 原地修改
 *
 * 编译（debug）:   gcc -Wall -Wextra -O2 -std=gnu11 -o bkey-three-types-demo bkey-three-types-demo.c
 * 运行:           ./bkey-three-types-demo
 * 编译（release）: gcc -DNDEBUG -Wall -Wextra -O2 -std=gnu11 -o bkey-three-types-demo bkey-three-types-demo.c
 *
 * 依赖: 无, 纯 C 标准库
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>

/* =================================================================
 * 第一部分: 基础设施 — 对齐 bcachefs 类型系统骨架
 * ================================================================= */

struct bpos { uint64_t inode, offset; uint32_t snapshot; };
struct bch_val { uint64_t v[0]; };

struct bkey {
	uint8_t  u64s;
	uint8_t  format;
	uint8_t  type;
	uint8_t  pad;
	uint64_t version;
	uint32_t size;
	struct bpos p;
};

#define BKEY_U64s (sizeof(struct bkey) / sizeof(uint64_t))

static void bkey_init(struct bkey *k)
{
	memset(k, 0, sizeof(*k));
	k->u64s = BKEY_U64s;
}

/* --- 三种泛型基础类型 --- */

struct bkey_i {
	uint64_t     _data[0];
	struct bkey  k;
	struct bch_val v;
};

struct bkey_s_c {
	const struct bkey   *k;
	const struct bch_val *v;
};

struct bkey_s {
	union {
		struct { struct bkey *k; struct bch_val *v; };
		struct bkey_s_c  s_c;
	};
};

static inline struct bkey_s bkey_i_to_s(struct bkey_i *k)
{
	return (struct bkey_s){ .k = &k->k, .v = &k->v };
}

/* =================================================================
 * 第二部分: 几何值类型定义
 * ================================================================= */

struct bch_point { int32_t x, y; };
struct bch_rect  { int32_t x, y; uint32_t w, h; };
struct bch_tag   { uint8_t data[0]; };
struct bch_whiteout { uint64_t _unused[0]; };

/* =================================================================
 * 第三部分: X-macro — 值类型列表
 *
 * 对齐 bcachefs BCH_BKEY_TYPES() (bcachefs_format.h:416)。
 * 每行定义一种值类型，宏展开生成:
 *   3 struct + 7 conv + 1 init = 11 个实体/类型
 * ================================================================= */

#define VALUE_TYPES()				\
	x(point,    1,  "2D point")		\
	x(rect,     2,  "Rectangle")		\
	x(tag,      3,  "Variable-length tag")	\
	x(whiteout, 4,  "Deletion marker")

/* 展开 (1): 枚举 */
enum value_type {
#define x(name, nr, desc) VALUE_TYPE_##name = nr,
	VALUE_TYPES()
#undef x
};

/* 展开 (2): 类型名称数组 */
static __attribute__((unused)) const char * const value_type_name[] = {
#define x(name, nr, desc) [nr] = #name,
	VALUE_TYPES()
#undef x
};

/* 展开 (3): 三种类型 + 转换函数 + init（核心）
 * 对齐 fs/btree/bkey_types.h:187-296
 */
#define x(name, nr, desc)							\
struct bkey_i_##name {								\
	union { struct bkey k; struct bkey_i k_i; };				\
	struct bch_##name v;							\
};										\
										\
struct bkey_s_c_##name {							\
	union {									\
		struct { const struct bkey *k; const struct bch_##name *v; };	\
		struct bkey_s_c s_c;						\
	};									\
};										\
										\
struct bkey_s_##name {								\
	union {									\
		struct { struct bkey *k; struct bch_##name *v; };		\
		struct bkey_s_c_##name  c;					\
		struct bkey_s           s;					\
		struct bkey_s_c         s_c;					\
	};									\
};										\
										\
static inline struct bkey_i_##name *						\
bkey_i_to_##name(struct bkey_i *k)						\
{ assert(!k || k->k.type == VALUE_TYPE_##name);					\
  return (struct bkey_i_##name *)k; }						\
										\
static inline const struct bkey_i_##name *					\
bkey_i_to_##name##_c(const struct bkey_i *k)					\
{ assert(!k || k->k.type == VALUE_TYPE_##name);					\
  return (const struct bkey_i_##name *)k; }					\
										\
static inline struct bkey_s_##name						\
bkey_s_to_##name(struct bkey_s k)						\
{ assert(!k.k || k.k->type == VALUE_TYPE_##name);				\
  return (struct bkey_s_##name){ .k = k.k,					\
    .v = (struct bch_##name *)k.v }; }						\
										\
static inline struct bkey_s_c_##name						\
bkey_s_c_to_##name(struct bkey_s_c k)						\
{ assert(!k.k || k.k->type == VALUE_TYPE_##name);				\
  return (struct bkey_s_c_##name){ .k = k.k,					\
    .v = (const struct bch_##name *)k.v }; }					\
										\
static inline struct bkey_s_##name						\
name##_i_to_s(struct bkey_i_##name *k)						\
{ return (struct bkey_s_##name){ .k = &k->k, .v = &k->v }; }			\
										\
static inline struct bkey_s_c_##name						\
name##_i_to_s_c(const struct bkey_i_##name *k)					\
{ return (struct bkey_s_c_##name){ .k = &k->k, .v = &k->v }; }		\
										\
static inline struct bkey_s_##name						\
bkey_i_to_s_##name(struct bkey_i *k)						\
{ assert(!k || k->k.type == VALUE_TYPE_##name);					\
  return (struct bkey_s_##name){ .k = &k->k,					\
    .v = (struct bch_##name *)&k->v }; }					\
										\
static inline struct bkey_s_c_##name						\
bkey_i_to_s_c_##name(const struct bkey_i *k)					\
{ assert(!k || k->k.type == VALUE_TYPE_##name);					\
  return (struct bkey_s_c_##name){ .k = &k->k,					\
    .v = (const struct bch_##name *)&k->v }; }				\
										\
static inline struct bkey_i_##name *						\
bkey_##name##_init(struct bkey_i *_k)						\
{ struct bkey_i_##name *k = (struct bkey_i_##name *)_k;			\
  bkey_init(&k->k); memset(&k->v, 0, sizeof(k->v));				\
  k->k.type = VALUE_TYPE_##name;						\
  k->k.u64s = BKEY_U64s +							\
    (sizeof(struct bch_##name) + sizeof(uint64_t) - 1) / sizeof(uint64_t);	\
  return k; }

VALUE_TYPES()
#undef x

/* =================================================================
 * 辅助
 * ================================================================= */

static void print_header(const char *title)
{
	printf("\n========== %s ==========\n", title);
}

/* =================================================================
 * main — 优劣分场景演示
 * ================================================================= */

int main(void)
{
	printf("============================================================\n");
	printf(" bcachefs bkey 三种类型设计模式 — 独立案例演示\n");
	printf(" 场景: 简化的几何索引系统 (point/rect/tag)\n");
	printf(" 类型: bkey_i(inline构造)  bkey_s_c(split只读)  bkey_s(split修改)\n");
	printf("============================================================\n");

	/* =============================================================
	 * [优点 1] 三阶段类型安全
	 * ============================================================= */
	print_header("优点 1: 三阶段类型安全 [构造/只读/修改 分离]");

	printf("【point 键的完整生命周期】\n\n");

	struct bkey_i_point pt;
	bkey_point_init(&pt.k_i);
	pt.v.x = 42;
	pt.v.y = -7;
	printf("  [构造] bkey_i_point: sizeof=%zu, v=(%d,%d), type=%d, u64s=%d\n",
	       sizeof(pt), pt.v.x, pt.v.y, pt.k.type, pt.k.u64s);
	printf("    同一 malloc, key+value 物理连续, 适合批量构造\n\n");

	struct bkey_s_c_point sc = bkey_i_to_s_c_point(&pt.k_i);
	printf("  [只读] bkey_s_c_point: k->type=%d, v->(%d,%d)\n",
	       sc.k->type, sc.v->x, sc.v->y);
	printf("    const 指针, 下列代码无法编译:\n");
	printf("    sc.k->type = 0;  /* ❌ */   sc.v->x = 0;  /* ❌ */\n\n");

	struct bkey_s_point s = bkey_i_to_s_point(&pt.k_i);
	s.v->x = 100;
	s.v->y = 200;
	printf("  [修改] bkey_s_point: v->(%d,%d) ✅ 改成功了\n\n", s.v->x, s.v->y);

	printf("  如果只有一种 struct bkey* 类型:\n");
	printf("    - 要么全 const (改不了)   - 要么全可变 (遍历时不小心就写坏了)\n");
	printf("    三类型分离把「你不能写」编码到类型里, 编译器强制执行\n\n");

	/* =============================================================
	 * [优点 2] 匿名 union 自动 upcast, 零成本
	 * ============================================================= */
	print_header("优点 2: 匿名 union 自动 upcast（零成本）");

	struct bkey_i_rect r;
	bkey_rect_init(&r.k_i);
	r.v.x = 10; r.v.y = 20; r.v.w = 100; r.v.h = 200;

	struct bkey_s_rect sr = rect_i_to_s(&r);

	printf("  bkey_s_rect 的 union 成员:\n");
	printf("    sr.s    → 泛型 bkey_s     @ %p\n", (void*)&sr.s);
	printf("    sr.s_c  → 泛型 bkey_s_c   @ %p\n", (void*)&sr.s_c);
	printf("    sr.c    → bkey_s_c_rect   @ %p\n\n", (void*)&sr.c);
	printf("  它们共享内存起始地址, upcast 完全在编译器完成\n");
	printf("  向下转型同样省力:\n");
	struct bkey_s s_gen = { .k = &r.k, .v = (struct bch_val *)&r.v };
	struct bkey_s_rect sr2 = bkey_s_to_rect(s_gen);
	printf("    bkey_s_to_rect(泛型) → (%d,%d %dx%d) ✅\n\n",
	       sr2.v->x, sr2.v->y, sr2.v->w, sr2.v->h);

	/* =============================================================
	 * [优点 3] X-macro 批量生成保证一致性
	 * ============================================================= */
	print_header("优点 3: X-macro 一致性");

	unsigned n_types = 0;
#define x(name, nr, desc) n_types++;
	VALUE_TYPES()
#undef x
	printf("  VALUE_TYPES() %u 种类型, 每种 11 实体 = %u 个\n",
	       n_types, n_types * 11);
	printf("  加新类型: 加 1 行宏定义, 其余全部自动\n");
	printf("  手写代价: ~80 行/类型, 且跨类型一致性不可控\n\n");

	/* =============================================================
	 * [优点 4] assert 类型安全检查
	 * ============================================================= */
	print_header("优点 4: assert 类型安全检查");

	printf("  每个转换函数 assert(k->type == VALUE_TYPE_xxx):\n\n");

	/* fork 子进程演示 assert crash */
	struct bkey_i_point pt_bad;
	bkey_point_init(&pt_bad.k_i);
	pt_bad.v.x = 1; pt_bad.v.y = 2;
	struct bkey_s s_bad = bkey_i_to_s(&pt_bad.k_i);

#ifndef NDEBUG
	pid_t pid = fork();
	if (pid == 0) {
		bkey_s_to_rect(s_bad);
		_exit(0);
	}
	int status;
	waitpid(pid, &status, 0);
	if (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT)
		printf("  debug 构建: 类型不匹配 → SIGABRT ✅ 被拦截\n\n");
	else
		printf("  ? exit status=%d\n", status);
#else
	int32_t gx = bkey_s_to_rect(s_bad).v->x;
	printf("  NDEBUG 构建: 无 assert → 读到垃圾 x=%d ⚠️\n\n", gx);
#endif

	/* =============================================================
	 * [缺点 1] 宏不可调试
	 * ============================================================= */
	print_header("缺点 1: 宏展开不可调试 + 报错信息难读");

	printf("  X-macro 展开 ~7600 行不可见代码（38 类型 × 200 行）\n");
	printf("  编译报错行号指向 VALUE_TYPES() 调用点, 不是宏体内\n");
	printf("  gdb 单步步入转换函数, 看到的是宏展开后的晦涩代码\n");
	printf("  对学习者和调试者都是负担\n\n");

	/* =============================================================
	 * [缺点 2] 代码体积膨胀, 编译时间增长
	 * ============================================================= */
	print_header("缺点 2: 代码体积膨胀");

	printf("  %u 类型 × %u 实体 = %u 个\n", n_types, 11, n_types * 11);
	printf("  内核 38 种 → 418 实体\n");
	printf("  即使翻译单元只用 1 种类型, 也要解析全部宏展开\n");
	printf("  增大二进制体积 + 增加编译时间\n\n");

	/* =============================================================
	 * [缺点 3] assert/BUG_ON 双刃剑
	 * ============================================================= */
	print_header("缺点 3: assert/BUG_ON 的双刃剑");

	printf("  debug 构建: 每个转换函数多一次分支比较\n");
	printf("  内核最内层遍历循环中, 这个开销叠加\n\n");
	printf("  release 构建:\n");
	printf("    assert → NOP (本 demo), 但 BUG_ON(内核) 保留\n");
	printf("    磁盘数据损坏导致 type 错乱 → 遍历到该键即内核 panic\n");
	printf("    这是内核为了可靠性做的选择: 宁可 panic 也不传播坏数据\n\n");

	/* =============================================================
	 * [缺点 4] 匿名 union strict aliasing
	 * ============================================================= */
	print_header("缺点 4: 匿名 union strict aliasing");

	printf("  bkey_i_point 的 union { struct bkey k; struct bkey_i k_i; };\n");
	printf("  通过 k_i 写入、通过 k 读取, 在标准 C 中存在争议\n");
	printf("  GCC/Clang 实践中支持（共用前导公共序列）\n");
	printf("  内核通过 -fno-strict-aliasing 回避此问题\n");
	printf("  但在严格 -fstrict-aliasing 的项目中, 这是 UB\n\n");

	/* =============================================================
	 * [缺点 5] 学习曲线陡峭
	 * ============================================================= */
	print_header("缺点 5: 学习曲线陡峭");

	printf("  新开发者需要理解的 8 种转换函数签名:\n");
	printf("    bkey_i_to_##name      bkey_i_to_##name_c\n");
	printf("    bkey_s_to_##name      bkey_s_c_to_##name\n");
	printf("    ##name_i_to_s         ##name_i_to_s_c\n");
	printf("    bkey_i_to_s_##name    bkey_i_to_s_c_##name\n\n");
	printf("  以及三种类型的选择规则:\n");
	printf("    构造? → bkey_i    遍历? → bkey_s_c    修改? → bkey_s\n");
	printf("  以及 X-macro 的 #define x / #undef x 机制\n");
	printf("  以及每个转换函数带 assert, 到底会不会崩?\n\n");
	printf("  在纯 C 项目里这是合理的取舍, 但超出大部分应用开发的需要\n\n");

	/* =============================================================
	 * 汇总
	 * ============================================================= */
	print_header("汇总");

	printf("  ┌─────────────────────────────────┬─────────────────────────┐\n");
	printf("  │ 优点                            │ 缺点                    │\n");
	printf("  ├─────────────────────────────────┼─────────────────────────┤\n");
	printf("  │ 1. 三阶段类型安全, 编译器防误写 │ 5. 学习曲线陡峭         │\n");
	printf("  │ 2. 匿名 union 零成本 upcast     │ 4. strict aliasing 隐患 │\n");
	printf("  │ 3. X-macro 一致性保证            │ 1. 宏不可调试           │\n");
	printf("  │ 4. assert 类型检查捉 bug         │ 3. BUG_ON 宕机风险     │\n");
	printf("  │                                  │ 2. 代码体积膨胀         │\n");
	printf("  └─────────────────────────────────┴─────────────────────────┘\n\n");
	printf("  本质: 在纯 C 中「用宏模拟泛型」\n");
	printf("  用代码体积 + 学习成本 交换 类型安全 + 一致性\n");
	printf("  在文件系统（极致可靠性）场景下这笔交易划算\n");
	printf("  在普通应用开发场景下大幅超出必要复杂度\n\n");

	return 0;
}
