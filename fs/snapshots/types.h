/* SPDX-License-Identifier: GPL-2.0 */
// 备注：快照数据类型定义
// 备注：
// 备注：本文件定义了快照系统的核心数据结构，包括：
// 备注：
// 备注：1. 快照表 (snapshot_table)
// 备注：   - 内存中的快照 ID -> snapshot_t 映射表
// 备注：   - 通过 RCU 保护，允许无锁读取
// 备注：
// 备注：2. 快照节点 (snapshot_t)
// 备注：   - 快照树中的节点，ID 递减构成父子关系
// 备注：   - 三层祖先查询策略：跳表 -> 位图 -> 线性遍历
// 备注：
// 备注：3. 快照删除状态机 (snapshot_delete)
// 备注：   - 两阶段删除：叶节点 -> 内部节点
// 备注：   - 后台线程异步执行清理
// 备注：
// 备注：快照树结构：
// 备注：- 每个快照有一个唯一 ID
// 备注：- 父快照 ID > 子快照 ID
// 备注：- 子卷 (subvolume) 指向快照树中的某个节点
// 备注：- 支持创建快照、删除快照、查询祖先关系
#ifndef _BCACHEFS_SNAPSHOT_TYPES_H
#define _BCACHEFS_SNAPSHOT_TYPES_H

#include <linux/percpu-rwsem.h>
#include <linux/rwsem.h>

#include "btree/bbpos_types.h"
#include "init/progress.h"
#include "util/darray.h"

// 备注：DEFINE_DARRAY_NAMED - 定义动态数组类型
// 备注：snapshot_id_list: u32 类型的动态数组，用于存储快照 ID 列表
DEFINE_DARRAY_NAMED(snapshot_id_list, u32);

// 备注：IS_ANCESTOR_BITMAP - 位图大小
// 备注：使用 128 位位图记录最近 128 个祖先关系
// 备注：位 (ancestor_id - snapshot_id - 1) 表示 ancestor 是否为 snapshot 的祖先
#define IS_ANCESTOR_BITMAP	128

/*
 * In-memory snapshot table entry, indexed by snapshot ID.
 *
 * Snapshots form a binary tree where IDs decrease going deeper: a parent's ID
 * is always greater than its children's.
 *
 * Ancestor lookups use a three-tier strategy:
 *  1. Skiplist (skip[]): jump up the tree in O(log n) steps
 *  2. Bitmap (is_ancestor[]): O(1) lookup for ancestors within 128 IDs
 *  3. Parent walk: fallback linear traversal
 *
 * Read under RCU; partial is_ancestor[] updates are tolerable since readers
 * fall back to the skiplist.
 */
// 备注：struct snapshot_t - 内存中的快照表项
// 备注：
// 备注：快照树结构：
// 备注：- 每个快照节点存储父节点 ID、深度、子节点数组
// 备注：- ID 递减规则：父 ID > 子 ID（用于快速比较）
// 备注：
// 备注：祖先查询三层策略（复杂度从低到高）：
// 备注：1. 跳表 (skip[]): 随机选择 3 个祖先，快速跳升
// 备注：   - skip[0/1/2] 存储递增的祖先 ID
// 备注：   - 从 skip[2] 开始尝试，快速逼近目标
// 备注：2. 位图 (is_ancestor[]): O(1) 判断 128 范围内的祖先关系
// 备注：   - 位图按需更新，允许部分不一致（读 RCU 可安全降级）
// 备注：3. 父节点遍历: O(depth) 回溯，最坏情况备用
// 备注：
// 备注：字段说明：
// 备注：- state: 快照状态 (empty/live/deleted)
// 备注：- parent: 父快照 ID
// 备注：- skip[3]: 跳表索引，随机选择用于快速查找
// 备注：- depth: 树深度（根节点为 0）
// 备注：- children[2]: 子快照 ID（规范化：children[0] >= children[1]）
// 备注：- subvol: 如果有子卷指向此节点，则为子卷 ID
// 备注：- tree: 所属快照树 ID
// 备注：- is_ancestor[]: 位图，记录 128 范围内的祖先关系
struct snapshot_t {
	 // 备注：snapshot_id_state - 快照生命周期状态
	 // 备注：- SNAPSHOT_ID_empty: 未使用（已删除或未分配）
	 // 备注：- SNAPSHOT_ID_live: 活跃快照，可访问
	 // 备注：- SNAPSHOT_ID_deleted: 标记删除，等待清理
	enum snapshot_id_state {
		SNAPSHOT_ID_empty,
		SNAPSHOT_ID_live,
		SNAPSHOT_ID_deleted,
	}			state;
	// 备注：父快照 ID
	u32			parent;
	/* skiplist: random ancestors, sorted ascending; try [2] first */
	// 备注：skip[] - 跳表索引
	// 备注：存储 3 个随机选择的祖先快照 ID（递增排序）
	// 备注：用于 O(log n) 快速跳升到祖先
	u32			skip[3];
	// 备注：树深度
	u32			depth;
	// 备注：children[] - 子快照数组
	// 备注：最多两个子节点，按 ID 规范化排序：children[0] >= children[1]
	// 备注：删除时可能变为空（0 表示无子节点）
	u32			children[2];	/* normalized: [0] >= [1] */
	// 备注：subvol - 关联的子卷 ID
	// 备注：非零表示有子卷指向此快照节点
	// 备注：子卷创建时指向新快照，删除时清零
	u32			subvol; /* Nonzero only if a subvolume points to this node: */
	u32			tree;
	/* bit (ancestor - id - 1) set for ancestors within 128 IDs */
	// 备注：is_ancestor[] - 祖先位图
	// 备注：位 (ancestor_id - snapshot_id - 1) 表示 ancestor 是否为当前快照的祖先
	// 备注：仅覆盖 128 范围，超出则需要其他方式判断
	unsigned long		is_ancestor[BITS_TO_LONGS(IS_ANCESTOR_BITMAP)];
};

