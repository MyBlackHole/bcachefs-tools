/* SPDX-License-Identifier: GPL-2.0 */
// 备注：B-tree 迭代器接口定义
// 备注：
// 备注：本文件定义了 B-tree 迭代器的核心操作接口，包括：
// 备注：
// 备注：1. 路径管理 (btree_path)
// 备注：   - __btree_path_get: 增加路径引用计数
// 备注：   - __btree_path_put: 减少路径引用计数，释放不再使用的路径
// 备注：   - 路径引用计数机制允许多个迭代器共享同一路径
// 备注：
// 备注：2. 迭代器状态管理
// 备注：   - btree_node_lock_seq_matches: 检查节点锁序列是否匹配（检测并发修改）
// 备注：
// 备注：3. 节点关系查询
// 备注：   - btree_path_node: 获取路径中指定层级的节点
// 备注：   - btree_node_parent: 获取节点的父节点
// 备注：
// 备注：迭代器工作流程：
// 备注：1. trans_get -> 分配/复用 btree_path
// 备注：2. btree_iter_traverse -> 从根遍历到目标位置
// 备注：3. btree_iter_next / btree_iter_next_slot -> 迭代访问键
// 备注：4. trans_put -> 释放路径引用
// 备注：
// 备注：路径复用与锁：
// 备注：- 路径按锁顺序排序，避免死锁
// 备注：- 持有锁时标记 should_be_locked，事务重启时自动重获取
#ifndef _BCACHEFS_BTREE_ITER_H
#define _BCACHEFS_BTREE_ITER_H

#include "btree/bset.h"
#include "btree/cache.h"
#include "btree/types.h"

#include "closure.h"
#include "sb/counters.h"

// 备注：bch2_trans_updates_to_text - 打印事务中的所有待处理更新
// 备注：@out: 输出缓冲区
// 备注：@trans: 事务上下文
// 备注：
// 备注：用于调试：显示 trans->updates 数组中的所有更新条目
void bch2_trans_updates_to_text(struct printbuf *, struct btree_trans *);

// 备注：bch2_btree_path_to_text_short - 打印路径的简要信息
// 备注：@out: 输出缓冲区
// 备注：@trans: 事务上下文
// 备注：@idx: 路径索引
// 备注：@path: B-tree 路径
// 备注：
// 备注：输出格式：btree_id pos [level]
void bch2_btree_path_to_text_short(struct printbuf *, struct btree_trans *,
				   btree_path_idx_t, struct btree_path *);

// 备注：bch2_btree_path_to_text - 打印路径的详细信息
// 备注：@out: 输出缓冲区
// 备注：@trans: 事务上下文
// 备注：@idx: 路径索引
// 备注：@path: B-tree 路径
// 备注：
// 备注：输出包括：路径索引、位置、btree_id、每层节点信息、锁状态
void bch2_btree_path_to_text(struct printbuf *, struct btree_trans *,
			     btree_path_idx_t, struct btree_path *);

// 备注：bch2_trans_paths_to_text - 打印事务中所有路径的简要信息
// 备注：@out: 输出缓冲区
// 备注：@trans: 事务上下文
void bch2_trans_paths_to_text(struct printbuf *, struct btree_trans *);

// 备注：bch2_dump_trans_paths_updates - 调试转储路径和更新信息
// 备注：@trans: 事务上下文
// 备注：
// 备注：当路径引用计数溢出时调用，打印详细的调试信息
void bch2_dump_trans_paths_updates(struct btree_trans *);

// 备注：__bkey_err - 从 bkey 指针获取错误码
// 备注：@k: bkey 指针（可能是错误指针）
// 备注：
// 备注：返回值：如果是错误指针返回负错误码，否则返回 0
// 备注：这是内核 PTR_ERR 宏的封装，用于错误传播
static inline int __bkey_err(const struct bkey *k)
{
	return PTR_ERR_OR_ZERO(k);
}

// 备注：bkey_err - 获取 bkey 值的错误码
// 备注：@_k: bkey 值类型
// 备注：
// 备注：用于 try 宏的错误检查
#define bkey_err(_k)	__bkey_err((_k).k)

// 备注：bkey_try - 安全获取 bkey 值，传播错误
// 备注：@_do: 要执行的表达式
// 备注：
// 备注：如果表达式返回错误，try 宏会将错误传播给调用者
// 备注：否则返回 bkey 值
#define bkey_try(_do)					\
({							\
	typeof(_do) _k = _do;				\
	try(bkey_err(_k));				\
	_k;						\
})

// 备注：__btree_path_get - 增加路径引用计数
// 备注：@trans: 事务上下文
// 备注：@path: B-tree 路径
// 备注：@intent: 是否增加 intent 引用计数
// 备注：
// 备注：路径引用机制：
// 备注：- 多个迭代器可以共享同一路径（通过 ref 计数）
// 备注：- intent_ref 用于跟踪 intent 锁的持有数量
// 备注：- 路径分配时记录 IP 地址用于调试
// 备注：
// 备注：边界检查：
// 备注：- 索引必须在有效范围内
// 备注：- 路径必须已分配
// 备注：- ref 不能溢出 U8_MAX
static inline void __btree_path_get(struct btree_trans *trans, struct btree_path *path, bool intent)
{
	unsigned idx = path - trans->paths;

	EBUG_ON(idx >= trans->nr_paths);
	EBUG_ON(!test_bit(idx, trans->paths_allocated));
	if (unlikely(path->ref == U8_MAX)) {
		bch2_dump_trans_paths_updates(trans);
		panic("path %u refcount overflow\n", idx);
	}

	path->ref++;
	path->intent_ref += intent;
#ifdef CONFIG_BCACHEFS_DEBUG
	event_trace(trans->c, btree_path_get_ll, buf, ({
		prt_printf(&buf, "%s: path %3u ref %u btree ", trans->fn,
			   idx, path->ref);
		bch2_btree_id_to_text(&buf, path->btree_id);
		prt_str(&buf, " pos ");
		bch2_bpos_to_text(&buf, path->pos);
	}));
#endif
}

// 备注：__btree_path_put - 减少路径引用计数
// 备注：@trans: 事务上下文
// 备注：@path: B-tree 路径
// 备注：@intent: 是否减少 intent 引用计数
// 备注：
// 备注：返回：true 如果引用计数降为 0，路径可以回收
// 备注：
// 备注：释放过程：
// 备注：1. 减少 intent_ref（如果有 intent 锁）
// 备注：2. 减少 ref
// 备注：3. 如果 ref 为 0，清除分配位，归还路径到空闲池
// 备注：
// 备注：断言检查：
// 备注：- 路径必须在有效范围内
// 备注：- 路径必须已分配
// 备注：- ref 必须大于 0
// 备注：- 如果 intent 为 true，intent_ref 必须大于 0
static inline bool __btree_path_put(struct btree_trans *trans, struct btree_path *path, bool intent)
{
	EBUG_ON(path - trans->paths >= trans->nr_paths);
	EBUG_ON(!test_bit(path - trans->paths, trans->paths_allocated));
	EBUG_ON(!path->ref);
	EBUG_ON(!path->intent_ref && intent);
#ifdef CONFIG_BCACHEFS_DEBUG
	event_trace(trans->c, btree_path_put_ll, buf, ({
		prt_printf(&buf, "%s: path %3zu ref %u btree ", trans->fn,
			   path - trans->paths, path->ref);
		bch2_btree_id_to_text(&buf, path->btree_id);
		prt_str(&buf, " pos ");
		bch2_bpos_to_text(&buf, path->pos);
	}));
#endif
	path->intent_ref -= intent;
	return --path->ref == 0;
}

