/* SPDX-License-Identifier: BSD-3-Clause */
/* RAID5 and RAID6 reconstruction for reads, as Linux rebuilds a data stripe
 * (fs/btrfs/raid56.c, lib/raid6): in each row of a chunk's stripes, P is the
 * XOR of the row's data stripes and Q the sum over them of g^index times the
 * stripe in GF(2^8), with the polynomial x^8 + x^4 + x^3 + x^2 + 1 and g = 2.
 * Bytes of a data stripe rebuild from the same offset in the row's other
 * stripes: from P and the other data, from Q and the other data, or, with a
 * second data stripe lost too, from P and Q. */
#include "internal.h"

/* GF(2^8) of lib/raid6: x^8 + x^4 + x^3 + x^2 + 1, generator 2. */
#define BT_GF_POLYNOMIAL 0x11dU
#define BT_GF_GENERATOR 2U
#define BT_GF_INVERSE_EXPONENT 254U

static uint8_t
bt_gf_mul(uint8_t a, uint8_t b)
{
	unsigned product = 0;
	unsigned value = a;

	while (b != 0) {
		if (b & 1U) {
			product ^= value;
		}
		value <<= 1;
		if (value & 0x100U) {
			value ^= BT_GF_POLYNOMIAL;
		}
		b >>= 1;
	}
	return (uint8_t)product;
}

static uint8_t
bt_gf_pow(uint8_t base, unsigned exponent)
{
	uint8_t result = 1;

	while (exponent != 0) {
		if (exponent & 1U) {
			result = bt_gf_mul(result, base);
		}
		base = bt_gf_mul(base, base);
		exponent >>= 1;
	}
	return result;
}

static uint8_t
bt_gf_inverse(uint8_t value)
{
	return bt_gf_pow(value, BT_GF_INVERSE_EXPONENT);
}

/* Every byte value times factor. */
#define BT_GF_VALUES 256U

static void
bt_gf_table(uint8_t *table, uint8_t factor)
{
	unsigned value;

	for (value = 0; value < BT_GF_VALUES; value++) {
		table[value] = bt_gf_mul((uint8_t)value, factor);
	}
}

/* out ^= factor * in, bytewise. */
static void
bt_gf_add_scaled(uint8_t *out, const uint8_t *in, size_t length, uint8_t factor)
{
	uint8_t table[BT_GF_VALUES];
	size_t i;

	if (factor == 1) {
		for (i = 0; i < length; i++) {
			out[i] ^= in[i];
		}
		return;
	}
	bt_gf_table(table, factor);
	for (i = 0; i < length; i++) {
		out[i] ^= table[in[i]];
	}
}

static void
bt_gf_scale(uint8_t *bytes, size_t length, uint8_t factor)
{
	uint8_t table[BT_GF_VALUES];
	size_t i;

	bt_gf_table(table, factor);
	for (i = 0; i < length; i++) {
		bytes[i] = table[bytes[i]];
	}
}

unsigned
bt_raid56_copies(uint64_t profile, unsigned stripes)
{
	/* The data stripe and its rebuild from P; RAID6 adds the rebuild from Q
	 * and, from P and Q, one with each other data stripe of the row lost. */
	return profile == BT_BLOCK_RAID5 ? 2U : 3U + (stripes - 2U) - 1U;
}

/* Reads the bytes at offset in the row of stripe index into buffer. */
static enum btrfs_result
bt_raid56_read(const struct btrfs_fs *fs, const struct bt_chunk *chunk, unsigned index,
    uint64_t row, uint64_t offset, uint8_t *buffer, size_t length)
{
	return bt_read_device(fs, chunk->devices[index],
	    chunk->physical[index] + row * BT_STRIPE_LENGTH + offset, buffer, length);
}

