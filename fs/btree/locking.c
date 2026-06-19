// SPDX-License-Identifier: GPL-2.0

/* DOC_LATEX(btree-locking)
 *
 * Bcachefs uses SIX locks (shared, intent, exclusive) for btree nodes rather
 * than traditional read/write locks. The three states are:
 *
 * \begin{itemize}
 * \item \textbf{Shared}: Does not conflict with other shared locks (like a read lock)
 * \item \textbf{Intent}: Conflicts with other intent locks but not shared locks
 * \item \textbf{Exclusive}: Conflicts with everything (like a write lock)
 * \end{itemize}
 *
 * \paragraph{Why intent locks?}
 *
 * With a regular read/write lock, a read lock cannot be upgraded to a write
 * lock---that leads to deadlock when multiple threads with read locks try to
 * upgrade simultaneously. With complicated data structures like btrees, updates
 * often need to hold write locks for exclusion with other updates for much
 * longer than the part where they actually modify data that needs exclusion
 * from readers.
 *
 * Consider a btree node split. The update starts at a leaf node and discovers
 * it needs to split. Before starting the split, it must acquire a write lock
 * on the parent node---primarily to avoid deadlocking with other splits. It
 * needs at least a read lock on the parent to lock the path to the child node,
 * but it cannot upgrade that read lock to a write lock (to update the parent
 * with pointers to the new children) because that would deadlock with threads
 * splitting sibling leaf nodes.
 *
 * Intent locks solve this. When doing a split, we acquire an intent lock on
 * the parent---exclusive locks (for the actual in-memory modification) are
 * only ever held while modifying in-memory btree contents, which is a much
 * shorter duration than the entire split operation (which requires waiting for
 * new nodes to be written to disk). Readers can continue accessing the parent
 * throughout the split; only the final pointer update requires exclusive
 * access.
 *
 * \paragraph{Parent-child ordering}
 *
 * Intent locks with only three states do introduce another potential deadlock:
 *
 * \begin{verbatim}
 *     Thread A                        Thread B
 *     read            | Parent |      intent
 *     intent          | Child  |      intent
 * \end{verbatim}
 *
 * Thread B is splitting the child node: it has allocated new nodes and written
 * them out, and now needs an exclusive lock on the parent to add the new
 * pointers (after which it will free the old child). Thread A just wants to
 * insert into the child---it has a read lock on the parent, has looked up the
 * child node, and is waiting on thread B to get an intent lock on the child.
 *
 * But thread A has blocked thread B from taking its exclusive lock on the
 * parent, and thread B cannot drop its intent lock on the child until after
 * the new nodes are visible and the old child is freed.
 *
 * The solution: we drop read locks on parent nodes \emph{before} taking intent
 * locks on child nodes. This might cause us to race with the node being freed,
 * so after grabbing the intent lock we verify the node is still valid and redo
 * the traversal if necessary.
 *
 * \paragraph{Sequence numbers and optimistic relocking}
 *
 * SIX locks include embedded sequence numbers, incremented when taking and
 * releasing exclusive locks (much like seqlocks). This allows us to
 * aggressively drop locks---we can usually retake the lock by checking the
 * sequence number rather than redoing the full btree traversal. We also use
 * this for \texttt{try\_upgrade()}: if we discover we need an intent lock (e.g.
 * for a split, or because the caller is inserting into a leaf node they did
 * not get an intent lock for), we can often upgrade without unwinding and
 * redoing the traversal.
 *
 * \paragraph{Cycle detection}
 *
 * Bcachefs uses database-style cycle detection to avoid deadlocks entirely.
 * Before a transaction sleeps waiting on a contended lock, it invokes
 * \texttt{bch2\_check\_for\_deadlock()}, which walks the graph of transactions
 * waiting on locks. The algorithm follows the chain of dependencies: for each
 * lock a transaction holds, check if any other transaction is waiting on that
 * lock; if so, recursively check what locks \emph{that} transaction holds, and
 * so on.
 *
 * If the walk returns to the original transaction, a cycle exists. One
 * transaction in the cycle is selected to abort: it releases all its locks and
 * restarts from the beginning. The transaction layer is designed so that all
 * operations are idempotent and can be safely restarted at any point.
 *
 * This approach eliminates deadlocks entirely and keeps worst-case latency
 * bounded, at the cost of requiring restartable transactions. The same
 * restart infrastructure also provides crash resilience: since every operation
 * can be interrupted and restarted, the filesystem is inherently resilient to
 * interruption at any point---including during recovery itself.
 *
 * The cycle detector runs only when a transaction would block, so it adds no
 * overhead to the fast path. When cycles are detected, they are broken
 * immediately rather than timing out, keeping latency predictable.
 */

#include "bcachefs.h"

#include "btree/bbpos.h"
#include "btree/cache.h"
#include "btree/locking.h"
#include "btree/write.h"

#include "sb/counters.h"

static struct lock_class_key bch2_btree_node_lock_key;

DEFINE_PER_CPU(struct lock_graph, bch2_lock_graph);

void bch2_lock_graph_init_one(struct lock_graph *g)
{
	for (unsigned i = 0; i < ARRAY_SIZE(g->g); i++)
		darray_init(&g->g[i].waitlist);
}

void bch2_lock_graph_exit_one(struct lock_graph *g)
{
	for (unsigned i = 0; i < ARRAY_SIZE(g->g); i++)
		darray_exit(&g->g[i].waitlist);
}

int bch2_lock_graph_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		bch2_lock_graph_init_one(per_cpu_ptr(&bch2_lock_graph, cpu));
	return 0;
}

void bch2_lock_graph_exit(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		bch2_lock_graph_exit_one(per_cpu_ptr(&bch2_lock_graph, cpu));
}

void bch2_btree_lock_init(struct btree_bkey_cached_common *b,
			  enum six_lock_init_flags flags,
			  gfp_t gfp)
{
	__six_lock_init(&b->lock, "b->c.lock", &bch2_btree_node_lock_key, flags, gfp);
	lockdep_set_notrack_class(&b->lock);
}

/* Btree node locking: */

struct six_lock_count bch2_btree_node_lock_counts(struct btree_trans *trans,
						  struct btree_path *skip,
						  struct btree_bkey_cached_common *b,
						  unsigned level)
{
	struct btree_path *path;
	struct six_lock_count ret;
	unsigned i;

	memset(&ret, 0, sizeof(ret));

	if (IS_ERR_OR_NULL(b))
		return ret;

	trans_for_each_path(trans, path, i)
		if (path != skip && &path->l[level].b->c == b) {
			int t = btree_node_locked_type(path, level);

			if (t != BTREE_NODE_UNLOCKED)
				ret.n[t]++;
		}

	return ret;
}

/* unlock */

void bch2_btree_node_unlock_write(struct btree_trans *trans,
			struct btree_path *path, struct btree *b)
{
	bch2_btree_node_unlock_write_inlined(trans, path, b);
}

/* lock */

static noinline void print_cycle(struct printbuf *out, struct lock_graph *g)
{
	struct trans_waiting_for_lock *i;

	prt_printf(out, "Found lock cycle (%u entries):\n", g->nr);

	for (i = g->g; i < g->g + g->nr; i++) {
		struct task_struct *task = READ_ONCE(i->trans->locking_wait.task);
		if (!task)
			continue;

		bch2_btree_trans_to_text(out, i->trans);
		bch2_prt_task_backtrace(out, task, i == g->g ? 5 : 1, GFP_NOWAIT);
	}
}

static noinline void print_chain(struct printbuf *out, struct lock_graph *g)
{
	if (g->printed_chain || g->nr <= 1)
		return;
	g->printed_chain = true;

	struct trans_waiting_for_lock *i;

	for (i = g->g; i != g->g + g->nr; i++) {
		struct task_struct *task = READ_ONCE(i->trans->locking_wait.task);
		if (i != g->g)
			prt_str(out, "<- ");
		prt_printf(out, "%u ", task ? task->pid : 0);
	}
	prt_newline(out);
}

static void lock_graph_pop_all(struct lock_graph *g)
{
	g->nr = 0;
}

static noinline void lock_graph_pop_from(struct lock_graph *g, struct trans_waiting_for_lock *i)
{
	g->nr = i - g->g;
}

