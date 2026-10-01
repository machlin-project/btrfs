/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_NAMESPACE_H
#define MACHLIN_BTRFS_NAMESPACE_H

#include "transaction.h"

/* Linux's PATH_MAX less the terminating NUL. */
#define BT_SYMLINK_LIMIT 4095U
#define BT_NANOSECONDS 1000000000U
#define BT_MODE_PERMISSIONS 07777U
#define BT_NAME_HASH_SEED (UINT32_MAX - 1U)
#define BT_PROPERTY_PREFIX "btrfs."
#define BT_COMPRESSION_PROPERTY "btrfs.compression"
#define BT_CAPABILITY_XATTR "security.capability"
#define BT_MODE_SET_UID 04000U
#define BT_MODE_SET_GID 02000U
#define BT_MODE_GROUP_EXECUTE 00010U
#define BT_ATTRIBUTE_MASK                                                                          \
	(BTRFS_ATTRIBUTE_MODE | BTRFS_ATTRIBUTE_UID | BTRFS_ATTRIBUTE_GID |                        \
	    BTRFS_ATTRIBUTE_ACCESS_TIME | BTRFS_ATTRIBUTE_MODIFY_TIME |                            \
	    BTRFS_ATTRIBUTE_REMOVE_CAPABILITY)

/* One name in a directory, resolved through DIR_ITEM, its back reference
 * (INODE_REF, or INODE_EXTREF when extended) and DIR_INDEX. */
struct bt_entry {
	uint64_t inode;
	uint64_t index;
	uint8_t type;
	int extended;
};

/* A name removed before another is added in one operation (a rename's old
 * name): its bytes leave the items it shares with the new name. */
struct bt_removed {
	uint64_t directory;
	const void *name;
	size_t length;
	int extended;
};

/* A packed item (DIR_ITEM, DIR_INDEX, INODE_REF or XATTR_ITEM) loaded into the
 * transaction's namespace item buffer. An absent item has size 0. */
struct bt_packed {
	struct bt_key key;
	uint8_t *bytes;
	size_t size;
	int present;
};

struct bt_codec {
	const char *name;
	uint64_t feature;
};

/* Namespace records shared by the namespace and subvolume editors
 * (core/namespace.c). */
uint8_t bt_ns_type(uint32_t mode);
int bt_ns_is_directory(const struct bt_disk_inode *item);
int bt_ns_immutable(const struct bt_disk_inode *item);
int bt_ns_frozen(const struct bt_disk_inode *item);
size_t bt_ns_length(const char *text);
uint64_t bt_ns_hash(const void *name, size_t length);
enum btrfs_result bt_ns_name(const void *name, size_t length);
size_t bt_ns_item_limit(const struct btrfs_transaction *transaction);
uint64_t bt_ns_transid(const struct btrfs_transaction *transaction);
enum btrfs_result bt_ns_neighbor(struct btrfs_transaction *transaction,
    const struct bt_owned_root *tree, struct bt_key key, int before, struct bt_key *result,
    int *found);
enum btrfs_result bt_ns_load(struct btrfs_transaction *transaction,
    const struct bt_owned_root *tree, struct bt_key key, struct bt_packed *packed);
enum btrfs_result bt_ns_dir_find(const struct bt_packed *packed, const void *name, size_t length,
    size_t *offset, size_t *entry_size, const struct bt_disk_dir **header);
enum btrfs_result bt_ns_append(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    struct bt_packed *packed, const uint8_t *entry, size_t entry_size);
enum btrfs_result bt_ns_cut(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    struct bt_packed *packed, size_t offset, size_t length);
size_t bt_ns_dir_entry(uint8_t *out, struct bt_key location, uint64_t transid, const void *name,
    size_t name_length, const void *data, size_t data_length, uint8_t type);
enum btrfs_result bt_ns_inode(struct btrfs_transaction *transaction,
    const struct bt_owned_root *tree, uint64_t inode, struct bt_disk_inode *item);
enum btrfs_result bt_ns_parent(struct btrfs_transaction *transaction,
    const struct bt_owned_root *tree, uint64_t directory, struct bt_disk_inode *item);
enum btrfs_result bt_ns_store(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    uint64_t inode, struct bt_disk_inode *item, struct btrfs_time time);
enum btrfs_result bt_ns_directory(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    uint64_t directory, size_t length, int add, struct btrfs_time time);
enum btrfs_result bt_ns_index(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    uint64_t directory, uint64_t *result);
void bt_ns_new_directory(struct btrfs_transaction *transaction, uint64_t tree, uint64_t directory);
enum btrfs_result bt_ns_absent(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    uint64_t directory, const void *name, size_t length);
enum btrfs_result bt_ns_room(struct btrfs_transaction *transaction, struct bt_owned_root *tree,
    uint64_t directory, const void *name, size_t length, int item, uint64_t inode,
    const struct bt_removed *removed);
enum btrfs_result bt_ns_insert_entry(struct btrfs_transaction *transaction,
    struct bt_owned_root *tree, uint64_t directory, const void *name, size_t length,
    struct bt_key location, uint8_t type, uint64_t index);
enum btrfs_result bt_ns_inherited_codec(struct btrfs_transaction *transaction,
    struct bt_owned_root *tree, uint64_t directory, uint64_t flags, const struct bt_codec **codec);
int bt_ns_can_compress(uint64_t flags);
void bt_ns_require_feature(struct btrfs_transaction *transaction, uint64_t feature);
enum btrfs_result bt_ns_begin(
    struct btrfs_transaction *transaction, uint64_t tree_id, struct bt_owned_root **tree);
enum btrfs_result bt_ns_poison(struct btrfs_transaction *transaction, enum btrfs_result error);

#endif
