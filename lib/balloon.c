/*
 *  Copyright (C) 2008-2012, Parallels, Inc. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <malloc.h>
#include <stdint.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <string.h>

#include "ploop.h"
#include "ploop_if.h"

#define EXT4_IOC_OPEN_BALLOON		_IO('f', 42)


char *mntn2str(int mntn_type)
{
	switch (mntn_type) {
	case PLOOP_MNTN_OFF:
		return "OFF";
	case PLOOP_MNTN_BALLOON:
		return "BALLOON";
	case PLOOP_MNTN_FBLOADED:
		return "FBLOADED";
	case PLOOP_MNTN_TRACK:
		return "TRACK";
	case PLOOP_MNTN_RELOC:
		return "RELOC";
	case PLOOP_MNTN_MERGE:
		return "MERGE";
	case PLOOP_MNTN_GROW:
		return "GROW";
	case PLOOP_MNTN_DISCARD:
		return "DISCARD";
	}

	return "UNKNOWN";
}

static int open_device(const char *device)
{
	int fd = open(device, O_RDONLY);
	if (fd < 0) {
		ploop_err(errno, "Can't open ploop device %s",
			device);
		return -1;
	}
	return fd;
}


#define ioctl_device(fd, req, arg)					\
	({								\
		int __ret = 0;						\
		if (ioctl(fd, req, arg)) {				\
			ploop_err(errno, "Error in ioctl(" #req ")");	\
			__ret = SYSEXIT_DEVIOC;				\
		}							\
		__ret;							\
	 })

static int fsync_balloon(int fd)
{
	if (fsync(fd)) {
		ploop_err(errno, "Can't fsync balloon");
		return(SYSEXIT_FSYNC);
	}
	return 0;
}

/*
 * Open, flock and stat balloon.
 *
 * Returns balloon fd.
 */
int get_balloon(const char *mount_point, struct stat *st, int *outfd)
{
	int fd, fd2;

	if (mount_point == NULL)
		return -1;

	fd = open(mount_point, O_RDONLY);
	if (fd < 0) {
		ploop_err(errno, "Can't open mount_point");
		return(SYSEXIT_OPEN);
	}

	fd2 = ioctl(fd, EXT4_IOC_OPEN_BALLOON, 0);
	close(fd);

	if (fd2 < 0) {
		ploop_err(errno, "Can't ioctl mount_point");
		return(SYSEXIT_DEVIOC);
	}

	if (outfd != NULL) {
		if (flock(fd2, LOCK_EX | LOCK_NB)) {
			close(fd2);
			if (errno == EWOULDBLOCK) {
				ploop_err(0, "Hidden balloon is in use "
					"by someone else!");
				return(SYSEXIT_EBUSY);
			}
			ploop_err(errno, "Can't flock balloon");
			return(SYSEXIT_FLOCK);
		}
		*outfd = fd2;
	}

	if (st != NULL && fstat(fd2, st)) {
		close(fd2);
		ploop_err(errno, "Can't stat balloon");
		return(SYSEXIT_FSTAT);
	}
	if (outfd == NULL)
		close(fd2);

	return 0;
}

static int open_top_delta(const char *device, struct delta *delta, int *lvl)
{
	char *image = NULL;
	char *fmt = NULL;

	if (ploop_get_attr(device, "top", lvl)) {
		ploop_err(0, "Can't find top delta");
		return(SYSEXIT_SYSFS);
	}

	if (find_delta_names(device, *lvl, *lvl, &image, &fmt))
		return(SYSEXIT_SYSFS);

	if (strcmp(fmt, "raw") == 0) {
		ploop_err(0, "Ballooning for raw format is not supported");
		return(SYSEXIT_PARAM);
	}

	if (open_delta(delta, image, O_RDONLY|O_DIRECT, OD_ALLOW_DIRTY)) {
		ploop_err(errno, "open_delta");
		return(SYSEXIT_OPEN);
	}
	return 0;
}

static __u32 *alloc_reverse_map(__u32 len)
{
	__u32 *reverse_map;

	reverse_map = malloc(len * sizeof(__u32));
	if (reverse_map == NULL) {
		ploop_err(errno, "Can't allocate reverse map");
		return NULL;
	}
	return reverse_map;
}

