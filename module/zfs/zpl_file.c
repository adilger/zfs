/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2011, Lawrence Livermore National Security, LLC.
 */


#include <sys/zfs_vfsops.h>
#include <sys/zfs_vnops.h>
#include <sys/zfs_znode.h>
#include <sys/zpl.h>


static int
zpl_open(struct inode *ip, struct file *filp)
{
	cred_t *cr = CRED();
	int error;

	crhold(cr);
	error = -zfs_open(ip, filp->f_mode, filp->f_flags, cr);
	crfree(cr);
	ASSERT3S(error, <=, 0);

	if (error)
		return (error);

	return generic_file_open(ip, filp);
}

static int
zpl_release(struct inode *ip, struct file *filp)
{
	cred_t *cr = CRED();
	int error;

	crhold(cr);
	error = -zfs_close(ip, filp->f_flags, cr);
	crfree(cr);
	ASSERT3S(error, <=, 0);

	return (error);
}

static int
zpl_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct dentry *dentry = filp->f_path.dentry;
	cred_t *cr = CRED();
	int error;

	crhold(cr);
	error = -zfs_readdir(dentry->d_inode, dirent, filldir,
	    &filp->f_pos, cr);
	crfree(cr);
	ASSERT3S(error, <=, 0);

	return (error);
}

/*
 * 2.6.35 API change,
 * As of 2.6.35 the dentry argument to the .fsync() vfs hook was deemed
 * redundant.  The dentry is still accessible via filp->f_path.dentry,
 * and we are guaranteed that filp will never be NULL.
 *
 * 2.6.34 API change,
 * Prior to 2.6.34 the nfsd kernel server would pass a NULL file struct *
 * to the .fsync() hook.  For this reason, we must be careful not to use
 * filp unconditionally in the 3 argument case.
 */
#ifdef HAVE_2ARGS_FSYNC
static int
zpl_fsync(struct file *filp, int datasync)
{
	struct dentry *dentry = filp->f_path.dentry;
#else
static int
zpl_fsync(struct file *filp, struct dentry *dentry, int datasync)
{
#endif /* HAVE_2ARGS_FSYNC */
	cred_t *cr = CRED();
	int error;

	crhold(cr);
	error = -zfs_fsync(dentry->d_inode, datasync, cr);
	crfree(cr);
	ASSERT3S(error, <=, 0);

	return (error);
}

ssize_t
zpl_read_common(struct inode *ip, const char *buf, size_t len, loff_t pos,
     uio_seg_t segment, int flags, cred_t *cr)
{
	int error;
	struct iovec iov;
	uio_t uio;

	iov.iov_base = (void *)buf;
	iov.iov_len = len;

	uio.uio_iov = &iov;
	uio.uio_resid = len;
	uio.uio_iovcnt = 1;
	uio.uio_loffset = pos;
	uio.uio_limit = MAXOFFSET_T;
	uio.uio_segflg = segment;

	error = -zfs_read(ip, &uio, flags, cr);
	if (error < 0)
		return (error);

	return (len - uio.uio_resid);
}

static ssize_t
zpl_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
	cred_t *cr = CRED();
	ssize_t read;

	crhold(cr);
	read = zpl_read_common(filp->f_mapping->host, buf, len, *ppos,
	    UIO_USERSPACE, filp->f_flags, cr);
	crfree(cr);

	if (read < 0)
		return (read);

	*ppos += read;
	return (read);
}

ssize_t
zpl_write_common(struct inode *ip, const char *buf, size_t len, loff_t pos,
    uio_seg_t segment, int flags, cred_t *cr)
{
	int error;
	struct iovec iov;
	uio_t uio;

	iov.iov_base = (void *)buf;
	iov.iov_len = len;

	uio.uio_iov = &iov;
	uio.uio_resid = len,
	uio.uio_iovcnt = 1;
	uio.uio_loffset = pos;
	uio.uio_limit = MAXOFFSET_T;
	uio.uio_segflg = segment;

	error = -zfs_write(ip, &uio, flags, cr);
	if (error < 0)
		return (error);

	return (len - uio.uio_resid);
}

static ssize_t
zpl_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{
	cred_t *cr = CRED();
	ssize_t wrote;

	crhold(cr);
	wrote = zpl_write_common(filp->f_mapping->host, buf, len, *ppos,
	    UIO_USERSPACE, filp->f_flags, cr);
	crfree(cr);

	if (wrote < 0)
		return (wrote);

	*ppos += wrote;
	return (wrote);
}

