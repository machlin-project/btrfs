/* SPDX-License-Identifier: BSD-3-Clause */
#import "BtrfsFileSystemInternal.h"
#include <btrfs/btrfs.h>
#include <btrfs/identity.h>
#include <btrfs/native.h>
#include <zlib.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>

@implementation FSBlockDeviceResource (BtrfsBlockReader)
@end

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

/* Verified tree nodes shared by a loaded volume's threads; the resource is
 * read-only, so nothing else changes it while the volume is loaded. */
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

enum btrfs_result
btrfs_fskit_open(id<BtrfsBlockReader> reader, struct btrfs_fskit_cache *cache, struct btrfs_fs **fs)
{
	struct btrfs_environment environment;

	*fs = NULL;
	if (reader.blockSize == 0 || reader.blockCount > UINT64_MAX / reader.blockSize) {
		return BTRFS_CORRUPT;
	}
	memset(&environment, 0, sizeof(environment));
	environment.context = (__bridge void *)reader;
	environment.size_bytes = reader.blockCount * reader.blockSize;
	environment.read = btrfs_resource_read;
	environment.allocate = btrfs_resource_allocate;
	environment.release = btrfs_resource_release;
	environment.decompress = btrfs_resource_decompress;
	environment.cache = cache == NULL ? NULL : cache->cache;
	return btrfs_mount(&environment, 0, fs);
}

static enum btrfs_result
btrfs_open_resource(FSResource *resource, struct btrfs_fskit_cache *cache, struct btrfs_fs **fs)
{
	*fs = NULL;
	if (![resource isKindOfClass:FSBlockDeviceResource.class]) {
		return BTRFS_UNSUPPORTED;
	}
	return btrfs_fskit_open((FSBlockDeviceResource *)resource, cache, fs);
}

static FSItemType
btrfs_item_type(uint16_t mode)
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

@implementation BtrfsItem
@end

@implementation BtrfsVolume {
	struct btrfs_fs *_fs;
	struct btrfs_fskit_cache *_cache;
	struct btrfs_info _info;
	id<BtrfsBlockReader> _reader;
	NSLock *_itemLock;
	NSMapTable<NSNumber *, BtrfsItem *> *_items;
	struct btrfs_identity_table *_identities;
}

- (instancetype)initWithReader:(id<BtrfsBlockReader>)reader
		    filesystem:(struct btrfs_fs *)fs
			 cache:(struct btrfs_fskit_cache *)cache
{
	struct btrfs_info info;
	struct btrfs_environment environment = { 0 };
	struct btrfs_object_id root;
	NSUUID *uuid;
	FSVolumeIdentifier *identifier;
	FSFileName *name;

	btrfs_get_info(fs, &info);
	uuid = [[NSUUID alloc] initWithUUIDBytes:info.uuid];
	identifier = [[FSVolumeIdentifier alloc] initWithUUID:uuid];
	name = [FSFileName nameWithBytes:info.label length:strnlen(info.label, BTRFS_LABEL_SIZE)];
	self = [super initWithVolumeID:identifier volumeName:name];
	if (self != nil) {
		environment.allocate = btrfs_resource_allocate;
		environment.release = btrfs_resource_release;
		root.tree = info.default_tree;
		root.inode = BTRFS_ROOT_INODE;
		if (btrfs_identity_create(&environment, root, &_identities) != BTRFS_OK) {
			return nil;
		}
		_fs = fs;
		_cache = cache;
		_info = info;
		_reader = reader;
		_itemLock = [[NSLock alloc] init];
		_items = [NSMapTable strongToWeakObjectsMapTable];
	}
	return self;
}

- (void)dealloc
{
	btrfs_identity_destroy(_identities);
	btrfs_unmount(_fs);
	/* After the mount: no view or stream uses the cache any more. */
	btrfs_fskit_cache_destroy(_cache);
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
		item->inode = *inode;
		[_items setObject:item forKey:key];
	}
	[_itemLock unlock];
	return item;
}

