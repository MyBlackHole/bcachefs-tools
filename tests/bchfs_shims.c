// SPDX-License-Identifier: GPL-2.0
//
// 备注：bchfs_shims.c — 直接文件操作 C API 实现
//
// 备注：不依赖 FUSE 的 bcachefs 文件操作封装。
// 备注：提供 fd-based API：open/read/write/close + 目录操作。
//
// 备注：依赖:
//   - bch2_fs_open(dev, opts) + bch2_fs_start(c) (nostart=false)
// 备注：  - 调用者线程需要 btree_trans / RCU 上下文
// 备注：    (本文件在首条 API 调用时自动初始化)
//
// 备注：线程模型（与内核 VFS 模式的对比）：
// 备注：
// 备注：  内核 VFS 模式（有 VFS 层）：
// 备注：    write() → page cache(脏) → [writeback thread] → journal + disk
// 备注：                                       ↑
// 备注：    回写线程负责异步刷脏页，有专门的 bcachefs_vfs_writeback workqueue
// 备注：
// 备注：  用户空间模式（-DNO_BCACHEFS_FS 排除 fs/vfs/）：
// 备注：    bchfs_write() → bchfs_do_write() → bch2_write()
// 备注：      BCH_WRITE_sync → 同步完成，不经过专用 workqueue 线程
// 备注：      └─ 底层 IO 提交走 bio 回调，事件处理走 events 线程
// 备注：
// 备注：    无页缓存层，写入直接走文件系统内部 IO 管线，同步等待完成。
// 备注：    所以 GDB 线程列表中没有 writeback 线程是正常行为。
// 备注：
// 备注：可观察到的线程 (NO_BCACHEFS_FS)：
// 备注：
// 备注：  ┌─ 线程名 (GDB 显示)        ─ 源码 workqueue 名      ─ 用途
// 备注：  ├─ bch-reclaim                bch-reclaim              回收（journal 压缩、bucket 清理）
// 备注：  ├─ bch-copygc                 bcachefs_copygc          碎片整理 GC
// 备注：  ├─ bch-reconcile              (内部 kthread)           reconciler
// 备注：  ├─ bcachefs_journal           (内部 kthread)           journal 持久化
// 备注：  ├─ bcachefs_write_            bcachefs_write_ref       后台维护（discard、gc_gens、
// 备注：  │                                                        EC stripe 删除等），
// 备注：  │                                                        __与用户数据写入无关__
// 备注：  ├─ bcachefs_btree_write_sumit    bcachefs_btree_write_     btree 节点写 IO 提交
// 备注：  │                                 sumit（源码 typo）     （max_active=1 串行）
// 备注：  ├─ bcachefs_btree_write_complet  bcachefs_btree_write_    btree 节点写完成处理
// 备注：  │                               complete                （释放 bounce buffer、清标志、
// 备注：  │                                                        唤醒等待者）
// 备注：  ├─ bcachefs                    bcachefs                 btree 更新（index_update_wq）
// 备注：  ├─ bcachefs_promotes           bcachefs_promotes        缓存提升（promote）
// 备注：  ├─ events                      system_wq               通用事件 workqueue
// 备注：  └─ events_unbound              system_unbound_wq       通用无绑定 workqueue
// 备注：
// 备注：  Linux pthread_setname_np 限制线程名 16 字节（含 \0），
// 备注：  超长名称（如 bcachefs_write_ref:19 字符）被自动截断。源码 typo 也保留。
// 备注：  用户数据写入（bch2_write + BCH_WRITE_sync）不在专用线程中执行，
// 备注：  同步完成于调用者上下文。
// 备注：
// 备注：btree 节点写 IO 与用户数据写 IO 差异：
// 备注：
// 备注：  ┌──── 用户数据写 IO（bchfs_write → bch2_write）
// 备注：  │  - 触发者：应用调用 bchfs_write()
// 备注：  │  - 数据内容：用户文件数据（extent）
// 备注：  │  - 线程模型：无专用线程，在调用者上下文中同步完成
// 备注：  │  - 并发度：无限制（谁调用谁执行）
// 备注：  │  - 排序要求：无（各写各的 extent）
// 备注：  │  - 完成处理：bchfs_write_endio → complete() → 调用者返回
// 备注：  │
// 备注：  └──── btree 节点写 IO（btree_node_write）
// 备注：     - 触发者：bcachefs 内部（btree split、journal flush、reclaim 等）
// 备注：     - 数据内容：btree 节点序列化后的 on-disk bset
// 备注：     - 线程模型：专用 workqueue（write_submit_wq + write_complete_wq）
// 备注：     - 并发度：max_active=1 串行化——btree 节点有父子关系，不能乱序
// 备注：     - 排序要求：严格——btree 节点写顺序必须保证（parent before child）
// 备注：     - 完成处理：释放 bounce buffer + 清 write_in_flight 标志 + 唤醒等待者
// 备注：
// 备注：  两者不是独立的：用户数据写入后会更新 extent btree（插入 extent key），
// 备注：  修改了 btree 节点。被修改的脏节点随后会被 btree_node_write() 刷盘。
// 备注：  所以用户写 → 修改 btree → btree 节点写，存在间接依赖关系。

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/uio.h>

#include "libbcachefs.h"
#include "fs/bcachefs.h"
#include "fs/fs/dirent.h"
#include "fs/fs/namei.h"
#include "fs/fs/inode.h"
#include "fs/alloc/foreground.h"
#include "fs/data/read.h"
#include "fs/data/write.h"
#include "fs/data/extents.h"
#include "fs/opts.h"
#include "fs/init/fs.h"
#include "fs/btree/iter.h"

#include "fuse_shims.h"    // rust_fuse_lookup, rust_fuse_create, etc.
#include "rust_shims.h"    // rust_read_submit, RUST_IO_MAX
#include "bchfs_shims.h"

#include <linux/completion.h>
#include <linux/dcache.h>

// =====================================================================
// 备注：FD 表
// =====================================================================
#define BCHFS_MAX_FD 64

struct bchfs_fd {
	bool			used;
	subvol_inum		inum;
	struct bch_inode_unpacked inode;
	u64			pos;
	int			flags;	// O_RDONLY / O_WRONLY / O_RDWR
};

static struct bchfs_fd bchfs_fds[BCHFS_MAX_FD];

// 备注：FD 分配：扫描静态数组找到第一个空闲槽位（O(n)，n=64）
// 备注：返回 0~63 的 fd 编号，与 POSIX fd 完全独立
// 备注：失败返回 -EMFILE（类比 POSIX 的进程 fd 耗尽）
// 备注：
// 备注：实现细节：
// 备注：  1. 遍历 bchfs_fds[0..63]，找 used==false 的空闲槽
// 备注：  2. memset(&fds[i], 0, sizeof) 清零所有字段
// 备注：     - used=false → true
// 备注：     - inum、inode 全部归零（惰性清理，不重复释放）
// 备注：     - pos=0（安全，open 后重新设 pos=0）
// 备注：  3. 返回 i（作为 fd 编号给调用者）
// 备注：
// 备注：设计选择：
// 备注：  - 静态数组而非链表：64 个固定槽位，无动态分配，简单可靠
// 备注：  - 线性扫描而非位图：64 个槽位 O(n) 无性能问题
// 备注：  - memset 惰性清零：不逐个字段赋值，避免遗漏
// 备注：  - 与 POSIX fd 独立：不经过内核 VFS，完全用户空间管理
static int bchfs_fd_alloc(void)
{
	for (int i = 0; i < BCHFS_MAX_FD; i++)
		if (!bchfs_fds[i].used) {
			memset(&bchfs_fds[i], 0, sizeof(bchfs_fds[i]));
			bchfs_fds[i].used = true;
			return i;
		}
	return -EMFILE;
}

