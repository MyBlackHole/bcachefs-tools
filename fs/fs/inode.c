// SPDX-License-Identifier: GPL-2.0

#include "bcachefs.h"

#include "alloc/accounting.h"
#include "alloc/buckets.h"

#include "btree/key_cache.h"
#include "btree/locking.h"
#include "btree/write_buffer.h"
#include "btree/bkey_methods.h"
#include "btree/update.h"

#include "data/compress.h"
#include "data/extents.h"
#include "data/extent_update.h"

#include "fs/dirent.h"
#include "fs/inode.h"
#include "fs/namei.h"
#include "fs/str_hash.h"

#include "vfs/fs.h"

#include "init/error.h"
#include "init/passes.h"

#include "snapshots/snapshot.h"
#include "snapshots/subvolume.h"

#include "util/varint.h"

#include <linux/random.h>
#include <linux/unaligned.h>

#define x(name, ...)	#name,
const char * const bch2_inode_opts[] = {
	BCH_INODE_OPTS()
	NULL,
};

static const char * const bch2_inode_flag_strs[] = {
	BCH_INODE_FLAGS()
	NULL
};
#undef  x

static int delete_ancestor_snapshot_inodes(struct btree_trans *, struct bpos);
static int may_delete_deleted_inum(struct btree_trans *, subvol_inum, struct bch_inode_unpacked *);

static const u8 byte_table[8] = { 1, 2, 3, 4, 6, 8, 10, 13 };

static int inode_decode_field(const u8 *in, const u8 *end,
			      u64 out[2], unsigned *out_bits)
{
	__be64 be[2] = { 0, 0 };
	unsigned bytes, shift;
	u8 *p;

	if (in >= end)
		return -BCH_ERR_inode_unpack_error;

	if (!*in)
		return -BCH_ERR_inode_unpack_error;

	/*
	 * position of highest set bit indicates number of bytes:
	 * shift = number of bits to remove in high byte:
	 */
	shift	= 8 - __fls(*in); /* 1 <= shift <= 8 */
	bytes	= byte_table[shift - 1];

	if (in + bytes > end)
		return -BCH_ERR_inode_unpack_error;

	p = (u8 *) be + 16 - bytes;
	memcpy(p, in, bytes);
	*p ^= (1 << 8) >> shift;

	out[0] = be64_to_cpu(be[0]);
	out[1] = be64_to_cpu(be[1]);
	*out_bits = out[0] ? 64 + fls64(out[0]) : fls64(out[1]);

	return bytes;
}

// 备注：bch2_inode_pack_inlined - 将未打包的inode结构打包为btree键值格式（内联版本）
// 备注：@packed:	输出的打包后inode缓冲区
// 备注：@inode:		输入的未打包inode结构
// 备注：
// 备注：【功能说明】
// 备注：
// 备注：这是inode打包的核心函数，将内存中的未打包inode结构转换为btree存储格式。
// 备注：打包过程使用变长编码压缩字段，减少存储空间占用。
// 备注：
// 备注：【打包格式】
// 备注：
// 备注：bcachefs使用v3 inode格式，包含：
// 备注：- 固定头部：inum, mode, flags, size等基本字段
// 备注：- 变长字段区：使用变长整数编码的可选字段
// 备注：- 字段采用稀疏存储，值为0的字段不占用空间
// 备注：
// 备注：【压缩策略】
// 备注：
// 备注：1. 变长整数编码：小数值使用较少字节
// 备注：2. 稀疏存储：跳过后缀的零值字段
// 备注：3. 字段排序：按访问频率排序，常用字段靠前
// 备注：
// 备注：【调试验证】
// 备注：
// 备注：在DEBUG模式下，函数会解包验证，确保打包/解包一致性。
static inline void bch2_inode_pack_inlined(struct bch_fs *c, struct bkey_inode_buf *packed,
					   const struct bch_inode_unpacked *inode)
{
	struct bkey_i_inode_v3 *k = &packed->inode;
	u8 *out = k->v.fields;
	u8 *end = (void *) &packed[1];
	u8 *last_nonzero_field = out;
	unsigned nr_fields = 0, last_nonzero_fieldnr = 0;
	unsigned bytes;
	int ret;

	// 备注：初始化inode v3键值
	bkey_inode_v3_init(&packed->inode.k_i);

	// 备注：填充固定头部字段
	packed->inode.k.p.offset	= inode->bi_inum;
	packed->inode.v.bi_journal_seq	= cpu_to_le64(inode->bi_journal_seq);
	packed->inode.v.bi_hash_seed	= inode->bi_hash_seed;
	packed->inode.v.bi_flags	= cpu_to_le64(inode->bi_flags);
	packed->inode.v.bi_sectors	= cpu_to_le64(inode->bi_sectors);
	packed->inode.v.bi_size		= cpu_to_le64(inode->bi_size);
	packed->inode.v.bi_version	= cpu_to_le64(inode->bi_version);
	SET_INODEv3_MODE(&packed->inode.v, inode->bi_mode);
	SET_INODEv3_FIELDS_START(&packed->inode.v, INODEv3_FIELDS_START_CUR);


#define x(_name, _bits)							\
	nr_fields++;							\
									\
	if (inode->_name) {						\
		ret = bch2_varint_encode_fast(out, inode->_name);	\
		out += ret;						\
									\
		if (_bits > 64)						\
			*out++ = 0;					\
									\
		last_nonzero_field = out;				\
		last_nonzero_fieldnr = nr_fields;			\
	} else {							\
		*out++ = 0;						\
									\
		if (_bits > 64)						\
			*out++ = 0;					\
	}

	BCH_INODE_FIELDS_v3()
#undef  x
	BUG_ON(out > end);

	out = last_nonzero_field;
	nr_fields = last_nonzero_fieldnr;

	bytes = out - (u8 *) &packed->inode.v;
	set_bkey_val_bytes(&packed->inode.k, bytes);
	memset_u64s_tail(&packed->inode.v, 0, bytes);

	SET_INODEv3_NR_FIELDS(&k->v, nr_fields);

	if (IS_ENABLED(CONFIG_BCACHEFS_DEBUG)) {
		struct bch_inode_unpacked unpacked;

		bch2_inode_unpack(c, bkey_i_to_s_c(&packed->inode.k_i), &unpacked);
		BUG_ON(unpacked.bi_inum		!= inode->bi_inum);
		BUG_ON(unpacked.bi_hash_seed	!= inode->bi_hash_seed);
		BUG_ON(unpacked.bi_sectors	!= inode->bi_sectors);
		BUG_ON(unpacked.bi_size		!= inode->bi_size);
		BUG_ON(unpacked.bi_version	!= inode->bi_version);
		BUG_ON(unpacked.bi_mode		!= inode->bi_mode);

#define x(_name, _bits)	if (unpacked._name != inode->_name)		\
			panic("unpacked %llu should be %llu",		\
			      (u64) unpacked._name, (u64) inode->_name);
		BCH_INODE_FIELDS_v3()
#undef  x
	}
}

void bch2_inode_pack(struct bch_fs *c,
		     struct bkey_inode_buf *packed,
		     const struct bch_inode_unpacked *inode)
{
	bch2_inode_pack_inlined(c, packed, inode);
}

noinline
static void bch2_inode_unpack_v2_error(struct bch_fs *c, struct bkey_s_c k,
				       struct bch_inode_unpacked *unpacked,
				       unsigned fieldnr)
{
	/* Zero this field and all that follow — see v3_error for the rationale. */
	unsigned i = 0;
#define x(_name, _bits)					\
	if (i++ >= fieldnr)				\
		unpacked->_name = 0;
	BCH_INODE_FIELDS_v2()
#undef  x

	/*
	 * v2 field indices are NOT the same as v3: v2 has bi_size/bi_sectors
	 * at 4/5, so everything after is shifted by two. Derive the indices
	 * from the x-macro so they can't drift:
	 */
	enum {
#define x(_name, _bits)	BCH_INODE_V2_FIELD_##_name,
		BCH_INODE_FIELDS_v2()
#undef  x
	};

	u64 passes = 0;

	if (fieldnr <= BCH_INODE_V2_FIELD_bi_nlink)
		passes |= BIT_ULL(BCH_RECOVERY_PASS_check_nlinks);
	if (fieldnr <= BCH_INODE_V2_FIELD_bi_subvol)
		passes |= BIT_ULL(BCH_RECOVERY_PASS_check_subvols);
	if (fieldnr <= BCH_INODE_V2_FIELD_bi_parent_subvol)
		passes |= BIT_ULL(BCH_RECOVERY_PASS_check_subvolume_structure);

	CLASS(bch_log_msg, msg)(c);
	msg.m.suppress = true;
	prt_printf(&msg.m, "inode unpack error at field %u in ", fieldnr);
	bch2_bkey_val_to_text(&msg.m, c, k);

	bch2_count_fsck_err(c, inode_unpack_error, &msg.m);

	while (passes) {
		unsigned pass = __ffs64(passes);
		bch2_run_explicit_recovery_pass(c, &msg.m, pass, 0);
		passes &= passes - 1;
	}
}

