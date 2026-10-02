/* SPDX-License-Identifier: BSD-3-Clause */
/* Randomized differential scenarios: a seeded generator draws namespace, xattr,
 * data and attribute operations, including refusals, against a model of the
 * expected tree under /fuzz. The model's state after every commit becomes the
 * stage's expectations, which the reader, both audits and the Linux oracle
 * check in sampled crash states. */
#define _POSIX_C_SOURCE 200809L
#include "scenario.h"

#define MODEL_INODES 256U
#define MODEL_ENTRIES 512U
#define MODEL_TARGET 48U
#define MODEL_XATTR_VALUE 200U
#define MODEL_FILE_LIMIT (256U * 1024U)
#define MODEL_WRITE_LIMIT 16384U
#define MODEL_GROWTH 9000U
#define MODEL_LINK_LIMIT 4U
#define MODEL_ROOT 0U
#define MODEL_NONE UINT32_MAX
#define RANDOM_OPERATIONS 24U
#define RANDOM_COMMITS 3U
#define RANDOM_PREFIX_POINTS 12U
#define RANDOM_FAULT_POINTS 24U
#define RANDOM_BASE "/fuzz"
#define RANDOM_PATH 512U
#define MODE_SET_UID 04000U
#define MODE_SET_GID 02000U
#define MODE_GROUP_EXECUTE 00010U

/* Directory and file names: short ones and four that share one CRC32C name
 * hash, so entries are appended to and cut from packed items. */
static const char *const random_names[] = { "a", "b", "c", "d", "e", "f", "ethvq997ethvq997",
	"ethvq997wdkjavbx", "wdkjavbxethvq997", "wdkjavbxwdkjavbx" };
#define RANDOM_NAMES (sizeof(random_names) / sizeof(random_names[0]))

static const char *const random_xattrs[] = { "user.a", "user.b", "user.ethvq997ethvq997",
	"user.ethvq997wdkjavbx", "user.wdkjavbxethvq997" };
#define RANDOM_XATTRS (sizeof(random_xattrs) / sizeof(random_xattrs[0]))

static const uint32_t random_permissions[] = { 0600, 0644, 0640, 0755, 0700 };
static const uint32_t random_owners[] = { 0, 1001, NAMESPACE_UID };

enum random_kind {
	RANDOM_CREATE,
	RANDOM_MKDIR,
	RANDOM_SYMLINK,
	RANDOM_LINK,
	RANDOM_UNLINK,
	RANDOM_RENAME,
	RANDOM_SET_XATTR,
	RANDOM_REMOVE_XATTR,
	RANDOM_WRITE,
	RANDOM_TRUNCATE,
	RANDOM_ATTRIBUTES,
	RANDOM_CLEAN,
	RANDOM_REFUSAL,
	RANDOM_EXCHANGE,
	RANDOM_WHITEOUT,
	RANDOM_KINDS
};

/* Weights of the operation kinds, in random_kind order. */
static const unsigned random_weights[RANDOM_KINDS] = { 9, 4, 3, 4, 6, 7, 5, 2, 12, 4, 4, 1, 4, 2,
	2 };

struct model_inode {
	int live;
	int orphan;
	int frozen;
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	uint32_t links;
	int64_t access_seconds;
	int64_t modify_seconds;
	uint8_t *data;
	size_t size;
	char target[MODEL_TARGET + 1];
	uint32_t xattr_seen;
	uint32_t xattr_present;
	uint8_t xattr_values[RANDOM_XATTRS][MODEL_XATTR_VALUE];
	size_t xattr_sizes[RANDOM_XATTRS];
};

struct model_entry {
	int used;
	uint32_t parent;
	uint32_t child;
	uint32_t name;
};

struct model {
	struct model_inode inodes[MODEL_INODES];
	struct model_entry entries[MODEL_ENTRIES];
	uint32_t state;
	size_t commit;
	int64_t now;
};

static uint32_t
model_random(struct model *model)
{
	model->state ^= model->state << 13;
	model->state ^= model->state >> 17;
	model->state ^= model->state << 5;
	return model->state;
}

static uint32_t
model_below(struct model *model, uint32_t bound)
{
	return bound == 0 ? 0 : model_random(model) % bound;
}

static uint32_t
model_type(const struct model_inode *inode)
{
	return inode->mode & BTRFS_MODE_TYPE;
}

/* The entry naming inode: a directory's only name, a file's first one. */
static uint32_t
model_entry_of(const struct model *model, uint32_t inode)
{
	uint32_t i;

	for (i = 0; i < MODEL_ENTRIES; i++) {
		if (model->entries[i].used && model->entries[i].child == inode) {
			return i;
		}
	}
	return MODEL_NONE;
}

