/* SPDX-License-Identifier: BSD-3-Clause */
#include "btrfs_xnu.h"

#include <sys/buf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/malloc.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/vnode_if.h>

int
btrfs_xnu_error(enum btrfs_result result)
{
	switch (result) {
	case BTRFS_OK:
		return 0;
	case BTRFS_INVALID_ARGUMENT:
	case BTRFS_NOT_BTRFS:
		return EINVAL;
	case BTRFS_UNSUPPORTED:
		return ENOTSUP;
	case BTRFS_NO_MEMORY:
		return ENOMEM;
	case BTRFS_NOT_FOUND:
		return ENOENT;
	case BTRFS_NOT_DIRECTORY:
		return ENOTDIR;
	case BTRFS_READ_ONLY:
		return EROFS;
	case BTRFS_IS_DIRECTORY:
		return EISDIR;
	case BTRFS_RANGE:
		return EOVERFLOW;
	case BTRFS_STALE:
		return ESTALE;
	default:
		return EIO;
	}
}

static void *
btrfs_xnu_allocate(void *context, size_t size)
{
	(void)context;
	return _MALLOC(size, M_TEMP, M_WAITOK | M_NULL);
}

static void
btrfs_xnu_release(void *context, void *buffer, size_t size)
{
	(void)context;
	(void)size;
	_FREE(buffer, M_TEMP);
}

static enum btrfs_result
btrfs_xnu_device_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct btrfs_xnu_mount *mount = context;
	uint8_t *destination = buffer;
	buf_t block;
	size_t within;
	size_t amount;
	int error;

	error = vnode_getwithref(mount->device);
	if (error != 0) {
		return BTRFS_IO;
	}
	while (length != 0) {
		within = (size_t)(offset % mount->device_block_size);
		amount = mount->device_block_size - within;
		if (amount > length) {
			amount = length;
		}
		block = NULL;
		/* A fixed sector-sized cache key prevents overlapping metadata buffers. */
		error =
		    buf_meta_bread(mount->device, (daddr64_t)(offset / mount->device_block_size),
			(int)mount->device_block_size, NOCRED, &block);
		if (error == 0 && buf_resid(block) != 0) {
			error = EIO;
		}
		if (error == 0) {
			memcpy(destination, (const uint8_t *)buf_dataptr(block) + within, amount);
		}
		if (block != NULL) {
			buf_brelse(block);
		}
		if (error != 0) {
			break;
		}
		offset += amount;
		destination += amount;
		length -= amount;
	}
	vnode_put(mount->device);
	return error == 0 ? BTRFS_OK : BTRFS_IO;
}

static void
btrfs_xnu_free_mount(struct btrfs_xnu_mount *mount)
{
	btrfs_identity_destroy(mount->identities);
	if (mount->fs != NULL) {
		btrfs_unmount(mount->fs);
	}
	if (mount->nodes_lock != NULL) {
		lck_mtx_free(mount->nodes_lock, btrfs_xnu_locks);
	}
	if (mount->creation_lock != NULL) {
		lck_mtx_free(mount->creation_lock, btrfs_xnu_locks);
	}
	vnode_rele(mount->device);
	_FREE(mount, M_TEMP);
}

