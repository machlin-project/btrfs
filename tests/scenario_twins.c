/* SPDX-License-Identifier: BSD-3-Clause */
/* Compression twins: Linux wrote each twin file of tests/prepare_linux.py in
 * steps under compress=zlib, compress-force=zlib or a zlib compression
 * property, one sync after each step. The scenarios write a copy of every twin
 * with the same steps, bytes, mount option and property, one write per step,
 * and each copy must have its twin's inode flags and extent layout: which
 * pieces compress, where a chunk stops compressing, whether a failed attempt
 * marks the file NOCOMPRESS and what later writes do then. Linux's oracle
 * compares the same layouts in its own tree dumps. */
#include "scenario.h"

#define TWIN_MANIFEST "/twins/manifest.tsv"
#define TWIN_STEPS "/twins/steps/"
#define TWIN_COPY ".copy"
#define TWIN_TRACKED "/greeting"
#define TWIN_MAX_STEPS (MAX_STAGES - 1U)
#define TWIN_PATH_BYTES 128U
#define TWIN_FIELDS 3U
#define COMPRESSION_PROPERTY "btrfs.compression"
#define COMPRESSION_CODEC "zlib"

/* A directory of twins and how Linux wrote them. */
struct twin_mode {
	const char *name;
	const char *plan;
	enum btrfs_compression compression;
	int force;
	int property;
};

static const struct twin_mode twin_modes[] = {
	{ "zlib", "twins-zlib", BTRFS_COMPRESSION_ZLIB, 0, 0 },
	{ "force", "twins-force", BTRFS_COMPRESSION_ZLIB, 1, 0 },
	{ "prop", "twins-property", BTRFS_COMPRESSION_NONE, 0, 1 },
};

/* Splits text at separator into at most count fields, terminating each;
 * returns the number found. */
static size_t
split(char *text, char separator, char **fields, size_t count)
{
	size_t found = 0;
	char *next;

	while (text != NULL && found < count) {
		fields[found++] = text;
		next = strchr(text, separator);
		if (next != NULL) {
			*next++ = '\0';
		}
		text = next;
	}
	return text == NULL ? found : count + 1U;
}

/* One manifest line of mode's twins: the copy's steps and expectations. */
static void
plan_twin(struct context *context, struct plan *plan, const struct twin_mode *mode, char *name,
    char *steps)
{
	char *entries[TWIN_MAX_STEPS];
	char *step[2];
	char twin[TWIN_PATH_BYTES];
	char copy[TWIN_PATH_BYTES];
	char source[TWIN_PATH_BYTES];
	uint8_t *data;
	unsigned long long offset;
	size_t count;
	size_t size;
	size_t k;

	REQUIRE(snprintf(twin, sizeof(twin), "/twins/%s/%s", mode->name, name) < (int)sizeof(twin));
	REQUIRE(snprintf(copy, sizeof(copy), "%s%s", twin, TWIN_COPY) < (int)sizeof(copy));
	count = split(steps, ',', entries, TWIN_MAX_STEPS);
	REQUIRE(count >= 1 && count <= TWIN_MAX_STEPS);
	plan_create(plan, 1, copy, BTRFS_MODE_REGULAR | 0644, NULL);
	if (mode->property) {
		plan_set_xattr(plan, 1, copy, COMPRESSION_PROPERTY, COMPRESSION_CODEC,
		    strlen(COMPRESSION_CODEC), 0);
	}
	for (k = 0; k < count; k++) {
		REQUIRE(split(entries[k], ':', step, 2) == 2);
		offset = strtoull(step[0], NULL, 10);
		REQUIRE(snprintf(source, sizeof(source), "%s%s", TWIN_STEPS, step[1]) <
		    (int)sizeof(source));
		data = fixture_bytes(context, source, &size);
		plan_write_new(plan, k + 1, copy, offset, data, size);
		/* A twin's first step writes from offset 0: after it, the copy
		 * holds that step's bytes. */
		if (k == 0 && count > 1) {
			REQUIRE(offset == 0);
			expect_file(plan, 1, 1, copy, data, size);
		}
		free(data);
	}
	expect_current(context, plan, count, LAST_STAGE, copy, twin);
	expect_layout(plan, count, LAST_STAGE, copy, twin);
}

void
twin_scenarios(struct context *context)
{
	char *fields[TWIN_FIELDS];
	char *line;
	char *next;
	uint8_t *manifest;
	size_t size;
	size_t twins;
	size_t i;
	struct plan plan;

	for (i = 0; i < sizeof(twin_modes) / sizeof(twin_modes[0]); i++) {
		manifest = fixture_bytes(context, TWIN_MANIFEST, &size);
		manifest[size] = '\0';
		plan_init(&plan);
		plan.name = twin_modes[i].plan;
		/* A tracked file gives each stage its record in the export. */
		(void)plan_file(context, &plan, TWIN_TRACKED);
		twins = 0;
		for (line = (char *)manifest; *line != '\0'; line = next) {
			next = strchr(line, '\n');
			REQUIRE(next != NULL);
			*next++ = '\0';
			REQUIRE(split(line, '\t', fields, TWIN_FIELDS) == TWIN_FIELDS);
			if (strcmp(fields[0], twin_modes[i].name) == 0) {
				plan_twin(context, &plan, &twin_modes[i], fields[1], fields[2]);
				twins++;
			}
		}
		free(manifest);
		REQUIRE(twins > 0);
		context->writer.compression = twin_modes[i].compression;
		context->writer.compress_force = twin_modes[i].force;
		run_plan(context, &plan);
		context->writer.compression = BTRFS_COMPRESSION_NONE;
		context->writer.compress_force = 0;
		printf("twins %s: %zu copies match Linux's layouts\n", twin_modes[i].name, twins);
	}
}
