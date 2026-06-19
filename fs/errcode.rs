// 备注：bcachefs 错误码封装
//
// 备注：BchError 包装了 bcachefs 的两层错误码系统：
// 备注：  1. 标准 errno（1..2047）
// 备注：  2. bcachefs 特有错误码（BCH_ERR_START=2048 起），
// 备注：     通过父链关联到根 errno（如 EIO、ENOSPC）。
//
// 备注：关键函数：
// 备注：  __bch2_err_matches(code, class)：检查错误码是否属于某个类别
// 备注：  __bch2_err_class(code)：提取根 errno
// 备注：  bch2_err_str(code)：获取静态错误描述字符串
//
// 备注：ret_to_result:  将 C 风格的负值返回码转为 Rust Result
// 备注：errptr_to_result: 将 ERR_PTR 风格的指针转为 Rust Result
use crate::c;
use core::ffi::{c_int, CStr};
use core::fmt;

pub use crate::c::bch_errcode;

include!(concat!(env!("OUT_DIR"), "/errcodes_gen.rs"));

/// Safe wrapper for bcachefs/errno error codes.
/// Stores the positive error code — either a standard errno (1..2047)
/// or a bcachefs-specific code (BCH_ERR_START=2048..).
///
/// Unlike `bch_errcode` (a repr(u32) enum), this never creates an invalid
/// enum discriminant from a raw errno value.
// 备注：BchError 存储正数错误码（和内核风格一致），而 C 函数返回负数。
// 备注：ret_to_result 负责把 -EIO → Err(BchError(5))。
// 备注：matches() 支持错误码类别匹配：如 EIO 类下的所有子错误。
#[derive(Copy, Clone, PartialEq, Eq)]
pub struct BchError(i32);

impl BchError {
    pub fn from_raw(code: i32) -> Self { Self(code) }

    /// Construct from a `bch_errcode` enum value.
    pub fn from_errcode(code: bch_errcode) -> Self { Self(code as i32) }

    pub fn raw(&self) -> i32 { self.0 }

    /// Get the error message string.
    ///
    /// Returns a static string since bch2_err_str() returns strings
    /// that live for the process lifetime.
    pub fn msg(&self) -> &'static str {
        unsafe { CStr::from_ptr(c::bch2_err_str(self.0)) }
            .to_str()
            .unwrap_or("unknown error")
    }

    pub fn matches(&self, class: bch_errcode) -> bool {
        if self.0 != 0 {
            unsafe { c::__bch2_err_matches(self.0, class as i32) }
        } else {
            false
        }
    }

    pub fn matches_errno(&self, class: i32) -> bool {
        if self.0 != 0 {
            unsafe { c::__bch2_err_matches(self.0, class) }
        } else {
            false
        }
    }

    /// Return the standard errno that this error maps to.
    /// bcachefs error codes (>= 2048) walk the parent chain to their
    /// root errno; standard errnos pass through unchanged.
    pub fn errno(&self) -> i32 {
        if self.0 == 0 {
            0
        } else {
            // __bch2_err_class takes and returns negative error codes
            -unsafe { c::__bch2_err_class(-self.0) }
        }
    }
}

impl fmt::Display for BchError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "{}", self.msg())
    }
}

impl fmt::Debug for BchError {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        write!(f, "BchError({}, {})", self.0, self)
    }
}

impl core::error::Error for BchError {}

pub fn bch_err_throw(code: bch_errcode) -> BchError {
    BchError::from_errcode(code)
}

pub fn ret_to_result(ret: c_int) -> Result<c_int, BchError> {
    if ret < 0 && ret > -4096 {
        Err(BchError(-ret))
    } else {
        Ok(ret)
    }
}

pub fn ret_to_result_void(ret: c_int) -> Result<(), BchError> {
    ret_to_result(ret).map(|_| ())
}

pub fn errptr_to_result<T>(p: *mut T) -> Result<*mut T, BchError> {
    let addr = p as usize;
    let max_err: isize = -4096;
    if addr > max_err as usize {
        Err(BchError(-(addr as i32)))
    } else {
        Ok(p)
    }
}

pub fn errptr_to_result_c<T>(p: *const T) -> Result<*const T, BchError> {
    let addr = p as usize;
    let max_err: isize = -4096;
    if addr > max_err as usize {
        Err(BchError(-(addr as i32)))
    } else {
        Ok(p)
    }
}