static int
btrfs_xnu_mount_volume(mount_t mp, vnode_t device, user_addr_t data, vfs_context_t context)
{
	struct btrfs_xnu_mount *mount;
	struct btrfs_environment environment;
	struct btrfs_inode root;
	struct vfsioattr io;
	uint64_t blocks;
	uint32_t block_size;
	int error;

	(void)data;
	if (vfs_flags(mp) & MNT_UPDATE) {
		return ENOTSUP;
	}
	if (!(vfs_flags(mp) & MNT_RDONLY)) {
		return EROFS;
	}
	if (device == NULL || vnode_vtype(device) != VBLK) {
		return ENOTBLK;
	}
	error = VNOP_IOCTL(device, DKIOCGETBLOCKSIZE, (caddr_t)&block_size, 0, context);
	if (error != 0) {
		return error;
	}
	error = VNOP_IOCTL(device, DKIOCGETBLOCKCOUNT, (caddr_t)&blocks, 0, context);
	if (error != 0) {
		return error;
	}
	if (block_size < DEV_BSIZE || block_size > PAGE_SIZE ||
	    (block_size & (block_size - 1)) != 0 || blocks > INT64_MAX / block_size) {
		return ENOTSUP;
	}
	mount = _MALLOC(sizeof(*mount), M_TEMP, M_WAITOK | M_ZERO | M_NULL);
	if (mount == NULL) {
		return ENOMEM;
	}
	error = vnode_ref(device);
	if (error != 0) {
		_FREE(mount, M_TEMP);
		return error;
	}
	mount->mount = mp;
	mount->device = device;
	mount->device_block_size = block_size;
	mount->nodes_lock = lck_mtx_alloc_init(btrfs_xnu_locks, LCK_ATTR_NULL);
	mount->creation_lock = lck_mtx_alloc_init(btrfs_xnu_locks, LCK_ATTR_NULL);
	if (mount->nodes_lock == NULL || mount->creation_lock == NULL) {
		btrfs_xnu_free_mount(mount);
		return ENOMEM;
	}
	bzero(&environment, sizeof(environment));
	environment.context = mount;
	environment.size_bytes = blocks * block_size;
	environment.read = btrfs_xnu_device_read;
	environment.allocate = btrfs_xnu_allocate;
	environment.release = btrfs_xnu_release;
	error = btrfs_xnu_error(btrfs_mount(&environment, 0, &mount->fs));
	if (error != 0) {
		btrfs_xnu_free_mount(mount);
		return error;
	}
	btrfs_get_info(mount->fs, &mount->info);
	(void)btrfs_root(mount->fs, &root);
	error = btrfs_xnu_error(btrfs_identity_create(&environment, root.id, &mount->identities));
	if (error != 0) {
		btrfs_xnu_free_mount(mount);
		return error;
	}
	if (mount->info.sector_size < block_size) {
		btrfs_xnu_free_mount(mount);
		return ENOTSUP;
	}
	vfs_setfsprivate(mp, mount);
	vfs_setflags(mp, MNT_LOCAL | MNT_RDONLY);
	vfs_getnewfsid(mp);
	vfs_setlocklocal(mp);
	vfs_ioattr(mp, &io);
	io.io_devblocksize = block_size;
	vfs_setioattr(mp, &io);
	return 0;
}

static int
btrfs_xnu_unmount_volume(mount_t mp, int flags, vfs_context_t context)
{
	struct btrfs_xnu_mount *mount = vfs_fsprivate(mp);
	int error;

	(void)context;
	error = vflush(mp, NULL, (flags & MNT_FORCE) ? FORCECLOSE : 0);
	if (error != 0) {
		return error;
	}
	vfs_setfsprivate(mp, NULL);
	btrfs_xnu_free_mount(mount);
	return 0;
}

static int
btrfs_xnu_root(mount_t mp, vnode_t *result, vfs_context_t context)
{
	(void)context;
	return btrfs_xnu_get_node(vfs_fsprivate(mp), BTRFS_NATIVE_ROOT_ID, NULL, NULL, result);
}

static int
btrfs_xnu_vget(mount_t mp, ino64_t number, vnode_t *result, vfs_context_t context)
{
	(void)context;
	return btrfs_xnu_get_node(vfs_fsprivate(mp), number, NULL, NULL, result);
}

static int
btrfs_xnu_volume_attributes(mount_t mp, struct vfs_attr *attributes, vfs_context_t context)
{
	struct btrfs_xnu_mount *mount = vfs_fsprivate(mp);
	struct btrfs_info *info = &mount->info;

	(void)context;
	VFSATTR_RETURN(attributes, f_bsize, info->sector_size);
	VFSATTR_RETURN(attributes, f_iosize, info->sector_size);
	VFSATTR_RETURN(attributes, f_blocks, info->total_bytes / info->sector_size);
	VFSATTR_RETURN(
	    attributes, f_bfree, (info->total_bytes - info->used_bytes) / info->sector_size);
	VFSATTR_RETURN(attributes, f_bavail, 0);
	VFSATTR_RETURN(attributes, f_bused, info->used_bytes / info->sector_size);
	VFSATTR_RETURN(attributes, f_files, 0);
	VFSATTR_RETURN(attributes, f_ffree, 0);
	VFSATTR_RETURN(attributes, f_fssubtype, 0);
	return 0;
}

static int
btrfs_xnu_sync(mount_t mp, int flags, vfs_context_t context)
{
	(void)mp;
	(void)flags;
	(void)context;
	return 0;
}

struct vfsops btrfs_xnu_vfsops = { .vfs_mount = btrfs_xnu_mount_volume,
	.vfs_unmount = btrfs_xnu_unmount_volume,
	.vfs_root = btrfs_xnu_root,
	.vfs_getattr = btrfs_xnu_volume_attributes,
	.vfs_sync = btrfs_xnu_sync,
	.vfs_vget = btrfs_xnu_vget };
