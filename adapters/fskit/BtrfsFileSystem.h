/* SPDX-License-Identifier: BSD-3-Clause */
#import <FSKit/FSKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface BtrfsFileSystem
    : FSUnaryFileSystem <FSUnaryFileSystemOperations, FSManageableResourceMaintenanceOperations>
@end

NS_ASSUME_NONNULL_END