// 备注：lock_graph_down - 将事务入栈（DFS 向下推进一层）
// 备注：
// 备注：在新的栈帧中记录事务正在等待的锁信息：
// 备注：  - trans: 等待者事务
// 备注：  - node_want: 它想要的 btree 节点
// 备注：  - lock_want: 它想要的锁类型
// 备注：  - level/path_idx/waitlist 归零：开始遍历其持有锁
// 备注：
// 备注：栈帧间连续性检查：如果顶层帧的 node_want 不等于下一个帧的 node_have，
// 备注：说明两个帧之间的锁依赖链已经断裂（中间状态变化了），
// 备注：直接退栈（--g->nr）消除断裂的帧。
// 备注：
// 备注：字段逐个初始化而非聚合赋值：复用之前 walk 已分配的 heap buffer
// 备注：（waitlist 的 data/size 保留，walk 间重用避免重复 malloc）
static void lock_graph_down(struct lock_graph *g, struct btree_trans *trans)
{
	/*
	 * Field-by-field init rather than aggregate: we keep waitlist_snap's
	 * data/size across walks so any grown heap buffer is reused.
	 */
	struct trans_waiting_for_lock *top = &g->g[g->nr++];

	top->trans			= trans;
	top->node_want			= trans->locking;
	top->lock_want			= READ_ONCE(trans->locking_wait.lock_want);
	top->level			= 0;
	top->path_idx			= 0;
	top->waitlist_idx		= 0;
	top->node_have			= NULL;
	top->waitlist.nr		= 0;

	g->printed_chain = false;

	if (unlikely(top > g->g &&
		     top->node_want != top[-1].node_have))
		--g->nr;
}

// 备注：lock_graph_remove_non_waiters - 验证已构建的锁依赖链是否仍然有效
// 备注：
// 备注：在从 @from 帧向下递归的间隙中，@from 对应的事务可能已经获取到了锁并
// 备注：继续执行了（甚至可能阻塞在另一个完全不同的锁上）。我们需要验证链仍然有效。
// 备注：
// 备注：两种过期检测（逐帧检查）：
// 备注：  1. @from->trans->locking != @from->node_want
// 备注：     事务不再等待之前记录的节点了（它可能已经得到了锁）
// 备注：  2. i[0].node_have != i[1].node_want
// 备注：     父帧不再持有子帧等待的节点了（父帧的路径已经变化）
// 备注：
// 备注：任一条件满足 → 从过期帧位置 pop → 调用者重试
/*
 * Revalidate the "who is blocked on whom" chain we've built up in @g before
 * acting on a suspected cycle.
 *
 * Between the time we descended into frame @i and now, @i's trans could have
 * acquired its lock and moved on (possibly blocked waiting on something else
 * entirely). Two staleness checks, per frame:
 *
 *   - @from->trans->locking != @from->node_want
 *     @from's trans is no longer waiting for the node we recorded at descent.
 *
 *   - i[0].node_have != i[1].node_want
 *     The parent frame is no longer looking at the node the child frame was
 *     blocked on - the edge we built between them is stale.
 *
 * Either makes the cycle hypothesis invalid; pop from the stale frame down
 * and let the caller retry.
 */
static bool lock_graph_remove_non_waiters(struct lock_graph *g,
					  struct trans_waiting_for_lock *from)
{
	struct trans_waiting_for_lock *i;

	if (from->trans->locking != from->node_want) {
		lock_graph_pop_from(g, from);
		return true;
	}

	for (i = from ; i + 1 < g->g + g->nr; i++)
		if (i[0].node_have != i[1].node_want) {
			lock_graph_pop_from(g, i + 1);
			return true;
		}

	return false;
}

static void trace_would_deadlock(struct lock_graph *g, struct btree_trans *trans)
{
	event_inc_trace(trans->c, trans_restart_would_deadlock, buf, ({
		guard(printbuf_atomic)(&buf);
		prt_printf(&buf, "%s\n", trans->fn);
		print_cycle(&buf, g);
	}));
}

// 备注：wake_up_trans - 唤醒阻塞在锁等待中的事务
// 备注：通过 closure_get_not_zero 检查事务是否仍然存活，
// 备注：然后 wake_up_process 唤醒其等待任务。
// 备注：closure_put 释放引用计数（与 get_not_zero 配对）。
static void wake_up_trans(struct btree_trans *trans)
{
	if (closure_get_not_zero(&trans->ref)) {
		wake_up_process(trans->locking_wait.task);
		closure_put(&trans->ref);
	}
}

// 备注：abort_lock - 中止环中的指定事务以打破死锁
// 备注：
// 备注：根据牺牲者在环中的位置有两种处理方式：
// 备注：
// 备注：  1. 牺牲者是当前事务（i == g->g）：
// 备注：     当前运行死锁检测器的事务就是最应该中止的。
// 备注：     调用 bch2_trans_restart_foreign_task() 返回 RESTART 错误码，
// 备注：     让当前事务重启（释放锁、重做遍历）。
// 备注：     注意：函数名中的 "foreign" 指的是"中止的任务是自己"这个事实与
// 备注：     "foreign" 原意不完全一致，但接口是同一套。
// 备注：
// 备注：  2. 牺牲者是其他事务（i > g->g）：
// 备注：     设置其 lock_must_abort = true（标记强制中止），
// 备注：     然后唤醒其任务。被唤醒的事务在 six_lock 睡眠循环中检测到
// 备注：     lock_must_abort 后，会放弃锁等待并返回 RESTART。
// 备注：     不返回错误码（返回 0）——当前事务继续检查是否还有更多环。
static int abort_lock(struct lock_graph *g, struct trans_waiting_for_lock *i,
		      int err)
{
	if (i == g->g) {
		trace_would_deadlock(g, i->trans);
		return bch2_trans_restart_foreign_task(i->trans,
					BCH_ERR_transaction_restart_would_deadlock,
					_THIS_IP_);
	} else {
		i->trans->lock_must_abort = true;
		wake_up_trans(i->trans);
		return 0;
	}
}

// 备注：btree_trans_abort_preference - 选择环中的牺牲者
// 备注：
// 备注：比较两个锁等待图中的事务，决定谁更适合被中止以打破死锁。
// 备注：优先级规则：
// 备注：  1. 不能失败的事务（lock_may_not_fail=true）优先级最高
// 备注：     → 永远不做牺牲品（返回另一方）
// 备注：     lock_may_not_fail 表示事务已持有其他锁，不能放弃，
// 备注：     如 btree node split 过程中获取 write lock。
// 备注：  2. 否则选开始时间最晚的事务
// 备注：     = 让开始得更早（等待更久）的事务继续运行
// 备注：     这是一种公平性策略：让已经等了很久的事务优先完成。
// 备注：
// 备注：返回优先被中止的事务（牺牲者）。
static struct trans_waiting_for_lock *
btree_trans_abort_preference(struct trans_waiting_for_lock *l,
			     struct trans_waiting_for_lock *r)
{
	if (l->trans->lock_may_not_fail !=
	    r->trans->lock_may_not_fail)
		return l->trans->lock_may_not_fail ? r : l;

	return time_after64(l->trans->locking_wait.trans_start_time,
			    r->trans->locking_wait.trans_start_time)
		? l : r;
}

static noinline __noreturn void break_cycle_fail(struct lock_graph *g)
{
	CLASS(printbuf, buf)();
	guard(printbuf_atomic)(&buf);

	prt_printf(&buf, bch2_fmt(g->g->trans->c, "cycle of nofail locks"));

	for (struct trans_waiting_for_lock *i = g->g; i < g->g + g->nr; i++) {
		struct btree_trans *trans = i->trans;

		bch2_btree_trans_to_text(&buf, trans);

		prt_printf(&buf, "backtrace:\n");
		scoped_guard(printbuf_indent, &buf)
			bch2_prt_task_backtrace(&buf, trans->locking_wait.task, 2, GFP_NOWAIT);
		prt_newline(&buf);
	}

	bch2_print_str(g->g->trans->c, KERN_ERR, buf.buf);
	BUG();
}

static noinline int break_cycle(struct lock_graph *g, struct printbuf *cycle,
				struct trans_waiting_for_lock *from,
				int err)
{
	struct trans_waiting_for_lock *i, *abort = NULL;
	int ret;

	if (lock_graph_remove_non_waiters(g, from))
		return 0;

	/* Only checking, for debugfs: */
	if (cycle) {
		print_cycle(cycle, g);
		ret = -1;
	} else {
		for (i = from; i < g->g + g->nr; i++)
			abort = !abort ? i : btree_trans_abort_preference(abort, i);

		if (unlikely(abort->trans->lock_may_not_fail))
			break_cycle_fail(g);

		ret = abort_lock(g, abort, BCH_ERR_transaction_restart_would_deadlock);
	}

	if (ret)
		lock_graph_pop_all(g);
	else
		lock_graph_pop_from(g, abort);
	return ret;
}