// 备注：btree_path_node - 获取路径中指定层级的节点
// 备注：@path: B-tree 路径
// 备注：@level: 层级索引
// 备注：
// 备注：返回：指定层级的 B-tree 节点，如果超出范围则返回 NULL
// 备注：注意：level 从 0 开始（叶子节点），深度递增
static inline struct btree *btree_path_node(struct btree_path *path,
					    unsigned level)
{
	return level < BTREE_MAX_DEPTH ? path->l[level].b : NULL;
}

// 备注：btree_node_lock_seq_matches - 检查锁序列是否匹配
// 备注：@path: B-tree 路径
// 备注：@b: B-tree 节点
// 备注：@level: 节点层级
// 备注：
// 备注：检查路径中记录的锁序列号与节点当前锁序列号是否一致
// 备注：用于检测节点是否在其他事务中被修改
// 备注：
// 备注：工作原理：
// 备注：- 每次获取写锁时，锁序列号递增
// 备注：- 如果节点被修改，序列号会变化
// 备注：- 序列号不匹配意味着需要重新获取锁和遍历
static inline bool btree_node_lock_seq_matches(const struct btree_path *path,
					const struct btree *b, unsigned level)
{
	return path->l[level].lock_seq == six_lock_seq(&b->c.lock);
}

// 备注：btree_node_parent - 获取节点的父节点
// 备注：@path: B-tree 路径
// 备注：@b: B-tree 节点
// 备注：
// 备注：根据节点的 level 字段计算父节点在路径中的层级
// 备注：父节点的层级 = 子节点层级 + 1
static inline struct btree *btree_node_parent(struct btree_path *path,
					      struct btree *b)
{
	return btree_path_node(path, b->c.level + 1);
}

/* Iterate over paths within a transaction: */

void __bch2_btree_trans_sort_paths(struct btree_trans *);

static inline void btree_trans_sort_paths(struct btree_trans *trans)
{
	if (!IS_ENABLED(CONFIG_BCACHEFS_DEBUG) &&
	    trans->paths_sorted)
		return;
	__bch2_btree_trans_sort_paths(trans);
}

static inline unsigned long *trans_paths_nr(struct btree_path *paths)
{
	return &container_of(paths, struct btree_trans_paths, paths[0])->nr_paths;
}

static inline unsigned long *trans_paths_allocated(struct btree_path *paths)
{
	unsigned long *v = trans_paths_nr(paths);
	return v - BITS_TO_LONGS(*v);
}

#define trans_for_each_path_idx_from(_paths_allocated, _nr, _idx, _start)\
	for (_idx = _start;						\
	     (_idx = find_next_bit(_paths_allocated, _nr, _idx)) < _nr;	\
	     _idx++)

static inline struct btree_path *
__trans_next_path(struct btree_trans *trans, unsigned *idx)
{
	unsigned long *w = trans->paths_allocated + *idx / BITS_PER_LONG;
	/*
	 * Open coded find_next_bit(), because
	 *  - this is fast path, we can't afford the function call
	 *  - and we know that nr_paths is a multiple of BITS_PER_LONG,
	 */
	while (*idx < trans->nr_paths) {
		unsigned long v = *w >> (*idx & (BITS_PER_LONG - 1));
		if (v) {
			*idx += __ffs(v);
			return trans->paths + *idx;
		}

		*idx += BITS_PER_LONG;
		*idx &= ~(BITS_PER_LONG - 1);
		w++;
	}

	return NULL;
}

/*
 * This version is intended to be safe for use on a btree_trans that is owned by
 * another thread, for bch2_btree_trans_to_text();
 */
#define trans_for_each_path_from(_trans, _path, _idx, _start)		\
	for (_idx = _start;						\
	     (_path = __trans_next_path((_trans), &_idx));		\
	     _idx++)

#define trans_for_each_path(_trans, _path, _idx)			\
	trans_for_each_path_from(_trans, _path, _idx, 1)

static inline struct btree_path *next_btree_path(struct btree_trans *trans, struct btree_path *path)
{
	unsigned idx = path ? path->sorted_idx + 1 : 0;

	EBUG_ON(idx > trans->nr_sorted);

	return idx < trans->nr_sorted
		? trans->paths + trans->sorted[idx]
		: NULL;
}

static inline struct btree_path *prev_btree_path(struct btree_trans *trans, struct btree_path *path)
{
	unsigned idx = path ? path->sorted_idx : trans->nr_sorted;

	return idx
		? trans->paths + trans->sorted[idx - 1]
		: NULL;
}

#define trans_for_each_path_idx_inorder(_trans, _iter)			\
	for (_iter = (struct trans_for_each_path_inorder_iter) { 0 };	\
	     (_iter.path_idx = trans->sorted[_iter.sorted_idx],		\
	      _iter.sorted_idx < (_trans)->nr_sorted);			\
	     _iter.sorted_idx++)

struct trans_for_each_path_inorder_iter {
	btree_path_idx_t	sorted_idx;
	btree_path_idx_t	path_idx;
};

#define trans_for_each_path_inorder(_trans, _path, _iter)		\
	for (_iter = (struct trans_for_each_path_inorder_iter) { 0 };	\
	     (_iter.path_idx = trans->sorted[_iter.sorted_idx],		\
	      _path = (_trans)->paths + _iter.path_idx,			\
	      _iter.sorted_idx < (_trans)->nr_sorted);			\
	     _iter.sorted_idx++)

#define trans_for_each_path_inorder_reverse(_trans, _path, _i)		\
	for (_i = trans->nr_sorted - 1;					\
	     ((_path) = (_trans)->paths + trans->sorted[_i]), (_i) >= 0;\
	     --_i)

static inline bool __path_has_node(const struct btree_path *path,
				   const struct btree *b)
{
	return path->l[b->c.level].b == b &&
		btree_node_lock_seq_matches(path, b, b->c.level);
}

static inline struct btree_path *
__trans_next_path_with_node(struct btree_trans *trans, struct btree *b,
			    unsigned *idx)
{
	struct btree_path *path;

	while ((path = __trans_next_path(trans, idx)) &&
		!__path_has_node(path, b))
	       (*idx)++;

	return path;
}

#define trans_for_each_path_with_node(_trans, _b, _path, _iter)		\
	for (_iter = 1;							\
	     (_path = __trans_next_path_with_node((_trans), (_b), &_iter));\
	     _iter++)

btree_path_idx_t __bch2_btree_path_make_mut(struct btree_trans *, btree_path_idx_t,
					    bool, unsigned long);

static inline btree_path_idx_t __must_check
bch2_btree_path_make_mut(struct btree_trans *trans,
			 btree_path_idx_t path, bool intent,
			 unsigned long ip)
{
	if (trans->paths[path].ref > 1 ||
	    trans->paths[path].preserve)
		path = __bch2_btree_path_make_mut(trans, path, intent, ip);
	trans->paths[path].should_be_locked = false;
	return path;
}

btree_path_idx_t __must_check
__bch2_btree_path_set_pos(struct btree_trans *, btree_path_idx_t,
			  const struct bpos *, bool, unsigned long);

static inline btree_path_idx_t __must_check
bch2_btree_path_set_pos(struct btree_trans *trans,
			btree_path_idx_t path,
			const struct bpos *new_pos,
			bool intent, unsigned long ip)
{
	return !bpos_eq(*new_pos, trans->paths[path].pos)
		? __bch2_btree_path_set_pos(trans, path, new_pos, intent, ip)
		: path;
}

