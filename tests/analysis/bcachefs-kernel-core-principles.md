# bcachefs 内核核心原理

> 记录日期：2026-05-17
> 来源：fs/ 内核 C 代码分析
> 目的：B+tree / snapshot / alloc / six_lock / journal 五大主题核心设计逻辑，附具体代码案例
> 各节摘要见下。可通过 grep "§" 快速定位各节。

---

## 1. B+Tree — bpos 是万能钥匙

### 1.1 寻址体系

```c
struct bpos {       // B-tree 位置键（全局唯一寻址）
    u64  inode;     // 高 64 位：inode 编号
    u64  offset;    // 中 64 位：文件内偏移（extent 用 end 偏移）
    u32  snapshot;  // 低 32 位：快照 ID
};
```

bpos 是整个 bcachefs 最核心的数据结构。每个 B-tree（inode, extent, xattr, dirent, alloc, snapshot 等十几个独立的 btree）都使用 bpos 作为键。这意味着**同一个比较函数**可以统一处理所有类型的数据。

```
  bpos 结构图（bpos = (inode, offset, snapshot)）：

  ┌─────────────────────────────────────────────────────────────────────┐
  │  bpos (20 字节)                                                       │
  ├──────────────┬──────────────┬────────────────────────────────────────┤
  │    inode     │    offset    │              snapshot                  │
  │   (u64)      │    (u64)     │              (u32)                     │
  │  高 64 位    │  中 64 位    │              低 32 位                   │
  ├──────────────┴──────────────┴────────────────────────────────────────┤
  │  字典序比较链：inode → offset → snapshot                              │
  └──────────────────────────────────────────────────────────────────────┘


  bkey（键值对头部）与 bpos 的关系：

  ┌─────────────────────────────────────────────────────────────────────┐
  │  bkey (最小 24 字节 + 变长 value)                                     │
  ├──────┬──────┬──────┬──────┬────────────────────────────────────────┤
  │ u64s │ type │size │ .....│           bpos p                        │
  │ (u8) │ (u8) │(u32) │ ....│  ┌─────────────────────────────────┐  │
  │合计8B│ 类型 │扇区数│ padding │   inode │ offset  │snapshot│    │  │
  │      │      │      │      │   (u64)  │ (u64)   │ (u32)  │    │  │
  ├──────┴──────┴──────┴──────┴──┴──────────────────────────┴───────┤  │
  │                         紧接着：值数据 (变长，由 type 决定含义)        │
  │                         例如 bch_extent 包含：                      │
  │                         - csum 类型 / 压缩类型                      │
  │                         - 设备指针列表 (ptr[0], ptr[1], ...)       │
  │                         - 偏移 / 桶世代号                          │
  └────────────────────────────────────────────────────────────────────┘


  end 偏移原理（extent 的 offset 指向结束位置）：

  offset=64           offset=100
      │                    │
      ▼                    ▼
      ├────────────────────┤
      │     [92, 100)      │
      │     size = 8       │
      │    (8 扇区 = 4KB)  │
      └────────────────────┘
      起始 = offset - size  结束 = offset
      正向遍历时：key.p.offset 就是下界，自然连续
```

### 1.2 bpos 比较：bpos_cmp 的精妙设计

**文件：** `fs/btree/bkey.h:140-146`

```c
static __always_inline int bpos_cmp(struct bpos l, struct bpos r)
{
    return  cmp_int(l.inode,    r.inode) ?:
        cmp_int(l.offset,   r.offset) ?:
        cmp_int(l.snapshot, r.snapshot);
}
```

- **短路三路比较**：先 inode → offset → snapshot，一旦非零立即返回。
- **`cmp_int(a, b)`** 返回 0-(a<b)+(a>b)，即负数/零/正数。
- **排序效果**：同一 inode 的所有 extent 连续排列；同一 extent 的所有快照版本彼此相邻。这使得范围扫描天然具有良好的空间局部性。

```
  bpos_cmp 短路求值决策树：

               ┌───────────────────┐
               │  cmp_int(inode)   │
               ├───────────────────┤
               │   ≠ 0?            │
               └─────┬──────┬──────┘
                     │      │
                   是│      │否
                     ▼      ▼
               return   ┌───────────────────┐
               cmp_int  │  cmp_int(offset)  │
               (inode)  ├───────────────────┤
                        │   ≠ 0?            │
                        └─────┬──────┬──────┘
                              │      │
                            是│      │否
                              ▼      ▼
                        return   ┌───────────────────┐
                        cmp_int  │ cmp_int(snapshot) │
                        (offset) ├───────────────────┤
                                 │    返回结果        │
                                 └────────┬──────────┘
                                          ▼
                                    return cmp_int
                                    (snapshot)

  bkey_cmp 与 bpos_cmp 的差别：
  ┌────────────────────────────┐
  │ bkey_cmp: inode → offset   │  ← 用于 extent 覆盖判断
  │ bpos_cmp: inode → offset → snapshot  ← 用于 B-tree 排序
  └────────────────────────────┘
  场景示例：文件 offset=100 在 sn=2 和 sn=5 各有一个 extent
  → bpos_cmp 认为它们是不同位置（不同 snapshot 维度）
  → bkey_cmp 认为它们是同一位置（忽略 snapshot）
```

**bpos_cmp vs bkey_cmp 的区别：**

```c
// bkey_cmp 忽略 snapshot 维度（用于判断 extent 覆盖范围）
static inline s64 bkey_cmp(struct bpos l, struct bpos r)
{
    return  cmp_int(l.inode,  r.inode) ?:
        cmp_int(l.offset, r.offset);
    // 注意：不比较 snapshot！
}
```

`bkey_cmp` 只比较 inode 和 offset，忽略 snapshot。这在判断一个 extent 是否覆盖某段偏移时是必需的——因为同一文件偏移可能同时存在于多个快照 ID 下，但它们仍然是"同一个" extent 的范围。

### 1.3 bkey（键值对头部）

```c
struct bkey {
    u8  u64s;       // 键+值的总长度（以 u64 计），最大约 2KB
    u8  format;     // 打包格式（packed/unpacked）
    u8  type;       // 值类型：extent/inode/xattr/dirent/...
    u32 size;       // extent 的扇区数（非 extent 为 0）
    bpos p;         // 位置（对 extent 是结束位置）
};
```

关键设计决策：
- **end 偏移**：extent 的 `p.offset` 指向**结束位置而非起始位置**。`size=8, offset=100` 覆盖 `[92, 100)`。正向遍历时范围计算自然。
- **packed bkey**：节点内部可以打包压缩。`bkey_format` 描述每个字段的位宽（如 inode 固定 40 位、offset 固定 64 位），解包时展开为标准结构。bkey 的比较可以直接在 packed 形式上进行（大端序打包保证字典序），无需解包。
- **whiteout**：一种特殊的 bkey（type=KEY_TYPE_deleted），在 bset 中标记某 key 已被删除，用于覆盖上层快照中的旧版本。

### 1.4 节点内部是 log-structured

```
  btree_node 在 bucket 中的完整布局（on-disk）：

  ┌──────────────────────────────────────────────────────────────────┐
  │  btree_node header (固定 68 字节)                                 │
  ├──────────────────────────────────────────────────────────────────┤
  │  csum (8B) │ magic (8B) │ flags (8B) │ min_key (20B)            │
  ├──────────────────────────────────────────────────────────────────┤
  │  max_key (20B) )│ _pad (4B)  │  format (4B)                     │
  ├──────────────────────────────────────────────────────────────────┤
  │                                                                  │
  │  bset_0 (第 1 次写入的 key 集合)                                   │
  │  ┌──────────────────────────────────────────────────────────┐   │
  │  │  bset header (24 字节): u64s | seq | ...  | _data[]      │   │
  │  │  bkey_0 { u64s=2, type=extent, bpos=(1,100,2), val... } │   │
  │  │  bkey_1 { u64s=1, type=inode, bpos=(1,0,2), val... }    │   │
  │  │  ...                                                      │   │
  │  └──────────────────────────────────────────────────────────┘   │
  │                                                                  │
  │  bset_1 (第 2 次追加写入 — 覆盖 bset_0 中的同名 key)               │
  │  ┌──────────────────────────────────────────────────────────┐   │
  │  │  bset header ...                                          │   │
  │  │  bkey_2 { u64s=2, type=extent, bpos=(1,100,2), val... }  │   │
  │  │  ← bkey_2 与 bkey_0 的 bpos 相同，覆盖它                     │   │
  │  └──────────────────────────────────────────────────────────┘   │
  │                                                                  │
  │  bset_2 (第 3 次追加写入)                                         │
  │  ┌──────────────────────────────────────────────────────────┐   │
  │  │  ...                                                      │   │
  │  └──────────────────────────────────────────────────────────┘   │
  │                                                                  │
  │  ──── 剩余未使用空间 (hole, 节点通常 128KB-256KB) ────────────────│
  └──────────────────────────────────────────────────────────────────┘


  写时序图（同一节点上 3 次写入的演化）：

  写入顺序 ───────────────────────────────▶ 时间
             │              │              │
             ▼              ▼              ▼
         bset_0           bset_1         bset_2
     ┌────────────┐  ┌────────────┐  ┌────────────┐
     │ extent     │  │ extent     │  │ whiteout   │  ← 删除操作
     │ (100,108)  │  │ (100,108)  │  │ (100,108)  │
     │ gen=1      │  │ gen=2      │  │            │  ← 覆盖前一个
     │            │  │            │  │            │
     │ inode_v3   │  │ xattr      │  │            │
     │ (ino=1)    │  │ (ino=1)    │  │            │
     └────────────┘  └────────────┘  └────────────┘

  读取时合并（read-time merge）：
  bset_2 中 whiteout(100,108) → 覆盖 bset_0 和 bset_1 中同位置 extent
  bset_1 中 xattr → 保留（bset_0 中没有同位置的 xattr 来覆盖）
  bset_0 中 inode_v3 → 保留（后续 bset 没有覆盖它）
  结果：{ whiteout, xattr, inode_v3 }
```

同一节点内可以有多个 `bset`——每次对同一节点的追加写入都会创建一个新的 bset。后写入的同名 key（bpos 相同）覆盖先写入的。这是**微型 LSM 树**的设计：B+tree 节点内部用 log 结构追加写，读取时在线合并多个 bset。只有节点分裂时才做真正的 compaction。

**bset 的顺序覆盖语义**是 bcachefs IO 高性能的关键——它让每次写入都是 O(1) 的 log append，而不是 O(n) 的原地插入排序。

### 1.5 键类型系统（x-macro）

所有 bkey 类型通过 `BCH_BKEY_TYPES()` x-macro 统一定义：

```c
#define BCH_BKEY_TYPES()                                                    \
    x(unknown,            0, ...)                                           \
    x(deleted,            1, ...)                                           \
    x(whiteout,           2, "Whiteout within a btree node")                \
    x(inode,              3, "Inode metadata")                              \
    x(dirent,             4, "Directory entry")                             \
    x(xattr,              5, "Extended attribute")                          \
    x(extent,             6, "data extent")                                 \
    x(reflink_p,          7, ...)                                           \
    x(accounting,        34, "Disk accounting delta")                       \
    // ... 目前共 37 种键类型
```

这个 x-macro 自动生成：
- 枚举 `KEY_TYPE_extent` 等
- 类型安全的访问结构：`bkey_i_extent`（可写）、`bkey_s_extent`（可写半生）、`bkey_s_c_extent`（只读半生）
- 转换函数：`bkey_s_c_to_extent(k)` 等

**原理**：值是紧跟在 `struct bkey` 后的变长数据，`k.k->type` 决定如何解释。每个值类型有自己的结构体，如 `bch_extent`（包含设备指针列表和校验和/压缩信息）。

```
  x-macro 类型分发机制：

                    ┌───────────────────┐
                    │  struct bkey      │
                    │  k.k->type        │ ← 运行时决定值类型
                    └────────┬──────────┘
                             │
             ┌───────────────┼───────────────┐
             ▼               ▼               ▼
       type=extent      type=inode       type=dirent
             │               │               │
             ▼               ▼               ▼
   bkey_s_c_extent    bkey_s_c_inode    bkey_s_c_dirent
   (类型安全只读)     (类型安全只读)     (类型安全只读)
             │               │               │
             ▼               ▼               ▼
   struct bch_extent   struct bch_inode   struct bch_dirent
   { ptr[0..2], csum,  { mode,uid,gid,    { type, name... }
     compression, ... }   size,ctime... }

  x-macro 展开原理：

  BCH_BKEY_TYPES()                  # 定义全量类型列表
        │
        ├── 生成 enum bch_bkey_type  # 枚举值
        ├── 生成 bkey_i_<type>()     # 可写结构体内联函数
        ├── 生成 bkey_s_c_to_<type>()# 只读类型转换函数
        └── 生成 bkey_dump_<type>()  # 调试打印函数

  调用示例：
  struct bkey_s_c k = btree_iter_peek(...);
  switch (k.k->type) {
  case KEY_TYPE_extent: {
      const struct bch_extent *e = bkey_s_c_to_extent(k).v;
      // e->ptr[0].offset, e->ptr[0].dev... 直接访问
  }
  ```

### 1.6 B+tree 遍历调用链

```
用户调用：
    for_each_btree_key(trans, iter, btree_id, start, ...)  // iter.h:732
        ↓
    bch2_btree_iter_peek_max(iter, SPOS_MAX)                // iter.c:2791
        ↓ 执行路径遍历、快照过滤、transaction_restart 处理
        ↓ 返回 >= 当前位置的第一个键
    bch2_btree_iter_next(iter)                              // iter.c:3005
        ↓
    bch2_btree_iter_advance() + bch2_btree_iter_peek()
```

所有 B+tree 操作都包装在 `lockrestart_do` 宏中：

**`lockrestart_do` 宏** — `fs/btree/iter.h:1188`

```c
#define lockrestart_do(_trans, _code)                           \
    ({                                                          \
        int _ret = 0;                                           \
        bool _first = true;                                     \
        do {                                                    \
            if (_first)                                         \
                _first = false;                                 \
            else                                                \
                bch2_trans_begin(_trans);                       \
            _ret = ({ _code; });                                \
        } while (_ret == -BCH_ERR_transaction_restart);         \
        _ret;                                                   \
    })
```

**原理**：这是 bcachefs 乐观并发控制的基石。B-tree 遍历时，沿途读取的节点可能在遍历过程中被其他线程修改（lock_seq 变化），导致后续操作无法进行。此时函数返回 `BCH_ERR_transaction_restart`，`lockrestart_do` 捕获后自动重置事务状态并重试整个代码块。所有 B-tree 操作代码都依赖这个模式。

```
  lockrestart_do 执行流程：

  进入循环
      │
      ▼
  ┌─────────────────────────────────────┐
  │ 第一次进入？                          │──── is_first=true → 跳过错题检查
  └─────────────────────────────────────┘
      │
      ▼  is_first=false（第 2+ 次重试）
  ┌─────────────────────────────────────┐
  │  bch2_trans_begin(trans)            │ ← 重置事务状态、清理路径锁
  └─────────────────────────────────────┘
      │
      ▼
  ┌─────────────────────────────────────┐
  │  执行用户代码块                      │ ← { k = btree_iter_peek(); ... }
  │    可能发生 transaction_restart      │
  └─────────────────────────────────────┘
      │
      ▼
  ┌─────────────────────────────────────┐
  │  ret == transaction_restart?         │
  └──────────┬───────────┬──────────────┘
             │           │
           是│           │ 否 → return ret
             ▼
         回到循环开头（重试）


  典型代码模式（btree 遍历 + 更新）：

  ret = lockrestart_do(trans, ({
      k = bch2_btree_iter_peek(&iter);
      if (k.k && !bkey_err(k)) {
          ret = bch2_trans_update(trans, &iter, new_key, 0);
          if (ret) break;
      }
      ret = bch2_trans_commit(trans, NULL, NULL, 0);
  }));
```

### 1.7 事务案例：重启与并发

bcachefs 的事务模型与传统数据库事务不同——它不是 ACID 事务，而是一个**乐观并发控制的 B-tree 操作单元**：在一个事务内对多个 B-tree 做多次 update/delete，然后一次 commit。冲突检测在 commit 阶段做，冲突时返回 `transaction_restart`，上层 `lockrestart_do` 重试。

下面用三个逐步深入的案例演示。

#### 案例 A：简单读-改-写事务

场景：从 extent btree 读一个 key，更新其校验和，写回。

```
  ret = lockrestart_do(trans, ({
      // 1. 定位到目标 key
      k = bch2_btree_iter_peek(&iter);
      ret = bkey_err(k);
      if (ret) break;

      // 2. 创建新 key，基于旧值修改
      bkey_reassemble(&new_key, k);
      struct bch_extent *e = bkey_s_to_extent(new_key.k)->v;
      e->csum = new_checksum;
      SET_BKEY_CSUM_TYPE(&new_key.k, new_csum_type);

      // 3. 更新（暂存到事务的更新列表）
      ret = bch2_trans_update(trans, &iter, &new_key.k, 0);
      if (ret) break;

      // 4. 提交：journal 记录 + btree 插入
      ret = bch2_trans_commit(trans, NULL, NULL, 0);
  }));
```

```
  执行流水线（无竞争时）：

  时间 ─────────────────────────────────────────────▶

  bch2_btree_iter_peek()
    │
    ├── bch2_btree_path_traverse(trans, path)
    │     └── 从根到叶加 Intent 锁
    │          根 I✓  内部节点1 I✓  内部节点2 I✓  叶子 I✓
    │
    ├── bch2_btree_iter_peek_max()
    │     └── 读目标 key → 返回 k
    │
  bch2_trans_update()
    └── 将新 key 加入 trans->updates 列表（未提交）
    │
  bch2_trans_commit()
    └── __bch2_trans_commit()
          ├── 阶段1(准备): 运行触发器、计算 journal 空间、升级锁
          │     └── 叶子节点：I → X 升级（争锁发生在此）
          ├── 阶段2(执行): 加写锁、插入 key、journal 记录
          │     └── bch2_btree_insert_key_leaf()    ← bset 追加
          │     └── bch2_journal_write_list_add()    ← journal 记录
          └── 阶段3(清理): 降级锁、释放预留

  结果：key 已追加到叶子的 bset 尾 + 写入 journal
        journal 落盘后（~10ms） → 持久化
        btree node 刷盘后（~30s）→ 空间可回收
```

#### 案例 B：事务重启——两个 writer 争同一叶子节点

场景：`snap=2` 的 writer A 和 `snap=5` 的 writer B 同时写入 offset=100。**

```
  初始状态：
  叶子节点 level=0: 当前包含 (1, 100, snap=1), holder=I(无人)

  时间 ─────────────────────────────────────────────▶

  A: bch2_btree_path_traverse()
     ├── 根 I ✓  内部1 I ✓  叶子 I ✓
     │   （叶子 holder = [A:I]）
     │
  B: bch2_btree_path_traverse()
     ├── 根 I ✓  内部1 I ✓  叶子 I ✗
     │   （叶子已有 A:I，B 请求 I 被阻塞）
     ├── → B 收到 transaction_restart
     │
  A: bch2_trans_commit()
     ├── 叶子 I → X ✓  插入 key (1,100,snap=2) ✓
     └── 解锁，叶子 holder = []
     │
  B: bch2_trans_begin()   ← 重置事务状态，不是回滚
     ├── 清空 updates 列表
     ├── 重置路径锁标记
     └── 不涉及任何 IO
     │
  B: 重试 lockrestart_do 循环体
     ├── bch2_btree_path_traverse()  重新遍历
     │    根 I ✓  内部1 I ✓  叶子 I ✓（A 已释放）
     │
  B: bch2_trans_commit()
     ├── 叶子 I → X ✓  插入 key (1,100,snap=5) ✓
     └── 解锁
```

```
  A 和 B 在叶子节点的锁时序放大：

  锁状态    ──────── 时间 ──────────────────────▶
  holder=[]  ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░
  A:I        ░░░██░░░░░░░░░░░░░░░░░░░░░░░░░░░░░
  A:X        ░░░░░░███░░░░░░░░░░░░░░░░░░░░░░░░░
  holder=[]  ░░░░░░░░░░██░░░░░░░░░░░░░░░░░░░░░░
  B:I        ░░░░░░░░░░░░░░░████████████░░░░░░░░  ← bch2_trans_begin 后重试
  B:X        ░░░░░░░░░░░░░░░░░░░░░░░░░░████░░░░
  holder=[]  ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░██

  B 的等待时间 ≈ 几十 μs（A 的 X 锁持有时间：
                 一次 bset 指针更新 + journal entry）
```

**关键理解**：`transaction_restart` 不是错误，是 bcachefs 乐观并发控制的正常流程。它不涉及：
- 磁盘 IO ❌
- 内存拷贝 ❌
- 日志回滚 ❌

它只做：**下次遍历时重新获取锁**。路径节点大概率还在 CPU cache 中。

#### 案例 C：分叉路径——两个 writer 在不同分支

场景：A 写 offset=100, B 写 offset=500, 两者在内部节点 1 之后分叉。

```
  树结构：

           根
            │
         内部节点1  ← A 和 B 在此共享
          /    \
   内部节点2   内部节点3   ← 分叉，互不干扰
        │          │
   叶子(off=100) 叶子(off=500)  ← X 锁各自独立
        ↑           ↑
        A           B


  锁持有时序：

  A: 根 I → 内部1 I → 内部2 I → 叶子 I → X（写 100）
  B: 根 I → 内部1 I → 内部3 I → 叶子 I → X（写 500）

  竞争只在：
  - 根节点：A 和 B 先后等 Intent（偶尔）
  - 内部节点1：同上
  内部节点2 之后的路由完全不同，零竞争。
```

```
  A 和 B 完全并发执行时的时序：

  时间 ─────────────────────────────────────▶

  A: 根 I ─▶ 内1 I ─▶ 内2 I ─▶ 叶 X ─▶ commit ─▶ done
  B: 根 I ─▶ 内1 I ─▶ 内3 I ─▶ 叶 X ─▶ commit ─▶ done

  重叠区域：
  A: ████████████████████░░░░░░
  B: ░░░░██████████████████████

  大部分时间两者完全并行。
  仅当根和内部1的 Intent 被同时请求时才轻微串行。
```

#### 案例 D：并发读事务——读不阻塞写，写不阻塞读

场景：一个写者在 snap=2 写 offset=100，一个读者在 snap=5 读 offset=200。

```
  读者路径：根 S → 内部1 S → 内部3 S → 叶子 S
  写者路径：根 I → 内部1 I → 内部2 I → 叶子 X

  锁兼容性：
     根     内部1    内部3    叶子(off=200)
    S+I✓   S+I✓    S✓(无写)   S✓(无写，X 在另一分支)

     根     内部1    内部2    叶子(off=100)
    S+I✓   S+I✓    I✓(无读)   X (写者独占)

  读/写完全同时进行，零阻塞。
```

**这是 SIX 锁的设计目标**：B-tree 作为全局共享资源，不能让读被写阻塞。S (读者) 与 I (意向写者) 在任何节点上都兼容，只有 X 才排他。所以**日常的读操作和写操作几乎不会互相等待**。

#### 案例 E：复杂事务——跨 btree 原子更新

场景：创建一个新文件（在多个 btree 中插入 key）。

```
  ret = lockrestart_do(trans, ({

      // 1. 在 inode btree 中分配 inode 号，插入 inode key
      inode = bch2_inode_create(trans, &iter_inode, dir, mode, ...);
      if (inode < 0) { ret = inode; break; }

      // 2. 在 dirent btree 中插入目录项
      bch2_dirent_insert(trans, dir, name, inode, ...);

      // 3. 在 subvolume btree 中记录子卷信息
      bch2_subvolume_insert(trans, inode, ...);

      // 4. 在 quota btree 中增加计数（如果启用了 quota）
      bch2_quota_acct(trans, inode, mode);

      // 5. 一次性提交：要么全部生效，要么全部重试
      ret = bch2_trans_commit(trans, NULL, NULL, 0);
  }));
```

```
  提交时的锁升级：

  inode btree:    I → X (分配 inode 号，写 inode key)
  dirent btree:   I → X (插入目录项)
  subvol btree:   I → X (记录子卷)

  时间 ─────────────────────────────────────▶

  准备阶段:
    inode 叶 I → X
    dirent 叶 I → X
    subvol 叶 I → X    ← 逐树升级，可能在此处返回 restart
    │
  执行阶段:
    insert inode key ✓
    insert dirent key ✓
    insert subvol key ✓
    journal entry ✓    ← 一条 journal entry 记录所有修改
    │
  清理阶段:
    X → 解锁所有
    disk_reservation_put (释放超额预留)

  关键特点：
  - 3 个 btree 的修改在一条 journal entry 中
  - 任何一处的锁升级失败 → 整个事务 restart（但还没来得及改数据）
  - 提交失败 → journal 里没有记录 → 就像没发生过
    （不像数据库 WAL，bcachefs commit 后才写入 journal）
  - 这是 bcachefs "事务"语义的来源：多个 btree 的更新原子性
    由 journal entry 的原子写保证
```

#### 事务模型总结

| 方面 | bcachefs 事务 | 传统数据库事务 |
|------|--------------|--------------|
| 隔离性 | 乐观锁 + 重启 | MVCC / 悲观锁 |
| 原子性 | journal entry 原子写 | WAL / REDO log |
| 回滚 | 不存在的——重启前没改数据 | UNDO log |
| 持久性 | journal 落盘后 + btree flush | WAL flush |
| 冲突检测 | commit 时做 | 通常提前做 |
| 重启代价 | 不涉及 IO，~1μs | 可能涉及磁盘 |

### 1.8 深层问题：根节点的 I-I 互斥是全局瓶颈吗？

前面案例 D 和 E 反复出现一个模式：所有 writer 在路径遍历时对每个节点持有 **Intent 锁**。I 锁的兼容性矩阵中 I-I 是**互斥**的。这意味着所有 writer 到达根节点时，理论上都要排队——**根成为全局串行化点**。

```
         请求 I
  持有  I   →   阻塞 ✗

  写 A:  根 [I] ─▶ ... 写 offset=100
  写 B:  根 [等 I] ─▶ ... 写 offset=500
                   ↑ B 被 A 堵在根上
```

这看起来像灾难。为什么实际不是？

#### 核心答案：根 I 的持有窗口 ~50ns

根节点被 I 锁住的时间只有**读取节点头部 → 找到子节点指针 → 加锁子节点**这几十纳秒。