static void
model_path(const struct model *model, uint32_t inode, char *path, size_t size)
{
	char tail[RANDOM_PATH];
	uint32_t entry;

	if (inode == MODEL_ROOT) {
		REQUIRE(snprintf(path, size, "%s", RANDOM_BASE) < (int)size);
		return;
	}
	entry = model_entry_of(model, inode);
	REQUIRE(entry != MODEL_NONE);
	model_path(model, model->entries[entry].parent, tail, sizeof(tail));
	REQUIRE(snprintf(path, size, "%s/%s", tail, random_names[model->entries[entry].name]) <
	    (int)size);
}

static void
model_entry_path(const struct model *model, uint32_t entry, char *path, size_t size)
{
	char parent[RANDOM_PATH];

	model_path(model, model->entries[entry].parent, parent, sizeof(parent));
	REQUIRE(snprintf(path, size, "%s/%s", parent, random_names[model->entries[entry].name]) <
	    (int)size);
}

static uint32_t
model_find(const struct model *model, uint32_t directory, uint32_t name)
{
	uint32_t i;

	for (i = 0; i < MODEL_ENTRIES; i++) {
		if (model->entries[i].used && model->entries[i].parent == directory &&
		    model->entries[i].name == name) {
			return i;
		}
	}
	return MODEL_NONE;
}

static int
model_empty(const struct model *model, uint32_t directory)
{
	uint32_t i;

	for (i = 0; i < MODEL_ENTRIES; i++) {
		if (model->entries[i].used && model->entries[i].parent == directory) {
			return 0;
		}
	}
	return 1;
}

/* Whether ancestor is inode or one of its directories. */
static int
model_below_directory(const struct model *model, uint32_t inode, uint32_t ancestor)
{
	uint32_t entry;

	for (;;) {
		if (inode == ancestor) {
			return 1;
		}
		if (inode == MODEL_ROOT) {
			return 0;
		}
		entry = model_entry_of(model, inode);
		REQUIRE(entry != MODEL_NONE);
		inode = model->entries[entry].parent;
	}
}

/* An object renamed as a directory earlier in this commit, or inside one,
 * keeps its committed path until the commit ends; later operations of the
 * same commit leave it alone. */
static int
model_usable(const struct model *model, uint32_t inode)
{
	uint32_t entry;

	for (;;) {
		if (model->inodes[inode].frozen) {
			return 0;
		}
		if (inode == MODEL_ROOT) {
			return 1;
		}
		entry = model_entry_of(model, inode);
		if (entry == MODEL_NONE) {
			return 0;
		}
		inode = model->entries[entry].parent;
	}
}

static uint32_t
model_pick_inode(struct model *model, uint32_t type)
{
	uint32_t candidates[MODEL_INODES];
	uint32_t count = 0;
	uint32_t i;

	for (i = 0; i < MODEL_INODES; i++) {
		if (model->inodes[i].live && !model->inodes[i].orphan &&
		    (type == 0 || model_type(&model->inodes[i]) == type) &&
		    model_usable(model, i)) {
			candidates[count++] = i;
		}
	}
	return count == 0 ? MODEL_NONE : candidates[model_below(model, count)];
}

static uint32_t
model_pick_entry(struct model *model, int (*accept)(const struct model *, uint32_t))
{
	uint32_t candidates[MODEL_ENTRIES];
	uint32_t count = 0;
	uint32_t i;

	for (i = 0; i < MODEL_ENTRIES; i++) {
		/* The chosen name's own directory chain decides, not the inode's
		 * first name. */
		if (model->entries[i].used && model_usable(model, model->entries[i].parent) &&
		    !model->inodes[model->entries[i].child].frozen &&
		    (accept == NULL || accept(model, i))) {
			candidates[count++] = i;
		}
	}
	return count == 0 ? MODEL_NONE : candidates[model_below(model, count)];
}

static uint32_t
model_free_name(struct model *model, uint32_t directory)
{
	uint32_t start = model_below(model, RANDOM_NAMES);
	uint32_t i;

	for (i = 0; i < RANDOM_NAMES; i++) {
		if (model_find(model, directory, (start + i) % RANDOM_NAMES) == MODEL_NONE) {
			return (start + i) % RANDOM_NAMES;
		}
	}
	return MODEL_NONE;
}

static uint32_t
model_new_inode(struct model *model, uint32_t mode)
{
	struct model_inode *inode;
	uint32_t i;

	for (i = 1; i < MODEL_INODES && model->inodes[i].live; i++) {
	}
	REQUIRE(i < MODEL_INODES);
	inode = &model->inodes[i];
	free(inode->data);
	memset(inode, 0, sizeof(*inode));
	inode->live = 1;
	inode->mode = mode;
	inode->uid = NAMESPACE_UID;
	inode->gid = NAMESPACE_GID;
	inode->links = 1;
	inode->access_seconds = model->now;
	inode->modify_seconds = model->now;
	return i;
}