int __must_check bch2_btree_path_traverse_one(struct btree_trans *, btree_path_idx_t,
					      enum btree_iter_update_trigger_flags);

static inline void bch2_trans_verify_not_unlocked_or_in_restart(struct btree_trans *);

btree_path_idx_t bch2_path_get(struct btree_trans *,
			       enum btree_id, const struct bpos *,
			       unsigned, unsigned,
			       enum btree_iter_update_trigger_flags,
			       unsigned long);
btree_path_idx_t bch2_path_get_unlocked_mut(struct btree_trans *, enum btree_id,
					    unsigned, struct bpos, bool);

struct bkey_s_c bch2_btree_path_peek_slot(struct btree_path *, struct bkey *);

/*
 * bch2_btree_path_peek_slot() for a cached iterator might return a key in a
 * different snapshot:
 */
// 备注：缓存迭代器的 bch2_btree_path_peek_slot() 可能会返回不同快照中的键:
static inline struct bkey_s_c bch2_btree_path_peek_slot_exact(struct btree_path *path, struct bkey *u)
{
	struct bkey_s_c k = bch2_btree_path_peek_slot(path, u);

	if (k.k && bpos_eq(path->pos, k.k->p))
		return k;

	bkey_init(u);
	u->p = path->pos;
	return (struct bkey_s_c) { u, NULL };
}

void bch2_btree_path_level_init(struct btree_trans *, struct btree_path *, unsigned, struct btree *);

int __bch2_trans_mutex_lock(struct btree_trans *, struct mutex *);

static inline int bch2_trans_mutex_lock(struct btree_trans *trans, struct mutex *lock)
{
	return mutex_trylock(lock)
		? 0
		: __bch2_trans_mutex_lock(trans, lock);
}

/* Debug: */

void __bch2_trans_verify_paths(struct btree_trans *);
void __bch2_assert_pos_locked(struct btree_trans *, enum btree_id, struct bpos);

static inline void bch2_trans_verify_paths(struct btree_trans *trans)
{
	if (IS_ENABLED(CONFIG_BCACHEFS_DEBUG) &&
	    static_branch_unlikely(&bch2_debug_check_iterators))
		__bch2_trans_verify_paths(trans);
}

static inline void bch2_assert_pos_locked(struct btree_trans *trans, enum btree_id btree,
					  struct bpos pos)
{
	if (IS_ENABLED(CONFIG_BCACHEFS_DEBUG) &&
	    static_branch_unlikely(&bch2_debug_check_iterators))
		__bch2_assert_pos_locked(trans, btree, pos);
}

void bch2_btree_path_fix_key_modified(struct btree_trans *trans,
				      struct btree *, struct bkey_packed *);
void bch2_btree_node_iter_fix(struct btree_trans *trans, struct btree_path *,
			      struct btree *, struct btree_node_iter *,
			      struct bkey_packed *, unsigned, unsigned);

void bch2_path_put(struct btree_trans *, btree_path_idx_t, bool);

int __bch2_trans_relock(struct btree_trans *, bool);

static inline int bch2_trans_relock(struct btree_trans *trans)
{
	return trans->locked && !trans->restarted
		? 0
		: __bch2_trans_relock(trans, true);
}

int bch2_trans_relock(struct btree_trans *);
int bch2_trans_relock_notrace(struct btree_trans *);
void bch2_trans_unlock(struct btree_trans *);
void bch2_trans_unlock_long(struct btree_trans *);

/*
 * Returns the timeout to use for the next blocking wait done by this trans.
 *
 * The trans takes an SRCU read lock on bch2_trans_begin(); holding it across
 * a long blocking wait stalls memory reclaim. So if SRCU has been held for
 * longer than HZ when we're about to wait, drop it now (via unlock_long) and
 * let the caller wait the full timeout SRCU-free.
 *
 * Otherwise cap the returned timeout at the unconsumed portion of HZ — the
 * caller's wait will time out before SRCU has been held too long, and on
 * the next call (after the caller's natural retry) we'll be in the
 * elapsed >= HZ branch and drop SRCU here.
 */
static inline long bch2_trans_short_wait_budget(struct btree_trans *trans, long timeout)
{
	if (!trans || !trans->srcu_held)
		return timeout;

	long elapsed = jiffies - trans->srcu_lock_time;
	if (elapsed >= HZ) {
		bch2_trans_unlock_long(trans);
		return timeout;
	}
	return min(HZ - elapsed, timeout);
}

/*
 * SRCU-aware wrappers around closure_sync{,_timeout}: short waits under SRCU
 * until the budget is exhausted, then drop SRCU and wait the remainder.
 *
 * trans_closure_sync_timeout() returns 0 on success (closure drained),
 * -ETIME if timeout fully elapsed.
 */
static inline int trans_closure_sync_timeout(struct btree_trans *trans,
					     struct closure *cl,
					     long timeout)
{
	long remaining = timeout;

	while (remaining > 0) {
		if (closure_nr_remaining(cl) <= 1)
			return 0;

		unsigned long start = jiffies;
		long wait = bch2_trans_short_wait_budget(trans, remaining);

		if (!closure_sync_timeout(cl, wait))
			return 0;
		remaining -= (long)(jiffies - start);
	}
	return -ETIME;
}

static inline void trans_closure_sync(struct btree_trans *trans, struct closure *cl)
{
	trans_closure_sync_timeout(trans, cl, MAX_SCHEDULE_TIMEOUT);
}

/*
 * Wrappers around closure_wait_event{,_timeout} that drop SRCU before
 * blocking long. Use these instead of closure_wait_event() directly when a
 * btree_trans is in scope.
 *
 * Mirrors closure_wait_event_timeout(); the only change is that the inner
 * sync uses trans_closure_sync_timeout() so the SRCU short-wait budget is
 * honoured on each blocking pass.
 */
#define __trans_wait_event_timeout(_trans, waitlist, _cond, _until)		\
({										\
	CLASS(closure_stack, cl)();						\
	long _t;								\
										\
	while (1) {								\
		bch2_closure_wait(waitlist, &cl);				\
		if (_cond) {							\
			_t = max_t(long, 1L, _until - jiffies);			\
			break;							\
		}								\
		_t = max_t(long, 0L, _until - jiffies);				\
		if (!_t)							\
			break;							\
		trans_closure_sync_timeout(_trans, &cl, _t);			\
	}									\
	closure_wake_up(waitlist);						\
	_t;									\
})

#define trans_wait_event_timeout(_trans, waitlist, _cond, _timeout)		\
({										\
	unsigned long _until = jiffies + (_timeout);				\
	(_cond)									\
		? max_t(long, 1L, _until - jiffies)				\
		: __trans_wait_event_timeout(_trans, waitlist, _cond, _until);	\
})

#define trans_wait_event(_trans, _waitlist, _condition)				\
	trans_wait_event_timeout(_trans, _waitlist, _condition, MAX_SCHEDULE_TIMEOUT)

/*
 * SRCU-aware wrapper around wait_on_bit_io(): under SRCU, do bounded
 * io_schedule_timeout() waits; once the SRCU short-wait budget is
 * exhausted, drop SRCU and fall through to an unbounded io_schedule()
 * wait.
 */
static inline void trans_wait_on_bit_io(struct btree_trans *trans,
					unsigned long *word, int bit)
{
	while (test_bit_acquire(bit, word)) {
		long budget = bch2_trans_short_wait_budget(trans, MAX_SCHEDULE_TIMEOUT);

		if (budget == MAX_SCHEDULE_TIMEOUT)
			wait_on_bit_io(word, bit, TASK_UNINTERRUPTIBLE);
		else
			wait_on_bit_io_timeout(word, bit, TASK_UNINTERRUPTIBLE, budget);
	}
}