// 备注：FD 查找：根据 fd 编号获取表项指针，含边界和 used 校验
// 备注：返回 NULL 表示非法 fd（未分配或已关闭）
static struct bchfs_fd *bchfs_fd_get(int fd)
{
	if (fd < 0 || fd >= BCHFS_MAX_FD || !bchfs_fds[fd].used)
		return NULL;
	return &bchfs_fds[fd];
}

// 备注：FD 释放：仅标记 used=false，不清空（惰性清零，下次 alloc 时 memset）
static void bchfs_fd_free(int fd)
{
	if (fd >= 0 && fd < BCHFS_MAX_FD)
		bchfs_fds[fd].used = false;
}

// =====================================================================
// 备注：线程初始化
// =====================================================================
static pthread_once_t bchfs_thread_once = PTHREAD_ONCE_INIT;

// 备注：bchfs_thread_init_impl - 一次性线程初始化
// 备注：每个调用 bchfs_* API 的线程需要：
// 备注：  1. rust_fuse_ensure_current() — 确保 Rust Tokio runtime 可用
// 备注：     因为 rust_fuse_lookup/create/unlink/rename 需要调用 Rust 侧异步函数
// 备注：  2. rust_fuse_rcu_register() — 注册 RCU 上下文
// 备注：     bcachefs btree_trans 需要 RCU 读锁，此 API 注册当前线程的 RCU 状态
// 备注：pthread_once 保证每个进程只执行一次（单线程调用 bchfs_* API 场景）
static void bchfs_thread_init_impl(void)
{
	rust_fuse_ensure_current();
	rust_fuse_rcu_register();
}

// 备注：bchfs_thread_init - 线程安全的单次初始化包装
// 备注：使用 pthread_once 确保 bchfs_thread_init_impl 只被执行一次
// 备注：所有对外 API 函数的第一行都调用此函数
static inline void bchfs_thread_init(void)
{
	pthread_once(&bchfs_thread_once, bchfs_thread_init_impl);
}

// =====================================================================
// 备注：路径解析
// =====================================================================
// 备注：从 root {subvol=1, inum=4096} 开始，逐级 lookup
// 备注：支持绝对路径如 "/dir1/dir2/file.txt"

// 备注：bchfs_resolve_path - 绝对路径逐级解析
// 备注：从 root 目录 (subvol=1, inum=BCACHEFS_ROOT_INO=4096) 开始，
// 备注：对路径的每个分量调用 rust_fuse_lookup 逐级查找。
// 备注：支持 "/dir1/dir2/file.txt" 格式的绝对路径。
// 备注：不处理 ".." / "." / 相对路径。
// 备注：
// 备注：实现逻辑：
// 备注：  1. 检查路径格式（必须以 / 开头）
// 备注：  2. 获取 root inode（bch2_inode_find_by_inum）
// 备注：  3. 如果路径就是 "/"，直接返回 root inode
// 备注：  4. 用 strtok_r 按 "/" 拆分路径，循环遍历每个分量
// 备注：  5. 每个分量调用 rust_fuse_lookup(c, cur, comp) 查找下一级
// 备注：  6. 任何 lookup 失败 → 立即返回错误
static int bchfs_resolve_path(struct bch_fs *c, const char *path,
			      subvol_inum *inum_out,
			      struct bch_inode_unpacked *inode_out)
{
	// 备注：路径必须以 '/' 开头，支持绝对路径格式
	if (!path || *path != '/')
		return -EINVAL;

	// 备注：从 root 目录开始遍历：bcachefs 的根目录固定为
	// 备注：subvol=1（默认子卷），inum=BCACHEFS_ROOT_INO（通常为 4096）
	subvol_inum cur = { .subvol = 1, .inum = BCACHEFS_ROOT_INO };
	struct bch_inode_unpacked cur_inode;

	// 备注：bch2_inode_find_by_inum 直接从磁盘 btree 读取 root inode 元数据
	// 备注：注意：这里不走 Rust FUSE 层，直接访问 bcachefs btree
	int ret = bch2_inode_find_by_inum(c, cur, &cur_inode);
	if (ret)
		return ret;

	// 备注：特例：路径仅为 "/"，直接返回 root inode，无需 lookup
	if (!path[1]) {
		*inum_out = cur;
		if (inode_out)
			*inode_out = cur_inode;
		return 0;
	}

	// 备注：strdup 复制路径字符串，strtok_r 按 "/" 拆分各分量
	// 备注：使用 strtok_r（可重入版本）而非 strtok，支持多线程安全
	char *dup = strdup(path);
	if (!dup)
		return -ENOMEM;

	char *save = NULL;
	char *comp = strtok_r(dup, "/", &save);

	// 备注：逐级遍历路径组件，每步调用 rust_fuse_lookup 查找下一级
	// 备注：rust_fuse_lookup → Rust 侧 FUSE 后端 → bch2_dirent_lookup
	// 备注：起始条件：cur=root，comp="dir1"（第一个分量）
	// 备注：每轮结果更新 cur 和 cur_inode，供下一轮 lookup 使用
	while (comp) {
		subvol_inum next;
		struct bch_inode_unpacked next_inode;

		ret = rust_fuse_lookup(c, cur, (const unsigned char *)comp,
				       strlen(comp), &next, &next_inode);
		// 备注：如果任一分量 lookup 失败（路径不存在、权限不足等），
		// 备注：立即释放 dup 并返回错误码（不继续后续分量查找）
		if (ret) {
			free(dup);
			return ret;
		}

		cur = next;
		cur_inode = next_inode;
		comp = strtok_r(NULL, "/", &save);
	}

	free(dup);

	// 备注：写入最终结果：inum_out 和 inode_out 均非 NULL
	// 备注：调用者可以根据 inode_out 获取文件的 bi_size、bi_mode 等元数据
	*inum_out = cur;
	if (inode_out)
		*inode_out = cur_inode;
	return 0;
}

// =====================================================================
// 备注：拆分路径 → 父目录 + 文件名
// =====================================================================
// 备注：将 "/dir1/dir2/file" → parent={subvol,inum_of_dir2}, name="file"
// 备注：返回的 name 指向 path 内部，不分配内存