static void bch2_inode_unpack_v1(struct bch_fs *c, struct bkey_s_c k,
				 struct bkey_s_c_inode inode,
				 struct bch_inode_unpacked *unpacked)
{
	const u8 *in = inode.v->fields;
	const u8 *end = bkey_val_end(inode);
	u64 field[2];
	unsigned fieldnr = 0, field_bits;
	int ret;

#define x(_name, _bits)							\
	if (fieldnr == INODEv1_NR_FIELDS(inode.v)) {			\
		unsigned offset = offsetof(struct bch_inode_unpacked, _name);\
		memset((void *) unpacked + offset, 0,			\
		       sizeof(*unpacked) - offset);			\
		return;							\
	}								\
									\
	ret = inode_decode_field(in, end, field, &field_bits);		\
	if (ret < 0)							\
		return bch2_inode_unpack_v2_error(c, k, unpacked, fieldnr);\
									\
	if (field_bits > sizeof(unpacked->_name) * 8)			\
		return bch2_inode_unpack_v2_error(c, k, unpacked, fieldnr);\
									\
	unpacked->_name = field[1];					\
	in += ret;							\
	fieldnr++;

	BCH_INODE_FIELDS_v2()
#undef  x

	/* XXX: signal if there were more fields than expected? */
}

static void bch2_inode_unpack_v2(struct bch_fs *c, struct bkey_s_c k,
				 struct bch_inode_unpacked *unpacked,
				 const u8 *in, const u8 *end,
				 unsigned nr_fields)
{
	unsigned fieldnr = 0;
	int ret;

#define x(_name, _bits)							\
	if (fieldnr < nr_fields) {					\
		u64 v[2];						\
									\
		ret = bch2_varint_decode_fast(in, end, &v[0]);		\
		if (ret < 0)						\
			return bch2_inode_unpack_v2_error(c, k, unpacked, fieldnr);\
		in += ret;						\
									\
		if (_bits > 64) {					\
			ret = bch2_varint_decode_fast(in, end, &v[1]);	\
			if (ret < 0)					\
				return bch2_inode_unpack_v2_error(c, k, unpacked, fieldnr);\
			in += ret;					\
		} else {						\
			v[1] = 0;					\
		}							\
									\
		unpacked->_name = v[0];					\
		if (v[1] || v[0] != unpacked->_name)			\
			return bch2_inode_unpack_v2_error(c, k, unpacked, fieldnr);\
	} else {							\
		unpacked->_name = 0;					\
	}								\
									\
	fieldnr++;

	BCH_INODE_FIELDS_v2()
#undef  x

	/* XXX: signal if there were more fields than expected? */
}

noinline
static void bch2_inode_unpack_v3_error(struct bch_fs *c, struct bkey_s_c k,
				       struct bch_inode_unpacked *unpacked,
				       unsigned fieldnr)
{
	/*
	 * The fastpath bailed mid-loop; zero this field and all that follow
	 * so the caller always gets a well-defined struct. The hot path stays
	 * zero-cost; only this (rare) error helper pays for the clear.
	 */
	unsigned i = 0;
#define x(_name, _bits)					\
	if (i++ >= fieldnr)				\
		unpacked->_name = 0;
	BCH_INODE_FIELDS_v3()
#undef  x

	/*
	 * Schedule recovery passes for fields whose zero value is NOT a legal
	 * state we can quietly live with. Most fields are lost-is-lost — zero
	 * is either legal (bi_dir/offset for orphaned hardlinks, bi_depth) or
	 * just degrades behavior (timestamps, option overrides).
	 */
	enum {
#define x(_name, _bits)	BCH_INODE_V3_FIELD_##_name,
		BCH_INODE_FIELDS_v3()
#undef  x
	};

	u64 passes = 0;

	if (fieldnr <= BCH_INODE_V3_FIELD_bi_nlink)
		passes |= BIT_ULL(BCH_RECOVERY_PASS_check_nlinks);
	if (fieldnr <= BCH_INODE_V3_FIELD_bi_subvol)
		passes |= BIT_ULL(BCH_RECOVERY_PASS_check_subvols);
	if (fieldnr <= BCH_INODE_V3_FIELD_bi_parent_subvol)
		passes |= BIT_ULL(BCH_RECOVERY_PASS_check_subvolume_structure);
	/* dirent hashes depend on bi_casefold: */
	if (fieldnr <= BCH_INODE_V3_FIELD_bi_casefold)
		passes |= BIT_ULL(BCH_RECOVERY_PASS_check_dirents);

	CLASS(bch_log_msg, msg)(c);
	msg.m.suppress = true;
	prt_printf(&msg.m, "inode unpack error at field %u in ", fieldnr);
	bch2_bkey_val_to_text(&msg.m, c, k);

	bch2_count_fsck_err(c, inode_unpack_error, &msg.m);

	while (passes) {
		unsigned pass = __ffs64(passes);
		bch2_run_explicit_recovery_pass(c, &msg.m, pass, 0);
		passes &= passes - 1;
	}
}

static void bch2_inode_unpack_v3(struct bch_fs *c, struct bkey_s_c k,
				 struct bch_inode_unpacked *unpacked)
{
	struct bkey_s_c_inode_v3 inode = bkey_s_c_to_inode_v3(k);
	const u8 *in = inode.v->fields;
	const u8 *end = bkey_val_end(inode);
	unsigned nr_fields = INODEv3_NR_FIELDS(inode.v);
	unsigned fieldnr = 0;
	int ret;

	unpacked->bi_inum	= inode.k->p.offset;
	unpacked->bi_snapshot	= inode.k->p.snapshot;
	unpacked->bi_journal_seq= le64_to_cpu(inode.v->bi_journal_seq);
	unpacked->bi_hash_seed	= inode.v->bi_hash_seed;
	unpacked->bi_flags	= le64_to_cpu(inode.v->bi_flags);
	unpacked->bi_sectors	= le64_to_cpu(inode.v->bi_sectors);
	unpacked->bi_size	= le64_to_cpu(inode.v->bi_size);
	unpacked->bi_version	= le64_to_cpu(inode.v->bi_version);
	unpacked->bi_mode	= INODEv3_MODE(inode.v);

#define x(_name, _bits)							\
	if (fieldnr < nr_fields) {					\
		u64 v[2];						\
									\
		ret = bch2_varint_decode_fast(in, end, &v[0]);		\
		if (ret < 0)						\
			return bch2_inode_unpack_v3_error(c, k, unpacked, fieldnr);\
		in += ret;						\
									\
		if (_bits > 64) {					\
			ret = bch2_varint_decode_fast(in, end, &v[1]);	\
			if (ret < 0)					\
				return bch2_inode_unpack_v3_error(c, k, unpacked, fieldnr);\
			in += ret;					\
		} else {						\
			v[1] = 0;					\
		}							\
									\
		unpacked->_name = v[0];					\
		if (v[1] || v[0] != unpacked->_name)			\
			return bch2_inode_unpack_v3_error(c, k, unpacked, fieldnr);\
	} else {							\
		unpacked->_name = 0;					\
	}								\
									\
	fieldnr++;

	BCH_INODE_FIELDS_v3()
#undef  x

	/* XXX: signal if there were more fields than expected? */
}

static noinline void bch2_inode_unpack_slowpath(struct bch_fs *c, struct bkey_s_c k,
						struct bch_inode_unpacked *unpacked)
{
	memset(unpacked, 0, sizeof(*unpacked));

	switch (k.k->type) {
	case KEY_TYPE_inode: {
		struct bkey_s_c_inode inode = bkey_s_c_to_inode(k);

		unpacked->bi_inum	= inode.k->p.offset;
		unpacked->bi_snapshot	= inode.k->p.snapshot;
		unpacked->bi_journal_seq= 0;
		unpacked->bi_hash_seed	= inode.v->bi_hash_seed;
		unpacked->bi_flags	= le32_to_cpu(inode.v->bi_flags);
		unpacked->bi_mode	= le16_to_cpu(inode.v->bi_mode);

		if (INODEv1_NEW_VARINT(inode.v))
			bch2_inode_unpack_v2(c, k, unpacked, inode.v->fields,
					     bkey_val_end(inode),
					     INODEv1_NR_FIELDS(inode.v));
		else
			bch2_inode_unpack_v1(c, k, inode, unpacked);
		break;
	}
	case KEY_TYPE_inode_v2: {
		struct bkey_s_c_inode_v2 inode = bkey_s_c_to_inode_v2(k);

		unpacked->bi_inum	= inode.k->p.offset;
		unpacked->bi_snapshot	= inode.k->p.snapshot;
		unpacked->bi_journal_seq= le64_to_cpu(inode.v->bi_journal_seq);
		unpacked->bi_hash_seed	= inode.v->bi_hash_seed;
		unpacked->bi_flags	= le64_to_cpu(inode.v->bi_flags);
		unpacked->bi_mode	= le16_to_cpu(inode.v->bi_mode);

		bch2_inode_unpack_v2(c, k, unpacked, inode.v->fields,
				     bkey_val_end(inode),
				     INODEv2_NR_FIELDS(inode.v));
		break;
	}
	default:
		BUG();
	}
}

