/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCACHEFS_ALLOC_TYPES_H
#define _BCACHEFS_ALLOC_TYPES_H

#include <linux/mutex.h>
#include <linux/spinlock.h>

#include "init/dev_types.h"

#include "alloc/buckets_types.h"
#include "util/clock_types.h"
#include "util/fifo.h"

#define BCH_WATERMARKS()		\
	x(stripe)			\
	x(normal)			\
	x(copygc)			\
	x(btree)			\
	x(btree_copygc)			\
	x(reclaim)			\
	x(interior_updates)

enum bch_watermark {
#define x(name)	BCH_WATERMARK_##name,
	BCH_WATERMARKS()
#undef x
	BCH_WATERMARK_NR,
};

#define BCH_WATERMARK_BITS	3
#define BCH_WATERMARK_MASK	~(~0U << BCH_WATERMARK_BITS)

#define OPEN_BUCKETS_COUNT	4096

// 备注：写点（write point）管理机制。
// 备注：
// 备注：write_point 是分配器的核心概念，标识"正在写入的 open bucket"集合。
// 备注：不同来源的 I/O 被隔离到不同的写点，避免相互干扰。
// 备注：
// 备注：WRITE_POINT_MAX=32 个写点池，每个写点包含若干 open bucket。
// 备注：
// 备注：写点分配规则（writepoint_find() in foreground.c）:
// 备注：  每次 bch2_alloc_sectors_req() 传入一个 write_point_specifier，
// 备注：  这个值通常来自当前 PID（写线程的 task pid）。
// 备注：  writepoint_find() 用 write_point_v 做 hash 查找已有写点。
// 备注：  如果没找到: 从池中找最近最久未使用(LRU)的写点，reuse 之。
// 备注：  如果池满: try_increase_writepoints() 扩容（最多 32 个）。
// 备注：
// 备注：隔离效果: 不同 PID 的写线程各自用不同的写点，即使用不同 bucket。
// 备注：这减少了并发写入时的锁竞争和 metadata 碎片。
// 备注：
// 备注：4 级分配优先级（bch2_alloc_sectors_req() 中）:
// 备注：  1) bucket_alloc_set_writepoint() — 当前写点已有 bucket 续写
// 备注：  2) bucket_alloc_set_partial()    — 其他写点释放的部分 bucket
// 备注：  3) bucket_alloc_from_stripe()    — EC 条带分配
// 备注：  4) bch2_bucket_alloc_set_trans() — 从设备分配全新 bucket
#define WRITE_POINT_HASH_NR	32
#define WRITE_POINT_MAX		32

/*
 * 0 is never a valid open_bucket_idx_t:
 */
typedef u16			open_bucket_idx_t;

struct open_bucket {
	// 备注：操作锁
	spinlock_t		lock;
	atomic_t		pin;
	// 备注：指向下一个空闲的 open_bucket
	open_bucket_idx_t	freelist;
	// 备注：指向 open_buckets_hash 数组的下标
	open_bucket_idx_t	hash;

	/*
	 * When an open bucket has an ec_stripe attached, this is the index of
	 * the block in the stripe this open_bucket corresponds to:
	 */
	u8			ec_idx;
	enum bch_data_type	data_type:5;
	bool			valid:1;
	bool			on_partial_list:1;
	bool			do_discards_fast:1;

	u8			dev;
	u8			gen;
	u32			sectors_free;
	// 备注：bucket number
	// 备注：桶号 */
	u64			bucket;
	struct ec_stripe_new	*ec;
};

struct open_buckets {
	open_bucket_idx_t	nr;
	open_bucket_idx_t	v[BCH_BKEY_PTRS_MAX];
};

/*
 * Per-(write_point, target) WFQ state: next_alloc[i] is the virtual time at
 * which device i should next be served. The smallest hand wins; the per-pick
 * increment is 1/free_space[i], so devs with more free space win more often -
 * the proportional bias is in the increment size.
 *
 * cached_devs records which dev mask the next_alloc values are valid for.
 * On mask change (target switch, rw_devs change, write_point recycled to a
 * different stream) we bump newly-included devs to min(next_alloc[i] of devs
 * that were already in scope), so they join at "current virtual time" rather
 * than at 0 - which would otherwise win every comparison until catching up
 * to the rest of the hands, starving the other devs in the meantime.
 */
struct dev_stripe_state {
	u64			next_alloc[BCH_SB_MEMBERS_MAX];
	struct bch_devs_mask	cached_devs;
};

