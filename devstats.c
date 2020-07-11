#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <linux/btrfs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "ops.h"
#include "util.h"

struct stat_entry {
	unsigned int index;
	const char *name;
};

static const struct stat_entry stat_entries[] = {
	{ .index = BTRFS_DEV_STAT_WRITE_ERRS, .name = "write errors" },
	{ .index = BTRFS_DEV_STAT_READ_ERRS, .name = "read errors" },
	{ .index = BTRFS_DEV_STAT_FLUSH_ERRS, .name = "flush errors" },
	{ .index = BTRFS_DEV_STAT_CORRUPTION_ERRS, .name = "corruption errors" },
	{ .index = BTRFS_DEV_STAT_GENERATION_ERRS, .name = "generation errors" },
};

struct cookie {
	const char *mountpoint;
	bool verbose;
	int fd;
	bool ok;
};

static bool do_devstats_one(const struct btrfs_ioctl_fs_info_args *fs_info, const struct btrfs_ioctl_dev_info_args *dev_info, void *cookie_raw) {
	(void) fs_info;

	struct cookie *cookie = cookie_raw;
	struct btrfs_ioctl_get_dev_stats dev_stats = {
		.devid = dev_info->devid,
		.nr_items = BTRFS_DEV_STAT_VALUES_MAX,
	};
	if(ioctl(cookie->fd, BTRFS_IOC_GET_DEV_STATS, &dev_stats) < 0) {
		perror(cookie->mountpoint);
		cookie->ok = false;
		return false;
	}
	for(size_t i = 0; i != sizeof(stat_entries) / sizeof(*stat_entries); ++i) {
		if(stat_entries[i].index < dev_stats.nr_items) {
			if(dev_stats.values[stat_entries[i].index]) {
				fprintf(stderr, "%s: device ID %" PRIu64 ": nonzero %s: %" PRIu64 "\n", cookie->mountpoint, (uint64_t) dev_info->devid, stat_entries[i].name, (uint64_t) dev_stats.values[stat_entries[i].index]);
				cookie->ok = false;
			} else if(cookie->verbose) {
				printf("%s: device ID %" PRIu64 ": zero %s\n", cookie->mountpoint, (uint64_t) dev_info->devid, stat_entries[i].name);
			}
		}
	}
	return true;
}

static bool do_devstats_fd(const char *mountpoint, bool verbose, int fd) {
	struct cookie cookie = { .mountpoint = mountpoint, .verbose = verbose, .fd = fd, .ok = true };
	return for_each_device(mountpoint, fd, &do_devstats_one, &cookie) && cookie.ok;
}

bool do_devstats(const char *mountpoint, bool verbose) {
	int fd = open(mountpoint, O_RDONLY | O_DIRECTORY);
	bool ret;
	if(fd >= 0) {
		ret = do_devstats_fd(mountpoint, verbose, fd);
		close(fd);
	} else {
		perror(mountpoint);
		ret = false;
	}
	return ret;
}
