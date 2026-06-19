/*
 * val-types.c — bcachefs 值类型系统与 X-macro 教学示例
 *
 * 本程序演示两种 X-macro 模式（VAL_TYPES / INODE_FIELDS），
 * 及其在值类型枚举、get/set 访问器、序列化 pack/unpack、
 * key_normalize 生命周期管理、以及类型安全转换函数中的运用。
 *
 * VAL_TYPES X-macro 每类型自动生成（对齐 fs/btree/bkey_types.h）：
 *   3 struct  — bkey_i_##name（inline 值，匿名 union 安全向上转型）
 *               bkey_s_c_##name / bkey_s_##name（拆分值指针）
 *   8 函数    — 类型安全向下转换（bkey_i_to_##name 等）
 *   1 函数    — bkey_##name##_init（键初始化）
 *
 * 6 种值类型覆盖率内核实际类型多样性:
 *   bch_deleted       — 零长度值（占位，不占用磁盘空间）
 *   bch_whiteout      — 零长度值（显式删除标记）
 *   bch_extent        — 变长 tagged entry 数组 → 遍历
 *   bch_inode         — 固定大小字段结构（42 字节）→ memcpy + 字段偏移
 *   bch_dirent        — 变长字符串（null-terminated）→ strnlen 安全截断
 *   bch_btree_ptr_v2  — 固定头 + 变长设备指针 → memcpy 头 + 遍历 ptr
 *
 * 关键机制:
 *   key_normalize — 返回 1（KEEP）/ 0（DROP），对齐内核 bch2_key_normalize()
 *   ops 表调度   — bch2_bkey_ops[] 教学简化版，含 pack/unpack/normalize
 *
 * 编译: gcc -Wall -Wextra -O2 -std=gnu11 -o val-types val-types.c
 * 运行: ./val-types
 * 文档: ./val-types --docs > types-doc.md
 *
 * 内核对照:
 *   VAL_TYPES()      → BCH_BKEY_TYPES()    bcachefs_format.h:416
 *   INODE_FIELDS()   → bkey_fields()       bkey.h:631
 *   bkey_i_##name    → BKEY_TYPE 宏        bkey.h
 *   bch_val_ops[]    → bch2_bkey_ops[]     bkey_methods.c
 *   key_normalize    → bch2_key_normalize() btree_key_cache.c
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#ifndef DIV_ROUND_UP
#define DIV_ROUND_UP(n, d)	(((n) + (d) - 1) / (d))
#endif

/* ===== 基础类型 ===== */
/* bpos — 键位置 */
struct bpos {
	uint64_t	inode;
	uint64_t	offset;
	uint32_t	snapshot;
} __attribute__((packed));

/* bch_extent_ptr — 设备指针（单 u64 bitfield）*/
struct bch_extent_ptr {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	uint64_t	type:1,
			cached:1,
			unused:1,
			unwritten:1,
			offset:44,
			dev:8,
			gen:8;
#else
	uint64_t	gen:8,
			dev:8,
			offset:44,
			unwritten:1,
			unused:1,
			cached:1,
			type:1;
#endif
} __attribute__((packed, aligned(8)));

/* bch_extent_crc32 — 32 位校验和 entry */
struct bch_extent_crc32 {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	uint32_t	type:5,
			compressed:1,
			pad0:5,
			csum_type:2,
			nonce:12,
			compressed_size:5,
			uncompressed_size:5,
			pad1:1;
#else
	uint32_t	pad1:1,
			uncompressed_size:5,
			compressed_size:5,
			nonce:12,
			csum_type:2,
			pad0:5,
			compressed:1,
			type:5;
#endif
	uint32_t	csum_lo;
} __attribute__((packed, aligned(4)));

/* Tagged entry union */
union bch_extent_entry {
	uint64_t		type;
	struct bch_extent_ptr		ptr;
	struct bch_extent_crc32		crc32;
};

/* ===== bkey 基础结构 ===== */

/* bch_val — 抽象值基类（零长数组）*/
struct bch_val {
	uint64_t	v[0];
};

/*
 * bkey — 未打包键（教学简化版）
 * 40 字节 = 5 u64s (BKEY_U64s)
 */
struct bkey {
	uint8_t		u64s;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	uint8_t		format:7,
			needs_whiteout:1;
#else
	uint8_t		needs_whiteout:1,
			format:7;
#endif
	uint8_t		type;
	uint8_t		pad0;
	uint8_t		_pad[36];
} __attribute__((packed));

#define BKEY_U64s	(sizeof(struct bkey) / sizeof(uint64_t))

/* bkey_i — 带内联值的可变键 */
struct bkey_i {
	struct bkey	k;
	struct bch_val	v;
};

/*
 * bkey_s / bkey_s_c — 带拆分值（指针对）的键，对齐内核 bkey_types.h:134
 *
 * bkey_s_c = const key + const value（只读查询用）
 * bkey_s   = mutable key + mutable value（修改用）
 * bkey_i   = inline value（构造/插入用）
 *
 * 三者关系：bkey_i_to_s(k) 从 inline 转到 split
 */
