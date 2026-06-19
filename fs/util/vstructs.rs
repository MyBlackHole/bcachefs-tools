use crate::c;

// vstruct_next(entry) = (u64*)entry._data + le16(entry.u64s)
// 备注：jset_entry 结构体大小为 8 字节
// 备注：_data 字段位于偏移 0 处（u64 灵活数组）
// 备注：
// 备注：计算下一个条目的起始地址：
// 备注：1. 获取当前条目的 u64s 字段（小端序）
// 备注：2. 偏移 = 8 字节头部 + u64s * 8 字节数据
pub(crate) unsafe fn vstruct_next_entry(entry: *const c::jset_entry) -> *const c::jset_entry {
    let u64s = u16::from_le((*entry).u64s) as usize;
    (entry as *const u8).add(8 + u64s * 8) as *const c::jset_entry
}