noinline __cold
static int lock_graph_recursion_limit(struct lock_graph *g, struct btree_trans *trans,
				      struct printbuf *cycle)
{
	if (!cycle)
		event_inc_trace(trans->c, trans_restart_would_deadlock_recursion_limit, buf, ({
			guard(printbuf_atomic)(&buf);
			prt_str(&buf, trans->fn);
		}));

	struct btree_trans *orig_trans = g->g->trans;

	if (orig_trans->lock_may_not_fail) {
		/* Other threads will have to rerun the cycle detector: */
		for (struct trans_waiting_for_lock *i = g->g + 1; i < g->g + g->nr; i++)
			wake_up_trans(i->trans);
		return 0;
	}

	return break_cycle(g, cycle, g->g, BCH_ERR_transaction_restart_deadlock_recursion_limit);
}

// 备注：lock_graph_descend - DFS 递归下降：从当前帧的事务"等待队列"中取出一个
// 备注：冲突的事务，将其作为下一帧入栈检查。
// 备注：
// 备注：入口条件：当前帧 top 的 waitlist 中有尚未遍历的冲突事务条目。
// 备注：
// 备注：执行逻辑：
// 备注：  1. 环检测：遍历整栈，如果这个 trans 已经在栈中某帧存在 →
// 备注：     发现环，调用 break_cycle 处理
// 备注：  2. 栈满检查：8 帧已满 → 递归上限路径
// 备注：  3. lock_graph_down：正常入栈，开始检查这个事务持有的所有路径的锁
// 备注：
// 备注：注意：这里不检查 trans 是否为 NULL（调用者保证 waitlist 中只存放了
// 备注：非 NULL 的 trans 指针）。
static inline int lock_graph_descend(struct lock_graph *g, struct btree_trans *trans,
				     struct printbuf *cycle)
{
	for (struct trans_waiting_for_lock *i = g->g; i < g->g + g->nr; i++)
		if (i->trans == trans)
			return break_cycle(g, cycle, i, BCH_ERR_transaction_restart_would_deadlock);

	if (unlikely(g->nr == ARRAY_SIZE(g->g)))
		return lock_graph_recursion_limit(g, trans, cycle);

	lock_graph_down(g, trans);
	return 0;
}

static bool lock_type_conflicts(enum six_lock_type t1, enum six_lock_type t2)
{
	return t1 + t2 > 1;
}

noinline __cold
static int waitlist_alloc_failed(struct lock_graph *g, struct printbuf *cycle)
{
	struct bch_fs *c = g->g->trans->c;

	if (cycle)
		return -1;

	event_inc_trace(c, trans_restart_deadlock_waitlist_alloc, buf, ({
		guard(printbuf_atomic)(&buf);
		prt_str(&buf, g->g->trans->fn);
	}));

	return btree_trans_restart(g->g->trans, BCH_ERR_transaction_restart_deadlock_waitlist_alloc);
}