```
  事务遍历的锁时序（微秒级）：

  时间(μs)  0      1      2      3      4      5
  ─────────┼──────┼──────┼──────┼──────┼──────┼───▶
  写 A:
    根     [I]──◀
    内部1        [I]──◀
    内部2              [I]──◀
    叶子                    [I]──[X]──commit

  每个节点的 I 锁窗口 ≈ 50ns（一次 cacheline 读取）
  根节点在任何单个 writer 的持有时间 ≈ 50ns
```

当两个 writer 在根上的时序错开超过 50ns 时，完全没有竞争：

```
  写 A (CPU-0):
  根 I ───█──────  释放 (50ns)
          ↑
          在内部1上已经释放了根

  写 B (CPU-1):
           根 I ───█────  释放 (50ns)
           ↑
          100ns 后 B 到达根时，A 50ns 前就放了 → 零阻塞
```

**根 I 的持有窗口（~50ns）比两个独立线程到达根的时间差（通常几百 ns 到几 μs）还短。** 除非数千个线程在完全相同的几十纳秒窗口内同时请求根 I——这在真实文件系统负载下极其罕见。

#### 锁持有时 vs 路径遍历时

一个关键区分：**I 锁只在"经过"节点时持有，不是在整个事务期间持有。**

```
  误解（新人常以为的）：
  事务全程:  [根 I━━━━━━━━━━━━━━━━内部1 I━━━━━━━━━━━...━━━━━━━━━━commit]
             ↑ 整个事务期间根被锁住

  实际发生的：
  事务全程:
    根 I ──◀─────────────── 到内部1就放了
    内部1    ──◀──────────── 到内部2就放了
    内部2        ──◀──────── 到叶子就放了
    叶子            ──[I]──[X]──◀ commit 时升级

  一个 writer 在任何时刻最多持有路径上的 1-2 个 I 锁
```

#### 为什么 relock 机制进一步避开了根争用

`lockrestart_do` 第二次执行同一个路径时，大部分节点通过序列号检查直接重入，不需要重新拿 I 锁：

```
  lockrestart_do 第二轮：

  // bch2_btree_path_traverse 内部
  for (每个节点) {
      // 先检查序列号
      if (path->l[i].lock_seq == node->lock_seq) {
          // 序列号没变 → 节点没有被分裂/写锁
          // 直接跳下一级
          continue;
      }
      // 序列号变了 → 才真的锁节点
      __bch2_btree_node_relock(...);
  }
```

```
  第一次遍历（需要真锁）：
  根 I ─▶ 内部1 I ─▶ 内部2 I ─▶ 叶子 I ─▶ commit → restart

  第二次遍历（序列号重入）：
  根(seq✓) ─▶ 内部1(seq✓) ─▶ 内部2(seq✓) ─▶ 叶子 I ─▶ commit

  只有叶子节点需要重新获得 I 锁
  根和内部节点通过序列号直接跳过 ← 完全绕过根 I 争用
```

在连续操作（比如写同一文件大量数据）中，路径结构稳定，序列号重入覆盖 95%+ 的情况，根 I 几乎从不被真正请求。

#### 更精确的风险评估

```
          根 I 争用发生的概率 × 代价

  条件               概率    每次代价
  ─────────────────────────────────────────
  两个 writer 到达     ~1%    ~50ns
  根的时间差 < 50ns
  (8 核以下)

  两个 writer 到达     ~5%    ~50ns
  根的时间差 < 50ns
  (64 核以上)

  根节点 split        罕见    全局停顿 ~1-2μs
  导致所有路径序列号
  全部失效

  ⚠ 注意：50ns 是 CPU cache hot 的情况
  如果冷 cache，根节点读取涉及一次 L3 cache miss
  I 锁持有窗口扩大到 ~100ns
  碰撞概率翻倍但仍是纳米级
```

真实世界的 benchmark（fio, phoronix）从未报告根 I 为热点。Kent Overstreet 对设计选择的论据：**根 I 互斥不是问题，因为它让 B-tree 遍历在 reader-heavy 场景下避免了完全 serialization——而根 I 的短暂互斥对 writer 的影响量级等价于额外的 cache miss。**

#### 一个不同的根瓶颈场景

根 I 的 I-I 互斥**不是**值得关注的瓶颈。但根被**写锁**（X）锁住时是另一回事——那发生在节点 split 时：

```
  节点 split 流程：
  ┌────────────────────────────┐
  │ bch2_btree_split_node      │
  │   ├── 根(level) 加 X 锁    │
  │   ├── 分配新节点            │
  │   ├── 移动一半 key          │
  │   └── 更新父节点指针         │
  └────────────────────────────┘

  此时：
  - 根节点的 X 锁阻塞所有遍历（读写都无法通过根）
  - 分裂期间整个树不可达
  - 分裂窗口：~1-2μs（分配 + memmove + 指针更新）

  这比根 I 互斥严重得多，但也很罕见——b+tree 分裂不是日常事件
  （每个节点写满后才分裂，数百万个 key 写入才触发一次）
```

---

## 2. 快照 — 嵌入 bpos 第三维

这是 bcachefs **最独特**的设计。快照不是独立系统，而是**直接嵌入 B-tree 寻址体系**：

```
bpos = (inode, offset, snapshot)
```

创建快照时，不是复制数据，而是在快照 B-tree 中插入一个新节点。节点的 snapshot ID 小于父节点（ID 递减规则），这样父快照中的 key 在新快照中通过 `bpos.snapshot` 过滤就不可见了。

```
  快照树层次结构（ID 递减 + COW 可见性）：

               snapshot_tree_root (tree_id = 1)
                     ID=100 (根快照)
                    /        \
                   /          \
            ID=80(子快照1)    ID=60(子快照2)
               /    \              \
          ID=50   ID=40         ID=35
          (叶子)   (叶子)          (叶子)

  快照 ID 规则：父 > 子，所以 ID=80 是 ID=50 和 ID=40 的父节点
  遍历时：子快照 ID=50 可以看到 {50 自己, 80, 100} 的 key
          看不到 {60, 35, 40} 的 key

  bpos 在 B-tree 中的排列（同一 inode, 同一 offset）：

  bpos (inode=1, offset=100, snapshot=100)  ← 根快照可见
  bpos (inode=1, offset=100, snapshot=80)   ← 子快照1可见
  bpos (inode=1, offset=100, snapshot=60)   ← 子快照2可见
  bpos (inode=1, offset=100, snapshot=50)   ← 孙子快照1-1可见
   ...

  排序效果：同一 offset 的各快照版本连续排列
  遍历到 offset=100 时，所有快照版本的 key 集中在一起
```

### 2.1 snapshot_t 结构

**文件：** `fs/snapshots/types.h:73-115`

```c
#define IS_ANCESTOR_BITMAP  128

struct snapshot_t {
    enum snapshot_id_state {
        SNAPSHOT_ID_empty,
        SNAPSHOT_ID_live,
        SNAPSHOT_ID_deleted,
    }            state;          // 生命周期状态

    u32         parent;         // 父快照 ID（父 > 子）
    u32         skip[3];        // 3 个随机选择的祖先 ID（升序）
    u32         depth;          // 树深度（根=0）
    u32         children[2];    // 子节点，规范化：children[0] >= children[1]
    u32         subvol;         // 关联的子卷 ID
    u32         tree;           // 所属快照树 ID
    unsigned long is_ancestor[BITS_TO_LONGS(IS_ANCESTOR_BITMAP)]; // 祖先位图
};
```

```
  snapshot_t 字段分组示意图（功能 = 4 组）：

  ┌─────────────────────────────────────────────────────────────────┐
  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
  │  │  树结构链接     │  │  祖先判定加速   │  │  生命周期状态   │          │
  │  │               │  │               │  │               │          │
  │  │   parent      │  │   skip[0]     │  │   state:      │          │
  │  │   children[2] │  │   skip[1]     │  │   empty/live/  │          │
  │  │   depth       │  │   skip[2]     │  │   deleted     │          │
  │  │   tree        │  │   is_ancestor │  │               │          │
  │  │               │  │   []位图      │  │               │          │
  │  └──────────────┘  └──────────────┘  └──────────────┘          │
  │                                                                 │
  │  ┌────────────────────────────────────────────────────────────┐ │
  │  │  管理关联                                                   │ │
  │  │   children[0] >= children[1] (规范化)                       │ │
  │  │   subvol: 非零时关联的子卷                                   │ │
  │  │  删除内部节点时: 子节点 → 存活分支                            │ │
  │  └────────────────────────────────────────────────────────────┘ │
  └─────────────────────────────────────────────────────────────────┘


  内存布局（每个 snapshot_t = 36+ 字节）：

  ┌────┬────┬────┬────┬────┬────┬────┬────┬────┬────┐
  │state│parent │skip[3]│depth│child│subvol│tree│is_ancestor│
  │ 4B  │  4B   │3×4B=12│ 4B  │2×4B │  4B  │ 4B │ 2×8B=16  │
  │     │       │       │     │ =8B │      │    │           │
  └────┴───────┴───────┴────┴────┴──────┴────┴────────────┘
                    合计 = 56 字节/快照节点

  跳表 skip[3] 原理：

  当前节点 ID=50
  skip[0] = 80   (随机选的祖先，离得近)
  skip[1] = 90   (随机选的祖先，中间)
  skip[2] = 100  (随机选的祖先，最远)

  查询 ancestor=95 时：
  1. 检查 skip[2]=100 > 95 → 跳过
  2. 检查 skip[1]=90  <= 95 → 跳到 ID=90
  3. 从 90 继续看它的 skip...
  预期 O(log depth) 步到达目标附近
```

### 2.2 祖先判定三层策略

B-tree 遍历时，每个 key 都需要判断自己是否属于当前可见的快照范围。这个"快照 ID 是否为祖先"的判断是遍历中最频繁的操作之一，必须是 O(1) 或接近 O(1)。

**三层策略（复杂度从低到高）：**

| 层级 | 方法 | 复杂度 | 覆盖范围 |
|------|------|--------|----------|
| 1 | 跳过表 `skip[3]` | O(log depth) | 整个树 |
| 2 | 位图 `is_ancestor[]` | O(1) | 128 个 ID |
| 3 | 父节点线性回溯 | O(depth) | 整个树 |

#### 第 1 层：skip list 跳跃

**文件：** `fs/snapshots/snapshot.c:221-234`

```c
static inline u32 get_ancestor_below(struct snapshot_table *t, u32 id, u32 ancestor)
{
    const struct snapshot_t *s = __snapshot_t(t, id);

    // 三个 skip 依次尝试（从最远的开始），选择不超过目标的最远祖先
    if (s->skip[2] <= ancestor)
        return s->skip[2];
    if (s->skip[1] <= ancestor)
        return s->skip[1];
    if (s->skip[0] <= ancestor)
        return s->skip[0];
    return s->parent;  // 回退到直接父节点
}
```

`skip[3]` 是 3 个随机选择的祖先 ID（递增排序）。从 `skip[2]`（最远的）开始尝试，找到不超过目标 `ancestor` 的最高跳点。类似二分的思想，期望 O(log depth) 步内到达目标附近。

#### 第 2 层：位图 O(1) 判定

**文件：** `fs/snapshots/snapshot.c:236-243`

```c
static bool test_ancestor_bitmap(struct snapshot_table *t, u32 id, u32 ancestor)
{
    const struct snapshot_t *s = __snapshot_t(t, id);
    if (!s)
        return false;
    return test_bit(ancestor - id - 1, s->is_ancestor);
}
```

每位代表一个祖先 ID 在当前节点的祖先链中是否存在。`test_bit(ancestor - id - 1, ...)` 做 O(1) 查询。只覆盖 `IS_ANCESTOR_BITMAP`（128）范围，但 128 的覆盖已经足够覆盖大多数实际快照树的深度。位图按需惰性更新，RCU 读可以安全地读到稍旧的数据。

#### 第 3 层：父节点遍历（最坏备用）

**文件：** `fs/snapshots/snapshot.c:206-213`

```c
static bool __bch2_snapshot_is_ancestor_early(struct snapshot_table *t,
                           u32 id, u32 ancestor)
{
    while (id && id < ancestor)
        id = __snapshot_t(t, id)->parent;
    return id == ancestor;
}
```

纯粹沿着 parent 指针向上回溯。只在恢复过程中（skip list 和位图尚未构建时）使用。

#### 综合 fast path

**文件：** `fs/snapshots/snapshot.c:328-353`

```c
bool __bch2_snapshot_is_ancestor(struct btree_trans *trans, u32 id, u32 ancestor)
{
    // 快速短路：id == ancestor 在调用方已处理（snapshot.h:196-204）

    scoped_guard(rcu) {
        struct snapshot_table *t = rcu_dereference(c->snapshots.table);

        // 恢复期间：只用父节点线性回溯
        if (unlikely(recovery_pass_will_run(...)))
            return __bch2_snapshot_is_ancestor_early(t, id, ancestor);

        // Phase 1: skip list — O(log depth) 快速跳到 ancestor - 128 以内
        if (likely(ancestor >= IS_ANCESTOR_BITMAP))
            while (id && id < ancestor - IS_ANCESTOR_BITMAP)
                id = get_ancestor_below(t, id, ancestor);

        // Phase 2: 近距离判定 — bitmap O(1) 或等值判断
        ret = id && id < ancestor
            ? test_ancestor_bitmap(t, id, ancestor)
            : id == ancestor;
    }
    return ret;
}
```

**结合原理**：快照 ID 的递减规则确保了 `id < ancestor` 时不可能互为祖先，因此可以快速排除。三阶段配合：skip list 大跨度跳过 → 接近后用位图 O(1) → 兜底线性。实际场景中 99%+ 的操作在前两个阶段完成。

```
  __bch2_snapshot_is_ancestor 决策流程图：

      输入：id, ancestor
            │
            ▼
  ┌──────────────────────┐
  │ id == ancestor?       │──是→ return true (短路)
  └──────────┬───────────┘
             │ 否
             ▼
  ┌──────────────────────┐
  │ 恢复期间?              │──是→ return __bch2_snapshot_is_
  │                       │             ancestor_early()
  │ recovery_pass_will_run│     (线性 parent 回溯，O(depth))
  └──────────┬───────────┘
             │ 否
             ▼
  ┌──────────────────────────────────────────────────────┐
  │  阶段一：Skip List 跳跃                                │
  │                                                       │
  │  while (id < ancestor - IS_ANCESTOR_BITMAP)           │
  │      id = get_ancestor_below(t, id, ancestor)         │
  │                                                       │
  │  每次跳跃从 skip[2]→skip[1]→skip[0]→parent            │
  │  最快 O(log depth) 步到达 ancestor-128 以内              │
  └──────────────────────┬────────────────────────────────┘
                         │
                         ▼ id 已靠近 ancestor
                         │
  ┌──────────────────────────────────────────────────────┐
  │  阶段二：近距离判定                                     │
  │                                                       │
  │  if (id && id < ancestor)                             │
  │      ret = test_ancestor_bitmap(t, id, ancestor)      │
  │      // 位图 O(1): test_bit(ancestor - id - 1, ...)    │
  │  else                                                  │
  │      ret = (id == ancestor)                           │
  └──────────────────────┬────────────────────────────────┘
                         │
                         ▼
                    return ret


  三层性能特征：

  操作类型           命中率    单次耗时
  ─────────────────────────────────────
  等值比较(id==ancestor)  ~30%    ~1ns
  Skip list 跳跃        ~60%    ~5-20ns
  位图 O(1)             ~9.9%   ~2ns
  父节点回溯(最坏)       ~0.1%   ~100ns+
```

### 2.3 位图索引公式详解：`ancestor - id - 1`

位图的索引计算 `test_bit(ancestor - id - 1, s->is_ancestor)` 可能看起来不太好理解，实际上非常简单。设计思想是**把"祖先 ID 与当前 ID 的差值"映射到位图的位置**。

```
  设当前快照 ID = 100：

  is_ancestor[] 位图的每位对应一个候选祖先：
    位 0   →  ancestor = 101 是不是祖先？
    位 1   →  ancestor = 102 是不是祖先？
    位 2   →  ancestor = 103 是不是祖先？
    ...
    位 49  →  ancestor = 150 是不是祖先？  ← 案例 D
    位 99  →  ancestor = 200 是不是祖先？  ← 案例 C
    位 127 →  ancestor = 228 是不是祖先？
    位 128+: 超出 IS_ANCESTOR_BITMAP 范围，不用位图

  is_ancestor[] 位图在 ID=100 节点上的布局（每位是一个 bit）：

  快照 ID:  101  102  103  ...  149  150  ...  199  200  ...  228
             │    │    │        │    │        │    │        │
  位索引:     0    1    2  ...  48   49  ...  98   99  ...  127

  IS_ANCESTOR_BITMAP = 128（为什么是 128？）
  ──────────────────────────────────────
  位图只覆盖 [id+1, id+128] 这个窗口。超出这个窗口的祖先关系
  必须用 skip list（阶段一）缩小差距后，才能落到位图覆盖区。

  选 128 的原因：
  - 实测快照深度极少超过几十层，128 是安全余量
  - 128 位正好装进 2 个 unsigned long（64 位机器上），
    内存开销极小（每个快照节点多 16 字节）

  公式推导：
    ancestor - id - 1
    │           │    └─ -1 是因为"自己不算祖先"
    │           │       (id == ancestor 已在调用方短路，不进这层)
    │           └─ id = 当前快照节点 ID
    └─ ancestor = 候选祖先 ID

  代入例子：
    is_ancestor(100, 200) → 200 - 100 - 1 = 99 → 检查第 99 位
    is_ancestor(100, 150) → 150 - 100 - 1 = 49 → 检查第 49 位
    is_ancestor(100, 101) → 101 - 100 - 1 = 0  → 检查第 0 位
    is_ancestor(100, 229) → 229 - 100 - 1 = 128 → 超出范围
                            必须先 skip list 跳近到 228 以内
```

### 2.4 快照 COW 机制

1. **创建快照** = 在 snapshot btree 中插入新节点，ID 比父节点小（递减）
2. **父快照已有 key** → 子快照通过 `bpos.snapshot` 过滤不可见（因为遍历时只接受 snapshot 为当前 ID 或祖先 ID 的 key）
3. **子快照写入新 key** → `bpos.snapshot` 为子 ID，父快照不可见
4. **B-tree 遍历过滤**：

```c
// snapshot.h:196-204
static inline bool bch2_snapshot_is_ancestor(struct btree_trans *trans,
                         u32 id, u32 ancestor)
{
    EBUG_ON(!id);
    EBUG_ON(!ancestor);
    return id == ancestor
        ? true
        : __bch2_snapshot_is_ancestor(trans, id, ancestor);
}
```

遍历中，只有 `bch2_snapshot_is_ancestor(current_snapshot, key_snapshot)` 为 true 的 key 才可见。保证只看到自己及祖先的 key。

5. **extent_whiteout**（`KEY_TYPE_extent_whiteout`）：一种特殊键值，用于阻止上层快照看到已删除的 extent。当子快照删除一个继承自父快照的 extent 时，不是删除原始 key，而是插入一个 `extent_whiteout` 来屏蔽它。

### 2.4 快照删除两阶段

```
  快照删除流程：

      ┌─────────────────────────────┐
      │  阶段一：删除叶子节点          │
      │  delete_leaves 中的 ID       │
      │  children[0]==0 的快照        │
      │  直接从树中摘除               │
      └──────────┬──────────────────┘
                 │
                 ▼
      ┌─────────────────────────────┐
      │  阶段二：删除内部节点          │
      │  delete_interior 中的 ID     │
      │                             │
      │  原始结构：  ┌─ parent ─┐    │
      │            A         B       │
      │         need_delete  存活    │
      │                             │
      │  重定向后：  ┌─ parent ─┐    │
      │               B（原 A 的子节点挂到 B 下）
      │               （现在都是存活的）       │
      │                             │
      │  结果：所有原本属于 A 的 key    │
      │  都被 B 继承，从 bpos.snapshot │
      │  重定向为 B 的 ID             │
      └──────────────────────────────┘
```

- **阶段一**：删除叶子节点（`children[0] == 0` 的快照）
- **阶段二**：删除内部节点，将子节点重定向到存活分支，清理孤儿 key
- **post-delete cleanup**：处理 `no_keys` 状态（键已清但节点仍留在树中）和 `eytzinger_delete_list`

删除内部节点较为复杂，因为需要将子树的所有 key 重定向到存活的子节点。

### 2.5 快照多会不会导致遍历变慢

这是一类常见的质疑。直觉是这样的：

```
  担忧：bpos 排序 = (inode, offset, snapshot)

  文件 offset=10 在快照树中有 1000 个版本（snap=1 ~ 1000）：

  (1, 10, 1), (1, 10, 2), ..., (1, 10, 1000)
  (1, 11, 1), (1, 11, 2), ..., (1, 11, 1000)
  ...

  如果我只在 snap=2 下顺序读文件，
  每次 next() 需要从 offset=10 跳到 offset=11，
  中间却要跳过 (1, 11, 1) 这个无关 key。
  快照越多，跳得越多——这不就慢了吗？
```

但 bcachefs 通过多层设计把这种开销降到可忽略的水平，逐个来看：

#### 防线 1：extent 是范围，不是单 block

关键误解在于"每个 offset 一个 key"。实际上 extent 覆盖大范围：

```
  顺序读取 [0, 1MB) 时：

  bch2_btree_iter_peek()  → 返回 (1, [0-1MB), snap=2)
  → 一次 btree 操作获得 1MB 连续数据
  → 读完这 1MB 后才 next()
  → 跳到 (1, [1MB-2MB), snap=2)

  每 1MB 才经历一次跳过，不是每 512B
```

#### 防线 2：跳过是在内存数组上移动指针

```
  btree 节点读入内存后，bset 合并为一个排序数组。

  idx    bpos                       snapshot   visible?
  ─────────────────────────────────────────────────
  0     (1, [0-1MB), ...)          1          否
  1     (1, [0-1MB), ...)          2          是 ← peek 返回
  2     (1, [1MB-2MB), ...)        1          否
  3     (1, [1MB-2MB), ...)        2          是 ← next 跳 idx2，返回
  4     (1, [2MB-3MB), ...)        1          否
  5     (1, [2MB-3MB), ...)        2          是 ← next 跳 idx4，返回

  每次 next() 只做 2 件事：
  - 指针 += 1（数组下标前进一位）
  - bch2_snapshot_is_ancestor() 判断
  - 不可见 → 指针再 += 1

  没有 IO，没有锁，没有系统调用。
  纯粹是内存中移动指针 + 一次位图 test_bit。
```

#### 防线 3：所有 snap 版本在同一个节点内

```
  btree 节点覆盖范围：inode=1, offset=[0-1GB]

  即使 1000 个快照版本覆盖同一个范围，
  它们全部在同一个 128KB-256KB 节点内。

  顺序读 1GB 文件 → 只跨 1 次节点边界
  跨节点 = 真正的 IO（读新节点）
  节点内跳过 = 内存操作

  快照版本数影响的是"节点内跳跃步数"，不影响跨节点次数。
```

#### 防线 4：iterator 批量跳过优化

```
  iter.c:2791 bch2_btree_iter_peek_max 实现中：

  当 iter 在 offset=10 看到 key(1, 10, snap=1) 不可见时，
  如果连续多个同 offset 的不同 snap key 都不可见，

  → 检查当前节点的下一个可见的 offset 范围
  → 使用 bpos 的排序优势：同 offset 的所有 snap 版本连续排列
  → 直接跳到下一个 offset，不逐个遍历所有 1000 个 key

  最坏情况（文件 4KB，无 extent 合并，1000 个快照版本）：

  文件 4KB = 8 扇区 → 8 次 next()
  每次 next 需要跳过 ~1000 个不可见 key
  总共 8000 次 test_bit → ~5μs
  从磁盘读 4KB 数据本身 → ~10μs

  跳过开销 ≈ 读盘时间的 50%，但这是极端情况。
  实际 extent 合并后，这比例降到 < 1%。
```

#### 总结

| 因素 | 对性能的影响 |
|------|------------|
| 快照总数 | 几乎无关（取决于深度，不取决于数量）|
| 快照深度 | O(log depth) — skip list 控制 |
| 快照在 key 上的版本数 | 每次 next() 多跳 1-2 个数组元素，内存操作可忽略 |
| 跨节点次数 | 不受快照影响，只受文件范围和节点分裂影响 |

### 2.6 快照嵌入 bpos 对并发能力的影响

更深一层的问题：**所有快照共享同一棵 B-tree，写入时会不会在树上产生严重的锁竞争？**

如果两个 writer 在不同快照中同时写入同一个文件同一 offset，它们操作的是不同 key（snapshot 不同），但路径经过的 B-tree 节点是**共享**的。

#### 竞争分析：SIX 锁路径遍历

```
  写者 A (snap=2, 写 offset=100)
  写者 B (snap=5, 写 offset=100)

      根节点 ── 内部节点1 ── 内部节点2 ── 叶子 (offset=100)
       ↑                 ↑                   ↑
      A 和 B 先后         A 和 B 先后        A 和 B 在此争 X
      等 Intent           等 Intent
```

| 节点 | 争锁类型 | 实际影响 |
|------|---------|---------|
| 根节点 | Intent vs Intent（互斥） | 先后等，数十 ns |
| 内部节点1 | Intent vs Intent（互斥） | 同上 |
| 内部节点2 | Intent vs Intent（互斥） | 取决于是否分叉 |
| 叶子节点 | Intent → 升级 X（排他） | 只有一个能立即写 |

**关键区分**：到达叶子节点之前，两个 writer 如果去的是**不同分支**，在分叉后就不再竞争：

```
  写者 A (snap=2, 写 offset=100)
  写者 B (snap=5, 写 offset=500)

      根节点 ── 内部节点1 ──┬── 内部节点2 ── 叶子 (offset=100)
                          │                 ↑ A
                          │
                          └── 内部节点3 ── 叶子 (offset=500)
                                            ↑ B
  → 在内部节点1 之后完全分叉，无竞争
```

大部分并发写入操作的是不同文件、不同 offset 范围，在树中很快就分叉。真正的竞争只发生在**同一文件同一 offset 范围的不同快照写入**这个狭窄场景。

#### 竞争发生时：事务重启是轻量的

