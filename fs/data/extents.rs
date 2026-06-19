// 备注：Extent 条目迭代器 — 遍历 bkey 值中的 extent entry
//
// 备注：bcachefs 的 extent 值（bch_val）由一组变长的 extent entry 组成。
// 备注：每个 entry 包含一个 type 字段（编码在 bit-position 中）和类型特定的数据。
// 备注：常见的 entry 类型：ptr（数据指针）、crc32/crc64（校验和）、
// 备注：  stripe（RAID stripe 映射）、reflink（引用计数）等。
//
// 备注：ExtentEntryIter: 遍历 bkey 值中的所有 entry
// 备注：ExtentPtrIter:   过滤出其中的 BCH_EXTENT_ENTRY_ptr 条目
use crate::btree::bkey::BkeyValSC;
use crate::c;
use crate::fs::Fs;
use core::marker::PhantomData;
use core::mem::size_of;

// Pull in generated extent_entry_type_u64s() from build.rs
include!(concat!(env!("OUT_DIR"), "/extent_entry_types_gen.rs"));

trait ExtentUnionField<T> {
    unsafe fn as_union_ref(&self) -> &T;
    unsafe fn as_union_mut(&mut self) -> &mut T;
}

impl<T> ExtentUnionField<T> for c::__BindgenUnionField<T> {
    unsafe fn as_union_ref(&self) -> &T {
        unsafe { self.as_ref() }
    }
    unsafe fn as_union_mut(&mut self) -> &mut T {
        unsafe { self.as_mut() }
    }
}

impl ExtentUnionField<core::ffi::c_ulong> for core::ffi::c_ulong {
    unsafe fn as_union_ref(&self) -> &core::ffi::c_ulong {
        self
    }
    unsafe fn as_union_mut(&mut self) -> &mut core::ffi::c_ulong {
        self
    }
}

impl ExtentUnionField<c::bch_extent_ptr> for c::bch_extent_ptr {
    unsafe fn as_union_ref(&self) -> &c::bch_extent_ptr {
        self
    }
    unsafe fn as_union_mut(&mut self) -> &mut c::bch_extent_ptr {
        self
    }
}

unsafe fn extent_union_field_ref<T, F: ExtentUnionField<T>>(field: &F) -> &T {
    unsafe { field.as_union_ref() }
}

unsafe fn extent_union_field_mut<T, F: ExtentUnionField<T>>(field: &mut F) -> &mut T {
    unsafe { field.as_union_mut() }
}

/// Get extent entry type from bit-position encoding (__ffs equivalent).
///
/// Returns `u32::MAX` if the type field is zero (invalid).
// 备注：extent_entry_type — 从 bit-position 编码中提取 entry 类型。
// 备注：type_ 字段使用单比特位置编码（类似 ffs），每种类型占一个 bit 位。
// 备注：__ffs（Find First Set）查找最低位的 1 的位置。
// 备注：例如 type_=0b00100000 → trailing_zeros=5 → 类型 5。
// 备注：0 表示无效条目，返回 u32::MAX。
pub fn extent_entry_type(entry: &c::bch_extent_entry) -> u32 {
    let t = unsafe { *extent_union_field_ref(&entry.type_) } as u64;
    if t != 0 { t.trailing_zeros() } else { u32::MAX }
}

/// Pointer past the last val u64 for a bkey.
///
/// # Safety
/// `v` must point to the start of the value region for `k`.
// 备注：bkey_val_end — 计算 bkey 值区域的结束指针。
// 备注：每个 bkey 的总长度是 k.u64s 个 u64，减去 bkey 头部的大小，
// 备注：剩下的就是值区域的大小（以字节为单位）。
unsafe fn bkey_val_end(k: &c::bkey, v: *const u8) -> *const c::bch_extent_entry {
    let val_u64s = k.u64s as usize - size_of::<c::bkey>() / 8;
    v.add(val_u64s * 8) as *const c::bch_extent_entry
}

/// Get the start and end pointers for extent entries from a typed bkey.
// 备注：bkey_ptrs_raw — 根据 bkey 类型，获取值中 extent entry 的起止指针。
// 备注：不同 bkey 类型（extent、btree_ptr、stripe、reflink_v）的 entry
// 备注：在值中的布局不同，需要按类型分别计算起止位置。
fn bkey_ptrs_raw(sc: &BkeyValSC<'_>) -> Option<(*const c::bch_extent_entry, *const c::bch_extent_entry)> {
    // Safety: all typed value pointers come from BkeyValSC dispatch,
    // which guarantees they point to valid bkey value data.
    unsafe { match sc {
        BkeyValSC::btree_ptr(k, v) =>
            Some((v.start.as_ptr() as _, bkey_val_end(k, *v as *const _ as _))),
        BkeyValSC::extent(k, v) =>
            Some((v.start.as_ptr() as _, bkey_val_end(k, *v as *const _ as _))),
        BkeyValSC::stripe(_k, v) =>
            Some((v.ptrs.as_ptr() as _, v.ptrs.as_ptr().add(v.nr_blocks as usize) as _)),
        BkeyValSC::reflink_v(k, v) =>
            Some((v.start.as_ptr() as _, bkey_val_end(k, *v as *const _ as _))),
        BkeyValSC::btree_ptr_v2(k, v) =>
            Some((v.start.as_ptr() as _, bkey_val_end(k, *v as *const _ as _))),
        _ => None,
    } }
}

