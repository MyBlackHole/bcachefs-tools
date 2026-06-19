# bcachefs Btree 事务实现分析

> 分析日期：2026-05-17
> 隶属分析系列：[启动分配器分析索引](bcachefs-alloc-analysis-index.md)

---

## 目录

1. [事务生命周期](#1-事务生命周期)
2. [事务重启机制](#2-事务重启机制)
3. [Btree 路径系统](#3-btree-路径系统)
4. [锁获取与排序](#4-锁获取与排序)
5. [事务提交流程](#5-事务提交流程)
6. [提交中的事务重启](#6-提交中的事务重启)
7. [事务内的内存分配](#7-事务内的内存分配)
8. [Btree 写缓冲区](#8-btree-写缓冲区)
9. [六状态 Btree 节点锁（SIX 锁）](#9-六状态-btree-节点锁six-锁)
10. [数据写入案例：从 VFS 到 btree 事务](#10-数据写入案例从-vfs-到-btree-事务)
11. [Direct IO 写入案例](#11-direct-io-写入案例)
12. [参考代码位置](#12-参考代码位置)

---

## 1. 事务生命周期

所有 btree 操作都通过 `struct btree_trans` 进行。

### 1.1 创建：`__bch2_trans_get()`

**位置：** `fs/btree/iter.c:3996-4057`

```c
1. bch2_trans_alloc() 分配 struct btree_trans
   ├─ 从 percpu 缓存获取（快速路径）
   └─ 从 mempool btree.trans.pool 获取（慢速路径）

2. 初始化内联数组：
   ├─ _paths[BTREE_ITER_INITIAL=64]     // 路径数组
   ├─ _sorted[68]                        // 排序后的路径索引
   └─ _updates[64]                       // 更新条目数组

3. trans->fn = 调用者名称（用于统计和调试）

4. srcu_read_lock(&c->btree.trans.barrier)  // 持有 SRCU 读锁
   // 保护 btree 节点内存免于被释放

5. 路径 #0 保留为哨兵（iter.c:4025）
```

**RAII 宏：**

```c
// fs/btree/iter.h
#define CLASS(btree_trans, trans)(c)    // 自动 get/put
```

### 1.2 初始化/分配

| 文件 | 行号 | 函数 | 功能 |
|------|------|------|------|
| `init.c` | 315-342 | `bch2_fs_btree_init()` | btree cache、iter、key cache 初始化 |
| `init.c` | 344-356 | `bch2_fs_btree_init_rw()` | workqueue、interior update、write buffer |

### 1.3 重置：`bch2_trans_begin()`

**位置：** `iter.c:3811-3927`

每次事务重启时调用：

```c
1. bch2_trans_reset_updates()           // 清空 updates 队列

2. restart_count++                        // 递增重启计数

3. mem_top = 0                            // 重置 bump allocator

4. 释放僵尸路径（无 ref、无 preserve）

5. 如果 SRCU 超过 10ms → bch2_trans_unlock_long()

6. 如果锁持有超过 BTREE_TRANS_MAX_LOCK_HOLD_TIME_NS (1ms)
   → 释放锁 + cond_resched()

7. 如果被重启 → bch2_btree_path_traverse_all()
   → 重新遍历所有路径

8. 返回新的 restart_count（供 lockrestart_do 验证）
```

### 1.4 销毁：`bch2_trans_put()`

**位置：** `iter.c:4101-4166`

```c
1. 释放所有更新路径
2. 等待 closure 同步
3. 释放内存（kmalloc 或 mempool）
4. srcu_read_unlock()                    // 释放 SRCU 读锁
5. 内核态：call_rcu(&trans->rcu, trans_rcu_free) → 延迟归还 mempool
   用户态：直接放回 percpu 缓存
```

---

## 2. 事务重启机制

### 2.1 触发重启

**位置：** `fs/btree/iter.h`

```c
// iter.h:684-687
btree_trans_restart(trans, err)
  └─ btree_trans_restart_ip(trans, err, _THIS_IP_)
       └─ 设置 trans->restarted = err
          trans->last_restarted_ip = ip
          返回 -err

// iter.h:662-670
btree_trans_restart_foreign_task()
  └─ 设置 trans->restarted + last_restarted_ip
     唤醒等待线程
```

所有重启返回值都必须是 `BCH_ERR_transaction_restart_*` 系列。

### 2.2 重启原因

| 错误码 | 含义 | 触发场景 |
|--------|------|----------|
| `transaction_restart_would_deadlock` | 检测到死锁 | 锁获取冲突 |
| `transaction_restart_would_deadlock_write` | 写入锁死锁 | write lock 竞争 |
| `transaction_restart_split_with_interior_updates` | btree 分裂 | 节点满需分裂 |
| `transaction_restart_mem_realloced` | 内存重分配 | kmalloc 空间不足 |
| `transaction_restart_journal_overwrites_changed` | journal 覆盖变化 | journal 回收 |
| `transaction_restart_fault_inject` | 故障注入 | 调试/测试 |
| `transaction_restart_nested` | 嵌套重启 | 嵌套事务 |
| `transaction_restart_lock_waitlist_alloc` | 锁等待列表 | locking.h:351 |

### 2.3 `lockrestart_do` 宏

**位置：** `iter.h:1188-1202`

```c
#define lockrestart_do(_trans, _do) ({
    int _ret2;
    do {
        u32 _restart_count = bch2_trans_begin(_trans);
        _ret2 = (_do);
        if (!_ret2)
            bch2_trans_verify_not_restarted(_trans, _restart_count);
    } while (bch2_err_matches(_ret2, BCH_ERR_transaction_restart));
    _ret2;
})
```

**模式：** `bch2_trans_begin()` → 执行 → 验证（无重启时）→ 检测 restart 后重试。

### 2.4 `nested_lockrestart_do` 宏

**位置：** `iter.h:1213-1227`

与 `lockrestart_do` 区别：先执行一次再 `bch2_trans_begin()`。如果成功但发生了重启 → 返回 `BCH_ERR_transaction_restart_nested`。

### 2.5 `commit_do` 宏

**位置：** `update.h:677`

结合 `lockrestart_do` + `bch2_trans_commit()` 的便捷宏：

```c
#define commit_do(trans, ...) \
    lockrestart_do(trans, bch2_trans_commit(trans, ...))
```

### 2.6 故障注入

**位置：** `iter.h:689-699`

```c
trans_maybe_inject_restart()
  └─ CONFIG_BCACHEFS_INJECT_TRANSACTION_RESTARTS
  └─ 概率随 restart_count_this_trans 指数下降 → 早期更频繁
```

---

## 3. Btree 路径系统

### 3.1 `struct btree_path`

**位置：** `fs/btree/types.h`

每个路径代表一条从 btree 根到叶子的下行路径：

| 字段 | 说明 |
|------|------|
| `btree_id` | 目标 btree 类型 |
| `level` | 当前层级（0 = 叶子） |
| `pos` | 当前位置 |
| `l[BTREE_MAX_DEPTH]` | 每层一个 `struct btree_path_level`（节点指针 + 迭代器 + 键） |
| `ref` / `intent_ref` | 引用计数 |
| `nodes_locked` | 位图（每层 2 位：读/意图/写锁状态） |
| `should_be_locked` | 是否应在 trans_begin 后保持锁 |
| `preserve` | 是否跨 trans_begin 存活 |
| `cached` | 是否使用键缓存 |
| `uptodate` | 状态：`UPTODATE` / `NEED_RELOCK` / `NEED_TRAVERSE` |

### 3.2 路径排序

**位置：** `iter.c` — `btree_path_cmp()`

按 `(btree_id, pos, level)` 排序。同一路径保证低层先于高层持有锁，避免死锁。

### 3.3 路径遍历

```c
bch2_btree_path_traverse(trans, path, flags)
  └─ 从根节点逐层下降到叶子（btree_path_down）
       ├─ iter.c:1150: btree_path_down()
       │   ├─ bch2_btree_node_get()       // 读取下一层节点
       │   └─ 下行到 level - 1
       └─ BTREE_ITER_prefetch → btree_path_prefetch() (iter.c:1010)
            // 预取兄弟节点

  状态变化：
    NEED_TRAVERSE  → 遍历 → UPTODATE
    NEED_RELOCK    → 重获锁 → UPTODATE
```

### 3.4 路径分配/释放

| 操作 | 位置 | 说明 |
|------|------|------|
| `bch2_path_get()` | `iter.c` | 从 `trans->paths_allocated` 位图分配新 ID |
| `bch2_path_put()` | `iter.c` | 释放路径 ID |
| 最大路径数 | `types.h:781` | `BTREE_ITER_MAX = 1024` |

---

## 4. 锁获取与排序

### 4.1 锁类型与路径

路径每层的 `btree_lock_want`：

| 值 | 含义 | 获取调用 |
|----|------|----------|
| `btree_node_read_locked` | S 锁 | 读取者 |
| `btree_node_intent_locked` | I 锁 | 写入者意图 |
| `btree_node_write_locked` | X 锁 | 写入者 |

`nodes_locked` 位图每层 2 位跟踪锁状态。

### 4.2 锁升级/降级

| 操作 | 位置 | 说明 |
|------|------|------|
| `bch2_btree_node_upgrade()` | `iter.h:701-713` | S→I 升级 |
| `__bch2_btree_path_downgrade()` | `locking.c` | 提交后 I→S 降级 |
| `bch2_trans_downgrade()` | `iter.h:715` | 所有路径批量降级 |

### 4.3 死锁检测

**位置：** `locking.c`（完整），`locking_types.h`（类型定义）

```c
struct lock_graph {
    struct trans_waiting_for_lock edges[LOCK_GRAPH_SIZE]; // D=8
    unsigned nr;
};
```

`bch2_btree_node_lock()` 检测循环等待：
1. 构建 `lock_graph`，DFS 遍历等待链
2. 如果检测到循环 → `-BCH_ERR_transaction_restart_would_deadlock`
3. 调用者释放锁并重试

### 4.4 锁获取排序

读锁：按路径排序的 `(btree_id, pos, level)` 获取。

写锁：`commit.c:144-161` `bch2_trans_lock_write()`：
1. 遍历 `trans->updates`
2. 对每个独立叶节点获取 write lock
3. 失败 → `trans_lock_write_fail()` → 回滚并重启

---

## 5. 事务提交流程

### 5.1 入口

**位置：** `commit.c:1432-1586` — `__bch2_trans_commit()`

两阶段设计：

#### 第一阶段：准备与验证（`commit.c:1439-1539`）

```c
1. 验证未被解锁且未在重启中（iter.h:655-659）
2. 故障注入检查（trans_maybe_inject_restart）
3. 空提交快速路径（无 updates）
4. bch2_trans_commit_run_triggers()    [commit.c:771-817]
   → 运行事务性 trigger，可能生成更多 updates
5. noop 过滤：删除旧键相等的更新 [commit.c:1462-1483]
   （inode 更新除外）
6. 计算 journal_u64s（每个 update 的真实大小）
7. bch2_disk_reservation_add()        [commit.c:1532-1539]
8. 可选的首部写入限速 [commit.c:1450-1456]
```

#### 第二阶段：`do_bch2_trans_commit()`（`commit.c:1087-1169`）

```c
1. 合并检测（btree_node_needs_merge）+ 带限制的分裂
2. 预留 journal entries subbuf 空间
3. bch2_trans_lock_write()
   → 获取所有相关叶节点的 write lock
4. 检查 journal replay 覆盖
5. bch2_trans_commit_write_locked()   [commit.c:830-1047]
   ├─ btree_key_can_insert()          // 验证插入空间
   ├─ bch2_trans_journal_res_get()    // 获取 journal 预留
   ├─ trans->hooks → 提交钩子调用
   ├─ bch2_accounting_trans_commit_hook()
   ├─ BTREE_TRIGGER_atomic 执行
   ├─ bch2_journal_entry_validate()   // 验证 journal entries
   ├─ bch2_bkey_validate()            // 验证每个 key
   ├─ bch2_journal_add_entry()        // 写入 journal entries
   ├─ bch2_btree_insert_key_leaf()    // 执行实际 btree 插入
   └─ bch2_btree_insert_key_cached()  // 或缓存路径写入
6. 释放 write lock（bch2_trans_lock_write 的逆操作）
7. 添加 Journal pin + bch2_journal_res_put()
8. bch2_trans_downgrade()             // SIX 锁降级
```

### 5.2 Trigger 两阶段系统

**位置：** `commit.c`

```c
// 事务内 trigger 阶段：
// 阶段 1 — 事务性（btree_trans_commit_run_triggers）
//   运行 BTREE_TRIGGER_transactional
//   → 生成更多 updates 条目
//   可重启（如需要更多内存）

// 阶段 2 — 原子性（BTREE_TRIGGER_atomic in write_locked）
//   运行 BTREE_TRIGGER_atomic
//   → 运行在写锁定下
//   不可重启（失败 = 致命错误）

// 每个更新条目标记：
// overwrite_trigger_run — 旧键 trigger 是否已运行
// insert_trigger_run — 新键 trigger 是否已运行
```

---

## 6. 提交中的事务重启

### 6.1 错误处理

**位置：** `commit.c:1191-1258` — `__bch2_trans_commit_error()`

| 错误码 | 处理方式 |
|--------|----------|
| `BCH_ERR_journal_res_blocked` | 释放锁，阻塞等待 journal 预留 |
| `BCH_ERR_btree_insert_btree_node_full` | `bch2_btree_split_leaf()` 分裂叶节点；如有 interior update 则重启 |
| `BCH_ERR_btree_insert_need_mark_replicas` | 释放锁，更新超级块副本 |
| `BCH_ERR_btree_insert_need_journal_reclaim` | 释放所有锁，等待 key cache flush / journal reclaim |
| 其他不可恢复错误 | BUG_ON |

### 6.2 提交重试循环

**位置：** `commit.c:1540-1585`

```c
retry:
    bch2_trans_verify_not_unlocked_or_in_restart(trans);
    ret = do_bch2_trans_commit(trans, flags, &errored_at, _RET_IP_);
    if (ret)
        goto err;
    // ... success ...

err:
    ret = bch2_trans_commit_error(trans, flags, errored_at, ret, _RET_IP_);
    if (ret)
        goto out;          // 不可恢复或需外层重启
    goto retry;             // 错误已恢复 → 重试
```

如果 `bch2_trans_commit_error` 返回 0 → 错误已恢复，跳回 retry。否则 → 由外层的 `lockrestart_do` 捕获。

### 6.3 特殊嵌套情况

**位置：** `commit.c:1580-1583`

如果 `BCH_TRANS_COMMIT_no_journal_res` 且错误恢复中发生了嵌套提交 → 必须将嵌套重启传递给外层。

---

## 7. 事务内的内存分配

### 7.1 Bump Allocator

**位置：** `iter.c:3700-3760`

```c
bch2_trans_kmalloc(trans, size)
  ├─ 从 trans->mem 的 bump allocator 分配
  ├─ 位置由 trans->mem_top 追踪
  ├─ 每次 bch2_trans_begin() 时重置 mem_top = 0
  │
  └─ 如果空间不足：
       ├─ 记录所需大小到 trans->realloc_bytes_required
       └─ btree_trans_restart(trans, BCH_ERR_transaction_restart_mem_realloced)
           → bch2_trans_begin() 检测到重分配
           → krealloc() 扩大内存 [iter.c:3835-3857]
```

### 7.2 限制

```c
// fs/btree/types.h:791
#define BTREE_TRANS_MEM_MAX   (1 << 16)    // 64KB
```

如果 kmalloc 失败 → fallback 到 mempool `malloc_pool`（大小 `BTREE_TRANS_MEM_MAX`），设置 `used_mempool = true`。

### 7.3 `struct btree_trans_subbuf`

**位置：** `types.h:806-810`

```c
struct btree_trans_subbuf {
    u64     base;
    unsigned u64s;
    unsigned size;
};
```

用于 `trans->journal_entries` 和 `trans->accounting`，支持提交前预留大小的 buffer。

---

## 8. Btree 写缓冲区

### 8.1 适用范围

**位置：** `write_buffer_types.h:16-27`

写缓冲区适用的 btree 类型（`BCH_WRITE_BUFFER_BTREES()` x-macro）：

- `lru`
- `need_discard`
- `backpointers`
- `deleted_inodes`
- `reconcile_*`
- `accounting`
- `stripe_backpointers`

适用于**大量小更新**的 btree，批量处理比逐一搜索更高效。

### 8.2 数据结构

**位置：** `write_buffer_types.h`

```c
struct btree_write_buffer_keys {
    struct btree_write_buffered_key *keys;   // key 数组
    unsigned nr;                              // 当前数量
    unsigned size;                            // 容量
    struct journal_entry_pin pin;             // journal pin
    struct mutex lock;
};

struct bch_fs_btree_write_buffer {
    struct btree_write_buffer_keys inc;      // 正在接收的入站
    struct btree_write_buffer_keys flushing;  // 正在刷新的
    struct wb_key_ref *sorted;               // 排序后引用
    unsigned sorted_size;
    struct work_struct flush_work;
    atomic64_t accounting;                   // 累计记账 delta
};
```

### 8.3 双缓冲设计

```
入站（inc）← bch2_btree_insert() 直接写入
           ↓
      当需要刷新时，swap(inc, flushing)
           ↓
刷新（flushing）→ 排序 → 去重 → 批量插入 btree
```

### 8.4 Flush 流程

**位置：** `write_buffer.c:552-769`

```c
bch2_btree_write_buffer_flush_locked()
│
├─ 1. move_keys_from_inc_to_flushing()
│       └─ swap(inc, flushing)，更新 journal pin
│
├─ 2. 构建 sorted 数组 [write_buffer.c:583-593]
│       └─ 为 flushing 中每个 key 填充 wb_key_ref
│
├─ 3. wb_sort() — 堆排序 [write_buffer.c:116-159]
│
├─ 4. 预刷新去重 [write_buffer.c:635-662]
│       ├─ 相邻同位置的 key → 丢弃旧版本
│       ├─ 记账 key → 累计 delta
│       └─ 清零已丢弃的 journal_seq
│
├─ 5. 快速路径 [write_buffer.c:475-550]
│       ├─ wb_flush_n_shards(): 如果 n_keys > 4096 → 分片
│       └─ wb_flush_sorted_sharded() → wb_flush_sorted_range()
│           → wb_flush_one(): 在叶节点原地插入（无 journal 预订）
│
├─ 6. 慢速路径
│       └─ 剩余 key 按 journal_seq 排序
│           → commit_do(btree_write_buffered_insert(...))
│
└─ 7. 收尾：清理 flushing buffer、更新统计
```

### 8.5 Sharded 快速路径并发

**位置：** `write_buffer.c:475-550`

```
wb_flush_n_shards()
  └─ 如果 n_keys > 4096
       ├─ sorted 范围分段
       ├─ 每段独立 btree_trans
       └─ workqueue 并行执行
  shard 范围在键空间上不重叠 → 无需同步
```

### 8.6 触发 Flush 的时机

| 触发点 | 位置 | 条件 |
|--------|------|------|
| `wb_maybe_flush()` | `write_buffer.h:30-44` | `keys.nr > keys.size * 3/4`（≥75%） |
| `bch2_btree_write_buffer_journal_flush()` | `write_buffer.c:891-902` | journal pin 回调 |
| `bch2_btree_write_buffer_flush_sync()` | `write_buffer.c:904-913` | 同步刷新 |
| `bch2_btree_write_buffer_tryflush()` | `write_buffer.c:951-972` | 无阻塞尝试 |

### 8.7 Journal → 写缓冲区

**位置：** `write_buffer.c:771-790`

```c
bch2_journal_keys_to_write_buffer()
  └─ journal 重放时将 journal entry 转为写缓冲区 key
  └─ BCH_JSET_ENTRY_write_buffer_keys 类型的 entry 用于记账更新
```

---

## 9. 六状态 Btree 节点锁（SIX 锁）

### 9.1 定义

**位置：** `locking.h:1-40`

SIX 锁是 bcachefs 定制的读写锁，针对日志结构 btree 节点优化。

### 9.2 锁状态

| 状态 | 名称 | 说明 |
|------|------|------|
| S | Shared | 读取者共存 |
| I | Intent | 写入者意图 |
| X | eXclusive | 写入者独占 |

每个状态还有阻塞变体（共 6 个状态）。

### 9.3 锁兼容矩阵

```
    S   I   X
S   ✓   ✓   ✗
I   ✓   ✗   ✗    ← intent-intent 冲突是关键
X   ✗   ✗   ✗
```

**S 和 I 兼容**：读取者不会被 intent 锁持有者阻塞。

### 9.4 SIX 锁设计动机

**位置：** `locking.c:3-12`（DOC_LATEX）

日志结构的 btree 节点可以在**读锁（S）**下写入，因为新的 bset 是追加的，不影响现有 bset。这极大减少了阻塞。

```
传统读写锁：  读者 ❌ 写入者（完全互斥）
SIX 锁：      读者 ✓ 意图写入者（同时读+追加）
```

### 9.5 实现

| 操作 | 位置 | 说明 |
|------|------|------|
| `__bch2_btree_node_lock()` | `locking.c` | S 锁获取 |
| `bch2_btree_node_lock_write()` | `locking.c` | X 锁获取（可能返回 restart） |
| `bch2_btree_node_unlock()` | `locking.c` | 释放锁 |
| `bch2_btree_node_relock()` | `locking.c` | 重获丢失的锁 |
| `bch2_btree_node_upgrade()` | `locking.h` | S→I 升级 |
| `__bch2_btree_path_downgrade()` | `locking.c` | I→S 降级 |

`path->nodes_locked`：位图记录每层 2 位的锁状态（读/意图/写）。

---

## 10. 数据写入案例：从 VFS 到 btree 事务

> 本节通过跟踪一次完整的缓冲写入（buffered write），展示事务子系统如何在实际数据写入中运作。所有概念（生命周期、路径、锁、提交、journal）在此汇聚。

---

### 10.1 写入流程三阶段概述

数据写入在 bcachefs 中分三个独立阶段完成，每个阶段都涉及不同的 btree_trans 使用模式：

```
用户态 write() 系统调用
         │
    ┌────▼────────────────────────────────────┐
    │ 阶段一：VFS 层                          │
    │ 数据写入 page cache，标记脏页            │
    │ 涉及文件：fs/vfs/buffered.c              │
    └──────────┬──────────────────────────────┘
               │ (回写触发)
    ┌──────────▼──────────────────────────────┐
    │ 阶段二：数据写入管道                    │
    │ 分配 bucket、编码数据、提交 BIO           │
    │ • bch2_alloc_sectors_req() → alloc btree │
    │ • bch2_write_extent() → 构建 extent key  │
    │ • bch2_submit_wbio_replicas() → 块层     │
    │ 涉及文件：fs/data/write.c                │
    └──────────┬──────────────────────────────┘
               │ (IO 完成)
    ┌──────────▼──────────────────────────────┐
    │ 阶段三：Btree 索引更新                  │
    │ 将 extent key 提交到 BTREE_ID_extents    │
    │ • bch2_extent_update() → 核心事务       │
    │ • bch2_trans_commit() → journal + btree  │
    │ 涉及文件：fs/btree/commit.c              │
    └─────────────────────────────────────────┘
```

关键点：**数据在阶段二已写入磁盘**，阶段三只更新元数据（extent key）。这种设计使写入延迟不受 btree 操作影响。

---

### 10.2 VFS 入口：`write()` 系统调用到脏页

**入口函数：** `bch2_write_iter()` — `fs/vfs/buffered.c:1131`

```c
bch2_write_iter(iocb, iov_iter)
  ├─ IOCB_DIRECT → bch2_direct_write()        // 直接 IO
  └─ 否则 → bch2_buffered_write()             // 缓冲写入（我们的案例）
```

**缓冲写入路径：** `bch2_buffered_write()` — `fs/vfs/buffered.c:1055`
```
1. 循环调用 __bch2_buffered_write()
   ├─ bch2_filemap_get_contig_folios_d()  ← 获取 page cache folio
   ├─ bch2_get_folio_disk_reservation()   ← 预留磁盘空间（disk reservation）
   ├─ copy_folio_from_iter_atomic()       ← 从用户态拷贝数据到 folio
   └─ bch2_set_folio_dirty()              ← 标记 folio 脏
```

此时数据只在 page cache 中，尚未写入磁盘。

**回写触发：**（之后某个时刻由内核内存管理或 `sync()` 触发）

```c
bch2_writepages()                          // fs/vfs/buffered.c:703
  └─ __bch2_writepage()                    // fs/vfs/buffered.c:571
       └─ bch2_writepage_io_alloc()        // fs/vfs/buffered.c:508
            └─ bch2_writepage_do_io()      // fs/vfs/buffered.c:496
                 └─ closure_call(&io->op.cl, bch2_write, ...)
                      // 进入阶段二
```

---

### 10.3 阶段二：数据写入管道

这一阶段通过 `bch2_write()`（闭包回调）进入统一写入管道。

#### 10.3.1 `bch2_write()` — 统一入口

**位置：** `fs/data/write.c:2751`

```
bch2_write(struct closure *cl)
  │
  ├─ bch2_write_op_init()                  // 从 inode 选项设置 csum/compress/replicas
  │
  ├─ 小数据优化路径（≤ min(block_bytes/2, 1024) 字节）:
  │   └─ bch2_write_data_inline()          // data/write.c:2616
  │        数据 inline 存储为 KEY_TYPE_inline_data
  │        然后直接进入阶段三：__bch2_write_index()
  │
  └─ 正常路径：__bch2_write()              // data/write.c:2485
```

#### 10.3.2 `__bch2_write()` — 主分配/编码/提交循环

**位置：** `fs/data/write.c:2485`

```c
// 先检查 NOCOW 路径（现有 extent 上原地覆盖）
bch2_nocow_write(c, op);  // data/write.c:2505-2508

// COW 主循环
do {
    // ── 步骤 A：分配 bucket ──
    bch2_alloc_sectors_req(c, wp, ...);      // alloc/foreground.c:1468
      ├─ writepoint_find()                   // 按进程 hash 找到/创建 write_point
      ├─ bucket_alloc_set_writepoint()       // 从现有 open_bucket 获取
      ├─ bucket_alloc_set_partial()          // 从部分填充的 bucket 获取
      ├─ bch2_bucket_alloc_set_trans()       // 分配新 bucket（走 freespace btree）
      │   └─ 内部创建 btree_trans            // 涉及 BTREE_ID_freespace
      └─ dev_stripe_increment()             // 跨设备条带化负载均衡

    bch2_open_bucket_get(c, wp, ...);        // 获取 open_bucket 引用

    // ── 步骤 B：数据编码 + extent key 构造 ──
    bch2_write_extent(c, op, wp, ...);       // data/write.c:1904
      │
      ├─ 压缩：bch2_bio_compress()           // data/write.c:1984
      │   （lz4 / zstd / gzip，取决于 inode 选项）
      ├─ 加密：bch2_encrypt_bio()            // data/write.c:2062
      │   （ChaCha20/Poly1305，开启加密时）
      └─ 校验和：bch2_checksum_bio()        // data/write.c:2067
           （CRC-32C / CRC-64 / CRC-128）
      │
      └─ init_append_extent()                // data/write.c:1707
           // 构建 struct bkey_i_extent
           e->k.p     = op->pos              // (inode, file offset)
           e->k.size  = crc.uncompressed_size
           e->k.bversion = version
           bch2_extent_crc_append()           // 追加 CRC entry
           bch2_alloc_sectors_append_ptrs_inlined()
                                              // 追加设备指针
           bch2_keylist_push(&op->insert_keys)
                                              // 入队到 keylist

    // ── 步骤 C：提交 BIO ──
    bio->bi_end_io = bch2_write_endio;        // data/write.c:2588
    bch2_submit_wbio_replicas(c, op, ...);    // data/write.c:2597
      ├─ 遍历 extent key 中每个 bch_extent_ptr
      ├─ 对每个副本（副本数由 inode 选项决定）：
      │   ├─ bch2_dev_get_ioref()            // 获取设备 IO 引用
      │   ├─ bio_alloc_clone() / 复用原始 bio
      │   └─ submit_bio()                    // 提交到块层
      └─ 写入类型：BCH_DATA_user

} while (bch2_keylist_empty(&op->insert_keys));
```

#### 10.3.3 BIO 完成回调

**位置：** `fs/data/write.c:1665`

```c
bch2_write_endio(struct bio *bio)
  ├─ 记录 IO 错误（如果有）
  ├─ bch2_open_bucket_put()                  // 释放 open_bucket
  └─ closure_put(cl)                         // 触发阶段三
```

---

### 10.4 阶段三：Btree 索引更新

这是事务子系统的核心工作区域。IO 完成后，系统创建 btree_trans 来更新 `BTREE_ID_extents` 的 extent key。

#### 10.4.1 索引更新入口

**位置：** `fs/data/write.c:1607`

```c
bch2_write_index(struct closure *cl)
  └─ 排入 index_update_wq（write_point 的工作队列）
      └─ bch2_write_point_do_index_updates()  // data/write.c:1638
           └─ __bch2_write_index()            // data/write.c:1503
```

#### 10.4.2 核心事务：`bch2_write_index_default()` → `bch2_extent_update()`

**位置：** `fs/data/write.c:1062` 和 `fs/data/write.c:993`

```c
bch2_write_index_default(op)
  │
  └─ 对每个 insert_key（lockrestart_do 循环）:
       │
       CLASS(btree_trans, trans)(c)          // ── 创建 btree_trans ──
       // 等价于 bch2_trans_get() + 自动 bch2_trans_put()
       // trans->fn = "bch2_write_index_default"
       │
       bch2_subvolume_get_snapshot(trans, ...) // 获取当前快照 ID
       │
       CLASS(btree_iter, iter)(trans,
           BTREE_ID_extents, pos, ...)       // ── 创建 btree iterator ──
       // 内部调用 bch2_path_get() 分配路径
       │
       bch2_extent_update(trans, &iter, ...) // ── 核心函数 ──
```

**`bch2_extent_update()` 详细事务流程：**

```
bch2_extent_update(trans, iter, k, ...)       // data/write.c:993
  │
  ├── (1) 原子性约束裁剪
  │   bch2_extent_trim_atomic()              // extend_update.c:106
  │   │ 计算需要多少 btree iterator slots
  │   │ 如果超过 EXTENT_ITERS_MAX (64) → 截断 insert->k.p
  │   │ 保证单次事务提交不会溢出迭代器限制
  │
  ├── (2) 计算覆盖扇区变化
  │   bch2_sum_sector_overwrites(trans, ...) // data/write.c:855
  │   │ 遍历与新 extent 重叠的所有旧 extent
  │   │ 计算：
  │   │   i_sectors_delta    = 新扇区 - 旧扇区（文件分配量变化）
  │   │   disk_sectors_delta = 磁盘扇区净变化（新分配 - 旧释放）
  │   │   usage_increasing   = 使用量是否增加
  │   │
  │   └── 如果 usage_increasing:
  │       bch2_disk_reservation_add(c, trans, delta, ...)
  │       调用 bch2_trans_begin() 内部重启事务
  │       （disk reservation 变更可能触发事务重启）
  │
  ├── (3) 更新 inode 元数据
  │   bch2_extent_update_i_size_sectors()    // data/write.c:901
  │   │ 更新 inode 的 bi_size（文件大小）和 bi_sectors（扇区数）
  │   │ 通过 bch2_trans_update(trans, inode_iter, &inode_k, ...)
  │   │ → 对 BTREE_ID_inodes 创建一个 update
  │   │ 小优化：BTREE_UPDATE_nojournal 跳过不需要的 journal 记录
  │
  ├── (4) 标记 reconcile 追踪
  │   bch2_bkey_set_needs_reconcile(k)       // data/write.c:1047
  │   在 extent 上设置 flag，便于后续 reconcile 检测
  │
  ├── (5) 插入 extent key（核心 btree 更新）
  │   bch2_trans_update(trans, iter, k, ...) // → btree/update.c
  │   │                                      // → bch2_trans_update_extent()
  │   │
  │   │  bch2_trans_update_extent()          // btree/update.c:239
  │   │   │
  │   │   ├── 尝试向前合并（与前置 extent 无缝衔接）:
  │   │   │   extent_front_merge()           // btree/update.c:40
  │   │   │   检查：快照相同、offset 连续、dev ptrs 兼容
  │   │   │   如果可合并：extent_front_merge() 更新旧 key 大小
  │   │   │   然后 return（无需插入新 key）
  │   │   │
  │   │   ├── 遍历当前位置的现有 extent:
  │   │   │   bch2_trans_update_extent_overwrite() // btree/update.c:151
  │   │   │   │
  │   │   │   │  处理三种重叠情况：
  │   │   │   │  （a）前拆分：旧 extent 尾部被新 extent 覆盖
  │   │   │   │       → bch2_cut_front(新的起始位置, old_k)
  │   │   │   │  （b）中拆分：新 extent 落在旧 extent 中间
  │   │   │   │       → bch2_cut_front() + bch2_cut_back()
  │   │   │   │  （c）后拆分：旧 extent 头部被新 extent 覆盖
  │   │   │   │       → bch2_cut_back(新的结束位置, old_k)
  │   │   │   │
  │   │   │   │  每个拆分片段通过 bch2_btree_insert_nonextent() 插入
  │   │   │   │  → 内部调用 __bch2_btree_insert()
  │   │   │   │  → trans->updates 队列追加条目
  │   │   │   └── 插入白化（snapshot whiteout）
  │   │   │       bch2_insert_snapshot_whiteouts()
  │   │   │
  │   │   └── 插入新 extent key
  │   │       bch2_btree_insert_nonextent(trans, ...)
  │   │       → trans->updates 队列入队新 key
  │   │
  │   └── 注意：此时所有更新仅在 trans->updates 队列中
  │       尚未写入 btree 或 journal
  │
  └── (6) 提交事务（关键步骤）
      bch2_trans_commit(trans, disk_res, ...)
      │                                    // btree/commit.c:1432
      │
      ├── 第一阶段 — 准备:
      │   ├─ bch2_trans_commit_run_triggers()
      │   │   运行事务性 trigger（BTREE_TRIGGER_transactional）
      │   │   → alloc btree 更新桶引用计数
      │   │   → backpointers btree 更新
      │   │   可能生成更多 updates 条目
      │   │
      │   ├─ noop 过滤：删除旧值相等的 update
      │   │   减少不必要的 journal 写入
      │   │
      │   └─ bch2_disk_reservation_add()
      │       确保磁盘空间已预留
      │
      ├── 第二阶段 — do_bch2_trans_commit():
      │   │                            // btree/commit.c:1087
      │   ├─ bch2_trans_lock_write()   // 获取所有相关叶节点的 X 锁
      │   │   （对 BTREE_ID_extents 和 BTREE_ID_inodes 的叶节点）
      │   │
      │   ├─ bch2_trans_journal_res_get()
      │   │   │                         // 预留 journal 空间
      │   │   └─ bch2_journal_res_get(&trans->journal_res)
      │   │       计算所有 update 条目所需的 journal_u64s
      │   │       从 journal buffer 分配连续空间
      │   │
      │   ├─ bch2_trans_commit_write_locked()
      │   │   │                         // btree/commit.c:830
      │   │   ├─ BTREE_TRIGGER_atomic 运行
      │   │   │   （在写锁保护下的原子 trigger）
      │   │   │
      │   │   ├─ bch2_bkey_validate() on each update
      │   │   │   验证每个 key 的内部一致性
      │   │   │
      │   │   ├─ bch2_journal_add_entry(trans->journal_res,
      │   │   │       BCH_JSET_ENTRY_btree_keys, ...)
      │   │   │   │                      // 写入 journal
      │   │   │   └─ journal_add_entry()
      │   │   │       将 btree_keys 条目复制到 journal buffer
      │   │   │       在 journal entry 中记录 trans->journal_res.seq
      │   │   │
      │   │   ├─ bch2_btree_insert_key_leaf()
      │   │   │   │                      // 修改 btree 叶节点
      │   │   │   ├─ btree_leaf_key.v = k （写入节点内存）
      │   │   │   ├─ bset 排序修正
      │   │   │   └─ journal.seq 记录（用于崩溃恢复）
      │   │   │
      │   │   └─ 或缓存路径：
      │   │       bch2_btree_insert_key_cached()
      │   │       （BTREE_ID_inodes 可能使用 key cache）
      │   │
      │   ├─ bch2_trans_downgrade()     // I→S 锁降级
      │   │
      │   ├─ bch2_journal_pin_add()
      │   │   固定 journal entry 直到 btree 节点写入完成
      │   │
      │   └─ bch2_journal_res_put()     // 释放 journal 预留
      │       → 可能触发实际 journal 写入（journal_write_submit()）
      │
      └── 返回：0（成功）或错误码

  └── bch2_write_done()                     // data/write.c:1450
       ├─ bch2_disk_reservation_put()        // 释放剩余 disk reservation
       ├─ bch2_keylist_free()                // 释放 keylist
       └─ op->end_io()                       // 通知调用者完成
```

---

### 10.5 Extent Key 构造详解

数据写入管道在 `init_append_extent()`（`fs/data/write.c:1707`）中完成 extent key 的构建：

```
struct bkey_i_extent:
  ├─ bkey header:
  │   ├─ k.p.inode     = 当前 inode 编号
  │   ├─ k.p.offset    = 文件偏移量（扇区单位）
  │   ├─ k.size        = crc.uncompressed_size（解压后大小）
  │   └─ k.bversion    = 版本号（原子递增）
  │
  ├─ bch_extent_crc32（未压缩的情况）:
  │   ├─ type          = 0（未压缩/未加密）
  │   ├─ compressed    = 0
  │   ├─ csum_type     = 根据 inode 选项
  │   └─ csum          = 校验和值
  │
  ├─ bch_extent_crc64（压缩或加密的情况）:
  │   ├─ compressed_size   = 压缩后大小
  │   ├─ uncompressed_size = 解压后大小
  │   ├─ live_size         = 有效数据大小
  │   ├─ csum_type         = CRC-32C/64/128
  │   ├─ csum              = 校验和
  │   └─ nonce             = 加密随机数
  │
  └─ bch_extent_ptr × N（每个副本一个）:
      ├─ dev        = 设备 ID
      ├─ offset     = bucket 内偏移
      ├─ gen        = bucket 代际号
      └─ cached     = 是否缓存副本（仅用于读取加速）
```

每个 extent key 可包含多个设备指针（多副本）、多个 CRC 条目（跨越压缩边界）。

---

### 10.6 部分覆盖处理用例

假设一个文件已有 [0, 8) 扇区的 extent，现在写入 [4, 12) 扇区：

```
写入前的 extent btree 状态：
  [0, 8)  extent A  ← 旧 extent

事务中的操作：
  ┌─────────────────────────────────────────────┐
  │ bch2_trans_update_extent()                  │
  │                                              │
  │ 步骤 1: extent_front_merge()                 │
  │   → 检查前置 extent: 无，跳过                │
  │                                              │
  │ 步骤 2: 遍历重叠的已有 extent               │
  │   → 找到 extent A [0, 8)                    │
  │                                              │
  │ 步骤 3: bch2_trans_update_extent_overwrite() │
  │   新 extent [4, 12) 覆盖 [4, 8) 部分         │
  │   这是"后拆分"情况：                          │
  │   └─ bch2_cut_back(4, extent A)               │
  │       将 extent A 从 [0, 8) 截断为 [0, 4)     │
  │      → trans->updates 追加：                 │
  │        - whiteout: 旧 [0,8)                    │
  │        - insert:   [0,4) extent A 截断版      │
  │                                              │
  │ 步骤 4: 插入新 extent                        │
  │   → trans->updates 追加：                    │
  │     - insert: [4,12) extent B（新写入数据）   │
  │                                              │
  │ 步骤 5: bch2_trans_commit()                  │
  │   → journal 记录 3 个更新                     │
  │   → btree 叶节点应用 3 个更新                  │
  └─────────────────────────────────────────────┘

写入后 btree 状态：
  [0, 4)  extent A（截断后）
  [4,12)  extent B（新写入）
```

这完全是元数据操作——物理数据已在阶段二写入磁盘，btree 只是更新了哪些 extent 指向哪些物理块。

---

### 10.7 完整事务流时序图

```
时间 │
     │
     │  bch2_extent_update()
     │    │
     │    ├─ bch2_extent_trim_atomic()
     │    │   └─ 可能触发 bch2_trans_begin() + 重启
     │    │
     │    ├─ bch2_sum_sector_overwrites()
     │    │   ├─ 遍历旧 extent → 计算扇区增量
     │    │   └─ 可能需要 bch2_disk_reservation_add()
     │    │       → 可能触发 bch2_trans_begin() + 重启
     │    │
     │    ├─ bch2_trans_update(&extent_iter, new_k)
     │    │   └─ bch2_trans_update_extent()
     │    │       ├─ extent_front_merge() 尝试合并
     │    │       ├─ bch2_trans_update_extent_overwrite()
     │    │       │   └─ 拆分旧 extent → 多个 trans->updates
     │    │       └─ bch2_btree_insert_nonextent()
     │    │
     │    └─ bch2_trans_commit()
     │        │
     │        ╞══ 第一阶段：准备 ═══
     │        │  ├─ commit_run_triggers()
     │        │  │   → alloc btree 更新（桶引用计数）
     │        │  │   → backpointers btree 更新
     │        │  └─ noop 过滤
     │        │
     │        ╞══ 第二阶段：执行 ═══
     │        │  ├─ bch2_trans_lock_write()
     │        │  │   → BTREE_ID_extents 叶节点：获取 intent(I) 锁
     │        │  │   → BTREE_ID_inodes  叶节点：获取 intent(I) 锁
     │        │  │
     │        │  ├─ bch2_trans_journal_res_get()
     │        │  │   → 分配 journal entries 空间
     │        │  │
     │        │  ├─ bch2_trans_commit_write_locked()
     │        │  │   ├─ atomic triggers 运行
     │        │  │   ├─ bch2_journal_add_entry()
     │        │  │   │   → journal buffer 写入 btree_keys
     │        │  │   ├─ bch2_btree_insert_key_leaf()
     │        │  │   │   → extent btree 叶节点写入新 key
     │        │  │   └─ bch2_btree_insert_key_leaf()
     │        │  │       → inode btree 叶节点写入 i_size 更新
     │        │  │
     │        │  ├─ bch2_trans_downgrade()
     │        │  │   → I→S 降级
     │        │  │
     │        │  └─ bch2_journal_res_put()
     │        │      → 触发 journal_write_submit()
     │        │        → 将 journal buffer 写入磁盘
     │        │
     │        ╞══ 事务结束 ═══
     │        │
     ▼        return 0
```

---

### 10.8 NOCOW 写入路径对比

当 `inode_opt.nocow` 开启且条件满足时，`__bch2_write()` 走 NOCOW 路径：

**位置：** `fs/data/write.c:2293`

```
bch2_nocow_write(c, op)
  │
  ├─ (1) bch2_subvolume_get_snapshot()  // 获取当前快照 ID
  │
  ├─ (2) bch2_btree_iter_peek_slot()
  │     在当前偏移量查找现有 extent
  │
  ├─ (3) bch2_extent_is_writeable()
  │     — 检查：未压缩、无 EC、足够副本、不是快照
  │
  ├─ (4) bkey_get_dev_iorefs()           // 提前获取设备 IO 引用
  │
  ├─ (5) bch2_bkey_nocow_lock()
  │     — 每个 bucket 加锁（防数据移动路径并发）
  │
  ├─ (6) 验证 bucket 代际号未过时
  │
  ├─ (7) bch2_submit_wbio_replicas()     // 直接写入现有位置
  │     — nocow=true，复用现有设备指针
  │
  └─ (8) 完成回调
        bch2_nocow_write_done()
          └─ __bch2_nocow_write_done()
               ├─ bch2_bkey_nocow_lock_unlock()
               ├─ bch2_nocow_write_convert_unwritten()
               │   将 unwritten → written（如果适用）
               └─ bch2_write_done()
```

**与 COW 的对比：**

| 特性 | COW 路径 | NOCOW 路径 |
|------|----------|------------|
| 数据放置 | 新分配 bucket | 原有位置覆盖 |
| bucket 分配 | `bch2_alloc_sectors_req()` | 无（复用现有） |
| extent 构造 | `init_append_extent()` | 复用并修改现有 key |
| btree 事务 | 插入新 extent + 拆分旧 extent | 更新现有 extent 状态 |
| 数据压缩 | 支持 | 不支持 |
| 快照兼容 | 完全支持 | 不支持 |
| 适用场景 | 通用 | overwrite-heavy workload |

---

### 10.9 本案例中涉及的事务概念

| 事务概念 | 在本案例中的体现 | 代码位置 |
|---------|----------------|----------|
| btree_trans 生命周期 | `CLASS(btree_trans, trans)(c)` 自动 get/put | `data/write.c:1062` |
| 路径系统 | `CLASS(btree_iter, iter)(trans, BTREE_ID_extents, ...)` 创建 extents 路径 | `data/write.c:1066` |
| 锁获取 | `bch2_trans_lock_write()` 获取叶节点 X 锁 | `commit.c:144-161` |
| 锁降级 | `bch2_trans_downgrade()` 提交后 I→S | `iter.h:715` |
| 事务重启 | `bch2_sum_sector_overwrites()` 内的 disk_reservation_add | `data/write.c:855` |
| 两阶段提交 | `__bch2_trans_commit()` 准备 + 执行 | `commit.c:1432-1586` |
| Journal 集成 | `bch2_journal_add_entry()` 写入 btree_keys | `commit.c:989-1033` |
| Trigger 系统 | alloc btree 和 backpointers 的事务性 trigger | `commit.c:771-817` |
| 内存分配 | trans->mem bump allocator 内部使用 | `iter.c:3700-3760` |
| Extent 拆分 | `bch2_trans_update_extent_overwrite()` | `btree/update.c:151` |
| 磁盘预留 | `bch2_disk_reservation_add()` / `bch2_disk_reservation_put()` | `alloc/foreground.c` |

---

### 10.10 本案例关键代码位置

| 位置 | 函数 | 作用 |
|------|------|------|
| `fs/vfs/buffered.c:1131` | `bch2_write_iter()` | VFS 写入入口 |
| `fs/vfs/buffered.c:1055` | `bch2_buffered_write()` | 缓冲写入（脏页） |
| `fs/vfs/buffered.c:703` | `bch2_writepages()` | 回写触发 |
| `fs/vfs/buffered.c:496` | `bch2_writepage_do_io()` | 单页 IO 提交 |
| `fs/vfs/direct.c:452` | `bch2_dio_write_loop()` | 直接 IO 写入 |
| `fs/data/write.c:2751` | `bch2_write()` | 统一写入入口 |
| `fs/data/write.c:2485` | `__bch2_write()` | 主分配/编码/IO 循环 |
| `fs/data/write.c:1904` | `bch2_write_extent()` | 数据编码管道 |
| `fs/data/write.c:1707` | `init_append_extent()` | extent key 构造 |
| `fs/data/write.c:2293` | `bch2_nocow_write()` | NOCOW 写入路径 |
| `fs/data/write.c:1665` | `bch2_write_endio()` | BIO 完成回调 |
| `fs/data/write.c:1607` | `bch2_write_index()` | 索引更新入口 |
| `fs/data/write.c:1503` | `__bch2_write_index()` | 索引更新主逻辑 |
| `fs/data/write.c:1062` | `bch2_write_index_default()` | 默认索引更新 |
| `fs/data/write.c:993` | `bch2_extent_update()` | **核心 btree 事务** |
| `fs/data/write.c:855` | `bch2_sum_sector_overwrites()` | 计算覆盖扇区变化 |
| `fs/data/write.c:901` | `bch2_extent_update_i_size_sectors()` | inode 元数据更新 |
| `fs/data/extent_update.c:106` | `bch2_extent_trim_atomic()` | 原子性约束裁剪 |
| `fs/btree/update.c:239` | `bch2_trans_update_extent()` | extent 更新入口 |
| `fs/btree/update.c:40` | `extent_front_merge()` | extent 向前合并 |
| `fs/btree/update.c:151` | `bch2_trans_update_extent_overwrite()` | extent 覆盖拆分 |
| `fs/alloc/foreground.c:1468` | `bch2_alloc_sectors_req()` | 前台 bucket 分配 |
| `fs/alloc/foreground.c:620` | `bch2_bucket_alloc_trans()` | 单 bucket 分配 |


## 11. Direct IO 写入案例

> 本节用结构体图和数据流图展示 Direct IO 写路径的全貌，与 10.10（缓冲写入）对比分析。
> 核心文档：[Direct IO 写路径](../../fs/vfs/direct.c)
> 共同后端：[统一写管道](../../fs/data/write.c)

### 11.1 关键数据结构

```
 struct dio_write {
     struct kiocb             *req;          // VFS IO 请求上下文
     struct address_space     *mapping;      // 文件地址空间
     struct bch_inode_info    *inode;        // bcachefs inode
     struct mm_struct         *mm;           // 用户进程 mm（用于异步续写时切换）
     const struct iovec       *iov;          // 用户 iovec 副本（异步场景）
     unsigned                 loop:1,        // 是否已开始循环
                              extending:1,   // 是否延展文件大小
                              sync:1,        // 同步等待标志
                              sync_done:1,   // 同步 IO 完成标志
                              flush:1;       // 需要 journal flush
     struct quota_res         quota_res;     // 配额预留
     u64                      written;       // 已写入字节数
     struct iov_iter          iter;          // 用户迭代器副本
     struct iovec             inline_vecs[2];// 内联 iovec（免分配路径）
     /* 必须是最后一个字段： */
     struct bch_write_op      op;            // 写操作（嵌入，末端是 bio）
 };

 struct bch_write_op {
     struct closure            cl;           // 闭包（异步完成跟踪）
     struct bch_fs            *c;            // 文件系统
     void                    (*end_io)(struct bch_write_op *); // 完成回调
     u64                      start_time;   // 开始时间（性能统计）
     unsigned                 written;       // 已写扇区数
     u16                      flags;         // BCH_WRITE_* 标志
     s16                      error;         // 错误码
     unsigned                 nr_replicas:4; // 副本数
     unsigned                 watermark:3;   // 分配优先级

     struct bch_devs_list     devs_have;     // 已有设备
     u16                      target;        // 目标设备
     struct bch_inode_opts    opts;          // inode 选项

     u32                      subvol;        // 子卷 ID
     struct bpos              pos;           // 写入位置（inode, offset）
     struct bversion          version;       // 版本号

     struct write_point_specifier write_point; // 写入点选择
     struct write_point       *wp;           // 写入点指针
     struct list_head         wp_list;       // 写入点队列

     struct disk_reservation  res;           // 磁盘空间预留
     struct open_buckets      open_buckets;  // 已分配的 open bucket
     u64                      new_i_size;    // 新的文件大小
     s64                      i_sectors_delta;// inode 扇区计数变化

     struct keylist           insert_keys;   // 待插入的 extent key 列表
     u64 inline_keys[...];                   // 内联 key 存储

     struct bch_devs_mask    *devs_need_flush;// 需要 flush 的设备
     /* 必须是最后一个字段： */
     struct bch_write_bio     wbio;          // 写入 bio
 };

 struct bch_write_bio {
     struct bch_fs            *c;
     struct bch_write_bio     *parent;       // 拆分时指向父 bio
     struct bch_dev           *ca;           // 目标设备
     u64                      submit_time;   // IO 提交时间
     u64                      inode_offset;  // inode 内偏移
     u64                      nocow_bucket;  // NOCOW bucket ID
     struct bch_io_failures   failed;        // 失败设备记录
     unsigned                 split:1,       // 是否拆分 bio
                              bounce:1,      // 是否 bounce 缓冲
                              nocow:1,       // 是否 NOCOW
                              ...;
     struct bio               bio;           // 内核 bio
 };
```

**结构体嵌套关系：**

```
 dio_write  ───→ bch_write_op ───→ bch_write_bio ───→ bio
      │                 │
      ├─ req (kiocb)    ├─ cl (closure)
      ├─ inode          ├─ insert_keys (keylist)
      ├─ iter (iov_iter)├─ open_buckets
      └─ quota_res      ├─ res (disk_reservation)
                         └─ wp (write_point)
```

### 11.2 与缓冲写入的关键差异

| 维度 | 缓冲写入 (Buffered) | 直接写入 (DIO) |
|------|-------------------|----------------|
| 数据源 | 内核 page cache 页 | 用户进程内存页 |
| 页生命周期 | 文件页（writeback 锁定） | 临时 pin（get_user_pages） |
| 页稳定性 | 稳定（`BCH_WRITE_pages_stable`） | **不稳定**（用户可并发修改） |
| 是否需要 bounce | 仅压缩/加密 | 校验和/压缩/加密都需要 |
| 数据到达 disk | 回写时（writeback 线程） | IO 提交后直接 |
| 同步语义 | `O_SYNC` → `generic_write_sync()` | 同步/异步 via `ki_complete` |
| i_size 更新时机 | 写入 page cache 时 | IO 完成后 |
| 错误恢复 | 脏页标记错误 | bio 释放 + 通知调用者 |

**核心差异：** 缓冲写入在 `bch2_buffered_write()` 时只把数据拷贝到 page cache、标记脏页就返回，实际 IO 推后到回写线程。Direct IO 在 `bch2_dio_write_loop()` 中每次循环都完整走完一次 `bch2_write()` → IO → btree 更新。

### 11.3 完整写入流程

```
 bch2_direct_write()
 │  [inode_lock, inode_dio_begin, pagecache_block_get]
 │
 ├─► 如果 file 有 page cache 页：
 │    bch2_write_invalidate_inode_pages_range()
 │    └─ 使范围失效，防止 stale 数据
 │
 ├─► bio_alloc_bioset(..., dio_write_bioset)
 │    └─ 从嵌入的 dio_write 反算指针：
 │       dio = container_of(bio, struct dio_write, op.wbio.bio)
 │
 ├─► 初始化 dio_write 字段
 │
 └─► bch2_dio_write_loop(dio)
       │
       └─► while (仍有数据)
             │
             ├─ (1) bch2_bio_iov_iter_get_pages()
             │     └─ pin 用户内存页到 bio
             │        [如果 fault 导致 dropped_locks：重做 page cache 失效]
             │
             ├─ (2) bch2_write_op_init(&dio->op, c, opts)
             │     └─ 设置 op：target, write_point, nr_replicas, subvol, pos
             │
             ├─ (3) bch2_quota_reservation_add()    // 配额预留
             ├─ (4) bch2_disk_reservation_get()     // 磁盘预留
             │
             ├─ (5) 检查 ENOSPC 时掉落到缓冲写入：
             │     bch2_dio_write_check_allocated()
             │     └─ 遍历 extent btree 看是否已被分配相同数据
             │
             ├─ (6a) [同步模式]
             │     closure_call(bch2_write)            // 直接等完成
             │     bch2_dio_write_end(dio)             // 更新状态
             │
             │  (6b) [异步模式]
             │     closure_call(bch2_write)            // 返回 EIOCBQUEUED
             │     └─ 完成后回调 bch2_dio_write_end()
             │         └─ bch2_dio_write_done()
             │             └─ ki_complete() 通知用户
             │
             └─► 还有数据 → 继续循环；否则 → bch2_dio_write_done()

```

### 11.4 后端写管道详解

`closure_call(&dio->op.cl, bch2_write, NULL, NULL)` 进入统一写管道：

```
 bch2_write(op)
 │  [async_object_list_add, keylist_init, wbio_init]
 │
 ├─► 小数据 + inline_data 开启：
 │    bch2_write_data_inline(op)
 │    └─ 数据嵌入 extent key，跳过磁盘 IO，直接更新 btree
 │
 └─► __bch2_write(op)
       │
       └─► do { 分配 + 写入循环 } while (ret > 0)
             │
             ├─► (1) 分配 bucket 空间：
             │     bch2_alloc_sectors_req(trans, req, write_point, &wp)
             │     └─ lockrestart_do() 中：
             │          alloc_request_get()     → 分配请求
             │          bch2_alloc_sectors_req() → open_bucket 分配
             │
             ├─► (2) 获取 open bucket：
             │     bch2_open_bucket_get(c, wp, &op->open_buckets)
             │
             ├─► (3) 数据编码管道：
             │     bch2_write_extent(op, wp, &bio)
             │     │
             │     ├─► 判断是否需要 bounce buffer：
             │     │   if (ec_buf || compression || 
             │     │       (csum && !pages_stable) ||
             │     │       (encryption && !pages_owned))
             │     │       → bch2_write_bio_alloc()  // 分配 bounce bio
             │     │         bounce = true
             │     │
             │     ├─► do {
             │     │     ├─ 压缩（如启用）
             │     │     ├─ 校验和计算
             │     │     ├─ 加密（如启用）
             │     │     ├─ init_append_extent()     // 构造 extent key
             │     │     └─ } while (还有数据)
             │     │
             │     └─► 返回编码后的 bio
             │
             ├─► (4) 提交 IO：
             │     bio->bi_end_io = bch2_write_endio
             │     bch2_submit_wbio_replicas(wbio, c, BCH_DATA_user, ...)
             │     └─ 写入到每个副本设备
             │
             └─► (5) IO 完成：
                   bch2_write_endio(bio)
                   └─ closure_put(&op->cl)  → 触发 bch2_write_index
```

### 11.5 Bounce Buffer 决策树

Direct IO 的页稳定性与缓冲写入不同，bounce 决策至关重要：

```
                  ┌─ 用户内存页（被 pin）
                  │
                  ▼
    bch2_write_extent()
          │
          ├─ EC 启用？        ───→ YES → 分配 bounce bio + copy
          │
          ├─ 压缩启用？        ───→ YES → 分配 bounce bio + compress → bounce bio
          │
          ├─ 校验和启用？
          │    └─ pages_stable? ──→ NO │
          │                        → YES│
          │                           └──→ 分配 bounce bio + copy → 计算校验和
          │
          ├─ 加密启用？
          │    └─ pages_owned? ──→ NO → 分配 bounce bio + encrypt
          │
          └─ 全部 NO ─────────→ 直接使用用户页 bio → 提交 IO
                                      ↑
                              【Direct IO 零拷贝路径】
                              无压缩、无校验和、无加密、无 EC
```

**关键区别：**

| 场景 | 缓冲写入 | Direct IO |
|------|---------|-----------|
| 无校验和、无压缩 | 无需 bounce（page cache 页稳定） | 无需 bounce（直接 pin 用户页） |
| 有校验和、无压缩 | 无需 bounce（`pages_stable`） | **需要 bounce**（用户页不稳定） |
| 有压缩 | 需要 bounce | 需要 bounce |
| 有加密 | 需要 bounce | 需要 bounce |

### 11.6 索引更新（btree 事务）

IO 完成后进入索引更新阶段（与缓冲写入共享同一路径）：

```
 bch2_write_endio(bio)
   └─ closure_put(&op->cl)
        │
        └─► bch2_write_index(&op->cl)    ← closure 回调
              └─ queue_work(index_update_wq)   ← 排队到工作队列
                    │
                    └─► bch2_write_point_do_index_updates()
                          │
                          └─► __bch2_write_index(op)
                                │
                                ├─► IO 错误处理：
                                │    bch2_write_drop_io_error_ptrs()
                                │    └─ 移除失败设备的指针（degraded 写入）
                                │
                                └─► bch2_write_index_default(op)
                                      │
                                      └─► do {
                                            ├─ bch2_subvolume_get_snapshot()
                                            ├─ CLASS(btree_iter, iter)(extents)
                                            └─ bch2_extent_update(trans, inum,
                                                     &iter, sk.k, ...)
                                                   ├─ bch2_sum_sector_overwrites()
                                                   ├─ bch2_extent_trim_atomic()
                                                   ├─ bch2_trans_update()  // 更新 extent
                                                   └─ bch2_trans_commit()  // 提交事务
                                                   [内含: journal, trigger, unlock]
                                      } while (!keylist_empty)
```

**btree 事务的三阶段提交流程**（10.7 节已详述）：

```
 bch2_trans_commit()
   │
   ├─ 阶段一：bch2_trans_commit_run_triggers()
   │   └─ extent trigger 执行：alloc btree + backpointers 更新
   │
   ├─ 阶段二：bch2_trans_commit_write_locked()
   │   ├─ bch2_journal_add_entry()    → journal 写
   │   └─ bch2_btree_insert_key_leaf()→ btree 节点内存更新
   │
   └─ 阶段三：bch2_trans_commit_write_unlocked()
       └─ btree_node_write_done()     → btree 节点异步写盘
```

### 11.7 Direct IO 写完成回调

```
 __bch2_write_index(op)
   │
   ├─ bch2_open_buckets_put()         // 释放 open bucket -> freelist
   └─ bch2_write_done(&op->cl)
         │
         ├─ disk_reservation_put()
         ├─ keylist_free()
         └─ op->end_io(op)
                │
                ├─ [同步] bch2_dio_write_sync_done(op)
                │     └─ dio->sync_done = true
                │
                └─ [异步] bch2_dio_write_loop_async(op)
                      └─ bch2_dio_write_end(dio)
                            │
                            ├─ req->ki_pos += written
                            ├─ dio->written += written
                            ├─ [延展] i_size_write()
                            ├─ __bch2_i_sectors_acct()
                            ├─ __bch2_quota_reservation_put()
                            ├─ bio_release_pages()  // unpin 用户页
                            │
                            └─► 还有数据？
                                ├─ YES → bch2_dio_write_continue()
                                │          └─ bch2_dio_write_loop()
                                │
                                └─ NO  → bch2_dio_write_done()
                                           ├─ pagecache_block_put()
                                           ├─ inode_dio_end()
                                           └─ ki_complete()  // 通知用户
```

### 11.8 Direct IO vs 缓冲写入的 Crash 一致性对比

| 阶段 | 缓冲写入 | Direct IO | 数据位置 |
|------|---------|-----------|---------|
| **调用者返回后** | 数据在 page cache（未落盘） | 数据在 bio 中（正在提交，或已完成） | DIO: 设备 / Buffered: 内存 |
| **写入完成、btree 更新前崩溃** | page cache 脏页丢失 | bio 数据丢失 | 数据丢失 |
| **btree 更新、btree node 落盘前崩溃** | 数据在设备但无索引（GC 发现 unreferenced bucket，reap） | 同左 | 设备上孤数据 |
| **btree node 落盘后** | 数据完整（journal replay 保证） | 数据完整 | 完整持久化 |

**关键差异场景：**

```
 [缓冲写入]
 write() return OK
   │
   ├─ 数据仅在 page cache
   ├─ 之后可能被 writeback 刷盘 → 自动重试
   └─ 崩溃 → 数据丢失（除非 O_SYNC/fsync）

 [Direct IO 同步]
 write() return OK
   │
   ├─ 数据已到设备
   ├─ btree 索引已更新
   └─ 崩溃 → journal replay 恢复

 [Direct IO 异步]
 write() return -EIOCBQUEUED
   │
   └─ ki_complete() 回调
        │
        ├─ 调用时：数据已到设备 + btree 已更新
        └─ 回调前崩溃 → 用户通过 AIO 事件循环发现未完成
```

### 11.9 本案例关键代码位置

| 位置 | 函数 | 作用 |
|------|------|------|
| `fs/vfs/direct.c:605` | `bch2_direct_write()` | DIO 入口：锁、bio 分配、page cache 失效 |
| `fs/vfs/direct.c:452` | `bch2_dio_write_loop()` | DIO 主循环：pin 页、配额/预留、调 write |
| `fs/vfs/direct.c:416` | `bch2_dio_write_end()` | DIO 结束：i_size、配额、解 pin 页 |
| `fs/vfs/direct.c:380` | `bch2_dio_write_done()` | DIO 完成：清理、ki_complete |
| `fs/vfs/direct.c:351` | `bch2_dio_write_flush()` | journal flush + nocow flush |
| `fs/vfs/direct.c:578` | `bch2_dio_write_continue()` | 异步续写（kthread_use_mm） |
| `fs/vfs/direct.c:592` | `bch2_dio_write_loop_async()` | 异步完成后的循环出口 |
| `fs/vfs/direct.c:289` | `bch2_dio_write_check_allocated()` | ENOSPC 检查是否已有分配 |
| `fs/vfs/direct.c:310` | `bch2_dio_write_copy_iov()` | 异步场景复制 iovec |
| `fs/vfs/direct.h:17` | `struct dio_write` | DIO 写上下文 |
| `fs/data/write_types.h:75` | `struct bch_write_op` | 写操作上下文 |
| `fs/data/write_types.h:45` | `struct bch_write_bio` | 写 bio（含设备错误记录） |
| `fs/data/write.c:2751` | `bch2_write()` | 写入口（与 buffered 共享） |
| `fs/data/write.c:2485` | `__bch2_write()` | 分配 + 编码 + IO 提交 |
| `fs/data/write.c:1904` | `bch2_write_extent()` | 数据编码管道（含 bounce 决策） |
| `fs/data/write.c:1665` | `bch2_write_endio()` | BIO 完成回调 |
| `fs/data/write.c:1607` | `bch2_write_index()` | 索引更新入口 |
| `fs/data/write.c:1503` | `__bch2_write_index()` | 索引更新主逻辑 |
| `fs/data/write.c:1062` | `bch2_write_index_default()` | 默认索引更新（btree 事务） |
| `fs/data/write.c:993` | `bch2_extent_update()` | **核心 btree 事务** |
| `fs/btree/commit.c:1432` | `__bch2_trans_commit()` | 三阶段事务提交 |


## 12. 参考代码位置

### 事务生命周期

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/btree/iter.c` | 3996-4057 | `__bch2_trans_get()` — 事务创建 |
| `fs/btree/iter.c` | 3811-3927 | `bch2_trans_begin()` — 事务重置 |
| `fs/btree/iter.c` | 4101-4166 | `bch2_trans_put()` — 事务销毁 |
| `fs/btree/iter.c` | 3700-3760 | `bch2_trans_kmalloc()` — 内存分配 |
| `fs/btree/iter.h` | 662-699 | 重启函数和故障注入 |
| `fs/btree/iter.h` | 1188-1227 | `lockrestart_do` / `nested_lockrestart_do` 宏 |
| `fs/btree/update.h` | 677 | `commit_do` 宏 |
| `fs/btree/init.c` | 315-356 | btree 子系统初始化 |

### 路径系统

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/btree/types.h` | — | `struct btree_path`、`struct btree_path_level` |
| `fs/btree/iter.c` | — | `btree_path_cmp()`、`bch2_path_get()`、`bch2_path_put()` |
| `fs/btree/iter.c` | 1150 | `btree_path_down()` — 路径下行 |
| `fs/btree/iter.c` | 1010 | `btree_path_prefetch()` — 预取 |

### 提交流程

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/btree/commit.c` | 1432-1586 | `__bch2_trans_commit()` — 完整入口 |
| `fs/btree/commit.c` | 1087-1169 | `do_bch2_trans_commit()` — 第二阶段 |
| `fs/btree/commit.c` | 830-1047 | `bch2_trans_commit_write_locked()` |
| `fs/btree/commit.c` | 1191-1258 | `__bch2_trans_commit_error()` — 错误恢复 |
| `fs/btree/commit.c` | 144-161 | `bch2_trans_lock_write()` — 写锁获取 |
| `fs/btree/commit.c` | 771-817 | trigger 两阶段运行 |

### 写缓冲区

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/btree/write_buffer_types.h` | 16-125 | 全部类型 |
| `fs/btree/write_buffer.c` | 552-769 | `bch2_btree_write_buffer_flush_locked()` |
| `fs/btree/write_buffer.c` | 475-550 | `wb_flush_n_shards()` — 并行分片 |
| `fs/btree/write_buffer.c` | 116-159 | `wb_sort()` — 堆排序 |
| `fs/btree/write_buffer.c` | 635-662 | 预刷新去重 |
| `fs/btree/write_buffer.c` | 891-972 | 各种 flush 触发点 |
| `fs/btree/write_buffer.c` | 771-790 | `bch2_journal_keys_to_write_buffer()` |
| `fs/btree/write_buffer.h` | 30-44 | `wb_maybe_flush()` — 自动触发 |

### 锁系统

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/btree/locking.h` | 1-40 | SIX 锁 API 和兼容矩阵 |
| `fs/btree/locking.c` | 全部 | `__bch2_btree_node_lock()` 等全部实现 |
| `fs/btree/locking_types.h` | 全部 | `struct lock_graph`、`struct trans_waiting_for_lock` |
| `fs/btree/types.h` | — | `path->nodes_locked` 位图 |
| `fs/btree/iter.h` | 701-715 | 锁升级/降级 |
