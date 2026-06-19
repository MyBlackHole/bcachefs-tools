/* SPDX-License-Identifier: GPL-2.0 */
// 备注：bcachefs B-tree 类型定义
// 备注：
// 备注：本文件定义了 B-tree 系统的核心数据类型，包括：
// 备注：
// 备注：1. B-tree 节点结构 (struct btree)
// 备注：- 内存中的 B-tree 节点，包含多个 bset（排序键集合）
// 备注：- 支持最多 3 个 bset：set[0] 为已持久化的数据，set[1]/set[2] 为内存中的增量
// 备注：
// 备注：2. B-tree 迭代器 (struct btree_iter, struct btree_path)
// 备注：- btree_iter: 高级迭代接口，供外部使用
// 备注：- btree_path: 低级路径结构，记录从根到叶的访问路径和锁状态
// 备注：
// 备注：3. 事务机制 (struct btree_trans)
// 备注：- 提供 ACID 语义：通过日志和锁机制保证原子性和一致性
// 备注：- 支持乐观并发：事务重启机制处理锁冲突
// 备注：
// 备注：4. B-tree 缓存 (struct bch_fs_btree_cache)
// 备注：- 内存缓存常用节点，避免磁盘 I/O
// 备注：- LRU 淘汰策略，支持 shrinker 回收
// 备注：
// 备注：架构特点：
// 备注：- 使用 SIX 锁（shared/intent/exclusive）实现细粒度并发控制
// 备注：- bset 机制支持增量更新，避免频繁磁盘写入
// 备注：- 事务通过 path 引用计数和锁顺序排序避免死锁
#ifndef _BCACHEFS_BTREE_TYPES_H
#define _BCACHEFS_BTREE_TYPES_H

#include <linux/list.h>
#include <linux/rhashtable.h>

#include "alloc/buckets_types.h"
#include "alloc/replicas_types.h"

#include "btree/bbpos_types.h"
#include "btree/bkey_types.h"
#include "btree/interior_types.h"
#include "btree/key_cache_types.h"
#include "btree/node_scan_types.h"
#include "btree/write_buffer_types.h"

#include "journal/types.h"

#include "util/darray.h"
#include "util/six.h"

struct bio;
struct open_bucket;
struct btree_update;
struct btree_trans;
struct lock_graph;

/* Btree nodes: */

// 备注：MAX_BSETS - B-tree 节点最大 bset 数量
// 备注：
// 备注：B-tree 节点内最多支持 3 个 bset：
// 备注：- set[0]: 已写入磁盘的所有 bset 合并后的数据
// 备注：- set[1]: 内存中未写入磁盘的增量 bkey
// 备注：- set[2]: 当 set[1] 超过 4KB 时扩展的额外 bset (见 bch2_btree_node_prep_for_write)
// 备注：
// 备注：这种设计允许：
// 备注：1. 增量更新：set[1] 收集内存中的修改，避免频繁刷盘
// 备注：2. 合并策略：定期将多个 bset 合并到 set[0]（compaction）
// 备注：3. 崩溃恢复：通过 journal 重放 set[1] 中的未持久化数据
#define MAX_BSETS		3U

// 备注：struct btree_nr_keys - B-tree 节点键计数结构
// 备注：
// 备注：存储节点中有效键的数量信息，用于：
// 备注：- 跟踪 live_u64s：压缩后存活数据的总大小（单位 u64）
// 备注：- 跟踪每个 bset 的键数量 (bset_u64s[])
// 备注：- 支持 packed/unpacked 格式转换
// 备注：
// 备注：字段说明：
// 备注：- live_u64s: 压缩后存活数据的总大小（compaction 后的大小）
// 备注：- bset_u64s[MAX_BSETS]: 每个 bset 的键数据大小
// 备注：- packed_keys: 压缩格式的键数量
// 备注：- unpacked_keys: 解压后的键数量
struct btree_nr_keys {

	/*
	 * Amount of live metadata (i.e. size of node after a compaction) in
	 * units of u64s
	 */
	u16			live_u64s;
	u16			bset_u64s[MAX_BSETS];

	/* live keys only: */
	u16			packed_keys;
	u16			unpacked_keys;
};

struct bset_tree {
	/*
	 * We construct a binary tree in an array as if the array
	 * started at 1, so that things line up on the same cachelines
	 * better: see comments in bset.c at cacheline_to_bkey() for
	 * details
	 */
	// 备注：我们在数组中构造一个二叉树，
	// 备注：就像数组从 1 开始一样，
	// 备注：以便更好地在相同的缓存行上排列：
	// 备注：有关详细信息，
	// 备注：请参阅 bset.c 中的 cacheline_to_bkey() 注释

	/* size of the binary tree and prev array */
	// 备注：二叉树和前一个数组的大小?
	u16			size;

	/* function of size - precalculated for to_inorder() */
	// 备注：大小函数 - 为 to_inorder() 预先计算
	u16			extra;

	// 备注：data_offset/end_offset 指出
	// 备注：本 bset_tree(bset) 在 btree::data 中的开始结束偏移
	u16			data_offset;
	// 备注：辅助数据(二叉树)在 btree::data 的偏移量
	u16			aux_data_offset;
	u16			end_offset;
};

struct btree_write {
	struct journal_entry_pin	journal;
};

struct btree_bkey_cached_common {
	struct six_lock		lock;
	u8			level;
	u8			btree_id;
	bool			cached;
};

/*
 * Membership state of a struct btree in the btree node cache.
 *
 * Stored in b->cache_state and maintained by bch2_btree_node_transition_state().
 * See the DOC block at the top of btree/cache.c for the state machine and
 * the bookkeeping each state implies.
 */
enum btree_node_cache_state {
	BTREE_NODE_CACHE_NONE,		/* off all lists; not in cache (kzalloc default) */
	BTREE_NODE_CACHE_FREED,		/* on bc->freed_{pcpu,nonpcpu}; no data buffer */
	BTREE_NODE_CACHE_FREEABLE,	/* on bc->freeable; has data; not hashed */
	BTREE_NODE_CACHE_CLEAN,		/* on bc->live[pinned].clean; hashed; has data */
	BTREE_NODE_CACHE_DIRTY,		/* on bc->live[pinned].dirty; hashed; has data */
};

