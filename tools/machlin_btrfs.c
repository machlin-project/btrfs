/* SPDX-License-Identifier: BSD-3-Clause */
/* machlin-btrfs: the maintenance commands of a mounted machlin_btrfs volume,
 * as btrfs-progs offers them on Linux:
 *
 *   machlin-btrfs balance start [-d[FILTERS]] [-m[FILTERS]] [-s[FILTERS]] [-f] PATH
 *   machlin-btrfs scrub start [-r] PATH
 *   machlin-btrfs verity enable [--hash-alg=sha256|sha512] [--block-size=N] [--salt=HEX] FILE
 *   machlin-btrfs verity measure FILE
 *
 * PATH is any file or directory of the volume. Balance takes btrfs-progs'
 * filters (usage=PERCENT, convert=PROFILE, soft); scrub repairs from good
 * copies unless -r; both run for the superuser. verity takes fsverity-utils'
 * options and prints the digest as it does. Each runs in the kernel through
 * the volume's file system controls. */
#include "../adapters/xnu/fsctl.h"
#include "balance_args.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
usage(void)
{
	fprintf(stderr,
	    "usage: machlin-btrfs balance start [-d[FILTERS]] [-m[FILTERS]] [-s[FILTERS]] [-f] "
	    "PATH\n"
	    "       machlin-btrfs scrub start [-r] PATH\n"
	    "       machlin-btrfs verity enable [--hash-alg=sha256|sha512] [--block-size=N] "
	    "[--salt=HEX] FILE\n"
	    "       machlin-btrfs verity measure FILE\n"
	    "FILTERS: comma-separated usage=PERCENT, convert=PROFILE, soft\n");
}

/* fsverity-utils' defaults: SHA-256 and 4 KiB blocks. */
#define VERITY_SHA256 1U
#define VERITY_SHA512 2U
#define VERITY_BLOCK_SIZE 4096U
#define OPTION_HASH "--hash-alg="
#define OPTION_BLOCK "--block-size="
#define OPTION_SALT "--salt="

static int
hex_value(char digit)
{
	if (digit >= '0' && digit <= '9') {
		return digit - '0';
	}
	if (digit >= 'a' && digit <= 'f') {
		return digit - 'a' + 10;
	}
	if (digit >= 'A' && digit <= 'F') {
		return digit - 'A' + 10;
	}
	return -1;
}

static int
verity_enable(int argc, char **argv)
{
	struct machlin_btrfs_verity_enable control;
	const char *text;
	size_t length;
	size_t i;
	int high;
	int low;
	int a;

	memset(&control, 0, sizeof(control));
	control.algorithm = VERITY_SHA256;
	control.block_size = VERITY_BLOCK_SIZE;
	for (a = 0; a < argc - 1; a++) {
		if (strcmp(argv[a], OPTION_HASH "sha256") == 0) {
			control.algorithm = VERITY_SHA256;
		} else if (strcmp(argv[a], OPTION_HASH "sha512") == 0) {
			control.algorithm = VERITY_SHA512;
		} else if (strncmp(argv[a], OPTION_BLOCK, strlen(OPTION_BLOCK)) == 0) {
			control.block_size =
			    (uint32_t)strtoul(argv[a] + strlen(OPTION_BLOCK), NULL, 10);
		} else if (strncmp(argv[a], OPTION_SALT, strlen(OPTION_SALT)) == 0) {
			text = argv[a] + strlen(OPTION_SALT);
			length = strlen(text);
			if (length % 2 != 0 || length / 2 > sizeof(control.salt)) {
				usage();
				return 2;
			}
			for (i = 0; i < length / 2; i++) {
				high = hex_value(text[2 * i]);
				low = hex_value(text[2 * i + 1]);
				if (high < 0 || low < 0) {
					usage();
					return 2;
				}
				control.salt[i] = (uint8_t)(high * 16 + low);
			}
			control.salt_size = (uint32_t)(length / 2);
		} else {
			usage();
			return 2;
		}
	}
	if (fsctl(argv[argc - 1], MACHLIN_BTRFS_FSCTL_ENABLE_VERITY, &control, 0) != 0) {
		fprintf(stderr, "machlin-btrfs: enabling fs-verity on %s: %s\n", argv[argc - 1],
		    strerror(errno));
		return 1;
	}
	return 0;
}

