/* SPDX-License-Identifier: BSD-3-Clause */
#include "balance_args.h"
#include <stdlib.h>
#include <string.h>

/* Profiles btrfs-progs names after convert=. */
static const struct {
	const char *name;
	uint64_t profile;
} balance_profiles[] = {
	{ "single", BTRFS_BALANCE_SINGLE },
	{ "dup", BTRFS_GROUP_DUP },
	{ "raid0", BTRFS_GROUP_RAID0 },
	{ "raid1", BTRFS_GROUP_RAID1 },
	{ "raid1c3", BTRFS_GROUP_RAID1C3 },
	{ "raid1c4", BTRFS_GROUP_RAID1C4 },
	{ "raid10", BTRFS_GROUP_RAID10 },
	{ "raid5", BTRFS_GROUP_RAID5 },
	{ "raid6", BTRFS_GROUP_RAID6 },
};

/* One kind's filters as btrfs-progs takes them after -d, -m or -s:
 * comma-separated usage=PERCENT, convert=PROFILE and soft. */
static int
balance_filters(const char *text, struct btrfs_balance_kind *kind)
{
	char copy[128];
	char *word;
	char *rest;
	size_t i;

	*kind = (struct btrfs_balance_kind){ 1, 101, BTRFS_BALANCE_KEEP, 0 };
	if (strlen(text) >= sizeof(copy)) {
		return -1;
	}
	strcpy(copy, text);
	for (word = strtok_r(copy, ",", &rest); word != NULL; word = strtok_r(NULL, ",", &rest)) {
		if (strncmp(word, "usage=", 6) == 0) {
			kind->usage = (unsigned)strtoul(word + 6, NULL, 10);
		} else if (strncmp(word, "convert=", 8) == 0) {
			for (i = 0; i < sizeof(balance_profiles) / sizeof(balance_profiles[0]) &&
			    strcmp(word + 8, balance_profiles[i].name) != 0;
			    i++) {
			}
			if (i == sizeof(balance_profiles) / sizeof(balance_profiles[0])) {
				return -1;
			}
			kind->convert = balance_profiles[i].profile;
		} else if (strcmp(word, "soft") == 0) {
			kind->soft = 1;
		} else {
			return -1;
		}
	}
	return 0;
}

/* btrfs balance start's selection: -d, -m and -s with their filters, -m
 * applying to system groups too unless -s is given; none selects every
 * group. -f reduces metadata redundancy. */
int
tool_balance_args(int argc, char **argv, struct btrfs_balance_args *args)
{
	int any = 0;
	int system = 0;
	int i;

	memset(args, 0, sizeof(*args));
	args->data.convert = BTRFS_BALANCE_KEEP;
	args->metadata.convert = BTRFS_BALANCE_KEEP;
	args->system.convert = BTRFS_BALANCE_KEEP;
	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "-f") == 0) {
			args->force = 1;
		} else if (strncmp(argv[i], "-d", 2) == 0 &&
		    balance_filters(argv[i] + 2, &args->data) == 0) {
			any = 1;
		} else if (strncmp(argv[i], "-m", 2) == 0 &&
		    balance_filters(argv[i] + 2, &args->metadata) == 0) {
			any = 1;
		} else if (strncmp(argv[i], "-s", 2) == 0 &&
		    balance_filters(argv[i] + 2, &args->system) == 0) {
			any = 1;
			system = 1;
		} else {
			return -1;
		}
	}
	if (!any) {
		args->data = (struct btrfs_balance_kind){ 1, 101, BTRFS_BALANCE_KEEP, 0 };
		args->metadata = args->data;
		args->system = args->data;
	} else if (args->metadata.enabled && !system) {
		args->system = args->metadata;
	}
	return 0;
}