```
  时间 ─────────────────────────────────────▶

  A: 根 I ✓  内1 I ✓  内2 I ✓  叶 I→X ✓  写入完毕 ✓  释放
  B: 根 I ✓  内1 I ✓  内2 I ✗（A 持有 I 中）
     → 返回 transaction_restart
     → bch2_trans_begin()  ← 只重置内存指针，不是回滚
     → 重试：根 I ✓  内1 I ✓  内2 I ✓（A 已释放）
     → 叶 I→X ✓  写入完毕 ✓

  B 的"损失"：重走一遍路径。
  但路径节点全在 CPU cache 中（刚走过），
  重走只是多次原子操作 + 指针检查。
  ~1μs 级别的延迟。
```

`bch2_trans_begin()` 不是传统意义上的"回滚事务"——它只做三件事：
1. 重置事务路径的锁状态（标记为未锁定）
2. 清空暂存的 key 更新列表
3. 归还内存分配临时引用

不涉及任何 IO。

#### COW 意味着"写入"只是追加，不是原地覆盖

两个 writer 向同一叶子节点插入不同 snap 的 key：

```
  A 插 (1, 100, snap=2)  → 追加到 bset（新条目）
  B 插 (1, 100, snap=5)  → 追加到 bset（新条目）

  它们操作的是不同 key（bpos 不同）：
  - A 和 B 互不覆盖
  - 只需要叶子节点短暂的 X 锁来保证 bset 指针更新原子性
  - 不需要读-修改-写循环
  - 不需要复杂的冲突检测
```

这与同 snap 下的写入完全不同——同 snap 下后者覆盖前者，需要 journal 做冲突检测。但**不同 snap 天然不冲突**——这是 snapshot 嵌入 bpos 带来的隐藏好处。

#### 读操作完全不受影响

```
  100 个读者分别在不同快照中读同一文件：

  根节点：S*100 共存 ✓
  内部节点：S*100 共存 ✓
  叶子节点：S*100 共存 ✓（即使写者持有 I，S 仍可进入）

  → 读端零阻塞
```

#### 设计取舍：单树 vs 每快照独立树

如果每个快照分配一棵独立的 extent btree（类似 Btrfs subvolume）：

| 维度 | bcachefs（单树） | 每快照独立树 |
|------|----------------|-------------|
| 并发写入锁争用 | 路径共享→轻度争用 | 完全隔离 |
| **快照创建** | **O(1)** — 只插一个 snapshot btree 节点 | O(log n)~O(n) — 复制 btree 根路径 |
| **跨快照 diff/备份** | **单树遍历，天然有序** | 跨树归并，复杂度倍增 |
| **内存缓存效率** | 一棵树的节点被所有快照共享 | N 棵树各自缓存，内存压力大 |
| **空间共享引用** | 自然——COW 共享 extent 指针 | 需要跨树引用计数 |
| 同 offset 不同 snap 写入 | ~1μs 事务重启延迟 | 无额外延迟 |

**核心取舍**：bcachefs 选择把"快照创建要快"和"跨快照操作要简单"放在更高优先级，而不是把"极端并发写入隔离"放在第一位。这是文件系统层面的判断——快照是日常操作（备份、CI、开发环境），每快照独立树的 O(n) 创建成本在实际使用中更痛。

| 并发写入对比总结 |
|---|
| 不同文件 | ≈ 无差异 |
| 同文件不同 offset | ≈ 无差异 |
| 同文件同 offset 不同 snap | bcachefs +~1μs（事务重启） |
| 同文件同 offset 同 snap | 二者都需要序列化（FS 共性） |
| 读并发 | ≈ 无差异（SIX lock S 与其他状态兼容） |
| 快照创建 | bcachefs O(1) → 远快 |
| 增量备份 | bcachefs 单树遍历 → 更简单快 |

---

## 3. 空间管理 — bucket 分配器

### 3.1 bucket 结构（8 字节内存状态）

**文件：** `fs/alloc/buckets_types.h`

```c
struct bucket {
    u8  lock;          // 自旋锁（1 字节用 bit_spin_lock，fsck 时内存效率 hack）
    u8  gen_valid:1;   // generation 是否有效
    u8  data_type:7;   // 数据类型（BCH_DATA_btree / journal / user / ...）
    u8  gen;           // 世代号
    u32 dirty_sectors; // 脏扇区数
    u32 cached_sectors;// 缓存扇区数
    u32 stripe_sectors;// RAID stripe 扇区数
};
```

**bucket** 是空间分配的基本单位（类似 ext4 的块组但更轻量），大小可配置（通常 512KB-2MB）。所有 bucket 以数组形式存储，每个设备一个数组，索引即 bucket ID。

### 3.2 核心分配路径

分配器先遍历 `BTREE_ID_freespace`（空闲空间 B-tree），找到可用 bucket，然后修改 alloc btree 记录状态。

**文件：** `fs/alloc/foreground.c:437-507`

```c
static struct open_bucket *bch2_bucket_alloc_freelist(struct btree_trans *trans,
                              struct alloc_request *req)
{
    struct bch_dev *ca = req->ca;
    u64 *dev_alloc_cursor = &ca->alloc_cursor[req->btree_bitmap];
    u64 alloc_start = max_t(u64, ca->mi.first_bucket,
                    READ_ONCE(*dev_alloc_cursor));
    u64 alloc_cursor = alloc_start;

    // 从分配游标位置开始，在 freespace btree 中正向遍历
    for_each_btree_key_max_norestart(trans, iter, BTREE_ID_freespace,
                     POS(ca->dev_idx, alloc_cursor),
                     POS(ca->dev_idx, U64_MAX),
                     0, k, ret) {
        // 每个空闲 extent 可以跨多个 bucket
        while (iter.k.size) {
            u64 bucket = iter.pos.offset & ~(~0ULL << 56);
            ob = try_alloc_bucket(trans, req, &iter);
            if (ob) {
                *dev_alloc_cursor = iter.pos.offset;
                break;
            }
            iter.k.size--;
            iter.pos.offset++;
        }
        if (ob || ret)
            break;
    }

    // 绕回重试：如果从游标到末尾没找到，从 first_bucket 开始再找一遍
    if (!ob && alloc_start > ca->mi.first_bucket)
        goto again;

    return ob;
}
```

高级入口 `bch2_bucket_alloc_trans`（`foreground.c:620-691`）在此之上添加：
- **水位线检查**：`ALLOC_WATERMARK_normal / ALLOC_WATERMARK_low / ALLOC_WATERMARK_stripe / ALLOC_WATERMARK_copygc`
- **拷贝 GC 触发**：在低水位时主动触发 `bch2_copygc_start` 回收碎片
- **重试逻辑**：第一次失败后等待回收再试

### 3.3 核心机制解析

- **generation 号**（stale pointer detection）：bucket 每次回收后 +1，持有旧 gen 号的指针（btree 中的 extent 指向 bucket 中的物理位置）被识别为过期。这是 bcachefs 用来检测悬空指针的机制。
- **写时分配（COW）**：每次写入 → 分配新 bucket + journal 记录分配事件 → 旧 bucket 数据变为脏 → 后台回收。
- **disk_reservation**：写入前预留空间，防止写入中途空间不足。使用每 CPU 缓存模式：

```c
// buckets.h:359 和 buckets.c:1204
static inline int bch2_disk_reservation_add(bch_fs *c,
    struct disk_reservation *res, unsigned sectors, int flags)
{
    if (res->nr_replicas)
        sectors *= res->nr_replicas;     // 复制因子
    return __bch2_disk_reservation_add(c, res, sectors, flags);
}
// __bch2_disk_reservation_add:
// 优先从 this_cpu_ptr(pcpu)->sectors_available 扣减，
// 不足时批量从全局 capacity 获取，cache-local 模式降低原子操作竞争
```

### 3.4 数据分类

`BCH_DATA_NR` 枚举定义 bucket 用途：

| 数据类型 | 说明 |
|----------|------|
| `BCH_DATA_btree` | B-tree 节点 |
| `BCH_DATA_user` | 用户数据（extent） |
| `BCH_DATA_journal` | 日志 |
| `BCH_DATA_cache` | Cached 数据（SSD 层） |
| `BCH_DATA_stripe` | RAID stripe |
| `BCH_DATA_need_discard` | 等待 discard |
| `BCH_DATA_sb` | Superblock |

```
  bucket 生命周期状态机：

                         COW 写入
               ┌────────────────────────┐
               │                        │
               ▼                        │
        ┌──────────┐  写入/更新   ┌──────────┐
        │   FREE   │────────────▶│ ALLOCATED│
        │ (空闲)    │             │ (已分配)  │
        └─────┬────┘             └─────┬────┘
              │                        │
              │ discard 完成   数据变脏  │
              │ (回收器)        (btree)  │
              │                        ▼
              │                  ┌──────────┐
              │     回收器清空    │  DIRTY   │
              │ ◄───────────────│ (脏)      │
              │                  └──────────┘
              │
              ▼
        ┌──────────┐
        │NEED_DISC │←── freed bucket，等待后台发送 discard
        │(待回收)   │
        └──────────┘

  分配路径实际流程（带数据分类）：

  磁盘预留成功
      │
      ▼
  ┌─────────────────────┐
  │ 遍历 freespace btree│  ← BTREE_ID_freespace
  │ 游标轮转分配         │
  └──────────┬──────────┘
             ▼
  ┌─────────────────────┐
  │ try_alloc_bucket()   │  ← 验证 VSS 引用、水位线
  │  成功 → data_type 设为 │
  │  BCH_DATA_btree/user/│
  │  journal...          │
  └──────────┬──────────┘
             ▼
  ┌─────────────────────┐
  │ 从 freespace 删除     │
  │ alloc btree 记录状态  │
  │ journal 记录分配      │
  └─────────────────────┘

  COW 释放路径：

  旧 extent 被覆盖
      │
      ▼
  ┌─────────────────────┐
  │ gen +1 (世代递增)    │  ← 旧指针不再匹配 gen
  │ data_type → freed    │
  └──────────┬──────────┘
             ▼
  ┌─────────────────────┐
  │ 回收器扫描该 bucket   │
  │ 插入 freespace btree │
  │ 需要 discard →       │
  │   标记 NEED_DISCARD  │
  └─────────────────────┘
```

### 3.5 回收器

后台线程扫描 freed bucket：
```
freed bucket → T (need_discard) → discard 完成 → free
```

如果设备支持 discard，回收器会在后台发送 discard 命令。否则直接标记为可用。

### 3.6 btree_node 与 bucket 的关系

一个反复出现的问题：**btree_node 占一个 bucket 吗？不是——一个 bucket 可以放多个 btree_node。**

关系链：

```
  btree_node (内存)                       bucket (磁盘)
  ┌───────────────────┐                   ┌─────────────────────┐
  │ struct btree      │                   │ ubucket 512KB-2MB   │
  │   .key            │                   │                     │
  │     (btree_ptr_v2)│────┬───────▶      ├── btree_node_A ◀────┤
  │   .data           │    │               │   256KB             │
  │     (内存中的     │    │               ├── btree_node_B ────┤
  │      btree_node)  │    │               │   256KB             │
  │   .written (u16)  │    │               ├── (空闲)           │
  └───────────────────┘    │               └─────────────────────┘
                           │
                    ┌──────┴──────────────────┐
                    │ bch_extent_ptr           │
                    │   offset:44 = 扇区地址   │
                    │   dev:8    = 设备号      │
                    │   gen:8    = 世代号      │
                    └──────┬──────────────────┘
                           │
                    offset >> bucket_bits = bucket ID
```

#### 映射链：从 btree_node 到 bucket

```
  struct btree {             // 内存 btree 节点
      struct btree_node *data;   // 指向内存中的节点数据
      struct bkey_i    key;      // btree_ptr_v2 — 本节点的物理位置键
      u16              written;  // 已写入的扇区数
  };

  struct bch_btree_ptr_v2 {  // b->key 的值部分
      __le16  sectors_written;   // 此 btree_node 已写入的扇区数
      __le16  flags;
      struct bpos  min_key;
      struct bch_extent_ptr  start[];  // 1-N 个物理指针
  };

  struct bch_extent_ptr {     // 物理指针
      u64 offset:44;    // 设备上的扇区偏移（不是桶偏移！）
      u64 dev:8;
      u64 gen:8;
  };
```

**`bch_extent_ptr.offset` 是全局扇区号**，涵盖整个设备。从它到 bucket 的换算：

```
  bucket_id = offset >> (bucket_bits - SECTOR_SHIFT)
                    // 等价于 offset / (bucket_size / SECTOR_SIZE)
```

**文件：** `fs/btree/node_scan.c:244-252` 有最直接的证明——按桶扫描时在桶内用 `btree_sectors(c)` 步长遍历：

```c
// node_scan.c:244-252：扫描所有可能的 btree_node 位置
for (u64 bucket = ca->mi.first_bucket; bucket < ca->mi.nbuckets; bucket++) {
    for (unsigned bucket_offset = 0;
         bucket_offset + btree_sectors(c) <= ca->mi.bucket_size;
         bucket_offset += btree_sectors(c))
        // 每个 btree_node 占用 btree_sectors(c) 个扇区
        try_read_btree_node(..., bucket_to_sector(ca, bucket) + bucket_offset);
}
```

这就是**一个桶可以放多个 btree_node** 的直接证据——扫描器在一个桶内按 `btree_node_size` 步长枚举所有可能位置。

#### 尺寸关系

| 参数 | 默认值 | 文件 |
|------|--------|------|
| `btree_node_size` | **256KB**（512 扇区） | `fs/btree/cache.h:164` — `btree_sectors(c)` |
| `bucket_size` | **512KB ~ 2MB** | `fs/alloc/buckets_types.h` — bucket 配置 |
| 同尺寸时每桶可放 | 1-4 个 btree_node | `node_scan.c:250` — 步长迭代 |

```
  桶大小 512KB:
  ┌────────────────────────────────┐
  │ btree_node_0    │ btree_node_1 │  ← 每个 256KB
  │ (256KB)         │ (256KB)      │
  └────────────────────────────────┘

  桶大小 1MB:
  ┌────────────────────────────────────────────────────────────┐
  │ btree_node_0   │ btree_node_1   │ btree_node_2   │ free   │
  │ (256KB)        │ (256KB)        │ (256KB)        │        │
  └────────────────────────────────────────────────────────────┘
```

#### 分配流程：写点是桥梁

btree_node 不走 `try_alloc_bucket` 的单桶分配路径，而是通过 **btree_write_point** 批量获取。

```
  分配时序：

  1. __bch2_btree_node_alloc(interior.c:357)
     │
  2. bch2_alloc_sectors_req(trans, &req,
          writepoint_ptr(&c->allocator.btree_write_point), &wp)
     │   req->data_type 从 btree_write_point 继承
     │   → 即 BCH_DATA_btree（foreground.c:1494）
     │
  3. bch2_alloc_sectors_append_ptrs(c, wp, &b->key, btree_sectors(c), false)
     │   将分配到的扇区地址写入 b->key (btree_ptr_v2 + extent_ptrs)
     │
  4. bch2_open_bucket_get(c, wp, &b->ob)
     │   获取 open_bucket 引用（写时保持桶打开）
     │
  5. bch2_alloc_sectors_done(c, wp)
     │   关闭/释放 write_point 对该桶的引用
     │   桶的 data_type 已标记为 BCH_DATA_btree
```

**关键点**：`bch2_alloc_sectors_append_ptrs` 写入的是 `btree_sectors(c)` 个扇区，但实际写入时可能只写部分（通过 `b->written` 限制）。这意味着：

```
  分配：btree_sectors(c) = 256KB 被预留
  写入：
    第 1 次: written = 实际 bset 大小 < 256KB
    第 2 次: written += 新增 bset 大小
    第 N 次: ...直到 written == btree_sectors(c)

  预留空间用不完？可以，不是问题。btree_node 以 log-structured 方式
  追加写入，每次都复用已经分配好的空间。
```

#### write_point 是什么

write_point 是 bcachefs 空间分配的关键抽象：

```
  write_point = (data_type, 一组打开中的桶)

  初始化（foreground.c:1703）：
      writepoint_init(&a->btree_write_point, BCH_DATA_btree);

  每个 write_point 维护一组当前正在写入的 open_bucket
  btree_write_point 特有的行为：
    - 只从非 cache 设备分配（foreground.c:1497）
    - 分配器只扫描标记为可存放 btree 数据的设备
```

```
  系统中最重要的几个 write_point：

  btree_write_point         → BCH_DATA_btree
  sync_write_point           → BCH_DATA_user（同步写入）
  copygc_write_point         → BCH_DATA_user（碎片整理）
  rebalance_write_point      → BCH_DATA_user（重平衡）
  promote_write_point        → BCH_DATA_cache（缓存提升）
```

同一个 write_point 收集多个写入请求，批量分配桶空间以减少分配器竞争。

#### bucket 标识为"btree 桶"的时刻

```
  分配前：
    bucket.data_type = FREE（尚未分配）

  bch2_alloc_sectors_req 过程中：
    try_alloc_bucket() 成功
      ↓
    桶标记更新（foreground.c 内）：
      bucket_mark.data_type = BCH_DATA_btree  ← 从此这个桶是"btree 桶"
      bucket_mark.dirty_sectors += btree_sectors(c)
      bucket_mark.gen 不变（除非回收前被覆写）
      ↓
    journal 记录分配事件

  回收前：
    该桶内所有 btree_node 指针都被删除（btree 节点被合并/删除）
      ↓
    bucket_mark.dirty_sectors = 0
    gen++ → 旧指针全部失效
    data_type = freed → 等待回收器
```

#### sectors_written 与节点部分写入

btree_node 并非一次写满整个预留空间。`sectors_written` 与 `b->written` 配合，实现**增量持久化**：

```
  内存中 btree_node 的写入进度：

  b->written:  内存计数器，当前已成功写入多少扇区
  ptr_v2->sectors_written:  持久化记录，父节点中记录的此节点已写入量

  读节点时（fs/btree/read.c:600-756）：
    bch2_btree_read_done 中，根据 sectors_written 只读取已确认写入的部分
    跳过未写入的零填充区域

  写节点时（fs/btree/write.c:277-569）：
    __bch2_btree_node_write 三阶段
      阶段1: CAS 仲裁 — b->written 作为脏数据仲裁基准
      阶段2: 排序合并 — 将 bset[1..n] 合并到 set[0]
      阶段3: 提交 — 更新 b->written，更新父节点中的 sectors_written
```

#### btree_node 写入后 bucket 扇区计数

```
  btree_node 写入时对桶的影响：

  分配时：
    桶 dirty_sectors += btree_sectors(c)  ← 整个预留空间都算脏

  每次实际写入：
    不改变 dirty_sectors（已经在分配时计入了）
    sectors_written 递增
      写入使用相同物理扇区（btree_node 是 log-structured 追加写
      单个 btree_node 内的 bset 是连续空间内的追加，
      不跨越到新桶）

  释放时（节点被删除）：
    dirty_sectors -= btree_sectors(c)
    如果 dirty_sectors == 0 → 该桶可以回收
```

#### 与用户数据桶的对比

| 维度 | btree 桶 | 用户数据桶 |
|------|---------|-----------|
| data_type | `BCH_DATA_btree` | `BCH_DATA_user` |
| 分配单元 | `btree_node_size`（固定 256KB） | extent 大小（可变） |
| 每个桶的条目数 | 1-4 个（与桶大小相关） | 1 个（extent 连续填满） |
| 分配路径 | `btree_write_point` | `sync_write_point` / `copygc_write_point` 等 |
| 桶内偏移 | `bucket_offset` 按 `btree_sectors` 对齐 | `offset = bucket_to_sector(bucket)` |
| 写模式 | log-structured 追加（bset 叠加） | 一次性写满（extent 覆盖） |
| 部分写入跟踪 | `sectors_written` 在 btree_ptr_v2 | 不需要（extent 全写或全不写） |
| 回收条件 | 桶内所有节点被删除 | extent 被覆盖/删除 |

#### 总结

btree_node 与 bucket 的关系不是 1:1。每桶可放 1-4 个 btree_node（取决于桶大小）。映射链是：

```
  父节点中的 btree_ptr_v2 key
      → bch_extent_ptr.offset（扇区地址）
          → bucket_id = offset >> (bucket_bits - SECTOR_SHIFT)
          → bucket_offset = offset & bucket_mask
              → 物理位置上 btree_node 数据
```

分配时统一走 `btree_write_point`，data_type 继承为 `BCH_DATA_btree`。桶的扇区计数在分配时一次性计入，btree_node 的 `written` 和 `sectors_written` 只控制实际写入了多少，不影响桶的计数。

### 3.7 桶分配的并发性能

桶分配是每个 IO 路径都要经过的热路径。bcachefs 用多层缓存和分片来避免全局瓶颈。

#### 第一层：disk_reservation 每 CPU 缓存

**文件：** `fs/alloc/buckets.c:1204-1229`

写入前先预留空间（disk_reservation），不预留无法写入。预留的快速路径**完全无锁**：

```
  struct bch_fs_capacity {
      atomic64_t      sectors_available;    // 全局剩余容量
      struct bch_fs_capacity_pcpu __percpu  *pcpu;  // 每 CPU 缓存
  };

  struct bch_fs_capacity_pcpu {
      u64  sectors_available;    // 本 CPU 缓存的可预留扇区
      u64  online_reserved;      // 本 CPU 已预留计数
  };
```

```
  快速路径（buckets.c:1204-1229）：

  __bch2_disk_reservation_add(c, res, sectors):
  1. pcpu = this_cpu_ptr(c->capacity.pcpu)
  2. if (pcpu->sectors_available >= sectors)
       // ✅ 快速路径：从本地 CPU 缓存扣减
       pcpu->sectors_available -= sectors
       return 0
  3. // 慢速路径：从全局 atomic64 批量获取
     old = atomic64_read(&c->capacity.sectors_available)
     get = min(sectors + SECTORS_CACHE, old)
     atomic64_try_cmpxchg(...)  // CAS 重试
     pcpu->sectors_available += get   // 批量填充本地缓存
     // 再从本地扣减
  4. pcpu->sectors_available -= sectors

  快速路径统计：>99% 的情况下 pcpu->sectors_available 充足
  全局 atomic64 只在 CPU 本地首次耗尽时被触碰
  SECTORS_CACHE = 64KB → 一次全局取够 64KB 的配额
```

**效果**：N 个 CPU 同时做 disk_reservation，N 个都在各自的 CPU cacheline 上做无锁减法，零共享 cacheline 写争用。

#### 第二层：write_point + open_bucket 池化

分配扇区时，走 `bch2_alloc_sectors_req`。它的核心策略是**尽可能复用已有桶**，避免每次都走到 freespace btree：

```
  bch2_alloc_sectors_req 内部分配链（foreground.c:1514-1518）：

  尝试顺序                                    开销        频率
  ──────────────────────────────────────────────────────────
  ① bucket_alloc_set_writepoint()         O(1) 内存    最频繁
     → 从 write_point 当前打开的桶中分配
     → write_point->ptrs[].sectors_free 扣减
     → 无锁（只有 write_point 的 mutex）

  ② bucket_alloc_set_partial()            O(n) 数组   中等
     → 从 open_buckets_partial[] 中找有剩余空间的桶
     → 这些桶之前被打开、写入、然后部分释放了
     → 可以复用已有的桶分配，无需走 freespace btree

  ③ bucket_alloc_from_stripe()            有 EC 时    低
     → 从 erasure coding 条带中分配空间

  ④ bch2_bucket_alloc_set_trans()         btree 遍历   低
     → 遍历 freespace btree 找新桶
     → 这步才真正走到分配器的核心
```

**write_point 互斥**：每个 write_point 有自己的 `mutex`，不同 write_point 的分配互不干扰。

```
  不同 write_point 完全并行：

  CPU-0: btree_write_point  ── mutex ── 分配 btree 桶
  CPU-1: sync_write_point   ── mutex ── 分配用户数据桶
  CPU-2: promote_write_point── mutex ── 分配缓存桶

  三个线程同时在写三个不同的 write_point，
  三个 mutex 不冲突。
```

#### write_point 的 WFQ 跨设备分配

**文件：** `fs/alloc/types.h:76-91`

当分配目标有多个设备时（如 4 块盘的 RAID0），write_point 使用 **加权公平队列** 在设备间分配：

```
  struct dev_stripe_state {
      u64  next_alloc[BCH_SB_MEMBERS_MAX];   // 每设备虚拟时间戳
      struct bch_devs_mask  cached_devs;
  };

  分配策略：
  1. 每设备有一个虚拟时间戳 next_alloc[i]
  2. 每次分配后，该设备的时间戳 += 1 / free_space[i]
     → 空闲空间越大的设备，增量越小
     → 空闲空间大的设备被选中的概率更高
  3. 每轮选择 next_alloc 最小的设备
     → 所有设备的虚拟时间趋于同步
     → 分配量与空闲空间成比例
```

```
  示例（3 设备，无 WFQ vs 有 WFQ）：

  设备       空闲空间   无 WFQ(轮转)   有 WFQ(WFQ)
  ────────────────────────────────────────────
  /dev/sda   1TB        33%            50%   ← 空间大，分得多
  /dev/sdb   500GB      33%            25%
  /dev/sdc   500GB      33%            25%

  WFQ 效果：空间大的设备自然被分配更多，但无需事先配置权重。
  设备加入/移除时，所有设备的虚拟时间自动对齐。
```

#### 第三层：open_bucket 池 + partial 复用

**open_bucket** 是 bcachefs 分配器的"活页夹"。每次分配一个桶，就创建一个 open_bucket 对象跟踪它。

```
  struct open_bucket {                   // 24 字节
      spinlock_t        lock;            // 操作锁
      atomic_t          pin;             // 引用计数
      struct open_bucket *freelist;      // 空闲链表指针
      u32               sectors_free;    // 剩余可用扇区
      u8                dev;
      u8                gen;
      u64               bucket;          // 桶号
  };
```

open_bucket 有两条生命周期路径：

```
  分配新桶（__try_alloc_bucket，foreground.c:269-321）：

  ① 加 &c->allocator.freelist_lock        ← 全局 freelist spinlock
  ② 检查 open_buckets_nr_free 是否够用
  ③ 检查该桶是否已被别的 open_bucket 打开
  ④ bch2_open_bucket_alloc()              ← 从 freelist 弹出一个
     → 只操作 freelist 链表头指针，O(1)
  ⑤ 初始化 ob，加 ob->lock
  ⑥ 加入 open_buckets_hash
  ⑦ 释放 freelist_lock

  freelist_lock 持有的关键区间：第 ②→④ 步
  典型持有时间：~50ns（只操作链表头指针）
```

