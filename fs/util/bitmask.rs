// ============================================
// 备注：LE64 位域读写（Bitmask）— C LE64_BITMASK 宏的 Rust 实现
//
// 备注：bcachefs 大量使用位域（bitfield）将多个字段压缩存储在一个 u64 中，
// 备注：例如 superblock 的 flags 字段同时存储加密类型、同步模式等。
// 备注：C 中通过 LE64_BITMASK 宏生成内联 getter/setter 函数。
//
// 备注：由于 bindgen 无法导出 C static inline 函数，本模块提供纯 Rust 等价实现。
// 备注：所需的 OFFSET/BITS 常量由 bindgen 正确导出。
//
// 备注：bitmask_accessors! 宏生成类型安全的 getter/setter 方法：
// 备注：  支持数组字段（如 bch_sb.flags[i]）和普通字段（如 bch_member.flags）
// 备注：  每个 NAME 对应一对 (getter, setter) 方法
// ============================================
//
// LE64 bitmask getter/setter — pure Rust equivalent of the C LE64_BITMASK macro.
//
// The C macro generates static inline getter/setter functions that bindgen can't
// export. The constants (NAME_OFFSET, NAME_BITS) ARE exported. This module provides
// the same operations in pure Rust using those constants, eliminating the C shim
// functions in rust_shims.c.

use crate::c;

/// Get a bitmask field from a little-endian u64.
#[inline]
pub fn le64_bitmask_get(field: c::__le64, offset: u32, bits: u32) -> u64 {
    (u64::from_le(field) >> offset) & !(!0u64 << bits)
}

/// Set a bitmask field in a little-endian u64.
#[inline]
pub fn le64_bitmask_set(field: &mut c::__le64, offset: u32, bits: u32, v: u64) {
    let mask = !(!0u64 << bits);
    let mut val = u64::from_le(*field);
    val &= !(mask << offset);
    val |= (v & mask) << offset;
    *field = val.to_le();
}

/// Generate getter and setter methods for LE64_BITMASK fields.
///
/// Uses the `paste` crate to construct constant names from the bitmask name prefix.
/// The constants `{NAME}_OFFSET` and `{NAME}_BITS` must exist in `crate::c`.
///
/// Usage:
/// ```ignore
/// bitmask_accessors! {
///     // Array field: struct_type, field[idx], NAME => (getter, setter), ...
///     bch_sb, flags[1],
///         BCH_SB_ENCRYPTION_TYPE => (encryption_type, set_encryption_type),
///         BCH_SB_PROMOTE_TARGET  => (promote_target, set_promote_target);
///
///     // Plain field: struct_type, field, NAME => (getter, setter), ...
///     bch_member, flags,
///         BCH_MEMBER_GROUP => (group, set_group);
/// }
/// ```
#[macro_export]
macro_rules! bitmask_accessors {
    // Array field variant
    ( $struct_ty:ident, $field:ident [ $idx:expr ],
      $( $name:ident => ( $getter:ident, $setter:ident ) ),+ $(,)?
      $(; $($rest:tt)* )?
    ) => {
        impl $crate::c::$struct_ty {
            $(
                #[inline]
                pub fn $getter(&self) -> u64 {
                    $crate::paste! {
                        $crate::util::bitmask::le64_bitmask_get(
                            self.$field[$idx],
                            $crate::c::[< $name _OFFSET >],
                            $crate::c::[< $name _BITS >],
                        )
                    }
                }

                #[inline]
                pub fn $setter(&mut self, v: u64) {
                    $crate::paste! {
                        $crate::util::bitmask::le64_bitmask_set(
                            &mut self.$field[$idx],
                            $crate::c::[< $name _OFFSET >],
                            $crate::c::[< $name _BITS >],
                            v,
                        )
                    }
                }
            )+
        }

        $( $crate::bitmask_accessors!{ $($rest)* } )?
    };

    // Plain field variant
    ( $struct_ty:ident, $field:ident,
      $( $name:ident => ( $getter:ident, $setter:ident ) ),+ $(,)?
      $(; $($rest:tt)* )?
    ) => {
        impl $crate::c::$struct_ty {
            $(
                #[inline]
                pub fn $getter(&self) -> u64 {
                    $crate::paste! {
                        $crate::util::bitmask::le64_bitmask_get(
                            self.$field,
                            $crate::c::[< $name _OFFSET >],
                            $crate::c::[< $name _BITS >],
                        )
                    }
                }

                #[inline]
                pub fn $setter(&mut self, v: u64) {
                    $crate::paste! {
                        $crate::util::bitmask::le64_bitmask_set(
                            &mut self.$field,
                            $crate::c::[< $name _OFFSET >],
                            $crate::c::[< $name _BITS >],
                            v,
                        )
                    }
                }
            )+
        }

        $( $crate::bitmask_accessors!{ $($rest)* } )?
    };

    // Base case — empty
    () => {};
}

