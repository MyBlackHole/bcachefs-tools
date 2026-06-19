// ============================================
// 备注：bcachefs IOCTL 编号计算
//
// 备注：Linux 内核 IOCTL 编号编码方式（include/uapi/asm-generic/ioctl.h）：
//   _IOW(type, nr, size)  = (1<<30) | (size<<16) | (type<<8) | nr
//   _IOWR(type, nr, size) = (3<<30) | (size<<16) | (type<<8) | nr
//
// 备注：bcachefs 使用 type=0xbc，nr 按功能分配（4=disk_add, 16=subvol_create 等）。
// 备注：size 编码在类型参数 T 中，由编译器计算 sizeof(T)。
//
// 备注：v2/v1 兼容：v2 IOCTL 在相同 nr 上添加错误消息缓冲区，
// 备注：如果内核不支持 v2 则返回 ENOTTY，fallback 到 v1。
// ============================================
use std::mem;

/// Compute a bcachefs _IOW ioctl number.
///
/// Equivalent to `_IOW(0xbc, nr, T)` — write direction, bcachefs magic,
/// size encoded from the type parameter.
pub const fn bch_ioc_w<T>(nr: u32) -> libc::Ioctl {
    ((1u32 << 30) | ((mem::size_of::<T>() as u32) << 16) | (0xbcu32 << 8) | nr) as libc::Ioctl
}

/// Compute a bcachefs _IOWR ioctl number.
pub const fn bch_ioc_wr<T>(nr: u32) -> libc::Ioctl {
    ((3u32 << 30) | ((mem::size_of::<T>() as u32) << 16) | (0xbcu32 << 8) | nr) as libc::Ioctl
}

