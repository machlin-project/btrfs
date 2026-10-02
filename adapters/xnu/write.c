/* SPDX-License-Identifier: BSD-3-Clause */
/* Native writes. On a synchronous mount every namespace and attribute change
 * is one transaction committed before the operation returns; otherwise
 * operations join the volume's running transaction and become durable when
 * fsync, sync, the periodic committer or unmount commits it. File data goes
 * through the unified buffer cache: write() and mmap dirty cached pages, and
 * pageout, fsync and sync push them to strategy, which applies each pushed
 * range copy-on-write in the running transaction on every mount. A
 * synchronous write (O_SYNC, O_DSYNC or a synchronous mount) pushes and
 * commits its data once before it returns. A file's logical size covers cached
 * data that is not applied yet. */
#include "btrfs_xnu.h"

#include <sys/buf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/ubc.h>
#include <sys/uio.h>
#include <sys/vnode_if.h>
#include <sys/xattr.h>

static const char btrfs_xnu_default_acl[] = "system.posix_acl_default";
static const char btrfs_xnu_capability[] = "security.capability";

/* Linux's should_remove_suid and file capability: what a write by an
 * unprivileged caller removes. */
#define BTRFS_XNU_SET_ID ((uint32_t)(S_ISUID | S_ISGID))
#define BTRFS_XNU_SGID_EXECUTE ((uint32_t)(S_ISGID | S_IXGRP))

/* Starts an operation that changes at most nodes tree nodes: alone in its own
 * transaction, or in the running one. */
static enum btrfs_result
btrfs_xnu_start(
    struct btrfs_xnu_mount *mount, int alone, size_t nodes, struct btrfs_transaction **transaction)
{
	if (alone) {
		return btrfs_volume_begin(mount->volume, transaction);
	}
	return btrfs_volume_join(mount->volume, nodes, transaction);
}

/* Ends an operation with its result: one alone is committed or aborted,
 * otherwise it leaves the running transaction. A successful operation marks
 * first and second (when not NULL) with the generation that publishes it, for
 * fsync. */
static enum btrfs_result
btrfs_xnu_stop(struct btrfs_xnu_mount *mount, int alone, struct btrfs_transaction *transaction,
    enum btrfs_result result, struct btrfs_xnu_node *first, struct btrfs_xnu_node *second)
{
	uint64_t pending = btrfs_volume_pending(mount->volume);

	if (alone) {
		if (result != BTRFS_OK) {
			btrfs_volume_abort(mount->volume, transaction);
			return result;
		}
		result = btrfs_volume_commit(mount->volume, transaction);
	} else {
		btrfs_volume_leave(mount->volume, transaction);
	}
	if (result == BTRFS_OK) {
		lck_mtx_lock(mount->nodes_lock);
		mount->changes++;
		if (first != NULL) {
			first->pending = pending;
		}
		if (second != NULL) {
			second->pending = pending;
		}
		lck_mtx_unlock(mount->nodes_lock);
	}
	return result;
}

/* A namespace or attribute operation, alone on a synchronous mount. */
static enum btrfs_result
btrfs_xnu_begin(struct btrfs_xnu_mount *mount, size_t nodes, struct btrfs_transaction **transaction)
{
	return btrfs_xnu_start(mount, mount->synchronous, nodes, transaction);
}

static enum btrfs_result
btrfs_xnu_end(struct btrfs_xnu_mount *mount, struct btrfs_transaction *transaction,
    enum btrfs_result result, struct btrfs_xnu_node *first, struct btrfs_xnu_node *second)
{
	return btrfs_xnu_stop(mount, mount->synchronous, transaction, result, first, second);
}

static int
btrfs_xnu_finish(struct btrfs_xnu_mount *mount, struct btrfs_transaction *transaction,
    enum btrfs_result result, struct btrfs_xnu_node *first, struct btrfs_xnu_node *second)
{
	return btrfs_xnu_error(btrfs_xnu_end(mount, transaction, result, first, second));
}

static int
btrfs_xnu_has_xattr(struct btrfs_xnu_node *node, const char *name, size_t length, int *present)
{
	struct btrfs_volume_view *view;
	struct btrfs_inode inode;
	const struct btrfs_fs *fs;
	size_t size;
	enum btrfs_result result;
	int error;

	*present = 0;
	error = btrfs_xnu_read_inode(node, &fs, &view, &inode);
	if (error != 0) {
		return error;
	}
	result = btrfs_get_xattr(fs, &inode, name, length, NULL, 0, &size);
	btrfs_volume_unread(node->mount->volume, view);
	*present = result == BTRFS_OK;
	return result == BTRFS_OK || result == BTRFS_NOT_FOUND ? 0 : btrfs_xnu_error(result);
}

