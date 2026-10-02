/* SPDX-License-Identifier: BSD-3-Clause */
/* Changes through the volume's running transaction, each one operation that
 * either applies completely or is refused before its first change. The 26.x
 * callbacks carry no caller credentials, so data and owner changes drop set-id
 * bits and the file capability, as a writer without CAP_FSETID does on Linux;
 * the extension's own identity is never taken for the caller's privilege. */
#import "BtrfsFileSystemInternal.h"
#include <btrfs/native.h>
#include <btrfs/write.h>

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

/* A directory with a default POSIX ACL would pass Linux's ACL inheritance to
 * new entries, which the native policy does not translate. */
static const char btrfs_fskit_default_acl[] = "system.posix_acl_default";

static struct btrfs_time
btrfs_fskit_time(struct timespec value)
{
	return (struct btrfs_time){ value.tv_sec, (uint32_t)value.tv_nsec };
}

/* Attributes FSKit asked to set. */
static FSItemAttribute
btrfs_fskit_supplied(FSItemSetAttributesRequest *request)
{
	FSItemAttribute result = 0;
	FSItemAttribute attribute;

	for (attribute = FSItemAttributeType; attribute <= FSItemAttributeInhibitKernelOffloadedIO;
	    attribute <<= 1) {
		if ([request isValid:attribute]) {
			result |= attribute;
		}
	}
	return result;
}

@implementation BtrfsVolume (Mutation)

/* The item for an inode the running transaction just created or changed. */
- (BtrfsItem *)itemForIdentity:(struct btrfs_object_id)identity result:(enum btrfs_result *)result
{
	struct btrfs_volume_view *view;
	struct btrfs_inode inode;
	const struct btrfs_fs *fs;
	BtrfsItem *item = nil;

	fs = btrfs_volume_read(_volume, &view);
	*result = btrfs_get_inode(fs, identity, &inode);
	btrfs_volume_unread(_volume, view);
	if (*result == BTRFS_OK) {
		item = [self itemForInode:&inode];
		*result = item == nil ? BTRFS_NO_MEMORY : BTRFS_OK;
	}
	return item;
}

- (struct btrfs_object_id)identityOf:(BtrfsItem *)item
{
	struct btrfs_object_id identity;

	[_itemLock lock];
	identity = item->inode.id;
	[_itemLock unlock];
	return identity;
}

/* After a name went away: an inode without names is an orphan while open and
 * gone otherwise; the item keeps its last record either way. */
- (void)nameRemovedFrom:(BtrfsItem *)item
{
	enum btrfs_result error = [self refreshItem:item];

	[_itemLock lock];
	if (error == BTRFS_OK && item->inode.links == 0 && item->openModes != 0) {
		item->orphan = YES;
	}
	[_itemLock unlock];
}

- (enum btrfs_result)checkParent:(BtrfsItem *)directory
{
	struct btrfs_volume_view *view;
	struct btrfs_inode inode;
	const struct btrfs_fs *fs;
	size_t length = 0;
	enum btrfs_result error;

	[_itemLock lock];
	inode = directory->inode;
	[_itemLock unlock];
	fs = btrfs_volume_read(_volume, &view);
	error = btrfs_get_inode(fs, inode.id, &inode);
	if (error == BTRFS_OK) {
		error = btrfs_get_xattr(fs, &inode, btrfs_fskit_default_acl,
		    sizeof(btrfs_fskit_default_acl) - 1, NULL, 0, &length);
		error = error == BTRFS_NOT_FOUND ? BTRFS_OK
		    : error == BTRFS_OK		 ? BTRFS_UNSUPPORTED
						 : error;
	}
	btrfs_volume_unread(_volume, view);
	return error;
}

