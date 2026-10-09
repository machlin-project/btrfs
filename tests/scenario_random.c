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
#define COMPRESSION_PROPERTY "btrfs.compression"
/* Live subvolumes the model keeps at most, and the node budget that drops
 * every deleted one at once. */
#define MODEL_SUBVOLUMES 8U
#define MODEL_DROP_BUDGET 4096U
#define STUB_MODE (BTRFS_MODE_DIRECTORY | 0755U)
/* The codec FS_IOC_SETFLAGS records without a mount codec. */
#define COMPRESSION_CODEC "zlib"
/* The inode flags FS_IOC_SETFLAGS and inheritance decide. */
#define MODEL_FLAGS                                                                                \
	(BT_INODE_SYNC | BT_INODE_IMMUTABLE | BT_INODE_APPEND | BT_INODE_NODUMP |                  \
	    BT_INODE_NOATIME | BT_INODE_DIRSYNC | BT_INODE_NODATACOW | BT_INODE_NODATASUM |        \
	    BT_INODE_COMPRESS | BT_INODE_NOCOMPRESS)

static const unsigned random_attribute_flags[] = { BTRFS_FS_SYNC_FL, BTRFS_FS_NODUMP_FL,
	BTRFS_FS_NOATIME_FL, BTRFS_FS_DIRSYNC_FL };
#define RANDOM_ATTRIBUTE_FLAGS (sizeof(random_attribute_flags) / sizeof(random_attribute_flags[0]))

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
	RANDOM_FALLOCATE,
	RANDOM_FSFLAGS,
	RANDOM_SUBVOLUME,
	RANDOM_DROP,
	RANDOM_KINDS
};

/* Weights of the operation kinds, in random_kind order. */
static const unsigned random_weights[RANDOM_KINDS] = { 9, 4, 3, 4, 6, 7, 5, 2, 12, 4, 4, 1, 4, 2, 2,
	3, 3, 2, 1 };
/* Allocation and zeroing; a punch entirely within a hole leaves the times,
 * which this model, holding bytes and not extents, cannot predict. */
static const unsigned random_fallocate_modes[] = { 0, BTRFS_FALLOCATE_KEEP_SIZE,
	BTRFS_FALLOCATE_ZERO_RANGE, BTRFS_FALLOCATE_ZERO_RANGE | BTRFS_FALLOCATE_KEEP_SIZE };