/* A write or truncation by an unprivileged caller removes set-id bits and the
 * file capability now, in its own transaction; a superuser's keeps them for
 * the cached data that strategy commits later. */
static int
btrfs_xnu_settle_privileges(struct btrfs_xnu_node *node, vfs_context_t context)
{
	struct btrfs_transaction *transaction;
	struct btrfs_time now;
	uint32_t mode;
	int capability = 0;
	int error;

	error = btrfs_xnu_refresh(node);
	if (error != 0) {
		return error;
	}
	lck_mtx_lock(node->mount->nodes_lock);
	mode = node->inode.mode;
	lck_mtx_unlock(node->mount->nodes_lock);
	error = btrfs_xnu_has_xattr(
	    node, btrfs_xnu_capability, sizeof(btrfs_xnu_capability) - 1, &capability);
	if (error != 0 ||
	    ((mode & S_ISUID) == 0 && (mode & BTRFS_XNU_SGID_EXECUTE) != BTRFS_XNU_SGID_EXECUTE &&
		!capability)) {
		return error;
	}
	lck_mtx_lock(node->mount->nodes_lock);
	node->privileged = vfs_context_suser(context) == 0;
	lck_mtx_unlock(node->mount->nodes_lock);
	if (node->privileged) {
		return 0;
	}
	btrfs_xnu_now(&now);
	error =
	    btrfs_xnu_error(btrfs_xnu_begin(node->mount, BTRFS_XNU_OPERATION_NODES, &transaction));
	if (error == 0) {
		error = btrfs_xnu_finish(node->mount, transaction,
		    btrfs_transaction_drop_privileges(transaction, node->inode.id, now), node,
		    NULL);
	}
	return error;
}

/* Applies [offset, offset + size) of a regular file in the running
 * transaction; the write, fsync or commit that needs it durable commits it. */
static enum btrfs_result
btrfs_xnu_commit_data(struct btrfs_xnu_node *node, uint64_t offset, const void *bytes, size_t size)
{
	struct btrfs_transaction *transaction;
	struct btrfs_time modified;
	int privileged;
	enum btrfs_result result;

	lck_mtx_lock(node->mount->nodes_lock);
	modified = node->modified;
	privileged = node->privileged;
	lck_mtx_unlock(node->mount->nodes_lock);
	result = btrfs_xnu_start(node->mount, 0,
	    BTRFS_XNU_OPERATION_NODES + size / BTRFS_XNU_DATA_BYTES_PER_NODE, &transaction);
	if (result != BTRFS_OK) {
		return result;
	}
	/* Without a privileged writer, data never keeps set-id bits. */
	result = privileged
	    ? btrfs_transaction_keep_privileges(transaction, node->inode.id)
	    : btrfs_transaction_drop_privileges(transaction, node->inode.id, modified);
	if (result == BTRFS_OK) {
		result = btrfs_transaction_write(
		    transaction, node->inode.id, offset, bytes, size, modified);
	}
	return btrfs_xnu_stop(node->mount, 0, transaction, result, node, NULL);
}

/* Pushes cached data, including pages dirtied through a mapping, then commits
 * the generation that publishes the object's last change (nothing to do once
 * it is durable). cluster_push_err returns how many clusters it pushed and
 * reports a failure through its last argument. */
static int
btrfs_xnu_sync_node(struct btrfs_xnu_node *node, vnode_t vnode, int wait)
{
	uint64_t pending;
	int error = 0;

	if (vnode_vtype(vnode) == VREG && btrfs_volume_writable(node->mount->volume)) {
		(void)cluster_push_err(vnode, wait ? IO_SYNC : 0, NULL, NULL, &error);
		if (error == 0) {
			error = ubc_msync(vnode, 0, ubc_getsize(vnode), NULL,
			    UBC_PUSHDIRTY | (wait ? UBC_SYNC : 0));
		}
		lck_mtx_lock(node->mount->nodes_lock);
		/* The operation's own failure (ENOSPC, EIO) explains a failed push. */
		if (node->write_error != 0) {
			error = node->write_error;
		}
		node->write_error = 0;
		lck_mtx_unlock(node->mount->nodes_lock);
	}
	if (error == 0 && btrfs_volume_writable(node->mount->volume)) {
		lck_mtx_lock(node->mount->nodes_lock);
		pending = node->pending;
		lck_mtx_unlock(node->mount->nodes_lock);
		error = btrfs_xnu_commit(node->mount, pending);
	}
	if (error == 0) {
		error = btrfs_xnu_error(btrfs_volume_failure(node->mount->volume));
	}
	return error;
}

