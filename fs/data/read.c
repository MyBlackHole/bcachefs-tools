// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2010, 2011 Kent Overstreet <kent.overstreet@gmail.com>
 * Copyright 2012 Google, Inc.
 */

/* DOC(data-read-path)
 *
 * Reads are transparent and self-healing: if a checksum failure or IO error
 * occurs on one replica, bcachefs automatically retries from another replica.
 * The failed device's error counter is incremented and the bad copy is
 * rewritten from the good one. If all replicas fail, the error is propagated
 * to the application.
 *
 * With multiple devices, reads go to the lowest-latency replica. This is
 * tracked per-device and adapts over time, so mixed SSD/HDD configurations
 * automatically prefer the SSD for reads without explicit configuration.
 *
 * End-to-end flow: extent lookup, device selection, disk read, checksum
 * verification, decryption, decompression. For compressed or checksummed
 * extents the full extent must be read even for partial requests, because
 * checksums and compression operate on the whole extent.
 */

#include "bcachefs.h"

#include "alloc/background.h"
#include "alloc/buckets.h"
#include "alloc/disk_groups.h"
#include "alloc/foreground.h"

#include "btree/update.h"

#include "data/checksum.h"
#include "data/compress.h"
#include "data/ec/io.h"
#include "data/io_misc.h"
#include "data/read.h"
#include "data/reflink.h"
#include "data/update.h"
#include "data/write.h"

#include "debug/async_objs.h"

#include "init/error.h"

#include "sb/counters.h"

#include "snapshots/subvolume.h"

#include "util/clock.h"
#include "util/enumerated_ref.h"

#include <linux/moduleparam.h>
#include <linux/random.h>
#include <linux/sched/mm.h>

static unsigned __maybe_unused bch2_read_corrupt_ratio;
static int __maybe_unused bch2_read_corrupt_device;

#ifdef CONFIG_BCACHEFS_DEBUG
module_param_named(read_corrupt_ratio, bch2_read_corrupt_ratio, uint, 0644);
MODULE_PARM_DESC(read_corrupt_ratio, "");

module_param_named(read_corrupt_device, bch2_read_corrupt_device, int, 0644);
MODULE_PARM_DESC(read_corrupt_device, "");
#endif

static bool bch2_poison_extents_on_checksum_error = true;
module_param_named(poison_extents_on_checksum_error,
		   bch2_poison_extents_on_checksum_error, bool, 0644);
MODULE_PARM_DESC(poison_extents_on_checksum_error,
		 "Extents with checksum errors are marked as poisoned - unsafe without read fua support");

static void bch2_read_bio_to_text_atomic(struct printbuf *, struct bch_read_bio *);

#ifndef CONFIG_BCACHEFS_NO_LATENCY_ACCT

static inline u32 bch2_dev_congested_read(struct bch_dev *ca, u64 now)
{
	s64 congested = atomic_read(&ca->congested);
	u64 last = READ_ONCE(ca->congested_last);
	if (time_after64(now, last))
		congested -= (now - last) >> 12;

	return clamp(congested, 0LL, CONGESTED_MAX);
}

static bool bch2_target_congested(struct bch_fs *c, u16 target)
{
	const struct bch_devs_mask *devs;
	unsigned d, nr = 0, total = 0;
	u64 now = local_clock();

	guard(rcu)();
	devs = bch2_target_to_mask(c, target) ?:
		&c->allocator.rw_devs[BCH_DATA_user];

	for_each_set_bit(d, devs->d, BCH_SB_MEMBERS_MAX) {
		struct bch_dev *ca = rcu_dereference(c->devs[d]);
		if (!ca)
			continue;

		total += bch2_dev_congested_read(ca, now);
		nr++;
	}

	return get_random_u32_below(nr * CONGESTED_MAX) < total;
}

__cold void bch2_dev_congested_to_text(struct printbuf *out, struct bch_dev *ca)
{
	printbuf_tabstop_push(out, 32);

	prt_printf(out, "current:\t%u%%\n",
		   bch2_dev_congested_read(ca, local_clock()) *
		   100 / CONGESTED_MAX);

	prt_printf(out, "raw:\t%i/%u\n", atomic_read(&ca->congested), CONGESTED_MAX);

	prt_printf(out, "last io over threshold:\t");
	bch2_pr_time_units(out, local_clock() - ca->congested_last);
	prt_newline(out);

	prt_printf(out, "read latency threshold:\t");
	bch2_pr_time_units(out,
			   ca->io_latency[READ].quantiles.entries[QUANTILE_IDX(1)].m << 2);
	prt_newline(out);

	prt_printf(out, "median read latency:\t");
	bch2_pr_time_units(out,
			   ca->io_latency[READ].quantiles.entries[QUANTILE_IDX(7)].m);
	prt_newline(out);

	prt_printf(out, "write latency threshold:\t");
	bch2_pr_time_units(out,
			   ca->io_latency[WRITE].quantiles.entries[QUANTILE_IDX(1)].m << 3);
	prt_newline(out);

	prt_printf(out, "median write latency:\t");
	bch2_pr_time_units(out,
			   ca->io_latency[WRITE].quantiles.entries[QUANTILE_IDX(7)].m);
	prt_newline(out);
}

#else

static bool bch2_target_congested(struct bch_fs *c, u16 target)
{
	return false;
}

#endif

/* Cache promotion on read */

static inline bool have_io_error(struct bch_io_failures *failed)
{
	return failed && failed->nr;
}

static inline struct data_update *rbio_data_update(struct bch_read_bio *rbio)
{
	EBUG_ON(rbio->split);

	return rbio->data_update
		? container_of(rbio, struct data_update, rbio)
		: NULL;
}

static bool ptr_being_rewritten(struct bch_fs *c, struct bch_read_bio *orig, unsigned dev)
{
	struct data_update *u = rbio_data_update(orig);
	if (!u)
		return false;

	struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(bkey_i_to_s_c(u->k.k));
	unsigned ptr_bit = 1;
	bkey_for_each_ptr(ptrs, ptr) {
		if (ptr->dev == dev && (u->opts.ptrs_kill & ptr_bit))
			return true;
		ptr_bit <<= 1;
	}

	return false;
}

static inline int should_promote(struct bch_fs *c, struct bkey_s_c k,
				 struct bch_inode_opts opts,
				 enum bch_read_flags flags)
{
	BUG_ON(!opts.promote_target);

	if (bch2_bkey_has_target(c, k, opts.promote_target)) {
		event_inc(c, data_read_nopromote_already_promoted);
		return bch_err_throw(c, nopromote_already_promoted);
	}

	if (bkey_extent_is_unwritten(c, k)) {
		event_inc(c, data_read_nopromote_unwritten);
		return bch_err_throw(c, nopromote_unwritten);
	}

	if (bch2_target_congested(c, opts.promote_target)) {
		event_inc(c, data_read_nopromote_congested);
		return bch_err_throw(c, nopromote_congested);
	}

	return 0;
}

static void promote_free_rcu(struct rcu_head *rcu)
{
	struct promote_op *op = container_of(rcu, struct promote_op, write.rcu);

	bch2_bkey_buf_exit(&op->write.k);
	kfree(op);
}

static noinline void promote_free(struct bch_read_bio *rbio, int ret)
{
	struct promote_op *op = container_of(rbio, struct promote_op, write.rbio);
	struct bch_fs *c = rbio->c;

	async_object_list_del(c, promote, op->list_idx);
	async_object_list_del(c, rbio, rbio->list_idx);

	up(per_cpu_ptr(c->promote_limit, op->cpu));

	bch2_data_update_exit(&op->write, ret);

	enumerated_ref_put(&c->writes, BCH_WRITE_REF_promote);
	call_rcu(&op->write.rcu, promote_free_rcu);
}

static void promote_done(struct bch_write_op *wop)
{
	struct promote_op *op = container_of(wop, struct promote_op, write.op);
	struct bch_fs *c = op->write.rbio.c;

	bch2_time_stats_update(&c->times[BCH_TIME_data_promote], op->start_time);
	promote_free(&op->write.rbio, 0);
}

static void promote_start_work(struct work_struct *work)
{
	struct promote_op *op = container_of(work, struct promote_op, work);

	bch2_data_update_read_done(&op->write);
}

static noinline void promote_start(struct bch_read_bio *rbio)
{
	struct promote_op *op = container_of(rbio, struct promote_op, write.rbio);
	struct bch_fs *c = op->write.op.c;

	event_add_trace(c, data_read_promote, op->write.k.k->k.size, buf, ({
		bch2_bkey_val_to_text(&buf, c, bkey_i_to_s_c(op->write.k.k));
	}));

	INIT_WORK(&op->work, promote_start_work);
	queue_work(rbio->c->promote_wq, &op->work);
}

static struct bch_read_bio *__promote_alloc(struct btree_trans *trans,
					    enum btree_id btree_id,
					    struct bkey_s_c k,
					    struct bpos pos,
					    struct extent_ptr_decoded *pick,
					    enum bch_read_flags flags,
					    unsigned sectors,
					    struct bch_read_bio *orig,
					    struct bch_io_failures *failed)
{
	struct bch_fs *c = trans->c;

	int ret = !failed
		? should_promote(c, k, orig->opts, flags)
		: 0;
	if (ret)
		return ERR_PTR(ret);

	struct data_update_opts update_opts = { .write_flags = BCH_WRITE_alloc_nowait };