// 备注：key 拆包给赋值 unpacked
void bch2_inode_unpack(struct bch_fs *c, struct bkey_s_c k,
		       struct bch_inode_unpacked *unpacked)
{
	if (likely(k.k->type == KEY_TYPE_inode_v3))
		bch2_inode_unpack_v3(c, k, unpacked);
	else
		bch2_inode_unpack_slowpath(c, k, unpacked);
}

// 备注：__bch2_inode_peek_snapshot - 在btree中查找并读取inode（内部函数）
// 备注：@trans:	btree事务
// 备注：@iter:		输出参数，指向inode的迭代器
// 备注：@inode:		输出的inode结构（未打包格式）
// 备注：@inum:		要查找的inode编号和子卷
// 备注：@flags:		迭代器标志
// 备注：@warn:		警告信息前缀（可为NULL）
// 备注：
// 备注：【功能说明】
// 备注：
// 备注：这是inode查找的核心内部函数，负责从inodes btree中读取指定inode的信息。
// 备注：函数处理子卷快照映射、迭代器初始化和inode解包。
// 备注：
// 备注：【查找流程】
// 备注：
// 备注：1. 获取子卷的快照ID（用于COW隔离）
// 备注：2. 初始化inodes btree迭代器，定位到指定位置
// 备注：3. 读取btree槽位的键值
// 备注：4. 验证键值类型是否为inode
// 备注：5. 解包inode数据到未打包结构
// 备注：
// 备注：【缓存优化】
// 备注：
// 备注：使用BTREE_ITER_cached标志，利用btree的键缓存机制，
// 备注：加速频繁访问的inode查找。
// 备注：
// 备注：【错误处理】
// 备注：
// 备注：- ENOENT_inode: inode不存在或类型不匹配
// 备注：- 其他错误: btree遍历或解包错误
// 备注：- 如提供warn参数，错误时会打印详细信息
int __bch2_inode_peek_snapshot(struct btree_trans *trans,
			       struct btree_iter *iter,
			       struct bch_inode_unpacked *inode,
			       subvol_inum inum, u32 snapshot,
			       unsigned flags, const char *warn)
{
	bch2_trans_iter_init(trans, iter, BTREE_ID_inodes, SPOS(0, inum.inum, snapshot),
			     flags|BTREE_ITER_cached);

	// 备注：读取btree槽位的键值
	struct bkey_s_c k = bch2_btree_iter_peek_slot(iter);
	int ret = bkey_err(k);
	if (ret)
		goto err;

	// 备注：验证键值类型是否为inode
	ret = bkey_is_inode(k.k) ? 0 : bch_err_throw(trans->c, ENOENT_inode);
	if (ret)
		goto err;

	// 备注：解包inode数据到未打包结构
	bch2_inode_unpack(trans->c, k, inode);
	return 0;
err:
	// 备注：如提供了警告前缀，打印详细的错误信息
	if (warn)
		bch_err_msg(trans->c, ret, "%s(): looking up inum %llu:%llu:",
			    warn, inum.subvol, inum.inum);
	return ret;
}

int __bch2_inode_peek(struct btree_trans *trans,
		      struct btree_iter *iter,
		      struct bch_inode_unpacked *inode,
		      subvol_inum inum, unsigned flags,
		      const char *warn)
{
	u32 snapshot;
	try(__bch2_subvolume_get_snapshot(trans, inum.subvol, &snapshot, warn));
	return __bch2_inode_peek_snapshot(trans, iter, inode, inum, snapshot, flags, warn);
}

int bch2_inode_find_by_inum_snapshot(struct btree_trans *trans,
				     u64 inode_nr, u32 snapshot,
				     struct bch_inode_unpacked *inode,
				     unsigned flags)
{
	CLASS(btree_iter, iter)(trans, BTREE_ID_inodes,
				SPOS(0, inode_nr, snapshot), flags);
	struct bkey_s_c k = bkey_try(bch2_btree_iter_peek_slot(&iter));

	if (!bkey_is_inode(k.k))
		return bch_err_throw(trans->c, ENOENT_inode);
	bch2_inode_unpack(trans->c, k, inode);
	return 0;
}

int bch2_inode_find_by_inum_snapshot2(struct btree_trans *trans,
				      subvol_inum inum, u32 snapshot,
				      struct bch_inode_unpacked *inode,
				      unsigned flags,
				      const char *warn)
{
	CLASS(btree_iter_uninit, iter)(trans);
	return __bch2_inode_peek_snapshot(trans, &iter, inode, inum, snapshot, 0, warn);
}

int __bch2_inode_find_by_inum_trans(struct btree_trans *trans,
				    subvol_inum inum,
				    struct bch_inode_unpacked *inode,
				    const char *warn)
{
	CLASS(btree_iter_uninit, iter)(trans);
	return __bch2_inode_peek(trans, &iter, inode, inum, 0, warn);
}

// 备注：bch2_inode_find_by_inum - 通过inode编号查找inode（便捷接口）
// 备注：@c:		文件系统实例
// 备注：@inum:		inode编号和子卷
// 备注：@inode:		输出的inode结构（未打包格式）
// 备注：
// 备注：【功能说明】
// 备注：
// 备注：这是查找inode的便捷接口函数，自动创建事务并处理事务重启。
// 备注：适用于简单的inode查找场景，不需要手动管理事务。
// 备注：
// 备注：【使用场景】
// 备注：
// 备注：- 文件系统操作前的inode验证
// 备注：- 获取inode元数据（大小、权限、时间戳等）
// 备注：- 路径解析过程中的inode查找
// 备注：
// 备注：【事务管理】
// 备注：
// 备注：函数自动创建临时事务，使用lockrestart_do宏处理可能的事务重启。
// 备注：调用者无需关心事务的生命周期管理。
// 备注：
// 备注：【性能考虑】
// 备注：
// 备注：对于频繁的inode查找，建议在调用者的事务中使用__bch2_inode_peek，
// 备注：避免重复创建事务的开销。
// 备注：
// 备注：【返回值】
// 备注：
// 备注：- 0: 成功，inode结构填充完成
// 备注：- -ENOENT_inode: 指定的inode不存在
// 备注：- 其他负值: 错误码
int bch2_inode_find_by_inum(struct bch_fs *c, subvol_inum inum,
			    struct bch_inode_unpacked *inode)
{
	// 备注：创建临时事务
	CLASS(btree_trans, trans)(c);

	// 备注：使用lockrestart_do自动处理事务重启
	return lockrestart_do(trans, bch2_inode_find_by_inum_trans(trans, inum, inode));
}

int bch2_inode_find_oldest_snapshot(struct btree_trans *trans, u64 inum, u32 snapshot,
				    struct bch_inode_unpacked *root)
{
	struct bkey_s_c k;
	int ret;

	for_each_btree_key_norestart(trans, iter, BTREE_ID_inodes,
				     SPOS(0, inum, snapshot),
				     BTREE_ITER_all_snapshots, k, ret) {
		if (k.k->p.offset != inum)
			break;
		if (!bkey_is_inode(k.k) ||
		    !bch2_snapshot_is_ancestor(trans, snapshot, k.k->p.snapshot))
			continue;
		bch2_inode_unpack(trans->c, k, root);
		return 0;
	}

	return ret ?: bch_err_throw(trans->c, ENOENT_inode);
}