static int do_truncate(int fd, int mntn_type, off_t old_size, off_t new_size)
{
	int ret;

	switch (mntn_type) {
	case PLOOP_MNTN_OFF:
	case PLOOP_MNTN_MERGE:
	case PLOOP_MNTN_GROW:
	case PLOOP_MNTN_TRACK:
		break;
	case PLOOP_MNTN_BALLOON:
		ploop_err(0, "Error: mntn_type is PLOOP_MNTN_BALLOON "
			"after IOC_BALLOON");
		return(SYSEXIT_PROTOCOL);
	case PLOOP_MNTN_FBLOADED:
	case PLOOP_MNTN_RELOC:
		ploop_err(0, "Can't truncate hidden balloon before previous "
		       "balloon operation (%s) is completed. Use \"ploop-balloon "
		       "complete\".", mntn2str(mntn_type));
		return(SYSEXIT_EBUSY);
	default:
		ploop_err(0, "Error: unknown mntn_type (%u)", mntn_type);
		return(SYSEXIT_PROTOCOL);
	}

	if (new_size == old_size) {
		ploop_log(0, "Nothing to do: new_size == old_size");
	} else if (ftruncate(fd, new_size)) {
		ploop_err(errno, "Can't truncate hidden balloon");
		fsync_balloon(fd);
		return(SYSEXIT_FTRUNCATE);
	} else {
		ret = fsync_balloon(fd);
		if (ret)
			return ret;
		ploop_log(0, "Successfully truncated balloon from %llu to %llu bytes",
			(unsigned long long)old_size, (unsigned long long)new_size);
	}
	return 0;
}

static int do_inflate(int fd, int mntn_type, off_t old_size, off_t *new_size, int *drop_state)
{
	struct stat st;
	int err;

	*drop_state = 0;
	switch (mntn_type) {
	case PLOOP_MNTN_BALLOON:
		break;
	case PLOOP_MNTN_MERGE:
	case PLOOP_MNTN_GROW:
	case PLOOP_MNTN_TRACK:
		ploop_err(0, "Can't inflate hidden balloon while another "
			"maintenance operation is in progress (%s)",
			mntn2str(mntn_type));
		return(SYSEXIT_EBUSY);
	case PLOOP_MNTN_FBLOADED:
	case PLOOP_MNTN_RELOC:
		ploop_err(0, "Can't inflate hidden balloon before previous "
			"balloon operation (%s) is completed. Use "
			"\"ploop-balloon complete\".", mntn2str(mntn_type));
		return(SYSEXIT_EBUSY);
	case PLOOP_MNTN_OFF:
		ploop_err(0, "Error: mntn_type is PLOOP_MNTN_OFF after "
			"IOC_BALLOON");
		return(SYSEXIT_PROTOCOL);
	default:
		ploop_err(0, "Error: unknown mntn_type (%u)", mntn_type);
		return(SYSEXIT_PROTOCOL);
	}
	err = sys_fallocate(fd, 0, 0, *new_size);
	if (err)
		ploop_err(errno, "Can't fallocate balloon");

	if (fstat(fd, &st)) {
		ploop_err(errno, "Can't stat balloon (2)");
		if (ftruncate(fd, old_size))
			ploop_err(errno, "Can't revert old_size back");
		return(err ? SYSEXIT_FALLOCATE : SYSEXIT_FSTAT);
	}

	if (err) {
		if (st.st_size != old_size) {
			if (ftruncate(fd, old_size))
				ploop_err(errno, "Can't revert old_size back (2)");
			else
				*drop_state = 1;
		}
		return(SYSEXIT_FALLOCATE);
	}

	if (st.st_size < *new_size) {
		ploop_err(0, "Error: after fallocate(%d, 0, 0, %llu) fstat "
			"reported size == %llu", fd,
				(unsigned long long)*new_size, (unsigned long long)st.st_size);
		if (ftruncate(fd, old_size))
			ploop_err(errno, "Can't revert old_size back (3)");
		else
			*drop_state = 1;
		return(SYSEXIT_FALLOCATE);
	}
	*new_size = st.st_size;

	err = fsync_balloon(fd);
	if (err)
		return err;

	ploop_log(0, "Successfully inflated balloon from %llu to %llu bytes",
			(unsigned long long)old_size, (unsigned long long)*new_size);
	return 0;
}

