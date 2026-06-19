/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FUSE_SHIMS_H
#define _FUSE_SHIMS_H

#include "fs/bcachefs.h"
#include "fs/fs/inode.h"
#include "fs/alloc/buckets.h"

// ============================================
// 备注：FUSE mount C shim 头文件
//
// 备注：为 Rust FUSE mount 命令提供 C wrapper 函数。
// 备注：这些 shims 封装了使用 static inline 函数、宏或复杂类型
// 备注：（qstr, btree_trans, closures）的内核操作。
//
// 备注：功能分类：
// 备注：  - 线程初始化：rust_fuse_ensure_current / rcu 注册
// 备注：  - Inline wrapper：block_bytes, inode_nlink_get, time 转换
// 备注：  - 文件系统操作：lookup, create, unlink, rename, link, setattr
// 备注：  - 目录读取：readdir（通过函数指针回调）
// 备注：  - 统计：usage_read_short, count_inodes
// ============================================
/*
 * C shims for the Rust FUSE mount command.
 *
 * These wrap kernel operations that use inline functions, macros,
 * or complex types (qstr, btree_trans, closures) that can't be
 * expressed through bindgen.
 */

/* Thread initialization — must be called on fuser worker threads */
void rust_fuse_ensure_current(void);
void rust_fuse_rcu_register(void);
void rust_fuse_rcu_unregister(void);

/* Directory reading */
typedef int (*rust_fuse_filldir_fn)(void *ctx,
				    const char *name, unsigned name_len,
				    u64 ino, unsigned type, u64 pos);

int rust_fuse_readdir(struct bch_fs *c, subvol_inum dir,
		      u64 pos, void *ctx, rust_fuse_filldir_fn filldir);

#endif /* _FUSE_SHIMS_H */
