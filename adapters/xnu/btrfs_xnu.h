/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_XNU_H
#define MACHLIN_BTRFS_XNU_H

#include <btrfs/btrfs.h>
#include <btrfs/identity.h>
#include <btrfs/native.h>
#include <btrfs/staging.h>
#include <btrfs/volume.h>
#include <kern/locks.h>
#include <kern/thread_call.h>
#include <sys/mount.h>
#include <sys/queue.h>
#include <sys/vnode.h>

#define BTRFS_XNU_NAME "machlin_btrfs"
/* Node hash buckets per mount: one for every eight vnodes the system keeps,
 * at least the minimum. */
#define BTRFS_XNU_VNODES_PER_BUCKET 8
#define BTRFS_XNU_MINIMUM_BUCKETS 128
/* Node cache per mount: 4,096 nodes of 4 KiB or 1,024 of 16 KiB. */
#define BTRFS_XNU_CACHE_BYTES (16U * 1024U * 1024U)
/* Seconds between commits of the running transaction (Linux's commit=). */
#define BTRFS_XNU_COMMIT_SECONDS 5U
/* Tree nodes one namespace or attribute operation declares when it joins the
 * running transaction, and the file bytes a data write declares per node. */
#define BTRFS_XNU_OPERATION_NODES 64U
#define BTRFS_XNU_DATA_BYTES_PER_NODE (64U * 1024U)
/* Directory records one readdir gathers before copying them out. */
#define BTRFS_XNU_READDIR_BATCH (32U * 1024U)

struct btrfs_xnu_node;

LIST_HEAD(btrfs_xnu_node_head, btrfs_xnu_node);

struct btrfs_xnu_mount {
	mount_t mount;
	vnode_t device;
	struct btrfs_volume *volume;
	struct btrfs_identity_table *identities;
	/* Geometry and identity of the filesystem; usage changes per view. */
	struct btrfs_info info;
	uint32_t device_block_size;
	uint64_t device_bytes;
	lck_mtx_t *nodes_lock;
	lck_mtx_t *creation_lock;
	lck_mtx_t *volume_lock;
	/* MNT_SYNCHRONOUS: each operation commits before it returns. Otherwise
	 * operations share the volume's running transaction, which fsync, sync,
	 * the periodic committer and unmount commit. */
	int synchronous;
	thread_call_t committer;
	/* Set while unmounting: the committer does not rearm. */
	int stopping;
	/* Set (nodes_lock) while the committer is scheduled or running; cleared
	 * by a cancel that removed it or by its last tick, which wakes unmount. */
	int committer_armed;
	/* Operations applied through this mount (nodes_lock); a node rereads its
	 * inode when the count moved. */
	uint64_t changes;
	/* Verified tree nodes shared by every view; only this mount changes the
	 * device while it is mounted. */
	struct btrfs_cache *cache;
	lck_mtx_t *cache_lock;
	/* Device write combining, below the core and UBC. Shared for reads;
	 * exclusive for staging and a complete drain/device barrier. */
	struct btrfs_staging *staging;
	lck_rw_t *staging_lock;
	/* Nodes by native number (nodes_lock); hashinit sizes it. */
	struct btrfs_xnu_node_head *nodes;
	u_long nodes_mask;
};

/* A vnode's object. inode is the newest state read; nodes_lock guards it and
 * the fields below it. A regular file's size is its logical size, which
 * includes cached writes that pageout has not applied yet. */
struct btrfs_xnu_node {
	LIST_ENTRY(btrfs_xnu_node) hash;
	struct btrfs_xnu_mount *mount;
	struct btrfs_inode inode;
	/* The mount's change count when inode was read. */
	uint64_t seen;
	/* The generation that publishes this object's last change; fsync waits
	 * for it. */
	uint64_t pending;
	uint64_t number;
	vnode_t vnode;
	uint32_t vid;
	int hashed;
	uint64_t size;
	/* The end of the write in progress. cluster_write may push its pages
	 * before size covers them; strategy and pageout keep them. */
	uint64_t push_end;
	struct btrfs_time modified;
	/* A privileged writer keeps set-id bits and file capabilities. */
	int privileged;
	/* Unlinked while open: an orphan item keeps the inode until inactive. */
	int orphan;
	/* The first failed commit of cached data, reported by fsync. */
	int write_error;
	/* Serializes writers and truncation of the file's logical size. Strategy
	 * writes never take it: cluster_write may push them while it is held. */
	lck_mtx_t *write_lock;
};

extern lck_grp_t *btrfs_xnu_locks;
extern struct vfsops btrfs_xnu_vfsops;
extern struct vnodeopv_desc btrfs_xnu_vnodeops;

int btrfs_xnu_error(enum btrfs_result result);
int btrfs_xnu_get_node(struct btrfs_xnu_mount *mount, const struct btrfs_inode *inode,
    vnode_t parent, struct componentname *name, vnode_t *result);
int btrfs_xnu_root_node(struct btrfs_xnu_mount *mount, vnode_t *result);
int btrfs_xnu_number_node(struct btrfs_xnu_mount *mount, uint64_t number, vnode_t *result);
/* Reads the node's inode from the volume's newest state; the caller ends the
 * read with btrfs_volume_unread before copying to user space or writing. */
int btrfs_xnu_read_inode(struct btrfs_xnu_node *node, const struct btrfs_fs **fs,
    struct btrfs_volume_view **view, struct btrfs_inode *inode);
/* Refreshes the cached inode after operations; keeps the logical size of a
 * file with cached writes. */
int btrfs_xnu_refresh(struct btrfs_xnu_node *node);
void btrfs_xnu_now(struct btrfs_time *time);
enum vtype btrfs_xnu_type(uint32_t mode);
uint32_t btrfs_xnu_inode_flags(uint64_t flags);
/* Pushes cached writes of every vnode of the mount and commits them. */
int btrfs_xnu_sync_all(mount_t mount, int wait);
/* Makes generation durable (grouped mounts). */
int btrfs_xnu_commit(struct btrfs_xnu_mount *mount, uint64_t generation);
int btrfs_xnu_push_data(vnode_t vnode, int wait);

/* Write operations (write.c). */
int btrfs_xnu_create(void *arguments);
int btrfs_xnu_mkdir(void *arguments);
int btrfs_xnu_symlink(void *arguments);
int btrfs_xnu_link(void *arguments);
int btrfs_xnu_remove(void *arguments);
int btrfs_xnu_rmdir(void *arguments);
int btrfs_xnu_rename(void *arguments);
int btrfs_xnu_setattr(void *arguments);
int btrfs_xnu_preallocate(void *arguments);
int btrfs_xnu_ioctl(void *arguments);
int btrfs_xnu_write(void *arguments);
int btrfs_xnu_setxattr(void *arguments);
int btrfs_xnu_removexattr(void *arguments);
int btrfs_xnu_fsync(void *arguments);
int btrfs_xnu_pageout(void *arguments);
int btrfs_xnu_inactive(void *arguments);
/* Commits a strategy write of cached file data. */
int btrfs_xnu_strategy_write(struct btrfs_xnu_node *node, buf_t buffer);

#endif