// 备注：bchfs_split_path - 拆分路径为"父目录 + 文件名"
// 备注：将 "/dir1/dir2/file" → parent={"subvol=1, inum_of_dir2"}, name="file"
// 备注：返回的 name 指针指向 path 内部，不分配新内存
// 备注：
// 备注：实现逻辑：
// 备注：  1. 用 strrchr 找到最后一个 '/'，其前面为父目录路径，后面为文件名
// 备注：  2. 特殊情况 "/file"（最后一个 '/' 就是开头）→ parent = root
// 备注：  3. 一般情况 → strndup 提取父目录路径 → bchfs_resolve_path 解析
// 备注：  4. name 直接指向 path+last_slash+1（零拷贝）
static int bchfs_split_path(struct bch_fs *c, const char *path,
			    subvol_inum *parent,
			    struct bch_inode_unpacked *parent_inode,
			    const char **name, unsigned *name_len)
{
	// 备注：路径必须以 '/' 开头，且不能为空
	if (!path || *path != '/')
		return -EINVAL;

	// 备注：strrchr 找到路径中最后一个 '/'，这是父目录与文件名的分界点
	// 备注：例如 "/dir1/dir2/file.txt" → last_slash 指向 "/file.txt" 前的 '/'
	// 备注：例如 "/file.txt"           → last_slash == path（根目录下的文件）
	const char *last_slash = strrchr(path, '/');
	if (!last_slash || last_slash == path) {
		// 备注：路径是 "/file" 格式 — 父目录就是 root
		// 备注：parent = {subvol=1, inum=BCACHEFS_ROOT_INO=4096}
		// 备注：name = path+1（跳过开头的 '/'），不分配内存
		*parent = (subvol_inum){ .subvol = 1, .inum = BCACHEFS_ROOT_INO };
		int ret = bch2_inode_find_by_inum(c, *parent, parent_inode);
		if (ret)
			return ret;
		*name = path + 1;
		*name_len = strlen(*name);
		return 0;
	}

	// 备注：提取父目录路径（strndup 复制 last_slash 之前的部分）
	// 备注：例如 "/dir1/dir2/file.txt" → parent_path = "/dir1/dir2"
	// 备注：注意：strndup 在堆上分配内存，需要 free
	size_t parent_len = last_slash - path;
	char *parent_path = strndup(path, parent_len);
	if (!parent_path)
		return -ENOMEM;

	// 备注：递归调用 bchfs_resolve_path 解析父目录 inode
	// 备注：此步骤会触发 rust_fuse_lookup 逐级解析父目录路径
	int ret = bchfs_resolve_path(c, parent_path, parent, parent_inode);
	free(parent_path);
	if (ret)
		return ret;

	// 备注：name 直接指向 path 中文件名部分的起始位置（last_slash + 1）
	// 备注：零拷贝设计：不复制文件名，返回指针指向原始 path 内部
	// 备注：调用者必须确保 path 的生命周期长于 name 的使用周期
	*name = last_slash + 1;
	*name_len = strlen(*name);
	return 0;
}

// =====================================================================
// 备注：阻塞式 I/O 完成回调
// =====================================================================

struct bchfs_io_ctx {
	struct completion done;
	int ret;
};

// 备注：bchfs_read_endio - 读 IO 完成回调（bio 完成时的通知函数）
// 备注：
// 备注：调用时机：块层完成磁盘读取后，调用此回调通知上层。
// 备注：
// 备注：数据流：
// 备注：  [磁盘] → [块层 bio 完成] → bchfs_read_endio(bio)
// 备注：    │
// 备注：    ├── to_rbio(bio)：从普通 bio 反解出 bch_read_bio 结构
// 备注：    │   （bch_read_bio 包含 bcachefs 读上下文，bio 是其内嵌成员）
// 备注：    │
// 备注：    ├── bio->bi_private → ctx：获取读完成上下文
// 备注：    │   （bchfs_do_read 中将 ctx 设置为 bi_private）
// 备注：    │
// 备注：    ├── ctx->ret = rbio->ret：保存读结果
// 备注：    │   rbio->ret = 0：成功
// 备注：    │   rbio->ret < 0：IO 错误（如介质错误、校验和失败）
// 备注：    │
// 备注：    └── complete(&ctx->done)：唤醒 bchfs_do_read 中的 wait_for_completion
// 备注：
// 备注：关键设计：
// 备注：  - ctx 在栈上分配（bchfs_do_read 中），理论上回调执行时栈帧可能已销毁。
// 备注：  - 但 bchfs_do_read 在 wait_for_completion 之后才返回，所以回调执行时
// 备注：    wait_for_completion 尚未返回 → ctx 的栈帧仍然有效。
// 备注：  - 这是 bcachefs closure/completion 模式的典型用法。
static void bchfs_read_endio(struct bio *bio)
{
	struct bch_read_bio *rbio = to_rbio(bio);
	struct bchfs_io_ctx *ctx = bio->bi_private;
	ctx->ret = rbio->ret;
	complete(&ctx->done);
}

// =====================================================================
// 备注：内部读 — 块对齐的同步读
//
// 备注：读执行流程：
// 备注：  bchfs_do_read(c, inum, offset, buf, len)
// 备注：    │
// 备注：    ├── bvec 分配（最多 256 页 = 1MB）
// 备注：    ├── bio_init + bio_add_virt_nofail   ← 将用户 buf 映射为 bio 向量
// 备注：    ├── bch2_inode_find_by_inum          ← 获取 inode 元数据
// 备注：    │   （确定 data_replicas 等 IO 参数）
// 备注：    ├── rbio_init                        ← 初始化 bcachefs 读上下文
// 备注：    ├── bch2_read(trans, ...)            ← 提交读取请求
// 备注：    │   └── 可能立即完成（extent 命中），也可能异步 IO
// 备注：    ├── wait_for_completion               ← 阻塞等待回调解锁
// 备注：    │   └── bchfs_read_endio → complete()
// 备注：    └── 释放 bvec，返回结果
// =====================================================================

