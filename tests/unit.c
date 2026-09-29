/* SPDX-License-Identifier: BSD-3-Clause */
#include "internal.h"
#include <assert.h>
#include <stdio.h>

static void
xattr_name_contract(void)
{
	struct {
		struct bt_disk_dir header;
		uint8_t name[6];
	} payload = { .header = { .name_length = { { 6, 0 } }, .type = BTRFS_FT_XATTR },
		.name = { 'u', 's', 'e', 'r', '/', 'a' } };
	struct bt_record record = { .key = { .type = BT_XATTR_ITEM },
		.data = (const void *)&payload,
		.size = sizeof(payload) };
	const struct bt_disk_dir *header;
	const uint8_t *name;
	const uint8_t *data;
	size_t offset = 0;

	/* Xattr names are namespace keys, not pathname components. */
	record.key.offset = bt_crc32c(UINT32_MAX - 1U, payload.name, sizeof(payload.name));
	assert(bt_dir_record(&record, &offset, &header, &name, &data) == BTRFS_OK);
	assert(offset == sizeof(payload) && bt_equal(name, payload.name, sizeof(payload.name)));
	record.key.type = BT_DIR_ITEM;
	payload.header.type = BTRFS_FT_REGULAR;
	offset = 0;
	assert(bt_dir_record(&record, &offset, &header, &name, &data) == BTRFS_CORRUPT);
	assert(bt_xattr_name_valid("user/a", 6));
	assert(!bt_xattr_name_valid("user\0a", 6));
}

int
main(void)
{
	struct btrfs_fs *fs = (void *)(uintptr_t)1;
	struct bt_key low = { .objectid = 1, .type = BT_DIR_ITEM, .offset = UINT64_MAX };
	struct bt_key high = { .objectid = 2, .type = BT_INODE_ITEM, .offset = 0 };
	struct bt_le64 wire = { { 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01 } };
	size_t completed = 999;

	assert(~bt_crc32c(UINT32_MAX, "123456789", 9) == UINT32_C(0xe3069283));
	assert(bt_u64(wire) == UINT64_C(0x0123456789abcdef));
	assert(bt_key_compare(low, high) < 0);
	assert(bt_key_compare(high, low) > 0);
	assert(bt_key_compare(high, high) == 0);
	assert(btrfs_mount(NULL, 0, &fs) == BTRFS_INVALID_ARGUMENT && fs == NULL);
	assert(btrfs_read(NULL, NULL, 0, NULL, 0, &completed) == BTRFS_INVALID_ARGUMENT);
	assert(completed == 0);
	assert(!bt_name_valid("a/b", 3));
	assert(!bt_name_valid("a\0b", 3));
	assert(bt_name_valid("\xff", 1));
	xattr_name_contract();
	btrfs_unmount(NULL);
	puts("wire encoding, CRC32C known vector, key order, names and invalid API inputs: PASS");
	return 0;
}