static inline int trans_was_restarted(struct btree_trans *trans, u32 restart_count)
{
	return restart_count != trans->restart_count
		? -BCH_ERR_transaction_restart_nested
		: 0;
}

void __noreturn bch2_trans_restart_error(struct btree_trans *, u32);

static inline void bch2_trans_verify_not_restarted(struct btree_trans *trans,
						   u32 restart_count)
{
	if (trans_was_restarted(trans, restart_count))
		bch2_trans_restart_error(trans, restart_count);
}

void __noreturn bch2_trans_unlocked_or_in_restart_error(struct btree_trans *);

static inline void bch2_trans_verify_not_unlocked_or_in_restart(struct btree_trans *trans)
{
#ifdef CONFIG_BCACHEFS_DEBUG
	if (trans->restarted || !trans->locked)
		bch2_trans_unlocked_or_in_restart_error(trans);
#endif
}

int bch2_trans_restart_foreign_task(struct btree_trans *, int, unsigned long);
int bch2_trans_restart_ip(struct btree_trans *, int, unsigned long);

__always_inline
static int btree_trans_restart(struct btree_trans *trans, int err)
{
	return bch2_trans_restart_ip(trans, err, _THIS_IP_);
}

static inline int trans_maybe_inject_restart(struct btree_trans *trans, unsigned long ip)
{
#ifdef CONFIG_BCACHEFS_INJECT_TRANSACTION_RESTARTS
	if (!(ktime_get_ns() & ~(~0ULL << min(63, (10 + trans->restart_count_this_trans))))) {
		event_inc_trace(trans->c, trans_restart_injected, buf, prt_str(&buf, trans->fn));
		return bch2_trans_restart_ip(trans,
					BCH_ERR_transaction_restart_fault_inject, ip);
	}
#endif
	return 0;
}

bool bch2_btree_node_upgrade(struct btree_trans *,
			     struct btree_path *, unsigned);

void __bch2_btree_path_downgrade(struct btree_trans *, struct btree_path *, unsigned);

static inline void bch2_btree_path_downgrade(struct btree_trans *trans,
					     struct btree_path *path)
{
	unsigned new_locks_want = path->level + !!path->intent_ref;

	if (path->locks_want > new_locks_want)
		__bch2_btree_path_downgrade(trans, path, new_locks_want);
}

void bch2_trans_downgrade(struct btree_trans *);

void bch2_trans_revalidate_updates_in_node(struct btree_trans *, struct btree *);
void bch2_trans_node_add(struct btree_trans *trans, struct btree *);
void bch2_trans_node_verify_not_in_iters(struct btree_trans *trans, struct btree *);
void bch2_trans_node_reinit_iter(struct btree_trans *, struct btree *);

int __must_check __bch2_btree_iter_traverse(struct btree_iter *iter);
int __must_check bch2_btree_iter_traverse(struct btree_iter *);

struct btree *bch2_btree_iter_peek_node(struct btree_iter *);

struct bkey_s_c bch2_btree_iter_peek_max(struct btree_iter *, const struct bpos *);
struct bkey_s_c bch2_btree_iter_next(struct btree_iter *);

static inline struct bkey_s_c bch2_btree_iter_peek(struct btree_iter *iter)
{
	return bch2_btree_iter_peek_max(iter, &SPOS_MAX);
}

struct bkey_s_c bch2_btree_iter_peek_prev_min(struct btree_iter *, struct bpos);

static inline struct bkey_s_c bch2_btree_iter_peek_prev(struct btree_iter *iter)
{
	return bch2_btree_iter_peek_prev_min(iter, POS_MIN);
}

struct bkey_s_c bch2_btree_iter_prev(struct btree_iter *);

struct bkey_s_c bch2_btree_iter_peek_slot(struct btree_iter *);
struct bkey_s_c bch2_btree_iter_next_slot(struct btree_iter *);
struct bkey_s_c bch2_btree_iter_prev_slot(struct btree_iter *);

bool bch2_btree_iter_advance(struct btree_iter *);
bool bch2_btree_iter_rewind(struct btree_iter *);

static inline void __bch2_btree_iter_set_pos(struct btree_iter *iter, struct bpos new_pos)
{
	iter->k.type = KEY_TYPE_deleted;
	iter->k.p.inode		= iter->pos.inode	= new_pos.inode;
	iter->k.p.offset	= iter->pos.offset	= new_pos.offset;
	iter->k.p.snapshot	= iter->pos.snapshot	= new_pos.snapshot;
	iter->k.size = 0;
}

// 备注：设置迭代的 pos
static inline void bch2_btree_iter_set_pos(struct btree_iter *iter, struct bpos new_pos)
{
	struct btree_trans *trans = iter->trans;

	if (unlikely(iter->update_path))
		bch2_path_put(trans, iter->update_path,
			      iter->flags & BTREE_ITER_intent);
	iter->update_path = 0;

	if (!(iter->flags & BTREE_ITER_all_snapshots))
		new_pos.snapshot = iter->snapshot;

	__bch2_btree_iter_set_pos(iter, new_pos);
}

static inline void bch2_btree_iter_set_pos_to_extent_start(struct btree_iter *iter)
{
	EBUG_ON(!(iter->flags & BTREE_ITER_is_extents));
	iter->pos = bkey_start_pos(&iter->k);
}

static inline void bch2_btree_iter_set_snapshot(struct btree_iter *iter, u32 snapshot)
{
	struct bpos pos = iter->pos;

	iter->snapshot = snapshot;
	pos.snapshot = snapshot;
	bch2_btree_iter_set_pos(iter, pos);
}

void bch2_trans_iter_exit(struct btree_iter *);

static inline bool btree_id_cached(enum btree_id btree)
{
	return BIT_ULL(btree) &
		(BIT_ULL(BTREE_ID_alloc)|
		 BIT_ULL(BTREE_ID_inodes)|
		 BIT_ULL(BTREE_ID_logged_ops)|
		 BIT_ULL(BTREE_ID_subvolumes));
}

static inline enum btree_iter_update_trigger_flags
bch2_btree_iter_flags(struct btree_trans *trans,
		      unsigned btree_id, unsigned level,
		      enum btree_iter_update_trigger_flags flags)
{
	if (level || !btree_id_cached(btree_id)) {
		flags &= ~BTREE_ITER_cached;
		flags &= ~BTREE_ITER_with_key_cache;
	} else if (!(flags & BTREE_ITER_cached))
		flags |= BTREE_ITER_with_key_cache;

	if (!(flags & (BTREE_ITER_all_snapshots|BTREE_ITER_not_extents)) &&
	    btree_id_is_extents(btree_id))
		flags |= BTREE_ITER_is_extents;

	if (!(flags & BTREE_ITER_snapshot_field) &&
	    !btree_type_has_snapshot_field(btree_id))
		flags &= ~BTREE_ITER_all_snapshots;

	if (!(flags & BTREE_ITER_all_snapshots) &&
	    btree_type_has_snapshots(btree_id))
		flags |= BTREE_ITER_filter_snapshots;

	if (trans->journal_replay_not_finished)
		flags |= BTREE_ITER_with_journal;

	return flags;
}

