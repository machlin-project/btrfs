/* SPDX-License-Identifier: BSD-3-Clause */
#include "btrfs_xnu.h"

#include <sys/buf.h>
#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/namei.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/systm.h>
#include <sys/ubc.h>
#include <sys/uio.h>
#include <sys/unistd.h>
#include <sys/vnode_if.h>

static int (**btrfs_xnu_dispatch)(void *);

static enum vtype
btrfs_xnu_type(uint16_t mode)
{
	switch (mode & BTRFS_MODE_TYPE) {
	case BTRFS_MODE_REGULAR:
		return VREG;
	case BTRFS_MODE_DIRECTORY:
		return VDIR;
	case BTRFS_MODE_SYMLINK:
		return VLNK;
	default:
		return VNON;
	}
}

static int
btrfs_xnu_cached_node(struct btrfs_xnu_mount *mount, uint64_t number, vnode_t *result)
{
	struct btrfs_xnu_node *node;
	vnode_t vnode = NULL;
	uint32_t vid = 0;
	int error;

	lck_mtx_lock(mount->nodes_lock);
	LIST_FOREACH(node, &mount->nodes[number % BTRFS_XNU_HASH_SIZE], hash)
	{
		if (node->number == number) {
			vnode = node->vnode;
			vid = node->vid;
			break;
		}
	}
	lck_mtx_unlock(mount->nodes_lock);
	if (vnode == NULL) {
		return ENOENT;
	}
	/* Reclaim may have run after dropping the hash lock; vid prevents reuse. */
	error = vnode_getwithvid(vnode, vid);
	if (error == 0) {
		*result = vnode;
	}
	return error;
}

int
btrfs_xnu_get_node(struct btrfs_xnu_mount *mount, uint64_t number, vnode_t parent,
    struct componentname *name, vnode_t *result)
{
	struct btrfs_xnu_node *node;
	struct vnode_fsparam parameters;
	struct btrfs_object_id identity;
	int error;

	*result = NULL;
	if (btrfs_xnu_cached_node(mount, number, result) == 0) {
		return 0;
	}
	/* vnode_create can reclaim an unrelated inode. Keep the hash lock free. */
	lck_mtx_lock(mount->creation_lock);
	if (btrfs_xnu_cached_node(mount, number, result) == 0) {
		lck_mtx_unlock(mount->creation_lock);
		return 0;
	}
	node = _MALLOC(sizeof(*node), M_TEMP, M_WAITOK | M_ZERO | M_NULL);
	if (node == NULL) {
		error = ENOMEM;
		goto out;
	}
	node->mount = mount;
	node->number = number;
	lck_mtx_lock(mount->nodes_lock);
	error = btrfs_xnu_error(btrfs_identity_lookup(mount->identities, number, &identity));
	lck_mtx_unlock(mount->nodes_lock);
	if (error == 0) {
		error = btrfs_xnu_error(btrfs_get_inode(mount->fs, identity, &node->inode));
	}
	if (error != 0) {
		goto free_node;
	}
	if (btrfs_xnu_type(node->inode.mode) == VNON) {
		error = ENOTSUP;
		goto free_node;
	}
	bzero(&parameters, sizeof(parameters));
	parameters.vnfs_mp = mount->mount;
	parameters.vnfs_vtype = btrfs_xnu_type(node->inode.mode);
	parameters.vnfs_str = BTRFS_XNU_NAME;
	parameters.vnfs_dvp = parent;
	parameters.vnfs_fsnode = node;
	parameters.vnfs_vops = btrfs_xnu_dispatch;
	parameters.vnfs_markroot = number == BTRFS_NATIVE_ROOT_ID;
	parameters.vnfs_filesize = (off_t)node->inode.size;
	parameters.vnfs_cnp = name;
	parameters.vnfs_flags = VNFS_ADDFSREF;
	error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &parameters, &node->vnode);
	if (error != 0) {
		goto free_node;
	}
	node->vid = vnode_vid(node->vnode);
	lck_mtx_lock(mount->nodes_lock);
	LIST_INSERT_HEAD(&mount->nodes[number % BTRFS_XNU_HASH_SIZE], node, hash);
	lck_mtx_unlock(mount->nodes_lock);
	*result = node->vnode;
	goto out;
