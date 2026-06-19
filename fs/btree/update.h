/* SPDX-License-Identifier: GPL-2.0 */
// 备注：B-tree 更新操作接口定义
// 备注：
// 备注：本文件定义了 B-tree 的核心更新操作，包括：
// 备注：
// 备注：1. 键插入 (bch2_btree_insert)
// 备注：- 将新键插入 B-tree 叶子节点
// 备注：- 处理节点分裂和父节点更新
// 备注：
// 备注：2. 键删除 (btree_delete, btree_delete_at)
// 备注：- 从 B-tree 中删除指定键
// 备注：- 处理节点合并和父节点更新
// 备注：
// 备注：3. 键修改 (bch2_btree_insert_key_leaf)
// 备注：- 在叶子节点中直接修改键值
// 备注：- 触发器回调通知上层
// 备注：
// 备注：4. 节点刷新 (btree_node_flush0/flush1)
// 备注：- 将内存中的 bset 刷新到磁盘
// 备注：- 通过 journal pin 确保崩溃一致性
// 备注：
// 备注：事务提交标志：
// 备注：- no_enospc: 不检查空间不足
// 备注：- no_check_rw: 不获取写引用
// 备注：- no_journal_res: 不预留日志空间
// 备注：- journal_reclaim: 允许日志回收操作
// 备注：
// 备注：更新流程：
// 备注：1. bch2_trans_update -> 将更新加入事务队列
// 备注：2. btree_path_sort -> 按锁顺序排序路径
// 备注：3. bch2_trans_commit -> 逐层获取锁并应用更新
// 备注：4. btree_node_prep_for_write -> 准备节点写入
// 备注：5. bch2_btree_node_flush* -> 写入磁盘
#ifndef _BCACHEFS_BTREE_UPDATE_H
#define _BCACHEFS_BTREE_UPDATE_H

#include "btree/iter.h"
#include "journal/journal.h"
#include "sb/io.h"
#include "snapshots/snapshot.h"

struct bch_fs;
struct btree;

// 备注：bch2_btree_node_prep_for_write - 准备 B-tree 节点写入
// 备注：@trans: 事务上下文
// 备注：@path: B-tree 路径
// 备注：@b: B-tree 节点
// 备注：
// 备注：在写入节点前调用，执行以下操作：
// 备注：1. 检查节点是否需要重写 (need_rewrite)
// 备注：2. 压缩 bset：将多个 bset 合并
// 备注：3. 更新节点格式 (format)
// 备注：4. 分配必要的内存
// 备注：
// 备注：此函数确保节点数据在写入磁盘前是正确的
void bch2_btree_node_prep_for_write(struct btree_trans *,
				    struct btree_path *, struct btree *);

// 备注：bch2_btree_bset_insert_key - 向 bset 中插入键
// 备注：@trans: 事务上下文
// 备注：@path: B-tree 路径
// 备注：@b: B-tree 节点
// 备注：@iter: 节点迭代器
// 备注：@k: 要插入的键
// 备注：
// 备注：将键插入到 B-tree 节点的指定 bset 中：
// 备注：1. 在迭代器位置处插入新键
// 备注：2. 移动后续键为新键腾出空间
// 备注：3. 更新 nr_keys 计数
// 备注：
// 备注：如果 bset 空间不足，返回 false（需要分裂）
bool bch2_btree_bset_insert_key(struct btree_trans *, struct btree_path *,
				struct btree *, struct btree_node_iter *,
				struct bkey_i *);

// 备注：bch2_btree_node_flush0/flush1 - 刷新 bset 到磁盘
// 备注：@journal: 日志结构
// 备注：@pin: 日志条目 pin
// 备注：@seq: 日志序列号
// 备注：
// 备注：两个函数的区别：
// 备注：- flush0: 刷新第一个 bset (set[0])
// 备注：- flush1: 刷新第二个 bset (set[1])
// 备注：
// 备注：这些函数由 journal 回调调用，确保数据在崩溃时持久化
int bch2_btree_node_flush0(struct journal *, struct journal_entry_pin *, u64);
int bch2_btree_node_flush1(struct journal *, struct journal_entry_pin *, u64);

