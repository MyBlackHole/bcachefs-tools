// SPDX-License-Identifier: GPL-2.0
// ============================================
// 备注：bcachefs 异步 I/O 操作
//
// 备注：ReadOp: 真正的异步 Future，C shim 提交读取请求后立即返回，
// 备注：         bio 完成回调在 libaio 完成线程中唤醒 Rust waker。
//
// 备注：WriteOp: 当前还是同步实现（等待异步化改造）。
//
// 备注：这个模块展示了 bcachefs 混合 Rust/C 异步模型的桥梁：
// 备注：  Rust Future → C IO 提交 → libaio 完成 → C 回调 → Rust waker
// ============================================
//
// IO operations on a bcachefs filesystem.
//
// ReadOp is a genuine async Future: the C shim submits the read and
// returns immediately; the bio endio callback wakes the Rust waker
// when IO completes (from the libaio completion thread in userspace).
//
// WriteOp is still synchronous pending the same treatment.

use std::cell::UnsafeCell;
use std::future::Future;
use std::pin::Pin;
use std::sync::atomic::{AtomicBool, Ordering};
use std::task::{Context, Poll, Waker};

use crate::c;
use bcachefs_kernel::errcode::{self, BchError};
use bcachefs_kernel::fs::Fs;

/// Maximum single IO size (must match RUST_IO_MAX in rust_shims.h).
pub const MAX_IO_SIZE: usize = 1 << 20;
const PAGE_SIZE: usize = 4096;
const MAX_BVECS: usize = MAX_IO_SIZE / PAGE_SIZE;

extern "C" {
    fn rust_write_submit(
        c: *mut c::bch_fs,
        op: *mut c::bch_write_op,
        bvecs: *mut c::bio_vec,
        nr_bvecs: u32,
        buf: *const std::ffi::c_void,
        len: usize,
        inum: u64,
        offset: u64,
        subvol: u32,
        replicas: u32,
        new_i_size: u64,
        end_io: Option<unsafe extern "C" fn(*mut c::bch_write_op)>,
    ) -> i32;

    fn rust_read_submit(
        c: *mut c::bch_fs,
        rbio: *mut c::bch_read_bio,
        bvecs: *mut c::bio_vec,
        nr_bvecs: u32,
        buf: *mut std::ffi::c_void,
        len: usize,
        offset: u64,
        opts: c::bch_inode_opts,
        inum: c::subvol_inum,
        endio: c::bio_end_io_t,
    );
}

/// Result of a write operation.
pub struct WriteResult {
    pub sectors_delta: i64,
}

// 备注：WriteState — 堆分配的写入操作状态。
// 备注：bch_write_op 必须在 offset 0（C 端 end_io 回调直接强转指针）。
// 备注：completed (AtomicBool) + waker 构成了 Rust Future 的完成通知机制。
// 备注：AtomicBool 用了 Release/Acquire 语义确保 waker 的可见性。
// Heap-allocated state for an in-flight write.
// bch_write_op is at offset 0 so end_io can cast directly to WriteState.
#[repr(C)]
struct WriteState {
    op:         c::bch_write_op,
    bvecs:      [c::bio_vec; MAX_BVECS],
    completed:  AtomicBool,
    waker:      UnsafeCell<Option<Waker>>,
}

unsafe impl Send for WriteState {}
unsafe impl Sync for WriteState {}

/// end_io callback for bch_write_op — signals completion and wakes
/// the Rust future.
// 备注：write_endio — C 回调：IO 完成时由 libaio 完成线程调用。
// 备注：op → WriteState 的转换依赖于 repr(C) + op at offset 0 的布局保证。
// 备注：先 take waker 再 store true，避免 waker 被并发消费。
unsafe extern "C" fn write_endio(op: *mut c::bch_write_op) {
    // WriteState has op at offset 0
    let state = op as *mut WriteState;

    let waker = (*(*state).waker.get()).take();
    (*state).completed.store(true, Ordering::Release);
    if let Some(w) = waker {
        w.wake();
    }
}

/// Async write operation on a bcachefs filesystem.
///
/// IO is submitted in new(); poll checks for completion.
// 备注：WriteOp — 异步写入 Future。
// 备注：new() 中通过 rust_write_submit C 函数提交写入请求，返回后立刻返回。
// 备注：poll() 检查 completed 标志，IO 完成前注册 waker 并返回 Pending。
pub struct WriteOp {
    state: Pin<Box<WriteState>>,
    /// Set if rust_write_submit returned an error (disk reservation failure).
    submit_err: Option<i32>,
}

impl WriteOp {
    pub fn new(
        fs: &Fs,
        inum: u64,
        offset: u64,
        subvol: u32,
        replicas: u32,
        data: &[u8],
        new_i_size: u64,
    ) -> Self {
        let state = Box::pin(WriteState {
            op:         unsafe { std::mem::zeroed() },
            bvecs:      unsafe { std::mem::zeroed() },
            completed:  AtomicBool::new(false),
            waker:      UnsafeCell::new(None),
        });

        let ret = unsafe {
            let state_ptr = &*state as *const WriteState as *mut WriteState;
            rust_write_submit(
                fs.raw,
                &raw mut (*state_ptr).op,
                (*state_ptr).bvecs.as_mut_ptr(),
                MAX_BVECS as u32,
                data.as_ptr() as *const _,
                data.len(),
                inum,
                offset,
                subvol,
                replicas,
                new_i_size,
                Some(write_endio),
            )
        };

        let submit_err = if ret != 0 { Some(ret) } else { None };
        Self { state, submit_err }
    }
}

