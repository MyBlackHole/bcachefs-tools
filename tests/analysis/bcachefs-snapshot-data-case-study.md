# bcachefs 快照子系统完全分析

> 分析日期：2026-05-17
> 隶属分析系列：[启动分配器分析索引](bcachefs-alloc-analysis-index.md)

---

## 目录

1. [快照树结构概述](#1-快照树结构概述)
2. [关键数据结构](#2-关键数据结构)
3. [快照创建](#3-快照创建)
4. [快照删除](#4-快照删除)
5. [键版本控制（COW 机制）](#5-键版本控制cow-机制)
6. [快照感知的 Btree 迭代](#6-快照感知的-btree-迭代)
7. [快照 Skiplist](#7-快照-skiplist)
8. [内部快照删除算法](#8-内部快照删除算法)
9. [持久性与 Fsck](#9-持久性与-fsck)
10. [参考代码位置](#10-参考代码位置)

---

## 1. 快照树结构概述

bcachefs 使用三棵独立的 Btree 管理快照元数据，外加一个内存快照表用于 O(1) 查询。

### 1.1 三棵 Btree 层次

```
snapshot_tree (BTREE_ID_snapshot_trees)
  └─ root_snapshot ──→ snapshot (BTREE_ID_snapshots)
       ├─ children[0] ──→ snapshot (内部节点或叶节点)
       ├─ children[1] ──→ snapshot (内部节点或叶节点)
       └─ subvol ────→ subvolume (BTREE_ID_subvolumes)  // 仅叶节点
```

### 1.2 BTREE_ID_snapshots — 快照 Btree

**位置：** `fs/snapshots/snapshot.c`（核心操作），`fs/snapshots/format.h:35-46`（格式定义）

存储所有快照节点。key 位置为 `POS(0, snapshot_id)`。

| 属性 | 说明 |
|------|------|
| ID 分配方向 | 单调递减（父快照 ID > 子快照 ID），`create_snapids()` 向 key 空间前向分配 |
| 子节点限制 | 每个节点最多 2 个子节点（`children[2]`），形成二叉树 |
| 子节点规范化 | `children[0] >= children[1]` |
| 叶节点 vs 内部节点 | 只有叶节点有关联的子卷；内部节点只有结构信息 |

### 1.3 BTREE_ID_subvolumes — 子卷 Btree

**位置：** `fs/snapshots/subvolume.c`

子卷是文件系统内的独立目录树。每个子卷有全局唯一的 32-bit ID。

| 固定 ID | 值 |
|---------|-----|
| `BCACHEFS_ROOT_SUBVOL` | `1`（定义在 `fs/snapshots/format.h:7`） |
| `BCACHEFS_ROOT_INODE` | `BCACHEFS_ROOT_INO`（根 inode） |

### 1.4 BTREE_ID_snapshot_trees — 快照树 Btree

**位置：** `fs/snapshots/snapshot.c:156-202`

提供每个快照树的持久稳定标识符。每个条目记录 `root_snapshot` 和 `master_subvol`。

### 1.5 内存快照表（`snapshot_table`）

**位置：** `fs/snapshots/types.h:128-139`

RCU 保护的动态数组，用于 O(1) 快照 ID → `snapshot_t` 查找：
- **索引计算**：`idx = U32_MAX - id`（`fs/snapshots/snapshot.h:38`）
- ID `U32_MAX`（根快照）在索引 0

---

## 2. 关键数据结构

### 2.1 `struct bch_subvolume` — 子卷（磁盘格式）

**位置：** `fs/snapshots/format.h:9-25`

| 字段 | 偏移 | 类型 | 说明 |
|------|------|------|------|
| `flags` | +8 | `__le32` | RO(bit 0), SNAP(bit 1), UNLINKED(bit 2) |
| `snapshot` | +12 | `__le32` | 关联的快照节点 ID |
| `inode` | +16 | `__le64` | 根 inode 号 |
| `creation_parent` | +24 | `__le32` | 创建来源子卷 ID |
| `fs_path_parent` | +28 | `__le32` | 文件系统路径父子卷 |
| `otime` | +32 | `__le64` | 创建/快照时间戳 |

**标志定义**（`fs/snapshots/format.h:27-33`）：

| 标志 | 说明 |
|------|------|
| `BCH_SUBVOLUME_RO` | 只读 |
| `BCH_SUBVOLUME_SNAP` | 是快照（非 master 子卷） |
| `BCH_SUBVOLUME_UNLINKED` | 标记删除，等待清理 |

### 2.2 `struct bch_snapshot` — 快照节点（磁盘格式）

**位置：** `fs/snapshots/format.h:35-46`

| 字段 | 偏移 | 类型 | 说明 |
|------|------|------|------|
| `flags` | +8 | `__le32` | `WILL_DELETE`、`SUBVOL`、`DELETED`、`NO_KEYS` |
| `parent` | +12 | `__le32` | 父快照 ID（0 = 根） |
| `children[2]` | +16 | `__le32[2]` | 子节点 ID，`[0] >= [1]` |
| `subvol` | +24 | `__le32` | 关联子卷 ID（叶节点）/ 0（内部节点） |
| `tree` | +28 | `__le32` | 所属 snapshot_tree ID |
| `depth` | +32 | `__le32` | 树深度（根为 0） |
| `skip[3]` | +36 | `__le32[3]` | 跳表索引，随机祖先 ID |
| `btime` | +48 | `__le64` | 创建时间戳 |

**标志定义**（`fs/snapshots/format.h:73-76`）：

| 标志 | 值 | 说明 |
|------|-----|------|
| `BCH_SNAPSHOT_WILL_DELETE` | bit 0 | 叶节点不再关联子卷，等待删除 |
| `BCH_SNAPSHOT_SUBVOL` | bit 1 | 有子卷指向此节点 |
| `BCH_SNAPSHOT_DELETED` | bit 2 | 节点已从树中移除（保留以区分"丢失的快照"） |
| `BCH_SNAPSHOT_NO_KEYS` | bit 3 | 内部节点，所有键已迁移，运行时不可删除（推迟到下次挂载） |

### 2.3 `struct bch_snapshot_tree` — 快照树（磁盘格式）

**位置：** `fs/snapshots/format.h:85-89`

| 字段 | 说明 |
|------|------|
| `master_subvol` | 此树的原始 master 子卷 ID |
| `root_snapshot` | 此树的根快照节点 ID |

### 2.4 `struct snapshot_t` — 内存快照表项

**位置：** `fs/snapshots/types.h:73-115`

从 `bch_snapshot` 的 btree trigger（`bch2_mark_snapshot`）填充，增加以下字段：

| 字段 | 说明 |
|------|------|
| `state` | `SNAPSHOT_ID_empty / live / deleted` |
| `is_ancestor[]` | 128 位祖先位图，用于 O(1) 祖先查询 |

### 2.5 `struct snapshot_delete` — 删除状态机

**位置：** `fs/snapshots/types.h:179-206`

| 字段 | 说明 |
|------|------|
| `delete_leaves` | 待删除叶节点 ID 列表 |
| `delete_interior` | 待删除内部节点 ID + 存活子节点 |
| `no_keys` | NO_KEYS 状态内部节点 |
| `eytzinger_delete_list` | Eytzinger 索引的删除列表（二叉堆加速） |
| `version` | 删除算法版本（v1/v2） |

### 2.6 `struct bpos` — 键位置（快照感知）

**位置：** `fs/btree/bkey_types.h:15-19`

```c
struct bpos {
    u64  inode;      // 高字节
    u64  offset;     // 中字节
    u32  snapshot;   // 低字节 — 快照 ID
};
```

**比较规则**（`fs/btree/bkey.h:113-146`）：

| 函数 | 比较字段 | 用途 |
|------|----------|------|
| `bpos_cmp()` | `inode → offset → snapshot` | 全字段比较 |
| `bkey_cmp()` | `inode → offset` | 忽略 snapshot，"同一键"判断 |
| `bkey_eq()` | `inode == inode && offset == offset` | 位置相同判断 |

**关键区别**：两个键在 `bpos` 层面不同（不同 snapshot ID），但在 `bkey` 层面视为"位置相同"，允许在同一文件偏移处存在多个快照版本。

---

## 3. 快照创建

### 3.1 入口：`bch2_subvolume_create()`

**位置：** `fs/snapshots/subvolume.c:575-646`

支持两种模式：

| 模式 | `src_subvolid` | 行为 |
|------|----------------|------|
| 创建空子卷 | 0 | 分配新子卷 ID + 创建新快照节点作为新树根 |
| 创建快照 | ≠ 0 | 分配两个新快照节点，一个给新子卷，一个替换源子卷 |

### 3.2 完整快照创建原子流程

```
bch2_subvolume_create()
│
├─ 1. bch2_bkey_get_empty_slot()
│       subvolumes btree 中分配新槽位
│
├─ 2. bch2_subvolume_get_mut()
│       获取源子卷的快照位置
│
├─ 3. bch2_snapshot_node_create()          [snapshot.c:765-779]
│   ├─ parent=0  → bch2_snapshot_node_create_tree()
│   │                    创建新快照树
│   └─ parent≠0  → bch2_snapshot_node_create_children() [snapshot.c:715-744]
│       └─ create_snapids()                [snapshot.c:669-710]
│           ├─ bch2_btree_iter_prev_slot()  // 分配新 ID（递减）
│           ├─ bch2_bkey_alloc()            // 创建 snapshot key
│           └─ bch2_snapshot_skiplist_get() // 随机选 3 个祖先
│               （get_random_u32_below(s->depth) 选层，
│                 冒泡排序保持 skip[0..2] 升序）[snapshot.c:697-700]
│
├─ 4. 更新源子卷 snapshot = new_nodes[1]   // 开始 COW
│       └─ 源子卷的快照节点变为内部节点（subvol=0, children[] 指向两个节点）
│
├─ 5. bch2_bkey_alloc() 创建新子卷 key
│      snapshot = new_nodes[0]
│      creation_parent = src_subvolid
│      flags: RO + SNAP（快照时）
│
└─ 6. 提交事务
```

### 3.3 COW 关键语义

快照创建后：

1. 源子卷的快照节点变为**内部节点**（`subvol=0`，`children[2]` 设置）
2. 源子卷的 `subvol->snapshot` → `new_nodes[1]`
3. 新子卷的 `subvol->snapshot` → `new_nodes[0]`
4. **所有已有的键对两个子卷都可见**（通过祖先关系）
5. **此后**，任一子卷的写入都在 `bpos.snapshot` 中用自己的 ID，只产生分歧

---

## 4. 快照删除

### 4.1 用户触发删除

```
用户执行删除子卷
  └─ bch2_subvolume_unlink()                 [subvolume.c:520-540]
       ├─ 设置 BCH_SUBVOLUME_UNLINKED 标志
       └─ 清除 BCH_SUBVOLUME_SNAP 标志
            （触发后台 work item）

  └─ bch2_subvolume_wait_for_pagecache_and_delete() [subvolume.c:463-495]
       ├─ 清空关联 inode 的页缓存
       └─ 调用 __bch2_subvolume_delete()
```

### 4.2 核心删除：`__bch2_subvolume_delete()`

**位置：** `fs/snapshots/subvolume.c:408-451`

```c
1. 扫描并重定父子卷的 creation_parent 引用
2. 如果此子卷是 master subvol → 清除 snapshot_tree.master_subvol
3. 从 subvolumes btree 删除子卷条目
4. bch2_snapshot_node_set_deleted(snapid)   [delete.c:127-144]
   ├─ 设置 BCH_SNAPSHOT_WILL_DELETE 标志
   └─ 清除 BCH_SNAPSHOT_SUBVOL 和 subvol 字段
5. 设置 BCH_FS_need_delete_dead_snapshots 位 → 触发后台线程
```

### 4.3 后台清理：`bch2_delete_dead_snapshots()`

**位置：** `fs/snapshots/delete.c:709-747`

获取互斥锁后调用 `delete_dead_snapshots_locked()`（`fs/snapshots/delete.c:664-707`）：

```
delete_dead_snapshots_locked()
│
├─ 1. check_should_delete_snapshot()         [delete.c:532-591]
│      遍历 snapshots btree 识别：
│      ├─ 无子节点 → delete_leaves（键直接删除）
│      ├─ 恰 1 个子节点 → delete_interior{id, live_child}（键迁移到 live_child）
│      └─ 2 个子节点 → 不可删除（跳过）
│
├─ 2. 构建 eytzinger_delete_list
│      二叉堆加速数组，用于 O(log n) 删除查找
│
├─ 3. delete_dead_snapshot_keys_v1/v2()     [delete.c:413,462]
│      遍历所有快照感知 btree：
│      delete_dead_snapshots_process_key()  [delete.c:345-384]
│      ├─ k.p.snapshot 在 delete_leaves 中 → bch2_btree_delete_at()
│      └─ k.p.snapshot 在 delete_interior（有 live_child）
│          → 复制键到 live_child 再删除原键
│
├─ 4. darray_for_each(delete_leaves)        // 删除叶节点
│      bch2_snapshot_node_delete(id, false)   [delete.c:167-290]
│
└─ 5. darray_for_each(delete_interior)       // 标记内部节点
       bch2_snapshot_node_set_no_keys(id)     [delete.c:146-159]
       // 实际删除推迟到下次挂载（NO_KEYS 状态）
```

### 4.4 两种删除算法版本

| 版本 | 位置 | 说明 |
|------|------|------|
| v1 | `delete.c:413` | 遍历所有快照感知 btree（extents, inodes, dirents, xattrs） |
| v2 | `delete.c:462` | 先扫描 inodes btree，对含死亡快照 ID 的 inode 仅扫描其范围；需要 `bcachefs_metadata_version_snapshot_deletion_v2` 特性 |

---

## 5. 键版本控制（COW 机制）

bcachefs **不使用版本计数器**。COW 完全通过快照 ID 实现。

### 5.1 核心规则

- **写入** → 在当前快照 ID 下创建新键
- **读取** → 向上遍历祖先链，找到最近匹配键
- **删除** → 在当前快照 ID 下插入 whiteout

### 5.2 快照 ID 比较

```c
// 两种比较层次：
bpos_cmp(a, b):  inode → offset → snapshot    ← 精确位置
bkey_cmp(a, b):  inode → offset                ← "同一数据"判断
```

同一 `(inode, offset)` 处，不同快照的键按 `snapshot` 排序共存。

### 5.3 Btree 写路径

写路径处理 `BTREE_UPDATE_internal_snapshot_node` 标志（`fs/btree/update.c`），确保在快照内部节点处正确创建 COW 副本。

---

## 6. 快照感知的 Btree 迭代

### 6.1 迭代器标志

**位置：** `fs/btree/iter.h:814-829`

| 用户标志 | 定义 | 含义 |
|---------|------|------|
| `BTREE_ITER_all_snapshots` | 用户显式设置 | 返回所有快照的键，不做过滤 |

**自动设置规则**（`iter.h:822-824`）：

```c
if (!(flags & BTREE_ITER_all_snapshots) &&
    btree_type_has_snapshots(btree_id))
    flags |= BTREE_ITER_filter_snapshots;
```

`BTREE_ITER_filter_snapshots` 是**自动设置的**，只在用户要求 `all_snapshots` 时才跳过。

### 6.2 位置跳跃

**位置：** `fs/btree/iter.c:211-233`

**前向跳跃（`bkey_successor`）：**

```c
if (iter->flags & BTREE_ITER_all_snapshots)
    → bpos_successor(p):          inode/offset/snapshot 全递增
else  // filter 模式
    → bpos_with_snapshot(bpos_nosnap_successor(p), iter->snapshot)
    // 跳到 (inode+1 或 offset+1, snapshot=iter->snapshot)
```

**后向跳跃（`bkey_predecessor`）：**

```c
if (all_snapshots)
    → bpos_predecessor(p)
else  // filter 模式
    → p = bpos_nosnap_predecessor(p); p.snapshot = iter->snapshot;
```

### 6.3 Peek 前向迭代的快照过滤

**位置：** `fs/btree/iter.c:2823-2936`

```
while (1) {
    k = __bch2_btree_iter_peek(iter, search_key);

    // 步骤 1：超过范围 → 结束
    if (k.k->p.inode > end.inode) goto end;

    // 步骤 2：跳过 snapshot < iter->snapshot 的键
    if (k.k->p.snapshot < iter->snapshot) {
        search_key = bpos_with_snapshot(k.k->p, iter->snapshot);
        continue;
    }

    // 步骤 3：祖先可见性检查
    if (!bch2_snapshot_is_ancestor(trans, iter->snapshot, k.k->p.snapshot)) {
        search_key = bpos_successor(k.k->p);
        continue;
    }

    // 步骤 4：跳过 extent whiteout
    if (bkey_extent_whiteout(k.k)) {
        search_key = bkey_successor(iter, k.k->p);
        continue;
    }

    break;
}
```

### 6.4 Peek 后向迭代的快照过滤

**位置：** `fs/btree/iter.c:3151-3213`

```
while (1) {
    k = __bch2_btree_iter_peek_prev(iter, search_key);

    // 跳过祖先不可见的键
    if (!bch2_snapshot_is_ancestor(trans, iter->snapshot, k.k->p.snapshot)) {
        search_key = bpos_predecessor(k.k->p);
        continue;
    }

    // 如果键属于祖先快照（非当前 snapshot）
    // → 保存候选，继续搜索（可能被子节点覆盖）
    if (k.k->p.snapshot != iter->snapshot) {
        saved_path = btree_path_clone(trans, iter->path, ...);
        search_key = bpos_predecessor(k.k->p);
        continue;
    }

    break;  // 找到精确匹配
}
```

**候选保存机制**：后向迭代时，祖先快照中的键不能立即返回（可能有子节点覆盖）。算法保存候选，继续查找。如果找到新键替换候选；如果到达 `saved_pos` 则返回候选。

### 6.5 后置验证

**位置：** `fs/btree/iter.c:431-459`

`bch2_btree_iter_verify_ret()` 验证返回的键对 `iter->snapshot` 可见且未被子节点覆盖。

---

## 7. 快照 Skiplist

### 7.1 数据结构

每个 `snapshot_t` / `bch_snapshot` 包含 `skip[3]` 数组，存储 3 个随机选择的祖先 ID（`fs/snapshots/format.h:44`）。

```
skip[0] ≤ skip[1] ≤ skip[2] ≤ parent  // 升序排列
```

### 7.2 Skiplist 获取

**位置：** `fs/snapshots/check_snapshots.c:211-221`

```c
u32 bch2_snapshot_skiplist_get(struct bch_fs *c, u32 id)
{
    if (!id) return 0;
    const struct snapshot_t *s = snapshot_t(c, id);
    return s->parent
        ? bch2_snapshot_nth_parent(c, id, get_random_u32_below(s->depth))
        : id;
}
```

从父节点开始，随机选择 `[0, depth-1]` 层祖先。创建新快照节点时调用 3 次，然后冒泡排序（`snapshot.c:697-700`）。

### 7.3 三层祖先查询

**位置：** `fs/snapshots/snapshot.c:328-353`

```
__bch2_snapshot_is_ancestor(trans, id, ancestor):
│
├─ 如果 recovery_pass_check_snapshots 未运行
│   → 线性遍历（__bch2_snapshot_is_ancestor_early, snapshot.c:206-213）
│
├─ 第一阶段：跳表跳跃 (O(log n))
│   while (ancestor >= IS_ANCESTOR_BITMAP (=128) &&
│          id < ancestor - IS_ANCESTOR_BITMAP)
│       id = get_ancestor_below(t, id, ancestor)
│
├─ 第二阶段：位图查询 (O(1))
│   if (id && id < ancestor)
│       return test_ancestor_bitmap(t, id, ancestor)
│   else
│       return id == ancestor
```

### 7.4 `get_ancestor_below()` — 跳表跳跃核心

**位置：** `fs/snapshots/snapshot.c:221-234`

```c
if (s->skip[2] <= ancestor) return s->skip[2];
if (s->skip[1] <= ancestor) return s->skip[1];
if (s->skip[0] <= ancestor) return s->skip[0];
return s->parent;
```

选择 ≤ ancestor 的最大跳跃值，以 O(log n) 步逼近目标。

### 7.5 祖先位图

**位置：** `fs/snapshots/types.h:46`, `fs/snapshots/snapshot.c:529-544`

- 128 位位图，位 `(ancestor_id - id - 1)` 表示 ancestor 是否为 id 的祖先
- 在 `bch2_mark_snapshot()` 中按需构建
- 通过 `barrier_data()` + `memcpy()` 原子更新
- RCU 读者可容忍部分不一致

### 7.6 快照树遍历

**位置：** `fs/snapshots/snapshot.c:581-605`

`__bch2_snapshot_tree_next()` — 二叉树深度优先遍历（先左后右），使用 `children[0]`（左）/ `children[1]`（右）。用于 `for_each_snapshot_child` 宏和 `snapshot_tree_keys_to_text()`。

---

## 8. 内部快照删除算法

### 8.1 两阶段设计

| 阶段 | 时机 | 位置 | 操作 |
|------|------|------|------|
| 第一阶段 | 运行时 | `delete.c:664-707` | 删除叶节点 + 键，将内部节点标记为 NO_KEYS |
| 第二阶段 | 挂载时 | `delete.c:811-851` | 删除 NO_KEYS 内部节点，修复 depth/skip |

内部节点在运行时不能删除，因为子树深度和 skiplist 需要原子更新。

### 8.2 第一阶段的 `check_should_delete_snapshot()`

**位置：** `delete.c:532-591`

```
检查每个 snapshot 节点的 children 和 subvol：
├─ 无 children → 叶节点 → delete_leaves
├─ children[1] == 0 → 单子内部节点 → delete_interior{id, live_child}
│   其中 live_child = children[0]（唯一活着的子节点）
└─ children[1] != 0 → 双子节点 → 不可删除
```

### 8.3 键处理策略

**位置：** `delete.c:345-384`

```c
delete_dead_snapshots_process_key():
  ├─ k.p.snapshot 在 delete_leaves 中
  │   → bch2_btree_delete_at()          // 直接删除键
  │
  └─ k.p.snapshot 在 delete_interior（有 live_child）
      → 复制键到 live_child 再删除原键  // 键迁移
```

### 8.4 第二阶段：`bch2_delete_dead_interior_snapshots()`

**位置：** `delete.c:811-851`

在单线程恢复上下文中运行：

```
1. bch2_get_dead_interior_snapshots()     [delete.c:776-809]
   扫描标记为 NO_KEYS 的内部节点

2. bch2_fix_child_of_deleted_snapshot()   [delete.c:611-662]
   更新受影响子节点的 depth 和 skip[] 字段

3. bch2_snapshot_node_delete(id, true)    [delete.c:167-290]
   物理删除节点：
   ├─ 重定向父节点 children[i] → 子节点
   ├─ 重定向子节点 parent → 祖父节点
   ├─ 更新 snapshot_tree（如果正在删除根节点）
   ├─ 设置 KEY_TYPE_deleted（新格式）或 DELETED 标志（旧格式）
   └─ 删除关联 accounting 条目
   需要 bcachefs_metadata_version_snapshot_deletion_v2 特性
```

### 8.5 `bch2_snapshot_node_delete()` 详细过程

**位置：** `delete.c:167-290`

```c
1. 获取被删除节点 s(id)
2. BUG_ON(s.children[1] != 0)           // 只能删除单子/无子节点
3. parent_id = s.parent, child_id = s.children[0]

4. if (parent_id):
   a. 获取父节点
   b. 在 parent->children[] 中找到指向 id 的条目
   c. 替换为 child_id
   d. normalize_snapshot_child_pointers()  // 规范化子指针排列

5. if (child_id):
   a. 获取子节点
   b. child->parent = parent_id          // 跳过被删除节点
   c. 检查 delete_interior 标志（运行时不允许删除）

6. if (!parent_id):                      // 删除根节点
   a. 更新 snapshot_tree.root_snapshot
   b. 或删除整个 snapshot_tree 条目

7. 清除节点内容或设置 KEY_TYPE_deleted
```

---

## 9. 持久性与 Fsck

### 9.1 恢复挂载时的三个校验步骤

#### 步骤 1: `bch2_check_snapshot_trees()`

**位置：** `fs/snapshots/check_snapshots.c:183-191`

遍历 `BTREE_ID_snapshot_trees`，验证：
- `root_snapshot` 存在且是树的根
- snapshot 条目回指正确的 `tree` ID
- `master_subvol` 存在且是最旧子卷（或修复）

#### 步骤 2: `bch2_check_snapshots()`

**位置：** `fs/snapshots/check_snapshots.c:429` / 详细实现在 `check_snapshots_trans()`：`415-426`

**反向遍历**（父节点在子节点之后验证）确保 depth 正确：

| 验证项 | 行号 | 操作 |
|--------|------|------|
| 父子指针双向一致性 | 270-322 | 修正 children/parent 指针 |
| 子节点规范化 | 453-455 | 确保 children[0] >= children[1] |
| SUBVOL 标志一致性 | 324-353 | 与子卷条目对照 |
| snapshot_tree 指针 | 355-363 | 修正错误 tree ID |
| depth 字段修正 | 366-376 | 从根重新计算 |
| skiplist 验证 | 378-410 | 重建无效 skip 条目 |

#### 步骤 3: `bch2_check_subvols()`

**位置：** `subvolume.c:181-188`

| 验证项 | 行号 | 操作 |
|--------|------|------|
| 指向有效快照叶节点 | 59-73 | 修复无效指针 |
| UNLINKED 标志 | 77-81 | 自动重新删除 |
| bi_subvol 一致性 | 110-152 | 确保 inode 和子卷交叉引用正确 |
| master/snapshot 标志 | 154-176 | 修正不一致的标志位 |

### 9.2 快照重建：`bch2_reconstruct_snapshots()`

**位置：** `check_snapshots.c:539-583`

当 snapshots btree 数据丢失时（`btrees_lost_data & BIT_ULL(BTREE_ID_snapshots)`），通过扫描所有快照感知 btree 的 snapshot ID 重建快照树。

### 9.3 键级联校验：`bch2_check_key_has_snapshot()`

**位置：** `snapshot.h:286-293`，实现 `check_snapshots.c:585-643`

调用自 `delete_dead_snapshots_process_key()` 开头。检查每个快照感知键的 `k.p.snapshot`：

| 状态 | 操作 |
|------|------|
| `SNAPSHOT_ID_deleted` | 自动删除键 |
| `SNAPSHOT_ID_empty` | 需要 running recovery passes 修复 |

### 9.4 快照表重建：`bch2_snapshots_read()`

**位置：** `snapshot.c:783-813`

挂载时调用，反向遍历 snapshots btree，通过 `__bch2_mark_snapshot()` 填充 `snapshot_table`，包括祖先位图计算。同时检测 NO_KEYS 内部节点并触发 `delete_dead_interior_snapshots` 恢复步骤。

---

## 10. 参考代码位置

### 快照核心

| 文件 | 行号 | 函数/内容 |
|------|------|-----------|
| `fs/snapshots/format.h` | 1-91 | 全部磁盘格式定义 |
| `fs/snapshots/types.h` | 1-270 | 全部内存数据类型 |
| `fs/snapshots/snapshot.c` | 1-945 | 快照核心操作（创建节点、skiplist、祖先查询、快照表重建） |
| `fs/snapshots/subvolume.c` | 1-714 | 子卷操作（创建、删除、获取） |
| `fs/snapshots/delete.c` | 1-878 | 快照删除全部逻辑 |
| `fs/snapshots/check_snapshots.c` | 1-643 | Fsck 校验全部 |

### 快照关键函数

| 函数 | 文件:行 |
|------|---------|
| `bch2_subvolume_create()` | `subvolume.c:575` |
| `__bch2_subvolume_delete()` | `subvolume.c:408` |
| `bch2_snapshot_node_create()` | `snapshot.c:765` |
| `create_snapids()` | `snapshot.c:669` |
| `get_ancestor_below()` | `snapshot.c:221` |
| `__bch2_snapshot_is_ancestor()` | `snapshot.c:328` |
| `__bch2_snapshot_tree_next()` | `snapshot.c:581` |
| `bch2_delete_dead_snapshots()` | `delete.c:709` |
| `delete_dead_snapshots_locked()` | `delete.c:664` |
| `check_should_delete_snapshot()` | `delete.c:532` |
| `delete_dead_snapshot_process_key()` | `delete.c:345` |
| `bch2_snapshot_node_delete()` | `delete.c:167` |
| `bch2_delete_dead_interior_snapshots()` | `delete.c:811` |
| `bch2_snapshots_read()` | `snapshot.c:783` |
| `bch2_check_snapshots()` | `check_snapshots.c:429` |
| `bch2_check_snapshot_trees()` | `check_snapshots.c:183` |
| `bch2_check_subvols()` | `subvolume.c:181` |
| `bch2_reconstruct_snapshots()` | `check_snapshots.c:539` |

### Btree 迭代（快照相关）

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/btree/bkey_types.h` | 15-19 | `struct bpos`（含 snapshot 字段） |
| `fs/btree/bkey.h` | 113-146 | `bpos_cmp()` / `bkey_cmp()` 比较 |
| `fs/btree/iter.h` | 814-829 | 自动设置 `BTREE_ITER_filter_snapshots` |
| `fs/btree/iter.c` | 211-233 | `bkey_successor()` / `bkey_predecessor()` |
| `fs/btree/iter.c` | 2823-2936 | 前向 peek 快照过滤 |
| `fs/btree/iter.c` | 3151-3213 | 后向 peek 快照过滤 |
| `fs/btree/iter.c` | 431-459 | 后置验证 |
| `fs/btree/types.h` | 1413-1438 | btree snapshot 标识 |