// 备注：bchfs_do_read - 块对齐的同步读（内部实现）
// 备注：直接调用 bcachefs 的 bch2_read 进行 IO，绕过 VFS page cache。
// 备注：
// 备注：实现步骤：
// 备注：  1. 分配 bio_vec 数组（最多 256 页 = 1MB，防止栈溢出）
// 备注：  2. bio_init + bio_add_virt_nofail：将用户 buf 映射为 bio 向量
// 备注：  3. bch2_inode_find_by_inum 获取 inode 元数据
// 备注：  4. bch2_inode_opts_get_inode 获取 IO 参数（data_replicas 等）
// 备注：  5. rbio_init 初始化 bcachefs 读上下文，注册 bchfs_read_endio 回调
// 备注：  6. bch2_read 提交读取请求
// 备注：     - 可能立即完成（如果 extent 在缓存中）
// 备注：     - 也可能异步等待 bio 完成
// 备注：  7. wait_for_completion 阻塞等待
// 备注：     - bchfs_read_endio 被 bio 回调触发 → complete()
// 备注：  8. 释放 bvecs，返回结果
// 备注：
// 备注：关键设计：ctx 在栈上分配，wait_for_completion 期间当前线程被阻塞。
// 备注：这是同步阻塞式读，不适用于异步/事件驱动场景。
static int bchfs_do_read(struct bch_fs *c, subvol_inum inum,
			 u64 offset, void *buf, size_t len)
{
	struct bch_read_bio rbio = {};
	struct bio_vec *bvecs;
	int ret;

	// 备注：计算需要的 bio_vec 数量（每个 page 需要一个 bvec）
	// 备注：上限 256 页 = 1MB I/O，防止栈上数组溢出（改用 calloc 堆分配）
	unsigned nr_vecs = min_t(size_t, DIV_ROUND_UP(len, PAGE_SIZE), 256);
	bvecs = calloc(nr_vecs, sizeof(struct bio_vec));
	if (!bvecs)
		return -ENOMEM;

	// 备注：bio_init 初始化 bio 控制块（不分配 bvec，用外部数组）
	// 备注：bi_opf = REQ_OP_READ | REQ_SYNC 标记为同步读操作
	// 备注：bi_sector = offset >> 9 将字节偏移转换为 512B 扇区偏移
	bio_init(&rbio.bio, NULL, bvecs, nr_vecs, 0);
	rbio.bio.bi_opf		= REQ_OP_READ | REQ_SYNC;
	rbio.bio.bi_iter.bi_sector = offset >> 9;

	// 备注：bio_add_virt_nofail 将用户提供的 buf 映射为 bio 数据向量
	// 备注：不拷贝数据，bio 直接引用用户缓冲区（类似 direct IO 的零拷贝）
	// 备注：len 必须在块对齐范围内由调用者保证
	bio_add_virt_nofail(&rbio.bio, buf, len);

	// 备注：从磁盘读取 inode 元数据，获取 data_replicas 等 IO 参数
	// 备注：bch2_inode_find_by_inum → btree 查询 → inode_unpack
	// 备注：bch2_inode_opts_get_inode → 从 inode 中提取 opts（replicas、compression 等）
	struct bch_inode_opts opts = {};
	struct bch_inode_unpacked inode_u;
	ret = bch2_inode_find_by_inum(c, inum, &inode_u);
	if (ret)
		goto out;
	bch2_inode_opts_get_inode(c, &inode_u, &opts);

	// 备注：初始化同步 completion（栈上分配）
	// 备注：bi_private 指向 ctx，回调函数 bchfs_read_endio 通过 bi_private 获取 ctx
	struct bchfs_io_ctx ctx;
	init_completion(&ctx.done);
	rbio.bio.bi_private = &ctx;

	// 备注：rbio_init 将普通 bio 包装为 bcachefs 读上下文（bch_read_bio）
	// 备注：设置 subvol（子卷 ID），后续 btree lookup 需要通过 subvol+inum 定位 extent
	struct bch_read_bio *r = rbio_init(&rbio.bio, c, opts, bchfs_read_endio);
	r->subvol = inum.subvol;

	// 备注：CLASS(btree_trans, trans) 声明并初始化 btree transaction
	// 备注：使用 cleanup 属性，离开作用域时自动调用 bch2_trans_put
	// 备注：bch2_read 在 transaction 上下文中查找 extent key 并发起磁盘 IO
	CLASS(btree_trans, trans)(c);
	ret = bch2_read(trans, r, r->bio.bi_iter, inum,
			NULL, NULL,
			BCH_READ_retry_if_stale|
			BCH_READ_may_promote|
			BCH_READ_user_mapped);

	// 备注：bch2_read 返回值处理：
	// 备注：- ret == 0 表示 IO 已提交到块层，需要 wait_for_completion
	// 备注：- ret != 0 表示立即失败（如 extent 不存在、权限错误等）
	if (ret)
		goto out;

	// 备注：阻塞等待 IO 完成
	// 备注：bchfs_read_endio 被 bio 回调触发，调用 complete(&ctx.done)
	// 备注：bch2_read 可能同步完成（extent 元数据在缓存中直接命中），
	// 备注：也可能异步等待磁盘（bio 飞行中）
	// 备注：wait_for_completion 处理两种情况（如果已经 complete，立即返回）
	wait_for_completion(&ctx.done);
	ret = ctx.ret;

out:
	free(bvecs);
	return ret;
}

// =====================================================================
// 备注：内部写 — 块对齐的同步写
//
// 备注：写入路径（无 VFS writeback）：
// 备注：  bchfs_do_write → bch2_write (closure_call)
// 备注：    │- bch2_disk_reservation_get   — 预分配磁盘空间
// 备注：    │- bch2_write                  — 提交写入（closure 回调链）
// 备注：    │   BCH_WRITE_sync             — 同步 flag，整个管线同步完成
// 备注：    │- wait_for_completion          — 阻塞等待完成
// 备注：    │   └─ bchfs_write_endio       — IO 完成回调 → complete()
// 备注：    └- 无专用 workqueue，写入不走后台线程
// 备注：
// 备注：  没有 page cache + writeback 延迟写入，数据立刻走 journal + IO，
// 备注：  同步等待完成后返回。
// 备注：  这是 -DNO_BCACHEFS_FS 模式与内核 VFS 模式的关键区别。
// =====================================================================

struct bchfs_write_ctx {
	struct bch_write_op	op;
	struct completion	done;
	int			ret;
};

// 备注：bchfs_write_endio - 写 IO 完成回调
// 备注：
// 备注：调用时机：bch2_write 管线全部完成后调用（包括 IO + btree 索引更新）。
// 备注：
// 备注：与 bchfs_read_endio 的关键差异：
// 备注：  - 读回调参数是 struct bio（块层完成触发）
// 备注：  - 写回调参数是 struct bch_write_op（bch2_write 管线完成触发）
// 备注：  - 读 ctx 在栈上分配，写 ctx 在堆上分配（calloc）
// 备注：
// 备注：数据流：
// 备注：  bch2_write 管线完成 → bchfs_write_endio(op)
// 备注：    │
// 备注：    ├── container_of(op, struct bchfs_write_ctx, op)
// 备注：    │   反解出 bchfs_write_ctx（其内嵌成员 op 就是 bch_write_op）
// 备注：    │
// 备注：    ├── ctx->ret = op->error ? -EIO : 0
// 备注：    │   op->error 在 bch2_write_index 中设置：
// 备注：    │   0 = 写入成功
// 备注：    │   非零 = btree 索引更新失败、IO 提交失败等
// 备注：    │
// 备注：    └── complete(&ctx->done)：唤醒 bchfs_do_write 中的 wait_for_completion
// 备注：
// 备注：关键设计：bchfs_write_ctx 嵌入 bch_write_op，通过 container_of 转换。
// 备注：这是 Linux kernel 中常见的"内嵌结构体"模式（类似面向对象继承）。
static void bchfs_write_endio(struct bch_write_op *op)
{
	struct bchfs_write_ctx *ctx = container_of(op, struct bchfs_write_ctx, op);
	ctx->ret = op->error ? -EIO : 0;
	complete(&ctx->done);
}