int ploop_balloon_change_size(const char *device, int balloonfd, off_t new_size)
{
	int    fd = -1;
	int    ret;
	off_t  old_size;
	__u32  dev_start;  /* /sys/block/ploop0/ploop0p1/start */
	__u32  n_free_blocks;
	__u32  freezed_a_h;
	struct ploop_balloon_ctl    b_ctl;
	struct stat		    st;
	struct pfiemap		   *pfiemap = NULL;
	struct freemap		   *freemap = NULL;
	struct freemap		   *rangemap = NULL;
	struct relocmap		   *relocmap = NULL;
	struct ploop_freeblks_ctl  *freeblks = NULL;
	struct ploop_relocblks_ctl *relocblks = NULL;
	__u32 *reverse_map = NULL;
	__u32  reverse_map_len;
	int top_level;
	struct delta delta = { .fd = -1 };
	int entries_used;
	int drop_state = 0;

	if (fstat(balloonfd, &st)) {
		ploop_err(errno, "Can't get balloon file size");
		return SYSEXIT_FSTAT;
	}

	old_size = st.st_size;
	new_size = (S2B(new_size) + st.st_blksize - 1) & ~(st.st_blksize - 1);

	ploop_log(0, "Changing balloon size old_size=%ld new_size=%ld",
			(long)old_size, (long)new_size);

	pfiemap = fiemap_alloc(128);
	freemap = freemap_alloc(128);
	rangemap = freemap_alloc(128);
	relocmap = relocmap_alloc(128);
	if (!pfiemap || !freemap || !rangemap || !relocmap) {
		ret = SYSEXIT_MALLOC;
		goto err;
	}

	fd = open_device(device);
	if (fd == -1) {
		ret = SYSEXIT_OPEN;
		goto err;
	}

	memset(&b_ctl, 0, sizeof(b_ctl));
	if (old_size < new_size)
		b_ctl.inflate = 1;
	ret = ioctl_device(fd, PLOOP_IOC_BALLOON, &b_ctl);
	if (ret)
		goto err;

	drop_state = 1;
	if (old_size >= new_size) {
		ret = do_truncate(balloonfd, b_ctl.mntn_type, old_size, new_size);
		goto err;
	}

	if (dev_num2dev_start(device, st.st_dev, &dev_start)) {
		ploop_err(0, "Can't find out offset from start of ploop "
			"device (%s) to start of partition",
			device);
		ret = SYSEXIT_SYSFS;
		goto err;
	}

	ret = open_top_delta(device, &delta, &top_level);
	if (ret)
		goto err;

	ret = do_inflate(balloonfd, b_ctl.mntn_type, old_size, &new_size, &drop_state);
	if (ret)
		goto err;

	reverse_map_len = delta.l2_size + delta.l2_size;
	reverse_map = alloc_reverse_map(reverse_map_len);
	if (reverse_map == NULL) {
		ret = SYSEXIT_MALLOC;
		goto err;
	}

	ret = fiemap_get(balloonfd, S2B(dev_start), old_size, new_size, &pfiemap);
	if (ret)
		goto err;
	fiemap_adjust(pfiemap, delta.blocksize);
	ret = fiemap_build_rmap(pfiemap, reverse_map, reverse_map_len, &delta);
	if (ret)
		goto err;

	ret = rmap2freemap(reverse_map, 0, reverse_map_len, &freemap, &entries_used);
	if (ret)
		goto err;
	if (entries_used == 0) {
		drop_state = 1;
		ploop_log(0, "no unused cluster blocks found");
		goto out;
	}

	ret = freemap2freeblks(freemap, top_level, &freeblks, &n_free_blocks);
	if (ret)
		goto err;
	ret = ioctl_device(fd, PLOOP_IOC_FREEBLKS, freeblks);
	if (ret)
		goto err;
	freezed_a_h = freeblks->alloc_head;
	if (freezed_a_h > reverse_map_len) {
		ploop_err(0, "Image corrupted: a_h=%u > rlen=%u",
			freezed_a_h, reverse_map_len);
		ret = SYSEXIT_PLOOPFMT;
		goto err;
	}

	ret = range_build(freezed_a_h, n_free_blocks, reverse_map, reverse_map_len,
		    &delta, freemap, &rangemap, &relocmap);
	if (ret)
		goto err;

	ret = relocmap2relocblks(relocmap, top_level, freezed_a_h, n_free_blocks,
			   &relocblks);
	if (ret)
		goto err;
	ret = ioctl_device(fd, PLOOP_IOC_RELOCBLKS, relocblks);
	if (ret)
		goto err;
	ploop_log(0, "TRUNCATED: %u cluster-blocks (%llu bytes)",
			relocblks->alloc_head,
			(unsigned long long)(relocblks->alloc_head * S2B(delta.blocksize)));
out:
	ret = 0;
err:
	if (drop_state) {
		memset(&b_ctl, 0, sizeof(b_ctl));
		ioctl(fd, PLOOP_IOC_BALLOON, &b_ctl);
	}
	close(fd);
	free(pfiemap);
	free(freemap);
	free(rangemap);
	free(relocmap);
	free(reverse_map);
	free(freeblks);
	free(relocblks);
	if (delta.fd != -1)
		close_delta(&delta);

	return ret;
}

