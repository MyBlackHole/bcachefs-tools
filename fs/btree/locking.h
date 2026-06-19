/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCACHEFS_BTREE_LOCKING_H
#define _BCACHEFS_BTREE_LOCKING_H

/*
 * Only for internal btree use:
 *
 * The btree iterator tracks what locks it wants to take, and what locks it
 * currently has - here we have wrappers for locking/unlocking btree nodes and
 * updating the iterator state
 */

// 备注：B-tree 锁机制实现
// 备注：
// 备注：本文件定义了 B-tree 节点的锁管理接口，使用 SIX 锁（Shared/Intent/Exclusive）实现细粒度并发控制。
// 备注：
// 备注：SIX 锁机制：
// 备注：- Shared (S): 共享锁，多个读者可以同时持有，阻止写者
// 备注：- Intent (I): 意图锁，持有者可以进行善意更新，阻止其他意图锁和写锁
// 备注：- Exclusive (X): 排他锁，独占访问，阻止所有其他锁
// 备注：
// 备注：锁兼容性矩阵：
// 备注：| 持有 \ 请求 | S    | I    | X    |
// 备注：|------------|------|------|------|
// 备注：| S          | ✓    | ✓    | ✗    |
// 备注：| I          | ✓    | ✗    | ✗    |
// 备注：| X          | ✗    | ✗    | ✗    |
// 备注：
// 备注：路径锁状态管理：
// 备注：- btree_path::nodes_locked 位图记录每层节点的锁状态
// 备注：- 每 2 位表示一层：00=无锁, 01=read, 10=intent, 11=write
// 备注：- 通过 lock_seq 检测节点是否被其他事务修改
// 备注：
// 备注：事务重启机制：
// 备注：- 当锁获取失败或检测到节点被修改（lock_seq 不匹配）时返回 RESTART
// 备注：- 使用 lockrestart_do 宏自动重试整个事务

#include "btree/cache.h"
#include "btree/iter.h"
#include "btree/locking_types.h"
#include "util/six.h"

// 备注：bch2_btree_lock_init - 初始化 B-tree 节点的锁
// 备注：@b: B-tree 节点通用结构
// 备注：@flags: 锁初始化标志
// 备注：@gfp: 内存分配标志
// 备注：
// 备注：在节点首次使用前调用，初始化 six_lock 结构
void bch2_btree_lock_init(struct btree_bkey_cached_common *, enum six_lock_init_flags, gfp_t gfp);

DECLARE_PER_CPU(struct lock_graph, bch2_lock_graph);

void bch2_lock_graph_init_one(struct lock_graph *);
void bch2_lock_graph_exit_one(struct lock_graph *);

int bch2_lock_graph_init(void);
void bch2_lock_graph_exit(void);

// 备注：bch2_trans_unlock_write - 解锁事务中所有写锁
// 备注：@trans: 事务上下文
// 备注：
// 备注：在事务提交或重启时调用，释放所有持有写锁的节点
void bch2_trans_unlock_write(struct btree_trans *);

// 备注：is_btree_node - 检查路径层级是否指向有效 B-tree 节点
// 备注：@path: B-tree 路径
// 备注：@l: 层级索引
// 备注：
// 备注：返回：true 如果层级有效且节点非空
static inline bool is_btree_node(struct btree_path *path, unsigned l)
{
	return l < BTREE_MAX_DEPTH && !IS_ERR_OR_NULL(path->l[l].b);
}

// 备注：btree_transaction_stats - 获取事务统计信息
// 备注：@trans: 事务上下文
// 备注：
// 备注：根据 fn_idx 返回对应函数的统计结构，用于性能分析
static inline struct btree_transaction_stats *btree_trans_stats(struct btree_trans *trans)
{
	return trans->fn_idx < ARRAY_SIZE(trans->c->btree.trans.stats)
		? &trans->c->btree.trans.stats[trans->fn_idx]
		: NULL;
}

/* trans locked state */