// 备注：bch2_btree_add_journal_pin - 添加日志 pin
// 备注：@c: 文件系统结构
// 备注：@b: B-tree 节点
// 备注：@seq: 日志序列号
// 备注：
// 备注：将节点与日志条目关联，确保：
// 备注：1. 节点写入前，日志已刷新
// 备注：2. 崩溃恢复时，节点与日志一致
void bch2_btree_add_journal_pin(struct bch_fs *, struct btree *, u64);

// 备注：bch2_btree_insert_key_leaf - 在叶子节点插入键
// 备注：@trans: 事务上下文
// 备注：@path: B-tree 路径
// 备注：@k: 要插入的键
// 备注：@journal_seq: 日志序列号
// 备注：
// 备注：叶子节点级别的键插入：
// 备注：1. 找到正确的插入位置
// 备注：2. 调用 btree_bset_insert_key
// 备注：3. 如果需要分裂，触发分裂逻辑
// 备注：4. 更新事务的待提交更新列表
void bch2_btree_insert_key_leaf(struct btree_trans *, struct btree_path *,
				struct bkey_i *, u64);

// 备注：BCH_TRANS_COMMIT_FLAGS - 事务提交标志定义
// 备注：
// 备注：用于控制提交行为的关键标志：
// 备注：
// 备注：no_enospc: 跳过空间检查，用于元数据操作
// 备注：no_check_rw: 不获取写引用，用于只读操作
// 备注：no_journal_res: 不预留日志，使用已有的 journal_res
// 备注：no_skip_noops: 不跳过空操作，用于调试
// 备注：journal_reclaim: 允许在日志回收时返回错误
// 备注：skip_accounting_apply: 跳过记账应用（重放时使用）
#define BCH_TRANS_COMMIT_FLAGS()							\
	x(no_enospc,	"don't check for enospc")					\
	x(no_check_rw,	"don't attempt to take a ref on c->writes")			\
	x(no_journal_res, "don't take a journal reservation, instead "			\
			"pin journal entry referred to by trans->journal_res.seq")	\
	x(no_skip_noops, "don't drop noop updates")					\
	x(journal_reclaim, "operation required for journal reclaim; may return error"	\
			"instead of deadlocking if BCH_WATERMARK_reclaim not specified")\
	x(journal_replay, "in journal replay")						\
	x(skip_accounting_apply, "we're in journal replay - accounting updates have already been applied")

enum __bch_trans_commit_flags {
	/* First bits for bch_watermark: */
	__BCH_TRANS_COMMIT_FLAGS_START = BCH_WATERMARK_BITS,
#define x(n, ...)	__BCH_TRANS_COMMIT_##n,
	BCH_TRANS_COMMIT_FLAGS()
#undef x
};

enum bch_trans_commit_flags {
#define x(n, ...)	BCH_TRANS_COMMIT_##n = BIT(__BCH_TRANS_COMMIT_##n),
	BCH_TRANS_COMMIT_FLAGS()
#undef x
};

// 备注：bch2_trans_commit_flags_to_text - 将提交标志转换为文本
// 备注：@out: 输出缓冲区
// 备注：@flags: 提交标志
void bch2_trans_commit_flags_to_text(struct printbuf *, enum bch_trans_commit_flags);

// 备注：bch2_btree_delete_at - 在指定迭代器位置删除键
// 备注：@trans: 事务上下文
// 备注：@iter: B-tree 迭代器
// 备注：@flags: 更新触发器标志
// 备注：
// 备注：删除迭代器当前位置的键：
// 备注：1. 获取键的引用用于触发器
// 备注：2. 从 bset 中删除键
// 备注：3. 标记 whiteout（可选）
// 备注：4. 执行 overwrite 触发器
// 备注：
// 备注：返回值：成功返回 0，失败返回错误码
int bch2_btree_delete_at(struct btree_trans *, struct btree_iter *,
			 enum btree_iter_update_trigger_flags);

// 备注：bch2_btree_delete - 删除指定位置的键
// 备注：@trans: 事务上下文
// 备注：@id: B-tree ID
// 备注：@pos: 删除位置
// 备注：@flags: 更新触发器标志
// 备注：
// 备注：高级删除接口：
// 备注：1. 创建/获取迭代器
// 备注：2. 定位到目标位置
// 备注：3. 调用 btree_delete_at
// 备注：
// 备注：简化使用场景：无需保留迭代器时使用
int bch2_btree_delete(struct btree_trans *, enum btree_id, struct bpos,
		      enum btree_iter_update_trigger_flags);

