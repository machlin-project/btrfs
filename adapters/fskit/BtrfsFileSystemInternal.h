/* SPDX-License-Identifier: BSD-3-Clause */
/* The adapter's volume and its block device, shared between the adapter's files
 * and with component tests. */
#import "BtrfsFileSystem.h"
#include <btrfs/btrfs.h>
#include <btrfs/identity.h>
#include <btrfs/volume.h>

NS_ASSUME_NONNULL_BEGIN

/* What the adapter reads through: FSKit's block-device resource, or an image
 * in component tests. */
@protocol BtrfsBlockReader <NSObject>
@property(readonly) uint64_t blockSize;
@property(readonly) uint64_t blockCount;
@property(readonly) uint64_t physicalBlockSize;
@property(readonly, getter=isRevoked) BOOL revoked;
- (size_t)readInto:(void *)buffer
	startingAt:(off_t)offset
	    length:(size_t)length
	     error:(NSError **)error;
@end

/* A device the writer may change: FSKit's resource writes straight to it. */
@protocol BtrfsBlockWriter <BtrfsBlockReader>
@property(readonly, getter=isWritable) BOOL writable;
- (size_t)writeFrom:(void *)buffer
	 startingAt:(off_t)offset
	     length:(size_t)length
	      error:(NSError **)error;
@end

@interface FSBlockDeviceResource (BtrfsBlockReader) <BtrfsBlockWriter>
@end

/* Makes every completed device write durable: the cache flush each of the
 * writer's barriers needs. FSKit's resource offers none, so a privileged
 * service provides it (BtrfsDeviceBarrier); tests use stand-ins. */
@protocol BtrfsDeviceFlusher <NSObject>
- (BOOL)synchronizeWithError:(NSError **)error;
@end

@interface BtrfsFileSystem ()
/* The device barrier a load binds; nil, and so a read-only volume, for a
 * mount with --rdonly, a read-only device or an unavailable service. */
- (nullable id<BtrfsDeviceFlusher>)flusherForDevice:(FSBlockDeviceResource *)device
					    options:(FSTaskOptions *)options;
/* Binds the privileged service to device (BtrfsDeviceBarrier). */
- (nullable id<BtrfsDeviceFlusher>)barrierForDevice:(FSBlockDeviceResource *)device
					      error:(NSError **)error;
@end

/* The FSKit name of the filesystem type, without an underscore: Disk
 * Arbitration appends "_fskit" and cuts names at their first underscore. */
#define BTRFS_FSKIT_TYPE_NAME @"machlinbtrfs"
/* The preferred transfer size, distinct from the sector size: it amortizes the
 * crossing into the extension over several sectors. */
#define BTRFS_FSKIT_IO_SIZE (128U * 1024U)
/* The running transaction commits at least this often. */
#define BTRFS_FSKIT_COMMIT_SECONDS 5U
/* Tree nodes one namespace or attribute operation may change, and the data
 * bytes one more node covers (btrfs_volume_join). */
#define BTRFS_FSKIT_OPERATION_NODES 64U
#define BTRFS_FSKIT_DATA_BYTES_PER_NODE (64U * 1024U)

struct btrfs_fskit_cache;
struct btrfs_fskit_locks;

/* A node cache for one volume; NULL when it cannot be set up. */
struct btrfs_fskit_cache *_Nullable btrfs_fskit_cache_create(void);
void btrfs_fskit_cache_destroy(struct btrfs_fskit_cache *_Nullable owner);
NSError *_Nullable btrfs_fskit_error(enum btrfs_result result);
/* Linux inode flags with a Darwin equivalent, as the XNU adapter reports them. */
uint32_t btrfs_fskit_flags(uint64_t flags);
struct btrfs_time btrfs_fskit_now(void);

@interface BtrfsItem : FSItem {
      @public
	/* The inode as of the item's last read or change (itemLock). */
	struct btrfs_inode inode;
	/* The item number of the directory this item was last reached through:
	 * a file's parent, as Darwin's vnode parent; 0 until known. */
	uint64_t holder;
	/* The generation that publishes the item's last change. */
	uint64_t pending;
	/* Open modes still in use, and whether its last name went while open. */
	FSVolumeOpenModes openModes;
	BOOL orphan;
	/* A directory's enumeration verifier; it changes with its entries. */
	FSDirectoryVerifier version;
}
@end

@interface BtrfsVolume : FSVolume <FSVolumeOperations, FSVolumeReadWriteOperations,
			     FSVolumeXattrOperations, FSVolumeOpenCloseOperations> {
      @package
	struct btrfs_volume *_volume;
	struct btrfs_fskit_cache *_cache;
	struct btrfs_fskit_locks *_locks;
	id<BtrfsBlockReader> _reader;
	id<BtrfsDeviceFlusher> _Nullable _flusher;
	NSLock *_itemLock;
	NSMapTable<NSNumber *, BtrfsItem *> *_items;
	struct btrfs_identity_table *_identities;
	struct btrfs_object_id _rootIdentity;
	FSDirectoryVerifier _versions;
	dispatch_queue_t _commitQueue;
	dispatch_source_t _Nullable _committer;
	BOOL _writable;
}
/* Mounts the filesystem read through reader. With a flusher and a writable
 * reader the volume accepts changes; otherwise it is read-only. Owns cache
 * once it returns a volume; on failure *result says why. */
