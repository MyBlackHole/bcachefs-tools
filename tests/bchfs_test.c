// SPDX-License-Identifier: GPL-2.0
// 备注：本文件测试 bchfs_shims FD-based 直接 I/O API——不经过 FUSE 层，
//       直接调用 bcachefs 文件系统内部接口进行 stat、create/write/close/open/read、
//       mkdir+rmdir、rename 等操作验证。
// bchfs_test.c — bchfs_shims API test
// Tests: stat, create/write/close/open/read, mkdir+rmdir, rename, allocation
// 备注：allocation 测试通过写入不同大小的文件触发 bucket 分配、
//       extent btree 更新（extent trigger）和 alloc btree 更新（alloc trigger），
//       用于 GDB 断点跟踪 bch2_bucket_alloc_trans、bch2_trigger_extent 等函数。
//
// Build:  make -f .tests/Makefile

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include "bcachefs.h"
#include "debug/tests.h"

// 备注：stub——独立测试模式不链接 http/scan 模块，
//       提供空实现避免链接错误。生产代码中这些函数由对应模块提供。
void bch2_start_http_lazy(void) { }
char *bch2_scan_devices(const char *path) { return NULL; }

#include "init/fs.h"
#include "opts.h"
#include "bchfs_shims.h"

static int n_pass = 0, n_fail = 0;
// 备注：TEST 宏——测试布尔表达式 cond，通过则 PASS 否则 FAIL，
//       用于测试文件系统特性状态的断言。
#define TEST(cond, msg) do {					\
	if (cond) { n_pass++; printf("  PASS: %s\n", msg); }	\
	else { n_fail++; printf("  FAIL: %s\n", msg); }	\
	fflush(stdout);					\
} while (0)

// 备注：TEST_OK 宏——检查系统调用返回值 >= 0，
//       失败时打印 errno 和对应的错误描述字符串。
#define TEST_OK(ret, msg) do {					\
	if ((ret) >= 0) {					\
		n_pass++;				\
		printf("  PASS: %s (ret=%d)\n", msg, (int)(ret)); \
	} else {					\
		n_fail++;				\
		printf("  FAIL: %s (err=%d %s)\n",	\
		       msg, -(int)(ret), strerror(-(int)(ret))); \
	}						\
	fflush(stdout);					\
} while (0)

// 备注：打印 bch_inode_unpacked 核心元数据字段：
//       bi_inum（inode 号）、bi_mode（文件类型+权限掩码）、bi_size（文件大小）。
static void print_inode(const char *l, const struct bch_inode_unpacked *bi)
{
	printf("  %s: inum=%llu mode=%o size=%llu\n",
	       l, (unsigned long long)bi->bi_inum,
	       (unsigned)bi->bi_mode,
	       (unsigned long long)bi->bi_size);
	fflush(stdout);
}

static void test_stat_root(struct bch_fs *c)
{
	printf("\n=== Test: stat /\n");
	struct bch_inode_unpacked bi;
	// 备注：bchfs_stat 查询根目录"/"的 inode 元数据。
	int ret = bchfs_stat(c, "/", &bi);
	TEST_OK(ret, "stat /");
	if (ret < 0) return;
	print_inode("/", &bi);
	TEST(S_ISDIR(bi.bi_mode), "/ is a directory");
	// 备注：bcachefs 根 inode 号固定为 4096（BCACHEFS_ROOT_INO），
	//       这是文件系统初始化时的约定值，用于快速定位根目录。
	TEST(bi.bi_inum == 4096, "root inum = 4096");
}

