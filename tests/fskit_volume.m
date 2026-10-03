/* SPDX-License-Identifier: BSD-3-Clause */
/* The FSKit volume through stand-ins for the framework's resource, packer,
 * file buffer and device barrier. Read-only: every regular file of the fixture
 * reads as the core reads it, aligned, unaligned and through partial device
 * reads; directories enumerate with "." and ".." only without attributes,
 * across page sizes; write opens and changes are refused; a revoked resource
 * fails reads; refused maintenance tasks complete through their task;
 * statistics name the Disk Arbitration type. Writable, on a scratch copy:
 * creation of the supported types, writes through partial device writes,
 * truncation, attributes, xattrs, links, renames, removal and an open-unlinked
 * file's eviction, each visible at once and durable after synchronization
 * through the barrier, which the core, the namespace audit and the reference
 * audit then confirm; a failed barrier fails synchronization and every later
 * change. Admission: a load binds no barrier for --rdonly or a read-only
 * device, and a process without a signing team never connects to one.
 * Recovery: after a cut between the secondary and primary superblock writes,
 * a read-only volume reads the primary and writes nothing, and a writable one
 * recovers to the newer root set before it opens. */
#import "../adapters/fskit/BtrfsDeviceBarrier.h"
#import "../adapters/fskit/BtrfsFileSystemInternal.h"
#include "../adapters/posix/image.h"
#include <btrfs/write.h>
#include "namespace_audit.h"
#include "references.h"
#include <copyfile.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REQUIRE(condition)                                                                         \
	do {                                                                                       \
		if (!(condition)) {                                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);            \
			exit(1);                                                                   \
		}                                                                                  \
	} while (0)

/* The fixture's large directory and its entry count. */
#define MANY_PATH "many"
#define MANY_ENTRIES 700U
#define SECTOR_BYTES 512U
/* A device that moves at most this many bytes per call. */
#define PARTIAL_BYTES 1536U
#define PAGE_SIZES 3U
#define READ_PIECES 5U
/* The owner the kernel would supply for a creating user. */
#define TEST_UID 501U
#define TEST_GID 20U
/* A file larger than an inline extent, written in uneven pieces. */
#define FILE_BYTES (300U * 1024U + 123U)
#define WRITE_PIECE (64U * 1024U + 7U)
#define TRUNCATED_BYTES (200U * 1024U + 1U)
/* Files of one data sector each that the staging test writes before one
 * barrier; their extents adjoin, so far fewer device writes issue them. */
#define STAGED_FILES 64U
#define STAGED_FILE_BYTES 4096U
/* One Btrfs superblock (BTRFS_SUPER_INFO_SIZE); recovery reports their offsets. */
#define SUPERBLOCK_BYTES 4096U
/* Every standard attribute FSKit may want; it faults on a reply missing one. */
#define STANDARD_ATTRIBUTES                                                                        \
	(FSItemAttributeType | FSItemAttributeMode | FSItemAttributeLinkCount |                    \
	    FSItemAttributeUID | FSItemAttributeGID | FSItemAttributeFlags | FSItemAttributeSize | \
	    FSItemAttributeAllocSize | FSItemAttributeFileID | FSItemAttributeParentID |           \
	    FSItemAttributeAccessTime | FSItemAttributeModifyTime | FSItemAttributeChangeTime |    \
	    FSItemAttributeBirthTime)

@interface ImageReader : NSObject <BtrfsBlockWriter>
@property(readonly) uint64_t blockSize;
@property(readonly) uint64_t blockCount;
@property(readonly) uint64_t physicalBlockSize;
@property(getter=isRevoked) BOOL revoked;
@property(readonly, getter=isWritable) BOOL writable;
@property(readonly) NSString *BSDName;
@property size_t partial;
@property BOOL failWrites;
@property uint64_t calls;
@property uint64_t writes;
- (instancetype)initWithPath:(const char *)path writable:(BOOL)writable;
@end

@implementation ImageReader {
	int _descriptor;
}

- (instancetype)initWithPath:(const char *)path writable:(BOOL)writable
{
	struct stat status;

	self = [super init];
	if (self != nil) {
		_writable = writable;
		_BSDName = @"disk9s9";
		_descriptor = open(path, writable ? O_RDWR : O_RDONLY);
		REQUIRE(_descriptor >= 0 && fstat(_descriptor, &status) == 0);
		_blockSize = SECTOR_BYTES;
		_physicalBlockSize = SECTOR_BYTES;
		_blockCount = (uint64_t)status.st_size / SECTOR_BYTES;
	}
	return self;
}

- (void)dealloc
{
	close(_descriptor);
}

- (size_t)readInto:(void *)buffer
	startingAt:(off_t)offset
	    length:(size_t)length
	     error:(NSError **)error
{
	ssize_t done;

	self.calls++;
	REQUIRE(offset % SECTOR_BYTES == 0 && length % SECTOR_BYTES == 0);
	if (self.partial != 0 && length > self.partial) {
		length = self.partial;
	}
	done = pread(_descriptor, buffer, length, offset);
	if (done < 0) {
		*error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
		return 0;
	}
	return (size_t)done;
}

- (size_t)writeFrom:(void *)buffer
	 startingAt:(off_t)offset
	     length:(size_t)length
	      error:(NSError **)error
{
	ssize_t done;

	REQUIRE(self.writable);
	REQUIRE(offset % SECTOR_BYTES == 0 && length % SECTOR_BYTES == 0);
	self.writes++;
	if (self.failWrites) {
		*error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:nil];
		return 0;
	}
	if (self.partial != 0 && length > self.partial) {
		length = self.partial;
	}
	done = pwrite(_descriptor, buffer, length, offset);
	if (done < 0) {
		*error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
		return 0;
	}
	return (size_t)done;
}

@end

/* A device barrier that counts its flushes and fails when told to. */
@interface TestFlusher : NSObject <BtrfsDeviceFlusher>
@property uint64_t flushes;
@property BOOL fail;
@end

@implementation TestFlusher

- (BOOL)synchronizeWithError:(NSError **)error
{
	self.flushes++;
	if (self.fail) {
		*error = [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO userInfo:nil];
		return NO;
	}
	return YES;
}