// 备注：bch2_btree_insert_nonextent - 插入非扩展类型键
// 备注：@trans: 事务上下文
// 备注：@id: B-tree ID
// 备注：@k: 要插入的键
// 备注：@flags: 更新触发器标志
// 备注：
// 备注：用于插入索引、目录项、扩展属性等非扩展数据：
// 备注：- inode B-tree
// 备注：- dirent B-tree
// 备注：- xattr B-tree
// 备注：
// 备注：扩展数据使用专门的 extent 插入接口
int bch2_btree_insert_nonextent(struct btree_trans *, enum btree_id,
				struct bkey_i *, unsigned,
				enum btree_iter_update_trigger_flags);

// 备注：bch2_btree_insert_trans - 事务内插入键
// 备注：@trans: 事务上下文
// 备注：@id: B-tree ID
// 备注：@k: 要插入的键
// 备注：@flags: 更新触发器标志
// 备注：
// 备注：事务内的键插入：
// 备注：1. 验证键格式和范围
// 备注：2. 分配路径（如需要）
// 备注：3. 获取锁
// 备注：4. 添加到事务更新队列
// 备注：
// 备注：注意：调用者负责后续提交
int bch2_btree_insert_trans(struct btree_trans *, enum btree_id, struct bkey_i *,
			enum btree_iter_update_trigger_flags);

// 备注：bch2_btree_insert - 完整的事务插入接口
// 备注：@c: 文件系统结构
// 备注：@id: B-tree ID
// 备注：@k: 要插入的键
// 备注：@disk_res: 磁盘空间预留
// 备注：@commit_flags: 提交标志
// 备注：@trigger_flags: 触发器标志
// 备注：
// 备注：一站式插入接口：
// 备注：1. 获取事务
// 备注：2. 调用 btree_insert_trans
// 备注：3. 提交事务
// 备注：4. 释放事务
// 备注：
// 备注：适用于不需要精细控制的简单场景
int bch2_btree_insert(struct bch_fs *, enum btree_id, struct bkey_i *,
		      struct disk_reservation *,
		      enum bch_trans_commit_flags,
		      enum btree_iter_update_trigger_flags);

// 备注：bch2_btree_delete_range_trans - 事务内删除范围
// 备注：@trans: 事务上下文
// 备注：@id: B-tree ID
// 备注：@start: 起始位置
// 备注：@end: 结束位置
// 备注：@flags: 触发器标志
// 备注：
// 备注：删除指定范围内的所有键：
// 备注：1. 使用迭代器遍历范围
// 备注：2. 对每个键调用 btree_delete_at
// 备注：3. 处理可能的部分删除
int bch2_btree_delete_range_trans(struct btree_trans *, enum btree_id,
				  struct bpos, struct bpos,
				  enum btree_iter_update_trigger_flags);

// 备注：bch2_btree_delete_range - 完整的事务范围删除接口
// 备注：@c: 文件系统结构
// 备注：@id: B-tree ID
// 备注：@start: 起始位置
// 备注：@end: 结束位置
// 备注：@flags: 触发器标志
// 备注：
// 备注：一站式范围删除：
// 备注：1. 获取事务
// 备注：2. 调用 btree_delete_range_trans
// 备注：3. 提交事务
int bch2_btree_delete_range(struct bch_fs *, enum btree_id,
			    struct bpos, struct bpos,
			    enum btree_iter_update_trigger_flags);

// 备注：bch2_btree_bit_mod_iter - 在迭代器位置修改位
// 备注：@trans: 事务上下文
// 备注：@iter: B-tree 迭代器
// 备注：@set: true 设置位，false 清除位
// 备注：
// 备注：用于修改特殊标志位（如 whiteout 标记）：
// 备注：- 比完整键插入/删除更高效
// 备注：- 只修改一个位的值
int bch2_btree_bit_mod_iter(struct btree_trans *, struct btree_iter *, bool);

// 备注：bch2_btree_bit_mod - 在指定位置修改位
// 备注：@trans: 事务上下文
// 备注：@id: B-tree ID
// 备注：@pos: 位置
// 备注：@set: true 设置位，false 清除位
// 备注：
// 备注：简化接口：自动创建迭代器
int bch2_btree_bit_mod(struct btree_trans *, enum btree_id, struct bpos, bool);

