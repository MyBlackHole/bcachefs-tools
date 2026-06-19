// SPDX-License-Identifier: GPL-2.0
// 备注：bcachefs 选项系统（Options）
//
// 备注：bcachefs 支持大量运行时和格式化选项（压缩、校验和、replica 数量等）。
// 备注：选项设计采用 Cap'n Proto 风格的"defined"位掩码：
// 备注：  每个选项有一个对应的 _defined 标志位，
// 备注：  未设置的选项从 bch2_opts_default 获取默认值。
//
// 备注：宏 API：
// 备注：  opt_set!($opts, $name, $val) — 设置选项 + 标记 defined
// 备注：  opt_defined!($opts, $name)   — 检查选项是否被显式设置
// 备注：  opt_get!($opts, $name)       — 获取值（未设置则取默认值）
//
// 备注：bch2_opt_table 是 bindgen 生成的选项描述表（零长数组），
// 备注：通过 bch2_opts_nr 获取实际数量来安全地转换为 slice。
//! Userspace option-string helpers. `bch_opt_strs` and its parse/free C API live
//! in the userspace `libbcachefs.h`, so they and these methods belong here.

use crate::c;

pub use bcachefs_kernel::opts::opt_id;

impl c::bch_opt_strs {
    /// Set a deferred option string by option id.
    ///
    /// The string is strdup'd into C heap memory so it can be freed by
    /// `bch2_opt_strs_free`.
    pub fn set(&mut self, id: c::bch_opt_id, val: &std::ffi::CStr) {
        unsafe {
            self.__bindgen_anon_1.by_id[id.0 as usize] = libc::strdup(val.as_ptr());
        }
    }

    /// Parse all option strings into a `bch_opts` struct.
    pub fn parse(&self) -> c::bch_opts {
        unsafe { c::bch2_parse_opts(*self) }
    }

    /// Free all strdup'd option strings.
    pub fn free(&mut self) {
        unsafe { c::bch2_opt_strs_free(self) }
    }
}
