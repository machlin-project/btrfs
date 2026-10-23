/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_TOOLS_BALANCE_ARGS_H
#define MACHLIN_BTRFS_TOOLS_BALANCE_ARGS_H

#include <btrfs/write.h>

/* btrfs balance start's selection as btrfs-progs takes it: -d, -m and -s
 * with comma-separated usage=PERCENT, convert=PROFILE and soft, -m applying
 * to system groups too unless -s is given, none selecting every group, and
 * -f to reduce metadata redundancy. -1 for anything else. */
int tool_balance_args(int argc, char **argv, struct btrfs_balance_args *args);

#endif
