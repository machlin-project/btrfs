/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_CSUM_H
#define MACHLIN_BTRFS_CSUM_H

#include "mutable.h"

/* Data checksum items in a private checksum tree: one checksum of the
 * filesystem's algorithm per sector keyed (EXTENT_CSUM_OBJECTID, EXTENT_CSUM,
 * first logical byte). Items never overlap and hold at most Linux's per-item
 * checksum count. Insertion refuses a range
 * that already has checksums; deletion trims, splits or removes covering items. */
enum btrfs_result bt_csum_insert(struct bt_mutation *mutation, struct bt_root *checksums,
    uint64_t logical, const uint8_t *data, uint64_t length);
/* Whether any checksum covers part of [logical, logical + length). */
enum btrfs_result bt_csum_exists(struct bt_mutation *mutation, struct bt_root checksums,
    uint64_t logical, uint64_t length, int *exists);
enum btrfs_result bt_csum_delete(
    struct bt_mutation *mutation, struct bt_root *checksums, uint64_t logical, uint64_t length);

#endif