+ (nullable instancetype)volumeWithReader:(id<BtrfsBlockReader>)reader
				  flusher:(nullable id<BtrfsDeviceFlusher>)flusher
				    cache:(struct btrfs_fskit_cache *_Nullable)cache
				   result:(enum btrfs_result *)result;
@property(readonly, getter=isWritable) BOOL writable;
- (nullable NSError *)checkMountEligibility;
/* Stops the committer and commits what remains; the volume then refuses
 * changes. */
- (enum btrfs_result)shutdown;

/* For the adapter's files. */
- (nullable NSNumber *)numberForIdentity:(struct btrfs_object_id)identity;
- (nullable BtrfsItem *)itemForInode:(const struct btrfs_inode *)inode;
/* Records that item was reached through directory. */
- (void)setHolder:(BtrfsItem *)directory ofItem:(BtrfsItem *)item;
/* Every standard attribute FSKit can ask for. parent is the item number of
 * the directory holding the name; 0 resolves it (setting *result on failure). */
- (nullable FSItemAttributes *)attributesForInode:(const struct btrfs_inode *)inode
					   parent:(uint64_t)parent
					   result:(enum btrfs_result *)result;
/* The attributes of item's current inode. */
- (nullable FSItemAttributes *)attributesForItem:(BtrfsItem *)item
					  result:(enum btrfs_result *)result;
/* Rereads item's inode from the newest state. */
- (enum btrfs_result)refreshItem:(BtrfsItem *)item;
/* Marks a directory's entries changed. */
- (void)changedDirectory:(BtrfsItem *)directory;
/* Runs one change in the running transaction; first and second (either may be
 * nil) learn the generation that publishes it. */
- (enum btrfs_result)changeWithNodes:(size_t)nodes
			       first:(nullable BtrfsItem *)first
			      second:(nullable BtrfsItem *)second
			   operation:(enum btrfs_result (^)(struct btrfs_transaction *))operation;
@end

/* The volume's changes (BtrfsVolumeMutation.m); the protocol methods of the
 * same names without "perform" forward to them. */
@interface BtrfsVolume (Mutation)
- (void)performCreateItemNamed:(FSFileName *)name
			  type:(FSItemType)type
		   inDirectory:(FSItem *)directory
		    attributes:(FSItemSetAttributesRequest *)newAttributes
		  replyHandler:
		      (void (^)(FSItem *_Nullable, FSFileName *_Nullable, NSError *_Nullable))reply;
- (void)performCreateSymbolicLinkNamed:(FSFileName *)name
			   inDirectory:(FSItem *)directory
			    attributes:(FSItemSetAttributesRequest *)newAttributes
			  linkContents:(FSFileName *)contents
			  replyHandler:(void (^)(FSItem *_Nullable, FSFileName *_Nullable,
					   NSError *_Nullable))reply;
- (void)performCreateLinkToItem:(FSItem *)item
			  named:(FSFileName *)name
		    inDirectory:(FSItem *)directory
		   replyHandler:(void (^)(FSFileName *_Nullable, NSError *_Nullable))reply;
- (void)performRemoveItem:(FSItem *)item
		    named:(FSFileName *)name
	    fromDirectory:(FSItem *)directory
	     replyHandler:(void (^)(NSError *_Nullable))reply;
- (void)performRenameItem:(FSItem *)item
	      inDirectory:(FSItem *)sourceDirectory
		    named:(FSFileName *)sourceName
		toNewName:(FSFileName *)destinationName
	      inDirectory:(FSItem *)destinationDirectory
		 overItem:(nullable FSItem *)overItem
	     replyHandler:(void (^)(FSFileName *_Nullable, NSError *_Nullable))reply;
- (void)performSetAttributes:(FSItemSetAttributesRequest *)newAttributes
		      onItem:(FSItem *)item
		replyHandler:(void (^)(FSItemAttributes *_Nullable, NSError *_Nullable))reply;
- (void)performWriteContents:(NSData *)contents
		      toFile:(FSItem *)item
		    atOffset:(off_t)offset
		replyHandler:(void (^)(size_t, NSError *_Nullable))reply;
- (void)performSetXattrNamed:(FSFileName *)name
		      toData:(nullable NSData *)value
		      onItem:(FSItem *)item
		      policy:(FSSetXattrPolicy)policy
		replyHandler:(void (^)(NSError *_Nullable))reply;
@end

NS_ASSUME_NONNULL_END