@end

/* Names stay raw bytes: Btrfs names need not be UTF-8. */
@interface TestEntry : NSObject
@property NSData *name;
@property FSItemType type;
@property uint64_t itemID;
@property uint64_t cookie;
@property BOOL attributes;
@property FSItemAttributes *values;
@end

@implementation TestEntry
@end

@interface TestPacker : NSObject
@property NSUInteger capacity;
@property NSMutableArray<TestEntry *> *entries;
@end

@implementation TestPacker

- (BOOL)packEntryWithName:(FSFileName *)name
		 itemType:(FSItemType)itemType
		   itemID:(FSItemID)itemID
	       nextCookie:(FSDirectoryCookie)nextCookie
	       attributes:(FSItemAttributes *)attributes
{
	TestEntry *entry;

	if (self.entries.count == self.capacity) {
		return NO;
	}
	entry = [TestEntry new];
	entry.name = name.data;
	entry.type = itemType;
	entry.itemID = itemID;
	entry.cookie = nextCookie;
	entry.attributes = attributes != nil;
	entry.values = attributes;
	[self.entries addObject:entry];
	return YES;
}

@end

@interface TestBuffer : NSObject
@property(readonly) NSUInteger length;
@property(readonly) void *mutableBytes;
- (instancetype)initWithLength:(NSUInteger)length;
@end

@implementation TestBuffer {
	NSMutableData *_data;
}

- (instancetype)initWithLength:(NSUInteger)length
{
	self = [super init];
	if (self != nil) {
		_data = [NSMutableData dataWithLength:length];
	}
	return self;
}

- (NSUInteger)length
{
	return _data.length;
}

- (void *)mutableBytes
{
	return _data.mutableBytes;
}

@end

@interface TestOptions : NSObject
@property NSArray<NSString *> *taskOptions;
@end

@implementation TestOptions
@end

@interface TestTask : NSObject
@property dispatch_semaphore_t done;
@property NSError *error;
@end

@implementation TestTask

- (void)didCompleteWithError:(NSError *)error
{
	self.error = error;
	dispatch_semaphore_signal(self.done);
}

@end

static BtrfsVolume *
open_volume(ImageReader *reader, TestFlusher *flusher)
{
	struct btrfs_fskit_cache *cache = btrfs_fskit_cache_create();
	enum btrfs_result result;
	BtrfsVolume *volume;

	REQUIRE(cache != NULL);
	volume = [BtrfsVolume volumeWithReader:reader flusher:flusher cache:cache result:&result];
	REQUIRE(volume != nil && result == BTRFS_OK);
	return volume;
}

static BtrfsItem *
root_item(BtrfsVolume *volume)
{
	__block BtrfsItem *root = nil;

	[volume activateWithOptions:(FSTaskOptions *)[TestOptions new]
		       replyHandler:^(FSItem *item, NSError *error) {
			 REQUIRE(error == nil && item != nil);
			 root = (BtrfsItem *)item;
		       }];
	return root;
}

static NSData *
bytes_of(const char *name)
{
	return [NSData dataWithBytes:name length:strlen(name)];
}

static FSFileName *
name_of(const char *name)
{
	return [FSFileName nameWithData:bytes_of(name)];
}

static BtrfsItem *
lookup(BtrfsVolume *volume, BtrfsItem *directory, NSData *name, NSError **failure)
{
	__block BtrfsItem *found = nil;
	__block NSError *error = nil;

	[volume lookupItemNamed:[FSFileName nameWithData:name]
		    inDirectory:directory
		   replyHandler:^(FSItem *item, FSFileName *reported, NSError *replyError) {
		     (void)reported;
		     found = (BtrfsItem *)item;
		     error = replyError;
		   }];
	if (failure != NULL) {
		*failure = error;
	}
	return found;
}

static void
require_complete(FSItemAttributes *attributes)
{
	unsigned bit;

	REQUIRE(attributes != nil);
	for (bit = 0; bit < 64; bit++) {
		if ((STANDARD_ATTRIBUTES & (UINT64_C(1) << bit)) != 0) {
			REQUIRE([attributes isValid:(FSItemAttribute)(UINT64_C(1) << bit)]);
		}
	}
}

/* The attributes FSKit would get when it wants every standard one. */
static FSItemAttributes *
attributes_of(BtrfsVolume *volume, BtrfsItem *item)
{
	FSItemGetAttributesRequest *request = [[FSItemGetAttributesRequest alloc] init];
	__block FSItemAttributes *found = nil;

	request.wantedAttributes = STANDARD_ATTRIBUTES;
	[volume getAttributes:request
		       ofItem:item
		 replyHandler:^(FSItemAttributes *attributes, NSError *error) {
		   REQUIRE(error == nil);
		   found = attributes;
		 }];
	require_complete(found);
	return found;
}

/* Every entry of directory, page by page, as FSKit resumes with the last
 * packed entry's cookie. */
static NSArray<TestEntry *> *
enumerate(BtrfsVolume *volume, BtrfsItem *directory, BOOL attributes, NSUInteger page)
{
	NSMutableArray<TestEntry *> *all = [NSMutableArray array];
	FSItemGetAttributesRequest *request =
	    attributes ? [[FSItemGetAttributesRequest alloc] init] : nil;

	request.wantedAttributes = STANDARD_ATTRIBUTES;
	__block FSDirectoryVerifier verifier = FSDirectoryVerifierInitial;
	__block NSError *error = nil;
	FSDirectoryCookie cookie = FSDirectoryCookieInitial;
	TestPacker *packer;
	unsigned rounds = 0;

	for (;;) {
		packer = [TestPacker new];
		packer.capacity = page;
		packer.entries = [NSMutableArray array];
		[volume enumerateDirectory:directory
			  startingAtCookie:cookie
				  verifier:verifier
		       providingAttributes:request
			       usingPacker:(FSDirectoryEntryPacker *)packer
			      replyHandler:^(FSDirectoryVerifier current, NSError *replyError) {
				verifier = current;
				error = replyError;
			      }];
		REQUIRE(error == nil && verifier != FSDirectoryVerifierInitial);
		[all addObjectsFromArray:packer.entries];
		if (packer.entries.count < page) {
			return all;
		}
		cookie = packer.entries.lastObject.cookie;
		REQUIRE(++rounds <= MANY_ENTRIES + 2);
	}
}