static inline void trans_maybe_disable_migrate(struct btree_trans *trans)
{
	/*
	 * Pin to CPU while btree locks are held: keeps cache footprint
	 * hot, and per-CPU cursors (e.g. inode allocation) stable
	 * across transaction restarts. Released in trans_set_unlocked,
	 * so any wait that goes through bch2_trans_unlock(_long)
	 * happens with migration enabled - including the cond_resched
	 * in bch2_trans_begin and the freezer-visible window during
	 * suspend.
	 */
	if (!trans->migrate_disabled &&
	    trans->shard_cpu >= 0 &&
	    trans->shard_cpu == raw_smp_processor_id()) {
		trans->migrate_disabled = true;
		migrate_disable();
	}
}

static inline void trans_enable_migrate(struct btree_trans *trans)
{
	if (trans->migrate_disabled) {
		trans->migrate_disabled = false;
		migrate_enable();
	}
}

static inline void trans_set_locked(struct btree_trans *trans, bool try)
{
	if (!trans->locked) {
		trans->locked = true;
		trans->last_unlock_ip = 0;
		lock_acquire_exclusive(&trans->dep_map, 0, try, NULL, _THIS_IP_);

		trans->pf_memalloc_nofs = (current->flags & PF_MEMALLOC_NOFS) != 0;
		current->flags |= PF_MEMALLOC_NOFS;

		trans_maybe_disable_migrate(trans);
	}
}

static inline void trans_set_unlocked(struct btree_trans *trans)
{
	if (trans->locked) {
		trans->locked = false;
		trans->last_unlock_ip = _RET_IP_;
		lock_release(&trans->dep_map, _THIS_IP_);

		if (!trans->pf_memalloc_nofs)
			current->flags &= ~PF_MEMALLOC_NOFS;
	}
}

/*
 * Shard index for inode-number allocation. We used to use the current CPU id,
 * but threads migrate across CPUs and the win from per-CPU allocator
 * separation evaporates — concurrent allocators end up sharing shards (and
 * fighting on the same alloc_cursor btree node) any time the scheduler
 * shuffles them onto the same core. Hashing the task's pid is stable per
 * thread, so concurrent allocators in different threads keep their separation
 * regardless of which CPU they're currently running on.
 */
static inline u64 bch2_inode_shard_idx(struct bch_fs *c)
{
	return c->opts.shard_inode_numbers_bits
		? hash_64((u64) current->pid, c->opts.shard_inode_numbers_bits)
		: 0;
}

static inline unsigned bch2_inode_shard_cpu(struct bch_fs *c)
{
	return c->inode_shard_cpu[bch2_inode_shard_idx(c)];
}

/* path lock state */

/* matches six lock types */
// 备注：锁类型枚举 - 映射 SIX 锁类型到 B-tree 上下文
// 备注：
// 备注：BTREE_NODE_UNLOCKED:    无锁
// 备注：BTREE_NODE_READ_LOCKED: 共享锁 - 允许并发读取
// 备注：BTREE_NODE_INTENT_LOCKED: 意图锁 - 允许善意更新
// 备注：BTREE_NODE_WRITE_LOCKED: 排他锁 - 独占访问
enum btree_node_locked_type {
	BTREE_NODE_UNLOCKED		= -1,
	BTREE_NODE_READ_LOCKED		= SIX_LOCK_read,
	BTREE_NODE_INTENT_LOCKED	= SIX_LOCK_intent,
	BTREE_NODE_WRITE_LOCKED		= SIX_LOCK_write,
};

// 备注：btree_node_locked_type - 获取路径指定层级的锁类型
// 备注：@path: B-tree 路径
// 备注：@level: 层级索引
// 备注：
// 备注：从 nodes_locked 位图中提取指定层级的锁状态
// 备注：每层使用 2 位：bits[2k:2k+1] 表示层级 k 的锁类型
static inline int btree_node_locked_type(struct btree_path *path,
					 unsigned level)
{
	return BTREE_NODE_UNLOCKED + ((path->nodes_locked >> (level << 1)) & 3);
}