int ploop_balloon_get_state(const char *device, __u32  *state)
{
	int fd, ret;
	struct ploop_balloon_ctl b_ctl;

	fd = open_device(device);
	if (fd == -1)
		return SYSEXIT_OPEN;

	bzero(&b_ctl, sizeof(b_ctl));
	b_ctl.keep_intact = 1;
	ret = ioctl_device(fd, PLOOP_IOC_BALLOON, &b_ctl);
	if (ret)
		goto err;

	*state = b_ctl.mntn_type;

err:
	close(fd);

	return ret;
}

int ploop_balloon_clear_state(const char *device)
{
	int fd, ret;
	struct ploop_balloon_ctl b_ctl;

	fd = open_device(device);
	if (fd == -1)
		return SYSEXIT_OPEN;

	bzero(&b_ctl, sizeof(b_ctl));
	ret = ioctl_device(fd, PLOOP_IOC_BALLOON, &b_ctl);
	if (ret)
		goto err;

	if (b_ctl.mntn_type != PLOOP_MNTN_OFF) {
		ploop_err(0, "Can't clear stale in-kernel \"BALLOON\" "
				"maintenance state because kernel is in \"%s\" "
				"state now", mntn2str(b_ctl.mntn_type));
		ret = SYSEXIT_EBUSY;
	}
err:
	close(fd);
	return ret;
}

static int ploop_balloon_relocation(int fd, struct ploop_balloon_ctl *b_ctl, const char *device)
{
	int    ret = -1;
	__u32  n_free_blocks = 0;
	__u32  freezed_a_h;
	struct freemap		   *freemap = NULL;
	struct freemap		   *rangemap = NULL;
	struct relocmap		   *relocmap = NULL;
	struct ploop_freeblks_ctl  *freeblks = NULL;
	struct ploop_relocblks_ctl *relocblks = NULL;;
	__u32 *reverse_map = NULL;
	__u32  reverse_map_len;
	int top_level;
	struct delta delta = {};

	freemap  = freemap_alloc(128);
	rangemap = freemap_alloc(128);
	relocmap = relocmap_alloc(128);
	if (freemap == NULL || rangemap == NULL || relocmap == NULL) {
		ret = SYSEXIT_NOMEM;
		goto err;
	}

	top_level   = b_ctl->level;
	freezed_a_h = b_ctl->alloc_head;

	if (b_ctl->mntn_type == PLOOP_MNTN_RELOC)
		goto reloc;

	if (b_ctl->mntn_type != PLOOP_MNTN_FBLOADED) {
		ploop_err(0, "Error: non-suitable mntn_type (%u)",
			b_ctl->mntn_type);
		ret = SYSEXIT_PROTOCOL;
		goto err;
	}

	ret = freeblks_alloc(&freeblks, 0);
	if (ret)
		goto err;
	ret = ioctl_device(fd, PLOOP_IOC_FBGET, freeblks);
	if (ret)
		goto err;

	if (freeblks->n_extents == 0)
		goto reloc;

	ret = freeblks_alloc(&freeblks, freeblks->n_extents);
	if (ret)
		goto err;
	ret = ioctl_device(fd, PLOOP_IOC_FBGET, freeblks);
	if (ret)
		goto err;

	ret = freeblks2freemap(freeblks, &freemap, &n_free_blocks);
	if (ret)
		goto err;

	ret = open_top_delta(device, &delta, &top_level);
	if (ret)
		goto err;
	reverse_map_len = delta.l2_size + delta.l2_size;
	reverse_map = alloc_reverse_map(reverse_map_len);
	if (reverse_map == NULL) {
		close_delta(&delta);
		ret = SYSEXIT_MALLOC;
		goto err;
	}

	ret = range_build(freezed_a_h, n_free_blocks, reverse_map, reverse_map_len,
		    &delta, freemap, &rangemap, &relocmap);
	close_delta(&delta);
	if (ret)
		goto err;
reloc:
	ret = relocmap2relocblks(relocmap, top_level, freezed_a_h, n_free_blocks,
			   &relocblks);
	if (ret)
		goto err;
	ret = ioctl_device(fd, PLOOP_IOC_RELOCBLKS, relocblks);
	if (ret)
		goto err;

	ploop_log(0, "TRUNCATED: %u cluster-blocks (%llu bytes)",
			relocblks->alloc_head,
			(unsigned long long)(relocblks->alloc_head * S2B(delta.blocksize)));
err:

	free(freemap);
	free(rangemap);
	free(relocmap);
	free(reverse_map);
	free(freeblks);
	free(relocblks);

	return ret;
}