enum btrfs_result
bt_raid56_rebuild(const struct btrfs_fs *fs, const struct bt_chunk *chunk, uint64_t within,
    uint8_t *out, size_t length, unsigned mirror)
{
	uint64_t profile = chunk->type & BT_BLOCK_PROFILES;
	unsigned stripes = chunk->stripes;
	unsigned data = stripes - (profile == BT_BLOCK_RAID5 ? 1U : 2U);
	uint64_t unit = within / BT_STRIPE_LENGTH;
	uint64_t offset = within % BT_STRIPE_LENGTH;
	uint64_t row = unit / data;
	unsigned target = (unsigned)(unit % data);
	unsigned lost = data;
	unsigned d;
	uint8_t *scratch;
	uint8_t *pxy = NULL;
	uint8_t factor;
	enum btrfs_result error;

	if (length > BT_STRIPE_LENGTH - offset || mirror == 0 ||
	    mirror >= bt_raid56_copies(profile, stripes)) {
		return BTRFS_INVALID_ARGUMENT;
	}
	/* Copies from the fourth on lose one more data stripe: the n-th other. */
	if (mirror >= 3) {
		lost = mirror - 3 < target ? mirror - 3 : mirror - 2;
	}
	scratch = fs->env.allocate(fs->env.context, length);
	if (scratch == NULL) {
		return BTRFS_NO_MEMORY;
	}
	/* P (or Q) of the row, then every data stripe but the lost ones. */
	error = bt_raid56_read(fs, chunk, (unsigned)((row + data + (mirror == 2)) % stripes), row,
	    offset, out, length);
	if (error == BTRFS_OK && mirror >= 3) {
		pxy = fs->env.allocate(fs->env.context, length);
		error = pxy == NULL ? BTRFS_NO_MEMORY
				    : bt_raid56_read(fs, chunk, (unsigned)((row + data) % stripes),
					  row, offset, pxy, length);
		if (error == BTRFS_OK) {
			/* out holds Q, pxy holds P. */
			error = bt_raid56_read(fs, chunk, (unsigned)((row + data + 1) % stripes),
			    row, offset, out, length);
		}
	}
	for (d = 0; error == BTRFS_OK && d < data; d++) {
		if (d == target || d == lost) {
			continue;
		}
		error = bt_raid56_read(
		    fs, chunk, (unsigned)((row + d) % stripes), row, offset, scratch, length);
		if (error != BTRFS_OK) {
			break;
		}
		if (mirror == 1) {
			bt_gf_add_scaled(out, scratch, length, 1);
		} else {
			bt_gf_add_scaled(out, scratch, length, bt_gf_pow(BT_GF_GENERATOR, d));
			if (pxy != NULL) {
				bt_gf_add_scaled(pxy, scratch, length, 1);
			}
		}
	}
	if (error == BTRFS_OK && mirror == 2) {
		/* out = g^target * D_target. */
		bt_gf_scale(out, length, bt_gf_inverse(bt_gf_pow(BT_GF_GENERATOR, target)));
	} else if (error == BTRFS_OK && pxy != NULL) {
		/* pxy = D_t + D_l, out = g^t D_t + g^l D_l: D_l = (out + g^t pxy) /
		 * (g^t + g^l), and D_t = pxy + D_l. */
		bt_gf_add_scaled(out, pxy, length, bt_gf_pow(BT_GF_GENERATOR, target));
		factor = bt_gf_inverse((uint8_t)(bt_gf_pow(BT_GF_GENERATOR, target) ^
		    bt_gf_pow(BT_GF_GENERATOR, lost)));
		bt_gf_scale(out, length, factor);
		bt_gf_add_scaled(out, pxy, length, 1);
	}
	if (pxy != NULL) {
		fs->env.release(fs->env.context, pxy, length);
	}
	fs->env.release(fs->env.context, scratch, length);
	return error;
}

void
bt_raid56_parity(uint64_t profile, const uint8_t *const *data, unsigned count, uint8_t *p,
    uint8_t *q, size_t length)
{
	unsigned d;

	bt_zero(p, length);
	if (profile == BT_BLOCK_RAID6) {
		bt_zero(q, length);
	}
	for (d = 0; d < count; d++) {
		bt_gf_add_scaled(p, data[d], length, 1);
		if (profile == BT_BLOCK_RAID6) {
			bt_gf_add_scaled(
			    q, data[d], length, bt_gf_pow(BT_GF_GENERATOR, (unsigned)d));
		}
	}
}

enum btrfs_result
bt_raid56_unit(const struct btrfs_fs *fs, const struct bt_chunk *chunk, uint64_t row,
    unsigned stripe, uint8_t *out, uint8_t *units)
{
	const uint8_t *data[BT_MAX_STRIPES];
	uint64_t profile = chunk->type & BT_BLOCK_PROFILES;
	unsigned stripes = chunk->stripes;
	unsigned count = stripes - (profile == BT_BLOCK_RAID5 ? 1U : 2U);
	unsigned position = (unsigned)((stripe + stripes - row % stripes) % stripes);
	unsigned d;
	enum btrfs_result error = BTRFS_OK;

	/* The row's data units and P, but the one the stripe holds. */
	for (d = 0; error == BTRFS_OK && d <= count; d++) {
		if (d != position) {
			error = bt_raid56_read(fs, chunk, (unsigned)((row + d) % stripes), row, 0,
			    units + d * BT_STRIPE_LENGTH, BT_STRIPE_LENGTH);
		}
	}
	if (error != BTRFS_OK) {
		return error;
	}
	for (d = 0; d < count; d++) {
		data[d] = units + d * BT_STRIPE_LENGTH;
	}
	if (position < count) {
		/* A data unit: P and the other data units. */
		bt_copy(out, units + count * BT_STRIPE_LENGTH, BT_STRIPE_LENGTH);
		for (d = 0; d < count; d++) {
			if (d != position) {
				bt_gf_add_scaled(out, data[d], BT_STRIPE_LENGTH, 1);
			}
		}
		return BTRFS_OK;
	}
	/* P or Q: the data units' parity again. */
	bt_raid56_parity(profile, data, count,
	    position == count ? out : units + count * BT_STRIPE_LENGTH,
	    position == count ? units + (count + 1) * BT_STRIPE_LENGTH : out, BT_STRIPE_LENGTH);
	return BTRFS_OK;
}