// 备注：btree_node_locked_type_nowrite - 获取不考虑写锁的锁类型
// 备注：@path: B-tree 路径
// 备注：@level: 层级索引
// 备注：
// 备注：如果当前持有写锁，返回意图锁（用于检查兼容性）
// 备注：这允许意图锁持有者看到写锁的存在但仍继续操作
static inline int btree_node_locked_type_nowrite(struct btree_path *path,
						 unsigned level)
{
	int have = btree_node_locked_type(path, level);
	return have == BTREE_NODE_WRITE_LOCKED
		? BTREE_NODE_INTENT_LOCKED
		: have;
}

// 备注：btree_node_write_locked - 检查是否持有写锁
// 备注：@path: B-tree 路径
// 备注：@l: 层级索引
static inline bool btree_node_write_locked(struct btree_path *path, unsigned l)
{
	return btree_node_locked_type(path, l) == BTREE_NODE_WRITE_LOCKED;
}

// 备注：btree_node_intent_locked - 检查是否持有意图锁
// 备注：@path: B-tree 路径
// 备注：@l: 层级索引
static inline bool btree_node_intent_locked(struct btree_path *path, unsigned l)
{
	return btree_node_locked_type(path, l) == BTREE_NODE_INTENT_LOCKED;
}

// 备注：btree_node_read_locked - 检查是否持有读锁
// 备注：@path: B-tree 路径
// 备注：@l: 层级索引
static inline bool btree_node_read_locked(struct btree_path *path, unsigned l)
{
	return btree_node_locked_type(path, l) == BTREE_NODE_READ_LOCKED;
}

// 备注：btree_node_locked - 检查是否持有任何锁
// 备注：@path: B-tree 路径
// 备注：@level: 层级索引
static inline bool btree_node_locked(struct btree_path *path, unsigned level)
{
	return btree_node_locked_type(path, level) != BTREE_NODE_UNLOCKED;
}

static inline int __must_check bch2_btree_path_traverse(struct btree_trans *trans,
					  btree_path_idx_t path,
					  enum btree_iter_update_trigger_flags flags)
{
	bch2_trans_verify_not_unlocked_or_in_restart(trans);

	return !trans->paths[path].nodes_locked
		? bch2_btree_path_traverse_one(trans, path, flags)
		: 0;
}

static inline void mark_btree_node_locked_noreset(struct btree_path *path,
						  unsigned level,
						  enum btree_node_locked_type type)
{
	/* relying on this to avoid a branch */
	BUILD_BUG_ON(SIX_LOCK_read   != 0);
	BUILD_BUG_ON(SIX_LOCK_intent != 1);

	path->nodes_locked &= ~(3U << (level << 1));
	path->nodes_locked |= (type + 1) << (level << 1);
}

// 备注：mark_btree_node_locked - 标记锁并更新统计时间戳
// 备注：
// 备注：在 mark_btree_node_locked_noreset 的基础上增加了
// 备注：CONFIG_BCACHEFS_LOCK_TIME_STATS 的计时记录。
// 备注：lock_taken_time 用于后续计算锁持有时间。
// 备注：
// 备注："Noreset" vs 不带后缀：Noreset 版本不更新时间戳，
// 备注：在 write_contended 等场景中锁状态回退时使用。
static inline void mark_btree_node_locked(struct btree_trans *trans,
					  struct btree_path *path,
					  unsigned level,
					  enum btree_node_locked_type type)
{
	mark_btree_node_locked_noreset(path, level, (enum btree_node_locked_type) type);
#ifdef CONFIG_BCACHEFS_LOCK_TIME_STATS
	path->l[level].lock_taken_time = local_clock();
#endif
}

// 备注：__btree_lock_want - 计算路径在指定层级期望的锁类型
// 备注：
// 备注：路径遍历 btree 时，内部节点（< locks_want）需要 intent 锁
// 备注：以防止结构变更（split/merge），叶子节点（== level）需要 read 锁。
// 备注：locks_want 表示路径想要持有锁的最高层级 + 1。
// 备注：内部节点用 intent 而非 read：因为 split/merge 需要阻塞其他
// 备注：可能修改内部节点指针的操作（intent 与 intent 互斥）。
static inline enum six_lock_type __btree_lock_want(struct btree_path *path, int level)
{
	return level < path->locks_want
		? SIX_LOCK_intent
		: SIX_LOCK_read;
}

