#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <linux/btrfs.h>
#include <sys/ioctl.h>
#include "util.h"

bool for_each_device(const char *mountpoint, int fd, bool (*cb)(const struct btrfs_ioctl_fs_info_args *, const struct btrfs_ioctl_dev_info_args *, void *), void *cookie) {
	struct btrfs_ioctl_fs_info_args fs_info;
	if(ioctl(fd, BTRFS_IOC_FS_INFO, &fs_info) < 0) {
		perror(mountpoint);
		return false;
	}

	for(uint64_t dev_id = 0, found = 0; found < fs_info.num_devices; ++dev_id) {
		if(dev_id > fs_info.max_id) {
			fprintf(stderr, "%s: expected to find %" PRIu64 " devices but only found %" PRIu64 "\n", mountpoint, (uint64_t) fs_info.num_devices, found);
			return false;
		}
		struct btrfs_ioctl_dev_info_args dev_info = {
			.devid = dev_id,
		};
		if(ioctl(fd, BTRFS_IOC_DEV_INFO, &dev_info) >= 0) {
			if(!cb(&fs_info, &dev_info, cookie)) {
				return true;
			}
			++found;
		} else if(errno == ENODEV) {
			// The device ID numbering space is sparse. Go on to the next
			// potential device ID.
		} else {
			perror(mountpoint);
			return false;
		}
	}

	return true;
}