// 备注：bch2_inode_write_flags - 将inode写回btree（带标志）
// 备注：@trans:	btree事务
// 备注：@iter:		指向inode的btree迭代器
// 备注：@inode:		要写入的inode（未打包格式）
// 备注：@flags:		更新标志
// 备注：
// 备注：【功能说明】
// 备注：
// 备注：这是inode更新的核心函数，将内存中的inode结构打包并写入btree。
// 备注：支持触发器标志控制，用于不同的更新场景。
// 备注：
// 备注：【打包流程】
// 备注：
// 备注：1. 在事务内存中分配inode缓冲区
// 备注：2. 将未打包的inode结构打包为btree键值格式
// 备注：3. 设置快照ID（继承自迭代器）
// 备注：4. 提交到事务更新队列
// 备注：
// 备注：【触发器处理】
// 备注：
// 备注：根据flags参数，可能触发：
// 备注：- 磁盘配额更新
// 备注：- 统计信息更新
// 备注：- 索引节点计数更新
// 备注：
// 备注：【使用场景】
// 备注：
// 备注：- 修改inode元数据后写回
// 备注：- 更新文件大小、时间戳
// 备注：- 设置inode标志
// 备注：
// 备注：【注意】
// 备注：
// 备注：此函数不立即提交事务，只是将更新加入事务队列。
// 备注：需要调用bch2_trans_commit()才能真正持久化。
int bch2_inode_write_flags(struct btree_trans *trans,
		     struct btree_iter *iter,
		     struct bch_inode_unpacked *inode,
		     enum btree_iter_update_trigger_flags flags)
{
	// 备注：在事务内存中分配inode键值缓冲区
	struct bkey_inode_buf *inode_p = errptr_try(bch2_trans_kmalloc(trans, sizeof(*inode_p)));

	// 备注：将未打包的inode打包为btree键值格式
	bch2_inode_pack_inlined(trans->c, inode_p, inode);
	// 备注：设置键值的快照ID（用于COW隔离）
	inode_p->inode.k.p.snapshot = iter->snapshot;

	// 备注：提交到事务更新队列
	return bch2_trans_update(trans, iter, &inode_p->inode.k_i, flags);
}

int __bch2_fsck_write_inode(struct btree_trans *trans, struct bch_inode_unpacked *inode)
{
	struct bkey_inode_buf *inode_p = errptr_try(bch2_trans_kmalloc(trans, sizeof(*inode_p)));

	bch2_inode_pack(trans->c, inode_p, inode);
	inode_p->inode.k.p.snapshot = inode->bi_snapshot;

	return bch2_btree_insert_trans(trans, BTREE_ID_inodes,
				       &inode_p->inode.k_i,
				       BTREE_ITER_cached|
				       BTREE_UPDATE_internal_snapshot_node);
}

int bch2_fsck_write_inode(struct btree_trans *trans, struct bch_inode_unpacked *inode)
{
	int ret = commit_do(trans, NULL, NULL, BCH_TRANS_COMMIT_no_enospc,
			    __bch2_fsck_write_inode(trans, inode));
	bch_err_fn(trans->c, ret);
	return ret;
}

struct bkey_i *bch2_inode_to_v3(struct btree_trans *trans, struct bkey_i *k)
{
	if (!bkey_is_inode(&k->k))
		return ERR_PTR(-ENOENT);

	struct bkey_inode_buf *inode_p = bch2_trans_kmalloc(trans, sizeof(*inode_p));
	if (IS_ERR(inode_p))
		return ERR_CAST(inode_p);

	struct bch_inode_unpacked u;
	bch2_inode_unpack(trans->c, bkey_i_to_s_c(k), &u);

	bch2_inode_pack(trans->c, inode_p, &u);
	return &inode_p->inode.k_i;
}

static int __bch2_inode_validate(struct bch_fs *c, struct bkey_s_c k,
				 const struct bkey_validate_context *from)
{
	int ret = 0;

	bkey_fsck_err_on(k.k->p.inode,
			 c, inode_pos_inode_nonzero,
			 "nonzero k.p.inode");

	bkey_fsck_err_on(k.k->p.offset < BLOCKDEV_INODE_MAX,
			 c, inode_pos_blockdev_range,
			 "fs inode in blockdev range");
fsck_err:
	return ret;
}

int bch2_inode_validate(struct bch_fs *c, struct bkey_s_c k,
			const struct bkey_validate_context *from)
{
	struct bkey_s_c_inode inode = bkey_s_c_to_inode(k);
	int ret = 0;

	bkey_fsck_err_on(INODEv1_STR_HASH(inode.v) >= BCH_STR_HASH_NR,
			 c, inode_str_hash_invalid,
			 "invalid str hash type (%llu >= %u)",
			 INODEv1_STR_HASH(inode.v), BCH_STR_HASH_NR);

	ret = __bch2_inode_validate(c, k, from);
fsck_err:
	return ret;
}

int bch2_inode_v2_validate(struct bch_fs *c, struct bkey_s_c k,
			   const struct bkey_validate_context *from)
{
	struct bkey_s_c_inode_v2 inode = bkey_s_c_to_inode_v2(k);
	int ret = 0;

	bkey_fsck_err_on(INODEv2_STR_HASH(inode.v) >= BCH_STR_HASH_NR,
			 c, inode_str_hash_invalid,
			 "invalid str hash type (%llu >= %u)",
			 INODEv2_STR_HASH(inode.v), BCH_STR_HASH_NR);

	ret = __bch2_inode_validate(c, k, from);
fsck_err:
	return ret;
}

int bch2_inode_v3_validate(struct bch_fs *c, struct bkey_s_c k,
			   const struct bkey_validate_context *from)
{
	struct bkey_s_c_inode_v3 inode = bkey_s_c_to_inode_v3(k);
	int ret = 0;

	bkey_fsck_err_on(INODEv3_FIELDS_START(inode.v) < INODEv3_FIELDS_START_INITIAL ||
			 INODEv3_FIELDS_START(inode.v) > bkey_val_u64s(inode.k),
			 c, inode_v3_fields_start_bad,
			 "invalid fields_start (got %llu, min %u max %zu)",
			 INODEv3_FIELDS_START(inode.v),
			 INODEv3_FIELDS_START_INITIAL,
			 bkey_val_u64s(inode.k));

	bkey_fsck_err_on(INODEv3_STR_HASH(inode.v) >= BCH_STR_HASH_NR,
			 c, inode_str_hash_invalid,
			 "invalid str hash type (%llu >= %u)",
			 INODEv3_STR_HASH(inode.v), BCH_STR_HASH_NR);

	ret = __bch2_inode_validate(c, k, from);
fsck_err:
	return ret;
}

static __cold void __bch2_inode_unpacked_to_text(struct printbuf *out,
					  struct bch_inode_unpacked *inode)
{
	prt_newline(out);

	unsigned type = mode_to_type(inode->bi_mode);

	prt_printf(out, "mode=%o (%s)\n", inode->bi_mode,
		   type < BCH_DT_MAX ? bch2_d_types[type] : "unknown");

	prt_str(out, "flags=");
	prt_bitflags(out, bch2_inode_flag_strs, inode->bi_flags & ((1U << 20) - 1));
	prt_printf(out, "(%x)\n", inode->bi_flags);

	prt_printf(out, "journal_seq=%llu\n",	inode->bi_journal_seq);
	prt_printf(out, "hash_seed=%llx\n",	inode->bi_hash_seed);
	prt_printf(out, "hash_type=");
	bch2_prt_str_hash_type(out, INODE_STR_HASH(inode));
	prt_newline(out);
	prt_printf(out, "bi_size=%llu\n",	inode->bi_size);
	prt_printf(out, "bi_sectors=%llu\n",	inode->bi_sectors);
	prt_printf(out, "bi_version=%llu\n",	inode->bi_version);

#define x(_name, _bits)						\
	prt_printf(out, #_name "=%llu\n", (u64) inode->_name);
	BCH_INODE_FIELDS_v3()
#undef  x
}

__cold void bch2_inode_unpacked_to_text(struct printbuf *out, struct bch_inode_unpacked *inode)
{
	prt_printf(out, "inum: %llu:%u ", inode->bi_inum, inode->bi_snapshot);
	guard(printbuf_indent)(out);
	__bch2_inode_unpacked_to_text(out, inode);
}

__cold void bch2_inode_to_text(struct printbuf *out, struct bch_fs *c, struct bkey_s_c k)
{
	struct bch_inode_unpacked inode;

	bch2_inode_unpack(c, k, &inode);
	__bch2_inode_unpacked_to_text(out, &inode);
}

static inline u64 bkey_inode_flags(struct bkey_s_c k)
{
	switch (k.k->type) {
	case KEY_TYPE_inode:
		return le32_to_cpu(bkey_s_c_to_inode(k).v->bi_flags);
	case KEY_TYPE_inode_v2:
		return le64_to_cpu(bkey_s_c_to_inode_v2(k).v->bi_flags);
	case KEY_TYPE_inode_v3:
		return le64_to_cpu(bkey_s_c_to_inode_v3(k).v->bi_flags);
	default:
		return 0;
	}
}

static inline void bkey_inode_flags_set(struct bkey_s k, u64 f)
{
	switch (k.k->type) {
	case KEY_TYPE_inode:
		bkey_s_to_inode(k).v->bi_flags = cpu_to_le32(f);
		return;
	case KEY_TYPE_inode_v2:
		bkey_s_to_inode_v2(k).v->bi_flags = cpu_to_le64(f);
		return;
	case KEY_TYPE_inode_v3:
		bkey_s_to_inode_v3(k).v->bi_flags = cpu_to_le64(f);
		return;
	default:
		BUG();
	}
}

static inline bool bkey_is_unlinked_inode(struct bkey_s_c k)
{
	unsigned f = bkey_inode_flags(k);

	return (f & BCH_INODE_unlinked) && !(f & BCH_INODE_has_child_snapshot);
}

static struct bkey_s_c
bch2_bkey_get_iter_snapshot_parent(struct btree_trans *trans, struct btree_iter *iter,
				   enum btree_id btree, struct bpos pos)
{
	struct bkey_s_c k;
	int ret = 0;