// 备注：bch2_btree_bit_mod_buffered - 缓冲的位修改
// 备注：@trans: 事务上下文
// 备注：@id: B-tree ID
// 备注：@pos: 位置
// 备注：@set: true 设置位，false 清除位
// 备注：
// 备注：延迟写入：修改记录在 write_buffer 中，
// 备注：稍后批量刷新到磁盘（提高性能）
int bch2_btree_bit_mod_buffered(struct btree_trans *, enum btree_id, struct bpos, bool);

// 备注：bch2_btree_delete_at_buffered - 缓冲的键删除
// 备注：
// 备注：使用 bit_mod_buffered 实现延迟删除
static inline int bch2_btree_delete_at_buffered(struct btree_trans *trans,
						enum btree_id btree, struct bpos pos)
{
	return bch2_btree_bit_mod_buffered(trans, btree, pos, false);
}

// 备注：__bch2_insert_snapshot_whiteouts - 插入快照白名单
// 备注：@trans: 事务上下文
// 备注：@id: B-tree ID
// 备注：@pos: 位置
// 备注：@snapids: 快照 ID 列表
// 备注：
// 备注：在快照场景中插入白名单键：
// 备注：- 当键在某个快照中不可见时，插入白名单标记
// 备注：- 白名单确保正确处理快照隔离
int __bch2_insert_snapshot_whiteouts(struct btree_trans *, enum btree_id,
				     struct bpos, snapshot_id_list *);

/*
 * For use when splitting extents in existing snapshots:
 *
 * If @old_pos is an interior snapshot node, iterate over descendent snapshot
 * nodes: for every descendent snapshot in whiche @old_pos is overwritten and
 * not visible, emit a whiteout at @new_pos.
 */
// 备注：bch2_insert_snapshot_whiteouts - 快照白名单插入（简化接口）
// 备注：
// 备注：用于在现有快照中分裂扩展时：
// 备注：- 遍历目标快照的所有后代快照
// 备注：- 对于被覆盖且不可见的位置，插入白名单
// 备注：
// 备注：场景示例：
// 备注：- 快照树中有 A -> B -> C
// 备注：- 修改 A 中的键会影响 B 和 C
// 备注：- 需要在 B 和 C 对应位置插入白名单
static inline int bch2_insert_snapshot_whiteouts(struct btree_trans *trans,
						 enum btree_id btree,
						 struct bpos old_pos,
						 struct bpos new_pos)
{
	BUG_ON(old_pos.snapshot != new_pos.snapshot);

	if (!btree_type_has_snapshots(btree) ||
	    bkey_eq(old_pos, new_pos))
		return 0;

	CLASS(snapshot_id_list, s)();
	try(bch2_get_snapshot_overwrites(trans, btree, old_pos, &s));

	return s.nr
		? __bch2_insert_snapshot_whiteouts(trans, btree, new_pos, &s)
		: 0;
}

static inline enum bch_bkey_type extent_whiteout_type(struct bch_fs *c, enum btree_id btree,
						      const struct bkey *k)
{
	/*
	 * KEY_TYPE_extent_whiteout indicates that there isn't a real extent
	 * present at that position: key start positions inclusive of
	 * KEY_TYPE_extent_whiteout (but not KEY_TYPE_whiteout) are
	 * monotonically increasing
	 */
	return btree_id_is_extents_snapshots(btree) &&
		bkey_deleted(k) &&
		bch2_snapshot_is_leaf(c, k->p.snapshot) &&
		!bch2_request_incompat_feature(c, bcachefs_metadata_version_extent_snapshot_whiteouts)
		? KEY_TYPE_extent_whiteout
		: KEY_TYPE_whiteout;
}

int bch2_trans_update_extent_overwrite(struct btree_trans *, struct btree_iter *,
				       enum btree_iter_update_trigger_flags,
				       struct bkey_s_c, struct bkey_s_c);

int bch2_bkey_get_empty_slot(struct btree_trans *, struct btree_iter *,
			     enum btree_id, struct bpos, struct bpos);

// 备注：bch2_trans_update_ip() - 添加更新到事务队列(内部实现)
// 备注：
// 备注：这是 bch2_trans_update() 的内部实现，通常不应直接调用。
// 备注：使用 _THIS_IP_ 宏记录调用点用于调试。
int __must_check bch2_trans_update_ip(struct btree_trans *, struct btree_iter *,
				      struct bkey_i *, unsigned,
				      enum btree_iter_update_trigger_flags,
				      unsigned long);