free_node:
	_FREE(node, M_TEMP);
out:
	lck_mtx_unlock(mount->creation_lock);
	return error;
}

static int
btrfs_xnu_unsupported(void *arguments)
{
	(void)arguments;
	return ENOTSUP;
}

static int
btrfs_xnu_read_only(void *arguments)
{
	(void)arguments;
	return EROFS;
}

static int
btrfs_xnu_noop(void *arguments)
{
	(void)arguments;
	return 0;
}

static int
btrfs_xnu_lookup(void *arguments)
{
	struct vnop_lookup_args *args = arguments;
	struct btrfs_xnu_node *directory = vnode_fsnode(args->a_dvp);
	struct btrfs_inode inode;
	struct componentname *name = args->a_cnp;
	uint64_t number;
	int error;

	*args->a_vpp = NULL;
	if (name->cn_namelen < 0) {
		return EINVAL;
	}
	if ((name->cn_flags & ISLASTCN) && name->cn_nameiop != LOOKUP) {
		return EROFS;
	}
	error = btrfs_xnu_error(btrfs_lookup(directory->mount->fs, &directory->inode,
	    (const uint8_t *)name->cn_nameptr, (size_t)name->cn_namelen, &inode));
	if (error != 0) {
		return error;
	}
	lck_mtx_lock(directory->mount->nodes_lock);
	error =
	    btrfs_xnu_error(btrfs_identity_get(directory->mount->identities, inode.id, &number));
	lck_mtx_unlock(directory->mount->nodes_lock);
	if (error != 0) {
		return error;
	}
	return btrfs_xnu_get_node(directory->mount, number,
	    (name->cn_flags & ISDOTDOT) ? NULL : args->a_dvp, name, args->a_vpp);
}

static int
btrfs_xnu_getattr(void *arguments)
{
	struct vnop_getattr_args *args = arguments;
	struct btrfs_xnu_node *node = vnode_fsnode(args->a_vp);
	struct btrfs_inode *inode = &node->inode;
	struct vnode_attr *attributes = args->a_vap;
	struct timespec time;

	VATTR_RETURN(attributes, va_type, btrfs_xnu_type(inode->mode));
	VATTR_RETURN(attributes, va_mode, inode->mode & ALLPERMS);
	VATTR_RETURN(attributes, va_uid, inode->uid);
	VATTR_RETURN(attributes, va_gid, inode->gid);
	VATTR_RETURN(attributes, va_nlink, inode->links);
	VATTR_RETURN(attributes, va_fileid, node->number);
	VATTR_RETURN(attributes, va_linkid, node->number);
	VATTR_RETURN(attributes, va_fsid, vfs_statfs(node->mount->mount)->f_fsid.val[0]);
	VATTR_RETURN(attributes, va_gen, inode->generation);
	VATTR_RETURN(attributes, va_data_size, inode->size);
	VATTR_RETURN(attributes, va_total_size, inode->size);
	VATTR_RETURN(attributes, va_data_alloc, inode->allocated_bytes);
	VATTR_RETURN(attributes, va_total_alloc, inode->allocated_bytes);
	VATTR_RETURN(attributes, va_iosize, node->mount->info.sector_size);
	time.tv_sec = inode->access_time.seconds;
	time.tv_nsec = inode->access_time.nanoseconds;
	VATTR_RETURN(attributes, va_access_time, time);
	time.tv_sec = inode->modify_time.seconds;
	time.tv_nsec = inode->modify_time.nanoseconds;
	VATTR_RETURN(attributes, va_modify_time, time);
	time.tv_sec = inode->change_time.seconds;
	time.tv_nsec = inode->change_time.nanoseconds;
	VATTR_RETURN(attributes, va_change_time, time);
	time.tv_sec = inode->birth_time.seconds;
	time.tv_nsec = inode->birth_time.nanoseconds;
	VATTR_RETURN(attributes, va_create_time, time);
	return 0;
}