static int
verity_measure(int argc, char **argv)
{
	struct machlin_btrfs_verity_digest control;
	uint32_t i;

	if (argc != 1) {
		usage();
		return 2;
	}
	memset(&control, 0, sizeof(control));
	if (fsctl(argv[0], MACHLIN_BTRFS_FSCTL_MEASURE_VERITY, &control, 0) != 0) {
		fprintf(stderr, "machlin-btrfs: measuring %s: %s\n", argv[0], strerror(errno));
		return 1;
	}
	if (control.size > sizeof(control.digest)) {
		return 1;
	}
	printf("%s:", control.algorithm == VERITY_SHA512 ? "sha512" : "sha256");
	for (i = 0; i < control.size; i++) {
		printf("%02x", control.digest[i]);
	}
	printf(" %s\n", argv[0]);
	return 0;
}

static void
kind_of(const struct btrfs_balance_kind *kind, struct machlin_btrfs_balance_kind *control)
{
	control->enabled = kind->enabled != 0;
	control->usage = kind->usage;
	control->convert = kind->convert;
	control->soft = kind->soft != 0;
	control->reserved = 0;
}

static int
balance(int argc, char **argv)
{
	struct btrfs_balance_args args;
	struct machlin_btrfs_balance control;
	const char *path = argv[argc - 1];

	if (tool_balance_args(argc - 1, argv, &args) != 0) {
		usage();
		return 2;
	}
	memset(&control, 0, sizeof(control));
	kind_of(&args.data, &control.data);
	kind_of(&args.metadata, &control.metadata);
	kind_of(&args.system, &control.system);
	control.force = args.force != 0;
	if (fsctl(path, MACHLIN_BTRFS_FSCTL_BALANCE, &control, 0) != 0) {
		fprintf(stderr, "machlin-btrfs: balance of %s: %s (%" PRIu64 " groups relocated)\n",
		    path, strerror(errno), control.groups);
		return 1;
	}
	printf("Done, had to relocate %" PRIu64 " out of the selected chunks: %" PRIu64
	       " tree blocks, %" PRIu64 " data extents of %" PRIu64 " bytes in %" PRIu64
	       " commits\n",
	    control.groups, control.blocks, control.extents, control.bytes, control.commits);
	return 0;
}

static int
scrub(int argc, char **argv)
{
	struct machlin_btrfs_scrub control;
	const char *path;

	memset(&control, 0, sizeof(control));
	control.repair = 1;
	if (argc == 2 && strcmp(argv[0], "-r") == 0) {
		control.repair = 0;
	} else if (argc != 1) {
		usage();
		return 2;
	}
	path = argv[argc - 1];
	if (fsctl(path, MACHLIN_BTRFS_FSCTL_SCRUB, &control, 0) != 0) {
		fprintf(stderr, "machlin-btrfs: scrub of %s: %s\n", path, strerror(errno));
		return 1;
	}
	printf("tree copies %" PRIu64 ", data copies %" PRIu64 ", unchecked %" PRIu64
	       ", corrupted %" PRIu64 ", unreadable %" PRIu64 ", repaired %" PRIu64
	       ", unrepairable %" PRIu64 ", superblock copies %" PRIu64 " (%" PRIu64
	       " failing), parity rows %" PRIu64 " (%" PRIu64 " parity errors)\n",
	    control.tree_copies, control.data_copies, control.unchecked, control.corrupted,
	    control.unreadable, control.repaired, control.unrepairable, control.super_copies,
	    control.super_errors, control.parity_rows, control.parity_errors);
	/* As btrfs scrub: uncorrected errors exit 3. */
	if (control.unrepairable != 0 || control.super_errors != 0 ||
	    (!control.repair &&
		control.corrupted + control.unreadable + control.parity_errors != 0)) {
		return 3;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc >= 4 && strcmp(argv[1], "balance") == 0 && strcmp(argv[2], "start") == 0) {
		return balance(argc - 3, argv + 3);
	}
	if (argc >= 4 && strcmp(argv[1], "scrub") == 0 && strcmp(argv[2], "start") == 0) {
		return scrub(argc - 3, argv + 3);
	}
	if (argc >= 4 && strcmp(argv[1], "verity") == 0 && strcmp(argv[2], "enable") == 0) {
		return verity_enable(argc - 3, argv + 3);
	}
	if (argc >= 4 && strcmp(argv[1], "verity") == 0 && strcmp(argv[2], "measure") == 0) {
		return verity_measure(argc - 3, argv + 3);
	}
	usage();
	return 2;
}
