/*
 * persist.c — bcachefs btree 数据持久化学习案例
 *
 * 本程序通过精简但完整的示例，演示 bcachefs btree 数据从内存结构
 * 到磁盘结构的完整转换链路，覆盖所有核心持久化类型：
 *
 *   类型体系（内存 ↔ 磁盘）：
 *
 *   struct bkey        ←→  struct bkey_packed   键：未打包 vs 打包
 *   struct bkey_i                              键 + 内联值
 *   struct bch_val                             抽象值类型
 *   struct bch_csum                            128 位校验和
 *   struct bpos                                键位置（128 位序）
 *   struct bkey_format                         打包格式描述
 *
 *   struct btree       [内存 btree 节点]
 *     ├─ struct bset_tree set[3]               bset 索引
 *     ├─ struct btree_nr_keys nr               键计数
 *     ├─ struct btree_write writes[2]          双缓冲写入跟踪
 *     └─ struct btree_node *data              → 指向磁盘缓冲区
 *
 *   struct btree_node  [磁盘首次写入]           头部 + 内联 bset
 *   struct btree_node_entry [后续追加写入]      日志结构追加
 *     └─ struct bset                           bkey_packed 数组
 *
 *   struct jset        日志条目包装器
 *   struct jset_entry  日志中的 btree 键更新
 *
 *   struct bch_extent_ptr     设备指针
 *   struct bch_btree_ptr_v2   btree 节点指针（父子引用）
 *
 *   struct sort_iter          多 bset 合并迭代器
 *
 * 编译: gcc -Wall -Wextra -O2 -std=gnu11 -o persist persist.c
 * 运行: ./persist
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <time.h>

/*
 * ================================================================
 * 第一部分：所有持久化数据类型定义
 * ================================================================
 */

/* ====== 基础类型 ====== */

/* 128 位校验和 —— 所有磁盘结构的第一个字段
 *
 * 内核定义: __le64 lo; __le64 hi; （明确为小端）
 * 教学限定 LE-only，使用 uint64_t 保持简洁 */
struct bch_csum {
	uint64_t	lo;
	uint64_t	hi;
};

/*
 * bpos —— 键位置，构成一个 128 位可比较大整数
 *
 * 小端序排列: inode(最高位) → offset → snapshot(最低位)
 * 使得 memcmp 可直接用于 bpos 比较
 */
struct bpos {
	uint64_t	inode;
	uint64_t	offset;
	uint32_t	snapshot;
} __attribute__((packed));

/* 抽象值类型 —— 所有值类型的基类 */
struct bch_val {
	uint64_t	v[0];
};

/*
 * bversion —— 版本戳
 *
 * 内核 bcachefs 用 96 位版本号（而非简单的 64 位），
 * 支持 CAS 风格的乐观并发控制。
 */
struct bversion {
	uint32_t	hi;
	uint64_t	lo;
} __attribute__((packed));

/*
 * bkey —— 未打包键（内存中使用的完整格式）
 *
 * 大小: 32 字节头部 + 变长值
 * 前 3 字节与 bkey_packed 共享布局
 */
struct bkey {
	uint8_t		u64s;		/* 键+值总大小(u64) */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	uint8_t		format:7,	/* 0=打包, 1=未打包 */
			needs_whiteout:1;/* 需要持久化 whiteout */
#else
	uint8_t		needs_whiteout:1,
			format:7;
#endif
	uint8_t		type;		/* 值类型 */
	uint8_t		pad;
	struct bversion	bversion;	/* 版本戳（96 位） */
	uint32_t	size;		/* extent 扇区数 */
	struct bpos	p;		/* 键位置 */
} __attribute__((packed));

/* 未打包键的最小大小（不含值） */
#define BKEY_U64s	(sizeof(struct bkey) / sizeof(uint64_t))

/* bkey_i —— 可变键，带内联值（事务提交使用） */
struct bkey_i {
	struct bkey	k;
	struct bch_val v;
};

/* ====== 键值类型枚举 ====== */
enum bkey_type {
	KEY_TYPE_deleted	= 0,	/* 已删除键（whiteout） */
	KEY_TYPE_whiteout	= 1,	/* 显式 whiteout */
	KEY_TYPE_extent		= 2,	/* 数据 extent */
	KEY_TYPE_inode		= 3,	/* inode 元数据 */
	KEY_TYPE_dirent		= 4,	/* 目录项 */
	KEY_TYPE_xattr		= 5,	/* 扩展属性 */
	KEY_TYPE_btree_ptr	= 6,	/* btree 节点指针 */
	KEY_TYPE_btree_ptr_v2	= 7,	/* btree 节点指针 v2 */
};

/*
 * bkey_packed —— 打包键（磁盘上的紧凑格式）
 *
 * 头部仅 3 字节，与 bkey 前 3 字节布局相同。
 * 之后是按 bkey_format 编码的位字段。
 */
struct bkey_packed {
	/* 兼容数组对齐 */
	uint64_t	_data[0];
	uint8_t		u64s;		/* 总大小(u64) */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	uint8_t		format:7,	/* 打包格式编号 */
			needs_whiteout:1;
#else
	uint8_t		needs_whiteout:1,
			format:7;
#endif
	uint8_t		type;		/* 值类型 */
	uint8_t		key_start[0];	/* 变长打包字段 */
	/* 确保 sizeof(bkey_packed) == sizeof(bkey)，
	 * 避免 struct 赋值拷贝时越界 */
	uint8_t		pad[sizeof(struct bkey) - 3];
} __attribute__((packed, aligned(8)));

/* ====== 打包格式 ====== */

/* bkey 的 6 个可打包字段 */
enum bkey_field {
	BKEY_FIELD_INODE,
	BKEY_FIELD_OFFSET,
	BKEY_FIELD_SNAPSHOT,
	BKEY_FIELD_SIZE,
	BKEY_FIELD_VERSION_HI,
	BKEY_FIELD_VERSION_LO,
	BKEY_NR_FIELDS
};

/*
 * bkey_format —— 每个 btree 节点的键打包格式
 *
 * 键压缩原理: packed = (value - field_offset)，用 bits_per_field 位存储
 * 如果 range==0，bits=0，该字段被省略（所有键值相同）
 *
 * 例: inode=10~10, offset=0~24, snapshot=1~1
 *   → bits: [0, 5, 0, 0, 0, 0] = 共 5 位
 *   → key_u64s = 1，即 8 字节可存下一个键
 *   → 而未打包的 bkey 是 28+ 字节
 */
struct bkey_format {
	uint8_t		key_u64s;		/* 打包后每键占 u64 数 */
	uint8_t		nr_fields;		/* 字段数(6) */
	uint8_t		bits_per_field[BKEY_NR_FIELDS];
	uint64_t	field_offset[BKEY_NR_FIELDS];
} __attribute__((packed));

/* ====== Extent / btree 指针类型 ====== */

/*
 * bch_extent_ptr —— 数据在物理设备上的位置
 *
 * 每个数据可能有多份副本（复制），每个副本对应一个指针。
 * 由单个 u64 位字段组成: type(1) | cached(1) | unused(1) | unwritten(1)
 *                         | offset(44) | dev(8) | gen(8)
 *
 * 内核定义见 fs/data/extents_format.h:224
 */