// 备注：bch2_check_for_deadlock - 锁等待图 DFS 环检测（核心死锁避免算法）
// 备注：
// 备注：当 btree_node_lock_nopath 即将睡眠等待一个被争用的锁时，
// 备注：six_lock 框架调用 bch2_six_check_for_deadlock() → 本函数。
// 备注：
// 备注：算法：以当前事务为根，DFS 遍历锁等待图。
// 备注：对每个事务，遍历其所有 btree_path 的所有层级，找到它持有的锁；
// 备注：扫描每个持锁节点的 wait_fifo（睡眠等待者队列），找出等待该锁的事务；
// 备注：递归检查那些事务又持有哪些锁... 如果回到起点（根事务），发现环。
// 备注：
// 备注：per-CPU 设计：每个 CPU 有自己的 lock_graph 实例，
// 备注：DFS 栈固定 8 层（g[8]），超出触发递归限制路径。
// 备注：所有遍历在 guard(rcu) + guard(preempt) 下进行（无锁、不睡眠）。
int bch2_check_for_deadlock(struct btree_trans *trans, struct printbuf *cycle)
{
	btree_path_idx_t path_idx;

	EBUG_ON(cycle && !cycle->atomic);

	/* trans->paths is rcu protected vs. freeing */
	guard(rcu)();
	guard(preempt)();

	// 备注：获取当前 CPU 的锁图栈（从 per-CPU 变量中读取）
	struct lock_graph *g = this_cpu_ptr(&bch2_lock_graph);
	g->nr = 0;

	// 备注：如果当前事务已被标记为 lock_must_abort（环中牺牲者），
	// 备注：且不是不能失败的事务（lock_may_not_fail），立即返回 RESTART。
	// 备注：这是对其他 CPU 检测到环后唤醒我们的响应。
	if (trans->lock_must_abort && !trans->lock_may_not_fail) {
		if (cycle)
			return -1;

		trace_would_deadlock(g, trans);
		return btree_trans_restart(trans, BCH_ERR_transaction_restart_would_deadlock);
	}

	// 备注：根事务入栈（DFS 起点）
	lock_graph_down(g, trans);
next:
	if (!g->nr)
		return 0;

	struct trans_waiting_for_lock *top = &g->g[g->nr - 1];

	// 备注：阶段 1 — 遍历 waitlist 快照（之前拍下的冲突事务列表）
	// 备注：如果 waitlist_idx < waitlist.nr，说明还有冲突事务待递归检查
	if (top->waitlist_idx < top->waitlist.nr) {
		try(lock_graph_descend(g, top->waitlist.data[top->waitlist_idx++], cycle));

		goto next;
	}

	// 备注：阶段 2 — 多路径持有锁扫描
	// 备注：
	// 备注：waitlist 已空 → 重新扫描 top->trans 的所有 btree_path。
	// 备注：在 bcachefs 中，一个 btree_transaction 可能同时持有多个
	// 备注：btree_path（trans->paths[] 数组，最多 BTREE_ITER_MAX 个）。
	// 备注：每个 btree_path 对应一次独立的 btree 遍历，可以在不同 btree ID
	// 备注：（如 extent、inode、dirent 等）的不同层级（叶子=level 0 到根）上
	// 备注：持有 six_lock。
	// 备注：
	// 备注：【多路径死锁场景】
	// 备注：  事务 A   → path[0]: 持有 extent 节点 X（读锁）
	// 备注：             path[1]: 等待 inode 节点 Y（意向锁）
	// 备注：  事务 B   → path[0]: 持有 inode 节点 Y（读锁）
	// 备注：             path[1]: 等待 extent 节点 X（意向锁）
	// 备注：
	// 备注：锁图必须跨路径追踪依赖边：A.path[1]→Y→B.path[0] 且 B.path[1]→X→A.path[0]，
	// 备注：形成环路。如果只检查单路径（如 A 只在 path[0] 上找等待者），会遗漏此环。
	// 备注：
	// 备注：【遍历策略】
	// 备注：每帧从 top->path_idx 记录的断点位置继续，而非每次从头扫描。
	// 备注：对每个路径，逐层级（level 0→BTREE_MAX_DEPTH）检查是否有持锁，
	// 备注：找到持锁节点 → 扫描 wait_fifo 找冲突者 → 入 waitlist 待递归。
	// 备注：
	// 备注：【linked_paths】
	// 备注：某些路径通过 path->linked 字段形成"共享锁路径群组"
	// 备注：（如 btree_trans 中 split/merge 时 parent 路径与 child 路径
	// 备注：指向同一节点的不同层级）。linked_paths 共享同一组已锁定节点
	// 备注：的锁计数，遍历时通过 btree_node_locked_type() 的聚合语义
	// 备注：（linked 路径合并检查）避免重复遍历。
	top->waitlist_idx = top->waitlist.nr = 0;

	struct btree_path *paths = rcu_dereference(top->trans->paths);
	if (!paths)
		goto up;

	unsigned long *paths_allocated = trans_paths_allocated(paths);

	// 备注：阶段 2a — 遍历事务持有的所有路径
	// 备注：trans_for_each_path_idx_from 从上次停下的 path_idx 继续（断点续扫）
	// 备注：path_idx 是路径在 trans->paths[] 数组中的索引，非指针：
	// 备注：paths 可能被 bch2_trans_realloc_paths() 重新分配使指针失效，
	// 备注：但 path_idx（数组内偏移）始终有效。
	trans_for_each_path_idx_from(paths_allocated, *trans_paths_nr(paths),
				     path_idx, top->path_idx) {
		struct btree_path *path = paths + path_idx;
		// 备注：跳过没有持有任何锁的路径
		// 备注：nodes_locked 是路径在各层级上 locks_want 的位图（bitmask），
		// 备注：路径可能已创建但尚未获取锁（如遍历中临时释放了所有锁的路径），
		// 备注：快速跳过节省 CPU。
		if (!path->nodes_locked)
			continue;

		if (path_idx != top->path_idx) {
			top->path_idx		= path_idx;
			top->level		= 0;
		}

		// 备注：阶段 2b — 遍历路径的每一层（叶子=0，根=BTREE_MAX_DEPTH-1）
		// 备注：btree 深度通常在 3-6 层（32 位扇区索引 -> radix tree），
		// 备注：但路径可能在任何层级持有锁，所以逐级扫描 0..BTREE_MAX_DEPTH。
		while (top->level < BTREE_MAX_DEPTH) {
			int lock_held = btree_node_locked_type(path, top->level);

			if (lock_held == BTREE_NODE_UNLOCKED) {
				top->level++;
				continue;
			}

			// 备注：读取路径在此层级持有的节点指针
			// 备注：使用 READ_ONCE 防止编译器优化导致撕裂读
			// 备注：注意：同一 btree_node 可能被多个路径锁住（如一个在
			// 备注：level 0，另一个在 level 1 指向同一节点的父级指针），
			// 备注：但因为锁图按 per-level per-path 帧化，不会重复入边。
			top->node_have = &READ_ONCE(path->l[top->level].b)->c;
			if (unlikely(IS_ERR_OR_NULL(top->node_have))) {
				/*
				 * If we get here, it means we raced with the
				 * other thread updating its btree_path
				 * structures - which means it can't be blocked
				 * waiting on a lock:
				 */
				if (!lock_graph_remove_non_waiters(g, g->g)) {
					/*
					 * If lock_graph_remove_non_waiters()
					 * didn't do anything, it must be
					 * because we're being called by debugfs
					 * checking for lock cycles, which
					 * invokes us on btree_transactions that
					 * aren't actually waiting on anything.
					 * Just bail out:
					 */
					lock_graph_pop_all(g);
				}

				goto next;
			}

			// 备注：阶段 2c — 无锁遍历节点 wait_fifo（RCU 保护下）
			// 备注：遍历节点的等待队列，找出所有"等待的锁类型与当前路径
			// 备注：在此层级持有的锁类型冲突"的事务。
			// 备注：
			// 备注：lock_type_conflicts(lock_held, want) 判断：
			// 备注：  两个锁类型之和 > 1 即为冲突
			// 备注：  S+S=0(不冲突)  S+I=1(不冲突)  S+X=2(冲突)
			// 备注：  I+I=2(冲突)     I+X=3(冲突)
			// 备注：
			// 备注：冲突者快照到 top->waitlist（per-frame darray），
			// 备注：然后递归（goto next → phase 1 → lock_graph_descend）
			// 备注：检查该事务持有的所有路径的锁。
			// 备注：
			// 备注：waitlist 使用 GFP_NOWAIT|__GFP_NOWARN 分配（不能睡眠），
			// 备注：如果分配失败 → waitlist_alloc_failed 路径。
			/*
			 * Lockless walk of wait_fifo: we're under guard(rcu),
			 * trans memory is RCU-deferred in bch2_trans_put, and
			 * wait_fifo slots only transition between NULL and a
			 * valid pointer (never torn). Per-CPU cache reuse inside
			 * a grace period can aim us at a reused trans — benign,
			 * cycles missed this pass are caught next.
			 *
			 * Snapshot the conflicting trans pointers into a per-frame
			 * darray so iteration is stable across concurrent wakeups.
			 * Heap allocation is GFP_NOWAIT|__GFP_NOWARN (can't sleep
			 * under rcu+preempt). If growth past the inline buffer
			 * fails, silently truncating would risk missing a cycle;
			 * bail out with a dedicated restart type + counter so we
			 * can tell if this ever actually fires in the wild.
			 */
			struct six_lock_wait_fifo *wf =
				rcu_dereference(top->node_have->lock.wait_fifo);
			darray_for_each(*wf, i) {
				trans = container_of_or_null(i->w, struct btree_trans, locking_wait);

				if (trans &&
				    trans != top->trans &&
				    lock_type_conflicts(lock_held, i->start_time & SIX_LOCK_WANT_MASK)) {
					if (unlikely(darray_push_gfp(&top->waitlist, trans,
								     GFP_NOWAIT|__GFP_NOWARN))) {
						return waitlist_alloc_failed(g, cycle);
					}
				}
			}

			top->level++;

			// 备注：如果 waitlist 中新增了冲突者，立即递归检查
			// 备注：性能优化 - 在继续扫描当前路径的其他层级之前，优先
			// 检查新发现的竞争事务是否形成环。这能更快发现死锁，避免不
			// 必要的路径遍历。然后通过 goto next 回到阶段 1。
			if (top->waitlist_idx < top->waitlist.nr)
				goto next;
		}
	}
up:
	if (cycle)
		print_chain(cycle, g);
	// 备注：回溯：当前帧所有路径已遍历完毕（没有更多冲突者），
	// 备注：退栈回到上一帧继续扫描它的 waitlist 或路径。
	--g->nr;
	goto next;
}

// 备注：locking_node - 从 six_lock 反解出 btree 节点指针
// 备注：container_of 从 six_lock 反解到 btree_bkey_cached_common，
// 备注：再根据 cached 标志决定是否转为 struct btree。
// 备注：如果节点是 key cache entry（b->cached=true），返回 NULL。
static inline struct btree *locking_node(struct six_lock *lock)
{
	struct btree_bkey_cached_common *b = container_of(lock, struct btree_bkey_cached_common, lock);
	return !b->cached
		? container_of(b, struct btree, c)
		: NULL;
}

// 备注：node_reuse_race - 检测节点是否已被回收重用
// 备注：比较锁请求时拍下的 hash_val（在 btree_node_lock_nopath 中记录）
// 备注：与当前节点的 hash_val。如果不同，说明节点已被回收并分配给另一个
// 备注：不同的 btree 节点（或同一节点的不同代）。
// 备注：
// 备注：有两种检查方式：
// 备注：  1. locking_hash_val ≠ 0 → 直接比较哈希值
// 备注：  2. locking_root_id ≠ -1 → 检查根节点指针是否变化
// 备注：  3. 都不满足 → 不需要检查（如 key cache entry）
// 备注：
// 备注：只有 btree 节点需要此检查（interior update 可能通过 off-path
// 备注：方式如 btree_node_reclaim 的 six_trylock_intent 偷取节点）。
static inline bool node_reuse_race(struct btree_trans *trans, struct btree *b)
{
	if (trans->locking_hash_val)
		return trans->locking_hash_val != b->hash_val;
	else if (trans->locking_root_id != -1)
		return bch2_btree_id_root_b(trans->c, trans->locking_root_id) != b;
	else
		return false;
}