// 备注：bchfs_do_write - 块对齐的同步写（内部实现）
// 备注：直接调用 bcachefs 的 bch2_write 管线进行 IO。
// 备注：
// 备注：实现步骤：
// 备注：  1. 分配 bchfs_write_ctx（堆上，含 completion 和结果码）
// 备注：  2. 分配 bio_vec → bio_init → bch2_bio_map 映射用户数据
// 备注：  3. bch2_write_op_init 初始化写操作上下文
// 备注：  4. 设置参数：write_point、nr_replicas、subvol、pos、new_i_size
// 备注：  5. bch2_disk_reservation_get 预分配磁盘空间
// 备注：  6. closure_call(&op->cl, bch2_write) 启动写入管线
// 备注：     - BCH_WRITE_sync: IO 完成后同步返回
// 备注：     - bch2_write → __bch2_write → bch2_write_extent
// 备注：       → bch2_submit_wbio_replicas → bch2_write_endio
// 备注：       → bch2_write_index → bchfs_write_endio → complete()
// 备注：  7. wait_for_completion 阻塞等待
// 备注：  8. 释放磁盘预留、bvecs、ctx
// 备注：
// 备注：关键设计：bchfs_write_ctx 嵌入 bch_write_op，closure 机制通过
// 备注：container_of 将 bch_write_op 转换回 bchfs_write_ctx。
// 备注：同步等待 + bch2_bio_map（零拷贝）实现了无 VFS page cache 的直接 IO。
static int bchfs_do_write(struct bch_fs *c, subvol_inum inum, u32 subvol,
			  u64 offset, const void *buf, size_t len,
			  u64 new_i_size, unsigned replicas)
{
	struct bchfs_write_ctx *ctx;
	int ret;

	// 备注：步骤 1 — 分配写完成上下文（bchfs_write_ctx 在堆上分配）
	// 备注：与读路径不同（read_ctx 在栈上），写路径的 ctx 需要嵌入 bch_write_op
	// 备注：因为 closure_call 内部可能延迟完成，栈对象在 closure 完成前可能被释放
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;

	init_completion(&ctx->done);
	ctx->ret = 0;

	struct bch_write_op *op = &ctx->op;

	// 备注：步骤 2 — 分配 bio_vec 数组，初始化 bio
	// 备注：bch2_bio_map 将用户 buf 映射为 bio 数据向量（零拷贝，不复制数据）
	unsigned nr_bvecs = DIV_ROUND_UP(len, PAGE_SIZE);
	struct bio_vec *bvecs = calloc(nr_bvecs, sizeof(struct bio_vec));
	if (!bvecs) {
		free(ctx);
		return -ENOMEM;
	}

	bio_init(&op->wbio.bio, NULL, bvecs, nr_bvecs, 0);
	bch2_bio_map(&op->wbio.bio, (void *)buf, len);

	// 备注：步骤 3 — 初始化写操作上下文
	// 备注：bch2_inode_opts_get 获取当前文件系统的默认 IO 参数
	// 备注：bch2_write_op_init 初始化 bch_write_op 结构体
	struct bch_inode_opts opts;
	bch2_inode_opts_get(c, &opts, false);

	bch2_write_op_init(op, c, opts);
	op->end_io	= bchfs_write_endio;
	// 备注：写点哈希选择目标设备
	op->write_point	= writepoint_hashed(0);
	// 备注：副本数（从 inode data_replicas 得到）
	op->nr_replicas	= replicas;
	// 备注：子卷 ID
	op->subvol	= subvol;
	op->pos		= SPOS(inum.inum, offset >> SECTOR_SHIFT, U32_MAX);
						// 备注：btree key 位置：inode + 扇区偏移
	// 备注：新文件大小（写后更新的 i_size）
	op->new_i_size	= new_i_size;
	op->flags	|= BCH_WRITE_sync | BCH_WRITE_only_specified_devs;
						// 备注：sync = 同步完成
						// 备注：only_specified_devs = 只写到指定设备

	// 备注：步骤 4 — 获取磁盘空间预留
	// 备注：bch2_disk_reservation_get 提前分配磁盘空间，防止写入过程中空间不足
	// 备注：参数：len >> SECTOR_SHIFT = 扇区数，replicas = 副本数
	// 备注：预留失败（ENOSPC）→ 在 IO 开始前即可报错，避免 IO 飞行中空间不足
	ret = bch2_disk_reservation_get(c, &op->res, len >> SECTOR_SHIFT,
					replicas, 0);
	if (ret) {
		free(bvecs);
		free(ctx);
		return ret;
	}

	// 备注：步骤 5 — 通过 closure_call 启动 bch2_write 管线
	// 备注：closure_call(&op->cl, bch2_write, NULL, NULL) 调用链：
	// 备注：  bch2_write → __bch2_write（主循环，COW 路径）
	// 备注：    ├── bch2_write_extent（数据转换：压缩→校验和→加密→提交 IO）
	// 备注：    ├── bch2_submit_wbio_replicas（提交多副本 IO）
	// 备注：    ├── bch2_write_endio（bio 完成回调）
	// 备注：    └── bch2_write_index（更新 extent btree 索引）
	// 备注：  → bchfs_write_endio（所有步骤完成 → complete(&ctx->done)）
	// 备注：
	// 备注：BCH_WRITE_sync 标记使整个管线在 closure_call 返回前可能已完成，
	// 备注：但为安全起见仍然等待 completion（handle 已经 complete 的情况）
	closure_call(&op->cl, bch2_write, NULL, NULL);

	// 备注：步骤 6 — 阻塞等待写完成
	// 备注：BCH_WRITE_sync 模式下，bch2_write 在 closure_call 内部同步完成
	// 备注：但用 wait_for_completion 兜底，确保即使异步也正确处理
	wait_for_completion(&ctx->done);
	ret = ctx->ret;

	// 备注：步骤 7 — 清理：释放磁盘预留、bio_vec 数组、写上下文
	bch2_disk_reservation_put(c, &op->res);
	free(bvecs);
	free(ctx);
	return ret;
}

// =====================================================================
// 备注：bchfs_read — 带块对齐和 pos 更新的完整读
//
// 备注：bchfs_read 执行流程（块对齐层）：
// 备注：
// 备注：  bchfs_read(fd, buf, count)
// 备注：    │
// 备注：    ├── fd_get  → 获取 fd 表项（含 pos + inode 缓存）
// 备注：    ├── 边界裁剪 (pos vs file_size)
// 备注：    ├── 块对齐计算:
// 备注：    │     aligned_start = pos & ~(block_size-1)
// 备注：    │     aligned_len   = round_up(pos+count, block) - aligned_start
// 备注：    │     pad_start     = pos - aligned_start
// 备注：    │
// 备注：    ├── aligned_alloc   ← 分配块对齐缓冲区（避免 IO 对齐错误）
// 备注：    ├── bchfs_do_read  ← 实际读（块对齐偏移）
// 备注：    ├── memcpy(buf, aligned_buf + pad_start, count)  ← 解对齐
// 备注：    ├── pos += count   ← 更新文件偏移
// 备注：    └── return count
// =====================================================================

ssize_t bchfs_read(struct bch_fs *c, int fd, void *buf, size_t count)
{
	bchfs_thread_init();

	struct bchfs_fd *f = bchfs_fd_get(fd);
	if (!f)
		return -EBADF;

	// 备注：边界处理：空读返回 0、pos 超出文件末尾返回 0、裁剪 count 到文件剩余大小
	if (count == 0)
		return 0;

	u64 file_size = f->inode.bi_size;
	if (f->pos >= file_size)
		return 0;

	u64 remaining = file_size - f->pos;
	if (count > remaining)
		count = remaining;

	// 备注：块对齐计算 — bcachefs 要求 IO 偏移和长度必须块对齐
	// 备注：aligned_start = 向下对齐到块边界
	// 备注：pad_start = 对齐前的偏移量（后续 memcpy 解对齐时使用）
	// 备注：aligned_end = 向上对齐到块边界（读整个块范围）
	unsigned block_size = block_bytes(c);
	u64 aligned_start = f->pos & ~(block_size - 1);
	unsigned pad_start  = (size_t)(f->pos - aligned_start);
	u64 aligned_end    = round_up(f->pos + count, block_size);
	size_t aligned_len = (size_t)(aligned_end - aligned_start);

	void *aligned_buf = aligned_alloc(block_size, aligned_len);
	if (!aligned_buf)
		return -ENOMEM;

	// 备注：执行块对齐的同步读，读取完整的对齐范围
	int ret = bchfs_do_read(c, f->inum, aligned_start, aligned_buf, aligned_len);
	if (ret) {
		free(aligned_buf);
		return ret;
	}

	// 备注：将对齐缓冲区中有效数据部分拷贝到用户 buf（解对齐）
	memcpy(buf, (char *)aligned_buf + pad_start, count);
	free(aligned_buf);

	// 备注：更新文件偏移（pos），下次读从新位置开始
	f->pos += count;
	return count;
}

