// 备注： Journal 模块 - Rust 绑定层
// 备注：
// 备注： 本模块提供 Journal 数据结构的 Rust 封装，包括：
// 备注：
// 备注： 1. jset (Journal Set) - 日志条目的集合容器
// 备注：    - 在磁盘上表示为一组有序的日志条目
// 备注：    - 包含头部元数据和条目数组
// 备注：
// 2. jset_entry (Journal Set Entry) - 单条日志条目
//    - 可以是 B-tree 更新、快照标记、计数器等
//    - 每个条目包含类型、所属 B-tree ID、数据负载
//
// 3. 迭代器接口
//    - JsetEntryIter: 遍历 jset 中的所有条目
//    - JsetEntryKeyIter: 遍历 jset_entry 中的所有键
//
// # 内存布局
//
// jset 结构（在磁盘上）：
// ```text
// |--------------------------|
// | jset {                  |
// |   u64s: 数量             |
// |   flags: 标志位           |
// |   ...                   |
// |   start[0]: 第一个条目   |
// | }                        |
// |--------------------------|
// | jset_entry_1 {          | <- start 指针
// |   type_, btree_id, u64s |
// |   _data: 变长数据        |
// | }                        |
// |--------------------------|
// | jset_entry_2 {          | <- vstruct_next_entry 计算
// |   ...                   |
// | }                        |
// |--------------------------|
// ```
//
// # 使用示例
//
// ```rust
// // 遍历 journal 中的所有条目
// for entry in jset_entries(&jset) {
//     // 获取条目类型
//     if let Some(entry_type) = entry_type(entry) {
//         // 处理不同类型的条目
//     }
//     // 遍历条目中的键
//     for key in jset_entry_keys(entry) {
//         // 处理键数据
//     }
// }
// ```

use crate::c;
use crate::util::vstructs::vstruct_next_entry;
use core::marker::PhantomData;

#[allow(non_camel_case_types)]
pub type journal_entry_type = c::bch_jset_entry_type;

/// Pointer to one past the last entry in a jset.
// 备注： 获取 jset 中最后一个条目之后的指针
// 备注：
// 备注： jset 头部占 56 字节，加上 u64s * 8 的数据区
unsafe fn vstruct_last_jset(jset: *const c::jset) -> *const c::jset_entry {
    let u64s = u32::from_le((*jset).u64s) as usize;
    (jset as *const u8).add(56 + u64s * 8) as *const c::jset_entry
}

/// bkey_next: advance past a bkey_i.
// 备注：  bkey_next_raw: 跳过 bkey_i 结构体，移动到下一个键的位置
// 备注：
// 备注：每个 bkey_i 的长度由 k.u64s 决定（变长结构）
unsafe fn bkey_next_raw(k: *const c::bkey_i) -> *const c::bkey_i {
    let u64s = (*k).k.u64s as usize;
    (k as *const u8).add(u64s * 8) as *const c::bkey_i
}

// ---- jset helpers ----

/// Total byte size of a jset including header.
// 备注：jset_vstruct_bytes - 计算 jset 的总字节数（包括头部）
// 备注：
// 备注：jset 头部占 56 字节，之后是 u64s 个 u64（每 个 8 字节）
// 备注：
// 备注：# 参数
// 备注：* `jset` - jset 引用
// 备注：
// 备注：# 返回值
// 备注：jset 结构占用的总字节数
pub fn jset_vstruct_bytes(jset: &c::jset) -> usize {
    let u64s = u32::from_le(jset.u64s) as usize;
    56 + u64s * 8
}

/// Number of sectors occupied by a jset on disk.
// 备注：jset_vstruct_sectors - 计算 jset 占用的磁盘扇区数
// 备注：
// 备注：根据块大小对齐计算所需的扇区数：
// 备注：1. 先计算字节大小
// 备注：2. 按 block_size 对齐向上取整
// 备注：3. 转换为扇区数（每扇区 512 字节）
// 备注：
// 备注：# 参数
// 备注：* `jset` - jset 引用
// 备注：* `block_bits` - 块大小指数 (block_size = 512 << block_bits)
// 备注：
// 备注：# 返回值
// 备注：jset 占用的扇区数（用于磁盘 I/O）
pub fn jset_vstruct_sectors(jset: &c::jset, block_bits: u16) -> usize {
    let bytes = jset_vstruct_bytes(jset);
    let block_size = 512usize << block_bits;
    (bytes.div_ceil(block_size) * block_size) >> 9
}

/// JSET_NO_FLUSH bitfield: bit 5 of le32 flags.
// 备注：jset_no_flush - 检查 JSET_NO_FLUSH 标志
// 备注：
// 备注：JSET_NO_FLUSH 是 jset flags 的第 5 位：
// 备注：- 如果设置，表示该 jset 不需要强制刷新到磁盘
// 备注：- 用于优化：某些 jset 可以延迟写入
// 备注：
// 备注：# 参数
// 备注：* `jset` - jset 引用
// 备注：
// 备注：# 返回值
// 备注：true 如果设置了 JSET_NO_FLUSH 标志
pub fn jset_no_flush(jset: &c::jset) -> bool {
    (u32::from_le(jset.flags) >> 5) & 1 != 0
}

// ---- vstruct iterators ----

/// Iterator over jset_entry references within a jset.
// 备注：JsetEntryIter - jset 中条目的迭代器
// 备注：
// 备注：安全保证：
// 备注：- 迭代器不持有生命周期，只引用数据
// 备注：- 调用者必须确保 jset 在迭代期间有效
// 备注：
// 备注：# 迭代过程
// 备注：1. 从 start 指针开始
// 备注：2. 每次调用 next() 使用 vstruct_next_entry 跳到下一个条目
// 备注：3. 直到 cur >= end 为止
pub struct JsetEntryIter<'a> {
    // 备注：当前条目指针
    cur: *const c::jset_entry,
    // 备注：结束边界
    end: *const c::jset_entry,
    // 备注：生命周期标记
    _phantom: PhantomData<&'a c::jset>,
}