// 备注：struct btree - B-tree 节点（内存中）
// 备注：
// 备注：代表一个 B-tree 节点，包含：
// 备注：- 节点元数据：btree_id、level、格式、键数量等
// 备注：- 节点数据：data 指针指向磁盘格式的 btree_node
// 备注：- 多个 bset：支持增量更新和 compaction
// 备注：- 锁状态：通过 six_lock 实现并发控制
// 备注：
// 备注：内存布局：
// 备注：|<----------- 256KB (节点大小) ----------->|
// 备注：|------------------------------------------|
// 备注：| btree_node (磁盘格式)                     | <- data 指针
// 备注：|   - bset[0]: 已持久化的键数据             |
// 备注：|------------------------------------------|
// 备注：| 空闲空间                                  |
// 备注：|------------------------------------------|
// 备注：| whiteout 区域 (反向生长)                  | <- whiteout_u64s
// 备注：|==========================================|
// 备注：| bset[1]: 内存中的增量键数据               |
// 备注：| bset[2]: 额外增量 (可选)                  |
// 备注：
// 备注：关键设计：
// 备注：- 双写缓冲：writes[0] 和 writes[1] 支持原子替换
// 备注：- 异步分裂：write_blocked 列表跟踪阻塞的更新
// 备注：- LRU 缓存：list 字段用于 LRU 链表管理
struct btree {
	struct btree_bkey_cached_common c;

	struct rhash_head	hash;
	u64			hash_val;

	unsigned long		flags;
	// 备注：已写入磁盘计数(单位是扇区)
	u16			written;
	// 备注：当前有多少 set
	// 备注：一个 btree_node(内部有 bset) 多个 btree_node_entry (内部有 bset)
	u8			nsets;
	u8			nr_key_bits;
	u16			version_ondisk;

	struct bkey_format	format;

	/*
	 * Per-field unpack constants, derived from @format at node init.
	 * Extract each field with:
	 *
	 *   field = (load_8_unaligned(bytes + byte_offset) >> (64 - bits))
	 *           + field_offset
	 *
	 * Load position chosen so the field ends at the top of the loaded
	 * value (load_offset + 8 == byte after field's MSB byte); junk from
	 * earlier-in-memory fields lands in the low bits and shifts off.
	 *
	 * byte_offset is signed: for a field near the start of @in, the
	 * load can need to start before @in. The byte(s) before @in are
	 * always valid memory in the callers we care about (bset payload
	 * after the bset header, or other bkeys in the same bset).
	 *
	 * Only handles formats where every field's MSB sits at a byte
	 * boundary (field_msb_bit % 8 == 7). bch2_bkey_format_done()
	 * rounds fields up to byte width when there are spare bits, so
	 * this is the common case. Formats too tight to byte-align take
	 * the slow path via byte_aligned_fields = false.
	 */
	bool				byte_aligned_fields;
	struct bkey_unpack_field {
		s8	byte_offset;
		u8	shift_right;	/* 64 - bits, or 64 if field has no bits in packed */
	} unpack[BKEY_NR_FIELDS];

	/*
	 * <---------- 256KB ----------->
	 * ------------------------------
	 * ||||||||||||****           ***
	 * ------------------------------
	 *            ^               ^
	 *            |               b->whiteout_u64s
	 *            b->written
	 * btree_node - btree_node_entry -  btree_node_entry - ....
	 */
	struct btree_node	*data;
	// 备注：二叉树, 每个 bset 一个
	void			*aux_data;

	/*
	 * Sets of sorted keys - the real btree node - plus a binary search tree
	 *
	 * set[0] is special; set[0]->tree, set[0]->prev and set[0]->data point
	 * to the memory we have allocated for this btree node. Additionally,
	 * set[0]->data points to the entire btree node as it exists on disk.
	 */
	// 备注：排序键的集合, 一个二叉搜索树
	// 备注：set[0]: 已写入磁盘的全部 bsets 合并在内存的一个 set
	// 备注：set[1]: 未写入磁盘的 bkeys 的 bset
	// 备注：set[2]: set[1] 过大( > 4kb, bch2_btree_node_prep_for_write)扩充支持]
	struct bset_tree	set[MAX_BSETS];

	struct btree_nr_keys	nr;
	u16			sib_u64s[2];
	// 备注：未写入的 whiteouts 计数(单位 u64)
	// 备注：这些 whiteout 掉的 bkeys 写入从
	// 备注：btree::data 尾部起反向生长的空间
	u16			whiteout_u64s;
	u8			byte_order;
	u8			unpack_fn_len;

	struct btree_write	writes[2];

	/* Key/pointer for this btree node */
	// 备注：指向该 btree 节点的键/指针
	// 备注：
	// 备注：父节点的 key value 的拷贝
	__BKEY_PADDED(key, BKEY_BTREE_PTR_VAL_U64s_MAX);

	/*
	 * XXX: add a delete sequence number, so when bch2_btree_node_relock()
	 * fails because the lock sequence number has changed - i.e. the
	 * contents were modified - we can still relock the node if it's still
	 * the one we want, without redoing the traversal
	 */

	/*
	 * For asynchronous splits/interior node updates:
	 * When we do a split, we allocate new child nodes and update the parent
	 * node to point to them: we update the parent in memory immediately,
	 * but then we must wait until the children have been written out before
	 * the update to the parent can be written - this is a list of the
	 * btree_updates that are blocking this node from being
	 * written:
	 */
	struct list_head	write_blocked;

	/*
	 * Also for asynchronous splits/interior node updates:
	 * If a btree node isn't reachable yet, we don't want to kick off
	 * another write - because that write also won't yet be reachable and
	 * marking it as completed before it's reachable would be incorrect:
	 */
	unsigned long		will_make_reachable;

	struct open_buckets	ob;

	/* lru list */
	struct list_head	list;

	enum btree_node_cache_state cache_state;
};

enum btree_node_sibling {
	btree_prev_sib,
	btree_next_sib,
};

/* Btree cache: */

#define BCH_BTREE_CACHE_NOT_FREED_REASONS()	\
	x(cache_reserve)			\
	x(lock_intent)				\
	x(lock_write)				\
	x(dirty)				\
	x(read_in_flight)			\
	x(write_in_flight)			\
	x(permanent)				\
	x(noevict)				\
	x(write_blocked)			\
	x(will_make_reachable)			\
	x(access_bit)