#define WRITE_POINT_STATES()		\
	x(stopped)			\
	x(waiting_io)			\
	x(waiting_work)			\
	x(runnable)			\
	x(running)

enum write_point_state {
#define x(n)	WRITE_POINT_##n,
	WRITE_POINT_STATES()
#undef x
	WRITE_POINT_STATE_NR
};

struct write_point {
	struct {
		struct hlist_node	node;
		struct mutex		lock;
		u64			last_used;
		unsigned long		write_point;
		enum bch_data_type	data_type;

		/* calculated based on how many pointers we're actually going to use: */
		unsigned		sectors_free;
		unsigned		prev_sectors_free;

		struct open_buckets	ptrs;
		struct dev_stripe_state	stripe;

		u64			sectors_allocated;
	} __aligned(SMP_CACHE_BYTES);

	struct {
		struct work_struct	index_update_work;

		struct list_head	writes;
		spinlock_t		writes_lock;

		enum write_point_state	state;
		u64			last_state_change;
		u64			time[WRITE_POINT_STATE_NR];
		u64			last_runtime;
	} __aligned(SMP_CACHE_BYTES);
};

struct write_point_specifier {
	unsigned long		v;
};

struct bch_fs_capacity_pcpu {
	struct bch_fs_usage_base	usage;
	u64			sectors_available;
	u64			online_reserved;
};

struct bch_fs_capacity {
	u64			capacity; /* sectors */
	u64			reserved; /* sectors */

	/*
	 * When capacity _decreases_ (due to a disk being removed), we
	 * increment capacity_gen - this invalidates outstanding reservations
	 * and forces them to be revalidated
	 */
	u32			capacity_gen;
	unsigned		bucket_size_max;

	atomic64_t		sectors_available;
	spinlock_t		sectors_available_lock;

	struct bch_fs_capacity_pcpu __percpu	*pcpu;

	struct percpu_rw_semaphore	mark_lock;
};

struct bch_fs_allocator {
	struct bch_devs_mask	rw_devs[BCH_DATA_NR];
	unsigned long		rw_devs_change_count;

	spinlock_t		freelist_lock;
	struct closure_waitlist	freelist_wait;
	unsigned long		last_stuck;

	open_bucket_idx_t	open_buckets_freelist;
	open_bucket_idx_t	open_buckets_nr_free;
	struct closure_waitlist	open_buckets_wait;
	struct open_bucket	open_buckets[OPEN_BUCKETS_COUNT];
	open_bucket_idx_t	open_buckets_hash[OPEN_BUCKETS_COUNT];

	open_bucket_idx_t	open_buckets_partial[OPEN_BUCKETS_COUNT];
	open_bucket_idx_t	open_buckets_partial_nr;

	struct write_point	write_points[WRITE_POINT_MAX];
	struct hlist_head	write_points_hash[WRITE_POINT_HASH_NR];
	struct mutex		write_points_hash_lock;
	unsigned		write_points_nr;

	struct write_point	btree_write_point;
	struct write_point	reconcile_write_point;
};

typedef struct {
	u64				dev_bucket;
	/*
	 * Stashed at add time so completion doesn't have to re-derive ca via
	 * c->devs[idx], which dev_remove may have cleared while our io_ref
	 * still pins the dev object.
	 */
	struct bch_dev			*ca;
	bool				complete;
	bool				marking_free;
} discard_in_flight;

struct discard_release {
	u64		buffer;
	u64		pending_need_flush;
	u64		pending_need_rewind_advance;
	u64		pending_total;
	u64		free;
	u64		reserve;
	u64		buffer_clamped;
	s64		release;
	u64		new_rewind_seq;
	bool		flush_journal;
	bool		flush_wb;
};

struct discard_state {
	u64			seen;
	u64			not_rw;
	u64			eexist;
	u64			eagain;
	u64			open;
	u64			need_journal_commit;
	u64			need_rewind_advance;
	u64			bad_data_type;
	u64			discarded;
	u64			committed;
	struct bpos		pos;
	struct discard_release	r;
};

struct bch_fs_discards {
	struct work_struct		work;
	struct bio_set			bioset;

	DARRAY(discard_in_flight)	in_flight;
	spinlock_t			lock;
	u32				ref;
	u8				refs[BCH_SB_MEMBERS_MAX];
	struct closure_waitlist		wait;

	struct discard_state		s;
};

#endif /* _BCACHEFS_ALLOC_TYPES_H */