static void
model_add_entry(struct model *model, uint32_t directory, uint32_t name, uint32_t child)
{
	uint32_t i;

	for (i = 0; i < MODEL_ENTRIES && model->entries[i].used; i++) {
	}
	REQUIRE(i < MODEL_ENTRIES);
	model->entries[i] = (struct model_entry){ 1, directory, child, name };
	model->inodes[directory].modify_seconds = model->now;
}

static void
model_delete_inode(struct model *model, uint32_t inode)
{
	free(model->inodes[inode].data);
	memset(&model->inodes[inode], 0, sizeof(model->inodes[inode]));
}

/* Removes a name; an inode without names is deleted, or kept as an orphan. */
static void
model_remove_entry(struct model *model, uint32_t entry, int open)
{
	struct model_inode *child = &model->inodes[model->entries[entry].child];
	uint32_t inode = model->entries[entry].child;

	model->inodes[model->entries[entry].parent].modify_seconds = model->now;
	model->entries[entry].used = 0;
	REQUIRE(child->links != 0);
	child->links--;
	if (child->links == 0) {
		if (open) {
			child->orphan = 1;
		} else {
			model_delete_inode(model, inode);
		}
	}
}

static int
accept_removable(const struct model *model, uint32_t entry)
{
	const struct model_inode *child = &model->inodes[model->entries[entry].child];

	return model_type(child) != BTRFS_MODE_DIRECTORY ||
	    model_empty(model, model->entries[entry].child);
}

static int
accept_any(const struct model *model, uint32_t entry)
{
	(void)model;
	(void)entry;
	return 1;
}

static int
accept_full_directory(const struct model *model, uint32_t entry)
{
	uint32_t child = model->entries[entry].child;

	return model_type(&model->inodes[child]) == BTRFS_MODE_DIRECTORY &&
	    !model_empty(model, child);
}

static void
model_fill(struct model *model, uint8_t *bytes, size_t size)
{
	uint32_t seed = model_random(model);
	size_t i;

	for (i = 0; i < size; i++) {
		bytes[i] = (uint8_t)(seed + i * 131U + (i >> 7));
	}
}

static void
model_resize(struct model_inode *inode, size_t size)
{
	uint8_t *grown = realloc(inode->data, size + 1);

	REQUIRE(grown != NULL);
	if (size > inode->size) {
		memset(grown + inode->size, 0, size - inode->size);
	}
	inode->data = grown;
	inode->size = size;
}

/* A write or truncation of a file with set-id bits needs a privilege
 * decision first: a kept decision leaves them, a dropped one clears them. */
static void
model_privileges(struct model *model, struct plan *plan, uint32_t inode, const char *path)
{
	struct model_inode *file = &model->inodes[inode];
	int keep;

	if ((file->mode & MODE_SET_UID) == 0 &&
	    (file->mode & (MODE_SET_GID | MODE_GROUP_EXECUTE)) !=
		(MODE_SET_GID | MODE_GROUP_EXECUTE)) {
		return;
	}
	keep = (int)model_below(model, 2);
	plan_privileges(plan, model->commit, path, keep);
	if (!keep) {
		file->mode &= ~MODE_SET_UID;
		if (file->mode & MODE_GROUP_EXECUTE) {
			file->mode &= ~MODE_SET_GID;
		}
	}
}

static int
random_create(struct model *model, struct plan *plan, uint32_t mode)
{
	char path[RANDOM_PATH];
	char target[MODEL_TARGET + 1];
	uint32_t directory = model_pick_inode(model, BTRFS_MODE_DIRECTORY);
	uint32_t name;
	uint32_t inode;
	size_t i;

	if (directory == MODEL_NONE) {
		return 0;
	}
	name = model_free_name(model, directory);
	if (name == MODEL_NONE) {
		return 0;
	}
	model_path(model, directory, path, sizeof(path));
	/* Bounded depth keeps every path well inside one buffer. */
	if (strlen(path) > RANDOM_PATH / 2) {
		return 0;
	}
	strcat(path, "/");
	strcat(path, random_names[name]);
	if ((mode & BTRFS_MODE_TYPE) == BTRFS_MODE_SYMLINK) {
		for (i = 0; i < 1 + model_below(model, MODEL_TARGET); i++) {
			target[i] = (char)('a' + model_below(model, 26));
		}
		target[i] = '\0';
		plan_create(plan, model->commit, path, mode, target);
	} else {
		plan_create(plan, model->commit, path, mode, NULL);
	}
	inode = model_new_inode(model, mode);
	if ((mode & BTRFS_MODE_TYPE) == BTRFS_MODE_SYMLINK) {
		strcpy(model->inodes[inode].target, target);
		model->inodes[inode].size = strlen(target);
	}
	model_add_entry(model, directory, name, inode);
	return 1;
}

