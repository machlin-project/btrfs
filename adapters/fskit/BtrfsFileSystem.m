/* SPDX-License-Identifier: BSD-3-Clause */
#import "BtrfsDeviceBarrier.h"
#import "BtrfsFileSystemInternal.h"
#include <btrfs/btrfs.h>
#include <btrfs/identity.h>
#include <btrfs/native.h>
#include <btrfs/volume.h>
#include <btrfs/write.h>
#include <zlib.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <time.h>

@implementation FSBlockDeviceResource (BtrfsBlockReader)
@end

@interface BtrfsDeviceBarrier (BtrfsDeviceFlusher) <BtrfsDeviceFlusher>
@end

@implementation BtrfsDeviceBarrier (BtrfsDeviceFlusher)
@end

uint32_t
btrfs_fskit_flags(uint64_t flags)
{
	uint32_t result = 0;

	if (flags & BTRFS_INODE_FLAG_IMMUTABLE) {
		result |= SF_IMMUTABLE;
	}
	if (flags & BTRFS_INODE_FLAG_APPEND) {
		result |= SF_APPEND;
	}
	if (flags & BTRFS_INODE_FLAG_NODUMP) {
		result |= UF_NODUMP;
	}
	return result;
}

/* Superblock copies left disagreeing by an interrupted publication make a
 * writable open fail (RECOVERY_REQUIRED): a commit from the primary could reuse
 * blocks a newer copy references. A writable volume resolves them by explicit
 * recovery to the newest complete root set, never older than an acknowledged
 * commit, and opens again. A read-only volume (writer NULL) never writes. */
static enum btrfs_result
btrfs_fskit_open_volume(const struct btrfs_environment *environment,
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
	NSLog(@"machlinbtrfs: superblock recovery selected generation %llu, rewrote %u copies: %s",
	    (unsigned long long)report.generation, report.rewritten, btrfs_result_string(result));
	return result == BTRFS_OK ? btrfs_volume_open(environment, writer, locks, 0, volume)
				  : result;
}

NSError *
btrfs_fskit_error(enum btrfs_result result)
{
	int error;

	switch (result) {
	case BTRFS_OK:
		return nil;
	case BTRFS_NO_MEMORY:
		error = ENOMEM;
		break;
	case BTRFS_NOT_FOUND:
		error = ENOENT;
		break;
	case BTRFS_NOT_DIRECTORY:
		error = ENOTDIR;
		break;
	case BTRFS_READ_ONLY:
		error = EROFS;
		break;
	case BTRFS_IS_DIRECTORY:
		error = EISDIR;
		break;
	case BTRFS_UNSUPPORTED:
		error = ENOTSUP;
		break;
	case BTRFS_INVALID_ARGUMENT:
	case BTRFS_NOT_BTRFS:
		error = EINVAL;
		break;
	case BTRFS_RANGE:
		error = EOVERFLOW;
		break;
	case BTRFS_STALE:
		error = ESTALE;
		break;
	case BTRFS_EXISTS:
		error = EEXIST;
		break;
	case BTRFS_NO_SPACE:
		error = ENOSPC;
		break;
	case BTRFS_NOT_EMPTY:
		error = ENOTEMPTY;
		break;
	case BTRFS_NOT_PERMITTED:
		error = EPERM;
		break;
	case BTRFS_NAME_TOO_LONG:
		error = ENAMETOOLONG;
		break;
	case BTRFS_CROSS_TREE:
		error = EXDEV;
		break;
	case BTRFS_TOO_MANY_LINKS:
		error = EMLINK;
		break;
	case BTRFS_CORRUPT:
	case BTRFS_RECOVERY_REQUIRED:
	case BTRFS_IO:
	default:
		error = EIO;
		break;
	}
	return [NSError errorWithDomain:NSPOSIXErrorDomain
				   code:error
			       userInfo:@{
				       NSLocalizedDescriptionKey : @(btrfs_result_string(result))
			       }];
}

struct btrfs_time
btrfs_fskit_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_REALTIME, &now);
	return (struct btrfs_time){ now.tv_sec, (uint32_t)now.tv_nsec };
}

/* Reads exactly length bytes at an aligned offset, continuing after partial
 * reads; a revoked resource or a read that makes no progress fails. */
static enum btrfs_result
btrfs_reader_exact(id<BtrfsBlockReader> reader, uint64_t offset, uint8_t *buffer, size_t length)
{
	NSError *error = nil;
	size_t done = 0;
	size_t completed;

	while (done < length) {
		if (reader.isRevoked) {
			return BTRFS_IO;
		}
		completed = [reader readInto:buffer + done
				  startingAt:(off_t)(offset + done)
				      length:length - done
				       error:&error];
		if (error != nil || completed == 0 || completed > length - done) {
			return BTRFS_IO;
		}
		done += completed;
	}
	return BTRFS_OK;
}

/* Aligned requests go straight into the caller's buffer; others read the
 * covering sectors into a bounce buffer. A failed read leaves no partial
 * data in the caller's buffer. */
static enum btrfs_result
btrfs_resource_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	id<BtrfsBlockReader> reader = (__bridge id<BtrfsBlockReader>)context;
	uint8_t *bounce;
	uint64_t alignment = reader.physicalBlockSize;
	uint64_t start;
	uint64_t total;
	enum btrfs_result error;

	if (alignment == 0 || alignment > SIZE_MAX || offset > INT64_MAX ||
	    length > (uint64_t)INT64_MAX - offset) {
		return BTRFS_IO;
	}
	if (length == 0) {
		return BTRFS_OK;
	}
	start = offset - offset % alignment;
	total = offset - start + length;
	if (total > SIZE_MAX - (alignment - 1)) {
		return BTRFS_RANGE;
	}
	total = ((total + alignment - 1) / alignment) * alignment;
	if (start > reader.blockCount * reader.blockSize ||
	    total > reader.blockCount * reader.blockSize - start) {
		return BTRFS_IO;
	}
	if (start == offset && total == length) {
		error = btrfs_reader_exact(reader, offset, buffer, length);
		if (error != BTRFS_OK) {
			memset(buffer, 0, length);
		}
		return error;
	}
	bounce = malloc((size_t)total);
	if (bounce == NULL) {
		return BTRFS_NO_MEMORY;
	}
	error = btrfs_reader_exact(reader, start, bounce, (size_t)total);
	if (error == BTRFS_OK) {
		memcpy(buffer, bounce + (offset - start), length);
	}
	free(bounce);
	return error;
}

/* The writer issues whole sectors at aligned offsets; partial writes continue
 * and a revoked resource fails before the device is touched again. */
static enum btrfs_result
btrfs_resource_write(void *context, uint64_t offset, const void *bytes, size_t length)
{
	id<BtrfsBlockWriter> writer = (__bridge id<BtrfsBlockWriter>)context;
	NSError *error = nil;
	uint64_t alignment = writer.physicalBlockSize;
	size_t done = 0;
	size_t completed;

	if (alignment == 0 || offset % alignment != 0 || length % alignment != 0 ||
	    offset > writer.blockCount * writer.blockSize ||
	    length > writer.blockCount * writer.blockSize - offset) {
		return BTRFS_INVALID_ARGUMENT;
	}
	while (done < length) {
		if (writer.isRevoked) {
			return BTRFS_IO;
		}
		completed = [writer writeFrom:(uint8_t *)(uintptr_t)bytes + done
				   startingAt:(off_t)(offset + done)
				       length:length - done
					error:&error];
		if (error != nil || completed == 0 || completed > length - done) {
			return BTRFS_IO;
		}
		done += completed;
	}
	return BTRFS_OK;
}