fn empty_iter<'a>() -> ExtentEntryIter<'a> {
    ExtentEntryIter { cur: core::ptr::null(), end: core::ptr::null(), _phantom: PhantomData }
}

/// Iterator over extent entries within a bkey.
// 备注：ExtentEntryIter — extent entry 迭代器。
// 备注：每个 entry 是变长的（由 extent_entry_type_u64s 决定长度），
// 备注：遍历时通过类型表获取每个 entry 的 u64s 大小来跳到下一个。
pub struct ExtentEntryIter<'a> {
    cur: *const c::bch_extent_entry,
    end: *const c::bch_extent_entry,
    _phantom: PhantomData<&'a c::bch_extent_entry>,
}

impl<'a> Iterator for ExtentEntryIter<'a> {
    type Item = &'a c::bch_extent_entry;

    fn next(&mut self) -> Option<Self::Item> {
        if self.cur >= self.end {
            return None;
        }
        let entry = unsafe { &*self.cur };
        let ty = extent_entry_type(entry);
        let u64s = extent_entry_type_u64s(ty)?;
        let next = unsafe { (self.cur as *const u64).add(u64s) as *const c::bch_extent_entry };
        if next > self.end {
            return None;
        }
        self.cur = next;
        Some(entry)
    }
}

/// Iterate over all extent entries in a typed bkey.
///
/// Returns an empty iterator for key types that don't have extent entries.
pub fn bkey_extent_entries_sc<'a>(sc: &BkeyValSC<'a>) -> ExtentEntryIter<'a> {
    match bkey_ptrs_raw(sc) {
        Some((start, end)) => ExtentEntryIter { cur: start, end, _phantom: PhantomData },
        None => empty_iter(),
    }
}

/// Iterate over all extent entries in a `bkey_i`.
pub fn bkey_extent_entries(k: &c::bkey_i) -> ExtentEntryIter<'_> {
    bkey_extent_entries_sc(&BkeyValSC::from_bkey_i(k))
}

/// Iterator over extent pointers within a bkey.
pub struct ExtentPtrIter<'a> {
    inner: ExtentEntryIter<'a>,
}

impl<'a> Iterator for ExtentPtrIter<'a> {
    type Item = &'a c::bch_extent_ptr;

    fn next(&mut self) -> Option<Self::Item> {
        for entry in self.inner.by_ref() {
            if extent_entry_type(entry) == c::bch_extent_entry_type::BCH_EXTENT_ENTRY_ptr as u32 {
                return Some(unsafe { extent_union_field_ref(&entry.ptr) });
            }
        }
        None
    }
}

/// Iterate over extent pointers in a typed bkey, skipping non-pointer entries.
pub fn bkey_ptrs_sc<'a>(sc: &BkeyValSC<'a>) -> ExtentPtrIter<'a> {
    ExtentPtrIter { inner: bkey_extent_entries_sc(sc) }
}

/// Iterate over extent pointers in a `bkey_i`.
pub fn bkey_ptrs(k: &c::bkey_i) -> ExtentPtrIter<'_> {
    bkey_ptrs_sc(&BkeyValSC::from_bkey_i(k))
}

pub struct ExtentEntryIterMut<'a> {
    fs:       &'a Fs,
    cur:      *mut c::bch_extent_entry,
    end:      *mut c::bch_extent_entry,
    _phantom: PhantomData<&'a mut c::bch_extent_entry>,
}

impl<'a> Iterator for ExtentEntryIterMut<'a> {
    type Item = &'a mut c::bch_extent_entry;

    fn next(&mut self) -> Option<Self::Item> {
        if self.cur >= self.end {
            return None;
        }

        let entry = unsafe { &mut *self.cur };
        let u64s = unsafe { c::extent_entry_u64s(self.fs.raw, self.cur) as usize };
        if u64s == 0 {
            return None;
        }

        let next = unsafe { (self.cur as *mut u64).add(u64s) as *mut c::bch_extent_entry };
        if next > self.end {
            return None;
        }

        self.cur = next;
        Some(entry)
    }
}

pub(crate) fn bkey_extent_entries_mut<'a>(
    fs: &'a Fs,
    k:  &'a mut c::bkey_i,
) -> ExtentEntryIterMut<'a> {
    let ptrs = unsafe { c::bch2_bkey_ptrs(c::bkey_i_to_s(k)) };

    ExtentEntryIterMut {
        fs,
        cur:      ptrs.start,
        end:      ptrs.end,
        _phantom: PhantomData,
    }
}

pub struct ExtentPtrIterMut<'a> {
    inner: ExtentEntryIterMut<'a>,
}

impl<'a> Iterator for ExtentPtrIterMut<'a> {
    type Item = &'a mut c::bch_extent_ptr;

    fn next(&mut self) -> Option<Self::Item> {
        for entry in self.inner.by_ref() {
            if extent_entry_type(entry) == c::bch_extent_entry_type::BCH_EXTENT_ENTRY_ptr as u32 {
                return Some(unsafe { extent_union_field_mut(&mut entry.ptr) });
            }
        }
        None
    }
}

pub(crate) fn bkey_ptrs_mut<'a>(
    fs: &'a Fs,
    k:  &'a mut c::bkey_i,
) -> ExtentPtrIterMut<'a> {
    ExtentPtrIterMut { inner: bkey_extent_entries_mut(fs, k) }
}
