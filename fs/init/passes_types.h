/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCACHEFS_RECOVERY_PASSES_TYPES_H
#define _BCACHEFS_RECOVERY_PASSES_TYPES_H

struct bch_fs_recovery {
	/* counterpart to c->sb.recovery_passes_required */
	u64			scheduled_passes_ephemeral;

	// 备注：记录当前的恢复进度 
	u64			current_passes;
	enum bch_recovery_pass	current_pass;
	enum bch_recovery_pass	rewound_from;
	enum bch_recovery_pass	rewound_to;

	/* never rewinds version of curr_pass */
	// 备注：永远不会倒回 curr_recovery_pass 版本 
	enum bch_recovery_pass	pass_done;

	/* bitmask of recovery passes that we actually ran */
	// 备注：我们实际运行的恢复过程的位掩码 
	u64			passes_complete;
	u64			passes_failing;
	u64			passes_ratelimiting;

	spinlock_t		lock;
	struct mutex		run_lock;
	struct work_struct	work;
};

#endif /* _BCACHEFS_RECOVERY_PASSES_TYPES_H */
