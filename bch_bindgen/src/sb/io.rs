// 备注：Superblock I/O 读写操作
//
// 备注：从磁盘设备读取 superblock，返回 bch_sb_handle。
// 备注：Superblock 存储在设备的固定偏移（通常位于设备前 4KB），
// 备注：包含：磁盘格式版本、设备成员列表、选项、加密密钥等。
//
// 备注：bch_sb_handle 是 superblock 的驻留表示，在使用后必须通过
// 备注：bch2_free_super() 释放。
//
// 备注：三种读取模式：
// 备注：  read_super：默认选项读取，返回 anyhow::Result
// 备注：  read_super_opts：自定义选项读取
// 备注：  read_super_silent：静默读取（不输出错误信息），返回 Result<_, BchError>
use bcachefs_kernel::c;
use bcachefs_kernel::errcode::BchError;
use bcachefs_kernel::path_to_cstr;
use anyhow::anyhow;

pub use c::bch2_free_super;

pub fn read_super_opts(
    path: &std::path::Path,
    mut opts: c::bch_opts,
) -> anyhow::Result<c::bch_sb_handle> {
    let path = path_to_cstr(path);
    let mut sb = std::mem::MaybeUninit::zeroed();

    let ret =
        unsafe { c::bch2_read_super(path.as_ptr(), &mut opts, sb.as_mut_ptr()) };

    if ret != 0 {
        Err(anyhow!(BchError::from_raw(ret)))
    } else {
        Ok(unsafe { sb.assume_init() })
    }
}

pub fn read_super(path: &std::path::Path) -> anyhow::Result<c::bch_sb_handle> {
    let opts = c::bch_opts::default();
    read_super_opts(path, opts)
}

pub fn read_super_silent(
    path: &std::path::Path,
    mut opts: c::bch_opts,
) -> Result<c::bch_sb_handle, BchError> {
    let path = path_to_cstr(path);
    let mut sb = std::mem::MaybeUninit::zeroed();

    let ret = unsafe {
        c::bch2_read_super_silent(path.as_ptr(), &mut opts, sb.as_mut_ptr())
    };

    if ret != 0 {
        Err(BchError::from_raw(ret))
    } else {
        Ok(unsafe { sb.assume_init() })
    }
}

