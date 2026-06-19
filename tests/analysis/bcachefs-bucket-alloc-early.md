# `bch2_bucket_alloc_early` 完整流程

> 分析日期：2026-05-17
> 隶属分析系列：[启动分配器分析索引](bcachefs-alloc-analysis-index.md)

---

## 目录

1. [概述与适用条件](#概述与适用条件)
2. [函数实现](#函数实现)
3. [`__try_alloc_bucket`：实际的桶分配](#__try_alloc_bucket实际的桶分配)
4. [关键设计：alloc btree 在分配时不被修改](#关键设计alloc-btree-在分配时不被修改)
5. [与正常路径的关系](#与正常路径的关系)
6. [完整调用链](#完整调用链)
7. [参考代码位置](#参考代码位置)

---

## 概述与适用条件

**文件：** `fs/alloc/foreground.c:347-435`

`bch2_bucket_alloc_early` 是**早分配路径**，仅在 freespace btree 尚未初始化时使用。代码注释明确：

```c
// 此路径用于初始化空闲空间 btree 之前:
// This path is for before the freespace btree is initialized:
```

适用场景：
- **mkfs 期间**：freespace btree 尚未构建，`freespace_initialized == false`
- **设备添加过程中**（可能崩溃后重新添加）：freespace 未完全初始化
- **恢复期间**：`check_alloc_info` pass 尚未完成

正常运行时（`freespace_initialized == true`），分配走 `bch2_bucket_alloc_freelist`，不要走此路径。

---

## 函数实现

```c
static noinline struct open_bucket *
bch2_bucket_alloc_early(struct btree_trans *trans,
                        struct alloc_request *req)
{
    struct bch_fs *c = trans->c;
    struct bch_dev *ca = req->ca;
    struct bkey_s_c k;
    struct open_bucket *ob = NULL;
    u64 first_bucket = ca->mi.first_bucket;
    u64 *dev_alloc_cursor = &ca->alloc_cursor[req->btree_bitmap];
    u64 alloc_start = max(first_bucket, *dev_alloc_cursor);
    u64 alloc_cursor = alloc_start;
    int ret;
```

### 遍历路径

```c
    // ① 从 alloc_cursor 开始，逐个槽位遍历 alloc btree
    for_each_btree_key_norestart(trans, iter, BTREE_ID_alloc,
        POS(ca->dev_idx, alloc_cursor),
        BTREE_ITER_slots, k, ret)
```

| 参数 | 值 | 说明 |
|---|---|---|
| Btree | `BTREE_ID_alloc` | 直接扫描 alloc btree |
| Flags | `BTREE_ITER_slots` | 逐个 bucket 精确位置遍历 |
| 起始位置 | `POS(ca->dev_idx, alloc_cursor)` | 从游标位置开始 |
| 缓存 | 非缓存 (uncached) | 避免污染 key cache（见第 367-374 行注释） |

### 核心检查链

```c
    // ② 跳过超尾范围：
    if (bkey_ge(iter.pos, POS(ca->dev_idx, ca->mi.nbuckets)))
        break;

    // ③ 跳过不在 btree_bitmap 范围内的 bucket：
    if (req->btree_bitmap != BTREE_BITMAP_YES &&
        alloc_key_not_in_btree_bitmap(ca, k.k->p.offset, req))
        continue;

    // ④ 读取 alloc 键 → alloc_v4，检查 data_type：
    struct bch_alloc_v4 a_convert;
    const struct bch_alloc_v4 *a = bch2_alloc_to_v4(k, &a_convert);
    if (a->data_type != BCH_DATA_free)
        continue;           // ← 非空闲 bucket 直接跳过

    // ⑤ 通过 cached 迭代器二次检查（序列化）：
    CLASS(btree_iter, citer)(trans, BTREE_ID_alloc, k.k->p,
        BTREE_ITER_cached | BTREE_ITER_intent | BTREE_ITER_nopreserve);

    a = bch2_alloc_to_v4(ck, &a_convert);
    if (a->data_type == BCH_DATA_free) {
        // ⑥ 尝试分配：
        ob = may_alloc_bucket(c, req, k.k->p) &&         // open / nocow 检查
             may_alloc_bucket_journal_seq(c, req,         // journal 冲刷检查
                 a->journal_seq_empty)
            ? __try_alloc_bucket(c, req, k.k->p.offset, a->gen)
            : NULL;
        if (ob) break;
    }
```

### 游标回绕

```c
    // 如果遍历到设备末尾仍未找到：
    if (unlikely(!ob)) {
        if (alloc_cursor > first_bucket) {
            // 从 first_bucket 重新开始
            alloc_cursor = first_bucket;
            goto again;
        }
        // 整个设备都扫描了，确实没有空闲 bucket
    }

    // 更新游标
    *dev_alloc_cursor = alloc_cursor;
    return ob;
}
```

---

## `__try_alloc_bucket`：实际的桶分配

**文件：** `fs/alloc/foreground.c:269-321`

这是 buckets 实际被"获取"的地方：

```c
static struct open_bucket *__try_alloc_bucket(struct bch_fs *c,
                          struct alloc_request *req,
                          u64 bucket, u8 gen)
{
    struct bch_dev *ca = req->ca;

    // ① 守卫检查
    if (unlikely(is_superblock_bucket(c, ca, bucket)))  return NULL;
    if (unlikely(bch2_bucket_nouse(ca, bucket)))        return NULL;

    // ② freelist_lock：检查 open bucket reserve
    guard(spinlock)(&c->allocator.freelist_lock);
    if (c->allocator.open_buckets_nr_free <=
        bch2_open_buckets_reserved(req->watermark))
        return ERR_PTR(...);

    // ③ 二次检查：bucket 尚未打开（在锁下）
    if (bch2_bucket_is_open(c, ca->dev_idx, bucket))  return NULL;

    // ④ 从 freelist 分配 open_bucket 结构体
    struct open_bucket *ob = bch2_open_bucket_alloc(&c->allocator);

    // ⑤ 初始化 open_bucket
    guard(spinlock)(&ob->lock);
    ob->valid        = true;
    ob->sectors_free = ca->mi.bucket_size;   // 初始化为完整空桶
    ob->dev          = ca->dev_idx;
    ob->gen          = gen;                  // 来自 alloc 键
    ob->bucket       = bucket;

    // ⑥ 跟踪
    ca->nr_open_buckets++;
    bch2_open_bucket_hash_add(c, ob);
    return ob;
}
```

### open_bucket 结构体

分配后的状态：

| 字段 | 值 | 说明 |
|---|---|---|
| `valid` | `true` | 在写完成后被置为 false |
| `sectors_free` | `ca->mi.bucket_size` | 初始化为完整桶大小 |
| `dev` | `ca->dev_idx` | 设备索引 |
| `gen` | 来自 alloc 键 | 桶代龄，防止 stale 指针重用 |
| `bucket` | bucket 编号 | 物理桶编号 |
| `data_type` | 见下文 | 在 `bch2_bucket_alloc_trans` 中设置 |

在 `bch2_bucket_alloc_trans` 第 703-704 行中：
```c
if (!ret) {
    ob->data_type = req->data_type;    // 设置目标数据类型
```

---

## 关键设计：alloc btree 在分配时不被修改

**这是最核心的设计点：`bch2_bucket_alloc_early` 只是读取 alloc btree，创建 open_bucket。实际的 alloc 键更新发生在数据写入时。** 

```
bch2_bucket_alloc_early / bch2_bucket_alloc_freelist
  │
  │  只读 alloc 键，检查 data_type == BCH_DATA_free
  │  创建 open_bucket（sectors_free=桶大小）
  │
  ▼
bch2_alloc_sectors_req / bch2_alloc_sectors_append_ptrs_inlined
  │
  │  将 open_bucket 附加到 write_point
  │  数据开始写入
  │
  ▼
bch2_trigger_pointer → __mark_pointer
  │
  │  更新 dirty_sectors / cached_sectors
  │
  ▼
bch2_trigger_alloc
  │
  │  io_time[READ/WRITE] = bch2_current_io_time(c, rw)
  │  data_type 根据扇区计数派生
  │  freespace btree 更新（在 trigger 事务阶段）
```

这意味着：
- **分配时没有 alloc btree 写入**——`open_bucket` 生命周期附着于 write_point
- **数据写入时 alloc 键才更新**——通过 extent trigger 链
- **如果数据从未写入**（写过程中崩溃），`open_bucket` 在下次挂载时丢失，桶仍标记为 `BCH_DATA_free`

---

## 与正常路径的关系

### 统一入口：`bch2_bucket_alloc_trans`

**文件：** `fs/alloc/foreground.c:620-722`

```c
struct open_bucket *bch2_bucket_alloc_trans(struct btree_trans *trans,
                                            struct alloc_request *req)
{
    bool freespace = READ_ONCE(ca->mi.freespace_initialized);
    // ...
alloc:
    ob = likely(freespace)
        ? bch2_bucket_alloc_freelist(trans, req)   // 正常路径
        : bch2_bucket_alloc_early(trans, req);      // 恢复路径
    // ...
}
```

### 早分配 vs 正常分配对比

| 特性 | 早分配路径 | 正常路径 |
|---|---|---|
| 条件 | `freespace_initialized == false` | `freespace_initialized == true` |
| Btree | `BTREE_ID_alloc` | `BTREE_ID_freespace` |
| 遍历方式 | `BTREE_ITER_slots` 逐个 bucket | extent 迭代 |
| 序列化 | 二次 cached 查找 | `bch2_check_freespace_key_async`（交叉验证） |
| 复杂度 | O(N) N=bucket 总数 | O(M) M=空闲 extent 数 |
| 回退 | — | 如果 freespace 发现不一致，降级到早分配 |

### 正常路径的分配检查

**文件：** `fs/alloc/foreground.c:437-507`

```c
static noinline struct open_bucket *
bch2_bucket_alloc_freelist(struct btree_trans *trans,
                           struct alloc_request *req)
{
    // 遍历 freespace btree：
    for_each_btree_key_norestart(trans, iter, BTREE_ID_freespace, ...) {
        // 交叉验证：读取实际 alloc 键确认空闲
        ret = bch2_check_freespace_key_async(c,  &trans->fs_usage_works,
                       k, &update_unavailable_before);
        if (!ret)
            ob = try_alloc_bucket(trans, req, ...);
    }
}
```

---

## 完整调用链

```
顶层入口：
bch2_alloc_sectors_start_trans()        [foreground.h:359]
  └─ bch2_alloc_sectors_req()           [foreground.c:1468-1653]
       ├─ writepoint_find()             // 查找/创建 write_point
       ├─ bucket_alloc_set_writepoint() // 复用已有 open_bucket
       ├─ bucket_alloc_set_partial()    // 复用部分填充的 bucket
       ├─ bucket_alloc_from_stripe()    // 尝试 EC stripe
       └─ bch2_bucket_alloc_set_trans() // 分配新 bucket
            └─ bch2_dev_alloc_list()    // 按 stripe clock 排序设备
            └─ bch2_bucket_alloc_trans()
                 ├─ bch2_bucket_alloc_early()    // !freespace_initialized
                 │    ├─ for_each_btree_key_norestart(BTREE_ID_alloc, BTREE_ITER_slots)
                 │    ├─ may_alloc_bucket()      // open / nocow 检查
                 │    ├─ may_alloc_bucket_journal_seq()  // journal 冲刷检查
                 │    └─ __try_alloc_bucket()
                 │         ├─ bch2_open_bucket_alloc()
                 │         └─ bch2_open_bucket_hash_add()
                 │
                 └─ bch2_bucket_alloc_freelist() // freespace_initialized
                      └─ try_alloc_bucket()
                           ├─ bch2_check_freespace_key_async()
                           └─ __try_alloc_bucket()
```

---

## 参考代码位置

| 文件 | 行号 | 函数/定义 |
|---|---|---|
| `fs/alloc/foreground.c` | 269-321 | `__try_alloc_bucket` — 创建 open_bucket |
| `fs/alloc/foreground.c` | 323-345 | `try_alloc_bucket` — 分配检查（正常路径） |
| `fs/alloc/foreground.c` | 347-435 | `bch2_bucket_alloc_early` — 早分配路径 |
| `fs/alloc/foreground.c` | 437-507 | `bch2_bucket_alloc_freelist` — 正常分配路径 |
| `fs/alloc/foreground.c` | 620-722 | `bch2_bucket_alloc_trans` — 统一入口 |
| `fs/alloc/foreground.c` | 1468-1653 | `bch2_alloc_sectors_req` — 分配器顶层 |
| `fs/alloc/foreground.h` | 359-379 | `bch2_alloc_sectors_start_trans` — 入口 inline |