static int
btrfs_xnu_open(void *arguments)
{
	struct vnop_open_args *args = arguments;

	return (args->a_mode & FWRITE) ? EROFS : 0;
}

static int
btrfs_xnu_read(void *arguments)
{
	struct vnop_read_args *args = arguments;
	struct btrfs_xnu_node *node = vnode_fsnode(args->a_vp);

	if (vnode_vtype(args->a_vp) != VREG) {
		return vnode_vtype(args->a_vp) == VDIR ? EISDIR : EINVAL;
	}
	if (uio_offset(args->a_uio) < 0) {
		return EINVAL;
	}
	return cluster_read(args->a_vp, args->a_uio, (off_t)node->inode.size, args->a_ioflag);
}

static int
btrfs_xnu_readlink(void *arguments)
{
	struct vnop_readlink_args *args = arguments;
	struct btrfs_xnu_node *node = vnode_fsnode(args->a_vp);
	uint8_t *buffer;
	size_t capacity;
	size_t completed;
	user_ssize_t residual;
	int error;

	if (vnode_vtype(args->a_vp) != VLNK) {
		return EINVAL;
	}
	if (node->inode.size >= node->mount->info.sector_size) {
		return EOPNOTSUPP;
	}
	residual = uio_resid(args->a_uio);
	if (residual < 0) {
		return EINVAL;
	}
	capacity = (size_t)node->inode.size;
	if ((uint64_t)residual < capacity) {
		capacity = (size_t)residual;
	}
	if (capacity == 0) {
		return 0;
	}
	buffer = _MALLOC(capacity, M_TEMP, M_WAITOK | M_NULL);
	if (buffer == NULL) {
		return ENOMEM;
	}
	error = btrfs_xnu_error(
	    btrfs_read(node->mount->fs, &node->inode, 0, buffer, capacity, &completed));
	if (error == 0 && completed != capacity) {
		error = EIO;
	}
	if (error == 0) {
		error = uiomove((char *)buffer, (int)completed, args->a_uio);
	}
	_FREE(buffer, M_TEMP);
	return error;
}

static int
btrfs_xnu_readdir(void *arguments)
{
	struct vnop_readdir_args *args = arguments;
	struct btrfs_xnu_node *node = vnode_fsnode(args->a_vp);
	struct btrfs_dir_entry entry;
	struct btrfs_directory *stream;
	struct direntry extended;
	struct dirent basic;
	void *record;
	uint64_t cookie;
	uint64_t number;
	off_t record_offset;
	size_t length;
	int count = 0;
	int error = 0;
	enum btrfs_result result;

	if (args->a_eofflag != NULL) {
		*args->a_eofflag = 0;
	}
	if (args->a_numdirent != NULL) {
		*args->a_numdirent = 0;
	}
	if (uio_offset(args->a_uio) < 0) {
		return EINVAL;
	}
	cookie = (uint64_t)uio_offset(args->a_uio);
	result = btrfs_directory_open(node->mount->fs, &node->inode, cookie, &stream);
	if (result != BTRFS_OK) {
		return btrfs_xnu_error(result);
	}
	while ((result = btrfs_directory_next(stream, &entry, &cookie)) == BTRFS_OK) {
		if (cookie > INT64_MAX) {
			result = BTRFS_RANGE;
			break;
		}
		lck_mtx_lock(node->mount->nodes_lock);
		result = btrfs_identity_get(node->mount->identities, entry.id, &number);
		lck_mtx_unlock(node->mount->nodes_lock);
		if (result != BTRFS_OK) {
			break;
		}
		if (args->a_flags & VNODE_READDIR_EXTENDED) {
			bzero(&extended, sizeof(extended));
			length =
			    roundup(offsetof(struct direntry, d_name) + entry.name_length + 1, 8);
			extended.d_ino = number;
			extended.d_seekoff = cookie;
			extended.d_namlen = entry.name_length;
			extended.d_type = IFTODT(btrfs_mode_for_type(entry.type));
			extended.d_reclen = (uint16_t)length;
			memcpy(extended.d_name, entry.name, entry.name_length);
			record = &extended;
		} else {
			bzero(&basic, sizeof(basic));
			length =
			    roundup(offsetof(struct dirent, d_name) + entry.name_length + 1, 4);
			basic.d_ino = number;
			basic.d_namlen = (uint8_t)entry.name_length;
			basic.d_type = IFTODT(btrfs_mode_for_type(entry.type));
			basic.d_reclen = (uint16_t)length;
			memcpy(basic.d_name, entry.name, entry.name_length);
			record = &basic;
		}
		if (uio_resid(args->a_uio) < (user_ssize_t)length) {
			error = count == 0 ? EINVAL : 0;
			break;
		}
		record_offset = uio_offset(args->a_uio);
		error = uiomove(record, (int)length, args->a_uio);
		if (error != 0) {
			uio_setoffset(args->a_uio, record_offset);
			break;
		}
		uio_setoffset(args->a_uio, (off_t)cookie);
		count++;
	}
	btrfs_directory_close(stream);
	if (result == BTRFS_NOT_FOUND) {
		if (args->a_eofflag != NULL) {
			*args->a_eofflag = 1;
		}
	} else if (result != BTRFS_OK) {
		error = btrfs_xnu_error(result);
	}
	if (args->a_numdirent != NULL) {
		*args->a_numdirent = count;
	}
	return error;
}