int bch2_trigger_get_mutable_new(struct btree_trans *,
				 struct btree_trigger_op,
				 unsigned needed_u64s,
				 struct bkey_s *);

static inline int __must_check
bch2_trans_update_buf(struct btree_trans *trans, struct btree_iter *iter,
		      struct bkey_i *k, unsigned k_buf_u64s,
		      enum btree_iter_update_trigger_flags flags)
{
	return bch2_trans_update_ip(trans, iter, k, k_buf_u64s, flags, _THIS_IP_);
}

// 备注：bch2_trans_update() - 将键更新添加到事务队列
// 备注：@trans: 当前事务
// 备注：@iter: 指向要更新位置的迭代器(确定btree_id, pos, path)
// 备注：@k: 新的键值(将被复制或引用)
// 备注：@flags: 更新标志(BTREE_UPDATE_*)
// 备注：
// 备注：Returns: 0 成功，负值错误码失败(如内存不足)
// 备注：
// 备注：这是事务更新的主要接口。调用此函数不会立即写入B树，
// 备注：而是将更新加入 trans->updates 队列，在 bch2_trans_commit() 时原子应用。
// 备注：
// 备注：工作机制:
// 备注：1. 根据 iter->path 找到对应的 btree_path
// 备注：2. 创建 btree_insert_entry 记录更新信息
// 备注：3. 将 entry 插入 trans->updates 数组(保持排序)
// 备注：4. 增加 path 的引用计数防止被释放
// 备注：5. 缓存旧键值用于触发器和日志
// 备注：
// 备注：注意:
// 备注：- 同一位置的多次更新会合并(覆盖旧entry)
// 备注：- 更新在提交前不会生效
// 备注：- 事务重启后 updates 队列会被清空
static inline int __must_check
bch2_trans_update(struct btree_trans *trans, struct btree_iter *iter,
		  struct bkey_i *k, enum btree_iter_update_trigger_flags flags)
{
	return bch2_trans_update_ip(trans, iter, k, k->k.u64s, flags, _THIS_IP_);
}

static inline void *btree_trans_subbuf_base(struct btree_trans *trans,
					    struct btree_trans_subbuf *buf)
{
	return (u64 *) trans->mem + buf->base;
}

static inline void *btree_trans_subbuf_top(struct btree_trans *trans,
					   struct btree_trans_subbuf *buf)
{
	return (u64 *) trans->mem + buf->base + buf->u64s;
}

void *__bch2_trans_subbuf_alloc(struct btree_trans *,
				struct btree_trans_subbuf *,
				unsigned, ulong);

int bch2_trans_subbuf_reserve(struct btree_trans *,
			      struct btree_trans_subbuf *,
			      unsigned);

static inline void *
bch2_trans_subbuf_alloc_ip(struct btree_trans *trans,
			   struct btree_trans_subbuf *buf,
			   unsigned u64s, ulong ip)
{
	if (buf->u64s + u64s > buf->size)
		return __bch2_trans_subbuf_alloc(trans, buf, u64s, ip);

	void *p = btree_trans_subbuf_top(trans, buf);
	buf->u64s += u64s;
	return p;
}

static inline void *
bch2_trans_subbuf_alloc(struct btree_trans *trans,
			struct btree_trans_subbuf *buf,
			unsigned u64s)
{
	return bch2_trans_subbuf_alloc_ip(trans, buf, u64s, _THIS_IP_);
}

static inline struct jset_entry *btree_trans_journal_entries_start(struct btree_trans *trans)
{
	return btree_trans_subbuf_base(trans, &trans->journal_entries);
}

static inline struct jset_entry *btree_trans_journal_entries_top(struct btree_trans *trans)
{
	return btree_trans_subbuf_top(trans, &trans->journal_entries);
}

static inline struct jset_entry *
bch2_trans_jset_entry_alloc_ip(struct btree_trans *trans, unsigned u64s, ulong ip)
{
	return bch2_trans_subbuf_alloc_ip(trans, &trans->journal_entries, u64s, ip);
}

static inline struct jset_entry *
bch2_trans_jset_entry_alloc(struct btree_trans *trans, unsigned u64s)
{
	return bch2_trans_jset_entry_alloc_ip(trans, u64s, _THIS_IP_);
}