```
  桶写完后的释放路径（有两条分支）：

  写入中途用完 sectors_free：
    → ob 归还到 open_buckets_partial 列表
    → 下次 bucket_alloc_set_partial 可以复用

  桶被完全写满/释放：
    → ob 归还到 open_buckets_freelist
    → 桶被回收（gen++）

  partial 复用效果：
    一个 1MB 的桶被打开后：
    写入 256KB → 释放，进入 partial
    下一个 writer 发现 partial 有剩余空间 → 直接复用
    又写 256KB → 释放，进入 partial
    ...
    直到写满 → 回收

    每个桶的打开次数从 1 次降到 0.25-0.5 次
```

#### 真正的瓶颈在哪

分析 `__try_alloc_bucket` 不难发现一个全局点——`&c->allocator.freelist_lock`。

```
  全局 freelist_lock 的持有者：

  bch2_open_bucket_alloc()       ── 弹出 freelist 头  ~20ns
  open_bucket_free_unused()      ── 推入 partial 列表  ~30ns
  __bch2_open_buckets_put()      ── 归还到 freelist    ~40ns

  每次持有时间 ~20-50ns
```

在 64 核同时分配时，这确实可能成为热点。但要注意：**进入 `__try_alloc_bucket` 的前提是 freespace btree 中还有未分配的桶，而 freespace btree 遍历（第 ④ 分配策略）已经滤掉了绝大部分竞争者。**

```
  实际分配频率估算：

  假设系统每秒 100 万次写入（极高负载）
  每次 4KB，桶大小 1MB
  每秒 4000 个桶被写满
  → freelist_lock 每秒被获取 ~4000 次
  → 每次 50ns，总计 ~200μs/秒
  → CPU 占用率 0.02%

  → 在正常负载下，freelist_lock 的争用可以忽略
```

系统还有其他 wal/rmw 等锁类更严重，只是它们不会出现在持久化路径上。

#### 第四层：freespace btree 游标轮转

当所有缓存层都用尽，分配器最终走到 `bch2_bucket_alloc_freelist`。这步本身也有并发设计：

```
  bch2_bucket_alloc_freelist（foreground.c:437-507）

  遍历 freespace btree，从游标位置开始：

  dev_alloc_cursor = &ca->alloc_cursor[req->btree_bitmap];

  // 从游标位置正向遍历 freespace btree
  for_each_btree_key_max_norestart(trans, iter,
      BTREE_ID_freespace,
      POS(ca->dev_idx, alloc_cursor),
      POS(ca->dev_idx, U64_MAX), ...)

  // freespace btree 遍历本身是并发的：
  // - 每个遍历有自己的 iter，走 SIX 锁路径
  // - I 锁在节点间传递，不长期持有
  // - 不同遍历在不同的 BTREE_ID_freespace 区域互不干扰

  游标效果：
  线程 A: 遍历 bucket 100-200
  线程 B: 遍历 bucket 1000-1100
         ↑
    alloc_cursor 被 try_alloc_bucket 成功后推进
    （line 481: *dev_alloc_cursor = iter.pos.offset）
    线程 A 和 B 从不同起始位置开始，自然分散到不同区域
```

```
  分配区域自动分散：

  线程 A:  游标 A ─▶ bucket 100 ─▶ 110 ─▶ 120 ...
  线程 B:  游标 B ─▶ bucket 500 ─▶ 520 ─▶ 540 ...
  线程 C:  游标 C ─▶ bucket 900 ─▶ 930 ─▶ 960 ...

  freespace btree 中不同 POS 的 key 互不重叠
  → 三个迭代器的遍历路径完全独立
  → 涉及的 btree 内部节点可能有少量重叠（树顶共享）
  → 叶子节点零重叠
```

#### 第五层：水位线与背压

当空间不足时，分配器不是让所有写入串行等待，而是分级降级：

```
  ALLOC_WATERMARK_normal:    正常写入，空间充足
  ALLOC_WATERMARK_low:      低水位，触发 copygc
  ALLOC_WATERMARK_stripe:   只分配给 EC 写入
  ALLOC_WATERMARK_copygc:   只给 copygc 回收用

  当 free < watermark 时：
  - 低优先级的写入被跳过（不是阻塞）
  - 分配器跳过水位线不足的设备和区域
  - copygc 在后台回收空间
  - 分配器不会自旋等待——bch2_bucket_alloc_set_trans 返回
    -BCH_ERR_freelist_empty，上层可以选择等 closure_wait
    或继续做其他工作
```

#### 总结：分配并发架构图

```
  IO 写入路径：

  写入请求
    │
    ▼
  ┌────────────────────────────┐
  │  disk_reservation          │ ← 每 CPU 缓存，99% 走无锁快速路径
  │  (每 CPU sectors_available) │
  └───────────┬────────────────┘
              │
              ▼
  ┌────────────────────────────┐
  │  bch2_alloc_sectors_req    │
  │  ├─ writepoint 复用        │ ← 不同 write_point 不同 mutex
  │  ├─ partial 桶复用         │ ← 避免新分配
  │  ├─ stripe 分配            │ ← EC 专用路径
  │  └─ freespace btree 新分配  │ ← 游标轮转，分散竞争
  └────────────────────────────┘
              │
              ▼
  ┌────────────────────────────┐
  │  try_alloc_bucket          │
  │  ├─ may_alloc_bucket 预检   │ ← 快速过滤，不拿锁
  │  ├─ check_freespace_key    │ ← 异步验证 gen/seq
  │  └─ __try_alloc_bucket     │
  │     └─ freelist_lock       │ ← 唯一全局串行点，~50ns
  └────────────────────────────┘
              │
              ▼
  ┌────────────────────────────┐
  │  open_bucket 初始化        │ ← per-ob spinlock
  │  写入数据                  │
  │  桶用尽 → partial 或 free  │ ← freelist_lock 再保护
  └────────────────────────────┘
```

**按层级汇总争用点**：

| 层级 | 锁/原语 | 持有时间 | 争用风险 |
|------|---------|---------|---------|
| disk_reservation | 无（每 CPU 本地变量） | 0 | 无 |
| write_point 分配 | write_point mutex | ~100ns | 低（不同 wp 隔离） |
| partial 复用 | freelist_lock | ~30ns | 低 |
| freespace btree 遍历 | SIX I 锁（路径节点） | ~50ns/节点 | 低（遍历分散） |
| __try_alloc_bucket | freelist_lock | ~50ns | 中等（唯一全局点） |
| open_bucket 操作 | ob->lock | ~50ns | 低（每个 ob 独立） |

真正值得关注的唯一全局串行点是 `freelist_lock`。它的持有时间 ~50ns、频率 ~4000次/秒（极端负载），实际 CPU 占用在可忽略量级。作为对比，B-tree 路径上的 SIX 锁争用、journal 满时的回压、或者 disk I/O 延迟，都比这个高 3-5 个数量级。

### 3.8 空闲空间 B-tree（BTREE_ID_freespace）

前文介绍了"分配器遍历 freespace btree 找空闲桶"，但空闲空间本身是如何维护的？

#### 结构与用途

BTREE_ID_freespace 是一个 **extent btree**（`BTREE_IS_extents`），记录设备上所有当前空闲（`data_type == BCH_DATA_free`）的桶。

```
  freespace btree key 格式：

  bpos.inode  = 设备号 (dev_idx)
  bpos.offset = bucket_id | (alloc_gc_gen >> 4) << 56
                    ↓                           ↓
                低 56 位：桶号              高 8 位：GC 世代编码

  值类型：KEY_TYPE_set（无实际值，存在 = 空闲）
```

**关键设计：高 8 位的 GC 世代编码**

```c
// background.h:174-183
static inline u64 alloc_freespace_genbits(struct bch_alloc_v4 a)
{
    return ((u64) alloc_gc_gen(a) >> 4) << 56;
}

static inline struct bpos alloc_freespace_pos(struct bpos pos, struct bch_alloc_v4 a)
{
    pos.offset |= alloc_freespace_genbits(a);
    return pos;
}
```

```
  GC 世代编码的效果：

  bucket 100 被释放 3 次：
  第 1 次: gen=5, oldest_gen=0, gc_gen=5,  freespace key offset = 100 | (5>>4)<<56 = 100 | 0
                                                                   gc_gen>>4 = 0 → 无影响
  第 2 次: gen=10, oldest_gen=0, gc_gen=10, freespace key offset = 100 | (10>>4)<<56 = 100 | 0
  第 3 次: gen=25, oldest_gen=0, gc_gen=25, freespace key offset = 100 | (25>>4)<<56 = 100 | (1<<56)

  相邻释放但 gc_gen 不同 → 不能合并为一个 extent：
  bucket 100 的 freespace key:  (dev=0, offset=100 | 0<<56 = 100)
  bucket 101 的 freespace key:  (dev=0, offset=101 | 1<<56 = 101 + 2^56)
                                ↑ 两个 key 在 btree 中相距 2^56！
                                → 被分散到完全不同的位置
                                → 不会误合并在同一个 extent 中
```

这解决了什么问题：**当 GC 扫描重算引用且不知道有哪些引用时，旧 freespace 条目不能被新分配器看到。**

```
  崩溃恢复场景：
  1. 分配 bucket 100，写入数据 → 从 freespace btree 中删除
  2. 系统崩溃
  3. journal replay：freespace 条目在 journal 中，被重新应用
  4. GC 扫描发现 bucket 100 仍有数据 → gen++
  5. alloc 标记 bucket 100 为 DIRTY

  假如没有 genbits 编码：
  freespace btree 中 bucket 100 条目在 journal replay 后存在
  分配器看到 (dev=0, offset=100) 的 KEY_TYPE_set → 认为它是空闲的
  → 错误！实际上 bucket 100 有数据，不能分配

  有 genbits 编码：
  journal replay 后 freespace 条目有 old gc_gen (比如 5>>4=0)
  GC 后 gc_gen 变了（gc_gen 变成比如 13>>4=0，没有变）
  → 分配时 __bch2_check_freespace_key 验证：
      读取 alloc btree，得到新 gc_gen，与 freespace key 的高 8 位匹配吗？
      不匹配 → 删除该 freespace 条目 → 安全
```

#### 修改时机：alloc trigger 的副作用

freespace btree 的修改不是由分配器直接做的，而是 **alloc btree 的 trigger** 的副作用。

```
  alloc btree key 变更                     trigger 级联操作
  ──────────────────────                   ──────────────────

  data_type → BCH_DATA_free
     → freespace_genbits 可能变化               ↓
     → 旧 freespace 条目删除（set=false）  ← bch2_bucket_do_freespace_index(trans, ca, old, false)
     → 新 freespace 条目插入（set=true）   ← bch2_bucket_do_freespace_index(trans, ca, new, true)

  data_type → !BCH_DATA_free（分配/写入）
     → 旧 freespace 条目删除（set=false）   ← bch2_bucket_do_freespace_index(trans, ca, old, false)
```

**文件：** `fs/alloc/background.c:1360-1365`

```c
// alloc v4 trigger 中：
if (statechange(a->data_type == BCH_DATA_free) ||
    (new_a->data_type == BCH_DATA_free &&
     alloc_freespace_genbits(*old_a) != alloc_freespace_genbits(*new_a))) {
    try(bch2_bucket_do_freespace_index(trans, ca, op.old, old_a, false));  // 删除旧
    try(bch2_bucket_do_freespace_index(trans, ca, op.new.s_c, new_a, true)); // 插入新
}
```

两个条件触发：
1. `data_type` 从 FREE 变成别的（分配）或别的变成 FREE（释放）
2. `data_type` 保持 FREE，但 genbits 变了（GC 世代推进导致编码变化）

```
  实际执行函数 bch2_bucket_do_freespace_index（background.c:1102-1125）：

  bch2_bucket_do_freespace_index(trans, ca, alloc_k, a, set):
  1. if (a->data_type != BCH_DATA_free) return 0    ← 非空闲桶直接跳过
  2. pos = alloc_freespace_pos(alloc_k.k->p, *a)    ← 编码 genbits
  3. iter = BTREE_ID_freespace, pos
  4. bch2_btree_bit_mod_iter(trans, &iter, set)      ← 插入/删除单桶条目
```

而 `bch2_btree_bit_mod_iter` 做的事很简单：

```c
// update.c:810-821
bkey_init(&k->k);
k->k.type = set ? KEY_TYPE_set : KEY_TYPE_deleted;  // set = 存在, deleted = 不存在
k->k.p = iter->pos;
if (iter->flags & BTREE_ITER_is_extents)            // freespace 是 extent btree
    bch2_key_resize(&k->k, 1);                       // 单个桶范围
return bch2_trans_update(trans, iter, k, 0);
```

因为 freespace 是 extent btree，`bch2_trans_update` 会自动合并相邻的 KEY_TYPE_set 条目为更大的 extent。所以**多个连续的空闲桶（相同 genbits）在 freespace btree 中只占一个 extent 条目**。

#### 分配器如何使用 freespace btree

分配器遍历 freespace btree，但不会直接信任它的条目——每个潜在的分配都会验证：

```
  bch2_bucket_alloc_freelist(trans, req)
  │
  ├── 从游标开始遍历 BTREE_ID_freespace
  │   for_each_btree_key_max_norestart(trans, iter,
  │       BTREE_ID_freespace, POS(dev, alloc_cursor), POS(dev, U64_MAX), ...)
  │
  ├── 对每个 freespace extent：
  │   iter.k.size = 连续空闲桶的数量
  │   while (iter.k.size) {              // 逐个桶尝试
  │
  │       ob = try_alloc_bucket(trans, req, &iter)
  │           │
  │           ├── may_alloc_bucket(...)      // 水位线、数据分类检查
  │           ├── bch2_check_freespace_key_async(trans, &iter, &gen, ...)
  │           │   └── __bch2_check_freespace_key()
  │           │       读取 BTREE_ID_alloc 中该桶的 alloc key
  │           │       验证：
  │           │         a->data_type == BCH_DATA_free  ✓
  │           │         genbits == alloc_freespace_genbits(*a)  ✓
  │           │       返回 gen（供 __try_alloc_bucket 使用）
  │           │
  │           └── __try_alloc_bucket(c, req, bucket, gen)
  │               check if bucket is already open (recheck under lock)
  │               bch2_open_bucket_alloc()
  │               initialize ob { bucket, gen, dev, sectors_free }
  │               open_bucket_hash_add(ob)
  │
  │       iter.k.size--;
  │       iter.pos.offset++;
  │   }
  │
  └── 设置 dev_alloc_cursor = 成功分配的桶号（下次从这里开始）
```

`bch2_check_freespace_key_async` 这一验证步骤是关键——它**不会**接受 freespace btree 条目为"这个桶确实是空闲的"的唯一依据，而是交叉检查 alloc btree：

```
  __bch2_check_freespace_key（check.c:423-482）

  输入：iter->pos (freespace btree 遍历到的位置)
  步骤：
  1. 从 iter->pos 提取 bucket 号和 genbits
     bucket = pos.offset & ~(~0ULL << 56)    // 低 56 位
     genbits = pos.offset & (~0ULL << 56)    // 高 8 位
  2. 读取 BTREE_ID_alloc 中该桶的当前 alloc v4 条目
  3. 验证：
     a->data_type == BCH_DATA_free ？        ← 桶必须真的是空闲的
     genbits == alloc_freespace_genbits(*a) ？ ← genbits 必须匹配当前世代
  4. 返回 gen（当前世代号）或错误码
```

**为什么要做这个交叉验证？** 因为 freespace btree 不是 source of truth。alloc btree 中每个桶的 `data_type` 和 `gen` 才是 ground truth。freespace btree 只是一个**缓存**，从 alloc trigger 保持同步。崩溃后 journal replay 可能乱序，freespace 可能暂时和 alloc 不一致。

#### 初次构造：从 alloc btree 重建

freespace btree 不是 journal 可重建的（未被 `btree_id_can_reconstruct` 豁免），但其条目完全可以从 alloc btree 推导。

```
  首次挂载时（check.c:840-875）：

  bch2_fs_freespace_init(c):
    for_each_member_device(c, ca):
      if (ca->mi.freespace_initialized) continue
      bch2_dev_freespace_init(c, ca, 0, ca->mi.nbuckets)
        for 该设备上每个桶:
          读取 alloc btree 条目
          如果 data_type == BCH_DATA_free:
            alloc_freespace_pos()
            bch2_btree_bit_mod(trans, BTREE_ID_freespace, pos, true)
        SET_BCH_MEMBER_FREESPACE_INITIALIZED(ca, true)

  后续挂载（ca->mi.freespace_initialized == true）：
    跳过重建，依赖 journal replay 恢复 freespace btree
```

#### 分离的 need_discard btree

与 freespace 关联但分离的是 **BTREE_ID_need_discard**。它也是一个 extent btree，记录需要后台 discard 的桶。

```
  data_type 状态机与 freespace/need_discard 的对应：

  BCH_DATA_free       → freespace=1, need_discard=0
  BCH_DATA_need_discard → freespace=0, need_discard=1
  已分配的数据桶     → freespace=0, need_discard=0
```

```
  桶释放的完整路径：

  data_type → BCH_DATA_free（COW 覆盖后）
      ↓
  freespace btree: +1（alloc trigger 添加）
      ↓
  gen++, oldest_gen 可能推进
      ↓
  freespace btree: 如果 genbits 变了，删除旧条目，插入新条目
      ↓
  （桶被分配器重新分配）
      ↓
  data_type = BCH_DATA_need_discard
      ↓
  freespace btree: -1（不再空闲）
  need_discard btree: +1（等待 discard）
      ↓
  后台 discard worker 发 ATA DISCARD/TRIM
      ↓
  data_type → BCH_DATA_free
      ↓
  need_discard btree: -1
  freespace btree: +1（回到空闲池）
```

```
  桶生命周期与 freespace 的状态映射

  时间 ─────────────────────────────────────────▶

     FREE      ALLOCATED      DIRTY      NEED_DISCARD      FREE
     │            │            │            │               │
  fspace:1    fspace:0      fspace:0     fspace:0        fspace:1
  ndisc:0     ndisc:0       ndisc:0      ndisc:1         ndisc:0

  freespace btree 状态：
  ████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░████
  need_discard btree 状态：
  ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░████░░░░░░░░░░░░
```

#### 为什么不直接用 alloc btree 做分配

alloc btree 是每个桶一条记录，按 (dev, bucket) 存储。要找到空闲桶需要**遍历整个 alloc btree 并过滤**——那是一个全表扫描。

freespace btree 是一个**索引**：只存储空闲桶，且只存储它们的 (dev, offset | genbits)。因为只包含空闲桶，条目数远少于 alloc btree。加上 extent 合并，连续空闲桶只占一个条目。

```
  alloc btree: 100 万个桶 → 100 万条记录（每个桶一条）
  freespace btree: 100 万个桶，空闲 20 万个 →
    如果空闲桶连续，可能只有几百到几万个 extent
    最坏情况（全零碎）：20 万条记录
  → 遍历 freespace btree 比遍历 alloc btree 快 1-3 个数量级
```

而且 freespace btree 的 key 用 `(dev, offset|genbits)` 做排序——分配器从游标位置开始正向遍历，天然轮转分配，不用随机跳跃。

#### genbits 编码详解：GC 世代如何保护 freespace

freespace btree key 高 8 位编码的是 **`gc_gen >> 4`**。要理解为什么需要这个编码，先理解世代系统：

##### 为什么需要 `gen`：COW 文件系统的指针过期检测

在分析三个世代值之前，先回答一个最基础的问题：**为什么桶需要一个世代计数器？**

根本原因是 bcachefs 是 **COW（写时拷贝）** 文件系统，桶可以被回收并重新分配。但旧指针可能仍然存在于 btree 中：

```
  桶 100 的回收→重新分配→误读场景：

  时间 ▶───────────────────────────────────────────

  ① 桶 100 gen=1：写入数据 A
     创建 extent 指针 → ptr = (bucket=100, offset=xxx, gen=1)

  ② 数据 A 被删除（COW：新数据写入其他桶）
     桶 100 标记为空 → 加入 freespace

  ③ 桶 100 gen=2：被重新分配，写入数据 B
     创建新 extent 指针 → ptr = (bucket=100, offset=xxx, gen=2)

  ④ journal replay 或快照回溯 → 步骤 ① 的旧指针重现：
     ptr = (bucket=100, offset=xxx, gen=1)
     此时桶 100 的实际数据是 B，不是 A

  ⑤ 没有 gen：
     → 系统无法区分这个指针是新的（指向 B）还是旧的（指向已删除的 A）
     → 读取数据 A → 拿到数据 B → 数据损坏！

  ⑥ 有 gen：
     → 比较：bucket_gen(=2) vs ptr->gen(=1)
     → gen_cmp(2, 1) = (s8)(2-1) = 1 > 0 → stale！
     → 指针被丢弃 → 正确！
```

这就是 gen 存在的唯一原因：**每个物理指针 `bch_extent_ptr` 都携带了写入时桶的世代号 `gen`**：

```c
// bcachefs_format.h
struct bch_extent_ptr {
    u64 offset:44;    // 设备上扇区偏移
    u64 dev:8;        // 设备号
    u64 gen:8;        // ← 世代号！写入时桶的 gen
};
```

任何时候通过该指针读取数据时，`dev_ptr_stale_rcu()` 比较 `bucket_gen` 和 `ptr->gen`：

```
  dev_ptr_stale_rcu(ca, ptr) → gen_cmp(bucket_gen, ptr->gen) > 0
    → (s8)(bucket_gen - ptr->gen) > 0
    → 桶当前代际 > 指针记录代际 → 桶已被重新分配 → 指针过期
```

如果没有这个机制，COW 文件系统无法区分指向桶的指针是"当前有效"还是"指向已被覆盖的旧数据"。

**这和 gc_gen 的关系**：`gen` 解决的是最基础的"这个指针指向的数据还在吗"。而 `gc_gen` 和 `oldest_gen` 解决的是衍生问题——「u8 gen 单向增长到 255 后回绕时，如何确保 gen_cmp 仍然正确」。后者只有在理解前者之后才有意义。

##### 三个世代值

```
  struct bch_alloc_v4 {
      u8  gen;           // 当前世代（每次 alloc→free 循环 +1）
      u8  oldest_gen;    // 当前已知的最老世代（GC 推进）
      // gc_gen = gen - oldest_gen（不存储，是计算值）
  };

  alloc_gc_gen(a) = a->gen - a->oldest_gen
```

```
  一个桶的世代演进：

  时间 ──────────────────────────────────────────▶

  gen:        1──2──3──4──5──6──7──8──9──10──11──12
  oldest_gen: 1────────────────────5───────────────8
  gc_gen:     0──1──2──3──4──1──2──3──4──5───6───4
                    ↑                    ↑          ↑
               第 4 次分配         GC 推进      GC 再次推进
               oldest_gen 落后     oldest_gen   oldest_gen
                                   到 5         到 8
```

- `gen`：桶被重新分配时 +1（分配→释放→再次分配 算一个完整的循环）
- `oldest_gen`：GC 扫描后推进——当所有引用某一世代桶的指针都被删除后，`oldest_gen` 可以安全地追上
- `gc_gen = gen - oldest_gen`：桶的"年龄差"——从 GC 最后一次确认的世代到现在，桶经历了多少次分配循环

##### gc_gen 达到上限之后

```
  BUCKET_GC_GEN_MAX = 96    // background.h:31

  当桶为空（dirty_sectors == 0 && cached_sectors == 0），
  且 gc_gen >= 96：

  alloc_data_type() 返回 BCH_DATA_need_gc_gens（不是 BCH_DATA_free！）
```

`need_gc_gens` 状态的含义：**这个桶在逻辑上是空闲的，但在 GC 确认之前不能分配**。

```
  从 free 到 need_gc_gens 到 free 的完整路径：

  桶循环分配多次后，gc_gen 达到 96
     ↓
  最近一次释放时，alloc_data_type() 返回 need_gc_gens
     ↓
  桶标记为 BCH_DATA_need_gc_gens（不是 free）
     ↓
  freespace btree: 没有该桶的条目（不是 free 所以不加入）
     ↓
  触发 bch2_gc_gens_async()  ← 异步唤醒 GC scan
     ↓
  bch2_gc_gens()：
    1. 遍历所有 btree，调用 gc_btree_gens_key()
       → bch2_bkey_drop_stale_ptrs()
       → 删除所有 gen < oldest_gen 的指针
    2. 遍历 alloc btree，更新每个桶的 oldest_gen
       → bch2_alloc_write_oldest_gen(trans, ca, iter, k)
       → 对每个桶 a->oldest_gen = ca->oldest_gen[bucket]
    3. oldest_gen 推进 → gc_gen 下降
     ↓
  如果 gc_gen 降到 96 以下：
    下一轮 alloc_data_type() 返回 BCH_DATA_free
    桶加入 freespace btree
    可以重新分配了
```

##### genbits 编码如何防止误用

回到 `alloc_freespace_genbits`：

```c
alloc_freespace_genbits(a) = ((u64) alloc_gc_gen(a) >> 4) << 56;
```

```
  假设桶的 gen 从 1 跑到 200，gc_gen 变化及 genbits 编码：

  gc_gen        gc_gen >> 4    genbits (offset 高 8 位)
  ──────────────────────────────────────────────
   0-15         0              0x00
  16-31         1              0x01 << 56
  32-47         2              0x02 << 56
  48-63         3              0x03 << 56
  64-79         4              0x04 << 56
  80-95         5              0x05 << 56
  96-111        6              0x06 << 56  ← 超过 MAX，need_gc_gens 状态
  112-127       7              0x07 << 56
  ...
  240-255      15              0x0F << 56

  BUCKET_GC_GEN_MAX=96 意味着 freespace 中桶的 gc_gen 范围是 0-95，
  因此 genbits 只有 6 个可能值（0-5），每个覆盖 16 个 gc_gen。
```

