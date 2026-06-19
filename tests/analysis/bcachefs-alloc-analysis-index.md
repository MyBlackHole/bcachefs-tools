# bcachefs 启动分配器分析索引

> 分析日期：2026-05-17
> 基于代码版本：bcachefs-tools v1.37.2

---

## 背景与问题发现

先前的 bootstrap 分析中有一个关键理解错误：

**错误理解：** 认为 `dev_freespace_init_iter` 在 mkfs 期间逐个 bucket 插入 alloc 键预留 journal，然后 `bch2_bucket_alloc_early` 再扫描这些键定位空闲区间。

**实际行为：**

- `dev_freespace_init_iter` **不是**逐个 bucket 插入 freespace 信息
- 它向 alloc btree 写入 journal 区域的 alloc 键后，用 `bch2_get_key_or_hole` 找空洞
- 为每个空洞创建**一个** freespace extent key 覆盖整个区域
- `bch2_bucket_alloc_early` 是**只**在 `freespace_initialized == false` 时的备用路径
- 正常路径走 `bch2_bucket_alloc_freelist`，从 freespace btree 分配

---

## 子文档索引

| # | 文档 | 核心内容 | 涉及文件 |
|---|------|---------|----------|
| 1 | [`bcachefs-btree-iter-slots.md`](bcachefs-btree-iter-slots.md) | `BTREE_ITER_slots` 内部机制、`peek_slot` vs `peek`、`bch2_get_key_or_hole` 完整实现 | `fs/btree/iter.c`, `fs/alloc/check.c` |
| 2 | [`bcachefs-bucket-alloc-early.md`](bcachefs-bucket-alloc-early.md) | `bch2_bucket_alloc_early` 完整流程、`__try_alloc_bucket`、早路径 vs 正常路径对比 | `fs/alloc/foreground.c` |
| 3 | [`bcachefs-freespace-atomicity.md`](bcachefs-freespace-atomicity.md) | freespace btree 更新原子性、两阶段 trigger、genbits 安全网、recovery 恢复机制 | `fs/alloc/background.c`, `fs/btree/commit.c` |
| 4 | [`bcachefs-journal-bucket-lifecycle.md`](bcachefs-journal-bucket-lifecycle.md) | journal bucket mkfs 分配、挂载加载、运行时扩容、`bch2_set_nr_journal_buckets` | `fs/journal/init.c`, `fs/init/fs.c` |
| 5 | [`bcachefs-freespace-init-lifecycle.md`](bcachefs-freespace-init-lifecycle.md) | `dev_freespace_init` 生命周期（3 个调用路径）、mkfs vs mount 行为差异、`freespace_initialized` 标志 | `fs/alloc/check.c`, `fs/init/recovery.c`, `fs/init/dev.c` |
| 6 | [`bcachefs-bucket-runtime-state.md`](bcachefs-bucket-runtime-state.md) | 桶运行时状态初始化（io_clock、discard、sector 计数）、`bch2_alloc_to_v4` 处理、完整状态转换图 | `fs/alloc/background.c`, `fs/alloc/format.h`, `fs/alloc/discard.c` |

---

## 案例研究

除分配器核心分析外，以下案例研究文档覆盖了在分析过程中深入学习的子系统：

| # | 文档 | 核心内容 | 涉及文件 |
|---|------|---------|----------|
| 7 | [`bcachefs-fsck-and-self-healing.md`](bcachefs-fsck-and-self-healing.md) | fsck 通道系统、自愈层次结构、btree 修复机制、journal replay | `fs/btree/fsck.c`、`fs/btree/fsck_types.h`、`fs/btree/repair.c` |
| 8 | [`bcachefs-snapshot-data-case-study.md`](bcachefs-snapshot-data-case-study.md) | 快照树三 btree 结构、COW 写入、子树删除算法、skiplist、btree 迭代过滤器 | `fs/snapshot.c`、`fs/snapshot.h` |
| 9 | [`bcachefs-transaction-implementation.md`](bcachefs-transaction-implementation.md) | btree_trans 生命周期、事务重启机制、路径系统、SIX 锁、两阶段提交、写缓冲区 | `fs/btree/iter.c`、`fs/btree/commit.c`、`fs/btree/locking.c`、`fs/btree/write_buffer.c` |

这些是**非分配器案例研究**，作为分配器分析的副产品，记录了反向工程学习过程中深入探索的子系统。事务文档是所有分配器操作的基础设施。

---

## 阅读顺序

```
理解基本背景和修复
  └─ 本文（索引）
      │
      ├─ ① btree-iter-slots       → peek_slot 和 get_key_or_hole 的底层机制
      ├─ ⑤ freespace-init-lifecycle → dev_freespace_init 何时被调用、做什么
      │      │
      │      ├─ ② bucket-alloc-early → 早分配路径的具体实现
      │      ├─ ③ freespace-atomicity → 正常路径的原子性保证
      │      └─ ④ journal-bucket-lifecycle → journal 桶分配的生命周期
      │
      └─ ⑥ bucket-runtime-state    → 桶状态如何在运行时初始化和管理
```

---

## 关键设计原则

1. **freespace btree 是派生索引**：真相在 `BTREE_ID_alloc`，freespace 通过 `bch2_trigger_alloc` 作为副作用维护
2. **alloc btree 在 bucket 分配时不被修改**：`__try_alloc_bucket` 只创建 `open_bucket`，实际 alloc 键更新通过 extent trigger 在数据写入时完成
3. **事务保证原子性**：alloc 和 freespace 更新在同一事务中提交，永不部分应用
4. **genbits 防止 stale entries**：freespace 键编码 `gc_gen >> 4` 到 offset 高位，跨 GC 周期安全
5. **`bch2_bucket_alloc_early` 是备用路径**：仅 freespace btree 未初始化时使用，正常路径走 `bch2_bucket_alloc_freelist`

---

## 参考代码位置速查

| 文件 | 核心内容 |
|------|---------|
| `fs/alloc/check.c` | `get_key_or_hole`、`dev_freespace_init_iter`、`bch2_dev_freespace_init`、`bch2_fs_freespace_init` |
| `fs/alloc/foreground.c` | `bch2_bucket_alloc_early`、`__try_alloc_bucket`、`bch2_bucket_alloc_freelist`、`bch2_bucket_alloc_trans` |
| `fs/alloc/background.c` | `bch2_trigger_alloc`、`bch2_bucket_do_freespace_index`、`__bch2_alloc_to_v4` |
| `fs/alloc/discard.c` | `__discard_mark_free` |
| `fs/btree/iter.c` | `bch2_btree_path_peek_slot`、`bch2_btree_iter_peek_slot`、`bch2_btree_iter_next_slot` |
| `fs/btree/commit.c` | 两阶段 trigger 系统 |
| `fs/journal/init.c` | `bch2_dev_journal_init`、`bch2_fs_journal_alloc`、`bch2_set_nr_journal_buckets_iter` |
| `fs/init/recovery.c` | `bch2_fs_initialize`（mkfs）、`__bch2_fs_recovery`（挂载） |
| `fs/init/dev.c` | `bch2_dev_add`、`bch2_dev_online`、`__bch2_dev_resize_alloc` |