static int
random_link(struct model *model, struct plan *plan)
{
	char source[RANDOM_PATH];
	char target[RANDOM_PATH];
	uint32_t inode = model_pick_inode(
	    model, model_below(model, 2) ? BTRFS_MODE_REGULAR : BTRFS_MODE_SYMLINK);
	uint32_t directory = model_pick_inode(model, BTRFS_MODE_DIRECTORY);
	uint32_t name;

	if (inode == MODEL_NONE || directory == MODEL_NONE ||
	    model->inodes[inode].links >= MODEL_LINK_LIMIT) {
		return 0;
	}
	name = model_free_name(model, directory);
	if (name == MODEL_NONE) {
		return 0;
	}
	model_path(model, inode, source, sizeof(source));
	model_path(model, directory, target, sizeof(target));
	if (strlen(target) > RANDOM_PATH / 2) {
		return 0;
	}
	strcat(target, "/");
	strcat(target, random_names[name]);
	plan_link(plan, model->commit, source, target);
	model->inodes[inode].links++;
	model_add_entry(model, directory, name, inode);
	return 1;
}

static int
random_unlink(struct model *model, struct plan *plan)
{
	char path[RANDOM_PATH];
	uint32_t entry = model_pick_entry(model, accept_removable);
	struct model_inode *child;
	int open;

	if (entry == MODEL_NONE) {
		return 0;
	}
	child = &model->inodes[model->entries[entry].child];
	/* An open regular file keeps its last name's inode as an orphan. */
	open = model_type(child) == BTRFS_MODE_REGULAR && child->links == 1 &&
	    model_below(model, 4) == 0;
	model_entry_path(model, entry, path, sizeof(path));
	plan_unlink(plan, model->commit, path, open);
	model_remove_entry(model, entry, open);
	return 1;
}

/* RENAME_EXCHANGE of two names: each then names the other's inode, and both
 * directories change; a directory may not end up below itself. Exchanged
 * directories keep their committed paths for the rest of the commit. */
static int
random_exchange(struct model *model, struct plan *plan)
{
	char source[RANDOM_PATH];
	char target[RANDOM_PATH];
	uint32_t first = model_pick_entry(model, accept_any);
	uint32_t second = model_pick_entry(model, accept_any);
	uint32_t a;
	uint32_t b;
	int directories;

	if (first == MODEL_NONE || second == MODEL_NONE || first == second) {
		return 0;
	}
	a = model->entries[first].child;
	b = model->entries[second].child;
	directories = model_type(&model->inodes[a]) == BTRFS_MODE_DIRECTORY ||
	    model_type(&model->inodes[b]) == BTRFS_MODE_DIRECTORY;
	if ((model_type(&model->inodes[a]) == BTRFS_MODE_DIRECTORY &&
		model_below_directory(model, model->entries[second].parent, a)) ||
	    (model_type(&model->inodes[b]) == BTRFS_MODE_DIRECTORY &&
		model_below_directory(model, model->entries[first].parent, b))) {
		return 0;
	}
	model_entry_path(model, first, source, sizeof(source));
	model_entry_path(model, second, target, sizeof(target));
	if (directories && (strlen(source) > RANDOM_PATH / 4 || strlen(target) > RANDOM_PATH / 4)) {
		return 0;
	}
	plan_exchange(plan, model->commit, source, target);
	if (a == b) {
		/* Two names of one inode: nothing changes. */
		return 1;
	}
	model->entries[first].child = b;
	model->entries[second].child = a;
	model->inodes[model->entries[first].parent].modify_seconds = model->now;
	model->inodes[model->entries[second].parent].modify_seconds = model->now;
	if (model_type(&model->inodes[a]) == BTRFS_MODE_DIRECTORY) {
		model->inodes[a].frozen = 1;
	}
	if (model_type(&model->inodes[b]) == BTRFS_MODE_DIRECTORY) {
		model->inodes[b].frozen = 1;
	}
	return 1;
}