struct model_inode {
	int live;
	int orphan;
	int frozen;
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	uint32_t links;
	/* Inode flags (MODEL_FLAGS) and the compression property, which
	 * expectations check once they have been set or inherited. */
	uint64_t flags;
	int property;
	int flags_seen;
	/* A subvolume's root directory (read-only for a read-only snapshot, a
	 * snapshot of source while that lives), or a stub: the empty directory
	 * a snapshot copies for a subvolume entry. */
	int subvolume;
	int read_only;
	int snapshot;
	uint32_t source;
	int stub;
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
	/* Deleted subvolumes the cleaner has not dropped. */
	size_t deleted;
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

/* The subvolume holding inode: its own root for a subvolume, MODEL_ROOT for
 * the top level, which holds RANDOM_BASE. */
static uint32_t
model_tree(const struct model *model, uint32_t inode)
{
	uint32_t entry;

	for (;;) {
		if (inode == MODEL_ROOT || model->inodes[inode].subvolume) {
			return inode;
		}
		entry = model_entry_of(model, inode);
		REQUIRE(entry != MODEL_NONE);
		inode = model->entries[entry].parent;
	}
}

/* Whether inode's tree accepts changes to it: a stub is no inode, and a
 * read-only snapshot refuses them. */
static int
model_writable(const struct model *model, uint32_t inode)
{
	return !model->inodes[inode].stub && !model->inodes[model_tree(model, inode)].read_only;
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
		    model_usable(model, i) && model_writable(model, i)) {
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
		    model_writable(model, model->entries[i].parent) &&
		    !model->inodes[model->entries[i].child].frozen &&
		    !model->inodes[model->entries[i].child].stub &&
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

static int
model_can_compress(uint64_t flags)
{
	return (flags & (BT_INODE_NODATACOW | BT_INODE_NODATASUM)) == 0;
}

/* A new inode's flags and compression property from its directory, as
 * btrfs_inherit_iflags and btrfs_inode_inherit_props pass them. */
static void
model_inherit(struct model *model, uint32_t inode, uint32_t directory)
{
	struct model_inode *child = &model->inodes[inode];
	const struct model_inode *parent = &model->inodes[directory];
	uint32_t type = model_type(child);

	child->flags = 0;
	if ((parent->flags & BT_INODE_NOCOMPRESS) != 0) {
		child->flags |= BT_INODE_NOCOMPRESS;
	} else if ((parent->flags & BT_INODE_COMPRESS) != 0) {
		child->flags |= BT_INODE_COMPRESS;
	}
	if ((parent->flags & BT_INODE_NODATACOW) != 0) {
		child->flags |= BT_INODE_NODATACOW;
		if (type == BTRFS_MODE_REGULAR) {
			child->flags |= BT_INODE_NODATASUM;
		}
	}
	child->property = (type == BTRFS_MODE_REGULAR || type == BTRFS_MODE_DIRECTORY) &&
	    parent->property && model_can_compress(parent->flags) &&
	    model_can_compress(child->flags);
	if (child->property) {
		child->flags = (child->flags & ~BT_INODE_NOCOMPRESS) | BT_INODE_COMPRESS;
	}
	child->flags_seen = child->flags != 0 || parent->flags_seen;
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

	/* unlink and rmdir leave subvolume entries alone (CROSS_TREE). */
	return !child->subvolume &&
	    (model_type(child) != BTRFS_MODE_DIRECTORY ||
		model_empty(model, model->entries[entry].child));
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
	    !model->inodes[child].subvolume && !model_empty(model, child);
}

static int
accept_subvolume(const struct model *model, uint32_t entry)
{
	return model->inodes[model->entries[entry].child].subvolume;
}

static int
accept_read_only(const struct model *model, uint32_t entry)
{
	return model->inodes[model->entries[entry].child].read_only;
}

static int
accept_inode(const struct model *model, uint32_t entry)
{
	return !model->inodes[model->entries[entry].child].subvolume;
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
	model_inherit(model, inode, directory);
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

	/* A link stays in its subvolume (CROSS_TREE, refused separately). */
	if (inode == MODEL_NONE || directory == MODEL_NONE ||
	    model->inodes[inode].links >= MODEL_LINK_LIMIT ||
	    model_tree(model, inode) != model_tree(model, directory)) {
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
	/* An open regular file keeps its last name's inode as an orphan; orphan
	 * cleanup runs on the top level. */
	open = model_type(child) == BTRFS_MODE_REGULAR && child->links == 1 &&
	    model_tree(model, model->entries[entry].parent) == MODEL_ROOT &&
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
	/* Subvolume entries cross subvolumes, other names do not; a read-only
	 * subvolume stays in its directory. */
	if (model_tree(model, model->entries[first].parent) !=
		model_tree(model, model->entries[second].parent) &&
	    !(model->inodes[a].subvolume && model->inodes[b].subvolume)) {
		return 0;
	}
	if (model->entries[first].parent != model->entries[second].parent &&
	    (model->inodes[a].read_only || model->inodes[b].read_only)) {
		return 0;
	}
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
	/* A subvolume entry may move into another subvolume, other names may
	 * not; a read-only subvolume stays in its directory. */
	if (!model->inodes[moved].subvolume &&
	    model_tree(model, directory) != model_tree(model, old_parent)) {
		return 0;
	}
	if (model->inodes[moved].read_only && directory != old_parent) {
		return 0;
	}
	name = model_below(model, RANDOM_NAMES);
	existing = model_find(model, directory, name);
	if (existing != MODEL_NONE) {
		struct model_inode *replaced = &model->inodes[model->entries[existing].child];

		if (model->entries[existing].child != moved &&
		    (replaced->subvolume ||
			(model_type(replaced) == BTRFS_MODE_DIRECTORY) != directory_moved ||
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
	    model->inodes[model->entries[existing].child].links == 1 &&
	    model_tree(model, directory) == MODEL_ROOT) {
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
		model_inherit(model, whiteout_inode, old_parent);
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
random_fallocate(struct model *model, struct plan *plan)
{
	char path[RANDOM_PATH];
	uint32_t inode = model_pick_inode(model, BTRFS_MODE_REGULAR);
	struct model_inode *file;
	unsigned mode;
	size_t offset;
	size_t length;
	size_t end;

	if (inode == MODEL_NONE) {
		return 0;
	}
	file = &model->inodes[inode];
	mode = random_fallocate_modes[model_below(
	    model, sizeof(random_fallocate_modes) / sizeof(random_fallocate_modes[0]))];
	offset = model_below(model, (uint32_t)(file->size + MODEL_GROWTH));
	length = 1 + model_below(model, MODEL_WRITE_LIMIT);
	end = offset + length;
	if (end > MODEL_FILE_LIMIT) {
		return 0;
	}
	model_path(model, inode, path, sizeof(path));
	model_privileges(model, plan, inode, path);
	plan_fallocate_new(plan, model->commit, path, mode, offset, length);
	if ((mode & BTRFS_FALLOCATE_ZERO_RANGE) != 0 && offset < file->size) {
		memset(file->data + offset, 0, (end < file->size ? end : file->size) - offset);
	}
	if ((mode & BTRFS_FALLOCATE_KEEP_SIZE) == 0 && end > file->size) {
		model_resize(file, end);
	}
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

/* The FS_*_FL flags btrfs_inode_fsflags reports for the model's flags. */
static unsigned
model_fsflags(const struct model_inode *inode)
{
	static const struct {
		uint64_t inode;
		unsigned attribute;
	} map[] = { { BT_INODE_SYNC, BTRFS_FS_SYNC_FL },
		{ BT_INODE_IMMUTABLE, BTRFS_FS_IMMUTABLE_FL },
		{ BT_INODE_APPEND, BTRFS_FS_APPEND_FL }, { BT_INODE_NODUMP, BTRFS_FS_NODUMP_FL },
		{ BT_INODE_NOATIME, BTRFS_FS_NOATIME_FL },
		{ BT_INODE_DIRSYNC, BTRFS_FS_DIRSYNC_FL },
		{ BT_INODE_NODATACOW, BTRFS_FS_NOCOW_FL } };

	unsigned result = 0;
	size_t i;

	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		if ((inode->flags & map[i].inode) != 0) {
			result |= map[i].attribute;
		}
	}
	if ((inode->flags & BT_INODE_NOCOMPRESS) != 0) {
		result |= BTRFS_FS_NOCOMP_FL;
	} else if ((inode->flags & BT_INODE_COMPRESS) != 0) {
		result |= BTRFS_FS_COMPR_FL;
	}
	return result;
}

static uint64_t
model_flag(uint64_t flags, uint64_t bit, unsigned set)
{
	return set != 0 ? flags | bit : flags & ~bit;
}

/* Applies FS_IOC_SETFLAGS to the model as btrfs_fileattr_set does: Linux's
 * mask for the type, check_fsflags and the compression check, then the
 * attribute flags, NOCOW (a regular file's only while it is empty) and the
 * compression flags with their property. Returns the refusal, if any. */
static enum btrfs_result
model_set_fsflags(struct model_inode *object, unsigned flags)
{
	uint32_t type = model_type(object);
	unsigned old_flags = model_fsflags(object);
	uint64_t result = object->flags;

	if (type == BTRFS_MODE_REGULAR) {
		flags &= ~BTRFS_FS_DIRSYNC_FL;
	} else if (type != BTRFS_MODE_DIRECTORY) {
		flags &= BTRFS_FS_NODUMP_FL | BTRFS_FS_NOATIME_FL;
	}
	if (((flags & BTRFS_FS_NOCOMP_FL) && (flags & BTRFS_FS_COMPR_FL)) ||
	    ((flags & BTRFS_FS_COMPR_FL) && (flags & BTRFS_FS_NOCOW_FL)) ||
	    ((old_flags & BTRFS_FS_NOCOW_FL) &&
		(flags & (BTRFS_FS_COMPR_FL | BTRFS_FS_NOCOMP_FL))) ||
	    ((flags & BTRFS_FS_NOCOW_FL) &&
		(old_flags & (BTRFS_FS_COMPR_FL | BTRFS_FS_NOCOMP_FL))) ||
	    ((flags & BTRFS_FS_COMPR_FL) && !model_can_compress(object->flags))) {
		return BTRFS_INVALID_ARGUMENT;
	}
	result = model_flag(result, BT_INODE_SYNC, flags & BTRFS_FS_SYNC_FL);
	result = model_flag(result, BT_INODE_IMMUTABLE, flags & BTRFS_FS_IMMUTABLE_FL);
	result = model_flag(result, BT_INODE_APPEND, flags & BTRFS_FS_APPEND_FL);
	result = model_flag(result, BT_INODE_NODUMP, flags & BTRFS_FS_NODUMP_FL);
	result = model_flag(result, BT_INODE_NOATIME, flags & BTRFS_FS_NOATIME_FL);
	result = model_flag(result, BT_INODE_DIRSYNC, flags & BTRFS_FS_DIRSYNC_FL);
	if (type != BTRFS_MODE_REGULAR) {
		result = model_flag(result, BT_INODE_NODATACOW, flags & BTRFS_FS_NOCOW_FL);
	} else if (object->size == 0) {
		result = model_flag(
		    result, BT_INODE_NODATACOW | BT_INODE_NODATASUM, flags & BTRFS_FS_NOCOW_FL);
	}
	object->property = 0;
	if ((flags & BTRFS_FS_NOCOMP_FL) != 0) {
		result = (result & ~BT_INODE_COMPRESS) | BT_INODE_NOCOMPRESS;
	} else if ((flags & BTRFS_FS_COMPR_FL) != 0) {
		result = (result & ~BT_INODE_NOCOMPRESS) | BT_INODE_COMPRESS;
		object->property = 1;
	} else {
		result &= ~(BT_INODE_COMPRESS | BT_INODE_NOCOMPRESS);
	}
	object->flags = result;
	object->flags_seen = 1;
	return BTRFS_OK;
}

/* FS_IOC_SETFLAGS as chattr sets it: attribute flags, NOCOW and the
 * compression flags. Sometimes a file is made immutable with its other flags
 * set again, refuses a write, and gets them back. */
static int
random_fsflags(struct model *model, struct plan *plan)
{
	static const uint8_t byte = 0x5a;
	char path[RANDOM_PATH];
	uint32_t inode = model_pick_inode(model, 0);
	struct model_inode *object;
	unsigned old_flags;
	unsigned flags = 0;
	enum btrfs_result result;
	size_t i;

	if (inode == MODEL_NONE) {
		return 0;
	}
	object = &model->inodes[inode];
	old_flags = model_fsflags(object);
	model_path(model, inode, path, sizeof(path));
	if (model_type(object) == BTRFS_MODE_REGULAR &&
	    (object->mode & (MODE_SET_UID | MODE_SET_GID)) == 0 && model_below(model, 4) == 0) {
		plan_set_fsflags(plan, model->commit, path, old_flags | BTRFS_FS_IMMUTABLE_FL);
		result = model_set_fsflags(object, old_flags | BTRFS_FS_IMMUTABLE_FL);
		if (result != BTRFS_OK) {
			plan_expect_refusal(plan, model->commit, result);
			return 1;
		}
		plan_write_new(plan, model->commit, path, object->size, &byte, 1);
		plan_expect_refusal(plan, model->commit, BTRFS_NOT_PERMITTED);
		plan_set_fsflags(plan, model->commit, path, old_flags);
		REQUIRE(model_set_fsflags(object, old_flags) == BTRFS_OK);
		return 1;
	}
	for (i = 0; i < RANDOM_ATTRIBUTE_FLAGS; i++) {
		if (model_below(model, 3) == 0) {
			flags |= random_attribute_flags[i];
		}
	}
	switch (model_below(model, 4)) {
	case 0:
		flags |= BTRFS_FS_COMPR_FL;
		break;
	case 1:
		flags |= BTRFS_FS_NOCOMP_FL;
		break;
	default:
		break;
	}
	if (model_below(model, 4) == 0) {
		flags |= BTRFS_FS_NOCOW_FL;
	}
	plan_set_fsflags(plan, model->commit, path, flags);
	result = model_set_fsflags(object, flags);
	if (result != BTRFS_OK) {
		plan_expect_refusal(plan, model->commit, result);
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

static size_t
model_subvolumes(const struct model *model)
{
	size_t count = 0;
	uint32_t i;

	for (i = 0; i < MODEL_INODES; i++) {
		count += model->inodes[i].live && model->inodes[i].subvolume;
	}
	return count;
}

/* A new subvolume: an empty root directory that inherits no inode flags,
 * only the compression property of the root directory of the subvolume
 * holding it (the top level's has none). */
static int
random_subvolume(struct model *model, struct plan *plan)
{
	char path[RANDOM_PATH];
	uint32_t directory = model_pick_inode(model, BTRFS_MODE_DIRECTORY);
	uint32_t parent;
	uint32_t inode;
	uint32_t name;

	if (directory == MODEL_NONE || model_subvolumes(model) >= MODEL_SUBVOLUMES) {
		return 0;
	}
	name = model_free_name(model, directory);
	if (name == MODEL_NONE) {
		return 0;
	}
	model_path(model, directory, path, sizeof(path));
	if (strlen(path) > RANDOM_PATH / 2) {
		return 0;
	}
	strcat(path, "/");
	strcat(path, random_names[name]);
	plan_subvolume(plan, model->commit, path);
	parent = model_tree(model, directory);
	inode = model_new_inode(model, BTRFS_MODE_DIRECTORY | 0755);
	model->inodes[inode].subvolume = 1;
	model->inodes[inode].source = MODEL_NONE;
	if (parent != MODEL_ROOT && model->inodes[parent].property) {
		model->inodes[inode].flags = BT_INODE_COMPRESS;
		model->inodes[inode].property = 1;
		model->inodes[inode].flags_seen = 1;
	}
	model_add_entry(model, directory, name, inode);
	return 1;
}

static void
model_link_entry(struct model *model, uint32_t directory, uint32_t name, uint32_t child)
{
	uint32_t i;

	for (i = 0; i < MODEL_ENTRIES && model->entries[i].used; i++) {
	}
	REQUIRE(i < MODEL_ENTRIES);
	model->entries[i] = (struct model_entry){ 1, directory, child, name };
}

/* The stub a snapshot copies for a subvolume entry. */
static uint32_t
model_new_stub(struct model *model)
{
	uint32_t stub = model_new_inode(model, STUB_MODE);

	model->inodes[stub].stub = 1;
	model->inodes[stub].uid = 0;
	model->inodes[stub].gid = 0;
	return stub;
}

/* Copies inode and, for a directory, everything below it in its subvolume,
 * as a snapshot shares them: a subvolume entry or a stub becomes a stub, and
 * names of one inode stay one inode (copies maps originals to copies). */
static uint32_t
model_copy(struct model *model, uint32_t inode, uint32_t *copies)
{
	uint32_t copy;
	uint32_t child;
	uint32_t i;

	if (copies[inode] != MODEL_NONE) {
		return copies[inode];
	}
	copy = model_new_inode(model, model->inodes[inode].mode);
	model->inodes[copy] = model->inodes[inode];
	model->inodes[copy].data = NULL;
	if (model->inodes[inode].size != 0 &&
	    model_type(&model->inodes[inode]) == BTRFS_MODE_REGULAR) {
		model->inodes[copy].data = malloc(model->inodes[inode].size + 1);
		REQUIRE(model->inodes[copy].data != NULL);
		memcpy(
		    model->inodes[copy].data, model->inodes[inode].data, model->inodes[inode].size);
	}
	copies[inode] = copy;
	if (model_type(&model->inodes[inode]) != BTRFS_MODE_DIRECTORY) {
		return copy;
	}
	for (i = 0; i < MODEL_ENTRIES; i++) {
		if (!model->entries[i].used || model->entries[i].parent != inode) {
			continue;
		}
		child = model->entries[i].child;
		if (model->inodes[child].subvolume || model->inodes[child].stub) {
			child = model_new_stub(model);
		} else {
			child = model_copy(model, child, copies);
		}
		model_link_entry(model, copy, model->entries[i].name, child);
	}
	return copy;
}

/* Inodes and entries in inode's subtree within its subvolume. */
static uint32_t
model_count(const struct model *model, uint32_t inode)
{
	uint32_t count = 1;
	uint32_t child;
	uint32_t i;

	for (i = 0; i < MODEL_ENTRIES; i++) {
		if (model->entries[i].used && model->entries[i].parent == inode) {
			child = model->entries[i].child;
			count += model->inodes[child].subvolume || model->inodes[child].stub
			    ? 1
			    : model_count(model, child);
		}
	}
	return count;
}

static uint32_t
model_free_inodes(const struct model *model)
{
	uint32_t count = 0;
	uint32_t i;

	for (i = 1; i < MODEL_INODES; i++) {
		count += !model->inodes[i].live;
	}
	return count;
}

static uint32_t
model_free_entries(const struct model *model)
{
	uint32_t count = 0;
	uint32_t i;

	for (i = 0; i < MODEL_ENTRIES; i++) {
		count += !model->entries[i].used;
	}
	return count;
}

/* A snapshot, writable or read-only, of a subvolume the transaction has not
 * changed yet (its first operation): the copy is taken before its entry is
 * added, even inside its source. Paths inside the copy are named from the
 * next commit on. */
static int
random_snapshot(struct model *model, struct plan *plan)
{
	uint32_t copies[MODEL_INODES];
	uint32_t candidates[MODEL_INODES];
	char source[RANDOM_PATH];
	char target[RANDOM_PATH];
	uint32_t count = 0;
	uint32_t original;
	uint32_t directory;
	uint32_t name;
	uint32_t copy;
	uint32_t i;
	int read_only;

	for (i = 0; i < MODEL_INODES; i++) {
		if (model->inodes[i].live && model->inodes[i].subvolume && model_usable(model, i)) {
			candidates[count++] = i;
		}
	}
	directory = model_pick_inode(model, BTRFS_MODE_DIRECTORY);
	if (count == 0 || directory == MODEL_NONE || model_subvolumes(model) >= MODEL_SUBVOLUMES) {
		return 0;
	}
	original = candidates[model_below(model, count)];
	count = model_count(model, original);
	name = model_free_name(model, directory);
	if (name == MODEL_NONE || count + 1 > model_free_inodes(model) ||
	    count + 1 > model_free_entries(model)) {
		return 0;
	}
	model_path(model, original, source, sizeof(source));
	model_path(model, directory, target, sizeof(target));
	if (strlen(target) > RANDOM_PATH / 4) {
		return 0;
	}
	strcat(target, "/");
	strcat(target, random_names[name]);
	read_only = model_below(model, 3) == 0;
	plan_snapshot(plan, model->commit, source, target, read_only);
	for (i = 0; i < MODEL_INODES; i++) {
		copies[i] = MODEL_NONE;
	}
	copy = model_copy(model, original, copies);
	model->inodes[copy].read_only = read_only;
	model->inodes[copy].snapshot = 1;
	model->inodes[copy].source = original;
	model->inodes[copy].frozen = 1;
	model_add_entry(model, directory, name, copy);
	return 1;
}

/* Deletes inode and everything below it in its subvolume. */
static void
model_delete_tree(struct model *model, uint32_t inode)
{
	uint32_t i;

	for (i = 0; i < MODEL_ENTRIES; i++) {
		if (model->entries[i].used && model->entries[i].parent == inode) {
			model->entries[i].used = 0;
			if (model->inodes[model->entries[i].child].live) {
				model_delete_tree(model, model->entries[i].child);
			}
		}
	}
	for (i = 0; i < MODEL_INODES; i++) {
		if (model->inodes[i].source == inode && model->inodes[i].snapshot) {
			model->inodes[i].source = MODEL_NONE;
		}
	}
	model_delete_inode(model, inode);
}

/* Whether a subvolume holds another subvolume's entry. */
static int
model_holds_subvolume(const struct model *model, uint32_t inode)
{
	uint32_t child;
	uint32_t i;

	for (i = 0; i < MODEL_ENTRIES; i++) {
		if (model->entries[i].used && model->entries[i].parent == inode) {
			child = model->entries[i].child;
			if (model->inodes[child].subvolume ||
			    (!model->inodes[child].stub &&
				model_type(&model->inodes[child]) == BTRFS_MODE_DIRECTORY &&
				model_holds_subvolume(model, child))) {
				return 1;
			}
		}
	}
	return 0;
}

/* Deletes a subvolume the transaction has not opened yet (its first
 * operation), or a stub alone; one holding a subvolume is NOT_EMPTY. */
static int
random_delete_subvolume(struct model *model, struct plan *plan)
{
	uint32_t candidates[MODEL_ENTRIES];
	char path[RANDOM_PATH];
	uint32_t count = 0;
	uint32_t entry;
	uint32_t child;
	uint32_t i;

	for (i = 0; i < MODEL_ENTRIES; i++) {
		child = model->entries[i].child;
		if (model->entries[i].used &&
		    (model->inodes[child].subvolume || model->inodes[child].stub) &&
		    model_usable(model, model->entries[i].parent) &&
		    model_writable(model, model->entries[i].parent)) {
			candidates[count++] = i;
		}
	}
	if (count == 0) {
		return 0;
	}
	entry = candidates[model_below(model, count)];
	child = model->entries[entry].child;
	model_entry_path(model, entry, path, sizeof(path));
	plan_delete_subvolume(plan, model->commit, path);
	if (model->inodes[child].subvolume && model_holds_subvolume(model, child)) {
		plan_expect_refusal(plan, model->commit, BTRFS_NOT_EMPTY);
		return 1;
	}
	model->entries[entry].used = 0;
	model->inodes[model->entries[entry].parent].modify_seconds = model->now;
	model->deleted += model->inodes[child].subvolume;
	model_delete_tree(model, child);
	return 1;
}

/* The cleaner drops every deleted subvolume. */
static int
random_drop(struct model *model, struct plan *plan)
{
	if (model->deleted == 0) {
		return 0;
	}
	plan_clean_subvolumes(plan, model->commit, MODEL_DROP_BUDGET, model->deleted, 0);
	model->deleted = 0;
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

	switch (model_below(model, 10)) {
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
	case 4:
		directory = model_pick_inode(model, BTRFS_MODE_DIRECTORY);
		if (directory == MODEL_NONE) {
			return 0;
		}
		model_path(model, directory, source, sizeof(source));
		strcat(source, "/missing");
		plan_unlink(plan, model->commit, source, 0);
		plan_expect_refusal(plan, model->commit, BTRFS_NOT_FOUND);
		return 1;
	case 5:
		/* unlink and rmdir leave subvolume entries alone. */
		entry = model_pick_entry(model, accept_subvolume);
		if (entry == MODEL_NONE) {
			return 0;
		}
		model_entry_path(model, entry, source, sizeof(source));
		plan_unlink(plan, model->commit, source, 0);
		plan_expect_refusal(plan, model->commit, BTRFS_CROSS_TREE);
		return 1;
	case 6:
		/* Other names stay in their subvolume; the VFS refuses a
		 * directory below itself first. */
		entry = model_pick_entry(model, accept_inode);
		directory = model_pick_inode(model, BTRFS_MODE_DIRECTORY);
		if (entry == MODEL_NONE || directory == MODEL_NONE ||
		    model_tree(model, directory) ==
			model_tree(model, model->entries[entry].parent)) {
			return 0;
		}
		inode = model->entries[entry].child;
		model_entry_path(model, entry, source, sizeof(source));
		model_path(model, directory, target, sizeof(target));
		strcat(target, "/moved");
		plan_rename(plan, model->commit, source, target, 0);
		plan_expect_refusal(plan, model->commit,
		    model_type(&model->inodes[inode]) == BTRFS_MODE_DIRECTORY &&
			    model_below_directory(model, directory, inode)
			? BTRFS_INVALID_ARGUMENT
			: BTRFS_CROSS_TREE);
		return 1;
	case 7:
		/* A read-only snapshot takes no names. */
		for (inode = 0; inode < MODEL_INODES; inode++) {
			if (model->inodes[inode].live && model->inodes[inode].read_only &&
			    model_usable(model, inode)) {
				break;
			}
		}
		if (inode == MODEL_INODES) {
			return 0;
		}
		model_path(model, inode, source, sizeof(source));
		strcat(source, "/inside");
		plan_create(plan, model->commit, source, BTRFS_MODE_REGULAR | 0644, NULL);
		plan_expect_refusal(plan, model->commit, BTRFS_READ_ONLY);
		return 1;
	case 8:
		/* A read-only subvolume stays in its directory. */
		entry = model_pick_entry(model, accept_read_only);
		directory = model_pick_inode(model, BTRFS_MODE_DIRECTORY);
		if (entry == MODEL_NONE || directory == MODEL_NONE ||
		    directory == model->entries[entry].parent ||
		    model_below_directory(model, directory, model->entries[entry].child)) {
			return 0;
		}
		model_entry_path(model, entry, source, sizeof(source));
		model_path(model, directory, target, sizeof(target));
		strcat(target, "/moved");
		plan_rename(plan, model->commit, source, target, 0);
		plan_expect_refusal(plan, model->commit, BTRFS_READ_ONLY);
		return 1;
	default:
		/* A stub does not move. */
		for (entry = 0; entry < MODEL_ENTRIES; entry++) {
			if (model->entries[entry].used &&
			    model->inodes[model->entries[entry].child].stub &&
			    model_usable(model, model->entries[entry].parent) &&
			    model_writable(model, model->entries[entry].parent)) {
				break;
			}
		}
		if (entry == MODEL_ENTRIES) {
			return 0;
		}
		model_entry_path(model, entry, source, sizeof(source));
		model_path(model, model->entries[entry].parent, target, sizeof(target));
		strcat(target, "/moved");
		plan_rename(plan, model->commit, source, target, 0);
		plan_expect_refusal(plan, model->commit, BTRFS_NOT_EMPTY);
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
	case RANDOM_FALLOCATE:
		return random_fallocate(model, plan);
	case RANDOM_ATTRIBUTES:
		return random_attributes(model, plan);
	case RANDOM_FSFLAGS:
		return random_fsflags(model, plan);
	case RANDOM_SUBVOLUME:
		return random_subvolume(model, plan);
	case RANDOM_DROP:
		return random_drop(model, plan);
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
		if (inode->stub) {
			/* Linux makes a stub's times when it looks one up. */
			expect_owner(plan, stage, stage, path, STUB_MODE, 0, 0, 1);
			expect_names(plan, stage, stage, path, NULL, 0);
			continue;
		}
		if (inode->subvolume && (!inode->snapshot || inode->source != MODEL_NONE)) {
			if (inode->snapshot) {
				model_path(model, inode->source, other, sizeof(other));
			}
			expect_subvolume(plan, stage, stage, path, inode->snapshot ? other : NULL,
			    inode->read_only);
		}
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
		if (inode->flags_seen) {
			expect_flags(plan, stage, stage, path, MODEL_FLAGS, inode->flags);
			if (model_type(inode) == BTRFS_MODE_REGULAR ||
			    model_type(inode) == BTRFS_MODE_DIRECTORY) {
				expect_xattr(plan, stage, stage, path, COMPRESSION_PROPERTY,
				    inode->property ? COMPRESSION_CODEC : NULL,
				    inode->property ? strlen(COMPRESSION_CODEC) : 0);
			}
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
		/* Snapshots and deletions open trees the transaction has not
		 * changed or opened: the commit's first operation. */
		switch (model_below(model, 4)) {
		case 0:
			(void)random_snapshot(model, &plan);
			break;
		case 1:
			(void)random_delete_subvolume(model, &plan);
			break;
		default:
			break;
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
				printf("  %u.%zu kind %d %s -> %s flags 0x%x expected %d\n", i,
				    commit, plan.operations[i][commit].kind,
				    plan.operations[i][commit].path,
				    plan.operations[i][commit].target
					? plan.operations[i][commit].target
					: "-",
				    (unsigned)plan.operations[i][commit].flags,
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