// =====================================================================
// 备注：bchfs_write — 带 RMW 和 pos 更新的完整写
//
// 备注：bchfs_write 执行流程（RMW + 对齐写）：
// 备注：
// 备注：  bchfs_write(fd, buf, count)
// 备注：    │
// 备注：    ├── fd_get → 获取 fd 表项（pos + inode）
// 备注：    ├── 块对齐计算 (同 bchfs_read)
// 备注：    ├── aligned_alloc
// 备注：    │
// 备注：    ├── [RMW] 读起始部分块 ──┬─ pad_start==0? → 跳过
// 备注：    │                       └─ pad_start>0  → bchfs_do_read 起始块
// 备注：    │
// 备注：    ├── [RMW] 读结束部分块 ──┬─ pad_end==0 或整块? → 跳过
// 备注：    │                       └─ 否则 → bchfs_do_read 结束块
// 备注：    │
// 备注：    ├── memcpy 覆盖用户数据到对齐缓冲区
// 备注：    ├── bch2_inode_opts_get_inode → 获取 data_replicas
// 备注：    ├── bchfs_do_write(aligned_start, aligned_buf, new_i_size)
// 备注：    │   └── bch2_write(closure_call)    ← BCH_WRITE_sync 同步完成
// 备注：    │       └── bchfs_write_endio → complete()
// 备注：    │
// 备注：    ├── rust_fuse_update_inode_after_write → 更新 mtime/ctime
// 备注：    ├── bch2_inode_find_by_inum → 刷新本地 inode 缓存
// 备注：    ├── pos += count
// 备注：    └── return count
// 备注：
// 备注：图例：├── 表示顺序步骤，└── 最后步骤
// 备注：       [RMW] 表示 Read-Modify-Write，为保证块对齐而读回未修改部分
// =====================================================================

ssize_t bchfs_write(struct bch_fs *c, int fd, const void *buf, size_t count)
{
	bchfs_thread_init();

	struct bchfs_fd *f = bchfs_fd_get(fd);
	if (!f)
		return -EBADF;

	if (count == 0)
		return 0;

	// 备注：块对齐计算（同 bchfs_read），但写需要额外的 RMW 步骤
	unsigned block_size = block_bytes(c);
	u64 aligned_start = f->pos & ~(block_size - 1);
	unsigned pad_start = (size_t)(f->pos - aligned_start);
	u64 aligned_end   = round_up(f->pos + count, block_size);
	size_t aligned_len = (size_t)(aligned_end - aligned_start);

	// 备注：RMW: 分配对齐缓冲区
	void *aligned_buf = aligned_alloc(block_size, aligned_len);
	if (!aligned_buf)
		return -ENOMEM;

	// 备注：[RMW 步骤 1/3] 读起始部分块
	// 备注：如果写入位置不在块开头（pad_start > 0），需要把该块的前半部分读回来
	// 备注：这样 memcpy 覆盖时不会破坏块中未写入的部分
	// 备注：例如写 pos=5, count=10 → 需要读 block[0..bs) 保留 bytes 0..4
	if (pad_start > 0) {
		int ret = bchfs_do_read(c, f->inum, aligned_start,
					aligned_buf, block_size);
		if (ret) {
			free(aligned_buf);
			return ret;
		}
	}

	// 备注：[RMW 步骤 2/3] 读结束部分块
	// 备注：如果写入在块中间结束（pad_end > 0），需要读最后一个块的后半部分
	// 备注：特例排除：当起始和结束是同一个块且已通过第一步读回时
	// 备注：例如写 count=10 → 需要读 block[末尾块) 保留末尾未覆盖的字节
	unsigned pad_end = (size_t)(aligned_end - f->pos - count);
	if (pad_end > 0 && !(pad_start > 0 && aligned_len == block_size)) {
		u64 end_block_offset = aligned_end - block_size;
		size_t end_buf_offset = aligned_len - block_size;
		int ret = bchfs_do_read(c, f->inum, end_block_offset,
					(char *)aligned_buf + end_buf_offset,
					block_size);
		if (ret) {
			free(aligned_buf);
			return ret;
		}
	}

	// 备注：[RMW 步骤 3/3] 覆盖用户数据到对齐缓冲区
	// 备注：此时 aligned_buf 中已包含未修改块的原始数据，
	// 备注：memcpy 仅覆盖用户需要写入的部分，不会破坏其他数据
	memcpy((char *)aligned_buf + pad_start, buf, count);

	// 备注：获取 inode 的 data_replicas 参数，确定写入副本数
	struct bch_inode_opts opts;
	bch2_inode_opts_get_inode(c, &f->inode, &opts);
	unsigned replicas = max_t(unsigned, opts.data_replicas, 1);

	// 备注：对齐写
	u64 new_i_size = max_t(u64, f->inode.bi_size, f->pos + count);
	int ret = bchfs_do_write(c, f->inum, f->inum.subvol,
				 aligned_start, aligned_buf, aligned_len,
				 new_i_size, replicas);
	free(aligned_buf);
	if (ret)
		return ret;

	// 备注：写入后更新 inode 的 mtime/ctime（通过 Rust FUSE 后端）
	ret = rust_fuse_update_inode_after_write(c, f->inum);
	if (ret)
		return ret;

	// 备注：重新读取 inode 刷新本地缓存
	// 备注：写入可能改变了 bi_size（文件扩展时），需要同步到 f->inode
	// 备注：如果刷新失败，保留旧的 inode 缓存（不中止操作）
	{
		struct bch_inode_unpacked updated;
		int r2 = bch2_inode_find_by_inum(c, f->inum, &updated);
		if (!r2)
			f->inode = updated;
	}

	// 备注：更新文件偏移
	f->pos += count;
	return count;
}

// =====================================================================
// 备注：bchfs_open — 打开文件，分配 fd
//
// 备注：bchfs_open 执行流程：
// 备注：  bchfs_open(path, flags)
// 备注：    │
// 备注：    ├── bchfs_thread_init()        ← 线程单次初始化
// 备注：    ├── bchfs_resolve_path()       ← 从 "/" 逐级 lookup 到目标
// 备注：    │   └── rust_fuse_lookup 逐级解析路径组件
// 备注：    ├── S_ISDIR 检查               ← 不允许 open 目录
// 备注：    ├── bchfs_fd_alloc()           ← 从 64 槽位 FD 表分配空闲项
// 备注：    ├── 填充 fd 表:
// 备注：    │   ├─ inum  (subvol + inum，用于后续 IO)
// 备注：    │   ├─ inode (缓存的 inode 元数据，含 bi_size/mode/replicas)
// 备注：    │   ├─ pos   (文件偏移，初始为 0)
// 备注：    │   └─ flags (O_RDONLY/O_WRONLY 等)
// 备注：    └── return fd (int，类似 POSIX fd)
// 备注：
// 备注：  FD 表是简单的 struct bchfs_fd[64] 静态数组，
// 备注：  不经过系统 VFS，与标准 POSIX fd 完全独立。
// =====================================================================

