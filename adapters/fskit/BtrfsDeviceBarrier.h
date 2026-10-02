/* SPDX-License-Identifier: BSD-3-Clause */
/* The device barrier: a privileged service that synchronizes one validated
 * block device's cache, the flush FSKit's resource does not offer. */
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

#define BTRFS_BARRIER_SERVICE @"group.org.machlin.btrfs.device-barrier"
#define BTRFS_BARRIER_IDENTIFIER @"org.machlin.btrfs.device-barrier"
#define BTRFS_EXTENSION_IDENTIFIER @"org.machlin.btrfs.filesystem"
#define BTRFS_APP_IDENTIFIER @"org.machlin.btrfs"
/* A lost or wedged service fails a barrier after this long. */
#define BTRFS_BARRIER_TIMEOUT_SECONDS 10

/* No raw data, file descriptors or general-purpose ioctls cross this interface. */
@protocol BtrfsDeviceBarrierProtocol
- (void)checkServiceWithReply:(void (^)(NSError *_Nullable))reply;
- (void)openDevice:(NSString *)name
	 blockSize:(uint64_t)blockSize
	blockCount:(uint64_t)blockCount
	     reply:(void (^)(NSError *_Nullable))reply;
- (void)synchronizeWithReply:(void (^)(NSError *_Nullable))reply;
@end

/* The code-signing requirement for a peer with identifier, signed by this
 * process's own team; nil when this process has no team. */
NSString *_Nullable btrfs_peer_requirement(NSString *identifier);

@interface BtrfsDeviceBarrier : NSObject
+ (BOOL)isServiceAvailable;
/* Binds the service to the named device after it checks its geometry and
 * synchronizes it once. */
- (nullable instancetype)initWithDevice:(NSString *)name
			      blockSize:(uint64_t)blockSize
			     blockCount:(uint64_t)blockCount
				  error:(NSError **)error;
- (BOOL)synchronizeWithError:(NSError **)error;
@end

NS_ASSUME_NONNULL_END