static int
btrfs_xnu_blockmap(void *arguments)
{
	struct vnop_blockmap_args *args = arguments;
	struct btrfs_xnu_node *node = vnode_fsnode(args->a_vp);

	if (args->a_flags & VNODE_WRITE) {
		return EROFS;
	}
	if (args->a_foffset < 0 || args->a_foffset % node->mount->device_block_size != 0) {
		return EINVAL;
	}
	/* This is a file-logical strategy address. It never escapes to the device:
	 * strategy below verifies/decompresses bytes before completing UBC I/O. */
	if (args->a_bpn != NULL) {
		*args->a_bpn = args->a_foffset / node->mount->device_block_size;
	}
	if (args->a_run != NULL) {
		*args->a_run = args->a_size < MAXPHYS ? args->a_size : MAXPHYS;
	}
	return 0;
}

static int
btrfs_xnu_blktooff(void *arguments)
{
	struct vnop_blktooff_args *args = arguments;
	struct btrfs_xnu_node *node = vnode_fsnode(args->a_vp);

	if (args->a_lblkno < 0 ||
	    (uint64_t)args->a_lblkno > INT64_MAX / node->mount->info.sector_size) {
		return EINVAL;
	}
	*args->a_offset = args->a_lblkno * node->mount->info.sector_size;
	return 0;
}

static int
btrfs_xnu_offtoblk(void *arguments)
{
	struct vnop_offtoblk_args *args = arguments;
	struct btrfs_xnu_node *node = vnode_fsnode(args->a_vp);

	if (args->a_offset < 0) {
		return EINVAL;
	}
	*args->a_lblkno = args->a_offset / node->mount->info.sector_size;
	return 0;
}

static int
btrfs_xnu_strategy(void *arguments)
{
	struct vnop_strategy_args *args = arguments;
	struct btrfs_xnu_node *node = vnode_fsnode(buf_vnode(args->a_bp));
	caddr_t address = NULL;
	size_t completed = 0;
	size_t length = buf_count(args->a_bp);
	daddr64_t block = buf_blkno(args->a_bp);
	uint64_t offset;
	int error;

	if (!(buf_flags(args->a_bp) & B_READ)) {
		error = EROFS;
	} else if (block < 0 || (uint64_t)block > INT64_MAX / node->mount->device_block_size) {
		error = EINVAL;
	} else {
		offset = (uint64_t)block * node->mount->device_block_size;
		error = buf_map(args->a_bp, &address);
		if (error == 0) {
			error = btrfs_xnu_error(btrfs_read(
			    node->mount->fs, &node->inode, offset, address, length, &completed));
			if (error == 0 && completed < length) {
				bzero(address + completed, length - completed);
				completed = length;
			}
			buf_unmap(args->a_bp);
		}
	}
	buf_setresid(args->a_bp, (int)(length - completed));
	buf_seterror(args->a_bp, error);
	buf_biodone(args->a_bp);
	return error;
}