struct bch_extent_ptr {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	uint64_t	type:1,
			cached:1,
			unused:1,
			unwritten:1,
			offset:44,	/* 8 petabytes */
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

/*
 * 内核 extent value 使用 tagged entry 数组（union bch_extent_entry），
 * 第一个 uint64_t 的低位标识 entry 类型：
 *
 *   type=0: bch_extent_ptr         — 设备指针
 *   type=1: bch_extent_crc32       — 32 位校验和
 *   type=2: bch_extent_crc64       — 64 位校验和
 *   type=3: bch_extent_crc128      — 128 位校验和
 *   type=4..12: bch_extent_stripe_ptr — 纠删码条带指针
 *   type=13: bch_extent_reconcile  — 数据同步信息
 *   type=14: bch_extent_flags      — 标志
 *
 * 条目连续排列在 value 中：ptr | crc32 | ptr | ptr | ...
 * 每个条目通过首个字段的 type 位自我描述。
 * 内核定义见 fs/data/extents_format.h:280。
 */

/* Extent entry types */
enum bch_extent_entry_type {
	BCH_EXTENT_ENTRY_ptr		= 0,
	BCH_EXTENT_ENTRY_crc32		= 1,
	BCH_EXTENT_ENTRY_crc64		= 2,
	BCH_EXTENT_ENTRY_crc128		= 3,
};

/*
 * bch_extent_crc32 —— 32 位校验和 + 压缩元数据
 *
 * nonce (12bit): 加密 nonce
 * csum (32bit): 校验和值
 * compressed/uncompressed_size: 原始 extent 边界（用于部分覆盖后读取）
 */
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

/*
 * bch_extent_crc64 —— 64 位校验和
 *
 * 与 crc32 布局相同，但 csum 扩展到 64 位（csum_lo + csum_hi）。
 */
struct bch_extent_crc64 {
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
	uint32_t	csum_hi;
} __attribute__((packed, aligned(4)));

/* Tagged entry union —— 所有 entry 类型共享前 64 位，type 位做区分 */
union bch_extent_entry {
	uint64_t		type;		/* 鉴别器：低 5 位标识类型 */
	struct bch_extent_ptr		ptr;
	struct bch_extent_crc32		crc32;
	struct bch_extent_crc64		crc64;
};

/*
 * bch_extent —— 数据 extent 值类型
 *
 * 内核中为可变长度 tagged entry 数组：ptr | crc | ptr | ...
 * 见 fs/data/extents_format.h:318
 */
struct bch_extent {
	struct bch_val			v;
	union bch_extent_entry		start[];	/* 可变长度 */
} __attribute__((packed, aligned(8)));

/*
 * bch_btree_ptr_v2 —— btree 节点指针（父→子引用）
 *
 * 内核定义见 fs/data/extents_format.h:304
 *
 * 父子节点的引用关系：
 *   - mem_ptr:    运行时内存地址（磁盘上为 0）
 *   - seq:        btree 节点版本号，用于并发控制
 *   - sectors_written: 子节点已写入的扇区数
 *   - flags:      标志位（如 BTREE_PTR_RANGE_UPDATED）
 *   - min_key:    子节点覆盖的最小键范围
 *   - start[]:    设备指针数组（同 bch_extent 的 entry 机制）
 */
struct bch_btree_ptr_v2 {
	struct bch_val			v;
	uint64_t			mem_ptr;
	uint64_t			seq;		/* __le64 */
	uint16_t			sectors_written;/* __le16 */
	uint16_t			flags;		/* __le16 */
	struct bpos			min_key;
	struct bch_extent_ptr		start[];	/* 可变长度指针数组 */
} __attribute__((packed, aligned(8)));

/* ====== bset 类型 ====== */

/* btree_nr_keys —— 节点内键的统计信息 */
struct btree_nr_keys {
	uint16_t	live_u64s;		/* 压缩后存活总大小(u64) */
	uint16_t	bset_u64s[3];		/* 每个 bset 的大小 */
	uint16_t	packed_keys;		/* 打包格式键数 */
	uint16_t	unpacked_keys;		/* 未打包格式键数 */
};

/* btree_write —— 写入跟踪状态（journal pin） */
struct btree_write {
	uint64_t	journal_seq;		/* 对应的日志序列号 */
};

/*
 * bset_tree —— bset 的索引结构
 *
 * 管理 bset 在 btree_node 数据缓冲区中的位置，
 * 以及辅助搜索树（用于二分查找加速）的偏移量。
 */
struct bset_tree {
	uint16_t	size;			/* 辅助树大小 */
	uint16_t	extra;			/* 辅助树类型 */
	uint16_t	data_offset;		/* bset 数据在 buffer 中的偏移(u64) */
	uint16_t	aux_data_offset;	/* 辅助树数据偏移 */
	uint16_t	end_offset;		/* bset 结束偏移 */
};

/* 辅助搜索树类型 */
enum bset_aux_tree_type {
	BSET_NO_AUX_TREE = 0,	/* 无双亲树 */
	BSET_RO_AUX_TREE = 1,	/* Eytzinger 布局二分搜索树（已持久化 bset） */
	BSET_RW_AUX_TREE = 2,	/* 平面查找表（未写入 bset） */
};

/*
 * bset —— 排序键集合（磁盘上的基本键存储单位）
 *
 * 每个 bset 是一次写入的键集合：
 *   - 首次写入: btree_node.keys（内联在节点头部后）
 *   - 后续写入: btree_node_entry.keys（追加）
 *
 * 字段说明:
 *   seq:         序列号，同一节点内单调递增
 *   journal_seq: 日志序列号，用于崩溃恢复
 *                如果 journal_seq 不在日志中，该 bset 被丢弃
 *   flags:       [3:0] 校验和类型, [4] 大端, [5] 独立 whiteout
 *   version:     元数据版本号，用于向前兼容
 *   u64s:        后续 bkey_packed[] 数据的总大小（u64 为单位）
 */
struct bset {
	uint64_t		seq;
	uint64_t		journal_seq;
	uint32_t		flags;
	uint16_t		version;
	uint16_t		u64s;
	struct bkey_packed	start[0];	/* bkey_packed 数组 */
	uint64_t		_data[];
} __attribute__((packed));

/* 安全获取 bset 的 start 指针（避免 packed 成员地址警告）
 * bset 头部固定 24 字节 → start 始终 8 字节对齐，此处仅是给编译器提供证据 */
static inline struct bkey_packed *bset_start(const struct bset *bs)
{
	return (struct bkey_packed *)((uint8_t *)bs + offsetof(struct bset, start));
}

/* ====== 磁盘 btree 节点类型 ====== */

/*
 * btree_node —— 磁盘 btree 节点（首次写入）
 *
 * 每个 btree 节点在磁盘上是一个日志结构：
 *   第一次写入: btree_node{ csum, magic, flags, min/max_key, format, bset{keys...} }
 *   第二次写入: btree_node_entry{ csum, bset{keys...} }  ← 追加
 *   第三次写入: btree_node_entry{ csum, bset{keys...} }  ← 追加
 *
 * magic 是校验和的 key：实际 = sb_uuid ^ BSET_MAGIC
 * flags 字段是加密的（与 bset->flags 不同）
 * bset 内联在 format 之后，占 24 字节（pad[22] + u64s 与 bset 无缝重叠）
 */
struct btree_node {
	struct bch_csum		csum;		/* 整个节点的校验和 */
	uint64_t		magic;		/* 魔数 */
	uint64_t		flags;		/* btree_id, level, seq(加密) */
	struct bpos		min_key;	/* 闭区间 [min_key, max_key] */
	struct bpos		max_key;
	struct bch_extent_ptr	_ptr;		/* extent 指针（不再使用） */
	struct bkey_format	format;		/* 本节点的键打包格式 */
	/* 内联 bset —— 紧跟在 format 字段之后 */
	/* 由 pad[22] + u64s 组成，与 bset 前 24 字节无缝重叠 */
	struct bset		keys;
} __attribute__((packed, aligned(8)));

/* btree_node_entry —— 后续追加写入的头部（比 btree_node 小得多） */
struct btree_node_entry {
	struct bch_csum		csum;	/* 校验和 */
	struct bset		keys;	/* bset（不含 min/max_key, format 等） */
} __attribute__((packed));

/* btree_node 头部固定部分大小（不含 keys bset 数据） */
#define BTREE_NODE_HEADER_SIZE	\
	offsetof(struct btree_node, keys)

/* ====== 缓存状态机 ====== */

/* btree 节点缓存状态（脏/干净转换） */
enum btree_node_cache_state {
	BTREE_NODE_CACHE_NONE,		/* 未缓存 */
	BTREE_NODE_CACHE_FREED,		/* 已释放缓冲区 */
	BTREE_NODE_CACHE_FREEABLE,	/* 可回收 */
	BTREE_NODE_CACHE_CLEAN,		/* 干净（已同步磁盘） */
	BTREE_NODE_CACHE_DIRTY,		/* 脏（需要写入） */
	BTREE_NODE_CACHE_NR,
};

/* 状态名 */
static const char *cache_state_name(enum btree_node_cache_state s)
{
	static const char *names[] = {
		[BTREE_NODE_CACHE_NONE]		= "NONE",
		[BTREE_NODE_CACHE_FREED]	= "FREED",
		[BTREE_NODE_CACHE_FREEABLE]	= "FREEABLE",
		[BTREE_NODE_CACHE_CLEAN]	= "CLEAN",
		[BTREE_NODE_CACHE_DIRTY]	= "DIRTY",
	};
	return s < BTREE_NODE_CACHE_NR ? names[s] : "?";
}

/* ====== 内存 btree 节点 ====== */

/*
 * struct btree —— 内存中的 btree 节点
 *
 * 这是 btree 节点在内存中的完整表示。关键设计：
 *
 * 1. data 指针指向磁盘格式缓冲区（struct btree_node *）
 *    数据在读取后或写入前使用同一缓冲区
 *
 * 2. set[0]/set[1]/set[2] 支持增量更新
 *    set[0]: 已持久化的 bset（全部合并后的唯⼀集合）
 *    set[1]: 内存中的增量键（未写入磁盘）
 *    set[2]: set[1] 超过 ~4KB 时的额外空间
 *
 * 3. 双缓冲写入跟踪 writes[0]/writes[1]
 *    允许前台写入进行时，后台写入仍可发起
 *
 * 4. whiteout_u64s 从 data 尾部反向生长
 *    用于记录删除标记而不影响主键区域
 *
 * 内存布局:
 *   |<-------------------- 256KB -> node_size ------------------->|
 *   |-------------------------------------------------------------|
 *   | btree_node (磁盘格式)                                        | ← data
 *   |   - bset[0] 键数据 (已持久化)                                |
 *   |-------------------------------------------------------------|
 *   | 空闲空间                                                     |
 *   |-------------------------------------------------------------|
 *   | whiteout 区域 (反向生长)                                     | ← whiteout_u64s
 *   |=============================================================|
 *   | set[1]: 内存增量键                                          |
 *   | set[2]: 溢出增量 (可选)                                      |
 */
#define MAX_BSETS	3

struct btree {
	/* btree_bkey_cached_common */
	uint64_t		lock;		/* six_lock */
	uint8_t			level;		/* 0=leaf */
	uint8_t			btree_id;
	bool			cached;

	/* 哈希和标志 */
	uint64_t		hash_val;
	unsigned long		flags;

	/* 节点数据 */
	uint16_t		written;	/* 已写入扇区数 */
	uint8_t			nsets;		/* 当前 set 数 */
	uint8_t			nr_key_bits;
	uint16_t		version_ondisk;
	struct bkey_format	format;
	bool			byte_aligned_fields;

	/* 数据缓冲区指针 */
	struct btree_node	*data;		/* 指向磁盘格式缓冲区 */
	void			*aux_data;	/* 辅助搜索树 */

	/* bset 索引数组 */
	struct bset_tree	set[MAX_BSETS];

	/* 键统计 */
	struct btree_nr_keys	nr;
	uint16_t		sib_u64s[2];
	uint16_t		whiteout_u64s;	/* whiteout 区域大小 */
	uint8_t			byte_order;
	uint8_t			unpack_fn_len;

	/* 写入跟踪 */
	struct btree_write	writes[2];

	/* 节点自己的键（父节点指向本节点的指针） */
	struct bkey_i		key;

