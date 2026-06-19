/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCACHEFS_BTREE_LOCKING_TYPES_H
#define _BCACHEFS_BTREE_LOCKING_TYPES_H

#include "util/darray.h"
#include "util/six.h"
#include "btree/types.h"

/* State used for the cycle detector */

/*
 * @trans wants to lock @b with type @type
 */

// 备注：备注：trans_waiting_for_lock - 锁等待图的一个节点（DFS 栈帧）
// 备注：备注：
// 备注：备注：@trans 当前正在等待获取 @node_want 上的 @lock_want 类型锁。
// 备注：备注：
// 备注：备注：DFS 遍历过程：
// 备注：备注：  1. lock_graph_down() 创建栈帧：记录当前事务等待的锁（node_want）
// 备注：备注：  2. 遍历 @path_idx 开始的路径，检查每一层持有的锁（node_have）
// 备注：备注：  3. 对每个 node_have，扫描其 wait_fifo 找出冲突的事务
// 备注：备注：  4. 冲突者加入 waitlist，然后递归（lock_graph_descend）
// 备注：备注：
// 备注：备注：waitlist 是冲突事务的快照（通过 RCU 无锁遍历拍下），
// 备注：备注：并发唤醒不会影响已拍下的快照，保证 DFS 遍历的一致性。
// 备注：备注：预分配 16 个槽位，超出则触发 waitlist_alloc_failed 路径。
struct trans_waiting_for_lock {
	// 备注：当前栈帧对应的事务
	struct btree_trans		*trans;

	// 备注：@trans 正在等待的 btree 节点（想要获取的锁的目标）
	struct btree_bkey_cached_common	*node_want;

	// 备注：@trans 想要获取的锁类型（SIX_LOCK_read/intent/write）
	enum six_lock_type		lock_want:8;

	/* for iterating over held locks :*/
	// 备注：当前正在检查的 btree 层级（从 0 到 BTREE_MAX_DEPTH 遍历）
	u8				level;

	// 备注：当前正在检查的路径索引（遍历 trans->paths 数组）
	// 备注：【多路径断点续扫】
	// 备注：事务有多个 btree_path（如同时操作 extent 和 inode），
	// 备注：path_idx 记录本帧上次扫描到的位置。每次重新进入阶段 2
	// 备注：时从 path_idx 而非 0 开始，避免重复扫描已完成的路径。
	// 备注：当全部路径扫描完毕（无 waitlist 新增）→ 帧回溯（up:)。
	btree_path_idx_t		path_idx;

	// 备注：waitlist 中下一个要递归的事务索引
	u16				waitlist_idx;

	// 备注：@trans 当前持有的 btree 节点（与遍历到的路径层级对应）
	// 备注：这个节点上的 wait_fifo 将被扫描找出冲突者
	struct btree_bkey_cached_common	*node_have;

	/*
	 * Conflicting waiters we found on the lock at (@path_idx, @level),
	 * cached at snapshot time so iteration is stable against concurrent
	 * wakeup activity. @waitlist_idx is the next entry to descend into.
	 */
	// 备注：备注：在 (path_idx, level) 处找到的冲突事务快照
	// 备注：备注：在 guard(rcu) + guard(preempt) 下无锁扫描 wait_fifo，
	// 备注：备注：事务指针拍入此数组后，后续的 DFS 递归从此读取而非重新扫描，
	// 备注：备注：避免因并发唤醒导致遍历过程中依赖链断裂。
	DARRAY_PREALLOCATED(struct btree_trans *, 16) waitlist;
};

// 备注：备注：lock_graph - 锁等待图（per-CPU DFS 栈）
// 备注：备注：
// 备注：备注：DFS 栈深度最大 8 层（g[8]），超出触发 lock_graph_recursion_limit 路径。
// 备注：备注：per-CPU 设计（DEFINE_PER_CPU），无需跨 CPU 同步。
// 备注：备注：8 层限制的经验依据：btree 最大深度通常不超过 5（扇出 ~128），
// 备注：备注：加上少数额外的锁依赖跳转，8 层足够覆盖所有实际死锁场景。
struct lock_graph {
	// 备注：DFS 栈帧数组（固定大小，无动态分配）
	struct trans_waiting_for_lock	g[8];

	// 备注：当前 DFS 栈深度（0=空，1=根事务，2+=递归深度）
	unsigned			nr;

	// 备注：是否已打印链信息（避免重复打印调试输出）
	bool				printed_chain;
};

#endif /* _BCACHEFS_BTREE_LOCKING_TYPES_H */