- (void)createNamed:(FSFileName *)name
	       mode:(uint32_t)type
	  directory:(FSItem *)directory
	 attributes:(FSItemSetAttributesRequest *)attributes
	     target:(FSFileName *)target
	      reply:(void (^)(FSItem *, FSFileName *, NSError *))reply
{
	BtrfsItem *parent = (BtrfsItem *)directory;
	BtrfsItem *item = nil;
	FSItemAttribute supplied = btrfs_fskit_supplied(attributes);
	FSItemAttribute owner = FSItemAttributeMode | FSItemAttributeUID | FSItemAttributeGID;
	struct btrfs_object_id within = [self identityOf:parent];
	struct btrfs_new_inode values;
	NSData *bytes = name.data;
	NSData *link = target.data;
	__block struct btrfs_object_id created = { 0, 0 };
	enum btrfs_result error = _writable ? BTRFS_OK : BTRFS_READ_ONLY;

	/* The kernel supplies the caller's mode and owner; nothing else is
	 * applied at creation. */
	if (error == BTRFS_OK && (supplied & owner) != owner) {
		error = BTRFS_INVALID_ARGUMENT;
	}
	if (error == BTRFS_OK) {
		error = [self checkParent:parent];
	}
	if (error == BTRFS_OK) {
		memset(&values, 0, sizeof(values));
		values.mode = type | (attributes.mode & ALLPERMS);
		values.uid = attributes.uid;
		values.gid = attributes.gid;
		values.time = btrfs_fskit_now();
		if (link != nil) {
			values.target = link.bytes;
			values.target_length = link.length;
		}
		error = [self
		    changeWithNodes:BTRFS_FSKIT_OPERATION_NODES
			      first:parent
			     second:nil
			  operation:^enum btrfs_result(struct btrfs_transaction *transaction) {
			    return btrfs_transaction_create(
				transaction, within, bytes.bytes, bytes.length, &values, &created);
			  }];
	}
	if (error == BTRFS_OK) {
		[self changedDirectory:parent];
		item = [self itemForIdentity:created result:&error];
	}
	if (item != nil) {
		attributes.consumedAttributes = supplied & owner;
		[_itemLock lock];
		item->pending = parent->pending;
		[_itemLock unlock];
	}
	reply(item, item != nil ? name : nil, btrfs_fskit_error(error));
}

- (void)performCreateItemNamed:(FSFileName *)name
			  type:(FSItemType)type
		   inDirectory:(FSItem *)directory
		    attributes:(FSItemSetAttributesRequest *)newAttributes
		  replyHandler:(void (^)(FSItem *, FSFileName *, NSError *))reply
{
	uint32_t mode;

	switch (type) {
	case FSItemTypeFile:
		mode = BTRFS_MODE_REGULAR;
		break;
	case FSItemTypeDirectory:
		mode = BTRFS_MODE_DIRECTORY;
		break;
	case FSItemTypeFIFO:
		mode = BTRFS_MODE_FIFO;
		break;
	case FSItemTypeSocket:
		mode = BTRFS_MODE_SOCKET;
		break;
	default:
		/* Device numbers do not reach this callback. */
		reply(nil, nil, btrfs_fskit_error(BTRFS_UNSUPPORTED));
		return;
	}
	[self createNamed:name
		     mode:mode
		directory:directory
	       attributes:newAttributes
		   target:nil
		    reply:reply];
}

- (void)performCreateSymbolicLinkNamed:(FSFileName *)name
			   inDirectory:(FSItem *)directory
			    attributes:(FSItemSetAttributesRequest *)newAttributes
			  linkContents:(FSFileName *)contents
			  replyHandler:(void (^)(FSItem *, FSFileName *, NSError *))reply
{
	[self createNamed:name
		     mode:BTRFS_MODE_SYMLINK
		directory:directory
	       attributes:newAttributes
		   target:contents
		    reply:reply];
}

