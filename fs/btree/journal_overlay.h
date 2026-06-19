/* SPDX-License-Identifier: GPL-2.0 */
// 备注：B-tree Journal Overlay - 日志覆盖迭代器
// 备注：
// 备注：本文件定义了日志覆盖机制，用于在读取 B-tree 时同时考虑：
// 备注：1. B-tree 中已持久化的键
// 备注：2. Journal 中未持久化的待应用键
// 备注：
// 备注：工作原理：
// 备注：- Journal 中记录了已提交但未刷写到磁盘的键更新
// 备注：- 读取时需要将 Journal 中的"覆盖"在 B-tree 键之上
// 备注：- 这样读取时看到的是最新的一致性视图
// 备注：
// 备注：数据结构：
// 备注：- journal_iter: 管理 Journal 键的迭代
// 备注：- btree_and_journal_iter: 组合 B-tree 和 Journal 的统一迭代器
// 备注：
// 备注：使用场景：
// 备注：- 崩溃恢复后，Journal 中的键需要"覆盖" B-tree
// 备注：- 读取操作需要返回最新的键版本（Journal 优先于 B-tree）
// 备注：- 事务提交后，键先进入 Journal，稍后刷新到 B-tree
// 备注：
// 备注：关键操作：
// 备注：- bch2_journal_keys_peek_max: 从 Journal 中获取最大的键
// 备注：- bch2_journal_keys_peek_prev_min: 获取前一个最小键
// 备注：- bch2_journal_keys_peek_slot: 获取指定位置的键
// 备注：- btree_and_journal_iter_prefetch: 预取优化
#ifndef _BCACHEFS_BTREE_JOURNAL_ITER_H
#define _BCACHEFS_BTREE_JOURNAL_ITER_H

#include "btree/bkey.h"

// 备注：struct journal_iter - Journal 键迭代器
// 备注：
// 备注：管理 Journal 中特定 B-tree 和层级的键：
// 备注：- list: 用于挂载到全局迭代列表
// 备注：- btree_id: 目标 B-tree ID
// 备注：- level: 目标层级（0 表示叶子）
// 备注：- idx: 当前键索引
// 备注：- keys: Journal 键数组
struct journal_iter {
	// 备注：迭代器链表
	struct list_head	list;
	/* B-tree ID */
	enum btree_id		btree_id;
	// 备注：层级
	unsigned		level;
	// 备注：当前索引
	size_t			idx;
	// 备注：Journal 键数组
	struct journal_keys	*keys;
};

/*
 * Iterate over keys in the btree, with keys from the journal overlaid on top:
 */

// 备注：struct btree_and_journal_iter - B-tree + Journal 组合迭代器
// 备注：
// 备注：统一的迭代接口，同时遍历：
// 备注：- B-tree 节点中的持久化键
// 备注：- Journal 中的未持久化键
// 备注：
// 备注：读取优先级：
// 备注：1. 如果 Journal 中有对应键，返回 Journal 版本（更新）
// 备注：2. 否则返回 B-tree 中的键（已持久化）
// 备注：
// 备注：字段说明：
// 备注：- trans: 事务上下文
// 备注：- b: 当前 B-tree 节点
// 备注：- node_iter: B-tree 节点内迭代器
// 备注：- unpacked: 解压后的键缓存
// 备注：- journal: Journal 迭代器
// 备注：- pos: 当前遍历位置
// 备注：- at_end: 是否到达结束
// 备注：- prefetch: 是否预取下一个节点
// 备注：- fail_if_too_many_whiteouts: 是否在过多 whiteout 时失败
struct btree_and_journal_iter {
	// 备注：事务上下文
	struct btree_trans	*trans;
	// 备注：当前 B-tree 节点
	struct btree		*b;
	// 备注：节点内迭代器
	struct btree_node_iter	node_iter;
	// 备注：解压后的键
	struct bkey		unpacked;

	struct journal_iter	journal;
	// 备注：当前位置
	struct bpos		pos;
	// 备注：结束标志
	bool			at_end;
	// 备注：预取标志
	bool			prefetch;
	// 备注：whiteout 过多是否报错
	bool			fail_if_too_many_whiteouts;
};

// 备注：journal_entry_radix_idx - 计算 Journal 条目的索引
// 备注：
// 备注：通过序列号计算在 radix 数组中的索引位置：
// 备注：idx = seq - base_seq
// 备注：
// 备注：这允许 O(1) 快速查找 Journal 条目
static inline u32 journal_entry_radix_idx(struct bch_fs *c, u64 seq)
{
	return seq - c->journal_entries_base_seq;
}

// 备注：journal_key_k - 获取 Journal 键对应的实际键数据
// 备注：
// 备注：Journal 键可以有两种存储方式：
// 备注：1. allocated: 单独分配的键（allocated_k 指针）
// 备注：2. embedded: 嵌入在 journal_replay 中的键（通过 offset 计算）
// 备注：
// 备注：此函数处理两种情况，返回正确的键指针
static inline struct bkey_i *journal_key_k(struct bch_fs *c,
					   const struct journal_key *k)
{
	if (k->allocated)
		return k->allocated_k;

	struct journal_replay *i = *genradix_ptr(&c->journal_entries, k->journal_seq_offset);

	return (struct bkey_i *) (i->j._data + k->journal_offset);
}

