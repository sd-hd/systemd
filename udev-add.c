/*
 * udev-add.c
 *
 * Userspace devfs
 *
 * Copyright (C) 2003 Greg Kroah-Hartman <greg@kroah.com>
 *
 *
 *	This program is free software; you can redistribute it and/or modify it
 *	under the terms of the GNU General Public License as published by the
 *	Free Software Foundation version 2 of the License.
 * 
 *	This program is distributed in the hope that it will be useful, but
 *	WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *	General Public License for more details.
 * 
 *	You should have received a copy of the GNU General Public License along
 *	with this program; if not, write to the Free Software Foundation, Inc.,
 *	675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <grp.h>
#ifndef __KLIBC__
#include <pwd.h>
#endif

#include "udev.h"
#include "udev_version.h"
#include "udev_dbus.h"
#include "namedev.h"
#include "udevdb.h"
#include "libsysfs/libsysfs.h"
#include "klibc_fixups.h"

/* 
 * Right now the major/minor of a device is stored in a file called
 * "dev" in sysfs.
 * The number is stored as:
 * 	MM:mm
 * 		MM is the major
 * 		mm is the minor
 * 		The value is in decimal.
 */
static int get_major_minor(struct sysfs_class_device *class_dev, struct udevice *udev)
{
	int retval = -ENODEV;

	char *dev;

	dev = sysfs_get_value_from_attributes(class_dev->directory->attributes, "dev");
	if (dev == NULL)
		goto exit;

	dbg("dev='%s'", dev);

	if (sscanf(dev, "%u:%u", &udev->major, &udev->minor) != 2)
		goto exit;

	dbg("found major=%d, minor=%d", udev->major, udev->minor);

	retval = 0;
exit:
	return retval;
}

static int create_path(char *file)
{
	char p[NAME_SIZE];
	char *pos;
	int retval;
	struct stat stats;
	
	strncpy(p, file, sizeof(p));
	pos = strchr(p+1, '/');
	while (1) {
		pos = strchr(pos+1, '/');
		if (pos == NULL)
			break;
		*pos = 0x00;
		if (stat(p, &stats)) {
			retval = mkdir(p, 0755);
			if (retval) {
				dbg("mkdir(%s) failed with error '%s'",
				    p, strerror(errno));
				return retval;
			}
			dbg("created '%s'", p);
		}
		*pos = '/';
	}
	return 0;
}

/*
 * we possibly want to add some symlinks here
 * only numeric owner/group id's are supported
 */
static int create_node(struct udevice *dev)
{
	char filename[255];
	char linktarget[255];
	int retval = 0;
	uid_t uid = 0;
	gid_t gid = 0;
	dev_t res;
	int i;
	int tail;


	strncpy(filename, udev_root, sizeof(filename));
	strncat(filename, dev->name, sizeof(filename));

#ifdef __KLIBC__
	res = (dev->major << 8) | (dev->minor);
#else
	res = makedev(dev->major, dev->minor);
#endif

	switch (dev->type) {
	case 'b':
		dev->mode |= S_IFBLK;
		break;
	case 'c':
	case 'u':
		dev->mode |= S_IFCHR;
		break;
	case 'p':
		dev->mode |= S_IFIFO;
		break;
	default:
		dbg("unknown node type %c\n", dev->type);
		return -EINVAL;
	}

	/* create parent directories if needed */
	if (strrchr(dev->name, '/'))
		create_path(filename);

	dbg("mknod(%s, %#o, %u, %u)", filename, dev->mode, dev->major, dev->minor);
	retval = mknod(filename, dev->mode, res);
	if (retval)
		dbg("mknod(%s, %#o, %u, %u) failed with error '%s'",
		    filename, dev->mode, dev->major, dev->minor, strerror(errno));

	dbg("chmod(%s, %#o)", filename, dev->mode);
	retval = chmod(filename, dev->mode);
	if (retval)
		dbg("chmod(%s, %#o) failed with error '%s'",
		    filename, dev->mode, strerror(errno));

	if (*dev->owner) {
		char *endptr;
		unsigned long id = strtoul(dev->owner, &endptr, 10);
		if (*endptr == 0x00)
			uid = (uid_t) id;
		else {
			struct passwd *pw = getpwnam(dev->owner);
			if (!pw)
				dbg("user unknown '%s'", dev->owner);
			else
				uid = pw->pw_uid;
		}
	}

	if (*dev->group) {
		char *endptr;
		unsigned long id = strtoul(dev->group, &endptr, 10);
		if (*endptr == 0x00)
			gid = (gid_t) id;
		else {
			struct group *gr = getgrnam(dev->group);
			if (!gr)
				dbg("group unknown '%s'", dev->group);
			else
				gid = gr->gr_gid;
		}
	}

	if (uid || gid) {
		dbg("chown(%s, %u, %u)", filename, uid, gid);
		retval = chown(filename, uid, gid);
		if (retval)
			dbg("chown(%s, %u, %u) failed with error '%s'",
			    filename, uid, gid, strerror(errno));
	}


	/* create symlink if requested */
	if (*dev->symlink) {
		strncpy(filename, udev_root, sizeof(filename));
		strncat(filename, dev->symlink, sizeof(filename));
		dbg("symlink '%s' to node '%s' requested", filename, dev->name);
		if (strrchr(dev->symlink, '/'))
			create_path(filename);

		/* optimize relative link */
		linktarget[0] = '\0';
		i = 0;
		tail = 0;
		while ((dev->name[i] == dev->symlink[i]) && dev->name[i]) {
			if (dev->name[i] == '/')
				tail = i+1;
			i++;
		}
		while (dev->symlink[i]) {
			if (dev->symlink[i] == '/')
				strcat(linktarget, "../");
			i++;
		}

		if (*linktarget == '\0')
			strcpy(linktarget, "./");
		strcat(linktarget, &dev->name[tail]);

		dbg("symlink(%s, %s)", linktarget, filename);
		retval = symlink(linktarget, filename);
		if (retval)
			dbg("symlink(%s, %s) failed with error '%s'",
			    linktarget, filename, strerror(errno));
	}

	return retval;
}

