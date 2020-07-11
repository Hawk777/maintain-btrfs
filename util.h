#if !defined(UTIL_H)
#define UTIL_H

#include <stdbool.h>

struct btrfs_ioctl_fs_info_args;
struct btrfs_ioctl_dev_info_args;

bool for_each_device(const char *mountpoint, int fd, bool (*cb)(const struct btrfs_ioctl_fs_info_args *, const struct btrfs_ioctl_dev_info_args *, void *), void *cookie);

#endif