enum bch_btree_cache_not_freed_reasons {
#define x(n) BCH_BTREE_CACHE_NOT_FREED_##n,
	BCH_BTREE_CACHE_NOT_FREED_REASONS()
#undef x
	BCH_BTREE_CACHE_NOT_FREED_REASONS_NR,
};

struct btree_cache_list {
	unsigned		idx;
	struct shrinker		*shrink;
	size_t			nr_clean;
	size_t			nr_dirty;
	struct list_head	clean;
	struct list_head	dirty;
};

static inline size_t btree_cache_list_nr(const struct btree_cache_list *l)
{
	return l->nr_clean + l->nr_dirty;
}

// 备注：树的根结构
struct btree_root {
	struct btree		*b;

	/* On disk root - see async splits: */
	__BKEY_PADDED(key, BKEY_BTREE_PTR_VAL_U64s_MAX);
	u8			level;
	u8			alive;
	s16			error;
};

struct bch_fs_btree_cache {
	/*
	 * Hot path: btree_path_lock_root reads root pointer + level per
	 * btree_id. We pack the level into the low 3 bits of the pointer so a
	 * single load yields both atomically (no torn read between b and
	 * b->c.level, no extra cacheline miss into the btree node to read
	 * level). See bch2_btree_root_{pack,unpack_b,unpack_level} in cache.h.
	 *
	 * Splitting this out of struct btree_root also keeps the read-side
	 * working set in a few cache lines instead of the full ~88 lines of
	 * roots_known[].
	 */
	unsigned long		roots_b[BTREE_ID_NR];

	struct btree_root	roots_known[BTREE_ID_NR];
	DARRAY(struct btree_root) roots_extra;
	struct mutex		root_lock;

	// 备注：记录 btree 节点的缓存
	struct rhashtable	table;
	bool			table_init_done;
	/*
	 * We never free a struct btree, except on shutdown - we just put it on
	 * the btree_cache_freed list and reuse it later. This simplifies the
	 * code, and it doesn't cost us much memory as the memory usage is
	 * dominated by buffers that hold the actual btree node data and those
	 * can be freed - and the number of struct btrees allocated is
	 * effectively bounded.
	 *
	 * btree_cache_freeable effectively is a small cache - we use it because
	 * high order page allocations can be rather expensive, and it's quite
	 * common to delete and allocate btree nodes in quick succession. It
	 * should never grow past ~2-3 nodes in practice.
	 */
	struct mutex		lock;
	struct list_head	freeable;
	struct list_head	freed_pcpu;
	struct list_head	freed_nonpcpu;
	struct btree_cache_list	live[2];

	size_t			nr_vmalloc;
	size_t			nr_freeable;
	size_t			nr_reserve;
	size_t			nr_by_btree[BTREE_ID_NR];

	/* Number of nodes with BTREE_NODE_write_in_flight set. */
	atomic_long_t		nr_in_flight;
	atomic_long_t		nr_in_flight_inner;
	struct closure_waitlist	nr_in_flight_wait;
	bool			should_throttle ____cacheline_aligned_in_smp;

	/* shrinker stats */
	size_t			nr_freed;
	size_t			nr_requested;
	u64			not_freed[BCH_BTREE_CACHE_NOT_FREED_REASONS_NR];

	/*
	 * Times the allocator hit the memory-pressure self reclaim path:
	 * journal replay watches for this going nonzero to switch off the
	 * sorted-order fastpath, which holds every journal pin until replay
	 * finishes - pins must be released for reclaim to clean btree nodes.
	 */
	unsigned long		nr_self_reclaim;

	/*
	 * If we need to allocate memory for a new btree node and that
	 * allocation fails, we can cannibalize another node in the btree cache
	 * to satisfy the allocation - lock to guarantee only one thread does
	 * this at a time:
	 */
	struct task_struct	*alloc_lock;
	struct closure_waitlist	alloc_wait;

	struct bbpos		pinned_nodes_start;
	struct bbpos		pinned_nodes_end;
	/* btree id mask: 0 for leaves, 1 for interior */
	u64			pinned_nodes_mask[2];
};

static inline size_t btree_cache_nr_live(const struct bch_fs_btree_cache *bc)
{
	return btree_cache_list_nr(&bc->live[0]) +
		btree_cache_list_nr(&bc->live[1]);
}

static inline size_t btree_cache_nr_dirty(const struct bch_fs_btree_cache *bc)
{
	return bc->live[0].nr_dirty + bc->live[1].nr_dirty;
}

/* Iterator, update, and trigger flags: */

struct btree_node_iter {
	struct btree_node_iter_set {
		// 备注：k == end 代表迭代到最后了
		u16	k, end;
	} data[MAX_BSETS];
};

#define BTREE_ITER_FLAGS()			\
	x(slots)				\
	x(intent)				\
	x(prefetch)				\
	x(is_extents)				\
	x(not_extents)				\
	x(cached)				\
	x(with_key_cache)			\
	x(with_journal)				\
	x(snapshot_field)			\
	x(all_snapshots)			\
	x(filter_snapshots)			\
	x(nofilter_whiteouts)			\
	x(nopreserve)				\
	x(nofill)				\
	x(cached_nofill)			\
	x(key_cache_fill)			\

#define STR_HASH_FLAGS()			\
	x(must_create)				\
	x(must_replace)

#define BTREE_UPDATE_FLAGS()			\
	x(internal_snapshot_node)		\
	x(nojournal)				\
	x(key_cache_reclaim)


/*
 * BTREE_TRIGGER_norun - don't run triggers at all
 *
 * BTREE_TRIGGER_transactional - we're running transactional triggers as part of
 * a transaction commit: triggers may generate new updates
 *
 * BTREE_TRIGGER_atomic - we're running atomic triggers during a transaction
 * commit: we have our journal reservation, we're holding btree node write
 * locks, and we know the transaction is going to commit (returning an error
 * here is a fatal error, causing us to go emergency read-only)
 *
 * BTREE_TRIGGER_gc - we're in gc/fsck: running triggers to recalculate e.g. disk usage
 *
 * BTREE_TRIGGER_insert - @new is entering the btree
 * BTREE_TRIGGER_overwrite - @old is leaving the btree
 */