int ploop_balloon_complete(const char *device)
{
	int fd, err;
	struct ploop_balloon_ctl b_ctl;

	fd = open_device(device);
	if (fd == -1)
		return -1;

	err = ioctl_device(fd, PLOOP_IOC_DISCARD_FINI, NULL);
	if (err && errno != EBUSY) {
		ploop_err(errno, "Can't finalize discard mode");
		goto out;
	}

	memset(&b_ctl, 0, sizeof(b_ctl));
	b_ctl.keep_intact = 1;
	err = ioctl_device(fd, PLOOP_IOC_BALLOON, &b_ctl);
	if (err)
		goto out;

	switch (b_ctl.mntn_type) {
	case PLOOP_MNTN_BALLOON:
	case PLOOP_MNTN_MERGE:
	case PLOOP_MNTN_GROW:
	case PLOOP_MNTN_TRACK:
	case PLOOP_MNTN_OFF:
		ploop_log(0, "Nothing to complete: kernel is in \"%s\" state",
			mntn2str(b_ctl.mntn_type));
		goto out;
	case PLOOP_MNTN_RELOC:
	case PLOOP_MNTN_FBLOADED:
		break;
	default:
		ploop_err(0, "Error: unknown mntn_type (%u)",
			b_ctl.mntn_type);
		err = SYSEXIT_PROTOCOL;
		goto out;
	}

	err = ploop_balloon_relocation(fd, &b_ctl, device);
out:
	close(fd);
	return err;
}