/*
 * It's worth taking a moment to describe how mmap is implemented
 * for zfs because it differs considerably from other Linux filesystems.
 * However, this issue is handled the same way under OpenSolaris.
 *
 * The issue is that by design zfs bypasses the Linux page cache and
 * leaves all caching up to the ARC.  This has been shown to work
 * well for the common read(2)/write(2) case.  However, mmap(2)
 * is problem because it relies on being tightly integrated with the
 * page cache.  To handle this we cache mmap'ed files twice, once in
 * the ARC and a second time in the page cache.  The code is careful
 * to keep both copies synchronized.
 *
 * When a file with an mmap'ed region is written to using write(2)
 * both the data in the ARC and existing pages in the page cache
 * are updated.  For a read(2) data will be read first from the page
 * cache then the ARC if needed.  Neither a write(2) or read(2) will
 * will ever result in new pages being added to the page cache.
 *
 * New pages are added to the page cache only via .readpage() which
 * is called when the vfs needs to read a page off disk to back the
 * virtual memory region.  These pages may be modified without
 * notifying the ARC and will be written out periodically via
 * .writepage().  This will occur due to either a sync or the usual
 * page aging behavior.  Note because a read(2) of a mmap'ed file
 * will always check the page cache first even when the ARC is out
 * of date correct data will still be returned.
 *
 * While this implementation ensures correct behavior it does have
 * have some drawbacks.  The most obvious of which is that it
 * increases the required memory footprint when access mmap'ed
 * files.  It also adds additional complexity to the code keeping
 * both caches synchronized.
 *
 * Longer term it may be possible to cleanly resolve this wart by
 * mapping page cache pages directly on to the ARC buffers.  The
 * Linux address space operations are flexible enough to allow
 * selection of which pages back a particular index.  The trick
 * would be working out the details of which subsystem is in
 * charge, the ARC, the page cache, or both.  It may also prove
 * helpful to move the ARC buffers to a scatter-gather lists
 * rather than a vmalloc'ed region.
 */
static int
zpl_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct inode *ip = filp->f_mapping->host;
	znode_t *zp = ITOZ(ip);
	int error;

	error = -zfs_map(ip, vma->vm_pgoff, (caddr_t *)vma->vm_start,
	    (size_t)(vma->vm_end - vma->vm_start), vma->vm_flags);
	if (error)
		return (error);

	error = generic_file_mmap(filp, vma);
	if (error)
		return (error);

	mutex_enter(&zp->z_lock);
	zp->z_is_mapped = 1;
	mutex_exit(&zp->z_lock);

	return (error);
}

static struct page **
pages_vector_from_list(struct list_head *pages, unsigned nr_pages)
{
	struct page **pl;
	struct page *t;
	unsigned page_idx;

	pl = kmalloc(sizeof(*pl) * nr_pages, GFP_NOFS);
	if (!pl)
		return ERR_PTR(-ENOMEM);

	page_idx = 0;
	list_for_each_entry_reverse(t, pages, lru) {
		pl[page_idx] = t;
		page_idx++;
	}

	return pl;
}

static int
zpl_readpages(struct file *file, struct address_space *mapping,
	struct list_head *pages, unsigned nr_pages)
{
	struct inode *ip;
	struct page  **pl;
	struct page  *p, *n;
	int          error;

	ip = mapping->host;

	pl = pages_vector_from_list(pages, nr_pages);
	if (IS_ERR(pl))
		return PTR_ERR(pl);

	error = -zfs_getpage(ip, pl, nr_pages);
	if (error)
		goto error;

	list_for_each_entry_safe_reverse(p, n, pages, lru) {

		list_del(&p->lru);

		flush_dcache_page(p);
		SetPageUptodate(p);
		unlock_page(p);
		page_cache_release(p);
	}

error:
	kfree(pl);
	return error;
}

/*
 * Populate a page with data for the Linux page cache.  This function is
 * only used to support mmap(2).  There will be an identical copy of the
 * data in the ARC which is kept up to date via .write() and .writepage().
 *
 * Current this function relies on zpl_read_common() and the O_DIRECT
 * flag to read in a page.  This works but the more correct way is to
 * update zfs_fillpage() to be Linux friendly and use that interface.
 */
static int
zpl_readpage(struct file *filp, struct page *pp)
{
	struct inode *ip;
	struct page *pl[1];
	int error = 0;

	ASSERT(PageLocked(pp));
	ip = pp->mapping->host;
	pl[0] = pp;

	error = -zfs_getpage(ip, pl, 1);

	if (error) {
		SetPageError(pp);
		ClearPageUptodate(pp);
	} else {
		ClearPageError(pp);
		SetPageUptodate(pp);
		flush_dcache_page(pp);
	}

	unlock_page(pp);
	return error;
}