#define BTREE_TRIGGER_FLAGS()			\
	x(norun)				\
	x(transactional)			\
	x(atomic)				\
	x(gc)					\
	x(insert)				\
	x(overwrite)				\
	x(is_discard)				\
	x(set_needs_reconcile_done)

enum {
#define x(n) BTREE_ITER_FLAG_BIT_##n,
	BTREE_ITER_FLAGS()
	STR_HASH_FLAGS()
	BTREE_UPDATE_FLAGS()
	BTREE_TRIGGER_FLAGS()
#undef x
};

/* iter flags must fit in a u16: */
//BUILD_BUG_ON(BTREE_ITER_FLAG_BIT_key_cache_fill > 15);

enum btree_iter_update_trigger_flags {
#define x(n) BTREE_ITER_##n	= 1U << BTREE_ITER_FLAG_BIT_##n,
	BTREE_ITER_FLAGS()
#undef x
#define x(n) STR_HASH_##n	= 1U << BTREE_ITER_FLAG_BIT_##n,
	STR_HASH_FLAGS()
#undef x
#define x(n) BTREE_UPDATE_##n	= 1U << BTREE_ITER_FLAG_BIT_##n,
	BTREE_UPDATE_FLAGS()
#undef x
#define x(n) BTREE_TRIGGER_##n	= 1U << BTREE_ITER_FLAG_BIT_##n,
	BTREE_TRIGGER_FLAGS()
#undef x
};

struct btree_trigger_op {
	enum btree_id				btree;
	unsigned				level;
	struct bkey_s_c				old;
	struct bkey_s				new;
	unsigned				new_buf_u64s;
	enum btree_iter_update_trigger_flags	flags;
};

/* Btree paths and iterators: */

#if defined(CONFIG_BCACHEFS_LOCK_TIME_STATS) || defined(CONFIG_BCACHEFS_DEBUG)
#define TRACK_PATH_ALLOCATED
#endif

typedef u16 btree_path_idx_t;

struct btree_path {
	btree_path_idx_t	sorted_idx;
	u8			ref;
	u8			intent_ref;

	/* btree_iter_copy starts here: */
	struct bpos		pos;

	enum btree_id		btree_id:7;
	bool			cached:1;
	bool			preserve:1;
	/*
	 * When true, failing to relock this path will cause the transaction to
	 * restart:
	 */
	bool			should_be_locked:1;
	unsigned		level:3,
				locks_want:3;
	u8			nodes_locked;

	struct btree_path_level {
		struct btree	*b;
		struct btree_node_iter iter;
		u32		lock_seq;
#ifdef CONFIG_BCACHEFS_LOCK_TIME_STATS
		u64             lock_taken_time;
#endif
	}			l[BTREE_MAX_DEPTH];
#ifdef TRACK_PATH_ALLOCATED
	unsigned long		ip_allocated;
#endif
};

static inline struct btree_path_level *path_l(struct btree_path *path)
{
	return path->l + path->level;
}

static inline unsigned long btree_path_ip_allocated(struct btree_path *path)
{
#ifdef TRACK_PATH_ALLOCATED
	return path->ip_allocated;
#else
	return _THIS_IP_;
#endif
}

/*
 * btree_iter: the high level btree iterator API, iterates over keys.
 * btree_path: the low level path to a btree node, holds locks.
 *
 * Multiple iterators can share the same btree_path via refcounting.
 *
 * bch2_trans_node_iter_class_init
 * bch2_trans_iter_exit
 */
struct btree_iter {
	struct btree_trans	*trans;
	btree_path_idx_t	path;
	btree_path_idx_t	update_path;
	btree_path_idx_t	key_cache_path;

	enum btree_id		btree_id:8;
	u8			min_depth;

	/* btree_iter_copy starts here: */
	u16			flags;

	/* When we're filtering by snapshot, the snapshot ID we're looking for: */
	// 备注：当我们按快照过滤时，我们要查找的快照 ID:
	unsigned		snapshot;

	// 备注：当前迭代器位置
	struct bpos		pos;
	/*
	 * Current unpacked key - so that bch2_btree_iter_next()/
	 * bch2_btree_iter_next_slot() can correctly advance pos.
	 */
	// 备注：Current unpacked key - so that bch2_btree_iter_next()/
	// 备注：bch2_btree_iter_next_slot() can correctly advance pos.
	// 备注：
	// 备注：内部树的迭代位置
	struct bkey		k;

	/* BTREE_ITER_with_journal: */
	size_t			journal_idx;
#ifdef TRACK_PATH_ALLOCATED
	unsigned long		ip_allocated;
#endif
};

struct get_locks_fail {
	unsigned	l;
	struct btree	*b;
};

/* Key cache: */

#define BKEY_CACHED_ACCESSED		0
#define BKEY_CACHED_DIRTY		1
#define BKEY_CACHED_IMMEDIATE_FLUSH	2

struct bkey_cached {
	struct btree_bkey_cached_common c;

	unsigned long		flags;
	u16			u64s;
	struct bkey_cached_key	key;

	struct rhash_head	hash;

	struct journal_entry_pin journal;
	u64			seq;

	struct bkey_i		*k;
	struct rcu_head		rcu;
};

static inline struct bpos btree_node_pos(struct btree_bkey_cached_common *b)
{
	return !b->cached
		? container_of(b, struct btree, c)->key.k.p
		: container_of(b, struct bkey_cached, c)->key.pos;
}

/* Transaction types: */

