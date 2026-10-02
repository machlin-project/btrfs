/* SPDX-License-Identifier: BSD-3-Clause */
/* The FSKit volume through stand-ins for the framework's resource, packer and
 * file buffer: every regular file of the fixture reads as the core reads it,
 * aligned, unaligned and through partial device reads; directories enumerate
 * with "." and ".." only without attributes, across page sizes; write opens
 * are refused; a revoked resource fails reads; refused maintenance tasks
 * complete through their task; statistics name the Disk Arbitration type. */
#import "../adapters/fskit/BtrfsFileSystemInternal.h"
#include "../adapters/posix/image.h"
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
/* A device that returns at most this many bytes per read call. */
#define PARTIAL_BYTES 1536U
#define PAGE_SIZES 3U
#define READ_PIECES 5U

@interface ImageReader : NSObject <BtrfsBlockReader>
@property(readonly) uint64_t blockSize;
@property(readonly) uint64_t blockCount;
@property(readonly) uint64_t physicalBlockSize;
@property(getter=isRevoked) BOOL revoked;
@property size_t partial;
@property uint64_t calls;
- (instancetype)initWithPath:(const char *)path;
@end

@implementation ImageReader {
	int _descriptor;
}

- (instancetype)initWithPath:(const char *)path
{
	struct stat status;

	self = [super init];
	if (self != nil) {
		_descriptor = open(path, O_RDONLY);
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

@end

/* Names stay raw bytes: Btrfs names need not be UTF-8. */
@interface TestEntry : NSObject
@property NSData *name;
@property FSItemType type;
@property uint64_t itemID;
@property uint64_t cookie;
@property BOOL attributes;
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
open_volume(ImageReader *reader)
{
	struct btrfs_fs *fs = NULL;
	struct btrfs_fskit_cache *cache = btrfs_fskit_cache_create();
	BtrfsVolume *volume;

	REQUIRE(cache != NULL);
	REQUIRE(btrfs_fskit_open(reader, cache, &fs) == BTRFS_OK);
	volume = [[BtrfsVolume alloc] initWithReader:reader filesystem:fs cache:cache];
	REQUIRE(volume != nil);
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

/* Every entry of directory, page by page, as FSKit resumes with the last
 * packed entry's cookie. */
static NSArray<TestEntry *> *
enumerate(BtrfsVolume *volume, BtrfsItem *directory, BOOL attributes, NSUInteger page)
{
	NSMutableArray<TestEntry *> *all = [NSMutableArray array];
	FSItemGetAttributesRequest *request =
	    attributes ? [[FSItemGetAttributesRequest alloc] init] : nil;
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
		/* With attributes: no dot entries, attributes for each. */
		entries = enumerate(volume, many, YES, pages[i]);
		REQUIRE(entries.count == MANY_ENTRIES);
		for (TestEntry *entry in entries) {
			REQUIRE(entry.attributes && ((const char *)entry.name.bytes)[0] != '.');
		}
	}
	/* The root's ".." is the root. */
	entries = enumerate(volume, root, NO, 512);
	REQUIRE(entries.count >= 2 && entries[0].itemID == FSItemIDRootDirectory &&
	    entries[1].itemID == FSItemIDRootDirectory);
}

/* Reads item through the volume in uneven pieces and compares them with the
 * core's read of the same file. */
static void
compare_file(BtrfsVolume *volume, BtrfsItem *item, struct btrfs_fs *reference)
{
	struct btrfs_inode inode;
	TestBuffer *buffer;
	uint8_t *expected;
	size_t completed;
	uint64_t offset;
	size_t length;
	unsigned piece = 0;
	__block size_t replied;
	__block NSError *error;

	REQUIRE(btrfs_get_inode(reference, item->inode.id, &inode) == BTRFS_OK);
	expected = malloc((size_t)inode.size + 1);
	REQUIRE(expected != NULL);
	REQUIRE(btrfs_read(reference, &inode, 0, expected, (size_t)inode.size, &completed) ==
		BTRFS_OK &&
	    completed == inode.size);
	for (offset = 0; offset < inode.size; offset += length) {
		length = (size_t)(3 * SECTOR_BYTES + 7) * (piece++ % READ_PIECES + 1);
		if (length > inode.size - offset) {
			length = (size_t)(inode.size - offset);
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

static void
policy_tests(BtrfsVolume *volume, BtrfsItem *root)
{
	FSStatFSResult *statistics = volume.volumeStatistics;
	__block NSError *error = nil;
	__block BOOL replied = NO;

	REQUIRE([statistics.fileSystemTypeName isEqualToString:BTRFS_FSKIT_TYPE_NAME] &&
	    ![statistics.fileSystemTypeName containsString:@"_"]);
	REQUIRE(statistics.fileSystemSubType == 0 && statistics.ioSize == BTRFS_FSKIT_IO_SIZE);
	REQUIRE(!volume.isOpenCloseInhibited);
	[volume openItem:root
	       withModes:FSVolumeOpenModesRead
	    replyHandler:^(NSError *replyError) {
	      error = replyError;
	      replied = YES;
	    }];
	REQUIRE(replied && error == nil);
	replied = NO;
	[volume openItem:root
	       withModes:FSVolumeOpenModesRead | FSVolumeOpenModesWrite
	    replyHandler:^(NSError *replyError) {
	      error = replyError;
	      replied = YES;
	    }];
	REQUIRE(
	    replied && [error.domain isEqualToString:NSPOSIXErrorDomain] && error.code == EROFS);
}

/* Once the resource is revoked, reads that need the device fail with EIO;
 * without a cache, activation needs it. */
static void
revocation_test(const char *path)
{
	ImageReader *reader = [[ImageReader alloc] initWithPath:path];
	struct btrfs_fs *fs = NULL;
	BtrfsVolume *volume;
	__block NSError *error = nil;
	__block FSItem *root = nil;
	uint64_t calls;

	REQUIRE(btrfs_fskit_open(reader, NULL, &fs) == BTRFS_OK);
	volume = [[BtrfsVolume alloc] initWithReader:reader filesystem:fs cache:NULL];
	REQUIRE(volume != nil);
	reader.revoked = YES;
	calls = reader.calls;
	[volume activateWithOptions:(FSTaskOptions *)[TestOptions new]
		       replyHandler:^(FSItem *item, NSError *replyError) {
			 root = item;
			 error = replyError;
		       }];
	REQUIRE(root == nil && [error.domain isEqualToString:NSPOSIXErrorDomain] &&
	    error.code == EIO && reader.calls == calls);
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

int
main(int argc, char **argv)
{
	@autoreleasepool {
		struct btrfs_image image;
		struct btrfs_fs *reference;
		ImageReader *reader;
		BtrfsVolume *volume;
		BtrfsItem *root;

		REQUIRE(argc == 2);
		reader = [[ImageReader alloc] initWithPath:argv[1]];
		volume = open_volume(reader);
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
		printf(
		    "FSKit volume: reads equal the core's (direct, bounced, partial), dot entries "
		    "only without attributes across pages, write opens refused, revocation "
		    "fails reads, maintenance refusals complete through the task, Disk "
		    "Arbitration type name PASS\n");
	}
	return 0;
}