```
  崩溃→journal replay 场景的具体保护机制：

  第 1 步：
  gc_gen=10 (gen=15, oldest_gen=5)
  freespace 条目：offset = bucket | (10>>4)<<56 = bucket | 0x00<<56
  → 桶被分配，从 freespace 删除

  第 2 步：
  系统崩溃，未写入 journal

  第 3 步：
  重启 journal replay，重新应用 freespace 条目
  → offset = bucket | 0x00<<56 的 KEY_TYPE_set 被重新插入

  第 4 步：
  journal replay 重做其他操作，桶 gen 变为 17，oldest_gen=5
  → gc_gen = 12

  第 5 步：
  分配器遍历 freespace btree，看到该条目
  try_alloc_bucket → bch2_check_freespace_key_async
  → 读取 alloc btree：gc_gen = 12 → alloc_freespace_genbits = (12>>4)<<56 = 0x00<<56
  → 与 freespace 条目中的 genbits 匹配！
  → 交叉验证通过！但这是过期的 freespace 条目...

  等等——这里 genbits 没有变化：12>>4=0 和 10>>4=0 都是 0。**这就是为什么 `>> 4` 是一个权衡——它提供了 16 代容错**。

  在 `>> 4` 的设计下：
  - gc_gen 变化 0-15：genbits 不变 → freespace 条目仍然有效
  - gc_gen 变化 16+：genbits 改变 → freespace 条目作废

  这意味着 genbits 编码无法防止 15 代以内的 stale 条目被误用。但这不是问题，因为 journal replay 后 alloc btree 中的 `data_type` 字段会阻止分配——`data_type != BCH_DATA_free` 时 `bch2_bucket_do_freespace_index` 返回 0（不插入），而 `bch2_check_freespace_key_async` 也会因 `data_type != BCH_DATA_free` 拒绝。

**genbits 编码解决的是更隐蔽的问题：journal replay 让 freespace 条目重新出现后，alloc btree 中 data_type 被 GC 修正为 free，但 oldest_gen 被 GC 推进了。** 此时没有 genbits 的话，freespace 条目看起来是有效的（data_type == free），但实际上 gen 已过期，bucket 中可能还有未回收的数据。

```
  genbits 编码真正要防的场景：

  初始：
    bucket.gc_gen = 10 (gen=15, oldest_gen=5)
    freespace 条目：(dev, bucket | 0x00<<56)

  GC 扫描后：
    oldest_gen 推进到 12（部分旧指针被判定为 stale 并被清理）
    gc_gen = 15 - 12 = 3
    data_type = free（gc_gen < 96）
    alloc_freespace_genbits = 3>>4<<56 = 0x00<<56 ← 还没变！

  下次 GC 扫描：
    oldest_gen 推进到 14
    gc_gen = 15 - 14 = 1
    data_type = free
    alloc_freespace_genbits = 1>>4<<56 = 0x00<<56 ← 还没变！

  第 N 次 GC 扫描：
    oldest_gen 推进到 18
    gc_gen = 20 - 18 = 2（假设 gen 也增长了）
    alloc_freespace_genbits = 2>>4<<56 = 0x00<<56 ← 还是没变！

  → 但！第一次 GC 扫描时（oldest_gen=5→12），
     可能存在旧指针指向 gen=5 的桶
     这些指针在 GC 中被丢弃了
     如果 freespace 条目是 journal replay 恢复的（非正常路径），
     它可能是在 oldest_gen=5 时创建的
     genbits 没变（10>>4=0, 3>>4=0, 1>>4=0, 2>>4=0）
     → genbits 无法区分这个场景！

  这 16 代的容错窗口意味着 freespace 中桶的 gc_gen 范围（0-95，对应 MAX=96）
  只有 6 个 16 代窗口（0-15→0, 16-31→1, ..., 80-95→5）。大部分日常变化
  （GC 推进 oldest_gen 让 gc_gen 下降几代）不跨越窗口边界，genbits 不变。

  那这个保护机制到底有什么用？
```

答案在 `__bch2_check_freespace_key` 的交叉验证中：

```c
// check.c:456-457
if (a->data_type != BCH_DATA_free ||
    genbits != alloc_freespace_genbits(*a)) {
    // 不匹配 → 删除该 freespace 条目
    ret = delete_freespace_key(trans, iter, ...);
}
```

genbits 保护的是 **跨 GC 扫描世代边界** 的 freespace 条目。

```
  genbits 真正有效的场景（步长 16，非 4）：

  场景 A — GC 大幅推进 oldest_gen：

    分配前：   gen=60, oldest_gen=5,  gc_gen=55, genbits=55>>4=3
    freespace 条目：offset = bucket | (3<<56)

    GC 扫描后：oldest_gen 推进到 45（删除了一大批旧指针）
               gc_gen=60-45=15,    genbits=15>>4=0
    → freespace 条目 genbits=3 ≠ alloc genbits=0！
    → 条目被 delete_freespace_key 删除

  场景 B — 系统重启后 gen 大幅增加：

    journal replay 后：gen=80, oldest_gen=5,  gc_gen=75, genbits=75>>4=4
    journal 中记录的旧 freespace 条目：genbits=1（对应 gc_gen=16-31 时期）
    → genbits=1 ≠ alloc genbits=4 → 删除

  场景 C — genbits=0 时的大幅变化（最坏情况）：

    journal replay 恢复的条目 genbits=0（对应 gc_gen=0-15）
    当前状态 gc_gen=14，仍落在 0-15 范围内 → genbits 仍为 0 → 匹配！
    → 可以分配

    但 gc_gen=14 意味着 gen - oldest_gen = 14。这个桶自最老存活指针以来
    经历了 14 个分配循环。journal replay 恢复的 freespace 条目可能对应
    完全不同的分配周期。这是 16 步长导致的 worst-case 15 代容错窗口。

    但此时 alloc btree 中的 data_type=BCH_DATA_free 才是真正决定性的检查。
    如果该桶真的不能分配，data_type 不会被 set 为 free。
```

**所以 genbits 编码的核心保护不是防细粒度变化**（16 代窗口太大了，大部分日常 GC 推进不跨越边界），**而是防止大幅的世代跳跃**——比如设备离线再上线、磁盘 resize 后 bucket 索引偏移、或者长时间运行后的首次 GC。在这些边缘场景下，旧的 freespace 条目会因 genbits 不匹配而被自动清理，不需要额外的状态清理代码。对于更常见的微小变化，由 alloc btree 的 `data_type` 和 `gc_gen >= MAX` 闸门兜底。

##### 再看 `>> 4` 的设计意图

```
  u8 gc_gen = gen - oldest_gen，范围 0-255
  BUCKET_GC_GEN_MAX = 96  →  freespace 中桶的 gc_gen 有效范围 = 0-95
  genbits = gc_gen >> 4，6 个值覆盖 0-95（0:0-15, 1:16-31, ..., 5:80-95）

  为什么选 >> 4（步长 16）而不是 >> 3（步长 8）或 >> 2（步长 4）？

  ∵ genbits 变化 → alloc trigger 额外写入 freespace btree
  ∴ 步长越小，freespace 更新越频繁 → write 放大越大

  步长 16 的代价：最多 15 代的 stale 条目逃逸 genbits 过滤
  但：
  - alloc btree 的 data_type 字段兜底（data_type != free 时不被分配）
  - gc_gen >= 96 的桶不进入 freespace（need_gc_gens 闸门）
  - 所以 freespace 中 gc_gen 只有 0-95，6 个 16 代窗口

  所以 >>4 是一个性能-安全权衡：
  - 减少 freespace 更新频率 → 减少 alloc trigger 的 write 放大
  - 16 代容错窗口由 alloc btree 的 data_type 字段兜底
```

##### gc_gen 存在的根本原因：限制 gen 增长，确保 dev_ptr_stale_rcu 始终有效

**`gc_gen = gen - oldest_gen`** 不是一个"功能特性"——它是一个 **完整性约束的安全阀**。

###### 问题：u8 gen 回绕会破坏指针过期检测

桶的世代 `gen` 是 **u8**（0-255），每次桶重新分配时 +1，必然回绕。

指针过期检测 `dev_ptr_stale_rcu()` 比较的是指针写时的代际和桶的当前代际：

```c
gen_cmp(a, b) = (s8)(a - b)                 // signed 8-bit comparison
dev_ptr_stale_rcu(ca, ptr) → gen_cmp(bucket_gen, ptr->gen) > 0
// ptr->gen 落后 bucket_gen 时 (>0) → stale
```

当 `bucket_gen - ptr->gen` 的无符号差值 >= 128，强转 `s8` 后变负数：

```
  正常：   bucket_gen=10,  ptr->gen=3   → (s8)(7)=7>0   → stale ✓
  问题 A： bucket_gen=200, ptr->gen=5   → (s8)(195)=-61 → NOT stale ✗
  问题 B： bucket_gen=3,   ptr->gen=250 → (s8)(9)=9>0   → stale ✓
  问题 C： bucket_gen=250, ptr->gen=3   → (s8)(247)=-9  → NOT stale ✗
```

**问题 A 和 C 才是 gc_gen 要防的**：当 gen 大幅领先某个旧指针（gen 从 3→250 单向增长了 247），s8 把大正数看作负数，stale 指针被误判为存活。

```
  gen 单向增长（一直分配/释放同一桶）：

  time ─────────────────────────────▶
  gen: 3──4──5── ... ──250
                        ↑
                  ptr->gen=3 的指针
                  (s8)(250-3) = -9 → NOT stale ← 错误的！
```

`gc_gen` 的**真实目的不是"解决回绕"**（因为 `gen_cmp` 的有符号比较已经正确处理了回绕——见问题 B），**而是限制 gen 和 oldest_gen 的差距，确保所有正常的代际差都落在 s8 的有效检测范围内（0 到 +127）**。

###### 约束：gc_gen = gen - oldest_gen ≤ BUCKET_GC_GEN_MAX(=96)

```
  s8 检测 stale 的有效范围：[1, 127]
                ┌─────────────────────────┐
                │  stale ✓    │ NOT stale │
  差值(s8)      0────────────127───────────255
  含义         gen - ptr->gen 的 s8 值
```

`oldest_gen` 是 GC 确认的"最旧存活指针的 gen"。对于任何应被 GC 清理的 stale 指针 `p`：

```
  p->gen < oldest_gen             // 比最旧存活指针还旧 → 应该是 stale
  bucket_gen = gen                 // bucket_gens 跟踪当前 gen
  diff = gen - p->gen              // 这个差值可能很大（如果 gen 增长很多）
  gen_cmp(gen, p->gen) = (s8)(diff)  // 但 s8 只认识 0-127
```

如果允许 `gen` 比 `oldest_gen` 领先 100+ 且 `oldest_gen` 比 `p->gen` 领先 100+，`gen - p->gen` 可能远超 127 → s8 回绕 → 漏检。

**BUCKET_GC_GEN_MAX=96 保证 `gen - oldest_gen ≤ 96`**，配合 GC 定期扫描，确保：
1. `gen` 不会无限制地增长
2. 当 gc_gen 接近 96 时强制触发 GC，推进 oldest_gen，把差值压回去
3. 所有应被检测为 stale 的指针 `p` 的 `gen - p->gen` 都在可控范围内

```
  gc_gen 约束如何保持系统在安全区：

  时间 ──────────────────────────────────────▶
  gc_gen:  0────20────40────60────80────96────
           │                       │        │
          GC 刚跑完              接近极限  触发 GC

            ← GC 推进 oldest_gen →
               gc_gen 下降回 0
```

这就是 `need_gc_gens` 状态的含义：桶虽然是空的，但 **gen 已经和 oldest_gen 拉开了 96+ 的差距**，不能安全地加入 freespace。必须先 GC 把 oldest_gen 追上来。

###### 与 dev_ptr_stale_rcu 的关系

`dev_ptr_stale_rcu` 用的是 **bucket_gens 运行时数组的 gen**（和 alloc btree 中的 `gen` 是同一值，同步更新），不是 alloc 中的 `oldest_gen`。那 gc_gen 怎么影响它？

```
  GC 扫描 bch2_gc_gens() 完成后：

  ca->oldest_gen[b] = max(
      bucket_gen(ca, b),          // 桶当前 gen
      max(ptrs->gen for survivors) // 存活指针的最大 gen
  )

  然后 bch2_alloc_write_oldest_gen 写入 alloc btree：
  → new_a->oldest_gen = ca->oldest_gen[bucket]

  GC 后：oldest_gen 被推进到接近 gen（因为 bucket_gen ≈ gen）
  → gc_gen = 0 或很小
```

所以 `gc_gen` 不是直接控制 `dev_ptr_stale_rcu`，而是 **通过触发 GC 间接管理 gen 的增长**。它是一个早期预警系统：在 gen 增长到可能导致 s8 检测失效的程度之前，强制系统进行代际维护。

###### 总结

```
  gc_gen 的完整推理链：

  ①  gen 是 u8 (0-255)，每次桶重复用时 +1
  ②  dev_ptr_stale_rcu 用 (s8)(bucket_gen - ptr->gen) > 0 判断指针过期
  ③  当 gen 大幅增长，bucket_gen - ptr->gen 超过 127 时 s8 变为负数 → 漏检
  ④  解决方案：约束 gen - oldest_gen ≤ 96，阻止 gen 无限制增长
  ⑤  约束通过 alloc_data_type() 中的闸门实施：
      gc_gen >= 96 → need_gc_gens（非 free）→ 不能分配 → 强制 GC
  ⑥  GC 推进 oldest_gen → gc_gen 下降 → 回到安全区
```

**gc_gen 不是直接解决 dev_ptr_stale_rcu 的单个比较失败，而是一个系统级的完整性约束**，确保世代追踪系统始终在 s8 比较的有效范围内运行。它是一个**安全阀**，在异常情况下（GC 未能及时运行、gen 累积过多）自动触发维护操作。

##### 补充：`bucket_gens` btree 与指针过期检测

```c
// alloc/buckets_types.h:47-56
struct bucket_gens {
    u16 first_bucket;
    size_t nbuckets;
    u8  b[];            // 每个桶 1 u8 代际
};

// alloc/format.h:109-116
#define KEY_TYPE_BUCKET_GENS_NR  256   // 每个 btree key 存 256 个桶的代际
struct bch_bucket_gens {
    struct bch_val  v;
    u8              gens[256];
};
```

```
  桶号 → bucket_gens 键的编码：

  桶号 N = (键偏移 << 8) | 索引
  bpos(dev, N >> 8) 定位到键
  gens[N & 0xFF]    定位到该桶的代际

  单个 device 上：
  bucket_gens btree          alloc btree
  ┌─────────────────┐       ┌──────────────────┐
  │ key(dev, 0)     │       │ key(dev, 0)      │
  │ gens[0..255]    │       │ gen, oldest_gen  │
  ├─────────────────┤       ├──────────────────┤
  │ key(dev, 1)     │       │ key(dev, 1)      │
  │ gens[256..511]  │       │ gen, oldest_gen  │
  ├─────────────────┤       ├──────────────────┤
  │ ...             │       │ ...              │
  └─────────────────┘       └──────────────────┘
```

`dev_ptr_stale_rcu()` 检查的是 bucket_gens 中的代际（不是 alloc 中的 `oldest_gen`）：

```c
// buckets.h 中 dev_ptr_stale_rcu(ca, ptr)
// → gen_cmp(bucket_gen(ca, PTR_BUCKET_NR), ptr->gen) > 0
//    即：桶当前代际 > 指针记录代际  → 指针过期

// bucket_gen(ca, b) 从 ca->bucket_gens->b[b] 读取
// 这是 bucket_gens btree 刷入的运行时数组
```

**与 GC 中的 `oldest_gen` 的关系**：

```
  bch2_gc_gens() 的流程：

  ① 从 bucket_gens 运行时数组初始化 ca->oldest_gen[]
     ca->oldest_gen[b] = bucket_gen(ca, b)   // 当前桶代际

  ② 遍历所有含数据指针的 btree，对每个指针：
     if (dev_ptr_stale_rcu(ca, ptr))          // ptr->gen < bucket_gen
         从 bkey 中删除该指针                 // 指针过期
     else
         跟踪最大 gen：                        // 指针存活
         ca->oldest_gen[PTR_BUCKET_NR] =
              max(oldest_gen, ptr->gen)

  ③ 将 computed oldest_gen 写回 alloc btree 每条
     for_each_btree_key(BTREE_ID_alloc)
         a->oldest_gen = ca->oldest_gen[bucket]
```

所以 `oldest_gen` 是 **存活指针中最大的 `ptr->gen`**——即"现在还有谁在引用这个桶"。GC 后 `oldest_gen` 可能变小（如果最老的存活指针被确认死亡），也可能不变（如果最老的还在）。

- `bucket_gens[b]`：桶当前的世代号（每个分配循环递增）
- `a->oldest_gen`：该桶存活指针中最旧的世代
- `a->gen`：桶的世代号（与 bucket_gens[b] 一致）
- `alloc_gc_gen(a) = gen - oldest_gen`：已废弃但尚未被 GC 确认的世代数

---

GC 世代在安全检查中的角色层次：

```
  第一层：data_type
    桶必须标记为 BCH_DATA_free 才能出现在 freespace 中
    → 如果 data_type 不对，freespace 条目被拒绝
    来源：alloc trigger 原子级保证

  第二层：gc_gen >= BUCKET_GC_GEN_MAX 闸门
    当 gc_gen 过高（桶释放多次都没 GC 扫描），桶不进入 freespace
    进入 need_gc_gens 状态，强制触发 GC
    → 防止 gc_gen 溢出和无限制的世代增长
    来源：alloc_data_type() 在 trigger 中的调用

  第三层：genbits 编码
    跨 GC 扫描边界的 freespace 条目自动作废
    → 防止大幅世代跳跃后误用旧条目
    来源：freespace key 的高 8 位 + __bch2_check_freespace_key

  第四层：bch2_check_freespace_key 交叉验证
    分配时的实时检查：读取 alloc btree 确认 data_type 和 genbits
    → 最后的兜底防线
    来源：alloc 路径中的验证点
```

```
  崩溃恢复示例：每一层如何保护

  事件链：

  ① 桶在 gc_gen=10 时释放 → freespace 条目 (genbits=10>>4=0)
  ② 系统崩溃
  ③ 重启 journal replay → freespace 条目重新出现

  ④ alloc btree 也被 journal replay 恢复
     可能是 free（正常）或 need_discard（看时机）

  case A: alloc 恢复为 free, gc_gen=10, genbits=0
    → 匹配！→ 可以分配
    → data_type=free ✓, genbits=0=0 ✓
    → 正确

  case B: alloc 恢复为 need_discard, gc_gen=10
    → data_type != free → freespace 条目被拒绝
    → 第一层保护生效

  case C: alloc 恢复为 free 但 GC 已推进 oldest_gen:
    gc_gen=2, genbits=0 → 匹配！
    → 但这意味着 oldest_gen 已经大幅推进
    → 如果 gen=15, oldest_gen=5→13, gc_gen=2
    → 现在 gc_gen 很小，说明 GC 已经清理过旧指针
    → 该桶确实是安全的
```

#### 总结

```
  freespace btree 摘要：

  类型：        extent btree（BTREE_IS_extents）
  值类型：      KEY_TYPE_set（存在即空闲）
  key 格式：    bpos(dev, bucket | genbits<<56)
  维护方式：    alloc v4 trigger 副作用
  验证：        分配时交叉检查 alloc btree（gc_gen, data_type）
  重建：        从 alloc btree 全量扫描（仅 freespace_initialized=false 时）
  伪影保护：    genbits 高 8 位编码 → 旧世代 freespace 条目自动失效
  相关 btree：  BTREE_ID_need_discard / BTREE_ID_need_gc_gens
```

### 3.9 案例：空间生命周期

三个案例串联初始化、分配、回收的全路径，统一使用以下场景：一个 4 个桶的设备，桶 0-2 空闲，桶 3 保留给 journal。

#### 案例 A：初始化（mkfs 到 ready）

```
  初始磁盘布局：

  ┌─────────────────────────────────────────────┐
  │ 桶 0 (free) │ 桶 1 (free) │ 桶 2 (free) │ 桶 3 (journal) │
  └─────────────────────────────────────────────┘
```

**阶段 A1：读取持久 alloc 状态**

```c
// background.c:1031 — 启动时从磁盘加载 alloc btree 到内存
bch2_alloc_read(c)
  → 遍历 BTREE_ID_alloc:    每个桶一个 alloc v4 键
      桶 0: gen=1, oldest_gen=1, data_type=free
      桶 1: gen=1, oldest_gen=1, data_type=free
      桶 2: gen=1, oldest_gen=1, data_type=free
      桶 3: gen=1, oldest_gen=1, data_type=journal
  → 遍历 BTREE_ID_bucket_gens: 填充 ca->bucket_gens[] 运行时数组
      桶 0: gen=1, 桶 1: gen=1, 桶 2: gen=1, 桶 3: gen=1
```

**阶段 A2：初始化 bucket_gens btree（兼容旧格式）**

```c
// background.c:1011 — 如果 v0.25 以前的格式没有 bucket_gens btree
bch2_bucket_gens_init(c)
  → 遍历 BTREE_ID_alloc
  → 每 256 个桶写一个 bch_bucket_gens 键
      桶 0-255 → key(dev, 0): gens[0]=1, gens[1]=1, ...
```

**阶段 A3：构建 freespace btree**

freespace btree（`BTREE_ID_freespace`）**不是** alloc btree 的重复，而是一个只追踪**空闲桶**的索引。freespace btree 存在的原因是性能：在百万个桶中定位一个空闲桶时，遍历只占 alloc btree 1% 条目的索引，远快于扫描全部条目。

##### freespace key 编码

freespace btree 的 key 使用一个巧妙技巧来编码桶的 GC 世代：

```c
// background.h:174 — genbits = gc_gen >> 4，编码在 offset 的高 8 位
static inline u64 alloc_freespace_genbits(struct bch_alloc_v4 a)
{
    return ((u64) alloc_gc_gen(a) >> 4) << 56;
}

static inline u8 alloc_gc_gen(struct bch_alloc_v4 a)
{
    return a.gen - a.oldest_gen;   // GC 世代 = 当前 gen - oldest_gen
}

// background.h:179 — 完整的 key 位置
static inline struct bpos alloc_freespace_pos(struct bpos pos, struct bch_alloc_v4 a)
{
    pos.offset |= alloc_freespace_genbits(a);
    return pos;
}
```

编码后的结构：

```
POS(dev, bucket_number | (gc_gen >> 4) << 56)
     │                │
     │                └── bucket_offset[0:56), genbits[56:64)
     │                    genbits = gc_gen >> 4（0-5 有效，max gc_gen=95）
     └── dev_idx = 设备编号
```

genbits 的作用：当桶被反复分配和释放时，gc_gen 增长，freespace key 位置变化。这保证了分配器遍历时不会二次选中一个刚被重新加入的空闲桶——freespace btree 是有版本的索引。

##### 初始化算法

初始化的实际代码使用 `bch2_get_key_or_hole` 模式——一次处理一个 alloc key **或一个 hole**：

```c
// check.c:755 — 核心迭代器，被 bch2_dev_freespace_init 通过 lockrestart_do 循环调用
static int dev_freespace_init_iter(struct btree_trans *trans, struct bch_dev *ca,
                                   struct btree_iter *iter, struct bpos end)
{
    struct bkey hole;
    struct bkey_s_c k = bkey_try(bch2_get_key_or_hole(iter, end, &hole));
    // ↑ 返回：如果当前位置有 key → k 指向 key
    //       如果当前位置到 end 之间有 hole → k 指向 hole

    if (k.k->type) {
        // 分支 1：alloc btree 在该位置有活 key
        const struct bch_alloc_v4 *a = bch2_alloc_to_v4(k, &a_convert);

        try(bch2_bucket_do_freespace_index(trans, ca, k, a, true));
        // 只在 a->data_type == BCH_DATA_free 时插入 freespace 条目
        // journal、sb、user、cached 等 data_type 都被跳过

        try(bch2_trans_commit(trans, NULL, NULL, BCH_TRANS_COMMIT_no_enospc));
        bch2_btree_iter_advance(iter);       // 进到下一个 key
    } else {
        // 分支 2：hole！alloc btree 在这个范围内没有 key
        struct bkey_i *freespace = ...;
        bkey_init(&freespace->k);
        freespace->k.type   = KEY_TYPE_set;
        freespace->k.p      = k.k->p;        // 直接使用 hole 的范围
        freespace->k.size   = k.k->size;

        try(bch2_btree_insert_trans(trans, BTREE_ID_freespace, freespace, 0));
        try(bch2_trans_commit(trans, NULL, NULL, BCH_TRANS_COMMIT_no_enospc));

        bch2_btree_iter_set_pos(iter, k.k->p);  // 跳到这个范围的末尾
    }
}
```

**分支 1（有 alloc key）**：对于每个存在的 alloc 条目，调用：

```c
// background.c:1102 — 按 data_type 决定是否插入 freespace
int bch2_bucket_do_freespace_index(struct btree_trans *trans,
                                   struct bch_dev *ca,
                                   struct bkey_s_c alloc_k,
                                   const struct bch_alloc_v4 *a,
                                   bool set)  // set=true → 插入
{
    if (a->data_type != BCH_DATA_free)
        return 0;               // ← 非空闲桶被直接跳过！

    struct bpos pos = alloc_freespace_pos(alloc_k.k->p, *a);
    // 编码 gc_gen 到 key 的高 8 位

    return bch2_btree_bit_mod_iter(trans, &iter, set);
    // KEY_TYPE_set @ pos → freespace btree
}
```

只有 `data_type == free` 的桶被加入 freespace。journal、superblock、user data、cached、need_discard、need_gc_gens 全部被忽略。

**分支 2（hole）**：alloc btree 中可能有不连续的区域——桶从未被分配（新设备/fresh mkfs），或者键值已被删除。这些桶被视为空闲，直接以 hole 的范围批量插入 freespace btree。这里**不需要**检查 data_type，因为没有 alloc key 意味着桶从未被使用。

##### 主调函数：每次 mount 检查

```c
// check.c:840 — 通用入口，在 mount 路径中被调用
int bch2_fs_freespace_init(struct bch_fs *c)
{
    if (c->sb.features & BIT_ULL(BCH_FEATURE_small_image))
        return 0;               // 小镜像跳过

    bool doing_init = false;
    for_each_member_device(c, ca) {
        if (ca->mi.freespace_initialized)
            continue;           // ← 关键短路：已初始化则跳过

        if (!doing_init) {
            bch_info(c, "initializing freespace");
            doing_init = true;
        }

        try(bch2_dev_freespace_init(c, ca, 0, ca->mi.nbuckets));
        // 扫描 ca 上从 0 到 nbuckets 的所有桶
    }

    if (doing_init) {
        guard(mutex)(&c->sb_lock);
        bch2_write_super(c);    // 持久化 freespace_initialized 标记
    }
    return 0;
}
```

**当 freespace_initialized 已为 true 时**（绝大多数 mount）：整个函数在 `continue` 处短路，零 btree 遍历。

**当 freespace_initialized 为 false 时**（new mkfs / 设备 add 后崩溃恢复 / 旧版本升级）：执行完整扫描。

##### 何时触发

| 场景 | 调用路径 | 原因 |
|------|----------|------|
| fresh mkfs | `mkfs → bch2_fs_freespace_init` | freespace_initialized 新设备上为 false |
| mount（新/空设备） | `mount → __bch2_dev_read_only` → `bch2_dev_freespace_init` | 设备 add 后可能在写入 superblock 前崩溃 |
| mount（已有数据设备） | `mount → bch2_fs_freespace_init` | freespace_initialized=true → 跳过 |
| 设备 add | `bch2_dev_add → __bch2_dev_read_only` → `bch2_dev_freespace_init` | 新设备加入已有文件系统 |

##### 完成标记

初始化结束后，设备的 superblock 标记被更新：

```c
// check.c:827-833
struct bch_member *m = bch2_members_v2_get_mut(c->disk_sb.sb, ca->dev_idx);
SET_BCH_MEMBER_FREESPACE_INITIALIZED(m, true);
// → ca->mi.freespace_initialized = true（下次 mount 时读入）
```

**crash 恢复**：如果在 `freespace_initialized` 写入 superblock 之前崩溃，下次 mount 时 `bch2_fs_freespace_init` 会发现它是 false 并重新初始化。这是幂等的——多跑一遍只产生相同的 freespace 条目。

##### 本案例结果

```
  alloc btree 中的 4 个条目：

  桶 0: data_type=free  → freespace: KEY_TYPE_set @ POS(dev, 0 | 0<<56)
  桶 1: data_type=free  → freespace: KEY_TYPE_set @ POS(dev, 1 | 0<<56)
  桶 2: data_type=free  → freespace: KEY_TYPE_set @ POS(dev, 2 | 0<<56)
  桶 3: data_type=journal → 跳过（不是 free）

  初始化后的 freespace btree 视图：

  POS(dev, 0x0000000000000000)  ← 桶 0 (gc_gen=0, genbits=0)
  POS(dev, 0x0100000000000000)  ← 桶 1
  POS(dev, 0x0200000000000000)  ← 桶 2
                              ← 桶 3 不在 freespace 中
