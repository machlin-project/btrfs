/* SPDX-License-Identifier: BSD-3-Clause */
#include "btrfs_xnu.h"
#include <btrfs/codec.h>

#include <libkern/zlib.h>
#include <mach/vm_param.h>
#include <sys/buf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/malloc.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/ubc.h>
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
	case BTRFS_EXISTS:
		return EEXIST;
	case BTRFS_NO_SPACE:
		return ENOSPC;
	case BTRFS_NOT_EMPTY:
		return ENOTEMPTY;
	case BTRFS_CROSS_TREE:
		return EXDEV;
	case BTRFS_TOO_MANY_LINKS:
		return EMLINK;
	case BTRFS_NAME_TOO_LONG:
		return ENAMETOOLONG;
	case BTRFS_NOT_PERMITTED:
		return EPERM;
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

static voidpf
btrfs_xnu_zalloc(voidpf context, uInt count, uInt size)
{
	(void)context;
	if (count == 0 || size > (1024U * 1024U) / count) {
		return NULL;
	}
	return _MALLOC((size_t)count * size, M_TEMP, M_WAITOK | M_ZERO | M_NULL);
}

static void
btrfs_xnu_zfree(voidpf context, voidpf allocation)
{
	(void)context;
	_FREE(allocation, M_TEMP);
}

static enum btrfs_result
btrfs_xnu_decompress(void *context, enum btrfs_compression codec, const void *input,
    size_t input_size, void *output, size_t capacity, size_t *produced)
{
	z_stream stream;
	void *workspace;
	enum btrfs_result decoded;
	int result;

	(void)context;
	if (codec == BTRFS_COMPRESSION_LZO) {
		return btrfs_lzo1x_decompress(input, input_size, output, capacity, produced);
	}
	if (codec == BTRFS_COMPRESSION_ZSTD) {
		workspace = _MALLOC(BTRFS_ZSTD_WORKSPACE_BYTES, M_TEMP, M_WAITOK | M_NULL);
		if (workspace == NULL) {
			return BTRFS_NO_MEMORY;
		}
		decoded =
		    btrfs_zstd_decompress(workspace, input, input_size, output, capacity, produced);
		_FREE(workspace, M_TEMP);
		return decoded;
	}
	if (codec != BTRFS_COMPRESSION_ZLIB) {
		return BTRFS_UNSUPPORTED;
	}
	if (input_size > UINT32_MAX || capacity > UINT32_MAX) {
		return BTRFS_RANGE;
	}
	bzero(&stream, sizeof(stream));
	stream.zalloc = btrfs_xnu_zalloc;
	stream.zfree = btrfs_xnu_zfree;
	stream.next_in = (Bytef *)input;
	stream.avail_in = (uInt)input_size;
	stream.next_out = output;
	stream.avail_out = (uInt)capacity;
	result = inflateInit(&stream);
	if (result != Z_OK) {
		return result == Z_MEM_ERROR ? BTRFS_NO_MEMORY : BTRFS_CORRUPT;
	}
	result = inflate(&stream, Z_FINISH);
	(void)inflateEnd(&stream);
	if (result != Z_STREAM_END) {
		return result == Z_MEM_ERROR ? BTRFS_NO_MEMORY : BTRFS_CORRUPT;
	}
	*produced = stream.total_out;
	return BTRFS_OK;
}

static enum btrfs_result
btrfs_xnu_read_aligned(struct btrfs_xnu_mount *mount, uint64_t offset, void *buffer, size_t length)
{
	buf_t request;
	int error;

	request = buf_alloc(mount->device);
	if (request == NULL) {
		return BTRFS_NO_MEMORY;
	}
	buf_setflags(request, B_READ);
	buf_setblkno(request, (daddr64_t)(offset / mount->device_block_size));
	buf_setlblkno(request, (daddr64_t)(offset / mount->device_block_size));
	buf_setcount(request, (uint32_t)length);
	buf_setsize(request, (uint32_t)length);
	buf_setresid(request, (uint32_t)length);
	buf_setdataptr(request, (uintptr_t)buffer);
	error = VNOP_STRATEGY(request);
	if (error == 0) {
		error = buf_biowait(request);
	}
	if (error == 0 && buf_resid(request) != 0) {
		error = EIO;
	}
	buf_free(request);
	return error == 0 ? BTRFS_OK : BTRFS_IO;
}