	if (!have_io_error(failed)) {
		update_opts.type = BCH_DATA_UPDATE_promote;
		update_opts.target = orig->opts.promote_target;
		update_opts.extra_replicas = 1;
		update_opts.write_flags |= BCH_WRITE_cached;
		update_opts.write_flags |= BCH_WRITE_only_specified_devs;
	} else {
		update_opts.type = BCH_DATA_UPDATE_self_heal;
		update_opts.target = orig->opts.foreground_target;

		struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(k);
		unsigned ptr_bit = 1;
		bkey_for_each_ptr(ptrs, ptr) {
			if (bch2_dev_io_failures(failed, ptr->dev) &&
			    !ptr_being_rewritten(c, orig, ptr->dev)) {
				update_opts.ptrs_io_error |= ptr_bit;
				update_opts.ptrs_kill |= ptr_bit;
			}
			ptr_bit <<= 1;
		}

		if (!update_opts.ptrs_kill)
			return ERR_PTR(bch_err_throw(c, nopromote_no_rewrites));
	}

	if (!enumerated_ref_tryget(&c->writes, BCH_WRITE_REF_promote))
		return ERR_PTR(bch_err_throw(c, nopromote_no_writes));

	int cpu = raw_smp_processor_id();
	if (down_trylock(per_cpu_ptr(c->promote_limit, cpu))) {
		ret = bch_err_throw(c, nopromote_ratelimited);
		goto err_put;
	}

	struct promote_op *op = kzalloc(sizeof(*op), GFP_KERNEL);
	if (!op) {
		ret = bch_err_throw(c, nopromote_enomem);
		goto err_up_limit;
	}

	op->start_time = local_clock();
	op->cpu	= cpu;

	ret = bch2_data_update_init(trans, NULL, NULL, &op->write,
			writepoint_hashed((unsigned long) current),
			&orig->opts,
			update_opts,
			btree_id, k);
	/*
	 * possible errors: -BCH_ERR_nocow_lock_blocked,
	 * -BCH_ERR_ENOSPC_disk_reservation:
	 */
	if (ret)
		goto err;

	rbio_init_fragment(&op->write.rbio.bio, orig, failed);
	op->write.rbio.bounce	= true;
	op->write.rbio.promote	= true;
	op->write.op.end_io = promote_done;
	async_object_list_add(c, promote, op, &op->list_idx);

	return &op->write.rbio;
err:
	bch2_bio_free_pages_pool(c, &op->write.op.wbio.bio);
	/* We may have added to the rhashtable and thus need rcu freeing: */
	kfree_rcu(op, write.rcu);
err_up_limit:
	up(per_cpu_ptr(c->promote_limit, cpu));
err_put:
	enumerated_ref_put(&c->writes, BCH_WRITE_REF_promote);
	return ERR_PTR(ret);
}

noinline
static struct bch_read_bio *promote_alloc(struct btree_trans *trans,
					struct bvec_iter iter,
					struct bkey_s_c k,
					struct extent_ptr_decoded *pick,
					enum bch_read_flags flags,
					struct bch_read_bio *orig,
					bool *bounce,
					bool *read_full,
					struct bch_io_failures *failed)
{
	struct bch_fs *c = trans->c;

	bool self_healing = failed != NULL;

	/*
	 * We're in the retry path, but we don't know what to repair yet, and we
	 * don't want to do a promote here:
	 */
	if (self_healing && !failed->nr)
		return NULL;

	/*
	 * We're already doing a data update, we don't need to kick off another
	 * write here - we'll just propagate IO errors back to the parent
	 * data_update:
	 */
	if (self_healing && orig->data_update)
		return NULL;

	/*
	 * if failed != NULL we're not actually doing a promote, we're
	 * recovering from an io/checksum error
	 */
	bool promote_full = (self_healing ||
			     *read_full ||
			     READ_ONCE(c->opts.promote_whole_extents));
	/* data might have to be decompressed in the write path: */
	unsigned sectors = promote_full
		? max(pick->crc.compressed_size, pick->crc.live_size)
		: bvec_iter_sectors(iter);
	struct bpos pos = promote_full
		? bkey_start_pos(k.k)
		: POS(k.k->p.inode, iter.bi_sector);

	struct bch_read_bio *promote =
		__promote_alloc(trans,
				k.k->type == KEY_TYPE_reflink_v
				? BTREE_ID_reflink
				: BTREE_ID_extents,
				k, pos, pick, flags, sectors, orig, failed);
	int ret = PTR_ERR_OR_ZERO(promote);
	if (unlikely(ret)) {
		event_inc_trace(c, data_read_nopromote, buf, ({
			prt_printf(&buf, "%s\n", bch2_err_str(ret));
			bch2_bkey_val_to_text(&buf, c, k);
		}));
		return NULL;
	}

	*bounce		= true;
	*read_full	= promote_full;

	orig->self_healing |= self_healing;
	return promote;
}

__cold void bch2_promote_op_to_text(struct printbuf *out,
			     struct bch_fs *c,
			     struct promote_op *op)
{
	if (!op->write.read_done) {
		prt_printf(out, "parent read: %px\n", op->write.rbio.parent);
		guard(printbuf_indent)(out);
		bch2_read_bio_to_text(out, c, op->write.rbio.parent);
	}

	bch2_data_update_to_text(out, &op->write);
}

/* Read */

void bch2_read_err_msg_trans(struct btree_trans *trans, struct printbuf *out,
			     struct bch_read_bio *rbio, struct bpos read_pos)
{
	prt_str(out, "data read error at ");
	bch2_inum_offset_err_msg_trans(trans, out, rbio->subvol, read_pos);

	if (rbio->data_update)
		prt_str(out, " (internal move) ");
	prt_str(out, ": ");
}

enum rbio_context {
	RBIO_CONTEXT_NULL,
	RBIO_CONTEXT_HIGHPRI,
	RBIO_CONTEXT_UNBOUND,
};

static inline struct bch_read_bio *
bch2_rbio_parent(struct bch_read_bio *rbio)
{
	return rbio->split ? rbio->parent : rbio;
}

__always_inline
static void bch2_rbio_punt(struct bch_read_bio *rbio, work_func_t fn,
			   enum rbio_context context,
			   struct workqueue_struct *wq)
{
	if (context <= rbio->context) {
		fn(&rbio->work);
	} else {
		rbio->work.func		= fn;
		rbio->context		= context;
		queue_work(wq, &rbio->work);
	}
}

static inline struct bch_read_bio *bch2_rbio_free(struct bch_read_bio *rbio)
{
	BUG_ON(rbio->bounce && !rbio->split);

	if (rbio->ca)
		enumerated_ref_put(&rbio->ca->io_ref[READ], BCH_DEV_READ_REF_io_read);

	if (rbio->split) {
		struct bch_read_bio *parent = rbio->parent;

		if (unlikely(rbio->promote)) {
			if (!rbio->ret)
				promote_start(rbio);
			else
				promote_free(rbio, -EIO);
		} else {
			async_object_list_del(rbio->c, rbio, rbio->list_idx);

			if (rbio->bounce)
				bch2_bio_free_pages_pool(rbio->c, &rbio->bio);

			bio_put(&rbio->bio);
		}

		rbio = parent;
	}

	return rbio;
}

/*
 * Only called on a top level bch_read_bio to complete an entire read request,
 * not a split:
 */
static void bch2_rbio_done(struct bch_read_bio *rbio)
{
	if (rbio->start_time)
		bch2_time_stats_update(&rbio->c->times[BCH_TIME_data_read],
				       rbio->start_time);
#ifdef CONFIG_BCACHEFS_ASYNC_OBJECT_LISTS
	if (rbio->list_idx)
		async_object_list_del(rbio->c, rbio, rbio->list_idx);
#endif
	bio_endio(&rbio->bio);
}

static int get_rbio_extent(struct btree_trans *trans, struct bch_read_bio *rbio, struct bkey_buf *sk)
{
	CLASS(btree_iter, iter)(trans, rbio->data_btree, rbio->data_pos, 0);
	struct bkey_s_c k;

	try(lockrestart_do(trans, bkey_err(k = bch2_btree_iter_peek_slot(&iter))));

	struct bch_fs *c = trans->c;
	struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(k);
	bkey_for_each_ptr(ptrs, ptr)
		if (bch2_extent_ptr_eq(*ptr, rbio->pick.ptr)) {
			bch2_bkey_buf_reassemble(sk, k);
			break;
		}

	return 0;
}

static noinline int maybe_poison_extent(struct btree_trans *trans, struct bch_read_bio *rbio,
					enum btree_id btree, struct bkey_s_c read_k)
{
	if (!bch2_poison_extents_on_checksum_error)
		return 0;

	struct bch_fs *c = trans->c;

	/* Can't commit during recovery — will be handled after going rw */
	if (!test_bit(BCH_FS_rw, &c->flags))
		return 0;
	struct data_update *u = rbio_data_update(rbio);
	if (u)
		read_k = bkey_i_to_s_c(u->k.k);

	u64 flags = bch2_bkey_extent_flags(read_k);
	if (flags & BIT_ULL(BCH_EXTENT_FLAG_poisoned))
		return 0;

	CLASS(btree_iter, iter)(trans, btree, bkey_start_pos(read_k.k), BTREE_ITER_intent);
	struct bkey_s_c k = bkey_try(bch2_btree_iter_peek_slot(&iter));