// 备注：btree_lock_want - 带级差异的锁期望策略
// 备注：
// 备注：相比 __btree_lock_want，多了一层 level 判断：
// 备注：  level < path->level → 不持有锁（该层不属于此路径的查找范围）
// 备注：  level < path->locks_want → intent 锁（内部节点）
// 备注：  level == path->level → read 锁（叶子/目标节点）
// 备注：  level > path->level → 无锁（超出遍历范围）
// 备注：
// 备注：path->level 是路径当前查找到的实际层级（从叶子往上走），
// 备注：path->locks_want 是配置想要的最深层级。当路径刚创建时，
// 备注：level 可能高于 locks_want（从根向下遍历中），此时不需要锁。
static inline enum btree_node_locked_type
btree_lock_want(struct btree_path *path, int level)
{
	if (level < path->level)
		return BTREE_NODE_UNLOCKED;
	if (level < path->locks_want)
		return BTREE_NODE_INTENT_LOCKED;
	if (level == path->level)
		return BTREE_NODE_READ_LOCKED;
	return BTREE_NODE_UNLOCKED;
}

static void btree_trans_lock_hold_time_update(struct btree_trans *trans,
					      struct btree_path *path, unsigned level)
{
#ifdef CONFIG_BCACHEFS_LOCK_TIME_STATS
	__bch2_time_stats_update(&btree_trans_stats(trans)->lock_hold_times,
				 path->l[level].lock_taken_time,
				 local_clock());
#endif
}

static inline int btree_path_lowest_level_locked(struct btree_path *path)
{
	return __ffs(path->nodes_locked) >> 1;
}

static inline int btree_path_highest_level_locked(struct btree_path *path)
{
	return __fls(path->nodes_locked) >> 1;
}

/* unlock: */

void bch2_btree_node_unlock_write(struct btree_trans *,
			struct btree_path *, struct btree *);

/*
 * Updates the saved lock sequence number, so that bch2_btree_node_relock() will
 * succeed:
 */
static inline void
__bch2_btree_node_unlock_write(struct btree_trans *trans, struct btree *b)
{
	if (!b->c.lock.write_lock_recurse) {
		struct btree_path *linked;
		unsigned i;

		trans_for_each_path_with_node(trans, b, linked, i)
			linked->l[b->c.level].lock_seq++;
	}

	six_unlock_write(&b->c.lock);
}

static inline void
bch2_btree_node_unlock_write_inlined(struct btree_trans *trans, struct btree_path *path,
				     struct btree *b)
{
	EBUG_ON(path->l[b->c.level].b != b);
	EBUG_ON(path->l[b->c.level].lock_seq != six_lock_seq(&b->c.lock));
	EBUG_ON(btree_node_locked_type(path, b->c.level) != SIX_LOCK_write);

	mark_btree_node_locked_noreset(path, b->c.level, BTREE_NODE_INTENT_LOCKED);
	__bch2_btree_node_unlock_write(trans, b);
}

static inline void btree_node_unlock(struct btree_trans *trans,
				     struct btree_path *path, unsigned level)
{
	int lock_type = btree_node_locked_type(path, level);

	EBUG_ON(level >= BTREE_MAX_DEPTH);

	if (lock_type != BTREE_NODE_UNLOCKED) {
		if (unlikely(lock_type == BTREE_NODE_WRITE_LOCKED)) {
			bch2_btree_node_unlock_write(trans, path, path->l[level].b);
			lock_type = BTREE_NODE_INTENT_LOCKED;
		}
		six_unlock_type(&path->l[level].b->c.lock, lock_type);
		btree_trans_lock_hold_time_update(trans, path, level);
		mark_btree_node_locked_noreset(path, level, BTREE_NODE_UNLOCKED);
	}
}