static enum btrfs_result
btrfs_xnu_resource_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct btrfs_xnu_mount *mount = context;
	uint8_t *destination = buffer;
	uint8_t *edge = NULL;
	size_t within;
	size_t amount;
	enum btrfs_result result = BTRFS_OK;

	if (vnode_getwithref(mount->device) != 0) {
		return BTRFS_IO;
	}
	while (length != 0) {
		within = (size_t)(offset % mount->device_block_size);
		if (within == 0 && length >= mount->device_block_size) {
			amount = length - length % mount->device_block_size;
			if (amount > MAXPHYS) {
				amount = MAXPHYS;
			}
			/* Private iobufs preserve range I/O without overlapping cache keys. */
			result = btrfs_xnu_read_aligned(mount, offset, destination, amount);
		} else {
			if (edge == NULL) {
				edge = _MALLOC(mount->device_block_size, M_TEMP, M_WAITOK | M_NULL);
				if (edge == NULL) {
					result = BTRFS_NO_MEMORY;
					break;
				}
			}
			amount = mount->device_block_size - within;
			if (amount > length) {
				amount = length;
			}
			result = btrfs_xnu_read_aligned(
			    mount, offset - within, edge, mount->device_block_size);
			if (result == BTRFS_OK) {
				memcpy(destination, edge + within, amount);
			}
		}
		if (result != BTRFS_OK) {
			break;
		}
		offset += amount;
		destination += amount;
		length -= amount;
	}
	if (edge != NULL) {
		_FREE(edge, M_TEMP);
	}
	vnode_put(mount->device);
	return result;
}

static enum btrfs_result
btrfs_xnu_write_aligned(
    struct btrfs_xnu_mount *mount, uint64_t offset, const void *buffer, size_t length)
{
	buf_t request;
	int error;

	request = buf_alloc(mount->device);
	if (request == NULL) {
		return BTRFS_NO_MEMORY;
	}
	buf_setflags(request, B_WRITE);
	buf_setblkno(request, (daddr64_t)(offset / mount->device_block_size));
	buf_setlblkno(request, (daddr64_t)(offset / mount->device_block_size));
	buf_setcount(request, (uint32_t)length);
	buf_setsize(request, (uint32_t)length);
	buf_setresid(request, (uint32_t)length);
	buf_setdataptr(request, (uintptr_t)buffer);
	/* Completion of a write buffer ends the device's write accounting. */
	vnode_startwrite(mount->device);
	error = VNOP_STRATEGY(request);
	if (error == 0) {
		error = buf_biowait(request);
	}
	if (error == 0 && buf_resid(request) != 0) {
		error = EIO;
	}
	buf_free(request);
	return error == 0 ? BTRFS_OK : BTRFS_IO;
}

/* The core writes whole sectors at sector-aligned offsets. */
static enum btrfs_result
btrfs_xnu_resource_write(void *context, uint64_t offset, const void *bytes, size_t length)
{
	struct btrfs_xnu_mount *mount = context;
	const uint8_t *source = bytes;
	size_t amount;
	enum btrfs_result result = BTRFS_OK;

	if (offset % mount->device_block_size != 0 || length % mount->device_block_size != 0 ||
	    offset > mount->device_bytes || length > mount->device_bytes - offset) {
		return BTRFS_INVALID_ARGUMENT;
	}
	if (vnode_getwithref(mount->device) != 0) {
		return BTRFS_IO;
	}
	while (length != 0 && result == BTRFS_OK) {
		amount = length > MAXPHYS ? MAXPHYS : length;
		result = btrfs_xnu_write_aligned(mount, offset, source, amount);
		offset += amount;
		source += amount;
		length -= amount;
	}
	vnode_put(mount->device);
	return result;
}

