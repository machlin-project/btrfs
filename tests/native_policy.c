/* SPDX-License-Identifier: BSD-3-Clause */
#include <btrfs/native.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

int
main(void)
{
	char names[] = "user.one\0trusted.secret\0security.capability\0user.two";
	char expected[] = "user.one\0user.two";
	size_t length;

	assert(btrfs_native_xattr_visible("user.binary", 11));
	assert(!btrfs_native_xattr_visible("trusted.secret", 14));
	assert(!btrfs_native_xattr_visible("security.capability", 19));
	assert(!btrfs_native_xattr_visible("user.", 5));
	assert(btrfs_native_filter_xattrs(names, sizeof(names), &length) == BTRFS_OK);
	assert(length == sizeof(expected) && memcmp(names, expected, length) == 0);
	assert(btrfs_native_filter_xattrs(names, 3, &length) == BTRFS_CORRUPT);
	assert(btrfs_native_filter_xattrs(NULL, 0, &length) == BTRFS_OK && length == 0);
	puts("native user xattrs, protected namespace filtering and malformed lists: PASS");
	return 0;
}