static void *
btrfs_resource_allocate(void *context, size_t size)
{
	(void)context;
	return malloc(size);
}

static void
btrfs_resource_release(void *context, void *allocation, size_t size)
{
	(void)context;
	(void)size;
	free(allocation);
}

static enum btrfs_result
btrfs_resource_decompress(void *context, enum btrfs_compression codec, const void *input,
    size_t input_size, void *output, size_t output_size)
{
	uLongf actual = output_size;
	int result;

	(void)context;
	if (codec != BTRFS_COMPRESSION_ZLIB) {
		return BTRFS_UNSUPPORTED;
	}
	result = uncompress(output, &actual, input, input_size);
	return result == Z_OK && actual == output_size
	    ? BTRFS_OK
	    : (result == Z_MEM_ERROR ? BTRFS_NO_MEMORY : BTRFS_CORRUPT);
}

/* Verified tree nodes shared by a loaded volume's threads; only the volume's
 * own commits change the device while it is loaded. */
#define BTRFS_FSKIT_CACHE_BYTES (32U * 1024U * 1024U)
/* The first DIR_INDEX key: lower enumeration cookies name "." and "..". */
#define BTRFS_FSKIT_FIRST_INDEX 2U

struct btrfs_fskit_cache {
	pthread_mutex_t mutex;
	struct btrfs_cache *cache;
};

static void
btrfs_fskit_cache_lock(void *context)
{
	pthread_mutex_lock(context);
}

static void
btrfs_fskit_cache_unlock(void *context)
{
	pthread_mutex_unlock(context);
}

void
btrfs_fskit_cache_destroy(struct btrfs_fskit_cache *owner)
{
	if (owner == NULL) {
		return;
	}
	btrfs_cache_destroy(owner->cache);
	pthread_mutex_destroy(&owner->mutex);
	free(owner);
}

/* NULL when the cache cannot be set up; the volume then reads uncached. */
struct btrfs_fskit_cache *
btrfs_fskit_cache_create(void)
{
	struct btrfs_environment environment = { 0 };
	struct btrfs_cache_locks locks;
	struct btrfs_fskit_cache *owner;

	owner = calloc(1, sizeof(*owner));
	if (owner == NULL) {
		return NULL;
	}
	if (pthread_mutex_init(&owner->mutex, NULL) != 0) {
		free(owner);
		return NULL;
	}
	environment.allocate = btrfs_resource_allocate;
	environment.release = btrfs_resource_release;
	locks.context = &owner->mutex;
	locks.lock = btrfs_fskit_cache_lock;
	locks.unlock = btrfs_fskit_cache_unlock;
	if (btrfs_cache_create(&environment, &locks, BTRFS_FSKIT_CACHE_BYTES, &owner->cache) !=
	    BTRFS_OK) {
		pthread_mutex_destroy(&owner->mutex);
		free(owner);
		return NULL;
	}
	return owner;
}

/* The volume layer's mutex and wake channels; every wake broadcasts, and the
 * volume rechecks its conditions after each wait. */
struct btrfs_fskit_locks {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
};

static void
btrfs_fskit_volume_lock(void *context)
{
	pthread_mutex_lock(&((struct btrfs_fskit_locks *)context)->mutex);
}

static void
btrfs_fskit_volume_unlock(void *context)
{
	pthread_mutex_unlock(&((struct btrfs_fskit_locks *)context)->mutex);
}

static void
btrfs_fskit_volume_wait(void *context, const void *channel)
{
	struct btrfs_fskit_locks *locks = context;

	(void)channel;
	pthread_cond_wait(&locks->condition, &locks->mutex);
}

static void
btrfs_fskit_volume_wake(void *context, const void *channel)
{
	(void)channel;
	pthread_cond_broadcast(&((struct btrfs_fskit_locks *)context)->condition);
}

static void
btrfs_fskit_locks_destroy(struct btrfs_fskit_locks *locks)
{
	if (locks == NULL) {
		return;
	}
	pthread_cond_destroy(&locks->condition);
	pthread_mutex_destroy(&locks->mutex);
	free(locks);
}

static struct btrfs_fskit_locks *
btrfs_fskit_locks_create(void)
{
	struct btrfs_fskit_locks *locks = calloc(1, sizeof(*locks));

	if (locks == NULL) {
		return NULL;
	}
	if (pthread_mutex_init(&locks->mutex, NULL) != 0) {
		free(locks);
		return NULL;
	}
	if (pthread_cond_init(&locks->condition, NULL) != 0) {
		pthread_mutex_destroy(&locks->mutex);
		free(locks);
		return NULL;
	}
	return locks;
}

/* The writer's device: direct writes through the resource and the flusher's
 * cache flush as every barrier. A failed flush fails the commit; it never
 * acknowledges persistence. */
@interface BtrfsDevice : NSObject
@property(readonly) id<BtrfsBlockWriter> writer;
@property(readonly) id<BtrfsDeviceFlusher> flusher;
- (instancetype)initWithWriter:(id<BtrfsBlockWriter>)writer flusher:(id<BtrfsDeviceFlusher>)flusher;
@end

@implementation BtrfsDevice

- (instancetype)initWithWriter:(id<BtrfsBlockWriter>)writer flusher:(id<BtrfsDeviceFlusher>)flusher
{
	self = [super init];
	if (self != nil) {
		_writer = writer;
		_flusher = flusher;
	}
	return self;
}

@end

static enum btrfs_result
btrfs_device_write(void *context, uint64_t offset, const void *bytes, size_t length)
{
	BtrfsDevice *device = (__bridge BtrfsDevice *)context;

	return btrfs_resource_write((__bridge void *)device.writer, offset, bytes, length);
}

static enum btrfs_result
btrfs_device_flush(void *context)
{
	BtrfsDevice *device = (__bridge BtrfsDevice *)context;
	NSError *error = nil;

	if (device.writer.isRevoked) {
		return BTRFS_IO;
	}
	return [device.flusher synchronizeWithError:&error] && error == nil ? BTRFS_OK : BTRFS_IO;
}

static FSItemType
btrfs_item_type(uint32_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:
		return FSItemTypeFile;
	case S_IFDIR:
		return FSItemTypeDirectory;
	case S_IFLNK:
		return FSItemTypeSymlink;
	case S_IFIFO:
		return FSItemTypeFIFO;
	case S_IFCHR:
		return FSItemTypeCharDevice;
	case S_IFBLK:
		return FSItemTypeBlockDevice;
	case S_IFSOCK:
		return FSItemTypeSocket;
	default:
		return FSItemTypeUnknown;
	}
}

static struct timespec
btrfs_timespec(struct btrfs_time time)
{
	struct timespec value;

	value.tv_sec = time.seconds;
	value.tv_nsec = time.nanoseconds;
	return value;
}

@implementation BtrfsItem
@end

@implementation BtrfsVolume {
	BtrfsDevice *_device;
}