// 备注：struct snapshot_table - 快照表
// 备注：
// 备注：RCU 保护机制：
// 备注：- 更新时分配新表，原子替换指针
// 备注：- 读取使用 rcu_dereference，无需加锁
// 备注：
// 备注：内存布局：
// 备注：- nr: 当前快照数量
// 备注：- s[]: 可变长数组，存储 snapshot_t 条目
struct snapshot_table {
	// 备注：RCU 释放回调
	struct rcu_head		rcu;
	// 备注：快照条目数量
	size_t			nr;
#ifndef RUST_BINDGEN
	DECLARE_FLEX_ARRAY(struct snapshot_t, s);
#else
	// 备注：可变长数组起始
	struct snapshot_t	s[0];
#endif
};

// 备注：struct snapshot_interior_delete - 内部节点删除记录
// 备注：
// 备注：当删除快照树的内部节点时：
// 备注：- id: 要删除的内部节点 ID
// 备注：- live_child: 存活的子节点 ID（用于重定向）
// 备注：
// 备注：内部节点删除需要特殊处理：
// 备注：- 需要将子树的键重定向到存活子节点
// 备注：- 可能涉及键的重新分发
struct snapshot_interior_delete {
	// 备注：内部节点 ID
	u32	id;
	// 备注：存活的子节点 ID
	u32	live_child;
};
DEFINE_DARRAY_NAMED(interior_delete_list, struct snapshot_interior_delete);

// 备注：struct snapshot_delete - 快照删除状态机
// 备注：
// 备注：两阶段删除流程：
// 备注：1. 阶段一：删除叶子节点
// 备注：   - 遍历 delete_leaves，逐个删除无子节点的快照
// 备注：2. 阶段二：删除内部节点
// 备注：   - 遍历 delete_interior，处理需要重定向的内部节点
// 备注：3. 阶段三：清理无效键
// 备注：   - 处理 no_keys 和 eytzinger_delete_list
// 备注：
// 备注：并发控制：
// 备注：- lock: 保护 delete 状态结构本身
// 备注：- progress_lock: 保护进度信息
// 备注：
// 备注：后台线程：
// 备注：- thread: 异步执行删除的工作线程
// 备注：- work: 延迟工作项
struct snapshot_delete {
	// 备注：保护自身结构
	struct mutex			lock;
	// 备注：延迟工作
	struct work_struct		work;
	// 备注：删除线程
	struct task_struct __rcu	*thread;

	// 备注：进度锁
	struct mutex			progress_lock;
	// 备注：正在删除的树
	snapshot_id_list		deleting_from_trees;
	// 备注：待删除叶节点
	snapshot_id_list		delete_leaves;
	// 备注：待删除内部节点
	interior_delete_list		delete_interior;
	// 备注：无键需清理
	interior_delete_list		no_keys;
	// 备注：二叉树删除列表
	interior_delete_list		eytzinger_delete_list;

	// 备注：删除进程运行标志
	bool				running;
	// 备注：删除版本号
	unsigned			version;
	// 备注：删除进度指示器
	struct progress_indicator	progress;
};

/*
 * Snapshot creation must prevent userspace from dirtying the page cache while
 * the snapshot is being taken: sync_inodes_sb flushes existing dirty pages
 * before the snapshot transaction, but if new pages get dirtied in the window
 * between sync_inodes_sb returning and the snapshot transaction running, those
 * dirty pages can be partially flushed (e.g. data page flushed but redo log
 * page not yet) such that the snapshot captures an inconsistent state — the
 * shape that bit MySQL/InnoDB.
 *
 * Page-cache dirtying paths (buffered write_iter and mmap mkdirty) take this
 * lock as readers; snapshot creation takes it as a writer. O_DIRECT doesn't
 * need it — direct writes commit as atomic btree transactions, no page cache
 * staleness window. Buffered writeback is fine too — each writeback insert
 * is atomic w.r.t. the snapshot transaction.
 */
// 备注：struct bch_fs_snapshots - 文件系统快照子系统
// 备注：
// 备注：快照创建：
// 备注：- create_lock: 保护快照创建过程
// 备注：
// 备注：快照删除：
// 备注：- delete: 删除状态机和工作线程
// 备注：
// 备注：未链接子卷：
// 备注：- unlinked: 已删除但仍有引用的子卷列表
// 备注：
// 备注：快照表：
// 备注：- table: RCU 保护的快照表指针
// 备注：- table_lock: 保护表结构变更
struct bch_fs_snapshots {
	// 备注：RCU 保护
	struct snapshot_table __rcu		*table;
	// 备注：表锁
	struct mutex				table_lock;
	// 备注：创建锁
	struct percpu_rw_semaphore		create_lock;
	// 备注：删除状态机
	struct snapshot_delete			delete;
	struct work_struct			wait_for_pagecache_and_delete_work;
	// 备注：未链接子卷
	snapshot_id_list			unlinked;
	struct mutex				unlinked_lock;
};

// 备注：subvol_inum - 子卷和 inode 号组合
// 备注：
// 备注：用于唯一标识子卷中的文件：
// 备注：- subvol: 子卷 ID
// 备注：- inum: inode 号
// 备注：
// 备注：这个结构体没有对齐填充，确保精确的 16 字节
typedef struct {
	/* we can't have padding in this struct: */
	// 备注：子卷 ID
	u64		subvol;
	// 备注：inode 号
	u64		inum;
} subvol_inum;

#endif /* _BCACHEFS_SNAPSHOT_TYPES_H */