	if (!bkey_and_val_eq(k, read_k))
		return 0;

	struct bkey_i *new = errptr_try(bch2_trans_kmalloc(trans,
						bkey_bytes(k.k) + sizeof(struct bch_extent_flags)));

	bkey_reassemble(new, k);
	try(bch2_bkey_extent_flags_set(c, new, flags|BIT_ULL(BCH_EXTENT_FLAG_poisoned)));
	try(bch2_trans_update(trans, &iter, new, BTREE_UPDATE_internal_snapshot_node));

	CLASS(disk_reservation, res)(c);
	try(bch2_trans_commit(trans, &res.r, NULL, 0));

	/*
	 * Propagate key change back to data update path, in particular so it
	 * knows the extent has been poisoned and it's safe to change the
	 * checksum
	 */
	if (u)
		bch2_bkey_buf_copy(&u->k, new);

	event_inc_trace(c, data_read_fail_and_poison, buf, ({
		bch2_bkey_val_to_text(&buf, c, k);
		prt_newline(&buf);
		bch2_read_bio_to_text_atomic(&buf, rbio);
	}));
	return 0;
}

static inline bool data_read_err_should_retry(int err)
{
	return  bch2_err_matches(err, BCH_ERR_transaction_restart) ||
		bch2_err_matches(err, BCH_ERR_data_read_retry) ||
		bch2_err_matches(err, BCH_ERR_blockdev_io_error);
}

static noinline int bch2_read_retry_nodecode(struct btree_trans *trans,
					struct bch_read_bio *rbio,
					struct bvec_iter bvec_iter,
					struct bch_io_failures *failed,
					enum bch_read_flags flags)
{
	struct data_update *u = container_of(rbio, struct data_update, rbio);
	int ret = 0;

	do {
		bch2_trans_begin(trans);

		CLASS(btree_iter, iter)(trans, u->btree_id, bkey_start_pos(&u->k.k->k), 0);
		struct bkey_s_c k;

		try(lockrestart_do(trans, bkey_err(k = bch2_btree_iter_peek_slot(&iter))));

		if (!bkey_and_val_eq(k, bkey_i_to_s_c(u->k.k))) {
			/* extent we wanted to read no longer exists: */
			ret = bch_err_throw(trans->c, data_read_key_overwritten);
			break;
		}

		// 备注：实际文件内容读取
		ret = __bch2_read_extent(trans, rbio, bvec_iter,
					 bkey_start_pos(&u->k.k->k),
					 u->btree_id,
					 bkey_i_to_s_c(u->k.k),
					 0, failed, flags, -1);
	} while (data_read_err_should_retry(ret));

	if (ret)
		rbio->ret = ret;

	return ret;
}

static void propagate_io_error_to_data_update(struct bch_fs *c,
					      struct bch_read_bio *rbio,
					      struct extent_ptr_decoded *pick)
{
	struct data_update *u = rbio_data_update(bch2_rbio_parent(rbio));

	if (u && !pick->do_ec_reconstruct) {
		struct bkey_ptrs_c ptrs = bch2_bkey_ptrs_c(bkey_i_to_s_c(u->k.k));
		unsigned ptr_bit = 1;
		bkey_for_each_ptr(ptrs, ptr) {
			if (pick->ptr.dev == ptr->dev)
				u->opts.ptrs_io_error |= ptr_bit;
			ptr_bit <<= 1;
		}
	}
}

static u32 bch2_io_failures_to_err_mask(struct bch_io_failures *failed)
{
	u32 errors = 0;
	for (unsigned i = 0; i < failed->nr; i++) {
		struct bch_dev_io_failures *f = &failed->data[i];

		if (f->csum_nr)
			errors |= BCH_READ_ERR_checksum;
		if (f->ec_errcode)
			errors |= BCH_READ_ERR_ec_reconstruct;
		if (bch2_err_matches(f->errcode, BCH_ERR_decompress))
			errors |= BCH_READ_ERR_decompression;
		else if (f->errcode)
			errors |= BCH_READ_ERR_io;
	}
	return errors;
}

static int rbio_mark_io_failure(struct bch_read_bio *rbio,
				struct extent_ptr_decoded *pick,
				struct bch_io_failures *failed,
				int ret)
{
	if (bch2_err_matches(ret, BCH_ERR_data_read_retry_avoid) ||
	    bch2_err_matches(ret, BCH_ERR_blockdev_io_error)) {
		bch2_mark_io_failure(failed, pick, ret);
		propagate_io_error_to_data_update(rbio->c, rbio, pick);

	}

	if (ret == BCH_ERR_BLK_STS_INVAL) {
		prt_printf(&failed->ec_msg, "Failing bio, after block layer completion:\n");
		guard(printbuf_indent)(&failed->ec_msg);
		bch2_bio_to_text(&failed->ec_msg, &rbio->bio);
	}

	return ret;
}

static void bch2_rbio_retry(struct work_struct *work)
{
	struct bch_read_bio *rbio =
		container_of(work, struct bch_read_bio, work);
	struct bch_fs *c	= rbio->c;
	struct bvec_iter iter	= rbio->bvec_iter;
	enum bch_read_flags flags = rbio->flags;
	subvol_inum inum = {
		.subvol = rbio->subvol,
		.inum	= rbio->read_pos.inode,
	};
	struct bpos read_pos = rbio->read_pos;
	CLASS(bch_io_failures, failed)();

	flags &= ~BCH_READ_hard_require_read_device;

	event_inc_trace(c, data_read_retry, buf,
			bch2_read_bio_to_text_atomic(&buf, rbio));

	{
		CLASS(btree_trans, trans)(c);

		struct bkey_buf sk __cleanup(bch2_bkey_buf_exit);
		bch2_bkey_buf_init(&sk);
		get_rbio_extent(trans, rbio, &sk);

		if (!bkey_deleted(&sk.k->k))
			rbio_mark_io_failure(rbio, &rbio->pick, &failed, rbio->ret);

		if (!rbio->split) {
			rbio->bio.bi_status	= 0;
			rbio->ret		= 0;
		}

		rbio = bch2_rbio_free(rbio);

		flags |= BCH_READ_in_retry;
		flags &= ~BCH_READ_may_promote;
		flags &= ~BCH_READ_last_fragment;
		flags |= BCH_READ_must_clone;

		int ret = rbio->data_update
			? bch2_read_retry_nodecode(trans, rbio, iter, &failed, flags)
			: bch2_read(trans, rbio, iter, inum, &failed, &sk, flags);

		if (ret)
			rbio->ret = ret;

		if (rbio->data_update &&
		    (bch2_err_matches(ret, BCH_ERR_data_read_key_overwritten) ||
		     bch2_err_matches(ret, BCH_ERR_data_read_ptr_stale_race)))
			ret = 0;

		if (failed.nr || failed.ec_msg.pos || ret) {
			struct printbuf *out;
			CLASS(bch_log_msg, msg)(c);

			if (rbio->err_report) {
				mutex_lock(&rbio->err_report->lock);
				out = &rbio->err_report->msg;
				rbio->err_report->errors |= bch2_io_failures_to_err_mask(&failed);
			} else {
				out = &msg.m;
				msg.m.suppress = !ret
					? bch2_ratelimit(c)
					: bch2_ratelimit(c);
			}

			bch2_read_err_msg_trans(trans, out, rbio, read_pos);
			prt_newline(out);

			if (!bkey_deleted(&sk.k->k)) {
				bch2_bkey_val_to_text(out, c, bkey_i_to_s_c(sk.k));
				prt_newline(out);
			}

			bch2_io_failures_to_text(out, c, &failed);

			if (!ret) {
				prt_str(out, "successful retry");
				if (rbio->self_healing)
					prt_str(out, ", self healing");
			} else
				prt_printf(out, "error %s", bch2_err_str(ret));

			if (rbio->err_report)
				mutex_unlock(&rbio->err_report->lock);
		}

		/* drop trans before calling rbio_done() */
	}

	bch2_rbio_done(rbio);
}

static int bch2_rbio_error(struct bch_read_bio *rbio, int ret)
{
	BUG_ON(ret >= 0);

	rbio->ret = ret;
	bch2_rbio_parent(rbio)->saw_error = true;

	if (!(rbio->flags & BCH_READ_in_retry)) {
		if (data_read_err_should_retry(ret)) {
			bch2_rbio_punt(rbio, bch2_rbio_retry, RBIO_CONTEXT_UNBOUND, system_unbound_wq);
		} else {
			rbio = bch2_rbio_free(rbio);
			rbio->ret = ret;
			bch2_rbio_done(rbio);
		}
	} else {
		rbio_mark_io_failure(rbio, &rbio->pick, rbio->failed, ret);
	}
	return ret;
}

static int __bch2_rbio_narrow_crcs(struct btree_trans *trans,
				   struct bch_read_bio *rbio,
				   struct bch_extent_crc_unpacked *new_crc)
{
	struct bch_fs *c = rbio->c;
	u64 data_offset = rbio->data_pos.offset - rbio->pick.crc.offset;

	CLASS(btree_iter, iter)(trans, rbio->data_btree, rbio->data_pos, BTREE_ITER_intent);
	struct bkey_s_c k = bkey_try(bch2_btree_iter_peek_slot(&iter));

