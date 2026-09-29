/* SPDX-License-Identifier: BSD-3-Clause */
#import "BtrfsFileSystem.h"
#include <btrfs/btrfs.h>
#include <btrfs/identity.h>
#include <btrfs/native.h>
#include <zlib.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>

static NSError *
btrfs_error(enum btrfs_result result)
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

static enum btrfs_result
btrfs_resource_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	FSBlockDeviceResource *resource = (__bridge FSBlockDeviceResource *)context;
	NSError *error = nil;
	uint8_t *bounce;
	uint64_t alignment = resource.physicalBlockSize;
	uint64_t start;
	uint64_t total;
	size_t completed;

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
	if (start > resource.blockCount * resource.blockSize ||
	    total > resource.blockCount * resource.blockSize - start) {
		return BTRFS_IO;
	}
	bounce = malloc((size_t)total);
	if (bounce == NULL) {
		return BTRFS_NO_MEMORY;
	}
	completed = [resource readInto:bounce
			    startingAt:(off_t)start
				length:(size_t)total
				 error:&error];
	if (error == nil && completed == total) {
		memcpy(buffer, bounce + (offset - start), length);
	}
	free(bounce);
	return error == nil && completed == total ? BTRFS_OK : BTRFS_IO;
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

static enum btrfs_result
btrfs_open_resource(FSResource *resource, struct btrfs_fs **fs)
{
	FSBlockDeviceResource *block;
	struct btrfs_environment environment;

	*fs = NULL;
	if (![resource isKindOfClass:FSBlockDeviceResource.class]) {
		return BTRFS_UNSUPPORTED;
	}
	block = (FSBlockDeviceResource *)resource;
	if (block.blockSize == 0 || block.blockCount > UINT64_MAX / block.blockSize) {
		return BTRFS_CORRUPT;
	}
	memset(&environment, 0, sizeof(environment));
	environment.context = (__bridge void *)block;
	environment.size_bytes = block.blockCount * block.blockSize;
	environment.read = btrfs_resource_read;
	environment.allocate = btrfs_resource_allocate;
	environment.release = btrfs_resource_release;
	environment.decompress = btrfs_resource_decompress;
	return btrfs_mount(&environment, 0, fs);
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

@interface BtrfsItem : FSItem {
      @public
	struct btrfs_inode inode;
}
@end

@implementation BtrfsItem
@end

@interface BtrfsVolume
    : FSVolume <FSVolumeOperations, FSVolumeReadWriteOperations, FSVolumeXattrOperations> {
	struct btrfs_fs *_fs;
	struct btrfs_info _info;
	FSResource *_resource;
	NSLock *_itemLock;
	NSMapTable<NSNumber *, BtrfsItem *> *_items;
	struct btrfs_identity_table *_identities;
}
- (instancetype)initWithResource:(FSResource *)resource filesystem:(struct btrfs_fs *)fs;
- (NSError *)checkMountEligibility;
@end

@implementation BtrfsVolume

- (instancetype)initWithResource:(FSResource *)resource filesystem:(struct btrfs_fs *)fs
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
		_info = info;
		_resource = resource;
		_itemLock = [[NSLock alloc] init];
		_items = [NSMapTable strongToWeakObjectsMapTable];
	}
	return self;
}