// 备注：bch2_six_check_for_deadlock - six_lock 框架回调的死锁检测入口
// 备注：
// 备注：当 six_lock_ip_waiter 或 six_lock_contended 准备将当前任务
// 备注：放入等待队列前，调用此函数检查是否会导致死锁。
// 备注：
// 备注：执行步骤：
// 备注：  1. smp_mb() 内存屏障：确保看到其他 CPU 的最新状态
// 备注：  2. node_reuse_race() 检查：节点不是被回收入其他用途？
// 备注：  3. 调度器 wake-CPU hint（内核模式）
// 备注：  4. bch2_check_for_deadlock() 运行真正的 DFS 环检测
// 备注：
// 备注：返回值：
// 备注：  0 → 无死锁，可以睡眠等待
// 备注：  RESTART → 检测到死锁或节点重用了，需要放弃锁并重启事务
int bch2_six_check_for_deadlock(struct six_lock *lock, struct six_lock_waiter *w)
{
	// 备注：smp_mb() — 全屏障
	// 备注：与每个插入 wait_fifo 插槽的 spin_unlock(&lock->wait_lock) 配对。
	// 备注：锁图遍历器即将在其他 CPU 上无锁读取各种数据
	// 备注：（trans->locking、->paths、->nodes_locked、各锁的 wait_fifo 槽），
	// 备注：这些数据都是在不同的 wait_lock 下写入的，写入者的 unlock 操作不会
	// 备注：与我们的读取操作形成 happens-before 关系。
	// 备注：没有此屏障，在弱内存序架构（ARM/PowerPC）上会读到过期快照，
	// 备注：可能错过刚在其他 CPU 上形成的环——该"错过"的容错是"下次会被检测到"，
	// 备注：但如果环中所有参与者都睡着了，就没有"下次"了。
	/*
	 * Full barrier paired with every inserter's spin_unlock(&lock->wait_lock)
	 * that published their wait_fifo slot. The lockless walker about to run
	 * reads other trans's ->locking, ->paths, ->nodes_locked, and other locks'
	 * wait_fifo slots - all written under unrelated wait_locks the walker
	 * never acquires, so the writers' releases don't pair with anything on
	 * our side. On weakly-ordered architectures, without smp_mb() here the
	 * walker can read a stale snapshot and miss a cycle whose closing edge
	 * was just published on another CPU.
	 *
	 * The walker's "cycles missed this pass are caught next" reassurance
	 * doesn't fire once every cycle participant is parked: nobody issues
	 * another lock request, so there is no next pass.
	 */
	smp_mb();

	/*
	 * The btree node we're about to sleep on may have been reclaimed/reused
	 * since the caller picked the lock — the path's b pointer is still
	 * valid memory, but the identity behind it is gone. Don't sleep on a
	 * phantom; force a restart so the trans re-traverses to the real
	 * current node (or learns there isn't one).
	 *
	 * Only btree nodes need this: interior updates take node locks
	 * off-path (e.g. via btree_node_reclaim's six_trylock_intent), so the
	 * cycle detector can't see the holder. Key cache entries don't have
	 * that pattern — they're always held via a path the detector walks.
	 *
	 * Compare against the hash_val snapshotted at lock-attempt time in
	 * btree_node_lock_nopath. Checking !hash_val alone is insufficient:
	 * the node may already have been freed *and* re-hashed to a different
	 * identity, in which case hash_val is non-zero but ≠ what we wanted.
	 *
	 * Publish order on the reclaim side:
	 * bch2_btree_node_transition_state_locked clears (or rotates)
	 * hash_val, smp_mb(), six_lock_wakeup_all() — pairs with the
	 * smp_mb() above.
	 */
	struct btree_trans *trans = container_of(w, struct btree_trans, locking_wait);
	struct btree *b = locking_node(lock);
	if (b && node_reuse_race(trans, b))
		return bch_err_throw(trans->c, no_btree_node_reused);

#ifdef __KERNEL__
	/*
	 * Wake-CPU hint, set at the moment of sleep: nudge the scheduler
	 * toward the CPU whose L1/L2 owns this task's shard's btree-node
	 * working set. Soft — sched is free to override under load; writes
	 * nothing when already matched. Placed here (vs. trans_begin)
	 * because select_task_rq_fair() consults wake_cpu only at wakeup,
	 * so the hint has to survive from the schedule() that follows.
	 */
	if (trans->shard_cpu >= 0 &&
	    trans->shard_cpu != raw_smp_processor_id())
		WRITE_ONCE(current->wake_cpu, trans->shard_cpu);
#endif

	return bch2_check_for_deadlock(trans, NULL);
}

// 备注：btree_node_lock_increment - six_lock 重入：递增已有锁的引用计数
// 备注：
// 备注：six_lock 支持重入（re-entrant）：同一锁可以被同一获取者多次获取。
// 备注：如果事务内已有（任意）btree_path 在目标节点的同一层级持有 >= want
// 备注：类型的锁，则无需重新经过 slowpath 等待，只需通过 six_lock_increment
// 备注：增加该锁类型的引用计数即可。
// 备注：
// 备注：trans_for_each_path 遍历事务的所有路径（非当前请求路径）检查：
// 备注：  &path->l[level].b->c == b               ← 同一节点
// 备注：  btree_node_locked_type(path, level) >= want  ← 锁类型足够
// 备注：如果找到 → six_lock_increment() 递增引用计数，返回 true。
// 备注：
// 备注：典型场景：btree split 时 parent 路径已锁住节点 A（INTENT），
// 备注：child 路径也需要节点 A 同一层级的锁——通过重入递增 INTENT 计数，
// 备注：避免 child 路径重复等待 parent 路径已经持有的锁。
/*
 * Lock a btree node if we already have it locked on one of our linked
 * iterators:
 */
static inline bool btree_node_lock_increment(struct btree_trans *trans,
					     struct btree_bkey_cached_common *b,
					     unsigned level,
					     enum btree_node_locked_type want)
{
	struct btree_path *path;
	unsigned i;

	trans_for_each_path(trans, path, i)
		if (&path->l[level].b->c == b &&
		    btree_node_locked_type(path, level) >= want) {
			six_lock_increment(&b->lock, (enum six_lock_type) want);
			return true;
		}

	return false;
}

// 备注：bch2_btree_node_lock_slowpath - B-tree 节点锁慢速路径
// 备注：
// 备注：当 btree_node_lock 的快速路径（six_trylock_type）失败时调用。
// 备注：
// 备注：执行流程：
// 备注：  1. 先尝试 btree_node_lock_increment：检查是否通过 six_lock
// 备注：     重入机制（事务内其他路径已持有该节点锁）可以递增引用计数。
// 备注：     是→ 递增后返回，无需阻塞。
// 备注：  2. 递增失败 → btree_node_lock_nopath()：
// 备注：     设置 trans->locking / locking_wait 信息，
// 备注：     然后 six_lock_ip_waiter()：
// 备注：       → 注册到节点 wait_fifo
// 备注：       → bch2_six_check_for_deadlock() 死锁检测
// 备注：       → 无死锁则 schedule() 睡眠
// 备注：  3. 返回 0（成功）或 RESTART（死锁/错误）
// 备注：
// 备注：慢速路径可能触发事务重启，调用者需支持 lockrestart_do 重试。
int bch2_btree_node_lock_slowpath(struct btree_trans *trans,
			struct btree_path *path,
			struct btree_bkey_cached_common *b,
			unsigned level,
			enum six_lock_type type)
{
	if (!btree_node_lock_increment(trans, b, level, (enum btree_node_locked_type) type)) {
#ifdef CONFIG_BCACHEFS_LOCK_TIME_STATS
		u64 contended_start = local_clock();
#endif
		int ret = btree_node_lock_nopath(trans, b, type, false,
						 btree_path_ip_allocated(path), true);
#ifdef CONFIG_BCACHEFS_LOCK_TIME_STATS
		__bch2_time_stats_update(&btree_trans_stats(trans)->lock_wait_times,
					 contended_start, local_clock());
#endif
		if (ret)
			return ret;
	}

	return 0;
}

// 备注：bch2_btree_node_lock_write_contended - 从 read 升级到 write 锁
// 备注：
// 备注：当 btree node split / merge / rebalance 等结构性操作时，
// 备注：需要将已持有的 intent+read 锁升级为 write 锁（独占写）。
// 备注：
// 备注：执行流程：
// 备注：  1. 清除 locking_hash_val / locking_root_id（off-path lock，
// 备注：     不需要节点重用检查，因为我们已持有 intent 锁）
// 备注：  2. 临时释放所有 read 锁计数（six_lock_readers_add(-readers)），
// 备注：     因为 six_lock_write 要求 reader count 为 0 才能获得 write 锁
// 备注：  3. 调用 btree_node_lock_nopath 请求 write 锁——这个调用会在
// 备注：     内部通过 six_lock_ip_waiter → bch2_six_check_for_deadlock
// 备注：     进行死锁检测
// 备注：  4. 如果获取失败（死锁或重启），恢复 read 锁计数
// 备注：  5. 标记路径的锁状态为 intent-locked（因为 write 失败后要回退到 intent）
// 备注：
// 备注：注意：临时释放 read lock 是安全的，因为我们仍持有 intent 锁，
// 备注：其他写者无法获取 write 锁（intent 阻塞 write），其他读者可以
// 备注：暂时进入但会在恢复时重新计数。
int bch2_btree_node_lock_write_contended(struct btree_trans *trans, struct btree_path *path,
				 struct btree_bkey_cached_common *b,
				 bool lock_may_not_fail)
{
	// 备注：清除路径标识——这是 off-path 锁请求（不通过 btree_path
	// 备注：描述符锁定），无需 hash_val 检查重用。
	trans->locking_hash_val = 0;
	trans->locking_root_id	= -1;

	/*
	 * Must drop our read locks before calling six_lock_write() -
	 * six_unlock() won't do wakeups until the reader count
	 * goes to 0, and it's safe because we have the node intent
	 * locked:
	 */
	int readers = bch2_btree_node_lock_counts(trans, NULL, b, b->level).n[SIX_LOCK_read];
	if (readers)
		six_lock_readers_add(&b->lock, -readers);

	int ret = btree_node_lock_nopath(trans, b, SIX_LOCK_write,
					 lock_may_not_fail, _RET_IP_, !readers);
	if (readers)
		six_lock_readers_add(&b->lock, readers);

	if (ret)
		mark_btree_node_locked_noreset(path, b->level, BTREE_NODE_INTENT_LOCKED);

	return ret;
}