	if (bversion_cmp(k.k->bversion, rbio->version) ||
	    !bch2_bkey_matches_ptr(c, k, rbio->pick.ptr, data_offset))
		return bch_err_throw(c, rbio_narrow_crcs_fail);

	/* Extent was trimmed/merged? */
	if (!bpos_eq(bkey_start_pos(k.k), rbio->data_pos) ||
	    k.k->p.offset != rbio->data_pos.offset + rbio->pick.crc.live_size)
		return bch_err_throw(c, rbio_narrow_crcs_fail);

	/*
	 * going to be temporarily appending another checksum entry:
	 */
	struct bkey_i *new =
		errptr_try(bch2_trans_kmalloc(trans, bkey_bytes(k.k) + sizeof(struct bch_extent_crc128)));

	bkey_reassemble(new, k);

	if (!bch2_bkey_narrow_crc(c, new, rbio->pick.crc, *new_crc))
		return bch_err_throw(c, rbio_narrow_crcs_fail);

	return bch2_trans_update(trans, &iter, new, BTREE_UPDATE_internal_snapshot_node);
}

static noinline void bch2_rbio_narrow_crcs(struct bch_read_bio *rbio)
{
	struct bch_fs *c = rbio->c;

	if (!rbio->pick.crc.csum_type ||
	    crc_is_compressed(rbio->pick.crc))
		return;

	u64 data_offset = rbio->data_pos.offset - rbio->pick.crc.offset;

	struct bch_extent_crc_unpacked new_crc;
	if (bch2_rechecksum_bio(c, &rbio->bio, rbio->version,
			rbio->pick.crc, NULL, &new_crc,
			rbio->data_pos.offset - data_offset, rbio->pick.crc.live_size,
			rbio->pick.crc.csum_type)) {
		bch_err(c, "error verifying existing checksum while narrowing checksum (memory corruption?)");
		return;
	}

	/* Can't commit during recovery */
	if (!test_bit(BCH_FS_rw, &c->flags))
		return;

	CLASS(btree_trans, trans)(c);
	int ret = commit_do(trans, NULL, NULL, BCH_TRANS_COMMIT_no_enospc,
			    __bch2_rbio_narrow_crcs(trans, rbio, &new_crc));
	if (!ret)
		event_inc_trace(c, data_read_narrow_crcs, buf,
				bch2_read_bio_to_text_atomic(&buf, rbio));
	else
		event_inc_trace(c, data_read_narrow_crcs_fail, buf, ({
			prt_printf(&buf, "%s\n", bch2_err_str(ret));
			bch2_read_bio_to_text_atomic(&buf, rbio);
		}));
}

static int bch2_rbio_decrypt(struct bch_fs *c, struct bch_read_bio *rbio,
			     struct bch_extent_crc_unpacked crc, struct nonce nonce)
{
	return bch2_encrypt_bio(c, crc.csum_type, nonce, &rbio->bio)
		? bch_err_throw(c, data_read_decrypt_err)
		: 0;
}

/* Inner part that may run in process context */
static int __bch2_read_endio_work(struct bch_read_bio *rbio)
{
	struct bch_fs *c	= rbio->c;
	struct bch_dev *ca = rbio->ca;
	struct bch_read_bio *parent	= bch2_rbio_parent(rbio);
	struct bio *src			= &rbio->bio;
	struct bio *dst			= &parent->bio;
	struct bvec_iter dst_iter	= rbio->bvec_iter;
	struct bch_extent_crc_unpacked crc = rbio->pick.crc;
	struct nonce nonce = extent_nonce(rbio->version, crc);
	struct bch_csum csum;
	int ret;

	guard(memalloc_flags)(PF_MEMALLOC_NOFS);

	if (bch2_read_corrupt_device == rbio->pick.ptr.dev ||
	    bch2_read_corrupt_device < 0)
		bch2_maybe_corrupt_bio(src, bch2_read_corrupt_ratio);

	csum = bch2_checksum_bio(c, crc.csum_type, nonce, src);
	bool csum_good = !bch2_crc_cmp(csum, rbio->pick.crc.csum) || c->opts.no_data_io;

	/*
	 * Checksum error: if the bio wasn't bounced, we may have been
	 * reading into buffers owned by userspace (that userspace can
	 * scribble over) - retry the read, bouncing it this time:
	 */
	if (!csum_good && !rbio->bounce && (rbio->flags & BCH_READ_user_mapped)) {
		rbio->flags |= BCH_READ_must_bounce;
		return bch_err_throw(c, data_read_retry_csum_err_maybe_userspace);
	}

	bch2_account_io_completion(ca, BCH_MEMBER_ERROR_checksum, 0, csum_good);

	/*
	 * XXX
	 * We need to rework the narrow_crcs path to deliver the read completion
	 * first, and then punt to a different workqueue, otherwise we're
	 * holding up reads while doing btree updates which is bad for memory
	 * reclaim.
	 */
	if (unlikely(rbio->narrow_crcs) && csum_good)
		bch2_rbio_narrow_crcs(rbio);

	if (likely(!parent->data_update)) {
		/* Adjust crc to point to subset of data we want: */
		crc.offset     += rbio->offset_into_extent;
		crc.live_size	= bvec_iter_sectors(rbio->bvec_iter);

		if (crc_is_compressed(crc)) {
			BUG_ON(!rbio->bounce);

			try(bch2_rbio_decrypt(c, rbio, crc, nonce));

			ret = bch2_bio_uncompress(c, src, dst, dst_iter, crc);
			if (ret && !c->opts.no_data_io)
				return ret;
		} else {
			/* don't need to decrypt the entire bio: */
			nonce = nonce_add(nonce, crc.offset << 9);
			bio_advance(src, crc.offset << 9);

			BUG_ON(src->bi_iter.bi_size < dst_iter.bi_size);
			src->bi_iter.bi_size = dst_iter.bi_size;

			try(bch2_rbio_decrypt(c, rbio, crc, nonce));

			if (rbio->bounce) {
				struct bvec_iter src_iter = src->bi_iter;

				bio_copy_data_iter(dst, &dst_iter, src, &src_iter);
			}
		}
	} else {
		if (parent->data_update_verify_decompress &&
		    crc_is_compressed(crc)) {
			BUG_ON(!rbio->bounce);

			try(bch2_rbio_decrypt(c, rbio, crc, nonce));

			/*
			 * dst_iter doesn't make sense here, it refers to the
			 * size of the compressed extent on disk (what the data
			 * update path generally wants), and here we're just
			 * verifying thath the data decompresses and throwing
			 * away the result:
			 */
			ret = bch2_bio_uncompress(c, src, dst, (struct bvec_iter) {}, crc);
			if (ret && !c->opts.no_data_io)
				return ret;

			/* We decrypted to decompress; re-encrypt: */
			try(bch2_rbio_decrypt(c, rbio, crc, nonce));
		}

		if (rbio->split)
			rbio->parent->pick = rbio->pick;

		if (rbio->bounce) {
			struct bvec_iter src_iter = src->bi_iter;

			bio_copy_data_iter(dst, &dst_iter, src, &src_iter);
		}
	}

	if (!csum_good)
		return bch_err_throw(c, data_read_retry_csum_err);

	if (rbio->promote) {
		/*
		 * Re encrypt data we decrypted, so it's consistent with
		 * rbio->crc:
		 */
		try(bch2_rbio_decrypt(c, rbio, crc, nonce));
	}

	if (likely(!(rbio->flags & BCH_READ_in_retry))) {
		rbio = bch2_rbio_free(rbio);
		bch2_rbio_done(rbio);
	}

	return 0;
}

static void bch2_read_endio_work(struct work_struct *work)
{
	struct bch_read_bio *rbio =
		container_of(work, struct bch_read_bio, work);
	int ret = __bch2_read_endio_work(rbio);
	if (ret)
		bch2_rbio_error(rbio, ret);
}

noinline __cold
static void data_read_reuse_race_trace(struct bch_read_bio *rbio)
{
	__event_trace(rbio->c, data_read_reuse_race, buf, ({
		guard(printbuf_atomic)(&buf);
		bch2_read_bio_to_text_atomic(&buf, rbio);
	}));
}

static void bch2_read_endio(struct bio *bio)
{
	struct bch_read_bio *rbio =
		container_of(bio, struct bch_read_bio, bio);
	struct bch_fs *c	= rbio->c;
	struct bch_dev *ca = rbio->ca;
	struct workqueue_struct *wq = NULL;
	enum rbio_context context = RBIO_CONTEXT_NULL;

	bch2_account_io_completion(ca, BCH_MEMBER_ERROR_read,
				   rbio->submit_time, !bio->bi_status);

	if (!rbio->split)
		rbio->bio.bi_end_io = rbio->end_io;

	/* Reset iterator for checksumming and copying bounced data: */
	if (rbio->bounce) {
		rbio->bio.bi_iter.bi_size	= rbio->pick.crc.compressed_size << 9;
		rbio->bio.bi_iter.bi_idx	= 0;
		rbio->bio.bi_iter.bi_bvec_done	= 0;
	} else {
		rbio->bio.bi_iter		= rbio->bvec_iter;
	}

	if (unlikely(bio->bi_status)) {
		bch2_rbio_error(rbio, __bch2_err_throw(c, -blk_status_to_bch_err(bio->bi_status)));
		return;
	}

	if (((rbio->flags & BCH_READ_retry_if_stale) && race_fault()) ||
	    (ca && dev_ptr_stale(ca, &rbio->pick.ptr))) {
		event_inc_trace_fn(c, data_read_reuse_race, data_read_reuse_race_trace(rbio));

		if (rbio->flags & BCH_READ_retry_if_stale)
			bch2_rbio_error(rbio, bch_err_throw(c, data_read_ptr_stale_retry));
		else
			bch2_rbio_error(rbio, bch_err_throw(c, data_read_ptr_stale_race));
		return;
	}

	if (rbio->narrow_crcs ||
	    rbio->promote ||
	    crc_is_compressed(rbio->pick.crc) ||
	    bch2_csum_type_is_encryption(rbio->pick.crc.csum_type))
		context = RBIO_CONTEXT_UNBOUND,	wq = system_unbound_wq;
	else if (rbio->pick.crc.csum_type)
		context = RBIO_CONTEXT_HIGHPRI,	wq = system_highpri_wq;

	bch2_rbio_punt(rbio, bch2_read_endio_work, context, wq);
}