// 备注：__journal_key_btree_cmp - 比较 Journal 键的 B-tree 和层级
// 备注：
// 备注：用于 Journal 键排序：先按层级，再按 B-tree ID
static inline int __journal_key_btree_cmp(enum btree_id	l_btree_id,
					  unsigned	l_level,
					  const struct journal_key *r)
{
	return -cmp_int(l_level,	r->level) ?:
		cmp_int(l_btree_id,	r->btree_id);
}

// 备注：__journal_key_cmp - 比较 Journal 键的完整顺序
// 备注：
// 备注：比较顺序：btree_id -> level -> position
// 备注：
// 备注：用于在 Journal 键数组中查找和排序
static inline int __journal_key_cmp(struct bch_fs *c,
				    enum btree_id	l_btree_id,
				    unsigned		l_level,
				    struct bpos	l_pos,
				    const struct journal_key *r)
{
	return __journal_key_btree_cmp(l_btree_id, l_level, r) ?:
		bpos_cmp(l_pos, journal_key_k(c, r)->k.p);
}

// 备注：journal_key_cmp - 比较两个 Journal 键
// 备注：
// 备注：完整比较：btree_id + level + position
static inline int journal_key_cmp(struct bch_fs *c,
				  const struct journal_key *l, const struct journal_key *r)
{
	return __journal_key_cmp(c, l->btree_id, l->level,
				 journal_key_k(c, l)->k.p, r);
}

// 备注：bch2_journal_keys_peek_max - 获取最大的 Journal 键
// 备注：
// 备注：在指定范围内查找最大的键：
// 备注：- btree_id + level 确定 B-tree 和层级
// 备注：- min_pos 和 max_pos 限定位置范围
// 备注：- idx 返回找到的键索引
// 备注：
// 备注：返回：找到的键指针，如果不存在返回 NULL
const struct bkey_i *bch2_journal_keys_peek_max(struct bch_fs *, enum btree_id,
				unsigned, struct bpos, struct bpos, size_t *);

// 备注：bch2_journal_keys_peek_prev_min - 获取前一个最小的 Journal 键
// 备注：
// 备注：用于反向遍历：找到指定位置之前的最大键
const struct bkey_i *bch2_journal_keys_peek_prev_min(struct bch_fs *, enum btree_id,
				unsigned, struct bpos, struct bpos, size_t *);

// 备注：bch2_journal_keys_peek_slot - 获取指定位置的 Journal 键
// 备注：
// 备注：精确查找：返回完全匹配 position 的键
// 备注：用于更新覆盖场景：找到要更新的键
const struct bkey_i *bch2_journal_keys_peek_slot(struct bch_fs *, enum btree_id,
					   unsigned, struct bpos);

// 备注：bch2_btree_and_journal_iter_prefetch - 预取 B-tree 和 Journal 迭代器
// 备注：
// 备注：优化读取性能：
// 备注：- 预加载下一个 B-tree 节点
// 备注：- 预取 Journal 键数据
// 备注：
// 备注：减少 I/O 等待，提高吞吐量

int bch2_btree_and_journal_iter_prefetch(struct btree_trans *, struct btree_path *,
					 struct btree_and_journal_iter *);

int bch2_journal_key_insert_take(struct bch_fs *, enum btree_id,
				 unsigned, struct bkey_i *);
int bch2_journal_key_insert(struct bch_fs *, enum btree_id,
			    unsigned, struct bkey_i *);
int bch2_journal_key_delete(struct bch_fs *, enum btree_id,
			    unsigned, struct bpos);
bool bch2_key_deleted_in_journal(struct btree_trans *, enum btree_id, unsigned, struct bpos);
int bch2_journal_key_check_or_overwrite(struct bch_fs *, enum btree_id, unsigned,
					struct bpos, bool);

void bch2_btree_and_journal_iter_advance(struct btree_and_journal_iter *);
struct bkey_s_c bch2_btree_and_journal_iter_peek(struct bch_fs *, struct btree_and_journal_iter *);

void bch2_btree_and_journal_iter_exit(struct btree_and_journal_iter *);
void __bch2_btree_and_journal_iter_init_node_iter(struct btree_trans *,
				struct btree_and_journal_iter *, struct btree *,
				struct btree_node_iter, struct bpos);
void bch2_btree_and_journal_iter_init_node_iter(struct btree_trans *,
				struct btree_and_journal_iter *, struct btree *);

void bch2_journal_keys_put(struct bch_fs *);

static inline void bch2_journal_keys_put_initial(struct bch_fs *c)
{
	if (c->journal_keys.initial_ref_held)
		bch2_journal_keys_put(c);
	c->journal_keys.initial_ref_held = false;
}

int bch2_journal_keys_sort(struct bch_fs *);

void bch2_shoot_down_journal_keys(struct bch_fs *, enum btree_id,
				  unsigned, unsigned,
				  struct bpos, struct bpos);

void bch2_journal_keys_dump(struct bch_fs *);

void bch2_fs_journal_keys_init(struct bch_fs *);

#endif /* _BCACHEFS_BTREE_JOURNAL_ITER_H */