// 备注：struct btree_insert_entry - 单个B树更新条目(事务中的待处理更新)
// 备注：
// 备注：每个 btree_insert_entry 代表事务中一个待处理的B树插入/更新/删除操作。
// 备注：这些条目被收集在 trans->updates 数组中，在事务提交时原子性应用。
// 备注：
// 备注：生命周期:
// 备注：1. 创建: bch2_trans_update() 分配并初始化 entry
// 备注：2. 排序: 按 (sort_order, cached, level, pos) 排序以确保正确应用顺序
// 备注：3. 触发器: bch2_trans_commit_run_triggers() 执行 insert/overwite 触发器
// 备注：4. 提交: do_bch2_trans_commit() 将 entry 写入 btree 节点
// 备注：5. 清理: bch2_trans_reset_updates() 释放 path 引用并重置 trans
struct btree_insert_entry {
	// 备注：BTREE_UPDATE_* 标志位
	unsigned		flags;
	// 备注：排序优先级，控制更新应用顺序
	u8			sort_order;
	// 备注：键类型(BKEY_TYPE_*)
	u8			bkey_type;
	// 备注：目标B树ID
	enum btree_id		btree_id:8;
	// 备注：B树层级(0=叶子)
	u8			level:3;
	// 备注：是否更新键缓存
	bool			cached:1;
	// 备注：insert触发器是否已运行
	bool			insert_trigger_run:1;
	// 备注：overwrite触发器是否已运行
	bool			overwrite_trigger_run:1;
	// 备注：键缓存是否已刷新
	bool			key_cache_already_flushed:1;
	// 备注：是否正在刷新键缓存
	bool			key_cache_flushing:1;
	/*
	 * @old_k may be a key from the journal or the key cache;
	 * @old_btree_u64s always refers to the size of the key being
	 * overwritten in the btree:
	 */
	// 备注：@old_k: 被覆盖的原始键(用于触发器比较和日志记录)
	// 备注：@old_btree_u64s: 原始键在 btree 中的大小(u64s单位)
	// 备注：@old_v: 原始键的值指针
	// 备注：
	// 备注：注意: @old_k 可能来自日志或键缓存，不一定反映当前 btree 状态。
	// 备注：提交时会重新验证，如不一致则返回 RESTART 错误。
	// 备注：原始键在 btree 中的大小(u64s单位)
	u8			old_btree_u64s;
	u8			k_buf_u64s;
	// 备注：关联的 btree_path 索引
	btree_path_idx_t	path;
	// 备注：新键值(要插入的数据)
	struct bkey_i		*k;
	/* key being overwritten: */
	// 备注：旧键值(用于触发器和日志)
	struct bkey		old_k;
	// 备注：旧值指针
	const struct bch_val	*old_v;
	// 备注：分配点的指令地址(调试)
	unsigned long		ip_allocated;
};

/* Number of btree paths we preallocate, usually enough */
// 备注：我们预先分配的 B 树路径数量，通常足够
#define BTREE_ITER_INITIAL		64
/*
 * Lmiit for btree_trans_too_many_iters(); this is enough that almost all code
 * paths should run inside this limit, and if they don't it usually indicates a
 * bug (leaking/duplicated btree paths).
 *
 * exception: some fsck paths
 *
 * bugs with excessive path usage seem to have possibly been eliminated now, so
 * we might consider eliminating this (and btree_trans_too_many_iter()) at some
 * point.
 */
#define BTREE_ITER_NORMAL_LIMIT		256
/* never exceed limit */
#define BTREE_ITER_MAX			(1U << 10)

struct btree_trans_commit_hook;
typedef int (btree_trans_commit_hook_fn)(struct btree_trans *, struct btree_trans_commit_hook *);

struct btree_trans_commit_hook {
	btree_trans_commit_hook_fn	*fn;
	struct btree_trans_commit_hook	*next;
};

#define BTREE_TRANS_MEM_MAX	(1U << 16)

#define BTREE_TRANS_MAX_LOCK_HOLD_TIME_NS	NSEC_PER_MSEC

struct btree_trans_paths {
	unsigned long		nr_paths;
	struct btree_path	paths[];
};

struct trans_kmalloc_trace {
	unsigned long		ip;
	size_t			bytes;
};
typedef DARRAY(struct trans_kmalloc_trace) darray_trans_kmalloc_trace;

struct btree_trans_subbuf {
	u16			base;
	u16			u64s;
	u16			size;
};

/*
 * Transaction context for btree operations.
 *
 * Holds iterators/paths, pending updates, locks, and a memory arena for a
 * single logical btree operation.  On lock contention or memory pressure,
 * the transaction restarts: releases all locks, resets state, and retries
 * (via lockrestart_do() or similar retry loops).
 *
 *  - mem/mem_top: bump allocator, invalidated on every restart
 *  - Paths kept sorted in lock order to prevent deadlocks
 *  - SRCU read lock protects btree node memory from being freed;
 *    released periodically to avoid stalling reclaim
 */
struct btree_trans {
	struct bch_fs		*c;

	unsigned long		*paths_allocated;
	struct btree_path	*paths;
	btree_path_idx_t	*sorted;
	struct btree_insert_entry *updates;

	/* bump allocator, invalidated on transaction restart */
	void			*mem;
	unsigned		mem_top;
	unsigned		mem_bytes;
	unsigned		realloc_bytes_required;
#ifdef CONFIG_BCACHEFS_TRANS_KMALLOC_TRACE
	darray_trans_kmalloc_trace trans_kmalloc_trace;
#endif

	btree_path_idx_t	nr_sorted;
	btree_path_idx_t	nr_paths;
	btree_path_idx_t	nr_paths_max;
	btree_path_idx_t	nr_updates;
	s16			shard_cpu;
	u8			fn_idx;
	u8			lock_must_abort;
	bool			lock_may_not_fail:1;
	bool			locked:1;
	bool			migrate_disabled:1;
	bool			write_locked:1;
	bool			srcu_held:1;
	bool			btree_cache_cannibalize_locked:1;
	bool			pf_memalloc_nofs:1;
	bool			used_mempool:1;
	bool			in_traverse_all:1;
	bool			paths_sorted:1;
	bool			memory_allocation_failure:1;
	bool			journal_transaction_names:1;
	bool			journal_replay_not_finished:1;
	bool			notrace_relock_fail:1;
	bool			has_interior_updates:1;
	enum bch_errcode	restarted:16;
	u32			restart_count;
#ifdef CONFIG_BCACHEFS_INJECT_TRANSACTION_RESTARTS
	u32			restart_count_this_trans;
#endif

	u64			last_begin_time;
	unsigned long		last_begin_ip;
	unsigned long		last_restarted_ip;
#ifdef CONFIG_BCACHEFS_DEBUG
	bch_stacktrace		last_restarted_trace;
#endif
	unsigned long		last_unlock_ip;
	unsigned long		srcu_lock_time;
	int			srcu_idx;
	enum btree_id		locking_root_id;

