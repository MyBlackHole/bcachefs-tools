/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCHFS_SHIMS_H
#define _BCHFS_SHIMS_H

/*
 * ============================================
 *  bchfs_shims.h — 直接文件操作 C API
 *
 *  提供不依赖 FUSE 的 bcachefs 文件操作 API。
 *  模仿 c_src/fuse_shims.c 的模式，但使用 fd-based
 *  接口（类似 POSIX），面向 C 程序直接调用。
 *
 *  依赖: 调用者必须已通过 bch2_fs_open() + bch2_fs_start()
 *        打开文件系统 (nostart=false)。
 *
 *  线程安全: Phase 1 单线程，不做并发保护。
 *
 *  文件位置: bchfs-shims/bchfs_shims.c (实现)
 * ============================================
 */

#include "fs/bcachefs.h"
#include "fs/fs/inode.h"

// 备注：---- 文件操作 ----

/*
 * bchfs_open - 打开文件
 * @c: 已启动的文件系统
 * @path: 绝对路径（如 "/dir/file.txt"）
 * @flags: O_RDONLY / O_WRONLY / O_RDWR
 *
 * 返回: fd (>=0) 或 负 errno
 */
int bchfs_open(struct bch_fs *c, const char *path, int flags);

/*
 * bchfs_close - 关闭文件
 * @c: 文件系统
 * @fd: bchfs_open 返回的文件描述符
 *
 * 返回: 0 成功，负 errno
 */
int bchfs_close(struct bch_fs *c, int fd);

/*
 * bchfs_read - 从文件当前位置读取数据
 * @c: 文件系统
 * @fd: 文件描述符
 * @buf: 输出缓冲区
 * @count: 读取字节数
 *
 * 返回: 实际读取字节数(>=0) 或 负 errno
 */
ssize_t bchfs_read(struct bch_fs *c, int fd, void *buf, size_t count);

/*
 * bchfs_write - 向文件当前位置写入数据
 * @c: 文件系统
 * @fd: 文件描述符
 * @buf: 输入缓冲区
 * @count: 写入字节数
 *
 * 返回: 实际写入字节数(>=0) 或 负 errno
 */
ssize_t bchfs_write(struct bch_fs *c, int fd, const void *buf, size_t count);

/*
 * bchfs_lseek - 设置文件位置
 * @c: 文件系统
 * @fd: 文件描述符
 * @offset: 偏移量
 * @whence: SEEK_SET / SEEK_CUR / SEEK_END
 *
 * 返回: 新位置(>=0) 或 负 errno
 */
ssize_t bchfs_lseek(struct bch_fs *c, int fd, ssize_t offset, int whence);

// 备注：---- 目录/文件操作 ----

/*
 * bchfs_create - 创建文件
 * @c: 文件系统
 * @path: 绝对路径
 * @mode: 文件权限模式 (如 0644)
 *
 * 返回: 0 成功，负 errno
 */
int bchfs_create(struct bch_fs *c, const char *path, mode_t mode);

/*
 * bchfs_mkdir - 创建目录
 * @c: 文件系统
 * @path: 目录路径
 * @mode: 目录权限
 *
 * 返回: 0 成功，负 errno
 */
int bchfs_mkdir(struct bch_fs *c, const char *path, mode_t mode);

/*
 * bchfs_unlink - 删除文件
 * @c: 文件系统
 * @path: 文件路径
 *
 * 返回: 0 成功，负 errno
 */
int bchfs_unlink(struct bch_fs *c, const char *path);

/*
 * bchfs_rmdir - 删除空目录
 * @c: 文件系统
 * @path: 目录路径
 *
 * 返回: 0 成功，负 errno
 */
int bchfs_rmdir(struct bch_fs *c, const char *path);

/*
 * bchfs_rename - 重命名文件或目录
 * @c: 文件系统
 * @oldpath: 原路径
 * @newpath: 新路径
 *
 * 返回: 0 成功，负 errno
 */
int bchfs_rename(struct bch_fs *c, const char *oldpath, const char *newpath);

// 备注：---- 查询 ----

/*
 * bchfs_stat - 查询文件或目录的 inode 信息
 * @c: 文件系统
 * @path: 路径
 * @inode_out: 输出 inode 信息
 *
 * 返回: 0 成功，负 errno
 */
int bchfs_stat(struct bch_fs *c, const char *path,
               struct bch_inode_unpacked *inode_out);

/*
 * bchfs_fstat - 查询已打开文件的 inode 信息
 * @c: 文件系统
 * @fd: 文件描述符
 * @inode_out: 输出 inode 信息
 *
 * 返回: 0 成功，负 errno
 */
int bchfs_fstat(struct bch_fs *c, int fd,
                struct bch_inode_unpacked *inode_out);

#endif /* _BCHFS_SHIMS_H */
