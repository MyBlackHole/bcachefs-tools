/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCACHEFS_SB_MEMBERS_TYPES_H
#define _BCACHEFS_SB_MEMBERS_TYPES_H

struct bch_member_cpu {
	// 备注：设备大小
	// 备注：fs_size / btree_node_size = nbuckets
	// 备注：1G / 256k = 4096
	u64			nbuckets;	/* device size */
	u64			nbuckets_minus_first;
	// 备注：使用的第一个桶的索引 
	u16			first_bucket;   /* index of first bucket used */
	// 备注：一个桶的扇区数量
	// 备注：btree_node_size / sector_size(512) = bucket_size
	// 备注：256k / 512 = 512
	u16			bucket_size;	/* sectors */
	u16			group;
	u8			state;
	u8			discard;
	u8			data_allowed;
	u8			durability;
	// 备注：是否初始化了空闲空间 
	u8			freespace_initialized;
	u8			initialized;
	u8			resize_on_mount;
	u8			rotational;
	u8			valid;
	u8			btree_bitmap_shift;
	u64			btree_allocated_bitmap;
};

#endif /* _BCACHEFS_SB_MEMBERS_H */