	u64			locking_hash_val;
	struct btree_bkey_cached_common *locking;
	/*
	 * Snapshot of locking->{btree}.hash_val at lock-attempt time, used by
	 * bch2_six_check_for_deadlock() to detect that the node identity
	 * rotated while we were about to sleep on it. 0 for cached entries.
	 */
	struct six_lock_waiter	locking_wait;

	/*
	 * btree node writes issued in this trans's context are queued here
	 * (singly linked via bi_next) instead of being submitted directly —
	 * no block layer work happens while we hold btree node locks.
	 * Submitted when the trans unlocks, and before waiting on btree
	 * node IO (see bch2_btree_node_wait_on_write()).
	 */
	struct bio		*queued_write_bios;

	const char		*fn;

	/* update path: */
	struct btree_trans_subbuf journal_entries;
	struct btree_trans_subbuf accounting;

	struct btree_trans_commit_hook *hooks;
	struct journal_entry_pin *journal_pin;

	struct journal_res	journal_res;
	u64			*journal_seq;
	struct disk_reservation *disk_res;
	struct closure		*flush;

	struct bch_fs_usage_base fs_usage_delta;

	unsigned		journal_u64s;
	u32			extra_journal_u64s;
	u64			extra_disk_res;

	__BKEY_PADDED(btree_path_down, BKEY_BTREE_PTR_VAL_U64s_MAX);

#ifdef CONFIG_DEBUG_LOCK_ALLOC
	struct lockdep_map	dep_map;
#endif
	/* Entries before this are zeroed out on every bch2_trans_get() call */

	struct list_head	list;
	struct closure		ref;
	struct rcu_head		rcu;

	unsigned long		_paths_allocated[BITS_TO_LONGS(BTREE_ITER_INITIAL)];
	struct btree_trans_paths trans_paths;
	struct btree_path	_paths[BTREE_ITER_INITIAL];
	btree_path_idx_t	_sorted[BTREE_ITER_INITIAL + 4];
	struct btree_insert_entry _updates[BTREE_ITER_INITIAL];
};

struct btree_trans_buf {
	struct btree_trans	*trans;
};

struct btree_transaction_stats {
	struct bch2_time_stats	duration;
	struct bch2_time_stats	lock_hold_times;
	struct bch2_time_stats	lock_wait_times;
	struct mutex		lock;
	unsigned		nr_max_paths;
	unsigned		max_mem;
#ifdef CONFIG_BCACHEFS_TRANS_KMALLOC_TRACE
	darray_trans_kmalloc_trace trans_kmalloc_trace;
#endif
	char			*max_paths_text;
};

#define BCH_TRANSACTIONS_NR 128

struct bch_fs_btree_trans {
	struct seqmutex			lock;
	struct list_head		list;
	mempool_t			pool;
	mempool_t			malloc_pool;
	struct btree_trans_buf		__percpu *bufs;

	struct srcu_struct		barrier;
	bool				barrier_initialized;

	struct btree_transaction_stats	stats[BCH_TRANSACTIONS_NR];

	struct mutex			stats_json_lock;
	struct printbuf			stats_json_buf;
};

static inline struct btree_path *btree_iter_path(struct btree_trans *trans, struct btree_iter *iter)
{
	return trans->paths + iter->path;
}

static inline struct btree_path *btree_iter_key_cache_path(struct btree_trans *trans, struct btree_iter *iter)
{
	return iter->key_cache_path
		? trans->paths + iter->key_cache_path
		: NULL;
}

/* Btree node write types and flags: */

#define BCH_BTREE_WRITE_TYPES()						\
	x(initial,		0)					\
	x(init_next_bset,	1)					\
	x(cache_reclaim,	2)					\
	x(journal_reclaim,	3)					\
	x(interior,		4)

enum btree_write_type {
#define x(t, n) BTREE_WRITE_##t,
	BCH_BTREE_WRITE_TYPES()
#undef x
	BTREE_WRITE_TYPE_NR,
};

#define BTREE_WRITE_TYPE_MASK	(roundup_pow_of_two(BTREE_WRITE_TYPE_NR) - 1)
#define BTREE_WRITE_TYPE_BITS	ilog2(roundup_pow_of_two(BTREE_WRITE_TYPE_NR))

#define BTREE_FLAGS()							\
	x(read_in_flight)						\
	x(read_error)							\
	x(dirty)							\
	x(need_write)							\
	x(write_blocked)						\
	x(will_make_reachable)						\
	x(noevict)							\
	x(write_idx)							\
	x(accessed)							\
	x(write_in_flight)						\
	x(write_in_flight_inner)					\
	x(just_written)							\
	x(dying)							\
	x(fake)								\
	x(need_rewrite)							\
	x(need_rewrite_error)						\
	x(need_rewrite_ptr_written_zero)				\
	x(never_write)							\
	x(pinned)							\
	x(permanent)

enum btree_flags {
	/* First bits for btree node write type */
	BTREE_NODE_FLAGS_START = BTREE_WRITE_TYPE_BITS - 1,
#define x(flag)	BTREE_NODE_##flag,
	BTREE_FLAGS()
#undef x
};