static struct sysfs_class_device *get_class_dev(char *device_name)
{
	char dev_path[SYSFS_PATH_MAX];
	struct sysfs_class_device *class_dev = NULL;

	strcpy(dev_path, sysfs_path);
	strcat(dev_path, device_name);

	dbg("looking at '%s'", dev_path);

	/* open up the sysfs class device for this thing... */
	class_dev = sysfs_open_class_device(dev_path);
	if (class_dev == NULL) {
		dbg ("sysfs_open_class_device failed");
		goto exit;
	}
	dbg("class_dev->name='%s'", class_dev->name);

exit:
	return class_dev;
}

/* wait for the "dev" file to show up in the directory in sysfs.
 * If it doesn't happen in about 10 seconds, give up.
 */
#define SECONDS_TO_WAIT_FOR_DEV		10
static int sleep_for_dev(char *path)
{
	char filename[SYSFS_PATH_MAX + 6];
	int loop = SECONDS_TO_WAIT_FOR_DEV;
	int retval;

	strcpy(filename, sysfs_path);
	strcat(filename, path);
	strcat(filename, "/dev");

	while (loop--) {
		struct stat buf;

		dbg("looking for '%s'", filename);
		retval = stat(filename, &buf);
		if (!retval)
			goto exit;

		/* sleep to give the kernel a chance to create the dev file */
		sleep(1);
	}
	retval = -ENODEV;
exit:
	return retval;
}

int udev_add_device(char *path, char *subsystem)
{
	struct sysfs_class_device *class_dev = NULL;
	struct udevice dev;
	int retval = -EINVAL;

	memset(&dev, 0x00, sizeof(dev));

	/* for now, the block layer is the only place where block devices are */
	if (strcmp(subsystem, "block") == 0)
		dev.type = 'b';
	else
		dev.type = 'c';

	retval = sleep_for_dev(path);
	if (retval)
		goto exit;

	class_dev = get_class_dev(path);
	if (class_dev == NULL)
		goto exit;

	retval = get_major_minor(class_dev, &dev);
	if (retval) {
		dbg("get_major_minor failed");
		goto exit;
	}

	retval = namedev_name_device(class_dev, &dev);
	if (retval)
		goto exit;

	retval = udevdb_add_dev(path, &dev);
	if (retval != 0)
		dbg("udevdb_add_dev failed, but we are going to try to create the node anyway. "
		    "But remove might not work properly for this device.");

	dbg("name='%s'", dev.name);
	retval = create_node(&dev);

	if (retval == 0)
		sysbus_send_create(&dev, path);

exit:
	if (class_dev)
		sysfs_close_class_device(class_dev);

	return retval;
}