static void test_create_write_read(struct bch_fs *c)
{
	// 备注：完整 I/O 周期测试：创建 → 写入 → close → 重新 open → 读取 → 验证内容
	printf("\n=== Test: create -> write -> close -> open -> read\n");

	const char *data = "Hello bcachefs direct I/O!";
	size_t dlen = strlen(data) + 1;

	printf("  >> calling bchfs_create...\n"); fflush(stdout);
	// 备注：bchfs_create 在根目录创建普通文件，mode 0644（用户读写、组读、其他人读）。
	TEST_OK(bchfs_create(c, "/test_file.txt", 0644), "create");

	// 备注：bchfs_open 以只写方式打开，返回 fd 供后续 bchfs_write 使用。
	int fd = bchfs_open(c, "/test_file.txt", O_WRONLY);
	TEST_OK(fd, "open O_WRONLY");
	if (fd < 0) return;

	struct bch_inode_unpacked bi;
	// 备注：bchfs_stat 查询刚创建的文件的 inode 元数据。
	int sr = bchfs_stat(c, "/test_file.txt", &bi);
	TEST_OK(sr, "stat");
	print_inode("/test_file.txt", &bi);

	// 备注：bchfs_write 通过 FD 写入数据到文件，返回写入字节数。
	ssize_t w = bchfs_write(c, fd, data, dlen);
	TEST(w == (ssize_t)dlen, "write all bytes");
	// 备注：bchfs_close 关闭 FD，数据已落盘（直接 I/O 模式无 page cache 延迟）。
	bchfs_close(c, fd);

	// 备注：再次 stat 验证写操作后文件大小与写入数据长度一致。
	sr = bchfs_stat(c, "/test_file.txt", &bi);
	TEST_OK(sr, "stat");
	if (sr >= 0)
		TEST(bi.bi_size == dlen, "size matches");
	print_inode("/test_file.txt", &bi);

	// 备注：bchfs_open 以只读方式重新打开，准备读取之前写入的数据。
	fd = bchfs_open(c, "/test_file.txt", O_RDONLY);
	TEST_OK(fd, "open O_RDONLY");
	if (fd < 0) return;

	char buf[256] = {};
	// 备注：bchfs_read 通过 FD 读取文件内容到缓冲区，返回实际读取字节数。
	ssize_t r = bchfs_read(c, fd, buf, sizeof(buf)-1);
	TEST(r == (ssize_t)dlen, "read all data");
	if (r > 0)
		TEST(strcmp(buf, data) == 0, "content matches");
	bchfs_close(c, fd);

	// 备注：bchfs_unlink 删除文件，释放磁盘空间。
	bchfs_unlink(c, "/test_file.txt");
}

static void test_mkdir_and_nested(struct bch_fs *c)
{
	// 备注：测试目录操作：创建目录 → 在目录中创建文件 → 读写 → 删除文件 → 删除目录
	printf("\n=== Test: mkdir + nested file\n");

	// 备注：bchfs_mkdir 创建目录，mode 0755（用户读写执行、组读执行、其他人读执行）。
	TEST_OK(bchfs_mkdir(c, "/testdir", 0755), "mkdir");
	TEST_OK(bchfs_create(c, "/testdir/nested.txt", 0644), "create nested");

	int fd = bchfs_open(c, "/testdir/nested.txt", O_WRONLY);
	TEST_OK(fd, "open nested");
	if (fd >= 0) {
		bchfs_write(c, fd, "nested", 6);
		bchfs_close(c, fd);
	}

	// 备注：必须先删除目录内的所有文件（unlink），然后才能 rmdir 删除空目录。
	TEST_OK(bchfs_unlink(c, "/testdir/nested.txt"), "unlink nested");
	// 备注：bchfs_rmdir 只能删除空目录，非空目录返回 -ENOTEMPTY。
	TEST_OK(bchfs_rmdir(c, "/testdir"), "rmdir");
}

static void test_rename(struct bch_fs *c)
{
	// 备注：测试 rename：创建源文件 → 写入 → 重命名 → 验证新路径可 stat
	printf("\n=== Test: rename\n");

	TEST_OK(bchfs_create(c, "/rename_src.txt", 0644), "create src");
	int fd = bchfs_open(c, "/rename_src.txt", O_WRONLY);
	if (fd >= 0) {
		bchfs_write(c, fd, "rename data", 11);
		bchfs_close(c, fd);
	}

	// 备注：bchfs_rename 原子性地将文件从源路径移动到目标路径，
	//       内核保证 rename 操作的原子性——其他操作者要么看到旧路径、要么看到新路径。
	TEST_OK(bchfs_rename(c, "/rename_src.txt", "/rename_dst.txt"), "rename");

	struct bch_inode_unpacked bi;
	int ret = bchfs_stat(c, "/rename_dst.txt", &bi);
	TEST_OK(ret, "stat new path");
	bchfs_unlink(c, "/rename_dst.txt");
}

