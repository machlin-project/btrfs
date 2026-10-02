/* SPDX-License-Identifier: BSD-3-Clause */
#import "BtrfsDeviceBarrier.h"
#include <errno.h>

@implementation BtrfsDeviceBarrier {
	NSXPCConnection *_connection;
}

/* Runs one request; a lost or wedged service fails it, never acknowledges it. */
- (BOOL)perform:(void (^)(id<BtrfsDeviceBarrierProtocol>, void (^)(NSError *)))operation
	  error:(NSError **)error
{
	dispatch_semaphore_t completed = dispatch_semaphore_create(0);
	NSObject *gate = [[NSObject alloc] init];
	__block NSError *failure = nil;
	__block BOOL finished = NO;
	id<BtrfsDeviceBarrierProtocol> proxy;
	void (^finish)(NSError *);

	finish = ^(NSError *value) {
	  @synchronized(gate) {
		  if (!finished) {
			  finished = YES;
			  failure = value;
			  dispatch_semaphore_signal(completed);
		  }
	  }
	};
	proxy = [_connection remoteObjectProxyWithErrorHandler:finish];
	operation(proxy, finish);
	if (dispatch_semaphore_wait(completed,
		dispatch_time(DISPATCH_TIME_NOW,
		    (int64_t)BTRFS_BARRIER_TIMEOUT_SECONDS * NSEC_PER_SEC)) != 0) {
		@synchronized(gate) {
			finished = YES;
		}
		[_connection invalidate];
		if (error != NULL) {
			*error = [NSError errorWithDomain:NSPOSIXErrorDomain
						     code:ETIMEDOUT
						 userInfo:nil];
		}
		return NO;
	}
	@synchronized(gate) {
		if (error != NULL) {
			*error = failure;
		}
		return failure == nil;
	}
}

- (instancetype)initWithError:(NSError **)error
{
	NSString *requirement = btrfs_peer_requirement(BTRFS_BARRIER_IDENTIFIER);

	if (requirement == nil) {
		if (error != NULL) {
			*error = [NSError errorWithDomain:NSPOSIXErrorDomain
						     code:EACCES
						 userInfo:nil];
		}
		return nil;
	}
	self = [super init];
	if (self != nil) {
		_connection =
		    [[NSXPCConnection alloc] initWithMachServiceName:BTRFS_BARRIER_SERVICE
							     options:NSXPCConnectionPrivileged];
		_connection.remoteObjectInterface =
		    [NSXPCInterface interfaceWithProtocol:@protocol(BtrfsDeviceBarrierProtocol)];
		[_connection setCodeSigningRequirement:requirement];
		[_connection activate];
	}
	return self;
}

+ (BOOL)isServiceAvailable
{
	BtrfsDeviceBarrier *service = [[self alloc] initWithError:NULL];

	return service != nil &&
	    [service
		perform:^(id<BtrfsDeviceBarrierProtocol> proxy, void (^finish)(NSError *)) {
		  [proxy checkServiceWithReply:finish];
		}
		  error:NULL];
}

- (instancetype)initWithDevice:(NSString *)name
		     blockSize:(uint64_t)blockSize
		    blockCount:(uint64_t)blockCount
			 error:(NSError **)error
{
	self = [self initWithError:error];
	if (self != nil &&
	    ![self
		perform:^(id<BtrfsDeviceBarrierProtocol> proxy, void (^finish)(NSError *)) {
		  [proxy openDevice:name blockSize:blockSize blockCount:blockCount reply:finish];
		}
		  error:error]) {
		return nil;
	}
	return self;
}

- (BOOL)synchronizeWithError:(NSError **)error
{
	return [self
	    perform:^(id<BtrfsDeviceBarrierProtocol> proxy, void (^finish)(NSError *)) {
	      [proxy synchronizeWithReply:finish];
	    }
	      error:error];
}

- (void)dealloc
{
	[_connection invalidate];
}

@end
