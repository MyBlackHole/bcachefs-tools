// SPDX-License-Identifier: GPL-2.0
// bchfs_btreetxn.c — 直接操作 btree 事务，跟踪 trans_commit→trigger→journal 链路
//
// 在一个事务中更新 3 个 alloc key (free→need_discard)，观察：
//   1. trigger_alloc 调用次数
//   2. journal entry 数量 (应该是 1 个 entry 含 3 个 jset_entry)
//   3. journal_buf_put_final 次数
//
// Build: make -f .tests/Makefile
// Run:   .tests/bchfs_btreetxn /tmp/test_alloc.bcachefs

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "bcachefs.h"

void bch2_start_http_lazy(void) { }
char *bch2_scan_devices(const char *path) { return NULL; }

#include "init/fs.h"
#include "opts.h"
#include "btree/update.h"
#include "btree/iter.h"
#include "alloc/background.h"
#include "journal/journal.h"

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
	printf("  btree txn + journal trace (3x alloc keys)\n  Image: %s\n", argv[1]);
	printf("============================================================\n");

	darray_const_str devs;
	darray_init(&devs);
	darray_push(&devs, argv[1]);

	struct bch_opts opts = bch2_opts_empty();
	opt_set(opts, nostart, false);
	printf(">> bch2_fs_open...\n");
	struct bch_fs *c = bch2_fs_open(&devs, &opts);
	int ret = PTR_ERR_OR_ZERO(c);
	if (ret) { printf(">> OPEN FAILED: %d\n", ret); return 1; }
	printf(">> OK\n");

	// === 事务：在同一个事务中更新 3 个 alloc key ===
	// bucket 100, 101, 102: free(0) → need_discard(2)
	// 使用 restart 循环（commit_do 可能会无限 loop，手动控制）
	printf("\n=== BTREE_TXN: 3x alloc key updates (free→need_discard) ===\n");

	struct btree_trans *trans = bch2_trans_get(c);
	int commit_ret = 0;
	int max_retries = 5;

	do {
		bch2_trans_begin(trans);

		// 如果 restart 了所有 ptr 都失效，必须重新获取
		struct bkey_i_alloc_v4 *a100 =
			bch2_trans_start_alloc_update(trans, POS(0, 100), 0);
		if (IS_ERR(a100)) {
			commit_ret = PTR_ERR(a100);
			if (bch2_err_matches(commit_ret, BCH_ERR_transaction_restart))
				continue;
			break;
		}
		struct bkey_i_alloc_v4 *a101 =
			bch2_trans_start_alloc_update(trans, POS(0, 101), 0);
		if (IS_ERR(a101)) {
			commit_ret = PTR_ERR(a101);
			if (bch2_err_matches(commit_ret, BCH_ERR_transaction_restart))
				continue;
			break;
		}
		struct bkey_i_alloc_v4 *a102 =
			bch2_trans_start_alloc_update(trans, POS(0, 102), 0);
		if (IS_ERR(a102)) {
			commit_ret = PTR_ERR(a102);
			if (bch2_err_matches(commit_ret, BCH_ERR_transaction_restart))
				continue;
			break;
		}

		// 所有 start_alloc_update 成功了，修改值
		printf("  b100: old type=%u dirty=%u\n", a100->v.data_type, a100->v.dirty_sectors);
		printf("  b101: old type=%u dirty=%u\n", a101->v.data_type, a101->v.dirty_sectors);
		printf("  b102: old type=%u dirty=%u\n", a102->v.data_type, a102->v.dirty_sectors);
		a100->v.data_type = 2; // BCH_DATA_need_discard
		a100->v.dirty_sectors = 0;
		a101->v.data_type = 2;
		a101->v.dirty_sectors = 0;
		a102->v.data_type = 2;
		a102->v.dirty_sectors = 0;

		printf("  calling bch2_trans_commit (3x updates)...\n");
		commit_ret = bch2_trans_commit(trans, NULL, NULL, 0);
		if (commit_ret && bch2_err_matches(commit_ret, BCH_ERR_transaction_restart)) {
			fprintf(stderr, "  -> restart, loop\n");
			continue;
		}
		break;
	} while (--max_retries > 0);

	if (max_retries <= 0 && commit_ret)
		fprintf(stderr, "  EXCEEDED MAX RETRIES\n");

	printf("  commit_ret: %d (0=OK)\n", commit_ret);

	if (!commit_ret) {
		printf(">> bch2_journal_meta (force flush)...\n");
		int meta_ret = bch2_journal_meta(&c->journal);
		printf(">> meta_ret: %d\n", meta_ret);
	}

	bch2_trans_put(trans);
	printf("  UPDATED: b100 b101 b102: free → need_discard\n");

	// === 清理退出 ===
	printf("\n>> bch2_fs_exit...\n");
	bch2_fs_exit(c);
	printf(">> Exit complete\n");

	darray_exit(&devs);
	return commit_ret ? 1 : 0;
}