static inline void bch2_trans_iter_init_common(struct btree_trans *trans,
					  struct btree_iter *iter,
					  enum btree_id btree, struct bpos pos,
					  unsigned locks_want,
					  unsigned depth,
					  enum btree_iter_update_trigger_flags flags,
					  unsigned long ip)
{
	iter->trans		= trans;
	iter->update_path	= 0;
	iter->key_cache_path	= 0;
	// 备注：设置迭代器的 btree_id, 类型
	iter->btree_id		= btree;
	iter->min_depth		= 0;
	iter->flags		= flags;
	// 备注：设置迭代器的 pos
	iter->snapshot		= pos.snapshot;
	iter->pos		= pos;
	iter->k			= POS_KEY(pos);
	iter->journal_idx	= 0;
#ifdef CONFIG_BCACHEFS_DEBUG
	iter->ip_allocated = ip;
#endif
	iter->path = bch2_path_get(trans, btree, &iter->pos, locks_want, depth, flags, ip);
}

void bch2_trans_iter_init_outlined(struct btree_trans *, struct btree_iter *,
			  enum btree_id, struct bpos,
			  enum btree_iter_update_trigger_flags,
			  unsigned long ip);

static inline void __bch2_trans_iter_init(struct btree_trans *trans,
			  struct btree_iter *iter,
			  enum btree_id btree, struct bpos pos,
			  enum btree_iter_update_trigger_flags flags)
{
	if (__builtin_constant_p(btree) &&
	    __builtin_constant_p(flags))
		bch2_trans_iter_init_common(trans, iter, btree, pos, 0, 0,
				bch2_btree_iter_flags(trans, btree, 0, flags),
				_THIS_IP_);
	else
		bch2_trans_iter_init_outlined(trans, iter, btree, pos, flags, _THIS_IP_);
}

// 备注：初始化迭代器
static inline void bch2_trans_iter_init(struct btree_trans *trans,
			  struct btree_iter *iter,
			  enum btree_id btree, struct bpos pos,
			  enum btree_iter_update_trigger_flags flags)
{
	bch2_trans_iter_exit(iter);
	__bch2_trans_iter_init(trans, iter, btree, pos, flags);
}

#define DEFINE_CLASS2(_name, _type, _exit, _init, _init_args...)		\
typedef _type class_##_name##_t;					\
static __always_inline void class_##_name##_destructor(_type *p)			\
{ _type _T = *p; _exit; }						\
static __always_inline _type class_##_name##_constructor(_init_args)		\
{ _type t = _init; return t; }

#define bch2_trans_iter_class_init(_trans, _btree, _pos, _flags)		\
({										\
	struct btree_iter iter;							\
	__bch2_trans_iter_init(_trans, &iter, (_btree), (_pos), (_flags));	\
	iter;									\
})

DEFINE_CLASS2(btree_iter, struct btree_iter,
	     bch2_trans_iter_exit(&_T),
	     bch2_trans_iter_class_init(trans, btree, pos, flags),
	     struct btree_trans *trans,
	     enum btree_id btree, struct bpos pos,
	     enum btree_iter_update_trigger_flags flags);

#define bch2_trans_iter_uninit_class_init()					\
({										\
	struct btree_iter iter = {};						\
	iter;									\
})

DEFINE_CLASS(btree_iter_uninit, struct btree_iter,
	     bch2_trans_iter_exit(&_T),
	     bch2_trans_iter_uninit_class_init(),
	     struct btree_trans *trans)

void bch2_trans_copy_iter(struct btree_iter *, struct btree_iter *);

#define bch2_trans_iter_copy_class_init(_src)					\
({										\
	struct btree_iter iter = {};						\
	bch2_trans_copy_iter(&iter, _src);					\
	iter;									\
})

DEFINE_CLASS(btree_iter_copy, struct btree_iter,
	     bch2_trans_iter_exit(&_T),
	     bch2_trans_iter_copy_class_init(src),
	     struct btree_iter *src)

void __bch2_trans_node_iter_init(struct btree_trans *, struct btree_iter *,
				 enum btree_id, struct bpos,
				 unsigned, unsigned,
				 enum btree_iter_update_trigger_flags);

static inline void bch2_trans_node_iter_init(struct btree_trans *trans,
			       struct btree_iter *iter,
			       enum btree_id btree,
			       struct bpos pos,
			       unsigned locks_want,
			       unsigned depth,
			       enum btree_iter_update_trigger_flags flags)
{
	bch2_trans_iter_exit(iter);
	__bch2_trans_node_iter_init(trans, iter, btree, pos, locks_want, depth, flags);
}

#define bch2_trans_node_iter_class_init(_trans, _btree, _pos, _locks_want, _depth, _flags)\
({										\
	struct btree_iter iter;							\
	__bch2_trans_node_iter_init(_trans, &iter, (_btree), (_pos),		\
				    (_locks_want), (_depth), (_flags));		\
	iter;									\
})

DEFINE_CLASS(btree_node_iter, struct btree_iter,
	     bch2_trans_iter_exit(&_T),
	     bch2_trans_node_iter_class_init(trans, btree, pos, locks_want, depth, flags),
	     struct btree_trans *trans,
	     enum btree_id btree, struct bpos pos,
	     unsigned locks_want, unsigned depth,
	     enum btree_iter_update_trigger_flags flags);

void bch2_set_btree_iter_dontneed(struct btree_iter *);

#ifdef CONFIG_BCACHEFS_TRANS_KMALLOC_TRACE
void bch2_trans_kmalloc_trace_to_text(struct printbuf *,
				      darray_trans_kmalloc_trace *);
#endif

void *__bch2_trans_kmalloc(struct btree_trans *, size_t, unsigned long);

static inline void bch2_trans_kmalloc_trace(struct btree_trans *trans, size_t size,
					    unsigned long ip)
{
#ifdef CONFIG_BCACHEFS_TRANS_KMALLOC_TRACE
	darray_push(&trans->trans_kmalloc_trace,
		    ((struct trans_kmalloc_trace) { .ip = ip, .bytes = size }));
#endif
}

static __always_inline void *bch2_trans_kmalloc_nomemzero_ip(struct btree_trans *trans, size_t size,
						    unsigned long ip)
{
	size = roundup(size, 8);

	bch2_trans_kmalloc_trace(trans, size, ip);

	if (likely(trans->mem_top + size <= trans->mem_bytes)) {
		void *p = trans->mem + trans->mem_top;

		trans->mem_top += size;
		return p;
	} else {
		return __bch2_trans_kmalloc(trans, size, ip);
	}
}

static __always_inline void *bch2_trans_kmalloc_ip(struct btree_trans *trans, size_t size,
					  unsigned long ip)
{
	size = roundup(size, 8);

	bch2_trans_kmalloc_trace(trans, size, ip);

	if (likely(trans->mem_top + size <= trans->mem_bytes)) {
		void *p = trans->mem + trans->mem_top;

		trans->mem_top += size;
		memset(p, 0, size);
		return p;
	} else {
		return __bch2_trans_kmalloc(trans, size, ip);
	}
}

/**
 * bch2_trans_kmalloc - allocate memory for use by the current transaction
 *
 * Must be called after bch2_trans_begin, which on second and further calls
 * frees all memory allocated in this transaction
 */
static __always_inline void *bch2_trans_kmalloc(struct btree_trans *trans, size_t size)
{
	return bch2_trans_kmalloc_ip(trans, size, _THIS_IP_);
}

static __always_inline void *bch2_trans_kmalloc_nomemzero(struct btree_trans *trans, size_t size)
{
	return bch2_trans_kmalloc_nomemzero_ip(trans, size, _THIS_IP_);
}

static inline struct bkey_s_c __bch2_bkey_get_typed(struct btree_iter *iter,
						    enum bch_bkey_type type)
{
	struct bkey_s_c k = bch2_btree_iter_peek_slot(iter);

