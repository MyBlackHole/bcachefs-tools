# bcachefs Journal 子系统架构总结

> 生成日期：2026-05-17
> 源码阅读范围：`fs/journal/`（types.h, journal.h, journal.c, write.c, reclaim.c, read.c, init.c, validate.c, seq_blacklist.c）
> 参考文档：`doc/analysis/bcachefs-journal-analysis.md`、`doc/analysis/bcachefs-journal-bucket-lifecycle.md`

---

## 1. 设计目标与核心职责

Journal（日志）是 bcachefs 崩溃一致性的核心机制，保证所有 btree 修改在写入磁盘之前先被日志记录。

**四个核心职责：**
1. **崩溃恢复** — 未写入 btree 节点的事务通过 journal replay 重放
2. **写入排序** — 保证 btree 节点写回和 journal 条目间的顺序关系
3. **批量提交** — 多个事务的 btree 更新合并到一个 journal 条目中
4. **空间回收** — 已刷入 btree 节点的 journal 条目可以被回收

**核心顺序约束（关键不变量）：**
```
事务提交 → journal 条目（必须先持久化）
               ↓
           btree 节点写盘（可延迟）
               ↓
           journal 条目可回收（journal reclaim）
```
Journal 必须在 btree 节点写盘之前持久化，否则 crash 后 journal 丢失而 btree 已更新 → 不一致。

---

## 2. 核心数据结构与设计模式

### 2.1 `union journal_res_state` — 无锁状态机

位于 `fs/journal/types.h:135`。打包在单个 `u64` 中，是 journal 最核心的热路径设计：

```
 63          42 41        22 21         0
├─────bufN_counts─────┤── idx ─┤─ offset ─┤
│  4 × 10 位引用计数  │  2 位  │  22 位   │
                         ↑ ring 缓冲区索引
```

- **`cur_entry_offset`**（22 位）：当前条目已用 u64 数。特殊哨兵值：
  - `JOURNAL_ENTRY_CLOSED_VAL` → 条目已关闭
  - `JOURNAL_ENTRY_BLOCKED_VAL` → 写入缓冲区 flush 阻塞中
  - `JOURNAL_ENTRY_ERROR_VAL` → Journal 错误停机
- **`idx`**（2 位）：当前活跃的 ring 缓冲区索引（0..3）。
- **`bufN_count`**（4 × 10 位）：每个 ring 槽位的并发预留引用计数（最大 1023）。

**设计意图**：整个预留/提交热路径通过一次 `cmpxchg` 修改此原子字完成，完全不依赖互斥锁。

### 2.2 `struct journal` — 子系统中枢

位于 `fs/journal/types.h:219`，嵌入 `struct bch_fs`。

**序列号系统（驱动整个日志的递增时钟）：**
- `seq`（atomic64）— 最新分配序列号，`journal_entry_open` 时原子递增
- `seq_write_started` — 已启动写入的最新 seq
- `seq_ondisk` — 已持久化到磁盘的最新 seq
- `flushed_seq_ondisk` — 已通过 FUA flush 写入的最新 seq（`fsync` 边界）
- `last_seq` — 最旧脏条目 seq（仍有 btree 节点引用）
- `last_seq_ondisk` — 最后写入磁盘的 last_seq

**关键不变量**：`last_seq` 之前的 journal 条目一定没有被任何 btree 节点引用。

### 2.3 Ring 缓冲与 FIFO

Journal 使用**三层缓冲结构**：
1. **Ring `[4]`** — `seq & 3` 索引，存有 `(journal_buf*, jset*)` 对，快速路径通过它直接写入
2. **`in_flight` FIFO** — 动态大小，范围 `(seq_ondisk, cur_seq]`，管理正在写入的条目
3. **`pin` FIFO** — 每 seq 的引用计数/状态，管理 btree 节点的 journal 引用

**为什么是 ring buffer 3（4 槽）？** `JOURNAL_STATE_BUF_NR = 4`，最多 4 个条目可同时处于打开状态。更多条目可以排入 in_flight FIFO 等待写入。

### 2.4 `struct journal_entry_pin` / `journal_entry_pin_list` — 引用计数

Pin 是 btree 节点对 journal 条目的引用：