static void
enumeration_tests(BtrfsVolume *volume, BtrfsItem *root)
{
	static const NSUInteger pages[PAGE_SIZES] = { 1, 7, 512 };
	NSArray<TestEntry *> *entries;
	NSMutableSet<NSData *> *names;
	BtrfsItem *many;
	unsigned i;

	many = lookup(volume, root, bytes_of(MANY_PATH), NULL);
	REQUIRE(many != nil);
	for (i = 0; i < PAGE_SIZES; i++) {
		/* Without attributes: ".", "..", then every entry once. */
		entries = enumerate(volume, many, NO, pages[i]);
		REQUIRE(entries.count == MANY_ENTRIES + 2);
		REQUIRE([entries[0].name isEqualToData:bytes_of(".")] &&
		    [entries[1].name isEqualToData:bytes_of("..")]);
		REQUIRE(entries[0].type == FSItemTypeDirectory &&
		    entries[1].type == FSItemTypeDirectory);
		REQUIRE(entries[1].itemID == FSItemIDRootDirectory && entries[0].itemID != 0 &&
		    entries[0].itemID != entries[1].itemID);
		names = [NSMutableSet set];
		for (TestEntry *entry in entries) {
			REQUIRE(!entry.attributes);
			[names addObject:entry.name];
		}
		REQUIRE(names.count == MANY_ENTRIES + 2);
		/* With attributes: no dot entries, complete attributes for each,
		 * whose parent is the enumerated directory. */
		entries = enumerate(volume, many, YES, pages[i]);
		REQUIRE(entries.count == MANY_ENTRIES);
		for (TestEntry *entry in entries) {
			REQUIRE(entry.attributes && ((const char *)entry.name.bytes)[0] != '.');
			require_complete(entry.values);
			REQUIRE(entry.values.parentID == attributes_of(volume, many).fileID &&
			    entry.values.fileID == entry.itemID);
		}
	}
	/* The root's ".." is the root; its parent is FSKit's parent of the root,
	 * and a directory's parent is the one holding it. */
	entries = enumerate(volume, root, NO, 512);
	REQUIRE(entries.count >= 2 && entries[0].itemID == FSItemIDRootDirectory &&
	    entries[1].itemID == FSItemIDRootDirectory);
	REQUIRE(attributes_of(volume, root).fileID == FSItemIDRootDirectory &&
	    attributes_of(volume, root).parentID == FSItemIDParentOfRoot);
	REQUIRE(attributes_of(volume, many).parentID == FSItemIDRootDirectory);
}

/* Reads item through the volume in uneven pieces and compares them with
 * expected. */
static void
compare_bytes(BtrfsVolume *volume, BtrfsItem *item, const uint8_t *expected, uint64_t size)
{
	TestBuffer *buffer;
	uint64_t offset;
	size_t length;
	unsigned piece = 0;
	__block size_t replied;
	__block NSError *error;

	for (offset = 0; offset < size; offset += length) {
		length = (size_t)(3 * SECTOR_BYTES + 7) * (piece++ % READ_PIECES + 1);
		if (length > size - offset) {
			length = (size_t)(size - offset);
		}
		buffer = [[TestBuffer alloc] initWithLength:length];
		[volume readFromFile:item
			      offset:(off_t)offset
			      length:length
			  intoBuffer:(FSMutableFileDataBuffer *)buffer
			replyHandler:^(size_t done, NSError *replyError) {
			  replied = done;
			  error = replyError;
			}];
		REQUIRE(error == nil && replied == length);
		REQUIRE(memcmp(buffer.mutableBytes, expected + offset, length) == 0);
	}
}

/* Reads item through the volume and compares it with the core's read. */
static void
compare_file(BtrfsVolume *volume, BtrfsItem *item, struct btrfs_fs *reference)
{
	struct btrfs_inode inode;
	uint8_t *expected;
	size_t completed;

	REQUIRE(btrfs_get_inode(reference, item->inode.id, &inode) == BTRFS_OK);
	expected = malloc((size_t)inode.size + 1);
	REQUIRE(expected != NULL);
	REQUIRE(btrfs_read(reference, &inode, 0, expected, (size_t)inode.size, &completed) ==
		BTRFS_OK &&
	    completed == inode.size);
	compare_bytes(volume, item, expected, inode.size);
	free(expected);
}

static void
read_tests(BtrfsVolume *volume, BtrfsItem *root, struct btrfs_fs *reference, ImageReader *reader)
{
	NSArray<TestEntry *> *entries = enumerate(volume, root, YES, 512);
	BtrfsItem *item;
	unsigned files = 0;

	for (TestEntry *entry in entries) {
		item = lookup(volume, root, entry.name, NULL);
		REQUIRE(item != nil);
		if (entry.type == FSItemTypeFile) {
			compare_file(volume, item, reference);
			files++;
		}
	}
	REQUIRE(files != 0);
	/* Devices may return less than asked without an error. */
	reader.partial = PARTIAL_BYTES;
	for (TestEntry *entry in entries) {
		if (entry.type == FSItemTypeFile) {
			compare_file(volume, lookup(volume, root, entry.name, NULL), reference);
		}
	}
	reader.partial = 0;
}

static NSError *
open_item(BtrfsVolume *volume, BtrfsItem *item, FSVolumeOpenModes modes)
{
	__block NSError *error = nil;
	__block BOOL replied = NO;

	[volume openItem:item
	       withModes:modes
	    replyHandler:^(NSError *replyError) {
	      error = replyError;
	      replied = YES;
	    }];
	REQUIRE(replied);
	return error;
}

static FSItemSetAttributesRequest *
owner_request(uint32_t mode)
{
	FSItemSetAttributesRequest *request = [[FSItemSetAttributesRequest alloc] init];

	request.mode = mode;
	request.uid = TEST_UID;
	request.gid = TEST_GID;
	REQUIRE([request isValid:FSItemAttributeMode] && [request isValid:FSItemAttributeUID] &&
	    [request isValid:FSItemAttributeGID]);
	return request;
}