int bchfs_open(struct bch_fs *c, const char *path, int flags)
{
	bchfs_thread_init();

	subvol_inum inum;
	struct bch_inode_unpacked inode;

	// 备注：解析路径从 root 逐级查找目标文件
	int ret = bchfs_resolve_path(c, path, &inum, &inode);
	if (ret)
		return ret;

	// 备注：bcachefs 的 open 不允许直接打开目录（类比 POSIX 的 EISDIR）
	// 备注：bchfs_stat 可用于获取目录信息，bchfs_create/mkdir 创建目录
	if (S_ISDIR(inode.bi_mode))
		return -EISDIR;

	// 备注：从 FD 表（64 槽位）分配空闲编号
	int fd = bchfs_fd_alloc();
	if (fd < 0)
		return fd;

	// 备注：填充 FD 表项：缓存 inum、inode 元数据（含 bi_size）、pos=0
	struct bchfs_fd *f = &bchfs_fds[fd];
	f->inum		= inum;
	f->inode	= inode;
	f->pos		= 0;
	f->flags	= flags;
	return fd;
}

// =====================================================================
//  bchfs_close
// =====================================================================

// 备注：bchfs_close - 释放 fd 槽位
// 备注：检查 fd 合法性后释放（仅标记 used=false）
// 备注：不刷新 inode、不写回任何数据（数据已通过 bchfs_write 同步写回）
int bchfs_close(struct bch_fs *c, int fd)
{
	struct bchfs_fd *f = bchfs_fd_get(fd);
	if (!f)
		return -EBADF;
	bchfs_fd_free(fd);
	return 0;
}

// =====================================================================
//  bchfs_lseek
// =====================================================================

// 备注：bchfs_lseek - 文件偏移定位
// 备注：支持 SEEK_SET/SEEK_CUR/SEEK_END，实现与 POSIX lseek 一致
// 备注：SEEK_END 基于 f->inode.bi_size（缓存的 inode 元数据，open 时获取）
// 备注：内部维护 f->pos，后续 bchfs_read/write 基于此偏移
ssize_t bchfs_lseek(struct bch_fs *c, int fd, ssize_t offset, int whence)
{
	struct bchfs_fd *f = bchfs_fd_get(fd);
	if (!f)
		return -EBADF;

	u64 new_pos;
	switch (whence) {
	// 备注：SEEK_SET：新位置 = offset（从文件开头绝对定位）
	// 备注：offset 为负数 → 转换为 u64 后是一个超大值，fs 层面会正常处理
	// 备注：但 bchfs_read 的边界裁剪会在 pos>=file_size 时返回 0
	case SEEK_SET:
		new_pos = offset;
		break;

	// 备注：SEEK_CUR：新位置 = 当前位置 + offset
	// 备注：offset 可以为负数（前移），但保证不能越过文件开头
	// 备注：例如 pos=5, offset=-3 → new_pos=2 ✓
	// 备注：例如 pos=5, offset=-10 → -10 > pos → -EINVAL（不越过 0）
	case SEEK_CUR:
		if (offset < 0 && (u64)(-offset) > f->pos)
			return -EINVAL;
		new_pos = f->pos + offset;
		break;

	// 备注：SEEK_END：新位置 = 文件大小 + offset
	// 备注：offset 负数 = 定位到文件末尾前的位置
	// 备注：例如 bi_size=100, offset=-10 → new_pos=90
	// 备注：同样不能越过文件开头（new_pos 不能 < 0）
	// 备注：注意：bi_size 是 open 时缓存的 inode 快照，
	// 备注：如果其他写入扩展了文件，此值可能过时
	case SEEK_END:
		if (offset < 0 && (u64)(-offset) > f->inode.bi_size)
			return -EINVAL;
		new_pos = f->inode.bi_size + offset;
		break;

	default:
		return -EINVAL;
	}

	// 备注：更新 fd 表项中的文件偏移，后续 bchfs_read/write 将从新位置开始
	f->pos = new_pos;
	return new_pos;
}

// =====================================================================
// 备注：bchfs_create / mkdir — 创建文件或目录
//
// 备注：bchfs_create / mkdir 执行流程：
// 备注：
// 备注：  bchfs_create(path, mode)         / bchfs_mkdir(path, mode)
// 备注：    │
// 备注：    └── bchfs_do_create(path, mode, is_dir)
// 备注：          │
// 备注：          ├── bchfs_split_path()       ← 拆父目录 + 文件名
// 备注：          │    "/dir/file" → parent="/dir", name="file"
// 备注：          │    "/file"     → parent="/" (root, inum=4096)
// 备注：          │
// 备注：          ├── mode |= S_IFDIR/S_IFREG  ← 补齐文件类型
// 备注：          │
// 备注：          └── rust_fuse_create(parent, name, mode)
// 备注：                └── (FUSE 后端创建 inode + dirent)
// 备注：                    ├── subvolume_get       ← 获取子卷
// 备注：                    ├── inode_peek_snapshot ← 快照检查
// 备注：                    ├── inode_init_late     ← 初始化新 inode
// 备注：                    ├── inode_create        ← 创建 inode 记录
// 备注：                    └── dirent_create_snapshot  ← 添加目录项
// 备注：
// 备注：  重复创建同一路径 → rust_fuse_create 返回 -2227 (EEXIST)，
// 备注：  bchfs_do_create 原样返回给调用者处理。
// =====================================================================

// 备注：bchfs_do_create - 内部创建文件/目录
// 备注：
// 备注：实现步骤：
// 备注：  1. bchfs_split_path 拆分路径为父目录 + 文件名
// 备注：  2. mode 设置文件类型标志（S_IFDIR/S_IFREG）
// 备注：  3. rust_fuse_create 委托给 Rust FUSE 后端执行创建
// 备注：     Rust 侧内部流程：
// 备注：       subvolume_get → inode_peek_snapshot → inode_init_late
// 备注：       → inode_create → dirent_create_snapshot
// 备注：  4. 重复创建返回 -2227 (EEXIST) 原样传递给调用者
// 备注：
// 备注：DEBUG 信息：stderr 输出用于调试，发布版本应移除
static int bchfs_do_create(struct bch_fs *c, const char *path,
			   mode_t mode, bool is_dir)
{
	bchfs_thread_init();

	subvol_inum parent;
	struct bch_inode_unpacked parent_inode;
	const char *name;
	unsigned name_len;

	// 备注：步骤 1 — 拆分路径：将 "/dir1/dir2/file" 拆为 parent(dst 父目录 inode) + name("file")
	fprintf(stderr, "DBG: bchfs_do_create(%s)\n", path);
	int ret = bchfs_split_path(c, path, &parent, &parent_inode,
				   &name, &name_len);
	if (ret) {
		fprintf(stderr, "DBG: split_path failed: %d\n", ret);
		return ret;
	}
	fprintf(stderr, "DBG: split_path OK, name=%s, parent.inum=%llu\n",
		name, (unsigned long long)parent.inum);

	// 备注：步骤 2 — 标记文件类型：S_IFREG（普通文件）或 S_IFDIR（目录）
	// 备注：mode 的低 12 位保留权限（rwx），高 4 位标记文件类型
	// 备注：调用者传入的 mode 通常只包含权限位（如 0644），这里补齐文件类型
	mode |= is_dir ? S_IFDIR : S_IFREG;

	// 备注：步骤 3 — 调用 Rust FUSE 后端创建 inode + dirent
	// 备注：rust_fuse_create 内部流程：
	// 备注：  rust_fuse_create(c, parent, name, len, mode, umask, &new_inode)
	// 备注：    ├── subvolume_get(parent.subvol)  → 获取父目录子卷
	// 备注：    ├── inode_peek_snapshot(...)       → 检查 snapshot 兼容性
	// 备注：    ├── inode_init_late(...)            → 初始化新 inode 的初始状态
	// 备注：    ├── inode_create(trans, new_inode)  → 在 btree 中创建 inode key
	// 备注：    └── dirent_create_snapshot(...)     → 在父目录下添加目录项
	// 备注：
	// 备注：返回值：0=成功，-2227(EEXIST)=目标路径已存在原样传递
	struct bch_inode_unpacked new_inode;
	fprintf(stderr, "DBG: calling rust_fuse_create...\n");
	ret = rust_fuse_create(c, parent, (const unsigned char *)name,
				name_len, mode, 0, &new_inode);
	fprintf(stderr, "DBG: rust_fuse_create returned %d\n", ret);
	return ret;
}

