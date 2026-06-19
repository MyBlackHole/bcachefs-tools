# `dev_freespace_init` 生命周期

> 分析日期：2026-05-17
> 隶属分析系列：[启动分配器分析索引](bcachefs-alloc-analysis-index.md)

---

## 目录

1. [函数实现](#函数实现)
2. [所有调用者](#所有调用者)
3. [mkfs vs 挂载的行为差异](#mkfs-vs-挂载的行为差异)
4. [如果 alloc btree 已经写完会发生什么](#如果-alloc-btree-已经写完会发生什么)
5. [`freespace_initialized` 标志生命周期](#freespace_initialized-标志生命周期)
6. [潜在问题](#潜在问题)
7. [参考代码位置](#参考代码位置)

---

## 函数实现

**文件：** `fs/alloc/check.c:793-837`

```c
int bch2_dev_freespace_init(struct bch_fs *c, struct bch_dev *ca,
                            u64 bucket_start, u64 bucket_end)
{
    struct bpos end = POS(ca->dev_idx, bucket_end);
    unsigned long last_updated = jiffies;

    BUG_ON(bucket_start > bucket_end);
    BUG_ON(bucket_end > ca->mi.nbuckets);

    CLASS(btree_trans, trans)(c);
    CLASS(btree_iter, iter)(trans, BTREE_ID_alloc,
        POS(ca->dev_idx, max_t(u64, ca->mi.first_bucket, bucket_start)),
        BTREE_ITER_prefetch);

    while (bkey_lt(iter.pos, end)) {
        if (time_after(jiffies, last_updated + HZ * 10)) {
            bch_info_dev(ca, "initializing freespace: %llu/%llu",
                iter.pos.offset, ca->mi.nbuckets);
            last_updated = jiffies;
        }
        try(lockrestart_do(trans,
            dev_freespace_init_iter(trans, ca, &iter, end)));
    }

    // 设置已初始化标志
    scoped_guard(memalloc_flags, PF_MEMALLOC_NOFS) {
        guard(mutex)(&c->sb_lock);
        struct bch_member *m = bch2_members_v2_get_mut(
            c->disk_sb.sb, ca->dev_idx);
        SET_BCH_MEMBER_FREESPACE_INITIALIZED(m, true);
    }
    return 0;
}
```

### `dev_freespace_init_iter` 内循环

**文件：** `fs/alloc/check.c:755-790`

```c
static int dev_freespace_init_iter(struct btree_trans *trans,
    struct bch_dev *ca, struct btree_iter *iter, struct bpos end)
{
    struct bkey hole;
    struct bkey_s_c k = bkey_try(bch2_get_key_or_hole(iter, end, &hole));

    if (k.k->type) {
        // 真实 alloc key 存在 → 仅当 data_type==free 时加入 freespace
        struct bch_alloc_v4 a_convert;
        const struct bch_alloc_v4 *a = bch2_alloc_to_v4(k, &a_convert);
        try(bch2_bucket_do_freespace_index(trans, ca, k, a, true));
        try(bch2_trans_commit(trans, NULL, NULL, BCH_TRANS_COMMIT_no_enospc));
        bch2_btree_iter_advance(iter);           // 步进
    } else {
        // 空洞 → 插入 KEY_TYPE_set 到 freespace btree
        struct bkey_i *freespace =
            errptr_try(bch2_trans_kmalloc(trans, sizeof(*freespace)));
        bkey_init(&freespace->k);
        freespace->k.type   = KEY_TYPE_set;
        freespace->k.p      = k.k->p;      // 空洞范围
        freespace->k.size   = k.k->size;   // 空洞大小
        try(bch2_btree_insert_trans(trans,
            BTREE_ID_freespace, freespace, 0));
        try(bch2_trans_commit(trans, NULL, NULL, BCH_TRANS_COMMIT_no_enospc));
        bch2_btree_iter_set_pos(iter, k.k->p);  // 跳到空洞末尾
    }
    return 0;
}
```

`bkey_try()` 将 `BCH_ERR_transaction_restart_nested` 转换为 `bkey_s_c_err(0)`，使函数在事务重启时回退到空洞路径（然后可重试）。

---

## 所有调用者

### `bch2_dev_freespace_init` 的调用者

| # | 调用者 | 文件 | 行号 | 上下文 | 范围 |
|---|---|---|---|---|---|
| 1 | `bch2_fs_freespace_init()` | `fs/alloc/check.c` | 864 | 迭代所有设备，仅 `!freespace_initialized` | `[0, ca->mi.nbuckets)` 整设备 |
| 2 | `bch2_dev_online()` | `fs/init/dev.c` | 1313 | 设备重新上线，如果 `!freespace_initialized` | `[0, ca->mi.nbuckets)` |
| 3 | `__bch2_dev_resize_alloc()` | `fs/init/dev.c` | 1462 | 扩容，仅新加的桶范围 | `[old_nbuckets, new_nbuckets)` |

### `bch2_fs_freespace_init` 的调用者

| # | 调用者 | 文件 | 行号 | 上下文 |
|---|---|---|---|---|
| 1 | `bch2_fs_initialize()` | `fs/init/recovery.c` | 1088 | **mkfs 路径** |
| 2 | `bch2_dev_add()` | `fs/init/dev.c` | 1225 | **在线设备添加**（仅当 `BCH_FS_started` 已设置） |

---

## mkfs vs 挂载的行为差异

**在 normal mount 期间，`bch2_fs_freespace_init` 不被调用。**

**文件：** `fs/init/fs.c:1489-1494`

```c
try(BCH_SB_INITIALIZED(c->disk_sb.sb)
    ? bch2_fs_recovery(c)      // ← 正常挂载路径
    : bch2_fs_initialize(c));  // ← mkfs 路径
```

`__bch2_fs_recovery`（`recovery.c:657-996`）的代码中没有任何对 `bch2_fs_freespace_init` 的调用。唯一的调用在 `bch2_fs_initialize`（mkfs 路径，`recovery.c:1088`）。

正常挂载时，freespace btree 已经存在于磁盘上（来自上一次成功的初始化），`freespace_initialized` 标志跨挂载持续存在在超级块的 member 字段中。恢复 pass（`check_alloc_info`）验证一致性，但仅在 `freespace_initialized == true` 时运行验证。

### 各场景的 alloc btree 状态对比

| 场景 | alloc btree 状态 | freespace init 行为 | 结果 |
|---|---|---|---|
| **mkfs** | 仅 superblock/journal 有 alloc 条目 | 扫描所有 bucket | non-free 桶跳过，空洞（大多数桶）→ freespace entries |
| **正常挂载** | 完整的 alloc btree | **不调用** freespace init | freespace btree 直接使用 |
| **设备添加** | 空（新设备） | 同上 mkfs | 新设备的空闲桶全部被索引 |
| **扩容** | 新范围无 alloc 条目 | 仅初始化新范围 | 新桶全部被索引为 freespace |
| **设备重新上线（崩溃后）** | 可能部分填充 | 如果 `!freespace_initialized` | 完全初始化，幂等 |

---

## 如果 alloc btree 已经写完会发生什么

**场景 A：mkfs 路径**

alloc btree 有**只有 superblock/journal bucket 条目**（由 `bch2_trans_mark_dev_sbs` 在 `recovery.c:1068` 填充）：

- `dev_freespace_init_iter` 扫描每个 bucket
- 对 alloc 条目：`bch2_bucket_do_freespace_index` 仅在 `data_type == BCH_DATA_free` 时添加。superblock/journal bucket 有 `data_type != BCH_DATA_free`，所以**被跳过**——不会加进 freespace btree
- 对空洞（大多数桶）：插入 `KEY_TYPE_set` 到 freespace btree——这些成为空闲空间
- **结果：** 所有非 superblock/journal 的桶获得 freespace 条目。**不是空操作。**

**场景 B：正常挂载已有文件系统**

freespace **根本不重新初始化**（在 `bch2_fs_freespace_init` 层面是空操作，因为 `freespace_initialized` 为 true）。

**场景 C：初始化失败的设备重新上线**

设备添加期间崩溃后：
- 可能存在一些 alloc 条目（来自部分添加的桶）
- `freespace_initialized` 为 false（崩溃发生在 `SET_BCH_MEMBER_FREESPACE_INITIALIZED` 之前）
- `bch2_dev_freespace_init` 运行：正确处理现有 alloc 条目（仅 free 的加入 freespace），填充空洞的 freespace 条目
- **这是幂等操作**——对已有完整 alloc btree 的设备调用，只是维护不变式：free 桶有 freespace 条目，非 free 桶没有

---

## `freespace_initialized` 标志生命周期

标志存储在每个 member 的 `BCH_MEMBER_FREESPACE_INITIALIZED` 位（`fs/sb/members_format.h:107`），跨挂载持续存在。

| 事件 | 标志值 | 文件:行号 |
|---|---|---|
| mkfs 开始 | 所有设备设为 false | `recovery.c:1042` |
| mkfs 完成（`dev_freespace_init` 结束） | 每设备设为 true | `check.c:833` |
| 正常挂载 | 读取（来自超级块） | — |
| 正常挂载时 `bch2_fs_freespace_init` | 检查；如果 true，跳过 | `check.c:854` |
| `check_alloc_info` recovery pass | 检查；如果 false，跳过验证 | `check.c:164, 243` |
| `bch2_bucket_alloc_trans` | 检查；影响分配路径 | `foreground.c:626` |
| `bch2_dev_online`（重新添加） | 检查；如果 false，初始化 | `dev.c:1311` |
| `bch2_dev_add`（新设备） | 新设备从 false 开始 | 通过 sb member 默认值 |
| 扩容 | 已有设备不变 | — |

`bch2_bucket_alloc_trans` 中的路由：

```c
ob = likely(freespace)
    ? bch2_bucket_alloc_freelist(trans, req)   // 正常路径
    : bch2_bucket_alloc_early(trans, req);      // 恢复路径
```

---

## 潜在问题

`bch2_fs_freespace_init` 的注释说：

```c
/*
 * We can crash during the device add path, so we need to check this on
 * every mount:
 */
```

但在当前代码中，`bch2_fs_freespace_init` **在正常挂载期间不被调用**（`__bch2_fs_recovery` 不包含此调用）。

如果设备添加期间发生崩溃（超级块写入之后、`SET_BCH_MEMBER_FREESPACE_INITIALIZED` 之前）：

1. **超级块**已经记录了这个设备（`bch2_dev_add` 阶段 1 结束）
2. **alloc btree** 已有部分条目（来自阶段 2 的部分写入）
3. **`freespace_initialized`** 为 false（设置失败）
4. **下次挂载时：** `bch2_fs_freespace_init` 不被调用（正常挂载路径不走 mkfs）
5. **恢复路径：**
   - `check_alloc_info` pass 看到 `freespace_initialized == false` → 跳过验证
   - 分配器看到 `freespace_initialized == false` → 使用早分配路径
   - 早分配路径没有 freespace btree 可依赖，直接扫描 alloc btree

所以恢复是可行的，但通过**回退到早分配**来运作，而不是通过纠正 freespace btree。如果是多设备设备添加后崩溃，可能需要 `bch2_dev_online` 路径来最终完成初始化。

---

## 参考代码位置

| 文件 | 行号 | 函数/定义 |
|---|---|---|
| `fs/alloc/check.c` | 755-790 | `dev_freespace_init_iter` — 内循环 |
| `fs/alloc/check.c` | 793-837 | `bch2_dev_freespace_init` — 每设备初始化 |
| `fs/alloc/check.c` | 840-875 | `bch2_fs_freespace_init` — 设备迭代 + 重建入口 |
| `fs/alloc/check.c` | 164, 243 | `freespace_initialized` 为 false 时跳过验证 |
| `fs/init/recovery.c` | 657-996 | `__bch2_fs_recovery` — 正常挂载（不调用 freespace init） |
| `fs/init/recovery.c` | 1013-1090 | `bch2_fs_initialize` — mkfs 路径（调用 freespace init） |
| `fs/init/recovery.c` | 1042 | mkfs 中 `freespace_initialized` 设为 false |
| `fs/init/recovery.c` | 1068 | `bch2_trans_mark_dev_sbs` — 标记 superblock/journal 桶 |
| `fs/init/dev.c` | 1097-1225 | `bch2_dev_add` — 在线添加设备 |
| `fs/init/dev.c` | 1274-1313 | `bch2_dev_online` — 设备重新上线 |
| `fs/init/dev.c` | 1453-1462 | `__bch2_dev_resize_alloc` — 扩容 |
| `fs/init/fs.c` | 1489-1494 | mount vs mkfs 路径分支 |
| `fs/init/passes_format.h` | 81 | `check_alloc_info` recovery pass 定义 |
| `fs/alloc/foreground.c` | 620-722 | `bch2_bucket_alloc_trans` — 分配器路由 |
| `fs/alloc/background.c` | 1102-1125 | `bch2_bucket_do_freespace_index` |
| `fs/sb/members_format.h` | 107 | `BCH_MEMBER_FREESPACE_INITIALIZED` bit field |
