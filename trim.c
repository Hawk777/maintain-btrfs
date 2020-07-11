#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "ops.h"

static bool do_trim_fd(const char *mountpoint, bool verbose, int fd) {
	struct fstrim_range args = {
		.start = 0,
		.len = (uint64_t) -1,
		.minlen = 0,
	};
	if(ioctl(fd, FITRIM, &args) >= 0) {
		if(verbose) {
			printf("%s: trimmed %" PRIu64 " unused bytes\n", mountpoint, (uint64_t) args.len);
		}
		return true;
	} else if(errno == EOPNOTSUPP) {
		if(verbose) {
			printf("%s: trim not supported\n", mountpoint);
		}
		return true;
	} else {
		perror(mountpoint);
		return false;
	}
}

bool do_trim(const char *mountpoint, bool verbose) {
	if(verbose) {
		printf("Trim %s:\n", mountpoint);
	}
	int fd = open(mountpoint, O_RDONLY | O_DIRECTORY);
	bool ret;
	if(fd >= 0) {
		ret = do_trim_fd(mountpoint, verbose, fd);
		close(fd);
	} else {
		perror(mountpoint);
		ret = false;
	}
	return ret;
}
