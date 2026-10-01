/* SPDX-License-Identifier: BSD-3-Clause */
/* Transaction scenarios on one Linux fixture: options select the scenario sets
 * for its profile; --export writes the crash cases for the Linux oracle. */
#define _POSIX_C_SOURCE 200809L
#include "scenario.h"

int
main(int argc, char **argv)
{
	struct context *context;
	struct btrfs_fs *fs;
	struct btrfs_info info;
	const char *image = NULL;
	int full = 0;
	int shared = 0;
	int keyed = 0;
	int data = 0;
	int fragment = 0;
	int grow = 0;
	int names = 0;
	uint32_t random_first = 0;
	uint32_t random_count = 0;
	int random_quick = 0;
	int i;

	context = calloc(1, sizeof(*context));
	REQUIRE(context != NULL);
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--export") == 0 && i + 1 < argc) {
			context->export_root = argv[++i];
		} else if (strcmp(argv[i], "--full") == 0) {
			full = 1;
		} else if (strcmp(argv[i], "--shared") == 0) {
			shared = 1;
		} else if (strcmp(argv[i], "--keyed") == 0) {
			keyed = 1;
		} else if (strcmp(argv[i], "--data") == 0) {
			data = 1;
		} else if (strcmp(argv[i], "--fragment") == 0) {
			fragment = 1;
		} else if (strcmp(argv[i], "--grow") == 0) {
			grow = 1;
		} else if (strcmp(argv[i], "--namespace") == 0) {
			names = 1;
		} else if ((strcmp(argv[i], "--random") == 0 ||
			       strcmp(argv[i], "--random-quick") == 0) &&
		    i + 2 < argc) {
			random_quick = strcmp(argv[i], "--random-quick") == 0;
			random_first = (uint32_t)strtoul(argv[++i], NULL, 10);
			random_count = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else {
			REQUIRE(image == NULL);
			image = argv[i];
		}
	}
	REQUIRE(image != NULL);
	REQUIRE(btrfs_image_open(image, &context->image) == 0);
	context->device = calloc(1, sizeof(*context->device));
	REQUIRE(context->device != NULL);
	context->device->image = &context->image;
	context->env = context->image.environment;
	context->env.context = context->device;
	context->env.read = read_device;
	context->env.allocate = allocate;
	context->env.release = release;
	context->env.decompress = decompress_device;
	context->writer =
	    (struct btrfs_write_environment){ context->device, write_device, flush_device };
	context->seed = UINT32_C(0x142857);
	REQUIRE(btrfs_mount(&context->env, BTRFS_TOP_LEVEL_TREE, &fs) == BTRFS_OK);
	btrfs_get_info(fs, &info);
	context->base_generation = info.generation;
	context->node_size = info.node_size;
	context->sector_size = info.sector_size;
	check_invariants(fs);
	btrfs_unmount(fs);
	admission_tests(context);
	copy_tests(context);
	allocation_map_tests(context);
	audit_self_test(context);
	if (full) {
		exhaustion_test(context);
	}
	plan_scenarios(context);
	if (shared) {
		shared_scenarios(context);
	}
	if (keyed) {
		keyed_scenarios(context);
	}
	if (data) {
		data_scenarios(context);
	}
	if (fragment) {
		fragment_scenarios(context);
	}
	if (grow) {
		grow_scenarios(context);
	}
	if (names) {
		namespace_scenarios(context);
	}
	if (random_count != 0) {
		random_scenarios(context, random_first, random_count, random_quick);
	}
	REQUIRE(context->image.live_allocations == 0);
	btrfs_image_close(&context->image);
	free(context->device->writes);
	free(context->device);
	printf("transactions (%u-byte nodes): %zu crash states, %zu explicit recoveries; fault "
	       "sweeps, stale copies and allocation maps PASS\n",
	    context->node_size, context->states, context->recoveries);
	free(context);
	return 0;
}