static noinline void read_from_stale_dirty_pointer(struct btree_trans *trans,
						   struct bch_dev *ca,
						   struct bkey_s_c k,
						   struct bch_extent_ptr ptr)
{
	struct bch_fs *c = trans->c;
	CLASS(printbuf, buf)();
	CLASS(btree_iter, iter)(trans, BTREE_ID_alloc,
				PTR_BUCKET_POS(ca, &ptr),
				BTREE_ITER_cached);

	int gen = bucket_gen_get(ca, iter.pos.offset);
	if (gen >= 0) {
		prt_printf(&buf, "Attempting to read from stale dirty pointer:\n");
		printbuf_indent_add(&buf, 2);

		bch2_bkey_val_to_text(&buf, c, k);
		prt_newline(&buf);

		prt_printf(&buf, "memory gen: %u", gen);

		int ret = lockrestart_do(trans, bkey_err(k = bch2_btree_iter_peek_slot(&iter)));
		if (!ret) {
			prt_newline(&buf);
			bch2_bkey_val_to_text(&buf, c, k);
		}
	} else {
		prt_printf(&buf, "Attempting to read from invalid bucket %llu:%llu:\n",
			   iter.pos.inode, iter.pos.offset);
		printbuf_indent_add(&buf, 2);

		prt_printf(&buf, "first bucket %u nbuckets %llu\n",
			   ca->mi.first_bucket, ca->mi.nbuckets);

		bch2_bkey_val_to_text(&buf, c, k);
		prt_newline(&buf);
	}

	bch2_fs_inconsistent(c, "%s", buf.buf);
}

static inline bool can_narrow_crc(struct bch_extent_crc_unpacked n)
{
	return n.csum_type &&
		n.uncompressed_size < n.live_size &&
		!crc_is_compressed(n);
}

noinline __cold
static void data_read_split_trace(struct bch_read_bio *rbio)
{
	__event_trace(rbio->c, data_read_split, buf, ({
		bch2_read_bio_to_text_atomic(&buf, rbio);
	}));
}

noinline __cold
static void data_read_bounce_trace(struct bch_read_bio *rbio)
{
	__event_trace(rbio->c, data_read_bounce, buf, ({
		bch2_read_bio_to_text_atomic(&buf, rbio);
	}));
}

noinline __cold
static void data_read_trace(struct bch_read_bio *rbio, struct bkey_s_c k)
{
	__event_trace(rbio->c, data_read, buf, ({
		bch2_bkey_val_to_text(&buf, rbio->c, k);
		prt_newline(&buf);
		bch2_read_bio_to_text_atomic(&buf, rbio);
	}));
}

noinline __cold
static void data_update_read_trace(struct bch_read_bio *rbio, struct bkey_s_c k)
{
	__event_trace(rbio->c, data_update_read, buf, ({
		bch2_bkey_val_to_text(&buf, rbio->c, k);
		prt_newline(&buf);
		bch2_read_bio_to_text_atomic(&buf, rbio);
	}));
}

static inline struct bch_read_bio *read_extent_rbio_alloc(struct btree_trans *trans,
			struct bch_read_bio *orig,
			struct bvec_iter iter, struct bpos read_pos,
			enum btree_id data_btree, struct bkey_s_c k,
			struct extent_ptr_decoded pick,
			struct bch_dev *ca,
			unsigned offset_into_extent,
			struct bch_io_failures *failed, enum bch_read_flags flags,
			bool bounce, bool read_full, bool narrow_crcs)
{
	struct bch_fs *c = trans->c;
	struct bpos data_pos = bkey_start_pos(k.k);

	struct bch_read_bio *rbio =
		(orig->opts.promote_target && (flags & BCH_READ_may_promote)) ||
		have_io_error(failed)
		? promote_alloc(trans, iter, k, &pick, flags, orig,
				&bounce, &read_full, failed)
		: NULL;

	/*
	 * If it's being moved internally, we don't want to flag it as a cache
	 * hit:
	 */
	if (ca && pick.ptr.cached && !orig->data_update)
		bch2_bucket_io_time_reset(trans, pick.ptr.dev,
			PTR_BUCKET_NR(ca, &pick.ptr), READ);

	/*
	 * Done with btree operations:
	 * Unlock the iterator while the btree node's lock is still in cache,
	 * before allocating the clone/fragment (if any) and doing the IO:
	 */
	if (!(flags & BCH_READ_in_retry))
		bch2_trans_unlock(trans);
	else
		bch2_trans_unlock_long(trans);

	if (!read_full) {
		EBUG_ON(crc_is_compressed(pick.crc));
		EBUG_ON(pick.crc.csum_type &&
			(bvec_iter_sectors(iter) != pick.crc.uncompressed_size ||
			 bvec_iter_sectors(iter) != pick.crc.live_size ||
			 pick.crc.offset ||
			 offset_into_extent));

		data_pos.offset += offset_into_extent;
		pick.ptr.offset += pick.crc.offset +
			offset_into_extent;
		offset_into_extent		= 0;
		pick.crc.compressed_size	= bvec_iter_sectors(iter);
		pick.crc.uncompressed_size	= bvec_iter_sectors(iter);
		pick.crc.offset			= 0;
		pick.crc.live_size		= bvec_iter_sectors(iter);
	}

	if (rbio) {
		/*
		 * promote already allocated bounce rbio:
		 * promote needs to allocate a bio big enough for uncompressing
		 * data in the write path, but we're not going to use it all
		 * here:
		 */
		EBUG_ON(rbio->bio.bi_iter.bi_size <
		       pick.crc.compressed_size << 9);
		rbio->bio.bi_iter.bi_size =
			pick.crc.compressed_size << 9;
	} else if (bounce) {
		unsigned sectors = pick.crc.compressed_size;

		rbio = rbio_init_fragment(bio_alloc_bioset(NULL,
						  DIV_ROUND_UP(sectors, PAGE_SECTORS),
						  0,
						  GFP_NOFS,
						  &c->bio_read_split),
				 orig, failed);

		bch2_bio_alloc_pages_pool(c, &rbio->bio, 512, sectors << 9);
		rbio->bounce	= true;
	} else if (flags & BCH_READ_must_clone) {
		/*
		 * Have to clone if there were any splits, due to error
		 * reporting issues (if a split errored, and retrying didn't
		 * work, when it reports the error to its parent (us) we don't
		 * know if the error was from our bio, and we should retry, or
		 * from the whole bio, in which case we don't want to retry and
		 * lose the error)
		 */
		rbio = rbio_init_fragment(bio_alloc_clone(NULL, &orig->bio, GFP_NOFS,
						 &c->bio_read_split),
				 orig, failed);
		rbio->bio.bi_iter = iter;
	} else {
		rbio = orig;
		rbio->bio.bi_iter = iter;
		EBUG_ON(bio_flagged(&rbio->bio, BIO_CHAIN));
	}

	EBUG_ON(bio_sectors(&rbio->bio) != pick.crc.compressed_size);

	rbio->submit_time	= local_clock();
	if (!rbio->split)
		rbio->end_io	= orig->bio.bi_end_io;
	rbio->bvec_iter		= iter;
	rbio->offset_into_extent= offset_into_extent;
	rbio->flags		= flags;
	rbio->ca		= ca;
	rbio->narrow_crcs	= narrow_crcs;
	rbio->ret		= 0;
	rbio->context		= 0;
	rbio->pick		= pick;
	rbio->subvol		= orig->subvol;
	rbio->read_pos		= read_pos;
	rbio->data_btree	= data_btree;
	rbio->data_pos		= data_pos;
	rbio->version		= k.k->bversion;
	INIT_WORK(&rbio->work, NULL);

	rbio->bio.bi_opf	= orig->bio.bi_opf;
	rbio->bio.bi_iter.bi_sector = pick.ptr.offset;
	rbio->bio.bi_end_io	= bch2_read_endio;

	if (!(flags & (BCH_READ_in_retry|BCH_READ_last_fragment))) {
		bio_inc_remaining(&orig->bio);
		event_inc_trace_fn(c, data_read_split, data_read_split_trace(rbio));
	}

	async_object_list_add(c, rbio, rbio, &rbio->list_idx);

	if (rbio->bounce)
		event_inc_trace_fn(c, data_read_bounce, data_read_bounce_trace(rbio));

	if (!orig->data_update)
		event_add_trace_fn(c, data_read, bio_sectors(&rbio->bio),
				   data_read_trace(rbio, k));
	else
		event_add_trace_fn(c, data_update_read, bio_sectors(&rbio->bio),
				   data_update_read_trace(rbio, k));

	bch2_increment_clock(c, bio_sectors(&rbio->bio), READ);
	return rbio;
}