int bch2_btree_insert_clone_trans(struct btree_trans *, enum btree_id, struct bkey_i *);

int bch2_btree_write_buffer_insert_err(struct bch_fs *, enum btree_id, struct bkey_i *);

static inline int bch2_btree_write_buffer_insert_checks(struct bch_fs *c, enum btree_id btree,
							struct bkey_i *k)
{
	if (unlikely(!btree_type_uses_write_buffer(btree)))
		try(bch2_btree_write_buffer_insert_err(c, btree, k));

	return 0;
}

static inline int __must_check bch2_trans_update_buffered(struct btree_trans *trans,
					    enum btree_id btree,
					    struct bkey_i *k)
{
	kmsan_check_memory(k, bkey_bytes(&k->k));

	try(bch2_btree_write_buffer_insert_checks(trans->c, btree, k));

	/*
	 * Most updates skip the btree write buffer until journal replay is
	 * finished because synchronization with journal replay relies on having
	 * a btree node locked - if we're overwriting a key in the journal that
	 * journal replay hasn't yet replayed, we have to mark it as
	 * overwritten.
	 *
	 * But accounting updates don't overwrite, they're deltas, and they have
	 * to be flushed to the btree strictly in order for journal replay to be
	 * able to tell which updates need to be applied:
	 */
	if (k->k.type != KEY_TYPE_accounting &&
	    btree != BTREE_ID_need_discard &&
	    unlikely(trans->journal_replay_not_finished))
		return bch2_btree_insert_clone_trans(trans, btree, k);

	struct jset_entry *e = errptr_try(bch2_trans_jset_entry_alloc(trans, jset_u64s(k->k.u64s)));

	journal_entry_init(e, BCH_JSET_ENTRY_write_buffer_keys, btree, 0, k->k.u64s);
	bkey_copy(e->start, k);
	return 0;
}

void bch2_trans_commit_hook(struct btree_trans *,
			    struct btree_trans_commit_hook *);
int __bch2_trans_commit(struct btree_trans *, enum bch_trans_commit_flags);

int bch2_trans_log_str(struct btree_trans *, const char *);
int bch2_trans_log_msg(struct btree_trans *, struct printbuf *);
int bch2_trans_log_bkey(struct btree_trans *, enum btree_id, unsigned, struct bkey_i *);

__printf(2, 3) int bch2_fs_log_msg(struct bch_fs *, const char *, ...);
__printf(2, 3) int bch2_journal_log_msg(struct bch_fs *, const char *, ...);

#define trans_for_each_update(_trans, _i)				\
	for (struct btree_insert_entry *_i = (_trans)->updates;		\
	     (_i) < (_trans)->updates + (_trans)->nr_updates;		\
	     (_i)++)

static inline bool bch2_trans_has_updates(struct btree_trans *trans)
{
	return trans->nr_updates ||
		trans->journal_entries.u64s ||
		trans->accounting.u64s;
}

static inline void bch2_trans_reset_updates(struct btree_trans *trans)
{
	trans_for_each_update(trans, i)
		bch2_path_put(trans, i->path, true);

	trans->nr_updates		= 0;
	trans->journal_entries.u64s	= 0;
	trans->journal_entries.size	= 0;
	trans->accounting.u64s		= 0;
	trans->accounting.size		= 0;
	trans->hooks			= NULL;
	trans->extra_disk_res		= 0;
	trans->extra_journal_u64s	= 0;
	trans->has_interior_updates	= 0;
}

/**
 * bch2_trans_commit - insert keys at given iterator positions
 *
 * This is main entry point for btree updates.
 *
 * Return values:
 * -EROFS: filesystem read only
 * -EIO: journal or btree node IO error
 */