static inline void __bch2_btree_path_unlock(struct btree_trans *trans,
					    struct btree_path *path)
{
	while (path->nodes_locked)
		btree_node_unlock(trans, path, btree_path_lowest_level_locked(path));
}

/* lock: */

int bch2_six_check_for_deadlock(struct six_lock *lock, struct six_lock_waiter *);

static inline void bch2_btree_node_unlock_with_path(struct btree_trans *trans,
						    btree_path_idx_t path_idx,
						    unsigned level)
{
	btree_node_unlock(trans, trans->paths + path_idx, level);
	bch2_path_put(trans, path_idx, true);
}

static inline int btree_node_lock_nopath(struct btree_trans *trans,
					 struct btree_bkey_cached_common *b,
					 enum six_lock_type type,
					 bool lock_may_not_fail,
					 unsigned long ip,
					 bool contended)
{
	trans->lock_may_not_fail = lock_may_not_fail;
	trans->lock_must_abort	= false;
	trans->locking		= b;

	/* trans->locking_hash_val is set by the caller; it must be the
	 * hash of the key used to look up this node (not the node's
	 * current hash_val), so that bch2_six_check_for_deadlock catches
	 * the case where the node was reclaimed AND re-hashed to a new
	 * identity. 0 disables the check (lock_root, cached, relock). */

	int ret = !contended
		? six_lock_ip_waiter(&b->lock, type, &trans->locking_wait, bch2_six_check_for_deadlock, ip)
		: six_lock_contended(&b->lock, type, &trans->locking_wait, bch2_six_check_for_deadlock, ip);

	BUG_ON(lock_may_not_fail && ret);

	if (unlikely(ret == -ENOMEM))
		ret = btree_trans_restart(trans, BCH_ERR_transaction_restart_lock_waitlist_alloc);

	WRITE_ONCE(trans->locking, NULL);

	trans_maybe_disable_migrate(trans);

#ifdef CONFIG_BCACHEFS_DEBUG
	event_trace(trans->c, btree_path_lock, buf,
		prt_printf(&buf, "%s ret %s\n"
			   "btree %s level %u lock seq %u node %px",
			   trans->fn, bch2_err_str(ret),
			   bch2_btree_id_str(b->btree_id),
			   b->level,
			   six_lock_seq(&b->lock),
			   b));
#endif
	return ret;
}

int bch2_btree_node_lock_slowpath(struct btree_trans *trans,
			struct btree_path *path,
			struct btree_bkey_cached_common *b,
			unsigned level,
			enum six_lock_type type);

// 备注：btree_node_lock - B-tree 节点快速路径锁获取
// 备注：
// 备注：btree 锁获取的入口函数，分为快速路径和慢速路径两层：
// 备注：
// 备注：  快速路径：six_trylock_type() — 非阻塞尝试获取锁
// 备注：    成功 → 立即返回（最常用情况，锁无争用）
// 备注：
// 备注：  慢速路径：six_trylock 失败 → bch2_btree_node_lock_slowpath()
// 备注：    → six_lock_ip_waiter() 准备睡眠
// 备注：    → bch2_six_check_for_deadlock() 死锁检测
// 备注：    → 若检测到死锁则返回 RESTART（事务重启）
// 备注：    → 否则 schedule() 睡眠等待锁释放
// 备注：
// 备注：try() 宏确保 slowpath 返回 RESTART 时被传播到事务引擎。
// 备注：事务重启后 btree_path 会通过 bch2_btree_path_relock() 重锁，
// 备注：重锁时走 __bch2_btree_node_relock() 路径（不经过 slowpath）。
static inline int btree_node_lock(struct btree_trans *trans,
			struct btree_path *path,
			struct btree_bkey_cached_common *b,
			unsigned level,
			enum six_lock_type type)
{
	EBUG_ON(level >= BTREE_MAX_DEPTH);
	bch2_trans_verify_not_unlocked_or_in_restart(trans);