static inline int read_extent_done(struct bch_read_bio *rbio, enum bch_read_flags flags, int ret)
{
	if (flags & BCH_READ_in_retry)
		return ret;

	if (ret)
		rbio->ret = ret;

	if (flags & BCH_READ_last_fragment)
		bch2_rbio_done(rbio);
	return 0;
}

static noinline int read_extent_inline(struct bch_fs *c,
				       struct bch_read_bio *rbio,
				       struct bvec_iter iter,
				       struct bkey_s_c k,
				       unsigned offset_into_extent,
				       enum bch_read_flags flags)
{
	event_add_trace(c, data_read_inline, bvec_iter_sectors(iter), buf, ({
		bch2_bkey_val_to_text(&buf, c, k);
		prt_newline(&buf);
		bch2_read_bio_to_text_atomic(&buf, rbio);
	}));

	// 备注：内联数据

	// 备注：内联数据大小
	unsigned bytes = min(iter.bi_size, offset_into_extent << 9);
	swap(iter.bi_size, bytes);
	zero_fill_bio_iter(&rbio->bio, iter);
	swap(iter.bi_size, bytes);

	bio_advance_iter(&rbio->bio, &iter, bytes);

	bytes = min(iter.bi_size, bkey_inline_data_bytes(k.k));

	swap(iter.bi_size, bytes);
	// 备注：移动数据到 orig bio
	memcpy_to_bio(&rbio->bio, iter, bkey_inline_data_p(k));
	swap(iter.bi_size, bytes);

	bio_advance_iter(&rbio->bio, &iter, bytes);

	zero_fill_bio_iter(&rbio->bio, iter);

	return read_extent_done(rbio, flags, 0);
}

static noinline int read_extent_hole(struct bch_fs *c,
				     struct bch_read_bio *rbio,
				     struct bvec_iter iter,
				     struct bkey_s_c k,
				     enum bch_read_flags flags)
{
	event_add_trace(c, data_read_hole, bvec_iter_sectors(iter), buf, ({
		bch2_bkey_val_to_text(&buf, c, k);
		prt_newline(&buf);
		bch2_read_bio_to_text_atomic(&buf, rbio);
	}));

	/*
	 * won't normally happen in the data update (bch2_move_extent()) path,
	 * but if we retry and the extent we wanted to read no longer exists we
	 * have to signal that:
	 */
	if (rbio->data_update)
		rbio->ret = bch_err_throw(c, data_read_key_overwritten);

	zero_fill_bio_iter(&rbio->bio, iter);

	return read_extent_done(rbio, flags, 0);
}

static noinline int read_extent_pick_err(struct btree_trans *trans,
					 struct bch_read_bio *rbio,
					 struct bpos read_pos,
					 enum btree_id data_btree, struct bkey_s_c k,
					 enum bch_read_flags flags, int ret)
{
	struct bch_fs *c = trans->c;

	if (ret == -BCH_ERR_data_read_csum_err) {
		/* We can only return errors directly in the retry path */
		BUG_ON(!(flags & BCH_READ_in_retry));

		try(maybe_poison_extent(trans, rbio, data_btree, k));
	}

	if (!(flags & BCH_READ_in_retry)) {
		CLASS(printbuf, buf)();
		bch2_read_err_msg_trans(trans, &buf, rbio, read_pos);
		prt_printf(&buf, "%s\n  ", bch2_err_str(ret));
		bch2_bkey_val_to_text(&buf, c, k);
		bch_err_ratelimited(c, "%s", buf.buf);
	}

	return read_extent_done(rbio, flags, ret);
}

static noinline int read_extent_no_encryption_key(struct btree_trans *trans,
					 struct bch_read_bio *rbio,
					 struct bpos read_pos,
					 struct bkey_s_c k,
					 enum bch_read_flags flags)
{
	struct bch_fs *c = trans->c;

	CLASS(printbuf, buf)();
	bch2_read_err_msg_trans(trans, &buf, rbio, read_pos);
	prt_printf(&buf, "attempting to read encrypted data without encryption key\n  ");
	bch2_bkey_val_to_text(&buf, c, k);

	bch_err_ratelimited(c, "%s", buf.buf);

	return read_extent_done(rbio, flags, bch_err_throw(c, data_read_no_encryption_key));
}

// 备注：实际文件内容读取
int __bch2_read_extent(struct btree_trans *trans,
		       struct bch_read_bio *orig,
		       struct bvec_iter iter, struct bpos read_pos,
		       enum btree_id data_btree, struct bkey_s_c k,
		       unsigned offset_into_extent,
		       struct bch_io_failures *failed,
		       enum bch_read_flags flags, int dev)
{
	struct bch_fs *c = trans->c;
	struct extent_ptr_decoded pick;
	bool bounce = false, read_full = false;
	int ret = 0;

	if (bkey_extent_is_inline_data(k.k))
		return read_extent_inline(c, orig, iter, k, offset_into_extent, flags);

	if (unlikely((bch2_bkey_extent_flags(k) & BIT_ULL(BCH_EXTENT_FLAG_poisoned))) &&
	    !orig->data_update) {
		if (!(flags & BCH_READ_no_poison_check))
			return read_extent_done(orig, flags, bch_err_throw(c, extent_poisoned));
		if (orig->err_report) {
			mutex_lock(&orig->err_report->lock);
			orig->err_report->errors |= BCH_READ_ERR_checksum;
			mutex_unlock(&orig->err_report->lock);
		}
	}

	ret = bch2_bkey_pick_read_device(c, k, failed, &pick, dev, flags);

	/* hole or reservation - just zero fill: */
	// 备注：洞或预留 - 只需零填充：
	if (unlikely(!ret))
		return read_extent_hole(c, orig, iter, k, flags);

	if (unlikely(ret < 0))
		return read_extent_pick_err(trans, orig, read_pos, data_btree, k, flags, ret);
	ret = 0;

	if (bch2_csum_type_is_encryption(pick.crc.csum_type) &&
	    unlikely(!c->chacha20_key_set))
		return read_extent_no_encryption_key(trans, orig, read_pos, k, flags);

	struct bch_dev *ca =
		likely(!pick.do_ec_reconstruct)
		? bch2_dev_get_ioref(c, pick.ptr.dev, READ,
				     BCH_DEV_READ_REF_io_read)
		: NULL;

	/*
	 * Stale dirty pointers are treated as IO errors, but @failed isn't
	 * allocated unless we're in the retry path - so if we're not in the
	 * retry path, don't check here, it'll be caught in bch2_read_endio()
	 * and we'll end up in the retry path:
	 */
	if (unlikely(flags & BCH_READ_in_retry) &&
	    !pick.ptr.cached &&
	    ca &&
	    unlikely(dev_ptr_stale(ca, &pick.ptr))) {
		enumerated_ref_put(&ca->io_ref[READ], BCH_DEV_READ_REF_io_read);
		read_from_stale_dirty_pointer(trans, ca, k, pick.ptr);

		return rbio_mark_io_failure(orig, &pick, failed,
					     bch_err_throw(c, data_read_ptr_stale_dirty));
	}

	bool narrow_crcs = !orig->data_update &&
		!(flags & BCH_READ_in_retry) &&
		can_narrow_crc(pick.crc);

	if (narrow_crcs && (flags & BCH_READ_user_mapped))
		flags |= BCH_READ_must_bounce;


	if (likely(!orig->data_update)) {
		/* Check we're not trying to read more than we have in this extent: */
		EBUG_ON(offset_into_extent + bvec_iter_sectors(iter) > k.k->size);
	} else {
		/*
		 * For data updates, iter.bi_size can be bigger than the key - it's just
		 * the buffer size, we're not trying to read a specific amount of data
		 * from this extent:
		 *
		 * If it's smaller, we raced with a merge during a retry:
		 */
		struct data_update *u = rbio_data_update(orig);
		if (unlikely(pick.crc.uncompressed_size > u->op.wbio.bio.bi_iter.bi_size)) {
			if (ca)
				enumerated_ref_put(&ca->io_ref[READ],
					BCH_DEV_READ_REF_io_read);
			return read_extent_done(orig, flags, bch_err_throw(c, data_read_buffer_too_small));
		}

		iter.bi_size = pick.crc.compressed_size << 9;
		read_full = true;
	}

	bool decode = !orig->data_update || orig->data_update_verify_decompress;

	if (likely(decode))
		if (crc_is_compressed(pick.crc) ||
		    (pick.crc.csum_type != BCH_CSUM_none &&
		     (bvec_iter_sectors(iter) != pick.crc.uncompressed_size ||
		      (bch2_csum_type_is_encryption(pick.crc.csum_type) &&
		       (flags & BCH_READ_user_mapped)) ||
		      (flags & BCH_READ_must_bounce)))) {
			read_full = true;
			bounce = true;
		}

	struct bch_read_bio *rbio =
		read_extent_rbio_alloc(trans, orig, iter, read_pos, data_btree, k,
				       pick, ca, offset_into_extent, failed, flags,
				       bounce, read_full, narrow_crcs);

