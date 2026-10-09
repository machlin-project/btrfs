/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_VERITY_H
#define MACHLIN_BTRFS_VERITY_H

#include "internal.h"

/* A file's fs-verity parameters, as Linux's fsverity_init_merkle_tree_params
 * derives them from its descriptor. Level 0 holds the hashes of data blocks;
 * the tree stores its highest level first, at level_start blocks. */
struct bt_verity {
	struct bt_disk_verity_descriptor descriptor;
	/* The hash state after the salt, zero-padded to the hash's block. */
	struct bt_sha2 salted;
	uint64_t level_start[BT_VERITY_LEVELS_MAX];
	uint64_t tree_size;
	uint32_t descriptor_size;
	uint32_t block_size;
	unsigned log_block;
	unsigned log_arity;
	unsigned levels;
	unsigned digest_size;
};

/* Validates the descriptor's hash algorithm (UNSUPPORTED when unknown), block
 * size (1 KiB up to a sector) and tree depth, and derives the rest. */
enum btrfs_result bt_verity_parameters(struct bt_verity *verity, uint32_t sector_size);
/* The salted hash of one data or tree block of verity->block_size bytes. */
void bt_verity_hash(const struct bt_verity *verity, const uint8_t *block, uint8_t *digest);

#endif
