/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_XNU_H
#define MACHLIN_BTRFS_XNU_H

#include <btrfs/btrfs.h>
#include <btrfs/identity.h>
#include <kern/locks.h>
#include <sys/mount.h>
#include <sys/queue.h>
#include <sys/vnode.h>

#define BTRFS_XNU_NAME "machlin_btrfs"
#define BTRFS_XNU_HASH_SIZE 128U

struct btrfs_xnu_node;

LIST_HEAD(btrfs_xnu_node_head, btrfs_xnu_node);

struct btrfs_xnu_mount {
	mount_t mount;
	vnode_t device;
	struct btrfs_fs *fs;
	struct btrfs_identity_table *identities;
	struct btrfs_info info;
	uint32_t device_block_size;
	lck_mtx_t *nodes_lock;
	lck_mtx_t *creation_lock;
	struct btrfs_xnu_node_head nodes[BTRFS_XNU_HASH_SIZE];
};

struct btrfs_xnu_node {
	LIST_ENTRY(btrfs_xnu_node) hash;
	struct btrfs_xnu_mount *mount;
	struct btrfs_inode inode;
	uint64_t number;
	vnode_t vnode;
	uint32_t vid;
};

extern lck_grp_t *btrfs_xnu_locks;
extern struct vfsops btrfs_xnu_vfsops;
extern struct vnodeopv_desc btrfs_xnu_vnodeops;

int btrfs_xnu_error(enum btrfs_result result);
int btrfs_xnu_get_node(struct btrfs_xnu_mount *mount, uint64_t number, vnode_t parent,
    struct componentname *name, vnode_t *result);

#endif
