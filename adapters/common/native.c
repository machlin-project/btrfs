/* SPDX-License-Identifier: BSD-3-Clause */
#include <btrfs/native.h>

int
btrfs_native_xattr_visible(const void *name, size_t length)
{
	const uint8_t *bytes = name;
	static const uint8_t prefix[] = "user.";
	size_t i;

	if (bytes == NULL || length <= sizeof(prefix) - 1 || length > BTRFS_NATIVE_XATTR_NAME_MAX) {
		return 0;
	}
	for (i = 0; i < sizeof(prefix) - 1; i++) {
		if (bytes[i] != prefix[i]) {
			return 0;
		}
	}
	return 1;
}

enum btrfs_result
btrfs_native_filter_xattrs(void *buffer, size_t length, size_t *filtered)
{
	uint8_t *bytes = buffer;
	size_t offset = 0;
	size_t end;
	size_t result = 0;
	size_t i;

	if (filtered == NULL || (buffer == NULL && length != 0)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	*filtered = 0;
	while (offset < length) {
		end = offset;
		while (end < length && bytes[end] != 0) {
			end++;
		}
		if (end == length || end == offset) {
			return BTRFS_CORRUPT;
		}
		if (btrfs_native_xattr_visible(bytes + offset, end - offset)) {
			for (i = offset; i <= end; i++) {
				bytes[result++] = bytes[i];
			}
		}
		offset = end + 1;
	}
	*filtered = result;
	return BTRFS_OK;
}

enum btrfs_result
btrfs_native_inode_supported(const struct btrfs_fs *fs, const struct btrfs_inode *inode)
{
	static const char access_acl[] = "system.posix_acl_access";
	size_t length;
	enum btrfs_result result;

	/* Until ACL evaluation is integrated, never silently substitute mode bits. */
	result = btrfs_get_xattr(fs, inode, access_acl, sizeof(access_acl) - 1, NULL, 0, &length);
	return result == BTRFS_NOT_FOUND ? BTRFS_OK
					 : (result == BTRFS_OK ? BTRFS_UNSUPPORTED : result);
}