// 备注：bch2_btree_node_lock_with_path - 为未通过路径访问的节点获取锁
// 备注：
// 备注：当调用者需要锁住一个 btree 节点，但该节点不是通过正常的
// 备注：btree_path 遍历到达时（如 interior node update 中 off-path
// 备注：加锁），此函数创建一个临时未锁定路径来记录锁持有关系。
// 备注：
// 备注：关键设计：
// 备注：  1. bch2_path_get_unlocked_mut：分配一个临时路径，不锁定任何节点
// 备注：  2. 设置 trans->locking_hash_val=0：跳过节点重用检查（调用者
// 备注：     已验证节点有效性）
// 备注：  3. btree_node_lock：走标准锁获取路径（快/慢路径 + 死锁检测）
// 备注：  4. 成功后：标记路径锁状态、记录 lock_seq 和节点指针
// 备注：  5. 失败后：释放临时路径
// 备注：
// 备注：调用者通过 bch2_btree_node_unlock_with_path() 释放锁，
// 备注：传回 path_idx_out 标识路径。
/*
 * Lock @b when the caller doesn't already have a path for it: create a
 * temporary unlocked path, take the lock, then record the lock on the path
 * so the cycle detector can find us as the holder.
 *
 * Caller releases via bch2_btree_node_unlock_with_path().
 *
 * May return a transaction_restart; wrap in lockrestart_do().
 */
int __must_check
bch2_btree_node_lock_with_path(struct btree_trans *trans,
			       struct btree_bkey_cached_common *b,
			       enum six_lock_type type,
			       btree_path_idx_t *path_idx_out)
{
	btree_path_idx_t path_idx = bch2_path_get_unlocked_mut(trans,
				b->btree_id, b->level, btree_node_pos(b), b->cached);

	struct btree_path *path = trans->paths + path_idx;
	/* No key context here — caller already has the b. Skip the hash_val
	 * check; we're acquiring on a node the caller already validated. */
	trans->locking_hash_val = 0;
	trans->locking_root_id	= -1;
	int ret = btree_node_lock(trans, path, b, b->level, type);
	if (ret) {
		bch2_path_put(trans, path_idx, true);
		return ret;
	}

	mark_btree_node_locked(trans, path, b->level,
			       (enum btree_node_locked_type) type);
	path->l[b->level].lock_seq	= six_lock_seq(&b->lock);
	path->l[b->level].b		= (struct btree *) b;

	*path_idx_out = path_idx;
	return 0;
}

/* relock */

static void get_locks_fail_to_text(struct printbuf *out, struct btree_trans *trans,
				   struct btree_path *old_path,
				   struct btree_path *path,
				   struct get_locks_fail *f)
{
	bch2_bpos_to_text(out, path->pos);
	prt_printf(out, " %s l=%u seq=%u node seq=",
		   bch2_btree_id_str(path->btree_id),
		   f->l, path->l[f->l].lock_seq);
	if (IS_ERR_OR_NULL(f->b)) {
		prt_str(out, bch2_err_str(PTR_ERR(f->b)));
	} else {
		prt_printf(out, "%u", f->b->c.lock.seq);

		struct six_lock_count c =
			bch2_btree_node_lock_counts(trans, NULL, &f->b->c, f->l);
		prt_printf(out, " self locked %u.%u.%u", c.n[0], c.n[1], c.n[2]);

		c = six_lock_counts(&f->b->c.lock);
		prt_printf(out, " total locked %u.%u.%u", c.n[0], c.n[1], c.n[2]);
	}

	prt_newline(out);
	bch2_btree_path_to_text(out, trans, path - trans->paths, old_path);
}

static int btree_path_get_locks(struct btree_trans *trans,
				struct btree_path *path,
				bool upgrade,
				struct get_locks_fail *f,
				int restart_err)
{
	unsigned l = path->level;

	do {
		if (!btree_path_node(path, l))
			break;

		if (!(upgrade
		      ? bch2_btree_node_upgrade(trans, path, l)
		      : bch2_btree_node_relock(trans, path, l)))
			goto err;

		l++;
	} while (l < path->locks_want);

	return 0;
err:
	if (f) {
		f->l	= l;
		f->b	= path->l[l].b;
	}

	/*
	 * Do transaction restart before unlocking, so we don't pop
	 * should_be_locked asserts
	 */
	if (restart_err) {
		btree_trans_restart(trans, restart_err);
	} else if (path->should_be_locked && !trans->restarted) {
		if (upgrade)
			path->locks_want = l;
		return -1;
	}

	__bch2_btree_path_unlock(trans, path);

	/*
	 * When we fail to get a lock, we have to ensure that any child nodes
	 * can't be relocked so bch2_btree_path_traverse has to walk back up to
	 * the node that we failed to relock:
	 */
	do {
		path->l[l].b = upgrade
			? ERR_PTR(-BCH_ERR_no_btree_node_upgrade)
			: ERR_PTR(-BCH_ERR_no_btree_node_relock);
	} while (l--);

	return -restart_err ?: -1;
}

// 备注：__bch2_btree_node_relock - 单层级 btree 节点重锁
// 备注：
// 备注：在事务重启后，尝试重新获取路径在指定层级上的锁。
// 备注：重锁不使用 slowpath（不睡眠、不注册 wait_fifo、不检测死锁），
// 备注：因为此时事务刚重启，尚未持有任何锁。
// 备注：
// 备注：两种重锁路径（任一成功即可）：
// 备注：  1. six_relock_type(&b->c.lock, want, lock_seq)：标准 SIX 重锁
// 备注：     需要提供 lock_seq 验证节点未在间隙期被回收。
// 备注：     lock_seq 是 six_lock 的单调递增序列号，每次写锁释放时递增。
// 备注：     如果 lock_seq 匹配 → 节点仍然是原来那个节点（未被回收重用）。
// 备注：
// 备注：  2. btree_node_lock_increment：通过 six_lock 重入机制，
// 备注：     如果事务内其他路径已经锁住了同一个节点，可以直接递增
// 备注：     引用计数（无需再次阻塞等待），需要 lock_seq 先验证身份。
// 备注：
// 备注：race_fault()：故障注入测试点，用于测试重锁失败路径。
// 备注：
// 备注：失败时记录 trace 事件（btree_path_relock_fail）用于调试。
bool __bch2_btree_node_relock(struct btree_trans *trans,
			      struct btree_path *path, unsigned level,
			      bool trace)
{
	struct btree *b = btree_path_node(path, level);
	int want = __btree_lock_want(path, level);

	if (race_fault())
		goto fail;

	if (six_relock_type(&b->c.lock, want, path->l[level].lock_seq) ||
	    (btree_node_lock_seq_matches(path, b, level) &&
	     btree_node_lock_increment(trans, &b->c, level, want))) {
		mark_btree_node_locked(trans, path, level, want);
		return true;
	}
fail:
	if (trace && !trans->notrace_relock_fail)
		event_inc_trace(trans->c, btree_path_relock_fail, buf, ({
			prt_printf(&buf, "%s\n", trans->fn);
			bch2_btree_path_to_text(&buf, trans, path - trans->paths, path);
		}));
	return false;
}

/* upgrade */


// 备注：【upgrade 锁升级】
// 备注：在 btree 遍历过程中，路径可能最初只要求 read 锁（遍历时），
// 备注：但后续操作需要 intent 锁（如插入/删除时需要阻塞结构变更）。
// 备注：锁升级从当前已持有的锁类型尝试升级到更高的类型。