static int
btrfs_xnu_pagein(void *arguments)
{
	struct vnop_pagein_args *args = arguments;
	struct btrfs_xnu_node *node = vnode_fsnode(args->a_vp);

	return cluster_pagein(args->a_vp, args->a_pl, args->a_pl_offset, args->a_f_offset,
	    (int)args->a_size, (off_t)node->inode.size, args->a_flags);
}

static int
btrfs_xnu_pathconf(void *arguments)
{
	struct vnop_pathconf_args *args = arguments;

	switch (args->a_name) {
	case _PC_LINK_MAX:
		*args->a_retval = UINT32_MAX;
		return 0;
	case _PC_NAME_MAX:
		*args->a_retval = BTRFS_NAME_MAX;
		return 0;
	case _PC_CHOWN_RESTRICTED:
	case _PC_NO_TRUNC:
		*args->a_retval = 1;
		return 0;
	default:
		return EINVAL;
	}
}

static int
btrfs_xnu_reclaim(void *arguments)
{
	struct vnop_reclaim_args *args = arguments;
	struct btrfs_xnu_node *node = vnode_fsnode(args->a_vp);

	lck_mtx_lock(node->mount->nodes_lock);
	LIST_REMOVE(node, hash);
	lck_mtx_unlock(node->mount->nodes_lock);
	vnode_clearfsnode(args->a_vp);
	vnode_removefsref(args->a_vp);
	_FREE(node, M_TEMP);
	return 0;
}

static struct vnodeopv_entry_desc btrfs_xnu_operations[] = {
	{ &vnop_default_desc, btrfs_xnu_unsupported }, { &vnop_lookup_desc, btrfs_xnu_lookup },
	{ &vnop_open_desc, btrfs_xnu_open }, { &vnop_close_desc, btrfs_xnu_noop },
	{ &vnop_getattr_desc, btrfs_xnu_getattr }, { &vnop_read_desc, btrfs_xnu_read },
	{ &vnop_readdir_desc, btrfs_xnu_readdir }, { &vnop_readlink_desc, btrfs_xnu_readlink },
	{ &vnop_blockmap_desc, btrfs_xnu_blockmap }, { &vnop_blktooff_desc, btrfs_xnu_blktooff },
	{ &vnop_offtoblk_desc, btrfs_xnu_offtoblk }, { &vnop_strategy_desc, btrfs_xnu_strategy },
	{ &vnop_pagein_desc, btrfs_xnu_pagein }, { &vnop_mmap_desc, btrfs_xnu_noop },
	{ &vnop_mnomap_desc, btrfs_xnu_noop }, { &vnop_fsync_desc, btrfs_xnu_noop },
	{ &vnop_inactive_desc, btrfs_xnu_noop }, { &vnop_reclaim_desc, btrfs_xnu_reclaim },
	{ &vnop_pathconf_desc, btrfs_xnu_pathconf }, { &vnop_create_desc, btrfs_xnu_read_only },
	{ &vnop_mkdir_desc, btrfs_xnu_read_only }, { &vnop_mknod_desc, btrfs_xnu_read_only },
	{ &vnop_write_desc, btrfs_xnu_read_only }, { &vnop_setattr_desc, btrfs_xnu_read_only },
	{ &vnop_link_desc, btrfs_xnu_read_only }, { &vnop_symlink_desc, btrfs_xnu_read_only },
	{ &vnop_remove_desc, btrfs_xnu_read_only }, { &vnop_rmdir_desc, btrfs_xnu_read_only },
	{ &vnop_rename_desc, btrfs_xnu_read_only }, { NULL, NULL }
};

struct vnodeopv_desc btrfs_xnu_vnodeops = { &btrfs_xnu_dispatch, btrfs_xnu_operations };