static void test_allocation(struct bch_fs *c)
{
	// 备注：本测试验证文件系统空间分配和释放路径——通过写入不同大小的文件
	//       触发 bucket 分配、extent btree 更新（extent trigger）和
	//       alloc btree 更新（alloc trigger），供 GDB 设置断点跟踪。
	// 备注：小文件（64KB < 默认 bucket 256KB）→ 部分填充单个 bucket
	// 备注：大文件（2MB ~ 256KB/桶 × 8 桶）→ 触发 stripe 分配器跨 bucket 分配
	printf("\n=== Test: allocation (write small + large files)\n");

	char *buf = NULL;
	int fd;
	struct bch_inode_unpacked bi;

	// ---- small_file.bin: 64KB of 0x41 ----
	// 备注：64KB < 默认 bucket 大小，测试部分填充 bucket 路径
	TEST_OK(bchfs_create(c, "/small_file.bin", 0644), "create small_file.bin");

	fd = bchfs_open(c, "/small_file.bin", O_WRONLY);
	TEST_OK(fd, "open small_file.bin O_WRONLY");
	if (fd >= 0) {
		size_t small_size = 64 * 1024;
		buf = malloc(small_size);
		if (buf) {
			memset(buf, 0x41, small_size);

			// 备注：bchfs_write 64KB → 触发 bch2_alloc_sectors_req
			// → bch2_bucket_alloc_trans 分配 bucket 空间
			ssize_t w = bchfs_write(c, fd, buf, small_size);
			TEST(w == (ssize_t)small_size, "write 64KB to small_file.bin");
			bchfs_close(c, fd);

			// 备注：stat 验证写入后文件大小 = 64KB
			int ret = bchfs_stat(c, "/small_file.bin", &bi);
			TEST_OK(ret, "stat small_file.bin");
			if (ret >= 0)
				TEST(bi.bi_size == small_size,
				     "small_file.bin size matches");

			// 备注：重新以只读方式打开，读取全部数据并验证内容
			fd = bchfs_open(c, "/small_file.bin", O_RDONLY);
			TEST_OK(fd, "open small_file.bin O_RDONLY");
			if (fd >= 0) {
				char *rbuf = malloc(small_size);
				if (rbuf) {
					ssize_t r = bchfs_read(c, fd, rbuf, small_size);
					TEST(r == (ssize_t)small_size,
					     "read 64KB from small_file.bin");
					if (r > 0)
						TEST(memcmp(rbuf, buf, small_size) == 0,
						     "small_file.bin content matches");
					free(rbuf);
				}
				bchfs_close(c, fd);
			}
			free(buf);
			buf = NULL;
		}
	}

	// ---- large_file.bin: 2MB with 256-byte pattern ----
	// 备注：2MB 数据在默认 256KB/bucket 下约需 8 个 bucket，
	//       测试 stripe 分配器跨 bucket 分配逻辑
	TEST_OK(bchfs_create(c, "/large_file.bin", 0644), "create large_file.bin");

	fd = bchfs_open(c, "/large_file.bin", O_WRONLY);
	TEST_OK(fd, "open large_file.bin O_WRONLY");
	if (fd >= 0) {
		size_t large_size = 2 * 1024 * 1024;
		buf = malloc(large_size);
		if (buf) {
			// 备注：生成带位置信息的 256 字节重复模式
			//       每个 256 字节块 = (块序号 ^ j) 其中 j 为块内偏移
			//       确保每个块内容唯一且与位置相关
			for (size_t i = 0; i < large_size; i += 256)
				for (int j = 0; j < 256; j++)
					buf[i + j] = (unsigned char)((i / 256) ^ j);

			// 备注：bchfs_write 2MB → 多次触发 bch2_alloc_sectors_req
			// → bch2_bucket_alloc_trans（每次写入跨多 bucket 边界时分配新 bucket）
			//       大文件同时触发 extent trigger（bch2_trigger_extent）
			//       在 extent btree 中插入范围键
			ssize_t w = bchfs_write(c, fd, buf, large_size);
			TEST(w == (ssize_t)large_size, "write 2MB to large_file.bin");
			bchfs_close(c, fd);

			// 备注：stat 验证写入后文件大小 = 2MB
			int ret = bchfs_stat(c, "/large_file.bin", &bi);
			TEST_OK(ret, "stat large_file.bin");
			if (ret >= 0)
				TEST(bi.bi_size == large_size,
				     "large_file.bin size matches");

			// 备注：重新以只读方式打开，读取全部数据并验证内容
			fd = bchfs_open(c, "/large_file.bin", O_RDONLY);
			TEST_OK(fd, "open large_file.bin O_RDONLY");
			if (fd >= 0) {
				char *rbuf = malloc(large_size);
				if (rbuf) {
					ssize_t r = bchfs_read(c, fd, rbuf, large_size);
					TEST(r == (ssize_t)large_size,
					     "read 2MB from large_file.bin");
					if (r > 0)
						TEST(memcmp(rbuf, buf, large_size) == 0,
						     "large_file.bin content matches");
					free(rbuf);
				}
				bchfs_close(c, fd);
			}
			free(buf);
			buf = NULL;
		}
	}

	// 备注：bchfs_unlink 删除文件 → 触发 alloc trigger
	//       （bucket 状态从非空 transition 到空，回收空间）
	//       删除会修改 alloc btree，触发 bch2_trigger_alloc 回调
	TEST_OK(bchfs_unlink(c, "/large_file.bin"), "unlink large_file.bin");
	TEST_OK(bchfs_unlink(c, "/small_file.bin"), "unlink small_file.bin");
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <bcachefs image>\n", argv[0]);
		return 1;
	}

	/* Initialize totalram_pages for journal mem_limit:
	   linux_shrinkers_init() isn't called in standalone test mode,
	   so set _totalram_pages from sysconf. Without this, mem_limit=0
	   clamps journal space to 0, causing journal_full on every write. */
	// 备注：__totalram_pages 修复——bcachefs journal 模块用 _totalram_pages
	//       计算 mem_limit（可用的 journal 空间上限）。独立测试模式下不调用
	//       linux_shrinkers_init()，需要手动从 sysconf(_SC_PHYS_PAGES) 获取。
	//       如果 _totalram_pages 为 0，mem_limit=0 会导致每次写入都 journal full。
	extern unsigned long _totalram_pages;
	if (!_totalram_pages) {
		long npages = sysconf(_SC_PHYS_PAGES);
		if (npages > 0)
			_totalram_pages = npages;
	}

	printf("============================================================\n");
	printf("  bchfs API Test\n  Image: %s\n", argv[1]);
	printf("============================================================\n");
	fflush(stdout);

	// 备注：darray 是 bcachefs 的动态数组容器——darray_const_str 声明 const char* 数组，
	//       darray_init 初始化，darray_push 追加元素（此处是设备路径）。
	darray_const_str devs;
	darray_init(&devs);
	darray_push(&devs, argv[1]);

	// 备注：nostart=false 表示在 bch2_fs_open 时同时启动文件系统，
	//       即加载 btree 根节点、恢复 journal、开始 IO 线程。
	//       若 nostart=true 则只加载超块元数据，不启动完整运行。
	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, nostart, false);
	printf(">> bch2_fs_open(nostart=false)...\n"); fflush(stdout);
	// 备注：bch2_fs_open 创建并启动 bcachefs 文件系统实例，
	//       返回的 struct bch_fs 是所有后续操作的上下文（类似内核的 sb）。
	//       PTR_ERR_OR_ZERO 将 ERR_PTR 编码的错误转换为负 errno。
	struct bch_fs *c = bch2_fs_open(&devs, &opts);
	int ret = PTR_ERR_OR_ZERO(c);
	if (ret) { printf(">> OPEN FAILED: %d\n", ret); return 1; }
	printf(">> bch2_fs_open OK\n"); fflush(stdout);

	// 备注：allocation 测试验证核心空间分配机制，作为文件系统基本功能测试
	//       放在 stat 之前，确保写入/分配路径在查询路径之前验证
	test_allocation(c);
	test_stat_root(c);
	test_create_write_read(c);
	test_mkdir_and_nested(c);
	test_rename(c);

	// 备注：bch2_fs_exit 停止所有后台任务（journal 刷新、btree GC、IO 线程等），
	//       释放 bcachefs 文件系统实例的所有资源。
	printf("\n>> bch2_fs_exit()...\n"); fflush(stdout);
	bch2_fs_exit(c);
	printf(">> Exit complete\n"); fflush(stdout);

	// 备注：释放 darray 动态数组占用的内存。
	darray_exit(&devs);

	printf("\n============================================================\n");
	printf("  Results: %d pass, %d fail\n", n_pass, n_fail);
	printf("============================================================\n");
	return n_fail > 0 ? 1 : 0;
}