int bchfs_create(struct bch_fs *c, const char *path, mode_t mode)
{
	return bchfs_do_create(c, path, mode, false);
}

int bchfs_mkdir(struct bch_fs *c, const char *path, mode_t mode)
{
	return bchfs_do_create(c, path, mode, true);
}

// =====================================================================
// 备注：bchfs_unlink / rmdir — 删除文件或目录
//
// 备注：bchfs_unlink / rmdir 执行流程：
// 备注：  bchfs_unlink(path) / bchfs_rmdir(path)
// 备注：    │
// 备注：    └── bchfs_do_unlink(path)
// 备注：          ├── bchfs_split_path()       ← 拆父目录 + 文件名
// 备注：          └── rust_fuse_unlink(parent, name)
// 备注：                └── bcachefs 内部：删除 dirent，清理 inode 引用
// 备注：
// 备注：  bchfs_rmdir 与 bchfs_unlink 共用同一个实现。
// 备注：  rust_fuse_unlink 内部会判断目录是否为空。
// =====================================================================

// 备注：bchfs_do_unlink - 内部删除文件/目录
// 备注：bchfs_unlink 和 bchfs_rmdir 共用此实现。
// 备注：rust_fuse_unlink 内部判断目标类型（文件/目录）和目录是否为空。
// 备注：删除后不验证结果（信任 Rust 后端）。
static int bchfs_do_unlink(struct bch_fs *c, const char *path)
{
	bchfs_thread_init();

	subvol_inum parent;
	struct bch_inode_unpacked parent_inode;
	const char *name;
	unsigned name_len;

	// 备注：步骤 1 — 拆分路径：获取父目录 inode 和要删除的文件/目录名
	// 备注：name 指向 path 内部（零拷贝），parent 为父目录的 subvol+inum
	int ret = bchfs_split_path(c, path, &parent, &parent_inode,
				   &name, &name_len);
	if (ret)
		return ret;

	// 备注：步骤 2 — 委托给 Rust FUSE 后端执行删除
	// 备注：rust_fuse_unlink 内部流程：
	// 备注：  ├── fuse_lookup(parent, name)            → 找到目标的 inum
	// 备注：  ├── 目标类型检查：目录 → 检查是否为空目录
	// 备注：  ├── bch2_dirent_delete(trans, parent, name)  → 删除目录项
	// 备注：  ├── inode_dec_linkcount(trans, target)    → 减少 link count
	// 备注：  └── if linkcount==0 → inode_sub_optim()  → 标记 inode 为删除
	// 备注：
	// 备注：返回值：0=成功，-ENOENT=路径不存在，-ENOTEMPTY=目录非空
	return rust_fuse_unlink(c, parent, (const unsigned char *)name, name_len);
}

int bchfs_unlink(struct bch_fs *c, const char *path)
{
	return bchfs_do_unlink(c, path);
}

int bchfs_rmdir(struct bch_fs *c, const char *path)
{
	return bchfs_do_unlink(c, path);
}

// =====================================================================
// 备注：bchfs_rename — 重命名/移动文件
//
// 备注：bchfs_rename 执行流程（涉及两个路径）：
// 备注：
// 备注：  bchfs_rename(oldpath, newpath)
// 备注：    │
// 备注：    ├── bchfs_split_path(oldpath)    ← 拆 src 父目录 + 文件名
// 备注：    │    "/dir/a.txt" → src_parent="/dir", src_name="a.txt"
// 备注：    │
// 备注：    ├── bchfs_split_path(newpath)    ← 拆 dst 父目录 + 文件名
// 备注：    │    "/dir/b.txt" → dst_parent="/dir", dst_name="b.txt"
// 备注：    │
// 备注：    └── rust_fuse_rename(src_parent, src_name,
// 备注：                              dst_parent, dst_name)
// 备注：          └── bcachefs 内部：跨/同目录移动 dirent
// 备注：
// 备注：  注意：两个路径可以跨目录。newpath 的父目录必须存在。
// =====================================================================

int bchfs_rename(struct bch_fs *c, const char *oldpath, const char *newpath)
{
	bchfs_thread_init();

	subvol_inum src_parent, dst_parent;
	struct bch_inode_unpacked src_parent_inode, dst_parent_inode;
	const char *src_name, *dst_name;
	unsigned src_len, dst_len;

	// 备注：第一次 split_path：拆分旧路径为 src_parent + src_name
	int ret = bchfs_split_path(c, oldpath, &src_parent, &src_parent_inode,
				   &src_name, &src_len);
	if (ret)
		return ret;

	// 备注：第二次 split_path：拆分新路径为 dst_parent + dst_name
	// 备注：两个路径可跨目录，src_parent 和 dst_parent 可以不同
	ret = bchfs_split_path(c, newpath, &dst_parent, &dst_parent_inode,
			       &dst_name, &dst_len);
	if (ret)
		return ret;

	// 备注：委托给 Rust FUSE 后端执行 rename
	// 备注：Rust 内部：unlink destination（如果存在）→ 更新 dirent
	return rust_fuse_rename(c,
				src_parent, (const unsigned char *)src_name, src_len,
				dst_parent, (const unsigned char *)dst_name, dst_len);
}

// =====================================================================
// 备注：bchfs_stat / fstat — 获取 inode 元数据
//
// 备注：bchfs_stat 执行流程：
// 备注：  bchfs_stat(path, inode_out)
// 备注：    └── bchfs_resolve_path(path)   ← 从 "/" 逐级 lookup
// 备注：        └── rust_fuse_lookup 逐级解析
// 备注：
// 备注：bchfs_fstat 执行流程：
// 备注：  bchfs_fstat(fd, inode_out)
// 备注：    └── bchfs_fd_get(fd)           ← 查 FD 表
// 备注：        └── 直接返回缓存的 inode 副本（不重新查询磁盘）
// 备注：
// 备注：  注意：bchfs_fstat 返回的是 open 时的 inode 快照，
// 备注：  如果写入后未刷新（bchfs_write 内部会刷新），可能过时。
// =====================================================================

int bchfs_stat(struct bch_fs *c, const char *path,
	       struct bch_inode_unpacked *inode_out)
{
	bchfs_thread_init();

	// 备注：通过路径逐级 lookup 获取 inode 元数据
	subvol_inum inum;
	return bchfs_resolve_path(c, path, &inum, inode_out);
}

int bchfs_fstat(struct bch_fs *c, int fd,
		struct bch_inode_unpacked *inode_out)
{
	// 备注：直接从 FD 表返回缓存的 inode 副本，不重新查询磁盘
	// 备注：适用于 bchfs_read/write 后快速获取文件大小等属性
	// 备注：注意：如果其他路径修改了该 inode，本地缓存可能过时
	struct bchfs_fd *f = bchfs_fd_get(fd);
	if (!f)
		return -EBADF;
	*inode_out = f->inode;
	return 0;
}