struct bkey_s_c {
	const struct bkey	*k;
	const struct bch_val	*v;
};

struct bkey_s {
	union {
	struct {
		struct bkey	*k;
		struct bch_val	*v;
	};
	struct bkey_s_c		s_c;
	};
};

#define bkey_s_null	((struct bkey_s)   { .k = NULL })
#define bkey_s_c_null	((struct bkey_s_c) { .k = NULL })

static inline struct bkey_s bkey_i_to_s(struct bkey_i *k)
{
	return (struct bkey_s) { .k = &k->k, .v = &k->v };
}

static inline struct bkey_s_c bkey_i_to_s_c(const struct bkey_i *k)
{
	return (struct bkey_s_c) { .k = &k->k, .v = &k->v };
}

static inline void bkey_init(struct bkey *k)
{
	memset(k, 0, sizeof(*k));
	k->u64s = BKEY_U64s;
}

/* ===== 值类型 struct 定义 ===== */

/*
 * bch_inode — 教学简化 inode 值类型
 *
 * 固定大小，pack/unpack 使用 memcpy。
 * 不自含 struct bch_val（对齐内核模式），
 * bkey_i_inode.v 直接提供 struct bch_inode 类型。
 */
struct bch_inode {
	uint16_t	mode;
	uint32_t	uid;
	uint32_t	gid;
	uint64_t	size;
	uint64_t	atime;
	uint64_t	mtime;
	uint64_t	ctime;
} __attribute__((packed));
/* sizeof = 42 Bytes → min_size = DIV_ROUND_UP(42,8) = 6 */

/* bch_extent — 数据 extent（变长 tagged entry 数组）*/
struct bch_extent {
	union bch_extent_entry	start[];
};

/*
 * bch_btree_ptr_v2 — btree 节点指针
 * 固定头（mem_ptr, seq, ...）+ 变长设备指针数组
 */
struct bch_btree_ptr_v2 {
	uint64_t		mem_ptr;
	uint64_t		seq;
	uint16_t		sectors_written;
	uint16_t		flags;
	struct bpos		min_key;
	struct bch_extent_ptr	start[];
} __attribute__((packed, aligned(8)));

/* ===== 零长度值类型 ===== */

/*
 * bch_deleted / bch_whiteout — 零长度值类型
 *
 * 内核：deleted=whiteout 标记（键本身带元数据，值不占空间）*/
struct bch_deleted { uint64_t	_unused[0]; };
struct bch_whiteout { uint64_t	_unused[0]; };

/* ===== dirent — 变长字符串值类型 ===== */

/*
 * bch_dirent — 目录项（教学简化版）
 *
 * 序列化格式：
 *   u64 0: d_inode
 *   bytes 8+: d_name 字符串（含 null 终止符），填充到 u64 边界
 *
 * 变长类型：runtime size = sizeof(d_inode) + strlen(name) + 1
 * 内核对应：fs/bcachefs_format.h:118 — struct bch_dirent
 */
struct bch_dirent {
	uint64_t	d_inode;
	uint8_t		d_name[0];
} __attribute__((packed));

/* ===== X-macro 头文件（VAL_TYPES / INODE_FIELDS 宏定义）===== */
#include "types-defs.h"

/* ================================================================
 * X-macro 展开 (1-5)：VAL_TYPES() 五用途
 * ================================================================
 */

/* (1) 生成 enum bkey_type — 对齐 BCH_BKEY_TYPES() */
enum bkey_type {
#define x(name, nr, flags, desc) KEY_TYPE_##name = nr,
	VAL_TYPES()
#undef x
};

/* (2) 生成类型名称数组 */
static const char * const bkey_type_name[] = {
#define x(name, nr, flags, desc) [nr] = #name,
	VAL_TYPES()
#undef x
};

/* (3) 生成类型描述数组 */
static const char * const bkey_type_desc[] = {
#define x(name, nr, flags, desc) [nr] = desc,
	VAL_TYPES()
#undef x
};

/* (4) 生成类型化结构体、转换函数和初始化函数
 *     对齐内核 fs/btree/bkey_types.h:187-296
 *
 * 每类型生成：
 *   3 struct    — bkey_i_##name / bkey_s_c_##name / bkey_s_##name
 *   8 functions — 6 种类型安全向下转换 + 2 种 inline→split 转换
 *   1 init      — bkey_##name##_init()
 *
 * 关键设计：
 *   bkey_i_##name 使用匿名 union 实现安全向上转换
 *   （bkey_i_inode.k_i 可当 struct bkey_i* 用，无需强制转型）
 *   转换函数用 assert 代替内核的 BUG_ON
 */
#define x(name, nr, flags, desc)					\
struct bkey_i_##name {							\
	union {								\
		struct bkey		k;				\
		struct bkey_i		k_i;				\
	};								\
	struct bch_##name		v;				\
};									\
									\
struct bkey_s_c_##name {						\
	union {								\
	struct {							\
		const struct bkey	*k;				\
		const struct bch_##name	*v;				\
	};								\
	struct bkey_s_c			s_c;				\
	};								\
};									\
									\