- (FSItemAttributes *)attributesForInode:(const struct btrfs_inode *)inode
{
	FSItemAttributes *attributes = [[FSItemAttributes alloc] init];
	NSNumber *number = [self numberForIdentity:inode->id];
	struct timespec time;

	if (number == nil) {
		return nil;
	}
	attributes.uid = inode->uid;
	attributes.gid = inode->gid;
	attributes.mode = inode->mode & ALLPERMS;
	attributes.type = btrfs_item_type(inode->mode);
	attributes.linkCount = inode->links;
	attributes.size = inode->size;
	attributes.allocSize = inode->allocated_bytes;
	attributes.fileID = number.unsignedLongLongValue;
	attributes.inhibitKernelOffloadedIO = YES;
	time.tv_nsec = inode->modify_time.nanoseconds;
	time.tv_sec = inode->modify_time.seconds;
	attributes.modifyTime = time;
	time.tv_nsec = inode->change_time.nanoseconds;
	time.tv_sec = inode->change_time.seconds;
	attributes.changeTime = time;
	time.tv_nsec = inode->access_time.nanoseconds;
	time.tv_sec = inode->access_time.seconds;
	attributes.accessTime = time;
	time.tv_nsec = inode->birth_time.nanoseconds;
	time.tv_sec = inode->birth_time.seconds;
	attributes.birthTime = time;
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

/* Writes stay disabled: the block-device resource has no device cache flush,
 * so the writer's flush contract (every acknowledged write durable and
 * ordered before the next barrier) cannot be met through FSKit. */
- (FSMountOptions)requestedMountOptions
{
	return FSMountOptionsReadOnly;
}

- (FSVolumeSupportedCapabilities *)supportedVolumeCapabilities
{
	FSVolumeSupportedCapabilities *capabilities = [[FSVolumeSupportedCapabilities alloc] init];

	capabilities.supportsPersistentObjectIDs = NO;
	capabilities.supportsSymbolicLinks = YES;
	capabilities.supportsHardLinks = YES;
	capabilities.supportsSparseFiles = YES;
	capabilities.supportsFastStatFS = YES;
	capabilities.doesNotSupportSettingFilePermissions = YES;
	capabilities.doesNotSupportImmutableFiles = YES;
	capabilities.caseFormat = FSVolumeCaseFormatSensitive;
	return capabilities;
}

- (FSStatFSResult *)volumeStatistics
{
	FSStatFSResult *statistics =
	    [[FSStatFSResult alloc] initWithFileSystemTypeName:BTRFS_FSKIT_TYPE_NAME];

	statistics.blockSize = _info.sector_size;
	/* Matches the personality's FSSubType in the extension's Info.plist. */
	statistics.fileSystemSubType = 0;
	statistics.ioSize = MAX(_info.sector_size, BTRFS_FSKIT_IO_SIZE);
	statistics.totalBlocks = _info.total_bytes / _info.sector_size;
	statistics.freeBlocks = (_info.total_bytes - _info.used_bytes) / _info.sector_size;
	statistics.availableBlocks = 0;
	statistics.usedBlocks = _info.used_bytes / _info.sector_size;
	statistics.totalFiles = 0;
	statistics.freeFiles = 0;
	return statistics;
}

- (void)mountWithOptions:(FSTaskOptions *)options replyHandler:(void (^)(NSError *))reply
{
	(void)options;
	reply(nil);
}

- (void)unmountWithReplyHandler:(void (^)(void))reply
{
	reply();
}

- (void)synchronizeWithFlags:(FSSyncFlags)flags replyHandler:(void (^)(NSError *))reply
{
	(void)flags;
	/* The read-only implementation has no pending writes or dirty metadata. */
	reply(nil);
}

- (void)activateWithOptions:(FSTaskOptions *)options
	       replyHandler:(void (^)(FSItem *, NSError *))reply
{
	struct btrfs_inode root;
	BtrfsItem *item = nil;
	enum btrfs_result error;

	(void)options;
	error = btrfs_root(_fs, &root);
	if (error == BTRFS_OK) {
		error = btrfs_native_inode_supported(_fs, &root);
	}
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
	reply(nil);
}

- (void)reclaimItem:(FSItem *)item replyHandler:(void (^)(NSError *))reply
{
	/* The weak identity table expires after the last concurrent holder exits. */
	(void)item;
	reply(nil);
}

- (BOOL)isOpenCloseInhibited
{
	return NO;
}

/* Write access is refused at open, before the kernel admits cached writes or
 * shared writable mappings, whatever the exported mount flags say. */
- (void)openItem:(FSItem *)item
       withModes:(FSVolumeOpenModes)modes
    replyHandler:(void (^)(NSError *))reply
{
	(void)item;
	reply((modes & FSVolumeOpenModesWrite) != 0 ? btrfs_fskit_error(BTRFS_READ_ONLY) : nil);
}

- (void)closeItem:(FSItem *)item
     keepingModes:(FSVolumeOpenModes)modes
     replyHandler:(void (^)(NSError *))reply
{
	(void)item;
	(void)modes;
	reply(nil);
}

- (NSError *)checkMountEligibility
{
	struct btrfs_inode root;
	enum btrfs_result error;

	error = btrfs_root(_fs, &root);
	return btrfs_fskit_error(error);
}

- (void)lookupItemNamed:(FSFileName *)name
	    inDirectory:(FSItem *)directory
	   replyHandler:(void (^)(FSItem *, FSFileName *, NSError *))reply
{
	BtrfsItem *parent = (BtrfsItem *)directory;
	struct btrfs_inode inode;
	BtrfsItem *item = nil;
	NSData *bytes = name.data;
	enum btrfs_result error;

	error = btrfs_lookup(_fs, &parent->inode, bytes.bytes, bytes.length, &inode);
	if (error == BTRFS_OK) {
		error = btrfs_native_inode_supported(_fs, &inode);
	}
	if (error == BTRFS_OK) {
		item = [self itemForInode:&inode];
		if (item == nil) {
			error = BTRFS_NO_MEMORY;
		}
	}
	reply(item, error == BTRFS_OK ? name : nil, btrfs_fskit_error(error));
}

- (void)getAttributes:(FSItemGetAttributesRequest *)desiredAttributes
	       ofItem:(FSItem *)item
	 replyHandler:(void (^)(FSItemAttributes *, NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;
	FSItemAttributes *attributes;

	(void)desiredAttributes;
	attributes = [self attributesForInode:&owned->inode];
	reply(attributes, attributes == nil ? btrfs_fskit_error(BTRFS_NO_MEMORY) : nil);
}

/* Packs "." and ".." from cookie on; *packed is the cookie after the last
 * packed one. The mount root is its own parent. */
- (enum btrfs_result)packDots:(BtrfsItem *)directory
			 from:(uint64_t)cookie
		       packer:(FSDirectoryEntryPacker *)packer
		       packed:(uint64_t *)packed
{
	struct btrfs_inode parent;
	NSNumber *number;
	enum btrfs_result error;

	*packed = cookie;
	for (; *packed < BTRFS_FSKIT_FIRST_INDEX; (*packed)++) {
		parent = directory->inode;
		if (*packed == 1) {
			error = btrfs_parent(_fs, &directory->inode, &parent);
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
	struct btrfs_dir_entry entry;
	struct btrfs_directory *stream;
	struct btrfs_inode inode;
	FSItemAttributes *itemAttributes;
	FSFileName *name;
	NSNumber *number;
	uint64_t next = cookie;
	FSDirectoryVerifier current = (uint64_t)parent->inode.generation + 1;
	enum btrfs_result error;

	if (verifier != FSDirectoryVerifierInitial && verifier != current) {
		reply(
		    current, [NSError errorWithDomain:NSPOSIXErrorDomain code:ESTALE userInfo:nil]);
		return;
	}
	/* Without attributes FSKit expects "." and ".." first: cookies 0 and 1,
	 * below the first DIR_INDEX key. */
	if (attributes == nil && next < BTRFS_FSKIT_FIRST_INDEX) {
		error = [self packDots:parent from:next packer:packer packed:&next];
		if (error != BTRFS_OK || next < BTRFS_FSKIT_FIRST_INDEX) {
			reply(current, btrfs_fskit_error(error));
			return;
		}
	}
	error = btrfs_directory_open(_fs, &parent->inode, next, &stream);
	if (error != BTRFS_OK) {
		reply(current, btrfs_fskit_error(error));
		return;
	}
	while ((error = btrfs_directory_next(stream, &entry, &next)) == BTRFS_OK) {
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
		itemAttributes = attributes == nil ? nil : [self attributesForInode:&inode];
		if (![packer packEntryWithName:name
				      itemType:btrfs_item_type(btrfs_mode_for_type(entry.type))
					itemID:number.unsignedLongLongValue
				    nextCookie:next
				    attributes:itemAttributes]) {
			break;
		}
	}
	btrfs_directory_close(stream);
	reply(current, error == BTRFS_NOT_FOUND ? nil : btrfs_fskit_error(error));
}

- (void)readFromFile:(FSItem *)item
	      offset:(off_t)offset
	      length:(size_t)length
	  intoBuffer:(FSMutableFileDataBuffer *)buffer
	replyHandler:(void (^)(size_t, NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;
	size_t completed = 0;
	enum btrfs_result error;

	if (offset < 0 || length > buffer.length) {
		reply(0, btrfs_fskit_error(BTRFS_INVALID_ARGUMENT));
		return;
	}
	if ((owned->inode.mode & BTRFS_MODE_TYPE) != BTRFS_MODE_REGULAR) {
		reply(0, btrfs_fskit_error(BTRFS_INVALID_ARGUMENT));
		return;
	}
	error = btrfs_read(
	    _fs, &owned->inode, (uint64_t)offset, buffer.mutableBytes, length, &completed);
	reply(completed, btrfs_fskit_error(error));
}

- (void)readSymbolicLink:(FSItem *)item replyHandler:(void (^)(FSFileName *, NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;
	NSMutableData *bytes;
	size_t completed = 0;
	enum btrfs_result error;

	if ((owned->inode.mode & BTRFS_MODE_TYPE) != BTRFS_MODE_SYMLINK) {
		reply(nil, btrfs_fskit_error(BTRFS_INVALID_ARGUMENT));
		return;
	}
	if (owned->inode.size >= _info.sector_size) {
		reply(nil, btrfs_fskit_error(BTRFS_UNSUPPORTED));
		return;
	}
	bytes = [NSMutableData dataWithLength:(NSUInteger)owned->inode.size];
	error = btrfs_read(_fs, &owned->inode, 0, bytes.mutableBytes, bytes.length, &completed);
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
	NSData *key = name.data;
	NSMutableData *value;
	size_t length = 0;
	enum btrfs_result result;

	if (!btrfs_native_xattr_visible(key.bytes, key.length)) {
		reply(nil, [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOATTR userInfo:nil]);
		return;
	}
	result = btrfs_get_xattr(_fs, &owned->inode, key.bytes, key.length, NULL, 0, &length);
	if (result != BTRFS_OK) {
		reply(nil,
		    result == BTRFS_NOT_FOUND ? [NSError errorWithDomain:NSPOSIXErrorDomain
								    code:ENOATTR
								userInfo:nil]
					      : btrfs_fskit_error(result));
		return;
	}
	if (length > BTRFS_NATIVE_XATTR_LIMIT) {
		reply(nil, [NSError errorWithDomain:NSPOSIXErrorDomain code:E2BIG userInfo:nil]);
		return;
	}
	value = [NSMutableData dataWithLength:length];
	result = btrfs_get_xattr(
	    _fs, &owned->inode, key.bytes, key.length, value.mutableBytes, value.length, &length);
	reply(result == BTRFS_OK ? value : nil, btrfs_fskit_error(result));
}

- (void)listXattrsOfItem:(FSItem *)item
	    replyHandler:(void (^)(NSArray<FSFileName *> *, NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;
	NSMutableData *buffer;
	NSMutableArray<FSFileName *> *names;
	const char *bytes;
	size_t length = 0;
	size_t filtered = 0;
	size_t offset;
	size_t name_length;
	enum btrfs_result result;

	result = btrfs_list_xattrs(_fs, &owned->inode, NULL, 0, &length);
	if (result != BTRFS_OK) {
		reply(nil, btrfs_fskit_error(result));
		return;
	}
	if (length > BTRFS_NATIVE_XATTR_LIMIT) {
		reply(nil, [NSError errorWithDomain:NSPOSIXErrorDomain code:E2BIG userInfo:nil]);
		return;
	}
	buffer = [NSMutableData dataWithLength:length];
	result = btrfs_list_xattrs(_fs, &owned->inode, buffer.mutableBytes, length, &length);
	if (result == BTRFS_OK) {
		result = btrfs_native_filter_xattrs(buffer.mutableBytes, length, &filtered);
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

- (void)setXattrNamed:(FSFileName *)name
	       toData:(NSData *)value
	       onItem:(FSItem *)item
	       policy:(FSSetXattrPolicy)policy
	 replyHandler:(void (^)(NSError *))reply
{
	(void)name;
	(void)value;
	(void)item;
	(void)policy;
	reply(btrfs_fskit_error(BTRFS_READ_ONLY));
}

- (void)createItemNamed:(FSFileName *)name
		   type:(FSItemType)type
	    inDirectory:(FSItem *)directory
	     attributes:(FSItemSetAttributesRequest *)newAttributes
	   replyHandler:(void (^)(FSItem *, FSFileName *, NSError *))reply
{
	(void)name;
	(void)type;
	(void)directory;
	(void)newAttributes;
	reply(nil, nil, btrfs_fskit_error(BTRFS_READ_ONLY));
}

- (void)createSymbolicLinkNamed:(FSFileName *)name
		    inDirectory:(FSItem *)directory
		     attributes:(FSItemSetAttributesRequest *)newAttributes
		   linkContents:(FSFileName *)contents
		   replyHandler:(void (^)(FSItem *, FSFileName *, NSError *))reply
{
	(void)name;
	(void)directory;
	(void)newAttributes;
	(void)contents;
	reply(nil, nil, btrfs_fskit_error(BTRFS_READ_ONLY));
}

- (void)createLinkToItem:(FSItem *)item
		   named:(FSFileName *)name
	     inDirectory:(FSItem *)directory
	    replyHandler:(void (^)(FSFileName *, NSError *))reply
{
	(void)item;
	(void)name;
	(void)directory;
	reply(nil, btrfs_fskit_error(BTRFS_READ_ONLY));
}

- (void)renameItem:(FSItem *)item
       inDirectory:(FSItem *)sourceDirectory
	     named:(FSFileName *)sourceName
	 toNewName:(FSFileName *)destinationName
       inDirectory:(FSItem *)destinationDirectory
	  overItem:(FSItem *)overItem
      replyHandler:(void (^)(FSFileName *, NSError *))reply
{
	(void)item;
	(void)sourceDirectory;
	(void)sourceName;
	(void)destinationName;
	(void)destinationDirectory;
	(void)overItem;
	reply(nil, btrfs_fskit_error(BTRFS_READ_ONLY));
}

- (void)removeItem:(FSItem *)item
	     named:(FSFileName *)name
     fromDirectory:(FSItem *)directory
      replyHandler:(void (^)(NSError *))reply
{
	(void)item;
	(void)name;
	(void)directory;
	reply(btrfs_fskit_error(BTRFS_READ_ONLY));
}

- (void)setAttributes:(FSItemSetAttributesRequest *)newAttributes
	       onItem:(FSItem *)item
	 replyHandler:(void (^)(FSItemAttributes *, NSError *))reply
{
	(void)newAttributes;
	(void)item;
	reply(nil, btrfs_fskit_error(BTRFS_READ_ONLY));
}

- (void)writeContents:(NSData *)contents
	       toFile:(FSItem *)item
	     atOffset:(off_t)offset
	 replyHandler:(void (^)(size_t, NSError *))reply
{
	(void)contents;
	(void)item;
	(void)offset;
	reply(0, btrfs_fskit_error(BTRFS_READ_ONLY));
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

	/* A quick check admits only clean read-only media; it is not an fsck repair. */
	(void)error;
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
	struct btrfs_fs *fs = NULL;
	struct btrfs_info info;
	NSUUID *uuid;
	NSString *name;
	FSContainerIdentifier *identifier;
	enum btrfs_result error;

	error = btrfs_open_resource(resource, NULL, &fs);
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
	/* Disk Arbitration rejects a usable-but-limited result; read-only access
	 * is the mount policy, enforced by the volume. */
	reply([FSProbeResult usableProbeResultWithName:name containerID:identifier], nil);
}

- (void)loadResource:(FSResource *)resource
	     options:(FSTaskOptions *)options
	replyHandler:(void (^)(FSVolume *, NSError *))reply
{
	struct btrfs_fs *fs = NULL;
	struct btrfs_fskit_cache *cache = NULL;
	BtrfsVolume *volume = nil;
	NSError *loadError = nil;
	enum btrfs_result error;

	(void)options;
	@synchronized(self) {
		if (_volume != nil) {
			loadError = [NSError errorWithDomain:NSPOSIXErrorDomain
							code:EBUSY
						    userInfo:nil];
		} else {
			cache = btrfs_fskit_cache_create();
			error = btrfs_open_resource(resource, cache, &fs);
			if (error == BTRFS_OK) {
				volume = [[BtrfsVolume alloc]
				    initWithReader:(FSBlockDeviceResource *)resource
					filesystem:fs
					     cache:cache];
				if (volume == nil) {
					btrfs_unmount(fs);
					error = BTRFS_NO_MEMORY;
				}
			}
			if (volume == nil) {
				btrfs_fskit_cache_destroy(cache);
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
	(void)resource;
	(void)options;
	@synchronized(self) {
		_volume = nil;
		self.containerStatus = [FSContainerStatus
		    notReadyWithStatus:[NSError errorWithDomain:NSPOSIXErrorDomain
							   code:ENXIO
						       userInfo:nil]];
	}
	reply(nil);
}

@end