+ (instancetype)volumeWithReader:(id<BtrfsBlockReader>)reader
			 flusher:(id<BtrfsDeviceFlusher>)flusher
			   cache:(struct btrfs_fskit_cache *)cache
			  result:(enum btrfs_result *)result
{
	struct btrfs_environment environment;
	struct btrfs_write_environment writer;
	struct btrfs_volume_locks locks;
	struct btrfs_fskit_locks *owned;
	struct btrfs_volume *volume = NULL;
	struct btrfs_volume_view *view;
	struct btrfs_info info;
	const struct btrfs_fs *fs;
	BtrfsDevice *device = nil;
	BtrfsVolume *created;

	*result = BTRFS_OK;
	if (reader.blockSize == 0 || reader.blockCount > UINT64_MAX / reader.blockSize) {
		*result = BTRFS_CORRUPT;
		return nil;
	}
	owned = btrfs_fskit_locks_create();
	if (owned == NULL) {
		*result = BTRFS_NO_MEMORY;
		return nil;
	}
	memset(&environment, 0, sizeof(environment));
	environment.context = (__bridge void *)reader;
	environment.size_bytes = reader.blockCount * reader.blockSize;
	environment.read = btrfs_resource_read;
	environment.allocate = btrfs_resource_allocate;
	environment.release = btrfs_resource_release;
	environment.decompress = btrfs_resource_decompress;
	environment.cache = cache == NULL ? NULL : cache->cache;
	locks = (struct btrfs_volume_locks){ owned, btrfs_fskit_volume_lock,
		btrfs_fskit_volume_unlock, btrfs_fskit_volume_wait, btrfs_fskit_volume_wake };
	if (flusher != nil && [reader conformsToProtocol:@protocol(BtrfsBlockWriter)] &&
	    ((id<BtrfsBlockWriter>)reader).isWritable) {
		device = [[BtrfsDevice alloc] initWithWriter:(id<BtrfsBlockWriter>)reader
						     flusher:flusher];
		memset(&writer, 0, sizeof(writer));
		writer.context = (__bridge void *)device;
		writer.write = btrfs_device_write;
		writer.flush = btrfs_device_flush;
		writer.compression = BTRFS_COMPRESSION_NONE;
	}
	*result =
	    btrfs_fskit_open_volume(&environment, device == nil ? NULL : &writer, &locks, &volume);
	if (*result != BTRFS_OK) {
		btrfs_fskit_locks_destroy(owned);
		return nil;
	}
	fs = btrfs_volume_pin(volume, &view);
	btrfs_get_info(fs, &info);
	btrfs_volume_unpin(volume, view);
	created = [[self alloc]
	    initWithVolumeID:[[FSVolumeIdentifier alloc]
				 initWithUUID:[[NSUUID alloc] initWithUUIDBytes:info.uuid]]
		  volumeName:[FSFileName nameWithBytes:info.label
						length:strnlen(info.label, BTRFS_LABEL_SIZE)]];
	if (created == nil) {
		btrfs_volume_close(volume);
		btrfs_fskit_locks_destroy(owned);
		*result = BTRFS_NO_MEMORY;
		return nil;
	}
	*result = [created adoptVolume:volume
				 locks:owned
				reader:reader
				device:device
				 cache:cache
				  tree:info.default_tree];
	return *result == BTRFS_OK ? created : nil;
}

/* Takes ownership of volume and locks; a failure releases them. */
- (enum btrfs_result)adoptVolume:(struct btrfs_volume *)volume
			   locks:(struct btrfs_fskit_locks *)locks
			  reader:(id<BtrfsBlockReader>)reader
			  device:(BtrfsDevice *)device
			   cache:(struct btrfs_fskit_cache *)cache
			    tree:(uint64_t)tree
{
	struct btrfs_environment environment = { 0 };
	enum btrfs_result error;

	_volume = volume;
	_locks = locks;
	_reader = reader;
	_device = device;
	_flusher = device.flusher;
	_cache = cache;
	_writable = btrfs_volume_writable(volume) != 0;
	_itemLock = [[NSLock alloc] init];
	_items = [NSMapTable strongToWeakObjectsMapTable];
	_versions = 1;
	_rootIdentity = (struct btrfs_object_id){ tree, BTRFS_ROOT_INODE };
	environment.allocate = btrfs_resource_allocate;
	environment.release = btrfs_resource_release;
	error = btrfs_identity_create(&environment, _rootIdentity, &_identities);
	if (error == BTRFS_OK && _writable) {
		error = [self cleanOrphans];
	}
	if (error == BTRFS_OK && _writable) {
		[self startCommitter];
	}
	return error;
}

/* Deletes the inodes a crash left open-unlinked, as Linux does at mount. */
- (enum btrfs_result)cleanOrphans
{
	struct btrfs_transaction *transaction;
	size_t cleaned = 0;
	enum btrfs_result result;

	result = btrfs_volume_begin(_volume, &transaction);
	if (result != BTRFS_OK) {
		return result;
	}
	result = btrfs_transaction_clean_orphans(transaction, _rootIdentity.tree, &cleaned);
	if (result == BTRFS_OK && cleaned != 0) {
		return btrfs_volume_commit(_volume, transaction);
	}
	btrfs_volume_abort(_volume, transaction);
	return result;
}

- (void)startCommitter
{
	__weak BtrfsVolume *weak = self;

	_commitQueue = dispatch_queue_create("org.machlin.btrfs.commit", DISPATCH_QUEUE_SERIAL);
	_committer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _commitQueue);
	dispatch_source_set_timer(_committer,
	    dispatch_time(DISPATCH_TIME_NOW, (int64_t)BTRFS_FSKIT_COMMIT_SECONDS * NSEC_PER_SEC),
	    (uint64_t)BTRFS_FSKIT_COMMIT_SECONDS * NSEC_PER_SEC, NSEC_PER_SEC);
	dispatch_source_set_event_handler(_committer, ^{
	  BtrfsVolume *volume = weak;

	  if (volume != nil) {
		  (void)btrfs_volume_sync(volume->_volume, btrfs_volume_pending(volume->_volume));
	  }
	});
	dispatch_resume(_committer);
}

- (void)stopCommitter
{
	dispatch_source_t committer;

	@synchronized(self) {
		committer = _committer;
		_committer = nil;
	}
	if (committer != nil) {
		dispatch_source_cancel(committer);
		/* A tick already running finishes before the volume goes. */
		dispatch_sync(_commitQueue,
		    ^{
		    });
	}
}

- (enum btrfs_result)shutdown
{
	[self stopCommitter];
	if (!_writable) {
		return BTRFS_OK;
	}
	return btrfs_volume_sync(_volume, btrfs_volume_pending(_volume));
}

- (void)dealloc
{
	[self stopCommitter];
	btrfs_identity_destroy(_identities);
	if (_volume != NULL) {
		btrfs_volume_close(_volume);
	}
	/* After every view: nothing reads through the cache any more. */
	btrfs_fskit_cache_destroy(_cache);
	btrfs_fskit_locks_destroy(_locks);
}

- (BOOL)isWritable
{
	return _writable;
}

- (NSNumber *)numberForIdentity:(struct btrfs_object_id)identity
{
	uint64_t number;
	enum btrfs_result result;

	[_itemLock lock];
	result = btrfs_identity_get(_identities, identity, &number);
	[_itemLock unlock];
	return result == BTRFS_OK ? @(number) : nil;
}