int ploop_balloon_check_and_repair(const char *device, char *mount_point, int repair)
{
	int   ret, fd = -1;
	int   balloonfd = -1;
	__u32 n_free_blocks;
	__u32 freezed_a_h;
	__u32 dev_start;  /* /sys/block/ploop0/ploop0p1/start */
	struct ploop_balloon_ctl    b_ctl;
	struct stat		    st;
	struct pfiemap		   *pfiemap  = NULL;
	struct freemap		   *freemap  = NULL;
	struct freemap		   *rangemap = NULL;
	struct relocmap		   *relocmap = NULL;
	struct ploop_freeblks_ctl  *freeblks = NULL;
	struct ploop_relocblks_ctl *relocblks= NULL;
	char *msg = repair ? "repair" : "check";
	__u32 *reverse_map = NULL;
	__u32  reverse_map_len;
	int top_level;
	int entries_used;
	struct delta delta = {};
	int drop_state = 0;

	ret = get_balloon(mount_point, &st, &balloonfd);
	if (ret)
		return ret;

	if (st.st_size == 0) {
		ploop_log(0, "Nothing to do: hidden balloon is empty");
		close(balloonfd);
		return 0;
	}

	pfiemap = fiemap_alloc(128);
	freemap = freemap_alloc(128);
	rangemap = freemap_alloc(128);
	relocmap = relocmap_alloc(128);
	if (!pfiemap || !freemap || !rangemap || !relocmap) {
		ret = SYSEXIT_MALLOC;
		goto err;
	}

	fd = open_device(device);
	if (fd == -1) {
		ret = SYSEXIT_OPEN;
		goto err;
	}

	memset(&b_ctl, 0, sizeof(b_ctl));
	/* block other maintenance ops even if we only check balloon */
	b_ctl.inflate = 1;
	ret = ioctl_device(fd, PLOOP_IOC_BALLOON, &b_ctl);
	if (ret)
		goto err;

	switch (b_ctl.mntn_type) {
	case PLOOP_MNTN_BALLOON:
		drop_state = 1;
		ret = open_top_delta(device, &delta, &top_level);
		if (ret)
			goto err;
		reverse_map_len = delta.l2_size + delta.l2_size;
		reverse_map = alloc_reverse_map(reverse_map_len);
		if (reverse_map == NULL) {
			ret = SYSEXIT_MALLOC;
			goto err;
		}
		break;
	case PLOOP_MNTN_MERGE:
	case PLOOP_MNTN_GROW:
	case PLOOP_MNTN_TRACK:
		ploop_err(0, "Can't %s hidden balloon while another "
		       "maintenance operation is in progress (%s)",
			msg, mntn2str(b_ctl.mntn_type));
		ret = SYSEXIT_EBUSY;
		goto err;
	case PLOOP_MNTN_FBLOADED:
	case PLOOP_MNTN_RELOC:
		ploop_err(0, "Can't %s hidden balloon before previous "
			"balloon operation (%s) is completed. Use "
			"\"ploop-balloon complete\".",
			msg, mntn2str(b_ctl.mntn_type));
		ret = SYSEXIT_EBUSY;
		goto err;
	case PLOOP_MNTN_OFF:
		ploop_err(0, "Error: mntn_type is PLOOP_MNTN_OFF after "
			"IOC_BALLOON");
		ret = SYSEXIT_PROTOCOL;
		goto err;
	default:
		ploop_err(0, "Error: unknown mntn_type (%u)",
			b_ctl.mntn_type);
		ret = SYSEXIT_PROTOCOL;
		goto err;
	}

	if (dev_num2dev_start(device, st.st_dev, &dev_start)) {
		ploop_err(0, "Can't find out offset from start of ploop "
			"device (%s) to start of partition where fs (%s) "
			"resides", device, mount_point);
		ret = SYSEXIT_SYSFS;
		goto err;
	}

	ret = fiemap_get(balloonfd, S2B(dev_start), 0, st.st_size, &pfiemap);
	if (ret)
		goto err;
	fiemap_adjust(pfiemap, delta.blocksize);

	ret = fiemap_build_rmap(pfiemap, reverse_map, reverse_map_len, &delta);
	if (ret)
		goto err;

	ret = rmap2freemap(reverse_map, 0, reverse_map_len, &freemap, &entries_used);
	if (ret)
		goto err;
	if (entries_used == 0) {
		ploop_log(0, "No free blocks found");
		goto err;
	}

	ret = freemap2freeblks(freemap, top_level, &freeblks, &n_free_blocks);
	if (ret)
		return ret;
	if (!repair) {
		ploop_log(0, "Found %u free blocks. Consider using "
		       "\"ploop-balloon repair\"", n_free_blocks);
		ret = 0;
		goto err;
	} else {
		ploop_log(0, "Found %u free blocks", n_free_blocks);
	}

	ret = ioctl_device(fd, PLOOP_IOC_FREEBLKS, freeblks);
	if (ret)
		return ret;
	drop_state = 0;
	freezed_a_h = freeblks->alloc_head;
	if (freezed_a_h > reverse_map_len) {
		ploop_err(0, "Image corrupted: a_h=%u > rlen=%u",
			freezed_a_h, reverse_map_len);
		ret = SYSEXIT_PLOOPFMT;
		goto err;
	}

	ret = range_build(freezed_a_h, n_free_blocks, reverse_map, reverse_map_len,
		    &delta, freemap, &rangemap, &relocmap);
	if (ret)
		goto err;

	ret = relocmap2relocblks(relocmap, top_level, freezed_a_h, n_free_blocks,
			   &relocblks);
	if (ret)
		goto err;
	ret = ioctl_device(fd, PLOOP_IOC_RELOCBLKS, relocblks);
	if (ret)
		return ret;

	ploop_log(0, "TRUNCATED: %u cluster-blocks (%llu bytes)",
			relocblks->alloc_head,
			(unsigned long long)(relocblks->alloc_head * S2B(delta.blocksize)));

err:
	if (drop_state) {
		memset(&b_ctl, 0, sizeof(b_ctl));
		ioctl(fd, PLOOP_IOC_BALLOON, &b_ctl);
	}

	// FIXME: close_delta()
	close(balloonfd);
	close(fd);
	free(freemap);
	free(rangemap);
	free(relocmap);
	free(reverse_map);
	free(freeblks);
	free(relocblks);

	return ret;
}

static int trim_stop = 0;
static void stop_trim_handler(int sig)
{
	trim_stop = 1;
}

static int ploop_trim(const char *mount_point, __u64 minlen_b)
{
	struct fstrim_range range = {0, ULLONG_MAX, minlen_b};
	int fd, ret;

	struct sigaction sa = {
		.sa_handler     = stop_trim_handler,
	};
	sigemptyset(&sa.sa_mask);

	if (sigaction(SIGUSR1, &sa, NULL)) {
		ploop_err(errno, "Can't set signal handler");
		exit(1);
	}

	fd = open(mount_point, O_RDONLY);
	if (fd < 0) {
		ploop_err(errno, "Can't open mount_point");
		return -1;
	}

	sys_syncfs(fd);

	ret = ioctl(fd, FITRIM, &range);
	if (ret < 0) {
		if (trim_stop)
			ret = 0;
		else
			ploop_err(errno, "Can't trim file system");
	}

	close(fd);

	return ret;
}

#define ilog2(n) (			\
	(n) >= (1 << 9) ? 9 :		\
	(n) & 1 << 8	 ? 8 :		\
	(n) & 1 << 7	 ? 7 :		\
	(n) & 1 << 6	 ? 6 :		\
	(n) & 1 << 5	 ? 5 :		\
	(n) & 1 << 4	 ? 4 :		\
	(n) & 1 << 3	 ? 3 :		\
	(n) & 1 << 2	 ? 2 :		\
	(n) & 1 << 1	 ? 1 :	0	\
)

