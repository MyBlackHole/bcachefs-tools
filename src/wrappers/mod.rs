// ============================================
// 备注：wrappers — bcachefs 用户空间 API 的 Rust 包装层
//
// 备注：本模块将 C IOCTL / sysfs 接口封装为类型安全的 Rust API。
// 备注：命令模块（src/commands/）通过这些 wrapper 与内核交互。
//
// 备注：子模块：
// 备注：  handle:     BcachefsHandle — 已挂载文件系统的 IOCTL 操作入口
// 备注：  bdev:       块设备大小和块大小的读取
// 备注：  super_io:   Superblock 的磁盘读写（纯 Rust 实现）
// 备注：  sb_display: Superblock 格式化输出（解决 C 分配器不匹配问题）
// 备注：  ioctl:      bcachefs IOCTL 编号计算（_IOW / _IOWR）
// 备注：  sysfs:      sysfs 属性读取（设备名、内核版本等）
// 备注：  accounting: 记账查询 IOCTL 封装
//
// 备注：辅助类型：
// 备注：  SbLockGuard — superblock 锁的 RAII 守卫
// ============================================
pub mod accounting;
pub mod bdev;
pub mod handle;
pub mod ioctl;
pub mod sb_display;
pub mod super_io;
pub mod sysfs;

/// Convert a bcachefs error code to a human-readable string.
pub fn bch_err_str(err: i32) -> std::borrow::Cow<'static, str> {
    unsafe { std::ffi::CStr::from_ptr(bch_bindgen::c::bch2_err_str(err)).to_string_lossy() }
}

/// RAII guard for the bch_fs superblock lock. Unlocks on drop.
// 备注：SbLockGuard — superblock 锁的 RAII 守卫。
// 备注：用于 protect 运行时的 superblock 写入访问（如 IOCTL 处理过程中）。
pub struct SbLockGuard(*mut libc::pthread_mutex_t);

impl Drop for SbLockGuard {
    fn drop(&mut self) {
        unsafe { libc::pthread_mutex_unlock(self.0); }
    }
}

/// Lock the superblock mutex and return a guard that unlocks on drop.
///
/// # Safety
/// `fs` must be a valid pointer to an open `bch_fs`.
pub unsafe fn sb_lock(fs: *mut bch_bindgen::c::bch_fs) -> SbLockGuard {
    let lock = &mut (*fs).sb_lock.lock as *mut _ as *mut libc::pthread_mutex_t;
    libc::pthread_mutex_lock(lock);
    SbLockGuard(lock)
}