/* A full cache flush: every completed write is durable when it returns. */
static enum btrfs_result
btrfs_xnu_resource_flush(void *context)
{
	struct btrfs_xnu_mount *mount = context;
	dk_synchronize_t synchronize;
	vfs_context_t ioctl_context;
	int error;

	if (vnode_getwithref(mount->device) != 0) {
		return BTRFS_IO;
	}
	bzero(&synchronize, sizeof(synchronize));
	ioctl_context = vfs_context_create(NULL);
	error = VNOP_IOCTL(
	    mount->device, DKIOCSYNCHRONIZE, (caddr_t)&synchronize, FWRITE, ioctl_context);
	vfs_context_rele(ioctl_context);
	vnode_put(mount->device);
	return error == 0 ? BTRFS_OK : BTRFS_IO;
}

static enum btrfs_result
btrfs_xnu_device_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct btrfs_xnu_mount *mount = context;
	enum btrfs_result result;

	if (mount->staging == NULL) {
		return btrfs_xnu_resource_read(context, offset, buffer, length);
	}
	lck_rw_lock_shared(mount->staging_lock);
	result = btrfs_staging_read(mount->staging, offset, buffer, length);
	lck_rw_unlock_shared(mount->staging_lock);
	return result;
}

static enum btrfs_result
btrfs_xnu_device_write(void *context, uint64_t offset, const void *bytes, size_t length)
{
	struct btrfs_xnu_mount *mount = context;
	enum btrfs_result result;

	if (offset % mount->device_block_size != 0 || length % mount->device_block_size != 0) {
		return BTRFS_INVALID_ARGUMENT;
	}
	lck_rw_lock_exclusive(mount->staging_lock);
	result = btrfs_staging_write(mount->staging, offset, bytes, length);
	lck_rw_unlock_exclusive(mount->staging_lock);
	return result;
}

static enum btrfs_result
btrfs_xnu_device_flush(void *context)
{
	struct btrfs_xnu_mount *mount = context;
	enum btrfs_result result;

	if (mount->staging == NULL) {
		return btrfs_xnu_resource_flush(context);
	}
	lck_rw_lock_exclusive(mount->staging_lock);
	result = btrfs_staging_flush(mount->staging);
	lck_rw_unlock_exclusive(mount->staging_lock);
	return result;
}

static void
btrfs_xnu_volume_lock(void *context)
{
	lck_mtx_lock(((struct btrfs_xnu_mount *)context)->volume_lock);
}

static void
btrfs_xnu_volume_unlock(void *context)
{
	lck_mtx_unlock(((struct btrfs_xnu_mount *)context)->volume_lock);
}

static void
btrfs_xnu_volume_wait(void *context, const void *channel)
{
	(void)msleep((event_t)(uintptr_t)channel, ((struct btrfs_xnu_mount *)context)->volume_lock,
	    PRIBIO, "btrfsvol", NULL);
}

static void
btrfs_xnu_volume_wake(void *context, const void *channel)
{
	(void)context;
	wakeup((event_t)(uintptr_t)channel);
}

static void
btrfs_xnu_cache_lock(void *context)
{
	lck_mtx_lock(((struct btrfs_xnu_mount *)context)->cache_lock);
}

static void
btrfs_xnu_cache_unlock(void *context)
{
	lck_mtx_unlock(((struct btrfs_xnu_mount *)context)->cache_lock);
}

/* Superblock copies left disagreeing by an interrupted publication make a
 * writable open fail (RECOVERY_REQUIRED): a commit from the primary could reuse
 * blocks a newer copy references. A read-write mount, which asked to write,
 * resolves them by explicit recovery to the newest complete root set, which is
 * never older than an acknowledged commit, and opens again. A read-only mount
 * (writer NULL) never writes. */
static enum btrfs_result
btrfs_xnu_open_volume(const struct btrfs_environment *environment,
    const struct btrfs_write_environment *writer, const struct btrfs_volume_locks *locks,
    struct btrfs_volume **volume)
{
	struct btrfs_recovery_report report;
	enum btrfs_result result;