	bch2_trans_iter_init(trans, iter, btree, bpos_successor(pos),
			     BTREE_ITER_all_snapshots);

	for_each_btree_key_max_continue_norestart(*iter, SPOS(pos.inode, pos.offset, U32_MAX),
						  BTREE_ITER_all_snapshots, k, ret)
		if (bch2_snapshot_is_ancestor(trans, pos.snapshot, k.k->p.snapshot))
			return k;

	return ret ? bkey_s_c_err(ret) : bkey_s_c_null;
}

static struct bkey_s_c
bch2_inode_get_iter_snapshot_parent(struct btree_trans *trans, struct btree_iter *iter,
				    struct bpos pos)
{
	while (1) {
		struct bkey_s_c k =
			bch2_bkey_get_iter_snapshot_parent(trans, iter, BTREE_ID_inodes, pos);
		if (!k.k ||
		    bkey_err(k) ||
		    bkey_is_inode(k.k))
			return k;

		pos = k.k->p;
	}
}

int __bch2_inode_has_child_snapshots(struct btree_trans *trans, struct bpos pos)
{
	struct bkey_s_c k;
	int ret = 0;

	for_each_btree_key_max_norestart(trans, iter,
			BTREE_ID_inodes, POS(0, pos.offset), bpos_predecessor(pos),
			BTREE_ITER_all_snapshots, k, ret)
		if (bch2_snapshot_is_ancestor(trans, k.k->p.snapshot, pos.snapshot) &&
		    bkey_is_inode(k.k)) {
			ret = 1;
			break;
		}
	return ret;
}

static int update_inode_has_children(struct btree_trans *trans,
				     struct bkey_s k,
				     bool have_child)
{
	if (!have_child) {
		int ret = bch2_inode_has_child_snapshots(trans, k.k->p);
		if (ret)
			return ret < 0 ? ret : 0;
	}

	u64 f = bkey_inode_flags(k.s_c);
	if (have_child != !!(f & BCH_INODE_has_child_snapshot))
		bkey_inode_flags_set(k, f ^ BCH_INODE_has_child_snapshot);

	return 0;
}

static int update_parent_inode_has_children(struct btree_trans *trans, struct bpos pos,
					    bool have_child)
{
	CLASS(btree_iter_uninit, iter)(trans);
	struct bkey_s_c k = bkey_try(bch2_inode_get_iter_snapshot_parent(trans, &iter, pos));
	if (!k.k)
		return 0;

	if (!have_child) {
		int ret = bch2_inode_has_child_snapshots(trans, k.k->p);
		if (ret)
			return ret < 0 ? ret : 0;
	}

	u64 f = bkey_inode_flags(k);
	if (have_child != !!(f & BCH_INODE_has_child_snapshot)) {
		struct bkey_i *update = errptr_try(bch2_bkey_make_mut(trans, &iter, &k,
					     BTREE_UPDATE_internal_snapshot_node));

		bkey_inode_flags_set(bkey_i_to_s(update), f ^ BCH_INODE_has_child_snapshot);
	}

	return 0;
}

int bch2_trigger_inode(struct btree_trans *trans, struct btree_trigger_op op)
{
	struct bch_fs *c = trans->c;

	if ((op.flags & BTREE_TRIGGER_atomic) && (op.flags & BTREE_TRIGGER_insert)) {
		BUG_ON(!trans->journal_res.seq);
		bkey_s_to_inode_v3(op.new).v->bi_journal_seq = cpu_to_le64(trans->journal_res.seq);
	}

	s64 nr[1] = { bkey_is_inode(op.new.k) - bkey_is_inode(op.old.k) };
	if ((op.flags & (BTREE_TRIGGER_transactional|BTREE_TRIGGER_gc)) && nr[0]) {
		try(bch2_disk_accounting_mod2(trans, op.flags & BTREE_TRIGGER_gc, nr, nr_inodes));
	}

	if (op.flags & BTREE_TRIGGER_transactional) {
		int unlinked_delta =	(int) bkey_is_unlinked_inode(op.new.s_c) -
					(int) bkey_is_unlinked_inode(op.old);
		if (unlinked_delta) {
			try(bch2_btree_bit_mod_buffered(trans, BTREE_ID_deleted_inodes,
							op.new.k->p, unlinked_delta > 0));
		}

		/*
		 * If we're creating or deleting an inode at this snapshot ID,
		 * and there might be an inode in a parent snapshot ID, we might
		 * need to set or clear the has_child_snapshot flag on the
		 * parent.
		 */
		int deleted_delta = (int) bkey_is_inode(op.new.k) -
				    (int) bkey_is_inode(op.old.k);
		if (deleted_delta &&
		    bch2_snapshot_parent(c, op.new.k->p.snapshot))
			try(update_parent_inode_has_children(trans, op.new.k->p, deleted_delta > 0));

		/*
		 * When an inode is first updated in a new snapshot, we may need
		 * to clear has_child_snapshot
		 */
		if (deleted_delta > 0)
			try(update_inode_has_children(trans, op.new, false));
	}

	return 0;
}

int bch2_inode_generation_validate(struct bch_fs *c, struct bkey_s_c k,
				   const struct bkey_validate_context *from)
{
	int ret = 0;

	bkey_fsck_err_on(k.k->p.inode,
			 c, inode_pos_inode_nonzero,
			 "nonzero k.p.inode");
fsck_err:
	return ret;
}

__cold void bch2_inode_generation_to_text(struct printbuf *out, struct bch_fs *c,
				   struct bkey_s_c k)
{
	struct bkey_s_c_inode_generation gen = bkey_s_c_to_inode_generation(k);

	prt_printf(out, "generation: %u", le32_to_cpu(gen.v->bi_generation));
}

// 备注：bch_inode 初始化
void bch2_inode_init_early(struct bch_fs *c,
			   struct bch_inode_unpacked *inode_u)
{
	enum bch_str_hash_type str_hash =
		bch2_str_hash_opt_to_type(c, c->opts.str_hash);

	// 备注：内存初始化
	memset(inode_u, 0, sizeof(*inode_u));

	SET_INODE_STR_HASH(inode_u, str_hash);
	get_random_bytes(&inode_u->bi_hash_seed, sizeof(inode_u->bi_hash_seed));
}

// 备注：初始化 inode 数据包
void bch2_inode_init_late(struct bch_fs *c,
			  struct bch_inode_unpacked *inode_u, u64 now,
			  uid_t uid, gid_t gid, umode_t mode, dev_t rdev,
			  struct bch_inode_unpacked *parent)
{
	inode_u->bi_mode	= mode;
	inode_u->bi_uid		= uid;
	inode_u->bi_gid		= gid;
	inode_u->bi_dev		= rdev;
	inode_u->bi_atime	= now;
	inode_u->bi_mtime	= now;
	inode_u->bi_ctime	= now;
	inode_u->bi_otime	= now;

	if (parent && parent->bi_mode & S_ISGID) {
		inode_u->bi_gid = parent->bi_gid;
		if (S_ISDIR(mode))
			inode_u->bi_mode |= S_ISGID;
	}

	if (parent) {
#define x(_name, ...)	inode_u->bi_##_name = parent->bi_##_name;
		BCH_INODE_OPTS()
#undef x
	}

	if (!S_ISDIR(mode))
		inode_u->bi_casefold = 0;