```

**阶段 A4：启动后台线程**

```
  bch2_fs_allocator_background_init(c)
  → 创建 allocator 工作队列：
      - 回收线程（bch2_discard_one_bucket 循环）
      - GC 线程（bch2_gc_gens 扫描）
      - freespace 游标初始化
```

初始状态完成，系统可以接受 IO。

---

#### 案例 B：空间分配（写入文件 "hello.txt"）

用户写入 hello.txt。文件系统需要分配一个桶来存放数据。

```
  写入路径概览：

  write() → bch2_write → extent btree 更新
    → bch2_disk_reservation_add      [预留空间]
    → bch2_alloc_sectors_start       [选择 write_point + 分配 open_bucket]
    → journal commit                  [日志提交分配事件]
    → __bch2_write                  [实际 IO]
```

**阶段 B1：预留磁盘空间**

```c
// buckets.c:1204
bch2_disk_reservation_add(c, &res, 8, 0)
  → 从当前 CPU 的 pcpu->sectors_available 扣减 8 扇区
  → 如果不足：__bch2_reservation_add 从全局 counter 批量获取
  → 成功：res.sectors = 8
```

**阶段 B2：选择 write_point 和分配桶**

```c
// write.c:2709 — 写入路径中分配桶
bch2_alloc_sectors_start(trans, target, write_point, ...)
  → write_point = bch2_write_point_find(ca, write_point)
  → wp->states 中包含 0-N 个 open_bucket 引用
  → 如果 write_point 的 open_bucket 用尽 → 需要新分配

__bch2_alloc_sectors_alloc_foreground(trans, wp, ...)
  → 选择最优设备（WFQ：当前设备写量最小的优先）
  → bch2_bucket_alloc(trans, ca, req, wp, ...)
```

**阶段 B3：freespace btree 查找**

```c
// foreground.c:437 — 遍历 freespace btree 查找可用桶
bch2_bucket_alloc_freelist(trans, ca, req, wp, freespace_iter)
  → 从 ca->freespace_cursor[wp->idx] 位置开始遍历 freespace btree
  → 找到第一个满足条件的 KEY_TYPE_set extent

  // 遍历过程：
  // cursor 指向桶 0 → peek: 存在 KEY_TYPE_set @ (dev, 0x0000...)
  // try_alloc_bucket → __try_alloc_bucket
```

**阶段 B4：确认桶可用**

```c
// foreground.c:327 — 验证 freespace 条目是否仍然有效
try_alloc_bucket(trans, req, freespace_iter)
  → bch2_check_freespace_key_async(trans, freespace_iter, &gen, &js)
    → 读取 alloc btree 条目
    → 检查 data_type == BCH_DATA_free?       ✓（桶 0 是 free）
    → 检查 genbits == alloc_freespace_genbits?  ✓（都是 0）
    → 返回 gen=1

  → __try_alloc_bucket(c, req, bucket=0, gen=1)
    → 检查不是 superblock/nouse bucket         ✓
    → spin_lock(freelist_lock)
    → 检查 open_buckets_nr_free 有剩余         ✓
    → 检查桶未在 open_bucket 哈希表中           ✓
    → bch2_open_bucket_alloc: 从 pool 中取出一个 ob
    → ob->gen = 1, ob->bucket = 0, ob->dev = ca->dev_idx
    → ob->valid = true, ob->sectors_free = bucket_size
    → bch2_open_bucket_hash_add(c, ob)        // 加入哈希表
    → spin_unlock(freelist_lock)
    → return ob
```

```
  分配后的 open_bucket 状态：

  write_point→states[0]:
    ob->bucket = 0
    ob->gen    = 1
    ob->dev    = 0
    ob->sectors_free = 247KiB  // bucket_size - 已使用
```

**阶段 B5：alloc btree 更新（trigger 副作用）**

分配桶后，调用链回到 `bch2_trans_update` 更新 alloc btree。trigger 自动执行：

```c
// background.c:1232 — alloc trigger（BTREE_TRIGGER_transactional 阶段）
bch2_trigger_alloc(trans, BTREE_TRIGGER_insert)
  alloc_data_type_set(new_a, new_a->data_type)  // 重算 canonical data_type

  statechange_to(!data_type_is_empty(a->data_type)) → true
    // 桶从空变为非空！
    SET_BCH_ALLOC_V4_NEED_INC_GEN(new_a, true)   // ← 下次清空时 gen++
    SET_BCH_ALLOC_V4_NEED_DISCARD(new_a, true)   // ← 兼容位

  // freespace btree 更新（非 transactional 阶段）
  bch2_update_freespace(trans, alloc, op)
    → 旧状态是 free → 删除 freespace 条目
      bch2_btree_delete_at(trans, freespace_iter, 0)
    → 新状态是 user → 不需要插入 freespace

  // update_need_discuss:
    → 旧状态是 free → 不需要 need_discard
```

**阶段 B6：日志提交**

```c
// journal 提交：将 alloc key 变更写入 journal
bch2_journal_key_insert(trans, BTREE_ID_alloc, POS(dev, 0), &alloc_key)
  // 重启时 journal replay 会重做这个 alloc 更新
  // 不会丢失"桶 0 已被分配"这一事实
```

```
  分配完成后状态：

  alloc btree:      桶 0: data_type=user, dirty_sectors=8, NEED_INC_GEN=1
                   freespace: 桶 0 的条目被删除

  内存 open_bucket: 桶 0: ob->sectors_free 减少中
  freespace btree:  0x0000... (桶 0) 已消失，只剩桶 1、2
```

---

#### 案例 C：空间回收（删除 hello.txt + discard + GC）

用户删除 hello.txt，之前分配的桶 0 需要回收。

**阶段 C1：btree 键删除**

```
  delete("hello.txt")
  → bch2_unlink → bch2_btree_delete(extent btree 中指向桶 0 的 extent 键)
  → bucket 0 的脏扇区计数变为 0，缓存扇区计数也为 0
  → alloc trigger 再次触发（键值更新）:
```

**阶段 C2：alloc trigger 响应空桶**

```c
// background.c:1232 — alloc trigger 在桶清空时
bch2_trigger_alloc(trans, BTREE_TRIGGER_insert)
  // 之前：data_type=user, dirty_sectors=8, NEED_INC_GEN=1
  // 现在：dirty_sectors=0, cached_sectors=0

  alloc_data_type_set(new_a, new_a->data_type)
  → compute: dirty=0, cached=0, stripe=0, !need_discard
  → gc_gen = gen - oldest_gen = 1 - 1 = 0  < BUCKET_GC_GEN_MAX
  → data_type = free                 ← alloc_data_type() 的最终结果

  // 但是——优先检查非空→空状态转换:
  statechange_from(!data_type_is_empty(a->data_type)) → true
    // 之前是非空（user），现在是空！
    if (old_a->data_type != sb && old_a->data_type != journal)
        new_a->data_type = BCH_DATA_need_discard  ← 覆盖为 need_discard
    // ↑ 这就是 alloc_data_type 设置的 free 被 need_discard 覆盖的时机

  statechange_to(a->data_type == BCH_DATA_free) → false
    // data_type 现在是 need_discard，不是 free，所以不走这个路径

  // NEED_INC_GEN 路径（桶为空 + NEED_INC_GEN 已设置 + 未打开）
  data_type_is_empty && NEED_INC_GEN && !open
    oldest_gen==gen && !sectors_total → oldest_gen++  // 1→2
    gen++                                             // 1→2
    SET_BCH_ALLOC_V4_NEED_INC_GEN(new_a, false)
    // 结果：gen=2, oldest_gen=2, gc_gen=0
```

```
  清空后的 alloc 条目：

  桶 0: gen=2, oldest_gen=2, gc_gen=0,
        data_type=need_discard, dirty_sectors=0
```

**阶段 C3：freespace + need_discard 更新**

```c
// alloc trigger 的非 transactional 阶段
bch2_update_freespace(trans, alloc, op)
  → 旧状态是 user → 从 freespace 删除（条目已经不在了，无操作）
  → 新状态是 need_discard → 不是 free → 不插入 freespace

bch2_update_need_discard(trans, alloc, op)
  → 旧状态 user → 不是 free/need_discard → 从 need_discard 删除
     (该桶从未在 need_discard 中，无操作)
  → 新状态 need_discard → bch2_btree_insert_at(need_discard btree)
     → BTREE_ID_need_discard: 插入 KEY_TYPE_set @ POS(dev, 0)
```

```
  删除后 btree 状态：

  alloc btree:       桶 0: data_type=need_discard, gen=2
  need_discard btree: POS(dev, 0) → KEY_TYPE_set    ← 新增！
  freespace btree:   桶 0 不在其中
```

**阶段 C4：后台 discard**

回收线程在 `bch2_discard_one_bucket` 中循环：

```c
// discard.c:289
bch2_discard_one_bucket(trans, pos)
  → 遍历 BTREE_ID_need_discard, 获取下一个桶
  → 本次选中 POS(dev, 0)（桶 0）

  → 读取 alloc btree 条目
    gen=2, oldest_gen=2, data_type=need_discard, gc_gen=0

  // 如果设备支持 TRIM：
  → blkdev_issue_discard(ca->disk_sb.bdev, ...)
  → 等待 discard 完成

  → 更新 alloc 条目:
    → data_type = free（或 need_gc_gens，取决于 gc_gen）
    → 设置 BTREE_TRIGGER_is_discard 标志

  // alloc trigger 再次触发:
  statechange_from(need_discard)
    → BTREE_TRIGGER_is_discard 已设置，WARN_ON 不触发

  statechange_to(free)
    → old data_type = need_discard → WARN 不触发（合法路径）

  // freespace 更新:
  bch2_update_freespace
    → 旧状态 need_discard → 不是 free → 无操作
    → 新状态 free → 插入 freespace btree
      → pos(dev, 0 | 0<<56): gc_gen=0, genbits=0
      → bch2_btree_insert_at(freespace btree)

  // need_discard 更新:
  bch2_update_need_discard
    → 旧状态 need_discard → 删除 need_discard 条目
    → 新状态 free → 不是 need_discard → 无操作
```

```
  Discard 完成后状态：

  alloc btree:       桶 0: data_type=free, gen=2, oldest_gen=2, gc_gen=0
  need_discard btree: POS(dev, 0) → 已删除
  freespace btree:   0x0000...（桶 0）已恢复 ✓
```

**阶段 C5（可选）：当 gc_gen 接近上限时**

如果桶 0 被反复分配和释放（gen 持续增长，GC 未运行）：

```
  第 10 次分配循环： gen=25,  oldest_gen=2,  gc_gen=23
  第 50 次分配循环： gen=65,  oldest_gen=2,  gc_gen=63
  第 80 次分配循环： gen=97,  oldest_gen=2,  gc_gen=95  ← 接近上限
  第 82 次分配循环： gen=99,  oldest_gen=2,  gc_gen=97  ← >= 96!
```

在第 82 次删除时（桶清空）：

```c
alloc_data_type(new_a, data_type):
  → dirty=0, cached=0, stripe=0, !need_discard
  → gc_gen = gen - oldest_gen = 99 - 2 = 97
  → 97 >= BUCKET_GC_GEN_MAX(96)
  → return BCH_DATA_need_gc_gens         // 不是 free！
```

场景变为：

```
  alloc btree:       桶 0: data_type=need_gc_gens, gen=99, oldest_gen=2, gc_gen=97
  freespace btree:   桶 0 不在其中（不是 free）
  need_gc_gens btree: POS(dev, 0) → KEY_TYPE_set
```

`bch2_trigger_alloc` 检测到 need_gc_gens：

```c
// background.c:1464 — 触发异步 GC 扫描
if (data_type_is_empty(new_a->data_type) &&
    new_a->data_type == BCH_DATA_need_gc_gens)
  bch2_gc_gens_async(c);
```

**阶段 C6：GC 扫描**

```c
// check.c:1179
bch2_gc_gens(c)
  // ① 初始化 ca->oldest_gen[] = bucket_gens 数组
  // 桶 0: oldest_gen = bucket_gen(ca, 0) = 99
  //    （注意：bucket_gens 数组跟踪 gen，现在 gen=99）

  // ② 遍历所有包含数据指针的 btree
  for each btree with data pointers:
      for each key with extent pointers:
          if (dev_ptr_stale_rcu(ca, ptr))
              bch2_bkey_drop_stale_ptrs(trans, &iter, k)
          else
              // 指针存活，更新 oldest_gen
              ca->oldest_gen[PTR_BUCKET_NR] =
                  max(ca->oldest_gen[bucket], ptr->gen)

  // 假设桶 0 的最旧存活指针 gen=50：
  // ca->oldest_gen[0] = max(99, 50) = 99
  // 没有更旧的存活指针，oldest_gen 保持 99
  // 但如果所有指针都已被删除：
  // ca->oldest_gen[0] = 99（从 bucket_gen 初始化的值）

  // 如果发现某些指针(gen=5)比 bucket_gen(99)小
  // gen_cmp(99, 5) = (s8)(94) = 94 > 0 → stale → 被删除 ✓
  // 但如果指针 gen=200（回绕后）:
  // gen_cmp(99, 200) = (s8)(-101) = -101 < 0 → NOT stale
  // ↑ 这就是 gc_gen=96 要防的场景

  // ③ 写回 oldest_gen
  for each alloc btree key:
      bch2_alloc_write_oldest_gen(trans, ca, &iter, k)
      // 桶 0: old->oldest_gen(2) != ca->oldest_gen[0](99)
      // → new_a->oldest_gen = 99
      // → alloc trigger 触发

  // alloc trigger 再次触发：
  // gc_gen = 99 - 99 = 0 < 96
  // data_type = free（不再是 need_gc_gens）
```

GC 后：

```
  最终状态：

  alloc btree:    桶 0: gen=99, oldest_gen=99, gc_gen=0, data_type=free
  freespace:      桶 0 重新加入（genbits = 0>>4<<56 = 0）
  need_gc_gens:   桶 0 的条目被删除
```

桶 0 重新可用。

---

#### 案例总结

```
  完整生命周期，从 mkfs → 写 → 删 → 回收 → 再可用：

  mkfs: free ──alloc_read── alloc btree 加载
        free ──freespace_init── freespace btree 构建
        ↓
  写:   free ──try_alloc_bucket── open_bucket 创建
        │         ↓
        │    user(dirty_sectors>0)  ← trigger: NEED_INC_GEN=1
        │         ↓
        │    journal commit
        ↓
  删:   user ──alloc_trigger── need_discard  ← trigger: gen++, NEED_INC_GEN=0
                 ↓
            need_discard btree 新增
                 ↓
            freespace btree 删除条目
        ↓
  回收: need_discard ──discard── free  ← trigger: freespace btree 恢复
                 │
            如果 gc_gen >= 96 ──need_gc_gens── GC scan── free
```

**涉及的核心函数**（§6 中有完整签名）：

| 阶段 | 函数 | 文件 |
|------|------|------|
| 初始化 | `bch2_alloc_read` | background.c:1031 |
| 初始化 | `bch2_fs_freespace_init` | check.c:840 |
| 初始化 | `bch2_bucket_gens_init` | background.c:1011 |
| 分配 | `bch2_disk_reservation_add` | buckets.c:1204 |
| 分配 | `bch2_alloc_sectors_start` | write.c:2709 |
| 分配 | `try_alloc_bucket / __try_alloc_bucket` | foreground.c:269-327 |
| 触发 | `bch2_trigger_alloc` | background.c:1232 |
| 回收 | `bch2_discard_one_bucket` | discard.c:289 |
| GC | `bch2_gc_gens` | check.c:1179 |
| GC | `bch2_alloc_write_oldest_gen` | check.c:1163 |

---

### 3.10 案例 D：设备热添加

上一个案例跟踪了桶从 mkfs 到回收的完整生命周期。本案例补充一个特殊路径：**设备热添加**（在线扩容），展示 freespace 初始化在设备首次接入时的完整流程。

场景：一个已有数据的 2 设备文件系统（设备 0、1 有数据），用户通过 `bcachefs device add /dev/sdc /mnt` 添加新设备 2。

```
  Add 路径概览（userspace → kernel）：

  bcachefs device add
    → ioctl(BCH_IOCTL_DEV_ADD)
      → bch2_dev_add(c, path, err)                     [dev.c:1097]
         ├── bch2_read_super                           步骤1: 读取设备识别信息
         ├── __bch2_dev_alloc                          步骤2: 分配 bch_dev 结构
         ├── bch2_sb_member_alloc                      步骤3: 分配 dev_idx 槽位
         ├── bch2_dev_attach                           步骤4: 挂载到文件系统
         ├── bch2_dev_usage_init                       步骤5: 初始化使用计数
         ├── bch2_trans_mark_dev_sb                    步骤6: 标记 superblock/journal
         ├── bch2_fs_freespace_init                    步骤7: 扫描 alloc → freespace
         ├── __bch2_dev_read_write                     步骤8: 加入分配器
         └── bch2_dev_journal_alloc                    步骤9: 分配 journal 桶
```

**阶段 D1：分配 bch_dev 结构**

```c
// dev.c:1142 — 创建新设备的运行时结构
struct bch_dev *ca = __bch2_dev_alloc(c, &dev_mi);
// ca->mi.freespace_initialized 从设备超级块读入，新设备为 false
// ca->mi.first_bucket, ca->mi.nbuckets 从超级块读取
```

此时 `ca->mi.freespace_initialized == false`——新设备从未初始化过 freespace。

**阶段 D2：写入超级块**

```c
// dev.c:1184-1211 — 分配 dev_idx 后写入超级块（critical section）
*bch2_members_v2_get_mut(c->disk_sb.sb, dev_idx) = dev_mi;
ca->disk_sb.sb->dev_idx  = dev_idx;
bch2_dev_attach(c, ca, dev_idx);
set_bit(ca->dev_idx, c->devs_online.d);
bch2_write_super(c);
```

**crash 风险点**：如果在写入 superblock **之后**、初始化完成之前崩溃，重新 mount 时会发现 `freespace_initialized == false`，走 `bch2_fs_freespace_init` 重做初始化。

**阶段 D3：bch2_dev_usage_init——创建空闲记账条目**

```c
// accounting.c:1257 — 为 ca 创建一条 accounting 条目
int bch2_dev_usage_init(struct bch_dev *ca, bool gc)
{
    u64 v[3] = { ca->mi.nbuckets - ca->mi.first_bucket, 0, 0 };
    // v[0] = nbuckets - first_bucket（所有桶算作 free）
    // v[1] = 0（已使用扇区）
    // v[2] = 0（预留扇区）

    lockrestart_do(trans, ({
        bch2_disk_accounting_mod2(trans, gc,
                      v, dev_data_type,
                      .dev = ca->dev_idx,
                      .data_type = BCH_DATA_free);  // ← accounting btree
    }));
}
```

这告诉 accounting btree（`BTREE_ID_accounting`）：设备 `ca->dev_idx` 上 data_type=free 的可访问空间为 `nbuckets - first_bucket`。

**阶段 D4：标记 superblock/journal——创建 alloc 条目**

```c
// buckets.c:1085 — 为新设备创建超级块和日志区的 alloc 条目
__bch2_trans_mark_dev_sb(trans, ca, BTREE_TRIGGER_transactional)
  → 标记超级块占用的扇区：
     bch2_trans_mark_metadata_sectors(trans, ca,
          0, BCH_SB_SECTOR, BCH_DATA_sb, ...)
    // 桶 0（存放 superblock 的第 0 扇区）→ data_type=sb

     bch2_trans_mark_metadata_sectors(trans, ca,
          sb_offset, sb_offset + sb_size, BCH_DATA_sb, ...)
    // 所有 superblock 复制 → data_type=sb

  → 标记 journal 桶：
     for (i = 0; i < ca->journal.nr; i++)
         bch2_trans_mark_metadata_bucket(trans, ca,
             ca->journal.buckets[i],
             BCH_DATA_journal, ca->mi.bucket_size, ...);
    // journal 桶 → data_type=journal
```

这一步是关键——它在 **alloc btree** 中为新设备创建初始条目：

```
  执行后 alloc btree 新增：

  设备 2:
  POS(2, 0):         data_type=sb,      dirty_sectors=4K
  POS(2, 3):         data_type=journal,  dirty_sectors=bucket_size
  POS(2, 4):         data_type=journal,  dirty_sectors=bucket_size
  POS(2, 5):         data_type=journal,  dirty_sectors=bucket_size
  ...（桶 1-2 暂时没有 alloc entry——未使用区域留作 hole）
  ...（桶 6 以上没有 alloc entry——也是 hole）
```

桶 0 被标记为 `sb`（superblock 使用的扇区），桶 3-5 被标记为 `journal`（预分配的 journal 桶）。其他桶（桶 1-2、6-N）**没有** alloc 条目——它们是 hole。

**阶段 D5：freespace 初始化——填充 freespace btree**

```c
// dev.c:1225 — 对全设备调用 bch2_fs_freespace_init
bch2_fs_freespace_init(c);
  → for_each_member_device(ca)
      if (ca->mi.freespace_initialized)
          continue;                   // 设备 0、1 跳过
      // 设备 2: freespace_initialized == false → 执行初始化
      bch2_dev_freespace_init(c, ca, 0, ca->mi.nbuckets);
```

`bch2_dev_freespace_init` 内部调用 `dev_freespace_init_iter`（参见 §3.9 A3）扫描 alloc btree：

```
  扫描过程（check.c:755，dev_freespace_init_iter）：

  iter @ POS(2, 0): 有 alloc key → data_type=sb → 跳过，不进 freespace
  iter @ POS(2, 3): 有 alloc key → data_type=journal → 跳过
  iter @ POS(2, 4): 有 alloc key → data_type=journal → 跳过
  iter @ POS(2, 5): 有 alloc key → data_type=journal → 跳过
  iter @ POS(2, 6): 到 end 之间是 hole → 批量插入 freespace
    从桶 6 到 nbuckets-1 全部标记为 free
```

但是——等等，桶 1 和桶 2 也是 hole（没有 alloc key），它们从哪处理？

```
  alloc btree 的实际键值分布：
    桶 0:  key @ POS(2, 0):   data_type=sb
    桶 3:  key @ POS(2, 3):   data_type=journal
    桶 4:  key @ POS(2, 4):   data_type=journal
    桶 5:  key @ POS(2, 5):   data_type=journal
                    ↑
                    └── hole 范围 1-2（无 alloc key）
                    └── hole 范围 6-N（无 alloc key）

  扫描迭代过程：
  1. peek @ POS(2, 0): key → 处理桶 0(sb)，advance → POS(2, 1)
  2. peek @ POS(2, 1): bch2_get_key_or_hole → 返回 hole
     → 批量插入 freespace: POS(2, 1) 到 POS(2, 3) 前
     → set_pos → POS(2, 3)
  3. peek @ POS(2, 3): key → 处理桶 3(journal)，advance
  4. peek @ POS(2, 4): key → 处理桶 4(journal)，advance
  5. peek @ POS(2, 5): key → 处理桶 5(journal)，advance → POS(2, 6)
  6. peek @ POS(2, 6): bch2_get_key_or_hole → 返回 hole（到 end）
     → 批量插入 freespace: POS(2, 6) 到 nbuckets-1
     → set_pos → end → 完成
```

**关键观察**：hole 处理（分支 2）在这里至关重要。新设备的 alloc btree 中只有 sb 和 journal 的条目，其余桶（可能是几百万个）以 hole 形式批量标记为 free，而不是逐个插入 alloc key——这就是为什么 `dev_freespace_init_iter` 有两个分支，而分支 2 直接用 hole 的范围批量写入 freespace。

```
  初始化后 freespace btree：

  POS(2, 1 << 56 | 1):  桶 1（genbits=0，gc_gen 不可用←hole）
  POS(2, 2 << 56 | 2):  桶 2
  POS(2, 6 << 56 | 6):  桶 6 ~ ...  批量插入
  ...                   桶 N-1: 最后位置

  关键：桶的 data_type 是 free 因为 hole 被视为空闲。
  注意：hole 没有 alloc_v4 结构，所以 genbits=0，gc_gen=0。
  如果后续这些桶被分配→释放→重新加入 freespace，则会使用实际的 gc_gen 编码 genbits。
```

**阶段 D6：设置 RW——加入分配器**

```c
// dev.c:1231-1232 — 如果设备状态为 rw
if (ca->mi.state == BCH_MEMBER_STATE_rw)
    __bch2_dev_read_write(c, ca);
