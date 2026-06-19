# freespace btree 更新原子性

> 分析日期：2026-05-17
> 隶属分析系列：[启动分配器分析索引](bcachefs-alloc-analysis-index.md)

---

## 目录

1. [核心设计：freespace 是派生索引](#核心设计freespace-是派生索引)
2. [两阶段 trigger 机制](#两阶段-trigger-机制)
3. [事务提交保证](#事务提交保证)
4. [genbits 安全网](#genbits-安全网)
5. [运行时交叉检查](#运行时交叉检查)
6. [恢复如何处理部分更新](#恢复如何处理部分更新)
7. [重建路径：`bch2_fs_freespace_init`](#重建路径bch2_fs_freespace_init)
8. [相关函数速查](#相关函数速查)

---

## 核心设计：freespace 是派生索引

`BTREE_ID_freespace` 是一个**派生的缓存**，**不是权威数据源**（not source of truth）。真相在 `BTREE_ID_alloc` 中——每个 bucket 的 `data_type`、`gen`、扇区计数存储在那里。

freespace btree 通过 btree trigger 系统作为 alloc key 变更的**副作用**来维护。这意味着：

- 从不直接向 freespace btree 写入（除 `bch2_fs_freespace_init` 重建外）
- freespace 的内容**总是**通过 alloc btree 的状态派生
- 每次 alloc key 被修改，`bch2_trigger_alloc` 确保 freespace 保持同步

---

## 两阶段 trigger 机制

**核心文件：**
- `fs/alloc/background.c:1232-1478` — `bch2_trigger_alloc`
- `fs/btree/commit.c:771, 830, 939, 1432` — commit 引擎

当一个 alloc key 被修改，commit 分两个阶段处理 trigger：

### Phase 1：事务性 Trigger（`BTREE_TRIGGER_transactional`）

**运行时机：** `commit.c:771` — `bch2_trans_commit_run_triggers()`

**在 journal 预留之前运行。** 在这个阶段，trigger 可以调用 `bch2_trans_update()` 向**同一个事务**添加额外的 btree 更新。

freespace 更新在第 1360-1365 行排队：

```c
// fs/alloc/background.c:1360-1365
if (statechange(a->data_type == BCH_DATA_free) ||
    (new_a->data_type == BCH_DATA_free &&
     alloc_freespace_genbits(*old_a) != alloc_freespace_genbits(*new_a))) {

    try(bch2_bucket_do_freespace_index(trans, ca, op.old, old_a, false));
    // ↑ 删除旧的 freespace 条目（如果桶不再空闲）

    try(bch2_bucket_do_freespace_index(trans, ca, op.new.s_c, new_a, true));
    // ↑ 插入新的 freespace 条目（如果桶变为空闲）
}
```

`bch2_bucket_do_freespace_index`（`background.c:1102-1125`）会分配一个 `KEY_TYPE_set` 或 `KEY_TYPE_deleted` 键，调用 `bch2_trans_update()` 将其加入 `BTREE_ID_freespace`——与 alloc key 更新在**同一个事务**中：

```c
int bch2_bucket_do_freespace_index(struct btree_trans *trans,
    struct bch_dev *ca, struct bkey_s_c k,
    const struct bch_alloc_v4 *a, bool set)
{
    struct bpos pos = alloc_freespace_pos(k.k->p, *a);
    return   set
        ? bch2_btree_bit_mod_iter(trans, BTREE_ID_freespace, pos, true)
        : bch2_btree_bit_mod_iter(trans, BTREE_ID_freespace, pos, false);
}
```

### Phase 2：原子性 Trigger（`BTREE_TRIGGER_atomic`）

**运行时机：** `commit.c:939`

**在 journal 预留之后运行（point of no return）。** 处理内存中副作用：
- 更新 `bucket_gen[]` 运行时数组
- 唤醒分配等待器
- 记录 `journal_seq_nonempty` / `journal_seq_empty` 时间戳
- 通过 write buffer 更新 need_discard btree

---

## 事务提交保证

`btree_trans` 系统保证同一事务中的所有更新同时提交或同时失败：

```
__bch2_trans_commit()                    [commit.c:1432]
  │
  ├─ bch2_trans_commit_run_triggers()    [commit.c:771]
  │    // 运行事务性 trigger（可以排队更多更新到事务中）
  │    // alloc trigger → freespace 更新加入同一事务
  │
  ├─ bch2_trans_commit_write_locked()    [commit.c:830]
  │    ├─ 过滤 noop 更新
  │    ├─ 获取 journal 预留（覆盖所有更新）
  │    ├─ bch2_trans_commit_run_triggers() // 运行原子性 trigger
  │    │    [commit.c:939]
  │    ├─ 将所有键写入各自 btree 叶子节点
  │    └─ 写入单个 journal 条目
```

**如果事务中止**（例如 `BCH_ERR_transaction_restart`）：
- 所有更新被丢弃
- 整个操作重试
- alloc key 更新和 freespace key 更新**永不部分应用**

---

## genbits 安全网

**文件：** `fs/alloc/background.h:174-183`

freespace 键将 `alloc_gc_gen(a) >> 4` 编码到 offset 的高 8 位中：

```c
static inline u64 alloc_freespace_genbits(struct bch_alloc_v4 a) {
    return ((u64) alloc_gc_gen(a) >> 4) << 56;
}

static inline struct bpos alloc_freespace_pos(struct bpos pos, struct bch_alloc_v4 a) {
    pos.offset |= alloc_freespace_genbits(a);
    return pos;
}
```

### 为什么需要 genbits

考虑以下崩溃 + 恢复场景：

```
1. GC 运行，设置桶的 gc_gen = 32
2. 分配该桶，数据写入，free → user
3. freespace 条目被删除（在 trigger 中）
4. 系统崩溃
5. GC 重新运行，gc_gen = 33
6. 桶变为空闲，新的 freespace 条目有 genbits = 1 (33>>4)
```

如果没有 genbits，旧的 freespace 条目可能持有错误的 gen 信息。有了 genbits，跨 GC 周期的 freespace 条目会因 genbits 不匹配而被运行时交叉检查拒绝和自动删除。

---

## 运行时交叉检查：每次分配都验证

**文件：** `fs/alloc/foreground.c:323-345` — `try_alloc_bucket`

前台分配器从不盲目信任 freespace 条目。对于每个候选：

```c
static struct open_bucket *try_alloc_bucket(
    struct btree_trans *trans, struct bch_dev *ca,
    struct open_bucket *ob, struct btree_iter *iter)
{
    // 如果应跳过此 bucket（超时 / 游标环），提前返回
    if (bch2_bucket_alloc_update_unavailable(trans, ca, iter))
        return NULL;

    // 交叉验证：读取实际 alloc 键
    ret = bch2_check_freespace_key_async(c,
        &trans->fs_usage_workspace, k, &unavail);
    if (ret) goto err;

    // 一致性检查通过
    return bch2_open_bucket_get(c, ca, ob);
}
```

`__bch2_check_freespace_key`（`fs/alloc/check.c:423-482`）验证：
1. **`a->data_type == BCH_DATA_free`** — 桶真的空闲
2. **genbits == alloc_freespace_genbits(*a)** — 代龄匹配

如果任一检查失败，freespace 条目被异步删除，跳过该桶：

```c
// check.c:350 — delete_freespace_key
// 通过 workqueue 调度删除，避免递归到分配器
```

---

## 恢复如何处理部分更新

### Journal Replay（第一道防线）

由于 alloc 更新和 freespace 更新在同一个 journal 条目中，replay 同时应用它们：

```c
// journal replay 会重新运行 trigger
// alloc key 更新 → trigger → freespace 条目自动重新生成
```

### `check_alloc_info` recovery pass（PASS #10）

**文件：** `fs/init/passes_format.h:81`

```c
BCH_RECOVERY_PASS(check_alloc_info,
    PASS_ONLINE | PASS_FSCK_ALLOC,     // 在线 fsck 中运行
    PASS_ALWAYS,                       // 始终运行
    10, ...)                           // PASS #10
```

此 pass 执行完整的 alloc ↔ freespace 交叉检查：

1. **遍历 alloc btree**：对每个键，验证 `data_type == free ⇔ freespace 条目存在`
2. **检查 alloc 空洞**：`bch2_check_alloc_hole_freespace` — 如果 alloc 某段没有 key，freespace 中也需要有对应条目
3. **独立遍历 freespace btree**：`__bch2_check_freespace_key` 验证每个条目

如果 `freespace_initialized == false`，整个 pass 跳过验证（`check.c:164, 243`）。

### 从头重建

**文件：** `fs/alloc/check.c:840-875`

`bch2_fs_freespace_init` 通过扫描 alloc btree，为所有 `data_type == free` 的桶插入 `KEY_TYPE_set`，从头构建 freespace btree：

```c
int bch2_fs_freespace_init(struct bch_fs *c)
{
    for_each_member_device(c, ca) {
        if (ca->mi.freespace_initialized)
            continue;
        try(bch2_dev_freespace_init(c, ca, 0, ca->mi.nbuckets));
    }
}
```

---

## 重建路径：`bch2_fs_freespace_init`

**文件：** `fs/alloc/check.c:840-875`

```c
int bch2_fs_freespace_init(struct bch_fs *c)
{
    if (c->sb.features & BIT_ULL(BCH_FEATURE_small_image))
        return 0;

    /*
     * 设备添加路径可能崩溃，所以我们需要在每次挂载时检查
     */
    bool doing_init = false;
    for_each_member_device(c, ca) {
        if (ca->mi.freespace_initialized)
            continue;

        if (!doing_init) {
            bch_info(c, "initializing freespace");
            doing_init = true;
        }

        try(bch2_dev_freespace_init(c, ca, 0, ca->mi.nbuckets));
    }

    if (doing_init) {
        guard(memalloc_flags)(PF_MEMALLOC_NOFS);
        guard(mutex)(&c->sb_lock);
        bch2_write_super(c);
    }
    return 0;
}
```

被调用时机：
- **`bch2_fs_initialize`**（mkfs 路径）— `recovery.c:1088`
- **`bch2_dev_add`**（在线设备添加）— `dev.c:1225`

注意：**正常挂载时不调用**此函数（恢复路径 `__bch2_fs_recovery` 不包含此调用）。

---

## 相关函数速查

| 函数 | 文件:行号 | 角色 |
|---|---|---|
| `bch2_trigger_alloc` | `background.c:1232` | 主 trigger；事务阶段排队 freespace 更新 |
| `bch2_bucket_do_freespace_index` | `background.c:1102` | 插入/删除单个 freespace 条目 |
| `bch2_bucket_alloc_freelist` | `foreground.c:437` | 从 freespace btree 扫描分配 |
| `__bch2_check_freespace_key` | `check.c:423` | 交叉验证 freespace 条目 vs alloc btree |
| `bch2_check_freespace_key_async` | `check.h:20` | 分配器热路径中使用的内联包装 |
| `bch2_check_alloc_info` | `check.c:631` | 恢复 pass：完整 alloc ↔ freespace 交叉检查 |
| `bch2_check_alloc_hole_freespace` | `check.c:235` | 修复与 alloc 空洞匹配的 freespace 空洞 |
| `bch2_fs_freespace_init` | `check.c:840` | 从头重建 freespace |
| `bch2_btree_bit_mod_iter` | `btree/update.c:810` | 底层 KEY_TYPE_set/deleted 插入 |
| `__bch2_trans_commit` | `btree/commit.c:1432` | 两阶段 commit 引擎 |
| `alloc_freespace_pos` | `background.h:179` | genbits 编码到 freespace 键位置 |
| `alloc_freespace_genbits` | `background.h:174` | 从 alloc_v4 提取 genbits |
