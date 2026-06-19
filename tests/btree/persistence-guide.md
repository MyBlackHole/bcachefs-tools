# bcachefs btree 数据持久化原理

> 本文档结合 `persist.c` 示例，系统性地讲解 bcachefs btree 数据如何从内存结构转换为磁盘结构。
> 建议先编译运行 `persist.c`（`gcc -Wall -Wextra -o persist persist.c && ./persist`），再阅读本文。

---

## 目录

1. [概述](#1-概述)
2. [核心数据结构](#2-核心数据结构)
3. [键的打包与解包](#3-键的打包与解包)
4. [bset 与 btree_node 布局](#4-bset-与-btreenode-布局)
5. [写入路径详解](#5-写入路径详解)
6. [读取路径详解](#6-读取路径详解)
7. [日志结构的节点设计](#7-日志结构的节点设计)
8. [压缩效率分析](#8-压缩效率分析)
9. [与真实代码的关联](#9-与真实代码的关联)
10. [参考资料](#10-参考资料)

---

## 1. 概述

bcachefs 的 btree 是一个 **B+ 树** 的实现，其数据持久化有以下关键设计特点：

| 特点 | 说明 |
|------|------|
| **日志结构** | btree 节点采用日志结构追加写入，而非原地覆写 |
| **键压缩** | 每个节点使用独立的打包格式 (`bkey_format`)，大幅减小键的存储空间 |
| **bset 机制** | 每个节点包含多个排序键集合（bset），支持增量更新 |
| **写时复制** | 节点分裂使用 COW（Copy on Write）语义 |
| **校验和** | 每个节点和 bset 都带校验和，保证数据完整性 |
| **日志集成** | 每个 bset 记录 journal_seq，崩溃时通过日志重放恢复 |

### 持久化全链路

```
内存键 (struct bkey)                 磁盘 (struct btree_node)
┌──────────────────────┐           ┌──────────────────────────────┐
│ inode=100, offset=16  │           │ ┌─ btree_node 头部 ─────────┐ │
│ size=8, type=extent   │           │ │ csum | magic | flags     │ │
│ snapshot=1            │           │ │ min_key | max_key        │ │
└──────┬───────────────┘           │ │ format (bkey_format)     │ │
       │                           │ └──────────────────────────┘ │
       │ bkey_pack()               │ ┌─ bset ────────────────────┐ │
       ▼                           │ │ seq | journal_seq        │ │
┌──────────────────────┐           │ │ u64s = 3                 │ │
│ bkey_packed (打包)     │           │ ├─ bkey_packed[0] ─────────┤ │
│ format=0, u64s=3      │           │ │ u64s=2 | type=extent    │ │
│ [打包的位字段]          │           │ │ <打包的位数据>           │ │
└──────┬───────────────┘           │ ├─ bkey_packed[1] ─────────┤ │
       │                           │ │ ...                      │ │
       │ bset_add_key()            │ └──────────────────────────┘ │
       ▼                           └──────────────────────────────┘
┌──────────────────────┐
│ bset                 │
│ ┌─ bkey_packed[0] ─┐│
│ │ ...               ││
│ ├─ bkey_packed[1] ─┤│
│ │ ...               ││
│ └───────────────────┘│
└──────────────────────┘
```

---

## 2. 核心数据结构

### 2.1 bpos — 键位置

`struct bpos` 是 bcachefs 中键的定位信息，由三个字段构成一个 128 位的可比较大整数：

```
 63              0  63              0  31              0
├─────────────────┼─────────────────┼────────────────┤
│    inode (u64)   │   offset (u64)  │ snapshot (u32) │
├─────────────────┴─────────────────┴────────────────┤
│                 比较方向: 大端序                      │
└────────────────────────────────────────────────────┘
```

- **inode**：文件系统 inode 号（高位）
- **offset**：文件偏移或扇区号（中位）
- **snapshot**：快照 ID（低位），使 bcachefs 支持快照感知的查找

### 2.2 bkey — 未打包键（内存格式）

`struct bkey` 是 btree 键在内存中的完整表示，占用约 32 字节：

| 字段 | 大小 | 说明 |
|------|------|------|
| `u64s` | 1B | 键 + 值的总大小（以 u64 为单位） |
| `format` | 7bit | 0=本地打包格式，1=未打包格式 |
| `needs_whiteout` | 1bit | 是否需要 whiteout 标记 |
| `type` | 1B | 值类型（extent, inode, xattr, dirent 等） |
| `version` | 8B | 版本戳，用于并发控制 |
| `size` | 4B | extent 覆盖的扇区数（非 extent 则为 0） |
| `p` (bpos) | 20B | 键位置（inode + offset + snapshot） |

### 2.3 bkey_packed — 打包键（磁盘格式）

`struct bkey_packed` 是键在磁盘上的紧凑格式。前 3 字节与 `bkey` 共享相同布局，之后是按 `bkey_format` 编码的位字段：

```
┌──────┬────────┬──────┬─────────────────────────────┐
│ u64s │fmt+wn  │ type │    打包的位字段（变长）      │
│ 1B   │  1B    │  1B  │   由 bkey_format 描述       │
├──────┴────────┴──────┴─────────────────────────────┤
│                  bkey 前 3 字节与未打包版本相同      │
└────────────────────────────────────────────────────┘
```

### 2.4 bkey_format — 打包格式

每个 btree 节点拥有独立的 `bkey_format`，定义键的 6 个字段如何压缩：

```c
struct bkey_format {
    uint8_t  key_u64s;        // 打包后每个键占多少 u64
    uint8_t  nr_fields;       // 始终 = 6
    uint8_t  bits_per_field[6]; // 每个字段分配多少位
    uint64_t field_offset[6];   // 每个字段的基线偏移量
};
```

**压缩原理**：`packed_value = (original_value - field_offset)`，用 `bits_per_field` 位存储。如果某个字段在所有键中相同（range=0），可以省略（bits=0）。

以 `persist.c` 的示例为例：

```
原始 inode: 0~300 → range=300 → 需要 9 位（而非 64 位）
原始 offset: 0~32 → range=32  → 需要 6 位（而非 64 位）
原始 snapshot: 1~2 → range=1  → 需要 1 位（而非 32 位）

打包前: 32 字节   打包后: ~4 字节   压缩率: ~87%
```

---

## 3. 键的打包与解包

### 3.1 打包算法 (`bkey_pack`)

打包过程由 `pack_state` 状态机驱动，逐位将字段写入输出缓冲区：

```
输入: struct bkey (32 字节)
     inode=100, offset=16, snapshot=1, size=8, ...
                       │
                       ▼
    1. 复制头部: u64s, format=0, type
    2. 逐字段处理:
       for each field f:
         raw = bkey_field_val(k, f)         // 提取字段值
         packed = raw - field_offset[f]      // 减去偏移量
         write_bits(packed, bits_per_field[f]) // 用限定位数写入
                       │
                       ▼
输出: struct bkey_packed (key_u64s u64s)
     [u64s=3][format=0][type=2][打包的位数据...]
```

### 3.2 解包算法 (`bkey_unpack`)

解包是打包的逆过程：

```
输入: struct bkey_packed
     [u64s=3][format=0][type=2][打包的位数据...]
                       │
                       ▼
    1. 复制头部: u64s, format=1, type
    2. 逐字段读取:
       for each field f:
         packed = read_bits(bits_per_field[f]) // 读取限定位数
         raw = packed + field_offset[f]        // 加上偏移量
         bkey_set_field_val(k, f, raw)         // 设置字段值
                       │
                       ▼
输出: struct bkey (32 字节)
     inode=100, offset=16, snapshot=1, size=8, ...
```

### 3.3 跨 u64 边界的位操作

打包和解包的核心难点在于处理跨 64 位边界的读写。`pack_bits` 和 `unpack_bits` 函数通过以下方式处理：

```
位偏移: 0           64          128         192
        ├───────────┼───────────┼───────────┤
字段 A     字段 B        字段 C
  (20位)    (60位)        (30位)
           ^^^^^^ 跨越边界!

处理方式:
  字段 B = buf[1]的低60位
  但 buf[1] 只有 64-20=44 位剩余
  → 低44位在 buf[1], 高16位在 buf[2]
  → 需要从两个 u64 中拼接
```

---

## 4. bset 与 btree_node 布局

### 4.1 btree_node 的磁盘布局

一个 btree 节点在磁盘上的布局如下（`persist.c` 中的 `btree_node_write` 函数演示了此过程）：

```
┌─────────────────────────────────────────────────────┐
│ btree_node 头部 (固定部分)                            │
│  ├─ csum       (16B) — 校验和                        │
│  ├─ magic      (8B)  — 魔数 (0x90135c78b99e07f5)    │
│  ├─ flags      (8B)  — btree_id, level, seq 等      │
│  ├─ min_key    (20B) — 节点覆盖的最小键（闭区间）     │
│  ├─ max_key    (20B) — 节点覆盖的最大键（闭区间）     │
│  ├─ _ptr       (8B)  — extent 指针（不再使用）       │
│  └─ format     (10B) — bkey_format 描述              │
├─────────────────────────────────────────────────────┤
│ bset 头部（内联在 btree_node 中）                     │
│  ├─ seq        (8B)   — bset 序列号                  │
│  ├─ journal_seq (8B)  — 日志序列号                   │
│  ├─ flags      (4B)   — 校验和类型/字节序/偏移量     │
│  ├─ version    (2B)   — 元数据版本                   │
│  └─ u64s       (2B)   — 后续键数据的总 u64 数        │
├─────────────────────────────────────────────────────┤
│ bkey_packed[] 数组（变长）                             │
│  ├─ bkey_packed[0]: [u64s=3][format=0][type][位数据] │
│  ├─ bkey_packed[1]: 紧跟在 [0] 之后                  │
│  ├─ ...                                              │
│  └─ bkey_packed[N-1]                                 │
├─────────────────────────────────────────────────────┤
│ 空闲空间（可被后续 btree_node_entry 写入）             │
└─────────────────────────────────────────────────────┘
```

### 4.2 键在 bset 中的排列

bset 中的 bkey_packed 是**紧密排列**的，通过每个键的 `u64s` 字段定位下一个键：

```c
// 遍历 bset 中的所有键
struct bkey_packed *k = bs->start;
while ((uint64_t *)k < (uint64_t *)bs->start + bs->u64s) {
    // 处理 k
    k = (struct bkey_packed *)((uint64_t *)k + k->u64s);
}
```

键在 bset 中是**排序**的（按 bpos 比较）。读取时通过辅助搜索树加速查找。

---

## 5. 写入路径详解

btree 节点写入路径的完整流程（对应 `persist.c` 的 `btree_node_write` 函数）：

```
写入触发（节点变脏）
       │
       ▼
┌─────────────────────────────┐
│ 1. 格式分析                  │
│    - 扫描所有键的最小/最大值  │
│    - 调用 bch2_bkey_format   │
│    - 计算 bits_per_field[]   │
│    - 计算 key_u64s           │
└─────────────────────────────┘
       │
       ▼
┌─────────────────────────────┐
│ 2. 键打包                    │
│    - 为每个键调用 bkey_pack  │
│    - 从 struct bkey →        │
│      struct bkey_packed      │
│    - 键按 inode/offset/       │
│      snapshot 排序            │
└─────────────────────────────┘
       │
       ▼
┌─────────────────────────────┐
│ 3. 填充头部                  │
│    - btree_node 头部         │
│      (magic, flags, range)   │
│    - bset 头部               │
│      (seq, journal_seq, u64s)│
└─────────────────────────────┘
       │
       ▼
┌─────────────────────────────┐
│ 4. 布局数据                  │
│    - 打包键复制到 bset 数据区 │
│    - 计算总写入大小           │
└─────────────────────────────┘
       │
       ▼
┌─────────────────────────────┐
│ 5. 校验和                    │
│    - 对整个 btree_node 计算  │
│      csum_vstruct            │
│    - 首字段存储校验和         │
└─────────────────────────────┘
       │
       ▼
┌─────────────────────────────┐
│ 6. I/O 提交                  │
│    - 分配 bio                │
│    - 通过 bch2_submit_wbio   │
│    - 写入复制目标             │
│    - 等待完成                 │
└─────────────────────────────┘
       │
       ▼
┌─────────────────────────────┐
│ 7. 写入后清理                │
│    - 合并 bset (排序 + 去重) │
│    - 构建辅助搜索树           │
│    - 初始化下一个 bset        │
└─────────────────────────────┘
```

真实代码中的对应函数：

| 步骤 | 真实函数 | 文件 |
|------|---------|------|
| 1. 格式分析 | `bch2_bkey_format_init()` + `bch2_bkey_format_done()` | `fs/btree/bkey.c` |
| 2. 键打包 | `bch2_bkey_pack_key()` | `fs/btree/bkey.h` |
| 3. 键排序 | `bch2_sort_keys_keep_unwritten_whiteouts()` | `fs/btree/sort.c` |
| 4. 填充头部 | `__bch2_btree_node_write()` | `fs/btree/write.c` |
| 5. 校验和 | `csum_vstruct()` | `fs/data/checksum.h` |
| 6. I/O | `bch2_submit_wbio_replicas()` | `fs/data/io.c` |
| 7. 清理 | `bch2_btree_post_write_cleanup()` | `fs/btree/write.c` |

---

## 6. 读取路径详解

btree 节点读取路径的完整流程（对应 `persist.c` 的 `btree_node_read_and_verify` 函数）：

```
读取请求
       │
       ▼
┌─────────────────────────────┐
│ 1. I/O 提交                  │
│    - 选择最优设备            │
│    - 分配 bio               │
│    - 读取完整节点缓冲区       │
│    - 等待 IO 完成             │
└─────────────────────────────┘
       │
       ▼
┌─────────────────────────────┐
│ 2. 验证魔数                  │
│    - 检查 magic ==           │
│      sb_uuid ^ BSET_MAGIC    │
│    - 失败 → 无效节点          │
└─────────────────────────────┘
       │
       ▼
┌─────────────────────────────┐
│ 3. 校验和验证                │
│    - 重新计算 csum           │
│    - 与存储的 csum 比较      │
│    - 失败 → 报告错误         │
└─────────────────────────────┘
       │
       ▼
┌─────────────────────────────┐
│ 4. 解密（如果加密）          │
│    - bset_encrypt()          │
│    - 使用 btree 随机数解密   │
└─────────────────────────────┘
       │
       ▼
┌─────────────────────────────┐
│ 5. 遍历所有 bset             │
│    - 第一个: btree_node.keys │
│    - 后续: btree_node_entry  │
│    - 验证每个 bset 头部       │
│    - 检查 journal_seq         │
│    - 黑名单中的 bset 跳过    │
└─────────────────────────────┘
       │
       ▼
┌─────────────────────────────┐
│ 6. 合并 bset                 │
│    - bch2_key_sort_fix_     │
│      overlapping()           │
│    - 将所有有效 bset 合并    │
│    - 处理重叠键（去重）       │
└─────────────────────────────┘
       │
       ▼
┌─────────────────────────────┐
│ 7. 构建辅助搜索树            │
│    - bch2_bset_build_aux_   │
│      tree()                  │
│    - 加速后续查找             │
└─────────────────────────────┘
       │
       ▼
┌─────────────────────────────┐
│ 8. 键验证                    │
│    - 验证每个 bkey 的有效性   │
│    - 检查键范围是否在节点内   │
│    - 标记无效键               │
└─────────────────────────────┘
```

真实代码中的对应函数：

| 步骤 | 真实函数 | 文件 |
|------|---------|------|
| 1. I/O | `bch2_btree_node_read()` | `fs/btree/read.c` |
| 2-4. 验证 | `bch2_validate_bset()` | `fs/btree/read.c` |
| 5. 遍历 | `bch2_btree_node_read_done()` | `fs/btree/read.c` |
| 6. 合并 | `bch2_key_sort_fix_overlapping()` | `fs/btree/sort.c` |
| 7. 搜索树 | `bch2_bset_build_aux_tree()` | `fs/btree/bset.c` |
| 8. 键验证 | `bch2_validate_bset_keys()` | `fs/btree/read.c` |

---

## 7. 日志结构的节点设计

### 7.1 为什么使用日志结构

传统的 B+ 树更新一个键需要：

1. 读取整个 btree 节点（~256KB）
2. 在内存中修改键
3. 重写整个节点

日志结构的设计允许：

1. **增量写入**：只写入变更的键，而非整个节点
2. **避免读-改-写**：新写入直接追加，无需先读旧数据
3. **崩溃安全**：通过 journal_seq 实现原子性

### 7.2 多 bset 管理

内存中的 `struct btree` 维护最多 3 个 bset：

```
第一次写入 →  btree_node + bset[0]  (写入磁盘)
内存更新   →  set[1] (未写入的增量 bset)
             set[2] (set[1] 溢出, >4KB 时启用)

再次写入 →  btree_node_entry + bset[1]  (追加写入磁盘)
```

读取时，多个 bset 通过 `bch2_key_sort_fix_overlapping()` 合并为单个有序集合。

### 7.3 Journal 集成

每个 bset 记录 `journal_seq`（该 bset 对应的最高日志序列号）。崩溃恢复时：

```
1. 读取所有 btree 节点
2. 对每个节点，遍历所有 bset
3. 检查 bset 的 journal_seq
4. 如果 journal_seq 不在日志中 → bset 被忽略
5. 如果 journal_seq 在日志中 → bset 有效，参与合并
```

这保证了**写入的顺序性**：如果一次写入没有成功记录到日志，它的数据也不会被误用。

---

## 8. 压缩效率分析

`persist.c` 的第 6 步通过两个场景对比展示了打包压缩的效果：

### 场景 A：密集数据

```
同一 inode (10)，连续 offset (0~24)，同一 snapshot (1)
→ inode 范围: 0，offset 范围: 24，snapshot 范围: 0
→ inode 可省略 (bits=0)，offset 需 5 位，snapshot 可省略
→ 打包键大小: 约 1-2 u64s
→ 压缩率: >90%
```

### 场景 B：稀疏数据

```
inode 范围: 1000~1000000，offset 范围: 0~999999
→ inode 需 20 位，offset 需 20 位，snapshot 需 2 位
→ 打包键大小: 约 4-5 u64s
→ 压缩率: ~75%
```

### 影响压缩率的因素

| 因素 | 数据密集时 | 数据稀疏时 |
|------|-----------|-----------|
| inode 范围 | 小 → 省比特 | 大 → 费比特 |
| offset 范围 | 小 → 省比特 | 大 → 费比特 |
| snapshot 版本数 | 少 → 省比特 | 多 → 费比特 |
| size/version 变化 | 少 → 省比特 | 多 → 费比特 |
| 键数量 | 多 → 分摊头部 | 少 → 头部占比大 |

**典型压缩率**：在生产环境中，bcachefs 通常能达到 60-80% 的键存储压缩率。

---

## 9. 与真实代码的关联

`persist.c` 中的每个组件都与真实 bcachefs 代码中的对应实现相关联：

| persist.c | 真实代码 | 文件 |
|-----------|---------|------|
| `struct bpos` | `struct bpos` | `fs/bcachefs_format.h:137` |
| `struct bkey` | `struct bkey` | `fs/bcachefs_format.h:211` |
| `struct bkey_packed` | `struct bkey_packed` | `fs/bcachefs_format.h:280` |
| `struct bkey_format` | `struct bkey_format` | `fs/bcachefs_format.h:126` |
| `struct bset` | `struct bset` | `fs/bcachefs_format.h:1905` |
| `struct btree_node` | `struct btree_node` | `fs/bcachefs_format.h:1944` |
| `bkey_pack()` | `bch2_bkey_pack_key()` | `fs/btree/bkey.h` |
| `bkey_unpack()` | `bch2_bkey_unpack_key()` | `fs/btree/bkey.h` |
| `pack_bits()` | `set_inc_field()` | `fs/btree/bkey.c:198` |
| `unpack_bits()` | `get_inc_field()` | `fs/btree/bkey.c:162` |
| `btree_node_write()` | `__bch2_btree_node_write()` | `fs/btree/write.c:323` |
| `btree_node_read_and_verify()` | `bch2_btree_node_read_done()` | `fs/btree/read.c:574` |

### 简化与差异说明

为了教学目的，`persist.c` 做了以下简化：

| 项目 | 真实实现 | persist.c |
|------|---------|-----------|
| 校验和 | 加密 HMAC/Checksum | 简化为 XOR |
| 加密 | 支持 ChaCha20 加密 | 未实现 |
| bset 合并 | 多 bset 排序+去重 | 仅单个 bset |
| 辅助搜索树 | Eytzinger 二叉树 | 未实现 |
| 日志结构 | 多个 btree_node_entry 追加 + 反向 whiteout 区域 | 基础追加 |
| format 计算 | 按 key_u64s、byte_aligned 路径优化 | 简单计算 |
| bkey 头部大小 | 44 字节 (含 pad[5] + _pad 字段) | 36 字节（省略填充） |
| `struct bch_extent_ptr` | 单 u64 bitfield: type/cached/unused/unwritten/offset/dev/gen | ✅ 已对齐 |
| `struct btree_node._ptr` | 唯一的 `struct bch_extent_ptr` (8 字节) | ✅ 已对齐（从 `_ptr[3]` 修正）|
| 写时复制 | 完整的 COW | 未实现 |
| I/O 层 | bio + replicas | 内存缓冲区 |

---

## 10. 参考资料

### 源码文件

| 文件 | 内容 | 行数 |
|------|------|------|
| `fs/bcachefs_format.h` | 所有磁盘格式定义（终极权威） | ~2016 |
| `fs/btree/types.h` | 内存 btree 类型定义 | ~1371 |
| `fs/btree/bkey.h` | 键打包/解包 API | ~691 |
| `fs/btree/bkey.c` | 键打包/解包实现 | ~1100 |
| `fs/btree/bset.h` | bset 操作 API + 辅助树文档 | ~618 |
| `fs/btree/bset.c` | bset 操作实现 | ~1873 |
| `fs/btree/sort.h` | 排序/压缩 API | ~93 |
| `fs/btree/sort.c` | 排序/压缩实现 | ~620 |
| `fs/btree/read.c` | btree 节点读取路径 | ~1400 |
| `fs/btree/write.c` | btree 节点写入路径 | ~920 |
| `fs/btree/write.h` | 写入路径头部 | ~45 |
| `fs/btree/read.h` | 读取路径头部 + 兼容性 | ~185 |
| `fs/btree/cache.c` | btree 节点缓存管理 | ~1800 |
| `fs/btree/commit.c` | 事务提交 | ~1400 |
| `fs/btree/interior.c` | 节点分裂/合并 | ~3300 |
| `fs/data/extents_format.h` | extent 指针格式 | ~310 |
| `fs/data/checksum.h` | 校验和计算宏 | ~46 |

### 分析文档

| 文档 | 内容 |
|------|------|
| `doc/analysis/bcachefs-kernel-core-principles.md` | 核心设计原则 |
| `doc/analysis/bcachefs-transaction-implementation.md` | 事务实现分析 |
| `doc/analysis/bcachefs-btree-iter-slots.md` | btree 迭代器与槽 |
| `doc/analysis/journal-subsystem.md` | 日志子系统分析 |

### 外部资源

- [bcachefs 官方网站](https://bcachefs.org/)
- [bcachefs 文档 - Btree Iterators](https://bcachefs.org/BtreeIterators/)
- Linux 内核源码: `fs/bcachefs/` 目录