#define x(flag)								\
static inline bool btree_node_ ## flag(const struct btree *b)		\
{	return test_bit(BTREE_NODE_ ## flag, &b->flags); }		\
									\
static inline void set_btree_node_ ## flag(struct btree *b)		\
{	set_bit(BTREE_NODE_ ## flag, &b->flags); }			\
									\
static inline void clear_btree_node_ ## flag(struct btree *b)		\
{	clear_bit(BTREE_NODE_ ## flag, &b->flags); }

// 备注：set_btree_node_fake: 假 btree 节点
// 备注：set_btree_node_need_rewrite: 需要重写的
// 备注：*/
BTREE_FLAGS()
#undef x

#define BTREE_NODE_REWRITE_REASON()					\
	x(none)								\
	x(unknown)							\
	x(error)							\
	x(ptr_written_zero)

enum btree_node_rewrite_reason {
#define x(n)	BTREE_NODE_REWRITE_##n,
	BTREE_NODE_REWRITE_REASON()
#undef x
};

/* Filesystem-level btree state: */

/*
 * Sidecar cache: when a btree node is evicted (transitioned out of the in-cache
 * hash table) we stash its live_u64s here, keyed by btree_ptr_hash_val. The
 * merge gate uses this as a cheap "what would be the combined size?" estimate
 * for siblings that aren't currently in cache, before paying for a real read.
 *
 * Fixed-size, no chaining: insert overwrites whatever's in the slot. Lookup
 * verifies the stored hash matches the request; collisions naturally degrade
 * to "no info" and the caller falls back to reading the sibling. Sized at
 * ~capacity/1000/btree_node_size entries, which keeps memory tiny.
 */
struct btree_evicted_size {
	u64					mask;	/* table size - 1 (power of 2) */
	u64					*entries;
};

#define BTREE_EVICTED_SIZE_HASH_BITS	48
#define BTREE_EVICTED_SIZE_HASH_MASK	((1ULL << BTREE_EVICTED_SIZE_HASH_BITS) - 1)

struct bch_fs_btree {
	u16					foreground_merge_threshold;

	mempool_t				bounce_pool;

	struct btree_write_stats {
		atomic64_t	nr;
		atomic64_t	bytes;
	}			write_stats[BTREE_WRITE_TYPE_NR];

	struct bio_set				bio;
	mempool_t				fill_iter;
	// 备注：worke 队列结构
	struct workqueue_struct			*read_complete_wq;
	struct ratelimit_state			read_errors_soft;
	struct ratelimit_state			read_errors_hard;

	struct workqueue_struct			*write_complete_wq;

	struct journal_entry_res		root_journal_res;

	// 备注：btree 节点缓存
	struct bch_fs_btree_cache		cache;
	struct btree_evicted_size		evicted_size;
	struct bch_fs_btree_key_cache		key_cache;
	/*
	 * One per BTREE_IS_write_buffer btree (indexed by BCH_WB_BTREE_*, see
	 * bch_wb_btree_idx()). Each instance has its own intake/flushing
	 * buffers and locks.
	 */
	struct bch_fs_btree_write_buffer	write_buffer[BCH_WB_BTREE_NR];
	/*
	 * Per-btree flush_work runs on write_buffer_wq; once the sorted key
	 * list crosses a threshold the flush parallelizes across CPUs by
	 * queuing sub-shards on write_buffer_shard_wq and closure_sync()ing.
	 * The two must be separate workqueues — the outer flush worker
	 * blocks waiting for the inner shards to complete, so they can't
	 * share a wq. WQ_MEM_RECLAIM on both because the flush sits in the
	 * journal-reclaim path.
	 */
	struct workqueue_struct			*write_buffer_wq;
	struct workqueue_struct			*write_buffer_shard_wq;
	/*
	 * Sync flushers (btree_write_buffer_flush_seq) wake the per-btree
	 * flush_works and then wait here for the worker to drain pins past
	 * their target seq. Waked from the flush worker after drain and from
	 * journal_keys_to_write_buffer_end after pin drops.
	 */
	struct closure_waitlist			write_buffer_flush_wait;
	struct bch_fs_btree_trans		trans;
	struct bch_fs_btree_reserve_cache	reserve_cache;
	struct bch_fs_btree_interior_updates	interior_updates;
	struct bch_fs_btree_node_rewrites	node_rewrites;
	struct find_btree_nodes			node_scan;
};

/* Btree node/bset accessors: */

static inline enum btree_node_rewrite_reason btree_node_rewrite_reason(struct btree *b)
{
	if (btree_node_need_rewrite_ptr_written_zero(b))
		return BTREE_NODE_REWRITE_ptr_written_zero;
	if (btree_node_need_rewrite_error(b))
		return BTREE_NODE_REWRITE_error;
	if (btree_node_need_rewrite(b))
		return BTREE_NODE_REWRITE_unknown;
	return BTREE_NODE_REWRITE_none;
}

static inline struct btree_write *btree_current_write(struct btree *b)
{
	return b->writes + btree_node_write_idx(b);
}

static inline struct btree_write *btree_prev_write(struct btree *b)
{
	return b->writes + (btree_node_write_idx(b) ^ 1);
}

// 备注：獲取最後一個 bset_tree
static inline struct bset_tree *bset_tree_last(struct btree *b)
{
	EBUG_ON(!b->nsets);
	return b->set + b->nsets - 1;
}

// 备注：通過偏移量獲取地址
static inline void *
__btree_node_offset_to_ptr(const struct btree *b, u16 offset)
{
	return (void *) ((u64 *) b->data + offset);
}

// 备注：通過地址獲取偏移量
static inline u16
__btree_node_ptr_to_offset(const struct btree *b, const void *p)
{
	u16 ret = (u64 *) p - (u64 *) b->data;

	// 备注：確認偏移量是否正確
	EBUG_ON(__btree_node_offset_to_ptr(b, ret) != p);
	return ret;
}

static inline struct bset *bset(const struct btree *b,
				const struct bset_tree *t)
{
	return __btree_node_offset_to_ptr(b, t->data_offset);
}

// 备注：設置 bset 結束偏移量
static inline void set_btree_bset_end(struct btree *b, struct bset_tree *t)
{
	t->end_offset =
		__btree_node_ptr_to_offset(b, vstruct_last(bset(b, t)));
}

// 备注：設置 bset 開始偏移量結束偏移量
static inline void set_btree_bset(struct btree *b, struct bset_tree *t,
				  const struct bset *i)
{
	// 备注：設置 bset 開始偏移量
	t->data_offset = __btree_node_ptr_to_offset(b, i);
	// 备注：設置 bset 結束偏移量
	set_btree_bset_end(b, t);
}

// 备注：首個 bset
static inline struct bset *btree_bset_first(struct btree *b)
{
	return bset(b, b->set);
}

static inline struct bset *btree_bset_last(struct btree *b)
{
	return bset(b, bset_tree_last(b));
}

static inline u16
__btree_node_key_to_offset(const struct btree *b, const struct bkey_packed *k)
{
	return __btree_node_ptr_to_offset(b, k);
}

static inline struct bkey_packed *
__btree_node_offset_to_key(const struct btree *b, u16 k)
{
	return __btree_node_offset_to_ptr(b, k);
}

static inline unsigned btree_bkey_first_offset(const struct bset_tree *t)
{
	return t->data_offset + offsetof(struct bset, _data) / sizeof(u64);
}

#define btree_bkey_first(_b, _t)					\
({									\
	EBUG_ON(bset(_b, _t)->start !=					\
		__btree_node_offset_to_key(_b, btree_bkey_first_offset(_t)));\
									\
	bset(_b, _t)->start;						\
})

#define btree_bkey_last(_b, _t)						\
({									\
	EBUG_ON(__btree_node_offset_to_key(_b, (_t)->end_offset) !=	\
		vstruct_last(bset(_b, _t)));				\
									\
	__btree_node_offset_to_key(_b, (_t)->end_offset);		\
})

static inline unsigned bset_u64s(struct bset_tree *t)
{
	return t->end_offset - t->data_offset -
		sizeof(struct bset) / sizeof(u64);
}

static inline unsigned bset_dead_u64s(struct btree *b, struct bset_tree *t)
{
	return bset_u64s(t) - b->nr.bset_u64s[t - b->set];
}

static inline unsigned bset_byte_offset(struct btree *b, void *i)
{
	return i - (void *) b->data;
}

/* Btree ID properties: */

enum btree_node_type {
	BKEY_TYPE_btree,
#define x(kwd, val, ...) BKEY_TYPE_##kwd = val + 1,
	BCH_BTREE_IDS()
#undef x
	BKEY_TYPE_NR
};

/* Type of a key in btree @id at level @level: */
static inline enum btree_node_type __btree_node_type(unsigned level, enum btree_id id)
{
	return level ? BKEY_TYPE_btree : (unsigned) id + 1;
}

/* Type of keys @b contains: */
static inline enum btree_node_type btree_node_type(struct btree *b)
{
	return __btree_node_type(b->c.level, b->c.btree_id);
}

const char *bch2_btree_node_type_str(enum btree_node_type);

#define BTREE_NODE_TYPE_HAS_TRANS_TRIGGERS		\
	(BIT_ULL(BKEY_TYPE_extents)|			\
	 BIT_ULL(BKEY_TYPE_alloc)|			\
	 BIT_ULL(BKEY_TYPE_inodes)|			\
	 BIT_ULL(BKEY_TYPE_stripes)|			\
	 BIT_ULL(BKEY_TYPE_reflink)|			\
	 BIT_ULL(BKEY_TYPE_subvolumes)|			\
	 BIT_ULL(BKEY_TYPE_btree))

#define BTREE_NODE_TYPE_HAS_ATOMIC_TRIGGERS		\
	(BIT_ULL(BKEY_TYPE_alloc)|			\
	 BIT_ULL(BKEY_TYPE_inodes)|			\
	 BIT_ULL(BKEY_TYPE_stripes)|			\
	 BIT_ULL(BKEY_TYPE_snapshots))

#define BTREE_NODE_TYPE_HAS_TRIGGERS			\
	(BTREE_NODE_TYPE_HAS_TRANS_TRIGGERS|		\
	 BTREE_NODE_TYPE_HAS_ATOMIC_TRIGGERS)

static inline bool btree_node_type_has_trans_triggers(enum btree_node_type type)
{
	return BIT_ULL(type) & BTREE_NODE_TYPE_HAS_TRANS_TRIGGERS;
}

static inline bool btree_node_type_has_atomic_triggers(enum btree_node_type type)
{
	return BIT_ULL(type) & BTREE_NODE_TYPE_HAS_ATOMIC_TRIGGERS;
}

static inline bool btree_node_type_has_triggers(enum btree_node_type type)
{
	return BIT_ULL(type) & BTREE_NODE_TYPE_HAS_TRIGGERS;
}

/* A mask of btree id bits that have triggers for their leaves */
__maybe_unused
static const u64 btree_leaf_has_triggers_mask = BTREE_NODE_TYPE_HAS_TRIGGERS >> 1;

static const u64 btree_is_extents_mask = 0
#define x(name, nr, flags, ...)	|((!!((flags) & BTREE_IS_extents)) << nr)
BCH_BTREE_IDS()
#undef x
;

static inline bool btree_id_is_extents(enum btree_id btree)
{
	return BIT_ULL(btree) & btree_is_extents_mask;
}

static inline bool btree_node_type_is_extents(enum btree_node_type type)
{
	return type != BKEY_TYPE_btree && btree_id_is_extents(type - 1);
}

static const u64 btree_has_snapshots_mask = 0
#define x(name, nr, flags, ...)	|((!!((flags) & BTREE_IS_snapshots)) << nr)
BCH_BTREE_IDS()
#undef x
;

static inline bool btree_type_has_snapshots(enum btree_id btree)
{
	return BIT_ULL(btree) & btree_has_snapshots_mask;
}

static inline bool btree_id_is_extents_snapshots(enum btree_id btree)
{
	return BIT_ULL(btree) & btree_has_snapshots_mask & btree_is_extents_mask;
}

static inline bool btree_type_has_snapshot_field(enum btree_id btree)
{
	const u64 mask = 0
#define x(name, nr, flags, ...)	|((!!((flags) & (BTREE_IS_snapshot_field|BTREE_IS_snapshots))) << nr)
	BCH_BTREE_IDS()
#undef x
	;

	return BIT_ULL(btree) & mask;
}

static const u64 btree_has_data_ptrs_mask = 0
#define x(name, nr, flags, ...)	|((!!((flags) & BTREE_IS_data)) << nr)
	BCH_BTREE_IDS()
#undef x
	;

static inline bool btree_type_has_data_ptrs(enum btree_id btree)
{
	return BIT_ULL(btree) & btree_has_data_ptrs_mask;
}

static inline bool btree_type_uses_write_buffer(enum btree_id btree)
{
	const u64 mask = 0
#define x(name, nr, flags, ...)	|((!!((flags) & BTREE_IS_write_buffer)) << nr)
	BCH_BTREE_IDS()
#undef x
	;

	return BIT_ULL(btree) & mask;
}

static inline u8 btree_trigger_order(enum btree_id btree)
{
	switch (btree) {
	case BTREE_ID_alloc:
		return U8_MAX;
	case BTREE_ID_stripes:
		return U8_MAX - 1;
	default:
		return btree;
	}
}

#endif /* _BCACHEFS_BTREE_TYPES_H */