// 备注：bch2_btree_node_upgrade - 升级路径指定层级的锁（read→intent）
// 备注：
// 备注：入口条件：path 在 level 层级已持有 read 锁或无锁（relock 场景）。
// 备注：
// 备注：btree_lock_want 决策路径：
// 备注：  - UNLOCKED: 该层不在需要范围 → 已满足
// 备注：  - READ_LOCKED: 只需要读锁 → 调用 bch2_btree_node_relock
// 备注：  - INTENT_LOCKED: 需要升级 → 进入升级流程
// 备注：  - WRITE_LOCKED: BUG（不应该通过 upgrade 路径获取写锁）
// 备注：
// 备注：升级策略（三种方式依次尝试）：
// 备注：  1. 如果已持有 read 锁 → six_lock_tryupgrade()：
// 备注：     尝试将 SIX 锁从 read 原子升级到 intent
// 备注：     无需释放重取，无死锁风险
// 备注：  2. 如果无锁（relock 场景）→ six_relock_type(INTENT)：
// 备注：     直接请求 intent 锁（跳过 read 中间态）
// 备注：  3. 如果 lock_seq 匹配 → btree_node_lock_increment + btree_node_unlock：
// 备注：     six_lock 支持重入（re-entrant）：事务内已有路径持有该节点的
// 备注：     INTENT 锁时，six_lock_increment 增加 intent 引用计数后，
// 备注：     当前路径可以"重入"获取同一 INTENT 锁（无需阻塞等待）。
// 备注：     然后释放当前路径原有的读锁（或无锁），跳转到 success 标记。
// 备注：     这等价于将另一个路径的 INTENT 引用"转移"给当前路径。
// 备注：
// 备注：成功：标记路径锁类型为 INTENT_LOCKED（使用 noreset 版本，
// 备注：因为升级不改变锁的"开始持有"时间）。
// 备注：失败：记录 trace 事件（btree_path_upgrade_fail），返回 false。
/*
 * upgrade
 */
bool bch2_btree_node_upgrade(struct btree_trans *trans,
			     struct btree_path *path, unsigned level)
{
	struct btree *b = path->l[level].b;

	if (!is_btree_node(path, level))
		return false;

	switch (btree_lock_want(path, level)) {
	case BTREE_NODE_UNLOCKED:
		EBUG_ON(btree_node_locked(path, level));
		return true;
	case BTREE_NODE_READ_LOCKED:
		EBUG_ON(btree_node_intent_locked(path, level));
		return bch2_btree_node_relock(trans, path, level);
	case BTREE_NODE_INTENT_LOCKED:
		break;
	case BTREE_NODE_WRITE_LOCKED:
		BUG();
	}

	if (btree_node_intent_locked(path, level))
		return true;

	if (race_fault())
		return false;

	if (btree_node_locked(path, level)
	    ? six_lock_tryupgrade(&b->c.lock)
	    : six_relock_type(&b->c.lock, SIX_LOCK_intent, path->l[level].lock_seq))
		goto success;

	if (btree_node_lock_seq_matches(path, b, level) &&
	    btree_node_lock_increment(trans, &b->c, level, BTREE_NODE_INTENT_LOCKED)) {
		btree_node_unlock(trans, path, level);
		goto success;
	}

	event_inc_trace(trans->c, btree_path_upgrade_fail, buf, ({
		prt_printf(&buf, "%s\n", trans->fn);
		bch2_btree_path_to_text(&buf, trans, path - trans->paths, path);
	}));
	return false;
success:
	mark_btree_node_locked_noreset(path, level, BTREE_NODE_INTENT_LOCKED);
	return true;
}

/* Btree path locking: */

__flatten
bool bch2_btree_path_relock_norestart(struct btree_trans *trans, struct btree_path *path)
{
	bool ret = !btree_path_get_locks(trans, path, false, NULL, 0);
	bch2_trans_verify_locks(trans);
	return ret;
}

noinline __cold
static int bch2_btree_path_relock_trace(struct btree_trans *trans, struct btree_path *path)
{
	struct get_locks_fail f;
	struct btree_path old_path = *path;
	int ret = 0;

	if (btree_path_get_locks(trans, path, false, &f, 0)) {
		event_inc_trace(trans->c, trans_restart_relock_path, buf, ({
			prt_printf(&buf, "%s\n", trans->fn);
			get_locks_fail_to_text(&buf, trans, &old_path, path, &f);
		}));
		ret = btree_trans_restart(trans, BCH_ERR_transaction_restart_relock_path);
	}

	bch2_trans_verify_locks(trans);
	return ret;
}

int __bch2_btree_path_relock(struct btree_trans *trans, struct btree_path *path)
{
	if (unlikely(trace_trans_restart_relock_path_enabled()))
		return bch2_btree_path_relock_trace(trans, path);

	int ret = 0;
	if (btree_path_get_locks(trans, path, false, NULL, 0)) {
		event_inc(trans->c, trans_restart_relock_path);
		ret = btree_trans_restart(trans, BCH_ERR_transaction_restart_relock_path);
	}

	bch2_trans_verify_locks(trans);
	return ret;
}

bool __bch2_btree_path_upgrade_norestart(struct btree_trans *trans,
					 struct btree_path *path,
					 unsigned new_locks_want)
{
	path->locks_want = new_locks_want;

	/*
	 * If we need it locked, we can't touch it. Otherwise, we can return
	 * success - bch2_path_get() will use this path, and it'll just be
	 * retraversed:
	 */
	bool ret = !btree_path_get_locks(trans, path, true, NULL, 0) ||
		!path->should_be_locked;

	bch2_btree_path_verify_locks(trans, path);
	return ret;
}

int __bch2_btree_path_upgrade(struct btree_trans *trans,
			      struct btree_path *path,
			      unsigned new_locks_want)
{
	unsigned old_locks = path->nodes_locked;
	unsigned old_locks_want = path->locks_want;

	path->locks_want = max_t(unsigned, path->locks_want, new_locks_want);

	struct get_locks_fail f = {};
	int ret = btree_path_get_locks(trans, path, true, &f,
				BCH_ERR_transaction_restart_upgrade);
	if (!ret)
		goto out;

	/*
	 * XXX: this is ugly - we'd prefer to not be mucking with other
	 * iterators in the btree_trans here.
	 *
	 * On failure to upgrade the iterator, setting iter->locks_want and
	 * calling get_locks() is sufficient to make bch2_btree_path_traverse()
	 * get the locks we want on transaction restart.
	 *
	 * But if this iterator was a clone, on transaction restart what we did
	 * to this iterator isn't going to be preserved.
	 *
	 * Possibly we could add an iterator field for the parent iterator when
	 * an iterator is a copy - for now, we'll just upgrade any other
	 * iterators with the same btree id.
	 *
	 * The code below used to be needed to ensure ancestor nodes get locked
	 * before interior nodes - now that's handled by
	 * bch2_btree_path_traverse_all().
	 */
	if (!path->cached && !trans->in_traverse_all) {
		struct btree_path *linked;
		unsigned i;

		trans_for_each_path(trans, linked, i)
			if (linked != path &&
			    linked->cached == path->cached &&
			    linked->btree_id == path->btree_id &&
			    linked->locks_want < new_locks_want) {
				linked->locks_want = new_locks_want;
				btree_path_get_locks(trans, linked, true, NULL, 0);
			}
	}

	event_inc_trace(trans->c, trans_restart_upgrade, buf, ({
		prt_printf(&buf, "%s\n", trans->fn);
		prt_printf(&buf, "%s %pS\n", trans->fn, (void *) _RET_IP_);
		bch2_bbpos_to_text(&buf, BBPOS(path->btree_id, path->pos));
		prt_newline(&buf);
		prt_printf(&buf, "locks want %u -> %u level %u\n",
			   old_locks_want, new_locks_want, f.l);
		prt_printf(&buf, "nodes_locked %x -> %x\n",
			   old_locks, path->nodes_locked);
		prt_printf(&buf, "node %s ", IS_ERR(f.b) ? bch2_err_str(PTR_ERR(f.b)) :
			   !f.b ? "(null)" : "(node)");
		prt_printf(&buf, "path seq %u node seq %u",
			   IS_ERR_OR_NULL(f.b) ? 0 : f.b->c.lock.seq,
			   path->l[f.l].lock_seq);

		if (!IS_ERR_OR_NULL(f.b)) {
			struct six_lock_count c = six_lock_counts(&f.b->c.lock);
			prt_printf(&buf, " locked %u.%u.%u", c.n[0], c.n[1], c.n[2]);
		}
		prt_printf(&buf, "\npath idx %zu", path - trans->paths);
#ifdef TRACK_PATH_ALLOCATED
		prt_printf(&buf, " allocated: %ps", (void *) path->ip_allocated);
#endif
		prt_newline(&buf);
	}));
out:
	bch2_trans_verify_locks(trans);
	return ret;
}

