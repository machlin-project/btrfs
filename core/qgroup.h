/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_QGROUP_H
#define MACHLIN_BTRFS_QGROUP_H

#include "transaction.h"

/* Linux's drop_subtree_threshold default: a deleted subvolume's shared subtree
 * rooted at this level or above is not traced; quotas become inconsistent. */
#define BT_QGROUP_SUBTREE_LEVEL 3U

/* Loads the quota tree of the transaction's base into transaction->qgroups
 * and observes the mutation's extent changes. Admission refuses simple quotas,
 * a running rescan and disabled quotas (UNSUPPORTED). A status generation that
 * is not the base's, or a qgroup missing one of its items, marks quotas
 * inconsistent, which stops accounting as Linux's NO_ACCOUNTING does. */
enum btrfs_result bt_qgroup_begin(struct btrfs_transaction *transaction);
void bt_qgroup_end(struct btrfs_transaction *transaction);
/* Accounts every observed extent from the subvolume trees reaching it in the
 * base and in the transaction's view (btrfs_qgroup_account_extent), then
 * stores changed qgroups, removes those of fully dropped subvolumes and
 * updates the status item. Runs once, after every subvolume tree edit and
 * reference update of the commit. */
enum btrfs_result bt_qgroup_commit(struct btrfs_transaction *transaction);
/* The qgroup of a new subvolume: zeroed INFO and LIMIT items. */
enum btrfs_result bt_qgroup_create(struct btrfs_transaction *transaction, uint64_t subvolume);
/* Admits bytes more for tree's qgroup and every qgroup above it, as Linux's
 * qgroup_reserve: QUOTA_EXCEEDED when a referenced or exclusive limit would be
 * passed. Admitted bytes count until the transaction ends. */
enum btrfs_result bt_qgroup_reserve(
    struct btrfs_transaction *transaction, uint64_t tree, uint64_t bytes);
/* Traces every extent of a deleted subvolume's shared subtree that the drop
 * leaves (btrfs_qgroup_trace_subtree): its root sets lose the subvolume. A
 * subtree at BT_QGROUP_SUBTREE_LEVEL or above marks quotas inconsistent. */
enum btrfs_result bt_qgroup_trace_subtree(
    struct btrfs_transaction *transaction, struct bt_root block);
/* A snapshot target of source, whose root copy is the committed root of
 * source: btrfs_qgroup_inherit's counts at commit. The snapshot must be the
 * transaction's first change, so that the state it copies is the base with
 * the copy added, and accounting needs no other intermediate state. */
enum btrfs_result bt_qgroup_snapshot(struct btrfs_transaction *transaction, uint64_t source,
    uint64_t target, uint64_t source_root, uint64_t copy_root);
/* A deleted subvolume whose drop completed: its qgroup goes at commit. */
enum btrfs_result bt_qgroup_dropped(struct btrfs_transaction *transaction, uint64_t subvolume);
/* The lowest subvolume id above every level-0 qgroup, or 0 without quotas:
 * Linux never reuses the id of a qgroup that outlived its subvolume. */
uint64_t bt_qgroup_next_id(const struct btrfs_transaction *transaction);

#endif
