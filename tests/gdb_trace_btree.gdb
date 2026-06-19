# gdb_trace_btree.gdb
# 追踪 64KB 写入对 5 个 btree 的更新
# Usage: gdb -batch -x gdb_trace_btree.gdb ./bchfs_write64k
set pagination off
set confirm off

# === B1: 入口，在 bchfs_write 调用前激活所有断点 ===
b bchfs_write64k.c:62
commands 1
  silent
  printf "=== 64KB WRITE: enabling all trigger breakpoints ===\n"
  enable 2
  enable 3
  enable 4
  enable 5
  enable 6
  enable 7
  enable 8
  c
end

# === B2: 桶分配（初始禁用）===
b bch2_bucket_alloc_trans
disable 2
commands 2
  silent
  printf "--- ALLOC: bucket_alloc_trans ---\n"
  printf "  data_type=%u watermark=%u\n", req->data_type, req->watermark
  c
end

# === B3: extent trigger（初始禁用）===
b bch2_trigger_extent
disable 3
commands 3
  silent
  printf "--- EXTENT TRIGGER btree=%u level=%u old_type=%u old_size=%u new_type=%u new_size=%u ---\n", \
    op.btree, op.level, \
    op.old.k->type, op.old.k->size, \
    op.new.k->type, op.new.k->size
  c
end

# === B4: trigger_pointer（初始禁用）===
b fs/alloc/buckets.c:655
disable 4
commands 4
  silent
  printf "--- TRIGGER_POINTER dev=%u bkt=%llu gen=%u cached=%d len=%lld ---\n", \
    p.ptr.dev, p.ptr.offset, p.ptr.gen, p.ptr.cached, bp.v.bucket_len
  c
end

# === B5: backpointer（初始禁用）===
b bch2_bucket_backpointer_mod
disable 5
commands 5
  silent
  printf "--- BACKPOINTER ---\n"
  c
end

# === B6: alloc trigger（初始禁用）===
b fs/alloc/background.c:1253
disable 6
commands 6
  silent
  printf "--- TRIGGER_ALLOC ---\n"
  printf "  dev=%llu bucket=%llu\n", op.new.k->p.inode, op.new.k->p.offset
  printf "  old: data_type=%u dirty=%u cached=%u stripe=%u gen=%u\n", \
    old_a->data_type, old_a->dirty_sectors, old_a->cached_sectors, old_a->stripe_sectors, old_a->gen
  printf "  new: data_type=%u dirty=%u cached=%u stripe=%u gen=%u flags=%u\n", \
    new_a->data_type, new_a->dirty_sectors, new_a->cached_sectors, new_a->stripe_sectors, new_a->gen, new_a->flags
  c
end

# === B7: LRU（初始禁用）===
b __bch2_lru_change
disable 7
commands 7
  silent
  printf "--- LRU ---\n"
  printf "  lru_id=%u dev_bucket=%llu old=%llu new=%llu\n", lru_id, dev_bucket, old_time, new_time
  c
end

# === B8: accounting（初始禁用）===
b bch2_disk_accounting_mod
disable 8
commands 8
  silent
  printf "--- ACCT type=%d delta0=%lld ---\n", k->type, d[0]
  c
end

run /tmp/test_alloc.bcachefs
quit
