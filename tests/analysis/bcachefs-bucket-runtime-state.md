# 桶运行时状态初始化与管理

> 分析日期：2026-05-17  
> 隶属分析系列：[启动分配器分析索引](bcachefs-alloc-analysis-index.md)

---

## 目录

1. [概述](#概述)
2. [数据结构：`struct bucket` vs `bch_alloc_v4`](#数据结构struct-bucket-vs-bch_alloc_v4)
3. [alloc 键版本规范化和 `bch2_alloc_to_v4`](#alloc-键版本规范化和-bch2_alloc_to_v4)
4. [`alloc_data_type` 状态推导](#alloc_data_type-状态推导)
5. [桶状态转换图](#桶状态转换图)
6. [io_clock 与 io_time 系统](#io_clock-与-io_time-系统)
7. [discard 管理路径](#discard-管理路径)
8. [sector 计数体系](#sector-计数体系)
9. [运行时状态初始化：挂载流程](#运行时状态初始化挂载流程)
10. [GC 周期与 `struct bucket` 同步](#gc-周期与-struct-bucket-同步)
11. [参考代码位置](#参考代码位置)

---

## 概述

bcachefs 的桶（bucket）有两层状态表示：

1. **磁盘格式**（alloc btree）：多个版本的 alloc 键（v1/v2/v3/v4），通过 `bch2_alloc_to_v4` 统一规范化为 `struct bch_alloc_v4`
2. **运行时内存**（`struct bucket`）：精简版的每个桶状态数组，用于快速 GC 和分配决策

运行时的 `struct bucket` 数组本质上是 alloc btree 的稀疏内存缓存，仅保留分配决策所需的关键字段。

---

## 数据结构：`struct bucket` vs `bch_alloc_v4`

### 运行时结构 `struct bucket`

```c
// fs/alloc/buckets_types.h:37-45
struct bucket {
    u8      lock;           // 字节级自旋锁（bit_spin_lock）
    u8      gen_valid:1;    // gen 字段是否有效
    u8      data_type:7;    // 当前数据类型（BCH_DATA_* 枚举）
    u8      gen;            // 桶代次
    u32     dirty_sectors;  // 脏扇区计数（含 stripe_sectors）
    u32     cached_sectors; // 缓存扇区计数
    u32     stripe_sectors; // stripe 扇区计数
} __aligned(sizeof(long));
```

**大小：** 16 字节（`sizeof(long)` 对齐），对大容量设备（数亿桶）仍然紧凑。

### 磁盘格式 `struct bch_alloc_v4`

```c
// fs/alloc/format.h:82-99
struct bch_alloc_v4 {
    struct bch_val   v;
    __u64            journal_seq_nonempty;    // 非空时的 journal seq
    __u32            flags;                   // NEED_DISCARD, NEED_INC_GEN, backpointer 计数等
    __u8             gen;
    __u8             oldest_gen;
    __u8             data_type;               // 提示的数据类型
    __u8             stripe_redundancy_obsolete;
    __u32            dirty_sectors;
    __u32            cached_sectors;
    __u64            io_time[2];              // [READ, WRITE] I/O 时间戳
    __u32            stripe_refcount;         // stripe 引用计数
    __u32            nr_external_backpointers;
    __u64            journal_seq_empty;       // 变空时的 journal seq
    __u32            stripe_sectors;
    __u32            pad;
};
```

### 字段映射关系

| `struct bucket` | `struct bch_alloc_v4` | 说明 |
|---|---|---|
| `gen` | `gen` | 桶代次（1:1 映射） |
| `data_type` | `data_type` | alloc_v4 存储提示值，`alloc_data_type()` 推导 |
| `dirty_sectors` | `dirty_sectors + stripe_sectors` | bucket 存储**写直和** |
| `cached_sectors` | `cached_sectors` | 直接映射 |
| `stripe_sectors` | `stripe_sectors` | 直接映射 |
| — | `oldest_gen` | bucket 没有，存在 `bucket_gens` btree 中 |
| — | `io_time[2]` | bucket 没有——运行时不跟踪 |

**重要：** `struct bucket` 不存储 `io_time`。io_time 只在 alloc btree 中持久化。`bch2_bucket_io_time_reset`（background.c:1527-1552）直接修改 alloc btree 而不经过 `struct bucket`。

---

## alloc 键版本规范化和 `bch2_alloc_to_v4`

bcachefs 有 4 个版本的 alloc 键格式（v1/v2/v3/v4）。`bch2_alloc_to_v4` 将任意版本统一为 v4：

```c
// fs/alloc/background.c:832-865
void __bch2_alloc_to_v4(struct bkey_s_c k, struct bch_alloc_v4 *out)
{
    if (k.k->type == KEY_TYPE_alloc_v4) {
        // v4 → 复制，填充 backpointer 结构
        bkey_val_copy_pad(out, bkey_s_c_to_alloc_v4(k));
        // 移动 backpointer 到末尾，清理中间区域
        // ...
    } else {
        // v1/v2/v3 → 通过 bch2_alloc_unpack 解包
        *out = (struct bch_alloc_v4) {
            .journal_seq_nonempty   = u.journal_seq,
            .gen                    = u.gen,
            .oldest_gen             = u.oldest_gen,
            .data_type              = u.data_type,
            .dirty_sectors          = u.dirty_sectors,
            .cached_sectors         = u.cached_sectors,
            .io_time[READ]          = u.read_time,
            .io_time[WRITE]         = u.write_time,
            .stripe_refcount        = u.stripe != 0,
            // ...
        };
    }
}
```

存在两个变体系列：

| 函数 | 用途 | 调用时机 |
|---|---|---|
| `bch2_alloc_to_v4(k, &a)` | 读取转换，静态缓冲区 | 任意只读场景 |
| `bch2_alloc_to_v4_mut(trans, k)` | 写转换，分配事务内存 | 需要修改 alloc 键时 |

当前 v4 是主要格式，v1-v2-v3 在升级过程中逐步淘汰。

---

## `alloc_data_type` 状态推导

`alloc_data_type` 是核心状态推导函数，根据 sector 计数、stripe_refcount 和 gc_gen 决定桶的运行时状态：

```c
// fs/alloc/background.h:124-138
static inline enum bch_data_type alloc_data_type(struct bch_alloc_v4 a,
                                                 enum bch_data_type data_type)
{
    // (1) stripe 引用优先
    if (a.stripe_refcount)
        return data_type == BCH_DATA_parity ? data_type : BCH_DATA_stripe;

    // (2) 有脏扇区 → 数据桶
    if (bch2_bucket_sectors_dirty(a))
        return bucket_data_type(data_type);    // cached/stripe 归一化为 user

    // (3) 有缓存扇区 → cached
    if (a.cached_sectors)
        return BCH_DATA_cached;

    // (4) need_discard 标记（空→自由转换的粘性状态）
    if (data_type == BCH_DATA_need_discard)
        return BCH_DATA_need_discard;

    // (5) 空桶：free vs need_gc_gens
    return alloc_gc_gen(a) >= BUCKET_GC_GEN_MAX
        ? BCH_DATA_need_gc_gens
        : BCH_DATA_free;
}
```

**优先级：** stripe_refcount > dirty > cached > need_discard > need_gc_gens > free

---

## 桶状态转换图

```
                      分配（alloc_sectors）
   ╔══════════════════════════════════════════════════════════════╗
   ║                     非空桶                                   ║
   ║  data_type ∈ {user, btree, cached, stripe, parity}          ║
   ╚══════════════════════════════════════════════════════════════╝
                              │
                              │ 最后引用释放（extent trigger）
                              ▼
                   ┌─────────────────────┐
                   │  BCH_DATA_need_discard │
                   │  （trigger 设置，      │
                   │   journal_seq_empty  │
                   │   控制重用延迟）       │
                   └─────────────────────┘
                              │
                              │ discard_one_bucket (discard.c)
                              │
                              ▼
                   ┌─────────────────────┐
                   │    BCH_DATA_free     │
                   │  （可被 freespace     │
                   │   btree 索引）        │
                   └─────────────────────┘
                              │
                    ┌─────────┴──────────┐
                    │ alloc_gc_gen >= 96 │ 否
                    ▼                    ▼
           ┌──────────────┐    ┌──────────────┐
           │ need_gc_gens  │    │   free       │
           └──────────────┘    └──────────────┘
                    │                    │
                    │                    │ bch2_bucket_alloc_freelist
                    │                    ▼
                    │              ┌──────────────┐
                    │              │  open_bucket  │
                    │              │  data_type    │
                    │              │  由请求设置    │
                    │              └──────┬───────┘
                    │                     │ 数据写入完成
                    │                     ▼
                    │             ┌────────────────┐
                    │             │  非空桶（再次）   │
                    └─────────────┤  循环被重置      │
                                  └────────────────┘

  注：trigger 设置 NEED_DISCARD 时同时更新 journal_seq_empty。
  在 journal_seq_empty 被刷入磁盘前，该桶不会被重用。
  这就是"noflush"优化：短生命周期数据可能快速反复用同一个桶。
```

### 关键转换点

| 转换 | 触发函数 | 文件行号 | 原子性保证 |
|---|---|---|---|
| 非空 → need_discard | `bch2_trigger_alloc` | background.c:1232-1478 | 同一事务中通过 trigger 系统 |
| need_discard → free | `bch2_discard_one_bucket` | discard.c 中 | 单独事务 |
| free → 非空 | `bch2_alloc_sectors_done` | foreground.c:1666 | extent trigger 链 |
| free → need_gc_gens | `alloc_data_type`（推导） | background.h:135-136 | 自动计算，非持久状态 |
| need_gc_gens → free | `bch2_discard_one_bucket`（含 inc_gen） | discard.c | GC 维护 |
| cached → need_discard | `bch2_trigger_cached_data` | 在 trigger 系统内 | trigger 链 |

### `data_type` vs `alloc_data_type` 的关系

`bch_alloc_v4.data_type` 字段是一个**提示**（hint），并不总是反映实际状态：

- 对于非空桶：由 `alloc_data_type` 完全决定（基于 sector 计数和 stripe 引用）
- 对于空桶：`data_type == BCH_DATA_need_discard` 是**粘性标记**，在非空→空时由 trigger 设置，在 discard 处理时清除
- `alloc_data_type_set()`（background.h:140-142）会在写盘时根据「提交的数据类型 + sector 计数」计算正确的 `data_type`

---

## io_clock 与 io_time 系统

### io_clock 结构

```c
// fs/util/clock_types.h:29-36
struct io_clock {
    atomic64_t          now;        // 当前时间（扇区单位）
    u16 __percpu        *pcpu_buf;  // per-CPU 批处理缓冲区
    unsigned            max_slop;
    spinlock_t          timer_lock;
    io_timer_heap       timers;     // 最小堆定时器
};
```

每个 `struct bch_fs` 有两个 io_clock：

```c
// fs/bcachefs.h:775
struct io_clock   io_clock[2];   // [READ] 和 [WRITE]
```

**单位：** 扇区。每次 I/O 操作完成后，`__bch2_increment_clock`（util/clock.c）将操作扇区数加到时钟上。clock 的值单调递增，近似于"已处理的 I/O 总量"。

### io_time 在 alloc btree 中的用途

`struct bch_alloc_v4` 中的 `io_time[2]` 记录桶最近一次读/写操作的 io_clock 值：

```c
// fs/alloc/format.h:92
__u64    io_time[2];    // [READ], [WRITE]
```

**用途（2 个）：**

1. **LRU 淘汰索引**（`alloc_lru_idx_read`）：对于 cached 类型的桶，`io_time[READ] & LRU_TIME_MAX` 作为 LRU btree 的索引键值，用于决定哪个 cached 桶被淘汰

2. **碎片化索引**（`alloc_lru_idx_fragmentation`）：`dirty_sectors / bucket_size` 映射到 31 位范围，用于碎片化 LRU

3. **gc 判断桶活跃度**：通过 `io_time` 比较 `io_clock->now` 的差值，`copygc` 判断哪些桶值得回收

### io_time 重置

```c
// fs/alloc/background.c:1527-1552
static int __bch2_bucket_io_time_reset(struct btree_trans *trans,
    unsigned dev, size_t bucket_nr, int rw)
{
    struct bkey_i_alloc_v4 *a =
        bch2_trans_start_alloc_update_noupdate(trans, &iter, POS(dev, bucket_nr));

    u64 now = bch2_current_io_time(trans->c, rw);
    if (a->v.io_time[rw] == now)
        return 0;           // 已经是最新，跳过

    a->v.io_time[rw] = now;
    bch2_trans_update(trans, &iter, &a->k_i, 0);
    bch2_trans_commit(trans, NULL, NULL, 0);
    return 0;
}
```

此函数在以下场景被调用：
- **桶被分配用于写入时**（reset write_time）
- **桶被读缓存命中时**（reset read_time，用于 LRU 顺序）

**关键：** io_time 只存在于 alloc btree 的持久化 alloc 键中，`struct bucket` 不缓存 io_time。每次 io_time 查询都触发 alloc btree 读取。

---

## discard 管理路径

### 为什么需要 discard？

SSD 在覆盖写入前需要擦除（discard）。bcachefs 维护一个"待 discard 桶"队列，在桶变空后、重新分配前发出 discard 请求。

### discard 状态机

```
非空 → trigger_alloc 设置 need_discard
     ↓
  need_discard → discard_one_bucket 发送 discard IO
     ↓
  等待 discard IO 完成 → 清 need_discard，设 data_type=free
     ↓
  free → 可被分配器分配
```

### 快速 discard 路径

```c
// fs/alloc/foreground.h:293-299
static inline bool bch2_bucket_set_discard_fast(struct bch_fs *c,
    unsigned dev, u64 bucket)
{
    // 在分配路径中快速标记桶需要 discard
    ob->do_discards_fast = true;
}
```

`do_discards_fast` 是 `struct open_bucket` 上的布尔标记。当桶在释放后短时间内重新分配（journal 未满），可以跳过 discard 操作。这是**写入同一性（write identity）**优化的一部分。

### discard 基础设施

| 组件 | 位置 | 说明 |
|---|---|---|
| `bch2_discard_one_bucket` | `discard.c` | 每桶 discard 处理 |
| `bch2_do_discards_async` | `discard.c` | 异步调度 discard |
| `bch2_dev_discards_init` | `discard.c` | 设备 discard 初始化 |
| `discard_in_flight` 队列 | `types.h:208` | 正在 discard 的桶列表 |
| `bch2_fast_discard_bucket_add/del` | `discard.c` | 快速路径管理 |

---

## sector 计数体系

`struct bch_alloc_v4` 中有三组 sector 计数：

```c
__u32  dirty_sectors;     // 脏扇区（数据 + btree）
__u32  cached_sectors;    // 缓存扇区
__u32  stripe_sectors;    // stripe 专用扇区
```

辅助函数：

```c
// background.h:82-96
sectors_total   = dirty_sectors + cached_sectors + stripe_sectors
sectors_dirty   = dirty_sectors + stripe_sectors   // 需要 GC 关注
sectors(a)      = (data_type == cached) ? cached_sectors : sectors_dirty
```

`stripe_sectors` 从 `dirty_sectors` 分出，原因是 stripe 数据在 RAID 布局中有特殊处理需求。在 `struct bucket` 中，`dirty_sectors` 已经是 `dirty + stripe` 的和（见 `alloc_to_bucket`）。

### 碎片化计算

```c
// background.h:99-107
static inline s64 bch2_bucket_sectors_fragmented(struct bch_dev *ca,
                                                 struct bch_alloc_v4 a)
{
    int d = bch2_bucket_sectors(a);
    return d ? max(0, ca->mi.bucket_size - d)
             : !data_type_is_empty(a.data_type) ? ca->mi.bucket_size
             : 0;
}
```

碎片化扇区 = `bucket_size - 已用扇区`（如果桶非空），或者完整的 `bucket_size`（如果桶非空但 sector 计数为 0——表示元数据桶）。碎片化指标用于 copygc 选择需要搬移的桶。

---

## 运行时状态初始化：挂载流程

在挂载（`bch2_fs_recovery`）期间，桶运行时状态通过以下步骤初始化：

### 步骤 1：分配 bucket 数组

```c
// 在 bch2_dev_allocator_add 中分配
ca->buckets[0]  = kvmalloc(sizeof(struct bucket) * ca->mi.nbuckets, GFP_KERNEL);
ca->buckets_nr  = ca->mi.nbuckets;
```

数组在设备添加时分配，全零初始化。

### 步骤 2：`bch2_alloc_read` — 从 alloc btree 读取代次

```c
// fs/alloc/background.c:1031-1097
int bch2_alloc_read(struct bch_fs *c)
{
    // 遍历 alloc btree 中的所有 alloc 键
    for_each_btree_key(trans, iter, BTREE_ID_alloc, POS_MIN, ..., k, ({
        // 提取 gen
        *bucket_gen(ca, k.k->p.offset) = bch2_alloc_to_v4(k, &a)->gen;
    }));
}
```

**这里只填充 `gen` 字段！** 为什么？

- 此时文件系统可能正在 fsck 修复中，alloc 键可能不正确
- `data_type`、sector 计数等会在稍后的 `bch2_check_alloc_key`（fsck 阶段）中重新推导
- 在 `bch2_alloc_to_v4_mut` 被触发时，通过 `bch2_trigger_alloc` 更新 `struct bucket`

### 步骤 3：`bch2_dev_allocator_set_rw` — 分配器设置为读写

设备标记为 rw 时触发完整初始化：

1. 触发 discard 子系统初始化（`bch2_dev_discards_init`）
2. 启动分配器后台线程（copygc、discard 等）
3. `freespace_initialized` 确保 freespace btree 可用

### 步骤 4：惰性填充

`struct bucket` 的 `data_type`、`dirty_sectors`、`cached_sectors`、`stripe_sectors` 等字段在以下场景**按需**填充：

- GC 运行时遍历 alloc btree，为每个桶调用 `alloc_to_bucket`（buckets.h:143-150）
- `bch2_trigger_alloc` 在每次 alloc 键更新时同步 `struct bucket`
- `bch2_bucket_do_freespace_index` 使用 `bch2_alloc_to_v4` 读取最新状态

所以 `struct bucket` 在挂载初期大部分字段为 0，只在 GC 周期或 trigger 执行时才完整反映磁盘状态。

---

## GC 周期与 `struct bucket` 同步

`struct bucket` 数组是 GC 的主要工作区。每个 GC 周期：

```
bch2_gc_thread()
  └─ bch2_gc_start()
       └─ memset(ca->buckets[0], 0, ...)    ← 清除所有 runtime bucket
  └─ bch2_gc_mark_key()                        ← 再次遍历 btree，重新填充
       └─ alloc_to_bucket(&ca->buckets[dev][bucket_nr], a)
```

`alloc_to_bucket` 将 `bch_alloc_v4` 映射到 `struct bucket`：

```c
// fs/alloc/buckets.h:143-150
static inline void alloc_to_bucket(struct bucket *dst, struct bch_alloc_v4 src)
{
    dst->gen            = src.gen;
    dst->data_type      = src.data_type;
    dst->stripe_sectors = src.stripe_sectors;
    dst->dirty_sectors  = src.dirty_sectors;
    dst->cached_sectors = src.cached_sectors;
}
```

每次 GC 周期都会重新同步。非 GC 期间，`struct bucket` 通过 trigger 系统保持更新。

---

## 参考代码位置

| 文件 | 行号 | 函数/定义 |
|---|---|---|
| `fs/alloc/buckets_types.h` | 37-45 | `struct bucket` 定义 |
| `fs/alloc/format.h` | 82-99 | `struct bch_alloc_v4` 磁盘格式 |
| `fs/alloc/background.h` | 82-112 | sector 计数辅助函数 |
| `fs/alloc/background.h` | 124-138 | `alloc_data_type` 状态推导 |
| `fs/alloc/background.h` | 140-142 | `alloc_data_type_set` |
| `fs/alloc/background.h` | 147-172 | LRU 索引计算 |
| `fs/alloc/background.h` | 174-183 | freespace genbits |
| `fs/alloc/background.c` | 832-865 | `__bch2_alloc_to_v4` — 版本规范化 |
| `fs/alloc/background.c` | 898-908 | `bch2_alloc_to_v4_mut_inlined` |
| `fs/alloc/background.c` | 915-928 | `bch2_trans_start_alloc_update_noupdate` |
| `fs/alloc/background.c` | 1031-1097 | `bch2_alloc_read` — 挂载时初始化 |
| `fs/alloc/background.c` | 1102-1125 | `bch2_bucket_do_freespace_index` |
| `fs/alloc/background.c` | 1527-1552 | `__bch2_bucket_io_time_reset` |
| `fs/alloc/background.c` | 1232-1478 | `bch2_trigger_alloc` — 状态转换原子性 |
| `fs/alloc/buckets.h` | 143-150 | `alloc_to_bucket` — v4 到 bucket 映射 |
| `fs/alloc/foreground.h` | 293-299 | `bch2_bucket_set_discard_fast` |
| `fs/alloc/types.h` | 56-63 | `struct open_bucket` 定义 |
| `fs/util/clock_types.h` | 29-36 | `struct io_clock` 定义 |
| `fs/bcachefs.h` | 775 | `io_clock[2]` 声明 |
| `fs/alloc/discard.c` | — | discard 完整实现 |