static int
random_rename(struct model *model, struct plan *plan, int whiteout)
{
	char source[RANDOM_PATH];
	char target[RANDOM_PATH];
	uint32_t entry = model_pick_entry(model, accept_any);
	uint32_t directory = model_pick_inode(model, BTRFS_MODE_DIRECTORY);
	uint32_t moved;
	uint32_t name;
	uint32_t existing;
	uint32_t old_parent;
	uint32_t old_name;
	uint32_t whiteout_inode;
	int directory_moved;
	int open = 0;

	if (entry == MODEL_NONE || directory == MODEL_NONE) {
		return 0;
	}
	old_parent = model->entries[entry].parent;
	old_name = model->entries[entry].name;
	moved = model->entries[entry].child;
	directory_moved = model_type(&model->inodes[moved]) == BTRFS_MODE_DIRECTORY;
	if (directory_moved && model_below_directory(model, directory, moved)) {
		return 0;
	}
	name = model_below(model, RANDOM_NAMES);
	existing = model_find(model, directory, name);
	if (existing != MODEL_NONE) {
		struct model_inode *replaced = &model->inodes[model->entries[existing].child];

		if (model->entries[existing].child != moved &&
		    ((model_type(replaced) == BTRFS_MODE_DIRECTORY) != directory_moved ||
			(directory_moved && !model_empty(model, model->entries[existing].child)) ||
			!model_usable(model, model->entries[existing].child))) {
			return 0;
		}
	}
	model_entry_path(model, entry, source, sizeof(source));
	model_path(model, directory, target, sizeof(target));
	/* A moved directory's subtree must stay within the path bound too. */
	if (strlen(target) > RANDOM_PATH / 4) {
		return 0;
	}
	strcat(target, "/");
	strcat(target, random_names[name]);
	if (existing != MODEL_NONE && model->entries[existing].child == moved) {
		/* Two names of one inode: nothing changes. */
		if (whiteout) {
			plan_rename_whiteout(plan, model->commit, source, target, 0);
		} else {
			plan_rename(plan, model->commit, source, target, 0);
		}
		return 1;
	}
	if (existing != MODEL_NONE &&
	    model_type(&model->inodes[model->entries[existing].child]) == BTRFS_MODE_REGULAR &&
	    model->inodes[model->entries[existing].child].links == 1) {
		open = model_below(model, 4) == 0;
	}
	if (whiteout) {
		plan_rename_whiteout(plan, model->commit, source, target, open);
	} else {
		plan_rename(plan, model->commit, source, target, open);
	}
	model->inodes[model->entries[entry].parent].modify_seconds = model->now;
	model->entries[entry].used = 0;
	if (existing != MODEL_NONE) {
		model_remove_entry(model, existing, open);
	}
	model_add_entry(model, directory, name, moved);
	if (directory_moved) {
		model->inodes[moved].frozen = 1;
	}
	if (whiteout) {
		/* The whiteout under the old name; the harness resolves it by name
		 * only from the next commit on. */
		whiteout_inode = model_new_inode(model, BTRFS_MODE_CHARACTER);
		model_add_entry(model, old_parent, old_name, whiteout_inode);
		model->inodes[whiteout_inode].frozen = 1;
	}
	return 1;
}

static uint32_t
model_pick_data_inode(struct model *model)
{
	uint32_t inode = model_pick_inode(
	    model, model_below(model, 3) == 0 ? BTRFS_MODE_DIRECTORY : BTRFS_MODE_REGULAR);

	return inode;
}

static int
random_set_xattr(struct model *model, struct plan *plan, int refusal)
{
	uint8_t value[MODEL_XATTR_VALUE];
	char path[RANDOM_PATH];
	uint32_t inode = model_pick_data_inode(model);
	uint32_t name = model_below(model, RANDOM_XATTRS);
	size_t size = model_below(model, MODEL_XATTR_VALUE);
	struct model_inode *object;
	int present;
	int flags;

	if (inode == MODEL_NONE) {
		return 0;
	}
	object = &model->inodes[inode];
	present = (object->xattr_present >> name) & 1U;
	model_path(model, inode, path, sizeof(path));
	model_fill(model, value, size);
	if (refusal) {
		flags = present ? BTRFS_XATTR_CREATE : BTRFS_XATTR_REPLACE;
		plan_set_xattr(plan, model->commit, path, random_xattrs[name], value, size, flags);
		plan_expect_refusal(plan, model->commit, present ? BTRFS_EXISTS : BTRFS_NOT_FOUND);
		return 1;
	}
	flags = present ? (model_below(model, 2) ? BTRFS_XATTR_REPLACE : 0)
			: (model_below(model, 2) ? BTRFS_XATTR_CREATE : 0);
	plan_set_xattr(plan, model->commit, path, random_xattrs[name], value, size, flags);
	memcpy(object->xattr_values[name], value, size);
	object->xattr_sizes[name] = size;
	object->xattr_present |= 1U << name;
	object->xattr_seen |= 1U << name;
	return 1;
}

static int
random_remove_xattr(struct model *model, struct plan *plan)
{
	char path[RANDOM_PATH];
	uint32_t inode = model_pick_data_inode(model);
	uint32_t name = model_below(model, RANDOM_XATTRS);
	struct model_inode *object;

	if (inode == MODEL_NONE) {
		return 0;
	}
	object = &model->inodes[inode];
	model_path(model, inode, path, sizeof(path));
	plan_remove_xattr(plan, model->commit, path, random_xattrs[name]);
	if (((object->xattr_present >> name) & 1U) == 0) {
		plan_expect_refusal(plan, model->commit, BTRFS_NOT_FOUND);
		return 1;
	}
	object->xattr_present &= ~(1U << name);
	return 1;
}