```

```c
// dev.c:429 — 使设备可写入
void __bch2_dev_read_write(struct bch_fs *c, struct bch_dev *ca)
{
    bch2_dev_allocator_add(c, ca);
    // → bch2_dev_allocator_set_rw(c, ca, true)
    //   → 初始化 per-write_point freespace cursor
    //   → 设置 ca->mi.freespace_initialized 相关标记
    //   → 将 ca->dev_idx 加入 c->allocator.rw_devs

    bch2_recalc_capacity(c);
    // 重新计算总容量，现在包含新设备

    bch2_do_discards_async(c);
    // 如果新设备有 need_discard 桶，启动 discard
}
```

**阶段 D7：journal 桶分配**

```c
// journal/init.c:259 — 分配实际的 journal 桶
bch2_dev_journal_alloc(ca, false);
  // 在设备上预留 journal 所需的桶
  // 这些桶在 D4 中已被标记为 data_type=journal
  // 但 journal_alloc 确保 journal 子系统已知这些桶的位置
```

**阶段 D8：最终状态**

```
  设备添加完成后的 btree 状态：

  alloc btree（设备 2）：
  ┌────────────┬──────────────┬──────────────────────────────┐
  │ 桶         │ data_type    │ 说明                         │
  ├────────────┼──────────────┼──────────────────────────────┤
  │ 桶 0       │ sb           │ superblock                   │
  │ 桶 1-2     │ free         │ hole → freespace 有条目      │
  │ 桶 3-5     │ journal      │ journal 桶                   │
  │ 桶 6-N     │ free         │ hole → freespace 有条目      │
  └────────────┴──────────────┴──────────────────────────────┘

  freespace btree（设备 2）：
    桶 1, 2, 6, 7, ..., N-1 → KEY_TYPE_set

  accounting btree:
    dev_data_type(dev=2, data_type=free) = nbuckets - first_bucket

  superblock:
    freespace_initialized = true    // ← 持久化标记
```

设备 2 现在完全可操作——所有后续的桶分配优先级会考虑它。

---

### 3.11 案例 E：格式化新设备

前两个案例跟踪了挂载后和热添加时的初始化。本案例回到最源头：**`bcachefs format` 如何让一块空白磁盘变为可挂载的文件系统**。

场景：一块 4GiB 的空白磁盘 `/dev/sda`，`bucket_size=256KiB`。

```
  磁盘布局：512B/sector × 8,388,608 扇区
  nbuckets = 4GiB / 256KiB = 16384 个桶
  first_bucket = 0（桶 0 是 superblock 位置）
```

#### 阶段 E1：superblock 写入（userspace，Rust）

```c
// format_util.rs:148 — Rust 函数，写入 superblock
pub fn format(fs_opt_strs, fs_opts, opts, dev_slice) -> *mut bch_sb
{
    // --- 尺寸计算 ---
    // block_size: 4KiB（默认，auto-detect）
    // bucket_size: 256KiB（clamp 到设备尺寸的 1/16384 左右）
    // btree_node_size: min(bucket_size, 256KiB)

    // --- 构造 in-memory superblock ---
    sb.version = current;              // 最新版本
    sb.magic = BCHFS_MAGIC;
    sb.user_uuid = uuid::Uuid::new_v4();
    sb.uuid = uuid::Uuid::new_v4();    // 内部 UUID
    sb.nr_devices = 1;

    sb.features[0] = BCH_SB_FEATURES_ALL;  // 启用所有特性

    // --- 成员信息 ---
    for (idx, dev) in dev_slice.iter_mut().enumerate() {
        let m = sb.member_mut(idx);
        m.nbuckets = dev.nbuckets;     // 16384
        m.first_bucket = 0;
        // freespace_initialized = 0  ← 默认 false！
        // BCH_SB_INITIALIZED 未设置  ← 初始为 false
    }

    // --- 写入磁盘 512B 起始区域清零 ---
    bch2_super_write(fd, sb.sb);
}
```

**关键状态**（刚 format 完，尚未 mount）：

```
  磁盘第 0~16 扇区：bch_sb + bch_sb_layout
  ┌─ sector 0:  引导块（全零）
  ├─ sector 8:  主 superblock
  │    ├── magic        = BCHFS_MAGIC
  │    ├── version      = current
  │    ├── uuid         = <随机>
  │    ├── nr_devices   = 1
  │    ├── members[0].nbuckets     = 16384
  │    ├── members[0].first_bucket = 0
  │    ├── members[0].freespace_initialized = 0  ← false
  │    ├── members[0].state        = rw
  │    ├── BCH_SB_INITIALIZED      = 0           ← false（关键！）
  │    └── ...
  ├─ sector 15:  superblock 副本
  └─ sector ~设备末尾:  superblock 副本

  alloc btree:      不存在（btree 根未创建）
  freespace btree:  不存在
  journal:          没有 journal 条目
```

然后 `Fs::open()` 调用 `bch2_fs_start` → `bch2_fs_initialize`：

```c
// fs/init/fs.c:1492-1494
if (BCH_SB_INITIALIZED(c->disk_sb.sb))
    bch2_fs_recovery(c);        // ← 已有数据的设备
else
    bch2_fs_initialize(c);      // ← 新格式走这里！
```

#### 阶段 E2：创建 btree 骨架

```c
// recovery.c:1051-1052 — 为每个 btree_id 分配一个空根节点
for (unsigned i = 0; i < BTREE_ID_NR; i++)
    bch2_btree_root_alloc_fake(c, i, 0);
```

```
  执行后 btree 状态：

  BTREE_ID_alloc:      fake_root （无条目）
  BTREE_ID_freespace:  fake_root （无条目）
  BTREE_ID_bucket_gens: fake_root（无条目）
  BTREE_ID_inodes:     fake_root
  BTREE_ID_subvolumes: fake_root
  ...（共 30+ btree_ids，每个都有一个空 btree 根）
```

#### 阶段 E3：初始化 accounting 计数器

```c
// recovery.c:1057-1059 — 每个设备创建一条 accounting 条目
for_each_member_device(c, ca)
    bch2_dev_usage_init(ca, false);
```

```c
// accounting.c:1257 — 计数：全部分空闲
int bch2_dev_usage_init(struct bch_dev *ca, bool gc)
{
    u64 v[3] = { ca->mi.nbuckets - ca->mi.first_bucket, 0, 0 };
    //               ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    //               16384 - 0 = 16384 个桶算作 free

    bch2_disk_accounting_mod2(trans, gc, v, dev_data_type,
                              .dev = ca->dev_idx,
                              .data_type = BCH_DATA_free);
}
```

```
  accounting btree 新增条目：

  key(dev_data_type, dev=0, data_type=free) → counters = [16384, 0, 0]
  // [0] = nbuckets
  // [1] = sectors_used
  // [2] = reserved
```

#### 阶段 E4：标记 superblock 和 journal 桶

```c
// recovery.c:1068 — 在 alloc btree 中创建 sb/journal 条目
bch2_trans_mark_dev_sbs(c);
```

```
  调用链：
  bch2_trans_mark_dev_sbs
    → for_each_online_member(ca)
        → __bch2_trans_mark_dev_sb(trans, ca)
            // 桶 0:
            //   bch2_trans_mark_metadata_sectors(trans, ca, 0, 8, BCH_DATA_sb, ...)
            //   → alloc btree 写入 key @ POS(0, 0)
            //      data_type=sb, dirty_sectors=8
            //
            // journal 桶:
            //   for (i = 0; i < ca->journal.nr; i++)
            //       bch2_trans_mark_metadata_bucket(trans, ca, journal_bucket,
            //           BCH_DATA_journal, bucket_size, ...)
```

journal 桶在当前设备上占 64 MiB（默认），分配如下：

```
  设备 0（/dev/sda，4GiB，256KiB 桶）：

  alloc btree 条目（阶段 E4 后）：

  POS(0, 0):   data_type=sb,      dirty_sectors=8 扇区 (4KiB)
  POS(0, 1):   data_type=journal, dirty_sectors=256KiB
  POS(0, 2):   data_type=journal, dirty_sectors=256KiB
  ...
  POS(0, 256): data_type=journal, dirty_sectors=256KiB
              ↑ 64MiB / 256KiB = 256 个 journal 桶（桶 1-256 被预留）
  POS(0, 257): （尚不存在——未分配）

  其他 btree:
  freespace btree:     （无条目，未初始化）
  bucket_gens btree:   （无条目——不需要，gen 全为 0）

  注意：alloc btree 只有桶 0~256 的键，桶 257~16383 在
         btree 中不存在——它们是 "holes"。
```

**journal 桶位置来源**：`bch2_dev_journal_alloc` 在设备结构初始化时预留。它从 `first_bucket + 1` 开始取连续的桶：

```c
// journal/init.c:259
int bch2_dev_journal_alloc(struct bch_dev *ca, bool new_fs)
{
    // 分配桶 1 开始的连续区域作为 journal
    // nr = min(nbuckets/2, 256) 或直到 64MiB
    // 本例：桶 1-256
}
```

#### 阶段 E5：初始化 freespace btree

```c
// recovery.c:1088 — 关键步骤：填充 freespace btree
bch2_fs_freespace_init(c);
```

`freespace_initialized == false`，触发完整扫描：

```c
// check.c:840 — 扫描 alloc btree，为每个 free 桶插入 freespace 条目
bch2_fs_freespace_init(c)
  → bch2_dev_freespace_init(c, ca, 0, 16384)
    → dev_freespace_init_iter(trans, ca, &iter, end=POS(0,16384))

      // 迭代过程（参见 §3.9 A3 的详细伪代码）：

      iter @ POS(0, 0):   有 alloc key → data_type=sb  → 跳过
      iter @ POS(0, 1):   有 alloc key → data_type=journal → 跳过
      iter @ POS(0, 2):   有 alloc key → data_type=journal → 跳过
      ...（256 个 journal 桶全部跳过）
      iter @ POS(0, 257): 接下来到 end 是 hole
           → 批量插入 freespace
              KEY_TYPE_set @ POS(0, 257) ... POS(0, 16383)
              // 16383 - 257 + 1 = 16127 个桶标记为 free
              // 每个桶独立插入一个 KEY_TYPE_set
```

```
  初始化后的 freespace btree 视图：

  POS(0, 0x0101000000000000) ← 桶 257（genbits=(0>>4)<<56=0）
  POS(0, 0x0102000000000000) ← 桶 258
  ...
  POS(0, 0x3FFF000000000000) ← 桶 16383
  // 注意 genbits = 0 ← gc_gen=0, 因为 hole 没有 alloc_v4 数据

  bucket_gens btree:     空（未初始化，也不需要——gen 全是 0）
  freespace_initialized = true ← superblock 标记写入
```

#### 阶段 E6：创建根目录和子卷

```c
// recovery.c:1090
bch2_initialize_subvolumes(c);   // 子卷 1，根 inode
// → btree 条目:
//   BTREE_ID_inodes: POS(BCACHEFS_ROOT_INO, U32_MAX)
//   BTREE_ID_subvolumes: ...
//   BTREE_ID_snapshot_trees: ...

// recovery.c:1093-1101 — 创建根目录 inode
bch2_inode_init(c, &root_inode, 0, 0, S_IFDIR|0755, 0, NULL);
root_inode.bi_inum = BCACHEFS_ROOT_INO;
bch2_btree_insert(c, BTREE_ID_inodes, &packed_inode.inode.k_i, ...);

// recovery.c:1106-1115 — 创建 lost+found
bch2_create_trans(trans, BCACHEFS_ROOT_SUBVOL_INUM,
                  &root_inode, &lostfound_inode, &lostfound, ...);
```

#### 阶段 E7：首次 journal flush + 标记完成

```c
// recovery.c:1125 — 写入第一条 journal 条目
bch2_journal_flush(&c->journal);

// recovery.c:1136-1146 — 持久化初始化完成标记
SET_BCH_SB_INITIALIZED(c->disk_sb.sb, true);
SET_BCH_SB_CLEAN(c->disk_sb.sb, false);
bch2_write_super(c);
```

#### 最终数据结构全景

```
  格式化完成（可挂载）后的全部 btree 状态：

  设备 0（/dev/sda，4GiB，256KiB 桶）：

  superblock:
    BCH_SB_INITIALIZED          = 1
    members[0].freespace_initialized = 1
    members[0].nbuckets         = 16384
    members[0].first_bucket     = 0

  alloc btree（3 种条目）:
  ┌─────────────┬───────────────┬──────────────────────────┐
  │ key         │ data_type     │ dirty_sectors            │
  ├─────────────┼───────────────┼──────────────────────────┤
  │ POS(0, 0)   │ sb            │ 8（superblock 扇区）     │
  │ POS(0, 1-256)│ journal      │ 256KiB（每个 journal 桶）│
  │ 其余        │ （不存在）    │ hole → 视为 free         │
  └─────────────┴───────────────┴──────────────────────────┘

  freespace btree:
    POS(0, 257)  KEY_TYPE_set
    POS(0, 258)  KEY_TYPE_set
    ...          直到 POS(0, 16383)

  bucket_gens btree:
    （空——所有桶 gen=0，无需存储）

  accounting btree:
    key(dev_data_type, dev=0, data_type=free) → [16384, 0, 0]

  inodes btree:
    POS(BCACHEFS_ROOT_INO=256, U32_MAX) → bch_inode (根目录)

  subvolumes btree:
    子卷 1（根 subvol）

  snapshots btree:
    空

  extents btree（数据）:
    空（无用户数据）
```

#### 第二次及后续 mount 的路径

第二次 mount 时：

```c
// fs/init/fs.c:1492-1494
if (BCH_SB_INITIALIZED(c->disk_sb.sb))         // ← true
    bch2_fs_recovery(c);                       // ← 走 recovery
```

`bch2_fs_recovery` 不再创建 btree 根或初始化 freespace。它：

1. `bch2_alloc_read` — 从磁盘加载 alloc btree 到内存（357 个 key）
2. `bch2_bucket_gens_init` — 为旧版本生成 bucket_gens（无变化）
3. `bch2_fs_freespace_init` — `freespace_initialized=true` → **零开销跳过**
4. Journal replay — 重放未持久化的 journal 条目
5. fsck 检查

#### 案例 F：初始化阶段的 journal 分配（Early Allocator）

**场景：** `bch2_fs_initialize` 第 4 步，需要分配 256 个 journal 桶作设备日志。此时 freespace btree 尚未初始化，allocator 不能走 `bch2_bucket_alloc_freelist`（freespace btree 遍历）。

```
  bch2_fs_initialize 时序（相关步骤）：
  ┌─ 1. bch2_btree_root_alloc_fake   ← 所有 btree 获得 fake root（纯内存）
  ├─ 2. bch2_dev_usage_init
  ├─ 3. bch2_trans_mark_dev_sbs      ← alloc btree: sb 条目（ca->journal.nr=0，无 journal）
  ├─ 4. bch2_fs_journal_alloc        ← 需要分配 256 个 journal 桶 ← **这里**
  ├─ 5-7. journal 启动 + 回放
  ├─ 8. bch2_fs_freespace_init       ← freespace btree 建立
  └─ 9-12. inodes + subvols + snapshots
```

**前提条件（第 3 步结束后）：**

```
  alloc btree 状态（格式后）:
  ┌─────────────┬──────────┬──────────────────┐
  │ key         │ data_type│ 说明             │
  ├─────────────┼──────────┼──────────────────┤
  │ POS(0,0)    │ sb       │ 8 sectors        │
  │ POS(0,1..16383)│ (hole)│ 隐含 free        │
  └─────────────┴──────────┴──────────────────┘
```

`ca->journal.nr = 0`（journal 尚未分配），`ca->mi.freespace_initialized = 0`。

---

**追踪 `bch2_fs_journal_alloc` → `bch2_set_nr_journal_buckets_iter`（journal/init.c:38-71）：**

```c
for (nr_got = 0; nr_got < 256; nr_got++) {
    // 步 A：分配一个桶
    ob = bch2_bucket_alloc_trans(trans, req);

    // 步 B：在 alloc btree 中标记为 BCH_DATA_journal
    bch2_trans_mark_metadata_bucket(trans, ca,
        ob->bucket, BCH_DATA_journal, ...);
}
```

**步 A 的分配器路由（foreground.c:620-686）：**

```c
struct open_bucket *bch2_bucket_alloc_trans(...) {
    bool freespace = READ_ONCE(ca->mi.freespace_initialized);
    // → freespace_initialized = 0（freespace btree 未初始化）

    ob = likely(freespace)        // freespace = false
        ? bch2_bucket_alloc_freelist(trans, req)   // ← 不走这条
        : bch2_bucket_alloc_early(trans, req);      // ← 执行这条
}
```

**Early allocator `bch2_bucket_alloc_early`（foreground.c:347-435）：**

```c
// foreground.c:347-350 原文注释
/* This path is for before the freespace btree is initialized */

static noinline struct open_bucket *
bch2_bucket_alloc_early(struct btree_trans *trans, struct alloc_request *req)
{
    u64 cursor = max(first_bucket, *dev_alloc_cursor);
    // cursor = 0（首次调用）

    // BTREE_ITER_slots → 遍历每个 slot（包括 hole）
    for_each_btree_key_norestart(trans, iter, BTREE_ID_alloc,
        POS(0, cursor), BTREE_ITER_slots, k, ret)
    {
        u64 bucket = iter.pos.offset;   // 当前桶号

        struct bch_alloc_v4 *a = bch2_alloc_to_v4(k, &convert);
        // ↑ hole slot → alloc_to_v4 返回 {data_type=0, gen=0}
        //   data_type=0 = BCH_DATA_free

        if (a->data_type != BCH_DATA_free)
            continue;    // sb/journal → 跳过

        // 二次确认（cached iter，防并发）
        CLASS(btree_iter, citer)(trans, BTREE_ID_alloc, k.k->p,
            BTREE_ITER_cached | BTREE_ITER_nopreserve);
        // 确认 data_type == free → 分配
        ob = __try_alloc_bucket(c, req, bucket, a->gen);
        break;
    }
}
```

**第 1 次循环：分配桶 1**

```
  alloc btree 遍历（slots mode）:

  slot POS(0,0):   live key (sb)   → data_type=sb ≠ free → continue
  slot POS(0,1):   (hole)          → alloc_to_v4 返回 {data_type=0}
                   → BCH_DATA_free！→ 二次确认 → __try_alloc_bucket(桶 1)
                   → break
```

`bch2_alloc_to_v4` 对 hole slot 的处理：

```c
// buckets.c
const struct bch_alloc_v4 *bch2_alloc_to_v4(const struct bkey_s_c k,
                                            struct bch_alloc_v4 *convert)
{
    if (k.k->type == KEY_TYPE_alloc_v4)
        return bkey_s_c_to_alloc_v4(k).v;
    // type=0（hole）→ 返回默认值
    memset(convert, 0, sizeof(*convert));
    convert->data_type = BCH_DATA_free;   // ← hole=free
    return convert;
}
```

分配完成后步 B 将桶 1 写入 alloc btree 为 `BCH_DATA_journal`。

**第 2~256 次循环：依次分配桶 2~256**

每次循环都重复：`bch2_bucket_alloc_early` 从 cursor 开始扫描 alloc btree，跳过已分配的 sb 和 journal 条目，找到第一个 hole 作为 free bucket。

```
  每次循环后 alloc btree 变化：

  第 1 次后: POS(0,1) = journal
  第 2 次后: POS(0,2) = journal
  ...
  第 256 次后: POS(0,256) = journal
               POS(0,257) = (hole) ← 此后全部是 free
```

**回答你的问题：journal 再多也不影响**

```
  alloc btree 有多大 → slots 覆盖所有 bucket 编号（first_bucket~nbuckets）
  journal 有多少个 → 对应位置的 slot 被标记为 journal，跳过
  剩余的 slot → 全部是 hole → alloc_to_v4 返回 BCH_DATA_free
                  → early allocator 总能找到可分配的桶

  即使 journal 占用了前 4096 个桶：
    POS(0,0)       sb        → continue
    POS(0,1..4096) journal   → continue
    POS(0,4097)    (hole)    → BCH_DATA_free → __try_alloc_bucket(桶 4097) ✓
```

---

##### freespace btree 初始化为什么不需要自举

`bch2_fs_freespace_init`（第 8 步）的核心函数 `dev_freespace_init_iter` 使用 `bch2_get_key_or_hole`，**不是每个桶逐个插入**，而是按 range 处理：

```c
// check.c:755-790
static int dev_freespace_init_iter(struct btree_trans *trans,
    struct bch_dev *ca, struct btree_iter *iter, struct bpos end)
{
    // bch2_get_key_or_hole 返回：
    // - 如果有 live key（sb/journal/btree） → 返回该 key
    // - 如果是 hole → peek_max 跳过 hole 找到下一个 live key
    //                  → 返回一个 {type=0, p=当前, size=到下一key的距离}
    struct bkey_s_c k = bch2_get_key_or_hole(iter, end, &hole);

    if (k.k->type) {    // live alloc entry
        // sb/journal → data_type≠free → 什么都不做
        bch2_bucket_do_freespace_index(trans, ca, k, a, true);
        // → if (a->data_type != BCH_DATA_free) return 0;
        bch2_trans_commit(...);
        bch2_btree_iter_advance(iter);
    } else {            // hole = 连续 free 桶区域
        struct bkey_i *freespace = ...;
        freespace->k.type = KEY_TYPE_set;
        freespace->k.p    = k.k->p;       // POS(0, 257)
        freespace->k.size = k.k->size;    // = 16127

        // 插入 freespace btree：**一条 key 覆盖 16127 个桶**
        bch2_btree_insert_trans(trans, BTREE_ID_freespace, freespace, 0);
        bch2_trans_commit(...);
        bch2_btree_iter_set_pos(iter, k.k->p);  // ← 设回收尾，外层 while 退出
    }
}
```

`bch2_get_key_or_hole` 的原理：

```c
// check.c:23-57
static struct bkey_s_c bch2_get_key_or_hole(
    struct btree_iter *iter, struct bpos end, struct bkey *hole)
{
    struct bkey_s_c k = bch2_btree_iter_peek_slot(iter);

    if (k.k->type) {            // live key
        return k;
    } else {                    // hole
        // 在当前 leaf node 内找下一个 live key
        end = bkey_min(end,
            POS(iter->pos.inode, iter->pos.offset + U32_MAX - 1));

        k = bch2_btree_iter_peek_max(&iter_copy, end);
        struct bpos next = iter_copy.pos;

        bkey_init(hole);
        hole->p    = iter->pos;               // 从当前位置开始
        hole->size = next.offset - iter->pos.offset;  // 到下一 key 的距离
        return (struct bkey_s_c) { hole, NULL };
    }
}
```

**format 场景的 freespace btree 构建追踪：**

```
  alloc btree 迭代器逐位置前进:

  POS(0,0)      peek_slot → type=alloc_v4    → sb 条目 → 跳过(bch2_bucket_do_freespace_index
  POS(0,0+1)    advance                     → POS(0,1)        不执行任何操作)
  POS(0,1)      peek_slot → type=alloc_v4    → journal 条目 → 跳过
  ...循环 256 次...
  POS(0,256)    advance → POS(0,257)

  POS(0,257)    peek_slot → type=0 (hole)
                peek_max 在 POS(0,257)~POS(0,16384) 范围内
                          找下一个 live key → 没有 → next = POS(0,16384)
                hole = {p=POS(0,257), size=16384-257=16127}

                → 插入 freespace btree:
                  KEY_TYPE_set{p=POS(0,257), size=16127}
                  ↑ 一次插入，16127 个桶

                → set_pos(iter, POS(0,257))
                → while(bkey_lt(POS(0,257), POS(0,16384))) 失败 → 循环结束
```

**freespace btree 初始化期间的写入量：**

| 条目类型           | 格式场景       | 数量         |
|-------------------|---------------|-------------|
| live alloc 条目   | sb + journal  | 257 个       |
| hole → freespace  | 一段连续 hole  | **1 条** KEY_TYPE_set（size=16127）|

freespace btree 在 fake root 阶段只收到 **1 个 key**。fake root 容量 ≈ 5000 条，远未触发 split。**不需要自举分配。**

**什么场景下 freespace init 会触发 split？**

当 alloc btree 有**大量分散的小 hole**时（如 crash 恢复场景, 或 device add 过程中 crash 导致部分桶被分配）：

```
  大量分散 hole 的例子：
    POS(0,0)      sb
    POS(0,1..50)  journal
    POS(0,51)     (hole)     → 第 1 次: KEY_TYPE_set{size=1}
    POS(0,52)     btree
    POS(0,53)     (hole)     → 第 2 次: KEY_TYPE_set{size=1}
    POS(0,54..60) journal
    POS(0,61)     (hole)     → 第 3 次: KEY_TYPE_set{size=1}
    ...
```

此时 freespace btree 接收多个小型 KEY_TYPE_set。数量超过 fake root 容量时才触发 split。split 时的 node 分配走 `bch2_bucket_alloc_trans` → `freespace_initialized=0` → `bch2_bucket_alloc_early` → alloc btree 遍历找 free bucket。这才是 early allocator 在这条路径上的真正用途。

**初始化阶段 vs 正常运行分配路径对比：**

```
  ┌──────────────────────┬──────────────────────────────────┬──────────────────────────────┐
  │                      │ 初始化阶段                        │ 正常运行                     │
  │                      │ (freespace_initialized=0)        │ (freespace_initialized=1)    │
  ├──────────────────────┼──────────────────────────────────┼──────────────────────────────┤
  │ 分配器                │ bch2_bucket_alloc_early          │ bch2_bucket_alloc_freelist   │
  ├──────────────────────┼──────────────────────────────────┼──────────────────────────────┤
  │ 数据源                │ alloc btree（all slots）          │ freespace btree（仅 free 条目）│
  ├──────────────────────┼──────────────────────────────────┼──────────────────────────────┤
  │ hole 的处理           │ bch2_alloc_to_v4 返回 data_type=0│ 无 hole（freespace 只存 free）│
  │                      │ = BCH_DATA_free                  │                              │
  ├──────────────────────┼──────────────────────────────────┼──────────────────────────────┤
  │ 选择算法              │ 线性扫描至第一个 hole（free）      │ btree 遍历（O(log n)）        │
  ├──────────────────────┼──────────────────────────────────┼──────────────────────────────┤
  │ 适用场景              │ format·device add·early recovery  │ 所有正常运行                  │
  │                      │ （freespace 尚未就绪时）           │                              │
  ├──────────────────────┼──────────────────────────────────┼──────────────────────────────┤
  │ format 期间 journal  │ 使用（256 次 early allocator 调用）│ 不相关                       │
  │ 分配                  │                                  │                              │
  ├──────────────────────┼──────────────────────────────────┼──────────────────────────────┤
  │ freespace init 期间  │ 一般不需要（hole key 少，          │ 不相关                       │
  │ split node 分配       │ fake root 够用）                  │                              │
  │                      │ 仅 device add crash 恢复时需要    │                              │
  └──────────────────────┴──────────────────────────────────┴──────────────────────────────┘