	if (!bkey_err(k) && type && k.k->type != type)
		k = bkey_s_c_err(bch_err_throw(iter->trans->c, ENOENT_bkey_type_mismatch));
	return k;
}

#define bch2_bkey_get_typed(_iter, _type)						\
	bkey_s_c_to_##_type(__bch2_bkey_get_typed(_iter, KEY_TYPE_##_type))

static inline void __bkey_val_copy_pad(void *dst_v, unsigned dst_size, struct bkey_s_c src_k)
{
	unsigned b = min_t(unsigned, dst_size, bkey_val_bytes(src_k.k));
	memcpy(dst_v, src_k.v, b);
	if (unlikely(b < dst_size))
		memset(dst_v + b, 0, dst_size - b);
}

#define bkey_val_copy_pad(_dst_v, _src_k)				\
do {									\
	BUILD_BUG_ON(!__typecheck(*_dst_v, *_src_k.v));			\
	__bkey_val_copy_pad(_dst_v, sizeof(*_dst_v), _src_k.s_c);	\
} while (0)

static inline int __bch2_bkey_get_val_typed(struct btree_trans *trans,
				enum btree_id btree, struct bpos pos,
				enum btree_iter_update_trigger_flags flags,
				enum bch_bkey_type type,
				unsigned val_size, void *val)
{
	CLASS(btree_iter, iter)(trans, btree, pos, flags);
	struct bkey_s_c k = __bch2_bkey_get_typed(&iter, type);
	int ret = bkey_err(k);
	if (!ret)
		__bkey_val_copy_pad(val, val_size, k);
	return ret;
}

#define bch2_bkey_get_val_typed(_trans, _btree_id, _pos, _flags, _type, _val)\
	__bch2_bkey_get_val_typed(_trans, _btree_id, _pos, _flags,	\
				  KEY_TYPE_##_type, sizeof(*_val), _val)

u32 bch2_trans_begin(struct btree_trans *);

// 备注：bch2_trans_node_iter_class_init
// 备注：bch2_trans_iter_exit
#define for_each_btree_node(_trans, _iter, _btree_id, _start,			\
			    _depth, _flags, _b, _do)				\
({										\
	bch2_trans_begin((_trans));						\
										\
	CLASS(btree_node_iter, _iter)((_trans), (_btree_id), _start,		\
				      0, _depth, _flags);			\
	struct btree *_b;							\
	int _ret3 = 0;								\
	do {									\
		u32 _restart_count = bch2_trans_begin((_trans));		\
										\
		_b = bch2_btree_iter_peek_node(&(_iter));			\
		_ret3 = PTR_ERR_OR_ZERO(_b);					\
		if (_ret3)							\
			continue; /* may be restart; re-evaluated below */	\
										\
		if (!_b)							\
			break;							\
										\
		_ret3 = (_do);							\
		if (_ret3)							\
			continue;						\
										\
		bch2_trans_verify_not_restarted((_trans), _restart_count);	\
										\
		if (bpos_eq((_b)->key.k.p, SPOS_MAX))				\
			break;							\
										\
		bch2_btree_iter_set_pos(&(_iter), bpos_successor((_b)->key.k.p));\
	} while (bch2_err_matches(_ret3, BCH_ERR_transaction_restart) ||	\
		 !_ret3);							\
										\
	_ret3;									\
})

static inline struct bkey_s_c bch2_btree_iter_peek_prev_type(struct btree_iter *iter,
							     enum btree_iter_update_trigger_flags flags)
{
	return  flags & BTREE_ITER_slots      ? bch2_btree_iter_peek_slot(iter) :
						bch2_btree_iter_peek_prev(iter);
}

static inline struct bkey_s_c bch2_btree_iter_peek_type(struct btree_iter *iter,
							enum btree_iter_update_trigger_flags flags)
{
	return  flags & BTREE_ITER_slots      ? bch2_btree_iter_peek_slot(iter) :
						bch2_btree_iter_peek(iter);
}

// 备注：迭代下一个键
static inline struct bkey_s_c bch2_btree_iter_peek_max_type(struct btree_iter *iter,
							    struct bpos end,
							    enum btree_iter_update_trigger_flags flags)
{
	if (!(flags & BTREE_ITER_slots))
		return bch2_btree_iter_peek_max(iter, &end);

	if (bkey_gt(iter->pos, end))
		return bkey_s_c_null;

	return bch2_btree_iter_peek_slot(iter);
}

int __bch2_btree_trans_too_many_iters(struct btree_trans *);

static inline int btree_trans_too_many_iters(struct btree_trans *trans)
{
	if (bitmap_weight(trans->paths_allocated, trans->nr_paths) > BTREE_ITER_NORMAL_LIMIT - 8)
		return __bch2_btree_trans_too_many_iters(trans);

	return 0;
}

/*
 * Loop-form, so that __cleanup/CLASS attributes on resources allocated inside
 * _do fire their cleanup on each restart iteration. Callers that need break/
 * continue inside _do to refer to an outer loop must open-code their own
 * restart loop instead of using lockrestart_do (see for_each_btree_key_*).
 */
// 备注：lockrestart_do - 事务核心宏：带自动重启的事务执行
// 备注：
// 备注：这是bcachefs事务系统的核心机制，实现乐观并发控制：
// 备注：- 首次执行或重启后调用 bch2_trans_begin() 初始化事务状态
// 备注：- 执行 _do 表达式(通常是B树操作)
// 备注：- 如果返回 RESTART 错误码，跳转到 transaction_restart 标签重试
// 备注：- 成功返回后验证事务未被意外重启
// 备注：
// 备注：为什么使用 goto 而不是循环:
// 备注：- 为了在 for_each_btree_key2() 等宏中 break/continue 能正确工作
// 备注：- 避免多层循环嵌套导致的控制流复杂
// 备注：
// 备注：典型使用模式:
// 备注：lockrestart_do(trans, {
// 备注：ret = some_btree_operation(trans);
// 备注：if (ret) return ret;
// 备注：ret = bch2_trans_commit(trans, ...);
// 备注：});
// 备注：
// 备注：事务重启的常见原因:
// 备注：- 锁获取失败(如 bch2_btree_node_lock_write 返回 RESTART)
// 备注：- btree节点被其他线程修改或分裂
// 备注：- 路径遍历失败需要重新traverse
#define lockrestart_do(_trans, _do)					\
({									\
	int _ret2;							\
	do {								\
		u32 _restart_count = bch2_trans_begin(_trans);		\
		_ret2 = (_do);						\
									\
		if (!_ret2)						\
			bch2_trans_verify_not_restarted(_trans, _restart_count);\
	} while (bch2_err_matches(_ret2, BCH_ERR_transaction_restart));	\
	_ret2;								\
})

/*
 * nested_lockrestart_do(), nested_commit_do():
 *
 * These are like lockrestart_do() and commit_do(), with two differences:
 *
 *  - We don't call bch2_trans_begin() unless we had a transaction restart
 *  - We return -BCH_ERR_transaction_restart_nested if we succeeded after a
 *  transaction restart
 */
#define nested_lockrestart_do(_trans, _do)				\
({									\
	u32 _restart_count, _orig_restart_count;			\
	int _ret2;							\
									\
	_restart_count = _orig_restart_count = (_trans)->restart_count;	\
									\
	while (bch2_err_matches(_ret2 = (_do), BCH_ERR_transaction_restart))\
		_restart_count = bch2_trans_begin(_trans);		\
									\
	if (!_ret2)							\
		bch2_trans_verify_not_restarted(_trans, _restart_count);\
									\
	_ret2 ?: trans_was_restarted(_trans, _orig_restart_count);	\
})