static BtrfsItem *
create(BtrfsVolume *volume, BtrfsItem *directory, const char *name, FSItemType type, uint32_t mode,
    NSError **failure)
{
	__block BtrfsItem *created = nil;
	__block NSError *error = nil;

	[volume createItemNamed:name_of(name)
			   type:type
		    inDirectory:directory
		     attributes:owner_request(mode)
		   replyHandler:^(FSItem *item, FSFileName *reported, NSError *replyError) {
		     (void)reported;
		     created = (BtrfsItem *)item;
		     error = replyError;
		   }];
	if (failure != NULL) {
		*failure = error;
	}
	return created;
}

static NSError *
write_data(
    BtrfsVolume *volume, BtrfsItem *item, uint64_t offset, const uint8_t *bytes, size_t length)
{
	__block NSError *error = nil;
	__block size_t written = SIZE_MAX;

	[volume writeContents:[NSData dataWithBytes:bytes length:length]
		       toFile:item
		     atOffset:(off_t)offset
		 replyHandler:^(size_t done, NSError *replyError) {
		   written = done;
		   error = replyError;
		 }];
	REQUIRE(error != nil || written == length);
	return error;
}

static NSError *
synchronize(BtrfsVolume *volume)
{
	__block NSError *error = nil;

	[volume synchronizeWithFlags:FSSyncFlagsWait
			replyHandler:^(NSError *replyError) {
			  error = replyError;
			}];
	return error;
}

static void
policy_tests(BtrfsVolume *volume, BtrfsItem *root)
{
	FSStatFSResult *statistics = volume.volumeStatistics;
	NSError *error = nil;

	REQUIRE([statistics.fileSystemTypeName isEqualToString:BTRFS_FSKIT_TYPE_NAME] &&
	    ![statistics.fileSystemTypeName containsString:@"_"]);
	REQUIRE(statistics.fileSystemSubType == 0 && statistics.ioSize == BTRFS_FSKIT_IO_SIZE);
	REQUIRE(!volume.isOpenCloseInhibited && !volume.isWritable &&
	    volume.requestedMountOptions == FSMountOptionsReadOnly);
	REQUIRE(open_item(volume, root, FSVolumeOpenModesRead) == nil);
	error = open_item(volume, root, FSVolumeOpenModesRead | FSVolumeOpenModesWrite);
	REQUIRE([error.domain isEqualToString:NSPOSIXErrorDomain] && error.code == EROFS);
	REQUIRE(create(volume, root, "refused", FSItemTypeFile, 0644, &error) == nil &&
	    error.code == EROFS);
}

/* Once the resource is revoked, reads that need the device fail with EIO. */
static void
revocation_test(const char *path)
{
	ImageReader *reader = [[ImageReader alloc] initWithPath:path writable:NO];
	BtrfsVolume *volume;
	__block NSError *error = nil;
	__block FSItem *root = nil;
	enum btrfs_result result;
	uint64_t calls;

	volume = [BtrfsVolume volumeWithReader:reader flusher:nil cache:NULL result:&result];
	REQUIRE(volume != nil && result == BTRFS_OK);
	reader.revoked = YES;
	calls = reader.calls;
	[volume activateWithOptions:(FSTaskOptions *)[TestOptions new]
		       replyHandler:^(FSItem *item, NSError *replyError) {
			 root = item;
			 error = replyError;
		       }];
	if (root != nil) {
		/* Activation found the root without the device; a lookup needs it. */
		REQUIRE(lookup(volume, (BtrfsItem *)root, bytes_of(MANY_PATH), &error) == nil);
	}
	REQUIRE([error.domain isEqualToString:NSPOSIXErrorDomain] && error.code == EIO &&
	    reader.calls == calls);
}

static void
refusal_tests(void)
{
	BtrfsFileSystem *filesystem = [[BtrfsFileSystem alloc] init];
	TestOptions *options = [TestOptions new];
	TestTask *task = [TestTask new];
	NSProgress *progress;
	NSError *error = nil;

	/* A full check and formatting are refused through the task, not
	 * synchronously. */
	options.taskOptions = @[ @"-n" ];
	task.done = dispatch_semaphore_create(0);
	progress = [filesystem startCheckWithTask:(FSTask *)task
					  options:(FSTaskOptions *)options
					    error:&error];
	REQUIRE(progress != nil && error == nil);
	REQUIRE(dispatch_semaphore_wait(
		    task.done, dispatch_time(DISPATCH_TIME_NOW, 10 * (int64_t)NSEC_PER_SEC)) == 0);
	REQUIRE(task.error.code == ENOTSUP);
	task = [TestTask new];
	task.done = dispatch_semaphore_create(0);
	progress = [filesystem startFormatWithTask:(FSTask *)task
					   options:(FSTaskOptions *)options
					     error:&error];
	REQUIRE(progress != nil && error == nil);
	REQUIRE(dispatch_semaphore_wait(
		    task.done, dispatch_time(DISPATCH_TIME_NOW, 10 * (int64_t)NSEC_PER_SEC)) == 0);
	REQUIRE(task.error.code == EROFS);
}

/* A scratch copy of the fixture, removed by the caller. */
static NSString *
scratch_copy(const char *fixture)
{
	NSString *path = [NSTemporaryDirectory()
	    stringByAppendingPathComponent:[NSString stringWithFormat:@"btrfs-fskit-%d-%@.raw",
					       getpid(), NSUUID.UUID.UUIDString]];

	REQUIRE(copyfile(fixture, path.fileSystemRepresentation, NULL,
		    COPYFILE_DATA | COPYFILE_CLONE) == 0);
	return path;
}