int
btrfs_xnu_strategy_write(struct btrfs_xnu_node *node, buf_t buffer)
{
	caddr_t address = NULL;
	size_t length = buf_count(buffer);
	size_t amount;
	daddr64_t block = buf_blkno(buffer);
	uint64_t offset;
	uint64_t size;
	int error;

	if (block < 0 || (uint64_t)block > INT64_MAX / node->mount->device_block_size) {
		error = EINVAL;
	} else {
		offset = (uint64_t)block * node->mount->device_block_size;
		lck_mtx_lock(node->mount->nodes_lock);
		size = node->size > node->push_end ? node->size : node->push_end;
		lck_mtx_unlock(node->mount->nodes_lock);
		/* Bytes of the last page beyond the logical size (or the end of the
		 * write in progress) are not data. */
		amount = offset >= size
		    ? 0
		    : (size - offset < length ? (size_t)(size - offset) : length);
		error = amount == 0 ? 0 : buf_map(buffer, &address);
		if (error == 0 && amount != 0) {
			error =
			    btrfs_xnu_error(btrfs_xnu_commit_data(node, offset, address, amount));
			buf_unmap(buffer);
		}
	}
	if (error != 0) {
		lck_mtx_lock(node->mount->nodes_lock);
		if (node->write_error == 0) {
			node->write_error = error;
		}
		lck_mtx_unlock(node->mount->nodes_lock);
	}
	buf_setresid(buffer, error == 0 ? 0 : (uint32_t)length);
	buf_seterror(buffer, error);
	buf_biodone(buffer);
	return error;
}

int
btrfs_xnu_write(void *arguments)
{
	struct vnop_write_args *args = arguments;
	struct btrfs_xnu_node *node = vnode_fsnode(args->a_vp);
	user_ssize_t residual = uio_resid(args->a_uio);
	uint64_t old_size;
	uint64_t offset;
	uint64_t end;
	off_t head = 0;
	int flags = args->a_ioflag;
	int error;

	if (vnode_vtype(args->a_vp) != VREG) {
		return vnode_vtype(args->a_vp) == VDIR ? EISDIR : EINVAL;
	}
	if (!btrfs_volume_writable(node->mount->volume)) {
		return EROFS;
	}
	if (uio_offset(args->a_uio) < 0 || residual < 0) {
		return EINVAL;
	}
	if (residual == 0) {
		return 0;
	}
	error = btrfs_xnu_settle_privileges(node, args->a_context);
	if (error != 0) {
		return error;
	}
	lck_mtx_lock(node->write_lock);
	lck_mtx_lock(node->mount->nodes_lock);
	old_size = node->size;
	lck_mtx_unlock(node->mount->nodes_lock);
	if (flags & IO_APPEND) {
		uio_setoffset(args->a_uio, (off_t)old_size);
	}
	offset = (uint64_t)uio_offset(args->a_uio);
	if ((uint64_t)residual > INT64_MAX - offset) {
		lck_mtx_unlock(node->write_lock);
		return EFBIG;
	}
	end = offset + (uint64_t)residual;
	/* Cached pages between the old end and the write start read as zeros. */
	if (offset > old_size) {
		head = (off_t)old_size;
		flags |= IO_HEADZEROFILL;
	}
	lck_mtx_lock(node->mount->nodes_lock);
	node->push_end = end;
	lck_mtx_unlock(node->mount->nodes_lock);
	error = cluster_write(args->a_vp, args->a_uio, (off_t)old_size,
	    (off_t)(end > old_size ? end : old_size), head, 0, flags | IO_NOZERODIRTY);
	lck_mtx_lock(node->mount->nodes_lock);
	/* Pages pushed later lie below what the write copied, which size now
	 * covers. */
	if ((uint64_t)uio_offset(args->a_uio) > node->size) {
		node->size = (uint64_t)uio_offset(args->a_uio);
	}
	node->push_end = 0;
	end = node->size;
	btrfs_xnu_now(&node->modified);
	lck_mtx_unlock(node->mount->nodes_lock);
	if (end > old_size) {
		ubc_setsize(args->a_vp, (off_t)end);
	}
	lck_mtx_unlock(node->write_lock);
	/* O_SYNC and O_DSYNC writes are durable when they return. */
	if (error == 0 && (args->a_ioflag & IO_SYNC)) {
		error = btrfs_xnu_sync_node(node, args->a_vp, 1);
	}
	return error;
}