#define for_each_btree_key_max_continue(_trans, _iter,			\
					 _end, _flags, _k, _do)		\
({									\
	struct bkey_s_c _k;						\
	int _ret3 = 0;							\
									\
	do {								\
		u32 _restart_count = bch2_trans_begin(_trans);		\
		_ret3 = 0;						\
									\
		(_k) = bch2_btree_iter_peek_max_type(&(_iter),		\
						_end, (_flags));	\
		if (!(_k).k)						\
			break;						\
									\
		_ret3 = bkey_err(_k) ?: (_do);				\
		if (!_ret3)						\
			bch2_trans_verify_not_restarted(_trans, _restart_count);\
	} while (bch2_err_matches(_ret3, BCH_ERR_transaction_restart) ||\
		 (!_ret3 && bch2_btree_iter_advance(&(_iter))));	\
									\
	_ret3;								\
})

#define for_each_btree_key_continue(_trans, _iter, _flags, _k, _do)	\
	for_each_btree_key_max_continue(_trans, _iter, SPOS_MAX, _flags, _k, _do)

#define for_each_btree_key_max(_trans, _iter, _btree_id,			\
				_start, _end, _flags, _k, _do)			\
({										\
	bch2_trans_begin(trans);						\
										\
	CLASS(btree_iter, _iter)((_trans), (_btree_id), (_start), (_flags));	\
	for_each_btree_key_max_continue(_trans, _iter, _end, _flags, _k, _do);	\
})

// 备注：遍历 btree 的每一个键
#define for_each_btree_key(_trans, _iter, _btree_id, _start, _flags, _k, _do)	\
	for_each_btree_key_max(_trans, _iter, _btree_id, _start, SPOS_MAX, _flags, _k, _do)

#define for_each_btree_key_reverse(_trans, _iter, _btree_id,			\
				   _start, _flags, _k, _do)			\
({										\
	int _ret3 = 0;								\
										\
	CLASS(btree_iter, iter)((_trans), (_btree_id), (_start), (_flags));	\
										\
	do {									\
		u32 _restart_count = bch2_trans_begin(_trans);			\
		_ret3 = 0;							\
										\
		struct bkey_s_c _k =						\
			bch2_btree_iter_peek_prev_type(&(_iter), (_flags));	\
		if (!(_k).k)							\
			break;							\
										\
		_ret3 = bkey_err(_k) ?: (_do);					\
		if (!_ret3)							\
			bch2_trans_verify_not_restarted(_trans, _restart_count);\
	} while (bch2_err_matches(_ret3, BCH_ERR_transaction_restart) ||	\
		 (!_ret3 && bch2_btree_iter_rewind(&(_iter))));			\
										\
	_ret3;									\
})

#define for_each_btree_key_commit(_trans, _iter, _btree_id,		\
				  _start, _iter_flags, _k,		\
				  _disk_res, _journal_seq, _commit_flags,\
				  _do)					\
	for_each_btree_key(_trans, _iter, _btree_id, _start, _iter_flags, _k,\
			    (_do) ?: bch2_trans_commit(_trans, (_disk_res),\
					(_journal_seq), (_commit_flags)))

#define for_each_btree_key_reverse_commit(_trans, _iter, _btree_id,	\
				  _start, _iter_flags, _k,		\
				  _disk_res, _journal_seq, _commit_flags,\
				  _do)					\
	for_each_btree_key_reverse(_trans, _iter, _btree_id, _start, _iter_flags, _k,\
			    (_do) ?: bch2_trans_commit(_trans, (_disk_res),\
					(_journal_seq), (_commit_flags)))

#define for_each_btree_key_max_commit(_trans, _iter, _btree_id,	\
				  _start, _end, _iter_flags, _k,	\
				  _disk_res, _journal_seq, _commit_flags,\
				  _do)					\
	for_each_btree_key_max(_trans, _iter, _btree_id, _start, _end, _iter_flags, _k,\
			    (_do) ?: bch2_trans_commit(_trans, (_disk_res),\
					(_journal_seq), (_commit_flags)))

struct bkey_s_c bch2_btree_iter_peek_and_restart_outlined(struct btree_iter *);

struct bkey_s_c bch2_btree_iter_peek_root(struct btree_trans *, struct btree_iter *,
					  enum btree_id, unsigned);

#define for_btree_root_key_at_level(_trans, _iter, _btree_id, _level, _k, _do)		\
	lockrestart_do(trans, ({							\
		CLASS(btree_iter_uninit, iter)(trans);					\
		struct bkey_s_c _k;							\
		bkey_err(k = bch2_btree_iter_peek_root(_trans, &_iter,			\
						       _btree_id, level)) ?:		\
		(k.k ? _do : 0);							\
	}))

#define for_each_btree_key_max_norestart(_trans, _iter, _btree_id,			\
			   _start, _end, _flags, _k, _ret)				\
	for (CLASS(btree_iter, _iter)((_trans), (_btree_id), (_start), (_flags));	\
	     (_k) = bch2_btree_iter_peek_max_type(&(_iter), _end, _flags),		\
	     !((_ret) = bkey_err(_k)) && (_k).k;					\
	     bch2_btree_iter_advance(&(_iter)))

#define for_each_btree_key_norestart(_trans, _iter, _btree_id,				\
				     _start, _flags, _k, _ret)				\
	for_each_btree_key_max_norestart(_trans, _iter, _btree_id, _start,		\
					  SPOS_MAX, _flags, _k, _ret)

#define for_each_btree_key_max_continue_norestart(_iter, _end, _flags, _k, _ret)	\
	for (;										\
	     (_k) = bch2_btree_iter_peek_max_type(&(_iter), _end, _flags),		\
	     !((_ret) = bkey_err(_k)) && (_k).k;					\
	     bch2_btree_iter_advance(&(_iter)))

#define for_each_btree_key_continue_norestart(_iter, _flags, _k, _ret)			\
	for_each_btree_key_max_continue_norestart(_iter, SPOS_MAX, _flags, _k, _ret)

#define for_each_btree_key_reverse_norestart(_trans, _iter, _btree_id,			\
					     _start, _flags, _k, _ret)			\
	for (CLASS(btree_iter, _iter)((_trans), (_btree_id),				\
				      (_start), (_flags));				\
	     (_k) = bch2_btree_iter_peek_prev_type(&(_iter), _flags),			\
	     !((_ret) = bkey_err(_k)) && (_k).k;					\
	     bch2_btree_iter_rewind(&(_iter)))

/*
 * This should not be used in a fastpath, without first trying _do in
 * nonblocking mode - it will cause excessive transaction restarts and
 * potentially livelocking:
 */
#define drop_locks_do(_trans, _do)					\
({									\
	bch2_trans_unlock(_trans);					\
	(_do) ?: bch2_trans_relock(_trans);				\
})

#define allocate_dropping_locks_errcode(_trans, _do)			\
({									\
	gfp_t _gfp = GFP_NOWAIT;					\
	int _ret = _do;							\
									\
	if (bch2_err_matches(_ret, ENOMEM)) {				\
		_gfp = GFP_KERNEL;					\
		_ret = drop_locks_do(_trans, _do);			\
	}								\
	_ret;								\
})

#define allocate_dropping_locks(_trans, _ret, _do)			\
({									\
	gfp_t _gfp = GFP_NOWAIT;					\
	typeof(_do) _p = _do;						\
									\
	_ret = 0;							\
	if (unlikely(!_p)) {						\
		_gfp = GFP_KERNEL;					\
		_ret = drop_locks_do(_trans, ((_p = _do), 0));		\
	}								\
	_p;								\
})