- (void)dealloc
{
	btrfs_identity_destroy(_identities);
	btrfs_unmount(_fs);
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
	    [[FSStatFSResult alloc] initWithFileSystemTypeName:@"machlin_btrfs"];

	statistics.blockSize = _info.sector_size;
	statistics.ioSize = _info.sector_size;
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
	reply(item, btrfs_error(error));
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

- (NSError *)checkMountEligibility
{
	struct btrfs_inode root;
	enum btrfs_result error;

	error = btrfs_root(_fs, &root);
	return btrfs_error(error);
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
	reply(item, error == BTRFS_OK ? name : nil, btrfs_error(error));
}

- (void)getAttributes:(FSItemGetAttributesRequest *)desiredAttributes
	       ofItem:(FSItem *)item
	 replyHandler:(void (^)(FSItemAttributes *, NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;
	FSItemAttributes *attributes;

	(void)desiredAttributes;
	attributes = [self attributesForInode:&owned->inode];
	reply(attributes, attributes == nil ? btrfs_error(BTRFS_NO_MEMORY) : nil);
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
	error = btrfs_directory_open(_fs, &parent->inode, next, &stream);
	if (error != BTRFS_OK) {
		reply(current, btrfs_error(error));
		return;
	}
	while ((error = btrfs_directory_next(stream, &entry, &next)) == BTRFS_OK) {
		if (attributes != nil &&
		    ((entry.name_length == 1 && entry.name[0] == '.') ||
			(entry.name_length == 2 && entry.name[0] == '.' && entry.name[1] == '.'))) {
			continue;
		}
		if (attributes != nil) {
			error = btrfs_get_inode(_fs, entry.id, &inode);
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
	reply(current, error == BTRFS_NOT_FOUND ? nil : btrfs_error(error));
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
		reply(0, btrfs_error(BTRFS_INVALID_ARGUMENT));
		return;
	}
	if ((owned->inode.mode & BTRFS_MODE_TYPE) != BTRFS_MODE_REGULAR) {
		reply(0, btrfs_error(BTRFS_INVALID_ARGUMENT));
		return;
	}
	error = btrfs_read(
	    _fs, &owned->inode, (uint64_t)offset, buffer.mutableBytes, length, &completed);
	reply(completed, btrfs_error(error));
}

- (void)readSymbolicLink:(FSItem *)item replyHandler:(void (^)(FSFileName *, NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;
	NSMutableData *bytes;
	size_t completed = 0;
	enum btrfs_result error;

	if ((owned->inode.mode & BTRFS_MODE_TYPE) != BTRFS_MODE_SYMLINK) {
		reply(nil, btrfs_error(BTRFS_INVALID_ARGUMENT));
		return;
	}
	if (owned->inode.size >= _info.sector_size) {
		reply(nil, btrfs_error(BTRFS_UNSUPPORTED));
		return;
	}
	bytes = [NSMutableData dataWithLength:(NSUInteger)owned->inode.size];
	error = btrfs_read(_fs, &owned->inode, 0, bytes.mutableBytes, bytes.length, &completed);
	if (error == BTRFS_OK && completed != bytes.length) {
		error = BTRFS_IO;
	}
	reply(error == BTRFS_OK ? [FSFileName nameWithData:bytes] : nil, btrfs_error(error));
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
					      : btrfs_error(result));
		return;
	}
	if (length > BTRFS_NATIVE_XATTR_LIMIT) {
		reply(nil, [NSError errorWithDomain:NSPOSIXErrorDomain code:E2BIG userInfo:nil]);
		return;
	}
	value = [NSMutableData dataWithLength:length];
	result = btrfs_get_xattr(
	    _fs, &owned->inode, key.bytes, key.length, value.mutableBytes, value.length, &length);
	reply(result == BTRFS_OK ? value : nil, btrfs_error(result));
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
		reply(nil, btrfs_error(result));
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
		reply(nil, btrfs_error(result));
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
	reply(btrfs_error(BTRFS_READ_ONLY));
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
	reply(nil, nil, btrfs_error(BTRFS_READ_ONLY));
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
	reply(nil, nil, btrfs_error(BTRFS_READ_ONLY));
}

- (void)createLinkToItem:(FSItem *)item
		   named:(FSFileName *)name
	     inDirectory:(FSItem *)directory
	    replyHandler:(void (^)(FSFileName *, NSError *))reply
{
	(void)item;
	(void)name;
	(void)directory;
	reply(nil, btrfs_error(BTRFS_READ_ONLY));
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
	reply(nil, btrfs_error(BTRFS_READ_ONLY));
}

- (void)removeItem:(FSItem *)item
	     named:(FSFileName *)name
     fromDirectory:(FSItem *)directory
      replyHandler:(void (^)(NSError *))reply
{
	(void)item;
	(void)name;
	(void)directory;
	reply(btrfs_error(BTRFS_READ_ONLY));
}

- (void)setAttributes:(FSItemSetAttributesRequest *)newAttributes
	       onItem:(FSItem *)item
	 replyHandler:(void (^)(FSItemAttributes *, NSError *))reply
{
	(void)newAttributes;
	(void)item;
	reply(nil, btrfs_error(BTRFS_READ_ONLY));
}

- (void)writeContents:(NSData *)contents
	       toFile:(FSItem *)item
	     atOffset:(off_t)offset
	 replyHandler:(void (^)(size_t, NSError *))reply
{
	(void)contents;
	(void)item;
	(void)offset;
	reply(0, btrfs_error(BTRFS_READ_ONLY));
}

@end

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
	if (![options.taskOptions containsObject:@"-q"]) {
		if (error != NULL) {
			*error = [NSError
			    errorWithDomain:NSPOSIXErrorDomain
				       code:ENOTSUP
				   userInfo:@{
					   NSLocalizedDescriptionKey :
					       @"Only a quick read-only check is supported."
				   }];
		}
		return nil;
	}
	@synchronized(self) {
		volume = _volume;
	}
	if (volume == nil) {
		if (error != NULL) {
			*error = [NSError errorWithDomain:NSPOSIXErrorDomain
						     code:ENXIO
						 userInfo:nil];
		}
		return nil;
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
	(void)task;
	(void)options;
	if (error != NULL) {
		*error = btrfs_error(BTRFS_READ_ONLY);
	}
	return nil;
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

	error = btrfs_open_resource(resource, &fs);
	if (error != BTRFS_OK) {
		reply(error == BTRFS_NOT_BTRFS ? FSProbeResult.notRecognizedProbeResult : nil,
		    btrfs_error(error == BTRFS_NOT_BTRFS ? BTRFS_OK : error));
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
	reply([FSProbeResult usableButLimitedProbeResultWithName:name containerID:identifier], nil);
}

- (void)loadResource:(FSResource *)resource
	     options:(FSTaskOptions *)options
	replyHandler:(void (^)(FSVolume *, NSError *))reply
{
	struct btrfs_fs *fs = NULL;
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
			error = btrfs_open_resource(resource, &fs);
			if (error == BTRFS_OK) {
				volume = [[BtrfsVolume alloc] initWithResource:resource
								    filesystem:fs];
				if (volume == nil) {
					btrfs_unmount(fs);
					error = BTRFS_NO_MEMORY;
				}
			}
			loadError = btrfs_error(error);
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
