// ============================================
// 备注：libbcachefs C API 头文件
//
// 备注：声明 libbcachefs 库的 C API，供混合 C/Rust 用户空间使用：
// 备注：  bch_opt_strs — 选项字符串的 union 类型（BCH_OPTS() x-macro 生成）
// 备注：  format_opts — 格式化选项（label, uuid, etc.）
//
// 备注：选项解析流程：
// 备注：  字符串输入 → bch2_parse_opts() → bch_opts（位打包的内部表示）
//
// 备注：bch_opt_strs 的 union 布局确保可以通过名称（strs.label）或
// 备注：索引（strs.by_id[i]）访问选项。
// ============================================
#ifndef _LIBBCACHE_H
#define _LIBBCACHE_H

#include <linux/uuid.h>
#include <stdbool.h>

#include "bcachefs.h"
#include "bcachefs_format.h"

#include "tools-util.h"

/* option parsing */

struct bch_opt_strs {
union {
	char			*by_id[bch2_opts_nr];
struct {
#define x(_name, ...)	char	*_name;
	BCH_OPTS()
#undef x
};
};
};

void bch2_opt_strs_free(struct bch_opt_strs *);
struct bch_opts bch2_parse_opts(struct bch_opt_strs);

struct format_opts {
	char		*label;
	__uuid_t	uuid;
	unsigned	version;
	unsigned	superblock_size;
	bool		encrypted;
	char		*passphrase_file;
	char		*passphrase;
	char		*source;
	bool		no_sb_at_end;
};

struct dev_opts {
	struct file	*file;
	struct block_device *bdev;
	const char	*path;

	u64		sb_offset;
	u64		sb_end;

	u64		nbuckets;
	u64		fs_size;

	const char	*label; /* make this a bch_opt */

	struct bch_opts	opts;
};

typedef DARRAY(struct dev_opts) dev_opts_list;

static inline struct dev_opts dev_opts_default()
{
	return (struct dev_opts) { .opts = bch2_opts_empty() };
}

#endif /* _LIBBCACHE_H */