```
journal_entry_pin_list（pin FIFO 每个槽位）：
  ├─ unflushed[6]：每种 pin 类型的未刷新 pin 链表
  ├─ flushed[6]：  每种 pin 类型的已刷新 pin 链表
  ├─ count：       引用计数（btree 节点写入完成归零）
  └─ bytes：       此条目大小

6 种 pin 类型按刷新优先级排序：
  btree3 > btree2 > btree1 > btree0 > key_cache > other
```

Btree 节点通过**双写入槽**（`writes[0]`/`writes[1]`）实现 ping-pong pin 管理，每个节点最多持有两个 journal pin（一个在写盘中，一个在创建中）。

### 2.5 `struct journal_buf` — 缓冲区生命周期

每条目暂存区，`fs/journal/types.h:37`。

```
创建 → journal_entry_open() 中 fifo_push_ref
活跃 → 通过 journal_res 接收数据
关闭 → __journal_entry_close() 设置最终 u64s/last_seq
写入 → bch2_journal_write() 提交 BIO
完成 → journal_write_done() 中 data 放回 free_buf
```

---

## 3. 核心代码路径

### 3.1 预留路径（Reservation）

**快速路径** `journal_res_get_fast()` (`journal.h:256`） — 完全无锁：

```c
1. 原子读取 j->reservations.counter
2. 检查：cur_entry_offset + needed <= cur_entry_u64s  // 有空间吗？
3. 检查：watermark >= j->watermark                       // 低于压力水位吗？
4. 检查：bufN_count 不溢出                               // 并发限制
5. cmpxchg：递增 cur_entry_offset + bufN_count
6. 输出：res->seq, res->offset, res->ref = true
```

所有操作在**一次 64 位原子 cmpxchg** 中完成。失败时进入慢路径 `__journal_res_get()` (`journal.c:593`）。

**慢路径**在 `j->lock` 下执行：预分配下一个缓冲区、必要时关闭当前条目并打开新条目、阻塞等待 flush。

### 3.2 条目打开/关闭（`journal_entry_open` / `__journal_entry_close`）

**打开** (`journal.c:440`）：检查状态 → 分配 seq → push pin FIFO → push in_flight FIFO → 设置 ring 槽位 → cmpxchg 发布状态字。

**关闭** (`journal.c:301`）：触发条件有四种：
- 条目满了（慢路径检测到空间不足）
- 定时器到期（`bch2_journal_write_work` 在 `journal_flush_delay` ms 后触发）
- 显式关闭（`journal_entry_close_locked`）
- 最后一个预留释放（`bch2_journal_buf_put_final`）

关闭操作：cmpxchg offset → CLOSED_VAL → 冻结 u64s/last_seq → 取消定时器 → 释放引用。

### 3.3 写入路径（`bch2_journal_write`）

`write.c:842`，写入管道：

```
1. Pick flush 策略          → flush（FUA）还是 noflush 条目
2. 准备数据                 → 刷写缓冲区键、压缩空条目、传播 btree root、追加保留条目
3. 分配磁盘空间             → 每个设备上分配 journal 桶空间（桶底部向上增长）
4. 校验和 & 加密           → csum_vstruct 计算全条目校验和
5. 副本记账                 → 注册副本集
6a. [Flush] 发送 PREFLUSH   → 等待先前写入完成，向每个设备发送 REQ_PREFLUSH bio
6b. [Noflush] 直接提交
7. BIO 提交                 → closure_bio_submit，完成时 journal_write_done
```

**流水线特性**：最多 4 个条目同时打开，写入按 seq 顺序完成（`journal_write_done` 中强制顺序推进 `seq_ondisk`）。

**Flush vs Noflush**：
| 特征 | Flush | Noflush |
|------|-------|---------|
| BIO 标志 | `PREFLUSH\|FUA` | 无特殊标志 |
| 崩溃安全性 | 条目在掉电后存在 | 可能丢失 |
| `flushed_seq_ondisk` | 更新 | 不更新 |
| 何时使用 | fsync/journal_flush | 批量写入无需耐久保证 |

### 3.4 回收路径（Reclaim）

三种触发方式：
1. **后台回收线程** `bch2_journal_reclaim_thread` — 定期运行
2. **直接回收** `bch2_journal_reclaim` — journal 空间不足时在 `journal_next_bucket` 中调用
3. **Kicked 回收** `journal_reclaim_kick` — journal 满时唤醒回收线程

`__bch2_journal_reclaim()` (`reclaim.c:350`）执行流程：

```
1. 计算目标 seq（seq_to_flush）：取设备桶半满阈值与 pin FIFO 半满阈值中的较大者
2. 确定最少刷新 pin 数（min_nr）
3. journal_flush_pins()：遍历 pin FIFO，调用 flush 回调，将 pin 从 unflushed→flushed
4. bch2_journal_update_last_seq()：推进 last_seq 越过 count==0 && seq<=seq_ondisk 的条目
5. bch2_journal_space_available()：推进每设备的 dirty_idx 越过可回收桶
6. bch2_journal_dev_do_discards()：TRIM 已丢弃桶
```

**水位与背压机制**：`bch2_journal_set_watermark()` 计算三个低水位标记（space/pin/wb），当压力提升到 `BCH_WATERMARK_reclaim` 时，只有高优先级分配能通过，普通 btree 更新被阻塞。

### 3.5 读取与重放（Read & Replay）

`bch2_journal_read()` (`read.c:856`）是恢复入口：

```
1. 多设备并发读取（每个设备一个 closure）
2. 反向遍历确定重放边界
3. 完整验证 + 元数据提取
```

**设备级读取** `bch2_journal_read_device()`：大 journal（>32 桶）先 peek 每个桶的第一块确定 seq 顺序，再按序完整读取；小 journal 直接顺序读每桶。

**条目去重** `journal_entry_add()`：seq 重复时，校验和正确的条目胜出，多设备指针合并。

**重放边界计算**：反向遍历找到最新 flush 条目，解析其 `last_seq`（重放起点）和 `seq`（重放终点）。

**重放** `bch2_journal_replay()`：
- 第一阶段：重放 accounting 键（优先，版本比较去重）
- 第二阶段：重放所有键（先按排序顺序以获得更好 btree 局部性，失败则回退到 seq 顺序）

---

## 4. 初始化与关闭

**初始化 5 阶段：**
1. **早期 init** — spinlock/mutex/waitqueues/delayed_work
2. **设备 init** — 从超级块加载桶信息（`bch2_dev_journal_init`，**不分配新桶**）
3. **文件系统 init** — 分配 free_buf（64K）, in_flight FIFO（256 槽）, 工作队列
4. **桶分配**（mkfs 时） — `bch2_dev_journal_alloc` 分配桶（`max(8, min(nbuckets/128, 8192, 8GB))`），标记为 `BCH_DATA_journal`
5. **启动**（重放后）— `bch2_fs_journal_start` 设置序列号范围，`bch2_journal_set_replay_done` 开始接受预留

**关闭** `bch2_fs_journal_stop()`：停止回收线程 → flush 所有 pin → 写入 meta 条目 → 等待最后一个条目同步 → 清除 `JOURNAL_running`。

**错误处理** `bch2_journal_halt()`：写入失败 → 文件系统只读 → 设置 `err_seq` → 后续所有预留失败。

---

## 5. 序列号黑名单

为什么需要：崩溃后，如果 btree 节点 B 已被刷盘而节点 A 未刷盘，但 B 引用了比 journal 中最高 seq 还新的更新，一致性被破坏。解决方案：忽略所有 journal seq 高于最新 journal 条目的 bset 更新。

黑名单条目存储在超级块 `BCH_SB_FIELD_journal_seq_blacklist` 中，通过 eytzinger 树进行 `O(log n)` 查找。

创建时机：
1. 脏关闭后跳 64 个 seq（btree 写引用可能晚于 journal 写完成）
2. `replay_end` 到 `cur_seq` 之间的未刷写范围

GC 在恢复结束时清理不再与磁盘上任何 journal 桶相关的陈旧黑名单条目。

---

## 6. Journal Bucket 生命周期

关键事实：**journal bucket 不是在挂载时分配的。** 它们在 mkfs 时预先分配并持久化存储在超级块中。挂载时从磁盘加载预先分配的列表。

**分配路径选择**（`bch2_bucket_alloc_trans`）：
| 场景 | `freespace_initialized` | 使用的分配器 |
|---|---|---|
| mkfs | false | `bch2_bucket_alloc_early`（扫描 alloc btree） |
| 运行时扩容 | true | `bch2_bucket_alloc_freelist`（freespace btree） |

**桶索引不变量（每设备）**：`discard_idx ≤ dirty_idx_ondisk ≤ dirty_idx ≤ cur_idx`

`bucket_seq[]` 数组记录每个桶中包含的最大 seq，用于判断桶是否可以回收。

---

## 7. 关键设计模式总结

| 模式 | 应用位置 | 说明 |
|------|---------|------|
| **无锁预留** | `union journal_res_state` + `cmpxchg` | 预留快速路径单原子操作，零锁竞争 |
| **FIFO ring（seq 驱动）** | `ring[4]` / `in_flight` / `pin` FIFO | 单调递增 seq 作为索引键，顺序天然保证 |
| **写时关闭** | `__journal_entry_close` → `bch2_journal_do_writes_locked` | 条目写时关闭（非满时才关闭），驱动写入管道 |
| **GC 驱动 reclaim** | `__bch2_journal_reclaim` → `bch2_journal_update_last_seq` | 回收由后台线程和空间压力共同驱动 |
| **水位背压** | `bch2_journal_set_watermark` | 空间不足时限制新预留进入，迫使调用者等待 |
| **双写入槽 ping-pong** | `bch2_btree_add_journal_pin` / `bch2_journal_pin_drop` | Btree 节点通过两个写入槽交替引用 journal，最多持有一个活跃 pin |
| **Eytzinger 二分查找** | `seq_blacklist.c` | 黑名单查找通过 eytzinger 树实现 O(log n) 查询 |
| **流水线写入** | write.c | 打开/写入/完成三个阶段的流水线，最多 4 个条目同时打开 |

---

## 8. 文件索引

| 文件 | 行数 | 核心功能 |
|------|------|---------|
| `fs/journal/types.h` | ~470 | 所有核心数据结构定义 |
| `fs/journal/journal.h` | ~440 | 内联 API：快速路径预留、添加条目、预留释放 |
| `fs/journal/journal.c` | ~1341 | 主逻辑：打开/关闭条目、预留慢路径、flush |
| `fs/journal/write.c` | ~966 | 写入路径：分配、准备、校验和、BIO 提交 |
| `fs/journal/reclaim.c` | ~478 | 回收：pin 管理、last_seq 推进、空间计算、discard |
| `fs/journal/read.c` | ~1082 | 读取与恢复：桶扫描、条目去重、重放边界 |
| `fs/journal/init.c` | ~669 | 初始化与关闭：桶分配、start/stop |
| `fs/journal/validate.c` | ~787 | 条目校验：jset_validate、per-entry-type 校验 |
| `fs/journal/seq_blacklist.c` | ~311 | 序列号黑名单：添加/合并/查询/GC/eytzinger 树 |
| `fs/btree/journal_overlay.c` | ~910 | Journal 键提取与排序（重放前半段） |
| `fs/init/recovery.c` | ~511-1010 | 重放入口、早期重放、黑名单跳跃 |

---

## 9. 与关联子系统的关系

- **事务系统**（`bch2_trans_commit`）：在 `write_locked` 阶段调用 `bch2_journal_add_entry` 写入 btree 更新；`write_unlocked` 阶段异步写盘后调用 `bch2_journal_pin_drop`
- **Btree 写入**（`bch2_btree_node_write`）：通过 `bch2_btree_add_journal_pin` 建立 journal pin，写入完成后调用 `bch2_journal_pin_drop`
- **写缓冲区**（write buffer）：`write_buffer_keys` 类型的 jset_entry 在 journal 写入前被 flush 到 btree 中
- **分配器**（allocator）：journal 桶的分配通过通用分配器 `bch2_bucket_alloc_trans`，标记为 `BCH_DATA_journal`
- **超级块**（superblock）：存储 journal 桶列表（`BCH_SB_FIELD_journal`/`journal_v2`）和序列号黑名单（`BCH_SB_FIELD_journal_seq_blacklist`）