- (BtrfsItem *)itemForInode:(const struct btrfs_inode *)inode
{
	BtrfsItem *item;
	NSNumber *key = [self numberForIdentity:inode->id];

	if (key == nil) {
		return nil;
	}
	[_itemLock lock];
	item = [_items objectForKey:key];
	if (item == nil) {
		item = [[BtrfsItem alloc] init];
		item->version = _versions;
		[_items setObject:item forKey:key];
	}
	/* The newest state wins; an orphan keeps its own record. */
	if (!item->orphan) {
		item->inode = *inode;
	}
	[_itemLock unlock];
	return item;
}

- (void)changedDirectory:(BtrfsItem *)directory
{
	[_itemLock lock];
	if (++_versions == FSDirectoryVerifierInitial) {
		_versions++;
	}
	directory->version = _versions;
	[_itemLock unlock];
}

- (enum btrfs_result)refreshItem:(BtrfsItem *)item
{
	struct btrfs_volume_view *view;
	struct btrfs_inode inode;
	struct btrfs_object_id identity;
	const struct btrfs_fs *fs;
	enum btrfs_result error;

	[_itemLock lock];
	identity = item->inode.id;
	[_itemLock unlock];
	fs = btrfs_volume_read(_volume, &view);
	error = btrfs_get_inode(fs, identity, &inode);
	btrfs_volume_unread(_volume, view);
	if (error == BTRFS_OK) {
		[_itemLock lock];
		item->inode = inode;
		[_itemLock unlock];
	}
	return error;
}

- (enum btrfs_result)changeWithNodes:(size_t)nodes
			       first:(BtrfsItem *)first
			      second:(BtrfsItem *)second
			   operation:(enum btrfs_result (^)(struct btrfs_transaction *))operation
{
	struct btrfs_transaction *transaction;
	uint64_t pending;
	enum btrfs_result result;

	if (!_writable) {
		return BTRFS_READ_ONLY;
	}
	result = btrfs_volume_join(_volume, nodes, &transaction);
	if (result != BTRFS_OK) {
		return result;
	}
	pending = btrfs_volume_pending(_volume);
	result = operation(transaction);
	btrfs_volume_leave(_volume, transaction);
	if (result == BTRFS_OK) {
		[_itemLock lock];
		if (first != nil) {
			first->pending = pending;
		}
		if (second != nil) {
			second->pending = pending;
		}
		[_itemLock unlock];
	}
	return result;
}

- (void)setHolder:(BtrfsItem *)directory ofItem:(BtrfsItem *)item
{
	NSNumber *number = [self numberForIdentity:[self identityOfItem:directory]];

	if (number != nil) {
		[_itemLock lock];
		item->holder = number.unsignedLongLongValue;
		[_itemLock unlock];
	}
}

- (struct btrfs_object_id)identityOfItem:(BtrfsItem *)item
{
	struct btrfs_object_id identity;

	[_itemLock lock];
	identity = item->inode.id;
	[_itemLock unlock];
	return identity;
}

/* The mount root's parent is FSItemIDParentOfRoot; a directory has one parent
 * in its tree. A file's parent is the holder its caller knows. */
- (enum btrfs_result)parentOfInode:(const struct btrfs_inode *)inode number:(uint64_t *)number
{
	struct btrfs_volume_view *view;
	struct btrfs_inode parent;
	const struct btrfs_fs *fs;
	NSNumber *found;
	enum btrfs_result error;

	if (inode->id.tree == _rootIdentity.tree && inode->id.inode == _rootIdentity.inode) {
		*number = FSItemIDParentOfRoot;
		return BTRFS_OK;
	}
	if ((inode->mode & BTRFS_MODE_TYPE) != BTRFS_MODE_DIRECTORY) {
		return BTRFS_NOT_FOUND;
	}
	fs = btrfs_volume_read(_volume, &view);
	error = btrfs_parent(fs, inode, &parent);
	btrfs_volume_unread(_volume, view);
	if (error == BTRFS_OK) {
		found = [self numberForIdentity:parent.id];
		error = found == nil ? BTRFS_NO_MEMORY : BTRFS_OK;
		*number = found.unsignedLongLongValue;
	}
	return error;
}

- (FSItemAttributes *)attributesForItem:(BtrfsItem *)item result:(enum btrfs_result *)result
{
	struct btrfs_inode inode;
	uint64_t holder;
	uint64_t parent = 0;

	[_itemLock lock];
	inode = item->inode;
	holder = item->holder;
	[_itemLock unlock];
	*result = [self parentOfInode:&inode number:&parent];
	/* A file, or a stub directory without a tree parent, keeps the directory
	 * it was reached through. */
	if (*result == BTRFS_NOT_FOUND && holder != 0) {
		parent = holder;
		*result = BTRFS_OK;
	}
	return *result == BTRFS_OK ? [self attributesForInode:&inode parent:parent result:result]
				   : nil;
}

- (FSItemAttributes *)attributesForInode:(const struct btrfs_inode *)inode
				  parent:(uint64_t)parent
				  result:(enum btrfs_result *)result
{
	FSItemAttributes *attributes = [[FSItemAttributes alloc] init];
	NSNumber *number = [self numberForIdentity:inode->id];

	*result = BTRFS_OK;
	if (parent == 0) {
		*result = [self parentOfInode:inode number:&parent];
	}
	if (*result == BTRFS_OK && number == nil) {
		*result = BTRFS_NO_MEMORY;
	}
	if (*result != BTRFS_OK) {
		return nil;
	}
	attributes.parentID = parent;
	attributes.flags = btrfs_fskit_flags(inode->flags);
	attributes.uid = inode->uid;
	attributes.gid = inode->gid;
	attributes.mode = inode->mode & ALLPERMS;
	attributes.type = btrfs_item_type(inode->mode);
	attributes.linkCount = inode->links;
	attributes.size = inode->size;
	attributes.allocSize = inode->allocated_bytes;
	attributes.fileID = number.unsignedLongLongValue;
	attributes.inhibitKernelOffloadedIO = YES;
	attributes.modifyTime = btrfs_timespec(inode->modify_time);
	attributes.changeTime = btrfs_timespec(inode->change_time);
	attributes.accessTime = btrfs_timespec(inode->access_time);
	attributes.birthTime = btrfs_timespec(inode->birth_time);
	return attributes;
}

- (NSInteger)maximumLinkCount
{
	return UINT32_MAX;
}

- (NSInteger)maximumNameLength
{
	return BTRFS_NAME_MAX;
}

- (BOOL)restrictsOwnershipChanges
{
	return YES;
}

- (BOOL)truncatesLongNames
{
	return NO;
}

- (uint64_t)maximumFileSize
{
	return INT64_MAX;
}

/* A volume without a device barrier stays read-only: FSKit's resource has no
 * cache flush to order the writer's commits. */
- (FSMountOptions)requestedMountOptions
{
	return _writable ? 0 : FSMountOptionsReadOnly;
}