impl<'a> Iterator for JsetEntryIter<'a> {
    type Item = &'a c::jset_entry;

    fn next(&mut self) -> Option<Self::Item> {
        // 备注：检查是否到达结束位置
        if self.cur >= self.end {
            return None;
        }

        // 备注：安全读取当前条目
        let entry = unsafe { &*self.cur };

        // 备注：计算下一个条目的位置
        let next = unsafe { vstruct_next_entry(self.cur) };

        // 备注：边界检查：确保没有越界
        if next > self.end {
            return None;
        }

        self.cur = next;
        Some(entry)
    }
}

// 备注：jset_entries - 创建 jset 条目的迭代器
// 备注：
// 备注：# 参数
// 备注：* `jset` - jset 引用
// 备注：
// 备注：# 返回值
// 备注：JsetEntryIter 迭代器
pub fn jset_entries(jset: &c::jset) -> JsetEntryIter<'_> {
    let start = jset.start.as_ptr();
    let end = unsafe { vstruct_last_jset(jset as *const c::jset) };
    JsetEntryIter { cur: start, end, _phantom: PhantomData }
}

/// Iterator over bkey_i references within a jset_entry.
// 备注：JsetEntryKeyIter - jset_entry 中键的迭代器
// 备注：
// 备注：每个 jset_entry 可能包含多个 bkey_i 键：
// 备注：- B-tree 键数据（extent、inode 等）
// 备注：- 每个键的长度由 k.u64s 决定
// 备注：
// 备注：# 迭代过程
// 备注：1. 从 entry->start 开始
// 备注：2. 使用 bkey_next_raw 跳到下一个键
// 备注：3. 遇到 u64s == 0 时停止（表示无效键）
pub struct JsetEntryKeyIter<'a> {
    cur: *const c::bkey_i,
    end: *const c::bkey_i,
    _phantom: PhantomData<&'a c::jset_entry>,
}

impl<'a> Iterator for JsetEntryKeyIter<'a> {
    type Item = &'a c::bkey_i;

    fn next(&mut self) -> Option<Self::Item> {
        // 备注：检查是否到达结束位置
        if self.cur >= self.end {
            return None;
        }

        // 备注：安全读取当前键
        let k = unsafe { &*self.cur };

        // 备注：u64s == 0 表示无效键，停止迭代
        if k.k.u64s == 0 {
            return None;
        }

        // 备注：计算下一个键的位置
        let next = unsafe { bkey_next_raw(self.cur) };

        // 备注：边界检查
        if next > self.end {
            return None;
        }

        self.cur = next;
        Some(k)
    }
}

// 备注：jset_entry_keys - 创建 jset_entry 中键的迭代器
// 备注：
// 备注：# 参数
// 备注：* `entry` - jset_entry 引用
// 备注：
// 备注：# 返回值
// 备注：JsetEntryKeyIter 迭代器
pub fn jset_entry_keys(entry: &c::jset_entry) -> JsetEntryKeyIter<'_> {
    let start = entry.start.as_ptr();
    let end = unsafe { vstruct_next_entry(entry as *const c::jset_entry) as *const c::bkey_i };
    JsetEntryKeyIter { cur: start, end, _phantom: PhantomData }
}

// ---- entry type conversion ----

/// Convert entry type byte to the raw journal entry type.
// 备注：将条目类型字节转换为枚举（如果已知类型）
// 备注：
// 备注：jset_entry.type_ 字段存储原始字节，转换为 bch_jset_entry_type 枚举：
// 备注：- 如果类型值在有效范围内，返回 Some(enum_value)
// 备注：- 如果超出范围，返回 None（未知类型）
// 备注：
// 备注：# 参数
// 备注：* `entry` - jset_entry 引用
// 备注：
// 备注：# 返回值
// 备注：Some(类型枚举) 或 None
pub fn entry_type(entry: &c::jset_entry) -> c::bch_jset_entry_type {
    c::bch_jset_entry_type(entry.type_ as u32)
}

pub fn entry_type_is_known(t: c::bch_jset_entry_type) -> bool {
    t.0 < journal_entry_type::nr.0
}

/// Convert entry btree_id byte to the enum, if it's a known btree.
pub fn entry_btree_id(entry: &c::jset_entry) -> Option<c::btree_id> {
    c::btree_id::from_raw(entry.btree_id as u32)
}

// ---- jset_entry_log helpers ----

/// Get log message bytes from a jset_entry of type log.
/// Layout: jset_entry header (8 bytes) followed by d[] message bytes.
pub fn entry_log_msg(entry: &c::jset_entry) -> &[u8] {
    let msg_bytes = u16::from_le(entry.u64s) as usize * 8;
    if msg_bytes == 0 {
        return &[];
    }
    let ptr = entry as *const c::jset_entry as *const u8;
    let data = unsafe { core::slice::from_raw_parts(ptr.add(8), msg_bytes) };
    // Trim trailing nulls
    let len = data.iter().rposition(|&b| b != 0).map_or(0, |i| i + 1);
    &data[..len]
}

pub fn entry_log_str_eq(entry: &c::jset_entry, s: &str) -> bool {
    let msg = entry_log_msg(entry);
    msg.len() >= s.len() && &msg[..s.len()] == s.as_bytes()
}