	if (bch2_inode_casefold(c, inode_u))
		inode_u->bi_flags |= BCH_INODE_has_case_insensitive;
}

void bch2_inode_init(struct bch_fs *c, struct bch_inode_unpacked *inode_u,
		     uid_t uid, gid_t gid, umode_t mode, dev_t rdev,
		     struct bch_inode_unpacked *parent)
{
	bch2_inode_init_early(c, inode_u);
	bch2_inode_init_late(c, inode_u, bch2_current_time(c),
			     uid, gid, mode, rdev, parent);
}

int bch2_inode_alloc_cursor_validate(struct bch_fs *c, struct bkey_s_c k,
				   const struct bkey_validate_context *from)
{
	int ret = 0;

	bkey_fsck_err_on(k.k->p.inode != LOGGED_OPS_INUM_inode_cursors,
			 c, inode_alloc_cursor_inode_bad,
			 "k.p.inode bad");
fsck_err:
	return ret;
}

/*
 * Populate inode_shard_cpu[]: each shard gets a preferred CPU,
 * spread across online CPUs with NUMA topology awareness.
 * bch2_trans_cpu_hint() reads this on every trans_begin to nudge the
 * scheduler toward the CPU that owns the task's shard's working set.
 */
void bch2_fs_inode_shard_cpu_init(struct bch_fs *c)
{
#ifdef __KERNEL__
	unsigned bits = c->opts.shard_inode_numbers_bits;
	if (!bits)
		return;

	unsigned nr_shards = 1U << bits;
	BUG_ON(nr_shards > ARRAY_SIZE(c->inode_shard_cpu));

	for (unsigned shard = 0; shard < nr_shards; shard++) {
		unsigned cpu = cpumask_local_spread(shard, NUMA_NO_NODE);
		c->inode_shard_cpu[shard] = cpu < nr_cpu_ids ? cpu : 0;
	}
#endif
}

/*
 * Default for shard_inode_numbers_bits when the user didn't set it.
 *
 * Scales with nr_cpus (x2, for thread-oversubscription headroom), capped so the
 * pre-split shard-boundary metadata (4 sharded btrees, one node per shard)
 * stays under 1% of the filesystem, and clamped to [0, 8] - the range the
 * option and the inode_shard_cpu[] array support.
 *
 * Shared so the format-time default (userspace) and the kernel sb_validate
 * rewrite of legacy bits=0 filesystems pick the same value from one place.
 */
unsigned bch2_shard_inode_numbers_bits_default(unsigned nr_cpus,
					       u64 fs_size,
					       u64 btree_node_bytes)
{
	unsigned cpu_bits = ilog2(roundup_pow_of_two(max(nr_cpus, 1U) * 2));

	/*
	 * 2^bits * 4 * btree_node_bytes <= fs_size / 100
	 *   =>  2^bits <= fs_size / (400 * btree_node_bytes)
	 */
	u64 denom = 400ULL * btree_node_bytes;
	unsigned size_bits = btree_node_bytes && fs_size >= denom
		? ilog2(fs_size / denom)
		: 0;

	return min(min(cpu_bits, size_bits), 8U);
}

static inline void cursor_idx_min_max(struct bch_fs *c, unsigned idx,
				      u64 *min, u64 *max)
{
	if (!idx) {
		*min = BLOCKDEV_INODE_MAX;
		*max = INT_MAX;
	} else {
		u64 shard = idx - 1;
		unsigned bits = 63 - c->opts.shard_inode_numbers_bits;

		*min = max(shard << bits, (u64) INT_MAX + 1);
		*max = (shard << bits) | ~(ULLONG_MAX << bits);
	}
}

__cold
void bch2_inode_alloc_cursor_to_text(struct printbuf *out, struct bch_fs *c,
				     struct bkey_s_c k)
{
	struct bkey_s_c_inode_alloc_cursor i = bkey_s_c_to_inode_alloc_cursor(k);

	u64 min, max, idx = le64_to_cpu(i.v->idx);
	cursor_idx_min_max(c, k.k->p.offset, &min, &max);