void __bch2_btree_path_downgrade(struct btree_trans *trans,
				 struct btree_path *path,
				 unsigned new_locks_want)
{
#ifdef CONFIG_BCACHEFS_DEBUG
	unsigned old_locks_want = path->locks_want;
#endif

	if (trans->restarted)
		return;

	EBUG_ON(path->locks_want < new_locks_want);

	path->locks_want = new_locks_want;

	unsigned l;
	while (path->nodes_locked &&
	       (l = btree_path_highest_level_locked(path)) >= path->locks_want) {
		if (l > path->level) {
			btree_node_unlock(trans, path, l);
		} else {
			if (btree_node_intent_locked(path, l)) {
				six_lock_downgrade(&path->l[l].b->c.lock);
				mark_btree_node_locked_noreset(path, l, BTREE_NODE_READ_LOCKED);
			}
			break;
		}
	}

	bch2_btree_path_verify_locks(trans, path);
#ifdef CONFIG_BCACHEFS_DEBUG
	event_trace(trans->c, path_downgrade, buf, ({
		prt_printf(&buf, "%s\n", trans->fn);
		prt_printf(&buf, "old locks_want: %u\n", old_locks_want);
		bch2_btree_path_to_text(&buf, trans, path - trans->paths, path);
	}));
#endif
}

/* Btree transaction locking: */

void bch2_trans_downgrade(struct btree_trans *trans)
{
	struct btree_path *path;
	unsigned i;

	if (trans->restarted)
		return;

	trans_for_each_path(trans, path, i)
		if (path->ref)
			bch2_btree_path_downgrade(trans, path);
}

static inline void __bch2_trans_unlock(struct btree_trans *trans)
{
	struct btree_path *path;
	unsigned i;

	trans_for_each_path(trans, path, i)
		__bch2_btree_path_unlock(trans, path);

	/*
	 * All locks dropped: submit any btree node writes queued in this
	 * trans's context.
	 */
	if (unlikely(trans->queued_write_bios))
		bch2_trans_submit_write_bios(trans);
}

noinline __cold
static int bch2_trans_relock_trace(struct btree_trans *trans)
{
	struct btree_path *path;
	unsigned i;

	trans_for_each_path(trans, path, i) {
		if (!path->should_be_locked)
			continue;

		struct get_locks_fail f;
		struct btree_path old_path = *path;
		int ret = btree_path_get_locks(trans, path, false, &f,
					       BCH_ERR_transaction_restart_relock);
		if (ret) {
			event_inc_trace(trans->c, trans_restart_relock, buf, ({
				prt_printf(&buf, "%s\n", trans->fn);
				get_locks_fail_to_text(&buf, trans, &old_path, path, &f);
			}));

			__bch2_trans_unlock(trans);
			bch2_trans_verify_locks(trans);
			return ret;
		}
	}

	trans_set_locked(trans, true);
	bch2_trans_verify_locks(trans);
	return 0;
}

int __bch2_trans_relock(struct btree_trans *trans, bool trace)
{
	bch2_trans_verify_locks(trans);

	if (unlikely(trans->restarted))
		return -((int) trans->restarted);
	if (unlikely(trace_trans_restart_relock_enabled() && trace))
		return bch2_trans_relock_trace(trans);

	struct btree_path *path;
	unsigned i;

	trans_for_each_path(trans, path, i) {
		if (!path->should_be_locked)
			continue;

		int ret = btree_path_get_locks(trans, path, false, NULL,
					       BCH_ERR_transaction_restart_relock);
		if (ret) {
			if (trace)
				event_inc(trans->c, trans_restart_relock);
			__bch2_trans_unlock(trans);
			bch2_trans_verify_locks(trans);
			return ret;
		}
	}

	trans_set_locked(trans, true);
	bch2_trans_verify_locks(trans);
	return 0;
}

int bch2_trans_relock_notrace(struct btree_trans *trans)
{
	return __bch2_trans_relock(trans, false);
}

void bch2_trans_unlock(struct btree_trans *trans)
{
	trans_set_unlocked(trans);

	__bch2_trans_unlock(trans);

	/*
	 * Drop the btree cache cannibalize lock too. Holding it across a
	 * trans_unlock - i.e. across a sleep - is the recipe for a resource
	 * deadlock: cannibalize-holder sleeps waiting on the allocator,
	 * allocator needs to grow the btree cache, growing the cache needs
	 * cannibalize, but we're holding it. Releasing on trans_unlock means
	 * cannibalize is only held over non-sleeping critical sections;
	 * callers that need it after a wake re-acquire normally.
	 */
	if (unlikely(trans->btree_cache_cannibalize_locked))
		bch2_btree_cache_cannibalize_unlock(trans);
}

void bch2_trans_unlock_long(struct btree_trans *trans)
{
	bch2_trans_unlock(trans);
	trans_enable_migrate(trans);

	if (trans->srcu_held) {
		struct bch_fs *c = trans->c;
		struct btree_path *path;
		unsigned i;

		trans_for_each_path(trans, path, i)
			if (path->cached && !btree_node_locked(path, 0))
				path->l[0].b = ERR_PTR(-BCH_ERR_no_btree_node_srcu_reset);

		if (unlikely(trans->srcu_held && time_after(jiffies, trans->srcu_lock_time + HZ * 10))) {
			CLASS(printbuf, buf)();

			prt_printf(&buf, "btree trans held srcu lock (delaying memory reclaim) for %lu seconds\n",
				   (jiffies - trans->srcu_lock_time) / HZ);
			bch2_sb_recent_counters_to_text(&buf, &trans->c->counters);

			WARN_RATELIMIT(true, "%s", buf.buf);
		}

		srcu_read_unlock(&c->btree.trans.barrier, trans->srcu_idx);
		trans->srcu_held = false;
	}
}

void bch2_trans_unlock_write(struct btree_trans *trans)
{
	struct btree_path *path;
	unsigned i;

	trans_for_each_path(trans, path, i)
		for (unsigned l = 0; l < BTREE_MAX_DEPTH; l++)
			if (btree_node_write_locked(path, l))
				bch2_btree_node_unlock_write(trans, path, path->l[l].b);
}

int __bch2_trans_mutex_lock(struct btree_trans *trans,
			    struct mutex *lock)
{
	int ret = drop_locks_do(trans, (mutex_lock(lock), 0));

	if (ret)
		mutex_unlock(lock);
	return ret;
}

/* Debug */

void __bch2_btree_path_verify_locks(struct btree_trans *trans, struct btree_path *path)
{
	if (!path->nodes_locked && btree_path_node(path, path->level)) {
		/*
		 * A path may be uptodate and yet have nothing locked if and only if
		 * there is no node at path->level, which generally means we were
		 * iterating over all nodes and got to the end of the btree
		 */
		BUG_ON(path->should_be_locked && trans->locked && !trans->restarted);
	}

	if (!path->nodes_locked)
		return;

	for (unsigned l = 0; l < BTREE_MAX_DEPTH; l++) {
		int want = btree_lock_want(path, l);
		int have = btree_node_locked_type_nowrite(path, l);

		BUG_ON(!is_btree_node(path, l) && have != BTREE_NODE_UNLOCKED);

		BUG_ON(is_btree_node(path, l) && want != have);

		BUG_ON(btree_node_locked(path, l) &&
		       path->l[l].lock_seq != six_lock_seq(&path->l[l].b->c.lock));
	}
}

static bool bch2_trans_locked(struct btree_trans *trans)
{
	struct btree_path *path;
	unsigned i;

	trans_for_each_path(trans, path, i)
		if (path->nodes_locked)
			return true;
	return false;
}

void __bch2_trans_verify_locks(struct btree_trans *trans)
{
	if (!trans->locked) {
		BUG_ON(bch2_trans_locked(trans));
		return;
	}

	struct btree_path *path;
	unsigned i;

	trans_for_each_path(trans, path, i)
		__bch2_btree_path_verify_locks(trans, path);
}
