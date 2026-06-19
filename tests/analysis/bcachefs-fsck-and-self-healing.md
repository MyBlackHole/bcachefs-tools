# bcachefs Fsck 与自愈子系统

> 分析日期：2026-05-17
> 隶属分析系列：[启动分配器分析索引](bcachefs-alloc-analysis-index.md)

---

## 目录

1. [Fsck 启动流程](#1-fsck-启动流程)
2. [关键修复函数](#2-关键修复函数)
3. [错误检测基础设施](#3-错误检测基础设施)
4. [Fsck 错误框架](#4-fsck-错误框架)
5. [自愈机制](#5-自愈机制)
6. [特定错误类型恢复](#6-特定错误类型恢复)
7. [Btree 节点修复](#7-btree-节点修复)
8. [日志重放 Fsck](#8-日志重放-fsck)
9. [参考代码位置](#9-参考代码位置)

---

## 1. Fsck 启动流程

### 1.1 没有独立的 `bch2_fsck_start()`

bcachefs 采用**恢复通道（recovery pass）系统**替代独立的 fsck 入口。Fsck 验证和修复是内联在挂载路径中的一系列通道。

### 1.2 恢复路径

```
__bch2_fs_start(c)                           [fs/init/fs.c]
  └─ bch2_fs_recovery(c)                     [fs/init/recovery.c:662]
       └─ 逐通道执行：
           for (p = 0; p < BCH_RECOVERY_PASS_NR; p++) {
               // 检查 pass.flags 是否应执行
               //    PASS_ALWAYS: 总是执行
               //    PASS_UNCLEAN: 仅非干净卸载时执行
               //    PASS_FSCK:    fsck 模式下执行
               //    PASS_ONLINE: 在线挂载 + fsck 时执行
               //    PASS_NODEFER: 不推迟到在线 fsck
               switch (p) {
                   case BCH_RECOVERY_PASS_alloc_read:
                       ...
                   case BCH_RECOVERY_PASS_check_extents:
                       bch2_check_extents(...)
                   ...
               }
           }
```

### 1.3 通道定义

所有通道 ID 定义在 `BCH_RECOVERY_PASSES()` x-macro 中：

```c
// fs/init/passes_format.h
#define BCH_RECOVERY_PASSES()                   \
    x(alloc_read,               0,  PASS_SILENT)                    \
    x(stripes_read,             1,  PASS_SILENT)                    \
    x(initialize_subvolumes,    2,  PASS_SILENT|PASS_UNCLEAN)       \
    x(snapshots_read,           3,  PASS_SILENT)                    \
    x(alloc_scan_old_stripe,    4,  PASS_SILENT)                    \
    x(set_may_go_rw,            5,  PASS_SILENT)                    \
    x(journal_replay,           6,  PASS_SILENT)                    \
    x(check_alloc_info,         7,  PASS_FSCK|PASS_ALLOC)           \
    x(check_lrus,               8,  PASS_FSCK|PASS_ALLOC)           \
    x(check_btree_backpointers, 9,  PASS_FSCK|PASS_ALLOC)           \
    x(check_backpointers_to_extents, 10, PASS_FSCK|PASS_ALLOC)      \
    x(check_extents_to_backpointers, 11, PASS_FSCK|PASS_ALLOC)      \
    x(check_extents,            12, PASS_FSCK|PASS_ALLOC)           \
    x(check_indirect_extents,   13, PASS_FSCK|PASS_ALLOC)           \
    x(check_alloc_to_lru_refs,  14, PASS_FSCK|PASS_ALLOC)           \
    x(check_alloc_copygc,       15, PASS_FSCK|PASS_ALLOC)           \
    x(check_alloc_btree_counts, 16, PASS_FSCK)                      \
    x(check_alloc_holes_freespace, 17, PASS_FSCK|PASS_ALLOC)        \
    x(check_alloc_holes_freespace2, 18, PASS_FSCK|PASS_ALLOC)       \
    x(check_inode_deletion_list, 19, PASS_FSCK|PASS_ONLINE)         \
    x(check_inodes,             20, PASS_FSCK)                       \
    x(check_reserved_inodes,    21, PASS_FSCK)                       \
    x(check_dirents,            22, PASS_FSCK)                       \
    x(check_xattrs,             23, PASS_FSCK)                       \
    x(check_root,               24, PASS_FSCK)                       \
    x(check_paths,              25, PASS_FSCK)                       \
    x(check_nlinks,             26, PASS_FSCK)                       \
    x(check_directory_structure,27, PASS_FSCK|PASS_ONLINE)          \
    x(check_btree_counts,       28, PASS_FSCK|PASS_ONLINE)          \
    x(rebalance_walks,          29, PASS_SILENT)                    \
    x(delete_dead_snapshots,    30, PASS_FSCK|PASS_ONLINE)          \
    x(check_snapshots,          31, PASS_SILENT)                    \
    x(check_snapshot_trees,     32, PASS_SILENT)                    \
    x(check_subvols,            33, PASS_SILENT)                    \
    x(fs_freespace_init,        34, PASS_FSCK|PASS_ALLOC)           \
    x(rendezvous_fsck,          35, PASS_FSCK|PASS_ONLINE)          \
    x(fs_freespace_init2,       36, PASS_FSCK|PASS_ALLOC)           \
    ...
```

### 1.4 通道标志

| 标志 | 含义 |
|------|------|
| `PASS_SILENT` | 静默执行（正常挂载所需） |
| `PASS_FSCK` | 仅在 fsck 模式下执行（`c->opts.fsck`） |
| `PASS_UNCLEAN` | 仅上次未干净卸载时执行 |
| `PASS_ALWAYS` | 无条件执行 |
| `PASS_ONLINE` | 支持在线 fsck（可推迟到挂载后） |
| `PASS_ALLOC` | 分配器相关，允许在某些情况下跳过 |
| `PASS_NODEFER` | 不可推迟到在线 fsck |

### 1.5 执行顺序

1. **预重放通道**（0-5）：读取 alloc、stripes、snapshots，初始化子卷
2. **`set_may_go_rw`**（5）：允许 btree 写入，启动 journal 回收
3. **`journal_replay`**（6）：重放 journal 条目
4. **fsck 分配通道**（7-18）：验证 alloc、lrus、backpointers、extents
5. **fsck 文件系统通道**（19-28）：验证 inodes、dirents、xattrs、nlinks
6. **快照清理**（29-33）：删除死亡快照、验证快照树
7. **收尾**（34+）：freespace 初始化，在线修复

---

## 2. 关键修复函数

### 2.1 文件系统结构检查

所有这些检查函数都通过 `fsck_err` 宏报告问题，并拥有自动修复能力：

| 函数 | 位置 | 作用 |
|------|------|------|
| `bch2_check_extents()` | `fs/fs/check_extents.c` | 验证 extent 键的引用计数、data_type 一致性、backpointer 合法性 |
| `bch2_check_indirect_extents()` | `fs/fs/check_extents.c` | 验证间接 extent（reflink）的引用计数 |
| `bch2_check_inodes()` | `fs/fs/check_inodes.c` | 验证每个 inode 的元数据一致性 |
| `bch2_check_dirents()` | `fs/fs/check_dirents.c` | 验证目录条目有效性和 inode 引用 |
| `bch2_check_xattrs()` | `fs/fs/check_xattrs.c` | 验证扩展属性 |
| `bch2_check_nlinks()` | `fs/fs/check_nlinks.c` | 验证链接计数一致性 |
| `bch2_check_directory_structure()` | `fs/fs/check_directory_structure.c` | 检查目录循环和连通性 |
| `bch2_check_paths()` | `fs/fs/check_paths.c` | 验证路径可达性 |

### 2.2 分配与后向指针检查

| 函数 | 文件:行 | 作用 |
|------|---------|------|
| `bch2_check_alloc_key()` | `fs/alloc/check.c:141` | 验证单个 alloc 键的 data_type 和 sector 计数一致性 |
| `bch2_check_alloc_info()` | `fs/alloc/check.c:631` | 全局分配信息交叉验证 |
| `bch2_check_alloc_to_lru_refs()` | `fs/alloc/check.c:737` | 验证 alloc → LRU 引用 |
| `bch2_check_lrus()` | `fs/alloc/lru.c:201` | 验证 LRU btree 条目 |
| `bch2_check_extents_to_backpointers()` | `fs/alloc/backpointers.c:1137` | 验证每个 extent 有合法的后向指针 |
| `bch2_check_backpointers_to_extents()` | `fs/alloc/backpointers.c:1318` | 验证每个后向指针对应有效 extent |
| `bch2_check_btree_backpointers()` | `fs/alloc/backpointers.c:415` | 验证 btree 节点的后向指针 |
| `bch2_check_btree_counts()` | `fs/fs/check_btree_counts.c` | 验证 btree 内部计数 |

### 2.3 Btree 拓扑检查

| 函数 | 位置 | 作用 |
|------|------|------|
| `bch2_check_btree_root()` | `fs/btree/check.c` | 验证 btree 根节点合法性 |
| `bch2_check_btree_keys()` | `fs/btree/check.c` | 验证 btree 键值范围和排序 |

### 2.4 快照检查

| 函数 | 位置 | 作用 |
|------|------|------|
| `bch2_check_snapshots()` | `fs/snapshots/check_snapshots.c:429` | 验证快照树一致性 |
| `bch2_check_snapshot_trees()` | `fs/snapshots/check_snapshots.c:183` | 验证快照树根节点和指针 |
| `bch2_check_subvols()` | `fs/snapshots/subvolume.c` | 验证子卷条目 |
| `bch2_check_key_has_snapshot()` | `fs/snapshots/snapshot.h:286` | 验证键的快照 ID 有效 |

### 2.5 EC 验证

| 函数 | 文件:行 | 作用 |
|------|---------|------|
| `bch2_stripe_validate()` | `fs/data/ec/trigger.c:45` | 验证 EC stripe 条目的数据布局和状态一致性 |

---

## 3. 错误检测基础设施

### 3.1 三层错误分类

```c
// fs/init/error_types.h
enum bch_on_error {
    BCH_ON_ERROR_continue,      // 记录并继续
    BCH_ON_ERROR_fix_safe,      // 自动修复（安全时）
    BCH_ON_ERROR_ro,            // 切换到只读
    BCH_ON_ERROR_panic,         // 内核 panic
};
```

行为由全局挂载选项 `c->opts.errors` 控制。

### 3.2 错误报告函数族

```c
// fs/init/error.h
int bch2_fsck_err(struct btree_trans *, enum bch_sb_error_id error_id,
                  const char *, ...);

int bch2_inconsistent_error(struct bch_fs *, const char *, ...);
//     → __bch2_inconsistent_error() [error.c]
//     → 设置 BCH_FS_ERROR 标志
//     → 根据 opts.errors 决定动作
//     → 返回标准 errno（-BCH_ERR_fsck_continue_to_fix、
//       -BCH_ERR_fsck_continue、-BCH_ERR_fsck_errors_unspecified）
//     → 自动修复的情况返回 0

bch2_fs_fatal_error_on(c, cond, ...)  // 条件触发的致命错误
bch2_fs_fatal_error(c, ...)           // 无条件致命错误
bch2_fs_inconsistent(c, ...)          // 不一致错误
```

### 3.3 `fsck_err` 宏家族

```c
// fs/init/error.h

// 标准 fsck 错误
#define fsck_err(trans, error_id, fmt, ...) ({                      \
    _fsck_err(trans, error_id, fmt, ##__VA_ARGS__);                 \
})

// bkey 级别的 fsck 错误（自动包含 key 的字符串表示）
#define bkey_fsck_err(trans, alloc_k, error_id, fmt, ...) ...

// 带条件的 fsck 错误
#define fsck_err_on(cond, trans, alloc_k, error_id, fmt, ...) ...

// 不自动修复的 fsck 错误
#define need_fsck_err(trans, error_id, fmt, ...) ...
```

所有这些宏最终调用 `__bch2_fsck_err()`（`fs/init/error.c`）。

### 3.4 错误 ID 系统

所有可识别的 fsck 错误 ID 定义在 `BCH_SB_ERRS()` x-macro 中：

```c
// fs/init/errors_format.h（约 364 个错误 ID）
#define BCH_SB_ERRS()                           \
    x(backpointer_to_bad_bp,                    \
      BACKPOINTER_BAD_BP)                       \
    x(backpointer_to_bad_offset,                \
      BACKPOINTER_BAD_OFFSET)                   \
    x(backpointer_to_overwritten,               \
      BACKPOINTER_OVERWRITTEN)                  \
    x(backpointer_to_missing,                   \
      BACKPOINTER_MISSING)                      \
    // ... 约 360+ 个错误 ID
```

每个错误 ID 关联一个 `BCH_SB_ERR_*` 常数（自动转换），后续版本用于统计和选择性修复。

### 3.5 错误计数和持久化

```c
// fs/init/error_types.h:struct bch_fs_errors
struct bch_fs_errors {
    struct bch_sb_errors_cpu counts;     // 所有错误 ID 的计数器数组
    unsigned nr_counters;
};

// 错误计数存储在 `c->sb_errors` 中
// 挂载时从超级块的 BCH_SB_FIELD_errors 字段恢复
// 卸载时写回超级块 → 跨挂载持久化
```

---

## 4. Fsck 错误框架

### 4.1 `__bch2_fsck_err()` 完整决策树

```c
// fs/init/error.c
int __bch2_fsck_err(struct btree_trans *trans,
                    struct bch_fs *c,
                    struct bkey_s_c *alloc_k,   // 可选，问题 key
                    enum bch_sb_error_id error_id,
                    enum bch_sb_error_id *auto_id,
                    const char *fmt, ...)
{
    1. 错误 ID 合法性检查
    2. 速率限制
    3. 错误记录（bch2_sb_error_count()）
    4. FSCK_AUTOFIX 检查
    5. opts.errors 决策：
       - continue：计数，可能设 BCH_FS_ERROR
       - fix_safe：根据 error_id 自动修复
       - ro：切换到只读
       - panic：立即 panic
    6. 返回：
       - 0：错误已处理
       - -BCH_ERR_fsck_continue_to_fix：需要事务上下文修复
       - -BCH_ERR_fsck_continue：已记录，继续扫描
       - -BCH_ERR_fsck_errors_unspecified：未知错误码
       - -BCH_ERR_erofs_no_transactions：切换只读
}
```

### 4.2 速率限制

```c
// fs/init/error.c
#define FSCK_ERR_RATELIMIT_NR  10   // 每个错误 ID 最多打印 10 次
#define FSCK_ERR_RATELIMIT_MS  5000 // 5 秒后重置计数

struct fsck_err_state {
    int printed;         // 已打印次数
    unsigned long last;  // 上次打印时间（jiffies）
};
```

通过 `printk_ratelimited` + 自定义每错误 ID 计数实现。

### 4.3 `FSCK_AUTOFIX` 机制

```c
// fs/init/error.c
// 如果 opts.errors == fix_safe 且 error_id 被标记为可自动修复
// → 自动执行修复，不询问用户

// 可自动修复的条件：
// 1. 修复结果是确定性的
// 2. 修复不会导致数据丢失（丢数据的不自动修复）
// 3. 修复不会影响不可逆的元数据（超级块等）
```

### 4.4 三类错误来源

```
1.  fsck_err() → __bch2_fsck_err()
    文件系统逻辑错误 — 调用 fsck_err 宏

2.  __bch2_inconsistent_error()
    元数据不一致 — 设置 BCH_FS_ERROR 标志

3.  bch2_fs_fatal_error()
    致命错误 — 无条件切换只读模式
```

### 4.5 错误升级路径

```
fsck_err (可修复)
   ↓ 如果修复失败或无法修复
inconsistent_error (可能切换只读)
   ↓ 如果自动修复无效或需要人工干预  
fatal_error (强制只读)
```

---

## 5. 自愈机制

bcachefs 在正常操作（非 fsck）中有多层自愈机制，不需要显式 fsck 扫描。

### 5.1 读取路径自愈

**位置：** `fs/btree/read.c` + `fs/io/read.c`

读取路径修复主要体现在三个层面：

| 层次 | 机制 | 代码位置 |
|------|------|----------|
| btree 节点 | 节点读取时校验 checksum，错误则 `bch2_btree_node_read_retry()` 从副本读取 | `fs/btree/read.c` |
| extent 数据 | 读取错误触发 `bch2_rbio_retry()` 从其他副本读取 | `fs/io/read.c` |
| 磁盘映射 | 读取 IO 错误触发 `bch2_rbio_error()`，发现 `ptr_stale()` 则更新 alloc btree 中的 stale 指针计数 | `fs/io/read.c` |

### 5.2 Btree 触发器自愈

**位置：** `fs/alloc/background.c`

```c
// bch2_trigger_alloc() 在每次 alloc 键更新时：
// 1. 检查 data_type 和 sector 计数的一致性
// 2. 如果 data_type == free 但 dirty_sectors > 0 → 自动修正
// 3. 如果 old_a->data_type != BCH_DATA_free 且 new_a->data_type == BCH_DATA_free
//    → 自动更新 freespace btree
```

### 5.3 GC 期间的 `check_repair`

**位置：** `fs/btree/check.c`

GC 标记阶段（`bch2_gc_mark_key()`）使用 `BTREE_TRIGGER_check_repair` 标志：
- 检查每个键的引用计数和数据类型一致性
- 发现不一致时自动修正 alloc btree
- 相当于 fsck 检查的轻量级在线版本

### 5.4 后向指针自愈

**位置：** `fs/alloc/backpointers.c`

```
bch2_trigger_backpointer() → 更新 extent 时触发
  └─ 检查后向指针合法性
  └─ 如果不合法：
       ├─ 后向指针指向不存在的 extent → 删除后向指针
       └─ extent 缺少后向指针 → 写入新后向指针
```

### 5.5 Reconcile 系统

**位置：** `fs/data/reconcile.c`

Reconcile 系统负责修复磁盘使用计数和存储池使用计数之间的不一致：
- 在挂载时运行，比较 `disk_accounting` btree 和 `reconcile` btree
- 发现差异则通过 `bch2_reconcile_difference()` 修正
- 修复点：分配计数、data_type 使用量、压缩比差异

### 5.6 自动修复的触发条件

自愈仅在以下条件同时满足时自动执行：
1. `c->opts.errors == BCH_ON_ERROR_fix_safe`
2. 错误是可确定修复的（error_id 在可修复白名单中）
3. 修复不修改键的范围（不导致数据丢失）
4. 当前不在 fsck 显式修复阶段（避免双重修复冲突）

### 5.7 拓扑验证自愈

**位置：** `fs/btree/check.c`

```
bch2_check_btree_root() → 检查 btree 根节点合法性
  └─ 如果根节点已损坏：
       ├─ 从 node_scan.c 扫描到的节点集合中选择替代
       └─ 更新根节点指针
```

### 5.8 IO 错误降级

**位置：** `fs/io/read.c`

读取 IO 错误触发自动降级：
1. `bch2_rbio_error()` 记录错误
2. 如果 `ptr_stale()` 检测到指针已过时 → 更新 alloc btree
3. 如果有其他副本 → `bch2_rbio_retry()` 从其他副本读取
4. 如果没有其他副本 → 返回 IO 错误，文件系统可能进入只读

### 5.9 自愈层次总结

```
┌─────────────────────────────────────────────────────┐
│                   自愈层次结构                        │
├─────────────────────────────────────────────────────┤
│ L10: 读取路径 retry（从副本重新读取）                    │
│ L9:  Btree 触发器自动修正 alloc/freespace             │
│ L8:  GC check_repair 在线一致性检查                     │
│ L7:  后向指针自动修复（trigger 中）                     │
│ L6:  Reconcile 系统修复使用计数                        │
│ L5:  拓扑验证自愈（根节点损坏恢复）                      │
│ L4:  IO 错误降级和副本切换                             │
│ L3:  错误计数和只读切换（阻止传播）                      │
│ L2:  速率限制和日志记录（避免日志泛滥）                   │
│ L1:  Fsck 路径的自动修复模式（FSCK_AUTOFIX）              │
└─────────────────────────────────────────────────────┘
```

---

## 6. 特定错误类型恢复

### 6.1 后向指针不匹配

后向指针系统有三向交叉验证：

```
extent btree  ←→  backpointer btree  ←→  alloc btree
```

修复通道：

| 通道 | 检测 | 修复 |
|------|------|------|
| `check_extents_to_backpointers` | extent 被引用但无对应后向指针 | 写入后向指针 |
| `check_backpointers_to_extents` | 后向指针指向不存在的 extent | 删除后向指针 |
| `check_btree_backpointers` | btree 节点的后向指针不匹配 | 修正后向指针或丢弃失效设备 |

**`bch2_check_extents_to_backpointers()`**（`fs/alloc/backpointers.c:1137`）：
- 遍历 extent btree，对每个 extent 检查对应后向指针是否存在
- 如果缺失且未报告错误 → 写入后向指针

**`bch2_check_backpointers_to_extents()`**（`fs/alloc/backpointers.c:1318`）：
- 遍历 backpointer btree，检查每个后向指针的 `k.p` 是否指向有效的 alloc key
- 如果 `data_type == free` 或 bucket 偏移超出范围 → 删除后向指针
- 验证 `bpos_offset` 与 extent 桶偏移一致

**`bch2_check_btree_backpointers()`**（`fs/alloc/backpointers.c:415`）：
- 遍历 btree 节点，为每个节点的桶生成后向指针并与 backpointer btree 对比
- 对于后向指针引用计数为 0 的桶 → 通过写入引用计数 1 来修复
- 不存在后向指针 → 写入后向指针

### 6.2 分配/记账不匹配

**`bch2_check_alloc_to_lru_refs()`**（`fs/alloc/check.c:737`）：
- 遍历 LRU btree 检查每个条目的 alloc 键
- 如果 `data_type != cached` 或 `io_time` 不匹配 → 删除 LRU 条目

**`bch2_check_alloc_holes_freespace()`**（`fs/alloc/check.c`）：
- 检查 freespace btree 与 alloc btree 的一致性
- 如果 `data_type == free` 但 freespace btree 缺失对应条目 → 插入
- 如果 `data_type != free` 但 freespace btree 中存在 → 删除

### 6.3 重叠 Extent 检测

**`bch2_check_extents()`**（`fs/fs/check_extents.c`）：
- 在 extent btree 中检测重叠 extent
- 重叠修复：如果新 extent 完全覆盖旧 extent → 删除旧 extent
- 部分重叠 → 裁剪重叠区域
- 使用 `bch2_extent_trim()` 和 `bch2_extent_drop_overlapping()` 辅助函数

### 6.4 EC Stripe 修复

**`bch2_stripe_validate()`**（`fs/data/ec/trigger.c:45`）：
- 验证 EC stripe 布局：stripe 块数、数据块数布局
- 校验和验证：stripe 的有效性
- 检查每个 block 的 `has_data` 标志与实际数据的一致性

EC 修复路径：
```
bch2_ec_validate_stripe()
  └─ 如果 stripe 损坏或校验和不匹配
       ├─ 标记 stripe 为失败（BCH_EC_STRIPE_NEED_REWRITE）
       └─ 触发 copygc 搬移数据并重建 stripe
```

`dev_ptr_stale_rcu()` 检查旧设备指针：
- 在读取路径和 GC 路径中皆可触发
- 识别过期指针并更新 alloc btree 中的 stale 计数器
- 在 `bucket_data_type_mismatch()` 发生时强制从新副本读取

### 6.5 Reconcile 修复

Reconcile 系统（`fs/data/reconcile.c`）比较两个来源的计数：
- 源 1：`disk_accounting` btree（每次 IO 操作触发 trigger 写入）
- 源 2：`reconcile` btree（块分配器离线扫描产生）

修复通过 `bch2_reconcile_go()` 执行：
1. 生成差异报告
2. 对每个不一致的计数器：
   - 如果差异小于阈值 → 使用 btree 作为真相修正 reconcile
   - 如果差异超过阈值 → 重新扫描该区域（可能并发 IO 导致）

---

## 7. Btree 节点修复

### 7.1 读取时验证

**位置：** `fs/btree/read.c`

```c
int bch2_btree_node_read(struct bch_fs *c, struct btree *b)
{
    // 1. 发送 IO 请求读取节点
    // 2. 收到后验证：
    //    a. btree node header 的 checksum
    //    b. bset buffer 校验和
    //    c. btree node format 合法性
    //    d. bset 内 key 值的排序
    // 3. 如果验证失败：
    //    → bch2_btree_node_read_retry(b)
}
```

### 7.2 读取重试

```c
// fs/btree/read.c
void bch2_btree_node_read_retry(struct btree *b)
{
    // 1. 如果已重试过（b->read_retries > 0）
    //    → bch2_btree_marked_bad(b) 标记节点损坏
    // 2. 否则 b->read_retries++，重新读取
    // 3. 如果有其他副本（mirror）→ 从其他 mirror 读取
    // 4. 所有 mirror 都失败 → bch2_btree_set_corrupt(b)
}
```

### 7.3 丢失数据恢复

```c
// fs/btree/read.c
static int btree_node_read_work(struct bch_fs *c, struct btree *b,
                                struct btree_read_bio *rb)
{
    // 如果读取成功但格式错误：
    // → 尝试 bch2_btree_node_read_done() 修复
    // 如果所有重试失败：
    // → 调用 bch2_btree_set_corrupt(b)
}

void bch2_btree_set_corrupt(struct btree *b)
{
    // 标记节点为 BCH_BTREE_NODE_IO_ERROR
    // 设置 bch2_btree_node_set_not_accessible(b)
    // 尝试 bch2_btree_node_rewrite 替换节点
}
```

### 7.4 Btree 节点扫描

**位置：** `fs/btree/node_scan.c`

```c
// 通过 magic number 在磁盘上扫描 btree 节点
int bch2_scan_btree_nodes(struct bch_fs *c, struct bch_dev *ca,
                          btree_node_scan_fn fn, void *data)
{
    // 1. 逐桶读取数据
    // 2. 检查每个扇区是否包含 btree node magic
    // 3. 对每个匹配 → 验证格式 → 调用 fn 回调
    // 用于：
    //   - bch2_check_btree_root() 寻找替代根节点
    //   - 恢复 btree 拓扑（当某层所有节点丢失时）
}
```

### 7.5 拓扑修复

**位置：** `fs/btree/check.c` + `fs/btree/node_scan.c`

```
bch2_check_btree_root()
  └─ 验证每层 btree 拓扑
       ├─ 如果层缺少节点 → bch2_scan_btree_nodes()
       ├─ 如果找到多个同层节点 → 验证并选择最佳
       ├─ 如果根节点损坏 → 在子节点中选择替代
       └─ 如果无法恢复 → 将 btree 标记为丢失数据
```

### 7.6 异步重写

```c
// fs/btree/read.c
// 当 btree 节点成功读取但内容格式轻微异常时：
// → 触发 bch2_btree_node_rewrite_async(c, b)
// → 通过 workqueue 重新写入节点
// 这不阻塞读取路径
```

### 7.7 Btree 节点读取验证层次

```
读取验证层次（7 层设计）：
┌──────────────────────────────────────────────┐
│ L7: 格式验证（header magic, version, levels）  │
│ L6: Checksum 验证（node header + bset）        │
│ L5: 键排序验证（bset 内 key 的顺序）            │
│ L4: 键范围验证（key 应在子树范围内）             │
│ L3: 重试机制（原副本身份 + 其他 mirror）         │
│ L2: 节点扫描恢复（通过 magic 扫描磁盘）          │
│ L1: 异步重写（轻微格式问题的透明修复）            │
└──────────────────────────────────────────────┘
```

---

## 8. 日志重放 Fsck

### 8.1 日志读取管道

**位置：** `fs/journal/read.c`

```
bch2_journal_replay(c)                           [fs/init/recovery.c]
  └─ bch2_journal_read(c)                        [fs/journal/read.c]
       ├─ bch2_journal_read_bucket()              // 逐桶读取 journal
       │   ├─ 读取 journal 区域至内存 buffer
       │   ├─ journal_entry_validate()            // 验证每个 journal entry
       │   └─ journal_entry_err_on()              // 发现错误
       │
       ├─ journal_validate_key()                  // 验证 journal entry 中的 key
       │
       ├─ bch2_journal_check_for_missing()        // 检查 journal 序列断裂
       │
       └─ 排序并重放 journal entries
```

### 8.2 Journal Entry 验证

```c
// fs/journal/validate.c

// 每个 journal entry 类型的验证器通过 BCH_JSET_ENTRY_VALIDATORS() x-macro 注册

// 关键验证函数：
int bch2_journal_entry_validate(struct bch_fs *c,
    struct jset_entry *entry,
    enum bch_validate_flags flags,
    int read_entire_seq_lo,
    unsigned nonce)
{
    // 1. 检查 entry->type 是否合法
    // 2. 检查 entry->u64s > 0
    // 3. 根据类型调用对应的验证器：
    //    - journal_entry_btree_keys_validate    (BTREE_KEYS)
    //    - journal_entry_btree_root_validate    (BTREE_ROOT)
    //    - journal_entry_prio_ptrs_validate     (PRIO_PTRS)
    //    - journal_entry_data_usage_validate    (DATA_USAGE)
    //    - journal_entry_dev_usage_validate     (DEV_USAGE)
    //    - journal_entry_accounting_validate    (ACCOUNTING)
    //    - journal_entry_replay_usage_validate  (REPLAY_USAGE)
    //    - journal_entry_global_iter_validate   (GLOBAL_ITER)
    //    - journal_entry_write_buffer_keys_validate (WRITE_BUFFER_KEYS)
    //    - journal_entry_str_validate           (STR)
    //    - journal_entry_inlined_validate       (INLINED)
    // 4. 如果验证失败 → journal_entry_err_on() 记录错误
}
```

### 8.3 Journal Entry 错误检测

```c
// fs/journal/validate.c
#define journal_entry_err_on(cond, c, entry, ...) ({            \
    bool _cond = !!(cond);                                      \
    if (unlikely(_cond))                                        \
        __journal_entry_err(c, entry, __VA_ARGS__);            \
    _cond;                                                      \
})
```

`__journal_entry_err()`（`fs/journal/validate.c`）：
1. 打印 journal entry 详情
2. 计数错误
3. 根据 `c->opts.errors` 决定是否切换只读
4. 如果在 fsck 模式 → 停止重放并触发修复

### 8.4 Journal 相关错误 ID

```c
// fs/init/errors_format.h — journal 相关错误
x(journal_entry_bad,            JOURNAL_ENTRY_BAD)
x(journal_entry_bad_key,        JOURNAL_ENTRY_BAD_KEY)
x(journal_entry_duplicate,      JOURNAL_ENTRY_DUPLICATE)
x(journal_entry_out_of_order,   JOURNAL_ENTRY_OUT_OF_ORDER)
x(journal_seq_missing,          JOURNAL_SEQ_MISSING)
x(journal_entry_corrupt,        JOURNAL_ENTRY_CORRUPT)
x(journal_entry_overrun,        JOURNAL_ENTRY_OVERRUN)
x(journal_entry_bad_csum,       JOURNAL_ENTRY_BAD_CSUM)
```

### 8.5 重放执行

```c
// fs/init/recovery.c

// 重放阶段：
// 1. bch2_journal_read() 收集所有有效 journal entry
// 2. 验证 journal sequence number 连续性
// 3. 对 journal entry 按 sequence number 排序
// 4. 对每个 btree_key 类型的 entry → journal_keys_append()
// 5. journal replay 完成后 → journal_keys_compact()
// 6. 后续 fsck pass 遍历 journal_keys 时发现不一致
```

### 8.6 Two-Pass 重放

正常挂载时 journal replays 使用两遍策略：

```
第一遍（前向）：
  ├─ 重放所有 btree_keys 类型的 entry（btree 更新）
  └─ 收集所有其他类型 entry（记账等）
第二遍（后向）：
  ├─ 回滚在崩溃前未提交的部分
  └─ 验证 journal entry 完整性
Fallback（崩溃后）：
  └─ 如果 journal 不完整 → 回滚到最后完整序列点
```

---

## 9. 参考代码位置

### 恢复通道系统

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/init/passes_format.h` | — | `BCH_RECOVERY_PASSES()` x-macro（~40 个 pass ID） |
| `fs/init/passes.c` | — | 通道执行和 pass 标志处理 |
| `fs/init/recovery.c` | 662 | `bch2_fs_recovery()` — 主恢复入口 |
| `fs/init/fs.c` | — | `__bch2_fs_start()` — 挂载入口 |

### 错误框架

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/init/error.c` | 全部 | `__bch2_fsck_err()`、`__bch2_inconsistent_error()`、速率限制 |
| `fs/init/error.h` | 全部 | `fsck_err`、`fsck_err_on`、`bkey_fsck_err` 宏 |
| `fs/init/error_types.h` | 全部 | `enum bch_on_error`、`struct bch_fs_errors` |
| `fs/init/errors_format.h` | 全部 | `BCH_SB_ERRS()` x-macro（~364 个错误 ID） |

### 文件系统 Fsck

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/fs/check.c` | — | 主 fsck 入口 |
| `fs/fs/check.h` | — | fsck 函数声明 |
| `fs/fs/check_extents.c` | — | extent 检查+修复 |
| `fs/fs/check_indirect_extents.c` | — | 间接 extent 检查 |
| `fs/fs/check_inodes.c` | — | inode 检查 |
| `fs/fs/check_dirents.c` | — | 目录条目检查 |
| `fs/fs/check_xattrs.c` | — | 扩展属性检查 |
| `fs/fs/check_nlinks.c` | — | 链接计数检查 |
| `fs/fs/check_directory_structure.c` | — | 目录结构检查 |
| `fs/fs/check_paths.c` | — | 路径可达性检查 |
| `fs/fs/check_btree_counts.c` | — | btree 计数检查 |

### 分配 Fsck

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/alloc/check.c` | 141 | `bch2_check_alloc_key()` |
| `fs/alloc/check.c` | 631 | `bch2_check_alloc_info()` |
| `fs/alloc/check.c` | 737 | `bch2_check_alloc_to_lru_refs()` |
| `fs/alloc/lru.c` | 201 | `bch2_check_lrus()` |
| `fs/alloc/backpointers.c` | 415 | `bch2_check_btree_backpointers()` |
| `fs/alloc/backpointers.c` | 1137 | `bch2_check_extents_to_backpointers()` |
| `fs/alloc/backpointers.c` | 1318 | `bch2_check_backpointers_to_extents()` |

### Btree 拓扑

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/btree/check.c` | — | `bch2_check_btree_root()`、`bch2_gc_mark_key()` |
| `fs/btree/node_scan.c` | — | 通过 magic number 扫描 btree 节点 |
| `fs/btree/read.c` | — | `bch2_btree_node_read()`、`bch2_btree_node_read_retry()` |

### Journal

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/journal/read.c` | — | `bch2_journal_read()`、`bch2_journal_replay()` |
| `fs/journal/validate.c` | — | `journal_entry_err_on()`、`bch2_journal_entry_validate()`、全部类型验证器 |

### 自愈

| 文件 | 行号 | 内容 |
|------|------|------|
| `fs/data/reconcile.c` | — | reconcile 修复系统 |
| `fs/io/read.c` | — | `bch2_rbio_error()`、`bch2_rbio_retry()` |
| `fs/alloc/backpointers.c` | — | `bch2_trigger_backpointer()`（trigger 自愈） |
| `fs/alloc/background.c` | — | `bch2_trigger_alloc()`（trigger 自愈） |
| `fs/data/ec/trigger.c` | 45 | `bch2_stripe_validate()`（EC 验证） |
