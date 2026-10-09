/* SPDX-License-Identifier: BSD-3-Clause */
/* Transaction scenarios on one Linux fixture: options select the scenario sets
 * for its profile; --export writes the crash cases for the Linux oracle. */
#define _POSIX_C_SOURCE 200809L
#include "scenario.h"
#include <btrfs/codec.h>

/* The kernel adapter's encoders: zlib is the platform's, Zstandard the shared
 * freestanding encoder in place of libzstd. */
static enum btrfs_result
kernel_compress(void *context, enum btrfs_compression codec, const void *input, size_t input_size,
    void *output, size_t capacity, size_t *size)
{
	static uint64_t workspace[BTRFS_ZSTD_COMPRESS_WORKSPACE_BYTES / sizeof(uint64_t)];

	if (codec == BTRFS_COMPRESSION_ZSTD) {
		return btrfs_zstd_compress(workspace, input, input_size, output, capacity, size);
	}
	return btrfs_image_compress(context, codec, input, input_size, output, capacity, size);
}

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
	int holes = 0;
	int convert = 0;
	int groups = 0;
	int grow = 0;
	int names = 0;
	int subvolumes = 0;
	int quotas = 0;
	int simple_quotas = 0;
	int verity = 0;
	int kernel_codecs = 0;
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
		} else if (strcmp(argv[i], "--holes") == 0) {
			holes = 1;
		} else if (strcmp(argv[i], "--convert") == 0) {
			convert = 1;
		} else if (strcmp(argv[i], "--groups") == 0) {
			groups = 1;
		} else if (strcmp(argv[i], "--grow") == 0) {
			grow = 1;
		} else if (strcmp(argv[i], "--namespace") == 0) {
			names = 1;
		} else if (strcmp(argv[i], "--subvolume") == 0) {
			subvolumes = 1;
		} else if (strcmp(argv[i], "--quota") == 0) {
			quotas = 1;
		} else if (strcmp(argv[i], "--squota") == 0) {
			simple_quotas = 1;
		} else if (strcmp(argv[i], "--verity") == 0) {
			verity = 1;
		} else if (strcmp(argv[i], "--kernel-codecs") == 0) {
			kernel_codecs = 1;
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
	context->writer = (struct btrfs_write_environment){ context->device, write_device,
		flush_device, kernel_codecs ? kernel_compress : btrfs_image_compress,
		BTRFS_COMPRESSION_NONE };
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
	if (holes) {
		holes_scenarios(context);
	}
	if (convert) {
		convert_scenarios(context);
	}
	if (groups) {
		groups_scenarios(context);
	}
	if (grow) {
		grow_scenarios(context);
	}
	if (names) {
		namespace_scenarios(context);
	}
	if (subvolumes) {
		subvolume_scenarios(context);
	}
	if (simple_quotas) {
		squota_scenarios(context);
	}
	if (quotas) {
		quota_scenarios(context);
	}
	if (verity) {
		verity_scenarios(context);
	}
	if (random_count != 0) {
		random_scenarios(context, random_first, random_count, random_quick);
	}
	REQUIRE(context->image.live_allocations == 0);
	btrfs_image_close(&context->image);
	free(context->device->writes);
	free(context->device);
	if (context->qgroup_audits != 0) {
		printf("qgroups: %zu audited states agree with the stored qgroup items\n",
		    context->qgroup_audits);
	}
	printf("transactions (%u-byte nodes): %zu crash states, %zu explicit recoveries, %zu "
	       "transaction views read as published; fault sweeps, stale copies and allocation "
	       "maps PASS\n",
	    context->node_size, context->states, context->recoveries, context->reader_digests);
	free(context);
	return 0;
}