int
btrfs_xnu_pageout(void *arguments)
{
	struct vnop_pageout_args *args = arguments;
	struct btrfs_xnu_node *node = vnode_fsnode(args->a_vp);
	uint64_t size;

	lck_mtx_lock(node->mount->nodes_lock);
	size = node->size > node->push_end ? node->size : node->push_end;
	lck_mtx_unlock(node->mount->nodes_lock);
	return cluster_pageout(args->a_vp, args->a_pl, args->a_pl_offset, args->a_f_offset,
	    (int)args->a_size, (off_t)size, args->a_flags);
}

int
btrfs_xnu_fsync(void *arguments)
{
	struct vnop_fsync_args *args = arguments;

	return btrfs_xnu_sync_node(
	    vnode_fsnode(args->a_vp), args->a_vp, args->a_waitfor == MNT_WAIT);
}

static int
btrfs_xnu_truncate(struct btrfs_xnu_node *node, vnode_t vnode, uint64_t size, vfs_context_t context)
{
	struct btrfs_transaction *transaction;
	struct btrfs_time now;
	uint64_t old_size;
	enum btrfs_result result;
	int error;

	error = btrfs_xnu_settle_privileges(node, context);
	if (error != 0) {
		return error;
	}
	btrfs_xnu_now(&now);
	lck_mtx_lock(node->write_lock);
	lck_mtx_lock(node->mount->nodes_lock);
	old_size = node->size;
	node->modified = now;
	lck_mtx_unlock(node->mount->nodes_lock);
	/* Cached data beyond a smaller size goes before the commit drops it. */
	if (size < old_size) {
		ubc_setsize(vnode, (off_t)size);
	}
	error =
	    btrfs_xnu_error(btrfs_xnu_begin(node->mount, BTRFS_XNU_OPERATION_NODES, &transaction));
	if (error == 0) {
		result = node->privileged
		    ? btrfs_transaction_keep_privileges(transaction, node->inode.id)
		    : btrfs_transaction_drop_privileges(transaction, node->inode.id, now);
		if (result == BTRFS_OK) {
			result = btrfs_transaction_truncate(transaction, node->inode.id, size, now);
		}
		error = btrfs_xnu_finish(node->mount, transaction, result, node, NULL);
	}
	if (error == 0) {
		lck_mtx_lock(node->mount->nodes_lock);
		node->size = size;
		lck_mtx_unlock(node->mount->nodes_lock);
		if (size > old_size) {
			ubc_setsize(vnode, (off_t)size);
		}
	} else if (size < old_size) {
		ubc_setsize(vnode, (off_t)old_size);
	}
	lck_mtx_unlock(node->write_lock);
	return error;
}