- (FSVolumeSupportedCapabilities *)supportedVolumeCapabilities
{
	FSVolumeSupportedCapabilities *capabilities = [[FSVolumeSupportedCapabilities alloc] init];

	capabilities.supportsPersistentObjectIDs = NO;
	capabilities.supportsSymbolicLinks = YES;
	capabilities.supportsHardLinks = YES;
	capabilities.supportsSparseFiles = YES;
	capabilities.supportsFastStatFS = YES;
	capabilities.doesNotSupportSettingFilePermissions = !_writable;
	capabilities.doesNotSupportImmutableFiles = YES;
	capabilities.caseFormat = FSVolumeCaseFormatSensitive;
	return capabilities;
}

- (FSStatFSResult *)volumeStatistics
{
	FSStatFSResult *statistics =
	    [[FSStatFSResult alloc] initWithFileSystemTypeName:BTRFS_FSKIT_TYPE_NAME];
	struct btrfs_volume_view *view;
	struct btrfs_info info;
	const struct btrfs_fs *fs;
	uint64_t free_bytes;

	fs = btrfs_volume_read(_volume, &view);
	btrfs_get_info(fs, &info);
	btrfs_volume_unread(_volume, view);
	free_bytes = info.used_bytes < info.total_bytes ? info.total_bytes - info.used_bytes : 0;
	statistics.blockSize = info.sector_size;
	/* Matches the personality's FSSubType in the extension's Info.plist. */
	statistics.fileSystemSubType = 0;
	statistics.ioSize = MAX(info.sector_size, BTRFS_FSKIT_IO_SIZE);
	statistics.totalBlocks = info.total_bytes / info.sector_size;
	statistics.freeBlocks = free_bytes / info.sector_size;
	statistics.availableBlocks = _writable ? free_bytes / info.sector_size : 0;
	statistics.usedBlocks = info.used_bytes / info.sector_size;
	statistics.totalFiles = 0;
	statistics.freeFiles = 0;
	return statistics;
}

- (void)mountWithOptions:(FSTaskOptions *)options replyHandler:(void (^)(NSError *))reply
{
	(void)options;
	reply(nil);
}

/* Unmount makes every change durable before it returns. */
- (void)unmountWithReplyHandler:(void (^)(void))reply
{
	(void)[self shutdown];
	reply();
}

- (void)synchronizeWithFlags:(FSSyncFlags)flags replyHandler:(void (^)(NSError *))reply
{
	enum btrfs_result error = BTRFS_OK;

	(void)flags;
	if (_writable) {
		error = btrfs_volume_sync(_volume, btrfs_volume_pending(_volume));
	}
	reply(btrfs_fskit_error(error));
}

- (void)activateWithOptions:(FSTaskOptions *)options
	       replyHandler:(void (^)(FSItem *, NSError *))reply
{
	struct btrfs_volume_view *view;
	struct btrfs_inode root;
	const struct btrfs_fs *fs;
	BtrfsItem *item = nil;
	enum btrfs_result error;

	(void)options;
	fs = btrfs_volume_read(_volume, &view);
	error = btrfs_root(fs, &root);
	if (error == BTRFS_OK) {
		error = btrfs_native_inode_supported(fs, &root);
	}
	btrfs_volume_unread(_volume, view);
	if (error == BTRFS_OK) {
		item = [self itemForInode:&root];
		if (item == nil) {
			error = BTRFS_NO_MEMORY;
		}
	}
	reply(item, btrfs_fskit_error(error));
}

- (void)deactivateWithOptions:(FSDeactivateOptions)options replyHandler:(void (^)(NSError *))reply
{
	(void)options;
	reply(btrfs_fskit_error([self shutdown]));
}

- (BOOL)isOpenCloseInhibited
{
	return NO;
}

/* Write access to a read-only volume is refused at open, before the kernel
 * admits cached writes or shared writable mappings. */
