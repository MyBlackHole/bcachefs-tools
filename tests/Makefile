# SPDX-License-Identifier: GPL-2.0
# Makefile for .tests/ — run from project root:  make -f .tests/Makefile
# Or from this dir:          make -C .tests

PKGCONFIG_LIBS = blkid uuid liburcu libsodium zlib liblz4 libzstd libudev libkeyutils libunwind
PKGCONFIG_CFLAGS = $(shell pkg-config --cflags $(PKGCONFIG_LIBS))
PKGCONFIG_LDLIBS = $(shell pkg-config --libs   $(PKGCONFIG_LIBS))

CFLAGS = -std=gnu11 -O2 -g -MMD -Wall -fPIC				\
	 -Wno-pointer-sign -Wno-deprecated-declarations			\
	 -fno-strict-aliasing -fno-delete-null-pointer-checks		\
	 -I.. -I../fs -I../include -I../c_src -I../raid -I../.bchfs-shims	\
	 -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -D_LGPL_SOURCE		\
	 -DRCU_MEMBARRIER -DZSTD_STATIC_LINKING_ONLY			\
	 -DFUSE_USE_VERSION=35 -DNO_BCACHEFS_CHARDEV -DNO_BCACHEFS_FS	\
	 -DCONFIG_DEBUG_FS -DCONFIG_UNICODE -DCONFIG_STACKTRACE		\
	 -D__SANE_USERSPACE_TYPES__					\
	 -Wno-unused-but-set-variable -Wno-stringop-overflow		\
	 -Wno-zero-length-bounds -Wno-missing-braces			\
	 -Wno-shift-overflow -Wno-enum-conversion -DBCACHEFS_FUSE	\
	 $(PKGCONFIG_CFLAGS)

LDFLAGS = -L.. -lbcachefs $(PKGCONFIG_LDLIBS)				\
	  -lm -lpthread -lrt -lkeyutils -laio -ldl

ALL = bchfs_test bchfs_write64k bchfs_btreetxn

all: $(ALL)

bchfs_test: bchfs_test.o ../.bchfs-shims/bchfs_shims.o ../c_src/fuse_shims.o
	$(CC) -ggdb -o $@ $^ $(LDFLAGS)

bchfs_test.o: bchfs_test.c
	$(CC) $(CFLAGS) -c -o $@ $<

bchfs_write64k: bchfs_write64k.o ../.bchfs-shims/bchfs_shims.o ../c_src/fuse_shims.o
	$(CC) -ggdb -o $@ $^ $(LDFLAGS)

bchfs_write64k.o: bchfs_write64k.c
	$(CC) $(CFLAGS) -c -o $@ $<

bchfs_btreetxn: bchfs_btreetxn.o ../.bchfs-shims/bchfs_shims.o ../c_src/fuse_shims.o
	$(CC) -ggdb -o $@ $^ $(LDFLAGS)

bchfs_btreetxn.o: bchfs_btreetxn.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(ALL) *.o *.d

-include *.d

.PHONY: all clean