#define DISCARD_TABLE_SIZE 10

static int discard_collect_stat(int fd, __u32 *distrib)
{
	struct ploop_freeblks_ctl  *freeblks = NULL;
	int ret, i;

	ret = freeblks_alloc(&freeblks, 0);
	if (ret)
		return ret;

	ret = ioctl_device(fd, PLOOP_IOC_FBGET, freeblks);
	if (ret)
		goto free;

	if (freeblks->n_extents == 0) {
		// This should never happen, and we don't want
		// to return an error here.
		ploop_err(EINVAL, "The number of extents is zero");
		goto free;
	}

	ret = freeblks_alloc(&freeblks, freeblks->n_extents);
	if (ret)
		goto free;

	ret = ioctl_device(fd, PLOOP_IOC_FBGET, freeblks);
	if (ret)
		goto free;

	for (i = 0; i < freeblks->n_extents; i++) {
		__u32 len = freeblks->extents[i].len;
		distrib[ilog2(len)] += len;
	}
	ret = ioctl_device(fd, PLOOP_IOC_FBDROP, 0);
	if (ret)
		goto free;
free:
	free(freeblks);
	return ret;
}

enum {
	PLOOP_DISCARD_STAT,
	PLOOP_DISCARD_COMPACT,

	PLOOP_DISCARD_MAX
};

static int __ploop_discard(int fd, const char *device, const char *mount_point,
			int state, __u32 *minlen_c, __u32 cluster, __u32 to_free)
{
	pid_t tpid;
	int exit_code = 0, ret, status;
	__u32 distrib[DISCARD_TABLE_SIZE], size = 0;

	memset(distrib, 0, sizeof(distrib));

	ploop_log(3, "Trying to find free extents bigger than %u clusters", *minlen_c);

	ret = ioctl_device(fd, PLOOP_IOC_DISCARD_INIT, NULL);
	if (ret) {
		ploop_err(errno, "Can't initialize discard mode");
		close(fd);
		return 1;
	}

	tpid = fork();
	if (tpid < 0) {
		ploop_err(errno, "Can't fork");
		if (ioctl_device(fd, PLOOP_IOC_DISCARD_FINI, NULL))
			ploop_err(errno, "Can't finalize discard mode");

		close(fd);
		return -1;
	}

	if (tpid == 0) {
		ret = ploop_trim(mount_point, (__u64) *minlen_c * cluster);
		if (ret < 0)
			exit_code = 1;

		if (ioctl_device(fd, PLOOP_IOC_DISCARD_FINI, NULL))
			ploop_err(errno, "Can't finalize discard mode");

		close(fd);
		exit(exit_code);
	}

	while (1) {
		struct ploop_balloon_ctl b_ctl;

		ploop_log(0, "Waiting");
		ret = ioctl(fd, PLOOP_IOC_DISCARD_WAIT, NULL);
		if (ret < 0) {
			ploop_err(errno, "Waiting for a discard request failed");
			break;
		} else if (ret == 0)
			break;

		ret = ioctl(fd, PLOOP_IOC_FBFILTER, *minlen_c);
		if (ret < 0) {
			ploop_err(errno, "Can't filter free blocks");
			break;
		} else if (ret == 0) {
			/* Nothing to do */
			ret = ioctl_device(fd, PLOOP_IOC_FBDROP, 0);
			if (ret)
				break;
			continue;
		} else
			size += ret;

		switch (state) {
		case PLOOP_DISCARD_COMPACT:
			memset(&b_ctl, 0, sizeof(b_ctl));
			b_ctl.keep_intact = 1;
			ret = ioctl_device(fd, PLOOP_IOC_BALLOON, &b_ctl);
			if (ret)
				break;

			if (b_ctl.mntn_type == PLOOP_MNTN_OFF) {
				ploop_log(0, "Unexpected maintenance type 0x%x", b_ctl.mntn_type);
				ret = -1;
				break;
			}

			if (size >= to_free) {
				ploop_log(3, "Killing the trim process %d", tpid);
				kill(tpid, SIGUSR1);
				ret = ioctl_device(fd, PLOOP_IOC_DISCARD_FINI, NULL);
				if (ret < 0 && errno != EBUSY)
					ploop_err(errno, "Can't finalize a discard mode");
			}

			ploop_log(0, "Starting relocation");
			ret = ploop_balloon_relocation(fd, &b_ctl, device);
			break;
		case PLOOP_DISCARD_STAT:
			ploop_log(0, "Getting extents");
			ret = discard_collect_stat(fd, distrib);
			break;
		default:
			ret = -EINVAL;
			break;
		}

		if (ret)
			break;
	}

	if (ret) {
		exit_code = 1;

		ret = ioctl_device(fd, PLOOP_IOC_DISCARD_FINI, NULL);
		if (ret < 0)
			ploop_err(errno, "Can't finalize discard mode");

		kill(tpid, SIGKILL);
	}

	ret = waitpid(tpid, &status, 0);
	if (ret == -1) {
		ploop_err(errno, "wait() failed");
		exit_code = 1;
	} else if(!WIFEXITED(status) || WEXITSTATUS(status)) {
		if (WIFEXITED(status))
			ploop_err(0, "The trim process failed with code %d",
							WEXITSTATUS(status));
		else
			ploop_err(0, "The trim process killed by signal %d",
							WTERMSIG(status));
		exit_code = 1;
	}

	if (exit_code == 0 && state == PLOOP_DISCARD_STAT) {
		int j;

		for (j = DISCARD_TABLE_SIZE - 1; j >= 0; j--)
			ploop_log(3, "%10d\t%u", j, distrib[j]);

		for (j = DISCARD_TABLE_SIZE - 1; j >= 0; j--) {
			if (to_free <= distrib[j]) {
				*minlen_c = (1 << j);
				break;
			}

			to_free -= distrib[j];
		}
	}

	return exit_code;
}