	if (likely(!rbio->pick.do_ec_reconstruct)) {
		if (unlikely(!rbio->ca)) {
			ret = bch2_rbio_error(rbio,
				bch_err_throw(c, data_read_retry_device_offline));
			goto out;
		}

		this_cpu_add(ca->io_done->sectors[READ][BCH_DATA_user],
			     bio_sectors(&rbio->bio));
		bio_set_dev(&rbio->bio, ca->disk_sb.bdev);

		if (unlikely(c->opts.no_data_io)) {
			if (likely(!(flags & BCH_READ_in_retry)))
				bio_endio(&rbio->bio);
		} else {
			if (likely(!(flags & BCH_READ_in_retry)))
				submit_bio(&rbio->bio);
			else
				submit_bio_wait(&rbio->bio);
		}

		/*
		 * We just submitted IO which may block, we expect relock fail
		 * events and shouldn't count them:
		 */
		trans->notrace_relock_fail = true;
	} else {
		if (!(flags & BCH_READ_in_retry)) {
			bch2_rbio_punt(rbio, bch2_rbio_retry, RBIO_CONTEXT_UNBOUND, system_unbound_wq);
			return 0;
		}

		/* Attempting reconstruct read: */
		ret = bch2_ec_read_extent(trans, rbio, k, &failed->ec_msg);
		if (bch2_err_matches(ret, BCH_ERR_transaction_restart)) {
			bch2_rbio_free(rbio);
			return ret;
		}
		if (ret) {
			bch2_rbio_error(rbio, ret);
			goto out;
		}
	}
out:
	if (likely(!(flags & BCH_READ_in_retry))) {
		return 0;
	} else {
		bch2_trans_unlock(trans);

		if (!ret) {
			rbio->context = RBIO_CONTEXT_UNBOUND;
			bch2_read_endio(&rbio->bio);

			ret = rbio->ret;
		}
		rbio = bch2_rbio_free(rbio);

		return ret;
	}
}

// 备注：============================================================================
// 备注：bcachefs 读取核心实现 - bch2_read
// 备注：============================================================================
// 备注：
// 备注：【函数定位】
// 备注：
// 备注：这是 bcachefs 读取路径的核心函数，负责从 extents B 树中查找数据位置
// 备注：并发起实际的块设备读取。
// 备注：
// 备注：【调用路径】
// 备注：
// 备注：上层调用者:
// 备注：1. FUSE: bcachefs_fuse_read() -> read_aligned() -> bch2_read()
// 备注：2. VFS: bch2_read_iter() -> bch2_read()
// 备注：3. 预读: bch2_readahead() -> bch2_read()
// 备注：
// 备注：【核心职责】
// 备注：
// 备注：1. 数据定位:
// 备注：- 遍历 extents B 树查找 inode + offset 对应的 extent
// 备注：- 处理快照 (subvolume/snapshot) 隔离
// 备注：- 解析间接 extent (bch2_read_indirect_extent)
// 备注：
// 备注：2. 读取执行:
// 备注：- 计算 extent 内的偏移量
// 备注：- 调用 __bch2_read_extent() 执行实际读取
// 备注：- 处理跨 extent 的连续读取
// 备注：
// 备注：3. 错误处理:
// 备注：- 校验和错误重试
// 备注：- 副本切换 (有多个副本时)
// 备注：- 记录错误历史避免重复失败
// 备注：
// 备注：【数据流】
// 备注：
// 备注：用户请求 (inode, offset, size)
// 备注：↓
// 备注：查找 extents B 树 (iter at POS(inode, offset))
// 备注：↓
// 备注：获取 extent 键值 (包含设备指针、校验和等)
// 备注：↓
// 备注：解析间接 extent (如需要)
// 备注：↓
// 备注：计算 extent 内偏移
// 备注：↓
// 备注：调用 __bch2_read_extent()
// 备注：↓
// 备注：选择设备/副本
// 备注：↓
// 备注：提交 bio 到块层
// 备注：↓
// 备注：IO 完成回调 (bch2_read_endio)
// 备注：↓
// 备注：校验和验证
// 备注：↓
// 备注：解压缩 (如需要)
// 备注：↓
// 备注：复制数据到用户缓冲区
// 备注：
// 备注：【关键逻辑】
// 备注：
// 备注：1. 事务重启处理:
// 备注：- 循环中使用 bch2_trans_begin() 支持事务重启
// 备注：- 快照 ID 在每次迭代中重新获取
// 备注：
// 备注：2. 跨 extent 读取:
// 备注：- while 循环处理多个 extent
// 备注：- bio_advance_iter() 推进到下一个片段
// 备注：- BCH_READ_last_fragment 标记最后一个片段
// 备注：
// 备注：3. 错误重试:
// 备注：- failed 结构记录错误历史
// 备注：- prev_read 记录上一次读取的键值
// 备注：- data_read_err_should_retry() 判断是否重试
// 备注：
// 备注：【与写入的区别】
// 备注：
// 备注：1. 读取不需要事务提交 (只读操作)
// 备注：2. 读取不需要磁盘空间预留
// 备注：3. 读取涉及设备选择策略 (多个副本)
// 备注：4. 读取需要处理压缩数据解压
// 备注：5. 读取有复杂的错误恢复机制
// 备注：
// 备注：【关键成员】
// 备注：
// 备注：- rbio: 读取 bio (包含用户缓冲区和回调)
// 备注：- bvec_iter: bio 向量迭代器 (跟踪读取进度)
// 备注：- inum: inode 编号 + 子卷
// 备注：- failed: 错误历史记录
// 备注：- flags: 读取标志 (如 BCH_READ_retry, BCH_READ_last_fragment)
int bch2_read(struct btree_trans *trans, struct bch_read_bio *rbio,
		struct bvec_iter bvec_iter, subvol_inum inum,
		struct bch_io_failures *failed,
		struct bkey_buf *prev_read,
		enum bch_read_flags flags)
{
	struct bch_fs *c = trans->c;
	struct bkey_s_c k;
	enum btree_id data_btree;
	int ret;

	// 备注：调试检查: 确保不是在数据更新路径中调用
	EBUG_ON(rbio->data_update);

	// 备注：初始化临时键值缓冲区 sk
	// 备注：用于存储重组后的键值（处理间接extent时需要用可修改的缓冲区）
	// 备注：__cleanup属性确保函数退出时自动释放
	struct bkey_buf sk __cleanup(bch2_bkey_buf_exit);
	bch2_bkey_buf_init(&sk);

	// 备注：初始化B树迭代器，用于遍历extents B树
	// 备注：BTREE_ID_extents: 数据extent树
	// 备注：POS(inum.inum, bvec_iter.bi_sector): 起始位置（inode + 扇区偏移）
	// 备注：BTREE_ITER_slots: 迭代模式，用于查找覆盖指定位置的slot
	CLASS(btree_iter, iter)(trans, BTREE_ID_extents,
				POS(inum.inum, bvec_iter.bi_sector),
				BTREE_ITER_slots);