	/* 缓存和 LRU 管理 */
	enum btree_node_cache_state cache_state;
};

/* 节点标志位 */
#define BTREE_NODE_dirty		(1 << 0)
#define BTREE_NODE_need_write		(1 << 1)
#define BTREE_NODE_write_in_flight	(1 << 2)
#define BTREE_NODE_read_in_flight	(1 << 3)

/* ====== 日志（Journal）类型 ====== */

/*
 * jset_entry —— 日志中的类型化键块
 *
 * 日志记录了 btree 的键操作。每个 jset_entry 包含：
 *   - btree_id: 操作的 btree
 *   - level:    btree 级别
 *   - type:     入口类型（btree_keys=0, btree_root=1）
 *   - 后续:    bkey_i 数组
 */
struct jset_entry {
	uint16_t	u64s;
	uint8_t		btree_id;
	uint8_t		level;
	uint8_t		type;
	uint8_t		pad[3];
	struct bkey_i	start[0];	/* bkey_i 数组 */
} __attribute__((packed));

/* jset_entry types */
enum jset_entry_type {
	BCH_JSET_ENTRY_btree_keys	= 0,
	BCH_JSET_ENTRY_btree_root	= 1,
	BCH_JSET_ENTRY_write_buffer_keys= 11,
};

/*
 * jset —— 日志条目包装器
 *
 * 每个日志条目由一个 jset 头部加上多个 jset_entry 组成。
 * last_seq 表示该 btree 仍引用的最旧脏日志序列号。
 */
struct jset {
	struct bch_csum	csum;
	uint64_t	magic;
	uint64_t	seq;		/* 单调递增序列号 */
	uint32_t	version;
	uint32_t	flags;
	uint32_t	u64s;		/* 总数据大小(u64) */
	uint64_t	last_seq;
	struct jset_entry start[0];
} __attribute__((packed));

/* ====== sort_iter —— 多 bset 合并迭代器 ====== */

struct sort_iter_set {
	const struct bkey_packed	*k;	/* 当前键指针 */
	const struct bkey_packed	*end;	/* 结束指针 */
};

#define SORT_ITER_MAX	8

struct sort_iter {
	struct sort_iter_set	data[SORT_ITER_MAX];
	int			used;
};

/* ====== 常量 ====== */

/* 魔数: bset 节点标识 */
#define BSET_MAGIC	0x90135c78b99e07f5ULL

/* 页/节点大小 */
#define PAGE_SIZE	4096
#define BTREE_NODE_SIZE	(PAGE_SIZE * 8)	/* 32KB 模拟 */

/* ================================================================
 * 第二部分：位级打包/解包引擎
 * ================================================================
 */

/* 打包状态机 */
struct pack_state {
	uint64_t	*buf;
	int		bit_offset;
};

/* 解包状态机 */
struct unpack_state {
	const uint64_t	*buf;
	int		bit_offset;
};

/* 写 'bits' 位到缓冲区
 *
 * 演示：用键[1] (inode=100, offset=0, snap=1, size=8, vhi=0, vlo=100)
 *      key_u64s=1, field_offset=[0,0,1,0,0,0], bits=[9,6,1,5,0,9]
 *
 * ┌──────┬───────┬──────┬──────────┬──────────────────────────┐
 * │ 字段  │ 值    │ bits │ 写入位置 │ 位运算                     │
 * ├──────┼───────┼──────┼──────────┼──────────────────────────┤
 * │ inode │ 100   │  9   │ bit 0~8 │ buf[0] |= (100 << 0)     │
 * │ offset│ 0     │  6   │ bit 9~14│ buf[0] |= (0 << 9)       │
 * │ snap  │ 0(1-1)│  1   │ bit 15  │ buf[0] |= (0 << 15)      │
 * │ size  │ 8     │  5   │ bit 16~20│ buf[0] |= (8 << 16)     │
 * │ vhi   │ 0     │  0   │ (跳过)  │ —                         │
 * │ vlo   │ 100   │  9   │ bit 21~29│ buf[0] |= (100 << 21)   │
 * └──────┴───────┴──────┴──────────┴──────────────────────────┘
 *
 * 逐字段运算:
 *   buf[0] = 0
 *   buf[0] |= 100             → 0x0000000000000064
 *   buf[0] |= (8 << 16)       → 0x0000000000080064   (0x80000=2^19)
 *   buf[0] |= (100 << 21)     → 0x000000000C880064   (0xC800000=100*2^21)
 *
 * 最终 buf[0] = 0x000000000C880064 = 210239588
 *
 * bit 63~30    29~21      20~16  15  14~9   8~0
 * ┌───────────┬──────────┬──────┬──┬──────┬──────────┐
 * │ 未使用(34)│ vlo=100  │size=8│0 │off=0 │inode=100 │
 * │ 00000000  │ 001100100│01000 │0 │000000│011001000 │
 * └───────────┴──────────┴──────┴──┴──────┴──────────┘
 *                                       ↑未使用的高位实际为 bit 30 起
 *
 * 跨 u64 边界检查：在 pack_bits 中，如果 shift + bits > 64，
 * 高位部分写入 buf[idx+1]。本例中 max(shift+bits)=30 ≤ 64，无跨边界。
 */
static void pack_bits(struct pack_state *s, uint64_t val, int bits)
{
	if (!bits) return;
	int idx = s->bit_offset / 64;
	int shift = s->bit_offset % 64;
	val &= (bits < 64) ? ((1ULL << bits) - 1) : ~0ULL;
	s->buf[idx] |= (val << shift);
	if (shift + bits > 64) {
		s->buf[idx + 1] |= (val >> (64 - shift));
	}
	s->bit_offset += bits;
}

/* 从缓冲区读 'bits' 位 */
static uint64_t unpack_bits(struct unpack_state *s, int bits)
{
	if (!bits) return 0;
	int idx = s->bit_offset / 64;
	int shift = s->bit_offset % 64;
	uint64_t val;
	if (shift + bits <= 64)
		val = s->buf[idx] >> shift;
	else
		val = s->buf[idx] >> shift |
		     s->buf[idx + 1] << (64 - shift);
	s->bit_offset += bits;
	return val & ((bits < 64) ? ((1ULL << bits) - 1) : ~0ULL);
}

/* ================================================================
 * 第三部分：bkey 操作
 * ================================================================
 */

/* 提取 bkey 字段值 */
static uint64_t bkey_field_val(const struct bkey *k, enum bkey_field f)
{
	switch (f) {
	case BKEY_FIELD_INODE:		return k->p.inode;
	case BKEY_FIELD_OFFSET:		return k->p.offset;
	case BKEY_FIELD_SNAPSHOT:	return k->p.snapshot;
	case BKEY_FIELD_SIZE:		return k->size;
	case BKEY_FIELD_VERSION_HI:	return k->bversion.hi;
	case BKEY_FIELD_VERSION_LO:	return k->bversion.lo;
	default:			return 0;
	}
}

/* 设置 bkey 字段值 */
static void bkey_set_field_val(struct bkey *k, enum bkey_field f, uint64_t v)
{
	switch (f) {
	case BKEY_FIELD_INODE:		k->p.inode = v;		break;
	case BKEY_FIELD_OFFSET:		k->p.offset = v;	break;
	case BKEY_FIELD_SNAPSHOT:	k->p.snapshot = v;	break;
	case BKEY_FIELD_SIZE:		k->size = v;		break;
	case BKEY_FIELD_VERSION_HI:	k->bversion.hi = v; break;
	case BKEY_FIELD_VERSION_LO:	k->bversion.lo = v; break;
	case BKEY_NR_FIELDS:		break;
	}
}

/* bkey 排序比较（用于 qsort） */
static int bkey_compare_for_sort(const void *a, const void *b)
{
	const struct bkey *ka = (const struct bkey *)a;
	const struct bkey *kb = (const struct bkey *)b;
	if (ka->p.inode != kb->p.inode)
		return ka->p.inode < kb->p.inode ? -1 : 1;
	if (ka->p.offset != kb->p.offset)
		return ka->p.offset < kb->p.offset ? -1 : 1;
	if (ka->p.snapshot != kb->p.snapshot)
		return ka->p.snapshot < kb->p.snapshot ? -1 : 1;
	return 0;
}

/* bpos 比较（按 inode, offset, snapshot） */
static int bpos_compare(const struct bpos *a, const struct bpos *b)
{
	if (a->inode != b->inode)
		return a->inode < b->inode ? -1 : 1;
	if (a->offset != b->offset)
		return a->offset < b->offset ? -1 : 1;
	if (a->snapshot != b->snapshot)
		return a->snapshot < b->snapshot ? -1 : 1;
	return 0;
}

/* 将 bkey 打包为 bkey_packed（使用 bkey_format） */
static void bkey_pack(struct bkey_packed *out, const struct bkey *in,
		      const struct bkey_format *fmt)
{
	int hdr_u64s = (offsetof(struct bkey_packed, key_start) +
			sizeof(uint64_t) - 1) / sizeof(uint64_t);
	memset(out, 0, (hdr_u64s + fmt->key_u64s) * sizeof(uint64_t));
	out->u64s = (in->u64s - BKEY_U64s) + hdr_u64s + fmt->key_u64s;
	out->format = 0;
	out->needs_whiteout = in->needs_whiteout;
	out->type = in->type;

	struct pack_state s = {
		.buf = (uint64_t *)out->key_start,
		.bit_offset = 0,
	};
	for (int i = 0; i < BKEY_NR_FIELDS; i++) {
		if (!fmt->bits_per_field[i]) continue;
		uint64_t val = bkey_field_val(in, i) - fmt->field_offset[i];
		pack_bits(&s, val, fmt->bits_per_field[i]);
	}
}

/* 将 bkey_packed 解包为 bkey */
static void bkey_unpack(struct bkey *out, const struct bkey_packed *in,
			const struct bkey_format *fmt)
{
	memset(out, 0, sizeof(*out));
	out->u64s = in->u64s;
	out->format = 1;
	out->needs_whiteout = in->needs_whiteout;
	out->type = in->type;

	struct unpack_state s = {
		.buf = (const uint64_t *)in->key_start,
		.bit_offset = 0,
	};
	for (int i = 0; i < BKEY_NR_FIELDS; i++) {
		int bits = fmt->bits_per_field[i];
		uint64_t val;
		if (bits)
			val = unpack_bits(&s, bits) + fmt->field_offset[i];
		else
			val = fmt->field_offset[i];
		bkey_set_field_val(out, i, val);
	}
}

/* bkey 初始化 */
static void bkey_init(struct bkey *k, uint8_t type, uint64_t inode,
		      uint64_t offset, uint32_t snap, uint32_t size)
{
	memset(k, 0, sizeof(*k));
	k->u64s = BKEY_U64s;
	k->format = 1;
	k->needs_whiteout = 0;
	k->type = type;
	k->p.inode = inode;
	k->p.offset = offset;
	k->p.snapshot = snap;
	k->size = size;
}

/* ================================================================
 * 第四部分：bset 操作
 * ================================================================
 */

/* bset 初始化 */
static void bset_init(struct bset *bs, uint64_t seq, uint64_t journal_seq)
{
	memset(bs, 0, sizeof(*bs));
	bs->seq = seq;
	bs->journal_seq = journal_seq;
	bs->version = 1;
}

/* 遍历 bset 中的键 */
#define bset_for_each_key(bs, k)					\
	for (const struct bkey_packed *k = (const void *)(bs)->start;	\
	     (const uint64_t *)k < (const uint64_t *)(bs)->start + (bs)->u64s; \
	     k = (const struct bkey_packed *)((const uint64_t *)k + k->u64s))

/* ================================================================
 * 第五部分：btree_node 写入（单次 + 多次追加）
 * ================================================================
 */

/* 计算 checksum: XOR 所有 u64（简化版本） */
static struct bch_csum compute_csum(const uint64_t *data, size_t words)
{
	struct bch_csum c = { 0, 0 };
	/* csum 占 2 个 u64，从 word 2 开始计算 */
	for (size_t i = 2; i < words; i++) {
		if (i & 1)
			c.hi ^= data[i];
		else
			c.lo ^= data[i];
	}
	return c;
}

// 备注：btree_node 单次写入主流程（7 个步骤）：
// 备注：
// 备注：  ┌─────────────────────────────────────────────────────────────────┐
// 备注：  │  输入：未打包 bkey 数组 + min_key/max_key + seq/journal_seq     │
// 备注：  │  输出：填充好的 btree_node 缓冲区                             │
// 备注：  └─────────────────────────┬───────────────────────────────────────┘
// 备注：                            ▼
// 备注：  ┌──────────────────────────────────────────────────────────────────┐
// 备注：  │ [1] 格式分析：扫描所有键，找每个字段的 [min,max]，算出 bits     │
// 备注：  │    bkey_format = { field_offset, bits_per_field, key_u64s }    │
// 备注：  └─────────────────────────┬────────────────────────────────────────┘
// 备注：                            ▼
// 备注：  ┌──────────────────────────────────────────────────────────────────┐
// 备注：  │ [2] 排序：bkey 按 (inode, offset, snapshot) 升序排列            │
// 备注：  │    bset 要求键有序，支持二分查找                                │
// 备注：  └─────────────────────────┬────────────────────────────────────────┘
// 备注：                            ▼
// 备注：  ┌──────────────────────────────────────────────────────────────────┐
// 备注：  │ [3] 打包：用 bkey_pack() 将每个 bkey 压缩为 bkey_packed         │
// 备注：  │    临时存到 tmp 缓冲区，累积 total_packed（u64 为单位）         │
// 备注：  └─────────────────────────┬────────────────────────────────────────┘
// 备注：                            ▼
// 备注：  ┌──────────────────────────────────────────────────────────────────┐
// 备注：  │ [4] 填头部：memset 清空 → format / magic / flags / min/max_key  │
// 备注：  │    注意：memset 放在 [4] 而不是开头，因为 [1] 只读键不写缓冲区  │
// 备注：  └─────────────────────────┬────────────────────────────────────────┘
// 备注：                            ▼
// 备注：  ┌──────────────────────────────────────────────────────────────────┐
// 备注：  │ [5] 填 bset 头部：seq / journal_seq / version / u64s            │
// 备注：  │    bset 嵌入在 btree_node 末尾（BTREE_NODE_HEADER_SIZE 之后）   │
// 备注：  └─────────────────────────┬────────────────────────────────────────┘
// 备注：                            ▼
// 备注：  ┌──────────────────────────────────────────────────────────────────┐
// 备注：  │ [6] 复制 packed 键：从 tmp 拷贝到 bn->keys.start               │
// 备注：  │    keys.start 紧跟在 bset 的 24B 头之后                         │
// 备注：  └─────────────────────────┬────────────────────────────────────────┘
// 备注：                            ▼
// 备注：  ┌──────────────────────────────────────────────────────────────────┐
// 备注：  │ [7] 校验和：XOR btree_node 头部 + bset 头部 + packed 键数据     │
// 备注：  │    写入 bytes = header_size + bset_header_size + packed_data    │
// 备注：  └──────────────────────────────────────────────────────────────────┘
// 备注：
// 备注：写入后磁盘布局：
// 备注：  ┌──────────────────────┬────────────┬────────────────────────────┐
// 备注：  │ btree_node 头部(160B) │ bset头部(24B)│ bkey_packed[] (total_packed*8B)│
// 备注：  │ csum|magic|flags|    │ seq|js|flg│ 键1 | 键2 | 键3 ...       │
// 备注：  │ min|max|_ptr|format  │ |ver|u64s │                            │
// 备注：  └──────────────────────┴────────────┴────────────────────────────┘
// 备注：  图例：┌──┐ 表示处理步骤，──▶ 表示流程方向
// 备注：
// 备注：@param buf        输出缓冲区（需 >= buf_size）
// 备注：@param buf_size   缓冲区大小
// 备注：@param min_key    节点覆盖的最小键（闭区间）
// 备注：@param max_key    节点覆盖的最大键（闭区间）
// 备注：@param keys       未打包的 bkey 数组
// 备注：@param nr_keys    键数量
// 备注：@param seq        序列号
// 备注：@param journal_seq 日志序列号
// 备注：@returns 实际写入字节数
/*
 * 单次写入：创建 btree_node 并写入所有键
 *
 * 返回总写入字节数
 */
static size_t write_btree_node(void *buf, size_t buf_size,
			       const struct bpos *min_key,
			       const struct bpos *max_key,
			       struct bkey *keys, int nr_keys,
			       uint64_t seq, uint64_t journal_seq)
{
	struct btree_node *bn = (struct btree_node *)buf;

	/* ---- [1] 格式分析：扫描所有键，计算最优打包格式 ---- */
	// 备注：遍历所有键的 6 个字段(inode/offset/snapshot/size/vhi/vlo)，
	// 备注：找出每个字段的最小值和最大值。然后算出每个字段需要的 bit 数：
	// 备注：  field_offset = min_val           （打包时减去此值）
	// 备注：  bits = ceil(log2(range+1))       （range=0 时 bits=0）
	// 备注：  key_u64s = ceil(total_bits / 64) （每键占的 u64 数）
	// 备注：
	// 备注：关键理解：bits_per_field 是"每个键的这个字段占多少 bit"，不是"所有键
	// 备注：合起来占多少 bit"。例如 bits_per_field[0]=9 意思是：
	// 备注：  每个写入的 packed 键，inode 字段都独立占 9 bit
	// 备注：  N 个键 → inode 总共占 N × 9 bit
	// 备注：  key_u64s=1 意思是每个 packed 键总共占 1 个 u64（8 字节）
	// 备注：
	// 备注：例如 inode=[0,300] → range=300 → bits=9
	// 备注：      vhi=[0,0]    → range=0   → bits=0（字段被省略）
	uint64_t min_val[BKEY_NR_FIELDS];
	uint64_t max_val[BKEY_NR_FIELDS];
	for (int i = 0; i < BKEY_NR_FIELDS; i++) {
		min_val[i] = UINT64_MAX;
		max_val[i] = 0;
	}
	for (int i = 0; i < nr_keys; i++) {
		for (int f = 0; f < BKEY_NR_FIELDS; f++) {
			uint64_t v = bkey_field_val(&keys[i], f);
			if (v < min_val[f]) min_val[f] = v;
			if (v > max_val[f]) max_val[f] = v;
		}
	}

	struct bkey_format fmt;
	fmt.nr_fields = BKEY_NR_FIELDS;
	int total_bits = 0;
	for (int f = 0; f < BKEY_NR_FIELDS; f++) {
		fmt.field_offset[f] = min_val[f];
		uint64_t range = max_val[f] - min_val[f];
		if (!range) {
			fmt.bits_per_field[f] = 0;
		} else {
			int bits = 0;
			uint64_t r = range;
			while (r) { bits++; r >>= 1; }
			fmt.bits_per_field[f] = bits;
			total_bits += bits;
		}
	}
	fmt.key_u64s = (total_bits + 63) / 64;

	printf("    格式: key_u64s=%d  bits=[inode=%d offset=%d snap=%d "
	       "size=%d vhi=%d vlo=%d]\n",
	       fmt.key_u64s,
	       fmt.bits_per_field[0], fmt.bits_per_field[1],
	       fmt.bits_per_field[2], fmt.bits_per_field[3],
	       fmt.bits_per_field[4], fmt.bits_per_field[5]);

	/* ---- [2] 键排序（bset 要求键按 bpos 排序） ---- */
	// 备注：bcachefs 的 bset 要求键严格按 (inode, offset, snapshot)
	// 备注：升序排列。这是二分查找的前提——btree 搜索时在 bset 内做
	// 备注：bch2_bkey_cmp_packed() 二分定位。
	// 备注：排序不影响数据语义（键的位置由 bpos 决定，不是数组下标）。
	struct bkey *sorted_keys = malloc(nr_keys * sizeof(struct bkey));
	memcpy(sorted_keys, keys, nr_keys * sizeof(struct bkey));
	qsort(sorted_keys, nr_keys, sizeof(struct bkey), bkey_compare_for_sort);

	/* ---- [3] 打包所有键 ---- */
	// 备注：对每个排序后的 bkey，用上一步算出的 format 调用 bkey_pack()。
	// 备注：bkey_pack 的输出是 bkey_packed：
	// 备注：  3B 头部(u64s|fmt|needs_whiteout|type) + 变长 packed 位字段
	// 备注：每个 packed 键的 u64s 可能不同（因为值部分长度不同）。
	// 备注：tp 指针每次向前跳 tp->u64s 个 u64，即 key_start[0] + tp->u64s，
	// 备注：按 (uint64_t*) 跳确保对齐。
	struct bkey_packed *tmp = malloc(nr_keys * sizeof(struct bkey) * 2);
	struct bkey_packed *tp = tmp;
	int total_packed = 0;

	for (int i = 0; i < nr_keys; i++) {
		bkey_pack(tp, &sorted_keys[i], &fmt);
		total_packed += tp->u64s;
		tp = (struct bkey_packed *)((uint64_t *)tp + tp->u64s);
	}

	free(sorted_keys);

	/* ---- [4] 清空并填充头部 ---- */
	// 备注：memset 放在这里而不是函数开头，因为 [1] 只是扫描键内容
	// 备注：（只读操作，不写 buf），[2] 排序操作的是 sorted_keys 不是 buf。
	// 备注：等到 [4] 才真正开始操作 buf。
	// 备注：
	// 备注：flags 字段：内核中用 bitfield 编码 btree_id / level / seq，
	// 备注：教学简化版只在高位存 seq（btree_id 和 level 默认为 0）。
	memset(buf, 0, buf_size);
	memcpy(&bn->format, &fmt, sizeof(fmt));
	bn->magic = BSET_MAGIC;
	bn->flags = (uint64_t)seq << 32;
	bn->min_key = *min_key;
	bn->max_key = *max_key;

	/* ---- [5] 填充 bset 头部 ---- */
	// 备注：bn->keys 是 btree_node 的最后一个成员，地址位于
	// 备注：BTREE_NODE_HEADER_SIZE 处。bset 的 start[0]（即 packed 数据
	// 备注：存放位置）在 bset 头部的 24 字节之后。
	// 备注：u64s 字段告诉读取者"从 start[0] 开始有多少个 u64 的 packed 键"。
	bset_init(&bn->keys, seq, journal_seq);
	bn->keys.u64s = total_packed;

	/* ---- [6] 复制打包键到 bset 数据区 ---- */
	// 备注：bn->keys.start 即 btree_node 缓冲区中 packed 键的最终位置，
	// 备注：位于 (uint8_t*)bn + BTREE_NODE_HEADER_SIZE + 24（bset 头部）。
	// 备注：之前打包到 tmp 是临时区，这里一次性 memcpy 到位。
	memcpy(bset_start(&bn->keys), tmp, total_packed * sizeof(uint64_t));
	free(tmp);

	/* ---- [7] 计算校验和 ---- */
	// 备注：compute_csum 从 word[2] 开始（跳过 csum 自身两个 u64），
	// 备注：XOR 所有后续 u64。校验和覆盖的范围 = 整个 btree_node 头部 +
	// 备注：bset 头部 + packed 键数据 = total_words 个 u64。
	// 备注：
	// 备注：written 计算 = 头部固定部分 + bset 头部固定部分 + packed 数据。
	// 备注：bset 的 start[0] 不占 sizeof，所以用 offsetof 取到 start 的偏移。
	size_t total_words = BTREE_NODE_HEADER_SIZE / sizeof(uint64_t)
			     + offsetof(struct bset, start) / sizeof(uint64_t)
			     + total_packed;
	bn->csum = compute_csum((const uint64_t *)bn, total_words);

	size_t written = BTREE_NODE_HEADER_SIZE
			 + offsetof(struct bset, start)
			 + total_packed * sizeof(uint64_t);

	printf("    写入 %zu 字节 (%d 打包键, %d u64s)\n",
	       written, nr_keys, total_packed);
	return written;
}

/*
 * 追加写入：使用 btree_node_entry 追加一组键
 *
 * 这是日志结构的关键特性：不修改现有数据，只追加新 bset。
 * 读取时将所有 bset 合并排序。
 */
static size_t append_to_node(void *buf, size_t buf_size,
			      struct bkey *keys, int nr_keys,
			      uint64_t journal_seq,
			      const struct bkey_format *fmt)
{
	/* 定位到空闲空间的起始位置 */
	struct btree_node *bn = (struct btree_node *)buf;
	uint64_t *end = (uint64_t *)buf + buf_size / sizeof(uint64_t);

	/* 计算已使用的空间 */
	struct bset *last_bs = NULL;
	uint64_t *wp = (uint64_t *)buf;

	/* 跳过 btree_node 头部和第一个 bset */
	wp += BTREE_NODE_HEADER_SIZE / sizeof(uint64_t);
	wp += offsetof(struct bset, start) / sizeof(uint64_t);
	wp += bn->keys.u64s;

	/* 遍历已有的 btree_node_entry */
	while (wp < end) {
		struct btree_node_entry *bne = (struct btree_node_entry *)wp;
		if (!bne->keys.u64s) break;
		last_bs = &bne->keys;
		wp += offsetof(struct btree_node_entry, keys.start) / sizeof(uint64_t);
		wp += bne->keys.u64s;
	}

	/* 检查是否有足够空间 */
	size_t remain = (uint8_t *)end - (uint8_t *)wp;
	if (remain < sizeof(struct btree_node_entry) + nr_keys * 64) {
		printf("    [错误] 空间不足\n");
		return 0;
	}

	/* ---- 追加 btree_node_entry ---- */
	struct btree_node_entry *bne = (struct btree_node_entry *)wp;

	/* 打包键到临时区 */
	struct bkey_packed *tmp = malloc(nr_keys * sizeof(struct bkey) * 2);
	struct bkey_packed *tp = tmp;
	int total_packed = 0;

	for (int i = 0; i < nr_keys; i++) {
		bkey_pack(tp, &keys[i], fmt);
		total_packed += tp->u64s;
		tp = (struct bkey_packed *)((uint64_t *)tp + tp->u64s);
	}

	/* 填充 btree_node_entry */
	bset_init(&bne->keys, last_bs ? last_bs->seq + 1 : bn->keys.seq + 1,
		  journal_seq);
	bne->keys.u64s = total_packed;
	memcpy(bset_start(&bne->keys), tmp, total_packed * sizeof(uint64_t));

	/* 校验和 */
	size_t words = offsetof(struct btree_node_entry, keys.start) / sizeof(uint64_t)
		       + total_packed;
	bne->csum = compute_csum((const uint64_t *)bne, words);
	free(tmp);

	size_t written = offsetof(struct btree_node_entry, keys.start)
			 + total_packed * sizeof(uint64_t);
	printf("    追加写入 %zu 字节 (%d 键, seq=%lu journal=%lu)\n",
	       written, nr_keys, bne->keys.seq, bne->keys.journal_seq);
	return written;
}

/*
 * 读取并验证 btree_node（处理多个 bset）
 *
 * 验证流程：
 *   1. 验证魔数
 *   2. 验证 btree_node 的校验和
 *   3. 遍历所有 bset（btree_node.keys + btree_node_entry[].keys）
 *   4. 验证每个 bset 的 checksum
 *   5. 合并所有 bset 为一个排序序列（bch2_key_sort_fix_overlapping）
 *   6. 解包并验证所有键
 */
static int read_and_verify_node(const void *buf, size_t buf_size,
				const struct bkey *orig_keys, int nr_orig)
{
	const struct btree_node *bn = (const struct btree_node *)buf;

	/* ---- 验证魔数 ---- */
	if (bn->magic != BSET_MAGIC) {
		printf("    [错误] 魔数不匹配\n");
		return -1;
	}
	printf("    [OK] 魔数: 0x%lx\n", (unsigned long)bn->magic);

	/* ---- 验证 btree_node 校验和 ---- */
	size_t bs_words = offsetof(struct bset, start) / sizeof(uint64_t) + bn->keys.u64s;
	size_t total_words = BTREE_NODE_HEADER_SIZE / sizeof(uint64_t) + bs_words;
	struct bch_csum expected = compute_csum((const uint64_t *)bn, total_words);

	if (expected.lo != bn->csum.lo || expected.hi != bn->csum.hi) {
		printf("    [错误] btree_node 校验和不匹配\n");
		printf("      stored: lo=%016lx hi=%016lx\n",
		       (unsigned long)bn->csum.lo, (unsigned long)bn->csum.hi);
		printf("      expect: lo=%016lx hi=%016lx\n",
		       (unsigned long)expected.lo, (unsigned long)expected.hi);
		printf("      words=%zu (header_sz=%zu bset_hdr=%zu)\n",
		       total_words,
		       BTREE_NODE_HEADER_SIZE,
		       offsetof(struct bset, start));
		return -1;
	}
	printf("    [OK] btree_node csum: %016lx%016lx\n",
	       (unsigned long)bn->csum.hi, (unsigned long)bn->csum.lo);

	printf("    范围: [%lu,%lu,%u] - [%lu,%lu,%u]\n",
	       (unsigned long)bn->min_key.inode,
	       (unsigned long)bn->min_key.offset,
	       bn->min_key.snapshot,
	       (unsigned long)bn->max_key.inode,
	       (unsigned long)bn->max_key.offset,
	       bn->max_key.snapshot);

	/* ---- 遍历所有 bset，收集到 sort_iter ---- */
	struct sort_iter iter = { .used = 0 };

	/* 第一个 bset（btree_node 内联） */
	iter.data[iter.used].k = bset_start(&bn->keys);
	iter.data[iter.used].end = (struct bkey_packed *)
		((uint64_t *)bset_start(&bn->keys) + bn->keys.u64s);
	iter.used++;
	printf("    bset[0]: seq=%lu journal=%lu u64s=%d\n",
	       (unsigned long)bn->keys.seq,
	       (unsigned long)bn->keys.journal_seq,
	       bn->keys.u64s);

	/* 后续 bset（btree_node_entry） */
	const uint64_t *pos = (const uint64_t *)buf
		+ BTREE_NODE_HEADER_SIZE / sizeof(uint64_t)
		+ offsetof(struct bset, start) / sizeof(uint64_t)
		+ bn->keys.u64s;

	while ((const uint8_t *)pos < (const uint8_t *)buf + buf_size) {
		const struct btree_node_entry *bne =
			(const struct btree_node_entry *)pos;
		if (!bne->keys.u64s || bne->keys.seq == 0) break;

		/* 验证条目校验和 */
		size_t bne_words = offsetof(struct btree_node_entry, keys.start)
				   / sizeof(uint64_t) + bne->keys.u64s;
		struct bch_csum ce = compute_csum((const uint64_t *)bne, bne_words);
		if (ce.lo != bne->csum.lo || ce.hi != bne->csum.hi) {
			printf("    [错误] btree_node_entry csum 失败\n");
			break;
		}

		iter.data[iter.used].k = bset_start(&bne->keys);
		iter.data[iter.used].end = (struct bkey_packed *)
			((uint64_t *)bset_start(&bne->keys) + bne->keys.u64s);
		iter.used++;

		printf("    bset[%d]: seq=%lu journal=%lu u64s=%d\n",
		       iter.used - 1,
		       (unsigned long)bne->keys.seq,
		       (unsigned long)bne->keys.journal_seq,
		       bne->keys.u64s);

		pos += offsetof(struct btree_node_entry, keys.start) / sizeof(uint64_t);
		pos += bne->keys.u64s;
	}

	printf("    共 %d 个 bset\n", iter.used);

	/* ---- 合并排序 + 解包验证 ---- */
	printf("\n    --- 合并解包验证 ---\n");

	/* 用简单的合并排序将多个 bset 合并 */
	struct bkey *merged = malloc(nr_orig * sizeof(struct bkey) * 2);
	int nr_merged = 0;
	const struct bkey_format *fmt = &bn->format;

	/* 为每个 bset 构建排序迭代器状态 */
	struct {
		const struct bkey_packed *cur;
		const struct bkey_packed *end;
	} readers[SORT_ITER_MAX];
	int nreaders = iter.used;

	for (int i = 0; i < nreaders; i++) {
		readers[i].cur = iter.data[i].k;
		readers[i].end = iter.data[i].end;
	}

	/* 多路合并：每次取最小键（模拟 bch2_key_sort_fix_overlapping） */
	while (1) {
		/* 找到最小的有效键 */
		int best = -1;
		struct bkey best_k;

		for (int i = 0; i < nreaders; i++) {
			while (readers[i].cur < readers[i].end &&
			       readers[i].cur->type == KEY_TYPE_deleted)
				readers[i].cur = (const struct bkey_packed *)
					((const uint64_t *)readers[i].cur + readers[i].cur->u64s);
			if (readers[i].cur >= readers[i].end) continue;

			struct bkey tk;
			bkey_unpack(&tk, readers[i].cur, fmt);
			if (best < 0 || bpos_compare(&tk.p, &best_k.p) < 0) {
				best = i;
				best_k = tk;
			}
		}
		if (best < 0) break;

		struct bkey unpacked;
		bkey_unpack(&unpacked, readers[best].cur, fmt);
		merged[nr_merged++] = unpacked;

		readers[best].cur = (const struct bkey_packed *)
			((const uint64_t *)readers[best].cur + readers[best].cur->u64s);
	}

	/* 验证合并结果（先排序 expected keys，过滤掉 whiteout） */
	struct bkey *sorted_orig = malloc(nr_orig * sizeof(struct bkey));
	int nr_sorted = 0;
	for (int i = 0; i < nr_orig; i++)
		if (orig_keys[i].type != KEY_TYPE_deleted)
			sorted_orig[nr_sorted++] = orig_keys[i];
	qsort(sorted_orig, nr_sorted, sizeof(struct bkey), bkey_compare_for_sort);

	int ok = 0, fail = 0;
	int cmp_nr = nr_merged < nr_sorted ? nr_merged : nr_sorted;
	for (int i = 0; i < cmp_nr; i++) {
		const struct bkey *o = &sorted_orig[i];
		const struct bkey *m = &merged[i];
		int match = (o->p.inode == m->p.inode &&
			     o->p.offset == m->p.offset &&
			     o->p.snapshot == m->p.snapshot &&
			     o->size == m->size &&
			     o->type == m->type);
		if (match) ok++; else fail++;
		printf("    键[%d]: %s  inode=%-5lu offset=%-5lu "
		       "snap=%-3u size=%-3u type=%u\n", i,
		       match ? "✔" : "✗",
		       (unsigned long)m->p.inode,
		       (unsigned long)m->p.offset,
		       m->p.snapshot, m->size, m->type);
		if (!match)
			printf("           期望: inode=%lu offset=%lu "
			       "snap=%u size=%u type=%u\n",
			       (unsigned long)o->p.inode,
			       (unsigned long)o->p.offset,
			       o->p.snapshot, o->size, o->type);
	}

	free(merged);
	free(sorted_orig);
	printf("\n    结果: %d✔ / %d✗ / 总计%d (已过滤 %d whiteout)\n",
	       ok, fail, cmp_nr, nr_orig - nr_sorted);
	return fail ? -1 : 0;
}

/* ================================================================
 * 第六部分：缓存状态机演示
 * ================================================================
 */

/* 模拟 btree 节点缓存状态机 */
static struct btree *create_btree_node_in_memory(void)
{
	struct btree *b = calloc(1, sizeof(struct btree));
	b->level = 0;
	b->cache_state = BTREE_NODE_CACHE_NONE;
	b->data = calloc(1, BTREE_NODE_SIZE);