	result = btrfs_volume_open(environment, writer, locks, 0, volume);
	if (result != BTRFS_RECOVERY_REQUIRED || writer == NULL) {
		return result;
	}
	result = btrfs_recover_supers(environment, writer, 0, &report);
	printf("machlin_btrfs: superblock recovery selected generation %llu, rewrote %u "
	       "copies: %s\n",
	    (unsigned long long)report.generation, report.rewritten, btrfs_result_string(result));
	return result == BTRFS_OK ? btrfs_volume_open(environment, writer, locks, 0, volume)
				  : result;
}

int
btrfs_xnu_commit(struct btrfs_xnu_mount *mount, uint64_t generation)
{
	if (generation == 0) {
		return 0;
	}
	return btrfs_xnu_error(btrfs_volume_sync(mount->volume, generation));
}

/* Commits the running transaction every BTRFS_XNU_COMMIT_SECONDS; with
 * nodes_lock held. */
static void
btrfs_xnu_arm_committer(struct btrfs_xnu_mount *mount)
{
	uint64_t deadline;

	clock_interval_to_deadline(BTRFS_XNU_COMMIT_SECONDS, NSEC_PER_SEC, &deadline);
	mount->committer_armed = 1;
	(void)thread_call_enter_delayed(mount->committer, deadline);
}

static void
btrfs_xnu_commit_tick(thread_call_param_t parameter, thread_call_param_t unused)
{
	struct btrfs_xnu_mount *mount = parameter;

	(void)unused;
	(void)btrfs_volume_sync(mount->volume, btrfs_volume_pending(mount->volume));
	lck_mtx_lock(mount->nodes_lock);
	if (mount->stopping) {
		/* The last access to the mount: unmount may free it once woken. */
		mount->committer_armed = 0;
		wakeup(&mount->committer_armed);
	} else {
		btrfs_xnu_arm_committer(mount);
	}
	lck_mtx_unlock(mount->nodes_lock);
}

/* Stops the committer and waits for a running tick, with the public thread
 * call interface only: a cancel that removes the scheduled call ends it,
 * otherwise the running tick ends it. */
static void
btrfs_xnu_stop_committer(struct btrfs_xnu_mount *mount)
{
	if (mount->committer == NULL) {
		return;
	}
	lck_mtx_lock(mount->nodes_lock);
	mount->stopping = 1;
	if (mount->committer_armed && thread_call_cancel(mount->committer)) {
		mount->committer_armed = 0;
	}
	while (mount->committer_armed) {
		(void)msleep(
		    &mount->committer_armed, mount->nodes_lock, PRIBIO, "btrfscommit", NULL);
	}
	lck_mtx_unlock(mount->nodes_lock);
}

static void
btrfs_xnu_free_mount(struct btrfs_xnu_mount *mount)
{
	btrfs_xnu_stop_committer(mount);
	if (mount->committer != NULL) {
		(void)thread_call_free(mount->committer);
	}
	btrfs_identity_destroy(mount->identities);
	btrfs_volume_close(mount->volume);
	btrfs_staging_destroy(mount->staging);
	if (mount->staging_lock != NULL) {
		lck_rw_free(mount->staging_lock, btrfs_xnu_locks);
	}
	/* After every view and stream of the mount is gone. */
	btrfs_cache_destroy(mount->cache);
	if (mount->cache_lock != NULL) {
		lck_mtx_free(mount->cache_lock, btrfs_xnu_locks);
	}
	if (mount->nodes != NULL) {
		hashdestroy(mount->nodes, M_TEMP, mount->nodes_mask);
	}
	if (mount->nodes_lock != NULL) {
		lck_mtx_free(mount->nodes_lock, btrfs_xnu_locks);
	}
	if (mount->creation_lock != NULL) {
		lck_mtx_free(mount->creation_lock, btrfs_xnu_locks);
	}
	if (mount->volume_lock != NULL) {
		lck_mtx_free(mount->volume_lock, btrfs_xnu_locks);
	}
	vnode_rele(mount->device);
	_FREE(mount, M_TEMP);
}