int
btrfs_xnu_setattr(void *arguments)
{
	struct vnop_setattr_args *args = arguments;
	struct btrfs_xnu_node *node = vnode_fsnode(args->a_vp);
	struct vnode_attr *attributes = args->a_vap;
	struct btrfs_transaction *transaction;
	struct btrfs_attributes changes;
	struct btrfs_inode inode;
	int error;

	if (!btrfs_volume_writable(node->mount->volume)) {
		return EROFS;
	}
	error = btrfs_xnu_refresh(node);
	if (error != 0) {
		return error;
	}
	lck_mtx_lock(node->mount->nodes_lock);
	inode = node->inode;
	lck_mtx_unlock(node->mount->nodes_lock);
	/* Inode flags have no Darwin mapping beyond their current value. */
	if (VATTR_IS_ACTIVE(attributes, va_flags)) {
		if (attributes->va_flags != btrfs_xnu_inode_flags(inode.flags)) {
			return ENOTSUP;
		}
		VATTR_SET_SUPPORTED(attributes, va_flags);
	}
	if (VATTR_IS_ACTIVE(attributes, va_data_size)) {
		if (vnode_vtype(args->a_vp) != VREG) {
			return vnode_vtype(args->a_vp) == VDIR ? EISDIR : EINVAL;
		}
		if (attributes->va_data_size < 0) {
			return EINVAL;
		}
		error = btrfs_xnu_truncate(
		    node, args->a_vp, (uint64_t)attributes->va_data_size, args->a_context);
		if (error != 0) {
			return error;
		}
		VATTR_SET_SUPPORTED(attributes, va_data_size);
	}
	bzero(&changes, sizeof(changes));
	changes.mode = inode.mode & ALLPERMS;
	if (VATTR_IS_ACTIVE(attributes, va_mode)) {
		changes.mask |= BTRFS_ATTRIBUTE_MODE;
		changes.mode = attributes->va_mode & ALLPERMS;
		VATTR_SET_SUPPORTED(attributes, va_mode);
	}
	if (VATTR_IS_ACTIVE(attributes, va_uid) && attributes->va_uid != inode.uid) {
		changes.mask |= BTRFS_ATTRIBUTE_UID;
		changes.uid = attributes->va_uid;
	}
	if (VATTR_IS_ACTIVE(attributes, va_gid) && attributes->va_gid != inode.gid) {
		changes.mask |= BTRFS_ATTRIBUTE_GID;
		changes.gid = attributes->va_gid;
	}
	VATTR_SET_SUPPORTED(attributes, va_uid);
	VATTR_SET_SUPPORTED(attributes, va_gid);
	/* A change of owner removes set-id bits and the file capability unless
	 * the superuser makes it, as Darwin and Linux chown do. */
	if ((changes.mask & (BTRFS_ATTRIBUTE_UID | BTRFS_ATTRIBUTE_GID)) != 0 &&
	    vfs_context_suser(args->a_context) != 0) {
		changes.mask |= BTRFS_ATTRIBUTE_MODE | BTRFS_ATTRIBUTE_REMOVE_CAPABILITY;
		changes.mode &= ~BTRFS_XNU_SET_ID;
	}
	if (VATTR_IS_ACTIVE(attributes, va_access_time)) {
		changes.mask |= BTRFS_ATTRIBUTE_ACCESS_TIME;
		changes.access_time.seconds = attributes->va_access_time.tv_sec;
		changes.access_time.nanoseconds = (uint32_t)attributes->va_access_time.tv_nsec;
		VATTR_SET_SUPPORTED(attributes, va_access_time);
	}
	if (VATTR_IS_ACTIVE(attributes, va_modify_time)) {
		changes.mask |= BTRFS_ATTRIBUTE_MODIFY_TIME;
		changes.modify_time.seconds = attributes->va_modify_time.tv_sec;
		changes.modify_time.nanoseconds = (uint32_t)attributes->va_modify_time.tv_nsec;
		VATTR_SET_SUPPORTED(attributes, va_modify_time);
	}
	if (changes.mask == 0) {
		return 0;
	}
	btrfs_xnu_now(&changes.time);
	error =
	    btrfs_xnu_error(btrfs_xnu_begin(node->mount, BTRFS_XNU_OPERATION_NODES, &transaction));
	if (error == 0) {
		error = btrfs_xnu_finish(node->mount, transaction,
		    btrfs_transaction_set_attributes(transaction, inode.id, &changes), node, NULL);
	}
	if (error == 0 && (changes.mask & BTRFS_ATTRIBUTE_MODIFY_TIME) != 0) {
		lck_mtx_lock(node->mount->nodes_lock);
		node->modified = changes.modify_time;
		lck_mtx_unlock(node->mount->nodes_lock);
	}
	return error;
}

/* Creating below a directory with a default POSIX ACL would need Linux's ACL
 * inheritance, which the native policy does not translate. */
static int
btrfs_xnu_check_parent(struct btrfs_xnu_node *directory)
{
	int present = 0;
	int error;

	error = btrfs_xnu_has_xattr(
	    directory, btrfs_xnu_default_acl, sizeof(btrfs_xnu_default_acl) - 1, &present);
	return error != 0 ? error : (present ? ENOTSUP : 0);
}

static int
btrfs_xnu_make(vnode_t parent, vnode_t *result, struct componentname *name,
    struct vnode_attr *attributes, const char *target, vfs_context_t context)
{
	struct btrfs_xnu_node *directory = vnode_fsnode(parent);
	struct btrfs_xnu_mount *mount = directory->mount;
	struct btrfs_transaction *transaction;
	struct btrfs_volume_view *view;
	struct btrfs_new_inode values;
	struct btrfs_object_id id = { 0, 0 };
	struct btrfs_inode inode;
	const struct btrfs_fs *fs;
	uint64_t pending = 0;
	uint32_t type;
	int error;

