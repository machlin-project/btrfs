/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef MACHLIN_BTRFS_ENCODE_H
#define MACHLIN_BTRFS_ENCODE_H

#include "internal.h"

static inline void
bt_put16(struct bt_le16 *field, uint16_t value)
{
	unsigned i;

	for (i = 0; i < sizeof(field->bytes); i++) {
		field->bytes[i] = (uint8_t)(value >> (i * 8U));
	}
}

static inline void
bt_put32(struct bt_le32 *field, uint32_t value)
{
	unsigned i;

	for (i = 0; i < sizeof(field->bytes); i++) {
		field->bytes[i] = (uint8_t)(value >> (i * 8U));
	}
}

static inline void
bt_put64(struct bt_le64 *field, uint64_t value)
{
	unsigned i;

	for (i = 0; i < sizeof(field->bytes); i++) {
		field->bytes[i] = (uint8_t)(value >> (i * 8U));
	}
}

static inline void
bt_key_encode(struct bt_disk_key *wire, struct bt_key key)
{
	bt_put64(&wire->objectid, key.objectid);
	wire->type = key.type;
	bt_put64(&wire->offset, key.offset);
}

#endif
