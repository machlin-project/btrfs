/* SPDX-License-Identifier: BSD-3-Clause */
#include "btrfs_xnu.h"
#include <mach/kmod.h>

lck_grp_t *btrfs_xnu_locks;
static vfstable_t btrfs_xnu_registration;
static struct vnodeopv_desc *btrfs_xnu_vectors[] = { &btrfs_xnu_vnodeops, NULL };
static struct vfs_fsentry btrfs_xnu_entry = { .vfe_vfsops = &btrfs_xnu_vfsops,
	.vfe_vopcnt = 1,
	.vfe_opvdescs = btrfs_xnu_vectors,
	.vfe_fsname = BTRFS_XNU_NAME,
	.vfe_flags = VFS_TBLTHREADSAFE | VFS_TBLNOTYPENUM | VFS_TBLLOCALVOL | VFS_TBL64BITREADY |
	    VFS_TBLREADDIR_EXTENDED };

static kern_return_t
btrfs_xnu_start(kmod_info_t *module, void *data)
{
	int error;

	(void)module;
	(void)data;
	btrfs_xnu_locks = lck_grp_alloc_init(BTRFS_XNU_NAME, LCK_GRP_ATTR_NULL);
	if (btrfs_xnu_locks == NULL) {
		return KERN_RESOURCE_SHORTAGE;
	}
	error = vfs_fsadd(&btrfs_xnu_entry, &btrfs_xnu_registration);
	if (error != 0) {
		lck_grp_free(btrfs_xnu_locks);
		btrfs_xnu_locks = NULL;
		return KERN_FAILURE;
	}
	return KERN_SUCCESS;
}

static kern_return_t
btrfs_xnu_stop(kmod_info_t *module, void *data)
{
	(void)module;
	(void)data;
	if (vfs_fsremove(btrfs_xnu_registration) != 0) {
		return KERN_FAILURE;
	}
	lck_grp_free(btrfs_xnu_locks);
	btrfs_xnu_locks = NULL;
	return KERN_SUCCESS;
}

KMOD_EXPLICIT_DECL(org.machlin.btrfs.kext, "0.1.0", btrfs_xnu_start, btrfs_xnu_stop)