	if (!likely(six_trylock_type(&b->lock, type)))
		try(bch2_btree_node_lock_slowpath(trans, path, b, level, type));

#ifdef CONFIG_BCACHEFS_LOCK_TIME_STATS
	path->l[b->level].lock_taken_time = local_clock();
#endif
	return 0;
}

int bch2_btree_node_lock_write_contended(struct btree_trans *, struct btree_path *,
					 struct btree_bkey_cached_common *b, bool);

// 备注：__btree_node_lock_write - 获取 btree 节点写锁
// 备注：
// 备注：在已持有 intent 锁的节点上获取 write 锁（intent→write 升级）。
// 备注：
// 备注：【锁状态转换】
// 备注：  调用前：节点至少被 intent 锁锁定（EBUG_ON 断言）
// 备注：  调用后：写入 nodes_locked 位图为 WRITE_LOCKED
// 备注：
// 备注：【关键设计 —— 先标记、后获取】
// 备注：SIX 锁不是公平锁，且读锁会阻塞写锁请求者。
// 备注：所以必须先通过 mark_btree_node_locked_noreset 将路径的锁状态
// 备注：标记为 WRITE_LOCKED，再实际获取写锁。
// 备注：
// 备注：这个顺序至关重要：标记操作使死锁检测器在遍历此路径时看到
// 备注：当前事务"已持有写锁"（虽然实际还没拿到），确保正确的依赖图
// 备注：构建——等待此节点写锁的其他事务会看到等待关系。
// 备注：如果先获取锁再标记，死锁检测器可能在间隙期看到旧状态。
// 备注：
// 备注：快速路径：six_trylock_write() 成功 → 立即返回
// 备注：慢速路径：→ bch2_btree_node_lock_write_contended()
// 备注：  → 临时释放读锁计数
// 备注：  → btree_node_lock_nopath() 获取写锁（含死锁检测）
// 备注：  → 失败时恢复读锁计数，锁状态回退为 INTENT_LOCKED
static inline int __btree_node_lock_write(struct btree_trans *trans,
					  struct btree_path *path,
					  struct btree_bkey_cached_common *b,
					  bool lock_may_not_fail)
{
	EBUG_ON(&path->l[b->level].b->c != b);
	EBUG_ON(path->l[b->level].lock_seq != six_lock_seq(&b->lock));
	EBUG_ON(!btree_node_intent_locked(path, b->level));

	/*
	 * six locks are unfair, and read locks block while a thread wants a
	 * write lock: thus, we need to tell the cycle detector we have a write
	 * lock _before_ taking the lock:
	 */
	mark_btree_node_locked_noreset(path, b->level, BTREE_NODE_WRITE_LOCKED);

	return likely(six_trylock_write(&b->lock))
		? 0
		: bch2_btree_node_lock_write_contended(trans, path, b, lock_may_not_fail);
}

static inline int __must_check
bch2_btree_node_lock_write(struct btree_trans *trans,
			   struct btree_path *path,
			   struct btree_bkey_cached_common *b)
{
	return __btree_node_lock_write(trans, path, b, false);
}

static inline void bch2_btree_node_lock_write_nofail(struct btree_trans *trans,
				       struct btree_path *path,
				       struct btree_bkey_cached_common *b)
{
	int ret = __btree_node_lock_write(trans, path, b, true);
	BUG_ON(ret);
}

int __must_check
bch2_btree_node_lock_with_path(struct btree_trans *,
			       struct btree_bkey_cached_common *,
			       enum six_lock_type, btree_path_idx_t *);

/* relock: */

bool bch2_btree_path_relock_norestart(struct btree_trans *, struct btree_path *);
int __bch2_btree_path_relock(struct btree_trans *, struct btree_path *);

static inline int bch2_btree_path_relock(struct btree_trans *trans, struct btree_path *path)
{
	return btree_node_locked(path, path->level)
		? 0
		: __bch2_btree_path_relock(trans, path);
}

bool __bch2_btree_node_relock(struct btree_trans *, struct btree_path *, unsigned, bool trace);