- (void)performCreateLinkToItem:(FSItem *)item
			  named:(FSFileName *)name
		    inDirectory:(FSItem *)directory
		   replyHandler:(void (^)(FSFileName *, NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;
	BtrfsItem *parent = (BtrfsItem *)directory;
	struct btrfs_object_id identity = [self identityOf:owned];
	struct btrfs_object_id within = [self identityOf:parent];
	struct btrfs_time now = btrfs_fskit_now();
	NSData *bytes = name.data;
	enum btrfs_result error;

	error = [self changeWithNodes:BTRFS_FSKIT_OPERATION_NODES
				first:owned
			       second:parent
			    operation:^enum btrfs_result(struct btrfs_transaction *transaction) {
			      return btrfs_transaction_link(
				  transaction, identity, within, bytes.bytes, bytes.length, now);
			    }];
	if (error == BTRFS_OK) {
		[self changedDirectory:parent];
		(void)[self refreshItem:owned];
	}
	reply(error == BTRFS_OK ? name : nil, btrfs_fskit_error(error));
}

- (void)performRemoveItem:(FSItem *)item
		    named:(FSFileName *)name
	    fromDirectory:(FSItem *)directory
	     replyHandler:(void (^)(NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;
	BtrfsItem *parent = (BtrfsItem *)directory;
	struct btrfs_object_id within = [self identityOf:parent];
	struct btrfs_time now = btrfs_fskit_now();
	NSData *bytes = name.data;
	int open;
	enum btrfs_result error;

	[_itemLock lock];
	open = owned->openModes != 0;
	[_itemLock unlock];
	error = [self changeWithNodes:BTRFS_FSKIT_OPERATION_NODES
				first:parent
			       second:owned
			    operation:^enum btrfs_result(struct btrfs_transaction *transaction) {
			      return btrfs_transaction_unlink(
				  transaction, within, bytes.bytes, bytes.length, now, open);
			    }];
	if (error == BTRFS_OK) {
		[self changedDirectory:parent];
		[self nameRemovedFrom:owned];
	}
	reply(btrfs_fskit_error(error));
}

- (void)performRenameItem:(FSItem *)item
	      inDirectory:(FSItem *)sourceDirectory
		    named:(FSFileName *)sourceName
		toNewName:(FSFileName *)destinationName
	      inDirectory:(FSItem *)destinationDirectory
		 overItem:(FSItem *)overItem
	     replyHandler:(void (^)(FSFileName *, NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;
	BtrfsItem *source = (BtrfsItem *)sourceDirectory;
	BtrfsItem *destination = (BtrfsItem *)destinationDirectory;
	BtrfsItem *over = (BtrfsItem *)overItem;
	struct btrfs_object_id from = [self identityOf:source];
	struct btrfs_object_id to = [self identityOf:destination];
	struct btrfs_time now = btrfs_fskit_now();
	NSData *oldBytes = sourceName.data;
	NSData *newBytes = destinationName.data;
	int open = 0;
	enum btrfs_result error;

	if (over != nil) {
		[_itemLock lock];
		open = over->openModes != 0;
		[_itemLock unlock];
	}
	error = [self changeWithNodes:BTRFS_FSKIT_OPERATION_NODES
				first:source
			       second:destination
			    operation:^enum btrfs_result(struct btrfs_transaction *transaction) {
			      return btrfs_transaction_rename(transaction, from, oldBytes.bytes,
				  oldBytes.length, to, newBytes.bytes, newBytes.length, now, open);
			    }];
	if (error == BTRFS_OK) {
		[self changedDirectory:source];
		if (destination != source) {
			[self changedDirectory:destination];
		}
		[_itemLock lock];
		owned->pending = source->pending;
		[_itemLock unlock];
		(void)[self refreshItem:owned];
		if (over != nil && over != owned) {
			[self nameRemovedFrom:over];
		}
	}
	reply(error == BTRFS_OK ? destinationName : nil, btrfs_fskit_error(error));
}

- (void)performSetAttributes:(FSItemSetAttributesRequest *)newAttributes
		      onItem:(FSItem *)item
		replyHandler:(void (^)(FSItemAttributes *, NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;
	FSItemAttribute supplied = btrfs_fskit_supplied(newAttributes);
	FSItemAttribute handled = FSItemAttributeMode | FSItemAttributeUID | FSItemAttributeGID |
	    FSItemAttributeAccessTime | FSItemAttributeModifyTime | FSItemAttributeChangeTime;
	FSItemAttributes *attributes = nil;
	struct btrfs_attributes changes;
	struct btrfs_inode inode;
	struct btrfs_object_id identity;
	uint64_t size = newAttributes.size;
	BOOL truncate;
	enum btrfs_result error = _writable ? BTRFS_OK : BTRFS_READ_ONLY;

	if (error == BTRFS_OK &&
	    (supplied &
		(FSItemAttributeType | FSItemAttributeLinkCount | FSItemAttributeAllocSize |
		    FSItemAttributeFileID | FSItemAttributeParentID)) != 0) {
		error = BTRFS_INVALID_ARGUMENT;
	}
	if (error == BTRFS_OK) {
		error = [self refreshItem:owned];
	}
	if (error != BTRFS_OK) {
		reply(nil, btrfs_fskit_error(error));
		return;
	}
	[_itemLock lock];
	inode = owned->inode;
	[_itemLock unlock];
	identity = inode.id;
	/* Inode flags have no Darwin mapping beyond the absence of any. */
	if ((supplied & FSItemAttributeFlags) != 0 && newAttributes.flags != 0) {
		reply(nil, btrfs_fskit_error(BTRFS_UNSUPPORTED));
		return;
	}
	handled |= supplied & FSItemAttributeFlags;
	/* FSKit ignores a size for anything but a regular file. */
	truncate = (supplied & FSItemAttributeSize) != 0 &&
	    (inode.mode & BTRFS_MODE_TYPE) == BTRFS_MODE_REGULAR;
	if (truncate) {
		handled |= FSItemAttributeSize;
	}
	memset(&changes, 0, sizeof(changes));
	changes.mode = inode.mode & ALLPERMS;
	changes.time = (supplied & FSItemAttributeChangeTime) != 0
	    ? btrfs_fskit_time(newAttributes.changeTime)
	    : btrfs_fskit_now();
	if ((supplied & FSItemAttributeMode) != 0) {
		changes.mask |= BTRFS_ATTRIBUTE_MODE;
		changes.mode = newAttributes.mode & ALLPERMS;
	}
	if ((supplied & FSItemAttributeUID) != 0 && newAttributes.uid != inode.uid) {
		changes.mask |= BTRFS_ATTRIBUTE_UID;
		changes.uid = newAttributes.uid;
	}
	if ((supplied & FSItemAttributeGID) != 0 && newAttributes.gid != inode.gid) {
		changes.mask |= BTRFS_ATTRIBUTE_GID;
		changes.gid = newAttributes.gid;
	}
	/* Without the caller's credentials a change of owner is taken as an
	 * unprivileged one: set-id bits and the capability go with it. */
	if ((changes.mask & (BTRFS_ATTRIBUTE_UID | BTRFS_ATTRIBUTE_GID)) != 0) {
		changes.mask |= BTRFS_ATTRIBUTE_MODE | BTRFS_ATTRIBUTE_REMOVE_CAPABILITY;
		changes.mode &= ~(uint32_t)(S_ISUID | S_ISGID);
	}
	if ((supplied & FSItemAttributeAccessTime) != 0) {
		changes.mask |= BTRFS_ATTRIBUTE_ACCESS_TIME;
		changes.access_time = btrfs_fskit_time(newAttributes.accessTime);
	}
	if ((supplied & FSItemAttributeModifyTime) != 0) {
		changes.mask |= BTRFS_ATTRIBUTE_MODIFY_TIME;
		changes.modify_time = btrfs_fskit_time(newAttributes.modifyTime);
	}
	if (truncate || changes.mask != 0 || (supplied & FSItemAttributeChangeTime) != 0) {
		error = [self
		    changeWithNodes:BTRFS_FSKIT_OPERATION_NODES
			      first:owned
			     second:nil
			  operation:^enum btrfs_result(struct btrfs_transaction *transaction) {
			    enum btrfs_result result = BTRFS_OK;

			    if (truncate) {
				    result = btrfs_transaction_drop_privileges(
					transaction, identity, changes.time);
			    }
			    if (result == BTRFS_OK && truncate) {
				    result = btrfs_transaction_truncate(
					transaction, identity, size, changes.time);
			    }
			    if (result == BTRFS_OK &&
				(changes.mask != 0 ||
				    (supplied & FSItemAttributeChangeTime) != 0)) {
				    result = btrfs_transaction_set_attributes(
					transaction, identity, &changes);
			    }
			    return result;
			  }];
	}
	if (error == BTRFS_OK) {
		newAttributes.consumedAttributes = supplied & handled;
		error = [self refreshItem:owned];
	}
	if (error == BTRFS_OK) {
		[_itemLock lock];
		inode = owned->inode;
		[_itemLock unlock];
		attributes = [self attributesForInode:&inode];
		error = attributes == nil ? BTRFS_NO_MEMORY : BTRFS_OK;
	}
	reply(attributes, btrfs_fskit_error(error));
}

/* One write callback is one operation: the core refuses it before its first
 * change or applies all of it, so no prefix of a refused request lands. */
- (void)performWriteContents:(NSData *)contents
		      toFile:(FSItem *)item
		    atOffset:(off_t)offset
		replyHandler:(void (^)(size_t, NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;
	struct btrfs_object_id identity = [self identityOf:owned];
	struct btrfs_time now = btrfs_fskit_now();
	size_t length = contents.length;
	enum btrfs_result error = _writable ? BTRFS_OK : BTRFS_READ_ONLY;

	if (error == BTRFS_OK && offset < 0) {
		error = BTRFS_INVALID_ARGUMENT;
	}
	if (error == BTRFS_OK && length != 0) {
		error = [self
		    changeWithNodes:BTRFS_FSKIT_OPERATION_NODES +
		    length / BTRFS_FSKIT_DATA_BYTES_PER_NODE
			      first:owned
			     second:nil
			  operation:^enum btrfs_result(struct btrfs_transaction *transaction) {
			    enum btrfs_result result = btrfs_transaction_write(transaction,
				identity, (uint64_t)offset, contents.bytes, length, now);

			    /* Set-id bits or a capability need the decision a writer
			     * without CAP_FSETID makes; the refusal changed nothing. */
			    if (result == BTRFS_UNSUPPORTED) {
				    result = btrfs_transaction_drop_privileges(
					transaction, identity, now);
				    if (result == BTRFS_OK) {
					    result = btrfs_transaction_write(transaction, identity,
						(uint64_t)offset, contents.bytes, length, now);
				    }
			    }
			    return result;
			  }];
	}
	if (error == BTRFS_OK && length != 0) {
		(void)[self refreshItem:owned];
	}
	reply(error == BTRFS_OK ? length : 0, btrfs_fskit_error(error));
}

/* Only user.* names on regular files and directories, as Linux allows them. */
- (void)performSetXattrNamed:(FSFileName *)name
		      toData:(NSData *)value
		      onItem:(FSItem *)item
		      policy:(FSSetXattrPolicy)policy
		replyHandler:(void (^)(NSError *))reply
{
	BtrfsItem *owned = (BtrfsItem *)item;
	struct btrfs_object_id identity = [self identityOf:owned];
	struct btrfs_time now = btrfs_fskit_now();
	NSData *key = name.data;
	uint32_t type;
	int flags = 0;
	enum btrfs_result error = _writable ? BTRFS_OK : BTRFS_READ_ONLY;

	[_itemLock lock];
	type = owned->inode.mode & BTRFS_MODE_TYPE;
	[_itemLock unlock];
	if (!btrfs_native_xattr_visible(key.bytes, key.length)) {
		reply([NSError errorWithDomain:NSPOSIXErrorDomain code:ENOTSUP userInfo:nil]);
		return;
	}
	if (type != BTRFS_MODE_REGULAR && type != BTRFS_MODE_DIRECTORY) {
		reply([NSError errorWithDomain:NSPOSIXErrorDomain code:EPERM userInfo:nil]);
		return;
	}
	if (error == BTRFS_OK && value.length > BTRFS_NATIVE_XATTR_LIMIT) {
		reply([NSError errorWithDomain:NSPOSIXErrorDomain code:E2BIG userInfo:nil]);
		return;
	}
	if (policy == FSSetXattrPolicyMustCreate) {
		flags = BTRFS_XATTR_CREATE;
	} else if (policy == FSSetXattrPolicyMustReplace) {
		flags = BTRFS_XATTR_REPLACE;
	}
	if (error == BTRFS_OK) {
		error = [self
		    changeWithNodes:BTRFS_FSKIT_OPERATION_NODES
			      first:owned
			     second:nil
			  operation:^enum btrfs_result(struct btrfs_transaction *transaction) {
			    return policy == FSSetXattrPolicyDelete
				? btrfs_transaction_remove_xattr(
				      transaction, identity, key.bytes, key.length, now)
				: btrfs_transaction_set_xattr(transaction, identity, key.bytes,
				      key.length, value.bytes, value.length, flags, now);
			  }];
	}
	if (error == BTRFS_NOT_FOUND) {
		reply([NSError errorWithDomain:NSPOSIXErrorDomain code:ENOATTR userInfo:nil]);
		return;
	}
	reply(btrfs_fskit_error(error));
}

@end