struct bkey_s_##name {							\
	union {								\
	struct {							\
		struct bkey		*k;				\
		struct bch_##name	*v;				\
	};								\
	struct bkey_s_c_##name		c;				\
	struct bkey_s			s;				\
	struct bkey_s_c			s_c;				\
	};								\
};									\
									\
static inline struct bkey_i_##name *					\
bkey_i_to_##name(struct bkey_i *k)					\
{									\
	assert(!k || k->k.type == KEY_TYPE_##name);			\
	return (struct bkey_i_##name *)k;				\
}									\
									\
static inline const struct bkey_i_##name *				\
bkey_i_to_##name##_c(const struct bkey_i *k)				\
{									\
	assert(!k || k->k.type == KEY_TYPE_##name);			\
	return (const struct bkey_i_##name *)k;				\
}									\
									\
static inline struct bkey_s_##name					\
bkey_s_to_##name(struct bkey_s k)					\
{									\
	assert(!k.k || k.k->type == KEY_TYPE_##name);			\
	return (struct bkey_s_##name) {					\
		.k = k.k,						\
		.v = (struct bch_##name *)k.v,				\
	};								\
}									\
									\
static inline struct bkey_s_c_##name					\
bkey_s_c_to_##name(struct bkey_s_c k)					\
{									\
	assert(!k.k || k.k->type == KEY_TYPE_##name);			\
	return (struct bkey_s_c_##name) {				\
		.k = k.k,						\
		.v = (const struct bch_##name *)k.v,			\
	};								\
}									\
									\
static inline struct bkey_s_##name					\
name##_i_to_s(struct bkey_i_##name *k)					\
{									\
	return (struct bkey_s_##name) {					\
		.k = &k->k,						\
		.v = &k->v,						\
	};								\
}									\
									\
static inline struct bkey_s_c_##name					\
name##_i_to_s_c(const struct bkey_i_##name *k)				\
{									\
	return (struct bkey_s_c_##name) {				\
		.k = &k->k,						\
		.v = &k->v,						\
	};								\
}									\
									\
static inline struct bkey_s_##name					\
bkey_i_to_s_##name(struct bkey_i *k)					\
{									\
	assert(!k || k->k.type == KEY_TYPE_##name);			\
	return (struct bkey_s_##name) {					\
		.k = &k->k,						\
		.v = (struct bch_##name *)&k->v,			\
	};								\
}									\
									\
static inline struct bkey_s_c_##name					\
bkey_i_to_s_c_##name(const struct bkey_i *k)				\
{									\
	assert(!k || k->k.type == KEY_TYPE_##name);			\
	return (struct bkey_s_c_##name) {				\
		.k = &k->k,						\
		.v = (const struct bch_##name *)&k->v,			\
	};								\
}									\
									\
static inline struct bkey_i_##name *					\
bkey_##name##_init(struct bkey_i *_k)					\
{									\
	struct bkey_i_##name *k = (struct bkey_i_##name *)_k;		\
	bkey_init(&k->k);						\
	memset(&k->v, 0, sizeof(k->v));					\
	k->k.type = KEY_TYPE_##name;					\
	k->k.u64s = BKEY_U64s						\
		+ DIV_ROUND_UP(sizeof(struct bch_##name),		\
			       sizeof(uint64_t));			\
	return k;							\
}
VAL_TYPES()
#undef x

/* ================================================================
 * X-macro 展开 (a-c)：INODE_FIELDS() 三用途
 * ================================================================
 */

/* (a) 生成 get/set 类型安全访问器 */
#define x(name, member, type, bits, desc)				\
	static inline type bch_inode_get_##name(			\
		const struct bch_inode *v) { return v->member; }	\
	static inline void bch_inode_set_##name(			\
		struct bch_inode *v, type val) { v->member = val; }
INODE_FIELDS()
#undef x

/* (b) 生成通用 set/get 调度（字段编号 enum + switch dispatch）*/
enum inode_fields {
#define x(name, member, type, bits, desc) INODE_##name,
	INODE_FIELDS()
#undef x
};

static void bch_inode_set_field(struct bch_inode *v,
				unsigned field, uint64_t val)
{
	switch (field) {
#define x(name, member, type, bits, desc)		\
	case INODE_##name: v->member = val; break;
	INODE_FIELDS()
#undef x
	default: break;
	}
}

static uint64_t bch_inode_get_field(const struct bch_inode *v,
				    unsigned field)
{
	switch (field) {
#define x(name, member, type, bits, desc)		\
	case INODE_##name: return v->member;
	INODE_FIELDS()
#undef x
	default: return 0;
	}
}

/* (c) 字段文档打印（--docs 模式）*/
static void print_inode_fields(void)
{
	printf("| Field | Type | Bits | Description |\n");
	printf("|-------|------|------|-------------|\n");
#define x(name, member, type, bits, desc)				\
	printf("| %-12s | %-10s | %-3d | %s |\n", #name, #type, bits, desc);
	INODE_FIELDS()
#undef x
}

/* ================================================================
 * 值序列化 pack/unpack
 * ================================================================
 */

/*
 * 所有 pack/unpack 函数使用统一签名以适配 ops 表:
 *   pack:   dst → bkey_i（输出），src → const void（值数据源）
 *   unpack: dst → void（值数据目标），src → const bkey_i（键+值源）
 *
 * 对于固定大小类型（bch_inode），pack 从 struct bch_inode * 拷贝固定字节。
 * 对于变长类型（bch_extent / bch_btree_ptr_v2），
 *   pack 假设 src 是已构造好的值数据（flat buffer），
 *   且 dst->k.u64s 已由调用者设为 BKEY_U64s + val_u64s，
 *   函数只做 memcpy 到 dst->v 并设置 type。
 */

static unsigned bch_inode_pack(struct bkey_i *dst, const void *src)
{
	unsigned u64s = DIV_ROUND_UP(sizeof(struct bch_inode),
				     sizeof(uint64_t));
	memcpy(&dst->v, src, sizeof(struct bch_inode));
	dst->k.u64s = BKEY_U64s + u64s;
	dst->k.type = KEY_TYPE_inode;
	return u64s;
}

static unsigned bch_inode_unpack(void *dst, const struct bkey_i *src)
{
	unsigned u64s = DIV_ROUND_UP(sizeof(struct bch_inode),
				     sizeof(uint64_t));
	memcpy(dst, &src->v, sizeof(struct bch_inode));
	return u64s;
}

/* bch_extent: 源 buffer 已含 tagged entries 值数据 */
static unsigned bch_extent_pack(struct bkey_i *dst, const void *src)
{
	unsigned val_u64s = dst->k.u64s - BKEY_U64s;
	memcpy(&dst->v, src, val_u64s * sizeof(uint64_t));
	dst->k.type = KEY_TYPE_extent;
	return val_u64s;
}

static unsigned bch_extent_unpack(void *dst, const struct bkey_i *src)
{
	unsigned val_u64s = src->k.u64s - BKEY_U64s;
	memcpy(dst, &src->v, val_u64s * sizeof(uint64_t));
	return val_u64s;
}

/* bch_btree_ptr_v2: 源 buffer 已含固定头 + ptr 数组 */
static unsigned bch_btree_ptr_v2_pack(struct bkey_i *dst, const void *src)
{
	unsigned val_u64s = dst->k.u64s - BKEY_U64s;
	memcpy(&dst->v, src, val_u64s * sizeof(uint64_t));
	dst->k.type = KEY_TYPE_btree_ptr_v2;
	return val_u64s;
}

static unsigned bch_btree_ptr_v2_unpack(void *dst, const struct bkey_i *src)
{
	unsigned val_u64s = src->k.u64s - BKEY_U64s;
	memcpy(dst, &src->v, val_u64s * sizeof(uint64_t));
	return val_u64s;
}

/* bunpc — null-terminated 变长字符串序列化 */

static unsigned bch_dirent_pack(struct bkey_i *dst, const void *src)
{
	const struct bch_dirent *d = src;
	unsigned name_len = strlen((const char *)d->d_name) + 1;
	unsigned val_u64s = DIV_ROUND_UP(sizeof(uint64_t) + name_len,
					 sizeof(uint64_t));

	memcpy(&dst->v, &d->d_inode, sizeof(uint64_t));
	memcpy((uint8_t *)dst->v.v + sizeof(uint64_t),
	       d->d_name, name_len);

	dst->k.u64s = BKEY_U64s + val_u64s;
	dst->k.type = KEY_TYPE_dirent;
	return val_u64s;
}

static unsigned bch_dirent_unpack(void *dst, const struct bkey_i *src)
{
	struct bch_dirent *d = dst;
	unsigned val_u64s = src->k.u64s - BKEY_U64s;
	unsigned name_max = val_u64s * sizeof(uint64_t) - sizeof(uint64_t);

	d->d_inode = ((const uint64_t *)src->v.v)[0];
	if (name_max) {
		/* 只复制实际字符串（含 null），不覆盖填充字节 */
		unsigned name_len = strnlen((const char *)src->v.v
					    + sizeof(uint64_t), name_max) + 1;
		memcpy(d->d_name, (const uint8_t *)src->v.v
		       + sizeof(uint64_t), name_len);
	}
	return val_u64s;
}

/* ===== 零长度值类型 pack/unpack ===== */

static unsigned bch_deleted_pack(struct bkey_i *dst, const void *src)
{
	(void)src;
	dst->k.u64s = BKEY_U64s;	/* 零值 */
	dst->k.type = KEY_TYPE_deleted;
	return 0;
}

static unsigned bch_deleted_unpack(void *dst, const struct bkey_i *src)
{
	(void)dst; (void)src;
	return 0;
}

static unsigned bch_whiteout_pack(struct bkey_i *dst, const void *src)
{
	(void)src;
	dst->k.u64s = BKEY_U64s;	/* 零值 */
	dst->k.type = KEY_TYPE_whiteout;
	return 0;
}

static unsigned bch_whiteout_unpack(void *dst, const struct bkey_i *src)
{
	(void)dst; (void)src;
	return 0;
}

/* ================================================================
 * key_normalize — 键生命周期管理（ops 表调度）
 *
 * 内核语义：
 *   deleted → 键已被删除，合并/读出时丢弃
 *   whiteout → 显式删除标记，检查后丢弃
 *   其余类型 → 有效键，保留
 *
 * 返回：1=保留，0=丢弃
 * ================================================================
 */

static int bch_deleted_normalize(struct bkey_i *k)
	{ (void)k; return 0; }
static int bch_whiteout_normalize(struct bkey_i *k)
	{ (void)k; return 0; }
static int bch_inode_normalize(struct bkey_i *k)
	{ (void)k; return 1; }
static int bch_extent_normalize(struct bkey_i *k)
	{ (void)k; return 1; }
static int bch_btree_ptr_v2_normalize(struct bkey_i *k)
	{ (void)k; return 1; }
static int bch_dirent_normalize(struct bkey_i *k)
	{ (void)k; return 1; }

/* ================================================================
 * X-macro 展开 (5)：ops 调度表
 * ================================================================
 */

struct val_ops {
	unsigned (*pack)(struct bkey_i *, const void *);
	unsigned (*unpack)(void *, const struct bkey_i *);
	int	(*normalize)(struct bkey_i *);
	void	(*set)(void *v, unsigned field_id, uint64_t val);
	uint64_t (*get)(const void *v, unsigned field_id);
	size_t	min_size;
};

/*
 * ops 表：pack/unpack + key_normalize + set/get（inode 专用）
 *
 * set/get 用显式函数指针 cast（struct bch_inode * → void * 安全向下转换）
 * normalize 对所有类型均正确（零长度类型返回 0 = drop）
 * min_size 通过 sizeof(struct) 自动推导，零长度类型自动为 0
 */
static const struct val_ops bch_val_ops[] = {
#define x(name, nr, flags, desc) [nr] = {				\
		.pack   = bch_##name##_pack,				\
		.unpack = bch_##name##_unpack,				\
		.normalize = bch_##name##_normalize,			\
		.set    = (void (*)(void *, unsigned, uint64_t))		\
			  bch_inode_set_field,				\
		.get    = (uint64_t (*)(const void *, unsigned))	\
			  bch_inode_get_field,				\
		.min_size = sizeof(struct bch_##name)			\
			    ? DIV_ROUND_UP(sizeof(struct bch_##name),	\
					   sizeof(uint64_t))		\
			    : 0,					\
	},
	VAL_TYPES()
#undef x
};

/* ================================================================
 * 辅助函数
 * ================================================================
 */

static void hex_dump(const char *label, const uint64_t *data, unsigned u64s)
{
	printf("  %s (%d u64s): ", label, u64s);
	for (unsigned i = 0; i < u64s; i++)
		printf("%s%016lx", i ? " " : "", (unsigned long)data[i]);
	printf("\n");
}

static void print_header(const char *title)
{
	printf("\n--- %s ---\n", title);
}

/* ================================================================
 * main() — 类型系统演示 + roundtrip 测试
 * ================================================================
 */

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--docs") == 0) {
		print_inode_fields();
		return 0;
	}

	/* bkey_type_name 数组大小 = 最大 nr + 1 = 19（因 designated init [18]）*/
	unsigned type_array_size = sizeof(bkey_type_name)
				   / sizeof(bkey_type_name[0]);

	printf("bcachefs 值类型系统 — X-macro + 序列化 roundtrip\n");
	printf("宏模式: BCH_BKEY_TYPES() + bkey_fields() 对齐内核\n");
	printf("类型: %d 种（deleted | whiteout | extent | inode | dirent | btree_ptr_v2）\n",
	       type_array_size);

	/* ---- 演示 1: VAL_TYPES() 类型枚举 + key_normalize 语义 ---- */
	print_header("演示 1: VAL_TYPES() 类型枚举");
	struct bkey_i normalize_dummy;
	memset(&normalize_dummy, 0, sizeof(normalize_dummy));
	for (unsigned i = 0; i < type_array_size; i++) {
		if (bkey_type_name[i]) {
			normalize_dummy.k.type = i;
			int keep = bch_val_ops[i].normalize
				   ? bch_val_ops[i].normalize(&normalize_dummy)
				   : 1;
			printf("  KEY_TYPE_%-14s = %2u  %-5s (%s)\n",
			       bkey_type_name[i], i,
			       keep ? "KEEP" : "DROP",
			       bkey_type_desc[i]);
		}
	}
	printf("  ---\n");
	printf("  内核 key_normalize 语义: deleted/whiteout 在合并时 DROP\n");

	/* ---- 演示 2: INODE_FIELDS() ---- */
	print_header("演示 2: INODE_FIELDS() 字段宏");

	struct bch_inode inode;
	memset(&inode, 0, sizeof(inode));

	/* 类型安全访问器 */
	bch_inode_set_mode(&inode, 0755);
	bch_inode_set_uid(&inode, 1000);
	bch_inode_set_gid(&inode, 100);
	bch_inode_set_size(&inode, 4096);

	printf("  set: mode=0755 uid=1000 gid=100 size=4096\n");
	printf("  get: mode=%04o uid=%u gid=%u size=%lu\n",
	       bch_inode_get_mode(&inode),
	       bch_inode_get_uid(&inode),
	       bch_inode_get_gid(&inode),
	       (unsigned long)bch_inode_get_size(&inode));

	/* 通用 set/get（通过字段 enum）*/
	bch_inode_set_field(&inode, INODE_atime, 1700000000);
	bch_inode_set_field(&inode, INODE_mtime, 1700000001);
	bch_inode_set_field(&inode, INODE_ctime, 1700000002);

	printf("  set_field: atime=1700000000 mtime=1700000001 ctime=1700000002\n");
	printf("  get_field: atime=%lu mtime=%lu ctime=%lu\n",
	       (unsigned long)bch_inode_get_field(&inode, INODE_atime),
	       (unsigned long)bch_inode_get_field(&inode, INODE_mtime),
	       (unsigned long)bch_inode_get_field(&inode, INODE_ctime));

	/* ---- 演示 3: inode roundtrip ---- */
	print_header("演示 3: inode roundtrip");

	struct bkey_i_inode inode_key;
	memset(&inode_key, 0, sizeof(inode_key));

	unsigned u64s = bch_inode_pack((struct bkey_i *)&inode_key, &inode);
	printf("  pack: u64s=%u total=%u type=%u\n",
	       u64s, inode_key.k.u64s, inode_key.k.type);

	hex_dump("packed", (const uint64_t *)&inode_key,
		 inode_key.k.u64s);

	struct bch_inode inode2;
	memset(&inode2, 0, sizeof(inode2));
	bch_inode_unpack(&inode2, (struct bkey_i *)&inode_key);

	int inode_ok = (inode2.mode == 0755 &&
			inode2.uid  == 1000 &&
			inode2.gid  == 100  &&
			inode2.size == 4096);
	printf("  unpack verify: %s (mode=%04o uid=%u gid=%u size=%lu)\n",
	       inode_ok ? "PASS" : "FAIL",
	       inode2.mode, inode2.uid, inode2.gid,
	       (unsigned long)inode2.size);

	/* ---- 演示 4: btree_ptr_v2 roundtrip ---- */
	print_header("演示 4: btree_ptr_v2 roundtrip");

	/* 构造源值数据 */
#define PTR_V2_HDR_U64S	(offsetof(struct bch_btree_ptr_v2, start)	\
			 / sizeof(uint64_t)) /* 5 u64s */
#define PTR_V2_PTRS	2

	uint64_t ptr_src_raw[PTR_V2_HDR_U64S + PTR_V2_PTRS];
	memset(ptr_src_raw, 0, sizeof(ptr_src_raw));
	struct bch_btree_ptr_v2 *ptr_src = (struct bch_btree_ptr_v2 *)ptr_src_raw;
	ptr_src->mem_ptr = 0xdeadbeef;
	ptr_src->seq = 42;
	ptr_src->sectors_written = 256;
	ptr_src->min_key.inode = 100;
	ptr_src->min_key.offset = 0;
	ptr_src->start[0].dev = 1;
	ptr_src->start[0].offset = 1024;
	ptr_src->start[1].dev = 2;
	ptr_src->start[1].offset = 2048;

	/* 分配足够大的 bkey_i 缓冲区 */
	unsigned ptr_val_u64s = PTR_V2_HDR_U64S + PTR_V2_PTRS;
	uint64_t ptr_key_raw[BKEY_U64s + ptr_val_u64s];
	struct bkey_i_btree_ptr_v2 *ptr_key = (struct bkey_i_btree_ptr_v2 *)ptr_key_raw;
	memset(ptr_key_raw, 0, sizeof(ptr_key_raw));
	ptr_key->k.u64s = BKEY_U64s + ptr_val_u64s; /* 通知 pack 值大小 */

	unsigned ptr_packed = bch_btree_ptr_v2_pack(
		(struct bkey_i *)ptr_key, ptr_src_raw);
	printf("  pack: u64s=%u total=%u type=%u\n",
	       ptr_packed, ptr_key->k.u64s, ptr_key->k.type);

	hex_dump("packed", (const uint64_t *)ptr_key,
		 ptr_key->k.u64s);

	/* unpack 到相同布局的 buffer */
	uint64_t ptr_out_raw[PTR_V2_HDR_U64S + PTR_V2_PTRS];
	memset(ptr_out_raw, 0, sizeof(ptr_out_raw));
	bch_btree_ptr_v2_unpack(ptr_out_raw, (struct bkey_i *)ptr_key);

	struct bch_btree_ptr_v2 *ptr_out = (struct bch_btree_ptr_v2 *)ptr_out_raw;
	int ptr_ok = (ptr_out->mem_ptr == 0xdeadbeef &&
		      ptr_out->seq == 42 &&
		      ptr_out->sectors_written == 256 &&
		      ptr_out->min_key.inode == 100 &&
		      ptr_out->start[0].dev == 1 &&
		      ptr_out->start[1].dev == 2);
	printf("  unpack verify: %s (mem_ptr=0x%lx seq=%lu sectors=%u "
	       "dev0=%u dev1=%u)\n",
	       ptr_ok ? "PASS" : "FAIL",
	       (unsigned long)ptr_out->mem_ptr,
	       (unsigned long)ptr_out->seq,
	       ptr_out->sectors_written,
	       ptr_out->start[0].dev,
	       ptr_out->start[1].dev);

	/* ---- 演示 5: extent roundtrip ---- */
	print_header("演示 5: extent roundtrip");

	/* 构造 3 个 tagged entries: ptr + crc32 + ptr
	 * 注意 sizeof(union bch_extent_entry) = 16（因 crc32 占 12 字节，对齐至 8）
	 */
#define EXTENT_ENTRIES	3
	union bch_extent_entry ext_src[EXTENT_ENTRIES];
	memset(ext_src, 0, sizeof(ext_src));

	ext_src[0].ptr.dev = 1;
	ext_src[0].ptr.offset = 4096;
	ext_src[1].crc32.type = 1;
	ext_src[1].crc32.csum_lo = 0xaabbccdd;
	ext_src[1].crc32.compressed_size = 8;
	ext_src[1].crc32.uncompressed_size = 16;
	ext_src[2].ptr.dev = 2;
	ext_src[2].ptr.offset = 8192;

	/* 值大小 = entries * sizeof(union) / sizeof(u64) = 3 * 2 = 6 u64s */
	unsigned ext_val_u64s = sizeof(ext_src) / (sizeof(uint64_t));
	uint64_t ext_key_raw[BKEY_U64s + ext_val_u64s];
	struct bkey_i_extent *ext_key = (struct bkey_i_extent *)ext_key_raw;
	memset(ext_key_raw, 0, sizeof(ext_key_raw));
	ext_key->k.u64s = BKEY_U64s + ext_val_u64s;

	unsigned ext_packed = bch_extent_pack(
		(struct bkey_i *)ext_key, ext_src);
	printf("  pack: u64s=%u total=%u type=%u (%d entries)\n",
	       ext_packed, ext_key->k.u64s, ext_key->k.type, EXTENT_ENTRIES);

	hex_dump("packed", (const uint64_t *)ext_key,
		 ext_key->k.u64s);

	/* unpack */
	union bch_extent_entry ext_out[EXTENT_ENTRIES];
	memset(ext_out, 0, sizeof(ext_out));
	bch_extent_unpack(ext_out, (struct bkey_i *)ext_key);

	int ext_ok = (ext_out[0].ptr.dev == 1 &&
		      ext_out[0].ptr.offset == 4096 &&
		      ext_out[1].crc32.type == 1 &&
		      ext_out[1].crc32.csum_lo == 0xaabbccdd &&
		      ext_out[2].ptr.dev == 2);
	printf("  unpack verify: %s (ptr0.dev=%u ptr0.off=%lu "
	       "crc32.type=%u csum=0x%x ptr1.dev=%u)\n",
	       ext_ok ? "PASS" : "FAIL",
	       ext_out[0].ptr.dev,
	       (unsigned long)ext_out[0].ptr.offset,
	       ext_out[1].crc32.type,
	       ext_out[1].crc32.csum_lo,
	       ext_out[2].ptr.dev);

	/* ---- 演示 7: dirent roundtrip（变长字符串序列化）---- */
	print_header("演示 7: dirent roundtrip（变长字符串序列化）");

	const char *dname = "hello.txt";
	unsigned dname_len = strlen(dname) + 1;
	uint8_t dirent_src_buf[sizeof(struct bch_dirent) + dname_len];
	memset(dirent_src_buf, 0, sizeof(dirent_src_buf));
	struct bch_dirent *dirent_src = (struct bch_dirent *)dirent_src_buf;
	dirent_src->d_inode = 42;
	memcpy(dirent_src->d_name, dname, dname_len);

	unsigned dval_u64s = DIV_ROUND_UP(sizeof(uint64_t) + dname_len,
					  sizeof(uint64_t));
	uint64_t dirent_key_raw[BKEY_U64s + dval_u64s];
	memset(dirent_key_raw, 0, sizeof(dirent_key_raw));

	struct bkey *dirent_bk = (struct bkey *)dirent_key_raw;
	dirent_bk->u64s = BKEY_U64s + dval_u64s;

	unsigned dent_packed = bch_dirent_pack(
		(struct bkey_i *)dirent_key_raw, dirent_src_buf);
	/* 通过 bkey* 读 header（避免 strict aliasing 问题）*/
	printf("  pack: u64s=%u total=%u type=%u name='%s'\n",
	       dent_packed, dirent_bk->u64s, dirent_bk->type, dname);

	hex_dump("packed", dirent_key_raw,
		 dirent_bk->u64s);

	uint8_t dirent_out_buf[sizeof(struct bch_dirent) + dname_len];
	memset(dirent_out_buf, 0, sizeof(dirent_out_buf));
	bch_dirent_unpack(dirent_out_buf, (struct bkey_i *)dirent_key_raw);

	struct bch_dirent *dirent_out = (struct bch_dirent *)dirent_out_buf;
	int dirent_ok = (dirent_out->d_inode == 42 &&
			 strcmp((const char *)dirent_out->d_name, dname) == 0);
	printf("  unpack verify: %s (inode=%lu name='%s')\n",
	       dirent_ok ? "PASS" : "FAIL",
	       (unsigned long)dirent_out->d_inode,
	       (const char *)dirent_out->d_name);

	/* ---- 演示 8: key_normalize 生命周期演示 ---- */
	print_header("演示 8: key_normalize 生命周期策略");

	struct {
		int	type;
		int	(*normalize)(struct bkey_i *);
		const char *name;
	} normalize_tests[] = {
		{ KEY_TYPE_deleted,  bch_deleted_normalize,  "deleted" },
		{ KEY_TYPE_whiteout, bch_whiteout_normalize, "whiteout" },
		{ KEY_TYPE_inode,     bch_inode_normalize,     "inode" },
		{ KEY_TYPE_extent,    bch_extent_normalize,    "extent" },
		{ KEY_TYPE_dirent,    bch_dirent_normalize,    "dirent" },
		{ KEY_TYPE_btree_ptr_v2, bch_btree_ptr_v2_normalize,
							"btree_ptr_v2" },
	};
	for (unsigned i = 0; i < ARRAY_SIZE(normalize_tests); i++) {
		struct bkey_i k;
		memset(&k, 0, sizeof(k));
		k.k.type = normalize_tests[i].type;
		int keep = normalize_tests[i].normalize(&k);
		printf("  %-14s → %s\n",
		       normalize_tests[i].name,
		       keep ? "KEEP（存活）" : "DROP（丢弃）");
	}

	/* ---- 演示 9: 类型安全转换函数与 init 函数 ---- */
	print_header("演示 9: 类型安全转换函数与 init 函数");

	struct bkey_i_inode init_inode;
	bkey_inode_init(&init_inode.k_i);
	int init_ok = (init_inode.k.type == KEY_TYPE_inode &&
		       init_inode.k.u64s == BKEY_U64s + 6);
	if (init_ok)
		printf("  bkey_inode_init:  type=%u u64s=%u ✓\n",
		       init_inode.k.type, init_inode.k.u64s);

	/* 验证 bkey_i_to_##name 向下转换通路 */
	struct bkey_i_inode *typed = bkey_i_to_inode(&init_inode.k_i);
	assert(typed == &init_inode);

	/* 验证 inline → split 转换 */
	struct bkey_s_inode s = inode_i_to_s(&init_inode);
	assert(s.k == &init_inode.k && s.v == &init_inode.v);

	/* 验证泛型 → 类型安全向下转换 */
	struct bkey_s s_gen = bkey_i_to_s(&init_inode.k_i);
	struct bkey_s_inode s_typed = bkey_s_to_inode(s_gen);
	assert(s_typed.k == &init_inode.k &&
	       s_typed.v == (struct bch_inode *)s_gen.v);

	/* 验证 const 通路 */
	struct bkey_s_c_inode sc = bkey_i_to_s_c_inode(&init_inode.k_i);
	assert(sc.k == &init_inode.k && sc.v == &init_inode.v);
	printf("  转换函数: 全部 PASS (向下转型 + split + const)\n");

	/* ---- 演示 6: 多态调度 ---- */
	print_header("演示 6: 多态调度（ops 表）");

	unsigned ops_size = sizeof(bch_val_ops) / sizeof(bch_val_ops[0]);
	printf("  ops 表 bch_val_ops[] (size=%u):\n", ops_size);
	for (unsigned i = 0; i < ops_size; i++) {
		if (bch_val_ops[i].pack) {
			normalize_dummy.k.type = i;
			int keep = bch_val_ops[i].normalize
				   ? bch_val_ops[i].normalize(&normalize_dummy)
				   : 1;
			printf("    [%2d] %-14s min_size=%zu normalize=%s\n",
			       i, bkey_type_name[i], bch_val_ops[i].min_size,
			       keep ? "KEEP" : "DROP");
		}
	}

	/* ---- 汇总 ---- */
	print_header("汇总");
	printf("  inode roundtrip:        %s\n", inode_ok ? "PASS" : "FAIL");
	printf("  btree_ptr_v2 roundtrip: %s\n", ptr_ok ? "PASS" : "FAIL");
	printf("  extent roundtrip:       %s\n", ext_ok ? "PASS" : "FAIL");
	printf("  dirent roundtrip:       %s\n", dirent_ok ? "PASS" : "FAIL");
	printf("  key_normalize DROP:     deleted whiteout (内核语义)\n");
	printf("  typed init + convert:   %s\n", init_ok ? "PASS" : "FAIL");

	int all_ok = inode_ok && ptr_ok && ext_ok && dirent_ok && init_ok;
	printf("\n  总体: %s\n", all_ok ? "全部通过" : "存在失败");

	return all_ok ? 0 : 1;
}
