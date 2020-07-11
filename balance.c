#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <unistd.h>
#include <linux/btrfs.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "ops.h"

static const int PROGRESS_INTERVAL = 5000;

static const unsigned int DATA_USAGE_THRESHOLD = 30;
static const unsigned int METADATA_USAGE_THRESHOLD = 10;
static const unsigned int SYSTEM_USAGE_THRESHOLD = METADATA_USAGE_THRESHOLD;

struct thread_info {
	int fd;
	int efd;
	struct btrfs_ioctl_balance_args args;
	int ioctl_ret;
	int ioctl_errno;
};

static int thread_proc(void *raw_ti) {
	struct thread_info *ti = raw_ti;
	ti->ioctl_ret = ioctl(ti->fd, BTRFS_IOC_BALANCE_V2, &ti->args);
	ti->ioctl_errno = errno;
	if(eventfd_write(ti->efd, 1) < 0) {
		perror("eventfd_write");
		abort();
	}
	return 0;
}

static bool do_balance_fd_auxfds(const char *mountpoint, bool verbose, int fd, int sigfd, int efd) {
	// Start a thread to do the balance.
	struct thread_info ti = {
		.fd = fd,
		.efd = efd,
		.args = {
			.flags = BTRFS_BALANCE_DATA | BTRFS_BALANCE_METADATA | BTRFS_BALANCE_SYSTEM,
			.data = { .flags = BTRFS_BALANCE_ARGS_USAGE, .usage = DATA_USAGE_THRESHOLD, },
			.meta = { .flags = BTRFS_BALANCE_ARGS_USAGE, .usage = METADATA_USAGE_THRESHOLD, },
			.sys = { .flags = BTRFS_BALANCE_ARGS_USAGE, .usage = SYSTEM_USAGE_THRESHOLD, },
		},
	};
	thrd_t thread;
	switch(thrd_create(&thread, &thread_proc, &ti)) {
		case thrd_success:
			break;

		case thrd_nomem:
			fprintf(stderr, "thrd_create: %s\n", strerror(ENOMEM));
			return false;

		case thrd_error:
			fputs("thrd_create: failed\n", stderr);
			return false;

		default:
			fputs("thrd_create: unknown error\n", stderr);
			return false;
	}

	// Monitor.
	bool done = false;
	while(!done) {
		struct pollfd pfds[] = {
			{ .fd = sigfd, .events = POLLIN, .revents = 0 },
			{ .fd = efd, .events = POLLIN, .revents = 0 },
		};
		if(poll(pfds, sizeof(pfds) / sizeof(*pfds), verbose ? PROGRESS_INTERVAL : -1) < 0) {
			perror("poll");
			break;
		}
		if(pfds[0].revents & POLLIN) {
			// A signal was received. Get out.
			break;
		}
		if(pfds[1].revents & POLLIN) {
			// The thread notified us of completion.
			done = true;
		}
		if(verbose) {
			struct btrfs_ioctl_balance_args args;
			if(ioctl(fd, BTRFS_IOC_BALANCE_PROGRESS, &args) >= 0) {
				unsigned int permille;
				if(!args.stat.expected) {
					permille = 500;
				} else if(args.stat.considered > args.stat.expected) {
					permille = 1000;
				} else {
					permille = args.stat.considered * 1000 / args.stat.expected;
				}
				printf("%" PRIu64 " / %" PRIu64 " considered = %u.%u%%\r", (uint64_t) args.stat.considered, (uint64_t) args.stat.expected, permille / 10, permille % 10);
				fflush(stdout);
			}
		}
	}

	// If the balance didn’t finish normally, cancel it.
	if(!done) {
		ioctl(fd, BTRFS_IOC_BALANCE_CTL, BTRFS_BALANCE_CTL_CANCEL);
	}

	// If we were displaying progress, print an empty line to avoid terminal
	// corruption.
	if(verbose) {
		putchar('\n');
	}

	// Join the thread and present the results.
	if(thrd_join(thread, 0) == thrd_error) {
		fputs("thrd_join: error\n", stderr);
		abort();
	}
	if(ti.ioctl_ret >= 0) {
		if(verbose && !(ti.args.state & BTRFS_BALANCE_STATE_CANCEL_REQ)) {
			printf("%s: relocated %" PRIu64" / %" PRIu64 " chunks\n", mountpoint, (uint64_t) ti.args.stat.completed, (uint64_t) ti.args.stat.considered);
		}
		return true;
	} else {
		if(ti.ioctl_errno != ECANCELED) {
			fprintf(stderr, "%s: balance failed: %s\n", mountpoint, strerror(ti.ioctl_errno));
		}
		return false;
	}
}

static bool do_balance_fd(const char *mountpoint, bool verbose, int fd) {
	// The balance ioctl is blocking and uninterruptible (in the traditional
	// signal-delivery sense) so just running it straight makes the process
	// unkillable (even with kill -9). However, BTRFS_BALANCE_CTL_CANCEL is
	// available, so make a signalfd to detect SIGINT, SIGQUIT, and SIGTERM and
	// cancel the in-progress scrubs. In order to accomplish that, the balance
	// runs in a separate thread and the main thread handles termination
	// signals, thread reaping, and progress display.
	//
	// A signalfd is used to take the termination signals. An eventfd is used
	// by the balance thread to notify the main thread that it is finished.
	sigset_t sigs;
	sigemptyset(&sigs);
	sigaddset(&sigs, SIGINT);
	sigaddset(&sigs, SIGQUIT);
	sigaddset(&sigs, SIGTERM);
	if(sigprocmask(SIG_BLOCK, &sigs, 0) < 0) {
		perror("sigprocmask");
		return false;
	}
	bool ret = true;
	int sigfd = signalfd(-1, &sigs, 0);
	if(sigfd >= 0) {
		int efd = eventfd(0, 0);
		if(efd >= 0) {
			ret = do_balance_fd_auxfds(mountpoint, verbose, fd, sigfd, efd);
		} else {
			perror("eventfd");
			ret = false;
		}
	} else {
		perror("signalfd");
		ret = false;
	}
	if(sigprocmask(SIG_UNBLOCK, &sigs, 0) < 0) {
		perror("sigprocmask");
		abort();
	}
	return ret;
}

bool do_balance(const char *mountpoint, bool verbose) {
	if(verbose) {
		printf("Balance %s:\n", mountpoint);
	}
	int fd = open(mountpoint, O_RDONLY | O_DIRECTORY);
	bool ret;
	if(fd >= 0) {
		ret = do_balance_fd(mountpoint, verbose, fd);
		close(fd);
	} else {
		perror(mountpoint);
		ret = false;
	}
	return ret;
}