impl Future for WriteOp {
    type Output = Result<WriteResult, BchError>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();

        // Submit-time error (e.g. disk reservation failure)
        if let Some(ret) = this.submit_err {
            return Poll::Ready(Err(BchError::from_raw(-ret)));
        }

        if this.state.completed.load(Ordering::Acquire) {
            let error = this.state.op.error as i32;
            let sectors_delta = this.state.op.i_sectors_delta;
            if error != 0 {
                Poll::Ready(Err(BchError::from_raw(-error)))
            } else {
                Poll::Ready(Ok(WriteResult { sectors_delta }))
            }
        } else {
            unsafe {
                *this.state.waker.get() = Some(cx.waker().clone());
            }
            if this.state.completed.load(Ordering::Acquire) {
                let error = this.state.op.error as i32;
                let sectors_delta = this.state.op.i_sectors_delta;
                if error != 0 {
                    Poll::Ready(Err(BchError::from_raw(-error)))
                } else {
                    Poll::Ready(Ok(WriteResult { sectors_delta }))
                }
            } else {
                Poll::Pending
            }
        }
    }
}

// Heap-allocated state for an in-flight read. The bch_read_bio is at
// offset 0 so the endio callback can cast bio → bch_read_bio → ReadState.
#[repr(C)]
struct ReadState {
    rbio:       c::bch_read_bio,
    bvecs:      [c::bio_vec; MAX_BVECS],
    completed:  AtomicBool,
    waker:      UnsafeCell<Option<Waker>>,
}

// Safety: the waker is only written from poll (single-threaded) and
// read from the endio callback. AtomicBool provides the ordering
// guarantee: the endio stores completed=true with Release, and poll
// loads with Acquire, so the waker write is visible before we read
// completed=true.
unsafe impl Send for ReadState {}
unsafe impl Sync for ReadState {}

/// Endio callback — called from the IO completion path (libaio thread).
/// Gets the bio pointer, walks up to ReadState via container_of, signals
/// completion and wakes the Rust future.
unsafe extern "C" fn read_endio(bio: *mut c::bio) {
    // container_of(bio, bch_read_bio, bio) — bio is the last field
    let rbio = (bio as *mut u8)
        .sub(std::mem::offset_of!(c::bch_read_bio, bio))
        as *mut c::bch_read_bio;
    // ReadState has rbio at offset 0
    let state = rbio as *mut ReadState;

    // Take and wake before setting completed, so the waker is consumed
    // before any future poll sees completed=true.
    let waker = (*(*state).waker.get()).take();
    (*state).completed.store(true, Ordering::Release);
    if let Some(w) = waker {
        w.wake();
    }
}

/// Async read operation on a bcachefs filesystem.
///
/// IO is submitted in new(); poll checks for completion.
pub struct ReadOp {
    state: Pin<Box<ReadState>>,
}

impl ReadOp {
    pub fn new(
        fs: &Fs,
        inum: c::subvol_inum,
        offset: u64,
        inode: &c::bch_inode_unpacked,
        buf: &mut [u8],
    ) -> Self {
        let mut opts: c::bch_inode_opts = unsafe { std::mem::zeroed() };
        unsafe {
            c::bch2_inode_opts_get_inode(
                fs.raw,
                inode as *const _ as *mut _,
                &mut opts,
            );
        }

        let state = Box::pin(ReadState {
            rbio:       unsafe { std::mem::zeroed() },
            bvecs:      unsafe { std::mem::zeroed() },
            completed:  AtomicBool::new(false),
            waker:      UnsafeCell::new(None),
        });

        unsafe {
            let state_ptr = &*state as *const ReadState as *mut ReadState;
            rust_read_submit(
                fs.raw,
                &raw mut (*state_ptr).rbio,
                (*state_ptr).bvecs.as_mut_ptr(),
                MAX_BVECS as u32,
                buf.as_mut_ptr() as *mut _,
                buf.len(),
                offset,
                opts,
                inum,
                Some(read_endio),
            );
        }

        Self { state }
    }
}

impl Future for ReadOp {
    type Output = Result<(), BchError>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let this = self.get_mut();

        if this.state.completed.load(Ordering::Acquire) {
            let ret = this.state.rbio.ret as i32;
            Poll::Ready(errcode::ret_to_result(ret).map(|_| ()))
        } else {
            unsafe {
                *this.state.waker.get() = Some(cx.waker().clone());
            }
            // Re-check after storing waker to avoid missed wakeup
            if this.state.completed.load(Ordering::Acquire) {
                let ret = this.state.rbio.ret as i32;
                Poll::Ready(errcode::ret_to_result(ret).map(|_| ()))
            } else {
                Poll::Pending
            }
        }
    }
}

/// Simple executor for futures — polls in a loop until Ready.
///
/// Handles both synchronous completion (ready on first poll) and
/// async completion (endio wakes from another thread).
pub fn block_on<F: Future>(fut: F) -> F::Output {
    let mut fut = std::pin::pin!(fut);
    let waker = noop_waker();
    let mut cx = Context::from_waker(&waker);
    loop {
        match fut.as_mut().poll(&mut cx) {
            Poll::Ready(val) => return val,
            Poll::Pending => std::thread::yield_now(),
        }
    }
}

fn noop_waker() -> Waker {
    use std::task::{RawWaker, RawWakerVTable};

    const VTABLE: RawWakerVTable = RawWakerVTable::new(
        |p| RawWaker::new(p, &VTABLE),
        |_| {},
        |_| {},
        |_| {},
    );

    unsafe { Waker::from_raw(RawWaker::new(std::ptr::null(), &VTABLE)) }
}

