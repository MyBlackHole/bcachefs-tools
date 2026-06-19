# bcachefs Journal 子系统分析

> 分析日期：2026-05-17
> 隶属分析系列：[事务实现分析](bcachefs-transaction-implementation.md)
> 前置知识：事务提交流程、btree 节点结构

---

## 目录

1. [概述](#1-概述)
2. [On-disk 格式](#2-on-disk-格式)
3. [核心数据结构](#3-核心数据结构)
4. [Journal 条目生命周期](#4-journal-条目生命周期)
5. [Journal 写入路径](#5-journal-写入路径)
6. [Journal 钉选与回收](#6-journal-钉选与回收)
7. [Journal 读取与恢复](#7-journal-读取与恢复)
8. [Journal 重放](#8-journal-重放)
9. [序列号黑名单](#9-序列号黑名单)
10. [初始化与关闭](#10-初始化与关闭)
11. [参考代码位置](#11-参考代码位置)

---

## 1. 概述

Journal（日志）是 bcachefs 崩溃一致性的核心机制。所有 btree 修改必须在写入 btree 节点之前先写入 journal，确保系统崩溃后可以通过 journal replay 恢复一致状态。

**Journal 的四个职责：**

| 职责 | 说明 |
|------|------|
| **崩溃恢复** | 未写入 btree 节点的事务通过 journal replay 重放 |
| **写入排序** | 保证 btree 节点写回和 journal 条目间的顺序关系 |
| **批量提交** | 多个事务的 btree 更新合并到一个 journal 条目中 |
| **空间回收** | 已刷入 btree 节点的 journal 条目可以被回收 |

**核心设计原则：**

```
Journal 是 btree 写入的前置写入日志：
  事务提交 → journal 条目（必须先持久化）
              ↓
          btree 节点写盘（可延迟）
              ↓
          journal 条目可回收（journal reclaim）
```

### 1.1 与事务提交的关系

```
bch2_trans_commit()
  │
  ├─ Phase 1: run triggers      // alloc/backpointers trigger
  │
  ├─ Phase 2: write_locked
  │    ├─ bch2_journal_add_entry()    // 写入 journal
  │    └─ bch2_btree_insert_key_leaf()// 插入 btree 节点内存
  │
  └─ Phase 3: write_unlocked
       └─ btree node 异步写盘
            └─ bch2_journal_pin_drop() // 释放 journal pin
```

关键顺序：**journal 必须在 btree 节点写盘之前持久化**，否则 crash 后 journal 丢失而 btree 节点已更新，造成不一致。

### 1.2 术语表

| 术语 | 说明 |
|------|------|
| **seq** | 序列号，单调递增 64 位整数，永不重用 |
| **journal entry** | 一个 journal 条目，包含 header + 多个 jset_entry |
| **jset_entry** | 带类型的子条目（btree_keys、btree_root、blacklist 等） |
| **journal bucket** | 每个设备上分配给 journal 的桶，环形使用 |
| **pin** | 钉选，btree 节点对 journal 条目的引用计数 |
| **flush entry** | 带 FUA/PREFLUSH 的 journal 写入（保证完整性） |
| **noflush entry** | 无 FUA 的 journal 写入（更快但可能丢失） |
| **replay** | 重放，崩溃后重新执行 journal 中的 btree 更新 |
| **blacklist** | 黑名单，标记哪些 seq 不可用 |

---

## 2. On-disk 格式

### 2.1 Journal 条目格式：`struct jset`

定义在 `fs/bcachefs_format.h:1801`

```
 offset │ 字段           │ 大小    │ 说明
────────┼─────────────────┼─────────┼──────────────────────
   0    │  csum           │  8×4    │ 整个条目的校验和
   16   │  magic          │  8      │ 魔数（用于识别）
   24   │  seq            │  8      │ 单调递增序列号
   32   │  version        │  4      │ 格式版本
   36   │  flags          │  4      │ 位域：

         │                │         │   JSET_CSUM_TYPE     [0:4]
         │                │         │   JSET_BIG_ENDIAN    [4:5]
         │                │         │   JSET_NO_FLUSH      [5:6]
         │                │         │   JSET_HAS_OVERWRITES[6:7]
         │                │         │
   40   │  u64s           │  4      │ 数据部分大小（u64 为单位）
   44   │  encrypted_start │  0      │ 加密数据起始标记
   48   │  _read_clock     │  2      │ 已废弃
   50   │  _write_clock    │  2      │ 写入时钟值（超块同步）
   52   │  last_seq        │  8      │ 最旧脏 journal 条目的 seq
    ────┼─────────────────┼─────────┤
   60   │  start[]         │  可变   │ jset_entry 数组
        │  _data[]         │  可变   │ 原始键数据存储
```

**关键字段解读：**

- **seq**：条目唯一标识，单调递增。Journal 条目从不重用 seq。
- **last_seq**：崩溃恢复的重放起点。标志着最早的、仍被 btree 节点引用的 journal 条目。
- **JSET_NO_FLUSH**：无 FUA 的无刷新写入标志。这种条目在崩溃后可能丢失。
- **JSET_HAS_OVERWRITES**：条目包含覆盖条目（用于 journal_rewind 功能）。

### 2.2 jset_entry 类型

定义在 `fs/bcachefs_format.h:1621`，每个条目由 `type` 字段区分：

| 类型值 | 名称 | 用途 |
|-------|------|------|
| 0 | `btree_keys` | Btree 更新（最常见的条目，包含完整 bkey 数据） |
| 1 | `btree_root` | Btree 根节点指针，每次 journal 写入时记录 |
| 3 | `blacklist` | 黑名单单个序列号 |
| 4 | `blacklist_v2` | 黑名单一个序列号范围 |
| 5 | `usage` | 最大 key_version（用于加密 nonce 派生） |
| 7 | `clock` | IO 时钟（自文件系统创建以来的总读写扇区数） |
| 10 | `overwrite` | 被覆盖的旧值（用于 journal_rewind 调试） |
| 11 | `write_buffer_keys` | 写缓冲区键，写入磁盘前会被转为 btree_keys |
| 12 | `datetime` | Journal 写入时的挂钟时间 |
| 13 | `log_bkey` | 包含 btree key 的结构化日志条目 |
| 14 | `rewind_limit` | 可安全回退的最旧 journal seq |
| 15 | `rewind` | 回退进行中标记 |

**`jset_entry` 头部格式（`struct jset_entry`，`fs/bcachefs_format.h:918`）：**

```
 offset │ 字段     │ 大小  │ 说明
────────┼──────────┼───────┼────────────────
   0    │  u64s    │  2    │ 条目大小（含头部，u64 为单位）
   2    │  btree_id│  1    │ 所属 btree ID
   3    │  level   │  1    │ Btree 层级
   4    │  type    │  1    │ 条目类型（见上表）
   5    │  pad[3]  │  3    │ 对齐填充
   8    │  start[] │  可变 │ Bkey 数组起始
```

### 2.3 Journal 桶布局

每个设备上的 journal 桶构成一个环形缓冲区：

```
设备设备 journal 区域：

  bucket[0]  bucket[1]  ...  bucket[nr-1]
  ┌─────────┬─────────┬──────┬──────────┐
  │ entry a │ entry b │ .... │ entry z  │
  │ seq=100 │ seq=105 │      │ seq=101  │
  └─────────┴─────────┴──────┴──────────┘
       ↑                          ↑
   cur_idx                  discard_idx
       (当前写入位置)         (可丢弃边界)
```

索引不变量（每设备）：`discard_idx ≤ dirty_idx_ondisk ≤ dirty_idx ≤ cur_idx`

| 索引 | 含义 |
|------|------|
| `cur_idx` | 当前正在写入的桶 |
| `dirty_idx` | 第一个脏桶（含未写入磁盘数据的桶） |
| `dirty_idx_ondisk` | 第一个已记录到磁盘的脏桶 |
| `discard_idx` | 下一个可丢弃的桶 |

`bucket_seq[]` 数组记录每个桶中包含的最大 seq，用于判断桶是否可以回收。

---

## 3. 核心数据结构

### 3.1 `struct journal`（嵌入 `struct bch_fs`）

定义在 `fs/journal/types.h:219`。这是整个 journal 子系统的中枢。

**快速路径组（cacheline 对齐）：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `reservations` | `union journal_res_state` | 原子状态字，用于无锁预留 |
| `watermark` | `enum bch_watermark` | 当前水位（控制准入） |

**环形缓冲（ring buffer）：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `ring[4]` | `struct journal_ringbuf` | 预留快速路径中继，按 `seq & 3` 索引 |
| `in_flight` | `FIFO(struct journal_buf)` | 动态 FIFO，范围 `(seq_ondisk, cur_seq]` |
| `free_buf` | `void *` | 预分配的下一个条目缓冲区 |

**序列号：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `seq` | `atomic64_t` | 最新分配序列号（`journal_entry_open` 中原子递增） |
| `seq_write_started` | `u64` | 已启动写入的最新 seq |
| `seq_ondisk` | `u64` | 已持久化到磁盘的最新 seq |
| `flushed_seq_ondisk` | `u64` | 已通过 flush 写入的最新 seq（`fsync` 保证边界） |
| `last_seq` | `u64` | 最旧脏条目 seq（btree 节点仍有未刷新的引用） |
| `last_seq_ondisk` | `u64` | 最后写入磁盘的 `last_seq` |
| `err_seq` | `u64` | 写入失败的 seq（0 = 无错误） |

**Pin FIFO & 空间：**

| 字段 | 类型 | 说明 |
|------|------|------|
| `pin` | `FIFO(struct journal_entry_pin_list)` | 每 seq 引用计数 FIFO |
| `dirty_entry_bytes` | `size_t` | 所有脏条目总字节数 |
| `space[4]` | `struct journal_space` | 四个空间水位类别的可用空间 |

**完整字段表见 [types.h](../../fs/journal/types.h)。**

### 3.2 `union journal_res_state`（原子状态机）

定义在 `fs/journal/types.h:135`。打包在单个 `u64` 中：

```
 63          42 41        22 21         0
 ├─────bufN_counts─────┤── idx ─┤─ offset ─┤
 │  4 × 10 位引用计数  │  2 位  │  22 位   │
                         ↑ ring 缓冲区索引
```

- **`cur_entry_offset`**（22 位）：当前条目已用 u64 数。特殊哨兵值：
  - `JOURNAL_ENTRY_CLOSED_VAL`（`OFFSET_MAX - 2`）→ 条目已关闭
  - `JOURNAL_ENTRY_BLOCKED_VAL`（`OFFSET_MAX - 1`）→ 写入缓冲区 flush 阻塞中
  - `JOURNAL_ENTRY_ERROR_VAL`（`OFFSET_MAX`）→ Journal 错误停机
- **`idx`**（2 位）：当前活跃的 ring 缓冲区索引（0..3）。
- **`bufN_count`**（4 × 10 位）：每个 ring 槽位的并发预留引用计数（最大 1023）。

这个原子字是整个 journal 的热路径核心——预留和提交都是通过 `cmpxchg` 修改这个字完成。

### 3.3 `struct journal_buf`（每条目暂存区）

定义在 `fs/journal/types.h:37`。每个正在写入的 journal 条目对应一个 `journal_buf`。

| 字段 | 类型 | 说明 |
|------|------|------|
| `data` | `struct jset *` | 条目数据指针（kvmalloc 分配） |
| `key` | extent key | 设备上 journal 写入的位置（bkey + 设备指针） |
| `cas[]` | `struct bch_dev *` | 设备指针（分配 → 提交间隙的防设备移除安全机制） |
| `devs_written` | `struct bch_devs_list` | 实际写入的设备集 |
| `wait` | `struct closure_waitlist` | 等待此条目写入完成的闭环等待队列 |
| `last_seq` | `u64` | 条目关闭时的 `data->last_seq` |
| `expires` | `long` | 自动关闭此条目的 jiffies 时间戳 |
| `must_flush` | `bool` | 是否需要 FUA 写入 |
| `write_started/allocated/done` | `bool` | 写入阶段标记 |
| `noflush` | `bool` | 标记为无刷新 |

`journal_buf` 的生命周期：
```
创建 → journal_entry_open() 中 fifo_push_ref
活跃 → 接收预留和数据
关闭 → __journal_entry_close() 设置最终 u64s/last_seq
写入 → bch2_journal_write() 提交 BIO
回收 → journal_write_done() 中 data 放回 free_buf
```

### 3.4 `struct journal_device`（每设备）

定义在 `fs/journal/types.h:391`。

| 字段 | 类型 | 说明 |
|------|------|------|
| `bucket_seq` | `u64 *` | 每个桶中包含的最大 seq |
| `sectors_free` | `unsigned` | 此设备上 journal 可用扇区数 |
| `discard_idx` | `unsigned` | 下一个可丢弃的桶索引 |
| `dirty_idx_ondisk` | `unsigned` | 下一个脏桶索引（已记录到磁盘） |
| `dirty_idx` | `unsigned` | 下一个脏桶索引 |
| `cur_idx` | `unsigned` | 当前写入的桶索引 |
| `nr` | `unsigned` | 为此设备分配的桶数 |
| `buckets` | `u64 *` | 桶号数组 |

### 3.5 `struct journal_res`（预留句柄）

定义在 `fs/journal/types.h:127`。

| 字段 | 类型 | 说明 |
|------|------|------|
| `ref` | `bool` | 预留是否活跃持有 |
| `has_overwrites` | `bool` | 条目是否包含覆盖条目 |
| `u64s` | `unsigned` | 预留的 u64 数量 |
| `offset` | `u32` | 当前条目数据中的 jset_entry 偏移量 |
| `seq` | `u64` | 预留绑定的 journal seq |

### 3.6 `struct journal_entry_pin` / `struct journal_entry_pin_list`

定义在 `fs/journal/types.h:107-125`。

```
journal_entry_pin_list（pin FIFO 的每个槽位）：
  ├─ unflushed[6]：每种 pin 类型的未刷新 pin 链表
  ├─ flushed[6]：  每种 pin 类型的已刷新 pin 链表
  ├─ count：       引用计数（btree 节点写入完成归零）
  ├─ unreplayed：  是否尚未重放
  ├─ devs：        此 journal 条目的副本集
  └─ bytes：       此条目大小

journal_entry_pin（单个钉选）：
  ├─ list：   pin_list 链表的节点
  ├─ flush：  刷新回调（journal_pin_flush_fn）
  └─ seq：    引用的 journal seq

Pin 类型（共 6 种，按刷新优先级排序）：
  JOURNAL_PIN_TYPE_btree3  → 根节点级（最高优先级）
  JOURNAL_PIN_TYPE_btree2
  JOURNAL_PIN_TYPE_btree1
  JOURNAL_PIN_TYPE_btree0  → 叶节点级
  JOURNAL_PIN_TYPE_key_cache
  JOURNAL_PIN_TYPE_other   → 其他
```

---

## 4. Journal 条目生命周期

一个 journal 条目从创建到可回收经历六个阶段：

```
  Open      Add      Close     Write     Complete    Reclaim
  │         │        │         │         │           │
  │ seq=N   │ 写入    │ 冻结    │ BIO      │ seq_ondisk│ last_seq
  │ buf开   │ jset_   │ u64s/   │ 提交     │ 推进      │ 推进后
  │ 始接收  │ entry   │ last_seq│ 到设备   │            │ 可回收
  ▼         ▼        ▼         ▼         ▼           ▼
  ┌─────────┬─────────┬─────────┬─────────┬───────────┬─────────┐
  │ 接收预留 │追加数据 │不接受新 │磁盘 I/O │完成清理   │桶空闲可 │
  │          │         │预留     │ 进行中  │           │被丢弃   │
  └─────────┴─────────┴─────────┴─────────┴───────────┴─────────┘
         ▲
         │ bch2_journal_res_get()/journal_res_get_fast()
         │ 无锁 cmpxchg reservations.counter
```

### 4.1 阶段一：打开条目（`journal_entry_open()`）

**前提**：当前条目已关闭（`reservations.cur_entry_offset == CLOSED_VAL`）。

1. **检查**：`j->blocked`、`j->cur_entry_error`、`bch2_journal_error()`
2. **容量检查**：pin FIFO 有空位、in_flight FIFO 有空位、不超过 `JOURNAL_STATE_BUF_NR`（4 个并发打开缓冲区上限）、seq 未溢出或与黑名单冲突
3. **缓冲区准备**：从 `j->free_buf` 取出预分配的缓冲区
4. **序列号分配**：`seq = atomic64_inc_return(&j->seq)` — 单调递增
5. **Pin FIFO**：`fifo_push_ref(&j->pin)`，新槽 `unreplayed = false`
6. **In_flight FIFO**：`fifo_push_ref(&j->in_flight)`，清零初始化
7. **缓冲区初始化**：设置 `expires`、`u64s_reserved`、`sectors`、`must_flush`、`data->seq` 等
8. **Ring 槽发布**：`j->ring[seq & 3] = { buf, buf->data }`
9. **状态机发布**：原子 `cmpxchg` reservations：`idx++`、`cur_entry_offset = init_u64s`
10. 此时该条目对并发预留可见

### 4.2 阶段二：添加数据（`bch2_journal_add_entry()`）

调用者持有 `journal_res` 后，调用 `bch2_journal_add_entry()` 写入带类型的 `jset_entry`：

```c
struct jset_entry *entry = journal_res_entry(j, res);
journal_entry_init(entry, type, id, level, u64s);
res->offset += actual;
res->u64s -= actual;
```

数据直接写入到 ring 槽缓存的 `buf->data` 中给定偏移量。

**常见条目类型：**
- `BCH_JSET_ENTRY_btree_keys` — btree 更新（事务提交时的主要载荷）
- `BCH_JSET_ENTRY_write_buffer_keys` — 写缓冲区键
- `BCH_JSET_ENTRY_btree_root` — btree 根指针

### 4.3 阶段三：关闭条目（`__journal_entry_close()`）

触发条件：
- **条目满了**：慢路径预留检测到空间不足
- **定时器到期**：`bch2_journal_write_work` 在 `journal_flush_delay` ms 后触发
- **显式关闭**：`journal_entry_close_locked()` 被调用
- **最后一个预留释放**：`bch2_journal_buf_put_final()` 触发

关闭操作：
1. **原子 cmpxchg** `reservations.cur_entry_offset` → `CLOSED_VAL`（或 `ERROR_VAL`）
2. **冻结大小**：`buf->data->u64s = le32_to_cpu(old.offset)`
3. **设置 last_seq**：`buf->last_seq = j->last_seq; buf->data->last_seq = cpu_to_le64(buf->last_seq)` — 这是重放起点
4. **取消定时器**：`cancel_delayed_work(&j->write_work)`
5. **更新空间**：`bch2_journal_space_available(j)`
6. **释放引用**：`__bch2_journal_buf_put()` 递减状态计数

### 4.4 阶段四：触发写入（`bch2_journal_do_writes_locked()`）

遍历 `in_flight` FIFO，找到满足以下条件的 seq：
- 已关闭（`cur_entry_offset == CLOSED_VAL`）
- 未开始写入（`!write_started`）
- 引用计数为 0（所有预留都已释放）

→ `write_started = true`，`closure_call(&buf->io, bch2_journal_write, ...)`

### 4.5 阶段五：写入完成（`journal_write_done()`）

核心工作：
1. 将 `buf->data` 交换到 `j->free_buf` 回收
2. **推进 seq_ondisk**：按 seq 顺序推进，确保不会乱序完成

```c
while ((seq = last_uncompleted_write_seq(j, seq_wrote))) {
    // 对于 flush 条目：更新 last_seq_ondisk
    // 更新 seq_ondisk = seq
    // 唤醒等待者
    // 推进 in_flight FIFO 头部
}
```

3. **更新 flushed_seq_ondisk**（仅 flush 条目）
4. **触发回收**：`journal_reclaim_kick()`、`bch2_journal_update_last_seq()`
5. **重新计算空间**：`bch2_journal_space_available(j)`
6. **安排下一个写入**：`bch2_journal_do_writes_locked(j)`

### 4.6 阶段六：回收（Reclaim）

见第 6 节。

---

## 5. Journal 写入路径

### 5.1 预留快速路径（`journal_res_get_fast()`）

这是最热的代码路径，完全无锁，只用一次 `cmpxchg`：

```c
1. 原子读取 j->reservations.counter
2. 检查：cur_entry_offset + needed <= cur_entry_u64s  // 有空间吗？
3. 检查：watermark >= j->watermark                       // 低于压力水位吗？
4. 检查：bufN_count 不溢出                               // 并发限制
5. cmpxchg：递增 cur_entry_offset + bufN_count
6. 输出：res->seq, res->offset, res->ref = true
```

**所有操作在一个 64 位原子 cmpxchg 中完成。**

### 5.2 预留慢路径（`__journal_res_get()` / `bch2_journal_res_get_slowpath()`）

快速路径失败时（条目满、水位过高），在 `j->lock` 下：
1. 如有必要预分配下一个缓冲区
2. 快速路径重试（锁下可能有空间了）
3. 必要时关闭当前条目 → 打开新条目
4. 重试（`-BCH_ERR_journal_retry_open` 信号）

### 5.3 预留释放（`bch2_journal_res_put()`）

```c
1. 未使用的 u64s 用空 btree_keys 条目填充（write_prep 时会被压缩掉）
2. bch2_journal_buf_put(j, res->seq)  → 原子递减 bufN_count
3. 计数归零 → bch2_journal_buf_put_final()
   → __bch2_journal_pin_put → bch2_journal_do_writes_locked()
```

### 5.4 写入阶段详解

```
bch2_journal_write(buf)    [write.c:842]
  │
  ├─ 1. Pick flush 策略
  │    bch2_journal_write_pick_flush()
  │    └─ 决定 flush（FUA）还是 noflush
  │       Noflush 条件：!error && noflush标记 && !must_flush
  │                     && 未超 journal_flush_delay
  │                     && JOURNAL_may_skip_flush 标记
  │
  ├─ 2. 准备数据
  │    bch2_journal_write_prep()
  │    ├─ 写缓冲区刷新：need_flush_to_write_buffer → 刷写缓冲区键到 btree
  │    ├─ 压缩：删除空 jset_entry，合并可合并项
  │    ├─ btree root 传播：复制 btree_root 条目到 c->btree_roots
  │    └─ 追加保留条目：缺失的 btree root、datetime、通用超块条目
  │
  ├─ 3. 分配磁盘空间
  │    journal_write_alloc()
  │    ├─ 遍历目标设备列表
  │    ├─ __journal_write_alloc()
  │    │   └─ buf->key 中追加 extent pointer
  │    │      (offset = bucket_base + bucket_size - sectors_free)
  │    ├─ 保存 dev 指针到 cas[]（防设备移除）
  │    ├─ 递减 ja->sectors_free
  │    └─ 更新 ja->bucket_seq[ja->cur_idx] = cur_seq
  │
  ├─ 4. 校验和 & 加密
  │    bch2_journal_write_checksum()
  │    ├─ 设置 magic/version/flags/csum_type
  │    ├─ 加密（如启用）：bch2_encrypt()
  │    └─ csum_vstruct() 计算全条目校验和
  │
  ├─ 5. 副本记账
  │    └─ bch2_replicas_entry_get() 注册副本集
  │
  ├─ 6a. [Flush 条目]
  │    journal_write_preflush()
  │    ├─ wait_event() 等待先前写入就绪
  │    ├─ 如需要：向每个设备发送 REQ_PREFLUSH bio
  │    └─ journal_write_submit()
  │
  ├─ 6b. [Noflush 条目]
  │    journal_write_submit()
  │
  └─ 7. BIO 提交
       journal_write_submit()
       ├─ 对 buf->key 中每个 extent pointer：
       │   ├─ 从 cas[] 查找设备
       │   ├─ 构建 bio：REQ_OP_WRITE|REQ_SYNC|REQ_IDLE|REQ_META
       │   │            + REQ_FUA/REQ_PREFLUSH（flush 条目）
       │   ├─ 设置 bi_end_io = journal_write_endio
       │   └─ closure_bio_submit(bio, cl)
       └─ continue_at(cl, journal_write_done, j->wq)
```

### 5.5 写入管道的流水线特性

Journal 写入是**流水线化**的：

```
时间 →
├─ Entry N: 打开 → 接收预留 → 关闭 → 写磁盘 →
├─ Entry N+1:            打开 → 接收预留 → 关闭 → 写磁盘 →
├─ Entry N+2:                       打开 → 接收预留 → 关闭 →
```

最多 4 个条目可同时打开（`JOURNAL_STATE_BUF_NR`），但 in_flight FIFO 深度更大。写入是按 seq 顺序完成的——`journal_write_done()` 中的循环强制推进 `seq_ondisk` 时必须按 `seq` 顺序。

### 5.6 Flush vs Noflush

| 特征 | Flush | Noflush |
|------|-------|---------|
| BIO 标志 | `REQ_PREFLUSH | REQ_FUA` | 无特殊标志 |
| 崩溃安全性 | 条目在掉电后存在 | 可能丢失 |
| `flushed_seq_ondisk` | 更新 | 不更新 |
| 何时使用 | 显式 fsync/OSYNC/journal_flush | 批量写入无需耐久保证 |
| Noflush 转换条件 | `must_flush=true` 时强制转为 flush | 见 5.4 节 pick_flush |

---

## 6. Journal 钉选与回收

### 6.1 Pin 的定义

Pin 是 btree 节点对 journal 条目的一种引用计数。每个 btree 节点修改会钉选其引用的最旧 journal seq，确保该 journal 条目在 btree 节点写盘前不会被回收。

**Pin 的生命周期：**

```
btree 节点修改
  → bch2_btree_add_journal_pin(c, b, journal_seq)
    → bch2_journal_pin_add()
      → 丢弃已有的 pin（如果有）
      → atomic_inc(&pin_list->count)    // 增加引用计数
      → pin->seq = seq, pin->flush = 回调
      → 加入 pin_list->unflushed[type]

btree 节点写盘完成
  → __btree_node_write_done()
    → bch2_journal_pin_drop(&c->journal, &w->journal)
      → 从 pin_list 链表中移除
      → atomic_dec(&pin_list->count)    // 减少引用计数

全部引用释放后
  → bch2_journal_update_last_seq()
    → 推进 last_seq 越过 count==0 的条目
```

**关键不变量**：`last_seq` 之前的 journal 条目一定没有被任何 btree 节点引用。

### 6.2 Pin 的两种链表

每个 `journal_entry_pin_list` 包含两个链表（每种 pin 类型各一个）：

```
pin_list（seq = N）：
  unflushed[btree0] → pin_A → pin_B  // 尚未刷新的 pin
  flushed[btree0]   → pin_C          // 已调用 flush 回调的 pin
```

- **unflushed**：pin 刚创建时加入此链表。
- **flushed**：pin 的 `flush` 回调被调用后移至此链表。

`bch2_journal_pin_drop()` 从当前所在链表中移除 pin。

### 6.3 Btree 节点的双写入槽

每个 btree 节点有**两个**写入槽（`writes[0]` 和 `writes[1]`），实现 ping-pong 机制：

```
btree_current_write(b) → 返回当前活跃写入槽

第一次写入：
  writes[0].journal_pin → pin on seq=100
  writes[0] 写盘完成 → pin drop
  writes[1] 变为活跃

第二次写入：
  writes[1].journal_pin → pin on seq=105
  writes[1] 写盘完成 → pin drop
  writes[0] 变为活跃
```

这种设计确保每个 btree 节点最多持有两个 journal pin，一个在写盘中，一个在创建中。

### 6.4 回收触发机制

回收有三种触发方式：

**1. 后台回收线程（`bch2_journal_reclaim_thread`）：**
```
kthread `bch-reclaim/<devname>`
  循环：
    sleep(journal_reclaim_delay)
    __bch2_journal_reclaim(j, direct=false, kicked)
  唤醒条件：
    - journal_reclaim_delay ms 已过
    - reclaim_kicked 标记被置位
    - pin FIFO 非空
```

**2. 直接回收（`bch2_journal_reclaim()`）：**
- 在 `journal_next_bucket()` 中调用——当 journal 即将用尽空间时
- `__bch2_journal_reclaim(j, direct=true, kicked=true)`

**3. Kicked 回收（`journal_reclaim_kick()`）：**
- `journal_write()` 发现 journal 满时调用
- 置位 `j->reclaim_kicked`，唤醒回收线程

### 6.5 回收执行流程（`__bch2_journal_reclaim()`）

```
1. 计算需刷新的目标 seq（seq_to_flush）：
   - 对每个设备：取 (cur_idx + nr/2) % nr 桶的 seq（让 journal 最多半满）
   - 取 journal_cur_seq() - pin.size / 2（pin FIFO 半满阈值）
   - 取两者的最大值

2. 确定最少要刷新的 pin 数（min_nr）：
   - 回收延迟已过 → 至少 1
   - low_on_space 或 low_on_pin → 至少 1
   - btree cache 脏节点 > 50% → 至少 1

3. journal_flush_pins()：
   - 遍历 pin FIFO 从 last_seq 到 seq_to_flush
   - 对每个找到的 pin：
     a. 调用 flush 回调（__btree_node_flush）
        → 设置 BTREE_NODE_need_write | BTREE_WRITE_journal_reclaim
        → btree_node_write_if_need()
     b. 将 pin 从 unflushed[] 移到 flushed[]
   - 重复直到刷够 min_nr 或没有更多 pin

4. bch2_journal_update_last_seq()：
   - 从 last_seq 向前扫描
   - 当 pin_list->count == 0 && seq <= seq_ondisk 时推进 last_seq

5. bch2_journal_space_available()：
   - 对每个设备：
     - 推进 dirty_idx 越过 bucket_seq < last_seq 的桶
     - 推进 dirty_idx_ondisk 越过 bucket_seq < last_seq_ondisk 的桶
   - 重新计算可用空间

6. bch2_journal_dev_do_discards()：
   - 推进 discard_idx 到 dirty_idx_ondisk
   - 如设备支持：blkdev_issue_discard()
```

### 6.6 水位与背压

`bch2_journal_set_watermark()` 计算当前压力水平：

```
三个低水位标记：
  JOURNAL_low_on_space  → 干净空间 ≤ 25% 总 journal 空间
  JOURNAL_low_on_pin    → pin FIFO ≤ 25% 空闲
  JOURNAL_low_on_wb     → 写缓冲区满（btree_write_buffer_must_wait()）

水位值：
  BCH_WATERMARK_stripe   → 正常（0）
  BCH_WATERMARK_reclaim  → 压力中（1）

背压机制：
  journal_res_get_fast() 中：
    if ((flags & BCH_WATERMARK_MASK) < j->watermark)
        return 0;  // 预留失败，进入慢路径阻塞
```

当 `watermark` 提升到 `reclaim` 时，只有请求 `reclaim` 水位的分配才能通过。这限制了普通 btree 更新的写入速度，迫使调用者进入慢路径等待回收完成。

---

## 7. Journal 读取与恢复

### 7.1 入口：`bch2_journal_read()`

在 `fs/journal/read.c:856`，这是恢复的起点。输出 `journal_start_info` 结构体用于确定重放边界。

```
bch2_journal_read(c, &journal_start)
  │
  ├─ 1. 多设备并发读取
  │    for_each_member_device(ca)
  │      closure_call(bch2_journal_read_device)
  │
  ├─ 2. 确定重放边界（反向遍历）
  │
  └─ 3. 完整验证 + 元数据提取
```

### 7.2 设备级读取（`bch2_journal_read_device()`）

每个设备上执行的读取逻辑：

```
bch2_journal_read_device(ca)
  │
  ├─ [优化] 大 journal（>32 桶）：
  │   journal_peek_bucket() 只读每个桶第一块，解析 seq
  │   按 seq 降序排列，按序完整读取
  │   遇到最大 seq < last_seq 的桶时停止（后面的可以跳过）
  │
  └─ [兜底] 顺序读取每个桶：
       journal_read_bucket()
         └─ 逐扇区遍历：
              bch2_jset_validate_early()  → 早期验证
              校验和检查（csum_good）
              解密（如启用）
              journal_entry_add()         → 加入 genradix
```

### 7.3 条目去重（`journal_entry_add()`）

```c
if (genradix 中 seq 已存在条目 dup) {
    if (相同物理位置)
        return;  // 相同数据，跳过
    if (相同设备、不同位置)
        fsck_err("journal_entry_dup_same_device");
    if (本条目校验和正确)
        替换旧条目，保留旧条目的所有 ptrs
    else
        保留现有的校验和正确的条目
}
```

关键设计：**校验和正确的条目胜出**，多设备的指针会被合并。

### 7.4 重放边界计算

读取全部条目后，**反向** 遍历 `journal_entries` genradix：

```
反向遍历：
  找到最高 seq → cur_seq = seq + 1
  找到最新的 flush 条目 → 解析出：
    last_seq    = i->j.last_seq    // 从此开始重放
    replay_end  = i->j.seq         // 重放到此为止
  cur_seq - 1 到 replay_end + 1 之间的条目 → 加入黑名单（noflush/撕裂写入）
```

三个序列号区域：

```
  │←── 已丢弃 ──→│←── 需要重放 ──→│←── 黑名单 ──→│←── 新写入 ──→│
                 │                │               │              │
               last_seq       replay_end    cur_seq (起始点)
```

### 7.5 日志回退（`bch2_journal_reread_for_rewind()`）

当指定 `--journal_rewind` 选项时触发。重新读取在第一次扫描中被丢弃（因早于 `last_seq`）但实际需要的条目。

---

## 8. Journal 重放

### 8.1 早期重放（`journal_replay_early()`）

在排序或 btree 访问之前的预处理阶段：

| 条目类型 | 操作 |
|---------|------|
| `btree_root` | 填充 `btree_root` 结构（btree ID、级别、根 key） |
| `usage` | 设置 `key_version`（用于加密 nonce 派生） |
| `blacklist/blacklist_v2` | 注册到黑名单表 |
| `clock` | 恢复 `io_clock` 时间 |

### 8.2 Journal 键提取与排序（`bch2_journal_keys_sort()`）

```c
bch2_journal_keys_sort(c)
  1. 遍历 journal_entries genradix
  2. 跳过标记为 ignore 的条目
  3. 对每个非忽略条目的 jset_entry：
     - btree_keys 条目 → 提取所有 bkey 为 journal_key
     - overwrite 条目（journal_rewind 时）→ 提取覆盖键
  4. 排序：(btree_id, level, pos, seq)
  5. 去重：非 accounting 键，完全相同时保留最早副本
  6. 输出：排序后的 journal_keys 数组

去重规则：
  排序：btree_id > level > pos > allocated（分配键优先）> seq（旧 seq 优先）
  去重：非 accounting 键的 btree_id/level/pos 完全相同时保留最早条目
  Accounting 键不去重——重放时逐个与 btree 版本比较
```

### 8.3 Journal 重放（`bch2_journal_replay()`）

**第一阶段：重放 accounting 键（优先）**

```c
darray_for_each(keys, k) {
    if (k->type == KEY_TYPE_accounting && !k->allocated) {
        commit_do(trans, ...,
            bch2_journal_replay_accounting_key(trans, k));
        k->overwritten = true;
    }
}
```

**`bch2_journal_replay_accounting_key()` 逻辑：**
1. 查找 btree 中的当前值
2. 如果 `old.bversion >= new.bversion` → 增量已应用，跳过
3. 否则：旧值累加到新增量中，`bch2_trans_update()` 更新

**第二阶段：重放所有键**

```c
// 快速路径：按排序顺序（更好的 btree 局部性）
darray_for_each(keys, k) {
    ret = commit_do(trans, ...,
        bch2_journal_replay_key(trans, k));
    if (ret) push到 keys_sorted;
}

// 排序路径：按 seq 顺序
sort(keys_sorted, journal_sort_seq_cmp);
darray_for_each(keys_sorted, kp) {
    bch2_journal_replay_pins_put(j, seq);  // 释放 journal pin
    commit_do(trans, ..., bch2_journal_replay_key(trans, *kp));
}
```

**`bch2_journal_replay_key()` 逻辑：**
1. 如果 `k->overwritten` → 跳过
2. 设置 `trans->journal_res.seq` → 正确分配 pin
3. 查找/创建 btree path
4. 如果 btree 节点不存在（btree 深度变化）：
   - 扫描/拓扑检查前 → 报错
   - 扫描/拓扑检查后 → 丢弃键或增加 btree 深度
5. `bch2_trans_update()` 更新，`BTREE_TRIGGER_norun`

### 8.4 重放完成（`bch2_journal_set_replay_done()`）

```c
bch2_journal_set_replay_done(j)
  → bch2_journal_space_available(j)  // 计算初始空间
  → set JOURNAL_need_flush_write
  → set JOURNAL_running               // 开始接受预留
  → set JOURNAL_replay_done
```

### 8.5 RO→RW 转换

```
bch2_set_may_go_rw(c)
  → move_gap(keys, keys->nr)           // 锁定 journal_keys 数组
  → set_bit(BCH_FS_may_go_rw)
  → [非只读] bch2_fs_read_write_early()
```

---

## 9. 序列号黑名单

### 9.1 为什么需要黑名单

```
崩溃后的 btree 更新顺序保证：

Btree 节点（bset）记录了其包含更新的最新 journal seq。
如果 bset.seq > newest_journal_entry.seq：
  这个 bset 必须被忽略
原因：
  btree node B 在 btree node A 之后被更新
  B 被刷入磁盘但 A 没有
  恢复时 B 的更新存在，但 A 的更新不存在
  → 破坏了顺序保证
解决：
  忽略所有 journal.seq 高于最新 journal 条目的 bset 是安全的
  因为这些更新都已记录在 journal 中
```

### 9.2 黑名单的结构

黑名单存储在超块字段 `BCH_SB_FIELD_journal_seq_blacklist` 中。每个条目是一个范围 `[start, end)`，表示 seq `start..end-1` 被列入黑名单。

通过 eytzinger 树进行 O(log n) 查找：

| 函数 | 用途 |
|------|------|
| `bch2_journal_seq_is_blacklisted(seq, dirty)` | 检查 seq 是否在黑名单中 |
| `bch2_journal_seq_next_blacklisted(seq)` | 从 seq 开始的下一黑名单范围起点 |
| `bch2_journal_seq_next_nonblacklisted(seq)` | 从 seq 开始的下一非黑名单序列号 |
| `bch2_journal_last_blacklisted_seq()` | 黑名单中的最高 seq |

### 9.3 黑名单的创建

**1. 脏关闭后的跳跃：**
```c
// recovery.c:807-816
journal_start.cur_seq += 64;  // 跳过 64 个 seq
// 原因：btree 写入可能在对应的 journal 写入完成之前引用 journal seq
// 这 64 个 seq 被加入黑名单
```

**2. replay_end 到 cur_seq 的范围：**
```c
// 从 replay_end + 1 到 cur_seq - 1 的未刷写范围
bch2_journal_seq_blacklist_add(c, blacklist_seq, journal_start.cur_seq);
```

**3. Journal 条目中的黑名单：**
- `BCH_JSET_ENTRY_blacklist`：单 seq `[seq, seq+1)`
- `BCH_JSET_ENTRY_blacklist_v2`：范围 `[start, end+1)`

### 9.4 黑名单的合并与 GC

**合并**（`bch2_journal_seq_blacklist_add()`）：
```
遍历现有条目，找到重叠的 [e.start, e.end)
合并：start = min(start, e.start), end = max(end, e.end)
删除旧条目，插入合并后条目
重建 eytzinger 查找表
```

**GC**（`bch2_blacklist_entries_gc()`）：
```
恢复结束时调用：
  移除以下黑名单条目：
  - 未被标记为 dirty
  - 且结束 seq >= oldest_seq_found_ondisk
  清理不再与磁盘上任何 journal 桶相关的陈旧黑名单条目
```

---

## 10. 初始化与关闭

### 10.1 初始化流程（5 阶段）

```
阶段 0：早期 init
  bch2_dev_journal_init_early()  → 每设备：discard_lock、discard work
  bch2_fs_journal_init_early()   → spinlock、mutex、waitqueues、delayed_work
  → 状态：reservations = CLOSED_VAL，journal 关闭锁定

阶段 1：设备 init
  bch2_dev_journal_init(ca, sb)
  → 从 SB 字段读取桶信息：bucket_seq[], buckets[], bioset
  → 状态：每设备：桶数组已填充，bioset 就绪

阶段 2：文件系统 init
  bch2_fs_journal_init(j)
  → 分配 free_buf（64K）
  → 分配 in_flight FIFO（256 槽）
  → 创建写入/丢弃工作队列
  → 状态：数据结构完全初始化

阶段 3：桶分配
  bch2_fs_journal_alloc(c)
  → 每设备：分配桶（max(8, min(nbuckets/128, 8192, 8GB/桶大小))）
  → 标记为 BCH_DATA_journal
  → 写入超级块
  → 状态：桶已分配

--- journal_read() 在此发生 ---

阶段 4：启动（重放后）
  bch2_fs_journal_start(j, info)
  → 分配 pin FIFO：max(nr + nr/4, 32768)
  → 设置序列号范围（seq、seq_ondisk、flushed_seq_ondisk 等）
  → 用 unreplayed=true 初始化 pin 条目
  → 遍历已读取条目进行副本验证

阶段 5：重放完成
  bch2_journal_set_replay_done(j)
  → journal_space_available()
  → set JOURNAL_need_flush_write
  → set JOURNAL_running      ← 开始接受预留
  → set JOURNAL_replay_done
  → 状态：journal 完全运行
```

### 10.2 关闭流程

```
bch2_fs_journal_stop(c)
  1. 停止回收线程
  2. 等待所有 pin 刷新（bch2_journal_flush_all_pins）
  3. 等待条目关闭
  4. 写入 meta 条目（确保超块时钟一致）
  5. 关闭并等待最后一个条目同步
  6. 取消 write_work 定时器
  7. 清除 JOURNAL_running

bch2_fs_journal_exit(c)
  → 销毁工作队列
  → 释放 early_journal_entries、rewind_ranges
  → 释放 in_flight FIFO 中所有缓冲区的 data
  → 释放 free_buf、pin FIFO
```

### 10.3 错误处理（`bch2_journal_halt()`）

```
journal 写入失败时：
  1. 记录错误消息
  2. bch2_fs_emergency_read_only() → 文件系统只读
  3. 设置 j->err_seq
  4. 释放所有设备引用
  5. 继续到 journal_write_done() 清理
```

Journal 错误后，所有新 journal 预留都会失败（`-BCH_ERR_journal_shutdown`）。文件系统进入只读模式，需要重新挂载才能恢复。

---

## 11. 参考代码位置

### 核心数据结构

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/journal/types.h` | 37-73 | `struct journal_buf` — 每条目暂存区 |
| `fs/journal/types.h` | 75 | `struct journal_ringbuf` — Ring 缓冲区槽 |
| `fs/journal/types.h` | 97-105 | `enum journal_pin_type` — Pin 类型枚举 |
| `fs/journal/types.h` | 107-114 | `struct journal_entry_pin_list` — Pin FIFO 槽 |
| `fs/journal/types.h` | 121-125 | `struct journal_entry_pin` — 钉选结构 |
| `fs/journal/types.h` | 127-133 | `struct journal_res` — 预留句柄 |
| `fs/journal/types.h` | 135-152 | `union journal_res_state` — 原子状态机 |
| `fs/journal/types.h` | 175-179 | `struct journal_space` — 空间信息 |
| `fs/journal/types.h` | 219-383 | `struct journal` — 主结构体 |
| `fs/journal/types.h` | 391-430 | `struct journal_device` — 每设备结构 |
| `fs/journal/types.h` | 435-437 | `struct journal_entry_res` — 每条目预留 |
| `fs/journal/types.h` | 465-470 | `struct journal_start_info` — 启动信息 |
| `fs/bcachefs_format.h` | 918-928 | `struct jset_entry` — 条目头部 |
| `fs/bcachefs_format.h` | 1621-1667 | `BCH_JSET_ENTRY_TYPES` — 条目类型定义 |
| `fs/bcachefs_format.h` | 1801-1826 | `struct jset` — On-disk 条目格式 |

### 初始化与关闭

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/journal/init.c` | 259-301 | `bch2_dev_journal_alloc()` — 每设备桶分配 |
| `fs/journal/init.c` | 304-319 | `bch2_fs_journal_alloc()` — 所有设备分配 |
| `fs/journal/init.c` | 344-371 | `bch2_fs_journal_stop()` — 停止 |
| `fs/journal/init.c` | 373-493 | `bch2_fs_journal_start()` — 重放后启动 |
| `fs/journal/init.c` | 495-507 | `bch2_journal_set_replay_done()` — 设置运行 |
| `fs/journal/init.c` | 523-527 | `bch2_dev_journal_init_early()` — 早期 init |
| `fs/journal/init.c` | 529-582 | `bch2_dev_journal_init()` — 设备 init |
| `fs/journal/init.c` | 584-610 | `bch2_fs_journal_exit()` — 销毁 |
| `fs/journal/init.c` | 612-630 | `bch2_fs_journal_init_early()` — FS 早期 init |
| `fs/journal/init.c` | 632-669 | `bch2_fs_journal_init()` — FS init |

### 写入路径

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/journal/journal.c` | 248-263 | `bch2_journal_do_writes_locked()` — 写入触发 |
| `fs/journal/journal.c` | 301-392 | `__journal_entry_close()` — 条目关闭 |
| `fs/journal/journal.c` | 440-591 | `journal_entry_open()` — 条目打开 |
| `fs/journal/journal.c` | 593-681 | `__journal_res_get()` — 预留慢路径 |
| `fs/journal/journal.c` | 883-920 | `bch2_journal_flush_seq_async()` — 异步 flush |
| `fs/journal/write.c` | 59-110 | `__journal_write_alloc()` — 磁盘分配 |
| `fs/journal/write.c` | 234-381 | `journal_write_done()` — 写入完成 |
| `fs/journal/write.c` | 460-530 | `journal_write_submit()` — BIO 提交 |
| `fs/journal/write.c` | 536-579 | `journal_write_preflush()` — Flush/FUA 提交 |
| `fs/journal/write.c` | 581-694 | `bch2_journal_write_prep()` — 写入准备 |
| `fs/journal/write.c` | 696-737 | `bch2_journal_write_checksum()` — 校验和 |
| `fs/journal/write.c` | 739-840 | `bch2_journal_write_pick_flush()` — Flush 决策 |
| `fs/journal/write.c` | 842-975 | `bch2_journal_write()` — 写入入口 |

### 快速路径预留（Inline）

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/journal/journal.h` | 256-277 | `journal_res_get_fast()` — 无锁预留 |
| `fs/journal/journal.h` | 285-305 | `bch2_journal_add_entry()` — 添加条目 |
| `fs/journal/journal.h` | 307-341 | `bch2_journal_res_get()` — 预留入口 |
| `fs/journal/journal.h` | 343-391 | `bch2_journal_res_put()` — 预留释放 |
| `fs/journal/journal.h` | 393-440 | `journal_res_get_fast()` 完整实现 |

### 回收与钉选

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/journal/reclaim.c` | 1-60 | `bch2_journal_pin_set()` — 设置 pin |
| `fs/journal/reclaim.c` | 62-82 | `bch2_journal_pin_drop()` — 释放 pin |
| `fs/journal/reclaim.c` | 84-180 | `bch2_journal_update_last_seq()` — 推进 last_seq |
| `fs/journal/reclaim.c` | 182-235 | `bch2_journal_space_available()` — 空间计算 |
| `fs/journal/reclaim.c` | 237-286 | `bch2_journal_dev_do_discards()` — 丢弃桶 |
| `fs/journal/reclaim.c` | 288-348 | `bch2_journal_set_watermark()` — 水位设置 |
| `fs/journal/reclaim.c` | 350-396 | `__bch2_journal_reclaim()` — 回收主函数 |
| `fs/journal/reclaim.c` | 398-433 | `bch2_journal_reclaim_thread()` — 回收线程 |
| `fs/journal/reclaim.c` | 435-458 | `journal_flush_pins()` — 刷新 pin |
| `fs/journal/reclaim.c` | 460-478 | `journal_get_next_pin()` — 查找下一个 pin |
| `fs/journal/reclaim.h` | 1-20 | 内联帮助函数（`fifo_push_ref`、`journal_pin_list_init`） |
| `fs/btree/commit.c` | 458-477 | `bch2_btree_add_journal_pin()` — Btree pin 设置 |
| `fs/btree/write.c` | 44 | `bch2_journal_pin_drop()` — Btree pin 释放 |

### 读取与恢复

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/journal/read.c` | 171-304 | `journal_entry_add()` — 条目去重 |
| `fs/journal/read.c` | 326-445 | `journal_read_bucket()` — 桶读取 |
| `fs/journal/read.c` | 447-710 | `bch2_journal_read_device()` — 设备读取 |
| `fs/journal/read.c` | 712-757 | `bch2_journal_check_for_missing()` — 检查缺失 |
| `fs/journal/read.c` | 767-854 | `bch2_journal_reread_for_rewind()` — 回退读取 |
| `fs/journal/read.c` | 856-1082 | `bch2_journal_read()` — 读取入口 |
| `fs/journal/validate.c` | 694-746 | `bch2_jset_validate()` — 完整验证 |
| `fs/journal/validate.c` | 748-787 | `bch2_jset_validate_early()` — 早期验证 |
| `fs/init/recovery.c` | 377-511 | `bch2_journal_replay()` — 重放 |
| `fs/init/recovery.c` | 585-610 | `journal_replay_early()` — 早期重放 |
| `fs/init/recovery.c` | 807-816 | 黑名单跳跃（+64 seq） |
| `fs/init/recovery.c` | 998-1010 | `bch2_fs_recovery()` — 恢复入口 |

### 黑名单

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/journal/seq_blacklist.c` | 49-96 | `bch2_journal_seq_blacklist_add()` — 添加/合并 |
| `fs/journal/seq_blacklist.c` | 98-140 | `bch2_blacklist_table_initialize()` — Eytzinger 索引 |
| `fs/journal/seq_blacklist.c` | 142-191 | `bch2_journal_seq_is_blacklisted()` — 查询 |
| `fs/journal/seq_blacklist.c` | 276-311 | `bch2_blacklist_entries_gc()` — GC |
| `fs/journal/seq_blacklist.h` | 全部 | 查询函数声明 |

### 键提取与排序

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/btree/journal_overlay.c` | 780-807 | `__journal_keys_sort()` — 排序+去重 |
| `fs/btree/journal_overlay.c` | 818-910 | `bch2_journal_keys_sort()` — 键提取 |
| `fs/btree/journal_overlay_types.h` | 全部 | `struct journal_replay`、`journal_ptr`、`journal_key` |

---

> **关联文档**：[事务实现分析](bcachefs-transaction-implementation.md)
> 事务提交中 journal 的集成在 `bch2_trans_commit()` 的 `bch2_journal_add_entry()` 调用点。
> Btree 节点写入与 journal pin 的交互在 `bch2_btree_node_write()` 的 `bch2_journal_pin_drop()` 调用点。
