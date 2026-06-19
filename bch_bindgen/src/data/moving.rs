// ============================================
// 备注：Moving Context — 后台数据移动流程
//
// 备注：Moving Context 负责三种数据移动场景：
// 备注：  1. Copy-on-Write（COW）：已有数据的间接指针被重新写入
// 备注：  2. Rebalance：改变数据的 replica 数量或压缩级别
// 备注：  3. 数据迁移：在设备之间移动数据
//
// 备注：核心架构：
// 备注：  moving_context 包含自引用的 list_head（init 后不可移动），
// 备注：  所以必须 Box pin 后在堆上原地初始化。
//
// 备注：使用 moving_io 池（固定数量的并行写入槽位）来控制并发。
// ============================================
//
// RAII wrapper for bch2_moving_ctxt_init / bch2_moving_ctxt_exit.
//
// The moving_context struct contains embedded list_heads that become
// self-referential after init — it must be pinned (allocated on the heap
// and never moved). We Box it first, then init in place.

use std::ffi::c_void;
use std::marker::PhantomPinned;
use std::pin::Pin;

use crate::c;
use bcachefs_kernel::errcode::{self, BchError};
use bcachefs_kernel::fs::Fs;

fn ret_to_result(ret: i32) -> Result<(), BchError> {
    errcode::ret_to_result(ret).map(|_| ())
}

/// RAII wrapper around `moving_context`. Calls `bch2_moving_ctxt_exit` on drop.
///
/// Heap-allocated and pinned because the C struct contains self-referential
/// list_head pointers after initialization.
pub struct MovingContext {
    raw: Pin<Box<c::moving_context>>,
    _pin: PhantomPinned,
}

impl MovingContext {
    pub fn new(fs: &Fs, wp: c::write_point_specifier, wait_on_copygc: bool) -> Self {
        let mut raw = Box::pin(unsafe { std::mem::zeroed::<c::moving_context>() });
        unsafe {
            c::bch2_moving_ctxt_init(
                &mut *raw.as_mut().get_unchecked_mut(),
                fs.raw,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
                wp,
                wait_on_copygc,
            );
        }
        MovingContext { raw, _pin: PhantomPinned }
    }

    /// # Safety
    /// `arg` must be valid for the predicate function `pred`.
    pub unsafe fn move_data_btree(
        &mut self,
        start: c::bpos,
        end: c::bpos,
        pred: c::move_pred_fn,
        arg: *mut c_void,
        btree: c::btree_id,
        level: u32,
    ) -> Result<(), BchError> {
        ret_to_result(unsafe {
            c::bch2_move_data_btree(
                &mut *self.raw.as_mut().get_unchecked_mut(),
                start,
                end,
                pred,
                arg,
                btree,
                level,
            )
        })
    }
}

impl Drop for MovingContext {
    fn drop(&mut self) {
        unsafe { c::bch2_moving_ctxt_exit(&mut *self.raw.as_mut().get_unchecked_mut()) }
    }
}

