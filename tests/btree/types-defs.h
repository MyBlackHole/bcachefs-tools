/*
 * types-defs.h — bcachefs 值类型 X-macro 定义
 *
 * 本文件是 val-types.c 的教学配套头文件，定义两种 X-macro 模式：
 *
 *   VAL_TYPES()   — BCH_BKEY_TYPES() 风格的类型枚举
 *                   x(name, nr, flags, desc)
 *                   对齐 fs/bcachefs_format.h:416
 *
 *   INODE_FIELDS() — bkey_fields() 风格的字段遍历
 *                    x(name, member, type, bits, desc)
 *                    对齐 fs/btree/bkey.h:631
 *
 * 两种宏均在 val-types.c 中以 #define x / #undef x 模式展开，
 * 每用途一次展开，与内核完全一致。
 *
 * 注意：
 *   nr 值使用内核标准 enum bkey_type 编号（extent=6, inode=8, btree_ptr_v2=18），
 *   与 persist.c 中教学简化 enum（extent=2, inode=3, btree_ptr_v2=7）不同。
 */
#ifndef _TYPES_DEFS_H
#define _TYPES_DEFS_H

/*
 * VAL_TYPES() — 内核 BCH_BKEY_TYPES() 对齐版
 *
 * 参数:
 *   name  — 类型名（生成 bkey_i_##name / bch_##name 标识符）
 *   nr    — enum bkey_type 编号（内核标准值）
 *   flags — 内核为 BKEY_TYPE_strict_btree_checks 等，教学简化传 0
 *   desc  — 类型说明，用于文档生成
 *
 * 覆盖 6 种典型类型，展示内核真实多样性：
 *   deleted       — 零长度值类型（whiteout 标记）
 *   whiteout      — 零长度值类型（显式删除）
 *   inode         — 固定大小（42 字节），有随机字段访问
 *   dirent        — 变长字符串序列化
 *   extent        — 变长 tagged entry 数组
 *   btree_ptr_v2  — 固定头 + 变长设备指针数组
 *
 * 编号使用内核标准 enum bkey_type 值：
 *   persist.c 教学简化版：extent=2, inode=3, btree_ptr_v2=7
 *   内核实际：          extent=6, inode=8, btree_ptr_v2=18
 */
#define VAL_TYPES()                              \
    x(deleted,      0,  0, "transient deleted key") \
    x(whiteout,     1,  0, "persistent whiteout")   \
    x(extent,       6,  0, "File data extent")      \
    x(inode,        8,  0, "Inode metadata")         \
    x(dirent,      12,  0, "Directory entry")        \
    x(btree_ptr_v2,18, 0, "Btree node pointer v2")

/*
 * INODE_FIELDS() — 内核 bkey_fields() 的教学版，为 bch_inode 定义字段列表
 *
 * 参数:
 *   name   — 字段名标识符（用于生成 INODE_##name enum / get/set 函数名）
 *   member — struct bch_inode 中的成员名
 *   type   — C 类型（用于文档）
 *   bits   — 位宽（用于文档）
 *   desc   — 字段说明（用于文档）
 *
 * 注意: extent 和 btree_ptr_v2 不是固定字段结构，因此无对应 FIELDS() 宏。
 *       extent 是变长 tagged entry 数组，btree_ptr_v2 是固定头 + 变长数组。
 *
 * 内核对应: fs/btree/bkey.h:631 — bkey_fields()
 */
#define INODE_FIELDS()                              \
    x(mode,   mode,      uint16_t, 16, "文件权限模式")  \
    x(uid,    uid,       uint32_t, 32, "用户 ID")      \
    x(gid,    gid,       uint32_t, 32, "组 ID")        \
    x(size,   size,      uint64_t, 64, "文件大小")      \
    x(atime,  atime,     uint64_t, 64, "访问时间")      \
    x(mtime,  mtime,     uint64_t, 64, "修改时间")      \
    x(ctime,  ctime,     uint64_t, 64, "属性时间")

#endif /* _TYPES_DEFS_H */