static int
random_write(struct model *model, struct plan *plan)
{
	uint8_t *bytes;
	char path[RANDOM_PATH];
	uint32_t inode = model_pick_inode(model, BTRFS_MODE_REGULAR);
	struct model_inode *file;
	size_t offset;
	size_t size;

	if (inode == MODEL_NONE) {
		return 0;
	}
	file = &model->inodes[inode];
	offset = model_below(model, (uint32_t)(file->size + MODEL_GROWTH));
	size = 1 + model_below(model, MODEL_WRITE_LIMIT);
	if (offset + size > MODEL_FILE_LIMIT) {
		return 0;
	}
	model_path(model, inode, path, sizeof(path));
	model_privileges(model, plan, inode, path);
	bytes = malloc(size);
	REQUIRE(bytes != NULL);
	model_fill(model, bytes, size);
	plan_write_new(plan, model->commit, path, offset, bytes, size);
	if (offset + size > file->size) {
		model_resize(file, offset + size);
	}
	memcpy(file->data + offset, bytes, size);
	file->modify_seconds = model->now;
	free(bytes);
	return 1;
}

static int
random_truncate(struct model *model, struct plan *plan)
{
	char path[RANDOM_PATH];
	uint32_t inode = model_pick_inode(model, BTRFS_MODE_REGULAR);
	struct model_inode *file;
	size_t size;

	if (inode == MODEL_NONE) {
		return 0;
	}
	file = &model->inodes[inode];
	size = model_below(model, (uint32_t)(file->size + MODEL_GROWTH));
	model_path(model, inode, path, sizeof(path));
	model_privileges(model, plan, inode, path);
	plan_truncate_new(plan, model->commit, path, size);
	model_resize(file, size);
	file->modify_seconds = model->now;
	return 1;
}

static int
random_attributes(struct model *model, struct plan *plan)
{
	char path[RANDOM_PATH];
	uint32_t inode = model_pick_inode(model, 0);
	struct model_inode *object;
	unsigned mask = 0;
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	int64_t access;
	int64_t modify;

	if (inode == MODEL_NONE) {
		return 0;
	}
	object = &model->inodes[inode];
	mode = random_permissions[model_below(model, 5)];
	/* Regular files sometimes get set-id bits, which later writes settle. */
	if (model_type(object) == BTRFS_MODE_REGULAR && model_below(model, 4) == 0) {
		mode |= model_below(model, 2) ? MODE_SET_UID : MODE_SET_GID | MODE_GROUP_EXECUTE;
	}
	uid = random_owners[model_below(model, 3)];
	gid = random_owners[model_below(model, 3)];
	access = 1500000000 + (int64_t)model_below(model, 100000000);
	modify = 1500000000 + (int64_t)model_below(model, 100000000);
	while (mask == 0) {
		mask = model_below(model, 32);
	}
	model_path(model, inode, path, sizeof(path));
	plan_set_attributes(plan, model->commit, path, mask, mode, uid, gid, access, modify);
	if (mask & BTRFS_ATTRIBUTE_MODE) {
		object->mode = (object->mode & BTRFS_MODE_TYPE) | mode;
	}
	if (mask & BTRFS_ATTRIBUTE_UID) {
		object->uid = uid;
	}
	if (mask & BTRFS_ATTRIBUTE_GID) {
		object->gid = gid;
	}
	if (mask & BTRFS_ATTRIBUTE_ACCESS_TIME) {
		object->access_seconds = access;
	}
	if (mask & BTRFS_ATTRIBUTE_MODIFY_TIME) {
		object->modify_seconds = modify;
	}
	return 1;
}

static int
random_clean(struct model *model, struct plan *plan)
{
	size_t orphans = 0;
	uint32_t i;

	for (i = 0; i < MODEL_INODES; i++) {
		if (model->inodes[i].live && model->inodes[i].orphan) {
			orphans++;
		}
	}
	if (orphans == 0) {
		return 0;
	}
	plan_clean(plan, model->commit, BTRFS_TOP_LEVEL_TREE, orphans);
	for (i = 0; i < MODEL_INODES; i++) {
		if (model->inodes[i].live && model->inodes[i].orphan) {
			model_delete_inode(model, i);
		}
	}
	return 1;
}