	// 备注：主循环：跨多个extent读取数据
	// 备注：当读取范围跨越多个extent时需要循环处理
	while (1) {
		// 备注：默认使用extents树，可能被间接extent改变
		data_btree = BTREE_ID_extents;

		// 备注：事务重启点
		// 备注：乐观并发控制：如遇到锁冲突或节点变化，从事务重启
		// 备注：清理所有待处理更新，重置路径，重新遍历
		bch2_trans_begin(trans);

		// 备注：获取子卷对应的快照ID
		// 备注：快照隔离：确保读取正确快照版本的数据
		// 备注：如子卷不存在或已被删除，返回错误
		u32 snapshot;
		ret = bch2_subvolume_get_snapshot(trans, inum.subvol, &snapshot);
		if (ret)
			goto err;

		// 备注：设置迭代器快照，确保读取一致性视图
		bch2_btree_iter_set_snapshot(&iter, snapshot);

		// 备注：设置迭代器位置到当前读取位置
		// 备注：注意：每次循环可能推进位置（跨extent读取）
		bch2_btree_iter_set_pos(&iter,
				POS(inum.inum, bvec_iter.bi_sector));

		// 备注：在extents树中查找覆盖当前位置的slot
		// 备注：返回：包含目标位置的extent键值
		// 备注：如未找到extent，k.k将为NULL
		k = bch2_btree_iter_peek_slot(&iter);
		ret = bkey_err(k);
		if (ret)
			goto err;

		// 备注：计算在extent内的偏移量
		// 备注：iter.pos.offset: 当前读取位置（以扇区为单位）
		// 备注：bkey_start_offset(k.k): extent起始位置
		// 备注：offset_into_extent: 需从extent内读取的起始偏移
		s64 offset_into_extent = iter.pos.offset -
			bkey_start_offset(k.k);
		// 备注：计算可从当前extent读取的扇区数
		// 备注：k.k->size: extent总大小（扇区数）
		// 备注：减去偏移量得到剩余可读数据量
		unsigned sectors = k.k->size - offset_into_extent;

		// 备注：将只读键值k复制到可修改缓冲区sk
		// 备注：需要修改键值（如解析间接extent时）必须使用可修改副本
		bch2_bkey_buf_reassemble(&sk, k);

		// 备注：解析间接extent（引用其他extent的元数据extent）
		// 备注：如extent是间接的，需查找实际数据extent
		// 备注：会修改data_btree指向正确的B树ID
		// 备注：更新offset_into_extent为实际extent内的偏移
		ret = bch2_read_indirect_extent(trans, &data_btree,
					&offset_into_extent, &sk);
		if (ret)
			goto err;

		// 备注：使用解析后的键值（可能是实际数据extent）
		k = bkey_i_to_s_c(sk.k);

		// 备注：重试路径的键值一致性检查
		// 备注：如键值在上次重试后发生变化（如被其他写入修改），
		// 备注：重置失败历史，因为之前的错误可能不再适用
		if (unlikely(flags & BCH_READ_in_retry) &&
		    !bkey_and_val_eq(k, bkey_i_to_s_c(prev_read->k))) {
			failed->nr = 0;
			bch2_bkey_buf_copy(prev_read, sk.k);
		}

		/*
		 * With indirect extents, the amount of data to read is the min
		 * of the original extent and the indirect extent:
		 */
		// 备注：计算实际可读取的扇区数
		// 备注：考虑因素：
		// 备注：1. extent剩余大小（sectors）
		// 备注：2. 间接extent的大小限制（k.k->size - offset_into_extent）
		// 备注：取较小值确保不越界读取
		sectors = min_t(unsigned, sectors, k.k->size - offset_into_extent);

		// 备注：计算本次读取的字节数
		// 备注：min(可用扇区数, bio剩余扇区数) * 512
		// 备注：确保不超出bio请求的读取范围
		unsigned bytes = min(sectors, bvec_iter_sectors(bvec_iter)) << 9;
		// 备注：临时交换bi_size用于调用__bch2_read_extent
		// 备注：该函数使用bi_size确定读取长度
		swap(bvec_iter.bi_size, bytes);

		// 备注：检查是否为最后一个片段
		// 备注：如bio剩余大小等于本次读取大小，说明是最后一段
		// 备注：设置BCH_READ_last_fragment标志通知下层处理
		if (bvec_iter.bi_size == bytes)
			flags |= BCH_READ_last_fragment;
		else
			flags |= BCH_READ_must_clone;

		// 备注：调用底层extent读取函数
		// 备注：实际执行设备选择、bio提交等操作
		// 备注：参数说明：
		// 备注：- trans: 事务上下文
		// 备注：- rbio: 读取bio
		// 备注：- bvec_iter: 当前bio向量迭代器
		// 备注：- iter.pos: 当前位置
		// 备注：- data_btree: 数据所在的B树
		// 备注：- k: extent键值
		// 备注：- offset_into_extent: extent内偏移
		// 备注：- failed: 失败历史记录
		// 备注：- flags: 读取标志
		// 备注：- -1: 设备选择（-1表示自动选择）
		ret = __bch2_read_extent(trans, rbio, bvec_iter, iter.pos,
					 data_btree, k,
					 offset_into_extent, failed, flags, -1);
		// 备注：恢复原始bi_size
		swap(bvec_iter.bi_size, bytes);

		// 备注：如读取失败，跳转到错误处理
		if (ret)
			goto err;

		// 备注：检查是否完成所有读取
		// 备注：如是最后一个片段，退出循环
		if (flags & BCH_READ_last_fragment)
			break;

		// 备注：推进bio迭代器到下一个位置
		// 备注：继续循环处理下一个extent
		bio_advance_iter(&rbio->bio, &bvec_iter, bytes);
err:
		// 备注：错误处理与重试逻辑
		// 备注：特殊错误：校验和错误可能是由于用户空间修改导致
		// 备注：设置BCH_READ_must_bounce标志，使用反弹缓冲区重试
		if (ret == -BCH_ERR_data_read_retry_csum_err_maybe_userspace)
			flags |= BCH_READ_must_bounce;

		// 备注：判断是否应重试
		// 备注：如错误不可重试（如ENOENT），退出循环
		// 备注：如可重试，循环继续，事务会重新初始化
		if (ret && !data_read_err_should_retry(ret))
			break;
	}

	// 备注：最终错误处理
	// 备注：如读取过程中发生错误且未被处理
	if (unlikely(ret)) {
		// 备注：记录错误日志（除非在重试中或extent被标记为poisoned）
		// 备注：避免重复记录相同错误
		if (!(flags & BCH_READ_in_retry) &&
		    ret != -BCH_ERR_extent_poisoned) {
			CLASS(printbuf, buf)();
			bch2_read_err_msg_trans(trans, &buf, rbio, POS(inum.inum, bvec_iter.bi_sector));
			prt_printf(&buf, "data read error: %s", bch2_err_str(ret));
			bch_err_ratelimited(c, "%s", buf.buf);
		}

		// 备注：保存错误码到rbio供上层查询
		rbio->ret = ret;

		// 备注：完成bio（如不在重试中）
		// 备注：调用完成回调，释放资源
		if (!(flags & BCH_READ_in_retry))
			bch2_rbio_done(rbio);
	}

	return ret;
}

static const char * const bch2_read_bio_flags[] = {
#define x(n)	#n,
	BCH_READ_FLAGS()
#undef x
	NULL
};

static __cold void __bch2_read_bio_to_text(struct printbuf *out,
				    struct bch_read_bio *rbio)
{
	if (!out->nr_tabstops)
		printbuf_tabstop_push(out, 20);

	/* Are we in a retry? */

	guard(printbuf_indent)(out);

	u64 now = local_clock();
	prt_printf(out, "start_time:\t");
	bch2_pr_time_units(out, max_t(s64, 0, now - rbio->start_time));
	prt_newline(out);

	prt_printf(out, "submit_time:\t");
	bch2_pr_time_units(out, max_t(s64, 0, now - rbio->submit_time));
	prt_newline(out);

	if (!rbio->split)
		prt_printf(out, "end_io:\t%ps\n", rbio->end_io);
	else
		prt_printf(out, "parent:\t%px\n", rbio->parent);

	prt_printf(out, "promote:\t%u\n",	rbio->promote);
	prt_printf(out, "bounce:\t%u\n",	rbio->bounce);
	prt_printf(out, "split:\t%u\n",		rbio->split);
	prt_printf(out, "have_ioref:\t%u\n",	rbio->ca != NULL);
	prt_printf(out, "narrow_crcs:\t%u\n",	rbio->narrow_crcs);
	prt_printf(out, "context:\t%u\n",	rbio->context);

	int ret = READ_ONCE(rbio->ret);
	if (ret < 0)
		prt_printf(out, "ret:\t%s\n",		bch2_err_str(ret));
	else
		prt_printf(out, "ret:\t%i\n",		ret);

	prt_printf(out, "flags:\t");
	bch2_prt_bitflags(out, bch2_read_bio_flags, rbio->flags);
	prt_newline(out);

	bch2_bio_to_text(out, &rbio->bio);
}

static void bch2_read_bio_to_text_atomic(struct printbuf *out, struct bch_read_bio *rbio)
{
	bch2_bpos_to_text(out, rbio->read_pos);
	prt_newline(out);
	__bch2_read_bio_to_text(out, rbio);
}

__cold void bch2_read_bio_to_text(struct printbuf *out,
			   struct bch_fs *c,
			   struct bch_read_bio *rbio)
{
	CLASS(btree_trans, trans)(c);
	bch2_inum_offset_err_msg_trans(trans, out, rbio->subvol, rbio->read_pos);

	if (rbio->data_update)
		prt_str(out, " (internal move) ");
	prt_newline(out);
	__bch2_read_bio_to_text(out, rbio);
}

void bch2_fs_io_read_exit(struct bch_fs *c)
{
	bioset_exit(&c->bio_read_split);
	bioset_exit(&c->bio_read);
	mempool_exit(&c->bio_bounce_bufs);
	free_percpu(c->promote_limit);
}

static void *bio_bounce_buf_alloc_fn(gfp_t gfp, void *pool_data)
{
	return (void *) __get_free_pages(gfp, PAGE_ALLOC_COSTLY_ORDER);
}

static void bio_bounce_buf_free_fn(void *p, void *pool_data)
{
	free_pages((unsigned long) p, PAGE_ALLOC_COSTLY_ORDER);
}

int bch2_fs_io_read_init(struct bch_fs *c)
{
	c->promote_limit = alloc_percpu(struct semaphore);
	if (!c->promote_limit)
		return bch_err_throw(c, ENOMEM_promote_limit_init);

	int cpu;
	for_each_possible_cpu(cpu)
		sema_init(per_cpu_ptr(c->promote_limit, cpu), 32);

	if (mempool_init(&c->bio_bounce_bufs,
			 max_t(unsigned,
			       c->opts.btree_node_size,
			       c->opts.encoded_extent_max) /
			 BIO_BOUNCE_BUF_POOL_LEN,
			 bio_bounce_buf_alloc_fn,
			 bio_bounce_buf_free_fn,
			 NULL))
		return bch_err_throw(c, ENOMEM_bio_bounce_pages_init);

	if (bioset_init(&c->bio_read, 1, offsetof(struct bch_read_bio, bio),
			BIOSET_NEED_BVECS))
		return bch_err_throw(c, ENOMEM_bio_read_init);

	if (bioset_init(&c->bio_read_split, 1, offsetof(struct bch_read_bio, bio),
			BIOSET_NEED_BVECS))
		return bch_err_throw(c, ENOMEM_bio_read_split_init);

	return 0;
}