	printf("    in-memory btree 节点创建:\n");
	printf("      地址: %p\n", (void *)b);
	printf("      data: %p\n", (void *)b->data);
	printf("      初始状态: %s\n", cache_state_name(b->cache_state));
	return b;
}

static void transition_cache_state(struct btree *b,
				   enum btree_node_cache_state to)
{
	printf("    状态转换: %s → %s",
	       cache_state_name(b->cache_state),
	       cache_state_name(to));
	b->cache_state = to;

	switch (to) {
	case BTREE_NODE_CACHE_DIRTY:
		b->flags |= BTREE_NODE_dirty;
		printf(" [flags: dirty]\n");
		break;
	case BTREE_NODE_CACHE_CLEAN:
		b->flags &= ~BTREE_NODE_dirty;
		printf(" [flags: clean]\n");
		break;
	default:
		printf("\n");
	}
}

static void destroy_btree_node(struct btree *b)
{
	free(b->data);
	free(b);
}

/* ================================================================
 * 第七部分：日志（Journal）演示
 * ================================================================
 */

/* 创建一个模拟的日志条目 */
static size_t create_journal_entry(void *buf, size_t buf_size_ignored,
				    uint64_t seq,
				    struct bkey *keys, int nr_keys,
				    uint8_t btree_id)
{
	(void)buf_size_ignored;
	struct jset *je = (struct jset *)buf;
	memset(je, 0, sizeof(*je));
	je->magic = BSET_MAGIC;
	je->seq = seq;
	je->version = 1;
	je->u64s = sizeof(struct jset) / sizeof(uint64_t);

	/* 创建 jset_entry */
	struct jset_entry *entry = &je->start[0];
	entry->u64s = offsetof(struct jset_entry, start) / sizeof(uint64_t);
	entry->btree_id = btree_id;
	entry->level = 0;
	entry->type = BCH_JSET_ENTRY_btree_keys;

	struct bkey_i *kp = (struct bkey_i *)((uint8_t *)entry
		+ sizeof(struct jset_entry));
	for (int i = 0; i < nr_keys; i++) {
		kp->k = keys[i];
		entry->u64s += keys[i].u64s;
		je->u64s += keys[i].u64s;
		kp = (struct bkey_i *)((uint64_t *)kp + keys[i].u64s);
	}

	/* 校验和 */
	je->csum = compute_csum((const uint64_t *)je, je->u64s);

	printf("    日志条目 seq=%lu, u64s=%u, 含 %d 个 btree 键更新\n",
	       seq, je->u64s, nr_keys);
	return je->u64s * sizeof(uint64_t);
}

/* ================================================================
 * 第八部分：Whiteout / 删除演示
 * ================================================================
 */

/*
 * 在 bcachefs 中，删除一个键不会将键从 bset 中移除。
 * 而是插入一个 KEY_TYPE_deleted 或 KEY_TYPE_whiteout 的标记。
 *
 * 读取时，whiteout 键覆盖其之前的同名键。
 * 压缩（compaction）时，被覆盖的键和 whiteout 一起被丢弃。
 */
static void demonstrate_whiteout(void)
{
	printf("\n===== Whiteout 删除机制 =====\n");
	printf("  bcachefs 的删除是标记式（soft delete）：\n");
	printf("  1. 插入一个 KEY_TYPE_deleted 键（whiteout）\n");
	printf("  2. whiteout 在合并时覆盖同名旧键\n");
	printf("  3. 压缩时，被覆盖的键和 whiteout 一起丢弃\n");
	printf("  4. 未写入的 whiteout 存在反向生长区域\n\n");

	uint8_t buf[4096];
	memset(buf, 0, sizeof(buf));

	/* 写入 2 个 extent 键 */
	struct bkey extent_keys[2];
	bkey_init(&extent_keys[0], KEY_TYPE_extent, 100, 0, 1, 8);
	bkey_init(&extent_keys[1], KEY_TYPE_extent, 100, 8, 1, 16);

	struct bpos min = { 100, 0, 1 };
	struct bpos max = { 100, 8, 1 };

	write_btree_node(buf, sizeof(buf), &min, &max, extent_keys, 2, 1, 100);
	printf("  [步骤1] 初始写入 2 个 extent 键:\n");
	read_and_verify_node(buf, sizeof(buf), extent_keys, 2);

	/* 追加 whiteout 删除 key[0] */
	printf("\n  [步骤2] 追加 KEY_TYPE_deleted (inode=100 offset=0 snap=1)\n");
	struct bkey del_key;
	bkey_init(&del_key, KEY_TYPE_deleted, 100, 0, 1, 0);
	/* 需要从已写入的节点获取 format */
	struct btree_node *bn = (struct btree_node *)buf;
	append_to_node(buf, sizeof(buf), &del_key, 1, 200, &bn->format);

	printf("  此时节点有 2 个 bset, 合并时 whiteout 会被跳过:\n");
	struct bkey all_keys[3];
	memcpy(all_keys, extent_keys, 2 * sizeof(struct bkey));
	memcpy(&all_keys[2], &del_key, sizeof(struct bkey));
	read_and_verify_node(buf, sizeof(buf), all_keys, 3);

	printf("\n  注意: 真实 bcachefs 的 bch2_key_sort_fix_overlapping()\n");
	printf("  还会丢弃被 whiteout 覆盖的旧键(此处为 inode=100 offset=0)。\n");
	printf("  本示例的简单合并仅跳过 whiteout 本身。\n");
}

/* ================================================================
 * 第九部分：btree 指针类型演示
 * ================================================================
 */

static void demonstrate_btree_ptr(void)
{
	printf("\n===== btree 节点指针 =====\n");

	/*
	 * bch_btree_ptr_v2 末尾有 FAM start[]，需用 buffer overlay。
	 * 分配 sizeof(struct) + sizeof(struct bch_extent_ptr) 给一个指针。
	 */
	uint8_t buf[sizeof(struct bch_btree_ptr_v2) + sizeof(struct bch_extent_ptr)];
	memset(buf, 0, sizeof(buf));

	struct bch_btree_ptr_v2 *bp = (struct bch_btree_ptr_v2 *)buf;
	bp->mem_ptr        = 0;
	bp->seq            = 42;
	bp->sectors_written = 16;
	bp->flags          = 0;
	bp->min_key        = (struct bpos){ 100, 0, 1 };
	bp->start[0]       = (struct bch_extent_ptr){
		.offset = 4096,		/* 设备偏移 4096 扇区 */
		.dev    = 0,		/* 设备 0 */
		.gen    = 1,
	};

	printf("  btree_ptr_v2 layout:\n");
	printf("    mem_ptr          = %lu\n",  (unsigned long)bp->mem_ptr);
	printf("    seq              = %lu\n",  (unsigned long)bp->seq);
	printf("    sectors_written  = %u\n",   bp->sectors_written);
	printf("    flags            = %u\n",   bp->flags);
	printf("    min_key          = {%lu,%lu,%u}\n",
	       (unsigned long)bp->min_key.inode,
	       (unsigned long)bp->min_key.offset,
	       bp->min_key.snapshot);
	printf("    ptr[0]: dev=%u offset=%lu gen=%u\n",
	       bp->start[0].dev,
	       (unsigned long)bp->start[0].offset,
	       bp->start[0].gen);
	printf("  sizeof = %zu (FAM 不计入, +%zu per ptr)\n\n",
	       sizeof(struct bch_btree_ptr_v2),
	       sizeof(struct bch_extent_ptr));
}

/* ================================================================
 * 第十部分：辅助打印函数
 * ================================================================
 */

static void hex_dump(const void *data, size_t size, const char *label)
{
	printf("\n  --- %s (%zu 字节) ---\n", label, size);
	const uint8_t *bytes = (const uint8_t *)data;
	int width = 32;
	for (size_t i = 0; i < size && i < 1024; i += width) {
		printf("  %04zx: ", i);
		for (int j = 0; j < width && i + j < size; j++)
			printf("%02x ", bytes[i + j]);
		printf("\n");
	}
	if (size > 1024)
		printf("  ... (共 %zu 字节, 截断到 1024)\n", size);
}

static void print_key(const struct bkey *k, const char *label)
{
	printf("  %s: inode=%-5lu offset=%-5lu snap=%-3u "
	       "size=%-3u type=%u u64s=%u\n",
	       label,
	       (unsigned long)k->p.inode,
	       (unsigned long)k->p.offset,
	       k->p.snapshot,
	       k->size, k->type, k->u64s);
}

/* ================================================================
 * 第十一部分：主演示流程
 * ================================================================
 */

int main(void)
{
	printf("╔════════════════════════════════════════════════════════════╗\n");
	printf("║   bcachefs btree 数据持久化 — 完整类型体系学习案例          ║\n");
	printf("║   覆盖：bkey | bkey_i | bkey_packed | bset | btree_node    ║\n");
	printf("║   btree_node_entry | jset | jset_entry | btree |           ║\n");
	printf("║   bch_csum | bch_val | bch_extent_ptr | bch_btree_ptr_v2  ║\n");
	printf("╚════════════════════════════════════════════════════════════╝\n");

	/* ============================================================
	 * 演示 1: 类型体系总览
	 * ============================================================ */
	printf("\n══════════════════════════════════════════════════════════\n");
	printf("  演示 1: 所有持久化类型大小一览\n");
	printf("══════════════════════════════════════════════════════════\n");
	printf("  struct bch_csum          = %2zu 字节\n", sizeof(struct bch_csum));
	printf("  struct bpos              = %2zu 字节\n", sizeof(struct bpos));
	printf("  struct bkey              = %2zu 字节\n", sizeof(struct bkey));
	printf("  struct bkey_packed       = %2zu 字节\n", sizeof(struct bkey_packed));
	printf("  struct bkey_i            = %2zu 字节\n", sizeof(struct bkey_i));
	printf("  struct bkey_format        = %2zu 字节\n", sizeof(struct bkey_format));
	printf("  struct bset              = %2zu 字节(头部)\n", sizeof(struct bset));
	printf("  struct btree_node        = %2zu 字节(头部)\n", sizeof(struct btree_node));
	printf("  struct btree_node_entry  = %2zu 字节(头部)\n", sizeof(struct btree_node_entry));
	printf("  struct btree             = %2zu 字节\n", sizeof(struct btree));
	printf("  struct jset              = %2zu 字节(头部)\n", sizeof(struct jset));
	printf("  struct jset_entry        = %2zu 字节(头部)\n", sizeof(struct jset_entry));
	printf("  struct bch_extent_ptr    = %2zu 字节\n", sizeof(struct bch_extent_ptr));
	printf("  struct bch_extent        = %2zu 字节\n", sizeof(struct bch_extent));
	printf("  struct bch_btree_ptr_v2  = %2zu 字节\n", sizeof(struct bch_btree_ptr_v2));

	/* ============================================================
	 * 演示 2: 单次写入 + 读取验证
	 * ============================================================ */
	printf("\n══════════════════════════════════════════════════════════\n");
	printf("  演示 2: btree 单次写入 → 读取验证（核心持久化路径）\n");
	printf("══════════════════════════════════════════════════════════\n\n");

	struct bkey keys[6] = {
		{ .u64s = BKEY_U64s, .format = 1, .type = KEY_TYPE_inode,
		  .p = { 0, 0, 1 }, .bversion = { .lo = 100 } },
		{ .u64s = BKEY_U64s, .format = 1, .type = KEY_TYPE_extent,
		  .p = { 100, 0, 1 }, .size = 8, .bversion = { .lo = 200 } },
		{ .u64s = BKEY_U64s, .format = 1, .type = KEY_TYPE_extent,
		  .p = { 100, 16, 1 }, .size = 16, .bversion = { .lo = 201 } },
		{ .u64s = BKEY_U64s, .format = 1, .type = KEY_TYPE_dirent,
		  .p = { 200, 0, 1 }, .bversion = { .lo = 300 } },
		{ .u64s = BKEY_U64s, .format = 1, .type = KEY_TYPE_inode,
		  .p = { 300, 0, 2 }, .bversion = { .lo = 400 } },
		{ .u64s = BKEY_U64s, .format = 1, .type = KEY_TYPE_extent,
		  .p = { 100, 32, 2 }, .size = 8, .bversion = { .lo = 500 } },
	};
	int nr = (int)(sizeof(keys) / sizeof(keys[0]));

	struct bpos min_k = { 0, 0, 1 };
	struct bpos max_k = { 300, 32, 2 };

	printf("  创建 %d 个内存 bkey（类型: inode/extent/dirent）:\n", nr);
	for (int i = 0; i < nr; i++) {
		char lbl[20];
		snprintf(lbl, sizeof(lbl), "键[%d]", i);
		print_key(&keys[i], lbl);
	}

	printf("\n  开始写入...\n");
	uint8_t buf[BTREE_NODE_SIZE];
	size_t w = write_btree_node(buf, sizeof(buf), &min_k, &max_k,
				    keys, nr, 1, 100);
	printf("  总写入: %zu 字节 (使用 %zu KB 缓冲区)\n\n", w, sizeof(buf) / 1024);

	hex_dump(buf, sizeof(struct btree_node) + 32,
		 "btree_node 头部 + bset 前部");

	printf("\n  读取验证...\n");
	read_and_verify_node(buf, sizeof(buf), keys, nr);

	/* ============================================================
	 * 演示 3: 日志结构追加写入
	 * ============================================================ */
	printf("\n══════════════════════════════════════════════════════════\n");
	printf("  演示 3: 日志结构 — 使用 btree_node_entry 追加写入\n");
	printf("══════════════════════════════════════════════════════════\n");
	printf("\n  关键概念：bcachefs 不原地覆写节点，而是追加新的 bset。\n");
	printf("  这避免了读-改-写（RMW），提高了崩溃安全性。\n\n");

	/* 使用演示 2 中相同的缓冲区，追加写入新键 */
	/* 偏移量需在初始格式的 6 位范围(0-63)内 */
	struct bkey extra_keys[2] = {
		{ .u64s = BKEY_U64s, .format = 1, .type = KEY_TYPE_extent,
		  .p = { 100, 48, 1 }, .size = 8, .bversion = { .lo = 600 } },
		{ .u64s = BKEY_U64s, .format = 1, .type = KEY_TYPE_extent,
		  .p = { 100, 56, 1 }, .size = 16, .bversion = { .lo = 601 } },
	};

	/* 使用之前相同的格式 */
	struct btree_node *bnode_ref = (struct btree_node *)buf;
	printf("  追加 2 个新 extent 键...\n");
	append_to_node(buf, sizeof(buf), extra_keys, 2, 101, &bnode_ref->format);

	printf("\n  此时磁盘上节点有 2 个 bset（首次 + 追加）:\n");
	struct bkey *all_keys = malloc((nr + 2) * sizeof(struct bkey));
	memcpy(all_keys, keys, nr * sizeof(struct bkey));
	memcpy(all_keys + nr, extra_keys, 2 * sizeof(struct bkey));

	read_and_verify_node(buf, sizeof(buf), all_keys, nr + 2);
	free(all_keys);

	/* ============================================================
	 * 演示 4: Whiteout / 删除机制
	 * ============================================================ */
	demonstrate_whiteout();

	/* ============================================================
	 * 演示 5: 缓存状态机
	 * ============================================================ */
	printf("\n══════════════════════════════════════════════════════════\n");
	printf("  演示 5: in-memory btree 节点 + 缓存状态机\n");
	printf("══════════════════════════════════════════════════════════\n\n");

	struct btree *my_node = create_btree_node_in_memory();

	printf("  正常生命周期: \n");
	transition_cache_state(my_node, BTREE_NODE_CACHE_CLEAN);
	/* 模拟插入键 → 变脏 */
	transition_cache_state(my_node, BTREE_NODE_CACHE_DIRTY);
	printf("    - 插入 bkey: inode=100 offset=0 size=8\n");
	my_node->nr.live_u64s = 3;
	my_node->nr.packed_keys = 1;
	/* 模拟写入 → 变干净 */
	transition_cache_state(my_node, BTREE_NODE_CACHE_CLEAN);
	/* 模拟回收 */
	transition_cache_state(my_node, BTREE_NODE_CACHE_FREEABLE);
	transition_cache_state(my_node, BTREE_NODE_CACHE_FREED);
	transition_cache_state(my_node, BTREE_NODE_CACHE_NONE);

	destroy_btree_node(my_node);

	/* ============================================================
	 * 演示 6: 日志集成
	 * ============================================================ */
	printf("\n══════════════════════════════════════════════════════════\n");
	printf("  演示 6: Journal 日志集成 — jset + jset_entry\n");
	printf("══════════════════════════════════════════════════════════\n\n");

	uint8_t jbuf[4096];
	struct bkey jkeys[2] = {
		{ .u64s = BKEY_U64s, .format = 1, .type = KEY_TYPE_extent,
		  .p = { 500, 0, 1 }, .size = 32, .bversion = { .lo = 700 } },
		{ .u64s = BKEY_U64s, .format = 1, .type = KEY_TYPE_extent,
		  .p = { 500, 32, 1 }, .size = 16, .bversion = { .lo = 701 } },
	};
	create_journal_entry(jbuf, sizeof(jbuf), 42, jkeys, 2, 0);

	hex_dump(jbuf, 128, "日志条目布局 (jset + jset_entry + bkey_i)");

	printf("\n  日志 → btree 持久化链路：\n");
	printf("    1. 事务提交 → btree 键更新 → 记录到 jset_entry\n");
	printf("    2. 日志写入（先于 btree 写入）→ 先写日志（WAL）\n");
	printf("    3. btree 节点变脏 → 后台写入\n");
	printf("    4. 崩溃恢复：重放日志中未刷新的 btree 更新\n");

	/* ============================================================
	 * 演示 7: btree 指针类型
	 * ============================================================ */
	demonstrate_btree_ptr();

	/* ============================================================
	 * 演示 8: 压缩效率对比
	 * ============================================================ */
	printf("\n══════════════════════════════════════════════════════════\n");
	printf("  演示 8: 打包压缩效率对比\n");
	printf("══════════════════════════════════════════════════════════\n");

	/* 密集数据 */
	printf("\n  场景 A — 密集（同一 inode, 连续 offset）:\n");
	struct bkey dense[4];
	for (int i = 0; i < 4; i++) {
		bkey_init(&dense[i], KEY_TYPE_extent, 10, i * 8, 1, 8);
	}
	uint8_t dbuf[BTREE_NODE_SIZE];
	write_btree_node(dbuf, sizeof(dbuf),
			 &(struct bpos){10, 0, 1},
			 &(struct bpos){10, 24, 1},
			 dense, 4, 2, 200);

	/* 稀疏数据 */
	printf("\n  场景 B — 稀疏（inode/offset 范围广）:\n");
	struct bkey sparse[4] = {
		{ .u64s = BKEY_U64s, .format = 1, .type = KEY_TYPE_inode,
		  .p = { 1000, 0, 1 }, .bversion = { .lo = 1000 } },
		{ .u64s = BKEY_U64s, .format = 1, .type = KEY_TYPE_inode,
		  .p = { 200000, 50000, 1 }, .bversion = { .lo = 2000 } },
		{ .u64s = BKEY_U64s, .format = 1, .type = KEY_TYPE_inode,
		  .p = { 400000, 100000, 2 }, .bversion = { .lo = 3000 } },
		{ .u64s = BKEY_U64s, .format = 1, .type = KEY_TYPE_inode,
		  .p = { 1000000, 999999, 3 }, .bversion = { .lo = 4000 } },
	};
	uint8_t sbuf[BTREE_NODE_SIZE];
	write_btree_node(sbuf, sizeof(sbuf),
			 &(struct bpos){1000, 0, 1},
			 &(struct bpos){1000000, 999999, 3},
			 sparse, 4, 3, 201);

	/* ============================================================
	 * 演示 9: bkey_i 和 bch_val
	 * ============================================================ */
	printf("\n══════════════════════════════════════════════════════════\n");
	printf("  演示 9: bkey_i — 键 + 内联值\n");
	printf("══════════════════════════════════════════════════════════\n");
	printf("  bkey_i = struct bkey + 内联的 struct bch_val\n");
	printf("  真实 bcachefs 通过 BKEY_TYPE 宏生成类型化包装器:\n");
	printf("    bkey_i_extent { bkey k_i; struct bch_extent v; }\n\n");

	/* 堆分配确保完整的 48 字节（bkey_i 40B + 值 2*u64） */
	struct bkey_i *my_val_key = malloc(sizeof(struct bkey_i)
					   + 2 * sizeof(uint64_t));
	bkey_init(&my_val_key->k, KEY_TYPE_extent, 999, 0, 1, 64);
	my_val_key->k.u64s = BKEY_U64s + 2; /* 加 2 个 u64 的值空间 */
	/* 写入值 */
	uint64_t *val_data = (uint64_t *)&my_val_key->v;
	val_data[0] = 0xDEADBEEF;
	val_data[1] = 0xCAFEBABE;

	print_key(&my_val_key->k, "bkey_i 示例");
	printf("    v[0]=0x%lx v[1]=0x%lx\n",
	       (unsigned long)val_data[0],
	       (unsigned long)val_data[1]);
	free(my_val_key);

	/* ============================================================
	 * 总结
	 * ============================================================ */
	printf("\n════════════════════════════════════════════════════════════\n");
	printf("  总结：bcachefs btree 完整持久化类型体系\n");
	printf("════════════════════════════════════════════════════════════\n\n");

	printf("  存储层:\n");
	printf("    bch_csum       128 位校验和，保护每个磁盘结构的完整性\n");
	printf("    bch_extent_ptr 设备指针，指向物理设备上的数据位置\n");
	printf("    bch_btree_ptr_v2 btree 节点指针，父子节点引用\n\n");

	printf("  键类型体系:\n");
	printf("    bpos           128 位可比较大整数键位置\n");
	printf("    bkey           未打包格式（内存中使用）\n");
	printf("    bkey_packed    打包格式（磁盘上使用，压缩率 ~80%%）\n");
	printf("    bkey_format    每个节点的独立压缩方案\n");
	printf("    bkey_i         可变键（bkey + 内联值）\n");
	printf("    bch_val        抽象值基类\n\n");

	printf("  节点层:\n");
	printf("    bset           排序的 bkey_packed 集合\n");
	printf("    btree_node     首次写入的完整头部\n");
	printf("    btree_node_entry 后续追加写入的轻量头部\n");
	printf("    btree          内存节点（set[], writes[], cache_state）\n");
	printf("    btree_nr_keys  键统计（live_u64s, packed/unpacked）\n\n");

	printf("  日志层:\n");
	printf("    jset           日志条目包装器\n");
	printf("    jset_entry     日志中的 btree 键操作记录\n\n");

	printf("  详细原理说明见同目录 persistence-guide.md\n");
	printf("  真实代码参考:\n");
	printf("    fs/bcachefs_format.h — 所有磁盘格式定义\n");
	printf("    fs/btree/types.h     — 内存 btree 类型\n");
	printf("    fs/btree/bkey.h/.c   — 键打包/解包引擎\n");
	printf("    fs/btree/write.c     — 写入路径实现\n");
	printf("    fs/btree/read.c      — 读取路径实现\n");

	return 0;
}


// ╔════════════════════════════════════════════════════════════╗
// ║   bcachefs btree 数据持久化 — 完整类型体系学习案例          ║
// ║   覆盖：bkey | bkey_packed | bkey_i | bset |              ║
// ║   btree_node | btree_node_entry | jset | jset_entry |     ║
// ║   bch_csum | bch_val | bpos | bversion |                  ║
// ║   bch_extent_ptr | bch_extent_crc32 | bch_extent_crc64 | ║
// ║   union bch_extent_entry | bch_extent | bch_btree_ptr_v2 ║
// ╚════════════════════════════════════════════════════════════╝
//
// ══════════════════════════════════════════════════════════
//   演示 1: 所有持久化类型大小一览
// ══════════════════════════════════════════════════════════
//   struct bch_csum          = 16 字节
//   struct bpos              = 20 字节
//   struct bkey              = 40 字节  (内核: 40B, BKEY_U64s=5)
//   struct bkey_packed       = 40 字节  (padding to sizeof(bkey))
//   struct bkey_i            = 40 字节
//   struct bkey_format        = 56 字节
//   struct bset              = 24 字节(头部)
//   struct btree_node        = 160 字节(头部)
//   struct btree_node_entry  = 40 字节(头部)
//   struct btree             = 232 字节
//   struct jset              = 52 字节(头部)
//   struct jset_entry        =  8 字节(头部)
//   struct bch_extent_ptr    =  8 字节 (64-bit bitfield)
//   struct bch_extent        =  0 字节 (FAM, 不计入 sizeof)
//   struct bch_btree_ptr_v2  = 40 字节  (v=0+mem=8+seq=8+sec_written=2+flags=2+min=20)
//
// ══════════════════════════════════════════════════════════
//   演示 2: btree 单次写入 → 读取验证（核心持久化路径）
// ══════════════════════════════════════════════════════════
//
//   创建 6 个内存 bkey（类型: inode/extent/dirent）:
//   键[0]: inode=0     offset=0     snap=1   size=0   type=3 u64s=5
//   键[1]: inode=100   offset=0     snap=1   size=8   type=2 u64s=5
//   键[2]: inode=100   offset=16    snap=1   size=16  type=2 u64s=5
//   键[3]: inode=200   offset=0     snap=1   size=0   type=4 u64s=5
//   键[4]: inode=300   offset=0     snap=2   size=0   type=3 u64s=5
//   键[5]: inode=100   offset=32    snap=2   size=8   type=2 u64s=5
//
//   开始写入...
//     格式: key_u64s=1  bits=[inode=9 offset=6 snap=1 size=5 vhi=0 vlo=9]
//     写入 256 字节 (6 打包键, 12 u64s)
//   总写入: 256 字节 (使用 32 KB 缓冲区)
//
//
//   --- btree_node 头部 + bset 前部 (192 字节) ---
//   0000: f5 01 91 3f 36 e8 1d 99 00 00 00 00 20 00 0c 00 f5 07 9e b9 78 5c 13 90 00 00 00 00 01 00 00 00
//   0020: 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 01 00 00 00 2c 01 00 00 00 00 00 00 20 00 00 00
//   0040: 00 00 00 00 02 00 00 00 00 00 00 00 00 00 00 00 01 06 09 06 01 05 00 09 00 00 00 00 00 00 00 00
//   0060: 00 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
//   0080: 64 00 00 00 00 00 00 00 01 00 00 00 00 00 00 00 64 00 00 00 00 00 00 00 00 00 00 00 01 00 0c 00
//   00a0: 02 00 03 00 00 00 00 00 00 00 00 00 00 00 00 00 02 00 02 64 00 88 0c 00 00 00 00 00 00 00 00 00
//
//   读取验证...
//     [OK] 魔数: 0x90135c78b99e07f5
//     [OK] btree_node csum: 000c002000000000991de8363f9101f5
//     范围: [0,0,1] - [300,32,2]
//     bset[0]: seq=1 journal=100 u64s=12
//     共 1 个 bset
//
//     --- 合并解包验证 ---
//     键[0]: ✔  inode=0     offset=0     snap=1   size=0   type=3
//     键[1]: ✔  inode=100   offset=0     snap=1   size=8   type=2
//     键[2]: ✔  inode=100   offset=16    snap=1   size=16  type=2
//     键[3]: ✔  inode=100   offset=32    snap=2   size=8   type=2
//     键[4]: ✔  inode=200   offset=0     snap=1   size=0   type=4
//     键[5]: ✔  inode=300   offset=0     snap=2   size=0   type=3
//
//     结果: 6✔ / 0✗ / 总计6 (已过滤 0 whiteout)
//
// ══════════════════════════════════════════════════════════
//   演示 3: 日志结构 — 使用 btree_node_entry 追加写入
// ══════════════════════════════════════════════════════════
//
//   关键概念：bcachefs 不原地覆写节点，而是追加新的 bset。
//   这避免了读-改-写（RMW），提高了崩溃安全性。
//
//   追加 2 个新 extent 键...
//     追加写入 72 字节 (2 键, seq=2 journal=101)
//
//   此时磁盘上节点有 2 个 bset（首次 + 追加）:
//     [OK] 魔数: 0x90135c78b99e07f5
//     [OK] btree_node csum: 000c002000000000991de8363f9101f5
//     范围: [0,0,1] - [300,32,2]
//     bset[0]: seq=1 journal=100 u64s=12
//     bset[1]: seq=2 journal=101 u64s=4
//     共 2 个 bset
//
//     --- 合并解包验证 ---
//     键[0]: ✔  inode=0     offset=0     snap=1   size=0   type=3
//     键[1]: ✔  inode=100   offset=0     snap=1   size=8   type=2
//     键[2]: ✔  inode=100   offset=16    snap=1   size=16  type=2
//     键[3]: ✔  inode=100   offset=32    snap=2   size=8   type=2
//     键[4]: ✔  inode=100   offset=48    snap=1   size=8   type=2
//     键[5]: ✔  inode=100   offset=56    snap=1   size=16  type=2
//     键[6]: ✔  inode=200   offset=0     snap=1   size=0   type=4
//     键[7]: ✔  inode=300   offset=0     snap=2   size=0   type=3
//
//     结果: 8✔ / 0✗ / 总计8 (已过滤 0 whiteout)
//
// ===== Whiteout 删除机制 =====
//   bcachefs 的删除是标记式（soft delete）：
//   1. 插入一个 KEY_TYPE_deleted 键（whiteout）
//   2. whiteout 在合并时覆盖同名旧键
//   3. 压缩时，被覆盖的键和 whiteout 一起丢弃
//   4. 未写入的 whiteout 存在反向生长区域
//
//     格式: key_u64s=1  bits=[inode=0 offset=4 snap=0 size=4 vhi=0 vlo=0]
//     写入 192 字节 (2 打包键, 4 u64s)
//   [步骤1] 初始写入 2 个 extent 键:
//     [OK] 魔数: 0x90135c78b99e07f5
//     [OK] btree_node csum: 00040008000000649013581d359e01fd
//     范围: [100,0,1] - [100,8,1]
//     bset[0]: seq=1 journal=100 u64s=4
//     共 1 个 bset
//
//     --- 合并解包验证 ---
//     键[0]: ✔  inode=100   offset=0     snap=1   size=8   type=2
//     键[1]: ✔  inode=100   offset=8     snap=1   size=16  type=2
//
//     结果: 2✔ / 0✗ / 总计2 (已过滤 0 whiteout)
//
//   [步骤2] 追加 KEY_TYPE_deleted (inode=100 offset=0 snap=1)
//     追加写入 56 字节 (1 键, seq=2 journal=200)
//   此时节点有 2 个 bset, 合并时 whiteout 会被跳过:
//     [OK] 魔数: 0x90135c78b99e07f5
//     [OK] btree_node csum: 00040008000000649013581d359e01fd
//     范围: [100,0,1] - [100,8,1]
//     bset[0]: seq=1 journal=100 u64s=4
//     bset[1]: seq=2 journal=200 u64s=2
//     共 2 个 bset
//
//     --- 合并解包验证 ---
//     键[0]: ✔  inode=100   offset=0     snap=1   size=8   type=2
//     键[1]: ✔  inode=100   offset=8     snap=1   size=16  type=2
//
//     结果: 2✔ / 0✗ / 总计2 (已过滤 1 whiteout)
//
//   注意: 真实 bcachefs 的 bch2_key_sort_fix_overlapping()
//   还会丢弃被 whiteout 覆盖的旧键(此处为 inode=100 offset=0)。
//   本示例的简单合并仅跳过 whiteout 本身。
//
// ══════════════════════════════════════════════════════════
//   演示 5: in-memory btree 节点 + 缓存状态机
// ══════════════════════════════════════════════════════════
//
//     in-memory btree 节点创建:
//       地址: 0x557b49980cd0
//       data: 0x557b49980dc0
//       初始状态: NONE
//   正常生命周期:
//     状态转换: NONE → CLEAN [flags: clean]
//     状态转换: CLEAN → DIRTY [flags: dirty]
//     - 插入 bkey: inode=100 offset=0 size=8
//     状态转换: DIRTY → CLEAN [flags: clean]
//     状态转换: CLEAN → FREEABLE
//     状态转换: FREEABLE → FREED
//     状态转换: FREED → NONE
//
// ══════════════════════════════════════════════════════════
//   演示 6: Journal 日志集成 — jset + jset_entry
// ══════════════════════════════════════════════════════════
//
//     日志条目 seq=42, u64s=16, 含 2 个 btree 键更新
//
//   --- 日志条目布局 (jset + jset_entry + bkey_i) (128 字节) ---
//   0000: 01 06 9e b9 da 5f 11 90 ce 01 00 00 98 03 02 00 f5 07 9e b9 78 5c 13 90 2a 00 00 00 00 00 00 00
//   0020: 01 00 00 00 00 00 00 00 10 00 00 00 00 00 00 00 00 00 00 00 0b 00 00 00 00 00 00 00 05 01 02 00
//   0040: 00 00 00 00 bc 02 00 00 00 00 00 00 20 00 00 00 f4 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00
//   0060: 01 00 00 00 05 01 02 00 00 00 00 00 bd 02 00 00 00 00 00 00 10 00 00 00 f4 01 00 00 00 00 00 00
//
//   日志 → btree 持久化链路：
//     1. 事务提交 → btree 键更新 → 记录到 jset_entry
//     2. 日志写入（先于 btree 写入）→ 先写日志（WAL）
//     3. btree 节点变脏 → 后台写入
//     4. 崩溃恢复：重放日志中未刷新的 btree 更新
//
// ===== btree 节点指针 =====
//   btree_ptr_v2 layout:
//     mem_ptr          = 0
//     seq              = 42
//     sectors_written  = 16
//     flags            = 0
//     min_key          = {100,0,1}
//     ptr[0]: dev=0 offset=4096 gen=1
//   sizeof = 40 (FAM 不计入, +8 per ptr)
//
//
// ══════════════════════════════════════════════════════════
//   演示 8: 打包压缩效率对比
// ══════════════════════════════════════════════════════════
//
//   场景 A — 密集（同一 inode, 连续 offset）:
//     格式: key_u64s=1  bits=[inode=0 offset=5 snap=0 size=0 vhi=0 vlo=0]
//     写入 224 字节 (4 打包键, 8 u64s)
//
//   场景 B — 稀疏（inode/offset 范围广）:
//     格式: key_u64s=1  bits=[inode=20 offset=20 snap=2 size=0 vhi=0 vlo=12]
//     写入 224 字节 (4 打包键, 8 u64s)
//
// ══════════════════════════════════════════════════════════
//   演示 9: bkey_i — 键 + 内联值
// ══════════════════════════════════════════════════════════
//   bkey_i = struct bkey + 内联的 struct bch_val
//   真实 bcachefs 通过 BKEY_TYPE 宏生成类型化包装器:
//     bkey_i_extent { bkey k_i; struct bch_extent v; }
//
//   bkey_i 示例: inode=999   offset=0     snap=1   size=64  type=2 u64s=7
//     v[0]=0xdeadbeef v[1]=0xcafebabe
//
// ════════════════════════════════════════════════════════════
//   总结：bcachefs btree 完整持久化类型体系
// ════════════════════════════════════════════════════════════
//
//   存储层:
//     bch_csum       128 位校验和，保护每个磁盘结构的完整性
//     bch_extent_ptr 设备指针，指向物理设备上的数据位置
//     bch_btree_ptr_v2 btree 节点指针，父子节点引用
//
//   键类型体系:
//     bpos           128 位可比较大整数键位置
//     bkey           未打包格式（内存中使用）
//     bkey_packed    打包格式（磁盘上使用，压缩率 ~80%）
//     bkey_format    每个节点的独立压缩方案
//     bkey_i         可变键（bkey + 内联值）
//     bch_val        抽象值基类
//
//   节点层:
//     bset           排序的 bkey_packed 集合
//     btree_node     首次写入的完整头部
//     btree_node_entry 后续追加写入的轻量头部
//     btree          内存节点（set[], writes[], cache_state）
//     btree_nr_keys  键统计（live_u64s, packed/unpacked）
//
//   日志层:
//     jset           日志条目包装器
//     jset_entry     日志中的 btree 键操作记录
//
//   详细原理说明见同目录 persistence-guide.md
//   真实代码参考:
//     fs/bcachefs_format.h — 所有磁盘格式定义
//     fs/btree/types.h     — 内存 btree 类型
//     fs/btree/bkey.h/.c   — 键打包/解包引擎
//     fs/btree/write.c     — 写入路径实现
//     fs/btree/read.c      — 读取路径实现
