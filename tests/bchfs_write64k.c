// SPDX-License-Identifier: GPL-2.0
// bchfs_write64k.c — 最小化 64KB 写入测试，仅用于 GDB 断点跟踪 5 个 btree 更新
// 无 read-back 验证，无大文件，无目录操作，无 unlink
//
// Build: make -f .tests/Makefile
// Run:   gdb -batch -x .tests/gdb_trace_btree.gdb .tests/bchfs_write64k

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "bcachefs.h"

void bch2_start_http_lazy(void) { }
char *bch2_scan_devices(const char *path) { return NULL; }

#include "init/fs.h"
#include "opts.h"
#include "bchfs_shims.h"

extern unsigned long _totalram_pages;

int main(int argc, char *argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <bcachefs image>\n", argv[0]);
		return 1;
	}

	if (!_totalram_pages) {
		long npages = sysconf(_SC_PHYS_PAGES);
		if (npages > 0)
			_totalram_pages = npages;
	}

	printf("============================================================\n");
	printf("  bcachefs 64KB Write Only Test\n  Image: %s\n", argv[1]);
	printf("============================================================\n");
	fflush(stdout);

	darray_const_str devs;
	darray_init(&devs);
	darray_push(&devs, argv[1]);

	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, nostart, false);
	printf(">> bch2_fs_open(nostart=false)...\n"); fflush(stdout);
	struct bch_fs *c = bch2_fs_open(&devs, &opts);
	int ret = PTR_ERR_OR_ZERO(c);
	if (ret) { printf(">> OPEN FAILED: %d\n", ret); return 1; }
	printf(">> bch2_fs_open OK\n"); fflush(stdout);

	// === 仅 64KB 写入 ===
	printf("\n=== 64KB WRITE ONLY ===\n");

	ret = bchfs_create(c, "/write64k.bin", 0644);
	printf("  create: %d\n", ret);

	int fd = bchfs_open(c, "/write64k.bin", O_WRONLY);
	printf("  open: %d\n", fd);

	if (fd >= 0) {
		size_t sz = 64 * 1024;
		char *buf = malloc(sz);
		if (buf) {
			memset(buf, 0x41, sz);
			ssize_t w = bchfs_write(c, fd, buf, sz);
			printf("  write 64KB: %zd\n", w);
			bchfs_close(c, fd);
			free(buf);
		}
	}

	// === 清理退出 ===
	printf(">> bch2_fs_exit()...\n"); fflush(stdout);
	bch2_fs_exit(c);
	printf(">> Exit complete\n"); fflush(stdout);

	darray_exit(&devs);
	return 0;
}