	*result = NULL;
	if (!btrfs_volume_writable(mount->volume)) {
		return EROFS;
	}
	switch (attributes->va_type) {
	case VREG:
		type = BTRFS_MODE_REGULAR;
		break;
	case VDIR:
		type = BTRFS_MODE_DIRECTORY;
		break;
	case VLNK:
		type = BTRFS_MODE_SYMLINK;
		break;
	default:
		/* Device, FIFO and socket vnodes need special-file operations. */
		return ENOTSUP;
	}
	error = btrfs_xnu_check_parent(directory);
	if (error == 0) {
		error = btrfs_xnu_refresh(directory);
	}
	if (error != 0) {
		return error;
	}
	bzero(&values, sizeof(values));
	values.mode = type |
	    ((VATTR_IS_ACTIVE(attributes, va_mode) ? attributes->va_mode : ACCESSPERMS) & ALLPERMS);
	values.uid = VATTR_IS_ACTIVE(attributes, va_uid)
	    ? attributes->va_uid
	    : kauth_cred_getuid(vfs_context_ucred(context));
	lck_mtx_lock(mount->nodes_lock);
	values.gid =
	    VATTR_IS_ACTIVE(attributes, va_gid) ? attributes->va_gid : directory->inode.gid;
	lck_mtx_unlock(mount->nodes_lock);
	btrfs_xnu_now(&values.time);
	if (target != NULL) {
		values.target = target;
		values.target_length = strlen(target);
	}
	error = btrfs_xnu_error(btrfs_xnu_begin(mount, BTRFS_XNU_OPERATION_NODES, &transaction));
	if (error == 0) {
		pending = btrfs_volume_pending(mount->volume);
		error = btrfs_xnu_finish(mount, transaction,
		    btrfs_transaction_create(transaction, directory->inode.id, name->cn_nameptr,
			(size_t)name->cn_namelen, &values, &id),
		    directory, NULL);
	}
	if (error != 0) {
		return error;
	}
	VATTR_SET_SUPPORTED(attributes, va_type);
	VATTR_SET_SUPPORTED(attributes, va_mode);
	VATTR_SET_SUPPORTED(attributes, va_uid);
	VATTR_SET_SUPPORTED(attributes, va_gid);
	fs = btrfs_volume_read(mount->volume, &view);
	error = btrfs_xnu_error(btrfs_get_inode(fs, id, &inode));
	btrfs_volume_unread(mount->volume, view);
	if (error == 0) {
		error = btrfs_xnu_get_node(mount, &inode, parent, name, result);
	}
	/* The new object is durable with the operation that created it. */
	if (error == 0) {
		lck_mtx_lock(mount->nodes_lock);
		((struct btrfs_xnu_node *)vnode_fsnode(*result))->pending = pending;
		lck_mtx_unlock(mount->nodes_lock);
	}
	return error;
}

int
btrfs_xnu_create(void *arguments)
{
	struct vnop_create_args *args = arguments;

	return btrfs_xnu_make(
	    args->a_dvp, args->a_vpp, args->a_cnp, args->a_vap, NULL, args->a_context);
}

int
btrfs_xnu_mkdir(void *arguments)
{
	struct vnop_mkdir_args *args = arguments;

	return btrfs_xnu_make(
	    args->a_dvp, args->a_vpp, args->a_cnp, args->a_vap, NULL, args->a_context);
}

int
btrfs_xnu_symlink(void *arguments)
{
	struct vnop_symlink_args *args = arguments;

	args->a_vap->va_type = VLNK;
	return btrfs_xnu_make(
	    args->a_dvp, args->a_vpp, args->a_cnp, args->a_vap, args->a_target, args->a_context);
}

int
btrfs_xnu_link(void *arguments)
{
	struct vnop_link_args *args = arguments;
	struct btrfs_xnu_node *node = vnode_fsnode(args->a_vp);
	struct btrfs_xnu_node *directory = vnode_fsnode(args->a_tdvp);
	struct btrfs_transaction *transaction;
	struct btrfs_time now;
	int error;

	if (vnode_mount(args->a_vp) != vnode_mount(args->a_tdvp)) {
		return EXDEV;
	}
	btrfs_xnu_now(&now);
	error =
	    btrfs_xnu_error(btrfs_xnu_begin(node->mount, BTRFS_XNU_OPERATION_NODES, &transaction));
	if (error == 0) {
		error = btrfs_xnu_finish(node->mount, transaction,
		    btrfs_transaction_link(transaction, node->inode.id, directory->inode.id,
			args->a_cnp->cn_nameptr, (size_t)args->a_cnp->cn_namelen, now),
		    node, directory);
	}
	return error;
}

/* After a name went away: an inode without names is either an orphan kept
 * for its open references or gone, and its vnode must not serve a later
 * inode that reuses the number. */