/* Operations the writer must refuse before any change. */
static int
random_refusal(struct model *model, struct plan *plan)
{
	char source[RANDOM_PATH];
	char target[RANDOM_PATH];
	uint32_t entry;
	uint32_t directory;
	uint32_t inode;

	switch (model_below(model, 5)) {
	case 0:
		entry = model_pick_entry(model, accept_any);
		if (entry == MODEL_NONE) {
			return 0;
		}
		model_entry_path(model, entry, source, sizeof(source));
		plan_create(plan, model->commit, source, BTRFS_MODE_REGULAR | 0644, NULL);
		plan_expect_refusal(plan, model->commit, BTRFS_EXISTS);
		return 1;
	case 1:
		entry = model_pick_entry(model, accept_full_directory);
		if (entry == MODEL_NONE) {
			return 0;
		}
		model_entry_path(model, entry, source, sizeof(source));
		plan_unlink(plan, model->commit, source, 0);
		plan_expect_refusal(plan, model->commit, BTRFS_NOT_EMPTY);
		return 1;
	case 2:
		inode = model_pick_inode(model, BTRFS_MODE_DIRECTORY);
		directory = model_pick_inode(model, BTRFS_MODE_DIRECTORY);
		if (inode == MODEL_NONE || directory == MODEL_NONE || inode == MODEL_ROOT ||
		    !model_below_directory(model, directory, inode)) {
			return 0;
		}
		model_path(model, inode, source, sizeof(source));
		model_path(model, directory, target, sizeof(target));
		strcat(target, "/inside");
		plan_rename(plan, model->commit, source, target, 0);
		plan_expect_refusal(plan, model->commit, BTRFS_INVALID_ARGUMENT);
		return 1;
	case 3:
		inode = model_pick_inode(model, BTRFS_MODE_DIRECTORY);
		directory = model_pick_inode(model, BTRFS_MODE_DIRECTORY);
		if (inode == MODEL_NONE || directory == MODEL_NONE || inode == MODEL_ROOT) {
			return 0;
		}
		model_path(model, inode, source, sizeof(source));
		model_path(model, directory, target, sizeof(target));
		strcat(target, "/hardlink");
		plan_link(plan, model->commit, source, target);
		plan_expect_refusal(plan, model->commit, BTRFS_IS_DIRECTORY);
		return 1;
	default:
		directory = model_pick_inode(model, BTRFS_MODE_DIRECTORY);
		if (directory == MODEL_NONE) {
			return 0;
		}
		model_path(model, directory, source, sizeof(source));
		strcat(source, "/missing");
		plan_unlink(plan, model->commit, source, 0);
		plan_expect_refusal(plan, model->commit, BTRFS_NOT_FOUND);
		return 1;
	}
}

static int
random_operation(struct model *model, struct plan *plan, enum random_kind kind)
{
	switch (kind) {
	case RANDOM_CREATE:
		return random_create(
		    model, plan, BTRFS_MODE_REGULAR | random_permissions[model_below(model, 5)]);
	case RANDOM_MKDIR:
		return random_create(model, plan, BTRFS_MODE_DIRECTORY | 0755);
	case RANDOM_SYMLINK:
		return random_create(model, plan, BTRFS_MODE_SYMLINK | 0777);
	case RANDOM_LINK:
		return random_link(model, plan);
	case RANDOM_UNLINK:
		return random_unlink(model, plan);
	case RANDOM_RENAME:
		return random_rename(model, plan, 0);
	case RANDOM_EXCHANGE:
		return random_exchange(model, plan);
	case RANDOM_WHITEOUT:
		return random_rename(model, plan, 1);
	case RANDOM_SET_XATTR:
		return random_set_xattr(model, plan, model_below(model, 8) == 0);
	case RANDOM_REMOVE_XATTR:
		return random_remove_xattr(model, plan);
	case RANDOM_WRITE:
		return random_write(model, plan);
	case RANDOM_TRUNCATE:
		return random_truncate(model, plan);
	case RANDOM_ATTRIBUTES:
		return random_attributes(model, plan);
	case RANDOM_CLEAN:
		return random_clean(model, plan);
	case RANDOM_REFUSAL:
		return random_refusal(model, plan);
	default:
		return 0;
	}
}

static enum random_kind
random_kind(struct model *model)
{
	unsigned total = 0;
	unsigned pick;
	unsigned i;

	for (i = 0; i < RANDOM_KINDS; i++) {
		total += random_weights[i];
	}
	pick = model_below(model, total);
	for (i = 0; pick >= random_weights[i]; i++) {
		pick -= random_weights[i];
	}
	return (enum random_kind)i;
}

static int
compare_name_pointers(const void *left, const void *right)
{
	return strcmp(*(const char *const *)left, *(const char *const *)right);
}