#define allocate_dropping_locks_norelock(_trans, _lock_dropped, _do)	\
({									\
	gfp_t _gfp = GFP_NOWAIT;					\
	typeof(_do) _p = _do;						\
	_lock_dropped = false;						\
	if (unlikely(!_p)) {						\
		bch2_trans_unlock(_trans);				\
		_lock_dropped = true;					\
		_gfp = GFP_KERNEL;					\
		_p = _do;						\
	}								\
	_p;								\
})

struct btree_trans *__bch2_trans_get(struct bch_fs *, unsigned);
void bch2_trans_put(struct btree_trans *);

bool bch2_current_has_btree_trans(struct bch_fs *);

extern const char *bch2_btree_transaction_fns[BCH_TRANSACTIONS_NR];
unsigned bch2_trans_get_fn_idx(const char *);

#define bch2_trans_get(_c)						\
({									\
	static unsigned trans_fn_idx;					\
									\
	if (unlikely(!trans_fn_idx))					\
		trans_fn_idx = bch2_trans_get_fn_idx(__func__);		\
	__bch2_trans_get(_c, trans_fn_idx);				\
})

/*
 * We don't use DEFINE_CLASS() because using a function for the constructor
 * breaks bch2_trans_get()'s use of __func__
 */
typedef struct btree_trans * class_btree_trans_t;
static inline void class_btree_trans_destructor(struct btree_trans **p)
{
	struct btree_trans *trans = *p;
	bch2_trans_put(trans);
}

#define class_btree_trans_constructor(_c)	bch2_trans_get(_c)

// 备注：============================================================================
// 备注：事务读写操作数据流示例
// 备注：============================================================================
// 备注：
// 备注：【读操作流程】
// 备注：--------------------------------
// 备注：  // 1. 获取事务(使用 CLASS 宏自动管理生命周期)
// 备注：  CLASS(btree_trans, trans)(c);
// 备注：
// 备注：  // 2. 初始化迭代器，定位到目标位置
// 备注：  struct btree_iter iter;
// 备注：  bch2_trans_iter_init(trans, &iter, BTREE_ID_extents,
// 备注：                       POS(inode, offset), BTREE_ITER_intent);
// 备注：
// 备注：  // 3. 读取键值(自动遍历 btree 节点)
// 备注：  struct bkey_s_c k = bch2_btree_iter_peek(&iter);
// 备注：  if (k.k) {
// 备注：      // 使用键值数据...
// 备注：  }
// 备注：
// 备注：  // 4. 清理(CLASS宏自动调用 bch2_trans_put)
// 备注：  bch2_trans_iter_exit(&iter);
// 备注：
// 备注：
// 备注：【写操作流程】
// 备注：--------------------------------
// 备注：  // 使用 lockrestart_do 宏包装，自动处理事务重启
// 备注：  lockrestart_do(trans, {
// 备注：      struct btree_iter iter;
// 备注：      struct bkey_i *new_k;
// 备注：      int ret = 0;
// 备注：
// 备注：      // 1. 初始化迭代器(带 intent 标志，表示可能修改)
// 备注：      bch2_trans_iter_init(trans, &iter, BTREE_ID_extents,
// 备注：                           POS(inode, offset),
// 备注：                           BTREE_ITER_intent|BTREE_ITER_is_extents);
// 备注：
// 备注：      // 2. 读取现有键值(用于判断覆盖或插入)
// 备注：      struct bkey_s_c old = bch2_btree_iter_peek(&iter);
// 备注：
// 备注：      // 3. 在事务内存中分配新键值空间
// 备注：      new_k = bch2_trans_kmalloc(trans, sizeof(*new_k) + val_size);
// 备注：      if (IS_ERR(new_k)) {
// 备注：          ret = PTR_ERR(new_k);
// 备注：          goto err;
// 备注：      }
// 备注：
// 备注：      // 4. 填充新键值数据
// 备注：      bkey_init(&new_k->k);
// 备注：      new_k->k.p = iter.pos;
// 备注：      new_k->k.type = KEY_TYPE_extent;
// 备注：      // ... 填充值数据 ...
// 备注：
// 备注：      // 5. 将更新添加到事务队列(此时还未写入 btree)
// 备注：      ret = bch2_trans_update(trans, &iter, new_k, 0);
// 备注：      if (ret)
// 备注：          goto err;
// 备注：
// 备注：      // 6. 提交事务(原子性应用所有更新)
// 备注：      ret = bch2_trans_commit(trans, disk_res, &journal_seq, 0);
// 备注：      // 如返回 RESTART 错误，lockrestart_do 自动重试
// 备注：
// 备注：  err:
// 备注：      bch2_trans_iter_exit(&iter);
// 备注：      ret;
// 备注：  });
// 备注：
// 备注：
// 备注：【事务重启场景】
// 备注：--------------------------------
// 备注：以下情况会触发事务重启(返回 -BCH_ERR_transaction_restart):
// 备注：
// 备注：1. 锁冲突: 尝试锁定 btree 节点时失败
// 备注：   -> 其他线程持有冲突锁
// 备注：   -> 节点正在被写入磁盘
// 备注：
// 备注：2. 节点变化: 遍历过程中 btree 节点被修改
// 备注：   -> 节点分裂(需要重新遍历路径)
// 备注：   -> 节点被回收或驱逐
// 备注：
// 备注：3. 资源不足: 无法立即获取所需资源
// 备注：   -> 内存分配失败(GFP_NOWAIT 模式下)
// 备注：   -> 日志空间不足
// 备注：
// 备注：4. 版本冲突: 乐观锁验证失败
// 备注：   -> 读取的旧键值在提交前已被其他事务修改
// 备注：
// 备注：重启流程:
// 备注：  lockrestart_do 捕获 RESTART 错误
// 备注：      -> 调用 bch2_trans_begin() 重置事务状态
// 备注：      -> 释放所有锁和临时资源
// 备注：      -> 跳转到 transaction_restart 标签重试
// 备注：      -> 重新执行整个操作(包括重新遍历 btree)
// 备注：
// 备注：
// 备注：【批量扫描操作】
// 备注：--------------------------------
// 备注：  // 使用 for_each_btree_key 宏自动处理事务重启
// 备注：  for_each_btree_key(trans, iter, BTREE_ID_extents,
// 备注：                     POS_MIN, BTREE_ITER_prefetch, k, {
// 备注：      // 处理每个键值 k
// 备注：      // 如需要修改，添加更新到事务
// 备注：      // 定期调用 bch2_trans_commit() 提交批次
// 备注：  });
// 备注：
// 备注：============================================================================

/* deprecated, prefer CLASS(btree_trans) */
// 备注：事务包
#define bch2_trans_run(_c, _do)						\
({									\
	CLASS(btree_trans, trans)(_c);					\
	(_do);								\
})

/* deprecated, prefer CLASS(btree_trans) */
#define bch2_trans_do(_c, _do)						\
({									\
	CLASS(btree_trans, trans)(_c);					\
	lockrestart_do(trans, _do);					\
})

void bch2_btree_trans_to_text(struct printbuf *, struct btree_trans *);

void bch2_fs_btree_iter_exit(struct bch_fs *);
void bch2_fs_btree_iter_init_early(struct bch_fs *);
int bch2_fs_btree_iter_init(struct bch_fs *);

#endif /* _BCACHEFS_BTREE_ITER_H */