/* Linux removes orphaned inodes of the mounted tree at a read-write mount. */
static int
btrfs_xnu_clean_orphans(struct btrfs_xnu_mount *mount)
{
	struct btrfs_transaction *transaction;
	size_t cleaned = 0;
	int pending = 1;
	enum btrfs_result result = BTRFS_OK;

	/* In bounded steps, each committed; a step without changes writes nothing. */
	while (result == BTRFS_OK && pending) {
		result = btrfs_volume_begin(mount->volume, &transaction);
		if (result != BTRFS_OK) {
			break;
		}
		result = btrfs_transaction_clean_orphans(transaction, mount->info.default_tree,
		    BTRFS_RELEASE_STEP_NODES, &cleaned, &pending);
		if (result == BTRFS_OK) {
			result = btrfs_volume_commit(mount->volume, transaction);
		} else {
			btrfs_volume_abort(mount->volume, transaction);
		}
	}
	return btrfs_xnu_error(result);
}

static int
btrfs_xnu_mount_volume(mount_t mp, vnode_t device, user_addr_t data, vfs_context_t context)
{
	struct btrfs_xnu_mount *mount;
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	struct btrfs_volume_locks locks;
	struct btrfs_cache_locks cache_locks;
	struct btrfs_staging_limits staging_limits = { BTRFS_STAGING_MAX_BYTES,
		BTRFS_STAGING_MAX_RUN_BYTES, BTRFS_STAGING_MAX_RUNS };
	struct btrfs_volume_view *view;
	const struct btrfs_fs *fs;
	struct btrfs_inode root;
	struct vfsioattr io;
	uint64_t blocks;
	uint32_t block_size;
	uint32_t writable = 0;
	int read_only = (vfs_flags(mp) & MNT_RDONLY) != 0;
	int error;

	(void)data;
	if (vfs_flags(mp) & MNT_UPDATE) {
		return ENOTSUP;
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
	if (block_size < DEV_BSIZE || block_size > page_size ||
	    (block_size & (block_size - 1)) != 0 || blocks > INT64_MAX / block_size) {
		return ENOTSUP;
	}
	if (!read_only) {
		error = VNOP_IOCTL(device, DKIOCISWRITABLE, (caddr_t)&writable, 0, context);
		if (error != 0 || writable == 0) {
			return EROFS;
		}
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
	mount->device_bytes = blocks * block_size;
	mount->nodes_lock = lck_mtx_alloc_init(btrfs_xnu_locks, LCK_ATTR_NULL);
	mount->creation_lock = lck_mtx_alloc_init(btrfs_xnu_locks, LCK_ATTR_NULL);
	mount->volume_lock = lck_mtx_alloc_init(btrfs_xnu_locks, LCK_ATTR_NULL);
	mount->cache_lock = lck_mtx_alloc_init(btrfs_xnu_locks, LCK_ATTR_NULL);
	if (!read_only) {
		mount->staging_lock = lck_rw_alloc_init(btrfs_xnu_locks, LCK_ATTR_NULL);
	}
	mount->nodes =
	    hashinit(MAX(desiredvnodes / BTRFS_XNU_VNODES_PER_BUCKET, BTRFS_XNU_MINIMUM_BUCKETS),
		M_TEMP, &mount->nodes_mask);
	if (mount->nodes_lock == NULL || mount->creation_lock == NULL ||
	    mount->volume_lock == NULL || mount->cache_lock == NULL || mount->nodes == NULL ||
	    (!read_only && mount->staging_lock == NULL)) {
		btrfs_xnu_free_mount(mount);
		return ENOMEM;
	}
	/* Durability needs a working cache flush before any write. */
	if (!read_only && btrfs_xnu_device_flush(mount) != BTRFS_OK) {
		btrfs_xnu_free_mount(mount);
		return ENOTSUP;
	}
	bzero(&environment, sizeof(environment));
	environment.context = mount;
	environment.size_bytes = mount->device_bytes;
	environment.read = btrfs_xnu_resource_read;
	environment.allocate = btrfs_xnu_allocate;
	environment.release = btrfs_xnu_release;
	environment.decompress = btrfs_xnu_decompress;
	cache_locks.context = mount;
	cache_locks.lock = btrfs_xnu_cache_lock;
	cache_locks.unlock = btrfs_xnu_cache_unlock;
	/* Storage is allocated at first use; without it reads bypass the cache. */
	error = btrfs_xnu_error(
	    btrfs_cache_create(&environment, &cache_locks, BTRFS_XNU_CACHE_BYTES, &mount->cache));
	if (error != 0) {
		btrfs_xnu_free_mount(mount);
		return error;
	}
	environment.cache = mount->cache;
	writer.context = mount;
	writer.write = btrfs_xnu_resource_write;
	writer.flush = btrfs_xnu_resource_flush;
	/* The kernel adapter writes no compressed data yet. */
	writer.compress = NULL;
	writer.compression = BTRFS_COMPRESSION_NONE;
	if (!read_only) {
		error = btrfs_xnu_error(
		    btrfs_staging_create(&environment, &writer, staging_limits, &mount->staging));
		if (error != 0) {
			btrfs_xnu_free_mount(mount);
			return error;
		}
		writer.write = btrfs_xnu_device_write;
		writer.flush = btrfs_xnu_device_flush;
	}
	environment.read = btrfs_xnu_device_read;
	locks.context = mount;
	locks.lock = btrfs_xnu_volume_lock;
	locks.unlock = btrfs_xnu_volume_unlock;
	locks.wait = btrfs_xnu_volume_wait;
	locks.wake = btrfs_xnu_volume_wake;
	error = btrfs_xnu_error(btrfs_xnu_open_volume(
	    &environment, read_only ? NULL : &writer, &locks, &mount->volume));
	if (error != 0) {
		btrfs_xnu_free_mount(mount);
		return error;
	}
	fs = btrfs_volume_pin(mount->volume, &view);
	btrfs_get_info(fs, &mount->info);
	error = btrfs_xnu_error(btrfs_root(fs, &root));
	btrfs_volume_unpin(mount->volume, view);
	if (error == 0) {
		error = btrfs_xnu_error(
		    btrfs_identity_create(&environment, root.id, &mount->identities));
	}
	if (error == 0 && mount->info.sector_size < block_size) {
		error = ENOTSUP;
	}
	if (error == 0 && !read_only) {
		error = btrfs_xnu_clean_orphans(mount);
	}
	mount->synchronous = (vfs_flags(mp) & MNT_SYNCHRONOUS) != 0;
	/* A synchronous mount commits its operations and synchronous writes;
	 * data pushed otherwise (mapped pages, write-behind) still waits. */
	if (error == 0 && !read_only) {
		mount->committer = thread_call_allocate(btrfs_xnu_commit_tick, mount);
		error = mount->committer == NULL ? ENOMEM : 0;
	}
	if (error != 0) {
		btrfs_xnu_free_mount(mount);
		return error;
	}
	if (mount->committer != NULL) {
		lck_mtx_lock(mount->nodes_lock);
		btrfs_xnu_arm_committer(mount);
		lck_mtx_unlock(mount->nodes_lock);
	}
	vfs_setfsprivate(mp, mount);
	vfs_setflags(mp, read_only ? MNT_LOCAL | MNT_RDONLY : MNT_LOCAL);
	vfs_getnewfsid(mp);
	vfs_setlocklocal(mp);
	vfs_ioattr(mp, &io);
	io.io_devblocksize = block_size;
	vfs_setioattr(mp, &io);
	return 0;
}

struct btrfs_xnu_sync {
	int wait;
	int error;
};

static int
btrfs_xnu_sync_vnode(vnode_t vnode, void *argument)
{
	struct btrfs_xnu_sync *sync = argument;
	int error;

	if (vnode_vtype(vnode) == VREG) {
		error = btrfs_xnu_push_data(vnode, sync->wait);
		if (sync->error == 0) {
			sync->error = error;
		}
	}
	return VNODE_RETURNED;
}

int
btrfs_xnu_sync_all(mount_t mp, int wait)
{
	struct btrfs_xnu_mount *mount = vfs_fsprivate(mp);
	struct btrfs_xnu_sync sync = { wait, 0 };
	int error;

	if (!btrfs_volume_writable(mount->volume)) {
		return btrfs_xnu_error(btrfs_volume_failure(mount->volume));
	}
	(void)vnode_iterate(mp, 0, btrfs_xnu_sync_vnode, &sync);
	/* The pushed data and every earlier operation. */
	error = btrfs_xnu_commit(mount, btrfs_volume_pending(mount->volume));
	if (sync.error != 0) {
		return sync.error;
	}
	return error != 0 ? error : btrfs_xnu_error(btrfs_volume_failure(mount->volume));
}

static int
btrfs_xnu_unmount_volume(mount_t mp, int flags, vfs_context_t context)
{
	struct btrfs_xnu_mount *mount = vfs_fsprivate(mp);
	int error;

	(void)context;
	error = btrfs_xnu_sync_all(mp, 1);
	if (error != 0 && !(flags & MNT_FORCE)) {
		return error;
	}
	error = vflush(mp, NULL, (flags & MNT_FORCE) ? FORCECLOSE : 0);
	if (error != 0) {
		return error;
	}
	/* Inactive orphans evicted during vflush joined the running transaction. */
	error = btrfs_xnu_commit(mount, btrfs_volume_pending(mount->volume));
	if (error == 0) {
		error = btrfs_xnu_error(btrfs_volume_failure(mount->volume));
	}
	if (error != 0 && !(flags & MNT_FORCE)) {
		return error;
	}
	btrfs_xnu_stop_committer(mount);
	vfs_setfsprivate(mp, NULL);
	btrfs_xnu_free_mount(mount);
	return 0;
}

static int
btrfs_xnu_root(mount_t mp, vnode_t *result, vfs_context_t context)
{
	(void)context;
	return btrfs_xnu_root_node(vfs_fsprivate(mp), result);
}

static int
btrfs_xnu_vget(mount_t mp, ino64_t number, vnode_t *result, vfs_context_t context)
{
	(void)context;
	return btrfs_xnu_number_node(vfs_fsprivate(mp), number, result);
}

static int
btrfs_xnu_volume_attributes(mount_t mp, struct vfs_attr *attributes, vfs_context_t context)
{
	struct btrfs_xnu_mount *mount = vfs_fsprivate(mp);
	struct btrfs_volume_view *view;
	struct btrfs_info info;

	(void)context;
	btrfs_get_info(btrfs_volume_pin(mount->volume, &view), &info);
	btrfs_volume_unpin(mount->volume, view);
	VFSATTR_RETURN(attributes, f_bsize, info.sector_size);
	VFSATTR_RETURN(attributes, f_iosize, info.sector_size);
	VFSATTR_RETURN(attributes, f_blocks, info.total_bytes / info.sector_size);
	VFSATTR_RETURN(
	    attributes, f_bfree, (info.total_bytes - info.used_bytes) / info.sector_size);
	VFSATTR_RETURN(attributes, f_bavail,
	    btrfs_volume_writable(mount->volume)
		? (info.total_bytes - info.used_bytes) / info.sector_size
		: 0);
	VFSATTR_RETURN(attributes, f_bused, info.used_bytes / info.sector_size);
	VFSATTR_RETURN(attributes, f_files, 0);
	VFSATTR_RETURN(attributes, f_ffree, 0);
	VFSATTR_RETURN(attributes, f_fssubtype, 0);
	return 0;
}

static int
btrfs_xnu_sync(mount_t mp, int flags, vfs_context_t context)
{
	(void)context;
	return btrfs_xnu_sync_all(mp, (flags & MNT_WAIT) != 0);
}

struct vfsops btrfs_xnu_vfsops = { .vfs_mount = btrfs_xnu_mount_volume,
	.vfs_unmount = btrfs_xnu_unmount_volume,
	.vfs_root = btrfs_xnu_root,
	.vfs_getattr = btrfs_xnu_volume_attributes,
	.vfs_sync = btrfs_xnu_sync,
	.vfs_vget = btrfs_xnu_vget };