/* The model's state after a commit becomes that stage's expectations. */
static void
model_expect(const struct model *model, struct plan *plan, size_t stage)
{
	const char *names[MODEL_ENTRIES];
	const struct model_inode *inode;
	char path[RANDOM_PATH];
	char other[RANDOM_PATH];
	uint32_t entry;
	uint32_t i;
	uint32_t j;
	size_t count;

	for (i = 0; i < MODEL_INODES; i++) {
		inode = &model->inodes[i];
		if (!inode->live || inode->orphan) {
			continue;
		}
		model_path(model, i, path, sizeof(path));
		expect_owner(
		    plan, stage, stage, path, inode->mode, inode->uid, inode->gid, inode->links);
		expect_times(
		    plan, stage, stage, path, inode->access_seconds, inode->modify_seconds);
		switch (model_type(inode)) {
		case BTRFS_MODE_REGULAR:
			expect_file(plan, stage, stage, path, inode->data, inode->size);
			break;
		case BTRFS_MODE_SYMLINK:
			expect_symlink(plan, stage, stage, path, inode->target);
			break;
		case BTRFS_MODE_CHARACTER:
			/* A whiteout: device 0:0. */
			expect_value(plan, stage, stage, EXPECT_DEVICE, path, 0);
			break;
		default:
			count = 0;
			for (j = 0; j < MODEL_ENTRIES; j++) {
				if (model->entries[j].used && model->entries[j].parent == i) {
					names[count++] = random_names[model->entries[j].name];
				}
			}
			qsort(names, count, sizeof(names[0]), compare_name_pointers);
			expect_names(plan, stage, stage, path, names, count);
			break;
		}
		for (j = 0; j < RANDOM_XATTRS; j++) {
			if ((inode->xattr_seen >> j) & 1U) {
				expect_xattr(plan, stage, stage, path, random_xattrs[j],
				    ((inode->xattr_present >> j) & 1U) ? inode->xattr_values[j]
								       : NULL,
				    inode->xattr_sizes[j]);
			}
		}
		entry = model_entry_of(model, i);
		for (j = 0; j < MODEL_ENTRIES; j++) {
			if (model->entries[j].used && model->entries[j].child == i && j != entry &&
			    i != MODEL_ROOT) {
				model_entry_path(model, j, other, sizeof(other));
				expect_same(plan, stage, stage, other, path);
			}
		}
	}
}

static void
random_plan(struct context *context, uint32_t seed, int quick)
{
	static char name[32];
	struct model *model;
	struct plan plan;
	size_t commit;
	size_t done;
	size_t attempts;
	uint32_t i;

	model = calloc(1, sizeof(*model));
	REQUIRE(model != NULL);
	model->state = seed * UINT32_C(2654435761) + 1;
	REQUIRE(snprintf(name, sizeof(name), "random-%u", seed) < (int)sizeof(name));
	namespace_plan(context, &plan, name);
	plan.quick = quick;
	plan.prefix_points = RANDOM_PREFIX_POINTS;
	plan.fault_points = RANDOM_FAULT_POINTS;
	expect_absent(&plan, 0, 0, RANDOM_BASE);
	for (commit = 1; commit <= RANDOM_COMMITS; commit++) {
		model->commit = commit;
		/* The harness commits at 1700000000 + commit seconds. */
		model->now = 1700000000 + (int64_t)commit;
		if (commit == 1) {
			plan_create(&plan, 1, RANDOM_BASE, BTRFS_MODE_DIRECTORY | 0755, NULL);
			model->inodes[MODEL_ROOT] = (struct model_inode){ .live = 1,
				.mode = BTRFS_MODE_DIRECTORY | 0755,
				.uid = NAMESPACE_UID,
				.gid = NAMESPACE_GID,
				.links = 1,
				.access_seconds = model->now,
				.modify_seconds = model->now };
		}
		for (i = 0; i < MODEL_INODES; i++) {
			model->inodes[i].frozen = 0;
		}
		for (done = attempts = 0;
		    done < RANDOM_OPERATIONS && attempts < 16 * RANDOM_OPERATIONS; attempts++) {
			done += (size_t)random_operation(model, &plan, random_kind(model));
		}
		model_expect(model, &plan, commit);
	}
	for (i = 0, done = attempts = 0; i <= RANDOM_COMMITS; i++) {
		for (commit = 0; commit < plan.operation_count[i]; commit++) {
			done++;
			attempts += plan.operations[i][commit].expected != BTRFS_OK;
		}
	}
	printf("%s: %zu operations, %zu refusals, %zu expectations\n", name, done, attempts,
	    plan.expectation_count);
	if (getenv("BTRFS_RANDOM_TRACE") != NULL) {
		for (i = 1; i <= RANDOM_COMMITS; i++) {
			for (commit = 0; commit < plan.operation_count[i]; commit++) {
				printf("  %u.%zu kind %d %s -> %s expected %d\n", i, commit,
				    plan.operations[i][commit].kind,
				    plan.operations[i][commit].path,
				    plan.operations[i][commit].target
					? plan.operations[i][commit].target
					: "-",
				    plan.operations[i][commit].expected);
			}
		}
	}
	for (i = 0; i < MODEL_INODES; i++) {
		free(model->inodes[i].data);
	}
	free(model);
	run_plan(context, &plan);
}

void
random_scenarios(struct context *context, uint32_t first, uint32_t count, int quick)
{
	uint32_t seed;

	for (seed = first; seed < first + count; seed++) {
		random_plan(context, seed, quick);
	}
}