- (void)openItem:(FSItem *)item
       withModes:(FSVolumeOpenModes)modes
    replyHandler:(void (^)(NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;

	if (!_writable && (modes & FSVolumeOpenModesWrite) != 0) {
		reply(btrfs_fskit_error(BTRFS_READ_ONLY));
		return;
	}
	[_itemLock lock];
	owned->openModes |= modes;
	[_itemLock unlock];
	reply(nil);
}

- (void)closeItem:(FSItem *)item
     keepingModes:(FSVolumeOpenModes)modes
     replyHandler:(void (^)(NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;

	[_itemLock lock];
	owned->openModes = modes;
	[_itemLock unlock];
	reply(nil);
}

/* An inode whose last name went while open is deleted with its last item, as
 * Linux's eviction does; a failure leaves it for orphan cleanup. */
- (void)reclaimItem:(FSItem *)item replyHandler:(void (^)(NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;
	struct btrfs_object_id identity;
	BOOL orphan;
	enum btrfs_result error = BTRFS_OK;

	[_itemLock lock];
	orphan = owned->orphan;
	owned->orphan = NO;
	identity = owned->inode.id;
	[_itemLock unlock];
	if (orphan) {
		error = [self
		    changeWithNodes:BTRFS_FSKIT_OPERATION_NODES
			      first:nil
			     second:nil
			  operation:^enum btrfs_result(struct btrfs_transaction *transaction) {
			    return btrfs_transaction_evict(transaction, identity);
			  }];
	}
	reply(btrfs_fskit_error(error));
}

- (NSError *)checkMountEligibility
{
	struct btrfs_volume_view *view;
	struct btrfs_inode root;
	const struct btrfs_fs *fs;
	enum btrfs_result error;

	fs = btrfs_volume_read(_volume, &view);
	error = btrfs_root(fs, &root);
	btrfs_volume_unread(_volume, view);
	return btrfs_fskit_error(error);
}

- (void)lookupItemNamed:(FSFileName *)name
	    inDirectory:(FSItem *)directory
	   replyHandler:(void (^)(FSItem *, FSFileName *, NSError *))reply
{
	BtrfsItem *parent = (BtrfsItem *)directory;
	struct btrfs_volume_view *view;
	struct btrfs_inode inode;
	struct btrfs_inode within;
	const struct btrfs_fs *fs;
	BtrfsItem *item = nil;
	NSData *bytes = name.data;
	enum btrfs_result error;

	[_itemLock lock];
	within = parent->inode;
	[_itemLock unlock];
	fs = btrfs_volume_read(_volume, &view);
	error = btrfs_get_inode(fs, within.id, &within);
	if (error == BTRFS_OK) {
		error = btrfs_lookup(fs, &within, bytes.bytes, bytes.length, &inode);
	}
	if (error == BTRFS_OK) {
		error = btrfs_native_inode_supported(fs, &inode);
	}
	btrfs_volume_unread(_volume, view);
	if (error == BTRFS_OK) {
		item = [self itemForInode:&inode];
		if (item == nil) {
			error = BTRFS_NO_MEMORY;
		} else {
			[self setHolder:parent ofItem:item];
		}
	}
	reply(item, error == BTRFS_OK ? name : nil, btrfs_fskit_error(error));
}

- (void)getAttributes:(FSItemGetAttributesRequest *)desiredAttributes
	       ofItem:(FSItem *)item
	 replyHandler:(void (^)(FSItemAttributes *, NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;
	FSItemAttributes *attributes = nil;
	BOOL orphan;
	enum btrfs_result error = BTRFS_OK;

	/* FSKit faults on a reply missing any attribute it wants: every standard
	 * attribute is always supplied. After a rename that replaced nothing it
	 * asks for the absent target's attributes with a nil item. */
	(void)desiredAttributes;
	if (![item isKindOfClass:BtrfsItem.class]) {
		reply(nil, btrfs_fskit_error(BTRFS_STALE));
		return;
	}
	[_itemLock lock];
	orphan = owned->orphan;
	[_itemLock unlock];
	/* An orphan has no names left to look it up by; its record is kept. */
	if (!orphan) {
		error = [self refreshItem:owned];
	}
	if (error == BTRFS_OK) {
		attributes = [self attributesForItem:owned result:&error];
	}
	reply(attributes, btrfs_fskit_error(error));
}

/* Packs "." and ".." from cookie on; *packed is the cookie after the last
 * packed one. The mount root is its own parent. */
- (enum btrfs_result)packDots:(const struct btrfs_inode *)directory
			   fs:(const struct btrfs_fs *)fs
			 from:(uint64_t)cookie
		       packer:(FSDirectoryEntryPacker *)packer
		       packed:(uint64_t *)packed
{
	struct btrfs_inode parent;
	NSNumber *number;
	enum btrfs_result error;

	*packed = cookie;
	for (; *packed < BTRFS_FSKIT_FIRST_INDEX; (*packed)++) {
		parent = *directory;
		if (*packed == 1) {
			error = btrfs_parent(fs, directory, &parent);
			if (error != BTRFS_OK) {
				return error;
			}
		}
		number = [self numberForIdentity:parent.id];
		if (number == nil) {
			return BTRFS_NO_MEMORY;
		}
		if (![packer packEntryWithName:[FSFileName nameWithBytes:".." length:*packed + 1]
				      itemType:FSItemTypeDirectory
					itemID:number.unsignedLongLongValue
				    nextCookie:*packed + 1
				    attributes:nil]) {
			break;
		}
	}
	return BTRFS_OK;
}

- (void)enumerateDirectory:(FSItem *)directory
	  startingAtCookie:(FSDirectoryCookie)cookie
		  verifier:(FSDirectoryVerifier)verifier
       providingAttributes:(FSItemGetAttributesRequest *)attributes
	       usingPacker:(FSDirectoryEntryPacker *)packer
	      replyHandler:(void (^)(FSDirectoryVerifier, NSError *))reply
{
	BtrfsItem *parent = (BtrfsItem *)directory;
	struct btrfs_volume_view *view;
	struct btrfs_dir_entry entry;
	struct btrfs_directory *stream = NULL;
	struct btrfs_inode inode;
	struct btrfs_inode within;
	const struct btrfs_fs *fs;
	FSItemAttributes *itemAttributes;
	FSFileName *name;
	NSNumber *number;
	uint64_t next = cookie;
	uint64_t holder;
	FSDirectoryVerifier current;
	enum btrfs_result error;

	[_itemLock lock];
	current = parent->version;
	within = parent->inode;
	[_itemLock unlock];
	/* Every entry's parent is this directory, so attributes never resolve one
	 * while this enumeration holds its view. */
	number = [self numberForIdentity:within.id];
	if (number == nil) {
		reply(current, btrfs_fskit_error(BTRFS_NO_MEMORY));
		return;
	}
	holder = number.unsignedLongLongValue;
	if (verifier != FSDirectoryVerifierInitial && verifier != current) {
		reply(
		    current, [NSError errorWithDomain:NSPOSIXErrorDomain code:ESTALE userInfo:nil]);
		return;
	}
	fs = btrfs_volume_read(_volume, &view);
	error = btrfs_get_inode(fs, within.id, &within);
	/* Without attributes FSKit expects "." and ".." first: cookies 0 and 1,
	 * below the first DIR_INDEX key. */
	if (error == BTRFS_OK && attributes == nil && next < BTRFS_FSKIT_FIRST_INDEX) {
		error = [self packDots:&within fs:fs from:next packer:packer packed:&next];
		if (error == BTRFS_OK && next < BTRFS_FSKIT_FIRST_INDEX) {
			btrfs_volume_unread(_volume, view);
			reply(current, nil);
			return;
		}
	}
	if (error == BTRFS_OK) {
		error = btrfs_directory_open(fs, &within, next, &stream);
	}
	while (error == BTRFS_OK &&
	    (error = btrfs_directory_next(stream, &entry, &next)) == BTRFS_OK) {
		if (attributes != nil) {
			error = btrfs_directory_inode(stream, &entry, &inode);
			/* An entry naming a missing inode is damage: ending the
			 * listing there would hide the remaining entries. */
			if (error == BTRFS_NOT_FOUND) {
				error = BTRFS_CORRUPT;
			}
			if (error != BTRFS_OK) {
				break;
			}
		}
		name = [FSFileName nameWithBytes:(const char *)entry.name length:entry.name_length];
		number = [self numberForIdentity:entry.id];
		if (number == nil) {
			error = BTRFS_NO_MEMORY;
			break;
		}
		itemAttributes = attributes == nil
		    ? nil
		    : [self attributesForInode:&inode parent:holder result:&error];
		if (attributes != nil && itemAttributes == nil) {
			break;
		}
		if (![packer packEntryWithName:name
				      itemType:btrfs_item_type(btrfs_mode_for_type(entry.type))
					itemID:number.unsignedLongLongValue
				    nextCookie:next
				    attributes:itemAttributes]) {
			break;
		}
	}
	btrfs_directory_close(stream);
	btrfs_volume_unread(_volume, view);
	reply(current, error == BTRFS_NOT_FOUND ? nil : btrfs_fskit_error(error));
}

- (void)readFromFile:(FSItem *)item
	      offset:(off_t)offset
	      length:(size_t)length
	  intoBuffer:(FSMutableFileDataBuffer *)buffer
	replyHandler:(void (^)(size_t, NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;
	struct btrfs_volume_view *view;
	struct btrfs_inode inode;
	const struct btrfs_fs *fs;
	size_t completed = 0;
	BOOL orphan;
	enum btrfs_result error = BTRFS_OK;

	if (offset < 0 || length > buffer.length) {
		reply(0, btrfs_fskit_error(BTRFS_INVALID_ARGUMENT));
		return;
	}
	[_itemLock lock];
	inode = owned->inode;
	orphan = owned->orphan;
	[_itemLock unlock];
	if ((inode.mode & BTRFS_MODE_TYPE) != BTRFS_MODE_REGULAR) {
		reply(0, btrfs_fskit_error(BTRFS_INVALID_ARGUMENT));
		return;
	}
	fs = btrfs_volume_read(_volume, &view);
	/* The newest size and data; an orphan keeps its inode until evicted. */
	if (!orphan) {
		error = btrfs_get_inode(fs, inode.id, &inode);
	}
	if (error == BTRFS_OK) {
		error = btrfs_read(
		    fs, &inode, (uint64_t)offset, buffer.mutableBytes, length, &completed);
	}
	btrfs_volume_unread(_volume, view);
	reply(completed, btrfs_fskit_error(error));
}

- (void)readSymbolicLink:(FSItem *)item replyHandler:(void (^)(FSFileName *, NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;
	struct btrfs_volume_view *view;
	struct btrfs_inode inode;
	const struct btrfs_fs *fs;
	NSMutableData *bytes = nil;
	size_t completed = 0;
	enum btrfs_result error;

	[_itemLock lock];
	inode = owned->inode;
	[_itemLock unlock];
	fs = btrfs_volume_read(_volume, &view);
	error = btrfs_get_inode(fs, inode.id, &inode);
	if (error == BTRFS_OK && (inode.mode & BTRFS_MODE_TYPE) != BTRFS_MODE_SYMLINK) {
		error = BTRFS_INVALID_ARGUMENT;
	}
	if (error == BTRFS_OK && inode.size > PATH_MAX) {
		error = BTRFS_UNSUPPORTED;
	}
	if (error == BTRFS_OK) {
		bytes = [NSMutableData dataWithLength:(NSUInteger)inode.size];
		error = btrfs_read(fs, &inode, 0, bytes.mutableBytes, bytes.length, &completed);
	}
	btrfs_volume_unread(_volume, view);
	if (error == BTRFS_OK && completed != bytes.length) {
		error = BTRFS_IO;
	}
	reply(error == BTRFS_OK ? [FSFileName nameWithData:bytes] : nil, btrfs_fskit_error(error));
}

- (void)getXattrNamed:(FSFileName *)name
	       ofItem:(FSItem *)item
	 replyHandler:(void (^)(NSData *, NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;
	struct btrfs_volume_view *view;
	struct btrfs_inode inode;
	const struct btrfs_fs *fs;
	NSData *key = name.data;
	NSMutableData *value = nil;
	size_t length = 0;
	enum btrfs_result result;

	if (!btrfs_native_xattr_visible(key.bytes, key.length)) {
		reply(nil, [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOATTR userInfo:nil]);
		return;
	}
	[_itemLock lock];
	inode = owned->inode;
	[_itemLock unlock];
	fs = btrfs_volume_read(_volume, &view);
	result = btrfs_get_xattr(fs, &inode, key.bytes, key.length, NULL, 0, &length);
	if (result == BTRFS_OK && length > BTRFS_NATIVE_XATTR_LIMIT) {
		result = BTRFS_RANGE;
	}
	if (result == BTRFS_OK) {
		value = [NSMutableData dataWithLength:length];
		result = btrfs_get_xattr(
		    fs, &inode, key.bytes, key.length, value.mutableBytes, value.length, &length);
	}
	btrfs_volume_unread(_volume, view);
	if (result == BTRFS_NOT_FOUND) {
		reply(nil, [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOATTR userInfo:nil]);
		return;
	}
	if (result == BTRFS_RANGE) {
		reply(nil, [NSError errorWithDomain:NSPOSIXErrorDomain code:E2BIG userInfo:nil]);
		return;
	}
	reply(result == BTRFS_OK ? value : nil, btrfs_fskit_error(result));
}

- (void)listXattrsOfItem:(FSItem *)item
	    replyHandler:(void (^)(NSArray<FSFileName *> *, NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;
	struct btrfs_volume_view *view;
	struct btrfs_inode inode;
	const struct btrfs_fs *fs;
	NSMutableData *buffer = nil;
	NSMutableArray<FSFileName *> *names;
	const char *bytes;
	size_t length = 0;
	size_t filtered = 0;
	size_t offset;
	size_t name_length;
	enum btrfs_result result;

	[_itemLock lock];
	inode = owned->inode;
	[_itemLock unlock];
	fs = btrfs_volume_read(_volume, &view);
	result = btrfs_list_xattrs(fs, &inode, NULL, 0, &length);
	if (result == BTRFS_OK && length > BTRFS_NATIVE_XATTR_LIMIT) {
		result = BTRFS_RANGE;
	}
	if (result == BTRFS_OK) {
		buffer = [NSMutableData dataWithLength:length];
		result = btrfs_list_xattrs(fs, &inode, buffer.mutableBytes, length, &length);
	}
	btrfs_volume_unread(_volume, view);
	if (result == BTRFS_OK) {
		result = btrfs_native_filter_xattrs(buffer.mutableBytes, length, &filtered);
	}
	if (result == BTRFS_RANGE) {
		reply(nil, [NSError errorWithDomain:NSPOSIXErrorDomain code:E2BIG userInfo:nil]);
		return;
	}
	if (result != BTRFS_OK) {
		reply(nil, btrfs_fskit_error(result));
		return;
	}
	names = [NSMutableArray array];
	bytes = buffer.bytes;
	for (offset = 0; offset < filtered; offset += name_length + 1) {
		name_length = strlen(bytes + offset);
		[names addObject:[FSFileName nameWithBytes:bytes + offset length:name_length]];
	}
	reply(names, nil);
}

- (void)createItemNamed:(FSFileName *)name
		   type:(FSItemType)type
	    inDirectory:(FSItem *)directory
	     attributes:(FSItemSetAttributesRequest *)newAttributes
	   replyHandler:(void (^)(FSItem *, FSFileName *, NSError *))reply
{
	[self performCreateItemNamed:name
				type:type
			 inDirectory:directory
			  attributes:newAttributes
			replyHandler:reply];
}

- (void)createSymbolicLinkNamed:(FSFileName *)name
		    inDirectory:(FSItem *)directory
		     attributes:(FSItemSetAttributesRequest *)newAttributes
		   linkContents:(FSFileName *)contents
		   replyHandler:(void (^)(FSItem *, FSFileName *, NSError *))reply
{
	[self performCreateSymbolicLinkNamed:name
				 inDirectory:directory
				  attributes:newAttributes
				linkContents:contents
				replyHandler:reply];
}

- (void)createLinkToItem:(FSItem *)item
		   named:(FSFileName *)name
	     inDirectory:(FSItem *)directory
	    replyHandler:(void (^)(FSFileName *, NSError *))reply
{
	[self performCreateLinkToItem:item named:name inDirectory:directory replyHandler:reply];
}

- (void)removeItem:(FSItem *)item
	     named:(FSFileName *)name
     fromDirectory:(FSItem *)directory
      replyHandler:(void (^)(NSError *))reply
{
	[self performRemoveItem:item named:name fromDirectory:directory replyHandler:reply];
}

- (void)renameItem:(FSItem *)item
       inDirectory:(FSItem *)sourceDirectory
	     named:(FSFileName *)sourceName
	 toNewName:(FSFileName *)destinationName
       inDirectory:(FSItem *)destinationDirectory
	  overItem:(FSItem *)overItem
      replyHandler:(void (^)(FSFileName *, NSError *))reply
{
	[self performRenameItem:item
		    inDirectory:sourceDirectory
			  named:sourceName
		      toNewName:destinationName
		    inDirectory:destinationDirectory
		       overItem:overItem
		   replyHandler:reply];
}

- (void)setAttributes:(FSItemSetAttributesRequest *)newAttributes
	       onItem:(FSItem *)item
	 replyHandler:(void (^)(FSItemAttributes *, NSError *))reply
{
	[self performSetAttributes:newAttributes onItem:item replyHandler:reply];
}

- (void)writeContents:(NSData *)contents
	       toFile:(FSItem *)item
	     atOffset:(off_t)offset
	 replyHandler:(void (^)(size_t, NSError *))reply
{
	[self performWriteContents:contents toFile:item atOffset:offset replyHandler:reply];
}

- (void)setXattrNamed:(FSFileName *)name
	       toData:(NSData *)value
	       onItem:(FSItem *)item
	       policy:(FSSetXattrPolicy)policy
	 replyHandler:(void (^)(NSError *))reply
{
	[self performSetXattrNamed:name toData:value onItem:item policy:policy replyHandler:reply];
}

@end

/* Completes a refused maintenance task through the task, asynchronously: a
 * synchronous refusal crashes the system's check and format clients. */
static NSProgress *
btrfs_fskit_refusal(FSTask *task, NSError *error)
{
	NSProgress *progress = [NSProgress progressWithTotalUnitCount:1];

	progress.cancellable = NO;
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
	  progress.completedUnitCount = 1;
	  [task didCompleteWithError:error];
	});
	return progress;
}

@implementation BtrfsFileSystem {
	BtrfsVolume *_volume;
}

- (NSProgress *)startCheckWithTask:(FSTask *)task
			   options:(FSTaskOptions *)options
			     error:(NSError **)error
{
	BtrfsVolume *volume;
	NSProgress *progress;

	(void)error;
	/* A quick check admits only clean media; it is not an fsck repair. */
	if (![options.taskOptions containsObject:@"-q"]) {
		return btrfs_fskit_refusal(task,
		    [NSError errorWithDomain:NSPOSIXErrorDomain
					code:ENOTSUP
				    userInfo:@{
					    NSLocalizedDescriptionKey :
						@"Only a quick read-only check is supported."
				    }]);
	}
	@synchronized(self) {
		volume = _volume;
	}
	if (volume == nil) {
		return btrfs_fskit_refusal(
		    task, [NSError errorWithDomain:NSPOSIXErrorDomain code:ENXIO userInfo:nil]);
	}
	progress = [NSProgress progressWithTotalUnitCount:1];
	progress.cancellable = NO;
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
	  NSError *checkError;

	  checkError = [volume checkMountEligibility];
	  progress.completedUnitCount = 1;
	  [task didCompleteWithError:checkError];
	});
	return progress;
}

- (NSProgress *)startFormatWithTask:(FSTask *)task
			    options:(FSTaskOptions *)options
			      error:(NSError **)error
{
	(void)options;
	(void)error;
	return btrfs_fskit_refusal(task, btrfs_fskit_error(BTRFS_READ_ONLY));
}

- (void)probeResource:(FSResource *)resource
	 replyHandler:(void (^)(FSProbeResult *, NSError *))reply
{
	struct btrfs_environment environment;
	struct btrfs_fs *fs = NULL;
	struct btrfs_info info;
	FSBlockDeviceResource *block;
	NSUUID *uuid;
	NSString *name;
	FSContainerIdentifier *identifier;
	enum btrfs_result error = BTRFS_UNSUPPORTED;

	if ([resource isKindOfClass:FSBlockDeviceResource.class]) {
		block = (FSBlockDeviceResource *)resource;
		error = block.blockSize == 0 || block.blockCount > UINT64_MAX / block.blockSize
		    ? BTRFS_CORRUPT
		    : BTRFS_OK;
		if (error == BTRFS_OK) {
			memset(&environment, 0, sizeof(environment));
			environment.context = (__bridge void *)block;
			environment.size_bytes = block.blockCount * block.blockSize;
			environment.read = btrfs_resource_read;
			environment.allocate = btrfs_resource_allocate;
			environment.release = btrfs_resource_release;
			environment.decompress = btrfs_resource_decompress;
			error = btrfs_mount(&environment, 0, &fs);
		}
	}
	if (error != BTRFS_OK) {
		reply(error == BTRFS_NOT_BTRFS ? FSProbeResult.notRecognizedProbeResult : nil,
		    btrfs_fskit_error(error == BTRFS_NOT_BTRFS ? BTRFS_OK : error));
		return;
	}
	btrfs_get_info(fs, &info);
	name = [[NSString alloc] initWithBytes:info.label
					length:strnlen(info.label, BTRFS_LABEL_SIZE)
				      encoding:NSUTF8StringEncoding];
	if (name.length == 0) {
		name = @"btrfs";
	}
	uuid = [[NSUUID alloc] initWithUUIDBytes:info.uuid];
	identifier = [[FSContainerIdentifier alloc] initWithUUID:uuid];
	btrfs_unmount(fs);
	/* Disk Arbitration rejects a usable-but-limited result; whether the
	 * volume accepts changes is decided at load. */
	reply([FSProbeResult usableProbeResultWithName:name containerID:identifier], nil);
}

- (id<BtrfsDeviceFlusher>)barrierForDevice:(FSBlockDeviceResource *)device error:(NSError **)error
{
	return (id<BtrfsDeviceFlusher>)[[BtrfsDeviceBarrier alloc] initWithDevice:device.BSDName
									blockSize:device.blockSize
								       blockCount:device.blockCount
									    error:error];
}

- (id<BtrfsDeviceFlusher>)flusherForDevice:(FSBlockDeviceResource *)device
				   options:(FSTaskOptions *)options
{
	id<BtrfsDeviceFlusher> barrier;
	NSError *error = nil;

	if ([options.taskOptions containsObject:@"--rdonly"] || !device.isWritable) {
		return nil;
	}
	barrier = [self barrierForDevice:device error:&error];
	if (barrier == nil) {
		NSLog(@"machlinbtrfs: %@ stays read-only without the device barrier: %@",
		    device.BSDName, error.localizedDescription);
	}
	return barrier;
}

- (void)loadResource:(FSResource *)resource
	     options:(FSTaskOptions *)options
	replyHandler:(void (^)(FSVolume *, NSError *))reply
{
	struct btrfs_fskit_cache *cache = NULL;
	FSBlockDeviceResource *device;
	BtrfsVolume *volume = nil;
	NSError *loadError = nil;
	enum btrfs_result error = BTRFS_UNSUPPORTED;

	@synchronized(self) {
		if (_volume != nil) {
			loadError = [NSError errorWithDomain:NSPOSIXErrorDomain
							code:EBUSY
						    userInfo:nil];
		} else {
			if ([resource isKindOfClass:FSBlockDeviceResource.class]) {
				device = (FSBlockDeviceResource *)resource;
				cache = btrfs_fskit_cache_create();
				volume = [BtrfsVolume
				    volumeWithReader:device
					     flusher:[self flusherForDevice:device options:options]
					       cache:cache
					      result:&error];
				if (volume == nil) {
					btrfs_fskit_cache_destroy(cache);
				}
			}
			loadError = btrfs_fskit_error(error);
			_volume = volume;
			self.containerStatus = loadError == nil
			    ? FSContainerStatus.ready
			    : [FSContainerStatus blockedWithStatus:loadError];
		}
	}
	reply(volume, loadError);
}

- (void)unloadResource:(FSResource *)resource
	       options:(FSTaskOptions *)options
	  replyHandler:(void (^)(NSError *))reply
{
	BtrfsVolume *volume;

	(void)resource;
	(void)options;
	@synchronized(self) {
		volume = _volume;
		_volume = nil;
		self.containerStatus = [FSContainerStatus
		    notReadyWithStatus:[NSError errorWithDomain:NSPOSIXErrorDomain
							   code:ENXIO
						       userInfo:nil]];
	}
	reply(btrfs_fskit_error(volume == nil ? BTRFS_OK : [volume shutdown]));
}

@end