static inline bool bch2_btree_node_relock(struct btree_trans *trans,
					  struct btree_path *path, unsigned level)
{
	EBUG_ON(btree_node_locked(path, level) &&
		!btree_node_write_locked(path, level) &&
		btree_node_locked_type(path, level) != __btree_lock_want(path, level));

	return likely(btree_node_locked(path, level)) ||
		(!IS_ERR_OR_NULL(path->l[level].b) &&
		 __bch2_btree_node_relock(trans, path, level, true));
}

static inline bool bch2_btree_node_relock_notrace(struct btree_trans *trans,
						  struct btree_path *path, unsigned level)
{
	EBUG_ON(btree_node_locked(path, level) &&
		btree_node_locked_type_nowrite(path, level) !=
		__btree_lock_want(path, level));

	return likely(btree_node_locked(path, level)) ||
		(!IS_ERR_OR_NULL(path->l[level].b) &&
		 __bch2_btree_node_relock(trans, path, level, false));
}

/* upgrade */

bool __bch2_btree_path_upgrade_norestart(struct btree_trans *, struct btree_path *, unsigned);

static inline bool bch2_btree_path_upgrade_norestart(struct btree_trans *trans,
			       struct btree_path *path,
			       unsigned new_locks_want)
{
	return new_locks_want > path->locks_want
		? __bch2_btree_path_upgrade_norestart(trans, path, new_locks_want)
		: true;
}

int __bch2_btree_path_upgrade(struct btree_trans *,
			      struct btree_path *, unsigned);

static inline int bch2_btree_path_upgrade(struct btree_trans *trans,
					  struct btree_path *path,
					  unsigned new_locks_want)
{
	new_locks_want = min(new_locks_want, BTREE_MAX_DEPTH);

	return likely(path->locks_want >= new_locks_want && path->nodes_locked)
		? 0
		: __bch2_btree_path_upgrade(trans, path, new_locks_want);
}

/* misc: */

static inline void btree_path_set_should_be_locked(struct btree_trans *trans, struct btree_path *path)
{
	EBUG_ON(!btree_node_locked(path, path->level));

	if (!path->should_be_locked) {
		path->should_be_locked = true;
#ifdef CONFIG_BCACHEFS_DEBUG
		event_trace(trans->c, btree_path_should_be_locked, buf, ({
			prt_printf(&buf, "%s\n", trans->fn);
			bch2_btree_path_to_text_short(&buf, trans, path - trans->paths, path);
		}));
#endif
	}
}

// 备注：重置坏路径
static inline void __btree_path_set_level_up(struct btree_trans *trans,
				      struct btree_path *path,
				      unsigned l)
{
	btree_node_unlock(trans, path, l);
	path->l[l].b = ERR_PTR(-BCH_ERR_no_btree_node_up);
}

static inline void btree_path_set_level_up(struct btree_trans *trans,
				    struct btree_path *path)
{
	__btree_path_set_level_up(trans, path, path->level++);
}

/* debug */

struct six_lock_count bch2_btree_node_lock_counts(struct btree_trans *,
				struct btree_path *,
				struct btree_bkey_cached_common *b,
				unsigned);

int bch2_check_for_deadlock(struct btree_trans *, struct printbuf *);

void __bch2_btree_path_verify_locks(struct btree_trans *, struct btree_path *);
void __bch2_trans_verify_locks(struct btree_trans *);

static inline void bch2_btree_path_verify_locks(struct btree_trans *trans,
						struct btree_path *path)
{
	if (IS_ENABLED(CONFIG_BCACHEFS_DEBUG) &&
	    static_branch_unlikely(&bch2_debug_check_btree_locking))
		__bch2_btree_path_verify_locks(trans, path);
}

static inline void bch2_trans_verify_locks(struct btree_trans *trans)
{
	if (IS_ENABLED(CONFIG_BCACHEFS_DEBUG) &&
	    static_branch_unlikely(&bch2_debug_check_btree_locking))
		__bch2_trans_verify_locks(trans);
}

#endif /* _BCACHEFS_BTREE_LOCKING_H */
