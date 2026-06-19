// ============================================
// 备注：libbcachefs C 包装 — 选项解析
//
// 备注：实现 libbcachefs.h 中声明的 libbcachefs 库 C API：
// 备注：  bch2_opt_strs_free() — 释放选项字符串数组
// 备注：  bch2_parse_opts() — 将字符串选项解析为 bch_opts
//
// 备注：选项解析流程：
// 备注：  1. 遍历所有已知选项（bch2_opt_table）
// 备注：  2. 对每个已设置的选项调用 bch2_opt_parse()
// 备注：  3. 将解析后的 u64 值设置到 bch_opts 中
// 备注：  4. 忽略需要打开文件系统的选项（BCH_ERR_option_needs_open_fs）
// ============================================
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "libbcachefs.h"
#include "tools-util.h"

#include "bcachefs.h"

void bch2_opt_strs_free(struct bch_opt_strs *opts)
{
	unsigned i;

	for (i = 0; i < bch2_opts_nr; i++) {
		free(opts->by_id[i]);
		opts->by_id[i] = NULL;
	}
}

struct bch_opts bch2_parse_opts(struct bch_opt_strs strs)
{
	struct bch_opts opts = bch2_opts_empty();
	struct printbuf err = PRINTBUF;
	unsigned i;
	int ret;
	u64 v;

	for (i = 0; i < bch2_opts_nr; i++) {
		if (!strs.by_id[i])
			continue;

		ret = bch2_opt_parse(NULL,
				     &bch2_opt_table[i],
				     strs.by_id[i], &v, &err);
		if (ret < 0 && ret != -BCH_ERR_option_needs_open_fs)
			die("Invalid option %s", err.buf);

		bch2_opt_set_by_id(&opts, i, v);
	}

	printbuf_exit(&err);
	return opts;
}

