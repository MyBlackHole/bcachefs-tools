# 展开宏
gcc -I./ -I./include -E -P  c_src/cmd_format.c > cmd_format.h

sudo ./bcachefs fusemount /dev/sdb /run/media/black/bcachefs_test

BCACHEFS_FUSE=1 make debug
