# journal bucket 分配生命周期

> 分析日期：2026-05-17
> 隶属分析系列：[启动分配器分析索引](bcachefs-alloc-analysis-index.md)

---

## 目录

1. [关键结论](#关键结论)
2. [mkfs 路径：初始分配](#mkfs-路径初始分配)
3. [挂载路径：从超级块加载](#挂载路径从超级块加载)
4. [`bch2_fs_journal_start` 的作用](#bch2_fs_journal_start-的作用)
5. [运行时扩容路径](#运行时扩容路径)
6. [分配器路由选择](#分配器路由选择)
7. [剩余疑问](#剩余疑问)
8. [参考代码位置](#参考代码位置)

---

## 关键结论

**journal bucket 不是在挂载时分配的。** 它们在 mkfs 时预先分配并持久化存储在超级块中。挂载时从磁盘加载预先分配的列表。**没有特殊的"journal 预留区域"**——journal bucket 只是通过 `bch2_trans_mark_metadata_bucket()` 标记为 `BCH_DATA_journal` 的普通桶。

- **没有 `bch2_journal_buckets_reserve` 函数**——正确的函数名是 `bch2_set_nr_journal_buckets()`（运行时）和 `bch2_dev_journal_alloc()`（初始分配）
- 分配路径根据 `ca->mi.freespace_initialized` 动态选择（早分配 vs 正常路径）

---

## mkfs 路径：初始分配

```
bch2_fs_initialize()                    [fs/init/recovery.c:1013]
  └─ bch2_fs_journal_alloc()            [fs/journal/init.c:304-318]
       │  对每个在线设备：
       └─ bch2_dev_journal_alloc(ca, true)  [fs/journal/init.c:258-301]
```

### 数量计算

**文件：** `fs/journal/init.c:258-301`

```c
int bch2_dev_journal_alloc(struct bch_dev *ca, bool new_fs)
{
    // 设备桶数的 1/128：
    unsigned nr = ca->mi.nbuckets >> 7;

    // 范围限制：
    nr = clamp_t(unsigned, nr,
        BCH_JOURNAL_BUCKETS_MIN,       // 最小（例如 4）
        min(1 << 13,                   // 最大 8192
            (1 << 24) / ca->mi.bucket_size));  // 或最大 8GB

    ret = bch2_set_nr_journal_buckets_loop(c, ca, nr, new_fs);
}
```

### 实际分配

**文件：** `fs/journal/init.c:19-141`

```c
static int bch2_set_nr_journal_buckets_iter(struct bch_dev *ca,
    unsigned nr, bool new_fs, enum bch_watermark watermark,
    struct closure *cl)
{
    for (nr_got = 0; nr_got < nr_want; nr_got++) {
        // 使用通用桶分配器分配一个桶：
        ob[nr_got] = bch2_bucket_alloc_trans(trans, req);

        // 标记为 BCH_DATA_journal：
        bch2_trans_mark_metadata_bucket(trans, ca,
            ob[nr_got]->bucket, BCH_DATA_journal, ...);
    }

    // 保存到超级块：
    bch2_journal_buckets_to_sb(c, ca, new_buckets, nr);
    bch2_write_super(c);
}
```

分配时 `freespace_initialized == false`（mkfs 尚未初始化 freespace 系统），所以 `bch2_bucket_alloc_trans` 回退到早分配路径 `bch2_bucket_alloc_early`：扫描 alloc btree 查找空闲 bucket。

分配请求的配置：

```c
struct alloc_request *req = alloc_request_get(trans,
    0, false, NULL, 1, 0,
    watermark,                   // mkfs 时 = BCH_WATERMARK_btree
    0, !nr_got ? cl : 0);
```

---

## 挂载路径：从超级块加载

```
bch2_fs_start()                        [fs/init/fs.c:1515]
  └─ __bch2_fs_start()                 [fs/init/fs.c:1446]
       │
       ├─ bch2_dev_allocator_add(ca)    // 设备上线
       │    └─ __bch2_dev_attach_bdev() [fs/init/dev.c:704]
       │         └─ bch2_dev_journal_init(ca, sb) [fs/journal/init.c:529]
       │              读取超级块的 BCH_SB_FIELD_journal / journal_v2
       │              填充 ca->journal.buckets[]
       │
       ├─ bch2_fs_init_rw()            [fs/init/fs.c:845]
       │    ├─ bch2_fs_journal_init()  [fs/journal/init.c:632]
       │    │   分配内存中缓冲区：
       │    │   - j->free_buf（释放 fifo）
       │    │   - j->in_flight（进行中 fifo）
       │    │   - j->wq（工作队列）
       │    └─ bch2_journal_reclaim_start() // 启动回收线程
       │
       ├─ bch2_fs_recovery()           [fs/init/recovery.c:998]
       │    └─ __bch2_fs_recovery()    [fs/init/recovery.c:657]
       │         ├─ bch2_journal_read()      // 读取日志条目
       │         ├─ bch2_fs_journal_start()  // 设置序列号/重放状态
       │         │    → ← 不分配桶！
       │         ├─ read_btree_roots()
       │         └─ bch2_run_recovery_passes_startup()
       │
       └─ bch2_fs_read_write()         [fs/init/fs.c:624]
            └─ 设置 JOURNAL_running 标志
```

### `bch2_dev_journal_init`：从超级块加载

**文件：** `fs/journal/init.c:529-582`

```c
int bch2_dev_journal_init(struct bch_dev *ca, struct bch_sb *sb)
{
    struct bch_sb_field_journal *journal_buckets =
        bch2_sb_field_get(sb, journal);
    struct bch_sb_field_journal_v2 *journal_buckets_v2 =
        bch2_sb_field_get(sb, journal_v2);

    // 解析 v2（压缩范围）或 v1（平坦数组）：
    if (journal_buckets_v2) {
        // 解压范围到 ja->buckets[]
    } else if (journal_buckets) {
        // 直接复制到 ja->buckets[]
    }

    ja->buckets = kcalloc(ja->nr, sizeof(u64), GFP_KERNEL);
}
```

---

## `bch2_fs_journal_start` 的作用

**文件：** `fs/journal/init.c:373-493`

此函数仅设置**内存中的日志状态**以进行重放。**不分配桶。**

```c
int bch2_fs_journal_start(struct journal *j, struct journal_start_info info)
{
    // ① 从 info 设置序列号：
    u64 cur_seq = info.cur_seq;
    u64 last_seq = info.last_seq ?: info.cur_seq;

    // ② 分配 pin fifo（跟踪未刷新的 journal 条目）：
    init_fifo(&j->pin, roundup_pow_of_two(nr), GFP_KERNEL);

    // ③ 设置序列号状态：
    j->replay_journal_seq  = last_seq;
    j->last_seq_ondisk     = last_seq;
    j->seq_ondisk          = cur_seq - 1;
    atomic64_set(&j->seq, cur_seq - 1);

    // ④ 初始化 in_flight fifo：
    init_fifo(&j->in_flight, roundup_pow_of_two(IN_FLIGHT_NR), GFP_KERNEL);
    // ...
}
```

实际 bucket 列表在设备初始化期间由 `bch2_dev_journal_init` 从超级块加载，与 `bch2_fs_journal_start` 无关。

---

## 运行时扩容路径

通过用户空间 ioctl 触发（或内部自动扩容）：

```
bch2_set_nr_journal_buckets(c, ca, new_nr)
  └─ bch2_set_nr_journal_buckets_loop(c, ca, nr, false)
       └─ bch2_set_nr_journal_buckets_iter(c, ca, nr,
                  false, BCH_WATERMARK_normal, &cl)
            └─ bch2_bucket_alloc_trans(trans, req)
                 // 此时 freespace_initialized == true
                 // → 使用 bch2_bucket_alloc_freelist（从 freespace btree）
```

与 mkfs 路径的关键区别：
- `new_fs = false` → 使用 `BCH_WATERMARK_normal` 水印（不是 `BCH_WATERMARK_btree`）
- `freespace_initialized == true` → 使用正常分配路径

---

## 分配器路由选择

**文件：** `fs/alloc/foreground.c:620-722`

```c
struct open_bucket *bch2_bucket_alloc_trans(struct btree_trans *trans,
                                            struct alloc_request *req)
{
    bool freespace = READ_ONCE(ca->mi.freespace_initialized);
    // ...水印和可用性检查...

alloc:
    ob = likely(freespace)
        ? bch2_bucket_alloc_freelist(trans, req)   // 正常：从 freespace btree
        : bch2_bucket_alloc_early(trans, req);      // 恢复：扫描 alloc btree

    // 回退：如果 freespace 尝试失败且仍在恢复早期
    if (!ob && freespace &&
        c->recovery.pass_done < BCH_RECOVERY_PASS_check_alloc_info) {
        freespace = false;
        goto alloc;
    }
}
```

| 场景 | `freespace_initialized` | 分配的路径 |
|---|---|---|
| mkfs | `false` | `bch2_bucket_alloc_early` |
| 正常挂载后运行 | `true` | `bch2_bucket_alloc_freelist` |
| 恢复早期（check_alloc_info 之前） | `true` 但可能不一致 | 尝试 freelist，回退到 early |
| 运行时扩容 | `true` | `bch2_bucket_alloc_freelist` |

---

## 剩余疑问

1. **正常运行时的 journal bucket 回收/重用机制**：当 journal 使用一个 bucket 后，如何将其归还给 freespace？reclaim 线程在什么条件下释放 journal bucket？

2. **`bch2_set_nr_journal_buckets` 的触发条件**：什么情况下用户空间会调用扩容？是手动还是自动监控触发的？

3. **设备添加时 journal 桶的同步**：如果在线添加一个设备，`bch2_dev_journal_alloc` 是否也被调用？新设备是否不需要 journal bucket（因为仅用于数据）？

---

## 参考代码位置

| 文件 | 行号 | 函数/定义 |
|---|---|---|
| `fs/journal/init.c` | 19-141 | `bch2_set_nr_journal_buckets_iter` — 实际分配 |
| `fs/journal/init.c` | 258-301 | `bch2_dev_journal_alloc` — 初始分配数量计算 |
| `fs/journal/init.c` | 304-318 | `bch2_fs_journal_alloc` — 所有设备分配 |
| `fs/journal/init.c` | 373-493 | `bch2_fs_journal_start` — 设置重放状态（不分配桶） |
| `fs/journal/init.c` | 529-582 | `bch2_dev_journal_init` — 从超级块加载桶列表 |
| `fs/journal/init.c` | 632+ | `bch2_fs_journal_init` — 分配内存缓冲区 |
| `fs/init/fs.c` | 845+ | `bch2_fs_init_rw` — mount 路径中的 journal 初始化 |
| `fs/init/recovery.c` | 657-996 | `__bch2_fs_recovery` — 正常挂载（不调用 freespace init） |
| `fs/init/recovery.c` | 1013-1090 | `bch2_fs_initialize` — mkfs 路径 |
| `fs/init/dev.c` | 704 | `__bch2_dev_attach_bdev` — 设备上线时初始化 journal |
| `fs/alloc/foreground.c` | 620-722 | `bch2_bucket_alloc_trans` — 分配器路由选择 |