static void
btrfs_xnu_name_removed(struct btrfs_xnu_node *node, vnode_t vnode, int open)
{
	struct btrfs_volume_view *view;
	struct btrfs_inode inode;
	const struct btrfs_fs *fs;
	int error;

	cache_purge(vnode);
	error = btrfs_xnu_read_inode(node, &fs, &view, &inode);
	if (error == 0) {
		btrfs_volume_unread(node->mount->volume, view);
	}
	if (error == 0 && inode.links != 0) {
		return;
	}
	lck_mtx_lock(node->mount->nodes_lock);
	if (error == 0 && open) {
		node->orphan = 1;
	} else if (node->hashed) {
		LIST_REMOVE(node, hash);
		node->hashed = 0;
	}
	lck_mtx_unlock(node->mount->nodes_lock);
	if (error != 0 || !open) {
		ubc_setsize(vnode, 0);
		(void)vnode_recycle(vnode);
	}
}

static int
btrfs_xnu_unlink(vnode_t parent, vnode_t vnode, struct componentname *name, int flags)
{
	struct btrfs_xnu_node *directory = vnode_fsnode(parent);
	struct btrfs_xnu_node *node = vnode_fsnode(vnode);
	struct btrfs_transaction *transaction;
	struct btrfs_time now;
	int open = vnode_isinuse(vnode, 0);
	int error;

	if ((flags & VNODE_REMOVE_NODELETEBUSY) && open) {
		return EBUSY;
	}
	btrfs_xnu_now(&now);
	error = btrfs_xnu_error(
	    btrfs_xnu_begin(directory->mount, BTRFS_XNU_OPERATION_NODES, &transaction));
	if (error == 0) {
		error = btrfs_xnu_finish(directory->mount, transaction,
		    btrfs_transaction_unlink(transaction, directory->inode.id, name->cn_nameptr,
			(size_t)name->cn_namelen, now, open),
		    directory, node);
	}
	if (error == 0) {
		btrfs_xnu_name_removed(node, vnode, open);
	}
	return error;
}

int
btrfs_xnu_remove(void *arguments)
{
	struct vnop_remove_args *args = arguments;

	return btrfs_xnu_unlink(args->a_dvp, args->a_vp, args->a_cnp, args->a_flags);
}

int
btrfs_xnu_rmdir(void *arguments)
{
	struct vnop_rmdir_args *args = arguments;

	return btrfs_xnu_unlink(args->a_dvp, args->a_vp, args->a_cnp, 0);
}

int
btrfs_xnu_rename(void *arguments)
{
	struct vnop_rename_args *args = arguments;
	struct btrfs_xnu_node *source = vnode_fsnode(args->a_fdvp);
	struct btrfs_xnu_node *target = vnode_fsnode(args->a_tdvp);
	struct btrfs_transaction *transaction;
	struct btrfs_time now;
	int open = args->a_tvp != NULL && vnode_isinuse(args->a_tvp, 0);
	int error;

	if (vnode_mount(args->a_fdvp) != vnode_mount(args->a_tdvp)) {
		return EXDEV;
	}
	btrfs_xnu_now(&now);
	error = btrfs_xnu_error(
	    btrfs_xnu_begin(source->mount, BTRFS_XNU_OPERATION_NODES, &transaction));
	if (error == 0) {
		/* Both directories; the moved object is durable with them. */
		error = btrfs_xnu_finish(source->mount, transaction,
		    btrfs_transaction_rename(transaction, source->inode.id,
			args->a_fcnp->cn_nameptr, (size_t)args->a_fcnp->cn_namelen,
			target->inode.id, args->a_tcnp->cn_nameptr,
			(size_t)args->a_tcnp->cn_namelen, now, open),
		    source, target);
	}
	if (error == 0) {
		lck_mtx_lock(source->mount->nodes_lock);
		((struct btrfs_xnu_node *)vnode_fsnode(args->a_fvp))->pending = source->pending;
		lck_mtx_unlock(source->mount->nodes_lock);
	}
	if (error == 0) {
		cache_purge(args->a_fvp);
		if (args->a_tvp != NULL && args->a_tvp != args->a_fvp) {
			btrfs_xnu_name_removed(vnode_fsnode(args->a_tvp), args->a_tvp, open);
		}
	}
	return error;
}