// 备注：bch2_trans_commit() - 事务提交主入口函数
// 备注：@trans: 要提交的事务
// 备注：@disk_res: 磁盘空间预留(可为NULL)
// 备注：@journal_seq: 输出参数，返回日志序列号(可为NULL)
// 备注：@flags: 提交标志(BCH_TRANS_COMMIT_*)
// 备注：
// 备注：Returns:
// 备注：0      - 成功
// 备注：-EROFS - 文件系统只读
// 备注：-EIO   - 日志或 btree 节点 IO 错误
// 备注：-ENOMEM- 内存不足
// 备注：-E...  - 其他错误码
// 备注：
// 备注：这是 btree 更新的主要入口点。调用前必须通过 bch2_trans_update()
// 备注：将更新添加到事务队列。此函数原子性地应用所有待处理更新。
// 备注：
// 备注：调用示例:
// 备注：CLASS(btree_trans, trans)(c);
// 备注：lockrestart_do(trans, {
// 备注：// 执行 btree 操作
// 备注：bch2_trans_iter_init(trans, &iter, BTREE_ID_extents, pos, 0);
// 备注：k = bch2_btree_iter_peek(&iter);
// 备注：// ... 修改 k ...
// 备注：ret = bch2_trans_update(trans, &iter, new_k, 0);
// 备注：bch2_trans_iter_exit(&iter);
// 备注：// 提交
// 备注：ret ?: bch2_trans_commit(trans, disk_res, &journal_seq, 0);
// 备注：});
// 备注：
// 备注：注意: 通常在 lockrestart_do 宏中使用，自动处理事务重启
static inline int bch2_trans_commit(struct btree_trans *trans,
				    struct disk_reservation *disk_res,
				    u64 *journal_seq,
				    enum bch_trans_commit_flags flags)
{
	trans->disk_res		= disk_res;
	trans->journal_seq	= journal_seq;
	trans->flush		= NULL;

	return __bch2_trans_commit(trans, flags);
}

static inline int bch2_trans_commit_flush(struct btree_trans *trans,
					  struct disk_reservation *disk_res,
					  u64 *journal_seq,
					  struct closure *flush,
					  enum bch_trans_commit_flags flags)
{
	trans->disk_res		= disk_res;
	trans->journal_seq	= journal_seq;
	trans->flush		= flush;

	return __bch2_trans_commit(trans, flags);
}

static inline int bch2_trans_commit_lazy(struct btree_trans *trans,
					 struct disk_reservation *disk_res,
					 u64 *journal_seq,
					 unsigned flags)
{
	return bch2_trans_has_updates(trans)
		? (bch2_trans_commit(trans, disk_res, journal_seq, flags) ?:
		   bch_err_throw(trans->c, transaction_restart_commit))
		: 0;
}

#define commit_do(_trans, _disk_res, _journal_seq, _flags, _do)	\
	lockrestart_do(_trans, _do ?: bch2_trans_commit(_trans, (_disk_res),\
					(_journal_seq), (_flags)))

#define nested_commit_do(_trans, _disk_res, _journal_seq, _flags, _do)	\
	nested_lockrestart_do(_trans, _do ?: bch2_trans_commit(_trans, (_disk_res),\
					(_journal_seq), (_flags)))

/* deprecated, prefer CLASS(btree_trans) */
#define bch2_trans_commit_do(_c, _disk_res, _journal_seq, _flags, _do)		\
	bch2_trans_run(_c, commit_do(trans, _disk_res, _journal_seq, _flags, _do))

static __always_inline struct bkey_i *__bch2_bkey_make_mut_noupdate(struct btree_trans *trans, struct bkey_s_c k,
						  unsigned type, unsigned min_bytes)
{
	unsigned bytes = max_t(unsigned, min_bytes, bkey_bytes(k.k));
	struct bkey_i *mut;

	if (type && k.k->type != type)
		return ERR_PTR(-ENOENT);

	/* extra padding for varint_decode_fast... */
	mut = bch2_trans_kmalloc_nomemzero(trans, bytes + 8);
	if (!IS_ERR(mut)) {
		bkey_reassemble(mut, k);

		if (unlikely(bytes > bkey_bytes(k.k))) {
			memset((void *) mut + bkey_bytes(k.k), 0,
			       bytes - bkey_bytes(k.k));
			mut->k.u64s = DIV_ROUND_UP(bytes, sizeof(u64));
		}
	}
	return mut;
}

static __always_inline struct bkey_i *bch2_bkey_make_mut_noupdate(struct btree_trans *trans, struct bkey_s_c k)
{
	return __bch2_bkey_make_mut_noupdate(trans, k, 0, 0);
}

#define bch2_bkey_make_mut_noupdate_typed(_trans, _k, _type)		\
	bkey_i_to_##_type(__bch2_bkey_make_mut_noupdate(_trans, _k,	\
				KEY_TYPE_##_type, sizeof(struct bkey_i_##_type)))