int ploop_discard(const char *device, const char *mount_point,
				__u64 minlen_b, __u64 to_free)
{
	int fd, ret, state;
	int blocksize;
	__u32  cluster, minlen_c;

	if (ploop_get_attr(device, "block_size", &blocksize)) {
		ploop_err(0, "Can't find block size");
		return SYSEXIT_SYSFS;
	}
	cluster = S2B(blocksize);

	if (minlen_b || to_free == ~0ULL)
		state = PLOOP_DISCARD_COMPACT;
	else
		state = PLOOP_DISCARD_STAT;

	minlen_c = (minlen_b + cluster - 1) / cluster;
	to_free = to_free  / cluster;
	if (!to_free) {
		ploop_err(0, "Can't shrink by less than %d bytes", cluster);
		return 0;
	}

	fd = open_device(device);
	if (fd == -1)
		return SYSEXIT_OPEN;

	for (; state < PLOOP_DISCARD_MAX; state++) {
		ret = __ploop_discard(fd, device, mount_point, state,
					&minlen_c, cluster, to_free);
		if (ret)
			break;
	}

	close(fd);

	return ret;
}

int ploop_complete_running_operation(const char *device)
{
	struct ploop_balloon_ctl b_ctl;
	int fd, ret;

	fd = open_device(device);
	if (fd == -1)
		return SYSEXIT_OPEN;

	bzero(&b_ctl, sizeof(b_ctl));
	b_ctl.keep_intact = 1;
	ret = ioctl_device(fd, PLOOP_IOC_BALLOON, &b_ctl);
	if (ret)
		goto err;
	if (b_ctl.mntn_type == PLOOP_MNTN_OFF)
		goto err;

	ploop_log(0, "Completing an on-going operation %s for device %s",
			mntn2str(b_ctl.mntn_type), device);

	switch (b_ctl.mntn_type) {
		case PLOOP_MNTN_MERGE:
			ret = ioctl(fd, PLOOP_IOC_MERGE, 0);
			if (ret < 0) {
				ploop_err(errno, "PLOOP_IOC_MERGE");
				ret = SYSEXIT_DEVIOC;
			}
			break;
		case PLOOP_MNTN_GROW:
			ret = ioctl(fd, PLOOP_IOC_GROW, 0);
			if (ret < 0) {
				ploop_err(errno, "PLOOP_IOC_GROW");
				ret = SYSEXIT_DEVIOC;
			}
			break;
		case PLOOP_MNTN_RELOC:
		case PLOOP_MNTN_FBLOADED:
			ret = ploop_balloon_complete(device);
			break;
		case PLOOP_MNTN_TRACK:
			ret = ioctl(fd, PLOOP_IOC_TRACK_ABORT, 0);
			if (ret < 0) {
				ploop_err(errno, "PLOOP_IOC_TRACK_ABORT");
				ret = SYSEXIT_DEVIOC;
			}
			break;
		case PLOOP_MNTN_DISCARD:
			/* FIXME: */
			ret = 0;
			break;
		case PLOOP_MNTN_BALLOON:
			/*  FIXME : ploop_balloon_check_and_repair(device, mount_point, 1; */
			ret = 0;
			break;
	}

err:
	close(fd);
	return ret;

}