/* The written image through the core: names, contents, and both audits. */
static void
verify_written(NSString *path, const uint8_t *expected)
{
	struct btrfs_image image;
	struct btrfs_fs *fs;
	struct btrfs_inode inode;
	struct namespace_audit names;
	struct reference_audit references;
	uint8_t *bytes = malloc(TRUNCATED_BYTES);
	char target[64];
	size_t completed;

	REQUIRE(bytes != NULL);
	REQUIRE(btrfs_image_open(path.fileSystemRepresentation, &image) == 0);
	REQUIRE(btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	REQUIRE(btrfs_image_lookup(fs, "/fskit-dir/moved", &inode) == BTRFS_OK);
	REQUIRE(inode.size == TRUNCATED_BYTES && (inode.mode & ALLPERMS) == 0600 &&
	    inode.uid == TEST_UID && inode.gid == TEST_GID && inode.links == 2);
	REQUIRE(btrfs_read(fs, &inode, 0, bytes, TRUNCATED_BYTES, &completed) == BTRFS_OK &&
	    completed == TRUNCATED_BYTES && memcmp(bytes, expected, TRUNCATED_BYTES) == 0);
	REQUIRE(btrfs_get_xattr(fs, &inode, "user.kept", 9, NULL, 0, &completed) == BTRFS_OK &&
	    completed == 5);
	REQUIRE(
	    btrfs_get_xattr(fs, &inode, "user.gone", 9, NULL, 0, &completed) == BTRFS_NOT_FOUND);
	REQUIRE(btrfs_image_lookup(fs, "/fskit-link", &inode) == BTRFS_OK && inode.links == 2);
	REQUIRE(btrfs_image_lookup(fs, "/fskit-symlink", &inode) == BTRFS_OK &&
	    (inode.mode & BTRFS_MODE_TYPE) == BTRFS_MODE_SYMLINK);
	REQUIRE(btrfs_read(fs, &inode, 0, target, sizeof(target), &completed) == BTRFS_OK &&
	    completed == strlen("fskit-dir/moved") &&
	    memcmp(target, "fskit-dir/moved", completed) == 0);
	REQUIRE(btrfs_image_lookup(fs, "/fskit-fifo", &inode) == BTRFS_OK &&
	    (inode.mode & BTRFS_MODE_TYPE) == BTRFS_MODE_FIFO);
	REQUIRE(btrfs_image_lookup(fs, "/fskit-open", &inode) == BTRFS_NOT_FOUND);
	REQUIRE(btrfs_image_lookup(fs, "/fskit-removed", &inode) == BTRFS_NOT_FOUND);
	if (namespace_audit(fs, &names) != 0) {
		fprintf(stderr, "namespace audit: %s\n", names.failure);
		REQUIRE(0);
	}
	REQUIRE(names.orphans == 0);
	if (reference_audit(fs, &references) != 0) {
		fprintf(stderr, "reference audit: %s\n", references.failure);
		REQUIRE(0);
	}
	btrfs_unmount(fs);
	btrfs_image_close(&image);
	free(bytes);
}

static void
write_tests(const char *fixture)
{
	NSString *path = scratch_copy(fixture);
	ImageReader *reader = [[ImageReader alloc] initWithPath:path.fileSystemRepresentation
						       writable:YES];
	TestFlusher *flusher = [TestFlusher new];
	BtrfsVolume *volume = open_volume(reader, flusher);
	BtrfsItem *root = root_item(volume);
	BtrfsItem *file;
	BtrfsItem *directory;
	BtrfsItem *open;
	FSItemSetAttributesRequest *request;
	FSItemAttributes *attributes;
	TestPacker *packer = [TestPacker new];
	uint8_t *expected = malloc(FILE_BYTES);
	__block NSError *error = nil;
	__block FSDirectoryVerifier verifier = FSDirectoryVerifierInitial;
	__block FSItem *made = nil;
	__block FSFileName *link = nil;
	uint64_t offset;
	uint64_t writes;
	size_t length;
	size_t i;

	REQUIRE(expected != NULL);
	for (i = 0; i < FILE_BYTES; i++) {
		expected[i] = (uint8_t)(i * 2654435761U >> 13);
	}
	REQUIRE(volume.isWritable && volume.requestedMountOptions == 0);
	REQUIRE(open_item(volume, root, FSVolumeOpenModesRead | FSVolumeOpenModesWrite) == nil);
	/* A changed directory invalidates its enumeration verifier. */
	packer.capacity = 1;
	packer.entries = [NSMutableArray array];
	[volume enumerateDirectory:root
		  startingAtCookie:FSDirectoryCookieInitial
			  verifier:FSDirectoryVerifierInitial
	       providingAttributes:nil
		       usingPacker:(FSDirectoryEntryPacker *)packer
		      replyHandler:^(FSDirectoryVerifier current, NSError *replyError) {
			verifier = current;
			error = replyError;
		      }];
	REQUIRE(error == nil);
	file = create(volume, root, "fskit-file", FSItemTypeFile, 0644, &error);
	REQUIRE(file != nil && error == nil);
	[volume enumerateDirectory:root
		  startingAtCookie:packer.entries.lastObject.cookie
			  verifier:verifier
	       providingAttributes:nil
		       usingPacker:(FSDirectoryEntryPacker *)packer
		      replyHandler:^(FSDirectoryVerifier current, NSError *replyError) {
			(void)current;
			error = replyError;
		      }];
	REQUIRE(error.code == ESTALE);
	/* Uneven writes, staged until the barrier, which issues them through a
	 * device that moves partial pieces; reads see the staged bytes. */
	reader.partial = PARTIAL_BYTES;
	writes = reader.writes;
	for (offset = 0; offset < FILE_BYTES; offset += length) {
		length =
		    FILE_BYTES - offset < WRITE_PIECE ? (size_t)(FILE_BYTES - offset) : WRITE_PIECE;
		REQUIRE(write_data(volume, file, offset, expected + offset, length) == nil);
	}
	REQUIRE(reader.writes == writes);
	compare_bytes(volume, file, expected, FILE_BYTES);
	REQUIRE(synchronize(volume) == nil);
	reader.partial = 0;
	REQUIRE(reader.writes > writes);
	compare_bytes(volume, file, expected, FILE_BYTES);
	/* Truncation and attributes in one request. */
	request = [[FSItemSetAttributesRequest alloc] init];
	request.size = TRUNCATED_BYTES;
	request.mode = 0600;
	[volume setAttributes:request
		       onItem:file
		 replyHandler:^(FSItemAttributes *changed, NSError *replyError) {
		   REQUIRE(replyError == nil);
		   REQUIRE(changed.size == TRUNCATED_BYTES && changed.mode == 0600);
		 }];
	REQUIRE([request wasAttributeConsumed:FSItemAttributeSize] &&
	    [request wasAttributeConsumed:FSItemAttributeMode]);
	compare_bytes(volume, file, expected, TRUNCATED_BYTES);
	[volume setXattrNamed:name_of("user.kept")
		       toData:bytes_of("value")
		       onItem:file
		       policy:FSSetXattrPolicyMustCreate
		 replyHandler:^(NSError *replyError) {
		   REQUIRE(replyError == nil);
		 }];
	[volume setXattrNamed:name_of("user.gone")
		       toData:bytes_of("x")
		       onItem:file
		       policy:FSSetXattrPolicyAlwaysSet
		 replyHandler:^(NSError *replyError) {
		   REQUIRE(replyError == nil);
		 }];
	[volume setXattrNamed:name_of("user.gone")
		       toData:nil
		       onItem:file
		       policy:FSSetXattrPolicyDelete
		 replyHandler:^(NSError *replyError) {
		   REQUIRE(replyError == nil);
		 }];
	[volume setXattrNamed:name_of("user.gone")
		       toData:nil
		       onItem:file
		       policy:FSSetXattrPolicyDelete
		 replyHandler:^(NSError *replyError) {
		   REQUIRE(replyError.code == ENOATTR);
		 }];
	[volume getXattrNamed:name_of("user.kept")
		       ofItem:file
		 replyHandler:^(NSData *value, NSError *replyError) {
		   REQUIRE(replyError == nil && [value isEqualToData:bytes_of("value")]);
		 }];
	/* A directory, a rename into it, a second name and a symbolic link. */
	directory = create(volume, root, "fskit-dir", FSItemTypeDirectory, 0755, &error);
	REQUIRE(directory != nil && error == nil);
	REQUIRE(create(volume, root, "fskit-dir", FSItemTypeDirectory, 0755, &error) == nil &&
	    error.code == EEXIST);
	[volume renameItem:file
	       inDirectory:root
		     named:name_of("fskit-file")
		 toNewName:name_of("moved")
	       inDirectory:directory
		  overItem:nil
	      replyHandler:^(FSFileName *name, NSError *replyError) {
		REQUIRE(replyError == nil && name != nil);
	      }];
	[volume createLinkToItem:file
			   named:name_of("fskit-link")
		     inDirectory:root
		    replyHandler:^(FSFileName *name, NSError *replyError) {
		      link = name;
		      error = replyError;
		    }];
	REQUIRE(error == nil && link != nil);
	attributes = attributes_of(volume, file);
	REQUIRE(attributes.linkCount == 2 && attributes.size == TRUNCATED_BYTES);
	/* FSKit asks for the attributes of a rename's absent target. */
	[volume getAttributes:[[FSItemGetAttributesRequest alloc] init]
		       ofItem:(FSItem *_Nonnull)nil
		 replyHandler:^(FSItemAttributes *nothing, NSError *replyError) {
		   REQUIRE(nothing == nil && replyError.code == ESTALE);
		 }];
	/* A renamed file's parent is the directory it moved into. */
	REQUIRE(attributes.parentID == attributes_of(volume, directory).fileID &&
	    attributes_of(volume, directory).parentID == FSItemIDRootDirectory);
	[volume createSymbolicLinkNamed:name_of("fskit-symlink")
			    inDirectory:root
			     attributes:owner_request(0777)
			   linkContents:name_of("fskit-dir/moved")
			   replyHandler:^(FSItem *item, FSFileName *name, NSError *replyError) {
			     (void)name;
			     made = item;
			     error = replyError;
			   }];
	REQUIRE(made != nil && error == nil);
	[volume readSymbolicLink:made
		    replyHandler:^(FSFileName *contents, NSError *replyError) {
		      REQUIRE(replyError == nil &&
			  [contents.data isEqualToData:bytes_of("fskit-dir/moved")]);
		    }];
	REQUIRE(create(volume, root, "fskit-fifo", FSItemTypeFIFO, 0644, &error) != nil);
	REQUIRE(create(volume, root, "fskit-device", FSItemTypeCharDevice, 0644, &error) == nil &&
	    error.code == ENOTSUP);
	/* Removal, and an open file that outlives its name until reclaimed. */
	REQUIRE(create(volume, root, "fskit-removed", FSItemTypeFile, 0644, &error) != nil);
	[volume removeItem:lookup(volume, root, bytes_of("fskit-removed"), NULL)
		     named:name_of("fskit-removed")
	     fromDirectory:root
	      replyHandler:^(NSError *replyError) {
		REQUIRE(replyError == nil);
	      }];
	REQUIRE(
	    lookup(volume, root, bytes_of("fskit-removed"), &error) == nil && error.code == ENOENT);
	open = create(volume, root, "fskit-open", FSItemTypeFile, 0644, &error);
	REQUIRE(open != nil && write_data(volume, open, 0, expected, 4096) == nil);
	REQUIRE(open_item(volume, open, FSVolumeOpenModesRead) == nil);
	[volume removeItem:open
		     named:name_of("fskit-open")
	     fromDirectory:root
	      replyHandler:^(NSError *replyError) {
		REQUIRE(replyError == nil);
	      }];
	REQUIRE(open->orphan);
	compare_bytes(volume, open, expected, 4096);
	[volume closeItem:open
	     keepingModes:0
	     replyHandler:^(NSError *replyError) {
	       REQUIRE(replyError == nil);
	     }];
	[volume reclaimItem:open
	       replyHandler:^(NSError *replyError) {
		 REQUIRE(replyError == nil);
	       }];
	REQUIRE(!open->orphan);
	/* Synchronization commits through the barrier. */
	REQUIRE(synchronize(volume) == nil && flusher.flushes >= 3);
	REQUIRE([volume shutdown] == BTRFS_OK);
	volume = nil;
	verify_written(path, expected);
	free(expected);
	REQUIRE([NSFileManager.defaultManager removeItemAtPath:path error:NULL]);
}

/* Writes wait for the barrier and merge: files of one sector each, written
 * together, reach the device in far fewer writes than files, and a fresh
 * load reads them. */
static void
staging_test(const char *fixture)
{
	NSString *path = scratch_copy(fixture);
	ImageReader *reader = [[ImageReader alloc] initWithPath:path.fileSystemRepresentation
						       writable:YES];
	TestFlusher *flusher = [TestFlusher new];
	BtrfsVolume *volume = open_volume(reader, flusher);
	BtrfsItem *root = root_item(volume);
	BtrfsItem *files[STAGED_FILES];
	uint8_t contents[STAGED_FILE_BYTES];
	char name[32];
	NSError *error = nil;
	uint64_t writes;
	uint64_t issued;
	unsigned i;

	REQUIRE(synchronize(volume) == nil);
	writes = reader.writes;
	for (i = 0; i < STAGED_FILES; i++) {
		snprintf(name, sizeof(name), "staged-%02u", i);
		memset(contents, (int)(i + 1U), sizeof(contents));
		files[i] = create(volume, root, name, FSItemTypeFile, 0644, &error);
		REQUIRE(files[i] != nil && error == nil);
		REQUIRE(write_data(volume, files[i], 0, contents, sizeof(contents)) == nil);
		compare_bytes(volume, files[i], contents, sizeof(contents));
	}
	REQUIRE(reader.writes == writes);
	REQUIRE(synchronize(volume) == nil);
	issued = reader.writes - writes;
	REQUIRE(issued != 0 && issued < STAGED_FILES / 4U);
	volume = nil;
	volume = open_volume(reader, flusher);
	root = root_item(volume);
	for (i = 0; i < STAGED_FILES; i++) {
		snprintf(name, sizeof(name), "staged-%02u", i);
		memset(contents, (int)(i + 1U), sizeof(contents));
		files[i] =
		    lookup(volume, root, [NSData dataWithBytes:name length:strlen(name)], &error);
		REQUIRE(files[i] != nil);
		compare_bytes(volume, files[i], contents, sizeof(contents));
	}
	volume = nil;
	REQUIRE([NSFileManager.defaultManager removeItemAtPath:path error:NULL]);
	printf("FSKit staging: %u files of one sector in %llu device writes at the barrier\n",
	    STAGED_FILES, (unsigned long long)issued);
}

/* A staged write that fails at the barrier fails synchronization, and the
 * volume refuses every later change instead of acknowledging lost ones. */
static void
staged_failure_test(const char *fixture)
{
	NSString *path = scratch_copy(fixture);
	ImageReader *reader = [[ImageReader alloc] initWithPath:path.fileSystemRepresentation
						       writable:YES];
	TestFlusher *flusher = [TestFlusher new];
	BtrfsVolume *volume = open_volume(reader, flusher);
	BtrfsItem *root = root_item(volume);
	BtrfsItem *file;
	uint8_t contents[STAGED_FILE_BYTES];
	NSError *error = nil;
	uint64_t flushes;

	memset(contents, 0x5a, sizeof(contents));
	file = create(volume, root, "unissued", FSItemTypeFile, 0644, &error);
	REQUIRE(file != nil && write_data(volume, file, 0, contents, sizeof(contents)) == nil);
	reader.failWrites = YES;
	flushes = flusher.flushes;
	error = synchronize(volume);
	REQUIRE(error != nil && error.code == EIO && flusher.flushes == flushes);
	reader.failWrites = NO;
	REQUIRE(create(volume, root, "after-failure", FSItemTypeFile, 0644, &error) == nil &&
	    error != nil);
	volume = nil;
	REQUIRE([NSFileManager.defaultManager removeItemAtPath:path error:NULL]);
}

/* A barrier that fails makes synchronization fail, and the volume refuses
 * every later change instead of acknowledging lost ones. */
static void
barrier_failure_test(const char *fixture)
{
	NSString *path = scratch_copy(fixture);
	ImageReader *reader = [[ImageReader alloc] initWithPath:path.fileSystemRepresentation
						       writable:YES];
	TestFlusher *flusher = [TestFlusher new];
	BtrfsVolume *volume = open_volume(reader, flusher);
	BtrfsItem *root = root_item(volume);
	NSError *error = nil;

	REQUIRE(create(volume, root, "unsynchronized", FSItemTypeFile, 0644, &error) != nil);
	flusher.fail = YES;
	error = synchronize(volume);
	REQUIRE(error != nil && error.code == EIO);
	REQUIRE(create(volume, root, "after-failure", FSItemTypeFile, 0644, &error) == nil &&
	    error != nil);
	volume = nil;
	REQUIRE([NSFileManager.defaultManager removeItemAtPath:path error:NULL]);
}

/* A filesystem whose barrier is a stand-in, counting the loads that ask for it. */
@interface AdmissionFileSystem : BtrfsFileSystem
@property TestFlusher *barrier;
@property uint64_t requests;
@end

@implementation AdmissionFileSystem

- (id<BtrfsDeviceFlusher>)barrierForDevice:(FSBlockDeviceResource *)device error:(NSError **)error
{
	(void)device;
	self.requests++;
	if (self.barrier == nil) {
		*error = [NSError errorWithDomain:NSPOSIXErrorDomain
					     code:ECONNREFUSED
					 userInfo:nil];
	}
	return self.barrier;
}

@end

/* Which loads bind the device barrier. This process has no signing team, so the
 * client cannot authenticate the service and refuses it before connecting. */
static void
admission_test(const char *fixture)
{
	BtrfsFileSystem *filesystem = [[BtrfsFileSystem alloc] init];
	AdmissionFileSystem *counted = [[AdmissionFileSystem alloc] init];
	NSString *path = scratch_copy(fixture);
	ImageReader *writable = [[ImageReader alloc] initWithPath:path.fileSystemRepresentation
							 writable:YES];
	ImageReader *readonly = [[ImageReader alloc] initWithPath:fixture writable:NO];
	TestOptions *options = [TestOptions new];
	NSError *error = nil;

	REQUIRE(btrfs_peer_requirement(BTRFS_BARRIER_IDENTIFIER) == nil);
	REQUIRE([[BtrfsDeviceBarrier alloc] initWithDevice:writable.BSDName
						 blockSize:writable.blockSize
						blockCount:writable.blockCount
						     error:&error] == nil);
	REQUIRE([error.domain isEqualToString:NSPOSIXErrorDomain] && error.code == EACCES);
	REQUIRE(![BtrfsDeviceBarrier isServiceAvailable]);
	options.taskOptions = @[ @"--rdonly" ];
	REQUIRE([filesystem flusherForDevice:(FSBlockDeviceResource *)writable
				     options:(FSTaskOptions *)options] == nil);
	options.taskOptions = @[];
	REQUIRE([filesystem flusherForDevice:(FSBlockDeviceResource *)readonly
				     options:(FSTaskOptions *)options] == nil);
	REQUIRE([filesystem flusherForDevice:(FSBlockDeviceResource *)writable
				     options:(FSTaskOptions *)options] == nil);
	/* Only a writable device without --rdonly asks for the barrier. */
	counted.barrier = [TestFlusher new];
	options.taskOptions = @[ @"--rdonly" ];
	REQUIRE([counted flusherForDevice:(FSBlockDeviceResource *)writable
				  options:(FSTaskOptions *)options] == nil);
	options.taskOptions = @[];
	REQUIRE([counted flusherForDevice:(FSBlockDeviceResource *)readonly
				  options:(FSTaskOptions *)options] == nil);
	REQUIRE(counted.requests == 0);
	REQUIRE([counted flusherForDevice:(FSBlockDeviceResource *)writable
				  options:(FSTaskOptions *)options] == counted.barrier);
	counted.barrier = nil;
	REQUIRE([counted flusherForDevice:(FSBlockDeviceResource *)writable
				  options:(FSTaskOptions *)options] == nil);
	REQUIRE(counted.requests == 2);
	REQUIRE(writable.writes == 0 && readonly.writes == 0);
	writable = nil;
	REQUIRE([NSFileManager.defaultManager removeItemAtPath:path error:NULL]);
}

/* The state a cut leaves between the secondary and primary superblock writes:
 * the primary one generation behind its secondary. */
static void
recovery_test(const char *fixture)
{
	NSString *path = scratch_copy(fixture);
	TestFlusher *flusher = [TestFlusher new];
	ImageReader *reader = [[ImageReader alloc] initWithPath:path.fileSystemRepresentation
						       writable:YES];
	BtrfsVolume *volume;
	struct btrfs_image image;
	struct btrfs_recovery_report report;
	uint8_t primary[SUPERBLOCK_BYTES];
	uint64_t offset;
	unsigned i;
	int descriptor;
	NSError *error = nil;

	REQUIRE(btrfs_image_open(path.fileSystemRepresentation, &image) == 0);
	REQUIRE(btrfs_recover_supers(&image.environment, NULL, 0, &report) == BTRFS_OK);
	btrfs_image_close(&image);
	offset = report.copies[0].offset;
	descriptor = open(path.fileSystemRepresentation, O_RDWR);
	REQUIRE(descriptor >= 0 &&
	    pread(descriptor, primary, sizeof(primary), (off_t)offset) == sizeof(primary));
	volume = open_volume(reader, flusher);
	REQUIRE(
	    create(volume, root_item(volume), "after-cut", FSItemTypeFile, 0644, &error) != nil);
	REQUIRE(synchronize(volume) == nil && [volume shutdown] == BTRFS_OK);
	volume = nil;
	REQUIRE(pwrite(descriptor, primary, sizeof(primary), (off_t)offset) == sizeof(primary) &&
	    close(descriptor) == 0);
	reader.writes = 0;
	/* Read-only: the primary's root set, and no byte written. */
	volume = open_volume(reader, nil);
	REQUIRE(lookup(volume, root_item(volume), bytes_of("after-cut"), &error) == nil &&
	    reader.writes == 0);
	volume = nil;
	/* Writable: recovery selects the newer root set and rewrites the primary. */
	volume = open_volume(reader, flusher);
	REQUIRE(lookup(volume, root_item(volume), bytes_of("after-cut"), NULL) != nil &&
	    reader.writes != 0);
	REQUIRE([volume shutdown] == BTRFS_OK);
	volume = nil;
	REQUIRE(btrfs_image_open(path.fileSystemRepresentation, &image) == 0);
	REQUIRE(btrfs_recover_supers(&image.environment, NULL, 0, &report) == BTRFS_OK);
	for (i = 0; i < BTRFS_SUPER_COPIES; i++) {
		REQUIRE(report.copies[i].status != BTRFS_OK || report.copies[i].current);
	}
	btrfs_image_close(&image);
	reader = nil;
	REQUIRE([NSFileManager.defaultManager removeItemAtPath:path error:NULL]);
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		struct btrfs_image image;
		struct btrfs_fs *reference;
		ImageReader *reader;
		BtrfsVolume *volume;
		BtrfsItem *root;

		REQUIRE(argc == 3);
		reader = [[ImageReader alloc] initWithPath:argv[1] writable:NO];
		volume = open_volume(reader, nil);
		root = root_item(volume);
		REQUIRE(btrfs_image_open(argv[1], &image) == 0);
		REQUIRE(
		    btrfs_mount(&image.environment, BTRFS_TOP_LEVEL_TREE, &reference) == BTRFS_OK);
		enumeration_tests(volume, root);
		read_tests(volume, root, reference, reader);
		policy_tests(volume, root);
		revocation_test(argv[1]);
		refusal_tests();
		btrfs_unmount(reference);
		btrfs_image_close(&image);
		write_tests(argv[2]);
		barrier_failure_test(argv[2]);
		staging_test(argv[2]);
		staged_failure_test(argv[2]);
		admission_test(argv[2]);
		recovery_test(argv[2]);
		printf(
		    "FSKit volume: reads equal the core's (direct, bounced, partial), dot entries "
		    "only without attributes across pages, read-only refusals, revocation, "
		    "maintenance refusals, Disk Arbitration type; writable creation, writes, "
		    "truncation, attributes, xattrs, links, renames, removal and eviction "
		    "durable through the barrier and audited, writes staged and merged until "
		    "the barrier, barrier and staged write failures fail the volume, no barrier "
		    "for --rdonly, read-only devices or an unsigned "
		    "process, recovery only for a writable volume PASS\n");
	}
	return 0;
}