static inline struct bkey_i *__bch2_bkey_make_mut(struct btree_trans *trans, struct btree_iter *iter,
					struct bkey_s_c *k,
					enum btree_iter_update_trigger_flags flags,
					unsigned type, unsigned min_bytes)
{
	struct bkey_i *mut = __bch2_bkey_make_mut_noupdate(trans, *k, type, min_bytes);
	if (IS_ERR(mut))
		return mut;

	int ret = bch2_trans_update(trans, iter, mut, flags);
	if (ret)
		return ERR_PTR(ret);

	*k = bkey_i_to_s_c(mut);
	return mut;
}

static inline struct bkey_i *bch2_bkey_make_mut(struct btree_trans *trans,
						struct btree_iter *iter, struct bkey_s_c *k,
						enum btree_iter_update_trigger_flags flags)
{
	return __bch2_bkey_make_mut(trans, iter, k, flags, 0, 0);
}

#define bch2_bkey_make_mut_typed(_trans, _iter, _k, _flags, _type)	\
	bkey_i_to_##_type(__bch2_bkey_make_mut(_trans, _iter, _k, _flags,\
				KEY_TYPE_##_type, sizeof(struct bkey_i_##_type)))

static inline struct bkey_i *__bch2_bkey_get_mut_noupdate(struct btree_iter *iter,
					 unsigned type, unsigned min_bytes)
{
	struct bkey_s_c k = __bch2_bkey_get_typed(iter, type);
	return IS_ERR(k.k)
		? ERR_CAST(k.k)
		: __bch2_bkey_make_mut_noupdate(iter->trans, k, 0, min_bytes);
}

static inline struct bkey_i *bch2_bkey_get_mut_noupdate(struct btree_iter *iter)
{
	return __bch2_bkey_get_mut_noupdate(iter, 0, 0);
}

static inline struct bkey_i *__bch2_bkey_get_mut(struct btree_trans *trans,
					 enum btree_id btree, struct bpos pos,
					 enum btree_iter_update_trigger_flags flags,
					 unsigned type, unsigned min_bytes)
{
	CLASS(btree_iter, iter)(trans, btree, pos, flags|BTREE_ITER_intent);
	struct bkey_i *mut = __bch2_bkey_get_mut_noupdate(&iter, type, min_bytes);
	if (IS_ERR(mut))
		return mut;
	int ret = bch2_trans_update(trans, &iter, mut, flags);
	if (ret)
		return ERR_PTR(ret);
	return mut;
}

static inline struct bkey_i *bch2_bkey_get_mut_minsize(struct btree_trans *trans,
						       unsigned btree_id, struct bpos pos,
						       enum btree_iter_update_trigger_flags flags,
						       unsigned min_bytes)
{
	return __bch2_bkey_get_mut(trans, btree_id, pos, flags, 0, min_bytes);
}

static inline struct bkey_i *bch2_bkey_get_mut(struct btree_trans *trans,
					       unsigned btree_id, struct bpos pos,
					       enum btree_iter_update_trigger_flags flags)
{
	return __bch2_bkey_get_mut(trans, btree_id, pos, flags, 0, 0);
}

#define bch2_bkey_get_mut_typed(_trans, _btree_id, _pos, _flags, _type)			\
	bkey_i_to_##_type(__bch2_bkey_get_mut(_trans, _btree_id, _pos, _flags,		\
			KEY_TYPE_##_type, sizeof(struct bkey_i_##_type)))

static inline struct bkey_i *__bch2_bkey_alloc(struct btree_trans *trans, struct btree_iter *iter,
					       enum btree_iter_update_trigger_flags flags,
					       unsigned type, unsigned val_size)
{
	struct bkey_i *k = bch2_trans_kmalloc(trans, sizeof(*k) + val_size);
	if (IS_ERR(k))
		return k;

	bkey_init(&k->k);
	k->k.p = iter->pos;
	k->k.type = type;
	set_bkey_val_bytes(&k->k, val_size);

	int ret = bch2_trans_update(trans, iter, k, flags);
	if (unlikely(ret))
		return ERR_PTR(ret);
	return k;
}

#define bch2_bkey_alloc(_trans, _iter, _flags, _type)			\
	bkey_i_to_##_type(__bch2_bkey_alloc(_trans, _iter, _flags,	\
				KEY_TYPE_##_type, sizeof(struct bch_##_type)))

#endif /* _BCACHEFS_BTREE_UPDATE_H */