	prt_printf(out, "min %llu max %llu consumed %llu idx %llu generation %llu",
		   min, max, idx - min, idx, le64_to_cpu(i.v->gen));
}

static struct bkey_i_inode_alloc_cursor *
bch2_inode_alloc_cursor_get(struct btree_trans *trans, u64 *min, u64 *max,
			    bool is_32bit)
{
	struct bch_fs *c = trans->c;

	u64 cursor_idx = is_32bit ? 0 : bch2_inode_shard_idx(c) + 1;

	cursor_idx_min_max(c, cursor_idx, min, max);

	CLASS(btree_iter, iter)(trans, BTREE_ID_logged_ops,
				POS(LOGGED_OPS_INUM_inode_cursors, cursor_idx),
				BTREE_ITER_intent|BTREE_ITER_cached);
	struct bkey_s_c k = bch2_btree_iter_peek_slot(&iter);
	int ret = bkey_err(k);
	if (ret)
		return ERR_PTR(ret);

	struct bkey_i_inode_alloc_cursor *cursor =
		k.k->type == KEY_TYPE_inode_alloc_cursor
		? bch2_bkey_make_mut_typed(trans, &iter, &k, 0, inode_alloc_cursor)
		: bch2_bkey_alloc(trans, &iter, 0, inode_alloc_cursor);
	if (IS_ERR(cursor))
		return cursor;

	cursor->v.bits = c->opts.shard_inode_numbers_bits;

	if (le64_to_cpu(cursor->v.idx) < *min)
		cursor->v.idx = cpu_to_le64(*min);

	if (le64_to_cpu(cursor->v.idx) >= *max) {
		cursor->v.idx = cpu_to_le64(*min);
		le32_add_cpu(&cursor->v.gen, 1);
	}

	return cursor;
}

/*
 * This just finds an empty slot:
 */
// 备注：bch2_inode_create - 在btree中分配新的inode号
// 备注：@trans:	btree事务
// 备注：@iter:		输出参数，指向新inode位置的迭代器
// 备注：@inode_u:	要填充的inode结构（输出）
// 备注：@snapshot:	快照ID（用于COW）
// 备注：@cpu:		CPU编号（用于inode分片）
// 备注：@is_32bit:	是否使用32位inode号
// 备注：
// 备注：【功能说明】
// 备注：
// 备注：这是inode分配的核心函数，负责在inodes btree中查找并分配一个未使用的inode号。
// 备注：使用分片策略避免多CPU并发分配时的冲突。
// 备注：
// 备注：【分配策略】
// 备注：
// 备注：1. 分片分配：
// 备注：- 根据cpu参数将inode号空间划分为多个片段
// 备注：- 每个CPU从其指定范围内分配inode号
// 备注：- 减少多CPU并发时的锁竞争
// 备注：
// 备注：2. 世代号管理：
// 备注：- 使用inode_generation btree记录每个inode号的世代号
// 备注：- 当inode号回绕时，增加世代号
// 备注：- 防止新创建的inode与已删除的inode混淆
// 备注：
// 备注：3. 查找空位：
// 备注：- 遍历inodes btree查找空槽位
// 备注：- 考虑快照祖先关系（同快照的不同版本）
// 备注：- 跳过已分配的inode号
// 备注：
// 备注：【参数说明】
// 备注：
// 备注：@cpu: 用于计算分片范围，通常为当前CPU编号
// 备注：@is_32bit: 如为true，分配32位范围内的inode号（兼容性模式）
// 备注：@snapshot: 新inode所属的快照，用于COW隔离
// 备注：
// 备注：【返回值】
// 备注：
// 备注：- 0: 成功，iter指向新inode的位置，inode_u填充了inode号
// 备注：- -ENOSPC: inode号耗尽
// 备注：- 其他负值: 错误码
int bch2_inode_create(struct btree_trans *trans,
		      struct btree_iter *iter,
		      struct bch_inode_unpacked *inode_u,
		      u32 snapshot, bool is_32bit)
{
	u64 min, max;
	// 备注：获取或创建inode分配游标，确定分配范围
	struct bkey_i_inode_alloc_cursor *cursor =
		errptr_try(bch2_inode_alloc_cursor_get(trans, &min, &max, is_32bit));

	// 备注：从游标记录的当前位置开始分配
	u64 start = le64_to_cpu(cursor->v.idx);
	u64 pos = start;

	// 备注：初始化迭代器，遍历inodes btree查找空槽位
	bch2_trans_iter_init(trans, iter, BTREE_ID_inodes, POS(0, pos),
			     BTREE_ITER_all_snapshots|
			     BTREE_ITER_intent);
	// 备注：外层循环：处理inode号回绕的情况
	while (1) {
		while (pos < max) {
			// 备注：内层循环：在当前范围内查找可用的inode号
			struct bkey_s_c k = bkey_try(bch2_btree_iter_peek_max(iter, &SPOS(0, pos, U32_MAX)));

			// 备注：检查是否是当前快照的祖先快照中的inode_generation记录
			// 备注：这用于获取该inode号的最新世代号
			if (!k.k ||
			    (k.k->type == KEY_TYPE_inode_generation &&
			     bch2_snapshot_is_ancestor(trans, snapshot, k.k->p.snapshot))) {
				inode_u->bi_inum	= pos;
				inode_u->bi_generation	= le64_to_cpu(cursor->v.gen);
				cursor->v.idx		= cpu_to_le64(pos + 1);

				if (k.k)
					inode_u->bi_generation = max(inode_u->bi_generation,
							le32_to_cpu(bkey_s_c_to_inode_generation(k).v->bi_generation));

				// 备注：设置迭代器到目标位置（指定快照）
				bch2_btree_iter_set_pos(iter, SPOS(0, pos, snapshot));
				try(bch2_btree_iter_traverse(iter));
				return 0;
			}

			pos++;
			bch2_btree_iter_set_pos(iter, POS(0, pos));
		}

		if (start == min)
			return bch_err_throw(trans->c, ENOSPC_inode_create);

		/* Retry from start */
		pos = start = min;
		bch2_btree_iter_set_pos(iter, POS(0, pos));
		le32_add_cpu(&cursor->v.gen, 1);
	}
}

static int bch2_inode_delete_keys(struct btree_trans *trans,
				  subvol_inum inum, enum btree_id id)
{
	struct bkey_s_c k;
	struct bkey_i delete;
	struct bpos end = POS(inum.inum, U64_MAX);
	u32 snapshot;
	int ret = 0;

	/*
	 * We're never going to be deleting partial extents, no need to use an
	 * extent iterator:
	 */
	CLASS(btree_iter, iter)(trans, id, POS(inum.inum, 0), BTREE_ITER_intent);

	while (1) {
		bch2_trans_begin(trans);

		ret = bch2_subvolume_get_snapshot(trans, inum.subvol, &snapshot);
		if (ret)
			goto err;

		bch2_btree_iter_set_snapshot(&iter, snapshot);

		k = bch2_btree_iter_peek_max(&iter, &end);
		ret = bkey_err(k);
		if (ret)
			goto err;

		if (!k.k)
			break;

		bkey_init(&delete.k);
		delete.k.p = iter.pos;

		if (iter.flags & BTREE_ITER_is_extents)
			bch2_key_resize(&delete.k,
					bpos_min(end, k.k->p).offset -
					iter.pos.offset);

		ret = bch2_trans_update(trans, &iter, &delete, 0) ?:
		      bch2_trans_commit(trans, NULL, NULL,
					BCH_TRANS_COMMIT_no_enospc);
err:
		if (ret && !bch2_err_matches(ret, BCH_ERR_transaction_restart))
			break;
	}

	return ret;
}

static int bch2_inode_rm_trans(struct btree_trans *trans, subvol_inum inum, u32 *snapshot)
{
	try(bch2_subvolume_get_snapshot(trans, inum.subvol, snapshot));

	CLASS(btree_iter, iter)(trans, BTREE_ID_inodes, SPOS(0, inum.inum, *snapshot),
				BTREE_ITER_intent|BTREE_ITER_cached);
	struct bkey_s_c k = bkey_try(bch2_btree_iter_peek_slot(&iter));

	if (!bkey_is_inode(k.k)) {
		bch2_fs_inconsistent(trans->c,
				     "inode %llu:%u not found when deleting",
				     inum.inum, *snapshot);
		return bch_err_throw(trans->c, ENOENT_inode);
	}

	return bch2_btree_delete_at(trans, &iter, 0);
}

int bch2_inode_rm(struct bch_fs *c, subvol_inum inum)
{
	CLASS(btree_trans, trans)(c);

	struct bch_inode_unpacked inode;
	int ret = lockrestart_do(trans, may_delete_deleted_inum(trans, inum, &inode));
	if (ret &&
	    !bch2_err_matches(ret, EIO) &&
	    !bch2_err_matches(ret, EROFS)) {
		CLASS(printbuf, buf)();
		prt_printf(&buf, "VFS incorrectly tried to delete inode\n");
		guard(printbuf_indent)(&buf);
		lockrestart_do(trans, bch2_inum_to_path(trans, inum, &buf));
		prt_newline(&buf);
		bch2_inode_unpacked_to_text(&buf, &inode);

		bch_err_msg(c, ret, "%s", buf.buf);
		bch2_sb_error_count(c, BCH_FSCK_ERR_vfs_bad_inode_rm);
	}
	try(ret);

	/*
	 * If this was a directory, there shouldn't be any real dirents left -
	 * but there could be whiteouts (from hash collisions) that we should
	 * delete:
	 *
	 * XXX: the dirent code ideally would delete whiteouts when they're no
	 * longer needed
	 */
	try((!S_ISDIR(inode.bi_mode)
	     ? bch2_inode_delete_keys(trans, inum, BTREE_ID_extents)
	     : bch2_inode_delete_keys(trans, inum, BTREE_ID_dirents)));

	try(bch2_inode_delete_keys(trans, inum, BTREE_ID_xattrs));

	u32 snapshot;
	try(commit_do(trans, NULL, NULL, BCH_TRANS_COMMIT_no_enospc,
		      bch2_inode_rm_trans(trans, inum, &snapshot)));

	return delete_ancestor_snapshot_inodes(trans, SPOS(0, inum.inum, snapshot));
}

int bch2_inode_nlink_inc(struct bch_inode_unpacked *bi)
{
	if (bi->bi_flags & BCH_INODE_unlinked)
		bi->bi_flags &= ~BCH_INODE_unlinked;
	else {
		if (bi->bi_nlink == BCH_LINK_MAX - nlink_bias(bi->bi_mode))
			return -BCH_ERR_too_many_links;

		bi->bi_nlink++;
	}

	return 0;
}

void bch2_inode_nlink_dec(struct btree_trans *trans, struct bch_inode_unpacked *bi)
{
	if (bi->bi_nlink && (bi->bi_flags & BCH_INODE_unlinked)) {
		bch2_trans_inconsistent(trans, "inode %llu unlinked but link count nonzero",
					bi->bi_inum);
		return;
	}

	if (bi->bi_flags & BCH_INODE_unlinked) {
		bch2_trans_inconsistent(trans, "inode %llu link count underflow", bi->bi_inum);
		return;
	}

	if (bi->bi_nlink)
		bi->bi_nlink--;
	else
		bi->bi_flags |= BCH_INODE_unlinked;
}

struct bch_opts bch2_inode_opts_to_opts(struct bch_inode_unpacked *inode)
{
	struct bch_opts ret = { 0 };
#define x(_name, _bits)							\
	if (inode->bi_##_name)						\
		opt_set(ret, _name, inode->bi_##_name - 1);
	BCH_INODE_OPTS()
#undef x
	return ret;
}

void bch2_inode_opts_get_inode(struct bch_fs *c,
			       struct bch_inode_unpacked *inode,
			       struct bch_inode_opts *ret)
{
#define x(_name, _bits)							\
	if ((inode)->bi_##_name) {					\
		ret->_name = inode->bi_##_name - 1;			\
		ret->_name##_from_inode = true;				\
	} else {							\
		ret->_name = c->opts._name;				\
		ret->_name##_from_inode = false;			\
	}
	BCH_INODE_OPTS()
#undef x

	/*
	 * Forward compatibility: inodes written by newer versions may carry
	 * checksum/compression types we don't know about — fall back to the
	 * filesystem option for new writes. Reads are unaffected, extents
	 * carry their own types. (This is why these aren't validated at
	 * btree read time: that would reject valid inodes from newer
	 * versions.)
	 */
	if (unlikely(ret->data_checksum >= BCH_CSUM_OPT_NR)) {
		ret->data_checksum = c->opts.data_checksum;
		ret->data_checksum_from_inode = false;
	}
	if (unlikely(!bch2_compression_opt_valid(ret->compression))) {
		ret->compression = c->opts.compression;
		ret->compression_from_inode = false;
	}
	if (unlikely(!bch2_compression_opt_valid(ret->background_compression))) {
		ret->background_compression = c->opts.background_compression;
		ret->background_compression_from_inode = false;
	}

	ret->change_cookie = c->opt_change_cookie;

	bch2_io_opts_fixups(ret);
}

int bch2_inode_set_casefold(struct btree_trans *trans, subvol_inum inum,
			    struct bch_inode_unpacked *bi, unsigned v)
{
	struct bch_fs *c = trans->c;

	int ret = bch2_fs_casefold_enabled(c);
	if (ret) {
		bch_err_ratelimited(c, "Cannot enable casefolding: %s", bch2_err_str(ret));
		return ret;
	}

	/* Not supported on individual files. */
	if (!S_ISDIR(bi->bi_mode))
		return bch_err_throw(c, casefold_opt_is_dir_only);

	/*
	 * Make sure the dir is empty, as otherwise we'd need to
	 * rehash everything and update the dirent keys.
	 */
	try(bch2_empty_dir_trans(trans, inum));
	try(bch2_request_incompat_feature(c, bcachefs_metadata_version_casefolding));

	bch2_check_set_feature(c, BCH_FEATURE_casefolding);

	bi->bi_casefold = v + 1;
	bi->bi_fields_set |= BIT(Inode_opt_casefold);

	return bch2_maybe_propagate_has_case_insensitive(trans, inum, bi);
}

static noinline int __bch2_inode_rm_snapshot(struct btree_trans *trans, u64 inum, u32 snapshot)
{
	bch2_btree_delete_range_trans(trans, BTREE_ID_extents,
				      SPOS(inum, 0, snapshot),
				      SPOS(inum, U64_MAX, snapshot),
				      BTREE_UPDATE_internal_snapshot_node);
	bch2_btree_delete_range_trans(trans, BTREE_ID_dirents,
				      SPOS(inum, 0, snapshot),
				      SPOS(inum, U64_MAX, snapshot),
				      BTREE_UPDATE_internal_snapshot_node);
	bch2_btree_delete_range_trans(trans, BTREE_ID_xattrs,
				      SPOS(inum, 0, snapshot),
				      SPOS(inum, U64_MAX, snapshot),
				      BTREE_UPDATE_internal_snapshot_node);
	try(commit_do(trans, NULL, NULL, BCH_TRANS_COMMIT_no_enospc,
		      bch2_btree_delete(trans, BTREE_ID_inodes, SPOS(0, inum, snapshot),
					BTREE_UPDATE_internal_snapshot_node)));
	return 0;
}

/*
 * After deleting an inode, there may be versions in older snapshots that should
 * also be deleted - if they're not referenced by sibling snapshots and not open
 * in other subvolumes:
 */
static int delete_ancestor_snapshot_inodes(struct btree_trans *trans, struct bpos pos)
{
	while (1) {
		CLASS(btree_iter_uninit, iter)(trans);
		struct bkey_s_c k;

		try(lockrestart_do(trans,
			bkey_err(k = bch2_inode_get_iter_snapshot_parent(trans, &iter, pos))));

		if (!k.k || !bkey_is_unlinked_inode(k))
			return 0;

		pos = k.k->p;
		int ret = lockrestart_do(trans, bch2_inode_or_descendents_is_open(trans, pos));
		if (ret)
			return ret < 0 ? ret : 0;

		try(__bch2_inode_rm_snapshot(trans, pos.offset, pos.snapshot));
	}
}

int bch2_inode_rm_snapshot(struct btree_trans *trans, u64 inum, u32 snapshot)
{
	return __bch2_inode_rm_snapshot(trans, inum, snapshot) ?:
		delete_ancestor_snapshot_inodes(trans, SPOS(0, inum, snapshot)) ?:
		bch_err_throw(trans->c, transaction_restart_nested);
}

static int may_delete_deleted_inode(struct btree_trans *trans, struct bpos pos,
				    struct bch_inode_unpacked *inode,
				    bool from_deleted_inodes)
{
	struct bch_fs *c = trans->c;
	CLASS(printbuf, buf)();

	CLASS(btree_iter, inode_iter)(trans, BTREE_ID_inodes, pos, BTREE_ITER_cached);
	struct bkey_s_c k = bkey_try(bch2_btree_iter_peek_slot(&inode_iter));

	int ret = bkey_is_inode(k.k) ? 0 : bch_err_throw(c, ENOENT_inode);
	if (fsck_err_on(from_deleted_inodes && ret,
			trans, deleted_inode_missing,
			"nonexistent inode %llu:%u in deleted_inodes btree",
			pos.offset, pos.snapshot))
		goto delete;
	if (ret)
		return ret;

	bch2_inode_unpack(trans->c, k, inode);

	if (S_ISDIR(inode->bi_mode)) {
		ret = bch2_empty_dir_snapshot(trans, pos.offset, 0, pos.snapshot);
		if (fsck_err_on(from_deleted_inodes &&
				bch2_err_matches(ret, ENOTEMPTY),
				trans, deleted_inode_is_dir,
				"non empty directory %llu:%u in deleted_inodes btree",
				pos.offset, pos.snapshot))
			goto delete;
		if (ret)
			return ret;
	}

	ret = inode->bi_flags & BCH_INODE_unlinked ? 0 : bch_err_throw(c, inode_not_unlinked);
	if (fsck_err_on(from_deleted_inodes && ret,
			trans, deleted_inode_not_unlinked,
			"non-deleted inode %llu:%u in deleted_inodes btree",
			pos.offset, pos.snapshot))
		goto delete;
	if (ret)
		return ret;

	ret = !(inode->bi_flags & BCH_INODE_has_child_snapshot)
		? 0 : bch_err_throw(c, inode_has_child_snapshot);

	if (fsck_err_on(from_deleted_inodes && ret,
			trans, deleted_inode_has_child_snapshots,
			"inode with child snapshots %llu:%u in deleted_inodes btree",
			pos.offset, pos.snapshot))
		goto delete;
	if (ret)
		return ret;

	ret = bch2_inode_has_child_snapshots(trans, k.k->p);
	if (ret < 0)
		return ret;

	if (ret) {
		if (fsck_err(trans, inode_has_child_snapshots_wrong,
			     "inode has_child_snapshots flag wrong (should be set)\n%s",
			     (printbuf_reset(&buf),
			      bch2_inode_unpacked_to_text(&buf, inode),
			      buf.buf))) {
			inode->bi_flags |= BCH_INODE_has_child_snapshot;
			try(__bch2_fsck_write_inode(trans, inode));
		}

		if (!from_deleted_inodes)
			return  bch2_trans_commit(trans, NULL, NULL, BCH_TRANS_COMMIT_no_enospc) ?:
				bch_err_throw(c, inode_has_child_snapshot);

		goto delete;

	}

	if (from_deleted_inodes) {
		if (test_bit(BCH_FS_clean_recovery, &c->flags) &&
		    !fsck_err(trans, deleted_inode_but_clean,
			      "filesystem marked as clean but have deleted inode %llu:%u",
			      pos.offset, pos.snapshot))
			return 0;

		ret = 1;
	}
fsck_err:
	return ret;
delete:
	return bch2_btree_bit_mod_buffered(trans, BTREE_ID_deleted_inodes, pos, false);
}

static int may_delete_deleted_inum(struct btree_trans *trans, subvol_inum inum,
				   struct bch_inode_unpacked *inode)
{
	u32 snapshot;

	return bch2_subvolume_get_snapshot(trans, inum.subvol, &snapshot) ?:
		may_delete_deleted_inode(trans, SPOS(0, inum.inum, snapshot), inode, false);
}

int bch2_delete_dead_inodes(struct bch_fs *c)
{
	CLASS(btree_trans, trans)(c);
	/*
	 * if we ran check_inodes() unlinked inodes will have already been
	 * cleaned up but the write buffer will be out of sync; therefore we
	 * alway need a write buffer flush
	 *
	 * Weird transaction restart handling here because on successful delete,
	 * bch2_inode_rm_snapshot() will return a nested transaction restart,
	 * but we can't retry because the btree write buffer won't have been
	 * flushed and we'd spin:
	 */
	return  bch2_btree_write_buffer_flush_sync(trans) ?:
		for_each_btree_key_commit(trans, iter, BTREE_ID_deleted_inodes, POS_MIN,
					BTREE_ITER_prefetch|BTREE_ITER_all_snapshots, k,
					NULL, NULL, BCH_TRANS_COMMIT_no_enospc, ({
		struct bch_inode_unpacked inode;
		int ret = may_delete_deleted_inode(trans, k.k->p, &inode, true);
		if (ret > 0) {
			bch_verbose_ratelimited(c, "deleting unlinked inode %llu:%u",
						k.k->p.offset, k.k->p.snapshot);

			ret = bch2_inode_rm_snapshot(trans, k.k->p.offset, k.k->p.snapshot);
			/*
			 * We don't want to loop here: a transaction restart
			 * error here means we handled a transaction restart and
			 * we're actually done, but if we loop we'll retry the
			 * same key because the write buffer hasn't been flushed
			 * yet
			 */
			if (bch2_err_matches(ret, BCH_ERR_transaction_restart)) {
				ret = 0;
				continue;
			}
		}

		ret;
	}));
}

int bch2_kill_i_generation_keys(struct bch_fs *c)
{
	struct progress_indicator progress;
	bch2_progress_init(&progress, __func__, c, BIT_ULL(BTREE_ID_inodes), 0);

	CLASS(btree_trans, trans)(c);
	return for_each_btree_key_commit(trans, iter, BTREE_ID_inodes, POS_MIN,
					 BTREE_ITER_prefetch|BTREE_ITER_all_snapshots, k,
					 NULL, NULL, BCH_TRANS_COMMIT_no_enospc, ({
		bch2_progress_update_iter(trans, &progress, &iter) ?:
		k.k->type == KEY_TYPE_inode_generation
		? bch2_btree_delete_at(trans, &iter, 0)
		: 0;
	}));
}