int
zpl_putpage(struct page *pp, struct writeback_control *wbc, void *data)
{
	int error;

	error = -zfs_putpage(pp, wbc, data);

	if (error) {
		SetPageError(pp);
		ClearPageUptodate(pp);
	} else {
		ClearPageError(pp);
		SetPageUptodate(pp);
		flush_dcache_page(pp);
	}

	unlock_page(pp);
	return error;
}

static int
zpl_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
	return write_cache_pages(mapping, wbc, zpl_putpage, mapping);
}

/*
 * Write out dirty pages to the ARC, this function is only required to
 * support mmap(2).  Mapped pages may be dirtied by memory operations
 * which never call .write().  These dirty pages are kept in sync with
 * the ARC buffers via this hook.
 */
static int
zpl_writepage(struct page *pp, struct writeback_control *wbc)
{
	return zpl_putpage(pp, wbc, pp->mapping);
}

/*
 * Map zfs file zp_flags (xvattr) to linux file attributes.  Note this
 * is not a 1-to-1 mapping.  Linux only has equivalent attributes for
 * ZFS_IMMUTABLE and ZFS_APPENDONLY.  The flags ZFS_DIRSYNC, ZFS_SYNC,
 * and ZFS_NOATIME were added for Linux compatibility with 'chattr'.
 * These three new flags do not overlap with any Solaris flags and
 * should be ignored on other platforms.  Long term a 'zattr' utility
 * should be written which can be used to manipulate the rest of the
 * ZFS_* flags in zp_flags.
 */
static unsigned int
zpl_get_ioctl_flags(uint64_t zfs_flags)
{
	unsigned int ioctl_flags = 0;

	if (zfs_flags & ZFS_IMMUTABLE)
		ioctl_flags |= FS_IMMUTABLE_FL;

	if (zfs_flags & ZFS_APPENDONLY)
		ioctl_flags |= FS_APPEND_FL;

	if (zfs_flags & ZFS_NODUMP)
		ioctl_flags |= FS_NODUMP_FL;

	if (zfs_flags & ZFS_DIRSYNC)
		ioctl_flags |= FS_DIRSYNC_FL;

	if (zfs_flags & ZFS_SYNC)
		ioctl_flags |= FS_SYNC_FL;

	if (zfs_flags & ZFS_NOATIME)
		ioctl_flags |= FS_NOATIME_FL;

	return (ioctl_flags & FS_FL_USER_VISIBLE);
}

static int
zpl_ioctl_getflags(struct file *filp, void __user *arg)
{
	struct inode *ip = filp->f_dentry->d_inode;
	unsigned int ioctl_flags;
	uint64_t zfs_flags;
	int error;

	/* Use zfs_getattr() ? */
	error = -zfs_getflags(ip, &zfs_flags);
	if (error)
		return (error);

	ioctl_flags = zpl_get_ioctl_flags(zfs_flags);
	error = copy_to_user(arg, &ioctl_flags, sizeof(ioctl_flags));

	return (error);
}

/* SET ATTR_XVATTR FOR REPLAY, lets update setattr to take xvattrs again */
static int
zpl_ioctl_setflags(struct file *filp, void __user *arg)
{
	struct inode *ip = filp->f_dentry->d_inode;
	unsigned int flags;
	int error;

	error = copy_from_user(&flags, arg, sizeof(flags));
	if (error)
		return (error);

	if ((flags & ~(FS_FL_USER_MODIFIABLE)) || (!is_owner_or_cap(inode)))
		return (-EACCES);

	if (flags & ~(FS_IMMUTABLE_FL | FS_APPEND_FL | FS_NODUMP_FL |
	    FS_DIRSYNC_FL | FS_SYNC_FL | FS_NOATIME_FL))
		return (-EOPNOTSUPP);

	error = -zfs_setattr();

	return (error);
}

static long
zpl_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case ZFS_IOC_GETFLAGS:
		return zpl_ioctl_getflags(filp, arg);
	case ZFS_IOC_SETFLAGS:
		return zpl_ioctl_setflags(filp, arg);
	}

	return (-ENOTTY);
}

const struct address_space_operations zpl_address_space_operations = {
	.readpages	= zpl_readpages,
	.readpage	= zpl_readpage,
	.writepage	= zpl_writepage,
	.writepages     = zpl_writepages,
};

const struct file_operations zpl_file_operations = {
	.open		= zpl_open,
	.release	= zpl_release,
	.llseek		= generic_file_llseek,
	.read		= zpl_read,
	.write		= zpl_write,
	.readdir	= zpl_readdir,
	.mmap		= zpl_mmap,
	.fsync		= zpl_fsync,
	.unlocked_ioctl	= zpl_ioctl,
};

const struct file_operations zpl_dir_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= generic_read_dir,
	.readdir	= zpl_readdir,
	.fsync		= zpl_fsync,
	.unlocked_ioctl	= zpl_ioctl,
};