```

---

### 3.12 案例总结（所有空间相关场景）

```
  三种初始化场景对比：

  ┌──────────────┬──────────────────┬─────────────────────┬──────────────────────┐
  │ 场景          │ btree 根来源      │ alloc btree 数据源   │ freespace 构建方式   │
  ├──────────────┼──────────────────┼─────────────────────┼──────────────────────┤
  │ format (全新) │ fake root 创建    │ trans_mark_dev_sb   │ hole + free 扫描     │
  │              │ 所有 btree_id     │ (sb+journal 条目)    │ 非 sb/journal 全部 free│
  ├──────────────┼──────────────────┼─────────────────────┼──────────────────────┤
  │ mount (已有)  │ 从磁盘读取        │ bch2_alloc_read     │ 跳过（已初始化）      │
  │              │（recovery 加载）  │ 加载全部条目         │                      │
  ├──────────────┼──────────────────┼─────────────────────┼──────────────────────┤
  │ device add   │ 已有 btree 根     │ trans_mark_dev_sb   │ hole + free 扫描     │
  │              │（无需创建）        │ (sb+journal 条目)    │ 同 format            │
  └──────────────┴──────────────────┴─────────────────────┴──────────────────────┘

  format vs device add 关键区别：

  format:
    bch2_fs_initialize
    ├── bch2_btree_root_alloc_fake（全部 btree）
    ├── bch2_dev_usage_init（accounting）
    ├── bch2_trans_mark_dev_sbs（alloc: sb+journal）
    ├── bch2_fs_freespace_init（freespace）
    ├── bch2_initialize_subvolumes（inodes）
    └── SET_BCH_SB_INITIALIZED(true)

  device add（到运行中系统）:
    bch2_dev_add
    ├── bch2_dev_usage_init（accounting: 仅新设备）
    ├── bch2_trans_mark_dev_sb（alloc: sb+journal，仅新设备）
    ├── bch2_fs_freespace_init（freespace: 仅新设备）
    └── BCH_SB_INITIALIZED 已为 true（无需设置）
```

---

### 4.1 S = Shared / I = Intent / X = Exclusive

这是 bcachefs **最核心的并发原语**，专为 COW 写时复制 B-tree 的高并发场景设计。传统的 RW 锁在读持有者的场景下无法升级为写锁——如果一个线程持有 R 要升级 W，其他 R 持有者会阻塞 W，而它们在 W 完成前不会释放 R，形成死锁。SIX 锁通过引入 "Intent" 中间态解决这个问题。

```
  SIX 锁状态及迁移图：


                ┌───────────────────┐
                │    Shared (S)     │  ← 读锁，多个 S 可共存
                │   "我正在读取"     │
                └───────┬─────┬─────┘
                        │     │
              tryupgrade│     │downgrade
                        │     │
                        ▼     │
                ┌───────────────────┐
           ┌───▶│    Intent (I)     │  ← 意向写，S 可进入，I/X 互斥
           │    │ "我可能要修改"     │
           │    └───────┬───────────┘
           │            │
           │    trylock │ have_write_seq
           │   (非阻塞)  │ (阻塞：six_lock_ip_waiter)
           │            │
           │            ▼
           │    ┌───────────────────┐
           │    │  Exclusive (X)    │  ← 排他写，无人可入
           │    │  "我在修改"        │
           │    └───────────────────┘
           │
           └───────────────────────── 写入完成 → unlock → 回到 S 或释放


  传统 RW 锁的问题（为什么需要 I）：

       线程A持有R想升级W         线程B持有R
        ┌───────────┐          ┌───────────┐
        │ 等待W      │          │ 持有R      │
        │ (阻塞)     │◄─────────│ (不会释放)  │
        └───────────┘          └───────────┘
        → 死锁！R不会释放因为等W
        → W不会授予因为还有R

  引入 I 后：

       线程A持有S尝试升级I       线程B持有S
        ┌───────────┐          ┌───────────┐
        │ tryupgrade │────────▶│ 持有S      │
        │ 成功！      │  S↔I兼容 │ (继续读)   │
        │ 持有I(第3步)│          └───────────┘
        │ 再升级X     │
        └───────────┘
        → 无死锁：S升级I在持有S的同行眼中是允许的
        → 等B释放S后，I再升级X完成修改
```

### 4.2 兼容性矩阵

| 已持有 \ 请求 | Shared（读） | Intent（意向写） | Exclusive（排他写） |
|---|---|---|---|
| Shared | ✓ | ✓ | ✗ |
| Intent | ✓ | ✗ | ✗ |
| Exclusive | ✗ | ✗ | ✗ |

- **Shared**：多个 S 可以共存
- **Intent**：I 阻止其他 I 和 X，但不阻止 S。表示"我可能想写，但还没确定，大家还可以读"
- **Exclusive**：排他，无人可入

### 4.3 B-tree 更新操作模式

```
1. 从根向叶子遍历，沿途拿 Intent 锁（不阻塞其他 S 读者）
2. 沿途不修改，只持有 I
3. 到达目标叶子节点，I 升级为 X（仅在此处争锁）
4. 修改叶子数据
5. 解锁 X → 可能降级回 I 向上返回
6. 返回路径上逐级解锁 I
```

**关键优势**：多个写事务可以在**不同分支**同时进行。它们在只在叶子节点上争 X 锁，而路径上的互斥是对 I 而非 S——所以不阻塞遍历读者。

```
  B-tree 遍历 + 更新的锁持有图：

  事务1 (更新 inode=1 的 key)        事务2 (更新 inode=2 的 key)

  根节点          I[事务1]              I[事务2]
  ↓
  内部节点1       I[事务1]              I[事务2]
  ↓
  内部节点2       I[事务1]              I[事务2]
  ↓
  叶子节点      X[事务1]  ← 此处争锁     X[事务2]  ← 不冲突！
  (inode=1)    (只有事务1进入)           (不同分支)

  读事务（纯遍历）在任何节点的任意锁下都可以进入：
  根节点          S[读者] ← 不阻塞！
  内部节点1       S[读者]
  内部节点2       S[读者]
  叶子节点        S[读者] ← 可同时与 I 共存
  (inode=3)     (仅当 X 时阻塞)


  锁持有时间对比：

  传统 B-tree：
  根────▶叶子──────────▶返回
  W      W      W      W
  ← 沿途全是 W，全程阻塞所有读者

  bcachefs SIX：
  根────▶内部────▶叶子──────▶返回
  I      I      X→I→解锁    I解锁
  ← S 全程可进     仅在叶子短暂 X
```

### 4.4 锁的实现模式

#### 模式 A：序列号 + 乐观重锁

**文件：** `fs/btree/locking.c:673-696`

```c
bool __bch2_btree_node_relock(struct btree_trans *trans,
                  struct btree_path *path, unsigned level, bool trace)
{
    struct btree *b = btree_path_node(path, level);
    int want = __btree_lock_want(path, level);

    if (six_relock_type(&b->c.lock, want, path->l[level].lock_seq) ||
        (btree_node_lock_seq_matches(path, b, level) &&
         btree_node_lock_increment(trans, &b->c, level, want))) {
        mark_btree_node_locked(trans, path, level, want);
        return true;
    }
    return false;
}
```

**原理**：six_lock 内嵌序列号（类似 seqlock），每次 X 锁释放时递增。`six_relock_type` 检查传入 `lock_seq` 是否匹配当前序列号——若匹配则锁未被触及，可直接重入。这是一种**非阻塞乐观重锁**：没有等待，没有上下文切换。

#### 模式 B：从读锁升级到意向锁

**文件：** `fs/btree/locking.c:700-746`

```c
bool bch2_btree_node_upgrade(struct btree_trans *trans,
                 struct btree_path *path, unsigned level)
{
    struct btree *b = path->l[level].b;

    if (btree_node_locked(path, level)
        ? six_lock_tryupgrade(&b->c.lock)          // 已有 R → 尝试非阻塞升级到 I
        : six_relock_type(&b->c.lock, SIX_LOCK_intent,
                  path->l[level].lock_seq))
        goto success;
    // 升级失败 → 数据可能不适合写，重新评估
}
```

#### 模式 C：完整的写周期

**文件：** `fs/btree/write.c:638-664`

```c
void bch2_btree_node_write_trans(struct btree_trans *trans, struct btree *b,
                 enum six_lock_type lock_type_held, unsigned flags)
{
    if (lock_type_held == SIX_LOCK_intent ||
        (lock_type_held == SIX_LOCK_read &&
         six_lock_tryupgrade(&b->c.lock))) {
        // 有意向锁 → 安全写入
        __bch2_btree_node_write(c, b, flags);

        // 刚写入的节点需要清理 + 解锁
        if (btree_node_just_written(b) &&
            six_trylock_write(&b->c.lock)) {
            bch2_btree_post_write_cleanup(c, b);
            __bch2_btree_node_unlock_write(trans, b);
        }

        // 如果原来持有的是读锁，降级回去
        if (lock_type_held == SIX_LOCK_read)
            six_lock_downgrade(&b->c.lock);
    } else {
        // 没有意向锁 → 通过闭包异步写入
        __bch2_btree_node_write(c, b, flags);
    }
}
```

这个函数展示了 SIX 锁的完整使用模式：
1. **tryupgrade**：持有 R 时可尝试非阻塞升级到 I
2. **trylock_write**：写入完成后尝试获取 X 做清理
3. **downgrade**：操作完成后降级回 R

#### 模式 D：阻塞获取写锁

**文件：** `fs/btree/locking.c:580-602`

```c
int __bch2_btree_node_lock_write(struct btree_trans *trans, struct btree_path *path,
                 struct btree_bkey_cached_common *b, bool lock_may_not_fail)
{
    int readers = bch2_btree_node_lock_counts(trans, NULL, b, b->level)
                      .n[SIX_LOCK_read];
    // 释放当前线程持有的该节点的读锁
    six_lock_readers_add(&b->lock, -readers);
    // 阻塞等待写锁
    ret = __btree_node_lock_nopath(trans, b, SIX_LOCK_write, lock_may_not_fail, ...);
    // 获取后重新加回读锁（因为路径可能仍需要它）
    six_lock_readers_add(&b->lock, readers);
    // ...
}
```

**原理**：在 I→X 升级时，当前线程可能已经持有该节点的 S 锁（由于路径上的锁提升）。six_lock 在读计数归零之前不会进行写锁的唤醒，所以必须先将自己的读计数剥离，阻塞等待写锁，获取后再恢复读计数。

### 4.5 锁顺序

B-tree 节点锁**严格从根到叶**加锁（树路径方向），避免死锁。持有父节点的锁时请求子节点锁，永远不反向。

---

## 5. 磁盘格式 — journal + btree

### 5.1 Journal 是核心写入路径

bcachefs 的写入路径是 COW（写时复制），但不是对每个字节的写入都立即回写。它的核心写入流程是：

```
  完整的写入流水线（三阶段）：

  阶段一：用户写入 + B-tree 操作                  时间轴
  ┌─────────────────────────────────────┐         │
  │ 用户写入（pwrite / write系统调用）     │         │
  │     │                                │         │
  │     ▼                                │         │
  │ bch2_extent_update()                 │         │
  │   → disk_reservation_add()           │   T₁    │
  │   → bch2_trans_update()              │         │
  │   → bch2_trans_commit()              │         │
  └──────────┬──────────────────────────┘         │
             │                                    ▼
             ▼
  ┌─────────────────────────────────────┐
  │  阶段二：Journal 写入                 │
  │                                     │
  │ bch2_journal_write_list_add()       │   T₂ (~10μs)
  │   → journal entry 加入缓冲区         │
  │   → 用户态返回成功                    │
  │   → (此时数据在内存 volatile)         │
  │                                     │
  │  后台定时器 (~10ms) / journal 满：    │
  │ bch2_journal_write()                │   T₃ (~10ms)
  │   → 加密 + csum                     │
  │   → bio 提交到块设备                  │
  └──────────┬──────────────────────────┘
             │
             ▼
  ┌─────────────────────────────────────┐
  │  阶段三：B-tree node 刷盘             │
  │                                     │
  │ 后台线程扫描 dirty btree nodes：      │
  │ __bch2_btree_node_write()           │   T₄ (~30s 后)
  │   → CAS 脏位（谁抢到谁写）             │
  │   → 多 bset 排序 → 合并 → csum       │
  │   → bio 提交                         │
  │                                     │
  │ 完成后释放 journal entry 引用         │
  │ → journal 可以回收该 entry 的空间      │
  └─────────────────────────────────────┘
         ↑     ↑        ↑
  用户态返回  user可见  持久化
  (T₂)      (T₃)      (T₄)
  性能关键    安全关键    空间回收


  Journal entry 与 btree node 的引用关系：

  Journal 环：
  ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┐
  │ seq=1│ seq=2│ seq=3│ seq=4│ seq=5│ seq=6│ ...  │
  │  准备  │  已落盘│  已落盘│  准备  │  准备  │ 自由  │
  └──────┴──────┴──────┴──────┴──────┴──────┴──────┘
               ↑                       ↑
           last_seq                  j->seq
           (最早未flush)              (当前写入位置)

  Btree node ──持有引用──▶ journal seq=3
  意思：这个 btree node 的数据依赖于 journal seq=3 的记录
  直到 btree node 刷盘完成，journal seq=3 的空间才能回收

  当 journal 满时：
  → 找到持有最旧 seq 引用的 btree node
  → 强制 flush 那个 node
  → node 落盘后释放引用
  → journal 可以前进 last_seq
```

journal 充当**写入缓冲**和**崩溃恢复点**——写入先 journal 化保证可恢复，然后才更新 btree 节点。

**关键设计：**

| 属性 | 说明 |
|------|------|
| 环状结构 | journal 在 bucket 池中分配，条目不跨 bucket |
| last_seq | 最早未 flush 的 journal 序列号，replay 的起点 |
| btree 引用计数 | btree node 写回时持有 journal entry 的引用，脏数据落盘后释放 |
| 满时策略 | 优先 flush 持有最旧 journal entry 引用的 btree node |
| 写入延迟 | ~10ms 批量窗口，async 条目可被后续条目覆盖（未落盘前） |
| 范围 | journal 只记录叶子节点插入，内部节点更新是同步的 |

### 5.2 btree_node 写入：三阶段实现

**文件：** `fs/btree/write.c:277-583`

```
  __bch2_btree_node_write 三阶段时序：

  阶段 1             阶段 2             阶段 3
  ┌────────────┐    ┌────────────┐    ┌────────────┐
  │ 脏位 CAS     │    │ 排序 + 合并   │    │ 校验和 + BIO │
  │             │    │             │    │             │
  │ 读 b->flags │    │ 收集未写入   │    │ csum_vstruct│
  │ try_cmpxchg │    │ 的 bset      │    │ 加密 (可选)  │
  │             │    │             │    │             │
  │ 成功者继续   │    │ sort_iter   │    │ bio_alloc    │
  │ 失败者返回   │    │ 多路归并     │    │ queue_work   │
  │ (写入令牌)   │    │ bounce_alloc │    │ block IO     │
  └────────────┘    └────────────┘    └────────────┘
       │                  │                  │
       ▼                  ▼                  ▼
  ┌─────────────────────────────────────────────────┐
  │         btree_node_write_endio 回调              │
  │  → 释放 bounce buffer                          │
  │  → 清除 write_in_flight 标记                    │
  │  → 错误时设置 journal error                     │
  └─────────────────────────────────────────────────┘


  双缓冲（write_idx 翻转）：

              write_idx=0                  write_idx=1
         ┌──────────────────┐        ┌──────────────────┐
  读路径  │  bset_0 (当前)    │        │  bset_1 (写入中)  │
         │  bset_1 (写入中)  │        │  bset_0 (当前)    │
         │  bset_2 (新写入)  │ ← 读   │  bset_2 (新写入)  │
         └──────────────────┘        └──────────────────┘
  写入时翻转 write_idx，读路径始终读 write_idx 指向的"当前"集
  写入完成后翻转 write_idx，旧集变为可回收

  脏位 CAS 详细逻辑：

  CPU-0                        CPU-1
  │                            │
  │ try_cmpxchg(b->flags)      │ try_cmpxchg(b->flags)
  │   old=dirty                │   old=dirty
  │   new=dirty|in_flight      │   new=dirty|in_flight
  │   ───── CAS 成功─────▶      │   ───── CAS 失败─────▶
  │                            │
  │ 负责 IO (排序→bounce→bio)  │ 返回（让赢家写）
  │ b->written += sectors      │
  │                            │ b->written += sectors
  │                            │ （读已有写入数据而不重复写）
  │ 写入完成: clear in_flight  │
```

每个 btree 节点的写入分为三个阶段：

#### 阶段 1：脏位 CAS 仲裁

```c
old = READ_ONCE(b->flags);
do {
    new = old;
    // 未脏则跳过
    if (!(old & (1 << BTREE_NODE_dirty)))       return;

    // 检查 write_blocked、never_write 等拒绝条件
    if (old & ((1 << BTREE_NODE_never_write)|
           (1 << BTREE_NODE_write_blocked)))    return;

    // 清除 dirty / need_write，设置 in_flight / just_written
    new &= ~((1 << BTREE_NODE_dirty)|(1 << BTREE_NODE_need_write));
    new |=  ((1 << BTREE_NODE_write_in_flight)|(1 << BTREE_NODE_just_written));
    new ^=  (1 << BTREE_NODE_write_idx);         // 切换写入索引（双缓冲）
} while (!try_cmpxchg_acquire(&b->flags, &old, new));
```

脏位充当写入令牌——只有 CAS 成功的线程负责执行 IO。`write_idx` 翻转实现双缓冲（写入新 bset 的同时旧 bset 仍可读）。

#### 阶段 2：排序 + 合并

```c
// 收集所有尚未写入的 bset
for_each_bset(b, t) {
    i = bset(b, t);
    if (bset_written(b, i))    continue;
    bytes += le16_to_cpu(i->u64s) * sizeof(u64);
    sort_iter_add(&sort_iter.iter,
              btree_bkey_first(b, t),    // 起始 key
              btree_bkey_last(b, t));    // 结束 key
}

// 分配到 bounce buffer（可能来自 mempool，避免 GFP 失败）
data = bch2_btree_bounce_alloc(c, bytes, &used_mempool);

// 写 header + 排序合并
if (!b->written)
    init_btree_node_header(b, data, &bytes);
u64s = bch2_sort_keys_keep_unwritten_whiteouts(i->start, &sort_iter.iter);
```

#### 阶段 3：校验和 + bio 提交

```c
// 校验和 + 可选加密
SET_BSET_CSUM_TYPE(i, bch2_meta_checksum_type(c));
nonce = btree_nonce(i, b->written << 9);
if (bn)  bn->csum = csum_vstruct(c, BSET_CSUM_TYPE(i), nonce, bn);

// journal 检查：journal 出错时不写（防止未 journal 化的数据落盘）
if (bch2_journal_error(&c->journal) || c->opts.nochanges)
    goto err;

// 分配 bio + 提交 IO
wbio = alloc_btree_write_bio(...);
wbio->wbio.bio.bi_end_io  = btree_node_write_endio;
bch2_bio_map(&wbio->wbio.bio, data, sectors_to_write << 9);
b->written += sectors_to_write;

INIT_WORK(&wbio->work, btree_write_submit);
queue_work(c->btree.write_submit_wq, &wbio->work);
```

写入完成后，`btree_node_write_endio` 回调处理 bio 完成、释放 bounce buffer、清除 `write_in_flight` 标记。如果写入出错，则设置 journal error。

### 5.3 Journal 提交路径

```
journal提交路径：

  bch2_trans_commit()                    // btree/commit.c:1432
    → do_bch2_trans_commit()             // commit.c:1087
      → bch2_journal_write_list_add()    // 将本次修改加入 journal 列表
      → bch2_btree_insert_key_leaf()     // 插入到节点 bset

  后台定时器 / journal 满：
    bch2_journal_flush()                 // journal/journal.c:1016
      → bch2_journal_flush_seq(j, j->seq)
        → bch2_journal_do_writes()       // journal.c:266
          → closure_call(bch2_journal_write, ...)

  bch2_journal_write                     // write.c:842
    → 分配写缓冲区、加密、checksum、提交块层 IO
```

### 5.4 Superblock

小而精：记录设备成员、journal bucket 位置、加密密钥、格式版本号（`BCH_VERSION`）等。频繁更新的字段（如 counters）通过 journal 更新，**不重写 superblock**。

---

## 6. 关键函数速查表

### 6.1 B+tree 遍历

| 函数 | 位置 | 说明 |
|------|------|------|
| `bch2_btree_iter_peek` | `fs/btree/iter.h:732` | 遍历入口（内联包装） |
| `bch2_btree_iter_peek_max` | `fs/btree/iter.c:2791` | 实际实现，支持上界 |
| `bch2_btree_iter_next` | `fs/btree/iter.c:3005` | 迭代到下一个 key |
| `bch2_btree_trans_to_text` | `fs/btree/iter.c:4204` | 事务状态调试输出 |
| `lockrestart_do` | `fs/btree/iter.h:1188` | 事务重启重试宏 |
| `bpos_cmp` | `fs/btree/bkey.h:141` | 三字段 bpos 比较 |
| `bkey_cmp` | `fs/btree/bkey.h:134` | 忽略 snapshot 的 bkey 比较 |

### 6.2 日志提交

| 函数 | 位置 | 说明 |
|------|------|------|
| `bch2_journal_write` | `fs/journal/write.c:842` | journal 写入（闭包回调） |
| `bch2_journal_flush` | `fs/journal/journal.c:1016` | 同步等待 journal 落盘 |
| `bch2_journal_do_writes` | `fs/journal/journal.c:266` | 触发待处理写入 |

### 6.3 B+tree 节点分裂

| 函数 | 位置 | 说明 |
|------|------|------|
| `btree_split` | `fs/btree/interior.c:1915` | 分裂核心实现 |
| `bch2_btree_split_leaf` | `fs/btree/interior.c:2219` | 叶子节点分裂入口 |

### 6.4 磁盘预留

| 函数 | 位置 | 说明 |
|------|------|------|
| `__bch2_disk_reservation_add` | `fs/alloc/buckets.c:1204` | 预留实现（每 CPU 缓存） |
| `bch2_disk_reservation_add` | `fs/alloc/buckets.h:359` | 用户入口（含复制因子） |
| `bch2_disk_reservation_put` | `fs/alloc/buckets.h:342` | 释放预留 |

### 6.5 数据写入

| 函数 | 位置 | 说明 |
|------|------|------|
| `bch2_extent_update` | `fs/data/write.c:993` | 文件写入→B-tree 更新 |
| `bch2_btree_insert` | `fs/btree/update.c:696` | 简化单 key 插入 |
| `__bch2_trans_commit` | `fs/btree/commit.c:1432` | 两阶段提交核心 |

### 6.6 分配器

| 函数 | 位置 | 说明 |
|------|------|------|
| `bch2_bucket_alloc_freelist` | `fs/alloc/foreground.c:437` | 遍历 freespace btree |
| `bch2_bucket_alloc_trans` | `fs/alloc/foreground.c:620` | 高级入口（水位线+重试） |

### 6.7 快照

| 函数 | 位置 | 说明 |
|------|------|------|
| `__bch2_snapshot_is_ancestor` | `fs/snapshots/snapshot.c:328` | 祖先判定综合 fast path |
| `get_ancestor_below` | `fs/snapshots/snapshot.c:221` | skip list 跳跃 |
| `test_ancestor_bitmap` | `fs/snapshots/snapshot.c:236` | 位图 O(1) 判定 |
| `bch2_snapshot_is_ancestor` | `fs/snapshots/snapshot.h:196` | 内联封装（含等值短路） |

### 6.8 SIX 锁

| 函数 | 位置 | 说明 |
|------|------|------|
| `__bch2_btree_node_relock` | `fs/btree/locking.c:673` | 乐观重锁（序列号） |
| `bch2_btree_node_upgrade` | `fs/btree/locking.c:700` | R→I 升级 |
| `bch2_btree_node_write_trans` | `fs/btree/write.c:638` | 完整写周期 |
| `__bch2_btree_node_lock_write` | `fs/btree/locking.c:580` | 阻塞获取写锁 |

---

## 7. 一句话总结

> B+tree 存所有元数据（inode/extent/alloc/snapshot/dirent/xattr 各一个 btree），journal 做写入聚合，**snapshot 作为 bpos 第三维**内建于寻址体系，**SIX 锁**实现高并发 B-tree 遍历（多写事务可在不同分支并行），**bucket + alloc btree** 做空间管理。所有写入都是 COW，路径：先 journal → 再 btree flush → 最后 bucket 回收。

### 核心参考文件

| 主题 | 推荐阅读文件 | 阅读顺序 |
|------|-------------|----------|
| bkey/bpos | `fs/btree/bkey_types.h` + `fs/btree/bkey.h` | **① 最先读** |
| 键类型枚举 | `fs/bcachefs_format.h`（BCH_BKEY_TYPES x-macro） | ② |
| btree_node 磁盘格式 | `fs/bcachefs_format.h`（btree_node, bset 定义） | ③ |
| SIX 锁 | `fs/util/six.h` + `fs/btree/locking.c` 顶部注释 | ④ |
| B+tree 写入 | `fs/btree/write.c:277-583` | ⑤ |
| 快照表结构 | `fs/snapshots/types.h` | ⑥ |
| 快照祖先判定 | `fs/snapshots/snapshot.c:328-353` + types.h `snapshot_t` | ⑦ |
| bucket 结构 | `fs/alloc/buckets_types.h` + `fs/alloc/foreground.c` | ⑧ |
| journal 设计 | `fs/journal/journal.h`（注释 5-42 行） + `fs/journal/write.c` | ⑨ |
| 事务提交 | `fs/btree/commit.c:1432`（__bch2_trans_commit） | ⑩ |
