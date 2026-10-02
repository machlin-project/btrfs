/* SPDX-License-Identifier: BSD-3-Clause */
/* The adapter's volume and its block source, shared with component tests. */
#import "BtrfsFileSystem.h"
#include <btrfs/btrfs.h>

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

@interface FSBlockDeviceResource (BtrfsBlockReader) <BtrfsBlockReader>
@end

/* The FSKit name of the filesystem type, without an underscore: Disk
 * Arbitration appends "_fskit" and cuts names at their first underscore. */
#define BTRFS_FSKIT_TYPE_NAME @"machlinbtrfs"
/* The preferred transfer size, distinct from the sector size: it amortizes the
 * crossing into the extension over several sectors. */
#define BTRFS_FSKIT_IO_SIZE (128U * 1024U)

struct btrfs_fskit_cache;

/* A node cache for one volume; NULL when it cannot be set up. */
struct btrfs_fskit_cache *_Nullable btrfs_fskit_cache_create(void);
void btrfs_fskit_cache_destroy(struct btrfs_fskit_cache *_Nullable owner);
/* Mounts the filesystem read through reader, with cache (may be NULL). */
enum btrfs_result btrfs_fskit_open(id<BtrfsBlockReader> reader,
    struct btrfs_fskit_cache *_Nullable cache, struct btrfs_fs *_Nullable *_Nonnull fs);
NSError *_Nullable btrfs_fskit_error(enum btrfs_result result);

@interface BtrfsItem : FSItem {
      @public
	struct btrfs_inode inode;
}
@end

@interface BtrfsVolume : FSVolume <FSVolumeOperations, FSVolumeReadWriteOperations,
			     FSVolumeXattrOperations, FSVolumeOpenCloseOperations>
/* Owns fs and cache once it returns a volume; reader stays referenced. */
- (nullable instancetype)initWithReader:(id<BtrfsBlockReader>)reader
			     filesystem:(struct btrfs_fs *)fs
				  cache:(struct btrfs_fskit_cache *_Nullable)cache;
- (nullable NSError *)checkMountEligibility;
@end

NS_ASSUME_NONNULL_END