/* An orphan's last reference is gone: delete it as Linux's eviction does. */
int
btrfs_xnu_inactive(void *arguments)
{
	struct vnop_inactive_args *args = arguments;
	struct btrfs_xnu_node *node = vnode_fsnode(args->a_vp);
	struct btrfs_transaction *transaction;
	int orphan;
	int error;

	lck_mtx_lock(node->mount->nodes_lock);
	orphan = node->orphan;
	lck_mtx_unlock(node->mount->nodes_lock);
	if (!orphan || !btrfs_volume_writable(node->mount->volume)) {
		return 0;
	}
	ubc_setsize(args->a_vp, 0);
	error =
	    btrfs_xnu_error(btrfs_xnu_begin(node->mount, BTRFS_XNU_OPERATION_NODES, &transaction));
	if (error == 0) {
		error = btrfs_xnu_finish(node->mount, transaction,
		    btrfs_transaction_evict(transaction, node->inode.id), NULL, NULL);
	}
	lck_mtx_lock(node->mount->nodes_lock);
	node->orphan = 0;
	if (node->hashed) {
		LIST_REMOVE(node, hash);
		node->hashed = 0;
	}
	lck_mtx_unlock(node->mount->nodes_lock);
	(void)vnode_recycle(args->a_vp);
	/* A failed eviction leaves the orphan item for cleanup at the next mount. */
	return error;
}

/* Native policy exposes user.* names only; Linux allows them on regular files
 * and directories. Other names stay unsupported (ENOTSUP). */
static int
btrfs_xnu_xattr_target(struct btrfs_xnu_node *node, vnode_t vnode, const char *name, size_t *length)
{
	*length = strnlen(name, XATTR_MAXNAMELEN + 1);
	if (!btrfs_native_xattr_visible(name, *length)) {
		return ENOTSUP;
	}
	if (vnode_vtype(vnode) != VREG && vnode_vtype(vnode) != VDIR) {
		return EPERM;
	}
	return btrfs_volume_writable(node->mount->volume) ? 0 : EROFS;
}

int
btrfs_xnu_setxattr(void *arguments)
{
	struct vnop_setxattr_args *args = arguments;
	struct btrfs_xnu_node *node = vnode_fsnode(args->a_vp);
	struct btrfs_transaction *transaction = NULL;
	struct btrfs_time now;
	user_ssize_t size = uio_resid(args->a_uio);
	void *value = NULL;
	size_t name_length;
	int flags = 0;
	int error;

	error = btrfs_xnu_xattr_target(node, args->a_vp, args->a_name, &name_length);
	if (error != 0) {
		return error;
	}
	if (size < 0 || (uint64_t)size > BTRFS_NATIVE_XATTR_LIMIT || uio_offset(args->a_uio) != 0) {
		return size < 0 || uio_offset(args->a_uio) != 0 ? EINVAL : E2BIG;
	}
	if (size != 0) {
		value = _MALLOC((size_t)size, M_TEMP, M_WAITOK | M_NULL);
		if (value == NULL) {
			return ENOMEM;
		}
		error = uiomove(value, (int)size, args->a_uio);
	}
	if (args->a_options & XATTR_CREATE) {
		flags |= BTRFS_XATTR_CREATE;
	}
	if (args->a_options & XATTR_REPLACE) {
		flags |= BTRFS_XATTR_REPLACE;
	}
	btrfs_xnu_now(&now);
	if (error == 0) {
		error = btrfs_xnu_error(
		    btrfs_xnu_begin(node->mount, BTRFS_XNU_OPERATION_NODES, &transaction));
	}
	if (error == 0) {
		error = btrfs_xnu_finish(node->mount, transaction,
		    btrfs_transaction_set_xattr(transaction, node->inode.id, args->a_name,
			name_length, value, (size_t)size, flags, now),
		    node, NULL);
	}
	if (value != NULL) {
		_FREE(value, M_TEMP);
	}
	if (error == ENOENT) {
		error = ENOATTR;
	}
	return error;
}

int
btrfs_xnu_removexattr(void *arguments)
{
	struct vnop_removexattr_args *args = arguments;
	struct btrfs_xnu_node *node = vnode_fsnode(args->a_vp);
	struct btrfs_transaction *transaction;
	struct btrfs_time now;
	size_t name_length;
	int error;

	error = btrfs_xnu_xattr_target(node, args->a_vp, args->a_name, &name_length);
	if (error != 0) {
		return error;
	}
	btrfs_xnu_now(&now);
	error =
	    btrfs_xnu_error(btrfs_xnu_begin(node->mount, BTRFS_XNU_OPERATION_NODES, &transaction));
	if (error == 0) {
		error = btrfs_xnu_finish(node->mount, transaction,
		    btrfs_transaction_remove_xattr(
			transaction, node->inode.id, args->a_name, name_length, now),
		    node, NULL);
	}
	return error == ENOENT ? ENOATTR : error;
}
