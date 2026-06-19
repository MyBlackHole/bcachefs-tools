# BTREE_ITER_slots 内部机制与 get_key_or_hole 实现

> 分析日期：2026-05-17
> 隶属分析系列：[启动分配器分析索引](bcachefs-alloc-analysis-index.md)

---

## 目录

1. [`BTREE_ITER_slots` 概述](#btree_iter_slots-概述)
2. [标志定义与路由](#标志定义与路由)
3. [`peek` vs `peek_slot`](#peek-vs-peek_slot)
4. [核心原语：`bch2_btree_path_peek_slot`](#核心原语bch2_btree_path_peek_slot)
5. [空洞合成代码路径](#空洞合成代码路径)
6. [合成已删除键的区分](#合成已删除键的区分)
7. [推进到下一个槽位](#推进到下一个槽位)
8. [`bch2_get_key_or_hole` 完整实现](#bch2_get_key_or_hole-完整实现)
9. [`dev_freespace_init_iter` 内循环](#dev_freespace_init_iter-内循环)
10. [`peek_all` vs `peek`](#peek_all-vs-peek)
11. [参考代码位置](#参考代码位置)

---

## `BTREE_ITER_slots` 概述

`BTREE_ITER_slots` 是 bcachefs btree 迭代器的一种特殊模式，允许调用者查询**精确位置**上的键是否存在。Btree 没有单独的"槽位"概念——槽位是迭代器 API 添加的一层约定。底层节点迭代器被定位在最近的真实键上，如果位置不匹配则合成已删除键。

---

## 标志定义与路由

```c
// fs/btree/types.h:449-519 — 通过 x-macro 定义
#define BTREE_ITER_FLAGS() \
    x(slots)                // 位 0
    x(intent)               // 位 1
    // ...

// 展开后：BTREE_ITER_slots = 1U << 0 = 0x1
```

调用 `bch2_btree_iter_peek_type` 时根据标志路由：

```c
// fs/btree/iter.h:1127-1131
static inline struct bkey_s_c bch2_btree_iter_peek_type(
    struct btree_iter *iter, enum btree_iter_update_trigger_flags flags)
{
    return flags & BTREE_ITER_slots ? bch2_btree_iter_peek_slot(iter)
                                    : bch2_btree_iter_peek(iter);
}
```

---

## `peek` vs `peek_slot`

| 特性 | `peek()`（常规） | `peek_slot()`（槽位模式） |
|---|---|---|
| 查找目标 | 找到下一个存在的键 >= iter->pos | 查找确切位于 iter->pos 的键 |
| 如果该位置不存在键 | 自动跳过，继续扫描 | **合成一个已删除的键都在该位置** |
| 返回值 | 真实存在的键，或 NULL | 真实键、合成已删除键，或 NULL（仅缓存路径） |
| 推进 iter->pos | 推进到返回的键之后 | 不推进 |
| 白名单键 | 跳过 | 返回（可能会转换类型为已删除） |

常规 `peek` 适用于"查找下一项"的遍历场景（如读文件、遍历目录）。`peek_slot` 适用于精确位置查询（如桶分配器检查指定 bucket 是否空闲）。

---

## 核心原语：`bch2_btree_path_peek_slot`

**文件：** `fs/btree/iter.c:2181-2218`

```c
struct bkey_s_c bch2_btree_path_peek_slot(struct btree_path *path, struct bkey *u)
{
    struct btree_path_level *l = path_l(path);
    struct bkey_packed *_k;
    struct bkey_s_c k;

    if (unlikely(!l->b))
        return bkey_s_c_null;

    EBUG_ON(path->uptodate != BTREE_ITER_UPTODATE);
    EBUG_ON(!btree_node_locked(path, path->level));

    if (!path->cached) {
        // ★ 非缓存路径：核心槽位查找
        _k = bch2_btree_node_iter_peek_all(&l->iter, l->b);
        // peek_all 返回原始打包键，不跳过任何键类型
        k = _k ? bkey_disassemble(l->b, _k, u) : bkey_s_c_null;

        EBUG_ON(k.k && bkey_deleted(k.k) && bpos_eq(k.k->p, path->pos));

        if (!k.k || !bpos_eq(path->pos, k.k->p)) {
            // ← 没有匹配的键！合成一个已删除的键
            bkey_init(u);           // type=KEY_TYPE_deleted(0), size=0, pos=(0,0)
            u->p = path->pos;       // 覆盖为确切的槽位位置
            return (struct bkey_s_c) { u, NULL };  // .v == NULL！
        }
    } else {
        // 缓存路径：从 key cache 获取
        struct bkey_cached *ck = (void *) path->l[0].b;
        if (!ck) return bkey_s_c_null;  // ← 缓存路径可能返回 NULL！
        *u = ck->k->k;
        k = (struct bkey_s_c) { u, &ck->k->v };
    }
    return k;
}
```

### 槽位合成的两个触发条件

- **`!k.k`**：节点迭代器已到达 bset 末尾，节点中没有 >= 当前位置的键
- **`!bpos_eq(path->pos, k.k->p)`**：找到了一个键，但它的位置 != 请求的槽位位置

关键：合成键的 `.v = NULL` 区别于真实删除键。

### 非缓存路径的关键差异

对于非缓存路径，此函数**永远不会返回 NULL**——它总是合成一个已删除键。只有缓存路径可能返回 NULL（无缓存条目）。`peek_slot` 调用者通过 `unlikely(!k.k)` 处理这个情况。

---

## 空洞合成代码路径

`peek_slot` 的完整入口路径（`fs/btree/iter.c:3273-3431`）：

```
bch2_btree_iter_peek_slot(iter)
  │
  ├─ btree_iter_search_key(iter)         // 计算搜索用的 key
  ├─ bch2_btree_path_set_pos(trans, path, search_key, ...)  // 设置路径位置
  ├─ bch2_btree_path_traverse(trans, path, flags)           // 遍历到叶子节点
  │    └─ bch2_btree_node_iter_init(c, l->b, &l->iter, &path->pos)
  │       // ↑ 二分查找 bset 中第一个 >= path->pos 的键
  │       //   如果没有这样的键，迭代器指向末尾
  │
  ├─ 分裂为两条路径：
  │
  ├─ 路径 A（非 extent + 非快照过滤 || cached）
  │    └─ bch2_btree_trans_peek_slot_updates()  // 检查未提交的事务更新
  │    └─ btree_trans_peek_slot_journal()        // 检查 journal
  │    └─ bch2_btree_path_peek_slot(path, &iter->k)  // ★ 核心函数
  │    │    └─ bch2_btree_node_iter_peek_all()  // 看当前迭代器位置的键
  │    │    └─ 如果位置不匹配 → bkey_init + u->p = path->pos → 合成已删除键
  │    └─ btree_trans_peek_key_cache()          // 检查 key cache
  │    └─ 白名单过滤                            // 将白名单转成已删除
  │
  └─ 路径 B（extent 或快照过滤）
       └─ CLASS(btree_iter_copy, iter2)(iter)
       └─ bch2_btree_iter_peek_max(&iter2, end)  // 向前扫描
       └─ 如果找到的键的位置 > iter->pos → 空洞
            └─ bkey_init(u); u->p = iter->pos
            └─ 如果是 extent 模式，调整大小填充空洞
```

---

## 合成已删除键的区分

调用者通过检查 `.v == NULL` 来区分"合成已删除键"和"真实已删除键"：

```c
// 合成已删除键：.v = NULL（bch2_btree_path_peek_slot 第 2203 行）
// 真实已删除键：.v = 指向键值的指针

if (k.k->type == KEY_TYPE_deleted && !k.v)
    /* 合成——此槽位无真实键 */;
```

在 alloc btree 场景中，`bch2_alloc_to_v4` 处理已删除键时返回全零 `alloc_v4`（`data_type=0, dirty_sectors=0`），这被视为空闲桶。所以分配器不需要显式区分合成 vs 真实删除。

---

## 推进到下一个槽位

```c
// fs/btree/iter.c:3433-3439
struct bkey_s_c bch2_btree_iter_next_slot(struct btree_iter *iter)
{
    if (!bch2_btree_iter_advance(iter))
        return bkey_s_c_null;
    return bch2_btree_iter_peek_slot(iter);
}

// fs/btree/iter.c:2408-2419
inline bool bch2_btree_iter_advance(struct btree_iter *iter)
{
    struct bpos pos = iter->k.p;                   // 上一个返回键的位置
    bool ret = !(iter->flags & BTREE_ITER_all_snapshots
                 ? bpos_eq(pos, SPOS_MAX)
                 : bkey_eq(pos, SPOS_MAX));
    if (ret && !(iter->flags & BTREE_ITER_is_extents))
        pos = bkey_successor(iter, pos);            // 增加 offset
    bch2_btree_iter_set_pos(iter, pos);
    return ret;
}

// fs/btree/iter.c:212-218
static inline struct bpos bkey_successor(struct btree_iter *iter, struct bpos p)
{
    return iter->flags & BTREE_ITER_all_snapshots
        ? bpos_successor(p)              // = POS(inode, offset+1)
        : bpos_with_snapshot(            // 带快照版本的 successor
            bpos_nosnap_successor(p), iter->snapshot);
}
```

**关键代价：每个 `next_slot` 都包含一次完整的 `path_set_pos` + `path_traverse`。**

在 `bch2_bucket_alloc_early` 中，这意味逐槽位扫描的代价是 O(n) 次遍历。因此它只在 `freespace_initialized == false` 的备用路径中使用。

---

## `bch2_get_key_or_hole` 完整实现

**文件：** `fs/alloc/check.c:17-58`

这是 `dev_freespace_init_iter` 的核心辅助函数。它使用 `peek_slot` 检查当前位置，并在发现空洞时合成一个覆盖整个空洞的 extent。

```c
/*
 * Synthesizes deleted extents for holes, similar to BTREE_ITER_slots for
 * extents style btrees, but works on non-extents btrees:
 */
static struct bkey_s_c bch2_get_key_or_hole(
    struct btree_iter *iter, struct bpos end, struct bkey *hole)
{
    /* (1) peek_slot 检查当前槽位 */
    struct bkey_s_c k = bch2_btree_iter_peek_slot(iter);

    if (bkey_err(k)) return k;

    if (k.k->type) {
        /* (2a) 存在真实键 → 直接返回 */
        return k;
    } else {
        /* (2b) 空洞 → 合成已删除 extent */
        CLASS(btree_iter_copy, iter2)(iter);  // 克隆迭代器

        /* 边界上限：不越过当前 leaf node */
        struct btree_path *path = btree_iter_path(iter->trans, iter);
        if (!bpos_eq(path->l[0].b->key.k.p, SPOS_MAX))
            end = bkey_min(end,
                bpos_nosnap_successor(path->l[0].b->key.k.p));

        /* 安全边界：空洞不超过 U32_MAX 个位置 */
        end = bkey_min(end,
            POS(iter->pos.inode, iter->pos.offset + U32_MAX - 1));

        /* (6) peek_max 找到下一个真实键的位置 */
        k = bch2_btree_iter_peek_max(&iter2, end);
        if (bkey_err(k)) return k;

        struct bpos next = iter2.pos;  // 下一个真实键的位置
        BUG_ON(next.offset >= iter->pos.offset + U32_MAX);

        /* 合成空洞 extent */
        bkey_init(hole);
        hole->p = iter->pos;                       // 起始 = 当前位置
        bch2_key_resize(hole, next.offset - iter->pos.offset);  // 大小 = 到下一个键的距离
        return (struct bkey_s_c) { hole, NULL };   // .v = NULL 标记合成
    }
}
```

### 机制要点

1. **`peek_slot` 查当前位置**：确定是否有真实键还是空洞
2. **`peek_max` 查下一个键**：找到空洞的右边界
3. **双重边界限制**：不超出 leaf node 末尾、不超过 U32_MAX
4. **合成 extent**：`hole->p = iter->pos`，`hole->size = next.offset - iter->pos.offset`

### 迭代器推进

`get_key_or_hole` **自身不推进迭代器**，由调用者负责。这样调用者可以选择跳过整个空洞：

```c
// 查到真实键 → bch2_btree_iter_advance(iter);  // 步进到下一个
// 查到空洞 → bch2_btree_iter_set_pos(iter, k.k->p); // 跳到空洞末尾
```

### 所有调用者

| 调用者 | 文件 | 行号 | 说明 |
|---|---|---|---|
| `bch2_get_key_or_real_bucket_hole` | `check.c` | 85-113 | 封装 + 设备边界验证，跳过无效设备范围 |
| `dev_freespace_init_iter` | `check.c` | 755-790 | freespace 初始化主循环 |
| `bch2_check_alloc_hole_freespace` | `check.c` | 235 | fsck 时验证空洞在 freespace btree 中有对应条目 |

---

## `dev_freespace_init_iter` 内循环

**文件：** `fs/alloc/check.c:755-790`

这是 freespace 初始化的每步动作：

```c
static int dev_freespace_init_iter(struct btree_trans *trans,
    struct bch_dev *ca, struct btree_iter *iter, struct bpos end)
{
    struct bkey hole;
    struct bkey_s_c k = bkey_try(bch2_get_key_or_hole(iter, end, &hole));

    if (k.k->type) {
        // 真实 alloc key 存在：
        struct bch_alloc_v4 a_convert;
        const struct bch_alloc_v4 *a = bch2_alloc_to_v4(k, &a_convert);
        try(bch2_bucket_do_freespace_index(trans, ca, k, a, true));
        try(bch2_trans_commit(trans, NULL, NULL, BCH_TRANS_COMMIT_no_enospc));
        bch2_btree_iter_advance(iter);           // → 步进到下一个位置
    } else {
        // 空洞：插入 KEY_TYPE_set 到 freespace btree
        struct bkey_i *freespace =
            errptr_try(bch2_trans_kmalloc(trans, sizeof(*freespace)));
        bkey_init(&freespace->k);
        freespace->k.type   = KEY_TYPE_set;
        freespace->k.p      = k.k->p;
        freespace->k.size   = k.k->size;
        try(bch2_btree_insert_trans(trans, BTREE_ID_freespace, freespace, 0));
        try(bch2_trans_commit(trans, NULL, NULL, BCH_TRANS_COMMIT_no_enospc));
        bch2_btree_iter_set_pos(iter, k.k->p);   // → 跳到空洞末尾
    }
    return 0;
}
```

### 效率分析

对于有 N 个真实 alloc key 的设备：
- 每个真实 key：1 次 `advance` + 1 次 `peek_slot`（真正遍历到下一个位置）
- 每个空洞区域：1 次 `set_pos`（跳到空洞末尾），跳过中间所有空槽位

总遍历次数 ≈ O(N)，而不是 O(bucket 总数)。

---

## `peek_all` vs `peek`

```c
// fs/btree/bset.h:382-400
peek_all(): 返回当前位置的所有键（包括已删除的）
    ↓
    用于 peek_slot（需要看到精确位置的任何键）

peek(): 自动跳过已删除键
    ↓
    用于常规 peek（不需要已删除键，跳过它们更快）
```

---

## 参考代码位置

| 文件 | 行号 | 函数/定义 |
|---|---|---|
| `fs/btree/types.h` | 449-530 | `BTREE_ITER_FLAGS()` x-macro，`BTREE_ITER_slots` 定义 |
| `fs/btree/iter.h` | 1127-1131 | `bch2_btree_iter_peek_type` — slots 路由 |
| `fs/btree/iter.c` | 212-218 | `bkey_successor` — 推进到下一个位置 |
| `fs/btree/iter.c` | 2181-2218 | `bch2_btree_path_peek_slot` — 核心槽位查找原语 |
| `fs/btree/iter.c` | 2408-2419 | `bch2_btree_iter_advance` — 推进迭代器 |
| `fs/btree/iter.c` | 3273-3431 | `bch2_btree_iter_peek_slot` — 槽位模式完整实现 |
| `fs/btree/iter.c` | 3433-3439 | `bch2_btree_iter_next_slot` — 推进并重新 peek |
| `fs/btree/iter.c` | 771 | `bch2_btree_node_iter_init` — 二分查找定位 |
| `fs/btree/bset.h` | 374-387 | `bch2_btree_node_iter_peek_all` |
| `fs/btree/bset.h` | 391-400 | `bch2_btree_node_iter_peek` |
| `fs/alloc/check.c` | 17-58 | `bch2_get_key_or_hole` |
| `fs/alloc/check.c` | 755-790 | `dev_freespace_init_iter` |
| `fs/bcachefs_format.h` | 379-390 | `bkey_init` |
| `fs/bcachefs_format.h` | 418-505 | `KEY_TYPE_deleted = 0`，白名单类型 |
| `fs/btree/bkey_types.h` | 123-131 | `bkey_deleted` / `bkey_whiteout` / `bkey_extent_whiteout` |
